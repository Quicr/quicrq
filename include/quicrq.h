#ifndef QUICRQ_H
#define QUICRQ_H

#include <stdint.h>
#include <picoquic.h>

#ifdef __cplusplus
extern "C" {
#endif

/* QUICR ALPN and QUICR port -- as defined in draft */
#define QUICRQ_ALPN "quicr-h00"
#define QUICRQ_PORT 853

/* QUICR error codes */
#define QUICRQ_ERROR_NO_ERROR 0x00
#define QUICRQ_ERROR_INTERNAL 0x01
#define QUICRQ_ERROR_PROTOCOL 0x02

/* QUICR client return codes
*/
typedef enum {
    quicrq_incoming_query = 0, /* Incoming callback query */
    quicrq_query_cancelled, /* Query cancelled before response provided */
    quicrq_response_complete, /* The last response to the current query arrived. */
    quicrq_response_partial, /* One of the first responses to a query has arrived */
    quicrq_response_cancelled, /* The response to the current query was cancelled by the peer. */
    quicrq_query_failed  /* Query failed for reasons other than cancelled. */
} quicrq_query_return_enum;


/* Connection context management functions.
 * The type quicrq_ctx_t is treated here as an opaque pointer, to
 * provide isolation between the app and the stack.
 */

typedef struct st_quicrq_ctx_t quicrq_ctx_t;
typedef struct st_quicrq_cnx_ctx_t quicrq_cnx_ctx_t;
typedef struct st_quicrq_stream_ctx_t quicrq_stream_ctx_t;

quicrq_ctx_t* quicrq_create_empty();
void quicrq_set_quic(quicrq_ctx_t* qr_ctx, picoquic_quic_t* quic);
quicrq_ctx_t* quicrq_create(char const* alpn,
    char const* cert_file_name, char const* key_file_name, char const* cert_root_file_name,
    char const* ticket_store_file_name, char const* token_store_file_name,
    const uint8_t* ticket_encryption_key, size_t ticket_encryption_key_length,
    uint64_t* simulated_time);
void quicrq_delete(quicrq_ctx_t* ctx);
picoquic_quic_t* quicrq_get_quic_ctx(quicrq_ctx_t* ctx);
void quicrq_init_transport_parameters(picoquic_tp_t* tp, int client_mode);

quicrq_cnx_ctx_t* quicrq_create_cnx_context(quicrq_ctx_t* qr_ctx, picoquic_cnx_t* cnx);
quicrq_cnx_ctx_t* quicrq_create_client_cnx(quicrq_ctx_t* qr_ctx,
    const char* sni, struct sockaddr* addr);
void quicrq_delete_cnx_context(quicrq_cnx_ctx_t* cnx_ctx);

/* Media stream definition.
 * Media is composed of series of frames, frames have
 * headers and content. Header provides information
 * about sufficient for synchronization and replay.
 */
typedef struct st_quicrq_media_frame_header_t {
    uint64_t timestamp; /* time from start of media segment */
    uint64_t number; /* start at 1 for media segment */
    size_t length; /* number of content bytes */
} quicrq_media_frame_header_t;

/* Media publisher API.
 * 
 * Publisher connect a source to a local context by calling `quicrq_publish_source`.
 * This registers an URL for the source, and creates a source entry in the local context.
 * 
 * - qr_ctx: QUICR context in which the media is published
 * - url, url_length: URL of the media segment
 * - media_publisher_subscribe_fn: callback function for subscribing a new consumer to the media source.
 * - media_publisher_fn: callback function for processing media arrival
 * - media_ctx: media context managed by the publisher, specific to that URL.
 * 
 * When a subscribe request arrives, the stack looks for a media source, which could be
 * an actual source, or the cached version of the media published by another node
 * for that URL. (In relays and origin servers, a new cache entry is automatically
 * created upon the request to an URL.) Once the stack has identified the source
 * context, it will make a first call to the "subscribe" function, which will
 * return a "media context" specific to that source and that subscription.
 * 
 * After that, the stack will try to send the media as a series of frames, each
 * composed of a series of segments. The data is obtained by a series of calls
 * to the "publisher" function, with the following parameters:
 * 
 * - action: set to get data for retrieving data, or close to indicate end of
 *   the transmission. After a call to close, the media context can be freed.
 * - media_ctx: as produced by the call to the subscribe function.
 * - data: either NULL, or a pointer to the memory location where data shall
 *   be copied. (See below for the logic of calling the function twice)
 * - data_max_size: the space available at the memory location.
 * - &data_length: the data available to fill that space.
 * - &is_last_segment: whether this is the last segment in a frame
 * - &is_media_finished: whether there is no more data to send.
 * - current_time: time, in microseconds. (May be virtual time during simulations
 *   and tests.)
 * 
 * The stack will make two calls to fill a packet: a first call with "data" set
 * to NULL to learn the number of bytes available, and the value of "is_last_segment"
 * and "is_media_finished", and a second call to actually request the data. It is
 * essential that data_length, is_last_segment and is_media_finished are set to
 * the same value in both calls.
 * 
 * The media is sent as a series of frames. The stack inserts a small header in
 * front of each segment to specify the frame number, the offset in the frame,
 * and whether this is the last segment. This is used by the reassembly
 * processes (see quicrq_reassembly.h). Intermediate relay may wait until the
 * last segment is received to forward data belonging to a frame.
 */

typedef enum {
    quicrq_media_source_get_data = 0,
    quicrq_media_source_close
} quicrq_media_source_action_enum;

typedef void* (*quicrq_media_publisher_subscribe_fn)(void* pub_ctx);
typedef int (*quicrq_media_publisher_fn)(
    quicrq_media_source_action_enum action,
    void* media_ctx,
    uint8_t* data,
    size_t data_max_size,
    size_t* data_length,
    int* is_last_segment,
    int* is_media_finished,
    uint64_t current_time);
typedef void (*quicrq_media_publisher_delete_fn)(void* pub_ctx);

typedef struct st_quicrq_media_source_ctx_t quicrq_media_source_ctx_t;
quicrq_media_source_ctx_t* quicrq_publish_source(quicrq_ctx_t* qr_ctx, const uint8_t* url, size_t url_length,
    void* pub_ctx, quicrq_media_publisher_subscribe_fn subscribe_fn, 
    quicrq_media_publisher_fn getdata_fn, quicrq_media_publisher_delete_fn delete_fn);
int quicrq_close_source(quicrq_ctx_t* qr_ctx, const uint8_t* url, size_t url_length, 
    void* pub_ctx);
void quicrq_delete_source(quicrq_media_source_ctx_t* srce_ctx, quicrq_ctx_t* qr_ctx);

/* Management of default sources, used for example by proxies or relays.
 * The callback creates a context for the specified URL, returning the parameters that would be otherwise
 * specified in the function "quicrq_publish_source".
 * When a quicrq context is deleted, the stack calls the default source function one last time, setting
 * all parameters except the default_source_ctx to zero or NULL values. This gives the application
 * an opportunity to clear memory and resource used by the default publication function.
 */

typedef int (*quicrq_default_source_fn)(void * default_source_ctx, quicrq_ctx_t* qr_ctx, const uint8_t* url, const size_t url_length);
void quicrq_set_default_source(quicrq_ctx_t* qr_ctx, quicrq_default_source_fn default_source_fn, void * default_source_ctx);


 /* Quic media consumer.
  * The application sets a "media consumer function" and a "media consumer context" for
  * the media stream. On the client side, this is done by a call to "quicrq_cnx_subscribe_media"
  * which will trigger the opening of the media stream through the protocol.
  * 
  * For client published streams, the client uses "quicrq_cnx_post_media"
  * to start the media stream. The server will receive an initial command
  * containing the media URL, and use 
  */
typedef enum {
    quicrq_media_datagram_ready = 0,
    quicrq_media_final_frame_id,
    quicrq_media_close
} quicrq_media_consumer_enum;

#define quicrq_consumer_finished 1
#define quicrq_consumer_continue 0
#define quicrq_consumer_error -1

typedef int (*quicrq_media_consumer_fn)(
    quicrq_media_consumer_enum action,
    void* media_ctx,
    uint64_t current_time,
    const uint8_t* data,
    uint64_t frame_id,
    uint64_t offset,
    int is_last_segment,
    size_t data_length);

int quicrq_cnx_subscribe_media(quicrq_cnx_ctx_t* cnx_ctx,
    const uint8_t* url, size_t url_length, int use_datagrams,
    quicrq_media_consumer_fn media_consumer_fn, void* media_ctx);

int quicrq_cnx_post_media(quicrq_cnx_ctx_t* cnx_ctx, const uint8_t* url, size_t url_length,
    int use_datagrams);

typedef int (*quicrq_media_consumer_init_fn)(quicrq_stream_ctx_t* stream_ctx, const uint8_t* url, size_t url_length);
int quicrq_set_media_init_callback(quicrq_ctx_t* ctx, quicrq_media_consumer_init_fn media_init_fn);

int quicrq_set_media_stream_ctx(quicrq_stream_ctx_t* stream_ctx, quicrq_media_consumer_fn media_fn, void* media_ctx);

quicrq_cnx_ctx_t* quicrq_first_connection(quicrq_ctx_t* qr_ctx);
int quicrq_cnx_has_stream(quicrq_cnx_ctx_t* cnx_ctx);
int quicrq_close_cnx(quicrq_cnx_ctx_t* cnx_ctx);
int quicrq_is_cnx_disconnected(quicrq_cnx_ctx_t* cnx_ctx);
void quicrq_source_wakeup(quicrq_media_source_ctx_t* srce_ctx);

int quicrq_callback(picoquic_cnx_t* cnx,
    uint64_t stream_id, uint8_t* bytes, size_t length,
    picoquic_call_back_event_t fin_or_event, void* callback_ctx, void* v_stream_ctx);

#ifdef __cplusplus
}
#endif


#endif /* QUICRQ_H */
