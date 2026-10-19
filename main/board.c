#include "board.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"

void board_init(void)
{
    gpio_config_t out = {
        .pin_bit_mask = (1ULL << BOARD_PIN_PWRKEY) | (1ULL << BOARD_PIN_FLIGHT) |
                        (1ULL << BOARD_PIN_DTR) | (1ULL << BOARD_PIN_LED),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&out);
    gpio_set_level(BOARD_PIN_PWRKEY, 0);
    gpio_set_level(BOARD_PIN_FLIGHT, 1);   /* radio on */
    gpio_set_level(BOARD_PIN_DTR, 0);      /* awake */
    gpio_set_level(BOARD_PIN_LED, 0);
    gpio_config_t in = { .pin_bit_mask = 1ULL << BOARD_PIN_STATUS, .mode = GPIO_MODE_INPUT };
    gpio_config(&in);
}

void board_modem_pwrkey_pulse(void)
{
    gpio_set_level(BOARD_PIN_PWRKEY, 1);
    vTaskDelay(pdMS_TO_TICKS(500));
    gpio_set_level(BOARD_PIN_PWRKEY, 0);
}

bool board_modem_status(void)
{
    return gpio_get_level(BOARD_PIN_STATUS) == 1;
}

void board_led(bool on)
{
    gpio_set_level(BOARD_PIN_LED, on ? 1 : 0);
}
