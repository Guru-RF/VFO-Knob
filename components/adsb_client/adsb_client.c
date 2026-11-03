/* The ADS-B Radar firmware's radio.h: there is no radio, but a feed of the
 * aircraft round the radar's middle. Its task asks the feed every Refresh
 * seconds -- at once when the dial changes the range -- reads the answer
 * without a tree (components/vfo_core adsb.c) and keeps the nearest
 * ADSB_KEEP for the face; between, it looks up the routes the face asks
 * for at adsbdb. One request at a time, on core 1, so its TLS never piles
 * up with another's on core 0 (the IDLE0 watchdog of v1.20.1). The feed's
 * connection is kept open between asks: a handshake costs the S3 a second
 * or two in software. */
#include "radio.h"
#include "adsb_client.h"
#include "net_prov.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_attr.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "adsb";

extern const uint8_t map_bin_start[]      asm("_binary_map_bin_start");
extern const uint8_t map_bin_end[]        asm("_binary_map_bin_end");
extern const uint8_t airports_bin_start[] asm("_binary_airports_bin_start");
extern const uint8_t airports_bin_end[]   asm("_binary_airports_bin_end");

#define BODY_FIRST   (128 * 1024)
#define BODY_MAX     (1536 * 1024)     /* 250 NM over Europe is about a megabyte */
#define ROUTES       16
#define WAKE_FEED    1u
#define WAKE_ROUTE   2u
#define STALE_US     (60LL * 1000 * 1000)
#define GIVE_UP_US   (30LL * 1000 * 1000)

enum { RT_EMPTY, RT_WANTED, RT_KNOWN, RT_UNKNOWN };
typedef struct {
    char    cs[9];
    char    from[24], to[24];
    uint8_t state;
} route_t;

typedef struct {
    char  *b;
    size_t len, cap;
    bool   over;
} body_t;

typedef struct {
    esp_http_client_handle_t h;
    bool fresh;                  /* the last get() opened the connection */
    char origin[96];
} conn_t;

static portMUX_TYPE      s_lock = portMUX_INITIALIZER_UNLOCKED;
static SemaphoreHandle_t s_snap_mx;
static TaskHandle_t      s_task;

/* The feed's answer read into one, handed over in the other. */
EXT_RAM_BSS_ATTR static adsb_ac_t s_parse[ADSB_KEEP];
EXT_RAM_BSS_ATTR static adsb_ac_t s_snap[ADSB_KEEP];
static adsb_snap_t s_hdr;

EXT_RAM_BSS_ATTR static route_t s_routes[ROUTES];
static uint8_t s_route_next;

static body_t s_body;
static conn_t s_feed, s_rt;
static char   s_ua[112];

/* The feed's state, for radio_get_status and the face's foot. */
static bool    s_no_pos, s_ever_ok;
static int64_t s_ok_us, s_fail_us;
static char    s_why[16];
static uint8_t s_src;
static uint8_t s_refresh_s = 10;

static esp_err_t on_http(esp_http_client_event_t *e)
{
    body_t *b = e->user_data;
    if (e->event_id != HTTP_EVENT_ON_DATA || b->over || e->data_len <= 0) return ESP_OK;
    const size_t want = b->len + (size_t)e->data_len + 1;
    if (want > b->cap) {
        size_t cap = b->cap ? b->cap : BODY_FIRST;
        while (cap < want) cap *= 2;
        char *nb = cap <= BODY_MAX ? heap_caps_realloc(b->b, cap, MALLOC_CAP_SPIRAM) : NULL;
        if (!nb) {
            b->over = true;
            return ESP_OK;
        }
        b->b = nb;
        b->cap = cap;
    }
    memcpy(b->b + b->len, e->data, (size_t)e->data_len);
    b->len += (size_t)e->data_len;
    b->b[b->len] = 0;
    return ESP_OK;
}

static void origin_of(const char *url, char *o, size_t n)
{
    const char *p = strstr(url, "://");
    p = p ? p + 3 : url;
    const char *e = strchr(p, '/');
    snprintf(o, n, "%.*s", (int)(e ? (size_t)(e - url) : strlen(url)), url);
}

/* One GET into s_body: 200, another status, or -1 with why. A connection
 * to the same scheme, host and port is reused while `keep`. */
static int get(conn_t *c, const char *url, bool keep, char *why, size_t wn)
{
    char o[96];
    origin_of(url, o, sizeof o);
    if (c->h && strcmp(o, c->origin)) {
        esp_http_client_cleanup(c->h);
        c->h = NULL;
    }
    if (!c->h) {
        const esp_http_client_config_t hc = {
            .url               = url,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .timeout_ms        = 15000,
            .keep_alive_enable = keep,
            .event_handler     = on_http,
            .user_data         = &s_body,
            .user_agent        = s_ua,
            .buffer_size       = 4096,
        };
        c->h = esp_http_client_init(&hc);
        if (!c->h) {
            snprintf(why, wn, "no memory");
            return -1;
        }
        strlcpy(c->origin, o, sizeof c->origin);
        c->fresh = true;
    } else {
        esp_http_client_set_url(c->h, url);
        c->fresh = false;
    }
    s_body.len = 0;
    s_body.over = false;
    if (s_body.b) s_body.b[0] = 0;
    const esp_err_t err = esp_http_client_perform(c->h);
    const int status = err == ESP_OK ? esp_http_client_get_status_code(c->h) : -1;
    if (err != ESP_OK || !keep) {
        esp_http_client_cleanup(c->h);
        c->h = NULL;
    }
    /* A kept connection the far end has since closed: asked again at once,
     * on a new one, rather than a failure shown until the next ask. */
    if (err != ESP_OK && !c->fresh) {
        ESP_LOGI(TAG, "%s: the kept connection closed (%s), asking on a new one", o, esp_err_to_name(err));
        return get(c, url, keep, why, wn);
    }
    if (err != ESP_OK) {
        snprintf(why, wn, "%s", err == ESP_ERR_HTTP_CONNECT ? "no answer"
                              : err == ESP_ERR_HTTP_EAGAIN  ? "timed out" : "cut off");
        ESP_LOGW(TAG, "%s: %s", o, esp_err_to_name(err));
        return -1;
    }
    if (status != 200) {
        snprintf(why, wn, "HTTP %d", status);
        return status;
    }
    if (s_body.over) {
        snprintf(why, wn, "too much");
        return -1;
    }
    return 200;
}

static void failed(const char *why)
{
    const int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_lock);
    const bool news = strcmp(s_why, why) != 0;
    const uint8_t src = s_src;
    if (!s_fail_us) s_fail_us = now;
    strlcpy(s_why, why, sizeof s_why);
    portEXIT_CRITICAL(&s_lock);
    /* Said once, not at each ask: what went wrong, and with which feed. */
    if (news) ESP_LOGW(TAG, "%s: %s", adsb_src_name(src), why);
}

static void publish(int n, int total)
{
    xSemaphoreTake(s_snap_mx, portMAX_DELAY);
    memcpy(s_snap, s_parse, (size_t)n * sizeof s_snap[0]);
    s_hdr.gen++;
    s_hdr.n = n;
    s_hdr.total = total;
    s_hdr.at_us = esp_timer_get_time();
    xSemaphoreGive(s_snap_mx);
    portENTER_CRITICAL(&s_lock);
    s_ever_ok = true;
    s_ok_us = s_hdr.at_us;
    s_fail_us = 0;
    s_why[0] = 0;
    portEXIT_CRITICAL(&s_lock);
}

/* The routes the face asked for, one by one, each on a connection of its
 * own and closed after: a second TLS session kept open beside the feed's
 * would hold internal RAM the whole time for a tap now and then. */
static void routes(void)
{
    for (int i = 0; i < ROUTES; i++) {
        char cs[9];
        portENTER_CRITICAL(&s_lock);
        const bool want = s_routes[i].state == RT_WANTED;
        memcpy(cs, s_routes[i].cs, sizeof cs);
        portEXIT_CRITICAL(&s_lock);
        if (!want) continue;
        char url[80], why[16], from[24] = "", to[24] = "";
        snprintf(url, sizeof url, "https://api.adsbdb.com/v0/callsign/%s", cs);
        const int64_t t0 = esp_timer_get_time();
        const int st = get(&s_rt, url, false, why, sizeof why);
        const bool known = st == 200 &&
                           adsb_route_parse(s_body.b, s_body.len, from, sizeof from, to, sizeof to);
        ESP_LOGI(TAG, "route %s: %s%s%s (%d ms)", cs, known ? from : st == 200 || st == 404 ? "unknown" : why,
                 known ? " -> " : "", known ? to : "", (int)((esp_timer_get_time() - t0) / 1000));
        portENTER_CRITICAL(&s_lock);
        if (!strcmp(s_routes[i].cs, cs)) {
            strlcpy(s_routes[i].from, from, sizeof s_routes[i].from);
            strlcpy(s_routes[i].to, to, sizeof s_routes[i].to);
            s_routes[i].state = known ? RT_KNOWN : RT_UNKNOWN;
        }
        portEXIT_CRITICAL(&s_lock);
    }
}

static void feed_task(void *arg)
{
    (void)arg;
    bool said_no_pos = false;
    int64_t said_at = 0;              /* the last feed line logged: one a minute */
    for (;;) {
        net_adsb_t c;
        net_prov_adsb(&c);
        portENTER_CRITICAL(&s_lock);
        s_src = c.source;
        s_refresh_s = c.refresh_s;
        s_no_pos = c.lat_e6 == NET_ADSB_NO_POS;
        portEXIT_CRITICAL(&s_lock);
        const int64_t t0 = esp_timer_get_time();
        int64_t wait_ms = (int64_t)c.refresh_s * 1000;

        if (c.lat_e6 == NET_ADSB_NO_POS) {
            if (!said_no_pos) ESP_LOGW(TAG, "no location: set the radar's middle on the configuration page");
            said_no_pos = true;
            wait_ms = 1000;
        } else if (!net_prov_is_connected()) {
            failed("no WiFi");
            wait_ms = 2000;
        } else {
            said_no_pos = false;
            const double lat = c.lat_e6 / 1e6, lon = c.lon_e6 / 1e6;
            const float nm = adsb_range_nm(c.units, c.range);
            char url[192], why[16];
            if (!adsb_feed_url(c.source, c.url, lat, lon, nm, url, sizeof url)) {
                failed("no address");
            } else if (get(&s_feed, url, true, why, sizeof why) != 200) {
                failed(why);
            } else {
                const int64_t t1 = esp_timer_get_time();
                int total = 0;
                const int n = adsb_parse(s_body.b, s_body.len, lat, lon, nm, s_parse, ADSB_KEEP, &total);
                if (n < 0) {
                    ESP_LOGW(TAG, "%s: %u bytes, not a feed", adsb_src_name(c.source), (unsigned)s_body.len);
                    failed("not a feed");
                } else {
                    publish(n, total);
                    /* The first answer, each new connection, and once a
                     * minute: what it costs. */
                    const int64_t t2 = esp_timer_get_time();
                    if (!said_at || s_feed.fresh || t2 - said_at > 60LL * 1000 * 1000) {
                        said_at = t2;
                        ESP_LOGI(TAG, "%s: %d of %d aircraft within %.0f NM, %u bytes in %d ms%s, parsed in %d ms;"
                                 " free internal %u",
                                 adsb_src_name(c.source), n, total, (double)nm, (unsigned)s_body.len,
                                 (int)((t1 - t0) / 1000), s_feed.fresh ? " (a new connection)" : "",
                                 (int)((t2 - t1) / 1000), (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
                    }
                }
            }
        }

        /* Until the next ask: a poke cuts it short, a route is looked up
         * between. */
        for (;;) {
            const int64_t left = wait_ms - (esp_timer_get_time() - t0) / 1000;
            uint32_t bits = 0;
            if (left > 0) xTaskNotifyWait(0, WAKE_FEED | WAKE_ROUTE, &bits, pdMS_TO_TICKS(left));
            if (bits & WAKE_ROUTE) routes();
            if (left <= 0 || (bits & WAKE_FEED)) break;
        }
    }
}

bool adsb_client_take(adsb_ac_t *out, int cap, adsb_snap_t *snap, uint32_t gen)
{
    if (!s_snap_mx) return false;
    bool got = false;
    xSemaphoreTake(s_snap_mx, portMAX_DELAY);
    if (s_hdr.gen != gen) {
        const int n = s_hdr.n < cap ? s_hdr.n : cap;
        memcpy(out, s_snap, (size_t)n * sizeof out[0]);
        *snap = s_hdr;
        snap->n = n;
        got = true;
    }
    xSemaphoreGive(s_snap_mx);
    return got;
}

void adsb_client_poke(void)
{
    if (s_task) xTaskNotify(s_task, WAKE_FEED, eSetBits);
}

bool adsb_client_route(const char *callsign, char *from, size_t fn, char *to, size_t tn, bool *pending)
{
    /* Only what a callsign can be: letters and digits, eight at most. */
    char cs[9];
    size_t n = 0;
    for (const char *p = callsign; p && *p && n < sizeof cs - 1; p++)
        if ((*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9')) cs[n++] = *p;
    cs[n] = 0;
    *pending = false;
    if (n < 3) return false;
    bool known = false, ask = false;
    portENTER_CRITICAL(&s_lock);
    int i = 0;
    while (i < ROUTES && strcmp(s_routes[i].cs, cs)) i++;
    if (i == ROUTES) {
        i = s_route_next;
        s_route_next = (uint8_t)((s_route_next + 1) % ROUTES);
        memset(&s_routes[i], 0, sizeof s_routes[i]);
        memcpy(s_routes[i].cs, cs, sizeof cs);
        s_routes[i].state = RT_WANTED;
        ask = true;
    }
    if (s_routes[i].state == RT_KNOWN) {
        strlcpy(from, s_routes[i].from, fn);
        strlcpy(to, s_routes[i].to, tn);
        known = true;
    }
    *pending = s_routes[i].state == RT_WANTED;
    portEXIT_CRITICAL(&s_lock);
    if (ask && s_task) xTaskNotify(s_task, WAKE_ROUTE, eSetBits);
    return known;
}

static bool fresh(int64_t now)
{
    const int64_t stale = (int64_t)s_refresh_s * 4 * 1000 * 1000;
    return s_ever_ok && now - s_ok_us < (stale > STALE_US ? stale : STALE_US);
}

void adsb_client_feed(char *out, size_t n, bool *ok)
{
    const int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_lock);
    const bool good = fresh(now) && !s_fail_us;
    const char *w = good ? adsb_src_name(s_src) : s_no_pos ? "no location" : s_why[0] ? s_why : "asking";
    strlcpy(out, w, n);
    portEXIT_CRITICAL(&s_lock);
    *ok = good;
}

void adsb_client_tables(const uint8_t **map, size_t *mn, const uint8_t **apt, size_t *an)
{
    *map = map_bin_start;
    *mn = (size_t)(map_bin_end - map_bin_start);
    *apt = airports_bin_start;
    *an = (size_t)(airports_bin_end - airports_bin_start);
}

/* --- radio.h ----------------------------------------------------------------- */

/* ------------------------------------------------------------- web page */

/* The radar's endpoints (adsb_web.c), which webcfg registers behind its
 * login in place of its weak default of none. */
extern const httpd_uri_t adsb_web_uris[];
extern const size_t      adsb_web_uris_n;

size_t radio_web_endpoints(const httpd_uri_t **out);
size_t radio_web_endpoints(const httpd_uri_t **out)
{
    *out = adsb_web_uris;
    return adsb_web_uris_n;
}

static atomic_bool s_saved;

void adsb_client_settings_saved(void)
{
    atomic_store(&s_saved, true);
    adsb_client_poke();
}

bool adsb_client_take_settings(void) { return atomic_exchange(&s_saved, false); }

/* ---------------------------------------------------------------- radio.h */

const char *radio_link_name(void) { return "ADS-B"; }

esp_err_t radio_start(const char *host, uint16_t port, const char *user, const char *pass)
{
    (void)host; (void)port; (void)user; (void)pass;
    if (s_task) return ESP_OK;
    if (!s_snap_mx && !(s_snap_mx = xSemaphoreCreateMutex())) return ESP_ERR_NO_MEM;
    const char *v = esp_app_get_description()->version;
    snprintf(s_ua, sizeof s_ua, "VFO-Knob-ADSB/%s (+github.com/Guru-RF/VFO-Knob)", *v == 'v' ? v + 1 : v);
    const uint8_t *m, *a;
    size_t mn, an;
    adsb_client_tables(&m, &mn, &a, &an);
    ESP_LOGI(TAG, "map %s (%u runs), %s airports (%u)", adsb_map_ok(m, mn) ? "whole" : "BROKEN",
             adsb_map_ok(m, mn) ? (unsigned)adsb_map_runs(m) : 0u, adsb_apt_ok(a, an) ? "whole" : "BROKEN",
             adsb_apt_ok(a, an) ? (unsigned)adsb_apt_count(a) : 0u);
    /* Its stack in PSRAM: it never writes flash (the page saves, app_main
     * keeps the range). On core 0, beside WiFi, and not the drawing's: a
     * TLS handshake is seconds of arithmetic, and on core 1 it took the
     * gaps the LVGL task leaves until the idle task starved into the
     * watchdog. */
    if (xTaskCreatePinnedToCoreWithCaps(feed_task, "adsb", 10240, NULL, 3, &s_task, 0,
                                        MALLOC_CAP_SPIRAM) != pdPASS) {
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void radio_get_status(radio_status_t *o)
{
    if (!o) return;
    memset(o, 0, sizeof *o);
    o->rx_only = true;
    const int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_lock);
    if (s_no_pos) {
        o->link = RADIO_LINK_DOWN;
        strlcpy(o->link_why, "NO LOCATION", sizeof o->link_why);
    } else if (fresh(now)) {
        o->link = RADIO_LINK_READY;
    } else if (!s_ever_ok && !(s_fail_us && now - s_fail_us > GIVE_UP_US)) {
        o->link = RADIO_LINK_CONNECTING;        /* the first answer on its way */
    } else {
        o->link = RADIO_LINK_DOWN;
        /* What went wrong, as the warning panel says it: in capitals. */
        const char *w = s_why[0] ? s_why : "no data";
        size_t i = 0;
        for (; w[i] && i < sizeof o->link_why - 1; i++)
            o->link_why[i] = (w[i] >= 'a' && w[i] <= 'z') ? (char)(w[i] - 32) : w[i];
        o->link_why[i] = 0;
    }
    portEXIT_CRITICAL(&s_lock);
}

int64_t radio_tune_by(int32_t detents, uint8_t accel_mult, int32_t step_hz)
{
    (void)detents; (void)accel_mult; (void)step_hz;
    return 0;
}

bool radio_is_ready(void) { return false; }
bool radio_on_air(void)   { return false; }

void radio_set_step(int32_t step_hz)               { (void)step_hz; }
void radio_audio_suspend(bool suspend)             { (void)suspend; }
void radio_ptt_key(void)                           {}
void radio_ptt_unkey(void)                         {}
void radio_ptt_toggle(void)                        {}
void radio_ptt_force_abort(uint8_t reason)         { (void)reason; }
void radio_set_mode(const char *mode)              { (void)mode; }
void radio_set_filter(int32_t lo, int32_t hi)      { (void)lo; (void)hi; }
void radio_select_filter(uint8_t n)                { (void)n; }
void radio_set_rit(int32_t hz)                     { (void)hz; }
void radio_set_agc(const char *agc)                { (void)agc; }
void radio_set_gain(int8_t gain)                   { (void)gain; }
void radio_goto_freq(int64_t hz)                   { (void)hz; }
void radio_memory_mode(bool on)                    { (void)on; }
void radio_memory_group(uint8_t group)             { (void)group; }
void radio_select_rx(uint8_t rx)                   { (void)rx; }
void radio_set_antenna(uint8_t ant, bool rx_ant)   { (void)ant; (void)rx_ant; }
void radio_tune(void)                              {}
void radio_atu_tune(void)                          {}
void radio_atu_memories(bool on)                   { (void)on; }
void radio_set_dsp(uint8_t which, bool on)         { (void)which; (void)on; }
void radio_set_rf_gain(uint8_t pct)                { (void)pct; }
void radio_set_rf_power(uint8_t pct)               { (void)pct; }
void radio_set_tuner(bool on)                      { (void)on; }
void radio_set_squelch(uint8_t pct)                { (void)pct; }
void radio_tg_lock(bool locked)                    { (void)locked; }
void radio_mute(bool muted)                        { (void)muted; }

bool radio_get_choice(uint8_t i, char *title, size_t tn, char *name, size_t nn)
{
    (void)i; (void)title; (void)tn; (void)name; (void)nn;
    return false;
}
void radio_choose(uint8_t i) { (void)i; }
