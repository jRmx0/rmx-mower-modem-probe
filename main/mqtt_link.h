/* MQTT over TLS to Mosquitto: request/response with rmx-backend-probe. */
#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "probe_proto.h"

/* Starts the client (auto-reconnect) and subscribes probe/res/<user>,
 * probe/events/records and probe/status/server on every connect. */
esp_err_t mqtt_link_start(void);

/* Waits until connected and probe/res/<user> is subscribed. */
bool mqtt_link_wait_ready(int timeout_ms);

/* Publishes a request on probe/req/<user> (QoS 1) and waits for the matching
 * response on probe/res/<user>. One request at a time. */
esp_err_t mqtt_link_request(probe_op_t op, int64_t id, const char *value,
                            probe_result_t *out, int timeout_ms);

void mqtt_link_stop(void);
