/* Firmware updates over HTTPS (esp_https_ota) and the first-boot check of a new image. */
#pragma once

#include <stdbool.h>
#include "cJSON.h"
#include "esp_err.h"

/* Call once at boot. Logs the running slot. If this image is new (pending
 * verify), arms a timer that rolls back to the previous image unless
 * ota_confirm() is called within CONFIG_PROBE_OTA_CONFIRM_S. */
void ota_boot_init(void);

/* The new image works (MQTT connected): cancel the rollback. Safe to call
 * any number of times. */
void ota_confirm(void);

/* After ota_confirm() of an image that came by OTA: tells the server once.
 * Publishes {"version","previous","partition","time"} retained on
 * mower/<user>/firmware and creates a probe record "firmware updated ...".
 * Call with MQTT connected; does nothing otherwise or when already sent. */
void ota_report_update(void);

/* True while a download runs. */
bool ota_busy(void);

/* Checks the request and starts the download on its own task. On success the
 * task acks "done" on mower/<user>/cmd/ack with this id and restarts into the
 * new image; on failure it acks the error. Returns false with an error code
 * and message for the immediate ack when the request is refused. */
bool ota_start(const char *cmd_id, const char *url, const char *version, int size,
               const char **error, const char **message);

/* Adds "version", "partition" and "ota" (last update attempt) to a status result. */
void ota_add_status(cJSON *res);
