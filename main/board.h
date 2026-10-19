/* LilyGo T-SIM7600 pins and modem power control. */
#pragma once

#include <stdbool.h>

#define BOARD_PIN_MODEM_TX  27   /* ESP TX -> modem RX (UART1) */
#define BOARD_PIN_MODEM_RX  26   /* ESP RX <- modem TX */
#define BOARD_PIN_PWRKEY    4    /* HIGH 500 ms toggles modem power */
#define BOARD_PIN_FLIGHT    25   /* HIGH = radio on */
#define BOARD_PIN_DTR       32   /* LOW = awake */
#define BOARD_PIN_STATUS    34   /* input, 1 = modem on */
#define BOARD_PIN_LED       12

/* Sets PWRKEY low, FLIGHT high, DTR low; STATUS as input; LED off. */
void board_init(void);

/* 500 ms PWRKEY pulse: turns the modem on if off, OFF if on. Send AT first. */
void board_modem_pwrkey_pulse(void);

/* STATUS pin: true while the modem is powered. */
bool board_modem_status(void);

void board_led(bool on);
