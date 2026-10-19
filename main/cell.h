/* Cellular link: esp_modem DTE/DCE on the SIM7600, PPP up/down, redial on loss. */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

/* Creates the DCE, brings the modem to command mode (powering it on if
 * needed), waits for registration and dials PPP. Blocks until the first IP
 * or timeout. Starts a task that redials when PPP drops. */
esp_err_t cell_start(int timeout_ms);

/* Waits until PPP has an IP (true) or timeout (false). */
bool cell_wait_ip(int timeout_ms);

bool cell_is_up(void);

/* Signal quality from the last check (99 = unknown). */
int cell_last_rssi(void);

/* Leaves PPP (modem back in command mode) so the next boot starts clean. */
void cell_stop(void);
