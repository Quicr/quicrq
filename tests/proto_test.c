/* Tests of message coding and decoding */
#include <stdlib.h>
#include <string.h>
#include "quicrq.h"
#include "quicrq_internal.h"
#include "quicrq_tests.h"
#include "quicrq_test_internal.h"
#include "picoquic_utils.h"

/*
#define QUICRQ_ACTION_OPEN_STREAM 1
#define QUICRQ_ACTION_OPEN_DATAGRAM 2
#define QUICRQ_ACTION_FIN_DATAGRAM 3
#define QUICRQ_ACTION_REQUEST_REPAIR 4
*/

#define URL1_BYTES 'e', 'x', 'a', 'm', 'p', 'l', 'e', '.', 'c', 'o', 'm', '/', 'm', 'e', 'd', 'i', 'a'

static uint8_t url1[] = { URL1_BYTES };

static quicrq_message_t stream_rq = {
    QUICRQ_ACTION_OPEN_STREAM,
    sizeof(url1),
    url1,
    0,
    0,
    0
};

static uint8_t stream_rq_bytes[] = {
    QUICRQ_ACTION_OPEN_STREAM,
    sizeof(url1),
    URL1_BYTES
};

static quicrq_message_t datagram_rq = {
    QUICRQ_ACTION_OPEN_DATAGRAM,
    sizeof(url1),
    url1,
    1234,
    0,
    0
};

static uint8_t datagram_rq_bytes[] = {
    QUICRQ_ACTION_OPEN_DATAGRAM,
    sizeof(url1),
    URL1_BYTES,
    0x44, 0xd2
};

static quicrq_message_t fin_msg = {
    QUICRQ_ACTION_FIN_DATAGRAM,
    0,
    NULL,
    0,
    123456,
    0
};

static uint8_t fin_msg_bytes[] = {
    QUICRQ_ACTION_FIN_DATAGRAM,
    0x80, 0x01, 0xe2, 0x40
};

static quicrq_message_t repair_msg = {
    QUICRQ_ACTION_REQUEST_REPAIR,
    0,
    NULL,
    0,
    123456,
    1234
};

static uint8_t repair_msg_bytes[] = {
    QUICRQ_ACTION_REQUEST_REPAIR,
    0x80, 0x01, 0xe2, 0x40,
    0x44, 0xd2
};

typedef struct st_proto_test_case_t {
    uint8_t* const data;
    size_t data_length;
    quicrq_message_t*  result;
} proto_test_case_t;

#define PROTO_TEST_ITEM(case_name, case_bytes) { case_bytes, sizeof(case_bytes), &case_name }
static proto_test_case_t proto_cases[] = {
    PROTO_TEST_ITEM(stream_rq, stream_rq_bytes),
    PROTO_TEST_ITEM(datagram_rq, datagram_rq_bytes),
    PROTO_TEST_ITEM(fin_msg, fin_msg_bytes),
    PROTO_TEST_ITEM(repair_msg, repair_msg_bytes)
};

static uint8_t bad_bytes1[] = {
    0xcf, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    sizeof(url1),
    URL1_BYTES
};

static uint8_t bad_bytes2[] = {
    QUICRQ_ACTION_OPEN_STREAM,
    0xcf, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    URL1_BYTES
};

static uint8_t bad_bytes3[] = {
    QUICRQ_ACTION_OPEN_STREAM,
    0x8f, 0xff, 0xff, 0xff,
    URL1_BYTES
};

static uint8_t bad_bytes4[] = {
    QUICRQ_ACTION_OPEN_STREAM,
    0x4f, 0xff,
    URL1_BYTES
};

static uint8_t bad_bytes5[] = {
    QUICRQ_ACTION_OPEN_STREAM,
    sizeof(url1) + 1,
    URL1_BYTES
};

static uint8_t bad_bytes6[] = {
    QUICRQ_ACTION_OPEN_DATAGRAM,
    0xcf, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    URL1_BYTES,
    0x44, 0xd2
};

static uint8_t bad_bytes7[] = {
    QUICRQ_ACTION_OPEN_DATAGRAM,
    0x8f, 0xff, 0xff, 0xff,
    URL1_BYTES,
    0x44, 0xd2
};

static uint8_t bad_bytes8[] = {
    QUICRQ_ACTION_OPEN_DATAGRAM,
    0x4f, 0xff,
    URL1_BYTES,
    0x44, 0xd2
};

static uint8_t bad_bytes9[] = {
    QUICRQ_ACTION_OPEN_DATAGRAM,
    sizeof(url1) + 1,
    URL1_BYTES,
    0x44, 0xd2
};


typedef struct st_proto_test_bad_case_t {
    uint8_t* const data;
    size_t data_length;
} proto_test_bad_case_t;

proto_test_bad_case_t bad_test = { bad_bytes1, sizeof(bad_bytes1) };

#define PROTO_TEST_BAD_ITEM(case_bytes) { case_bytes, sizeof(case_bytes) }

static proto_test_bad_case_t proto_bad_cases[] = {
    PROTO_TEST_BAD_ITEM(bad_bytes1),
    PROTO_TEST_BAD_ITEM(bad_bytes2),
    PROTO_TEST_BAD_ITEM(bad_bytes3),
    PROTO_TEST_BAD_ITEM(bad_bytes4),
    PROTO_TEST_BAD_ITEM(bad_bytes5),
    PROTO_TEST_BAD_ITEM(bad_bytes6),
    PROTO_TEST_BAD_ITEM(bad_bytes7),
    PROTO_TEST_BAD_ITEM(bad_bytes8),
    PROTO_TEST_BAD_ITEM(bad_bytes9)
};

int proto_msg_test()
{
    int ret = 0;

    /* Decoding tests */
    for (size_t i = 0; ret == 0 && i < sizeof(proto_cases) / sizeof(proto_test_case_t); i++) {
        const uint8_t * bytes = proto_cases[i].data;
        const uint8_t * bytes_max = bytes + proto_cases[i].data_length;
        quicrq_message_t result = { 0 };

        bytes = quicrq_msg_decode(bytes, bytes_max, &result);

        if (bytes == NULL) {
            ret = -1;
        }
        else if (bytes != bytes_max) {
            ret = -1;
        }
        else if (result.message_type != proto_cases[i].result->message_type) {
            ret = -1;
        }
        else if (result.url_length != proto_cases[i].result->url_length) {
            ret = -1;
        }
        else if (result.url_length != 0 && result.url == NULL) {
            ret = -1;
        }
        else if (result.url_length != 0 && memcmp(result.url, proto_cases[i].result->url, result.url_length) != 0) {
            ret = -1;
        }
        else if (result.offset != proto_cases[i].result->offset) {
            ret = -1;
        }
        else if (result.length != proto_cases[i].result->length) {
            ret = -1;
        }
    }

    /* Encoding tests */
    for (size_t i = 0; ret == 0 && i < sizeof(proto_cases) / sizeof(proto_test_case_t); i++) {
        uint8_t msg[512];
        uint8_t* bytes = quicrq_msg_encode(msg, msg + sizeof(msg), proto_cases[i].result);

        if (bytes == NULL) {
            ret = -1;
        }
        else if (bytes - msg != proto_cases[i].data_length) {
            ret = -1;
        }
        else if (memcmp(msg, proto_cases[i].data, proto_cases[i].data_length) != 0) {
            ret = -1;
        }
    }

    /* Bad length tests */
    for (size_t i = 0; ret == 0 && i < sizeof(proto_cases) / sizeof(proto_test_case_t); i++) {
        for (size_t l = 0; l < proto_cases[i].data_length; l++) {
            const uint8_t* bytes = proto_cases[i].data;
            const uint8_t* bytes_max = bytes + l;
            quicrq_message_t result = { 0 };

            bytes = quicrq_msg_decode(bytes, bytes_max, &result);
            if (bytes != NULL) {
                ret = -1;
            }
        }
    }

    /* Bad data tests */
    for (size_t i = 0; ret == 0 && i < sizeof(proto_bad_cases) / sizeof(proto_test_bad_case_t); i++) {
        const uint8_t* bytes = proto_bad_cases[i].data;
        const uint8_t* bytes_max = bytes + proto_bad_cases[i].data_length;
        quicrq_message_t result = { 0 };

        bytes = quicrq_msg_decode(bytes, bytes_max, &result);
        if (bytes != NULL) {
            ret = -1;
        }
    }

    return ret;
}