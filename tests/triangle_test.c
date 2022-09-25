#include <string.h>
#include "picoquic_set_textlog.h"
#include "picoquic_set_binlog.h"
#include "quicrq.h"
#include "quicrq_relay.h"
#include "quicrq_internal.h"
#include "quicrq_test_internal.h"

/* Triangle test
 * Test a "triangle" configuration, in which two clients communicate through a server.
 * Client #1 post a media fragment, which the server caches.
 * Client #2 gets the media fragment.
 */

/* Create a test network */
quicrq_test_config_t* quicrq_test_triangle_config_create(uint64_t simulate_loss, uint64_t extra_delay)
{
    /* Create a configuration with three nodes, four links, one source and 8 attachment points.*/
    quicrq_test_config_t* config = quicrq_test_config_create(3, 4, 4, 1);
    if (config != NULL) {
        /* Create the contexts for the origin (0),  client-1 (1) and client-2 (2) */
        config->nodes[0] = quicrq_create(QUICRQ_ALPN,
            config->test_server_cert_file, config->test_server_key_file, NULL, NULL, NULL,
            config->ticket_encryption_key, sizeof(config->ticket_encryption_key),
            &config->simulated_time);
        config->nodes[1] = quicrq_create(QUICRQ_ALPN,
            NULL, NULL, config->test_server_cert_store_file, NULL, NULL,
            NULL, 0, &config->simulated_time);
        config->nodes[2] = quicrq_create(QUICRQ_ALPN,
            NULL, NULL, config->test_server_cert_store_file, NULL, NULL,
            NULL, 0, &config->simulated_time);
        if (config->nodes[0] == NULL || config->nodes[1] == NULL || config->nodes[2] == NULL) {
            quicrq_test_config_delete(config);
            config = NULL;
        }
    }
    if (config != NULL) {
        /* Populate the attachments */
        config->return_links[0] = 1;
        config->attachments[0].link_id = 0;
        config->attachments[0].node_id = 0;
        config->return_links[1] = 0;
        config->attachments[1].link_id = 1;
        config->attachments[1].node_id = 1;
        config->return_links[2] = 3;
        config->attachments[2].link_id = 2;
        config->attachments[2].node_id = 0;
        config->return_links[3] = 2;
        config->attachments[3].link_id = 3;
        config->attachments[3].node_id = 2;
        /* Set the desired loss pattern */
        config->simulate_loss = simulate_loss;

        /* set the extra delay as specified.
         * we only test for extra delays in the triangle tests.
         */
        for (int i = 0; i < config->nb_nodes; i++) {
            if (extra_delay > 0) {
                quicrq_set_extra_repeat(config->nodes[i], 0, 1);
            }
            quicrq_set_extra_repeat_delay(config->nodes[i], extra_delay);
        }
    }
    return config;
}

/* Basic relay test */
int quicrq_triangle_test_one(int is_real_time, int use_datagrams, uint64_t simulate_losses, uint64_t extra_delay, uint64_t start_point,
    int test_cache_clear, int test_intent)
{
    int ret = 0;
    int nb_steps = 0;
    int nb_inactive = 0;
    int is_closed = 0;
    const uint64_t max_time = 360000000;
    const int max_inactive = 128;
    quicrq_test_config_t* config = quicrq_test_triangle_config_create(simulate_losses, extra_delay);
    quicrq_cnx_ctx_t* cnx_ctx_1 = NULL;
    quicrq_cnx_ctx_t* cnx_ctx_2 = NULL;
    char media_source_path[512];
    char result_file_name[512];
    char result_log_name[512];
    char text_log_name[512];
    size_t nb_log_chars = 0;
    int partial_closure = 0;
    uint64_t client2_close_time = UINT64_MAX;
    int subscribed = 0;
    uint64_t start_group_intent = 0;
    uint64_t start_object_intent = 0;

    (void)picoquic_sprintf(text_log_name, sizeof(text_log_name), &nb_log_chars, "triangle_textlog-%d-%d-%llx-%llu-%llu-%d-%d.txt", is_real_time, use_datagrams,
        (unsigned long long)simulate_losses, (unsigned long long) extra_delay, (unsigned long long) start_point, test_cache_clear, test_intent);
    /* TODO: name shall indicate the triangle configuration */
    ret = test_media_derive_file_names((uint8_t*)QUICRQ_TEST_BASIC_SOURCE, strlen(QUICRQ_TEST_BASIC_SOURCE),
        use_datagrams, is_real_time, 1,
        result_file_name, result_log_name, sizeof(result_file_name));

    if (config == NULL) {
        ret = -1;
    }

    /* Locate the source and reference file */
    if (picoquic_get_input_path(media_source_path, sizeof(media_source_path),
        quicrq_test_solution_dir, QUICRQ_TEST_BASIC_SOURCE) != 0) {
        ret = -1;
    }

    /* Add QUIC level log */
    if (ret == 0) {
        ret = picoquic_set_textlog(config->nodes[1]->quic, text_log_name);
    }

    if (ret == 0) {
        /* Enable origin on node 0 */
        ret = quicrq_enable_origin(config->nodes[0], use_datagrams);
        if (ret != 0) {
            DBG_PRINTF("Cannot enable origin, ret = %d", ret);
        }
    }

    if (ret == 0) {
        /* Add a test source to the configuration on client #1 (publisher) */
        quicrq_media_object_source_properties_t properties = { 0 };
        int publish_node = 1;

        if (test_cache_clear || test_intent > 0) {
            properties.use_real_time_caching = 1;
            quicrq_set_cache_duration(config->nodes[0], 5000000);
        }

        config->object_sources[0] = test_media_object_source_publish_ex(config->nodes[publish_node], (uint8_t*)QUICRQ_TEST_BASIC_SOURCE,
            strlen(QUICRQ_TEST_BASIC_SOURCE), media_source_path, NULL, is_real_time, config->simulated_time, &properties);
        if (config->object_sources[0] == NULL) {
            ret = -1;
        }
        else if (start_point > 0) {
            ret = test_media_object_source_set_start(config->object_sources[0], 0, start_point);
        }
    }

    if (ret == 0) {
        /* Create a quicrq connection context on client #1 */
        cnx_ctx_1 = quicrq_test_create_client_cnx(config, 1, 0);
        if (cnx_ctx_1 == NULL) {
            ret = -1;
            DBG_PRINTF("Cannot create client connection #1, ret = %d", ret);
        }
    }

    if (ret == 0) {
        /* Create a quicrq connection context on client #2 */
        cnx_ctx_2 = quicrq_test_create_client_cnx(config, 2, 0);
        if (cnx_ctx_2 == NULL) {
            ret = -1;
            DBG_PRINTF("Cannot create client connection #2, ret = %d", ret);
        }
    }

    if (ret == 0) {
        /* Start pushing from the client #1 */
        ret = quicrq_cnx_post_media(cnx_ctx_1, (uint8_t*)QUICRQ_TEST_BASIC_SOURCE, strlen(QUICRQ_TEST_BASIC_SOURCE), use_datagrams);
        if (ret != 0) {
            DBG_PRINTF("Cannot publish test media %s, ret = %d", QUICRQ_TEST_BASIC_SOURCE, ret);
        }
    }

    if (ret == 0) {
        if (test_intent > 0) {
            config->next_test_event_time = 4000000;
        } else {
            /* Create a subscription to the test source on client # 2*/
            test_object_stream_ctx_t* object_stream_ctx = NULL;
            object_stream_ctx = test_object_stream_subscribe(cnx_ctx_2, (const uint8_t*)QUICRQ_TEST_BASIC_SOURCE,
                strlen(QUICRQ_TEST_BASIC_SOURCE), use_datagrams, result_file_name, result_log_name);
            if (object_stream_ctx == NULL) {
                ret = -1;
            }
            else {
                subscribed = 1;
            }
            if (ret != 0) {
                DBG_PRINTF("Cannot subscribe to test media %s, ret = %d", QUICRQ_TEST_BASIC_SOURCE, ret);
            }
        }
    }

    while (ret == 0 && nb_inactive < max_inactive && config->simulated_time < max_time) {
        /* Run the simulation. Monitor the connection. Monitor the media. */
        int is_active = 0;

        if (!subscribed && config->simulated_time >= config->next_test_event_time) {
            /* Create a subscription to the test source on client # 2*/
            test_object_stream_ctx_t* object_stream_ctx = NULL;
            quicrq_subscribe_intent_t intent = { 0 };
            object_stream_ctx = test_object_stream_subscribe_ex(cnx_ctx_2, (const uint8_t*)QUICRQ_TEST_BASIC_SOURCE,
                strlen(QUICRQ_TEST_BASIC_SOURCE), use_datagrams, &intent, result_file_name, result_log_name);
            if (object_stream_ctx == NULL) {
                ret = -1;
                break;
            }
            else {
                subscribed = 1;
            }
            config->next_test_event_time = UINT64_MAX;
            if (intent.intent_mode == quicrq_subscribe_intent_current_group) {
                start_group_intent = 1;
                start_object_intent = 0;
            }
        }

        ret = quicrq_test_loop_step(config, &is_active, UINT64_MAX);
        if (ret != 0) {
            DBG_PRINTF("Fail on loop step %d, %d, active: ret=%d", nb_steps, is_active, ret);
        }

        nb_steps++;

        if (is_active) {
            nb_inactive = 0;
        }
        else {
            nb_inactive++;
            if (nb_inactive >= max_inactive) {
                DBG_PRINTF("Exit loop after too many inactive: %d", nb_inactive);
            }
        }
        /* if the media is sent and received, exit the loop */
        if (config->nodes[1]->first_cnx == NULL && config->nodes[2]->first_cnx == NULL) {
            DBG_PRINTF("%s", "Exit loop after client connection #2 closed.");
            break;
        }
        else {
            /* TODO: add closing condition on server */
            int client1_stream_closed = config->nodes[1]->first_cnx == NULL || config->nodes[1]->first_cnx->first_stream == NULL;
            int client2_stream_closed = config->nodes[2]->first_cnx == NULL ||
                (config->nodes[2]->first_cnx->first_stream == NULL && subscribed);

            if (client2_stream_closed && client2_close_time > config->simulated_time) {
                client2_close_time = config->simulated_time;
            }

            if (!is_closed && client1_stream_closed && client2_stream_closed) {
                /* Client is done. Close connections without waiting for timer -- if not closed yet */
                is_closed = 1;
                for (int c_nb = 1; ret == 0 && c_nb < 3; c_nb++) {
                    if (config->nodes[c_nb]->first_cnx != NULL) {
                        ret = quicrq_close_cnx(config->nodes[c_nb]->first_cnx);
                        if (ret != 0) {
                            DBG_PRINTF("Cannot close client connection, ret = %d", ret);
                        }
                    }
                }
            }
            else if (client1_stream_closed ^ client2_stream_closed) {
                if (!partial_closure) {
                    partial_closure = 1;
                    DBG_PRINTF("Partial closure: client 1 (%d), client 2 (%d), time = %" PRIu64, 
                        client1_stream_closed, client2_stream_closed, config->simulated_time);
                }
            }
        }
    }

    if (ret == 0 && (!is_closed || client2_close_time > 12000000)) {
        DBG_PRINTF("Session was not properly closed, time = %" PRIu64, client2_close_time);
        ret = -1;
    }

    if (ret == 0 && test_cache_clear) {
        /* Check that relay sources are deleted after a sufficient timer */
        uint64_t cache_time = config->simulated_time + 10000000;

        while (ret == 0 && nb_inactive < max_inactive && config->simulated_time < cache_time) {
            /* Run the simulation until the media caches have been deleted */
            int is_active = 0;

            ret = quicrq_test_loop_step(config, &is_active, UINT64_MAX);
            if (ret != 0) {
                DBG_PRINTF("Fail on cache loop step %d, %d, active: ret=%d", nb_steps, is_active, ret);
            }

            nb_steps++;

            if (is_active) {
                nb_inactive = 0;
            }
            else {
                nb_inactive++;
                if (nb_inactive >= max_inactive) {
                    DBG_PRINTF("Exit cache loop after too many inactive: %d", nb_inactive);
                }
            }
            /* Check whether the media is closed at origin and relay */
            if (config->nodes[0]->first_source == NULL) {
                DBG_PRINTF("Origin cache deleted at %" PRIu64, config->simulated_time);
                break;
            }
        }

        if (ret == 0 && config->nodes[0]->first_source != NULL) {
            DBG_PRINTF("Origin cache not deleted at %" PRIu64, config->simulated_time);
            ret = -1;
        }
    }

    /* Clear everything. */
    if (config != NULL) {
        quicrq_test_config_delete(config);
    }
    /* Verify that media file was received correctly */
    if (ret == 0) {
        ret = quicrq_compare_media_file_ex(result_file_name, media_source_path, NULL, NULL, start_group_intent, start_object_intent);
    }
    else {
        DBG_PRINTF("Test failed before getting results, ret = %d", ret);
    }

    return ret;
}

int quicrq_triangle_basic_test()
{
    int ret = quicrq_triangle_test_one(1, 0, 0, 0, 0, 0, 0);

    return ret;
}

int quicrq_triangle_basic_loss_test()
{
    int ret = quicrq_triangle_test_one(1, 0, 0x7080, 0, 0, 0, 0);

    return ret;
}

int quicrq_triangle_datagram_test()
{
    int ret = quicrq_triangle_test_one(1, 1, 0, 0, 0, 0, 0);

    return ret;
}

int quicrq_triangle_datagram_loss_test()
{
    int ret = quicrq_triangle_test_one(1, 1, 0x7080, 0, 0, 0, 0);

    return ret;
}

int quicrq_triangle_datagram_extra_test()
{
    int ret = quicrq_triangle_test_one(1, 1, 0x7080, 10000, 0, 0, 0);

    return ret;
}

int quicrq_triangle_start_point_test()
{
    int ret = quicrq_triangle_test_one(1, 1, 0x7080, 10000, 12345, 0, 0);

    return ret;
}

int quicrq_triangle_cache_test()
{
    int ret = quicrq_triangle_test_one(1, 1, 0, 0, 0, 1, 0);

    return ret;
}

int quicrq_triangle_cache_loss_test()
{
    int ret = quicrq_triangle_test_one(1, 1, 0x7080, 0, 0, 1, 0);

    return ret;
}

int quicrq_triangle_cache_stream_test()
{
    int ret = quicrq_triangle_test_one(1, 0, 0, 0, 0, 1, 0);

    return ret;
}

int quicrq_triangle_intent_test()
{
    int ret = quicrq_triangle_test_one(1, 0, 0, 0, 0, 1, 1);

    return ret;
}