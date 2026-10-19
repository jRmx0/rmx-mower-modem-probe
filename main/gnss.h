/* GNSS of the SIM7600 over AT, read while PPP is briefly paused (see cell.h).
 *
 * Fix:     {"gps":"fix","lat":12.345678,"lon":-98.765432,"alt":112.3,
 *           "utc":"2026-10-20T09:19:18Z","sats":9,"hdop":0.8,"mode":"3D"}
 * No fix:  gps unavailable: no fix (searching 42 s)
 *          gps unavailable: <reason>   (GNSS off, AT error, timeout, PPP down, ...)
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

/* Powers the active antenna (AUX 2.8 V) and turns GNSS on (AT+CGPS=1).
 * Needs PPP up (it uses a command-mode session). */
esp_err_t gnss_start(void);

/* Writes the location (or why there is none) to value and a short status
 * for the command ack to status. Returns true on a fix. Never fails:
 * value is always set. */
bool gnss_read(char *value, size_t value_len, char *status, size_t status_len);
