/* The AIS Radar firmware's radio.h: there is no radio, but the ships round
 * the radar's middle. See ais_client.h.
 *
 * AISStream.io: one WebSocket kept open, the subscription a box round the
 * radar's circle -- a little wider than its range, and never under
 * BOX_MIN_NM, so that a turn of the dial out finds the ships near already
 * there -- sent again when the dial or the page moves it. Each message is
 * one ship's position or its static data, laid over what is known of it by
 * its MMSI; a ship not heard for STALE_S is let go. A connection that ends
 * is opened again after a wait that grows; one that ends before the
 * subscription is confirmed was most likely refused its key, and waits
 * longest.
 *
 * An AIS-catcher: its ship list asked every Refresh seconds, on a kept
 * connection, as ADS-B Radar asks a feed. */
#include "radio.h"
#include "adsb.h"                  /* the distance, the map's check */
#include "ais_client.h"
#include "ais_ws.h"
#include "net_prov.h"

#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
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

static const char *TAG = "ais";

extern const uint8_t map_bin_start[]   asm("_binary_map_bin_start");
extern const uint8_t map_bin_end[]     asm("_binary_map_bin_end");
extern const uint8_t ports_bin_start[] asm("_binary_ports_bin_start");
extern const uint8_t ports_bin_end[]   asm("_binary_ports_bin_end");

#define STREAM_URL   "wss://stream.aisstream.io/v0/stream"
#define TABLE        700                 /* ships known: the box is wider than the circle */
#define STALE_S      600                 /* a ship not heard so long let go */
#define PUBLISH_MS   2000
#define BOX_MIN_NM   10.0f
#define BOX_WIDER    1.25f
#define PING_S       25                  /* nothing heard so long: a ping */
#define DEAD_S       90                  /* ...and so long: the connection is gone */
#define CONFIRM_S    10                  /* the subscription unconfirmed so long: closed and tried again */
#define BODY_FIRST   (128 * 1024)
#define BODY_MAX     (1024 * 1024)
#define FRESH_US     (60LL * 1000 * 1000)
#define GIVE_UP_US   (30LL * 1000 * 1000)

typedef struct {
    ais_ship_t s;
    int64_t    pos_us;          /* its position came: 0 never */
    int64_t    heard_us;        /* anything of it came */
} entry_t;

typedef struct {
    char  *b;
    size_t len, cap;
    bool   over;
} body_t;

static portMUX_TYPE      s_lock = portMUX_INITIALIZER_UNLOCKED;
static SemaphoreHandle_t s_snap_mx;
static TaskHandle_t      s_task;
static atomic_uint       s_gen;            /* the settings or the dial moved */

/* The task's own: the ships known, and the ones handed over. */
EXT_RAM_BSS_ATTR static entry_t    s_tab[TABLE];
static int                         s_tab_n;
EXT_RAM_BSS_ATTR static ais_ship_t s_work[TABLE];
EXT_RAM_BSS_ATTR static ais_ship_t s_snap[AIS_KEEP];
static ais_snap_t                  s_hdr;

static body_t s_body;
static esp_http_client_handle_t s_http;
static char   s_http_origin[96];
static char   s_ua[112];

/* The source's state, for radio_get_status, the face's foot and the page. */
static bool     s_no_pos, s_no_key, s_ever_ok;
static int64_t  s_ok_us, s_fail_us;
static char     s_why[24];
static uint8_t  s_src;
static uint32_t s_min_msgs, s_last_min_msgs;
static int64_t  s_min_at;

/* --------------------------------------------------------------- state */

static void failed(const char *why)
{
    const int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_lock);
    const bool news = strcmp(s_why, why) != 0;
    const uint8_t src = s_src;
    if (!s_fail_us) s_fail_us = now;
    strlcpy(s_why, why, sizeof s_why);
    portEXIT_CRITICAL(&s_lock);
    if (news) ESP_LOGW(TAG, "%s: %s", ais_src_name(src), why);
}

static void heard(void)
{
    const int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_lock);
    s_ever_ok = true;
    s_ok_us = now;
    s_fail_us = 0;
    s_why[0] = 0;
    s_min_msgs++;
    if (now - s_min_at >= 60LL * 1000 * 1000) {
        s_last_min_msgs = s_min_msgs;
        s_min_msgs = 0;
        s_min_at = now;
    }
    portEXIT_CRITICAL(&s_lock);
}

static void publish(const ais_ship_t *ships, int n, int total)
{
    if (n > AIS_KEEP) n = AIS_KEEP;
    xSemaphoreTake(s_snap_mx, portMAX_DELAY);
    memcpy(s_snap, ships, (size_t)n * sizeof s_snap[0]);
    s_hdr.gen++;
    s_hdr.n = n;
    s_hdr.total = total;
    s_hdr.at_us = esp_timer_get_time();
    xSemaphoreGive(s_snap_mx);
}

/* --------------------------------------------------------------- the table */

static void forget_all(void) { s_tab_n = 0; }

static float dist_of(const ais_ship_t *s, double lat, double lon)
{
    float d;
    adsb_dist_brg(lat, lon, s->lat, s->lon, &d, NULL);
    return d;
}

/* The entry for this MMSI, made when there is room -- or, full, in the
 * place of one long silent without a position, or of the farthest when the
 * update is nearer. NULL: it is let go. */
static entry_t *entry(const ais_ship_t *u, uint32_t f, double lat, double lon, int64_t now)
{
    for (int i = 0; i < s_tab_n; i++)
        if (s_tab[i].s.mmsi == u->mmsi) return &s_tab[i];
    int at = -1;
    if (s_tab_n < TABLE) {
        at = s_tab_n++;
    } else {
        float far = -1;
        int far_i = -1;
        for (int i = 0; i < TABLE && at < 0; i++) {
            const entry_t *e = &s_tab[i];
            if (!e->s.pos) {
                if (now - e->heard_us > 120LL * 1000 * 1000) at = i;
                continue;
            }
            const float d = dist_of(&e->s, lat, lon);
            if (d > far) { far = d; far_i = i; }
        }
        if (at < 0 && (f & AIS_F_POS) && far_i >= 0 && dist_of(u, lat, lon) < far) at = far_i;
        if (at < 0) return NULL;
    }
    ais_ship_init(&s_tab[at].s, u->mmsi);
    s_tab[at].pos_us = s_tab[at].heard_us = 0;
    return &s_tab[at];
}

static void expire(int64_t now)
{
    int k = 0;
    for (int i = 0; i < s_tab_n; i++)
        if (now - s_tab[i].heard_us < (int64_t)STALE_S * 1000000) s_tab[k++] = s_tab[i];
    s_tab_n = k;
}

static int by_dist(const void *l, const void *r)
{
    const float a = ((const ais_ship_t *)l)->dist_nm, b = ((const ais_ship_t *)r)->dist_nm;
    return (a > b) - (a < b);
}

/* The ships with a position within `nm` of the middle, nearest first, to
 * the face. */
static void hand_over(double lat, double lon, float nm, int64_t now)
{
    int n = 0, known = 0;
    for (int i = 0; i < s_tab_n; i++) {
        const entry_t *e = &s_tab[i];
        if (!e->s.pos) continue;
        known++;
        ais_ship_t s = e->s;
        adsb_dist_brg(lat, lon, s.lat, s.lon, &s.dist_nm, &s.brg);
        if (s.dist_nm > nm) continue;
        s.seen_s = (float)((now - e->pos_us) / 1000) / 1000.0f;
        s_work[n++] = s;
    }
    qsort(s_work, (size_t)n, sizeof s_work[0], by_dist);
    publish(s_work, n, known);
}

/* --------------------------------------------------------------- AISStream */

typedef struct { unsigned gen; } want_t;

static bool still_wanted(void *ctx) { return atomic_load(&s_gen) == ((want_t *)ctx)->gen; }

static bool subscribe(ais_ws_t *w, const net_ais_t *c)
{
    char sub[640];
    const float nm = fmaxf(ais_range_nm(c->units, c->range) * BOX_WIDER, BOX_MIN_NM);
    if (!ais_stream_sub(c->key, c->lat_e6 / 1e6, c->lon_e6 / 1e6, nm, sub, sizeof sub)) return false;
    return ais_ws_send_text(w, sub, strlen(sub));
}

/* One connection to the stream, until it ends or the source or the key
 * changes: how long to wait before the next, in seconds. */
static int stream(net_ais_t *c, int fails)
{
    want_t want = { atomic_load(&s_gen) };
    char why[48];
    const int64_t t0 = esp_timer_get_time();
    ais_ws_t *w = ais_ws_open(STREAM_URL, true, s_ua, still_wanted, &want, why, sizeof why);
    if (!w) {
        if (!strcmp(why, "stopped")) return 0;
        failed(why);
        return fails < 3 ? 5 : fails < 6 ? 20 : 60;
    }
    ESP_LOGI(TAG, "AISStream: connected in %d ms%s", (int)((esp_timer_get_time() - t0) / 1000),
             ais_ws_deflated(w) ? ", compressed" : "");
    if (!subscribe(w, c)) {
        ais_ws_close(w);
        failed("not subscribed");
        return 20;
    }
    int64_t sub_at = esp_timer_get_time(), last_pub = 0, last_ping = 0, said_at = sub_at;
    bool confirmed = false, resub = false;
    uint32_t msgs = 0, bad = 0;
    int wait_s = 0;
    double lat = c->lat_e6 / 1e6, lon = c->lon_e6 / 1e6;
    float nm = ais_range_nm(c->units, c->range);
    for (;;) {
        const char *m;
        size_t n;
        const int r = ais_ws_recv(w, 200, &m, &n, why, sizeof why);
        const int64_t now = esp_timer_get_time();
        if (r == AIS_WS_CLOSED || r == AIS_WS_FAILED) {
            /* Closed before the subscription was confirmed: the key, most
             * likely -- AISStream closes on one it refuses, saying nothing. */
            if (!confirmed) {
                failed("key refused");
                wait_s = 120;
            } else {
                failed(r == AIS_WS_CLOSED ? "closed" : "cut off");
                wait_s = 5;
            }
            ESP_LOGW(TAG, "AISStream: %s after %d s, %u messages", why, (int)((now - t0) / 1000000),
                     (unsigned)msgs);
            break;
        }
        if (r == AIS_WS_MSG) {
            ais_ship_t u;
            uint32_t f;
            bool comp = false;
            char err[48];
            const int k = ais_stream_parse(m, n, &u, &f, &comp, err, sizeof err);
            if (k == AIS_MSG_CONFIRM) {
                confirmed = true;
                heard();
                ESP_LOGI(TAG, "AISStream: subscribed%s", comp ? ", compressed" : "");
            } else if (k == AIS_MSG_ERROR) {
                ESP_LOGW(TAG, "AISStream: \"%s\"", err);
                failed(strstr(err, "Key") || strstr(err, "key") ? "key refused" : "refused");
                wait_s = 120;
                break;
            } else if (k == AIS_MSG_SHIP) {
                msgs++;
                heard();
                entry_t *e = entry(&u, f, lat, lon, now);
                if (e) {
                    ais_merge(&e->s, &u, f);
                    e->heard_us = now;
                    if (f & AIS_F_POS) e->pos_us = now;
                }
            } else if (k == AIS_MSG_BAD && bad++ < 3) {
                ESP_LOGW(TAG, "AISStream: not a message (%u bytes): %.60s", (unsigned)n, m);
            }
        }
        if (!confirmed && now - sub_at > CONFIRM_S * 1000000LL) {
            failed("not confirmed");
            wait_s = 20;
            break;
        }
        const int64_t quiet = now - ais_ws_heard_us(w);
        if (quiet > DEAD_S * 1000000LL) {
            failed("no data");
            wait_s = 5;
            break;
        }
        if (quiet > PING_S * 1000000LL && now - last_ping > PING_S * 1000000LL) {
            last_ping = now;
            ais_ws_ping(w);
        }
        /* The settings or the dial: a new source or key, a new connection;
         * a new middle or range, a new box -- not more than once a second. */
        const unsigned g = atomic_load(&s_gen);
        if (g != want.gen) {
            want.gen = g;
            net_ais_t nc;
            net_prov_ais(&nc);
            if (nc.source != c->source || strcmp(nc.key, c->key)) {
                *c = nc;
                break;
            }
            const bool moved = nc.lat_e6 != c->lat_e6 || nc.lon_e6 != c->lon_e6;
            if (moved) forget_all();
            if (moved || nc.range != c->range || nc.units != c->units) resub = true;
            *c = nc;
            lat = c->lat_e6 / 1e6;
            lon = c->lon_e6 / 1e6;
            nm = ais_range_nm(c->units, c->range);
            last_pub = 0;
        }
        if (resub && now - sub_at > 1000000) {
            resub = false;
            sub_at = now;
            if (!subscribe(w, c)) {
                failed("cut off");
                wait_s = 5;
                break;
            }
        }
        if (now - last_pub >= PUBLISH_MS * 1000LL) {
            last_pub = now;
            expire(now);
            hand_over(lat, lon, nm, now);
        }
        if (now - said_at > 60LL * 1000 * 1000) {
            said_at = now;
            uint64_t wire, plain;
            uint32_t dropped;
            ais_ws_counts(w, &wire, &plain, &dropped);
            ESP_LOGI(TAG, "AISStream: %u messages, %d ships known, %d within %.0f NM; %llu kB on the wire, "
                     "%llu kB inflated, %u let go; free internal %u", (unsigned)msgs, s_tab_n, s_hdr.n,
                     (double)nm, (unsigned long long)(wire / 1024), (unsigned long long)(plain / 1024),
                     (unsigned)dropped, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        }
    }
    ais_ws_close(w);
    return wait_s;
}

/* --------------------------------------------------------------- AIS-catcher */

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

/* One GET of the ship list into s_body, on a connection kept to the same
 * origin: 200, another status, or -1 and why. */
static int get(const char *url, char *why, size_t wn, bool again)
{
    char o[96];
    const char *p = strstr(url, "://");
    p = p ? p + 3 : url;
    const char *e = strchr(p, '/');
    snprintf(o, sizeof o, "%.*s", (int)(e ? (size_t)(e - url) : strlen(url)), url);
    if (s_http && strcmp(o, s_http_origin)) {
        esp_http_client_cleanup(s_http);
        s_http = NULL;
    }
    const bool fresh = !s_http;
    if (!s_http) {
        const esp_http_client_config_t hc = {
            .url               = url,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .timeout_ms        = 10000,
            .keep_alive_enable = true,
            .event_handler     = on_http,
            .user_data         = &s_body,
            .user_agent        = s_ua,
            .buffer_size       = 4096,
        };
        s_http = esp_http_client_init(&hc);
        if (!s_http) {
            snprintf(why, wn, "no memory");
            return -1;
        }
        strlcpy(s_http_origin, o, sizeof s_http_origin);
    } else {
        esp_http_client_set_url(s_http, url);
    }
    s_body.len = 0;
    s_body.over = false;
    if (s_body.b) s_body.b[0] = 0;
    const esp_err_t err = esp_http_client_perform(s_http);
    const int status = err == ESP_OK ? esp_http_client_get_status_code(s_http) : -1;
    if (err != ESP_OK) {
        esp_http_client_cleanup(s_http);
        s_http = NULL;
        if (!fresh && !again) return get(url, why, wn, true);     /* the kept one had closed */
        snprintf(why, wn, "%s", err == ESP_ERR_HTTP_CONNECT ? "no answer"
                              : err == ESP_ERR_HTTP_EAGAIN  ? "timed out" : "cut off");
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

static void catcher(const net_ais_t *c)
{
    char url[200], why[24];
    if (!ais_catcher_url(c->url, url, sizeof url)) {
        failed("no address");
        return;
    }
    const int64_t t0 = esp_timer_get_time();
    if (get(url, why, sizeof why, false) != 200) {
        failed(why);
        return;
    }
    const float nm = ais_range_nm(c->units, c->range);
    int total = 0;
    const int n = ais_catcher_parse(s_body.b, s_body.len, c->lat_e6 / 1e6, c->lon_e6 / 1e6, nm, s_work, TABLE,
                                    &total);
    if (n < 0) {
        ESP_LOGW(TAG, "AIS-catcher: %u bytes, not a ship list", (unsigned)s_body.len);
        failed("not a ship list");
        return;
    }
    heard();
    publish(s_work, n, total);
    static int64_t said_at;
    const int64_t t1 = esp_timer_get_time();
    if (!said_at || t1 - said_at > 60LL * 1000 * 1000) {
        said_at = t1;
        ESP_LOGI(TAG, "AIS-catcher: %d of %d ships within %.0f NM, %u bytes in %d ms", n, total, (double)nm,
                 (unsigned)s_body.len, (int)((t1 - t0) / 1000));
    }
}

/* --------------------------------------------------------------- the task */

static void wait_or_poke(int64_t ms)
{
    const unsigned g = atomic_load(&s_gen);
    for (int64_t left = ms; left > 0 && atomic_load(&s_gen) == g; left -= 200)
        vTaskDelay(pdMS_TO_TICKS(left < 200 ? left : 200));
}

static void ais_task(void *arg)
{
    (void)arg;
    bool said_no_pos = false;
    int fails = 0;
    for (;;) {
        net_ais_t c;
        net_prov_ais(&c);
        portENTER_CRITICAL(&s_lock);
        const bool switched = s_src != c.source;
        if (switched) { s_ever_ok = false; s_why[0] = 0; s_fail_us = 0; }
        s_src = c.source;
        s_no_pos = c.lat_e6 == NET_AIS_NO_POS;
        s_no_key = c.source == AIS_SRC_AISSTREAM && !c.key[0];
        portEXIT_CRITICAL(&s_lock);
        if (switched) forget_all();

        if (c.lat_e6 == NET_AIS_NO_POS) {
            if (!said_no_pos) ESP_LOGW(TAG, "no location: set the radar's middle on the configuration page");
            said_no_pos = true;
            wait_or_poke(1000);
            continue;
        }
        said_no_pos = false;
        if (!net_prov_is_connected()) {
            failed("no WiFi");
            wait_or_poke(2000);
            continue;
        }
        if (c.source == AIS_SRC_AISSTREAM) {
            if (s_http) {                         /* the AIS-catcher's connection, no longer wanted */
                esp_http_client_cleanup(s_http);
                s_http = NULL;
            }
            if (!c.key[0]) {
                failed("no key");
                forget_all();
                if (s_hdr.n) publish(s_work, 0, 0);
                wait_or_poke(2000);
                continue;
            }
            const int64_t t0 = esp_timer_get_time();
            const int wait_s = stream(&c, fails);
            /* A connection that lasted a while: the waits start over. */
            fails = esp_timer_get_time() - t0 > 120LL * 1000 * 1000 ? 0 : fails + 1;
            if (wait_s) wait_or_poke((int64_t)wait_s * 1000);
        } else {
            const int64_t t0 = esp_timer_get_time();
            catcher(&c);
            const int64_t spent = (esp_timer_get_time() - t0) / 1000;
            wait_or_poke((int64_t)c.refresh_s * 1000 - spent);
        }
    }
}

/* --------------------------------------------------------------- the API */

bool ais_client_take(ais_ship_t *out, int cap, ais_snap_t *snap, uint32_t gen)
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

void ais_client_poke(void) { atomic_fetch_add(&s_gen, 1); }

static bool fresh(int64_t now) { return s_ever_ok && now - s_ok_us < FRESH_US; }

void ais_client_feed(char *out, size_t n, bool *ok)
{
    const int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_lock);
    const bool good = fresh(now) && !s_fail_us;
    const char *w = s_no_pos ? "no location" : s_no_key ? "no key" : good ? ais_src_name(s_src)
                  : s_why[0] ? s_why : "connecting";
    strlcpy(out, good && s_src == AIS_SRC_CATCHER ? "AIS-catcher" : w, n);
    portEXIT_CRITICAL(&s_lock);
    *ok = good;
}

void ais_client_counts(uint32_t *per_min, int *known)
{
    portENTER_CRITICAL(&s_lock);
    *per_min = s_last_min_msgs ? s_last_min_msgs : s_min_msgs;
    portEXIT_CRITICAL(&s_lock);
    *known = s_hdr.total;
}

void ais_client_tables(const uint8_t **map, size_t *mn, const uint8_t **ports, size_t *pn)
{
    *map = map_bin_start;
    *mn = (size_t)(map_bin_end - map_bin_start);
    *ports = ports_bin_start;
    *pn = (size_t)(ports_bin_end - ports_bin_start);
}

/* ------------------------------------------------------------- web page */

extern const httpd_uri_t ais_web_uris[];
extern const size_t      ais_web_uris_n;

size_t radio_web_endpoints(const httpd_uri_t **out);
size_t radio_web_endpoints(const httpd_uri_t **out)
{
    *out = ais_web_uris;
    return ais_web_uris_n;
}

static atomic_bool s_saved;

void ais_client_settings_saved(void)
{
    atomic_store(&s_saved, true);
    ais_client_poke();
}

bool ais_client_take_settings(void) { return atomic_exchange(&s_saved, false); }

/* ---------------------------------------------------------------- radio.h */

const char *radio_link_name(void) { return "AIS"; }

esp_err_t radio_start(const char *host, uint16_t port, const char *user, const char *pass)
{
    (void)host; (void)port; (void)user; (void)pass;
    if (s_task) return ESP_OK;
    if (!s_snap_mx && !(s_snap_mx = xSemaphoreCreateMutex())) return ESP_ERR_NO_MEM;
    const char *v = esp_app_get_description()->version;
    snprintf(s_ua, sizeof s_ua, "VFO-Knob-AIS/%s (+github.com/Guru-RF/VFO-Knob)", *v == 'v' ? v + 1 : v);
    const uint8_t *m, *p;
    size_t mn, pn;
    ais_client_tables(&m, &mn, &p, &pn);
    ESP_LOGI(TAG, "map %s (%u runs), %s harbours (%u)", adsb_map_ok(m, mn) ? "whole" : "BROKEN",
             adsb_map_ok(m, mn) ? (unsigned)adsb_map_runs(m) : 0u, ais_port_ok(p, pn) ? "whole" : "BROKEN",
             ais_port_ok(p, pn) ? (unsigned)ais_port_count(p) : 0u);
    s_min_at = esp_timer_get_time();
    /* Its stack in PSRAM, on core 0 beside WiFi, as ADS-B Radar's: a TLS
     * handshake is seconds of arithmetic, kept off the drawing's core. */
    if (xTaskCreatePinnedToCoreWithCaps(ais_task, "ais", 10240, NULL, 3, &s_task, 0, MALLOC_CAP_SPIRAM) != pdPASS) {
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
    } else if (s_no_key) {
        o->link = RADIO_LINK_DOWN;
        strlcpy(o->link_why, "NO API KEY", sizeof o->link_why);
    } else if (fresh(now)) {
        o->link = RADIO_LINK_READY;
    } else if (!s_ever_ok && !(s_fail_us && now - s_fail_us > GIVE_UP_US)) {
        o->link = RADIO_LINK_CONNECTING;
    } else {
        o->link = RADIO_LINK_DOWN;
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
