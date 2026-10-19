/* rmx-backend-probe JSON envelopes (crates/probe-protocol in rmx-backend-probe). */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PROBE_REQUEST_ID_LEN 32

typedef enum {
    PROBE_OP_LIST,
    PROBE_OP_CREATE,
    PROBE_OP_UPDATE,
    PROBE_OP_DELETE,
} probe_op_t;

/* What a response or a REST body told us, in short. */
typedef struct {
    char request_id[PROBE_REQUEST_ID_LEN + 1];
    bool ok;                 /* result.status == "ok" */
    char kind[16];           /* "record", "records", "deleted" */
    int64_t id;              /* record id (record / deleted), -1 if none */
    int count;               /* number of records (records) */
    char connection_type[8]; /* "mqtt" / "https" / "wss" for a record */
    char error_code[24];     /* on error */
    char error_message[96];
} probe_result_t;

/* Fills buf with a new unique request id ("dev-<boot>-<n>"). */
void probe_proto_new_request_id(char buf[PROBE_REQUEST_ID_LEN + 1]);

/* {"request_id":..,"request":{"op":..,...}}. value may be NULL (JSON null);
 * id is used for update/delete. Returns a malloc'd string (free it). */
char *probe_proto_build_request(const char *request_id, probe_op_t op, int64_t id,
                                const char *value);

/* {"value": value|null}. Returns a malloc'd string. */
char *probe_proto_build_record_input(const char *value);

/* {"username":..,"password":..}. Returns a malloc'd string. */
char *probe_proto_build_login(const char *username, const char *password);

/* Extracts only request_id from a response envelope; false if absent. */
bool probe_proto_peek_request_id(const char *json, size_t len, char out[PROBE_REQUEST_ID_LEN + 1]);

/* Parses a ResponseEnvelope (MQTT). */
bool probe_proto_parse_response(const char *json, size_t len, probe_result_t *out);

/* Parses a REST body: a RecordRow object, a RecordRow array, or an
 * ApiError {"code","message"}. */
bool probe_proto_parse_rest(const char *json, size_t len, probe_result_t *out);
