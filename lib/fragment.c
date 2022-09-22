/* Handling of the fragment cache */
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include "picoquic_utils.h"
#include "picosplay.h"
#include "quicrq.h"
#include "quicrq_reassembly.h"
#include "quicrq_internal.h"
#include "quicrq_fragment.h"

/* Manage the cached fragments.
 * The cache has two access methods:
 * - by order of arrival -- used for example when sending datagrams at relays.
 * - by group-id/object-id/offset -- used for example when sending on streams.
 * The order of arrival ordering is handled as a double chained list.
 * The group-id/object-id/offset ordering is handled as a splay.
 * 
 */
void* quicrq_fragment_cache_node_value(picosplay_node_t* fragment_node)
{
    return (fragment_node == NULL) ? NULL : (void*)((char*)fragment_node - offsetof(struct st_quicrq_cached_fragment_t, fragment_node));
}

static int64_t quicrq_fragment_cache_node_compare(void* l, void* r) {
    quicrq_cached_fragment_t* ls = (quicrq_cached_fragment_t*)l;
    quicrq_cached_fragment_t* rs = (quicrq_cached_fragment_t*)r;
    int64_t ret = ls->group_id - rs->group_id;

    if (ret == 0) {
        ret = ls->object_id - rs->object_id;
        if (ret == 0) {
            if (ls->offset < rs->offset) {
                ret = -1;
            }
            else if (ls->offset > rs->offset) {
                ret = 1;
            }
        }
    }
    return ret;
}

static picosplay_node_t* quicrq_fragment_cache_node_create(void* v_media_object)
{
    return &((quicrq_cached_fragment_t*)v_media_object)->fragment_node;
}

static void quicrq_fragment_cache_node_delete(void* tree, picosplay_node_t* node)
{
#ifdef _WINDOWS
    UNREFERENCED_PARAMETER(tree);
#endif
    quicrq_fragment_cached_media_t* cached_media = (quicrq_fragment_cached_media_t*)((char*)tree - offsetof(struct st_quicrq_fragment_cached_media_t, fragment_tree));
    quicrq_cached_fragment_t* fragment = (quicrq_cached_fragment_t*)quicrq_fragment_cache_node_value(node);

    if (fragment->previous_in_order == NULL) {
        cached_media->first_fragment = fragment->next_in_order;
    }
    else {
        fragment->previous_in_order->next_in_order = fragment->next_in_order;
    }

    if (fragment->next_in_order == NULL) {
        cached_media->last_fragment = fragment->previous_in_order;
    }
    else {
        fragment->next_in_order->previous_in_order = fragment->previous_in_order;
    }

    free(quicrq_fragment_cache_node_value(node));
}

quicrq_cached_fragment_t* quicrq_fragment_cache_get_fragment(quicrq_fragment_cached_media_t* cached_ctx,
    uint64_t group_id, uint64_t object_id, uint64_t offset)
{
    quicrq_cached_fragment_t key = { 0 };
    key.group_id = group_id;
    key.object_id = object_id;
    key.offset = offset;
    picosplay_node_t* fragment_node = picosplay_find(&cached_ctx->fragment_tree, &key);
    return (quicrq_cached_fragment_t*)quicrq_fragment_cache_node_value(fragment_node);
}

void quicrq_fragment_cache_media_clear(quicrq_fragment_cached_media_t* cached_media)
{
    cached_media->first_fragment = NULL;
    cached_media->last_fragment = NULL;
    picosplay_empty_tree(&cached_media->fragment_tree);
}

void quicrq_fragment_cache_media_init(quicrq_fragment_cached_media_t* cached_media)
{
    picosplay_init_tree(&cached_media->fragment_tree, quicrq_fragment_cache_node_compare,
        quicrq_fragment_cache_node_create, quicrq_fragment_cache_node_delete,
        quicrq_fragment_cache_node_value);
}


/* Fragment cache progress.
 * Manage the "next_group" and "next_object" items.
 */
void quicrq_fragment_cache_progress(quicrq_fragment_cached_media_t* cached_ctx,
    quicrq_cached_fragment_t* fragment)
{
    /* Check whether the next object is present */
    picosplay_node_t* next_fragment_node = &fragment->fragment_node;

    do {
        int is_expected = 0;
        fragment = (quicrq_cached_fragment_t*)quicrq_fragment_cache_node_value(next_fragment_node);
        if (fragment == NULL) {
            break;
        }
        if (fragment->group_id == cached_ctx->next_group_id &&
            fragment->object_id == cached_ctx->next_object_id &&
            fragment->offset == cached_ctx->next_offset) {
            is_expected = 1;
        }
        else if (fragment->group_id == (cached_ctx->next_group_id + 1) &&
            fragment->object_id == 0 &&
            fragment->offset == 0 &&
            cached_ctx->next_object_id > 0 &&
            cached_ctx->next_offset == 0 &&
            cached_ctx->next_object_id == fragment->nb_objects_previous_group) {
            cached_ctx->next_group_id += 1;
            cached_ctx->next_object_id = 0;
            cached_ctx->next_offset = 0;
            is_expected = 1;
        }
        if (is_expected) {
            if (fragment->is_last_fragment) {
                cached_ctx->next_object_id += 1;
                cached_ctx->next_offset = 0;
            }
            else {
                cached_ctx->next_offset += fragment->data_length;
            }
        }
        else {
            break;
        }
    } while ((next_fragment_node = picosplay_next(next_fragment_node)) != NULL);
}

int quicrq_fragment_add_to_cache(quicrq_fragment_cached_media_t* cached_ctx,
    const uint8_t* data,
    uint64_t group_id,
    uint64_t object_id,
    uint64_t offset,
    uint64_t queue_delay,
    uint8_t flags,
    uint64_t nb_objects_previous_group,
    int is_last_fragment,
    size_t data_length,
    uint64_t current_time)
{
    int ret = 0;
    quicrq_cached_fragment_t* fragment = (quicrq_cached_fragment_t*)malloc(
        sizeof(quicrq_cached_fragment_t) + data_length);

    if (fragment == NULL) {
        ret = -1;
    }
    else {
        memset(fragment, 0, sizeof(quicrq_cached_fragment_t));
        if (cached_ctx->last_fragment == NULL) {
            cached_ctx->first_fragment = fragment;
        }
        else {
            fragment->previous_in_order = cached_ctx->last_fragment;
            cached_ctx->last_fragment->next_in_order = fragment;
        }
        cached_ctx->last_fragment = fragment;
        fragment->group_id = group_id;
        fragment->object_id = object_id;
        fragment->offset = offset;
        fragment->cache_time = current_time;
        fragment->queue_delay = queue_delay;
        fragment->flags = flags;
        fragment->nb_objects_previous_group = nb_objects_previous_group;
        fragment->is_last_fragment = is_last_fragment;
        fragment->data = ((uint8_t*)fragment) + sizeof(quicrq_cached_fragment_t);
        fragment->data_length = data_length;
        memcpy(fragment->data, data, data_length);
        picosplay_insert(&cached_ctx->fragment_tree, fragment);
        quicrq_fragment_cache_progress(cached_ctx, fragment);
    }

    return ret;
}

int quicrq_fragment_propose_to_cache(quicrq_fragment_cached_media_t* cached_ctx,
    const uint8_t* data,
    uint64_t group_id,
    uint64_t object_id,
    uint64_t offset,
    uint64_t queue_delay,
    uint8_t flags,
    uint64_t nb_objects_previous_group,
    int is_last_fragment,
    size_t data_length,
    uint64_t current_time)
{
    int ret = 0;
    int data_was_added = 0;
    /* First check whether the object is in the cache. */
    /* If the object is in the cache, check whether this fragment is already received */
    quicrq_cached_fragment_t * first_fragment_state = NULL;
    quicrq_cached_fragment_t key = { 0 };

    if (group_id < cached_ctx->first_group_id ||
        (group_id == cached_ctx->first_group_id &&
            object_id < cached_ctx->first_object_id)) {
        /* This fragment is too old to be considered. */
        return 0;
    }
    key.group_id = group_id;
    key.object_id = object_id;
    key.offset = UINT64_MAX;
    picosplay_node_t* last_fragment_node = picosplay_find_previous(&cached_ctx->fragment_tree, &key);
    do {
        first_fragment_state = (quicrq_cached_fragment_t*)quicrq_fragment_cache_node_value(last_fragment_node);
        if (first_fragment_state == NULL || 
            first_fragment_state->group_id != group_id ||
            first_fragment_state->object_id != object_id ||
            first_fragment_state->offset + first_fragment_state->data_length < offset) {          
            /* Insert the whole fragment */
            ret = quicrq_fragment_add_to_cache(cached_ctx, data, 
                group_id, object_id, offset, queue_delay, flags, nb_objects_previous_group, is_last_fragment, data_length, current_time);
            data_was_added = 1;
            /* Mark done */
            data_length = 0;
        }
        else
        {
            uint64_t previous_last_byte = first_fragment_state->offset + first_fragment_state->data_length;
            if (offset + data_length > previous_last_byte) {
                /* Some of the fragment data comes after this one. Submit */
                size_t added_length = offset + data_length - previous_last_byte;
                ret = quicrq_fragment_add_to_cache(cached_ctx, data, 
                    group_id, object_id, offset, queue_delay, flags, nb_objects_previous_group, is_last_fragment, added_length, current_time);
                data_was_added = 1;
                data_length -= added_length;
                /* Previous group count is only used on first fragment */
                nb_objects_previous_group = 0;
            }
            if (offset >= first_fragment_state->offset) {
                /* What remained of the fragment overlaps with existing data */
                data_length = 0;
            }
            else {
                if (first_fragment_state->offset < offset + data_length) {
                    /* Some of the fragment data overlaps, remove it */
                    data_length = first_fragment_state->offset - offset;
                }
                last_fragment_node = picosplay_previous(last_fragment_node);
            }
        }
    } while (ret == 0 && data_length > 0);

    if (ret == 0 && data_was_added) {
        /* Wake up the consumers of this source */
        quicrq_source_wakeup(cached_ctx->srce_ctx);
        /* Check whether this object is now complete */
        last_fragment_node = picosplay_find_previous(&cached_ctx->fragment_tree, &key);
        first_fragment_state = (quicrq_cached_fragment_t*)quicrq_fragment_cache_node_value(last_fragment_node);
        if (first_fragment_state != NULL) {
            int last_is_final = first_fragment_state->is_last_fragment;
            uint64_t previous_offset = first_fragment_state->offset;

            while (last_is_final && previous_offset > 0) {
                last_fragment_node = picosplay_previous(last_fragment_node);
                if (last_fragment_node == NULL) {
                    last_is_final = 0;
                }
                else {
                    first_fragment_state = (quicrq_cached_fragment_t*)quicrq_fragment_cache_node_value(last_fragment_node);
                    if (first_fragment_state->group_id != group_id ||
                        first_fragment_state->object_id != object_id ||
                        first_fragment_state->offset + first_fragment_state->data_length < previous_offset) {
                        last_is_final = 0;
                    }
                    else {
                        previous_offset = first_fragment_state->offset;
                    }
                }
            }
            if (last_is_final) {
                /* The object was just completely received. Keep counts. */
                cached_ctx->nb_object_received += 1;
            }
        }
    }

    return ret;
}

int quicrq_fragment_cache_learn_start_point(quicrq_fragment_cached_media_t* cached_ctx,
    uint64_t start_group_id, uint64_t start_object_id)
{
    int ret = 0;
    /* Find all cache fragments that might be before the start point,
    * and delete them */
    picosplay_node_t* first_fragment_node = NULL;
    cached_ctx->first_group_id = start_group_id;
    cached_ctx->first_object_id = start_object_id;
    if (cached_ctx->next_group_id < start_group_id ||
        (cached_ctx->next_group_id == start_group_id &&
            cached_ctx->next_object_id < start_object_id)) {
        cached_ctx->next_group_id = start_group_id;
        cached_ctx->next_object_id = start_object_id;
    }
    while ((first_fragment_node = picosplay_first(&cached_ctx->fragment_tree)) != NULL) {
        quicrq_cached_fragment_t* first_fragment_state = 
            (quicrq_cached_fragment_t*)quicrq_fragment_cache_node_value(first_fragment_node);
        if (first_fragment_state == NULL || 
            first_fragment_state->group_id > start_group_id ||
            (first_fragment_state->group_id == start_group_id &&
                first_fragment_state->object_id >= start_object_id)) {
            break;
        }
        else {
            picosplay_delete_hint(&cached_ctx->fragment_tree, first_fragment_node);
        }
    }

    if (ret == 0) {
        /* Set the start point for the dependent streams. */
        quicrq_stream_ctx_t* stream_ctx = cached_ctx->srce_ctx->first_stream;
        while (stream_ctx != NULL) {
            /* for each client waiting for data on this media,
            * update the start point and then wakeup the stream 
            * so the start point can be releayed. */
            stream_ctx->start_group_id = start_group_id;
            stream_ctx->start_object_id = start_object_id;
            if (stream_ctx->cnx_ctx->cnx != NULL) {
                picoquic_mark_active_stream(stream_ctx->cnx_ctx->cnx, stream_ctx->stream_id, 1, stream_ctx);
            }
            stream_ctx = stream_ctx->next_stream_for_source;
        }
    }

    /* TODO: if the end is known, something special? */
    return ret;
}

int quicrq_fragment_cache_learn_end_point(quicrq_fragment_cached_media_t* cached_ctx,
    uint64_t final_group_id, uint64_t final_object_id)
{
    int ret = 0;
    /* Document the final group-ID and object-ID in context */
    cached_ctx->final_group_id = final_group_id;
    cached_ctx->final_object_id = final_object_id;
    /* wake up the clients waiting for data on this media */
    quicrq_source_wakeup(cached_ctx->srce_ctx);
    
    return ret;
}

int quicrq_fragment_cache_set_real_time_cache(quicrq_fragment_cached_media_t* cached_ctx)
{
    int ret = 0;
    quicrq_stream_ctx_t* stream_ctx = cached_ctx->srce_ctx->first_stream;
    /* remember the policy */
    cached_ctx->srce_ctx->is_cache_real_time = 1;
    /* Set the cache policy for the dependent streams. */
    while (stream_ctx != NULL && ret == 0) {
        /* for each client waiting for data on this media,
        * update the cache policy
        * so the start point can be releayed. */
        stream_ctx->is_cache_real_time = 1;
        if (stream_ctx->cnx_ctx->cnx != NULL) {
            ret = picoquic_mark_active_stream(stream_ctx->cnx_ctx->cnx, stream_ctx->stream_id, 1, stream_ctx);
        }
        stream_ctx = stream_ctx->next_stream_for_source;
    }
    return ret;
}

/* Purging old fragments from the cache. 
 * This should only be done for caches of type "real time".
 * - Compute the latest GOB.
 *   - lowest of current read point for any reader and last GOB in cache.
 * - Delete all objects with GOB < last.
 */
void quicrq_fragment_cache_media_purge_to_gob(
    quicrq_media_source_ctx_t* srce_ctx)
{
    picosplay_node_t* fragment_node;
    quicrq_fragment_cached_media_t* cached_ctx = (quicrq_fragment_cached_media_t*)srce_ctx->pub_ctx;
    if (cached_ctx != NULL) {
        uint64_t kept_group_id = cached_ctx->next_group_id;
        quicrq_stream_ctx_t* stream_ctx = srce_ctx->first_stream;

        /* Find the smallest GOB not currently read by active connections */
        while (stream_ctx != NULL) {
            quicrq_fragment_publisher_context_t* media_ctx = (quicrq_fragment_publisher_context_t*)stream_ctx->media_ctx;
            quicrq_fragment_publisher_object_state_t* first_object = quicrq_fragment_cache_node_value(picosplay_first(&media_ctx->publisher_object_tree));

            if (first_object != NULL && first_object->group_id < kept_group_id) {
                kept_group_id = first_object->group_id;
            }
            stream_ctx = stream_ctx->next_stream_for_source;
        }

        /* Purge all segments below that GOB. */
        while ((fragment_node = picosplay_first(&cached_ctx->fragment_tree)) != NULL) {
            /* Locate the first fragment in object order */
            quicrq_cached_fragment_t* fragment =
                (quicrq_cached_fragment_t*)quicrq_fragment_cache_node_value(fragment_node);
            if (fragment->group_id >= kept_group_id) {
                /* keep this fragment */
                cached_ctx->first_group_id = fragment->group_id;
                cached_ctx->first_object_id = fragment->object_id;
                break;
            }
            else {
                picosplay_delete_hint(&cached_ctx->fragment_tree, fragment_node);
            }
        }
    }
}

/* Cache management.
 *
 * The cache is deleted if nobody is reading it, the final segment is stored,
 * and enough time has passed -- typically 30 seconds.
 * If the connection feeding the cache is closed, we will not get any new fragment,
 * so there is no point waiting for them to arrive.
 * 
 * Deleting cached entries updates the "first_object_id" visible in the cache.
 * If a client subscribes to the cached media after a cache update, that
 * client will see the object ID numbers from that new start point on.
 */

void quicrq_fragment_cache_media_purge(
    quicrq_fragment_cached_media_t* cached_media,
    uint64_t current_time,
    uint64_t cache_duration_max,
    uint64_t first_object_id_kept)
{
    picosplay_node_t* fragment_node;

    while ((fragment_node = picosplay_first(&cached_media->fragment_tree)) != NULL) {
        /* Locate the first fragment in object order */
        quicrq_cached_fragment_t* fragment =
            (quicrq_cached_fragment_t*)quicrq_fragment_cache_node_value(fragment_node);
        /* Check whether that fragment should be kept */
        if (fragment->object_id >= first_object_id_kept || fragment->cache_time + cache_duration_max > current_time) {
            /* This fragment and all coming after it shall be kept. */
            break;
        }
        else {
            int should_delete = 1;
            if (!cached_media->is_closed) {
                picosplay_node_t* next_fragment_node = fragment_node;
                size_t next_offset = fragment->data_length;
                int last_found = fragment->is_last_fragment;
                should_delete = (fragment->object_id != cached_media->first_object_id) && fragment->offset == 0;

                while (should_delete && (next_fragment_node = picosplay_next(next_fragment_node)) != NULL) {
                    quicrq_cached_fragment_t* next_fragment =
                        (quicrq_cached_fragment_t*)quicrq_fragment_cache_node_value(next_fragment_node);
                    if (next_fragment->object_id != fragment->object_id ||
                        next_fragment->cache_time + cache_duration_max > current_time ||
                        next_fragment->offset != next_offset) {
                        break;
                    }
                    else {
                        next_offset += next_fragment->data_length;
                        if (next_fragment->is_last_fragment) {
                            /* All fragments up to the last have been verified */
                            last_found = 1;
                            break;
                        }
                    }
                }
                should_delete &= last_found;
            }
            if (should_delete) {
                cached_media->first_object_id = fragment->object_id + 1;
                while ((fragment_node = picosplay_first(&cached_media->fragment_tree)) != NULL) {
                    quicrq_cached_fragment_t* fragment =
                        (quicrq_cached_fragment_t*)quicrq_fragment_cache_node_value(fragment_node);
                    if (fragment->object_id >= cached_media->first_object_id) {
                        break;
                    }
                    else {
                        picosplay_delete_hint(&cached_media->fragment_tree, fragment_node);
                    }
                }
            }
        }
    }
}

void quicrq_fragment_cache_delete_ctx(quicrq_fragment_cached_media_t* cache_ctx)
{
    quicrq_fragment_cache_media_clear(cache_ctx);

    free(cache_ctx);
}

quicrq_fragment_cached_media_t* quicrq_fragment_cache_create_ctx(quicrq_ctx_t* qr_ctx)
{
    quicrq_fragment_cached_media_t* cache_ctx = (quicrq_fragment_cached_media_t*)malloc(
        sizeof(quicrq_fragment_cached_media_t));
    if (cache_ctx != NULL) {
        memset(cache_ctx, 0, sizeof(quicrq_fragment_cached_media_t));
        cache_ctx->subscribe_stream_id = UINT64_MAX;
        quicrq_fragment_cache_media_init(cache_ctx);
        cache_ctx->qr_ctx = qr_ctx;
    }
    return cache_ctx;
}


/* Fragment publisher
 * 
 * The publisher functions tested at client and server delivers data in sequence.
 * We can do that as a first approximation, but proper relay handling needs to consider
 * delivering data out of sequence too.
 * Theory of interaction:
 * - The client calls for "in sequence data"
 * - If there is some, proceed as usual.
 * - If there is a hole in the sequence, inform of the hole.
 * Upon notification of a hole, the client may either wait for the inline delivery,
 * so everything is sent in sequence, or accept out of sequence transmission.
 * If out of sequence transmission is accepted, the client starts polling
 * for the new object-id, offset zero.
 * When the correction is available, the client is notified, and polls for the
 * missing object-id.
 */
/* Manage the splay of cached fragments */
static void* quicrq_fragment_publisher_object_node_value(picosplay_node_t* publisher_object_node)
{
    return (publisher_object_node == NULL) ? NULL : (void*)((char*)publisher_object_node - offsetof(struct st_quicrq_fragment_publisher_object_state_t, publisher_object_node));
}

static int64_t quicrq_fragment_publisher_object_node_compare(void* l, void* r) {
    quicrq_fragment_publisher_object_state_t* ls = (quicrq_fragment_publisher_object_state_t*)l;
    quicrq_fragment_publisher_object_state_t* rs = (quicrq_fragment_publisher_object_state_t*)r;
    int64_t ret = ls->group_id - rs->group_id;

    if (ret == 0) {
        ret = ls->object_id - rs->object_id;
    }
    return ret;
}

static picosplay_node_t* quicrq_fragment_publisher_object_node_create(void* v_publisher_object)
{
    return &((quicrq_fragment_publisher_object_state_t*)v_publisher_object)->publisher_object_node;
}

static void quicrq_fragment_publisher_object_node_delete(void* tree, picosplay_node_t* node)
{
    if (tree == NULL){
        /* quicrq_fragment_publisher_context_t* cached_media = (quicrq_fragment_publisher_context_t*)((char*)tree - offsetof(struct st_quicrq_fragment_publisher_context_t, publisher_object_tree)); */
        DBG_PRINTF("%s", "Calling object node delete with empty tree");

    }

    free(quicrq_fragment_publisher_object_node_value(node));
}


quicrq_fragment_publisher_object_state_t* quicrq_fragment_publisher_object_add(quicrq_fragment_publisher_context_t* media_ctx,
    uint64_t group_id, uint64_t object_id)
{
    quicrq_fragment_publisher_object_state_t* publisher_object = 
        (quicrq_fragment_publisher_object_state_t*)malloc(sizeof(quicrq_fragment_publisher_object_state_t));

    if (publisher_object != NULL) {
        memset(publisher_object, 0, sizeof(quicrq_fragment_publisher_object_state_t));
        publisher_object->group_id = group_id;
        publisher_object->object_id = object_id;
        picosplay_insert(&media_ctx->publisher_object_tree, publisher_object);
    }

    return publisher_object;
}

quicrq_fragment_publisher_object_state_t* quicrq_fragment_publisher_object_get(quicrq_fragment_publisher_context_t* media_ctx,
    uint64_t group_id, uint64_t object_id)
{
    quicrq_fragment_publisher_object_state_t key = { 0 };
    key.group_id = group_id;
    key.object_id = object_id;
    picosplay_node_t* publisher_object_node = picosplay_find(&media_ctx->publisher_object_tree, &key);
    return (quicrq_fragment_publisher_object_state_t*)quicrq_fragment_publisher_object_node_value(publisher_object_node);
}

void quicrq_fragment_publisher_close(quicrq_fragment_publisher_context_t* media_ctx)
{
    quicrq_fragment_cached_media_t * cached_ctx = media_ctx->cache_ctx;

    picosplay_empty_tree(&media_ctx->publisher_object_tree);

    if (cached_ctx->is_closed && cached_ctx->qr_ctx != NULL) {
        /* This may be the last connection served from this cache */
        cached_ctx->qr_ctx->is_cache_closing_needed = 1;
    }

    free(media_ctx);
}

int quicrq_fragment_publisher_fn(
    quicrq_media_source_action_enum action,
    void* v_media_ctx,
    uint8_t* data,
    size_t data_max_size,
    size_t* data_length,
    uint8_t* flags,
    int* is_new_group,
    int* is_last_fragment,
    int* is_media_finished,
    int* is_still_active,
    int* has_backlog,
    uint64_t current_time)
{
    int ret = 0;

    quicrq_fragment_publisher_context_t* media_ctx = (quicrq_fragment_publisher_context_t*)v_media_ctx;
    if (action == quicrq_media_source_get_data) {
        *is_new_group = 0;
        *is_media_finished = 0;
        *is_last_fragment = 0;
        *is_still_active = 0;
        *data_length = 0;
        *has_backlog = 0;
        /* In sequence access to objects
         * variable current_object_id = in sequence.
         * variable current_offset = current_offset sent.
         */
        if ((media_ctx->cache_ctx->final_group_id != 0 || media_ctx->cache_ctx->final_object_id != 0) &&
            (media_ctx->current_group_id > media_ctx->cache_ctx->final_group_id ||
                (media_ctx->current_group_id == media_ctx->cache_ctx->final_group_id &&
                    media_ctx->current_object_id >= media_ctx->cache_ctx->final_object_id))) {
            *is_media_finished = 1;
        }
        else {
            /* If skipping the current objet, check that the next object is available */
            if (media_ctx->is_current_object_skipped) {
                /* If the exact next object is present, then life if good. */
                media_ctx->current_fragment = quicrq_fragment_cache_get_fragment(media_ctx->cache_ctx,
                    media_ctx->current_group_id, media_ctx->current_object_id + 1, 0);
                if (media_ctx->current_fragment != NULL) {
                    media_ctx->current_object_id += 1;
                    media_ctx->current_offset = 0;
                    media_ctx->is_current_object_skipped = 0;
                }
                else {
                    /* If the next group is present & this is as expected, life is also good. */
                    quicrq_cached_fragment_t* next_group_fragment =
                    next_group_fragment = quicrq_fragment_cache_get_fragment(media_ctx->cache_ctx,
                        media_ctx->current_group_id + 1, 0, 0);
                    if (next_group_fragment != NULL && 
                        media_ctx->current_object_id + 1 >= next_group_fragment->nb_objects_previous_group) {
                        /* The next group begins just after the skipped object, so life is good here too */
                        media_ctx->current_group_id += 1;
                        media_ctx->current_object_id = 0;
                        media_ctx->current_offset = 0;
                        media_ctx->is_current_object_skipped = 0;
                        media_ctx->current_fragment = next_group_fragment;
                        *is_new_group = 1;
                    }
                }
            } else if (media_ctx->current_fragment == NULL) {
                /* Find the fragment with the expected offset */
                media_ctx->current_fragment = quicrq_fragment_cache_get_fragment(media_ctx->cache_ctx, 
                    media_ctx->current_group_id, media_ctx->current_object_id, media_ctx->current_offset);
                /* if there is no such fragment and this is the beginning of a new object, try the next group */
                if (media_ctx->current_fragment == NULL && media_ctx->current_offset == 0) {
                    quicrq_cached_fragment_t* next_group_fragment = quicrq_fragment_cache_get_fragment(media_ctx->cache_ctx,
                        media_ctx->current_group_id + 1, 0, 0);
                    if (next_group_fragment != NULL) {
                        /* This is the first fragment of a new group. Check whether the objects from the
                         * previous group have been all received. */
                        if (media_ctx->current_object_id >= next_group_fragment->nb_objects_previous_group) {
                            media_ctx->current_fragment = next_group_fragment;
                            media_ctx->current_group_id = media_ctx->current_group_id + 1;
                            media_ctx->current_object_id = 0;
                            media_ctx->current_offset = 0;
                            *is_new_group = 1;
                        }
                        else {
                            DBG_PRINTF("Group %" PRIu64 " is not complete, time= %" PRIu64, media_ctx->current_group_id, current_time);
                        }
                    }
                }
            }
            if (media_ctx->current_fragment == NULL) {
                /* Check for end of media maybe */
            }
            else {
                size_t available = media_ctx->current_fragment->data_length - media_ctx->length_sent;
                size_t copied = data_max_size;
                int end_of_fragment = 0;

                *flags = media_ctx->current_fragment->flags;

                if (data_max_size >= available) {
                    end_of_fragment = 1;
                    *is_last_fragment = media_ctx->current_fragment->is_last_fragment;
                    copied = available;
                }
                *data_length = copied;
                *is_still_active = 1;
                if (media_ctx->current_offset > 0) {
                    *has_backlog = media_ctx->has_backlog;
                } else if (media_ctx->current_group_id < media_ctx->cache_ctx->next_group_id ||
                    (media_ctx->current_group_id == media_ctx->cache_ctx->next_group_id &&
                        media_ctx->current_object_id + 1 < media_ctx->cache_ctx->next_object_id)) {
                    *has_backlog = 1;
                    media_ctx->has_backlog = 1;
                }
                else {
                    *has_backlog = 0;
                    media_ctx->has_backlog = 0;
                }
                if (data != NULL) {
                    /* If data is set to NULL, return the available size but do not copy anything */
                    memcpy(data, media_ctx->current_fragment->data + media_ctx->length_sent, copied);
                    media_ctx->length_sent += copied;
                    if (end_of_fragment) {
                        if (media_ctx->current_fragment->is_last_fragment) {
                            media_ctx->current_object_id++;
                            media_ctx->current_offset = 0;
                        }
                        else {
                            media_ctx->current_offset += media_ctx->current_fragment->data_length;
                        }

                        media_ctx->length_sent = 0;
                        media_ctx->current_fragment = NULL;
                    }
                }
            }
        }
    }
    /* Skip object: if the logic has decided to skip this object, look at the next one */
    else if (action == quicrq_media_source_skip_object) {
        media_ctx->is_current_object_skipped = 1;
    }
    else if (action == quicrq_media_source_close) {
        /* close the context */
        quicrq_fragment_publisher_close(media_ctx);
    }
    return ret;
}


/* Evaluate whether the media context has backlog, and check
* whether the current object should be skipped.
*/
int quicrq_fragment_datagram_publisher_object_eval(
    quicrq_stream_ctx_t* stream_ctx,
    quicrq_fragment_publisher_context_t* media_ctx, int* should_skip, uint64_t current_time)
{
    int ret = 0;

    *should_skip = 0;
    if (media_ctx->current_fragment->object_id != 0 &&
        media_ctx->current_fragment->data_length > 0) {
        if (stream_ctx->cnx_ctx->qr_ctx->quic != NULL &&
            media_ctx->current_fragment != NULL) {
            int64_t delta_t = current_time - media_ctx->current_fragment->cache_time;
            int has_backlog = delta_t > 33333;

            *should_skip = quicrq_congestion_check_per_cnx(stream_ctx->cnx_ctx,
                media_ctx->current_fragment->flags, has_backlog, current_time);
        }
    }

    return ret;
}


/* datagram_publisher_check_object:
 * evaluate and if necessary progress the "current fragment" pointer.
 * After this evaluation, expect the following results:
 *  - return code not zero: something went very wrong.
 *  - media_ctx->current_fragment == NULL: sending is not started yet.
 *  - media_ctx->current_fragment != NULL:
 *    - media_ctx->is_current_fragment_sent == 1: already sent. Nothing else available.
 *    - media_ctx->is_current_fragment_sent == 0: should be processed
 */

int quicrq_fragment_datagram_publisher_check_fragment(
    quicrq_stream_ctx_t* stream_ctx, quicrq_fragment_publisher_context_t* media_ctx, int * should_skip, uint64_t current_time)
{
    int ret = 0;
    quicrq_fragment_publisher_object_state_t* publisher_object = NULL;

    *should_skip = 0;

    /* The "current fragment" shall never be NULL, unless this is the very first one. */
    if (media_ctx->current_fragment == NULL) {
        media_ctx->current_fragment = media_ctx->cache_ctx->first_fragment;
    }
    if (media_ctx->current_fragment == NULL) {
        /* Nothing to send yet */
    }
    else if (media_ctx->is_current_fragment_sent) {
        /* Find the next fragment in order, but skip if already skipped. */
        while (media_ctx->current_fragment->next_in_order != NULL) {
            /* Progress to the next fragment */
            media_ctx->length_sent = 0;
            media_ctx->is_current_fragment_sent = 0;
            media_ctx->current_fragment = media_ctx->current_fragment->next_in_order;
            publisher_object =
                quicrq_fragment_publisher_object_get(media_ctx, media_ctx->current_fragment->group_id, media_ctx->current_fragment->object_id);
            if (publisher_object == NULL) {
                /* Check whether the object is before the start of the list */
                quicrq_fragment_publisher_object_state_t* first_object = (quicrq_fragment_publisher_object_state_t*)
                    quicrq_fragment_publisher_object_node_value(picosplay_first(&media_ctx->publisher_object_tree));
                if (first_object != NULL && (first_object->group_id > media_ctx->current_fragment->group_id ||
                    (first_object->group_id == media_ctx->current_fragment->group_id &&
                        first_object->object_id > media_ctx->current_fragment->object_id))) {
                    /* This fragment should be skipped. */
                    media_ctx->is_current_fragment_sent = 1;
                }
                else {
                    /* this is a new object. The fragment should be processed. */
                    ret = quicrq_fragment_datagram_publisher_object_eval(stream_ctx, media_ctx, should_skip, current_time);
                    break;
                }
            }
            else if (publisher_object->is_dropped){
                /* Continue looking for the next object */
                media_ctx->is_current_fragment_sent = 1;
            }
            else {
                /* new fragment of a valid object. Should be sent next. */
                break;
            }
        }
    }
    return ret;
}

/* Prune the publisher object tree, removing all nodes that
 * have a successor and have not already been sent.
 * This avoids keeping large lists in memory.
 */
int quicrq_fragment_datagram_publisher_object_prune(
    quicrq_fragment_publisher_context_t* media_ctx)
{
    int ret = 0;
    quicrq_fragment_publisher_object_state_t* first_object = (quicrq_fragment_publisher_object_state_t*)
        quicrq_fragment_publisher_object_node_value(picosplay_first(&media_ctx->publisher_object_tree));

    while (first_object != NULL && first_object->is_sent) {
        quicrq_fragment_publisher_object_state_t* next_object = (quicrq_fragment_publisher_object_state_t*)
            quicrq_fragment_publisher_object_node_value(picosplay_next(&first_object->publisher_object_node));
        if (next_object == NULL) {
            break;
        }
        else {
            if ((next_object->group_id == first_object->group_id && next_object->object_id == first_object->object_id + 1) ||
                (next_object->group_id == first_object->group_id + 1 && next_object->object_id == 0 &&
                    next_object->nb_objects_previous_group == first_object->object_id + 1)) {
                /* In sequence! */
                picosplay_delete_hint(&media_ctx->publisher_object_tree, &first_object->publisher_object_node);
                first_object = next_object;
            }
            else {
                break;
            }
        }
    }

    return ret;
}

/* Update the publisher object after a fragment was sent.
 * - Keep track of how many bytes were sent for the object.
 * - Keep track of the bytes needed:
 *   - zero if object is skipped
 *   - final offset if object is sent.
 * - Mark "sent" if all bytes sent.
 * - if sent, check whether to prune the tree
 */
int quicrq_fragment_datagram_publisher_object_update(
    quicrq_fragment_publisher_context_t* media_ctx,
    int should_skip,
    int is_last_fragment,
    uint64_t next_offset,
    size_t copied )
{
    int ret = 0;
    quicrq_fragment_publisher_object_state_t* publisher_object = 
        quicrq_fragment_publisher_object_get(media_ctx, media_ctx->current_fragment->group_id, media_ctx->current_fragment->object_id);
    if (publisher_object == NULL) {
        publisher_object = quicrq_fragment_publisher_object_add(media_ctx,
            media_ctx->current_fragment->group_id, media_ctx->current_fragment->object_id);
    }
    if (publisher_object == NULL) {
        ret = -1;
    }
    else {
        /* Document object properties */
        publisher_object->bytes_sent += copied;
        if (is_last_fragment) {
            publisher_object->final_offset = next_offset;
        }
        publisher_object->is_dropped = should_skip;
        if (media_ctx->current_fragment->nb_objects_previous_group > 0) {
            publisher_object->nb_objects_previous_group = media_ctx->current_fragment->nb_objects_previous_group;
        }
        /* Check whether fully sent.
         * Consider special case of zero length fragments, skipped at previous network node.
         */
        if ((is_last_fragment && copied >= next_offset) ||
            (publisher_object->final_offset > 0 && publisher_object->bytes_sent >= publisher_object->final_offset)) {
            publisher_object->is_sent = 1;
            ret = quicrq_fragment_datagram_publisher_object_prune(media_ctx);
        }
    }

    return ret;
}

/* Send the next fragment, or a placeholder if the object shall be skipped. 
 */
int quicrq_fragment_datagram_publisher_send_fragment(
    quicrq_stream_ctx_t* stream_ctx,
    quicrq_fragment_publisher_context_t* media_ctx,
    uint64_t datagram_stream_id,
    void* context,
    size_t space,
    int* media_was_sent,
    int* at_least_one_active,
    int should_skip)
{
    int ret = 0;
    size_t offset = (should_skip) ? 0 : media_ctx->current_fragment->offset + media_ctx->length_sent;
    uint8_t datagram_header[QUICRQ_DATAGRAM_HEADER_MAX];
    uint8_t flags = (should_skip) ? 0xff : media_ctx->current_fragment->flags;
    int is_last_fragment = (should_skip) ? 1: media_ctx->current_fragment->is_last_fragment;
    size_t h_size = 0;
    uint8_t* h_byte = quicrq_datagram_header_encode(datagram_header, datagram_header + QUICRQ_DATAGRAM_HEADER_MAX,
        datagram_stream_id, media_ctx->current_fragment->group_id, media_ctx->current_fragment->object_id, offset,
        media_ctx->current_fragment->queue_delay, flags, media_ctx->current_fragment->nb_objects_previous_group,
        is_last_fragment);
    if (h_byte == NULL) {
        /* Should never happen. */
        ret = -1;
    }
    else {
        h_size = h_byte - datagram_header;

        if (h_size > space) {
            /* TODO: should get a min encoding length per stream */
            /* Can't do anything there */
            *at_least_one_active = 1;
        }
        else {
            size_t available = 0;
            size_t copied = 0; 
            if (!should_skip && media_ctx->current_fragment->data_length > 0) {
                /* If we are not skipping this object, compute the exact number of bytes to be sent.
                 * Encode the header again if something changed, e.g., last fragment bit. 
                 */
                available = media_ctx->current_fragment->data_length - media_ctx->length_sent;
                copied = space - h_size;
                if (copied >= available) {
                    copied = available;
                } else if (is_last_fragment){
                    /* In the rare case where this was the last fragment but there is not enough space available, 
                     * we need to reset the header.
                     */
                    is_last_fragment = 0;
                    h_byte = quicrq_datagram_header_encode(datagram_header, datagram_header + QUICRQ_DATAGRAM_HEADER_MAX,
                        datagram_stream_id, media_ctx->current_fragment->group_id, media_ctx->current_fragment->object_id, offset,
                        media_ctx->current_fragment->queue_delay, media_ctx->current_fragment->flags, media_ctx->current_fragment->nb_objects_previous_group,
                        0);

                    if (h_byte != datagram_header + h_size) {
                        /* Can't happen, unless our coding assumptions were wrong. Would need to debug that. */
                        ret = -1;
                    }
                }
            }
            if (copied > 0 || should_skip || media_ctx->current_fragment->data_length == 0){
                /* Get a buffer inside the datagram packet */
                void* buffer = picoquic_provide_datagram_buffer(context, copied + h_size);
                if (buffer == NULL) {
                    ret = -1;
                }
                else {
                    /* Push the header */
                    if (ret == 0) {
                        memcpy(buffer, datagram_header, h_size);
                        /* Get the media */
                        if (copied > 0) {
                            memcpy(((uint8_t*)buffer) + h_size, media_ctx->current_fragment->data + media_ctx->length_sent, copied);
                            media_ctx->length_sent += copied;
                        }
                        media_ctx->is_current_fragment_sent |= (should_skip || media_ctx->length_sent >= media_ctx->current_fragment->data_length);
                        *media_was_sent = 1;
                        *at_least_one_active = 1;
                        if (stream_ctx != NULL) {
                            /* Keep track in stream context */
                            ret = quicrq_datagram_ack_init(stream_ctx,
                                media_ctx->current_fragment->group_id,
                                media_ctx->current_fragment->object_id, offset, flags,
                                media_ctx->current_fragment->nb_objects_previous_group,
                                ((uint8_t*)buffer) + h_size, copied,
                                media_ctx->current_fragment->queue_delay, is_last_fragment, NULL,
                                picoquic_get_quic_time(stream_ctx->cnx_ctx->qr_ctx->quic));
                            if (ret != 0) {
                                DBG_PRINTF("Datagram ack init returns %d", ret);
                            }
                        }
                        if (ret == 0) {
                            ret = quicrq_fragment_datagram_publisher_object_update(media_ctx,
                                should_skip, is_last_fragment, offset + copied, copied);
                        }
                    }
                }
            }
        }
    }

    return ret;
}

int quicrq_fragment_datagram_publisher_prepare(
    quicrq_stream_ctx_t* stream_ctx,
    quicrq_fragment_publisher_context_t* media_ctx,
    uint64_t datagram_stream_id,
    void* context,
    size_t space,
    int* media_was_sent,
    int* at_least_one_active,
    int* not_ready,
    uint64_t current_time)
{
    /* First, check if there is something to send. */
    int ret;
    int should_skip = 0;

    *media_was_sent = 0;
    *not_ready = 0;
    
    /* Evaluate fragment and congestion */
    ret = quicrq_fragment_datagram_publisher_check_fragment(stream_ctx, media_ctx, &should_skip, current_time);
    if (ret != 0 || media_ctx->current_fragment == NULL || media_ctx->is_current_fragment_sent) {
        *not_ready = 1;
    }
    else  {
        /* Then send the object */
        if (ret == 0) {
            ret = quicrq_fragment_datagram_publisher_send_fragment(stream_ctx, media_ctx, datagram_stream_id,
                context, space, media_was_sent, at_least_one_active, should_skip);
        }
    }
    return ret;
}

int quicrq_fragment_datagram_publisher_fn(
    quicrq_stream_ctx_t* stream_ctx,
    void* context,
    size_t space,
    int* media_was_sent,
    int* at_least_one_active,
    uint64_t current_time)
{
    int ret = 0;
    int not_ready = 0;
    quicrq_fragment_publisher_context_t* media_ctx = (quicrq_fragment_publisher_context_t*)stream_ctx->media_ctx;

    /* The "prepare" function has no dependency on stream context,
     * which helps designing unit tests.
     */
    ret = quicrq_fragment_datagram_publisher_prepare(stream_ctx, media_ctx,
        stream_ctx->datagram_stream_id, context, space, media_was_sent, at_least_one_active, &not_ready, current_time);

    if (not_ready){
        /* Nothing to send at this point. If the media sending is finished, mark the stream accordingly.
         * The cache filling function checks that the final ID is only marked when all fragments have been
         * received. At this point, we only check that the final ID is marked, and all fragments have 
         * been sent.
         */

        if ((media_ctx->cache_ctx->final_group_id != 0 || media_ctx->cache_ctx->final_object_id != 0) &&
            media_ctx->current_fragment != NULL &&
            media_ctx->is_current_fragment_sent &&
            media_ctx->current_fragment->next_in_order == NULL) {
            /* Mark the stream as finished, prepare sending a final message */
            stream_ctx->final_group_id = media_ctx->cache_ctx->final_group_id;
            stream_ctx->final_object_id = media_ctx->cache_ctx->final_object_id;
            /* Wake up the control stream so the final message can be sent. */
            picoquic_mark_active_stream(stream_ctx->cnx_ctx->cnx, stream_ctx->stream_id, 1, stream_ctx);
            stream_ctx->is_active_datagram = 0;
        }
    }

    return ret;
}

void* quicrq_fragment_publisher_subscribe(void* v_srce_ctx, quicrq_stream_ctx_t * stream_ctx)
{
    quicrq_fragment_cached_media_t* cache_ctx = (quicrq_fragment_cached_media_t*)v_srce_ctx;
    quicrq_fragment_publisher_context_t* media_ctx = (quicrq_fragment_publisher_context_t*)
        malloc(sizeof(quicrq_fragment_publisher_context_t));
    if (media_ctx != NULL) {
        memset(media_ctx, 0, sizeof(quicrq_fragment_publisher_context_t));
        media_ctx->cache_ctx = cache_ctx;
        if (stream_ctx != NULL) {
            stream_ctx->start_group_id = cache_ctx->first_group_id;
            stream_ctx->start_object_id = cache_ctx->first_object_id;
        }
        picosplay_init_tree(&media_ctx->publisher_object_tree, quicrq_fragment_publisher_object_node_compare,
            quicrq_fragment_publisher_object_node_create, quicrq_fragment_publisher_object_node_delete,
            quicrq_fragment_publisher_object_node_value);
    }
    return media_ctx;
}

void quicrq_fragment_publisher_delete(void* v_pub_ctx)
{
    quicrq_fragment_cached_media_t* cache_ctx = (quicrq_fragment_cached_media_t*)v_pub_ctx;
    quicrq_fragment_cache_media_clear(cache_ctx);
    free(cache_ctx);
}

int quicrq_publish_fragment_cached_media(quicrq_ctx_t* qr_ctx,
    quicrq_fragment_cached_media_t* cache_ctx, const uint8_t* url, const size_t url_length,
    int is_local_object_source, int is_cache_real_time)
{
    int ret = 0;
    /* if succeeded, publish the source */
    cache_ctx->srce_ctx = quicrq_publish_datagram_source(qr_ctx, url, url_length, cache_ctx, is_local_object_source, is_cache_real_time);
    return (cache_ctx->srce_ctx == NULL)?-1:0;
}
