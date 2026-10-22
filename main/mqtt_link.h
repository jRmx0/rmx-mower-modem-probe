/* MQTT over TLS to Mosquitto: request/response with rmx-backend-probe. */
#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "probe_proto.h"

/* Called on the MQTT task for each message on mower/<user>/cmd. Must not
 * block: hand the work to another task. */
typedef void (*mqtt_link_cmd_cb_t)(const char *data, size_t len);

/* Set before mqtt_link_start(). */
void mqtt_link_set_cmd_handler(mqtt_link_cmd_cb_t cb);

/* Starts the client (auto-reconnect) and subscribes probe/res/<user>,
 * probe/events/records, probe/status/server and (with a cmd handler)
 * mower/<user>/cmd on every connect. */
esp_err_t mqtt_link_start(void);

/* Waits until connected and probe/res/<user> is subscribed. */
bool mqtt_link_wait_ready(int timeout_ms);

/* Publishes a request on probe/req/<user> (QoS 1) and waits for the matching
 * response on probe/res/<user>. One request at a time. */
esp_err_t mqtt_link_request(probe_op_t op, int64_t id, const char *value,
                            probe_result_t *out, int timeout_ms);

/* Publishes on mower/<user>/<subtopic> (QoS 1), e.g. "firmware". */
esp_err_t mqtt_link_publish_own(const char *subtopic, const char *payload, bool retain);

/* Publishes a reply on mower/<user>/cmd/ack (QoS 1). */
esp_err_t mqtt_link_send_ack(const char *json);

void mqtt_link_stop(void);
