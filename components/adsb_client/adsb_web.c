/* The radar's settings on the configuration page, over HTTP. Registered by
 * webcfg, behind its login (radio_web_endpoints() in adsb_client.c).
 *
 *   GET  /api/adsb   the settings and the feed's state, as JSON
 *   POST /api/adsb   settings, form-encoded: centre= an airport's ICAO code,
 *                    its place the middle; else lat= and lon= in degrees,
 *                    both empty for none; src, url (a receiver of one's
 *                    own), units, show, opts, refresh, types. The range is
 *                    the dial's, and stays as it is. The feed asked at once;
 *                    app_main hands the face what was saved
 *                    (adsb_client_take_settings).
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"

#include "adsb.h"
#include "adsb_client.h"
#include "net_prov.h"

static const char *TAG = "adsb_web";

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

static esp_err_t adsb_get(httpd_req_t *r)
{
    net_adsb_t a;
    net_prov_adsb(&a);
    char feed[24];
    bool ok = false;
    adsb_client_feed(feed, sizeof feed, &ok);
    char *j = malloc(768);
    if (!j) return httpd_resp_send_500(r);
    size_t n = 0;
    n += (size_t)snprintf(j + n, 768 - n, "{");
    if (a.lat_e6 != NET_ADSB_NO_POS)
        n += (size_t)snprintf(j + n, 768 - n, "\"lat\":%.6f,\"lon\":%.6f,", a.lat_e6 / 1e6, a.lon_e6 / 1e6);
    n = json_str(j, n, 768, "centre", a.centre);
    n = json_str(j, n, 768, "url", a.url);
    n = json_str(j, n, 768, "types", a.types);
    n = json_str(j, n, 768, "feed", feed);
    if (n < 768)
        n += (size_t)snprintf(j + n, 768 - n,
                              "\"ok\":%s,\"src\":%u,\"units\":%u,\"range\":%u,\"show\":%u,\"opts\":%u,"
                              "\"refresh\":%u}",
                              ok ? "true" : "false", (unsigned)a.source, (unsigned)a.units, (unsigned)a.range,
                              (unsigned)a.show, (unsigned)a.opts, (unsigned)a.refresh_s);
    if (n >= 768) {
        free(j);
        return httpd_resp_send_500(r);
    }
    httpd_resp_set_type(r, "application/json");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    const esp_err_t e = httpd_resp_send(r, j, (ssize_t)n);
    free(j);
    return e;
}

/* One form field, URL-decoded: ESP_OK; ESP_ERR_NOT_FOUND when absent, and
 * ESP_ERR_INVALID_SIZE when too long for out. */
static esp_err_t field_e(const char *body, const char *key, char *out, size_t len)
{
    char raw[400];
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

static esp_err_t adsb_post(httpd_req_t *r)
{
    const int total = r->content_len;
    if (total <= 0 || total >= 1024) return refuse(r, "body size");
    char *body = malloc((size_t)total + 1);
    if (!body) return httpd_resp_send_500(r);
    int got = 0;
    while (got < total) {
        const int k = httpd_req_recv(r, body + got, total - got);
        if (k <= 0) {
            free(body);
            return ESP_FAIL;
        }
        got += k;
    }
    body[got] = 0;

    net_adsb_t a;
    net_prov_adsb(&a);
    const char *why = NULL;
    char t[sizeof a.url];
    long v;
    esp_err_t e;

    /* The middle: an airport, else a place. */
    char code[8] = "", lat[24] = "", lon[24] = "";
    field(body, "centre", code, sizeof code);
    for (char *c = code; *c; c++) if (*c >= 'a' && *c <= 'z') *c -= 32;
    const bool got_lat = field(body, "lat", lat, sizeof lat), got_lon = field(body, "lon", lon, sizeof lon);
    if (code[0]) {
        const uint8_t *map, *apt;
        size_t mn, an;
        adsb_client_tables(&map, &mn, &apt, &an);
        const long i = adsb_apt_ok(apt, an) ? adsb_apt_find(apt, code) : -1;
        if (i < 0) {
            why = "no airport of that ICAO code in the knob's list";
        } else {
            adsb_apt_t p;
            adsb_apt_get(apt, (size_t)i, &p);
            a.lat_e6 = p.lat * 100;
            a.lon_e6 = p.lon * 100;
            strlcpy(a.centre, p.code, sizeof a.centre);
        }
    } else if (got_lat || got_lon) {
        a.centre[0] = 0;
        if (!lat[0] && !lon[0]) {
            a.lat_e6 = NET_ADSB_HOME_LAT_E6;
            a.lon_e6 = NET_ADSB_HOME_LON_E6;
            strlcpy(a.centre, NET_ADSB_HOME, sizeof a.centre);
        } else if (!degrees(lat, 90, &a.lat_e6) || !degrees(lon, 180, &a.lon_e6)) {
            why = "a latitude of -90 to 90 and a longitude of -180 to 180, in degrees";
        }
    }
    if (!why && field_num(body, "src", &v)) {
        if (adsb_src_ok(v)) a.source = (uint8_t)v;
        else why = "no such feed";
    }
    if (!why && (e = field_e(body, "url", t, sizeof a.url)) != ESP_ERR_NOT_FOUND) {
        if (e == ESP_OK) strlcpy(a.url, t, sizeof a.url);
        else why = "the receiver's address is too long";
    }
    if (!why && a.source == ADSB_SRC_LOCAL && !a.url[0]) why = "your receiver's address, for its aircraft.json";
    if (!why && field_num(body, "units", &v)) {
        if (adsb_units_ok(v)) a.units = (uint8_t)v;
        else why = "no such unit";
    }
    if (!why && field_num(body, "show", &v)) {
        if (adsb_show_ok(v)) a.show = (uint8_t)v;
        else why = "no such filter";
    }
    if (!why && field_num(body, "opts", &v)) {
        if (v >= 0 && v <= (NET_ADSB_AIRPORTS | NET_ADSB_COUNTRIES | NET_ADSB_SWEEP | NET_ADSB_GROUND))
            a.opts = (uint8_t)v;
        else why = "no such options";
    }
    if (!why && field_num(body, "refresh", &v))
        a.refresh_s = (uint8_t)(v < NET_ADSB_REFRESH_MIN ? NET_ADSB_REFRESH_MIN
                              : v > NET_ADSB_REFRESH_MAX ? NET_ADSB_REFRESH_MAX : v);
    if (!why && (e = field_e(body, "types", t, sizeof a.types)) != ESP_ERR_NOT_FOUND) {
        if (e == ESP_OK) strlcpy(a.types, t, sizeof a.types);
        else why = "the types to call out are too long";
    }
    free(body);
    if (why) return refuse(r, why);

    net_prov_save_adsb(&a);
    adsb_client_settings_saved();
    ESP_LOGI(TAG, "saved: middle %s %.4f %.4f, %s, every %u s", a.centre[0] ? a.centre : "-",
             a.lat_e6 == NET_ADSB_NO_POS ? 0.0 : a.lat_e6 / 1e6, a.lon_e6 == NET_ADSB_NO_POS ? 0.0 : a.lon_e6 / 1e6,
             adsb_src_name(a.source), (unsigned)a.refresh_s);
    return httpd_resp_sendstr(r, "ok");
}

const httpd_uri_t adsb_web_uris[] = {
    { .uri = "/api/adsb", .method = HTTP_GET,  .handler = adsb_get },
    { .uri = "/api/adsb", .method = HTTP_POST, .handler = adsb_post },
};
const size_t adsb_web_uris_n = sizeof adsb_web_uris / sizeof adsb_web_uris[0];
