/* The Clock's alarms on the configuration page, over HTTP. Registered by
 * webcfg, behind its login (radio_web_endpoints() in clock_client.c).
 *
 *   GET  /api/alarms   the alarms, as JSON: "a" ten of {h, m, d, on} -- d
 *                      their days, Monday bit 0, 0 once -- with snooze and
 *                      ring in minutes, by what rings (NET_ALARM_*), vol
 *                      the sound's, and time whether the knob has the time
 *                      yet
 *   POST /api/alarms   form-encoded: t0..t9 HHMM, d0..d9 the days, o0..o9
 *                      1 on; snooze, ring, by, vol. What is left out is
 *                      kept.
 *                      app_main hands the face what was saved
 *                      (clock_client_take_alarms).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"

#include "net_prov.h"
#include "wallclock.h"

static const char *TAG = "clock_web";

void clock_client_alarms_saved(void);

static esp_err_t alarms_get(httpd_req_t *r)
{
    net_alarms_t al;
    net_prov_alarms(&al);
    char j[640];
    size_t n = (size_t)snprintf(j, sizeof j,
                                "{\"snooze\":%u,\"ring\":%u,\"by\":%u,\"vol\":%u,\"time\":%s,\"a\":[",
                                (unsigned)al.snooze_min, (unsigned)al.ring_min, (unsigned)al.by,
                                (unsigned)al.volume,
                                wallclock_age_s() != UINT32_MAX ? "true" : "false");
    for (int i = 0; i < ALARM_N && n < sizeof j; i++)
        n += (size_t)snprintf(j + n, sizeof j - n, "%s{\"h\":%u,\"m\":%u,\"d\":%u,\"on\":%s}", i ? "," : "",
                              (unsigned)al.a[i].hour, (unsigned)al.a[i].min, (unsigned)al.a[i].days,
                              al.a[i].on ? "true" : "false");
    if (n < sizeof j) n += (size_t)snprintf(j + n, sizeof j - n, "]}");
    if (n >= sizeof j) return httpd_resp_send_500(r);
    httpd_resp_set_type(r, "application/json");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    return httpd_resp_send(r, j, (ssize_t)n);
}

/* A field that is a whole number, nothing else in it. */
static bool field_num(const char *body, const char *key, long *v)
{
    char t[16], *end;
    if (httpd_query_key_value(body, key, t, sizeof t) != ESP_OK || !t[0]) return false;
    *v = strtol(t, &end, 10);
    return end != t && !*end;
}

static esp_err_t refuse(httpd_req_t *r, const char *why)
{
    httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, why);
    return ESP_FAIL;
}

static esp_err_t alarms_post(httpd_req_t *r)
{
    const int total = r->content_len;
    if (total <= 0 || total >= 640) return refuse(r, "body size");
    char body[640];
    int got = 0;
    while (got < total) {
        const int k = httpd_req_recv(r, body + got, total - got);
        if (k <= 0) return ESP_FAIL;
        got += k;
    }
    body[got] = 0;

    net_alarms_t al;
    net_prov_alarms(&al);
    long v;
    for (int i = 0; i < ALARM_N; i++) {
        char key[4];
        alarm_t a = al.a[i];
        snprintf(key, sizeof key, "t%d", i);
        if (field_num(body, key, &v)) {
            if (v < 0 || v / 100 > 23 || v % 100 > 59) return refuse(r, "a time of 0000 to 2359");
            a.hour = (uint8_t)(v / 100);
            a.min  = (uint8_t)(v % 100);
        }
        snprintf(key, sizeof key, "d%d", i);
        if (field_num(body, key, &v)) {
            if (v < 0 || v > ALARM_DAILY) return refuse(r, "days of 0 to 127");
            a.days = (uint8_t)v;
        }
        snprintf(key, sizeof key, "o%d", i);
        if (field_num(body, key, &v)) a.on = v != 0;
        al.a[i] = a;
    }
    if (field_num(body, "snooze", &v)) {
        if (v < 1 || v > NET_ALARM_MIN_MAX) return refuse(r, "a snooze of 1 to 30 minutes");
        al.snooze_min = (uint8_t)v;
    }
    if (field_num(body, "ring", &v)) {
        if (v < 1 || v > NET_ALARM_MIN_MAX) return refuse(r, "a ring of 1 to 30 minutes");
        al.ring_min = (uint8_t)v;
    }
    if (field_num(body, "by", &v)) {
        if (v < 0 || v >= NET_ALARM_BYS) return refuse(r, "the motor, the sound, or both");
        al.by = (uint8_t)v;
    }
    if (field_num(body, "vol", &v)) {
        if (v < 0 || v > 100) return refuse(r, "a volume of 0 to 100");
        al.volume = (uint8_t)v;
    }

    net_prov_save_alarms(&al);
    clock_client_alarms_saved();
    int on = 0;
    for (int i = 0; i < ALARM_N; i++) on += al.a[i].on;
    static const char *const BY[NET_ALARM_BYS] = { "motor and sound", "motor", "sound" };
    ESP_LOGI(TAG, "saved: %d of %d on, snooze %u min, ring %u min, %s, volume %u", on, ALARM_N,
             (unsigned)al.snooze_min, (unsigned)al.ring_min, BY[al.by], (unsigned)al.volume);
    return httpd_resp_sendstr(r, "ok");
}

const httpd_uri_t clock_web_uris[] = {
    { .uri = "/api/alarms", .method = HTTP_GET,  .handler = alarms_get },
    { .uri = "/api/alarms", .method = HTTP_POST, .handler = alarms_post },
};
const size_t clock_web_uris_n = sizeof clock_web_uris / sizeof clock_web_uris[0];
