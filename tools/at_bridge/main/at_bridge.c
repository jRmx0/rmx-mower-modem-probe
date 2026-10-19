/*
 * AT bridge for the LilyGo T-SIM7600 (ESP32 + SIM7600G-H).
 *
 * Powers the modem up with the board's pin sequence, then forwards bytes
 * between the USB console (UART0) and the modem (UART1), like LilyGo's
 * ATdebug example. Lines starting with '~' are bridge commands:
 *   ~pwr     pulse PWRKEY (toggles modem power: on if off, off if on)
 *   ~status  print the STATUS pin, FLIGHT and DTR levels
 *   ~flight0 / ~flight1   drive the FLIGHT pin (1 = radio allowed)
 *   ~baud N  change the ESP side baud rate of UART1
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "esp_chip_info.h"

#define MODEM_UART     UART_NUM_1
#define PIN_MODEM_TX   27   /* ESP TX -> modem RXD */
#define PIN_MODEM_RX   26   /* ESP RX <- modem TXD */
#define PIN_PWRKEY     4    /* HIGH pulse = PWRKEY pressed (inverted by a transistor) */
#define PIN_FLIGHT     25   /* HIGH = normal, LOW = flight mode */
#define PIN_DTR        32   /* LOW = keep the modem awake */
#define PIN_RING       33   /* input */
#define PIN_STATUS     34   /* input only: HIGH = modem running */
#define PIN_LED        12

#define PWRKEY_ON_MS   500
#define BOOT_WAIT_MS   20000

static const char *TAG = "at_bridge";

static int modem_read(char *buf, size_t len, int timeout_ms)
{
    int n = uart_read_bytes(MODEM_UART, buf, len - 1, pdMS_TO_TICKS(timeout_ms));
    if (n < 0) n = 0;
    buf[n] = 0;
    return n;
}

static bool modem_at_ok(int tries)
{
    char buf[128];
    for (int i = 0; i < tries; i++) {
        uart_flush_input(MODEM_UART);
        uart_write_bytes(MODEM_UART, "AT\r", 3);
        modem_read(buf, sizeof(buf), 300);
        if (strstr(buf, "OK")) return true;
    }
    return false;
}

static void pwrkey_pulse(int ms)
{
    gpio_set_level(PIN_PWRKEY, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(PIN_PWRKEY, 1);
    vTaskDelay(pdMS_TO_TICKS(ms));
    gpio_set_level(PIN_PWRKEY, 0);
}

static void print_status(void)
{
    printf("[bridge] STATUS(34)=%d RING(33)=%d FLIGHT(25)=%d DTR(32)=%d\n",
           gpio_get_level(PIN_STATUS), gpio_get_level(PIN_RING),
           gpio_get_level(PIN_FLIGHT), gpio_get_level(PIN_DTR));
}

static void modem_power_on(void)
{
    print_status();
    if (modem_at_ok(3)) {
        ESP_LOGI(TAG, "modem already answers AT");
        return;
    }
    ESP_LOGI(TAG, "pulsing PWRKEY %d ms", PWRKEY_ON_MS);
    int64_t t0 = xTaskGetTickCount();
    pwrkey_pulse(PWRKEY_ON_MS);
    while ((xTaskGetTickCount() - t0) < pdMS_TO_TICKS(BOOT_WAIT_MS)) {
        if (modem_at_ok(1)) {
            ESP_LOGI(TAG, "modem answered AT after %d ms",
                     (int)pdTICKS_TO_MS(xTaskGetTickCount() - t0));
            print_status();
            return;
        }
    }
    ESP_LOGW(TAG, "modem did not answer within %d ms", BOOT_WAIT_MS);
    print_status();
}

static void modem_to_console(void *arg)
{
    uint8_t buf[256];
    for (;;) {
        int n = uart_read_bytes(MODEM_UART, buf, sizeof(buf), pdMS_TO_TICKS(20));
        if (n > 0) uart_write_bytes(UART_NUM_0, buf, n);
    }
}

static void handle_bridge_cmd(char *line)
{
    if (!strcmp(line, "~pwr")) {
        pwrkey_pulse(PWRKEY_ON_MS);
        printf("[bridge] PWRKEY pulsed\n");
    } else if (!strcmp(line, "~pwroff")) {
        pwrkey_pulse(3000);
        printf("[bridge] PWRKEY held 3 s\n");
    } else if (!strcmp(line, "~status")) {
        print_status();
    } else if (!strcmp(line, "~flight0")) {
        gpio_set_level(PIN_FLIGHT, 0); print_status();
    } else if (!strcmp(line, "~flight1")) {
        gpio_set_level(PIN_FLIGHT, 1); print_status();
    } else if (!strncmp(line, "~baud ", 6)) {
        int b = atoi(line + 6);
        uart_set_baudrate(MODEM_UART, b);
        printf("[bridge] UART1 baud %d\n", b);
    } else {
        printf("[bridge] unknown: %s\n", line);
    }
}

void app_main(void)
{
    esp_chip_info_t ci;
    esp_chip_info(&ci);
    ESP_LOGI(TAG, "chip rev %d, %d cores, PSRAM %u bytes", ci.revision, ci.cores,
             (unsigned)esp_psram_get_size());

    gpio_config_t out = {
        .pin_bit_mask = (1ULL << PIN_PWRKEY) | (1ULL << PIN_FLIGHT) |
                        (1ULL << PIN_DTR) | (1ULL << PIN_LED),
        .mode = GPIO_MODE_INPUT_OUTPUT,
    };
    gpio_config(&out);
    gpio_set_level(PIN_PWRKEY, 0);
    gpio_set_level(PIN_FLIGHT, 1);
    gpio_set_level(PIN_DTR, 0);
    gpio_set_level(PIN_LED, 0);
    gpio_config_t in = {
        .pin_bit_mask = (1ULL << PIN_STATUS) | (1ULL << PIN_RING),
        .mode = GPIO_MODE_INPUT,
    };
    gpio_config(&in);

    uart_config_t uc = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(MODEM_UART, 4096, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(MODEM_UART, &uc));
    ESP_ERROR_CHECK(uart_set_pin(MODEM_UART, PIN_MODEM_TX, PIN_MODEM_RX,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM_0, 1024, 0, 0, NULL, 0));

    modem_power_on();
    gpio_set_level(PIN_LED, 1);
    printf("[bridge] ready: type AT commands (CR or CRLF). '~' lines are bridge commands.\n");

    xTaskCreate(modem_to_console, "m2c", 3072, NULL, 10, NULL);

    char line[256];
    size_t len = 0;
    uint8_t c;
    for (;;) {
        if (uart_read_bytes(UART_NUM_0, &c, 1, portMAX_DELAY) != 1) continue;
        if (len == 0 && c != '~') {
            /* Normal path: pass through byte by byte. */
            uart_write_bytes(MODEM_UART, &c, 1);
            continue;
        }
        if (c == '\r' || c == '\n') {
            line[len] = 0;
            if (len) handle_bridge_cmd(line);
            len = 0;
        } else if (len < sizeof(line) - 1) {
            line[len++] = c;
        }
    }
}
