#include "remote_cmd.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "cell.h"
#include "https_api.h"
#include "mqtt_link.h"
#include "probe_proto.h"

static const char *TAG = "cmd";

#define MAX_CMD     2048
#define SEEN_IDS    8
#define ID_LEN      64

static QueueHandle_t s_queue;

/* QoS 1 may deliver a command twice: remember the last few ids */
static char s_seen[SEEN_IDS][ID_LEN + 1];
static int s_seen_next;

typedef struct {
    char *buf;
    size_t len;
} cmd_msg_t;

/* Runs on the MQTT task: copy and hand over, never block. */
static void on_cmd(const char *data, size_t len)
{
    if (len > MAX_CMD) {
        ESP_LOGW(TAG, "command too large (%u bytes), dropped", (unsigned)len);
        return;
    }
    cmd_msg_t m = { .buf = malloc(len + 1), .len = len };
    if (!m.buf) {
        return;
    }
    memcpy(m.buf, data, len);
    m.buf[len] = '\0';
    if (xQueueSend(s_queue, &m, 0) != pdTRUE) {
        ESP_LOGW(TAG, "queue full, command dropped");
        free(m.buf);
    }
}

static bool clock_valid(void)
{
    return time(NULL) > 1700000000;   /* set by SNTP */
}

static bool seen_before(const char *id)
{
    for (int i = 0; i < SEEN_IDS; i++) {
        if (strcmp(s_seen[i], id) == 0) {
            return true;
        }
    }
    snprintf(s_seen[s_seen_next], ID_LEN + 1, "%s", id);
    s_seen_next = (s_seen_next + 1) % SEEN_IDS;
    return false;
}

static void send_reply(cJSON *reply, int64_t t0)
{
    cJSON_AddNumberToObject(reply, "ms", (double)((esp_timer_get_time() - t0) / 1000));
    if (clock_valid()) {
        cJSON_AddNumberToObject(reply, "time", (double)time(NULL));
    }
    char *json = cJSON_PrintUnformatted(reply);
    if (json) {
        mqtt_link_send_ack(json);
        free(json);
    }
}

static void set_error(cJSON *reply, const char *code, const char *message)
{
    cJSON_AddBoolToObject(reply, "ok", false);
    cJSON_AddStringToObject(reply, "error", code);
    cJSON_AddStringToObject(reply, "message", message);
}

static void add_record_result(cJSON *reply, const probe_result_t *r)
{
    cJSON_AddBoolToObject(reply, "ok", true);
    cJSON *res = cJSON_AddObjectToObject(reply, "result");
    cJSON_AddStringToObject(res, "kind", r->kind);
    if (r->id >= 0) {
        cJSON_AddNumberToObject(res, "record_id", (double)r->id);
    }
    if (r->connection_type[0]) {
        cJSON_AddStringToObject(res, "connection_type", r->connection_type);
    }
    if (strcmp(r->kind, "records") == 0) {
        cJSON_AddNumberToObject(res, "count", r->count);
    }
}

/* An MQTT request to the probe server, answered with its result. */
static void run_probe_request(cJSON *reply, probe_op_t op, int64_t id, const char *value)
{
    probe_result_t r;
    if (mqtt_link_request(op, id, value, &r, 30000) != ESP_OK) {
        set_error(reply, "timeout", "no answer from the probe server");
    } else if (!r.ok) {
        set_error(reply, r.error_code, r.error_message);
    } else {
        add_record_result(reply, &r);
    }
}

/* HTTPS call; logs in again once if it fails (expired session, closed socket). */
static void run_https(cJSON *reply, bool create, const char *value)
{
    probe_result_t r;
    for (int attempt = 0; attempt < 2; attempt++) {
        if (attempt == 1 && https_api_login() != ESP_OK) {
            break;
        }
        esp_err_t err = create ? https_api_create(value, &r) : https_api_list(&r);
        if (err == ESP_OK && r.ok) {
            add_record_result(reply, &r);
            return;
        }
    }
    set_error(reply, "failed", "HTTPS request failed (see the device log)");
}

static void add_status(cJSON *reply)
{
    cJSON_AddBoolToObject(reply, "ok", true);
    cJSON *res = cJSON_AddObjectToObject(reply, "result");
    cJSON_AddNumberToObject(res, "uptime_s", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddBoolToObject(res, "ppp", cell_is_up());
    cJSON_AddNumberToObject(res, "rssi", cell_last_rssi());
    cJSON_AddNumberToObject(res, "free_heap", esp_get_free_heap_size());
    cJSON_AddNumberToObject(res, "free_internal", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(res, "min_free_heap", esp_get_minimum_free_heap_size());
    cJSON_AddBoolToObject(res, "clock", clock_valid());
}

static void handle(const char *json, size_t len)
{
    int64_t t0 = esp_timer_get_time();
    ESP_LOGI(TAG, "<- %.*s", len > 300 ? 300 : (int)len, json);
    cJSON *cmd = cJSON_ParseWithLength(json, len);
    const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(cmd, "id"));
    const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(cmd, "cmd"));
    const cJSON *value_item = cJSON_GetObjectItem(cmd, "value");
    const char *value = cJSON_GetStringValue(value_item);   /* NULL for null/absent */
    const cJSON *record_id = cJSON_GetObjectItem(cmd, "record_id");
    const cJSON *expires_at = cJSON_GetObjectItem(cmd, "expires_at");

    cJSON *reply = cJSON_CreateObject();
    cJSON_AddStringToObject(reply, "id", id ? id : "");
    cJSON_AddStringToObject(reply, "cmd", name ? name : "");
    bool restart = false;

    if (!cmd || !id || !id[0] || strlen(id) > ID_LEN || !name) {
        set_error(reply, "bad_request", "need a JSON object with string \"id\" (1-64 chars) and \"cmd\"");
    } else if (seen_before(id)) {
        ESP_LOGW(TAG, "duplicate command id %s ignored", id);
        cJSON_Delete(reply);
        cJSON_Delete(cmd);
        return;
    } else if (expires_at && !cJSON_IsNumber(expires_at)) {
        set_error(reply, "bad_request", "expires_at must be a unix time in seconds");
    } else if (expires_at && !clock_valid()) {
        set_error(reply, "no_clock", "the device clock is not set, cannot check expires_at");
    } else if (expires_at && (double)time(NULL) > cJSON_GetNumberValue(expires_at)) {
        set_error(reply, "expired", "command expired before it arrived");
    } else if (value_item && !cJSON_IsString(value_item) && !cJSON_IsNull(value_item)) {
        set_error(reply, "bad_request", "value must be a string or null");
    } else if (strcmp(name, "ping") == 0) {
        cJSON_AddBoolToObject(reply, "ok", true);
        cJSON_AddStringToObject(cJSON_AddObjectToObject(reply, "result"), "pong", "probe-device");
    } else if (strcmp(name, "status") == 0) {
        add_status(reply);
    } else if (strcmp(name, "create") == 0) {
        run_probe_request(reply, PROBE_OP_CREATE, 0, value);
    } else if (strcmp(name, "list") == 0) {
        run_probe_request(reply, PROBE_OP_LIST, 0, NULL);
    } else if (strcmp(name, "delete") == 0) {
        if (!cJSON_IsNumber(record_id)) {
            set_error(reply, "bad_request", "delete needs a numeric record_id");
        } else {
            run_probe_request(reply, PROBE_OP_DELETE, (int64_t)cJSON_GetNumberValue(record_id), NULL);
        }
    } else if (strcmp(name, "https_create") == 0) {
        run_https(reply, true, value);
    } else if (strcmp(name, "https_list") == 0) {
        run_https(reply, false, NULL);
    } else if (strcmp(name, "restart") == 0) {
        cJSON_AddBoolToObject(reply, "ok", true);
        restart = true;
    } else {
        set_error(reply, "unknown_cmd",
                  "known: ping status create list delete https_create https_list restart");
    }

    send_reply(reply, t0);
    cJSON_Delete(reply);
    cJSON_Delete(cmd);

    if (restart) {
        ESP_LOGW(TAG, "restart requested, restarting in 3 s");
        vTaskDelay(pdMS_TO_TICKS(3000));   /* let the ack go out */
        mqtt_link_stop();
        cell_stop();
        esp_restart();
    }
}

static void cmd_task(void *arg)
{
    cmd_msg_t m;
    for (;;) {
        if (xQueueReceive(s_queue, &m, portMAX_DELAY) == pdTRUE) {
            handle(m.buf, m.len);
            free(m.buf);
        }
    }
}

esp_err_t remote_cmd_init(void)
{
    s_queue = xQueueCreate(4, sizeof(cmd_msg_t));
    if (!s_queue || xTaskCreate(cmd_task, "remote_cmd", 6144, NULL, 4, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    mqtt_link_set_cmd_handler(on_cmd);
    return ESP_OK;
}
