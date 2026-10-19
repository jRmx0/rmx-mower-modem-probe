#include "cell.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_ppp.h"
#include "esp_timer.h"
#include "esp_modem_api.h"
#include "board.h"
#include "sdkconfig.h"

static const char *TAG = "cell";

#define GOT_IP   BIT0
#define LOST_IP  BIT1

static EventGroupHandle_t s_ev;
static esp_modem_dce_t *s_dce;
static int s_rssi = 99;
/* One user of the modem's command channel at a time: redial vs. AT sessions. */
static SemaphoreHandle_t s_lock;

static void on_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == IP_EVENT_PPP_GOT_IP) {
        ip_event_got_ip_t *e = data;
        esp_netif_dns_info_t dns;
        esp_netif_get_dns_info(e->esp_netif, ESP_NETIF_DNS_MAIN, &dns);
        ESP_LOGI(TAG, "PPP up: ip " IPSTR " gw " IPSTR " dns " IPSTR,
                 IP2STR(&e->ip_info.ip), IP2STR(&e->ip_info.gw), IP2STR(&dns.ip.u_addr.ip4));
        xEventGroupClearBits(s_ev, LOST_IP);
        xEventGroupSetBits(s_ev, GOT_IP);
        board_led(true);
    } else if (id == IP_EVENT_PPP_LOST_IP) {
        ESP_LOGW(TAG, "PPP lost IP");
        xEventGroupClearBits(s_ev, GOT_IP);
        xEventGroupSetBits(s_ev, LOST_IP);
        board_led(false);
    }
}

/* Gets the modem answering AT in command mode; powers it on if it is off. */
static esp_err_t modem_to_command_mode(void)
{
    /* The modem may still be in PPP from a previous ESP boot (an ESP reset
     * does not reset the modem): detect and return to command mode. */
    if (esp_modem_set_mode(s_dce, ESP_MODEM_MODE_DETECT) == ESP_OK &&
            esp_modem_get_mode(s_dce) == ESP_MODEM_MODE_DATA) {
        ESP_LOGI(TAG, "modem was in data mode, switching to command mode");
        esp_modem_set_mode(s_dce, ESP_MODEM_MODE_COMMAND);
    }
    if (esp_modem_sync(s_dce) == ESP_OK) {
        return ESP_OK;
    }
    ESP_LOGI(TAG, "no AT answer, pulsing PWRKEY (STATUS=%d)", board_modem_status());
    board_modem_pwrkey_pulse();
    for (int i = 0; i < 30; i++) {
        if (esp_modem_sync(s_dce) == ESP_OK) {
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    return ESP_ERR_TIMEOUT;
}

static bool wait_registered(int seconds)
{
    int ber = 99;
    for (int i = 0; i < seconds; i++) {
        s_rssi = 99;
        esp_modem_get_signal_quality(s_dce, &s_rssi, &ber);
        if (s_rssi != 99) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    return false;
}

/* Command mode -> registered -> ATD; waits for the IP. */
static esp_err_t dial(int timeout_ms)
{
    esp_err_t err = modem_to_command_mode();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "modem does not answer AT");
        return err;
    }
    if (!wait_registered(60)) {
        ESP_LOGE(TAG, "no signal / not registered after 60 s");
        return ESP_ERR_TIMEOUT;
    }
    char op[ESP_MODEM_C_API_STR_BUF_SIZE] = {0};
    int act = 0;
    esp_modem_get_operator_name(s_dce, op, &act);
    ESP_LOGI(TAG, "registered: operator \"%s\" act %d, rssi %d", op, act, s_rssi);

    int64_t t0 = esp_timer_get_time();
    xEventGroupClearBits(s_ev, GOT_IP | LOST_IP);
    err = esp_modem_set_mode(s_dce, ESP_MODEM_MODE_DATA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "entering data mode failed: %s", esp_err_to_name(err));
        return err;
    }
    if (!cell_wait_ip(timeout_ms)) {
        ESP_LOGE(TAG, "no IP within %d ms", timeout_ms);
        esp_modem_set_mode(s_dce, ESP_MODEM_MODE_COMMAND);
        return ESP_ERR_TIMEOUT;
    }
    ESP_LOGI(TAG, "PPP connected in %lld ms", (esp_timer_get_time() - t0) / 1000);
    return ESP_OK;
}

/* Redials after IP loss. After several failures, power-cycles the modem. */
static void watchdog_task(void *arg)
{
    for (;;) {
        xEventGroupWaitBits(s_ev, LOST_IP, pdFALSE, pdFALSE, portMAX_DELAY);
        int failures = 0;
        while (!cell_is_up()) {
            ESP_LOGW(TAG, "redialing (attempt %d)", failures + 1);
            xSemaphoreTake(s_lock, portMAX_DELAY);
            esp_modem_set_mode(s_dce, ESP_MODEM_MODE_COMMAND);
            esp_err_t err = dial(60000);
            xSemaphoreGive(s_lock);
            if (err == ESP_OK) {
                break;
            }
            if (++failures % 5 == 0) {
                ESP_LOGW(TAG, "%d failed redials, power-cycling the modem", failures);
                board_modem_pwrkey_pulse();               /* off */
                vTaskDelay(pdMS_TO_TICKS(10000));
            }
            vTaskDelay(pdMS_TO_TICKS(5000 * (failures < 6 ? failures : 6)));
        }
    }
}

esp_err_t cell_start(int timeout_ms)
{
    s_ev = xEventGroupCreate();
    s_lock = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, on_ip, NULL));

    esp_modem_dte_config_t dte = ESP_MODEM_DTE_DEFAULT_CONFIG();
    /* The defaults (tx 25, rx 26, rts 27, cts 23) are WRONG for this board:
     * 25 is FLIGHT and 27 is our TX. No RTS/CTS is wired. */
    dte.uart_config.tx_io_num = BOARD_PIN_MODEM_TX;
    dte.uart_config.rx_io_num = BOARD_PIN_MODEM_RX;
    dte.uart_config.rts_io_num = -1;
    dte.uart_config.cts_io_num = -1;
    dte.uart_config.flow_control = ESP_MODEM_FLOW_CONTROL_NONE;
    dte.uart_config.rx_buffer_size = 8192;
    dte.uart_config.tx_buffer_size = 2048;
    dte.dte_buffer_size = 4096;
    esp_modem_dce_config_t dce_cfg = ESP_MODEM_DCE_DEFAULT_CONFIG(CONFIG_PROBE_APN);
    esp_netif_config_t ppp_cfg = ESP_NETIF_DEFAULT_PPP();
    esp_netif_t *ppp = esp_netif_new(&ppp_cfg);
    s_dce = esp_modem_new_dev(ESP_MODEM_DCE_SIM7600, &dte, &dce_cfg, ppp);
    if (!s_dce) {
        return ESP_FAIL;
    }

    esp_err_t err = modem_to_command_mode();
    if (err == ESP_OK) {
        char name[ESP_MODEM_C_API_STR_BUF_SIZE] = {0};
        esp_modem_get_module_name(s_dce, name);
        ESP_LOGI(TAG, "module %s, STATUS=%d", name, board_modem_status());
        err = dial(timeout_ms);
    }
    xTaskCreate(watchdog_task, "cell_wd", 4096, NULL, 5, NULL);
    if (err != ESP_OK) {
        xEventGroupSetBits(s_ev, LOST_IP);   /* let the watchdog keep trying */
    }
    return err;
}

bool cell_wait_ip(int timeout_ms)
{
    return xEventGroupWaitBits(s_ev, GOT_IP, pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms)) & GOT_IP;
}

bool cell_is_up(void)
{
    return s_ev && (xEventGroupGetBits(s_ev) & GOT_IP);
}

int cell_last_rssi(void)
{
    return s_rssi;
}

void cell_stop(void)
{
    if (s_dce) {
        xSemaphoreTake(s_lock, pdMS_TO_TICKS(15000));
        esp_modem_set_mode(s_dce, ESP_MODEM_MODE_COMMAND);
    }
}

esp_err_t cell_with_command_mode(cell_cmd_fn fn, void *ctx, int wait_ms)
{
    if (!cell_is_up()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(wait_ms)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (!cell_is_up()) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    int64_t t0 = esp_timer_get_time();
    /* Stops PPP output, waits 1 s (guard time), sends "+++" -> OK. The PPP
     * session stays alive in the modem; TCP retransmits cover the gap. */
    if (esp_modem_pause_net(s_dce, true) != ESP_OK) {
        ESP_LOGW(TAG, "could not leave data mode (+++), resuming PPP");
        esp_modem_pause_net(s_dce, false);
        xSemaphoreGive(s_lock);
        return ESP_ERR_NOT_FINISHED;
    }
    int64_t t1 = esp_timer_get_time();
    esp_err_t err = fn(ctx);
    int ber = 99;
    esp_modem_get_signal_quality(s_dce, &s_rssi, &ber);   /* free while we are here */
    int64_t t2 = esp_timer_get_time();

    esp_err_t back = esp_modem_pause_net(s_dce, false);   /* ATO -> CONNECT */
    if (back != ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(500));
        back = esp_modem_pause_net(s_dce, false);
    }
    if (back != ESP_OK) {
        /* PPP is gone: let the watchdog redial from command mode. */
        ESP_LOGE(TAG, "ATO failed, PPP lost; redialing");
        esp_modem_set_mode(s_dce, ESP_MODEM_MODE_COMMAND);
        xEventGroupClearBits(s_ev, GOT_IP);
        xEventGroupSetBits(s_ev, LOST_IP);
        board_led(false);
    }
    ESP_LOGI(TAG, "AT session: +++ %lld ms, commands %lld ms, ATO %lld ms (%s)",
             (t1 - t0) / 1000, (t2 - t1) / 1000, (esp_timer_get_time() - t2) / 1000,
             back == ESP_OK ? "PPP resumed" : "PPP lost");
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t cell_at(const char *cmd, char *out, size_t out_len, int timeout_ms)
{
    char buf[CONFIG_ESP_MODEM_C_API_STR_MAX] = {0};
    esp_err_t err = esp_modem_at(s_dce, cmd, buf, timeout_ms);
    if (out && out_len) {
        strlcpy(out, buf, out_len);
    }
    return err;
}
