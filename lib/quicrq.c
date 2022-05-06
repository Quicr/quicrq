/*
 * QUICR-Q:
 * 
 * Prototyping of quic real-time on top of picoquic.
 * 
 * The prototype will implement several variations of QUICR: stream, rush, and datagrams.
 * These variants use common "glue" code to interface with picoquic:
 *  - feeding media objects for transmission
 *  - providing media objects for rendering
 *  - implementing the picoquic callback
 *  - implementing the socket loop used by picoquic.
 * The socket loop is adapted to wait for media input or end of rendering as well as packet arrival.
 * 
 * The library can be used in three context:
 * - To implement an "origin" server
 * - To implement a relay (e.g., CDN relay)
 * - To implement a client.
 * 
 * The main transaction is the retrieval of a media stream from a server. In the
 * test implementation, this is done by the client setting a connection to the relay
 * (or reusing a suitable connection), and then queuing a "media fragment request", to
 * be sent on the first available client stream. The media request specifies,
 * at a minimum, the identification of the media, possibly the time to start
 * the replay, and the retrieval variant, e.g. stream or datagram.
 * 
 * If the media is available (e.g. at origin), it is sent immediately.
 * If not, the request is queued and the media is requested to an upstream server.
 */

#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include "picoquic_utils.h"
#include "quicrq.h"
#include "quicrq_internal.h"
#include "quicrq_relay.h"

/* Allocate space in the message buffer */
int quicrq_msg_buffer_alloc(quicrq_message_buffer_t* msg_buffer, size_t space, size_t bytes_stored)
{
    int ret = 0;

    if (bytes_stored > msg_buffer->buffer_alloc) {
        ret = -1;
    }
    else if (space > msg_buffer->buffer_alloc) {
        uint8_t* x = (uint8_t*)malloc(space);
        if (x == NULL) {
            /* internal error! */
            ret = -1;
        }
        else {
            if (bytes_stored > 0 && bytes_stored <= space) {
                memcpy(x, msg_buffer->buffer, bytes_stored);
            }
            free(msg_buffer->buffer);
            msg_buffer->buffer_alloc = space;
            msg_buffer->buffer = x;
        }
    }
    return ret;
}

/* Accumulate a protocol message from series of read data call backs */
uint8_t * quicrq_msg_buffer_store(uint8_t* bytes, size_t length, quicrq_message_buffer_t* msg_buffer, int* is_finished)
{
    *is_finished = 0;

    while (msg_buffer->nb_bytes_read < 2 && length > 0) {
        msg_buffer->nb_bytes_read++;
        msg_buffer->message_size <<= 8;
        msg_buffer->message_size += bytes[0];
        bytes++;
        length--;
    }

    if (msg_buffer->nb_bytes_read >= 2) {
        size_t bytes_stored = msg_buffer->nb_bytes_read - 2;
        size_t required = msg_buffer->message_size - bytes_stored;

        if (required > 0) {
            if (quicrq_msg_buffer_alloc(msg_buffer, msg_buffer->message_size, bytes_stored) != 0) {
                bytes = NULL;
            } else {
                if (length >= required) {
                    length = required;
                    *is_finished = 1;
                }
                memcpy(msg_buffer->buffer + bytes_stored, bytes, length);
                bytes += length;
                msg_buffer->nb_bytes_read += length;
            }
        }
        else {
            *is_finished = 1;
        }
    }

    return bytes;
}

void quicrq_msg_buffer_reset(quicrq_message_buffer_t* msg_buffer)
{

    msg_buffer->nb_bytes_read = 0;
    msg_buffer->message_size = 0;
}


void quicrq_msg_buffer_release(quicrq_message_buffer_t* msg_buffer)
{
    if (msg_buffer->buffer != NULL) {
        free(msg_buffer->buffer);
    }
    memset(msg_buffer, 0, sizeof(quicrq_message_buffer_t));
}

/* Send a protocol message through series of read data call backs.
 * The repair messages include some data after the header.
 * The "data" and "data_length" must be the same across all calls for the same message.
 * If message is fully sent, the state moves to "ready"
 */
int quicrq_msg_buffer_prepare_to_send(quicrq_stream_ctx_t* stream_ctx, void* context, size_t space, int more_to_send)
{
    int ret = 0;
    quicrq_message_buffer_t* msg_buffer = &stream_ctx->message_sent;
    size_t total_size = msg_buffer->message_size;
    size_t total_to_send = 2 + total_size;

    if (msg_buffer->nb_bytes_read < total_to_send) {
        uint8_t* buffer;
        size_t available = total_to_send - msg_buffer->nb_bytes_read;
        if (available > space) {
            more_to_send = 1;
            available = space;
        }

        buffer = picoquic_provide_stream_data_buffer(context, available, 0, more_to_send);
        if (buffer != NULL) {
            /* Feed the message length on two bytes */
            while (msg_buffer->nb_bytes_read < 2 && available > 0) {
                uint8_t b = (msg_buffer->nb_bytes_read == 0) ?
                    (uint8_t)((total_size >> 8) & 255) :
                    (uint8_t)(total_size & 255);
                *buffer = b;
                buffer++;
                available--;
                msg_buffer->nb_bytes_read++;
            }
            /* feed the remaining header content at offset */
            if (available > 0 && msg_buffer->nb_bytes_read < msg_buffer->message_size + 2) {
                size_t offset = msg_buffer->nb_bytes_read - 2;
                memcpy(buffer, msg_buffer->buffer + offset, available);
                msg_buffer->nb_bytes_read += available;
            }
        }
        else {
            ret = -1;
        }

        if (msg_buffer->nb_bytes_read >= total_to_send) {
            stream_ctx->send_state = quicrq_sending_ready;
            msg_buffer->nb_bytes_read = 0;
            msg_buffer->message_size = 0;
        }
    }
    return ret;
}

/* Sending in sequence on a stream. 
 * We do not want to spend too much effort there, so we are going to reuse the "send repair" object
 * to send data fragments of sufficient length. This is a bit of a hack, and it does add some overhead.
 * Need to maintain variables, e.g.:
 * - next object ID to send
 * - current object offset
 * - next 
 */
int quicrq_prepare_to_send_media_to_stream(quicrq_stream_ctx_t* stream_ctx, void* context, size_t space, uint64_t current_time)
{
    /* Find how much data is available on the media stream */
    int is_media_finished = 0;
    int is_last_fragment = 0;
    int is_still_active = 0;
    size_t available = 0;
    size_t data_length = 0;
    uint8_t stream_header[QUICRQ_STREAM_HEADER_MAX];
    size_t h_size;
    int ret = 0;

    /* First, create a "mock" buffer based on the available space instead of the actual number of bytes.
     * By design, we are creating a "repair" object, but using the "repair request" encoding. */
    uint8_t* h_byte = quicrq_repair_request_encode(stream_header+2, stream_header + QUICRQ_STREAM_HEADER_MAX, QUICRQ_ACTION_REPAIR,
        stream_ctx->next_object_id, stream_ctx->next_object_offset, 0, space);
    if (h_byte == NULL) {
        /* That should not happen, unless the stream_header size is way too small */
        ret = -1;
    }
    else {
        h_size = h_byte - stream_header;
        if (h_size > space) {
            /* That should not happen either, picoquic should never provide less than 17 bytes. */
            ret = -1;
        }
        else {
            /* Find how much data is actually available */
            ret = stream_ctx->publisher_fn(quicrq_media_source_get_data, stream_ctx->media_ctx, NULL, space - h_size, &available, &is_last_fragment, &is_media_finished, &is_still_active, current_time);
        }
    }

    if (ret == 0) {
        if (available == 0) {
            if (is_media_finished) {
                /* Send the fin object immediately, because it would be very hard to get
                 * a new "prepare to send" callback after an empty response.
                 */
                stream_ctx->final_object_id = stream_ctx->next_object_id;
                h_byte = quicrq_fin_msg_encode(stream_header + 2, stream_header + QUICRQ_STREAM_HEADER_MAX, QUICRQ_ACTION_FIN_DATAGRAM,
                    stream_ctx->final_object_id);
                if (h_byte == NULL || h_byte > stream_header + space) {
                    ret = -1;
                }
                else {
                    uint8_t* buffer;
                    h_size = h_byte - stream_header;
                    buffer = (uint8_t*)picoquic_provide_stream_data_buffer(context, h_size, 1, 0);
                    stream_ctx->is_local_finished = 1;
                    if (buffer == NULL) {
                        ret = -1;
                    }
                    else {
                        picoquic_log_app_message(stream_ctx->cnx_ctx->cnx, "Fin object of stream %" PRIu64 " : %" PRIu64,
                            stream_ctx->stream_id, stream_ctx->final_object_id);

                        stream_header[0] = (uint8_t)(h_size >> 8);
                        stream_header[1] = (uint8_t)(h_size & 0xff);
                        memcpy(buffer, stream_header, h_size);
                        stream_ctx->is_final_object_id_sent = 1;
                    }
                }
            }
            else {
                /* Mark stream as not ready. It will be awakened when data becomse available */
                picoquic_mark_active_stream(stream_ctx->cnx_ctx->cnx, stream_ctx->stream_id, 0, stream_ctx);
            }
        }
        else {
            /* Encode the actual header, instead of a prediction */
            h_byte = quicrq_repair_request_encode(stream_header + 2, stream_header + QUICRQ_STREAM_HEADER_MAX, QUICRQ_ACTION_REPAIR,
                stream_ctx->next_object_id, stream_ctx->next_object_offset, is_last_fragment, available);
            if (is_last_fragment) {
                picoquic_log_app_message(stream_ctx->cnx_ctx->cnx, "Final fragment of object %" PRIu64 " on stream %" PRIu64,
                    stream_ctx->next_object_id, stream_ctx->stream_id);
            }
            if (h_byte == NULL) {
                /* That should not happen, unless the stream_header size was way too small */
                ret = -1;
            }
            else {
                uint8_t* buffer;
                h_size = h_byte - stream_header;
                buffer = (uint8_t*)picoquic_provide_stream_data_buffer(context, h_size + available, 0, 1);
                if (buffer == NULL) {
                    ret = -1;
                }
                else {
                    /* copy the stream header to the packet */
                    memcpy(buffer, stream_header, h_size);
                    ret = stream_ctx->publisher_fn(quicrq_media_source_get_data, stream_ctx->media_ctx, buffer + h_size, available, &data_length,
                        &is_last_fragment, &is_media_finished, &is_still_active, current_time);
                    if (ret == 0 && available != data_length) {
                        ret = -1;
                    }
                    else
                    {
                        /* Set the message length */
                        size_t message_length = h_size - 2 + available;
                        buffer[0] = (uint8_t)(message_length >> 8);
                        buffer[1] = (uint8_t)(message_length&0xff);

                        if (is_last_fragment) {
                            stream_ctx->next_object_id++;
                            stream_ctx->next_object_offset = 0;
                        } else {
                            stream_ctx->next_object_offset += available;
                        }

                        if (is_media_finished) {
                            stream_ctx->final_object_id = stream_ctx->next_object_id;
                            stream_ctx->send_state = quicrq_sending_ready;
                        }
                    }
                }
            }
        }
    }

    return ret;
}

/* Find the stream context associated with a datagram */
quicrq_stream_ctx_t* quicrq_find_stream_ctx_for_datagram(quicrq_cnx_ctx_t* cnx_ctx, uint64_t datagram_stream_id, int is_sender)
{
    quicrq_stream_ctx_t* stream_ctx = NULL;

    /* Find the stream context by datagram ID */
    stream_ctx = cnx_ctx->first_stream;
    while (stream_ctx != NULL) {
        if ((stream_ctx->is_sender == is_sender) && stream_ctx->is_datagram && stream_ctx->datagram_stream_id == datagram_stream_id) {
            break;
        }
        stream_ctx = stream_ctx->next_stream;
    }
    return stream_ctx;
}

/* Receive data in a datagram */
int quicrq_receive_datagram(quicrq_cnx_ctx_t* cnx_ctx, const uint8_t* bytes, size_t length, uint64_t current_time)
{
    int ret = 0;
    quicrq_stream_ctx_t* stream_ctx = NULL;

    /* Parse the datagram header */
    const uint8_t* bytes_max = bytes + length;
    uint64_t datagram_stream_id;
    uint64_t object_id;
    uint64_t object_offset;
    int is_last_fragment;
    const uint8_t* next_bytes;

    next_bytes = quicrq_datagram_header_decode(bytes, bytes_max, &datagram_stream_id, &object_id, &object_offset, &is_last_fragment);
    if (next_bytes == NULL) {
        ret = -1;
    }
    else {
        /* Find the stream context by datagram ID */
        stream_ctx = quicrq_find_stream_ctx_for_datagram(cnx_ctx, datagram_stream_id, 0);
        if (stream_ctx == NULL) {
            if (datagram_stream_id >= cnx_ctx->next_abandon_datagram_id) {
                ret = -1;
                picoquic_log_app_message(cnx_ctx->cnx, "Unexpected datagram on stream %" PRIu64,
                    datagram_stream_id);
            }
        }
        else {
            /* Pass data to the media context. */
            if (is_last_fragment) {
                picoquic_log_app_message(cnx_ctx->cnx, "Received final fragment of object %" PRIu64 " on datagram stream %" PRIu64 ", stream %" PRIu64,
                    object_id, datagram_stream_id, stream_ctx->stream_id);
            }
            ret = stream_ctx->consumer_fn(quicrq_media_datagram_ready, stream_ctx->media_ctx, current_time, next_bytes, object_id, object_offset, is_last_fragment, bytes_max - next_bytes);
            ret = quicrq_cnx_handle_consumer_finished(stream_ctx, 0, 1, ret);
        }
    }

    return ret;
}

void quicrq_remove_repair_in_stream_ctx(quicrq_stream_ctx_t* stream_ctx, quicrq_datagram_queued_repair_t* repair)
{
    if (repair->previous_repair == NULL) {
        stream_ctx->datagram_repair_first = repair->next_repair;
    }
    else {
        repair->previous_repair->next_repair = repair->next_repair;
    }

    if (repair->next_repair == NULL) {
        stream_ctx->datagram_repair_last = repair->previous_repair;
    }
    else {
        repair->next_repair->previous_repair = repair->previous_repair;
    }

    free(repair);
}

int quicrq_add_repair_to_stream_ctx(quicrq_cnx_ctx_t* cnx_ctx, quicrq_stream_ctx_t* stream_ctx, const uint8_t* bytes, size_t length,
    uint64_t object_id, uint64_t object_offset, int is_last_fragment)
{
    int ret = 0;
    size_t target_size = sizeof(quicrq_datagram_queued_repair_t) + length;

    if (target_size < sizeof(quicrq_datagram_queued_repair_t)) {
        ret = -1;
    } else {
        /* Create a repair message, reserving space at the end to copy the datagram */
        quicrq_datagram_queued_repair_t* repair = (quicrq_datagram_queued_repair_t*)malloc(target_size);
        if (repair == NULL) {
            ret = -1;
        }
        else {
            memset(repair, 0, sizeof(quicrq_datagram_queued_repair_t));
            repair->object_id = object_id;
            repair->object_offset = object_offset;
            repair->is_last_fragment = is_last_fragment;
            repair->length = length;
            repair->datagram = ((uint8_t*)repair) + sizeof(quicrq_datagram_queued_repair_t);
            memcpy(repair->datagram, bytes, length);
            if (stream_ctx->datagram_repair_last == NULL) {
                stream_ctx->datagram_repair_last = repair;
                stream_ctx->datagram_repair_first = repair;
            }
            else {
                repair->previous_repair = stream_ctx->datagram_repair_last;
                stream_ctx->datagram_repair_last->next_repair = repair;
                stream_ctx->datagram_repair_last = repair;
            }
            /* Wake up the control stream so the final message can be sent. */
            picoquic_mark_active_stream(stream_ctx->cnx_ctx->cnx, stream_ctx->stream_id, 1, stream_ctx);
        }
    }
    return ret;
}

int quicrq_check_spurious_repair_in_stream_ctx(quicrq_cnx_ctx_t* cnx_ctx, quicrq_stream_ctx_t* stream_ctx, size_t length,
    uint64_t object_id, uint64_t object_offset, int is_last_fragment)
{
    int ret = 0;
    quicrq_datagram_queued_repair_t* repair = stream_ctx->datagram_repair_first;

    while (repair != NULL) {
        if (repair->object_id == object_id && repair->object_offset == object_offset &&
            repair->length == length && repair->is_last_fragment == is_last_fragment) {
            break;
        }
        else {
            repair = repair->next_repair;
        }
    }

    if (repair != NULL && (stream_ctx->datagram_repair_first != repair || stream_ctx->send_state != quicrq_sending_repair)) {
        quicrq_remove_repair_in_stream_ctx(stream_ctx, repair);
    }

    return ret;
}

/* Handle the list of datagrams pending acknowledgement or retransmission.
 * The code maintains an acknowledgement tree of the fragments that were sent.
 * TODO: handle whether we can have overlapping fragments. We will assume that
 * we will not, i.e., that the MTU will remain valid for the duration of
 * the datagram. Thus, we do not use the length field as part of the
 * key.
 */

static void* quicrq_datagram_ack_node_value(picosplay_node_t* datagram_ack_node)
{
    return (datagram_ack_node == NULL) ? NULL : (void*)((char*)datagram_ack_node - offsetof(struct st_quicrq_datagram_ack_state_t, datagram_ack_node));
}

static int64_t quicrq_datagram_ack_node_compare(void* l, void* r)
{
    quicrq_datagram_ack_state_t* da_l = (quicrq_datagram_ack_state_t*)l;
    quicrq_datagram_ack_state_t* da_r = (quicrq_datagram_ack_state_t*)r;
    int64_t ret = (int64_t)(da_l->object_id - da_r->object_id);

    if (ret == 0) {
        ret = da_l->object_offset - da_r->object_offset;
    }

    return ret;
}

static picosplay_node_t* quicrq_datagram_ack_node_create(void* v_datagram_ack_state)
{
    /* Do not actually create data. Simply return a pointer to the "node"
     * property in the datagram ack state record. This is in line with
     * expected behavior of "picosplay". */
    return &((quicrq_datagram_ack_state_t*)v_datagram_ack_state)->datagram_ack_node;
}

static void quicrq_datagram_ack_node_delete(void* tree, picosplay_node_t* node)
{
#ifdef _WINDOWS
    UNREFERENCED_PARAMETER(tree);
#endif
    free(quicrq_datagram_ack_node_value(node));
}

static void quicrq_datagram_ack_ctx_init(quicrq_stream_ctx_t* stream_ctx)
{
    stream_ctx->horizon_object_id = UINT64_MAX;
    stream_ctx->horizon_offset = UINT64_MAX;
    stream_ctx->horizon_is_last_fragment = 1;
    picosplay_init_tree(&stream_ctx->datagram_ack_tree, quicrq_datagram_ack_node_compare,
        quicrq_datagram_ack_node_create, quicrq_datagram_ack_node_delete, quicrq_datagram_ack_node_value);
}


static void quicrq_datagram_ack_ctx_release(quicrq_stream_ctx_t* stream_ctx)
{
    if (stream_ctx->datagram_ack_tree.size != 0) {
        picosplay_node_t * next_node = picosplay_first(&stream_ctx->datagram_ack_tree);
        int nb_fragments_acked = 0;
        int nb_fragments_nacked = 0;
        int nb_fragments_alone = 0;
        while (next_node != NULL) {
            quicrq_datagram_ack_state_t* das = (quicrq_datagram_ack_state_t*)quicrq_datagram_ack_node_value(next_node);
            if (das->is_acked) {
                nb_fragments_acked++;
            }
            if (das->fec_needed) {
                nb_fragments_nacked++;
            }
            if (!das->is_acked && !das->fec_needed) {
                nb_fragments_alone++;
            }

            next_node = picosplay_next(next_node);
        }

        DBG_PRINTF("End of stream  %" PRIu64 ", %d nodes in datagram list, %d acked, %d nacked, alone: %d",
            stream_ctx->stream_id, stream_ctx->datagram_ack_tree.size,
            nb_fragments_acked, nb_fragments_nacked, nb_fragments_alone);
        DBG_PRINTF("Horizon Object ID: %" PRIu64 ", offset: %" PRIu64,
            stream_ctx->horizon_object_id, stream_ctx->horizon_offset);
    }
    picosplay_empty_tree(&stream_ctx->datagram_ack_tree);
}

quicrq_datagram_ack_state_t* quicrq_datagram_ack_find(quicrq_stream_ctx_t* stream_ctx, uint64_t object_id, uint64_t object_offset)
{
    quicrq_datagram_ack_state_t* found = NULL;
    quicrq_datagram_ack_state_t target = { 0 };
    target.object_id = object_id;
    target.object_offset = object_offset;

    picosplay_node_t* node = picosplay_find(&stream_ctx->datagram_ack_tree, (void*)&target);
    if (node != NULL) {
        found = (quicrq_datagram_ack_state_t*)quicrq_datagram_ack_node_value(node);
    }
    return found;
}

int64_t quicrq_datagram_check_horizon(quicrq_stream_ctx_t* stream_ctx, uint64_t object_id, uint64_t object_offset)
{
    int64_t ret = object_id - stream_ctx->horizon_object_id;
    if (ret == 0) {
        ret = object_offset - stream_ctx->horizon_offset;
    }
    return ret;
}

int quicrq_datagram_ack_init(quicrq_stream_ctx_t* stream_ctx, uint64_t object_id, uint64_t object_offset, size_t length,
    int is_last_fragment, void** p_created_state)
{
    int ret = 0;
    /* Check whether the object is below the horizon */
    if (quicrq_datagram_check_horizon(stream_ctx, object_id, object_offset) < 0) {
        /* at or below horizon, not new. */
        stream_ctx->nb_horizon_events++;
    } else {
        /* Find whether the ack record is there. */
        quicrq_datagram_ack_state_t* found = quicrq_datagram_ack_find(stream_ctx, object_id, object_offset);

        /* if there, no need to send it. */
        if (found) {
            ret = 1;
        }
        else {
            /* else, create a record. */
            quicrq_datagram_ack_state_t* da_new = (quicrq_datagram_ack_state_t*)malloc(sizeof(quicrq_datagram_ack_state_t));
            if (da_new == NULL) {
                /* memory error */
                ret = -1;
            }
            else {
                memset(da_new, 0, sizeof(quicrq_datagram_ack_state_t));
                da_new->object_id = object_id;
                da_new->object_offset = object_offset;
                da_new->length = length;
                da_new->is_last_fragment = is_last_fragment;
                picosplay_insert(&stream_ctx->datagram_ack_tree, da_new);
                if (p_created_state != NULL) {
                    *p_created_state = da_new;
                }
            }
        }
    }
    return ret;
}

int quicrq_datagram_handle_ack(quicrq_stream_ctx_t* stream_ctx, uint64_t object_id, uint64_t object_offset)
{
    int ret = 0;
    /* Find whether the ack record is there. */
    quicrq_datagram_ack_state_t* found = quicrq_datagram_ack_find(stream_ctx, object_id, object_offset);

    /* if there, mark as acknowledged */
    /* TODO: handle length? */
    if (found) {
        found->is_acked = 1;
        /* Check if this is the oldest segment */
        if (quicrq_datagram_check_horizon(stream_ctx, object_id, object_offset) <= 1) {
            /* Progress the horizon */
            picosplay_node_t* next_node = picosplay_first(&stream_ctx->datagram_ack_tree);
            while (next_node != NULL) {
                int just_after = 0;
                quicrq_datagram_ack_state_t* das = (quicrq_datagram_ack_state_t*)quicrq_datagram_ack_node_value(next_node);
                if (!das->is_acked) {
                    break;
                }
                if (stream_ctx->horizon_is_last_fragment) {
                    just_after = ((das->object_id - stream_ctx->horizon_object_id) == 1) && (das->object_offset == 0);
                }
                else {
                    just_after = (das->object_id == stream_ctx->horizon_object_id) && (das->object_offset == stream_ctx->horizon_offset);
                }
                if (!just_after) {
                    break;
                }
                else {
                    /* collapse the horizon */
                    picosplay_node_t* to_be_forgotten = next_node;
                    stream_ctx->horizon_object_id = das->object_id;
                    stream_ctx->horizon_offset = das->object_offset + das->length;
                    stream_ctx->horizon_is_last_fragment = das->is_last_fragment;

                    next_node = picosplay_next(next_node);
                    picosplay_delete_hint(&stream_ctx->datagram_ack_tree, to_be_forgotten);
                }
            }
        }
    }
    return ret;
}

 /* If a datagram frame needs to be repeated, a copy of the frame will be
 * queued using the picoquic_queue_datagram_frame() API. That API can 
 * only handle datagram of at most PICOQUIC_DATAGRAM_QUEUE_MAX_LENGTH
 * bytes. If the original datagram is longer, it needs to be split.
 */

int quicrq_datagram_handle_repeat(quicrq_stream_ctx_t* stream_ctx,
    quicrq_datagram_ack_state_t* found,
    const uint8_t* data, size_t data_length)
{
    int ret = 0;
    /* Check that the connection is there */
    if (stream_ctx->cnx_ctx == NULL || stream_ctx->cnx_ctx->cnx == NULL) {
        ret = -1;
    }
    else {
        while (data_length > 0 && ret == 0) {
            uint8_t datagram[PICOQUIC_MAX_PACKET_SIZE];
            uint8_t* bytes = datagram;
            uint8_t* bytes_max = datagram + PICOQUIC_MAX_PACKET_SIZE;
            size_t fragment_length = data_length;
            size_t header_length;
            size_t datagram_length;
            /* Encode the header */
            found->last_sent_time = picoquic_get_quic_time(picoquic_get_quic_ctx(stream_ctx->cnx_ctx->cnx));
            bytes = quicrq_datagram_header_encode(bytes, bytes_max, stream_ctx->datagram_stream_id,
                found->object_id, found->object_offset, found->is_last_fragment);
            /* Check how much data should be send in this fragment */
            header_length = bytes - datagram;
            datagram_length = header_length + data_length;
            if (header_length + fragment_length > PICOQUIC_DATAGRAM_QUEUE_MAX_LENGTH) {
                if (found->is_last_fragment) {
                    /* Erase the last segment mark in datagram header */
                    bytes = quicrq_datagram_header_encode(datagram, bytes_max, stream_ctx->datagram_stream_id,
                        found->object_id, found->object_offset, 0);
                    header_length = bytes - datagram;
                }
                fragment_length = PICOQUIC_DATAGRAM_QUEUE_MAX_LENGTH - header_length;
                datagram_length = PICOQUIC_DATAGRAM_QUEUE_MAX_LENGTH;
            }
            /* Copy the data */
            if (bytes + fragment_length > bytes_max) {
                ret = -1;
            }
            else {
                memcpy(bytes, data, fragment_length);
                ret = picoquic_queue_datagram_frame(stream_ctx->cnx_ctx->cnx, datagram_length, datagram);
                if (ret == 0 && fragment_length < data_length) {
                    void* p_next_record = NULL;
                    size_t next_offset = found->object_offset + fragment_length;
                    data += fragment_length;
                    data_length -= fragment_length;

                    /* split the fragment, get a new one, update old record, point found to new record. */
                    ret = quicrq_datagram_ack_init(stream_ctx, found->object_id, next_offset, data_length,
                        found->is_last_fragment, &p_next_record);
                    if (ret == 0) {
                        quicrq_datagram_ack_state_t* next_record = (quicrq_datagram_ack_state_t*)p_next_record;
                        next_record->is_last_fragment = found->is_last_fragment;
                        next_record->fec_needed = found->fec_needed;
                        found->is_last_fragment = 0;
                        found->length = fragment_length;
                        found = next_record;
                    }
                }
                else {
                    break;
                }
            }
        }
    }
    return ret;
}

int quicrq_datagram_handle_lost(quicrq_stream_ctx_t* stream_ctx, uint64_t object_id, uint64_t object_offset, uint64_t sent_time,
    const uint8_t *bytes, size_t length)
{
    int ret = 0;
    /* Find whether the ack record is there. */
    quicrq_datagram_ack_state_t* found = quicrq_datagram_ack_find(stream_ctx, object_id, object_offset);

    /* if not there, assume acknowledged and hidden below the horizon */
    /* if found and is acked, do not repeat */
    /* If this is not the last transmission, do not repeat */
    if (found != NULL && !found->is_acked && found->last_sent_time <= sent_time + 1000) {
        /* Mark FEC needed. */
        found->fec_needed = 1;
        /* Update the datagram header, and queue as datagram */
        ret = quicrq_datagram_handle_repeat(stream_ctx, found, bytes, length);
    }
    return ret;
}

/* Handle the acknowledgements of datagrams */
int quicrq_handle_datagram_ack_nack(quicrq_cnx_ctx_t* cnx_ctx, picoquic_call_back_event_t picoquic_event, uint64_t send_time, const uint8_t* bytes, size_t length)
{
    int ret = 0;
    /* Obtain the datagram ID */
    const uint8_t* bytes_max = bytes + length;
    uint64_t datagram_stream_id;
    uint64_t object_id;
    uint64_t object_offset;
    int is_last_fragment;
    const uint8_t* next_bytes;

    if (bytes == NULL) {
        ret = -1;
    }
    else {
        next_bytes = quicrq_datagram_header_decode(bytes, bytes_max, &datagram_stream_id, &object_id, &object_offset, &is_last_fragment);
        /* Retrieve the stream context for the datagram */
        if (next_bytes == NULL) {
            ret = -1;
        }
        else {
            /* Find the stream context by datagram ID.
             * the stream may already be closed, so not finding it is not an error.
             */
            quicrq_stream_ctx_t* stream_ctx = quicrq_find_stream_ctx_for_datagram(cnx_ctx, datagram_stream_id, 1);
            if (stream_ctx != NULL) {
                switch (picoquic_event) {
                case picoquic_callback_datagram_acked: /* Ack for packet carrying datagram-object received from peer */
                    ret = quicrq_datagram_handle_ack(stream_ctx, object_id, object_offset);
                    break;
                case picoquic_callback_datagram_lost: /* Packet carrying datagram-object probably lost */
                    ret = quicrq_datagram_handle_lost(stream_ctx, object_id, object_offset, send_time,
                        next_bytes, (size_t)(bytes_max - next_bytes));
                    break;
                case picoquic_callback_datagram_spurious: /* Packet carrying datagram-object was not really lost */
                    ret = quicrq_datagram_handle_ack(stream_ctx, object_id, object_offset);
                    break;
                default:
                    ret = -1;
                }
            }
        }
    }

    return ret;
}


/* Prepare to send a datagram */

int quicrq_prepare_to_send_datagram(quicrq_cnx_ctx_t* cnx_ctx, void* context, size_t space, uint64_t current_time)
{
    /* Find a stream on which datagrams are available */
    int ret = 0;
    int at_least_one_active = 0;
    quicrq_stream_ctx_t* stream_ctx = cnx_ctx->first_stream;

    while (stream_ctx != NULL) {
        if (stream_ctx->is_datagram && stream_ctx->is_sender && stream_ctx->is_active_datagram) {
            if (stream_ctx->get_datagram_fn != NULL) {
                /* If the source can directly format datagrams, just poll it */
                int media_was_sent = 0;
                ret = stream_ctx->get_datagram_fn(stream_ctx, context, space, &media_was_sent, &at_least_one_active);
                if (media_was_sent || ret != 0) {
                    break;
                }
                else {
                    stream_ctx->is_active_datagram = 0;
                }
            }
            else {
                /* Check how much data is ready */
                size_t available = 0;
                size_t data_length = 0;
                /* Predict length of datagram_stream_id + length of offset.
                 * TODO: the number of bytes available depends on the header size, which depends on
                 * object_id, and offset. The object size and object offset are managed by the
                 * sender code and are known in advance, but the "last_fragment" value is not.
                 * We do a first encoding supposing last_fragment = 0. If this turns out
                 * to be the actual last fragment, the coding will have to be fixed.
                 */
                uint8_t datagram_header[QUICRQ_DATAGRAM_HEADER_MAX];
                size_t h_size;
                uint8_t* h_byte = quicrq_datagram_header_encode(datagram_header, datagram_header + QUICRQ_DATAGRAM_HEADER_MAX, stream_ctx->datagram_stream_id,
                    stream_ctx->next_object_id, stream_ctx->next_object_offset, 0);
                if (h_byte == NULL) {
                    /* Not enough space in header buffer to encode the header. That should never happen */
                    quicrq_log_message(stream_ctx->cnx_ctx, "Error: datagram header longer than %zu", QUICRQ_DATAGRAM_HEADER_MAX);
                    DBG_PRINTF("Error: datagram header longer than %zu", QUICRQ_DATAGRAM_HEADER_MAX);
                    ret = -1;
                    break;
                }
                h_size = h_byte - datagram_header;
                if (h_size >= space) {
                    /* TODO: should get a min encoding length per stream */
                    /* Can't do anything there */
                    at_least_one_active = 1;
                }
                else {
                    int is_last_fragment = 0;
                    int is_media_finished = 0;
                    int is_still_active = 0;

                    ret = stream_ctx->publisher_fn(quicrq_media_source_get_data, stream_ctx->media_ctx, NULL, space - h_size, &available, &is_last_fragment, &is_media_finished, &is_still_active, current_time);

                    /* Get a buffer inside the datagram packet */
                    if (ret < 0) {
                        quicrq_log_message(stream_ctx->cnx_ctx, "Error, first publisher function call returns %d, space = %zu, available = %zu", ret, space - h_size, available);
                        DBG_PRINTF("Error, first publisher function call returns %d, space = %zu, available = %zu", ret, space - h_size, available);
                    } else {
                        if (is_media_finished) {
                            /* Mark the stream as finished, prepare sending a final message */
                            stream_ctx->final_object_id = stream_ctx->next_object_id;
                            /* Wake up the control stream so the final message can be sent. */
                            picoquic_mark_active_stream(stream_ctx->cnx_ctx->cnx, stream_ctx->stream_id, 1, stream_ctx);
                        }
                        if (available > 0) {
                            void* buffer = picoquic_provide_datagram_buffer(context, available + h_size);
                            at_least_one_active = 1;
                            if (buffer == NULL) {
                                quicrq_log_message(stream_ctx->cnx_ctx, "Error, cannot obtain datagram buffer, space = %zu, available = %zu", space, available + h_size);
                                DBG_PRINTF("Error, cannot obtain datagram buffer, space = %zu, available = %zu", space, available + h_size);
                                ret = -1;
                            }
                            else {
                                /* Push the header */
                                if (is_last_fragment) {
                                    h_byte = quicrq_datagram_header_encode(datagram_header, datagram_header + QUICRQ_DATAGRAM_HEADER_MAX, stream_ctx->datagram_stream_id,
                                        stream_ctx->next_object_id, stream_ctx->next_object_offset, 1);
                                    if (h_byte != datagram_header + h_size) {
                                        /* Can't happen, unless our coding assumptions were wrong. Need to debug that. */
                                        quicrq_log_message(stream_ctx->cnx_ctx, "Error, cannot encode datagram header, expected = %zu", h_size);
                                        DBG_PRINTF("Error, cannot encode datagram header, expected = %zu", h_size);
                                        ret = -1;
                                    }
                                }
                                if (ret == 0) {
                                    memcpy(buffer, datagram_header, h_size);
                                    /* Get the media */
                                    ret = stream_ctx->publisher_fn(quicrq_media_source_get_data, stream_ctx->media_ctx, ((uint8_t*)buffer) + h_size, available, &data_length,
                                        &is_last_fragment, &is_media_finished, &is_still_active, current_time);
                                    if (ret == 0 && available != data_length) {
                                        /* Application returned different size on second call */
                                        quicrq_log_message(stream_ctx->cnx_ctx, "Error,  application datagram provided %zu, expected %zu", data_length, available);
                                        DBG_PRINTF("Error,  application datagram provided %zu, expected %zu", data_length, available);
                                        ret = -1;
                                    }
                                }
                                /* Keep track in stream context */
                                if (ret == 0) {
                                    ret = quicrq_datagram_ack_init(stream_ctx, stream_ctx->next_object_id, stream_ctx->next_object_offset, data_length, is_last_fragment, NULL);
                                    if (ret != 0) {
                                        DBG_PRINTF("Datagram ack init returns %d", ret);
                                    }
                                }
                                /* Update offset based on what is sent. */
                                if (ret == 0) {
                                    if (is_last_fragment) {
                                        stream_ctx->next_object_id++;
                                        stream_ctx->next_object_offset = 0;
                                    }
                                    else {
                                        stream_ctx->next_object_offset += data_length;
                                    }
                                }
                            }
                            /* Exit the loop, since data was copied */
                            break;
                        }
                        else {
                            stream_ctx->is_active_datagram = is_still_active;
                            at_least_one_active |= is_still_active;
                        }
                    }
                }
            }
        }
        stream_ctx = stream_ctx->next_stream;
    }

    if (ret == 0) {
        picoquic_mark_datagram_ready(cnx_ctx->cnx, at_least_one_active);
    }

    return ret;
}

/* Send the next message on a stream.
 * Messages include:
 * - initial opening message sent by the client, for either receiving or publishing a fragment,
 *   and specifying stream or datagram.
 * - possibly, initial synchronization message sent by the server in response to the client
 *   publishing some media.
 * - if sending the media as stream, stream data until the steam is closed.
 * - if sending the media as datagram and repairs are queued, repair messages.
 * - if repair message in progress, header followed by media.
 * - if all media sent as datagram, final offset message.
 * - possibly, if media received as datagram, repair request messages.
 * 
 * The behavior depends on the state of the stream, and at least two variables: is a message sending in progress,
 * and, is there something else to send after that message.
 * 
 * If no message is being sent, the application looks at what is queued. If sending media, that
 * means checking the repair queue, and also checking whether the final offset needs to be sent.
 * The header for the next message is then formatted, and the application sends that message.
 * 
 * If a protocol message is currently being sent, the application fills the buffer
 * with the next bytes in that message. There is a special case for repair messages,
 * which include a header and then teh original content of the lost datagram.
 * 
 * When the application is done sending the message, it updates it state, e.g., mark the
 * offset as sent and dequeues the repair message.
 * 
 * The sender will close the stream after the receiver has closed it.
 */
int quicrq_prepare_to_send_on_stream(quicrq_stream_ctx_t* stream_ctx, void* context, size_t space, uint64_t current_time)
{
    int ret = 0;
    int more_to_send = 0;
    if (stream_ctx->send_state == quicrq_sending_ready) {
        quicrq_message_buffer_t* message = &stream_ctx->message_sent;
        /* Ready to send next message */
        if (stream_ctx->is_sender) {
            if (stream_ctx->datagram_repair_first != NULL) {
                /* Encode the first repair in queue in the protocol buffer */
                if (quicrq_msg_buffer_alloc(message, quicrq_repair_msg_reserve(stream_ctx->datagram_repair_first->object_id,
                    stream_ctx->datagram_repair_first->object_offset, stream_ctx->datagram_repair_first->is_last_fragment,
                    stream_ctx->datagram_repair_first->length), 0) != 0) {
                    ret = -1;
                }
                else {
                    uint8_t* message_next = quicrq_repair_msg_encode(message->buffer, message->buffer + message->buffer_alloc, QUICRQ_ACTION_REPAIR,
                        stream_ctx->datagram_repair_first->object_id, stream_ctx->datagram_repair_first->object_offset, stream_ctx->datagram_repair_first->is_last_fragment,
                        stream_ctx->datagram_repair_first->length, stream_ctx->datagram_repair_first->datagram);
                    if (message_next == NULL) {
                        ret = -1;
                    }
                    else {
                        /* Queue the media request message to that stream */
                        message->message_size = message_next - message->buffer;
                        stream_ctx->send_state = quicrq_sending_repair;
                    }
                }
            }
            else if (stream_ctx->final_object_id > 0 && !stream_ctx->is_final_object_id_sent) {
                quicrq_log_message(stream_ctx->cnx_ctx, "Stream %" PRIu64 ", sending final object id: %" PRIu64,
                    stream_ctx->stream_id, stream_ctx->final_object_id);
                /* TODO: encode the final offset message in the protocol buffer */
                if (quicrq_msg_buffer_alloc(message, quicrq_fin_msg_reserve(stream_ctx->final_object_id), 0) != 0) {
                    ret = -1;
                }
                else {
                    uint8_t* message_next = quicrq_fin_msg_encode(message->buffer, message->buffer + message->buffer_alloc, QUICRQ_ACTION_FIN_DATAGRAM,
                        stream_ctx->final_object_id);
                    if (message_next == NULL) {
                        ret = -1;
                    }
                    else {
                        /* Queue the media request message to that stream */
                        message->message_size = message_next - message->buffer;
                        stream_ctx->send_state = quicrq_sending_offset;
                    }
                }
            }
            else {
                /* This is a bug. If there is nothing to send, we should not be sending any stream data */
                quicrq_log_message(stream_ctx->cnx_ctx, "Nothing to send on stream %" PRIu64 ", state: %d, final: %" PRIu64,
                    stream_ctx->stream_id, stream_ctx->send_state, stream_ctx->final_object_id);
                DBG_PRINTF("Nothing to send on stream %" PRIu64 ", state: %d, final: %" PRIu64, 
                    stream_ctx->stream_id, stream_ctx->send_state, stream_ctx->final_object_id);
                picoquic_mark_active_stream(stream_ctx->cnx_ctx->cnx, stream_ctx->stream_id, 0, stream_ctx);
            }
        }
        else {
            /* TODO: consider receiver messages */
            quicrq_log_message(stream_ctx->cnx_ctx, "Consider receiver messages on stream %" PRIu64 ", final: %" PRIu64, stream_ctx->stream_id,
                stream_ctx->final_object_id);
            DBG_PRINTF("Consider receiver messages on stream %" PRIu64 ", final: %" PRIu64, stream_ctx->stream_id,
                stream_ctx->final_object_id);
        }
    }

    if (ret == 0){
        switch (stream_ctx->send_state) {
        case quicrq_sending_ready:
            /* Nothing to send. Mark the stream as not active. */
            picoquic_mark_active_stream(stream_ctx->cnx_ctx->cnx, stream_ctx->stream_id, 0, stream_ctx);
            break;
        case quicrq_sending_stream:
            /* Send available stream data. Check whether the FIN is reached. */
            ret = quicrq_prepare_to_send_media_to_stream(stream_ctx, context, space, current_time);
            break;
        case quicrq_sending_initial:
            /* Send available buffer data. Mark state ready after sent. */
            more_to_send = (stream_ctx->datagram_repair_first != NULL ||
                (stream_ctx->final_object_id > 0 && !stream_ctx->is_final_object_id_sent));
            ret = quicrq_msg_buffer_prepare_to_send(stream_ctx, context, space, more_to_send);
            break;
        case quicrq_sending_repair:
            /* Send available buffer data and repair data. Dequeue repair and mark state ready after sent. */
            more_to_send = (stream_ctx->datagram_repair_first->next_repair != NULL ||
                (stream_ctx->final_object_id > 0 && !stream_ctx->is_final_object_id_sent));
            ret = quicrq_msg_buffer_prepare_to_send(stream_ctx, context, space, more_to_send);
            if (stream_ctx->send_state == quicrq_sending_ready){
                quicrq_remove_repair_in_stream_ctx(stream_ctx, stream_ctx->datagram_repair_first);
            }
            break;
        case quicrq_sending_offset:
            /* Send available buffer data and repair data. Mark offset sent and mark state ready after sent. */
            more_to_send = (stream_ctx->datagram_repair_first != NULL);
            ret = quicrq_msg_buffer_prepare_to_send(stream_ctx, context, space, more_to_send);
            if (stream_ctx->send_state == quicrq_sending_ready){
                stream_ctx->is_final_object_id_sent = 1;
            }
            break;
        case quicrq_sending_fin:
            (void) picoquic_provide_stream_data_buffer(context, 0, 1, 0);
            stream_ctx->send_state = quicrq_sending_no_more;
            stream_ctx->is_local_finished = 1;
            if (stream_ctx->is_peer_finished) {
                quicrq_delete_stream_ctx(stream_ctx->cnx_ctx, stream_ctx);
            }
            break;
        default:
            /* Someone forgot to upgrade this code... */
            quicrq_log_message(stream_ctx->cnx_ctx, "Unexpected state %s on stream %" PRIu64, stream_ctx->send_state, stream_ctx->stream_id);
            DBG_PRINTF("Unexpected state %s on stream %" PRIu64, stream_ctx->send_state, stream_ctx->stream_id);
            ret = -1;
            break;
        }
    }

    return ret;
}

/* Receive and process media control messages.
 * This is governed by the receive state variable, with the following values:
 * - not yet ready: the state of a client stream, before sending the initial message.
 * - receive initial: the state of a server stream, when it was just created.
 * - receive confirmation: the state of a client after sending a post
 * - receive stream: receiver state if expecting media on stream.
 * - receive repair: while receiving datagrams, receive repairs, or the final offset
 * - receive done: waiting for end of data by the peer.
 * The media receiver closes the stream when the fragment is completely received,
 * or when the receiver stopped listening, or if the sender closed its own stream.
 * The media sender closes the stream if the receiver closes it, or if the sender
 * has to abandon the stream. 
 */

int quicrq_receive_stream_data(quicrq_stream_ctx_t* stream_ctx, uint8_t* bytes, size_t length, int is_fin)
{
    int ret = 0;

    while (ret == 0 && length > 0) {
        /* There may be a set of messages back to back, and all have to be received. */
        if (stream_ctx->receive_state == quicrq_receive_done) {
            /* Protocol violation, was not expecting any message */
            ret = -1;
            break;
        }
        else {
            /* Receive the next message on the stream, if any */
            int is_finished = 0;
            uint8_t* next_bytes = quicrq_msg_buffer_store(bytes, length, &stream_ctx->message_receive, &is_finished);
            if (next_bytes == NULL) {
                /* Something went wrong */
                ret = -1;
            }
            else
            {
                length = (bytes + length) - next_bytes;
                bytes = next_bytes;
                if (is_finished) {
                    /* Decode the incoming message */
                    quicrq_message_t incoming = { 0 };
                    const uint8_t* r_bytes = quicrq_msg_decode(stream_ctx->message_receive.buffer, stream_ctx->message_receive.buffer + stream_ctx->message_receive.message_size, &incoming);

                    if (r_bytes == NULL) {
                        /* Message was incorrect */
                        ret = -1;
                    }
                    else switch (incoming.message_type) {
                    case QUICRQ_ACTION_OPEN_STREAM:
                    case QUICRQ_ACTION_OPEN_DATAGRAM:
                        if (stream_ctx->receive_state != quicrq_receive_initial) {
                            quicrq_log_message(stream_ctx->cnx_ctx, "Stream %" PRIu64 ", unexpected subscribe message is stream receive state %d",
                                stream_ctx->stream_id, stream_ctx->receive_state);
                            ret = -1;
                        }
                        else {
                            char url_text[256];
                            /* Process initial request */
                            stream_ctx->is_datagram = (incoming.message_type == QUICRQ_ACTION_OPEN_DATAGRAM);
                            /* Open the media -- TODO, variants with different actions. */
                            quicrq_log_message(stream_ctx->cnx_ctx, "Stream %" PRIu64 ", received a subscribe request for url %s, mode = %s",
                                stream_ctx->stream_id, quicrq_uint8_t_to_text(incoming.url, incoming.url_length, url_text, 256), 
                                (stream_ctx->is_datagram)?"datagram":"stream");
                            ret = quicrq_subscribe_local_media(stream_ctx, incoming.url, incoming.url_length);
                            if (ret == 0) {
                                quicrq_wakeup_media_stream(stream_ctx);
                            }
                            stream_ctx->is_sender = 1;
                            if (incoming.message_type == QUICRQ_ACTION_OPEN_STREAM) {
                                stream_ctx->send_state = quicrq_sending_stream;
                                stream_ctx->receive_state = quicrq_receive_done;
                                picoquic_mark_active_stream(stream_ctx->cnx_ctx->cnx, stream_ctx->stream_id, 1, stream_ctx);
                            }
                            else {
                                stream_ctx->send_state = quicrq_sending_ready;
                                stream_ctx->receive_state = quicrq_receive_done;
                            }
                        }
                        break;
                    case QUICRQ_ACTION_POST:
                        if (stream_ctx->receive_state != quicrq_receive_initial) {
                            quicrq_log_message(stream_ctx->cnx_ctx, "Stream %" PRIu64 ", unexpected publish message is stream receive state %d",
                                stream_ctx->stream_id, stream_ctx->receive_state);
                            ret = -1;
                        }
                        else {
                            char url_text[256];
                            quicrq_log_message(stream_ctx->cnx_ctx, "Stream %" PRIu64 ", received a publish request for url %s, mode = %s",
                                stream_ctx->stream_id, quicrq_uint8_t_to_text(incoming.url, incoming.url_length, url_text, 256),
                                (incoming.use_datagram) ? "datagram" : "stream");
                            /* Decide whether to receive the data as stream or as datagrams */
                            /* Prepare a consumer for the data. */
                            ret = quicrq_cnx_accept_media(stream_ctx, incoming.url, incoming.url_length, incoming.use_datagram);
                        }
                        break;
                    case QUICRQ_ACTION_ACCEPT:
                        /* Verify that the client just started a "post" -- if (stream_ctx->receive_state != quicrq_receive_initial) { */
                        /* Open the media provider */
                        /* Depending on mode, set media ready or datagram ready */
                        quicrq_log_message(stream_ctx->cnx_ctx, "Stream %" PRIu64 ", publish request accepted, mode = %s",
                            stream_ctx->stream_id, (incoming.use_datagram) ? "datagram" : "stream");
                        ret = quicrq_cnx_post_accepted(stream_ctx, incoming.use_datagram, incoming.datagram_stream_id);
                        break;
                    case QUICRQ_ACTION_FIN_DATAGRAM:
                        if (stream_ctx->receive_state != quicrq_receive_repair || stream_ctx->final_object_id != 0) {
                            /* Protocol error */
                            ret = -1;
                        }
                        else {
                            /* Pass the final offset to the media consumer. */
                            quicrq_log_message(stream_ctx->cnx_ctx, "Stream %" PRIu64 ", final object notified: %" PRIu64,
                                stream_ctx->stream_id, stream_ctx->final_object_id);
                            stream_ctx->final_object_id = incoming.object_id;
                            ret = stream_ctx->consumer_fn(quicrq_media_final_object_id, stream_ctx->media_ctx, picoquic_get_quic_time(stream_ctx->cnx_ctx->qr_ctx->quic), NULL,
                                stream_ctx->final_object_id, 0, 0, 0);
                            ret = quicrq_cnx_handle_consumer_finished(stream_ctx, 1, 0, ret);
                        }
                        break;
                    case QUICRQ_ACTION_REQUEST_REPAIR:
                        /* TODO - implement that */
                        ret = -1;
                        break;
                    case QUICRQ_ACTION_REPAIR:
                        if (stream_ctx->receive_state != quicrq_receive_repair) {
                            /* Protocol error */
                            ret = -1;
                        }
                        else {
                            /* Pass the repair data to the media consumer. */
                            ret = stream_ctx->consumer_fn(quicrq_media_datagram_ready, stream_ctx->media_ctx, picoquic_get_quic_time(stream_ctx->cnx_ctx->qr_ctx->quic),
                                incoming.data, incoming.object_id, incoming.offset, incoming.is_last_fragment, incoming.length);
                            ret = quicrq_cnx_handle_consumer_finished(stream_ctx, 0, 0, ret);
                        }
                        break;
                    default:
                        /* Some unknown message, maybe not implemented yet */
                        ret = -1;
                        break;
                    }
                    /* As the message was processed, reset the message buffer. */
                    quicrq_msg_buffer_reset(&stream_ctx->message_receive);
                }
            }
        }
    }

    if (is_fin) {
        /* The peer is finished. */
        stream_ctx->is_peer_finished = 1;
        if (stream_ctx->is_local_finished) {
            quicrq_cnx_ctx_t* cnx_ctx = stream_ctx->cnx_ctx;
            quicrq_delete_stream_ctx(cnx_ctx, stream_ctx);
        }
        else {
            stream_ctx->send_state = quicrq_sending_fin;
            picoquic_mark_active_stream(stream_ctx->cnx_ctx->cnx, stream_ctx->stream_id, 1, stream_ctx);
        }
    }

    return ret;
}

/* Callback from Quic
 */
int quicrq_callback(picoquic_cnx_t* cnx,
    uint64_t stream_id, uint8_t* bytes, size_t length,
    picoquic_call_back_event_t fin_or_event, void* callback_ctx, void* v_stream_ctx)
{

    int ret = 0;
    quicrq_cnx_ctx_t* cnx_ctx = (quicrq_cnx_ctx_t*)callback_ctx;
    quicrq_stream_ctx_t* stream_ctx = (quicrq_stream_ctx_t*)v_stream_ctx;

    /* If this is the first reference to the connection, the application context is set
     * to the default value defined for the server. This default value contains the pointer
     * to the global context in which streams and roles are defined.
     */
    if (callback_ctx == NULL || callback_ctx == picoquic_get_default_callback_context(picoquic_get_quic_ctx(cnx))) {
        if (fin_or_event == picoquic_callback_close) {
            picoquic_set_callback(cnx, NULL, NULL);
            return 0;
        }
        else {
            cnx_ctx = quicrq_create_cnx_context((quicrq_ctx_t*)callback_ctx, cnx);
            if (cnx_ctx == NULL) {
                /* cannot handle the connection */
                picoquic_close(cnx, PICOQUIC_ERROR_MEMORY);
                return -1;
            }
            else {
                picoquic_set_callback(cnx, quicrq_callback, cnx_ctx);
            }
        }
    }

    if (ret == 0) {
        switch (fin_or_event) {
        case picoquic_callback_stream_data:
        case picoquic_callback_stream_fin:
            /* Data arrival on stream #x, maybe with fin mark */
            if (stream_ctx == NULL) {
                /* Retrieve, or create and initialize stream context */
                stream_ctx = quicrq_find_or_create_stream(stream_id, cnx_ctx, 1);
            }

            if (stream_ctx == NULL) {
                /* Internal error */
                (void)picoquic_reset_stream(cnx, stream_id, QUICRQ_ERROR_INTERNAL);
                return(-1);
            }
            else {
                ret = quicrq_receive_stream_data(stream_ctx, bytes, length, (fin_or_event == picoquic_callback_stream_fin));
            }
            break;
        case picoquic_callback_prepare_to_send:
            /* Active sending API */
            if (stream_ctx == NULL) {
                /* This should never happen */
                picoquic_log_app_message(cnx, "QUICRQ callback returns %d, event %d", ret, fin_or_event);
                DBG_PRINTF("Prepare to send on NULL context, steam: %" PRIu64, stream_id);
                ret = -1;
            }
            else {
                ret = quicrq_prepare_to_send_on_stream(stream_ctx, bytes, length, picoquic_get_quic_time(stream_ctx->cnx_ctx->qr_ctx->quic));
            }
            break;
        case picoquic_callback_datagram:
            /* Receive data in a datagram */
            ret = quicrq_receive_datagram(cnx_ctx, bytes, length, picoquic_get_quic_time(cnx_ctx->qr_ctx->quic));
            break;
        case picoquic_callback_prepare_datagram:
            /* Prepare to send a datagram */
            ret = quicrq_prepare_to_send_datagram(cnx_ctx, bytes, length, picoquic_get_quic_time(cnx_ctx->qr_ctx->quic));
            break;
        case picoquic_callback_stream_reset: /* Client reset stream #x */
        case picoquic_callback_stop_sending: /* Client asks server to reset stream #x */
#if 0
            /* TODO: react to abandon stream, etc. */
            if (stream_ctx != NULL) {
                /* Mark stream as abandoned, close the file, etc. */
                sample_server_delete_stream_context(cnx_ctx, stream_ctx);
                picoquic_reset_stream(cnx, stream_id, PICOQUIC_SAMPLE_FILE_CANCEL_ERROR);
            }
#endif
            break;
        case picoquic_callback_stateless_reset: /* Received an error message */
        case picoquic_callback_close: /* Received connection close */
        case picoquic_callback_application_close: /* Received application close */
            /* Remove the connection from the context, and then delete it */
            cnx_ctx->cnx = NULL;
            quicrq_delete_cnx_context(cnx_ctx);
            picoquic_set_callback(cnx, NULL, NULL);
            break;
        case picoquic_callback_version_negotiation:
            /* The server should never receive a version negotiation response */
            break;
        case picoquic_callback_stream_gap:
            /* This callback is never used. */
            break;
        case picoquic_callback_almost_ready:
        case picoquic_callback_ready:
            /* Check that the transport parameters are what the sample expects */
            break;
        case picoquic_callback_datagram_acked:
            /* Ack for packet carrying datagram-object received from peer */
        case picoquic_callback_datagram_lost:
            /* Packet carrying datagram-object probably lost */
        case picoquic_callback_datagram_spurious:
            /* Packet carrying datagram-object was not really lost */
            ret = quicrq_handle_datagram_ack_nack(cnx_ctx, fin_or_event, stream_id /* encodes the send time!*/, bytes, length);
            break;
        case picoquic_callback_pacing_changed:
            /* Notification of rate change from congestion controller */
            break;
        default:
            /* unexpected */
            break;
        }
    }

    if (ret != 0) {
        picoquic_log_app_message(cnx, "QUICRQ callback returns %d, event %d", ret, fin_or_event);
        DBG_PRINTF("QUICRQ callback returns %d, event %d", ret, fin_or_event);
    }


    return ret;
}

void quicrq_init_transport_parameters(picoquic_tp_t* tp, int client_mode)
{
    memset(tp, 0, sizeof(picoquic_tp_t));
    tp->initial_max_stream_data_bidi_local = 0x200000;
    tp->initial_max_stream_data_bidi_remote = 65635;
    tp->initial_max_stream_data_uni = 65535;
    tp->initial_max_data = 0x100000;
    if (client_mode) {
        tp->initial_max_stream_id_bidir = 2049;
        tp->initial_max_stream_id_unidir = 2051;
    }
    else {
        tp->initial_max_stream_id_bidir = 2048;
        tp->initial_max_stream_id_unidir = 2050;
    }
    tp->idle_timeout = 30000;
    tp->max_packet_size = PICOQUIC_MAX_PACKET_SIZE;
    tp->ack_delay_exponent = 3;
    tp->active_connection_id_limit = 4;
    tp->max_ack_delay = 10000ull;
    tp->enable_loss_bit = 2;
    tp->min_ack_delay = 1000ull;
    tp->enable_time_stamp = 0;
    tp->max_datagram_frame_size = PICOQUIC_MAX_PACKET_SIZE;
}

/* get the quic context from quicqr context */
picoquic_quic_t* quicrq_get_quic_ctx(quicrq_ctx_t* qr_ctx)
{
    return (qr_ctx==NULL)?NULL:qr_ctx->quic;
}

/* Delete a QUICR configuration */
void quicrq_delete(quicrq_ctx_t* qr_ctx)
{
    struct st_quicrq_cnx_ctx_t* cnx_ctx = qr_ctx->first_cnx;
    struct st_quicrq_cnx_ctx_t* next = NULL;
    struct st_quicrq_media_source_ctx_t* srce_ctx = qr_ctx->first_source;
    struct st_quicrq_media_source_ctx_t* srce_next = NULL;

    while (cnx_ctx != NULL) {
        next = cnx_ctx->next_cnx;
        quicrq_delete_cnx_context(cnx_ctx);
        cnx_ctx = next;
    }

    while (srce_ctx != NULL) {
        srce_next = srce_ctx->next_source;
        quicrq_delete_source(srce_ctx, qr_ctx);
        srce_ctx = srce_next;
    }

    if (qr_ctx->quic != NULL) {
        picoquic_free(qr_ctx->quic);
    }

    quicrq_disable_relay(qr_ctx);

    free(qr_ctx);
}

/* Create a QUICRQ context
 * TODO: consider passing a picoquic configuration object 
 */
quicrq_ctx_t* quicrq_create_empty()
{
    quicrq_ctx_t* qr_ctx = (quicrq_ctx_t*)malloc(sizeof(quicrq_ctx_t));

    if (qr_ctx != NULL) {
        memset(qr_ctx, 0, sizeof(quicrq_ctx_t));
    }
    return qr_ctx;
}

void quicrq_set_quic(quicrq_ctx_t* qr_ctx, picoquic_quic_t* quic)
{
    qr_ctx->quic = quic;
}

quicrq_ctx_t* quicrq_create(char const* alpn,
    char const* cert_file_name, char const* key_file_name, char const* cert_root_file_name,
    char const* ticket_store_file_name, char const* token_store_file_name,
    const uint8_t* ticket_encryption_key, size_t ticket_encryption_key_length,
    uint64_t* p_simulated_time)
{
    quicrq_ctx_t* qr_ctx = quicrq_create_empty();
    uint64_t current_time = (p_simulated_time == NULL) ? picoquic_current_time() : *p_simulated_time;

    if (qr_ctx != NULL) {
        qr_ctx->quic = picoquic_create(QUICRQ_MAX_CONNECTIONS, cert_file_name, key_file_name, cert_root_file_name, alpn,
            quicrq_callback, qr_ctx, NULL, NULL, NULL, current_time, p_simulated_time,
            ticket_store_file_name, ticket_encryption_key, ticket_encryption_key_length);

        if (qr_ctx->quic == NULL ||
            (token_store_file_name != NULL && picoquic_load_retry_tokens(qr_ctx->quic, token_store_file_name) != 0)) {
            quicrq_delete(qr_ctx);
            qr_ctx = NULL;
        }
    }
    return qr_ctx;
}

/* Delete a connection context */
void quicrq_delete_cnx_context(quicrq_cnx_ctx_t* cnx_ctx)
{
    /* Delete the stream contexts */
    while (cnx_ctx->first_stream != NULL) {
        quicrq_delete_stream_ctx(cnx_ctx, cnx_ctx->first_stream);
    }

    /* Delete the quic connection */
    if (cnx_ctx->cnx != NULL) {
        picoquic_set_callback(cnx_ctx->cnx, NULL, NULL);
        picoquic_delete_cnx(cnx_ctx->cnx);
        cnx_ctx->cnx = NULL;
    }
    /* Remove the connection from the double linked list */
    if (cnx_ctx->qr_ctx != NULL) {
        if (cnx_ctx->next_cnx == NULL) {
            cnx_ctx->qr_ctx->last_cnx = cnx_ctx->previous_cnx;
        }
        else {
            cnx_ctx->next_cnx->previous_cnx = cnx_ctx->previous_cnx;
        }
        if (cnx_ctx->previous_cnx == NULL) {
            cnx_ctx->qr_ctx->first_cnx = cnx_ctx->next_cnx;
        }
        else {
            cnx_ctx->previous_cnx->next_cnx = cnx_ctx->next_cnx;
        }
    }
    /* Free the context */
    free(cnx_ctx);
}

/* Create a connection context. 
 * The QUIC connection has to be created before the QUICRQ connection. */
quicrq_cnx_ctx_t* quicrq_create_cnx_context(quicrq_ctx_t* qr_ctx, picoquic_cnx_t * cnx)
{
    quicrq_cnx_ctx_t* cnx_ctx = (quicrq_cnx_ctx_t*)malloc(sizeof(quicrq_cnx_ctx_t));

    if (cnx_ctx != NULL) {
        memset(cnx_ctx, 0, sizeof(quicrq_cnx_ctx_t));
        /* document quic connection */
        cnx_ctx->cnx = cnx;
        /* Add the connection in the double linked list */
        if (qr_ctx->last_cnx == NULL) {
            qr_ctx->first_cnx = cnx_ctx;
        }
        else {
            qr_ctx->last_cnx->next_cnx = cnx_ctx;
        }
        cnx_ctx->previous_cnx = qr_ctx->last_cnx;
        qr_ctx->last_cnx = cnx_ctx;
        cnx_ctx->qr_ctx = qr_ctx;
        picoquic_set_callback(cnx, quicrq_callback, cnx_ctx);
    }
    return cnx_ctx;
}

/* Create a client connection.
 */
quicrq_cnx_ctx_t* quicrq_create_client_cnx(quicrq_ctx_t* qr_ctx,
    const char* sni, struct sockaddr* addr)
{
    quicrq_cnx_ctx_t* cnx_ctx = NULL;
    picoquic_tp_t client_parameters;
    picoquic_cnx_t * cnx = picoquic_create_cnx(qr_ctx->quic,
        picoquic_null_connection_id, picoquic_null_connection_id,
        addr, picoquic_get_quic_time(qr_ctx->quic), 0, sni, QUICRQ_ALPN, 1);
    /* Set parameters */
    if (cnx != NULL) {
        quicrq_init_transport_parameters(&client_parameters, 1);
        picoquic_set_transport_parameters(cnx, &client_parameters);

        if (picoquic_start_client_cnx(cnx) != 0) {
            picoquic_delete_cnx(cnx);
            cnx = NULL;
        }
        if (cnx != NULL) {
            cnx_ctx = quicrq_create_cnx_context(qr_ctx, cnx);
            if (cnx_ctx == NULL) {
                picoquic_delete_cnx(cnx);
            }
        }
    }
    return cnx_ctx;
}

/* Access the server address behind a quicrq connection context
 */
void quicrq_get_peer_address(quicrq_cnx_ctx_t* cnx_ctx, struct sockaddr_storage* stored_addr)
{
    struct sockaddr* peer_addr;

    picoquic_get_peer_addr(cnx_ctx->cnx, &peer_addr);
    picoquic_store_addr(stored_addr, peer_addr);
}

quicrq_cnx_ctx_t* quicrq_first_connection(quicrq_ctx_t* qr_ctx)
{
    return qr_ctx->first_cnx;
}

void quicrq_delete_stream_ctx(quicrq_cnx_ctx_t* cnx_ctx, quicrq_stream_ctx_t* stream_ctx)
{
    quicrq_datagram_ack_ctx_release(stream_ctx);

    if (stream_ctx->next_stream == NULL) {
        cnx_ctx->last_stream = stream_ctx->previous_stream;
    }
    else {
        stream_ctx->next_stream->previous_stream = stream_ctx->previous_stream;
    }
    if (stream_ctx->previous_stream == NULL) {
        cnx_ctx->first_stream = stream_ctx->next_stream;
    }
    else {
        stream_ctx->previous_stream->next_stream = stream_ctx->next_stream;
    }

    quicrq_unsubscribe_local_media(stream_ctx);

    while (stream_ctx->datagram_repair_first != NULL) {
        quicrq_remove_repair_in_stream_ctx(stream_ctx, stream_ctx->datagram_repair_first);
    }

    if (cnx_ctx->cnx != NULL) {
        (void)picoquic_mark_active_stream(cnx_ctx->cnx, stream_ctx->stream_id, 0, NULL);
    }
    if (stream_ctx->media_ctx != NULL) {
        if (stream_ctx->is_sender) {
            if (stream_ctx->publisher_fn != NULL) {
                stream_ctx->publisher_fn(quicrq_media_source_close, stream_ctx->media_ctx, NULL, 0, NULL, NULL, NULL, NULL, 0);
            }
        }
        else {
            if (stream_ctx->consumer_fn != NULL) {
                stream_ctx->consumer_fn(quicrq_media_close, stream_ctx->media_ctx, 0, NULL, 0, 0, 0, 0);
            }
        }
    }

    quicrq_msg_buffer_release(&stream_ctx->message_receive);
    quicrq_msg_buffer_release(&stream_ctx->message_sent);

    free(stream_ctx);
}

quicrq_stream_ctx_t* quicrq_create_stream_context(quicrq_cnx_ctx_t* cnx_ctx, uint64_t stream_id)
{
    quicrq_stream_ctx_t* stream_ctx = (quicrq_stream_ctx_t*)malloc(sizeof(quicrq_stream_ctx_t));
    if (stream_ctx != NULL) {
        memset(stream_ctx, 0, sizeof(quicrq_stream_ctx_t));
        stream_ctx->cnx_ctx = cnx_ctx;
        stream_ctx->stream_id = stream_id;
        if (cnx_ctx->last_stream == NULL) {
            cnx_ctx->first_stream = stream_ctx;
        }
        else {
            cnx_ctx->last_stream->next_stream = stream_ctx;
        }
        stream_ctx->previous_stream = cnx_ctx->last_stream;
        cnx_ctx->last_stream = stream_ctx;
        quicrq_datagram_ack_ctx_init(stream_ctx);
    }

    return stream_ctx;
}

quicrq_stream_ctx_t* quicrq_find_or_create_stream(
    uint64_t stream_id,
    quicrq_cnx_ctx_t* cnx_ctx,
    int should_create)
{
    quicrq_stream_ctx_t* stream_ctx = cnx_ctx->first_stream;

    while (stream_ctx != NULL) {
        if (stream_ctx->stream_id == stream_id) {
            break;
        }
        stream_ctx = stream_ctx->next_stream;
    }
    if (stream_ctx == NULL && should_create) {
        stream_ctx = quicrq_create_stream_context(cnx_ctx, stream_id);
    }

    return stream_ctx;
}

int quicrq_cnx_has_stream(quicrq_cnx_ctx_t* cnx_ctx)
{
    return (cnx_ctx->first_stream != NULL);
}

int quicrq_close_cnx(quicrq_cnx_ctx_t* cnx_ctx)
{
    int ret = 0;

    if (cnx_ctx->cnx != NULL && picoquic_get_cnx_state(cnx_ctx->cnx) < picoquic_state_disconnecting) {
        ret = picoquic_close(cnx_ctx->cnx, 0);
    }

    return ret;
}

int quicrq_is_cnx_disconnected(quicrq_cnx_ctx_t* cnx_ctx)
{
    return  (cnx_ctx->cnx == NULL || picoquic_get_cnx_state(cnx_ctx->cnx) == picoquic_state_disconnected);
}


/* Media publisher API.
 * Simplified API for now:
 * - cnx_ctx: context of the QUICR connection
 * - media_url: URL of the media fragment
 * - media_publisher_fn: callback function for processing media arrival
 * - media_ctx: media context managed by the publisher
 */


/* Utility function, encode or decode a object header.
 */
const uint8_t* quicr_decode_object_header(const uint8_t* fh, const uint8_t* fh_max, quicrq_media_object_header_t* hdr)
{
    /* decode the object header */
    if ((fh = picoquic_frames_uint64_decode(fh, fh_max, &hdr->timestamp)) != NULL &&
        (fh = picoquic_frames_uint64_decode(fh, fh_max, &hdr->number)) != NULL){
        uint32_t length = 0;
        fh = picoquic_frames_uint32_decode(fh, fh_max, &length);
        hdr->length = length;
    }
    return fh;
}

uint8_t* quicr_encode_object_header(uint8_t* fh, const uint8_t* fh_max, const quicrq_media_object_header_t* hdr)
{
    /* decode the object header */
    if ((fh = picoquic_frames_uint64_encode(fh, fh_max, hdr->timestamp)) != NULL &&
        (fh = picoquic_frames_uint64_encode(fh, fh_max, hdr->number)) != NULL) {
        fh = picoquic_frames_uint32_encode(fh, fh_max, (uint32_t)hdr->length);
    }

    return fh;
}

/* Utility function, write an URL as a string. */
const char* quicrq_uint8_t_to_text(const uint8_t* u, size_t length, char* buffer, size_t buffer_length)
{
    if (buffer_length < 16) {
        return "???";
    }
    else {
        size_t available = buffer_length - 8;
        size_t i = 0;
        size_t l = 0;
        for (; l < available && i < length; i++) {
            int c = u[i];
            if (c == '\\') {
                buffer[l++] = '\\';
                buffer[l++] = '\\';
            }
            else if (c >= 32 && c <= 126 && c != '\\') {
                buffer[l++] = (char)c;
            }
            else {
                int d;

                buffer[l++] = '\\';
                d = c / 100;
                buffer[l++] = '0' + d;
                c -= 100 * d;
                d = c / 10;
                buffer[l++] = '0' + d;
                c -= 10 * d;
                buffer[l++] = '0' + c;
            }
        }
        if (i < length) {
            available = buffer_length - 1;
            for (int j = 0; j < 3 && l < available; j++) {
                buffer[l++] = '.';
            }
        }
        buffer[l++] = 0;
        return buffer;
    }
}

/* Logging function
 */
void quicrq_log_message(quicrq_cnx_ctx_t* cnx_ctx, const char* fmt, ...)
{
    if (cnx_ctx != NULL && cnx_ctx->cnx != NULL){
        va_list args;
        va_start(args, fmt);
        picoquic_log_app_message_v(cnx_ctx->cnx, fmt, args);
        va_end(args);
    }
}