/*
 * rmx-mower-modem-probe: LilyGo T-SIM7600 (ESP32 + SIM7600G-H) talking to
 * rmx-backend-probe over LTE.
 *
 * board -> cell (PPP) -> HTTPS (health, login, create, list) -> MQTT
 * (connect, create + list over probe/req|res/<user>) -> stay online and run
 * remote commands from mower/<user>/cmd (see remote_cmd.h).
 */
#include <stdio.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_app_desc.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "board.h"
#include "cell.h"
#include "gnss.h"
#include "https_api.h"
#include "mqtt_link.h"
#include "probe_proto.h"
#include "remote_cmd.h"
#include "sdkconfig.h"

static const char *TAG = "probe";

static void log_result(const char *what, const probe_result_t *r)
{
    if (!r->ok) {
        ESP_LOGE(TAG, "%s: error %s: %s", what, r->error_code, r->error_message);
    } else if (r->kind[0] && r->id >= 0) {
        ESP_LOGI(TAG, "%s: ok, %s id %lld (connection_type %s)", what, r->kind, r->id,
                 r->connection_type[0] ? r->connection_type : "-");
    } else {
        ESP_LOGI(TAG, "%s: ok, %s count %d", what, r->kind, r->count);
    }
}

static int self_test_https(void)
{
    int failures = 0;
    probe_result_t r;
    char value[64];

    if (https_api_health() != ESP_OK) {
        ESP_LOGE(TAG, "TEST https health: FAIL (probe off on the VPS -> 502)");
        return 1;
    }
    ESP_LOGI(TAG, "TEST https health: PASS");

    if (https_api_login() != ESP_OK) {
        ESP_LOGE(TAG, "TEST https login: FAIL");
        return 1;
    }
    ESP_LOGI(TAG, "TEST https login: PASS");

    snprintf(value, sizeof(value), "https from T-SIM7600, uptime %lld s, rssi %d",
             esp_timer_get_time() / 1000000, cell_last_rssi());
    if (https_api_create(value, &r) == ESP_OK && r.ok && r.id >= 0) {
        log_result("https create", &r);
        ESP_LOGI(TAG, "TEST https create: PASS");
    } else {
        ESP_LOGE(TAG, "TEST https create: FAIL");
        failures++;
    }

    if (https_api_list(&r) == ESP_OK && r.ok) {
        ESP_LOGI(TAG, "https list: %d records, newest id %lld (%s)", r.count, r.id,
                 r.connection_type[0] ? r.connection_type : "-");
        ESP_LOGI(TAG, "TEST https list: PASS");
    } else {
        ESP_LOGE(TAG, "TEST https list: FAIL");
        failures++;
    }
    return failures;
}

static int self_test_mqtt(void)
{
    int failures = 0;
    probe_result_t r;
    char value[64];

    if (mqtt_link_start() != ESP_OK || !mqtt_link_wait_ready(90000)) {
        ESP_LOGE(TAG, "TEST mqtt connect: FAIL");
        return 1;
    }
    ESP_LOGI(TAG, "TEST mqtt connect: PASS");

    snprintf(value, sizeof(value), "mqtt from T-SIM7600, uptime %lld s, rssi %d",
             esp_timer_get_time() / 1000000, cell_last_rssi());
    if (mqtt_link_request(PROBE_OP_CREATE, 0, value, &r, 30000) == ESP_OK && r.ok) {
        log_result("mqtt create", &r);
        ESP_LOGI(TAG, "TEST mqtt create: PASS");
    } else {
        ESP_LOGE(TAG, "TEST mqtt create: FAIL");
        failures++;
    }

    if (mqtt_link_request(PROBE_OP_LIST, 0, NULL, &r, 60000) == ESP_OK && r.ok) {
        log_result("mqtt list", &r);
        ESP_LOGI(TAG, "TEST mqtt list: PASS");
    } else {
        ESP_LOGE(TAG, "TEST mqtt list: FAIL");
        failures++;
    }
    return failures;
}

void app_main(void)
{
    const esp_app_desc_t *app = esp_app_get_description();
    ESP_LOGI(TAG, "%s %s (IDF %s)", app->project_name, app->version, app->idf_ver);

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    board_init();
    if (cell_start(60000) != ESP_OK && !cell_wait_ip(180000)) {
        ESP_LOGE(TAG, "no data connection, restarting in 30 s");
        vTaskDelay(pdMS_TO_TICKS(30000));
        cell_stop();
        esp_restart();
    }

    /* Clock for command expiry (expires_at) and TLS logs; not fatal if it fails. */
    esp_sntp_config_t sntp = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_netif_sntp_init(&sntp);
    if (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(15000)) == ESP_OK) {
        ESP_LOGI(TAG, "clock set by SNTP: %lld", (long long)time(NULL));
    } else {
        ESP_LOGW(TAG, "SNTP: no time yet (commands with expires_at are refused until it syncs)");
    }

    gnss_start();   /* the first fix takes a while: start searching early */

    int failures = self_test_https();
    https_api_close();   /* frees the TLS session (~40 KB); MQTT keeps its own */
    ESP_ERROR_CHECK(remote_cmd_init());
    failures += self_test_mqtt();
    ESP_LOGI(TAG, "SELF-TEST %s (%d failure%s), free heap %u, min %u, internal %u",
             failures ? "FAILED" : "PASSED", failures, failures == 1 ? "" : "s",
             (unsigned)esp_get_free_heap_size(), (unsigned)esp_get_minimum_free_heap_size(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    /* Stay online: esp-mqtt reconnects by itself, cell redials on PPP loss. */
    int64_t last_beat = esp_timer_get_time();
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(60000));
        ESP_LOGI(TAG, "alive: ppp %s, rssi %d, free heap %u", cell_is_up() ? "up" : "down",
                 cell_last_rssi(), (unsigned)esp_get_free_heap_size());
        if (CONFIG_PROBE_HEARTBEAT_S > 0 &&
                esp_timer_get_time() - last_beat >= (int64_t)CONFIG_PROBE_HEARTBEAT_S * 1000000) {
            last_beat = esp_timer_get_time();
            probe_result_t r;
            char value[64];
            snprintf(value, sizeof(value), "heartbeat, uptime %lld s", last_beat / 1000000);
            if (mqtt_link_request(PROBE_OP_CREATE, 0, value, &r, 30000) == ESP_OK) {
                log_result("heartbeat", &r);
            }
        }
    }
}
