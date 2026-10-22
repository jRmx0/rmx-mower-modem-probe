#include "ota.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "cell.h"
#include "mqtt_link.h"
#include "sdkconfig.h"

static const char *TAG = "ota";

#define ATTEMPTS        4       /* one download, resumed up to 3 times */
#define ID_LEN          64
#define URL_LEN         256
#define VERSION_LEN     32      /* esp_app_desc_t.version */

typedef struct {
    char id[ID_LEN + 1];
    char url[URL_LEN + 1];
    char version[VERSION_LEN];
    int size;                   /* expected image size, 0 = don't check */
} ota_req_t;

static ota_req_t s_req;
static volatile bool s_busy;
static esp_timer_handle_t s_confirm_timer;
static bool s_pending_verify;

/* Last attempt since boot, for the status command */
static const char *volatile s_state = "idle";   /* idle, downloading, done, failed */
static char s_target[VERSION_LEN];
static volatile int s_progress = -1;            /* percent, -1 = unknown */
static char s_error[32];

static bool clock_valid(void)
{
    return time(NULL) > 1700000000;   /* set by SNTP */
}

/* Acks on mower/<user>/cmd/ack in the same shape as remote_cmd.c. */
static void send_ack(const char *state, const char *error, const char *message, int64_t t0)
{
    cJSON *reply = cJSON_CreateObject();
    cJSON_AddStringToObject(reply, "id", s_req.id);
    cJSON_AddStringToObject(reply, "cmd", "ota");
    if (error) {
        cJSON_AddBoolToObject(reply, "ok", false);
        cJSON_AddStringToObject(reply, "error", error);
        cJSON_AddStringToObject(reply, "message", message);
    } else {
        cJSON_AddBoolToObject(reply, "ok", true);
        cJSON *res = cJSON_AddObjectToObject(reply, "result");
        cJSON_AddStringToObject(res, "state", state);
        cJSON_AddStringToObject(res, "version", s_req.version);
        cJSON_AddStringToObject(res, "from", esp_app_get_description()->version);
    }
    cJSON_AddNumberToObject(reply, "ms", (double)((esp_timer_get_time() - t0) / 1000));
    if (clock_valid()) {
        cJSON_AddNumberToObject(reply, "time", (double)time(NULL));
    }
    char *json = cJSON_PrintUnformatted(reply);
    if (json) {
        mqtt_link_send_ack(json);
        free(json);
    }
    cJSON_Delete(reply);
}

static void fail(const char *error, const char *message, int64_t t0)
{
    ESP_LOGE(TAG, "update to %s failed: %s (%s)", s_req.version, error, message);
    snprintf(s_error, sizeof(s_error), "%s", error);
    s_state = "failed";
    send_ack(NULL, error, message, t0);
}

/* The header of the new image: same project, the version that was asked for,
 * not the one already running. */
static bool check_header(esp_https_ota_handle_t h, int64_t t0)
{
    esp_app_desc_t desc;
    const esp_app_desc_t *running = esp_app_get_description();
    if (esp_https_ota_get_img_desc(h, &desc) != ESP_OK) {
        fail("bad_image", "could not read the image header", t0);
        return false;
    }
    ESP_LOGI(TAG, "image: %s %s (IDF %s)", desc.project_name, desc.version, desc.idf_ver);
    if (strncmp(desc.project_name, running->project_name, sizeof(desc.project_name)) != 0) {
        fail("bad_image", "the image is for another project", t0);
        return false;
    }
    if (strncmp(desc.version, s_req.version, sizeof(desc.version)) != 0) {
        fail("bad_image", "the image has another version than the command", t0);
        return false;
    }
    if (strncmp(desc.version, running->version, sizeof(desc.version)) == 0) {
        fail("same_version", "this version is already running", t0);
        return false;
    }
    int size = esp_https_ota_get_image_size(h);
    if (s_req.size > 0 && size != s_req.size) {
        ESP_LOGE(TAG, "image size %d, expected %d", size, s_req.size);
        fail("bad_size", "the image size differs from the command", t0);
        return false;
    }
    return true;
}

static void restart_into_new_image(void)
{
    ESP_LOGW(TAG, "restarting into %s in 3 s", s_req.version);
    vTaskDelay(pdMS_TO_TICKS(3000));   /* let the ack go out */
    mqtt_link_stop();
    cell_stop();
    esp_restart();
}

static void ota_task(void *arg)
{
    int64_t t0 = esp_timer_get_time();
    esp_http_client_config_t http = {
        .url = s_req.url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 30000,
        .keep_alive_enable = true,
        .buffer_size = 4096,
        .buffer_size_tx = 1024,
    };
    if (CONFIG_PROBE_OTA_PASSWORD[0]) {
        http.username = CONFIG_PROBE_OTA_USERNAME;
        http.password = CONFIG_PROBE_OTA_PASSWORD;
        http.auth_type = HTTP_AUTH_TYPE_BASIC;
    }

    int written = 0;   /* bytes already in the update slot, kept across retries */
    int size = s_req.size;
    for (int attempt = 0; attempt < ATTEMPTS; attempt++) {
        if (attempt > 0) {
            ESP_LOGW(TAG, "retry %d/%d in %d s, resuming at %d bytes", attempt, ATTEMPTS - 1,
                     10 * attempt, written);
            vTaskDelay(pdMS_TO_TICKS(10000 * attempt));
            if (!cell_wait_ip(120000)) {
                continue;
            }
        }
        esp_https_ota_config_t cfg = {
            .http_config = &http,
            .ota_resumption = written > 0,     /* Range: bytes=<written>- */
            .ota_image_bytes_written = written,
        };
        esp_https_ota_handle_t h = NULL;
        if (esp_https_ota_begin(&cfg, &h) != ESP_OK) {
            ESP_LOGE(TAG, "could not start the download from %s", s_req.url);
            continue;
        }
        if (written == 0 && !check_header(h, t0)) {
            esp_https_ota_abort(h);
            s_busy = false;
            vTaskDelete(NULL);
        }
        if (size <= 0) {
            size = esp_https_ota_get_image_size(h);   /* the full image on the first request */
        }
        int last_logged = -1;
        esp_err_t err;
        while ((err = esp_https_ota_perform(h)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            written = esp_https_ota_get_image_len_read(h);
            if (size > 0) {
                s_progress = (int)((int64_t)written * 100 / size);
                if (s_progress / 10 != last_logged) {
                    last_logged = s_progress / 10;
                    ESP_LOGI(TAG, "%d %% (%d of %d bytes)", s_progress, written, size);
                }
            }
        }
        written = esp_https_ota_get_image_len_read(h);
        if (err != ESP_OK || !esp_https_ota_is_complete_data_received(h)) {
            ESP_LOGE(TAG, "download stopped at %d bytes: %s", written, esp_err_to_name(err));
            esp_https_ota_abort(h);
            continue;
        }
        /* Checks the image hash and (with signed apps) the signature, then
         * makes the update slot the boot slot. */
        err = esp_https_ota_finish(h);
        if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
            fail("bad_image", "image check failed (signature or hash, see the device log)", t0);
        } else if (err != ESP_OK) {
            fail("failed", esp_err_to_name(err), t0);
        } else {
            ESP_LOGI(TAG, "update to %s written in %lld s", s_req.version,
                     (esp_timer_get_time() - t0) / 1000000);
            s_progress = 100;
            s_state = "done";
            send_ack("done", NULL, NULL, t0);
            restart_into_new_image();
        }
        s_busy = false;
        vTaskDelete(NULL);
    }
    fail("download_failed", "download failed, the running firmware is unchanged", t0);
    s_busy = false;
    vTaskDelete(NULL);
}

bool ota_start(const char *cmd_id, const char *url, const char *version, int size,
               const char **error, const char **message)
{
    *error = "bad_request";
    if (!url || strlen(url) > URL_LEN) {
        *message = "ota needs a string url (up to 256 chars)";
        return false;
    }
    if (strncmp(url, CONFIG_PROBE_OTA_URL_PREFIX, strlen(CONFIG_PROBE_OTA_URL_PREFIX)) != 0) {
        *message = "url must start with " CONFIG_PROBE_OTA_URL_PREFIX;
        return false;
    }
    if (!version || !version[0] || strlen(version) >= VERSION_LEN) {
        *message = "ota needs a string version (1-31 chars)";
        return false;
    }
    if (size < 0) {
        *message = "size must be the image size in bytes";
        return false;
    }
    if (strcmp(version, esp_app_get_description()->version) == 0) {
        *error = "same_version";
        *message = "this version is already running";
        return false;
    }
    if (s_pending_verify) {
        *error = "busy";
        *message = "the running image is not confirmed yet";
        return false;
    }
    if (s_busy) {
        *error = "busy";
        *message = "an update is already running";
        return false;
    }
    int rssi = cell_last_rssi();
    if (rssi != 99 && rssi < CONFIG_PROBE_OTA_MIN_CSQ) {
        *error = "low_signal";
        *message = "signal too weak for an update (CSQ below CONFIG_PROBE_OTA_MIN_CSQ)";
        return false;
    }

    s_busy = true;
    snprintf(s_req.id, sizeof(s_req.id), "%s", cmd_id);
    snprintf(s_req.url, sizeof(s_req.url), "%s", url);
    snprintf(s_req.version, sizeof(s_req.version), "%s", version);
    s_req.size = size;
    snprintf(s_target, sizeof(s_target), "%s", version);
    s_error[0] = '\0';
    s_progress = 0;
    s_state = "downloading";
    if (xTaskCreate(ota_task, "ota", 8192, NULL, 3, NULL) != pdPASS) {
        s_busy = false;
        s_state = "failed";
        *error = "failed";
        *message = "no memory for the update task";
        return false;
    }
    ESP_LOGI(TAG, "update %s -> %s from %s", esp_app_get_description()->version, version, url);
    return true;
}

bool ota_busy(void)
{
    return s_busy;
}

static void confirm_timeout(void *arg)
{
    ESP_LOGE(TAG, "new image not confirmed within %d s (no MQTT), rolling back",
             CONFIG_PROBE_OTA_CONFIRM_S);
    esp_ota_mark_app_invalid_rollback_and_reboot();
}

void ota_boot_init(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) != ESP_OK) {
        state = ESP_OTA_IMG_UNDEFINED;   /* flashed over USB */
    }
    ESP_LOGI(TAG, "running from %s at 0x%lx, state %d", running->label,
             (unsigned long)running->address, (int)state);

    const esp_partition_t *invalid = esp_ota_get_last_invalid_partition();
    esp_app_desc_t desc;
    if (invalid && esp_ota_get_partition_description(invalid, &desc) == ESP_OK) {
        ESP_LOGW(TAG, "rolled back from %s (%s)", desc.version, invalid->label);
    }

    if (state == ESP_OTA_IMG_PENDING_VERIFY) {
        s_pending_verify = true;
        const esp_timer_create_args_t args = { .callback = confirm_timeout, .name = "ota_confirm" };
        if (esp_timer_create(&args, &s_confirm_timer) == ESP_OK) {
            esp_timer_start_once(s_confirm_timer, (uint64_t)CONFIG_PROBE_OTA_CONFIRM_S * 1000000);
        }
        ESP_LOGW(TAG, "new image: confirm within %d s or roll back", CONFIG_PROBE_OTA_CONFIRM_S);
    }
}

void ota_confirm(void)
{
    if (!s_pending_verify) {
        return;
    }
    if (s_confirm_timer) {
        esp_timer_stop(s_confirm_timer);
    }
    if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
        s_pending_verify = false;
        ESP_LOGI(TAG, "image %s confirmed", esp_app_get_description()->version);
    }
}

void ota_add_status(cJSON *res)
{
    cJSON_AddStringToObject(res, "version", esp_app_get_description()->version);
    cJSON_AddStringToObject(res, "partition", esp_ota_get_running_partition()->label);
    cJSON_AddBoolToObject(res, "confirmed", !s_pending_verify);
    const esp_partition_t *invalid = esp_ota_get_last_invalid_partition();
    esp_app_desc_t desc;
    if (invalid && esp_ota_get_partition_description(invalid, &desc) == ESP_OK) {
        cJSON_AddStringToObject(res, "rolled_back_from", desc.version);
    }
    cJSON *ota = cJSON_AddObjectToObject(res, "ota");
    cJSON_AddStringToObject(ota, "state", s_state);
    if (s_target[0]) {
        cJSON_AddStringToObject(ota, "version", s_target);
    }
    if (s_progress >= 0) {
        cJSON_AddNumberToObject(ota, "progress", s_progress);
    }
    if (s_error[0]) {
        cJSON_AddStringToObject(ota, "error", s_error);
    }
}
