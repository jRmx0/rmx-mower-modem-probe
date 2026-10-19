/*
 * PPP smoke test for the LilyGo T-SIM7600 (ESP32 + SIM7600G-H).
 *
 * 1. powers the modem with the board's pin sequence (or finds it running),
 * 2. starts PPP with esp_modem (the modem is only a data pipe; TLS runs in
 *    mbedTLS on the ESP32),
 * 3. HTTPS GET https://api.rmx.lt/probe/health (cert bundle verification),
 * 4. MQTT over TLS to mqtts://mqtt.rmx.lt:8883 with a deliberately invalid
 *    login: a "not authorized" CONNACK proves TLS + MQTT reach Mosquitto
 *    without needing a real password.
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_ppp.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_modem_api.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "mqtt_client.h"
#include "nvs_flash.h"

#define PIN_MODEM_TX   27
#define PIN_MODEM_RX   26
#define PIN_PWRKEY     4
#define PIN_FLIGHT     25
#define PIN_DTR        32
#define PIN_STATUS     34
#define APN            "internet"   /* Tele2 LT / Pildyk */

static const char *TAG = "ppp_smoke";
static EventGroupHandle_t ev;
#define GOT_IP    BIT0
#define LOST_IP   BIT1
#define MQTT_DONE BIT2

static void on_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == IP_EVENT_PPP_GOT_IP) {
        ip_event_got_ip_t *e = data;
        esp_netif_dns_info_t dns;
        esp_netif_get_dns_info(e->esp_netif, ESP_NETIF_DNS_MAIN, &dns);
        ESP_LOGI(TAG, "PPP up: ip " IPSTR " gw " IPSTR " dns " IPSTR,
                 IP2STR(&e->ip_info.ip), IP2STR(&e->ip_info.gw), IP2STR(&dns.ip.u_addr.ip4));
        xEventGroupSetBits(ev, GOT_IP);
    } else if (id == IP_EVENT_PPP_LOST_IP) {
        ESP_LOGW(TAG, "PPP lost IP");
        xEventGroupSetBits(ev, LOST_IP);
    }
}

static void board_pins_init(void)
{
    gpio_config_t out = {
        .pin_bit_mask = (1ULL << PIN_PWRKEY) | (1ULL << PIN_FLIGHT) | (1ULL << PIN_DTR),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&out);
    gpio_set_level(PIN_PWRKEY, 0);
    gpio_set_level(PIN_FLIGHT, 1);   /* radio on */
    gpio_set_level(PIN_DTR, 0);      /* awake */
    gpio_config_t in = { .pin_bit_mask = 1ULL << PIN_STATUS, .mode = GPIO_MODE_INPUT };
    gpio_config(&in);
}

static void pwrkey_pulse(void)
{
    gpio_set_level(PIN_PWRKEY, 1);
    vTaskDelay(pdMS_TO_TICKS(500));
    gpio_set_level(PIN_PWRKEY, 0);
}

static void https_health(void)
{
    char body[256] = {0};
    esp_http_client_config_t cfg = {
        .url = "https://api.rmx.lt/probe/health",
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 20000,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    int64_t t0 = esp_timer_get_time();
    esp_err_t err = esp_http_client_open(c, 0);
    if (err == ESP_OK) {
        esp_http_client_fetch_headers(c);
        int n = esp_http_client_read_response(c, body, sizeof(body) - 1);
        ESP_LOGI(TAG, "HTTPS GET /probe/health -> %d in %lld ms: %.*s",
                 esp_http_client_get_status_code(c),
                 (esp_timer_get_time() - t0) / 1000, n > 0 ? n : 0, body);
    } else {
        ESP_LOGE(TAG, "HTTPS failed: %s", esp_err_to_name(err));
    }
    esp_http_client_cleanup(c);
}

static void mqtt_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_mqtt_event_handle_t e = data;
    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT connected (unexpected with an invalid login)");
        xEventGroupSetBits(ev, MQTT_DONE);
        break;
    case MQTT_EVENT_ERROR:
        if (e->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
            ESP_LOGI(TAG, "MQTT TLS OK, broker refused login as expected (connack code %d)",
                     e->error_handle->connect_return_code);
        } else {
            ESP_LOGE(TAG, "MQTT error type %d, tls esp_err 0x%x, mbedtls -0x%x, sock errno %d",
                     e->error_handle->error_type, e->error_handle->esp_tls_last_esp_err,
                     -e->error_handle->esp_tls_stack_err, e->error_handle->esp_transport_sock_errno);
        }
        xEventGroupSetBits(ev, MQTT_DONE);
        break;
    default:
        break;
    }
}

static void mqtts_probe(void)
{
    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = "mqtts://mqtt.rmx.lt:8883",
        .broker.verification.crt_bundle_attach = esp_crt_bundle_attach,
        .credentials.username = "kb-smoke-test",
        .credentials.authentication.password = "not-a-real-password",
        .network.disable_auto_reconnect = true,
    };
    esp_mqtt_client_handle_t c = esp_mqtt_client_init(&cfg);
    esp_mqtt_client_register_event(c, ESP_EVENT_ANY_ID, mqtt_event, NULL);
    int64_t t0 = esp_timer_get_time();
    esp_mqtt_client_start(c);
    xEventGroupWaitBits(ev, MQTT_DONE, pdTRUE, pdFALSE, pdMS_TO_TICKS(60000));
    ESP_LOGI(TAG, "MQTT attempt took %lld ms", (esp_timer_get_time() - t0) / 1000);
    esp_mqtt_client_stop(c);
    esp_mqtt_client_destroy(c);
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ev = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, on_ip, NULL));

    board_pins_init();

    esp_modem_dte_config_t dte = ESP_MODEM_DTE_DEFAULT_CONFIG();
    /* The defaults (tx 25, rx 26, rts 27, cts 23) are WRONG for this board:
     * 25 is FLIGHT and 27 is our TX. No RTS/CTS is wired. */
    dte.uart_config.tx_io_num = PIN_MODEM_TX;
    dte.uart_config.rx_io_num = PIN_MODEM_RX;
    dte.uart_config.rts_io_num = -1;
    dte.uart_config.cts_io_num = -1;
    dte.uart_config.flow_control = ESP_MODEM_FLOW_CONTROL_NONE;
    dte.uart_config.rx_buffer_size = 8192;
    dte.uart_config.tx_buffer_size = 2048;
    dte.dte_buffer_size = 4096;
    esp_modem_dce_config_t dce_cfg = ESP_MODEM_DCE_DEFAULT_CONFIG(APN);
    esp_netif_config_t ppp_cfg = ESP_NETIF_DEFAULT_PPP();
    esp_netif_t *ppp = esp_netif_new(&ppp_cfg);
    esp_modem_dce_t *dce = esp_modem_new_dev(ESP_MODEM_DCE_SIM7600, &dte, &dce_cfg, ppp);
    assert(dce);

    /* The modem may still be in PPP from a previous ESP boot (an ESP reset
     * does not reset the modem): detect and return to command mode. */
    if (esp_modem_set_mode(dce, ESP_MODEM_MODE_DETECT) == ESP_OK &&
            esp_modem_get_mode(dce) == ESP_MODEM_MODE_DATA) {
        ESP_LOGI(TAG, "modem was in data mode, switching to command mode");
        esp_modem_set_mode(dce, ESP_MODEM_MODE_COMMAND);
    }
    if (esp_modem_sync(dce) != ESP_OK) {
        ESP_LOGI(TAG, "no AT answer, pulsing PWRKEY (STATUS=%d)", gpio_get_level(PIN_STATUS));
        pwrkey_pulse();
        for (int i = 0; i < 30 && esp_modem_sync(dce) != ESP_OK; i++) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
    ESP_ERROR_CHECK(esp_modem_sync(dce));

    char name[ESP_MODEM_C_API_STR_BUF_SIZE] = {0};
    esp_modem_get_module_name(dce, name);
    int rssi = 99, ber = 99;
    for (int i = 0; i < 60; i++) {
        esp_modem_get_signal_quality(dce, &rssi, &ber);
        if (rssi != 99) break;
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    ESP_LOGI(TAG, "module %s, rssi %d, ber %d", name, rssi, ber);

    int64_t t0 = esp_timer_get_time();
    ESP_ERROR_CHECK(esp_modem_set_mode(dce, ESP_MODEM_MODE_DATA));
    EventBits_t b = xEventGroupWaitBits(ev, GOT_IP, pdFALSE, pdFALSE, pdMS_TO_TICKS(60000));
    if (!(b & GOT_IP)) {
        ESP_LOGE(TAG, "no IP within 60 s");
        esp_modem_set_mode(dce, ESP_MODEM_MODE_COMMAND);
        return;
    }
    ESP_LOGI(TAG, "PPP connected in %lld ms", (esp_timer_get_time() - t0) / 1000);

    https_health();
    mqtts_probe();
    ESP_LOGI(TAG, "free heap %u, min %u", (unsigned)esp_get_free_heap_size(),
             (unsigned)esp_get_minimum_free_heap_size());

    /* Leave PPP cleanly so the next boot finds the modem in command mode. */
    esp_modem_set_mode(dce, ESP_MODEM_MODE_COMMAND);
    ESP_LOGI(TAG, "SMOKE TEST DONE");
}
