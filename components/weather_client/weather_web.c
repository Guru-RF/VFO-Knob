/* The weather's settings on the configuration page, over HTTP. Registered
 * by webcfg, behind its login (radio_web_endpoints() in weather_client.c).
 *
 *   GET  /api/weather   the settings and the forecast's state, as JSON:
 *                       state "" while well, age the seconds since it came
 *   POST /api/weather   settings, form-encoded: lat= and lon= in degrees,
 *                       both empty for Brussels, with place= the name the
 *                       page found them by; temp (0 C, 1 F), wind (0 km/h,
 *                       1 m/s, 2 kn, 3 mph, 4 Bft), refresh (minutes),
 *                       cycle (seconds a face, 0 never). The face is the
 *                       dial's, and stays as it is. Open-Meteo
 *                       asked at once; app_main hands the face what was
 *                       saved (weather_client_take_settings).
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"

#include "net_prov.h"
#include "weather.h"
#include "weather_client.h"

static const char *TAG = "weather_web";

/* "key":"value", escaped, into j of cap. */
static size_t json_str(char *j, size_t at, size_t cap, const char *key, const char *s)
{
    at += (size_t)snprintf(j + at, at < cap ? cap - at : 0, "\"%s\":\"", key);
    for (; s && *s && at + 8 < cap; s++) {
        const unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { j[at++] = '\\'; j[at++] = (char)c; }
        else if (c < 0x20)         at += (size_t)snprintf(j + at, cap - at, "\\u%04x", c);
        else                       j[at++] = (char)c;
    }
    if (at < cap) at += (size_t)snprintf(j + at, cap - at, "\",");
    return at;
}

static esp_err_t weather_get(httpd_req_t *r)
{
    net_weather_t w;
    net_prov_weather(&w);
    char state[24];
    weather_client_state(state, sizeof state);
    char j[384];
    size_t n = (size_t)snprintf(j, sizeof j, "{\"lat\":%.6f,\"lon\":%.6f,", w.lat_e6 / 1e6, w.lon_e6 / 1e6);
    n = json_str(j, n, sizeof j, "place", w.place);
    n = json_str(j, n, sizeof j, "state", state);
    if (n < sizeof j)
        n += (size_t)snprintf(j + n, sizeof j - n,
                              "\"age\":%ld,\"temp\":%u,\"wind\":%u,\"refresh\":%u,\"cycle\":%u}",
                              (long)weather_client_age_s(), (unsigned)w.temp_unit, (unsigned)w.wind_unit,
                              (unsigned)w.refresh_min, (unsigned)w.cycle_s);
    if (n >= sizeof j) return httpd_resp_send_500(r);
    httpd_resp_set_type(r, "application/json");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    return httpd_resp_send(r, j, (ssize_t)n);
}

/* One form field, URL-decoded: ESP_OK; ESP_ERR_NOT_FOUND when absent, and
 * ESP_ERR_INVALID_SIZE when too long for out. */
static esp_err_t field_e(const char *body, const char *key, char *out, size_t len)
{
    char raw[200];
    const esp_err_t e = httpd_query_key_value(body, key, raw, sizeof raw);
    if (e == ESP_ERR_NOT_FOUND) return e;
    if (e != ESP_OK) return ESP_ERR_INVALID_SIZE;
    size_t o = 0;
    for (size_t i = 0; raw[i]; i++) {
        if (o + 1 >= len) return ESP_ERR_INVALID_SIZE;
        if (raw[i] == '+') {
            out[o++] = ' ';
        } else if (raw[i] == '%' && raw[i + 1] && raw[i + 2]) {
            const char h[3] = { raw[i + 1], raw[i + 2], 0 };
            out[o++] = (char)strtol(h, NULL, 16);
            i += 2;
        } else {
            out[o++] = raw[i];
        }
    }
    out[o] = 0;
    return ESP_OK;
}

static bool field(const char *body, const char *key, char *out, size_t len)
{
    return field_e(body, key, out, len) == ESP_OK;
}

static bool field_num(const char *body, const char *key, long *v)
{
    char t[16], *end;
    if (!field(body, key, t, sizeof t) || !t[0]) return false;
    *v = strtol(t, &end, 10);
    return end != t && !*end;
}

/* A degree: a number, and within lim either way. */
static bool degrees(const char *t, double lim, int32_t *e6)
{
    char *end;
    const double d = strtod(t, &end);
    if (end == t || *end || !isfinite(d) || fabs(d) > lim) return false;
    *e6 = (int32_t)lround(d * 1e6);
    return true;
}

static esp_err_t refuse(httpd_req_t *r, const char *why)
{
    httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, why);
    return ESP_FAIL;
}

static esp_err_t weather_post(httpd_req_t *r)
{
    const int total = r->content_len;
    if (total <= 0 || total >= 512) return refuse(r, "body size");
    char body[512];
    int got = 0;
    while (got < total) {
        const int k = httpd_req_recv(r, body + got, total - got);
        if (k <= 0) return ESP_FAIL;
        got += k;
    }
    body[got] = 0;

    net_weather_t w;
    net_prov_weather(&w);
    const char *why = NULL;
    long v;

    /* The place: both empty, Brussels; else a position, named as the page
     * found it -- or by its numbers, if it was typed. */
    char lat[24] = "", lon[24] = "", place[sizeof w.place] = "";
    const bool got_lat = field(body, "lat", lat, sizeof lat), got_lon = field(body, "lon", lon, sizeof lon);
    if (field_e(body, "place", place, sizeof place) == ESP_ERR_INVALID_SIZE) why = "the place's name is too long";
    if (!why && (got_lat || got_lon)) {
        if (!lat[0] && !lon[0]) {
            w.lat_e6 = NET_WX_HOME_LAT_E6;
            w.lon_e6 = NET_WX_HOME_LON_E6;
            strlcpy(w.place, NET_WX_HOME, sizeof w.place);
        } else if (!degrees(lat, 90, &w.lat_e6) || !degrees(lon, 180, &w.lon_e6)) {
            why = "a latitude of -90 to 90 and a longitude of -180 to 180, in degrees";
        } else if (place[0]) {
            strlcpy(w.place, place, sizeof w.place);
        } else {
            snprintf(w.place, sizeof w.place, "%.4f, %.4f", w.lat_e6 / 1e6, w.lon_e6 / 1e6);
        }
    }
    if (!why && field_num(body, "temp", &v)) {
        if (wx_temp_unit_ok(v)) w.temp_unit = (uint8_t)v;
        else why = "no such temperature unit";
    }
    if (!why && field_num(body, "wind", &v)) {
        if (wx_wind_unit_ok(v)) w.wind_unit = (uint8_t)v;
        else why = "no such wind unit";
    }
    if (!why && field_num(body, "refresh", &v))
        w.refresh_min = (uint8_t)(v < NET_WX_REFRESH_MIN ? NET_WX_REFRESH_MIN
                                : v > NET_WX_REFRESH_MAX ? NET_WX_REFRESH_MAX : v);
    if (!why && field_num(body, "cycle", &v))
        w.cycle_s = (uint8_t)(v < 0 ? 0 : v > NET_WX_CYCLE_MAX ? NET_WX_CYCLE_MAX : v);
    if (why) return refuse(r, why);

    net_prov_save_weather(&w);
    weather_client_settings_saved();
    ESP_LOGI(TAG, "saved: %s %.4f %.4f, %s, %s, every %u min, a face every %u s", w.place, w.lat_e6 / 1e6,
             w.lon_e6 / 1e6, w.temp_unit == WX_F ? "F" : "C", wx_wind_unit(w.wind_unit), (unsigned)w.refresh_min,
             (unsigned)w.cycle_s);
    return httpd_resp_sendstr(r, "ok");
}

const httpd_uri_t weather_web_uris[] = {
    { .uri = "/api/weather", .method = HTTP_GET,  .handler = weather_get },
    { .uri = "/api/weather", .method = HTTP_POST, .handler = weather_post },
};
const size_t weather_web_uris_n = sizeof weather_web_uris / sizeof weather_web_uris[0];
