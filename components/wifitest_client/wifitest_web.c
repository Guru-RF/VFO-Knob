/* The WiFi Test on the configuration page, over HTTP. Registered by webcfg,
 * behind its login (radio_web_endpoints() in wifitest_client.c).
 *
 *   GET  /api/wifitest          all measured, as JSON, with the hosts pinged
 *                               (the list's) and those the other firmwares
 *                               have, to choose from
 *   GET  /api/wifitest/report   the same as text, to paste into a report
 *   POST /api/wifitest          form-encoded: act=scan, the channels
 *                               scanned now; act=recount, the counts begun
 *                               again; act=hosts with h0..h2, the hosts
 *                               pinged beside the router ("" none)
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"

#include "net_prov.h"
#include "wifitest_client.h"

static const char *TAG = "wifitest_web";

#define JSON_CAP   (10 * 1024)
#define REPORT_CAP (8 * 1024)

typedef struct {
    char  *b;
    size_t n, o;
} out_t;

static void put(out_t *j, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void put(out_t *j, const char *fmt, ...)
{
    if (j->o >= j->n) return;
    va_list ap;
    va_start(ap, fmt);
    const int k = vsnprintf(j->b + j->o, j->n - j->o, fmt, ap);
    va_end(ap);
    if (k > 0) j->o += (size_t)k;
}

/* A string, quoted and escaped: an SSID may hold anything. */
static void str(out_t *j, const char *s)
{
    put(j, "\"");
    for (; *s && j->o < j->n; s++) {
        const unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') put(j, "\\%c", c);
        else if (c < 0x20) put(j, "\\u%04x", c);
        else put(j, "%c", c);
    }
    put(j, "\"");
}

static void bssid(out_t *j, const uint8_t *b)
{
    put(j, "\"%02x:%02x:%02x:%02x:%02x:%02x\"", b[0], b[1], b[2], b[3], b[4], b[5]);
}

static esp_err_t wt_get(httpd_req_t *r)
{
    wt_t  *w = heap_caps_malloc(sizeof *w, MALLOC_CAP_SPIRAM);
    char  *b = heap_caps_malloc(JSON_CAP, MALLOC_CAP_SPIRAM);
    if (!w || !b) {
        free(w);
        free(b);
        return httpd_resp_send_500(r);
    }
    wifitest_snapshot(w);
    out_t j = { b, JSON_CAP, 0 };
    char why[48];
    const int g = wt_verdict(w, why, sizeof why);
    put(&j, "{\"up\":%u,\"joined\":%s,\"ssid\":", (unsigned)w->up_s, w->joined ? "true" : "false");
    str(&j, w->ssid);
    put(&j, ",\"bssid\":");
    bssid(&j, w->bssid);
    put(&j, ",\"ch\":%u,\"ht40\":%u,\"phy\":\"%s\",\"auth\":\"%s\",\"cc\":\"%s\"", (unsigned)w->channel,
        (unsigned)w->ht40, w->phy, w->auth, w->country);
    put(&j, ",\"rssi\":%d,\"recent\":%d,\"rmin\":%d,\"rmax\":%d,\"rmean\":%d,\"word\":\"%s\"", w->rssi,
        wt_rssi_recent(w), w->rssi_min, w->rssi_max,
        w->rssi_n ? (int)((w->rssi_sum - (int32_t)w->rssi_n / 2) / (int32_t)w->rssi_n) : 0,
        wt_signal_word(w->rssi));
    put(&j, ",\"rh\":[");
    for (int i = 0; i < WT_HIST; i++) put(&j, "%s%d", i ? "," : "", w->rssi_hist[i]);
    put(&j, "],\"ip\":\"%s\",\"gw\":\"%s\",\"dns\":\"%s\"", w->ip, w->gw, w->dns);
    put(&j, ",\"join\":%u,\"down\":%u,\"drops\":%u,\"roams\":%u,\"beacons\":%u,\"ipch\":%u,\"silences\":%u",
        (unsigned)w->join_s, (unsigned)w->down_s, (unsigned)w->drops, (unsigned)w->roams, (unsigned)w->beacons,
        (unsigned)w->ip_changes, (unsigned)w->silences);
    put(&j, ",\"reason\":%u,\"reason_text\":", (unsigned)w->last_reason);
    str(&j, w->drops ? wt_reason_text(w->last_reason) : "");
    put(&j, ",\"hint\":");
    str(&j, w->drops ? wt_reason_hint(w->last_reason) : "");
    put(&j, ",\"grade\":%d,\"why\":", g);
    str(&j, why);
    put(&j, ",\"p\":[");
    for (int i = 0; i < WT_TARGETS; i++) {
        const wt_ping_t *p = &w->p[i];
        put(&j, "%s{\"name\":", i ? "," : "");
        str(&j, p->name);
        put(&j, ",\"addr\":\"%s\",\"state\":%u,\"sent\":%u,\"got\":%u,\"late\":%u,\"loss\":%u,\"mean\":%u,"
                "\"min\":%u,\"max\":%u,\"last\":%u,\"gap\":%u,\"worst\":%u,\"h\":[",
            p->addr, (unsigned)p->state, (unsigned)p->sent, (unsigned)p->got, (unsigned)p->late, wt_loss_pm(p),
            wt_mean_ms(p), (unsigned)p->min_ms, (unsigned)p->max_ms,
            p->last_ms == WT_LOST ? 0u : (unsigned)p->last_ms, (unsigned)p->gap_ms, (unsigned)p->worst_gap_ms);
        for (int k = 0; k < WT_HIST; k++) put(&j, "%s%u", k ? "," : "", (unsigned)p->hist[k]);
        put(&j, "]}");
    }
    put(&j, "],\"dns_ok\":%u,\"dns_fail\":%u,\"dns_ms\":%u,\"mdns_ok\":%u,\"mdns_fail\":%u", (unsigned)w->dns_ok,
        (unsigned)w->dns_fail, (unsigned)w->dns_ms, (unsigned)w->mdns_ok, (unsigned)w->mdns_fail);
    put(&j, ",\"ev\":[");
    for (int i = 0; i < w->n_ev; i++) {
        const wt_event_t *e = &w->ev[i];
        put(&j, "%s{\"t\":%lld,\"at\":%u,\"kind\":%u,\"what\":", i ? "," : "", (long long)e->unix_s,
            (unsigned)e->at_s, (unsigned)e->kind);
        str(&j, e->what);
        put(&j, "}");
    }
    put(&j, "],\"scanning\":%s,\"scan_at\":%u,\"heard\":%u,\"per\":[", w->scanning ? "true" : "false",
        (unsigned)w->scan_at_s, (unsigned)w->n_heard);
    for (int c = 1; c <= WT_CHANNELS; c++) put(&j, "%s%u", c > 1 ? "," : "", (unsigned)w->per_ch[c]);
    put(&j, "],\"ap\":[");
    for (int i = 0; i < w->n_ap; i++) {
        const wt_ap_t *a = &w->ap[i];
        put(&j, "%s{\"ssid\":", i ? "," : "");
        str(&j, a->ssid);
        put(&j, ",\"bssid\":");
        bssid(&j, a->bssid);
        put(&j, ",\"ch\":%u,\"rssi\":%d,\"auth\":%u,\"ours\":%s}", (unsigned)a->channel, a->rssi, (unsigned)a->auth,
            a->ours ? "true" : "false");
    }
    /* The list pinged, and what the other firmwares have, to choose from. */
    put(&j, "],\"hosts\":[");
    int nh = 0;
    for (int i = 0; i < net_prov_radio_count(); i++) {
        net_radio_t h;
        if (!net_prov_radio_get(i, &h) || !h.host[0]) continue;
        put(&j, "%s", nh++ ? "," : "");
        str(&j, h.host);
    }
    put(&j, "],\"elsewhere\":[");
    net_radio_t el[6];
    const int ne = net_prov_hosts_elsewhere(el, 6);
    for (int i = 0; i < ne; i++) {
        put(&j, "%s{\"name\":", i ? "," : "");
        str(&j, el[i].name);
        put(&j, ",\"host\":");
        str(&j, el[i].host);
        put(&j, "}");
    }
    put(&j, "]}");
    free(w);
    if (j.o >= j.n) {
        free(b);
        ESP_LOGE(TAG, "the JSON outgrew %u bytes", (unsigned)JSON_CAP);
        return httpd_resp_send_500(r);
    }
    httpd_resp_set_type(r, "application/json");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    const esp_err_t e = httpd_resp_send(r, b, (ssize_t)j.o);
    free(b);
    return e;
}

static esp_err_t wt_report(httpd_req_t *r)
{
    char *b = heap_caps_malloc(REPORT_CAP, MALLOC_CAP_SPIRAM);
    if (!b) return httpd_resp_send_500(r);
    const size_t n = wifitest_report(b, REPORT_CAP);
    httpd_resp_set_type(r, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    const esp_err_t e = httpd_resp_send(r, b, (ssize_t)n);
    free(b);
    return e;
}

static esp_err_t refuse(httpd_req_t *r, const char *why)
{
    httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, why);
    return ESP_FAIL;
}

/* A host as given: a name or an address, nothing that would break the list. */
static bool host_ok(const char *h)
{
    for (; *h; h++)
        if (*h <= ' ' || *h == '/' || *h == '\\' || *h == '"' || *h > '~') return false;
    return true;
}

static esp_err_t wt_post(httpd_req_t *r)
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
    char act[16] = "";
    httpd_query_key_value(body, "act", act, sizeof act);
    if (!strcmp(act, "scan")) {
        wifitest_scan();
        ESP_LOGI(TAG, "the page asks for a scan");
        return httpd_resp_sendstr(r, "ok");
    }
    if (!strcmp(act, "recount")) {
        wifitest_recount();
        ESP_LOGI(TAG, "the page begins the counts again");
        return httpd_resp_sendstr(r, "ok");
    }
    if (!strcmp(act, "hosts")) {
        net_radio_t list[WT_HOSTS];
        int n = 0;
        for (int i = 0; i < WT_HOSTS; i++) {
            char key[4], h[64] = "";
            snprintf(key, sizeof key, "h%d", i);
            if (httpd_query_key_value(body, key, h, sizeof h) != ESP_OK || !h[0]) continue;
            if (!host_ok(h)) return refuse(r, "a host is a name or an address, no spaces");
            memset(&list[n], 0, sizeof list[n]);
            strlcpy(list[n].host, h, sizeof list[n].host);
            n++;
        }
        /* None: the list emptied -- the router and the internet alone. */
        if ((n ? net_prov_radios_save(list, n, 0) : net_prov_radios_clear()) != ESP_OK)
            return refuse(r, "the hosts were not saved");
        wifitest_hosts_changed();
        ESP_LOGI(TAG, "hosts saved: %s%s%s%s%s", n ? list[0].host : "none", n > 1 ? ", " : "",
                 n > 1 ? list[1].host : "", n > 2 ? ", " : "", n > 2 ? list[2].host : "");
        return httpd_resp_sendstr(r, "ok");
    }
    return refuse(r, "act: scan, recount or hosts");
}

const httpd_uri_t wifitest_web_uris[] = {
    { .uri = "/api/wifitest",        .method = HTTP_GET,  .handler = wt_get },
    { .uri = "/api/wifitest/report", .method = HTTP_GET,  .handler = wt_report },
    { .uri = "/api/wifitest",        .method = HTTP_POST, .handler = wt_post },
};
const size_t wifitest_web_uris_n = sizeof wifitest_web_uris / sizeof wifitest_web_uris[0];
