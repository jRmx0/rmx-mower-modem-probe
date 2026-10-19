#include "gnss.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "cell.h"

static const char *TAG = "gnss";

#define LINE_LEN   128
#define MAX_FIELDS 24

static int64_t s_on_since;   /* esp_timer us when GNSS was seen/turned on, 0 = unknown */

typedef struct {
    bool want_info;
    bool ran;                  /* got the AT channel */
    esp_err_t cgps_err;        /* AT+CGPS? */
    bool was_off;
    esp_err_t start_err;       /* AT+CGPS=1 when it was off */
    esp_err_t info_err;        /* AT+CGNSSINFO */
    char cgps[LINE_LEN];
    char info[LINE_LEN];
} session_t;

static const char *at_err_name(esp_err_t err)
{
    return err == ESP_ERR_TIMEOUT ? "timeout" : "error";
}

/* Runs in command mode (cell_with_command_mode). */
static esp_err_t session(void *ctx)
{
    session_t *s = ctx;
    s->ran = true;
    s->cgps_err = cell_at("AT+CGPS?", s->cgps, sizeof(s->cgps), 3000);
    if (s->cgps_err != ESP_OK) {
        return s->cgps_err;
    }
    /* "+CGPS: <on>,<mode>" */
    const char *p = strstr(s->cgps, "+CGPS:");
    s->was_off = !p || atoi(p + 6) != 1;
    if (s->was_off) {
        /* LilyGo: the SIM7600 boards feed the active GPS antenna from VDD_AUX. */
        cell_at("AT+CVAUXV=2800", NULL, 0, 3000);
        cell_at("AT+CVAUXS=1", NULL, 0, 3000);
        s->start_err = cell_at("AT+CGPS=1", NULL, 0, 5000);
        if (s->start_err == ESP_OK) {
            s_on_since = esp_timer_get_time();
        }
    } else if (!s_on_since) {
        s_on_since = esp_timer_get_time();   /* already on (modem outlived an ESP reset) */
    }
    if (s->want_info && !s->was_off) {
        s->info_err = cell_at("AT+CGNSSINFO", s->info, sizeof(s->info), 3000);
    }
    return ESP_OK;
}

static esp_err_t run_session(session_t *s)
{
    memset(s, 0, sizeof(*s));
    s->want_info = true;
    s->cgps_err = s->start_err = s->info_err = ESP_FAIL;
    return cell_with_command_mode(session, s, 10000);
}

esp_err_t gnss_start(void)
{
    session_t s;
    esp_err_t err = run_session(&s);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "GNSS start failed: %s (%s)", esp_err_to_name(err), s.cgps);
        return err;
    }
    if (s.was_off) {
        ESP_LOGI(TAG, "GNSS was off (%s), AT+CGPS=1 -> %s", s.cgps,
                 s.start_err == ESP_OK ? "OK" : at_err_name(s.start_err));
        return s.start_err;
    }
    ESP_LOGI(TAG, "GNSS already on (%s), %s", s.cgps, s.info);
    return ESP_OK;
}

/* Splits on ',' keeping empty fields. Returns the field count. */
static int split(char *line, char *fields[], int max)
{
    int n = 0;
    fields[n++] = line;
    for (char *c = line; *c && n < max; c++) {
        if (*c == ',') {
            *c = '\0';
            fields[n++] = c + 1;
        }
    }
    return n;
}

/* ddmm.mmmm / dddmm.mmmm -> decimal degrees */
static double nmea_deg(const char *v, char hemi)
{
    double raw = atof(v);
    int deg = (int)(raw / 100);
    double d = deg + (raw - deg * 100) / 60.0;
    return (hemi == 'S' || hemi == 'W') ? -d : d;
}

/* "+CGNSSINFO: <mode>,<GPS-SVs>,<GLONASS-SVs>,<BEIDOU-SVs>,<lat>,<N/S>,<lon>,<E/W>,
 *  <ddmmyy>,<hhmmss.s>,<alt>,<speed>,<course>,<PDOP>,<HDOP>,<VDOP>"
 * Empty fields without a fix. The latitude is found by its N/S neighbour so
 * an extra satellite-count field (other firmware) does not break parsing. */
static bool parse_info(const char *line, char *value, size_t vlen, char *status, size_t slen)
{
    const char *p = strstr(line, "+CGNSSINFO:");
    if (!p) {
        return false;
    }
    char buf[LINE_LEN];
    strlcpy(buf, p + strlen("+CGNSSINFO:"), sizeof(buf));
    char *f[MAX_FIELDS];
    int n = split(buf, f, MAX_FIELDS);
    for (int i = 0; i < n; i++) {
        while (*f[i] == ' ') {
            f[i]++;
        }
    }
    int lat = -1;
    for (int i = 1; i + 3 < n; i++) {
        if ((!strcmp(f[i + 1], "N") || !strcmp(f[i + 1], "S")) && f[i][0]) {
            lat = i;
            break;
        }
    }
    if (lat < 0 || lat + 9 >= n) {
        return false;
    }
    int sats = 0;
    for (int i = 1; i < lat; i++) {
        sats += atoi(f[i]);
    }
    const char *date = f[lat + 4], *tm = f[lat + 5];
    char utc[24] = "";
    if (strlen(date) >= 6 && strlen(tm) >= 6) {
        snprintf(utc, sizeof(utc), "20%.2s-%.2s-%.2sT%.2s:%.2s:%.2sZ",
                 date + 4, date + 2, date, tm, tm + 2, tm + 4);
    }
    int mode = atoi(f[0]);
    const char *hdop = lat + 10 < n && f[lat + 10][0] ? f[lat + 10] : "null";
    snprintf(value, vlen,
             "{\"gps\":\"fix\",\"lat\":%.6f,\"lon\":%.6f,\"alt\":%s,\"utc\":\"%s\","
             "\"sats\":%d,\"hdop\":%s,\"mode\":\"%s\"}",
             nmea_deg(f[lat], f[lat + 1][0]), nmea_deg(f[lat + 2], f[lat + 3][0]),
             f[lat + 6][0] ? f[lat + 6] : "null", utc, sats, hdop,
             mode == 3 ? "3D" : mode == 2 ? "2D" : "?");
    snprintf(status, slen, "fix (%s, %d sats, %s)", mode == 3 ? "3D" : "2D", sats, utc);
    return true;
}

static bool has_digit(const char *s)
{
    return strpbrk(s, "0123456789") != NULL;
}

bool gnss_read(char *value, size_t vlen, char *status, size_t slen)
{
    session_t s;
    char why[160];
    esp_err_t err = run_session(&s);
    int on_s = s_on_since ? (int)((esp_timer_get_time() - s_on_since) / 1000000) : -1;

    if (err == ESP_ERR_INVALID_STATE) {
        snprintf(why, sizeof(why), "no data link (PPP down, modem redialing)");
    } else if (err == ESP_ERR_TIMEOUT && !s.ran) {
        snprintf(why, sizeof(why), "modem busy (AT channel not free within 10 s)");
    } else if (err == ESP_ERR_NOT_FINISHED) {
        snprintf(why, sizeof(why), "could not leave PPP data mode (+++ failed)");
    } else if (s.cgps_err != ESP_OK) {
        snprintf(why, sizeof(why), "AT+CGPS? %s", at_err_name(s.cgps_err));
    } else if (s.was_off && s.start_err != ESP_OK) {
        snprintf(why, sizeof(why), "GNSS off, AT+CGPS=1 %s", at_err_name(s.start_err));
    } else if (s.was_off) {
        snprintf(why, sizeof(why), "GNSS was off, turned on now (no fix yet)");
    } else if (s.info_err != ESP_OK) {
        snprintf(why, sizeof(why), "AT+CGNSSINFO %s", at_err_name(s.info_err));
    } else if (parse_info(s.info, value, vlen, status, slen)) {
        ESP_LOGI(TAG, "%s", status);
        return true;
    } else if (strstr(s.info, "+CGNSSINFO:") && !has_digit(strstr(s.info, ":"))) {
        snprintf(why, sizeof(why), "no fix (searching %d s)", on_s);
    } else {
        snprintf(why, sizeof(why), "unexpected reply \"%.80s\"", s.info);
    }
    snprintf(value, vlen, "gps unavailable: %s", why);
    strlcpy(status, value, slen);
    ESP_LOGW(TAG, "%s", value);
    return false;
}
