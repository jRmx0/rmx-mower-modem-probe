#include "https_api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "sdkconfig.h"

static const char *TAG = "https";

#define COOKIE_NAME "__Secure-probe_session"
#define MAX_BODY    (256 * 1024)   /* a full list is ~100 KB */

static esp_http_client_handle_t s_client;
static char s_cookie[sizeof(COOKIE_NAME) + 1 + 128];   /* "name=<64 hex>" */
static char s_retry_after[16];

/* response body, grown as data arrives (large buffers go to PSRAM) */
static char *s_body;
static size_t s_body_len;
static size_t s_body_cap;

static void body_reset(void)
{
    s_body_len = 0;
    if (s_body) {
        s_body[0] = '\0';
    }
}

static void body_append(const char *data, size_t len)
{
    if (s_body_len + len + 1 > s_body_cap) {
        size_t cap = s_body_cap ? s_body_cap : 1024;
        while (cap < s_body_len + len + 1) {
            cap *= 2;
        }
        if (cap > MAX_BODY) {
            ESP_LOGW(TAG, "body larger than %d bytes, truncated", MAX_BODY);
            return;
        }
        char *b = realloc(s_body, cap);
        if (!b) {
            ESP_LOGE(TAG, "no memory for the body");
            return;
        }
        s_body = b;
        s_body_cap = cap;
    }
    memcpy(s_body + s_body_len, data, len);
    s_body_len += len;
    s_body[s_body_len] = '\0';
}

/* Set-Cookie: __Secure-probe_session=<64 hex>; Path=/probe; ... */
static void take_cookie(const char *value)
{
    if (strncmp(value, COOKIE_NAME "=", sizeof(COOKIE_NAME)) != 0) {
        return;
    }
    size_t n = strcspn(value, ";");
    if (n >= sizeof(s_cookie)) {
        ESP_LOGW(TAG, "session cookie too long (%u)", (unsigned)n);
        return;
    }
    memcpy(s_cookie, value, n);
    s_cookie[n] = '\0';
    /* never log the token itself: it is a login */
    ESP_LOGI(TAG, "got session cookie %s=<%u chars>", COOKIE_NAME,
             (unsigned)(n - sizeof(COOKIE_NAME)));
}

static esp_err_t on_http_event(esp_http_client_event_t *e)
{
    switch (e->event_id) {
    case HTTP_EVENT_ON_HEADER:
        if (strcasecmp(e->header_key, "Set-Cookie") == 0) {
            take_cookie(e->header_value);
        } else if (strcasecmp(e->header_key, "Retry-After") == 0) {
            snprintf(s_retry_after, sizeof(s_retry_after), "%s", e->header_value);
        }
        break;
    case HTTP_EVENT_ON_DATA:
        body_append(e->data, e->data_len);
        break;
    default:
        break;
    }
    return ESP_OK;
}

static esp_err_t ensure_client(void)
{
    if (s_client) {
        return ESP_OK;
    }
    esp_http_client_config_t cfg = {
        .url = CONFIG_PROBE_API_BASE "/health",
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = on_http_event,
        .timeout_ms = 30000,
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
        .keep_alive_enable = true,
    };
    s_client = esp_http_client_init(&cfg);
    return s_client ? ESP_OK : ESP_FAIL;
}

/* One request on the shared client. Sends Origin on non-GET and the session
 * cookie once. Returns the HTTP status, or -1 on a transport error. */
static int request(esp_http_client_method_t method, const char *path, const char *json)
{
    if (ensure_client() != ESP_OK) {
        return -1;
    }
    char url[160];
    snprintf(url, sizeof(url), "%s%s", CONFIG_PROBE_API_BASE, path);
    esp_http_client_set_url(s_client, url);
    esp_http_client_set_method(s_client, method);
    if (method == HTTP_METHOD_GET) {
        esp_http_client_delete_header(s_client, "Origin");
    } else {
        esp_http_client_set_header(s_client, "Origin", CONFIG_PROBE_API_ORIGIN);
    }
    if (s_cookie[0]) {
        esp_http_client_set_header(s_client, "Cookie", s_cookie);   /* replaces, never duplicates */
    } else {
        esp_http_client_delete_header(s_client, "Cookie");
    }
    if (json) {
        esp_http_client_set_header(s_client, "Content-Type", "application/json");
        esp_http_client_set_post_field(s_client, json, strlen(json));
    } else {
        esp_http_client_delete_header(s_client, "Content-Type");
        esp_http_client_set_post_field(s_client, NULL, 0);
    }
    body_reset();
    s_retry_after[0] = '\0';

    static const char *const NAMES[] = {
        [HTTP_METHOD_GET] = "GET", [HTTP_METHOD_POST] = "POST",
        [HTTP_METHOD_PUT] = "PUT", [HTTP_METHOD_DELETE] = "DELETE",
    };
    int64_t t0 = esp_timer_get_time();
    esp_err_t err = esp_http_client_perform(s_client);
    int64_t ms = (esp_timer_get_time() - t0) / 1000;
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s %s failed after %lld ms: %s", NAMES[method], path, ms, esp_err_to_name(err));
        esp_http_client_close(s_client);
        return -1;
    }
    int status = esp_http_client_get_status_code(s_client);
    ESP_LOGI(TAG, "%s %s -> %d in %lld ms (%u bytes)", NAMES[method], path, status, ms,
             (unsigned)s_body_len);
    return status;
}

static void log_error_body(int status)
{
    probe_result_t r;
    if (s_body_len && probe_proto_parse_rest(s_body, s_body_len, &r) && r.error_code[0]) {
        ESP_LOGE(TAG, "error %d %s: %s", status, r.error_code, r.error_message);
    } else if (status > 0) {
        ESP_LOGE(TAG, "unexpected status %d: %.*s", status, s_body_len > 200 ? 200 : (int)s_body_len,
                 s_body ? s_body : "");
    }
    if (s_retry_after[0]) {
        ESP_LOGW(TAG, "Retry-After: %s s", s_retry_after);
    }
}

esp_err_t https_api_health(void)
{
    int status = request(HTTP_METHOD_GET, "/health", NULL);
    if (status > 0) {
        ESP_LOGI(TAG, "health: %.*s", s_body_len > 200 ? 200 : (int)s_body_len, s_body ? s_body : "");
    }
    return status == 200 ? ESP_OK : ESP_FAIL;
}

esp_err_t https_api_login(void)
{
    if (!CONFIG_PROBE_WEB_PASSWORD[0]) {
        ESP_LOGE(TAG, "CONFIG_PROBE_WEB_PASSWORD is empty: set it with menuconfig");
        return ESP_ERR_INVALID_STATE;
    }
    s_cookie[0] = '\0';
    char *json = probe_proto_build_login(CONFIG_PROBE_WEB_USERNAME, CONFIG_PROBE_WEB_PASSWORD);
    int status = request(HTTP_METHOD_POST, "/api/auth/login", json);
    free(json);
    if (status != 200) {
        log_error_body(status);
        return ESP_FAIL;
    }
    if (!s_cookie[0]) {
        ESP_LOGE(TAG, "login 200 but no %s cookie", COOKIE_NAME);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "logged in: %.*s", s_body_len > 100 ? 100 : (int)s_body_len, s_body);
    return ESP_OK;
}

esp_err_t https_api_create(const char *value, probe_result_t *out)
{
    char *json = probe_proto_build_record_input(value);
    int status = request(HTTP_METHOD_POST, "/api/records", json);
    free(json);
    if (status != 201) {
        log_error_body(status);
        return ESP_FAIL;
    }
    probe_proto_parse_rest(s_body, s_body_len, out);
    return ESP_OK;
}

esp_err_t https_api_list(probe_result_t *out)
{
    int status = request(HTTP_METHOD_GET, "/api/records", NULL);
    if (status != 200) {
        log_error_body(status);
        return ESP_FAIL;
    }
    return probe_proto_parse_rest(s_body, s_body_len, out) ? ESP_OK : ESP_FAIL;
}

esp_err_t https_api_delete(int64_t id)
{
    char path[48];
    snprintf(path, sizeof(path), "/api/records/%lld", id);
    int status = request(HTTP_METHOD_DELETE, path, NULL);
    if (status != 204) {
        log_error_body(status);
        return ESP_FAIL;
    }
    return ESP_OK;
}

void https_api_close(void)
{
    if (s_client) {
        esp_http_client_cleanup(s_client);
        s_client = NULL;
    }
}
