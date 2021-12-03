/* Handling of a relay
 */
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include "picoquic_utils.h"
#include "picosplay.h"
#include "quicrq.h"
#include "quicrq_reassembly.h"
#include "quicrq_internal.h"
#include "quicrq_relay.h"

/* A relay is a specialized node, acting both as client when acquiring a media
 * segment and as server when producing data.
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
 * The client half creates a list of media frames. For simplification, the server half will
 * only deal with the media frames that are fully received. When a media frame is
 * fully received, it becomes available. We may consider a difference in
 * availability between "in-order" and "out-of-sequence" availablity, which
 * may need to be reflected in the contract between connection and sources.
 */

 /* Relay definitions.
  * When acting as a client, the relay adds the media to a cache, which can then be read by the
  * server part of the relay. The publisher state indicates the current frame being read from the
  * cache, and the state of the response.
  *
  * In the current version, the cached media is represented in memory by an array of frames,
  * identified by a frame number. We may need to add metadata later,such as adding a timestamp
  * to a frame, or marking frames as potential restart point, potential skip on restart,
  * and maybe an indication of encoding layer.
  *
  * The frames are added when received on a client connection, organized as a binary tree.
  * The relayed media is kept until the end of the relay connection. This is of course not
  * sustainable, some version of cache management will have to be added later.
  *
  */

typedef struct st_quicrq_relay_cached_frame_t {
    picosplay_node_t frame_node;
    uint64_t frame_id;
    uint8_t* data;
    size_t data_length;
} quicrq_relay_cached_frame_t;

typedef struct st_quicrq_relay_cached_media_t {
    quicrq_media_source_ctx_t* srce_ctx;
    uint64_t final_frame_id;
    picosplay_tree_t frame_tree;
} quicrq_relay_cached_media_t;

typedef struct st_quicrq_relay_publisher_context_t {
    quicrq_relay_cached_media_t* cache_ctx;
    uint64_t current_frame_id;
    size_t current_offset;
    int is_frame_complete;
    int is_media_complete;
} quicrq_relay_publisher_context_t;

typedef struct st_quicrq_relay_consumer_context_t {
    quicrq_relay_cached_media_t* cached_ctx;
    quicrq_reassembly_context_t reassembly_ctx;
    int is_finished : 1;
} quicrq_relay_consumer_context_t;

typedef struct st_quicrq_relay_context_t {
    const char* sni;
    struct sockaddr_storage server_addr;
    quicrq_ctx_t* qr_ctx;
    quicrq_cnx_ctx_t* cnx_ctx;
    int use_datagrams : 1;
} quicrq_relay_context_t;

/* manage the splay of cached frames */

static void* quicrq_relay_cache_frame_node_value(picosplay_node_t* frame_node)
{
    return (frame_node == NULL) ? NULL : (void*)((char*)frame_node - offsetof(struct st_quicrq_relay_cached_frame_t, frame_node));
}

static int64_t quicrq_relay_cache_frame_node_compare(void* l, void* r) {
    return (int64_t)((quicrq_relay_cached_frame_t*)l)->frame_id - ((quicrq_relay_cached_frame_t*)r)->frame_id;
}

static picosplay_node_t* quicrq_relay_cache_frame_node_create(void* v_media_frame)
{
    return &((quicrq_relay_cached_frame_t*)v_media_frame)->frame_node;
}

static void quicrq_relay_cache_frame_node_delete(void* tree, picosplay_node_t* node)
{
#ifdef _WINDOWS
    UNREFERENCED_PARAMETER(tree);
#endif
    free(quicrq_relay_cache_frame_node_value(node));
}

void quicrq_relay_cache_media_init(quicrq_relay_cached_media_t* cached_media)
{
    picosplay_init_tree(&cached_media->frame_tree, quicrq_relay_cache_frame_node_compare,
        quicrq_relay_cache_frame_node_create, quicrq_relay_cache_frame_node_delete,
        quicrq_relay_cache_frame_node_value);
}

quicrq_relay_cached_frame_t* quicrq_relay_cache_frame_get(quicrq_relay_cached_media_t* cached_media, uint64_t frame_id)
{
    quicrq_relay_cached_frame_t key = { 0 };
    quicrq_relay_cached_frame_t * result = NULL;
    picosplay_node_t * found;
    key.frame_id = frame_id;
    found = picosplay_find(&cached_media->frame_tree, &key);
    result = quicrq_relay_cache_frame_node_value(found);
    if (result != NULL && result->frame_id != frame_id) {
        result = NULL;
    }
    return result;
}

/* Client part of the relay.
 * The connection is started when a context is specialized to become a relay
 */

int quicrq_relay_consumer_frame_ready(
    void* media_ctx,
    uint64_t current_time,
    uint64_t frame_id,
    const uint8_t* data,
    size_t data_length,
    quicrq_reassembly_frame_mode_enum frame_mode)
{
    /* Callback from the reassembly function when a frame is ready.
     * TODO: If this is a new frame, add it to the cache. */
    int ret = 0;
    quicrq_relay_consumer_context_t* cons_ctx = (quicrq_relay_consumer_context_t*)media_ctx;

    if (frame_mode != quicrq_reassembly_frame_repair) {
        quicrq_relay_cached_frame_t* frame = (quicrq_relay_cached_frame_t*)malloc(
            sizeof(quicrq_relay_cached_frame_t) + data_length);
        if (frame == NULL) {
            ret = -1;
        }
        else {
            memset(frame, 0, sizeof(quicrq_relay_cached_frame_t));
            frame->frame_id = frame_id;
            frame->data = ((uint8_t*)frame) + sizeof(quicrq_relay_cached_frame_t);
            frame->data_length = data_length;
            memcpy(frame->data, data, data_length);
            picosplay_insert(&cons_ctx->cached_ctx->frame_tree, frame);
        }
        /* wake up the clients waiting for data on this media */
        quicrq_source_wakeup(cons_ctx->cached_ctx->srce_ctx);
    }

    return ret;
}

int quicrq_relay_consumer_cb(
    quicrq_media_consumer_enum action,
    void* media_ctx,
    uint64_t current_time,
    const uint8_t* data,
    uint64_t frame_id,
    uint64_t offset,
    int is_last_segment,
    size_t data_length)
{
    int ret = 0;
    quicrq_relay_consumer_context_t * cons_ctx = (quicrq_relay_consumer_context_t*)media_ctx;

    switch (action) {
    case quicrq_media_datagram_ready:
        ret = quicrq_reassembly_input(&cons_ctx->reassembly_ctx, current_time, data, frame_id, offset,
            is_last_segment, data_length, quicrq_relay_consumer_frame_ready, media_ctx);
        if (ret == 0 && cons_ctx->is_finished) {
            ret = quicrq_consumer_finished;
        }
        break;
    case quicrq_media_final_frame_id:
        ret = quicrq_reassembly_learn_final_frame_id(&cons_ctx->reassembly_ctx, frame_id);
        if (ret == 0) {
            cons_ctx->cached_ctx->final_frame_id = frame_id;
            if (cons_ctx->is_finished) {
                ret = quicrq_consumer_finished;
            }
            if (ret == 0) {
                /* wake up the clients waiting for data on this media */
                quicrq_source_wakeup(cons_ctx->cached_ctx->srce_ctx);
            }
        }
        break;
    case quicrq_media_close: 
        /* Document the final frame */
        cons_ctx->cached_ctx->final_frame_id = quicrq_reassembly_frame_id_last(&cons_ctx->reassembly_ctx);
        /* Notify consumers of the stream */
        quicrq_source_wakeup(cons_ctx->cached_ctx->srce_ctx);
        /* Free the media context resource */
        quicrq_reassembly_release(&cons_ctx->reassembly_ctx);
        free(media_ctx);
        break;
    default:
        ret = -1;
        break;
    }
    return ret;
}

/* Initialize a media acquisition context on client post */
int quicrq_relay_consumer_init_fn(const uint8_t* url, size_t url_length)
{

    int ret = 0;
#if 0
    /* Create a media context for the URL */
    void* media_ctx;
    char result_file_name[512];
    char result_log_name[512];

    ret = test_media_derive_file_names(url, url_length, stream_ctx->is_datagram, 1, 1, result_file_name, result_log_name, sizeof(result_file_name));

    if (ret == 0) {
        /* Init the local media consumer */
        media_ctx = test_media_consumer_init(result_file_name, result_log_name);

        if (media_ctx == NULL) {
            ret = -1;
        }
        else
        {
            /* set the parameter in the stream context. */
            ret = quicrq_set_media_stream_ctx(stream_ctx, test_media_frame_consumer_cb, media_ctx);
        }
    }
#endif

    return ret;
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
 * for the new frame-id, offset zero.
 * When the correction is available, the client is notified, and polls for the
 * missing frame-id.
 */

void quicrq_relay_publisher_close(quicrq_relay_publisher_context_t* media_ctx)
{
    free(media_ctx);
}

int quicrq_relay_publisher_fn(
    quicrq_media_source_action_enum action,
    void* v_media_ctx,
    uint8_t* data,
    size_t data_max_size,
    size_t* data_length,
    int* is_last_segment,
    int* is_media_finished,
    uint64_t current_time)
{
    int ret = 0;
    /* TO DO: more complex behavior when data is not received in order. */

    quicrq_relay_publisher_context_t* media_ctx = (quicrq_relay_publisher_context_t*)v_media_ctx;
    if (action == quicrq_media_source_get_data) {
        *is_media_finished = 0;
        *is_last_segment = 0;
        *data_length = 0;
        if (media_ctx->cache_ctx->final_frame_id != 0 && media_ctx->current_frame_id >= media_ctx->cache_ctx->final_frame_id) {
            *is_media_finished = 1;
        }
        else {
            /* Access the current frame */
            quicrq_relay_cached_frame_t* frame = quicrq_relay_cache_frame_get(media_ctx->cache_ctx, media_ctx->current_frame_id);
            if (frame != NULL) {
                /* Copy data from frame in memory */
                size_t available = frame->data_length - media_ctx->current_offset;
                size_t copied = data_max_size;
                if (data_max_size >= available) {
                    *is_last_segment = 1;
                    copied = available;
                }
                *data_length = copied;
                if (data != NULL) {
                    /* If data is set to NULL, return the available size but do not copy anything */
                    memcpy(data, frame->data + media_ctx->current_offset, copied);
                    media_ctx->current_offset += copied;
                    if (media_ctx->current_offset >= frame->data_length) {
                        media_ctx->current_frame_id++;
                        media_ctx->current_offset = 0; 
                    }
                }
            }
            else {
                /* Error, data is not yet available */
            }
        }
    }
    else if (action == quicrq_media_source_close) {
        /* close the context */
        quicrq_relay_publisher_close(media_ctx);
    }
    return ret;
}

void* quicrq_relay_publisher_subscribe(void* v_srce_ctx)
{
    quicrq_relay_cached_media_t* cache_ctx = (quicrq_relay_cached_media_t*)v_srce_ctx;
    quicrq_relay_publisher_context_t* media_ctx = (quicrq_relay_publisher_context_t*)
        malloc(sizeof(quicrq_relay_publisher_context_t));
    if (media_ctx != NULL) {
        memset(media_ctx, 0, sizeof(quicrq_relay_publisher_context_t));
        media_ctx->cache_ctx = cache_ctx;
    }
    return media_ctx;
}


/* Default source is called when a client of a relay is loading a not-yet-cached
 * URL. This requires creating the desired URL, and then opening the stream to
 * the server. Possibly, starting a connection if there is no server available.
 */


void quicr_relay_clear_ctx(quicrq_relay_context_t* relay_ctx)
{
    free(relay_ctx);
}

int quicrq_relay_default_source_fn(void* default_source_ctx, quicrq_ctx_t* qr_ctx,
    const uint8_t* url, const size_t url_length)
{
    int ret = 0;
    quicrq_relay_context_t* relay_ctx = (quicrq_relay_context_t*)default_source_ctx;
    if (url == NULL) {
        /* By convention, this is a request to release the resource of the relay */
        quicrq_set_default_source(qr_ctx, NULL, NULL);
    }
    else {
        /* If there is no valid connection to the server, create one. */
        /* TODO: check for expiring connection */
        if (relay_ctx->cnx_ctx == NULL) {
            relay_ctx->cnx_ctx = quicrq_create_client_cnx(qr_ctx, relay_ctx->sni,
                (struct sockaddr*)&relay_ctx->server_addr);
        }
        if (relay_ctx->cnx_ctx == NULL) {
            ret = -1;
        }
        else {
            /* Create a cache context for the URL */
            quicrq_relay_consumer_context_t* cons_ctx = (quicrq_relay_consumer_context_t*)
                malloc(sizeof(quicrq_relay_consumer_context_t));
            quicrq_relay_cached_media_t* cache_ctx = (quicrq_relay_cached_media_t*)malloc(
                sizeof(quicrq_relay_cached_media_t));
            if (cache_ctx == NULL || cons_ctx == NULL) {
                ret = -1;
            }
            else {
                memset(cache_ctx, 0, sizeof(quicrq_relay_cached_media_t));
                quicrq_relay_cache_media_init(cache_ctx);

                memset(cons_ctx, 0, sizeof(quicrq_relay_consumer_context_t));
                cons_ctx->cached_ctx = cache_ctx;
                quicrq_reassembly_init(&cons_ctx->reassembly_ctx);

                /* Request a URL on a new stream on that connection */
                ret = quicrq_cnx_subscribe_media(relay_ctx->cnx_ctx, url, url_length,
                    relay_ctx->use_datagrams, quicrq_relay_consumer_cb, cons_ctx);
            }

            if (ret == 0) {
                /* if succeeded, publish the source */
                ret = quicrq_publish_source(qr_ctx, url, url_length, cache_ctx,
                    quicrq_relay_publisher_subscribe, quicrq_relay_publisher_fn);
                if (ret == 0) {
                    /* Assume that the quicrq_publish_source function added the new source at the end of the list */
                    cache_ctx->srce_ctx = qr_ctx->last_source;
                }
            }

            if (ret != 0){
                if (cache_ctx != NULL){
                    free(cache_ctx);
                }
                if (cons_ctx != NULL) {
                    free(cons_ctx);
                }
            }
        }
    }
    return ret;
}

int quicrq_enable_relay(quicrq_ctx_t* qr_ctx, const char* sni, const struct sockaddr* addr, int use_datagrams)
{
    int ret = 0;
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
    }
    return ret;
}