#include "probe_proto.h"

#include <stdio.h>
#include <string.h>
#include "cJSON.h"
#include "esp_random.h"

static const char *const OP_NAMES[] = { "list", "create", "update", "delete" };

void probe_proto_new_request_id(char buf[PROBE_REQUEST_ID_LEN + 1])
{
    static uint32_t boot_tag;
    static uint32_t counter;
    if (!boot_tag) {
        boot_tag = esp_random() | 1;
    }
    snprintf(buf, PROBE_REQUEST_ID_LEN + 1, "dev-%08lx-%lu",
             (unsigned long)boot_tag, (unsigned long)++counter);
}

static void add_value(cJSON *obj, const char *value)
{
    if (value) {
        cJSON_AddStringToObject(obj, "value", value);
    } else {
        cJSON_AddNullToObject(obj, "value");
    }
}

static char *print_and_free(cJSON *root)
{
    char *s = root ? cJSON_PrintUnformatted(root) : NULL;
    cJSON_Delete(root);
    return s;
}

char *probe_proto_build_request(const char *request_id, probe_op_t op, int64_t id,
                                const char *value)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "request_id", request_id);
    cJSON *req = cJSON_AddObjectToObject(root, "request");
    cJSON_AddStringToObject(req, "op", OP_NAMES[op]);
    if (op == PROBE_OP_UPDATE || op == PROBE_OP_DELETE) {
        cJSON_AddNumberToObject(req, "id", (double)id);
    }
    if (op == PROBE_OP_CREATE || op == PROBE_OP_UPDATE) {
        add_value(req, value);
    }
    return print_and_free(root);
}

char *probe_proto_build_record_input(const char *value)
{
    cJSON *root = cJSON_CreateObject();
    add_value(root, value);
    return print_and_free(root);
}

char *probe_proto_build_login(const char *username, const char *password)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "username", username);
    cJSON_AddStringToObject(root, "password", password);
    return print_and_free(root);
}

static void copy_str(char *dst, size_t size, const cJSON *item)
{
    const char *s = cJSON_GetStringValue(item);
    snprintf(dst, size, "%s", s ? s : "");
}

bool probe_proto_peek_request_id(const char *json, size_t len, char out[PROBE_REQUEST_ID_LEN + 1])
{
    cJSON *root = cJSON_ParseWithLength(json, len);
    const char *s = cJSON_GetStringValue(cJSON_GetObjectItem(root, "request_id"));
    bool found = s != NULL;
    if (found) {
        snprintf(out, PROBE_REQUEST_ID_LEN + 1, "%s", s);
    }
    cJSON_Delete(root);
    return found;
}

static void init_result(probe_result_t *out)
{
    memset(out, 0, sizeof(*out));
    out->id = -1;
}

static void parse_record(const cJSON *rec, probe_result_t *out)
{
    const cJSON *id = cJSON_GetObjectItem(rec, "id");
    if (cJSON_IsNumber(id)) {
        out->id = (int64_t)cJSON_GetNumberValue(id);
    }
    copy_str(out->connection_type, sizeof(out->connection_type),
             cJSON_GetObjectItem(rec, "connection_type"));
}

bool probe_proto_parse_response(const char *json, size_t len, probe_result_t *out)
{
    init_result(out);
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!root) {
        return false;
    }
    copy_str(out->request_id, sizeof(out->request_id), cJSON_GetObjectItem(root, "request_id"));
    const cJSON *result = cJSON_GetObjectItem(root, "result");
    const char *status = cJSON_GetStringValue(cJSON_GetObjectItem(result, "status"));
    out->ok = status && strcmp(status, "ok") == 0;
    if (out->ok) {
        const cJSON *data = cJSON_GetObjectItem(result, "data");
        copy_str(out->kind, sizeof(out->kind), cJSON_GetObjectItem(data, "kind"));
        if (strcmp(out->kind, "record") == 0) {
            parse_record(cJSON_GetObjectItem(data, "record"), out);
        } else if (strcmp(out->kind, "records") == 0) {
            out->count = cJSON_GetArraySize(cJSON_GetObjectItem(data, "records"));
        } else if (strcmp(out->kind, "deleted") == 0) {
            const cJSON *id = cJSON_GetObjectItem(data, "id");
            out->id = cJSON_IsNumber(id) ? (int64_t)cJSON_GetNumberValue(id) : -1;
        }
    } else {
        const cJSON *err = cJSON_GetObjectItem(result, "error");
        copy_str(out->error_code, sizeof(out->error_code), cJSON_GetObjectItem(err, "code"));
        copy_str(out->error_message, sizeof(out->error_message), cJSON_GetObjectItem(err, "message"));
    }
    cJSON_Delete(root);
    return status != NULL;
}

bool probe_proto_parse_rest(const char *json, size_t len, probe_result_t *out)
{
    init_result(out);
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!root) {
        return false;
    }
    if (cJSON_IsArray(root)) {
        out->ok = true;
        snprintf(out->kind, sizeof(out->kind), "records");
        out->count = cJSON_GetArraySize(root);
        const cJSON *first = cJSON_GetArrayItem(root, 0);
        if (first) {
            parse_record(first, out);   /* newest first */
        }
    } else if (cJSON_GetObjectItem(root, "code")) {
        copy_str(out->error_code, sizeof(out->error_code), cJSON_GetObjectItem(root, "code"));
        copy_str(out->error_message, sizeof(out->error_message), cJSON_GetObjectItem(root, "message"));
    } else if (cJSON_GetObjectItem(root, "id")) {
        out->ok = true;
        snprintf(out->kind, sizeof(out->kind), "record");
        parse_record(root, out);
    } else {
        out->ok = true;
    }
    cJSON_Delete(root);
    return true;
}
