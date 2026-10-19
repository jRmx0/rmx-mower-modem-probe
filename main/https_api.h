/* HTTPS REST to rmx-backend-probe: login (session cookie) and records CRUD.
 * One esp_http_client is kept open, so the TLS session is reused. */
#pragma once

#include "esp_err.h"
#include "probe_proto.h"

/* GET /probe/health. */
esp_err_t https_api_health(void);

/* POST /probe/api/auth/login; keeps the __Secure-probe_session cookie. */
esp_err_t https_api_login(void);

/* POST /probe/api/records {"value": value}. */
esp_err_t https_api_create(const char *value, probe_result_t *out);

/* GET /probe/api/records (newest 500). */
esp_err_t https_api_list(probe_result_t *out);

/* DELETE /probe/api/records/{id}. */
esp_err_t https_api_delete(int64_t id);

/* Frees the client (keeps the cookie). */
void https_api_close(void);
