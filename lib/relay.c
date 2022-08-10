/* Handling of a relay */
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include "picoquic_utils.h"
#include "picosplay.h"
#include "quicrq.h"
#include "quicrq_reassembly.h"
#include "quicrq_internal.h"
#include "quicrq_relay_internal.h"

/* A relay is a specialized node, acting both as client when acquiring a media
 * fragment and as server when producing data.
 * 
 * There is one QUICRQ context per relay, used both for initiating a connection to
 * the server, and accepting connections from the client.
 * 
 * When a client requests an URL from the relay, the relay checks whether that URL is
 * already published, i.e., present in the local cache. If it is, then the client is
 * connected to that source. If not, the source is created and a request to the
 * server is started, in order to acquire the URL.
 * 
 * When  a client posts an URL to the relay, the relay checks whether the URL exists
 * already. For now, we will treat that as an error case. If it does not, the
 * relay creates a context over which to receive the media, and POSTs the content to
 * the server.
 * 
 * The client half creates a list of media objects. For simplification, the server half will
 * only deal with the media objects that are fully received. When a media object is
 * fully received, it becomes available. We may consider a difference in
 * availability between "in-order" and "out-of-sequence" availablity, which
 * may need to be reflected in the contract between connection and sources.
 */

/* Manage the splay of cached fragments */
static void* quicrq_relay_cache_fragment_node_value(picosplay_node_t* fragment_node)
{
    return (fragment_node == NULL) ? NULL : (void*)((char*)fragment_node - offsetof(struct st_quicrq_relay_cached_fragment_t, fragment_node));
}

static int64_t quicrq_relay_cache_fragment_node_compare(void* l, void* r) {
    quicrq_relay_cached_fragment_t* ls = (quicrq_relay_cached_fragment_t*)l;
    quicrq_relay_cached_fragment_t* rs = (quicrq_relay_cached_fragment_t*)r;
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

static picosplay_node_t* quicrq_relay_cache_fragment_node_create(void* v_media_object)
{
    return &((quicrq_relay_cached_fragment_t*)v_media_object)->fragment_node;
}

static void quicrq_relay_cache_fragment_node_delete(void* tree, picosplay_node_t* node)
{
#ifdef _WINDOWS
    UNREFERENCED_PARAMETER(tree);
#endif
    quicrq_relay_cached_media_t* cached_media = (quicrq_relay_cached_media_t*)((char*)tree - offsetof(struct st_quicrq_relay_cached_media_t, fragment_tree));
    quicrq_relay_cached_fragment_t* fragment = (quicrq_relay_cached_fragment_t*)quicrq_relay_cache_fragment_node_value(node);

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

    free(quicrq_relay_cache_fragment_node_value(node));
}

quicrq_relay_cached_fragment_t* quicrq_relay_cache_get_fragment(quicrq_relay_cached_media_t* cached_ctx,
    uint64_t group_id, uint64_t object_id, uint64_t offset)
{
    quicrq_relay_cached_fragment_t key = { 0 };
    key.group_id = group_id;
    key.object_id = object_id;
    key.offset = offset;
    picosplay_node_t* fragment_node = picosplay_find(&cached_ctx->fragment_tree, &key);
    return (quicrq_relay_cached_fragment_t*)quicrq_relay_cache_fragment_node_value(fragment_node);
}

void quicrq_relay_cache_media_clear(quicrq_relay_cached_media_t* cached_media)
{
    cached_media->first_fragment = NULL;
    cached_media->last_fragment = NULL;
    picosplay_empty_tree(&cached_media->fragment_tree);
}

void quicrq_relay_cache_media_init(quicrq_relay_cached_media_t* cached_media)
{
    picosplay_init_tree(&cached_media->fragment_tree, quicrq_relay_cache_fragment_node_compare,
        quicrq_relay_cache_fragment_node_create, quicrq_relay_cache_fragment_node_delete,
        quicrq_relay_cache_fragment_node_value);
}


/* Client part of the relay.
 * The connection is started when a context is specialized to become a relay
 */

 /* Relay cache progress.
  * Manage the "next_group" and "next_object" items.
  */
void quicrq_relay_cache_progress(quicrq_relay_cached_media_t* cached_ctx,
    quicrq_relay_cached_fragment_t* fragment)
{
    /* Check whether the next object is present */
    picosplay_node_t* next_fragment_node = &fragment->fragment_node;

    do {
        int is_expected = 0;
        fragment = (quicrq_relay_cached_fragment_t*)quicrq_relay_cache_fragment_node_value(next_fragment_node);
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

int quicrq_relay_add_fragment_to_cache(quicrq_relay_cached_media_t* cached_ctx,
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
    quicrq_relay_cached_fragment_t* fragment = (quicrq_relay_cached_fragment_t*)malloc(
        sizeof(quicrq_relay_cached_fragment_t) + data_length);

    if (fragment == NULL) {
        ret = -1;
    }
    else {
        memset(fragment, 0, sizeof(quicrq_relay_cached_fragment_t));
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
        fragment->data = ((uint8_t*)fragment) + sizeof(quicrq_relay_cached_fragment_t);
        fragment->data_length = data_length;
        memcpy(fragment->data, data, data_length);
        picosplay_insert(&cached_ctx->fragment_tree, fragment);
        quicrq_relay_cache_progress(cached_ctx, fragment);
    }

    return ret;
}

int quicrq_relay_propose_fragment_to_cache(quicrq_relay_cached_media_t* cached_ctx,
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
    quicrq_relay_cached_fragment_t * first_fragment_state = NULL;
    quicrq_relay_cached_fragment_t key = { 0 };

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
        first_fragment_state = (quicrq_relay_cached_fragment_t*)quicrq_relay_cache_fragment_node_value(last_fragment_node);
        if (first_fragment_state == NULL || 
            first_fragment_state->group_id != group_id ||
            first_fragment_state->object_id != object_id ||
            first_fragment_state->offset + first_fragment_state->data_length < offset) {          
            /* Insert the whole fragment */
            ret = quicrq_relay_add_fragment_to_cache(cached_ctx, data, 
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
                ret = quicrq_relay_add_fragment_to_cache(cached_ctx, data, 
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
        first_fragment_state = (quicrq_relay_cached_fragment_t*)quicrq_relay_cache_fragment_node_value(last_fragment_node);
        if (first_fragment_state != NULL) {
            int last_is_final = first_fragment_state->is_last_fragment;
            uint64_t previous_offset = first_fragment_state->offset;

            while (last_is_final && previous_offset > 0) {
                last_fragment_node = picosplay_previous(last_fragment_node);
                if (last_fragment_node == NULL) {
                    last_is_final = 0;
                }
                else {
                    first_fragment_state = (quicrq_relay_cached_fragment_t*)quicrq_relay_cache_fragment_node_value(last_fragment_node);
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

int quicrq_relay_learn_start_point(quicrq_relay_cached_media_t* cached_ctx,
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
        quicrq_relay_cached_fragment_t* first_fragment_state = 
            (quicrq_relay_cached_fragment_t*)quicrq_relay_cache_fragment_node_value(first_fragment_node);
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
    /* TODO: if the end is known, something special? */
    return ret;
}

/* Purging the old fragments from the cache.
 * There are two modes of operation.
 * In the general case, we want to make sure that all data has a chance of being
 * sent to the clients reading data from the cache. That means:
 *  - only delete objects if all previous objects have been already received,
 *  - only delete objects if all fragments have been received,
 *  - only delete objects if all fragments are old enough.
 * If the connection feeding the cache is closed, we will not get any new fragment,
 * so there is no point waiting for them to arrive.
 * 
 * Deleting cached entries updates the "first_object_id" visible in the cache.
 * If a client subscribes to the cached media after a cache update, that
 * client will see the object ID numbers from that new start point on.
 */

void quicrq_relay_cache_media_purge(
    quicrq_relay_cached_media_t* cached_media,
    uint64_t current_time,
    uint64_t cache_duration_max,
    uint64_t first_object_id_kept)
{
    picosplay_node_t* fragment_node;

    while ((fragment_node = picosplay_first(&cached_media->fragment_tree)) != NULL) {
        /* Locate the first fragment in object order */
        quicrq_relay_cached_fragment_t* fragment =
            (quicrq_relay_cached_fragment_t*)quicrq_relay_cache_fragment_node_value(fragment_node);
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
                    quicrq_relay_cached_fragment_t* next_fragment =
                        (quicrq_relay_cached_fragment_t*)quicrq_relay_cache_fragment_node_value(next_fragment_node);
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
                    quicrq_relay_cached_fragment_t* fragment =
                        (quicrq_relay_cached_fragment_t*)quicrq_relay_cache_fragment_node_value(fragment_node);
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

int quicrq_relay_consumer_cb(
    quicrq_media_consumer_enum action,
    void* media_ctx,
    uint64_t current_time,
    const uint8_t* data,
    uint64_t group_id,
    uint64_t object_id,
    uint64_t offset,
    uint64_t queue_delay,
    uint8_t flags,
    uint64_t nb_objects_previous_group,
    int is_last_fragment,
    size_t data_length)
{
    int ret = 0;
    quicrq_relay_consumer_context_t * cons_ctx = (quicrq_relay_consumer_context_t*)media_ctx;

    switch (action) {
    case quicrq_media_datagram_ready:
        /* Check that this datagram was not yet received.
         * This requires accessing the cache by object_id, offset and length. */
         /* Add fragment (or fragments) to cache */
        ret = quicrq_relay_propose_fragment_to_cache(cons_ctx->cached_ctx, data, 
            group_id, object_id, offset, queue_delay, flags, nb_objects_previous_group, is_last_fragment, data_length, current_time);
        /* Manage fin of transmission */
        if (ret == 0) {
            /* If the final group id and object id are known, and the next expected
             * values match, then the transmission is finished. */
            if ((cons_ctx->cached_ctx->final_group_id > 0 || cons_ctx->cached_ctx->final_object_id > 0) &&
                cons_ctx->cached_ctx->next_group_id == cons_ctx->cached_ctx->final_group_id &&
                cons_ctx->cached_ctx->next_object_id == cons_ctx->cached_ctx->final_object_id) {
                ret = quicrq_consumer_finished;
            }
        }
        break;
    case quicrq_media_final_object_id:
        /* Document the final group-ID and object-ID in context */
        cons_ctx->cached_ctx->final_group_id = group_id;
        cons_ctx->cached_ctx->final_object_id = object_id;
        /* Manage fin of transmission */
        if (cons_ctx->cached_ctx->next_group_id == cons_ctx->cached_ctx->final_group_id &&
            cons_ctx->cached_ctx->next_object_id == cons_ctx->cached_ctx->final_object_id) {
            ret = quicrq_consumer_finished;
        }
        if (ret == 0) {
            /* wake up the clients waiting for data on this media */
            quicrq_source_wakeup(cons_ctx->cached_ctx->srce_ctx);
        }
        break;
    case quicrq_media_start_point:
        /* Document the start point, and clean the cache of data before that point */
        ret = quicrq_relay_learn_start_point(cons_ctx->cached_ctx, group_id, object_id);
        if (ret == 0) {
            /* Set the start point for the dependent streams. */
            quicrq_stream_ctx_t* stream_ctx = cons_ctx->cached_ctx->srce_ctx->first_stream;
            while (stream_ctx != NULL) {
                /* for each client waiting for data on this media,
                 * update the start point and then wakeup the stream 
                 * so the start point can be releayed. */
                stream_ctx->start_object_id = object_id;
                if (stream_ctx->cnx_ctx->cnx != NULL) {
                    picoquic_mark_active_stream(stream_ctx->cnx_ctx->cnx, stream_ctx->stream_id, 1, stream_ctx);
                }
                stream_ctx = stream_ctx->next_stream_for_source;
            }
        }
        break;
    case quicrq_media_close:
        /* Document the final object */
        if (cons_ctx->cached_ctx->final_group_id == 0 && cons_ctx->cached_ctx->final_object_id == 0) {
            /* cache delete time set in the future to allow for reconnection. */
            cons_ctx->cached_ctx->cache_delete_time = current_time + 30000000;
            /* Document the last group_id and object_id that were fully received. */
            if (cons_ctx->cached_ctx->next_offset == 0) {
                cons_ctx->cached_ctx->final_group_id = cons_ctx->cached_ctx->next_group_id;
                cons_ctx->cached_ctx->final_object_id = cons_ctx->cached_ctx->next_object_id;
            }
            else  if (cons_ctx->cached_ctx->next_object_id > 1) {
                cons_ctx->cached_ctx->final_group_id = cons_ctx->cached_ctx->next_group_id;
                cons_ctx->cached_ctx->final_object_id = cons_ctx->cached_ctx->next_object_id - 1;
            }
            else {
                /* find the last object that was fully received. If there is none,
                 * leave the final_group_id and final_object_id
                 */
                quicrq_relay_cached_fragment_t key = { 0 };
                picosplay_node_t* fragment_node = NULL;
                quicrq_relay_cached_fragment_t* fragment = NULL;

                key.group_id = cons_ctx->cached_ctx->next_group_id;
                key.object_id = 0;
                key.offset = 0;
                fragment_node = picosplay_find_previous(&cons_ctx->cached_ctx->fragment_tree, &key);
                if (fragment_node != NULL) {
                    fragment = (quicrq_relay_cached_fragment_t*)quicrq_relay_cache_fragment_node_value(fragment_node);
                }
                if (fragment != NULL) {
                    cons_ctx->cached_ctx->final_group_id = fragment->group_id;
                    cons_ctx->cached_ctx->final_object_id = fragment->object_id;
                }
                else {
                    cons_ctx->cached_ctx->final_group_id = cons_ctx->cached_ctx->first_group_id;
                    cons_ctx->cached_ctx->final_object_id = cons_ctx->cached_ctx->first_object_id;
                }
            }
        }
        else {
            /* cache delete time set at short interval. */
            cons_ctx->cached_ctx->cache_delete_time = current_time + 3000000;
        }
        cons_ctx->cached_ctx->is_closed = 1;
        
        /* Set the target delete date */
        /* Notify consumers of the stream */
        quicrq_source_wakeup(cons_ctx->cached_ctx->srce_ctx);
        /* Free the media context resource */
        free(media_ctx);
        break;

    default:
        ret = -1;
        break;
    }
    return ret;
}

void quicrq_relay_delete_cache_ctx(quicrq_relay_cached_media_t* cache_ctx)
{
    quicrq_relay_cache_media_clear(cache_ctx);

    free(cache_ctx);
}

/* Server part of the relay.
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
static void* quicrq_relay_publisher_object_node_value(picosplay_node_t* publisher_object_node)
{
    return (publisher_object_node == NULL) ? NULL : (void*)((char*)publisher_object_node - offsetof(struct st_quicrq_relay_publisher_object_state_t, publisher_object_node));
}

static int64_t quicrq_relay_publisher_object_node_compare(void* l, void* r) {
    quicrq_relay_publisher_object_state_t* ls = (quicrq_relay_publisher_object_state_t*)l;
    quicrq_relay_publisher_object_state_t* rs = (quicrq_relay_publisher_object_state_t*)r;
    int64_t ret = ls->group_id - rs->group_id;

    if (ret == 0) {
        ret = ls->object_id - rs->object_id;
    }
    return ret;
}

static picosplay_node_t* quicrq_relay_publisher_object_node_create(void* v_publisher_object)
{
    return &((quicrq_relay_publisher_object_state_t*)v_publisher_object)->publisher_object_node;
}

static void quicrq_relay_publisher_object_node_delete(void* tree, picosplay_node_t* node)
{
    if (tree == NULL){
        /* quicrq_relay_publisher_context_t* cached_media = (quicrq_relay_publisher_context_t*)((char*)tree - offsetof(struct st_quicrq_relay_publisher_context_t, publisher_object_tree)); */
        DBG_PRINTF("%s", "Calling object node delete with empty tree");

    }

    free(quicrq_relay_publisher_object_node_value(node));
}


quicrq_relay_publisher_object_state_t* quicrq_relay_publisher_object_add(quicrq_relay_publisher_context_t* media_ctx,
    uint64_t group_id, uint64_t object_id)
{
    int ret = 0;
    quicrq_relay_publisher_object_state_t* publisher_object = 
        (quicrq_relay_publisher_object_state_t*)malloc(sizeof(quicrq_relay_publisher_object_state_t));

    if (publisher_object != NULL) {
        memset(publisher_object, 0, sizeof(quicrq_relay_publisher_object_state_t));
        publisher_object->group_id = group_id;
        publisher_object->object_id = object_id;
        picosplay_insert(&media_ctx->publisher_object_tree, publisher_object);
    }

    return publisher_object;
}

quicrq_relay_publisher_object_state_t* quicrq_relay_publisher_object_get(quicrq_relay_publisher_context_t* media_ctx,
    uint64_t group_id, uint64_t object_id)
{
    quicrq_relay_publisher_object_state_t key = { 0 };
    key.group_id = group_id;
    key.object_id = object_id;
    picosplay_node_t* publisher_object_node = picosplay_find(&media_ctx->publisher_object_tree, &key);
    return (quicrq_relay_publisher_object_state_t*)quicrq_relay_publisher_object_node_value(publisher_object_node);
}



void quicrq_relay_publisher_close(quicrq_relay_publisher_context_t* media_ctx)
{
    quicrq_relay_cached_media_t * cached_ctx = media_ctx->cache_ctx;

    picosplay_empty_tree(&media_ctx->publisher_object_tree);

    if (cached_ctx->is_closed && cached_ctx->qr_ctx != NULL) {
        /* This may be the last connection served from this cache */
        cached_ctx->qr_ctx->is_cache_closing_needed = 1;
    }

    free(media_ctx);
}

int quicrq_relay_publisher_fn(
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

    quicrq_relay_publisher_context_t* media_ctx = (quicrq_relay_publisher_context_t*)v_media_ctx;
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
                media_ctx->current_fragment = quicrq_relay_cache_get_fragment(media_ctx->cache_ctx,
                    media_ctx->current_group_id, media_ctx->current_object_id + 1, 0);
                if (media_ctx->current_fragment != NULL) {
                    media_ctx->current_object_id += 1;
                    media_ctx->current_offset = 0;
                    media_ctx->is_current_object_skipped = 0;
                }
                else {
                    /* If the next group is present & this is as expected, life is also good. */
                    quicrq_relay_cached_fragment_t* next_group_fragment =
                    next_group_fragment = quicrq_relay_cache_get_fragment(media_ctx->cache_ctx,
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
                media_ctx->current_fragment = quicrq_relay_cache_get_fragment(media_ctx->cache_ctx, 
                    media_ctx->current_group_id, media_ctx->current_object_id, media_ctx->current_offset);
                /* if there is no such fragment and this is the beginning of a new object, try the next group */
                if (media_ctx->current_fragment == NULL && media_ctx->current_offset == 0) {
                    quicrq_relay_cached_fragment_t* next_group_fragment = quicrq_relay_cache_get_fragment(media_ctx->cache_ctx,
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
        quicrq_relay_publisher_close(media_ctx);
    }
    return ret;
}



/* Evaluate whether the media context has backlog, and check
* whether the current object should be skipped.
*/
int quicrq_relay_datagram_publisher_object_eval(
    quicrq_stream_ctx_t* stream_ctx,
    quicrq_relay_publisher_context_t* media_ctx, int* should_skip)
{
    int ret = 0;

    *should_skip = 0;
    if (media_ctx->current_fragment->object_id != 0 &&
        media_ctx->current_fragment->data_length > 0) {
        if (stream_ctx->cnx_ctx->qr_ctx->quic != NULL &&
            media_ctx->current_fragment != NULL) {
            uint64_t current_time = picoquic_get_quic_time(stream_ctx->cnx_ctx->qr_ctx->quic);
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

int quicrq_relay_datagram_publisher_check_fragment(
    quicrq_stream_ctx_t* stream_ctx, quicrq_relay_publisher_context_t* media_ctx, int * should_skip)
{
    int ret = 0;
    quicrq_relay_publisher_object_state_t* publisher_object = NULL;

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
                quicrq_relay_publisher_object_get(media_ctx, media_ctx->current_fragment->group_id, media_ctx->current_fragment->object_id);
            if (publisher_object == NULL) {
                /* Check whether the object is before the start of the list */
                quicrq_relay_publisher_object_state_t* first_object = (quicrq_relay_publisher_object_state_t*)
                    quicrq_relay_publisher_object_node_value(picosplay_first(&media_ctx->publisher_object_tree));
                if (first_object != NULL && (first_object->group_id > media_ctx->current_fragment->group_id ||
                    (first_object->group_id == media_ctx->current_fragment->group_id &&
                        first_object->object_id > media_ctx->current_fragment->object_id))) {
                    /* This fragment should be skipped. */
                    media_ctx->is_current_fragment_sent = 1;
                }
                else {
                    /* this is a new object. The fragment should be processed. */
                    ret = quicrq_relay_datagram_publisher_object_eval(stream_ctx, media_ctx, should_skip);
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
int quicrq_relay_datagram_publisher_object_prune(
    quicrq_relay_publisher_context_t* media_ctx)
{
    int ret = 0;
    quicrq_relay_publisher_object_state_t* first_object = (quicrq_relay_publisher_object_state_t*)
        quicrq_relay_publisher_object_node_value(picosplay_first(&media_ctx->publisher_object_tree));

    while (first_object != NULL && first_object->is_sent) {
        quicrq_relay_publisher_object_state_t* next_object = (quicrq_relay_publisher_object_state_t*)
            quicrq_relay_publisher_object_node_value(picosplay_next(&first_object->publisher_object_node));
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
int quicrq_relay_datagram_publisher_object_update(
    quicrq_relay_publisher_context_t* media_ctx,
    int should_skip,
    int is_last_fragment,
    uint64_t next_offset,
    size_t copied )
{
    int ret = 0;
    quicrq_relay_publisher_object_state_t* publisher_object = 
        quicrq_relay_publisher_object_get(media_ctx, media_ctx->current_fragment->group_id, media_ctx->current_fragment->object_id);
    if (publisher_object == NULL) {
        publisher_object = quicrq_relay_publisher_object_add(media_ctx,
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
            ret = quicrq_relay_datagram_publisher_object_prune(media_ctx);
        }
    }

    return ret;
}

/* Send the next fragment, or a placeholder if the object shall be skipped. 
 */
int quicrq_relay_datagram_publisher_send_fragment(
    quicrq_stream_ctx_t* stream_ctx,
    quicrq_relay_publisher_context_t* media_ctx,
    uint64_t datagram_stream_id,
    void* context,
    size_t space,
    int* media_was_sent,
    int* at_least_one_active,
    int should_skip)
{
    int ret = 0;
    size_t offset = media_ctx->current_fragment->offset + media_ctx->length_sent;
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
                                media_ctx->current_fragment->object_id, offset,
                                media_ctx->current_fragment->nb_objects_previous_group,
                                ((uint8_t*)buffer) + h_size, copied,
                                media_ctx->current_fragment->queue_delay, is_last_fragment, NULL,
                                picoquic_get_quic_time(stream_ctx->cnx_ctx->qr_ctx->quic));
                            if (ret != 0) {
                                DBG_PRINTF("Datagram ack init returns %d", ret);
                            }
                        }
                        if (ret == 0) {
                            ret = quicrq_relay_datagram_publisher_object_update(media_ctx,
                                should_skip, is_last_fragment, offset + copied, copied);
                        }
                    }
                }
            }
        }
    }

    return ret;
}

int quicrq_relay_datagram_publisher_prepare(
    quicrq_stream_ctx_t* stream_ctx,
    quicrq_relay_publisher_context_t* media_ctx,
    uint64_t datagram_stream_id,
    void* context,
    size_t space,
    int* media_was_sent,
    int* at_least_one_active,
    int* not_ready)
{
    /* First, check if there is something to send. */
    int ret;
    int should_skip = 0;

    *media_was_sent = 0;
    *not_ready = 0;
    
    /* Evaluate fragment and congestion */
    ret = quicrq_relay_datagram_publisher_check_fragment(stream_ctx, media_ctx, &should_skip);
    if (ret != 0 || media_ctx->current_fragment == NULL || media_ctx->is_current_fragment_sent) {
        *not_ready = 1;
    }
    else  {
        /* Then send the object */
        if (ret == 0) {
            ret = quicrq_relay_datagram_publisher_send_fragment(stream_ctx, media_ctx, datagram_stream_id,
                context, space, media_was_sent, at_least_one_active, should_skip);
        }
    }
    return ret;
}

int quicrq_relay_datagram_publisher_fn(
    quicrq_stream_ctx_t* stream_ctx,
    void* context,
    size_t space,
    int* media_was_sent,
    int* at_least_one_active)
{
    int ret = 0;
    int not_ready = 0;
    quicrq_relay_publisher_context_t* media_ctx = (quicrq_relay_publisher_context_t*)stream_ctx->media_ctx;

    /* The "prepare" function has no dependency on stream context,
     * which helps designing unit tests.
     */
    ret = quicrq_relay_datagram_publisher_prepare(stream_ctx, media_ctx,
        stream_ctx->datagram_stream_id, context, space, media_was_sent, at_least_one_active, &not_ready);

    if (not_ready){
        /* Nothing to send at this point. If the media sending is finished, mark the stream accordingly.
         * The cache filling function checks that the final ID is only marked when all fragments have been
         * received. At this point, we only check that the final ID is marked, and all fragments have 
         * been sent.
         */

        if ((media_ctx->cache_ctx->final_group_id != 0 || media_ctx->cache_ctx->final_object_id != 0) &&
            media_ctx->current_fragment != NULL &&
            media_ctx->length_sent >= media_ctx->current_fragment->data_length &&
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

void* quicrq_relay_publisher_subscribe(void* v_srce_ctx, quicrq_stream_ctx_t * stream_ctx)
{
    quicrq_relay_cached_media_t* cache_ctx = (quicrq_relay_cached_media_t*)v_srce_ctx;
    quicrq_relay_publisher_context_t* media_ctx = (quicrq_relay_publisher_context_t*)
        malloc(sizeof(quicrq_relay_publisher_context_t));
    if (media_ctx != NULL) {
        memset(media_ctx, 0, sizeof(quicrq_relay_publisher_context_t));
        media_ctx->cache_ctx = cache_ctx;
        if (stream_ctx != NULL) {
            stream_ctx->start_object_id = cache_ctx->first_object_id;
        }
        picosplay_init_tree(&media_ctx->publisher_object_tree, quicrq_relay_publisher_object_node_compare,
            quicrq_relay_publisher_object_node_create, quicrq_relay_publisher_object_node_delete,
            quicrq_relay_publisher_object_node_value);
    }
    return media_ctx;
}

void quicrq_relay_publisher_delete(void* v_pub_ctx)
{
    quicrq_relay_cached_media_t* cache_ctx = (quicrq_relay_cached_media_t*)v_pub_ctx;
    quicrq_relay_cache_media_clear(cache_ctx);
    free(cache_ctx);
}

/* Default source is called when a client of a relay is loading a not-yet-cached
 * URL. This requires creating the desired URL, and then opening the stream to
 * the server. Possibly, starting a connection if there is no server available.
 */

int quicrq_relay_check_server_cnx(quicrq_relay_context_t* relay_ctx, quicrq_ctx_t* qr_ctx)
{
    int ret = 0;
    /* If there is no valid connection to the server, create one. */
    /* TODO: check for expiring connection */
    if (relay_ctx->cnx_ctx == NULL) {
        relay_ctx->cnx_ctx = quicrq_create_client_cnx(qr_ctx, relay_ctx->sni,
            (struct sockaddr*)&relay_ctx->server_addr);
    }
    if (relay_ctx->cnx_ctx == NULL) {
        ret = -1;
    }
    return ret;
}

quicrq_relay_cached_media_t* quicrq_relay_create_cache_ctx(quicrq_ctx_t* qr_ctx)
{
    quicrq_relay_cached_media_t* cache_ctx = (quicrq_relay_cached_media_t*)malloc(
        sizeof(quicrq_relay_cached_media_t));
    if (cache_ctx != NULL) {
        memset(cache_ctx, 0, sizeof(quicrq_relay_cached_media_t));
        cache_ctx->subscribe_stream_id = UINT64_MAX;
        quicrq_relay_cache_media_init(cache_ctx);
        cache_ctx->qr_ctx = qr_ctx;
    }
    return cache_ctx;
}

quicrq_relay_consumer_context_t* quicrq_relay_create_cons_ctx()
{
    quicrq_relay_consumer_context_t* cons_ctx = (quicrq_relay_consumer_context_t*)
        malloc(sizeof(quicrq_relay_consumer_context_t));
    if (cons_ctx != NULL) {
        memset(cons_ctx, 0, sizeof(quicrq_relay_consumer_context_t));
    }
    return cons_ctx;
}

int quicrq_relay_publish_cached_media(quicrq_ctx_t* qr_ctx,
    quicrq_relay_cached_media_t* cache_ctx, const uint8_t* url, const size_t url_length)
{
    /* if succeeded, publish the source */
    cache_ctx->srce_ctx = quicrq_publish_datagram_source(qr_ctx, url, url_length, cache_ctx,
        quicrq_relay_publisher_subscribe, quicrq_relay_publisher_fn, quicrq_relay_datagram_publisher_fn,
        quicrq_relay_publisher_delete);
    return (cache_ctx->srce_ctx == NULL)?-1:0;
}

int quicrq_relay_default_source_fn(void* default_source_ctx, quicrq_ctx_t* qr_ctx,
    const uint8_t* url, const size_t url_length)
{
    int ret = 0;
    /* Should there be a single context type for relays and origin? */
    quicrq_relay_context_t* relay_ctx = (quicrq_relay_context_t*)default_source_ctx;
    if (url == NULL) {
        /* By convention, this is a request to release the resource of the origin */
        quicrq_set_default_source(qr_ctx, NULL, NULL);
    }
    else {
        quicrq_relay_cached_media_t* cache_ctx = quicrq_relay_create_cache_ctx(qr_ctx);
        quicrq_relay_consumer_context_t* cons_ctx = NULL;
        if (cache_ctx == NULL) {
            ret = -1;
        }
        else if (!relay_ctx->is_origin_only) {
            /* If there is no valid connection to the server, create one. */
            ret = quicrq_relay_check_server_cnx(relay_ctx, qr_ctx);

            if (ret == 0) {
                /* Create a consumer context for the relay to server connection */
                cons_ctx = quicrq_relay_create_cons_ctx();

                if (cons_ctx == NULL) {
                    ret = -1;
                }
                else {
                    cons_ctx->cached_ctx = cache_ctx;

                    /* Request a URL on a new stream on that connection */
                    ret = quicrq_cnx_subscribe_media(relay_ctx->cnx_ctx, url, url_length,
                        relay_ctx->use_datagrams, quicrq_relay_consumer_cb, cons_ctx);
                    if (ret == 0){
                        /* Document the stream ID for that cache */
                        char buffer[256];
                        cache_ctx->subscribe_stream_id = relay_ctx->cnx_ctx->last_stream->stream_id; 
                        picoquic_log_app_message(relay_ctx->cnx_ctx->cnx, "Asking server for URL: %s on stream %" PRIu64,
                            quicrq_uint8_t_to_text(url, url_length, buffer, 256), cache_ctx->subscribe_stream_id);
                    }
                }
            }
        }
        else {
            /* TODO: whatever is needed for origin only behavior. */
        }

        if (ret == 0) {
            /* if succeeded, publish the source */
            ret = quicrq_relay_publish_cached_media(qr_ctx, cache_ctx, url, url_length);
        }

        if (ret != 0) {
            if (cache_ctx != NULL) {
                free(cache_ctx);
            }
            if (cons_ctx != NULL) {
                free(cons_ctx);
            }
        }
    }
    return ret;
}

/* The relay consumer callback is called when receiving a "post" request from
 * a client. It will initialize a cached media context for the posted url.
 * The media will be received on the specified stream, as either stream or datagram.
 * The media shall be stored in a local cache entry.
 * The cached entry shall be pushed on a connection to the server.
 */

int quicrq_relay_consumer_init_callback(quicrq_stream_ctx_t* stream_ctx, const uint8_t* url, size_t url_length)
{
    int ret = 0;
    quicrq_ctx_t* qr_ctx = stream_ctx->cnx_ctx->qr_ctx;
    quicrq_relay_context_t* relay_ctx = (quicrq_relay_context_t*)qr_ctx->default_source_ctx;

    quicrq_relay_cached_media_t* cache_ctx = NULL;
    quicrq_relay_consumer_context_t* cons_ctx = NULL;

    /* If there is no valid connection to the server, create one. */
    ret = quicrq_relay_check_server_cnx(relay_ctx, qr_ctx);
    if (ret == 0) {
        quicrq_media_source_ctx_t* srce_ctx = quicrq_find_local_media_source(qr_ctx, url, url_length);

        if (srce_ctx != NULL) {
            cache_ctx = (quicrq_relay_cached_media_t*)srce_ctx->pub_ctx;
            if (cache_ctx == NULL) {
                ret = -1;
            }
            else {
                /* Abandon the stream that was open to receive the media */
                char buffer[256];
                quicrq_cnx_abandon_stream_id(relay_ctx->cnx_ctx, cache_ctx->subscribe_stream_id);
                picoquic_log_app_message(stream_ctx->cnx_ctx->cnx, "Abandon subscription to URL: %s",
                    quicrq_uint8_t_to_text(url, url_length, buffer, 256));
            }
        }
        else {
            /* Create a cache context for the URL */
            cache_ctx = quicrq_relay_create_cache_ctx(qr_ctx);
            if (cache_ctx != NULL) {
                char buffer[256];
                ret = quicrq_relay_publish_cached_media(qr_ctx, cache_ctx, url, url_length);
                picoquic_log_app_message(stream_ctx->cnx_ctx->cnx, "Create cache for URL: %s",
                    quicrq_uint8_t_to_text(url, url_length, buffer, 256));
                if (ret != 0) {
                    /* Could not publish the media, free the resource. */
                    free(cache_ctx);
                    cache_ctx = NULL;
                    ret = -1;
                }
            }
        }

        if (ret == 0) {
            cons_ctx = quicrq_relay_create_cons_ctx();
            if (cons_ctx == NULL) {
                ret = -1;
            }
            else {
                ret = quicrq_cnx_post_media(relay_ctx->cnx_ctx, url, url_length, relay_ctx->use_datagrams);
                if (ret != 0) {
                    /* TODO: unpublish the media context */
                    DBG_PRINTF("Should unpublish media context, ret = %d", ret);
                }
                else {
                    /* set the parameter in the stream context. */
                    char buffer[256];

                    cons_ctx->cached_ctx = cache_ctx;
                    ret = quicrq_set_media_stream_ctx(stream_ctx, quicrq_relay_consumer_cb, cons_ctx);
                    picoquic_log_app_message(stream_ctx->cnx_ctx->cnx, "Posting URL: %s to server on stream %" PRIu64,
                        quicrq_uint8_t_to_text(url, url_length, buffer, 256), stream_ctx->stream_id);
                }
            }
        }
    }

    return ret;
}



/* Management of subscriptions on relays.
 *
 * Every subscription managed from a client should have a corresponding subscription
 * request from the relay to the origin.
 */

int quicrq_relay_subscribe_notify(void* notify_ctx, const uint8_t* url, size_t url_length)
{
    int ret = 0;
    /* Retrieve the relay context */
    quicrq_ctx_t* qr_ctx = (quicrq_ctx_t*)notify_ctx;
    /* Find whether there is already a source with that name */
    quicrq_media_source_ctx_t* srce_ctx = qr_ctx->first_source;

    while (srce_ctx != NULL) {
        if (srce_ctx->media_url_length == url_length &&
            memcmp(srce_ctx->media_url, url, url_length) == 0) {
            break;
        }
        srce_ctx = srce_ctx->next_source;
    }
    if (srce_ctx == NULL) {
        /* If there is not, add the corresponding file to the catch, as
         * if a subscribe to a file had been received. */
        ret = quicrq_relay_default_source_fn(qr_ctx->relay_ctx, qr_ctx, url, url_length);
    }

    return ret;
}

quicrq_stream_ctx_t* quicrq_relay_find_subscription(quicrq_ctx_t* qr_ctx, const uint8_t* url, size_t url_length)
{
    quicrq_stream_ctx_t* stream_ctx = NULL;
    /* Locate the connection to the origin */
    if (qr_ctx->relay_ctx->cnx_ctx != NULL) {
        stream_ctx = qr_ctx->relay_ctx->cnx_ctx->first_stream;
        while (stream_ctx != NULL) {
            if (stream_ctx->subscribe_prefix != NULL &&
                stream_ctx->subscribe_prefix_length == url_length &&
                memcmp(stream_ctx->subscribe_prefix, url, url_length) == 0) {
                break;
            }
            stream_ctx = stream_ctx->next_stream;
        }
    }
    return stream_ctx;
}

void quicrq_relay_subscribe_pattern(quicrq_ctx_t* qr_ctx, quicrq_subscribe_action_enum action, const uint8_t* url, size_t url_length)
{
    if (action == quicrq_subscribe_action_unsubscribe) {
        if (qr_ctx->relay_ctx->cnx_ctx != NULL) {
            /* Check whether there is still a client connection subscribed to this pattern */
            quicrq_cnx_ctx_t* cnx_ctx = qr_ctx->first_cnx;
            int is_subscribed = 0;
            while (cnx_ctx != NULL && !is_subscribed) {
                /* Only examine the connections to this relay */
                if (cnx_ctx->is_server) {
                    quicrq_stream_ctx_t* stream_ctx = cnx_ctx->first_stream;
                    while (stream_ctx != NULL) {
                        if (stream_ctx->send_state == quicrq_notify_ready &&
                            stream_ctx->subscribe_prefix_length == url_length &&
                            memcmp(stream_ctx->subscribe_prefix, url, url_length) == 0) {
                            is_subscribed = 1;
                            break;
                        }
                        stream_ctx = stream_ctx->next_stream;
                    }
                }
            }
            /* Find the outgoing stream for that pattern and close it. */
            if (is_subscribed) {
                quicrq_stream_ctx_t* stream_ctx = quicrq_relay_find_subscription(qr_ctx, url, url_length);
                if (stream_ctx != NULL) {
                    int ret = quicrq_cnx_subscribe_pattern_close(qr_ctx->relay_ctx->cnx_ctx, stream_ctx);
                    if (ret != 0) {
                        char buffer[256];
                        quicrq_log_message(qr_ctx->relay_ctx->cnx_ctx, "Cannot unsubscribe relay from origin for %s*",
                            quicrq_uint8_t_to_text(url, url_length, buffer, 256));
                    }
                }
            }
        }
    }
    else if (action == quicrq_subscribe_action_subscribe) {
        /* new subscription from a client. Check the current connection to
         * see whether a matching subscription exists.*/
         /* If no connection to the server yet, create one */
        if (quicrq_relay_check_server_cnx(qr_ctx->relay_ctx, qr_ctx) != 0) {
            DBG_PRINTF("%s", "Cannot create a connection to the origin");
        }
        else {
            quicrq_stream_ctx_t* stream_ctx = quicrq_relay_find_subscription(qr_ctx, url, url_length);
            if (stream_ctx == NULL) {
                /* No subscription, create one. */
                stream_ctx = quicrq_cnx_subscribe_pattern(qr_ctx->relay_ctx->cnx_ctx, url, url_length,
                    quicrq_relay_subscribe_notify, qr_ctx);
            }

            if (stream_ctx == NULL) {
                char buffer[256];
                quicrq_log_message(qr_ctx->relay_ctx->cnx_ctx, "Cannot subscribe from relay to origin for %s*",
                    quicrq_uint8_t_to_text(url, url_length, buffer, 256));
            }
        }
    }
}

/* The relay functionality has to be established to add the relay
 * function to a QUICRQ node.
 */
int quicrq_enable_relay(quicrq_ctx_t* qr_ctx, const char* sni, const struct sockaddr* addr, int use_datagrams)
{
    int ret = 0;

    if (qr_ctx->relay_ctx != NULL) {
        /* Error -- cannot enable relaying twice without first disabling it */
        ret = -1;
    }
    else {
        size_t sni_len = (sni == NULL) ? 0 : strlen(sni);
        quicrq_relay_context_t* relay_ctx = (quicrq_relay_context_t*)malloc(
            sizeof(quicrq_relay_context_t) + sni_len + 1);
        if (relay_ctx == NULL) {
            ret = -1;
        }
        else {
            /* initialize the relay context. */
            uint8_t* v_sni = ((uint8_t*)relay_ctx) + sizeof(quicrq_relay_context_t);
            memset(relay_ctx, 0, sizeof(quicrq_relay_context_t));
            picoquic_store_addr(&relay_ctx->server_addr, addr);
            if (sni_len > 0) {
                memcpy(v_sni, sni, sni_len);
            }
            v_sni[sni_len] = 0;
            relay_ctx->sni = (char const*)v_sni;
            relay_ctx->use_datagrams = use_datagrams;
            /* set the relay as default provider */
            quicrq_set_default_source(qr_ctx, quicrq_relay_default_source_fn, relay_ctx);
            /* set a default post client on the relay */
            quicrq_set_media_init_callback(qr_ctx, quicrq_relay_consumer_init_callback);
            qr_ctx->relay_ctx = relay_ctx;
            qr_ctx->manage_relay_cache_fn = quicrq_manage_relay_cache;
            qr_ctx->manage_relay_subscribe_fn = quicrq_relay_subscribe_pattern;
        }
    }
    return ret;
}

void quicrq_disable_relay(quicrq_ctx_t* qr_ctx)
{
    if (qr_ctx->relay_ctx != NULL) {
        free(qr_ctx->relay_ctx);
        qr_ctx->relay_ctx = NULL;
        qr_ctx->manage_relay_cache_fn = NULL;
    }
}

/* Management of the relay cache.
 * Ensure that old segments are removed.
 */
uint64_t quicrq_manage_relay_cache(quicrq_ctx_t* qr_ctx, uint64_t current_time)
{
    uint64_t next_time = UINT64_MAX;

    if (qr_ctx->relay_ctx != NULL && (qr_ctx->cache_duration_max > 0 || qr_ctx->is_cache_closing_needed)) {
        int is_cache_closing_still_needed = 0;
        quicrq_media_source_ctx_t* srce_ctx = qr_ctx->first_source;

        /* Find all the sources that are cached by the relay function */
        while (srce_ctx != NULL) {
            quicrq_media_source_ctx_t* srce_to_delete = NULL;
            if (srce_ctx->subscribe_fn == quicrq_relay_publisher_subscribe &&
                srce_ctx->getdata_fn == quicrq_relay_publisher_fn &&
                srce_ctx->get_datagram_fn == quicrq_relay_datagram_publisher_fn &&
                srce_ctx->delete_fn == quicrq_relay_publisher_delete) {
                /* This is a source created by the relay */
                quicrq_relay_cached_media_t* cache_ctx = (quicrq_relay_cached_media_t*)srce_ctx->pub_ctx;

                if (qr_ctx->cache_duration_max > 0) {
                    /* TODO: Check the lowest value of the published object along subscribed clients;
                     * setting the lowest value to UIN64_MAX for now */
                     /* Purge cache from old entries */
                    quicrq_relay_cache_media_purge(cache_ctx,
                        current_time, qr_ctx->cache_duration_max, UINT64_MAX);
                }
                if (cache_ctx->is_closed) {
                    if (cache_ctx->first_fragment == NULL) {
                        /* If the cache is empty and the source is closed, schedule it for deletion. */
                        srce_to_delete = srce_ctx;
                    } else if (srce_ctx->first_stream == NULL) {
                        /* If the source is closed and has no reader, delete at scheduled time. */
                        if (current_time >= cache_ctx->cache_delete_time) {
                            srce_to_delete = srce_ctx;
                        }
                        else if (cache_ctx->cache_delete_time < next_time) {
                            /* Not ready to delete yet, ask for a wake up on timer */
                            next_time = cache_ctx->cache_delete_time;
                            is_cache_closing_still_needed = 1;
                        }
                    }
                }
            }
            srce_ctx = srce_ctx->next_source;
            if (srce_to_delete != NULL) {
                quicrq_delete_source(srce_to_delete, qr_ctx);
            }
        }
        qr_ctx->is_cache_closing_needed = is_cache_closing_still_needed;
    }

    return next_time;
}

/*
 * The origin server behavior is very similar to the behavior of a relay, but
 * there are some key differences:
 *  
 *  1) When receiving a "subscribe" request, the relay creates the media context and
 *     starts a connection. The server creates a media context but does not start the connection. 
 *  2) When receiving a "post" request, the relay creates a cache version and also
 *     forwards it to the server using an upload connection. There is no upload
 *     connection at the origin server. 
 *  3) When receiving a "post" request, the server must check whether the media context 
 *     already exists, and if it does connects it.
 */

int quicrq_origin_consumer_init_callback(quicrq_stream_ctx_t* stream_ctx, const uint8_t* url, size_t url_length)
{
    int ret = 0;
    quicrq_ctx_t* qr_ctx = stream_ctx->cnx_ctx->qr_ctx;
    quicrq_relay_cached_media_t* cache_ctx = NULL;
    quicrq_relay_consumer_context_t* cons_ctx = quicrq_relay_create_cons_ctx();
    char buffer[256];

    if (cons_ctx == NULL) {
        ret = -1;
    } else {
        /* Check whether there is already a context for this media */
        quicrq_media_source_ctx_t* srce_ctx = quicrq_find_local_media_source(qr_ctx, url, url_length);

        if (srce_ctx != NULL) {
            cache_ctx = (quicrq_relay_cached_media_t*)srce_ctx->pub_ctx;
            picoquic_log_app_message(stream_ctx->cnx_ctx->cnx, "Found cache context for URL: %s",
                quicrq_uint8_t_to_text(url, url_length, buffer, 256));
        }
        else {
            /* Create a cache context for the URL */
            cache_ctx = quicrq_relay_create_cache_ctx(qr_ctx);
            if (cache_ctx != NULL) {
                ret = quicrq_relay_publish_cached_media(qr_ctx, cache_ctx, url, url_length);
                if (ret != 0) {
                    /* Could not publish the media, free the resource. */
                    free(cache_ctx);
                    cache_ctx = NULL;
                    picoquic_log_app_message(stream_ctx->cnx_ctx->cnx, "Cannot create cache for URL: %s",
                        quicrq_uint8_t_to_text(url, url_length, buffer, 256));
                }
                else {
                    picoquic_log_app_message(stream_ctx->cnx_ctx->cnx, "Created cache context for URL: %s",
                        quicrq_uint8_t_to_text(url, url_length, buffer, 256));
                }
            }
        }

        if (ret == 0) {
            /* set the parameter in the stream context. */
            cons_ctx->cached_ctx = cache_ctx;
            ret = quicrq_set_media_stream_ctx(stream_ctx, quicrq_relay_consumer_cb, cons_ctx);
        }

        if (ret != 0) {
            free(cons_ctx);
        }
    }
    return ret;
}




int quicrq_enable_origin(quicrq_ctx_t* qr_ctx, int use_datagrams)
{
    int ret = 0;
    quicrq_relay_context_t* relay_ctx = (quicrq_relay_context_t*)malloc(
        sizeof(quicrq_relay_context_t));
    if (relay_ctx == NULL) {
        ret = -1;
    }
    else {
        /* initialize the relay context. */
        memset(relay_ctx, 0, sizeof(quicrq_relay_context_t));
        relay_ctx->use_datagrams = use_datagrams;
        relay_ctx->is_origin_only = 1;
        /* set the relay as default provider */
        quicrq_set_default_source(qr_ctx, quicrq_relay_default_source_fn, relay_ctx);
        /* set a default post client on the relay */
        quicrq_set_media_init_callback(qr_ctx, quicrq_origin_consumer_init_callback);
        /* Remember pointer */
        qr_ctx->relay_ctx = relay_ctx;
        /* Set the cache function */
        qr_ctx->manage_relay_cache_fn = quicrq_manage_relay_cache;
    }
    return ret;
}
