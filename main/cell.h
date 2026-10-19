/* Cellular link: esp_modem DTE/DCE on the SIM7600, PPP up/down, redial on loss. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
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

/* Runs fn(ctx) with the modem briefly out of PPP: pauses the netif, "+++",
 * fn, "ATO". The PPP session and TCP connections survive (~2-3 s gap).
 * Inside fn, talk to the modem with cell_at(). Returns fn's result, or
 * ESP_ERR_INVALID_STATE (PPP down / redialing), ESP_ERR_TIMEOUT (modem busy
 * for wait_ms), ESP_ERR_NOT_FINISHED ("+++" failed, PPP left running).
 * If "ATO" fails, PPP is treated as lost and the watchdog redials. */
typedef esp_err_t (*cell_cmd_fn)(void *ctx);
esp_err_t cell_with_command_mode(cell_cmd_fn fn, void *ctx, int wait_ms);

/* Raw AT command (without "\r"), only inside cell_with_command_mode().
 * out gets the last non-empty response line before OK (max 127 chars).
 * ESP_FAIL on ERROR, ESP_ERR_TIMEOUT without an answer. */
esp_err_t cell_at(const char *cmd, char *out, size_t out_len, int timeout_ms);
