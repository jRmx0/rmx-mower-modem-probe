/* Remote commands over MQTT: mower/<user>/cmd in, mower/<user>/cmd/ack out.
 *
 * Command: {"id":"c1","cmd":"ping","expires_at":1760900000,...}
 *   cmd: ping | status | create (value) | list | delete (record_id) |
 *        https_create (value) | https_list | ota (url, version, size) | restart
 *   expires_at: optional unix time (s); expired or unclocked commands are refused.
 *   gps: optional, create/https_create only: true sends the GNSS location as
 *        the value (or "gps unavailable: <why>"); the ack result gets
 *        "gps" (status) and "gps_fix". Not together with value.
 *   ota: url (must start with CONFIG_PROBE_OTA_URL_PREFIX), version (must match
 *        the image header), size (bytes, optional). Acks "started" at once,
 *        then a second ack "done" (and a restart into the new image) or an
 *        error: busy, same_version, low_signal, bad_image, bad_size,
 *        download_failed, failed. Progress is in "status" (result.ota).
 * Reply:   {"id":"c1","cmd":"ping","ok":true,"result":{...},"ms":12}
 *          {"id":"c1","cmd":"x","ok":false,"error":"<code>","message":"..."}
 */
#pragma once

#include "esp_err.h"

/* Creates the worker task and registers with mqtt_link. Call before
 * mqtt_link_start(). */
esp_err_t remote_cmd_init(void);
