/* The radar's settings on the configuration page, over HTTP. Registered by
 * webcfg, behind its login (radio_web_endpoints() in ais_client.c).
 *
 *   GET  /api/ais   the settings and the source's state, as JSON. The key
 *                   is never sent back: only whether one is set.
 *   POST /api/ais   settings, form-encoded: port= a harbour's name, its
 *                   place the middle; else lat= and lon= in degrees, both
 *                   empty for Westende, and place= a name for it; src; key
 *                   (empty leaves it as it is; keyclear=1 forgets it); url
 *                   (an AIS-catcher of one's own); units, show, opts,
 *                   refresh, watch. The range is the dial's, and stays as
 *                   it is. The source asked anew at once; app_main hands
 *                   the face what was saved (ais_client_take_settings).
 */
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_http_server.h"
#include "esp_log.h"

#include "adsb.h"
#include "ais.h"
#include "ais_client.h"
#include "net_prov.h"

static const char *TAG = "ais_web";

#define JSON_CAP 1024

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

static esp_err_t ais_get(httpd_req_t *r)
{
    net_ais_t a;
    net_prov_ais(&a);
    char feed[24];
    bool ok = false;
    ais_client_feed(feed, sizeof feed, &ok);
    uint32_t per_min;
    int known;
    ais_client_counts(&per_min, &known);
    char *j = malloc(JSON_CAP);
    if (!j) return httpd_resp_send_500(r);
    size_t n = 0;
    n += (size_t)snprintf(j + n, JSON_CAP - n, "{");
    if (a.lat_e6 != NET_AIS_NO_POS)
        n += (size_t)snprintf(j + n, JSON_CAP - n, "\"lat\":%.6f,\"lon\":%.6f,", a.lat_e6 / 1e6, a.lon_e6 / 1e6);
    n = json_str(j, n, JSON_CAP, "place", a.place);
    n = json_str(j, n, JSON_CAP, "url", a.url);
    n = json_str(j, n, JSON_CAP, "watch", a.watch);
    n = json_str(j, n, JSON_CAP, "feed", feed);
    if (n < JSON_CAP)
        n += (size_t)snprintf(j + n, JSON_CAP - n,
                              "\"ok\":%s,\"key\":%s,\"src\":%u,\"units\":%u,\"range\":%u,\"show\":%u,\"opts\":%u,"
                              "\"refresh\":%u,\"permin\":%u,\"known\":%d}",
                              ok ? "true" : "false", a.key[0] ? "true" : "false", (unsigned)a.source,
                              (unsigned)a.units, (unsigned)a.range, (unsigned)a.show, (unsigned)a.opts,
                              (unsigned)a.refresh_s, (unsigned)per_min, known);
    if (n >= JSON_CAP) {
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

static bool degrees(const char *t, double lim, int32_t *e6)
{
    char *end;
    const double d = strtod(t, &end);
    if (end == t || *end || !isfinite(d) || fabs(d) > lim) return false;
    *e6 = (int32_t)lround(d * 1e6);
    return true;
}

static void trim(char *t)
{
    size_t n = strlen(t);
    while (n && isspace((unsigned char)t[n - 1])) t[--n] = 0;
    size_t i = 0;
    while (t[i] && isspace((unsigned char)t[i])) i++;
    if (i) memmove(t, t + i, n - i + 1);
}

/* A harbour by its name in the knob's list: the first whose name is it,
 * any case, else the first that starts so. */
static bool port_find(const char *name, ais_port_t *out)
{
    const uint8_t *map, *ports;
    size_t mn, pn;
    ais_client_tables(&map, &mn, &ports, &pn);
    if (!ais_port_ok(ports, pn) || !name[0]) return false;
    const size_t c = ais_port_count(ports), len = strlen(name);
    long start = -1;
    for (size_t i = 0; i < c; i++) {
        ais_port_t p;
        ais_port_get(ports, i, &p);
        if (!strcasecmp(p.name, name)) {
            *out = p;
            return true;
        }
        if (start < 0 && !strncasecmp(p.name, name, len)) start = (long)i;
    }
    if (start < 0) return false;
    ais_port_get(ports, (size_t)start, out);
    return true;
}

static esp_err_t refuse(httpd_req_t *r, const char *why)
{
    httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, why);
    return ESP_FAIL;
}

static esp_err_t ais_post(httpd_req_t *r)
{
    const int total = r->content_len;
    if (total <= 0 || total >= 1536) return refuse(r, "body size");
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

    net_ais_t a;
    net_prov_ais(&a);
    const char *why = NULL;
    char t[sizeof a.url];
    long v;
    esp_err_t e;

    /* The middle: a harbour, else a place. */
    char port[24] = "", lat[24] = "", lon[24] = "", place[sizeof a.place] = "";
    field(body, "port", port, sizeof port);
    trim(port);
    const bool got_lat = field(body, "lat", lat, sizeof lat), got_lon = field(body, "lon", lon, sizeof lon);
    field(body, "place", place, sizeof place);
    trim(place);
    if (port[0]) {
        ais_port_t p;
        if (!port_find(port, &p)) {
            why = "no harbour of that name in the knob's list";
        } else {
            a.lat_e6 = p.lat * 100;
            a.lon_e6 = p.lon * 100;
            strlcpy(a.place, p.name, sizeof a.place);
        }
    } else if (got_lat || got_lon) {
        trim(lat);
        trim(lon);
        if (!lat[0] && !lon[0]) {
            a.lat_e6 = NET_AIS_HOME_LAT_E6;
            a.lon_e6 = NET_AIS_HOME_LON_E6;
            strlcpy(a.place, NET_AIS_HOME, sizeof a.place);
        } else if (!degrees(lat, 90, &a.lat_e6) || !degrees(lon, 180, &a.lon_e6)) {
            why = "a latitude of -90 to 90 and a longitude of -180 to 180, in degrees";
        } else {
            strlcpy(a.place, place, sizeof a.place);
        }
    }
    if (!why && field_num(body, "src", &v)) {
        if (ais_src_ok(v)) a.source = (uint8_t)v;
        else why = "no such source";
    }
    /* The key: kept as it is unless one is typed; forgotten when asked. */
    char key[sizeof a.key];
    if (!why && (e = field_e(body, "key", key, sizeof key)) != ESP_ERR_NOT_FOUND) {
        if (e != ESP_OK) {
            why = "the key is too long";
        } else {
            trim(key);
            for (const char *p = key; *p && !why; p++)
                if ((unsigned char)*p < 0x21 || *p == '"' || *p == '\\' || (unsigned char)*p > 0x7E)
                    why = "the key has a character no AISStream key has";
            if (!why && key[0]) strlcpy(a.key, key, sizeof a.key);
        }
    }
    if (!why && field_num(body, "keyclear", &v) && v == 1) a.key[0] = 0;
    if (!why && (e = field_e(body, "url", t, sizeof a.url)) != ESP_ERR_NOT_FOUND) {
        if (e == ESP_OK) {
            trim(t);
            strlcpy(a.url, t, sizeof a.url);
        } else {
            why = "the AIS-catcher's address is too long";
        }
    }
    if (!why && a.source == AIS_SRC_CATCHER && !a.url[0]) why = "your AIS-catcher's address, as http://host:port";
    if (!why && field_num(body, "units", &v)) {
        if (adsb_units_ok(v)) a.units = (uint8_t)v;
        else why = "no such unit";
    }
    if (!why && field_num(body, "show", &v)) {
        if (ais_show_ok(v)) a.show = (uint8_t)v;
        else why = "no such filter";
    }
    if (!why && field_num(body, "opts", &v)) {
        if (v >= 0 && v <= (NET_AIS_PORTS | NET_AIS_COUNTRIES | NET_AIS_SWEEP)) a.opts = (uint8_t)v;
        else why = "no such options";
    }
    if (!why && field_num(body, "refresh", &v))
        a.refresh_s = (uint8_t)(v < NET_AIS_REFRESH_MIN ? NET_AIS_REFRESH_MIN
                              : v > NET_AIS_REFRESH_MAX ? NET_AIS_REFRESH_MAX : v);
    char w[sizeof a.watch];
    if (!why && (e = field_e(body, "watch", w, sizeof w)) != ESP_ERR_NOT_FOUND) {
        if (e == ESP_OK) {
            trim(w);
            strlcpy(a.watch, w, sizeof a.watch);
        } else {
            why = "the ships to call out are too many";
        }
    }
    free(body);
    if (why) return refuse(r, why);

    net_prov_save_ais(&a);
    ais_client_settings_saved();
    /* Never the key: only that there is one. */
    ESP_LOGI(TAG, "saved: middle %s %.4f %.4f, %s%s", a.place[0] ? a.place : "-",
             a.lat_e6 == NET_AIS_NO_POS ? 0.0 : a.lat_e6 / 1e6, a.lon_e6 == NET_AIS_NO_POS ? 0.0 : a.lon_e6 / 1e6,
             ais_src_name(a.source), a.source == AIS_SRC_AISSTREAM ? (a.key[0] ? ", a key set" : ", no key") : "");
    return httpd_resp_sendstr(r, "ok");
}

const httpd_uri_t ais_web_uris[] = {
    { .uri = "/api/ais", .method = HTTP_GET,  .handler = ais_get },
    { .uri = "/api/ais", .method = HTTP_POST, .handler = ais_post },
};
const size_t ais_web_uris_n = sizeof ais_web_uris / sizeof ais_web_uris[0];
