/* Remote commands over MQTT: mower/<user>/cmd in, mower/<user>/cmd/ack out.
 *
 * Command: {"id":"c1","cmd":"ping","expires_at":1760900000,...}
 *   cmd: ping | status | create (value) | list | delete (record_id) |
 *        https_create (value) | https_list | restart
 *   expires_at: optional unix time (s); expired or unclocked commands are refused.
 *   gps: optional, create/https_create only: true sends the GNSS location as
 *        the value (or "gps unavailable: <why>"); the ack result gets
 *        "gps" (status) and "gps_fix". Not together with value.
 * Reply:   {"id":"c1","cmd":"ping","ok":true,"result":{...},"ms":12}
 *          {"id":"c1","cmd":"x","ok":false,"error":"<code>","message":"..."}
 */
#pragma once

#include "esp_err.h"

/* Creates the worker task and registers with mqtt_link. Call before
 * mqtt_link_start(). */
esp_err_t remote_cmd_init(void);
