/* The tailchaser firmware's radio: TailChaser repeaters, watched and steered
 * through their API (TailChaser docs/API.md, tc_proto.h).
 *
 * Nothing here transmits: the knob reads a node's status a few times a
 * second and shows it on SVXConnect's face, it may block the node's receiver
 * or transmitter, and it plays what the one shown transmits (tc_audio.c,
 * once the node's [status] monitor is on). radio.h maps onto it as onto a
 * reflector:
 *
 *   the dial        steps through the repeaters set on the page. Every one
 *                   is read all the time, so a turn shows the next at once,
 *                   and the face goes by itself to one that becomes busy -- a
 *                   talker on its talkgroup, a voice on its input -- once the
 *                   one shown has been quiet, and the knob left alone, for the
 *                   hold set on the page (20 s; 0 never)
 *   the frequency   is the repeater's callsign, its output, shift and tone
 *                   under it, and the talkgroup its link is on
 *   the S-meter     is the receiver's signal (gate.rssi, calibrated to the
 *                   S-points, tc_smeter_dbm); the thin line outside it the
 *                   voice the repeater transmits, as played; and under it who
 *                   is talking: the talkgroup's talker, LOCAL for a voice on
 *                   the input, CARRIER for a squelch held open without one
 *   the speaker     on the link's row mutes every repeater's audio, kept;
 *                   the volume beside it is the knob's own
 *   the slab        the transmitter: TX and why, red, while it is keyed
 *   lock and mute   become RX and TX, the blocks, set from the menu held on
 *                   STANDBY, on the one shown
 *
 * The nodes are reached over the LAN or the internet as they are, or over
 * the owner's tailnet (tc_ts.h): then on0ora-tx is found by its Tailscale
 * name, from wherever the knob is.
 *
 * A task for the face and the blocks, and two readers beside it asking each
 * repeater as it falls due, the one shown first -- core 0 beside lwIP, their
 * stacks in PSRAM. The API keys live in the settings store and leave only in
 * the X-API-Key header, never in the log. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_app_desc.h"
#include "kvstore.h"
#include "cJSON.h"

#include "radio.h"
#include "tailchaser_client.h"
#include "tc_http.h"
#include "tc_audio.h"
#include "tc_proto.h"
#include "tc_ts.h"

static const char *TAG = "tailchaser";

#define NS          "tchaser"
#define POLL_MS       600       /* the status changes ~3 times a second       */
#define STALE_MS     5000       /* nothing newer than this: not shown as live */
#define SETTLE_MS     500       /* the dial at rest: its choice saved         */
#define READERS         2       /* repeaters asked at once                    */
#define BLOCK_CAP  (8 * 1024)   /* a block's answer: {"rx":..,"tx":..}        */
#define REFUSED_MS  15000       /* a key refused: asked again this much later */
#define TS_LASTS_MS 60000       /* Tailscale up this long: it did not crash   */
#define TS_TRIES        2       /* ...starts that did not, then it stays off  */
#define ANSWER_CAP  (64 * 1024) /* ~9 kB; ~22 kB with both histories full     */
#define SILENT_DB   -120.0f

/* The first failure after a good read asked again at once: most are a
 * stream that stalled, and a fresh one answers (tc_http.c, STALL_MS). */
static const uint16_t BACKOFF_S[] = { 0, 1, 2, 4, 8, 15 };

static inline uint64_t now_ms(void) { return (uint64_t)(esp_timer_get_time() / 1000); }

/* cJSON's tree in PSRAM: a status is a thousand small pieces, twice a
 * second, which internal RAM cannot spare. MicroLink sets the same once
 * Tailscale starts. */
static void *ext_malloc(size_t n) { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }

static inline bool since(uint64_t now, uint64_t then, uint32_t ms)
{
    return (int64_t)(now - then) >= (int64_t)ms;
}

/* ------------------------------------------------------------------ state */

/* What a reader last found of a node. */
enum { RD_NONE = 0, RD_OK, RD_REFUSED, RD_WAIT, RD_FAIL };
typedef struct {
    tc_status_t st;
    bool     have;              /* st is this node's                         */
    uint64_t t_ok;              /* when it last answered                     */
    uint8_t  kind;              /* the last request's R_*                    */
    int      fails;
    uint16_t again_s;           /* after a failure: asked again in           */
    uint8_t  path;              /* through the tailnet: 1 direct, 2 a DERP relay, 0 not known */
    char     why[96];
    char     brief[24];         /* ...in a few words, for the face           */
    bool     refused;           /* its key refused                           */
    bool     tailnet;           /* reached through Tailscale                 */
    char     addr[20];
    uint32_t seq;               /* one more with each request's end          */
} rd_t;

typedef struct {
    tc_addr_t a;
    char      key[TC_KEY];
    char      name[TC_NAME];    /* the owner's, from the page; "" the callsign */
} cfg_t;

/* A node: as set on the page, and what the readers found -- theirs and the
 * task's, so under s_nmux. */
typedef struct {
    cfg_t    c;
    rd_t     r;
    uint64_t t_next;            /* when it is asked again                    */
    uint64_t t_blk;             /* a block set: a read begun before keeps it */
    bool     busy;              /* a reader on it                            */
} node_t;

/* The task's own view of each: the readers' last, who is talking on it, and
 * whether it is busy. */
typedef struct {
    rd_t     r;
    bool     live;
    char     talker[16], last[16], info[32];
    int64_t  talker_t;          /* ms, the task's clock: before the start for an over before it */
    uint32_t net_t;             /* its net history's newest, as last seen    */
    bool     active;            /* a talker on its talkgroup, a voice on its input */
    uint64_t t_act0, t_act;     /* when that began; when it was last seen    */
} view_t;

enum { BLK_NONE = 0, BLK_RX, BLK_TX };

/* In PSRAM: the keys too, which leave only in the request. */
EXT_RAM_BSS_ATTR static struct {
    node_t   nodes[TC_NODES];   /* under s_nmux                              */
    int      n;                 /* written by the task under s_nmux          */
    uint32_t gen;               /* one more with each load: an older read dropped */
    int      sel;               /* the one shown: the dial's, or the busy one */
    uint64_t t_dial;            /* the knob last used: turned, the menu open */
    bool     dialed;            /* ...the dial's choice not yet saved        */
    int      noted;             /* the one whose refused key was told; -1    */
    uint16_t hold_s;            /* the one shown kept; 0 never left by itself */
    bool     amute;             /* the audio muted, every repeater's         */
    bool     ts_on;
    char     ts_auth[128], ts_name[40];
    view_t   v[TC_NODES];       /* the task's                                */
    uint64_t t_edge;            /* the dial's last click at an end           */
    char    *buf;               /* the task's, for the blocks                */
} C;
EXT_RAM_BSS_ATTR static cfg_t s_cfg[TC_NODES];     /* a load, before it is the nodes' */
static portMUX_TYPE  s_nmux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool s_net;     /* WiFi up and repeaters set: the readers ask */
static volatile int  s_amute_want = -1;    /* the face's speaker: 0 on, 1 muted */

/* What radio_get_status() reports and what the page reads. */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static struct {
    radio_link_t link;
    uint32_t tg;
    char     name[32], server[40], talker[16], talker_info[32], last[16], note[16];
    char     where[24];         /* "simplex 2/2", "connecting 2/2"           */
    uint32_t note_seq;
    int64_t  talker_t;          /* ms, the task's clock: before the start for an over before it */
    float    smeter;            /* the one shown's signal, dBm (S0 -127)     */
    bool     tx, block_rx, block_tx;
    uint32_t connects, closes;
    char     last_close[48];
    /* for the page */
    char     phase[96], node[TC_HOST + 8], addr[20];
    int      sel, n;
    bool     have, tailnet, refused;
    uint8_t  path;              /* the tailnet's to it: 1 direct, 2 a relay  */
    tc_status_t st;
} P;

static bool            s_ts_tried, s_ts_held, s_ts_armed;
static uint64_t        s_ts_t0;
static volatile int32_t s_detents;
static volatile bool    s_reload, s_started;
/* A block from the face's menu, by BLK_*: -1 none, 0 open, 1 block. */
static volatile int     s_want[3] = { -1, -1, -1 };
static volatile int     s_page_which = -1;
static volatile bool    s_page_on;

__attribute__((weak)) void haptic_hook(uint8_t effect, uint8_t prio)
{
    (void)effect; (void)prio;
}

static void set_phase(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void set_phase(const char *fmt, ...)
{
    char b[96];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    taskENTER_CRITICAL(&s_mux);
    if (strcmp(P.phase, b) != 0) memcpy(P.phase, b, sizeof P.phase);
    taskEXIT_CRITICAL(&s_mux);
}

static void note(const char *s)
{
    taskENTER_CRITICAL(&s_mux);
    snprintf(P.note, sizeof P.note, "%s", s);
    P.note_seq++;
    taskEXIT_CRITICAL(&s_mux);
}

/* ---------------------------------------------------------------- settings */

static void kv_str(kv_handle_t h, const char *k, char *out, size_t cap)
{
    size_t n = cap;
    if (kv_get_str(h, k, out, &n) != ESP_OK) out[0] = 0;
}

static void default_name(char *out, size_t cap)
{
    uint8_t mac[6] = { 0 };
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(out, cap, "vfo-knob-%02x%02x", mac[4], mac[5]);
}

static void load_all(bool first)
{
    const int was = C.sel;
    memset(s_cfg, 0, sizeof s_cfg);
    int n = 0;
    C.ts_on = false;
    C.ts_auth[0] = 0;
    C.hold_s = TC_HOLD_S;
    default_name(C.ts_name, sizeof C.ts_name);
    uint8_t sel = 0;
    kv_handle_t h;
    if (kv_open(NS, &h) == ESP_OK) {
        char *list = kv_dup(h, "nodes", NULL);
        for (char *line = list, *next; line && *line && n < TC_NODES; line = next) {
            next = strchr(line, '\n');
            if (next) *next++ = 0;
            if (!tc_parse_addr(line, &s_cfg[n].a)) continue;
            tc_line_name(line, s_cfg[n].name, sizeof s_cfg[n].name);
            char k[16];
            snprintf(k, sizeof k, "key%d", n);
            kv_str(h, k, s_cfg[n].key, sizeof s_cfg[n].key);
            n++;
        }
        free(list);
        uint8_t b;
        if (kv_get_u8(h, "ts", &b) == ESP_OK) C.ts_on = b;
        uint16_t hold;
        if (kv_get_u16(h, "hold", &hold) == ESP_OK) C.hold_s = hold;
        if (kv_get_u8(h, "amute", &b) == ESP_OK) C.amute = b;
        kv_str(h, "tsauth", C.ts_auth, sizeof C.ts_auth);
        char name[40];
        kv_str(h, "tsname", name, sizeof name);
        if (name[0]) snprintf(C.ts_name, sizeof C.ts_name, "%s", name);
        kv_get_u8(h, "sel", &sel);
        kv_close(h);
    }
    if (!first) sel = (uint8_t)was;
    /* The readers' list replaced in one go: a read begun on the old one is
     * dropped when it ends (C.gen). */
    taskENTER_CRITICAL(&s_nmux);
    C.gen++;
    memset(C.nodes, 0, sizeof C.nodes);
    for (int i = 0; i < n; i++) C.nodes[i].c = s_cfg[i];
    C.n   = n;
    C.sel = sel < n ? sel : 0;
    taskEXIT_CRITICAL(&s_nmux);
    memset(s_cfg, 0, sizeof s_cfg);
    memset(C.v, 0, sizeof C.v);
    tc_audio_set_mute(C.amute);
    C.noted  = -1;
    C.dialed = false;
    char follow[48] = "the dial's alone";
    if (C.hold_s) snprintf(follow, sizeof follow, "the busy one shown after %u s", (unsigned)C.hold_s);
    ESP_LOGI(TAG, "%d repeaters, Tailscale %s, %s", n, C.ts_on ? "on" : "off", follow);
    for (int i = 0; i < n; i++) {
        const cfg_t *c = &C.nodes[i].c;
        ESP_LOGI(TAG, "  %d: %s:%u%s%s%s", i + 1, c->a.host, (unsigned)c->a.port, c->name[0] ? ", shown as " : "",
                 c->name, c->key[0] ? "" : " (no key)");
    }
}

static void save_u8(const char *key, uint8_t v)
{
    kv_handle_t h;
    if (kv_open(NS, &h) != ESP_OK) return;
    kv_set_u8(h, key, v);
    kv_commit(h);
    kv_close(h);
}

static void save_sel(void)
{
    kv_handle_t h;
    if (kv_open(NS, &h) != ESP_OK) return;
    uint8_t old = 0xff;
    if (kv_get_u8(h, "sel", &old) != ESP_OK || old != (uint8_t)C.sel) {
        kv_set_u8(h, "sel", (uint8_t)C.sel);
        kv_commit(h);
    }
    kv_close(h);
}

/* Tailscale's starts that never lasted a minute: a beta stack that crashes
 * at its start must not take the knob to safe mode (WiFi off, the page out
 * of reach), so after TS_TRIES it is left off until saved again. The count
 * is written -- and waited for -- before each start. */
/* The count belongs to the firmware it was made by: an update -- a fix, most
 * likely -- tries Tailscale again without a save on the page. */
static void build_id(char out[17])
{
    out[0] = 0;
    esp_app_get_elf_sha256(out, 17);
}

static uint8_t ts_fails(void)
{
    uint8_t v = 0;
    char was[17] = "", now[17];
    size_t n = sizeof was;
    build_id(now);
    kv_handle_t h;
    if (kv_open(NS, &h) == ESP_OK) {
        kv_get_u8(h, "tsfail", &v);
        if (v && (kv_get_str(h, "tsfailb", was, &n) != ESP_OK || strcmp(was, now))) {
            ESP_LOGI(TAG, "Tailscale: a new firmware, so tried again (%u start(s) failed on the last)", (unsigned)v);
            v = 0;
        }
        kv_close(h);
    }
    return v;
}

static void ts_fails_set(uint8_t v)
{
    kv_handle_t h;
    if (kv_open(NS, &h) != ESP_OK) return;
    if (v) {
        char now[17];
        build_id(now);
        kv_set_u8(h, "tsfail", v);
        kv_set_str(h, "tsfailb", now);
    } else {
        kv_erase_key(h, "tsfail");
        kv_erase_key(h, "tsfailb");
    }
    kv_commit_wait(h, 3000);
    kv_close(h);
}

static void ts_start_once(uint64_t t)
{
    if (s_ts_tried) return;
    s_ts_tried = true;
    const uint8_t n = ts_fails();
    if (n >= TS_TRIES) {
        s_ts_held = true;
        ESP_LOGW(TAG, "Tailscale left off: it started %u times without lasting a minute", (unsigned)n);
        return;
    }
    ts_fails_set((uint8_t)(n + 1));
    s_ts_armed = true;
    s_ts_t0 = t;
    tc_ts_start(C.ts_auth, C.ts_name);
}

bool tc_ts_held(void) { return s_ts_held; }

/* ----------------------------------------------------------------- publish */

/* "438.800 -7.6 T131.8": the output, the shift to the input, the tone. */
static void freq_line(const tc_status_t *s, char *out, size_t cap)
{
    char f[16], t[16] = "";
    if (s->tx_mhz > 0) {
        tc_mhz(s->tx_mhz, f, sizeof f);
        const double shift = s->rx_mhz > 0 ? s->rx_mhz - s->tx_mhz : 0;
        if (s->ctcss_tx[0] && strcmp(s->ctcss_tx, "none") != 0) snprintf(t, sizeof t, " T%s", s->ctcss_tx);
        if (shift > 0.0005 || shift < -0.0005)
            snprintf(out, cap, "%s %+.3g%s", f, shift, t);
        else
            snprintf(out, cap, "%s%s", f, t);
    } else if (s->rx_mhz > 0) {
        tc_mhz(s->rx_mhz, f, sizeof f);
        snprintf(out, cap, "RX %s", f);
    } else {
        snprintf(out, cap, "%s", s->role);
    }
}

/* When the talkgroup's last over ended, by the node's clock: its net
 * history stamps an over as its audio stops coming (engine.c net_over_end). */
static uint32_t net_end(const tc_status_t *s) { return s->net_t; }

/* The node still on the air for the talkgroup's over that just ended: keyed
 * for talkgroup audio since before that over's end. Its talker is cleared
 * the moment the reflector says it stopped, but the repeater sends the call
 * in Morse first, the audio waiting in its queue meanwhile, so the voice is
 * on the air after that -- then its tail and its roger beep: some 5 s more
 * (2026-11-20: overs of 1-3 s, keyed 5-6 s longer), all that talker's. */
static bool net_tail(const tc_status_t *s)
{
    return s->tx_keyed && s->net_call[0] && !strcmp(s->tx_why, "talkgroup audio") && s->now &&
           net_end(s) + 1u + s->tx_keyed_ms / 1000u >= s->now;
}

/* Each node as the readers last found it, who is talking on it, and whether
 * it is busy: a talker on its talkgroup or a voice on its input -- not an
 * announcement, nor a carrier with no voice. */
static void watch(uint64_t t)
{
    for (int k = 0; k < C.n; k++) {
        view_t *v = &C.v[k];
        taskENTER_CRITICAL(&s_nmux);
        if (C.nodes[k].r.seq != v->r.seq) v->r = C.nodes[k].r;
        taskEXIT_CRITICAL(&s_nmux);
        const tc_status_t *s = &v->r.st;
        const bool was_live = v->live;
        v->live = v->r.have && !since(t, v->r.t_ok, STALE_MS);
        if (was_live && !v->live)
            ESP_LOGW(TAG, "%s: no status for %u s%s", C.nodes[k].c.a.host, (unsigned)(STALE_MS / 1000),
                     k == C.sel ? ": NO LINK on the face" : "");
        /* Who is talking: the talkgroup's talker, or a voice on the input, or
         * a carrier the gate keeps back. */
        char who[16] = "", info[32] = "", sm[12];
        if (v->live) {
            tc_smeter(s->rssi, s->rssi_lo, s->rssi_hi, sm, sizeof sm);
            if (s->talker[0]) {
                snprintf(who, sizeof who, "%s", s->talker);
                snprintf(info, sizeof info, "TG %lu", (unsigned long)s->tg);
            } else if (s->open) {
                snprintf(who, sizeof who, "LOCAL");
                snprintf(info, sizeof info, "%s", sm[0] ? sm : "on the input");
            } else if (s->cos) {
                snprintf(who, sizeof who, "CARRIER");
                snprintf(info, sizeof info, "%s%sno voice", sm, sm[0] ? ", " : "");
            } else if (net_tail(s)) {
                snprintf(who, sizeof who, "%s", s->net_call);
                snprintf(info, sizeof info, "TG %lu", (unsigned long)s->tg);
            }
            if (s->tx_keyed && s->tx_why[0]) snprintf(info, sizeof info, "%s", s->tx_why);
        }
        if (strcmp(who, v->talker) != 0) {
            if (k == C.sel)
                ESP_LOGI(TAG, "%s: %s%s (read %u ms ago)", C.nodes[k].c.a.host, who[0] ? who : "quiet",
                         who[0] && s->tx_keyed ? ", on the air" : "", (unsigned)(t - v->r.t_ok));
            if (v->talker[0]) memcpy(v->last, v->talker, sizeof v->last);
            memcpy(v->talker, who, sizeof v->talker);
            v->talker_t = (int64_t)t;
        }
        /* An over from the talkgroup the readers never caught -- an over of
         * a second, between two reads -- or the one before the knob came on:
         * the last talker all the same, timed by the node's clock. */
        _Static_assert(sizeof v->last == sizeof s->net_call, "the last talker's size");
        if (v->live && s->net_call[0] && s->net_t != v->net_t) {
            v->net_t = s->net_t;
            if (!who[0]) {
                memcpy(v->last, s->net_call, sizeof v->last);
                v->last[sizeof v->last - 1] = 0;
                const uint32_t ago = s->now > net_end(s) ? s->now - net_end(s) : 0;
                v->talker_t = (int64_t)t - (int64_t)ago * 1000;
            }
        }
        memcpy(v->info, info, sizeof v->info);
        const bool busy = v->live && (s->talker[0] || s->open);
        if (busy && !v->active) v->t_act0 = t;
        if (busy) v->t_act = t;
        v->active = busy;
    }
}

/* The face's, where there is one (none in the PC's tests). */
extern bool ui_edit_active(void) __attribute__((weak));

/* The face goes by itself to a repeater that becomes busy, once the one
 * shown has been quiet -- and the knob left alone -- for the hold: two that
 * answer each other are not bounced between. Of several, the one busy last. */
static void follow(uint64_t t)
{
    if (!C.hold_s || C.n < 2) return;
    if (ui_edit_active && ui_edit_active()) C.t_dial = t;      /* the menu open: kept */
    const view_t *s = &C.v[C.sel];
    if (s->active || !since(t, MAX(s->t_act, C.t_dial), (uint32_t)C.hold_s * 1000u)) return;
    int best = -1;
    for (int k = 0; k < C.n; k++)
        if (k != C.sel && C.v[k].active && (best < 0 || C.v[k].t_act0 > C.v[best].t_act0)) best = k;
    if (best < 0) return;
    ESP_LOGI(TAG, "repeater %d: %s:%u, busy (%s)", best + 1, C.nodes[best].c.a.host,
             (unsigned)C.nodes[best].c.a.port, C.v[best].talker[0] ? C.v[best].talker : "?");
    C.sel = best;
}

/* The page's line for the one shown, and its refused key told on the face
 * once, when it is shown. */
static void tell(void)
{
    const int i = C.sel;
    const view_t *v = &C.v[i];
    const char *host = C.nodes[i].c.a.host;
    if (v->r.refused) {
        if (C.noted != i) {
            C.noted = i;
            note(C.nodes[i].c.key[0] ? "KEY REFUSED" : "NO KEY");
        }
    } else if (C.noted == i) {
        C.noted = -1;
    }
    switch (v->r.kind) {
    case RD_OK:      set_phase("reading %s", host); break;
    case RD_REFUSED: set_phase("%s", v->r.why); break;
    case RD_WAIT:    set_phase("%s: %s", host, v->r.why); break;
    case RD_FAIL:
        if (v->r.again_s) set_phase("%s: %s; again in %u s", host, v->r.why, (unsigned)v->r.again_s);
        else              set_phase("%s: %s; again now", host, v->r.why);
        break;
    default:        set_phase("asking %s", host); break;
    }
}

static void publish(void)
{
    const int sel = C.sel;
    const view_t *v = C.n ? &C.v[sel] : NULL;
    const bool live = v && v->live;
    const tc_status_t *s = v ? &v->r.st : NULL;
    const radio_link_t link = live ? RADIO_LINK_READY
                            : !v || v->r.refused || v->r.fails >= 3 ? RADIO_LINK_DOWN : RADIO_LINK_CONNECTING;
    char server[40];
    if (live)   freq_line(s, server, sizeof server);
    else if (v) snprintf(server, sizeof server, "%s", v->r.brief[0] ? v->r.brief : "connecting");
    else        snprintf(server, sizeof server, "no repeaters");
    /* The owner's name for it; else its callsign; else its address. */
    char name[32] = "", node[TC_HOST + 8] = "";
    if (v) {
        const cfg_t *c = &C.nodes[sel].c;
        if (c->name[0])                  snprintf(name, sizeof name, "%s", c->name);
        else if (live && s->callsign[0]) snprintf(name, sizeof name, "%s", s->callsign);
        else                             snprintf(name, sizeof name, "%.31s", c->a.host);
        snprintf(node, sizeof node, "%s:%u", c->a.host, (unsigned)c->a.port);
    }
    /* Which one: its role and its place on the dial -- two of one station
     * share a callsign -- or, not read, the link's state. */
    char pos[12] = "", where[24] = "";
    if (C.n > 1) snprintf(pos, sizeof pos, " %u/%u", (unsigned)(uint8_t)(sel + 1), (unsigned)(uint8_t)C.n);
    if (!v)                           where[0] = 0;
    else if (live)                    snprintf(where, sizeof where, "%.11s%s", s->role[0] ? s->role : "connected", pos);
    else if (link == RADIO_LINK_DOWN) snprintf(where, sizeof where, "disconnected%s", pos);
    else                              snprintf(where, sizeof where, "connecting%s", pos);

    taskENTER_CRITICAL(&s_mux);
    P.link = link;
    P.tg   = live && s->link_up ? s->tg : 0;
    memcpy(P.name, name, sizeof P.name);
    memcpy(P.where, where, sizeof P.where);
    memcpy(P.server, server, sizeof P.server);
    if (v) {
        memcpy(P.talker, v->talker, sizeof P.talker);
        memcpy(P.talker_info, v->info, sizeof P.talker_info);
        memcpy(P.last, v->last, sizeof P.last);
        P.talker_t = v->talker_t;
    } else {
        P.talker[0] = P.talker_info[0] = P.last[0] = 0;
    }
    P.smeter   = live ? tc_smeter_dbm(s->rssi, s->rssi_lo, s->rssi_hi) : -127.0f;
    P.tx       = live && s->tx_keyed;
    P.block_rx = v && v->r.have && s->block_rx;
    P.block_tx = v && v->r.have && s->block_tx;
    P.sel      = sel;
    P.n        = C.n;
    P.have     = live;
    P.tailnet  = v && v->r.tailnet;
    P.path     = v && v->r.tailnet ? v->r.path : 0;
    P.refused  = v && v->r.refused;
    if (v) memcpy(P.addr, v->r.addr, sizeof P.addr);
    else   P.addr[0] = 0;
    memcpy(P.node, node, sizeof P.node);
    if (v && v->r.have) P.st = *s;
    else                memset(&P.st, 0, sizeof P.st);
    taskEXIT_CRITICAL(&s_mux);
}

/* ------------------------------------------------------------------ the API */

/* One request of a reader's: node i's status, as the list was at gen. What
 * it found goes in only if the list is still that one. */
static void read_node(int i, uint32_t gen, const tc_addr_t *a, const char *key, char *buf, uint64_t t0)
{
    tc_http_t r;
    const bool got = tc_http(a, key, "GET", "/api/v1/status", NULL, buf, ANSWER_CAP, &r);
    const uint64_t t = now_ms();
    tc_status_t st;
    memset(&st, 0, sizeof st);
    uint8_t kind = RD_FAIL;
    char why[96] = "", brief[24] = "";
    if (got && r.code == 200) {
        char pw[48];
        if (tc_parse_status(r.body, r.body_len, &st, pw, sizeof pw)) {
            kind = RD_OK;
        } else {
            snprintf(why, sizeof why, "%s", pw);
            snprintf(brief, sizeof brief, "not a TailChaser");
        }
    } else if (got && r.code == 401) {
        kind = RD_REFUSED;
        snprintf(why, sizeof why, key[0] ? "the key was refused: make one in the node's admin portal"
                                         : "no key: set one on the web page");
        snprintf(brief, sizeof brief, key[0] ? "key refused" : "no API key");
    } else if (got) {
        snprintf(why, sizeof why, "answered %d", r.code);
        snprintf(brief, sizeof brief, "answered %d", r.code);
    } else if (r.wait_ts) {
        /* Not a failure: asked again soon, and at once when it is up. */
        kind = RD_WAIT;
        snprintf(why, sizeof why, "%s", r.why);
        snprintf(brief, sizeof brief, "waiting for Tailscale");
    } else {
        snprintf(why, sizeof why, "%s", r.why);
        snprintf(brief, sizeof brief, "%s", strstr(r.why, "no address") || strstr(r.why, "not on the tailnet")
                                                ? "no such repeater" : "no answer");
    }

    bool first = false, blocks = false, refused_now = false;
    int fails = 0;
    /* How the tailnet reaches it: direct, or relayed -- a relay carries
     * every stream to it in one TCP connection, which stalls them all at
     * once. Said when it changes. */
    const int path = r.tailnet ? tc_ts_path(r.ip) : 0;
    int path_was = 0;
    taskENTER_CRITICAL(&s_nmux);
    const bool mine = gen == C.gen && i < C.n;
    if (mine) {
        node_t *nd = &C.nodes[i];
        rd_t *d = &nd->r;
        nd->busy   = false;
        d->kind    = kind;
        d->tailnet = r.tailnet;
        path_was   = d->path;
        if (path) d->path = (uint8_t)path;
        if (r.addr[0]) memcpy(d->addr, r.addr, sizeof d->addr);
        if (kind == RD_OK) {
            /* A block set while this was asked: the newer kept. */
            if (nd->t_blk > t0) {
                st.block_rx = d->st.block_rx;
                st.block_tx = d->st.block_tx;
            }
            first      = !d->have || d->fails;
            blocks     = d->have && (st.block_rx != d->st.block_rx || st.block_tx != d->st.block_tx);
            d->st      = st;
            d->have    = true;
            d->t_ok    = t;
            d->fails   = 0;
            d->refused = false;
            d->why[0] = d->brief[0] = 0;
            nd->t_next = t + POLL_MS;
        } else {
            memcpy(d->why, why, sizeof d->why);
            memcpy(d->brief, brief, sizeof d->brief);
            if (kind == RD_REFUSED) {
                refused_now = !d->refused;
                d->refused  = true;
                nd->t_next  = t + REFUSED_MS;
            } else if (kind == RD_WAIT) {
                nd->t_next = t + 500;
            } else {
                fails      = d->fails++;
                d->again_s = BACKOFF_S[MIN(fails, (int)(sizeof BACKOFF_S / sizeof BACKOFF_S[0]) - 1)];
                nd->t_next = t + d->again_s * 1000u;
            }
        }
        d->seq++;
    }
    taskEXIT_CRITICAL(&s_nmux);
    if (!mine) return;
    if (path && path != path_was)
        ESP_LOGI(TAG, "%s: %s", a->host, path == 1 ? "a direct path through the tailnet"
                                                   : "through a DERP relay: no direct path through the tailnet");

    /* A read this slow leaves the face without a status for as long. */
    if (t - t0 > 2500)
        ESP_LOGW(TAG, "%s: a read took %u ms (%s)%s", a->host, (unsigned)(t - t0), kind == RD_OK ? "read" : why,
                 r.tailnet ? ", through the tailnet" : "");
    if (kind == RD_OK) {
        if (first) {
            ESP_LOGI(TAG, "%s: %s %s, %s%s", a->host, st.callsign[0] ? st.callsign : "?", st.role, st.version,
                     r.tailnet ? ", through the tailnet" : "");
            taskENTER_CRITICAL(&s_mux);
            P.connects++;
            taskEXIT_CRITICAL(&s_mux);
        }
        if (blocks)
            ESP_LOGI(TAG, "%s: receiver %s, transmitter %s", a->host, st.block_rx ? "blocked" : "open",
                     st.block_tx ? "blocked" : "open");
    } else if (kind == RD_REFUSED) {
        if (refused_now) ESP_LOGW(TAG, "%s refused the key", a->host);
    } else if (kind == RD_FAIL) {
        if (!fails || fails == 3) ESP_LOGW(TAG, "%s: %s", a->host, why);
        if (fails == 2) {
            taskENTER_CRITICAL(&s_mux);
            P.closes++;
            snprintf(P.last_close, sizeof P.last_close, "%.47s", why);
            taskEXIT_CRITICAL(&s_mux);
        }
    }
}

/* A reader: whichever repeater is due -- the one shown first -- and again. */
static void reader(void *arg)
{
    char *buf = arg;
    for (;;) {
        int i = -1;
        uint32_t gen = 0;
        tc_addr_t a;
        char key[TC_KEY];
        const uint64_t t = now_ms();
        if (s_net) {
            taskENTER_CRITICAL(&s_nmux);
            for (int k = 0; k < C.n; k++) {
                const node_t *nd = &C.nodes[k];
                if (nd->busy || t < nd->t_next) continue;
                if (i < 0 || k == C.sel || (i != C.sel && nd->t_next < C.nodes[i].t_next)) i = k;
            }
            if (i >= 0) {
                C.nodes[i].busy = true;
                a   = C.nodes[i].c.a;
                memcpy(key, C.nodes[i].c.key, sizeof key);
                gen = C.gen;
            }
            taskEXIT_CRITICAL(&s_nmux);
        }
        if (i < 0) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        read_node(i, gen, &a, key, buf, t);
        memset(key, 0, sizeof key);
    }
}

/* Block (or open) the receiver or the transmitter of the one shown. */
static void do_block(int which, bool on)
{
    const int i = C.sel;
    const cfg_t *c = &C.nodes[i].c;
    const char *what = which == BLK_RX ? "receiver" : "transmitter";
    char form[8];
    snprintf(form, sizeof form, "%s=%d", which == BLK_RX ? "rx" : "tx", on ? 1 : 0);
    tc_http_t r;
    const bool got = tc_http(&c->a, c->key, "POST", "/api/v1/block", form, C.buf, BLOCK_CAP, &r);
    bool rx = false, tx = false, set = false;
    if (got && r.code == 200 && tc_parse_block(r.body, r.body_len, &rx, &tx)) {
        set = true;
        const bool now_on = which == BLK_RX ? rx : tx;
        ESP_LOGI(TAG, "%s: %s %s", c->a.host, what, now_on ? "blocked" : "open again");
        note(which == BLK_RX ? (now_on ? "RX BLOCKED" : "RX OPEN") : (now_on ? "TX BLOCKED" : "TX OPEN"));
        haptic_hook(14, 2);
    } else if (got && r.code == 403) {
        ESP_LOGW(TAG, "%s: a read key may not block", c->a.host);
        note("READ-ONLY KEY");
    } else if (got && r.code == 401) {
        note("KEY REFUSED");
    } else {
        char e[48] = "";
        if (got) tc_parse_error(r.body, r.body_len, e, sizeof e);
        ESP_LOGW(TAG, "%s: blocking the %s failed: %s", c->a.host, what, got ? (e[0] ? e : "refused") : r.why);
        note("NOT DONE");
    }
    const uint64_t t = now_ms();
    taskENTER_CRITICAL(&s_nmux);
    node_t *nd = &C.nodes[i];
    if (set) {
        nd->r.st.block_rx = rx;
        nd->r.st.block_tx = tx;
        nd->r.seq++;
        nd->t_blk = t;
    }
    nd->t_next = 0;             /* asked again at once */
    taskEXIT_CRITICAL(&s_nmux);
}

/* ---------------------------------------------------------------- requests */

static void requests(uint64_t t)
{
    /* One repeater a detent, stopping at either end: wrapping round, a turn
     * of two detents among two repeaters came back where it began. At an end
     * the motor clicks, a fifth of a second apart at most. */
    const int32_t d = __atomic_exchange_n(&s_detents, 0, __ATOMIC_RELAXED);
    if (d && C.n > 0) {
        long s = (long)C.sel + d;
        const bool past = s < 0 || s >= C.n;
        if (s < 0) s = 0;
        if (s >= C.n) s = C.n - 1;
        if (s != C.sel) {
            C.sel = (int)s;
            C.dialed = true;
        }
        C.t_dial = t;           /* a hand on the knob: the one shown kept    */
        if (past && since(t, C.t_edge, 200)) {
            C.t_edge = t;
            haptic_hook(1, 1);
        }
    }
    /* The face's menu: each block as it is to be, not "the other way". */
    for (int w = BLK_RX; w <= BLK_TX; w++) {
        const int want = __atomic_exchange_n(&s_want[w], -1, __ATOMIC_RELAXED);
        if (want < 0) continue;
        if (!C.n || !C.v[C.sel].r.have) note("NOT CONNECTED");
        else do_block(w, want == 1);
    }
    const int pw = __atomic_exchange_n(&s_page_which, -1, __ATOMIC_RELAXED);
    if (pw >= 0 && C.n && C.v[C.sel].r.have) do_block(pw == 0 ? BLK_RX : BLK_TX, s_page_on);
}

/* ------------------------------------------------------------------- task */

static bool have_ip(void)
{
    esp_netif_t *n = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip;
    return n && esp_netif_get_ip_info(n, &ip) == ESP_OK && ip.ip.addr;
}

static void tc_task(void *arg)
{
    (void)arg;
    load_all(true);
    for (;;) {
        const uint64_t t = now_ms();
        if (s_reload) {
            s_reload = false;
            load_all(false);
        }
        requests(t);
        if (!C.n) {
            s_net = false;
            set_phase("no repeaters: add one on the web page");
        } else if (!have_ip()) {
            s_net = false;
            set_phase("waiting for WiFi");
        } else {
            if (C.ts_on) ts_start_once(t);
            if (s_ts_armed && since(t, s_ts_t0, TS_LASTS_MS)) {
                s_ts_armed = false;
                ts_fails_set(0);
            }
            s_net = true;
            if (C.dialed && since(t, C.t_dial, SETTLE_MS)) {
                C.dialed = false;
                save_sel();
                ESP_LOGI(TAG, "repeater %d: %s:%u", C.sel + 1, C.nodes[C.sel].c.a.host,
                         (unsigned)C.nodes[C.sel].c.a.port);
            }
        }
        const int am = __atomic_exchange_n(&s_amute_want, -1, __ATOMIC_RELAXED);
        if (am >= 0 && (am == 1) != C.amute) {
            C.amute = am == 1;
            tc_audio_set_mute(C.amute);
            save_u8("amute", C.amute);
            ESP_LOGI(TAG, "the repeaters' audio %s", C.amute ? "muted" : "on");
        }
        watch(t);
        if (C.n) {
            follow(t);
            if (s_net) tell();
        }
        /* The audio: the one shown's, once its status says its listening is
         * on. A number of its own for each repeater of each list. */
        if (s_net && C.n) {
            const view_t *v = &C.v[C.sel];
            tc_audio_focus(&C.nodes[C.sel].c.a, C.gen << 4 | (uint32_t)C.sel, v->r.have && v->r.st.monitor);
        } else {
            tc_audio_focus(NULL, 0, false);
        }
        publish();
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

/* ------------------------------------------------------------- for the page */

static void esc(char *o, size_t cap, const char *s)
{
    size_t k = 0;
    for (; *s && k + 7 < cap; s++) {
        const unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { o[k++] = '\\'; o[k++] = (char)c; }
        else if (c < 0x20)         k += (size_t)snprintf(o + k, cap - k, "\\u%04x", c);
        else                       o[k++] = (char)c;
    }
    o[k] = 0;
}

void tc_state_json(char *out, size_t cap)
{
    static char phase[96], node[TC_HOST + 8], addr[20], talker[16], last[16], name[32], server[40];
    static tc_status_t st;
    radio_link_t link;
    bool have, tailnet, refused, block_rx, block_tx;
    uint8_t path;
    int sel, n;
    taskENTER_CRITICAL(&s_mux);
    memcpy(phase, P.phase, sizeof phase);
    memcpy(node, P.node, sizeof node);
    memcpy(addr, P.addr, sizeof addr);
    memcpy(talker, P.talker, sizeof talker);
    memcpy(last, P.last, sizeof last);
    memcpy(name, P.name, sizeof name);
    memcpy(server, P.server, sizeof server);
    st = P.st;
    link = P.link;
    have = P.have;
    tailnet = P.tailnet;
    path = P.path;
    refused = P.refused;
    block_rx = P.block_rx;
    block_tx = P.block_tx;
    sel = P.sel;
    n = P.n;
    taskEXIT_CRITICAL(&s_mux);

    char url[256] = "";
    tc_ts_login_url(url, sizeof url);
    const uint32_t ip = tc_ts_ip();
    char sm[12];
    tc_smeter(st.rssi, st.rssi_lo, st.rssi_hi, sm, sizeof sm);
    static char e[12][128];     /* static: the web server's stack is small */
    const char *src[12] = { phase, node, addr, talker, last, name, server, st.role, st.version, st.tx_why,
                            st.reflector, url };
    for (int i = 0; i < 12; i++) esc(e[i], sizeof e[i], src[i]);
    char ws[16];
    esc(ws, sizeof ws, st.gate);
    snprintf(out, cap,
             "{\"link\":\"%s\",\"phase\":\"%s\",\"sel\":%d,\"n\":%d,\"node\":\"%s\",\"addr\":\"%s\","
             "\"tailnet\":%s,\"path\":\"%s\",\"refused\":%s,\"live\":%s,"
             "\"name\":\"%s\",\"freq\":\"%s\",\"role\":\"%s\",\"version\":\"%s\",\"uptime\":%lu,"
             "\"block_rx\":%s,\"block_tx\":%s,\"tx\":%s,\"tx_why\":\"%s\",\"tx_ms\":%lu,"
             "\"cos\":%s,\"open\":%s,\"gate\":\"%s\",\"smeter\":\"%s\",\"level\":%.1f,\"rx_site\":%s,"
             "\"link_up\":%s,\"tg\":%lu,\"home_tg\":%lu,\"reflector\":\"%s\",\"talker\":\"%s\",\"last\":\"%s\","
             "\"smeter_dbm\":%.1f,\"monitor\":%s,"
             "\"audio\":{\"state\":\"%s\",\"muted\":%s,\"chunks\":%lu,\"level\":%.1f},"
             "\"ts\":{\"state\":\"%s\",\"ip\":\"%lu.%lu.%lu.%lu\",\"peers\":%d,\"login\":\"%s\",\"held\":%s}}",
             link == RADIO_LINK_READY ? "online" : link == RADIO_LINK_DOWN ? "down" : "connecting", e[0], sel, n,
             e[1], e[2], tailnet ? "true" : "false", path == 1 ? "direct" : path == 2 ? "relay" : "",
             refused ? "true" : "false", have ? "true" : "false", e[5],
             e[6], e[7], e[8], (unsigned long)st.uptime_s, block_rx ? "true" : "false",
             block_tx ? "true" : "false", st.tx_keyed ? "true" : "false", e[9], (unsigned long)st.tx_keyed_ms,
             st.cos ? "true" : "false", st.open ? "true" : "false", ws, sm, (double)st.level_db,
             st.has_rxlink ? (st.rx_site_up ? "true" : "false") : "null", st.link_up ? "true" : "false",
             (unsigned long)st.tg, (unsigned long)st.home_tg, e[10], e[3], e[4],
             have ? (double)tc_smeter_dbm(st.rssi, st.rssi_lo, st.rssi_hi) : -127.0, st.monitor ? "true" : "false",
             tc_audio_state_word(tc_audio_state()), tc_audio_muted() ? "true" : "false",
             (unsigned long)tc_audio_chunks(), (double)tc_audio_level(), tc_ts_state_word(tc_ts_state()),
             (unsigned long)(ip >> 24), (unsigned long)((ip >> 16) & 255), (unsigned long)((ip >> 8) & 255),
             (unsigned long)(ip & 255), tc_ts_peers(), e[11], s_ts_held ? "true" : "false");
}

void tc_reload(void) { s_reload = true; }

void tc_set_audio_mute(bool on) { __atomic_store_n(&s_amute_want, on ? 1 : 0, __ATOMIC_RELAXED); }

void tc_request_block(int which, bool on)
{
    s_page_on = on;
    __atomic_store_n(&s_page_which, which, __ATOMIC_RELAXED);
}

extern const httpd_uri_t tc_web_uris[];
extern const size_t      tc_web_uris_n;

size_t radio_web_endpoints(const httpd_uri_t **out);
size_t radio_web_endpoints(const httpd_uri_t **out)
{
    *out = tc_web_uris;
    return tc_web_uris_n;
}

/* ---------------------------------------------------------------- radio.h */

const char *radio_link_name(void) { return "TailChaser"; }

esp_err_t radio_start(const char *host, uint16_t port, const char *user, const char *pass)
{
    (void)host; (void)port; (void)user; (void)pass;     /* the repeaters are the client's own */
    if (s_started) return ESP_OK;
    memset(&C, 0, sizeof C);
    C.noted = -1;
    C.buf = heap_caps_malloc(BLOCK_CAP, MALLOC_CAP_SPIRAM);
    char *rbuf[READERS];
    for (int i = 0; i < READERS; i++) rbuf[i] = heap_caps_malloc(ANSWER_CAP, MALLOC_CAP_SPIRAM);
    for (int i = 0; i < READERS; i++)
        if (!rbuf[i]) C.buf = NULL;
    if (!C.buf) return ESP_ERR_NO_MEM;
    cJSON_InitHooks(&(cJSON_Hooks){ .malloc_fn = ext_malloc, .free_fn = free });
    memset(&P, 0, sizeof P);
    P.link   = RADIO_LINK_CONNECTING;
    P.smeter = -127.0f;
    /* Core 0 with lwIP; the stack in PSRAM: it never touches flash (the
     * settings store writes on its own task). */
    if (xTaskCreatePinnedToCoreWithCaps(tc_task, "tailchaser", 8192, NULL, 5, NULL, 0, MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGE(TAG, "no PSRAM for the task");
        return ESP_ERR_NO_MEM;
    }
    for (int i = 0; i < READERS; i++) {
        char nm[12];
        snprintf(nm, sizeof nm, "tc_read%d", i + 1);
        if (xTaskCreatePinnedToCoreWithCaps(reader, nm, 8192, rbuf[i], 5, NULL, 0, MALLOC_CAP_SPIRAM) != pdPASS) {
            ESP_LOGE(TAG, "no PSRAM for a reader");
            return ESP_ERR_NO_MEM;
        }
    }
    if (!tc_audio_start()) ESP_LOGE(TAG, "no PSRAM for the audio: the repeaters are not heard");
    s_started = true;
    return ESP_OK;
}

int64_t radio_tune_by(int32_t detents, uint8_t accel_mult, int32_t step_hz)
{
    (void)accel_mult; (void)step_hz;        /* one repeater a detent */
    if (detents) __atomic_add_fetch(&s_detents, detents, __ATOMIC_RELAXED);
    return P.tg;
}

void radio_set_step(int32_t step_hz)            { (void)step_hz; }
void radio_audio_suspend(bool suspend)          { (void)suspend; }
void radio_set_mode(const char *mode)           { (void)mode; }
void radio_set_filter(int32_t lo, int32_t hi)   { (void)lo; (void)hi; }
void radio_select_filter(uint8_t n)             { (void)n; }
void radio_set_rit(int32_t hz)                  { (void)hz; }
void radio_set_agc(const char *agc)             { (void)agc; }
void radio_set_gain(int8_t gain)                { (void)gain; }
void radio_goto_freq(int64_t hz)                { (void)hz; }
void radio_memory_mode(bool on)                 { (void)on; }
void radio_memory_group(uint8_t group)          { (void)group; }
void radio_select_rx(uint8_t rx)                { (void)rx; }
void radio_set_antenna(uint8_t ant, bool rx_ant) { (void)ant; (void)rx_ant; }
void radio_set_tx_antenna(uint8_t ant)          { (void)ant; }
void radio_tune(void)     {}
void radio_atu_tune(void) {}
void radio_atu_memories(bool on) { (void)on; }
void radio_set_dsp(uint8_t which, bool on) { (void)which; (void)on; }
void radio_set_rf_gain(uint8_t pct)  { (void)pct; }
void radio_set_rf_power(uint8_t pct) { (void)pct; }
void radio_set_tuner(bool on)        { (void)on; }
void radio_set_squelch(uint8_t pct)  { (void)pct; }

bool radio_get_choice(uint8_t i, char *title, size_t tn, char *name, size_t nn)
{
    (void)i; (void)title; (void)tn; (void)name; (void)nn;
    return false;
}
void radio_choose(uint8_t i) { (void)i; }

/* The face's menu: the receiver's block (the reflector's lock) and the
 * transmitter's (its mute), each set as asked, on the one shown. */
void radio_tg_lock(bool locked) { __atomic_store_n(&s_want[BLK_RX], locked ? 1 : 0, __ATOMIC_RELAXED); }
void radio_mute(bool muted)     { __atomic_store_n(&s_want[BLK_TX], muted ? 1 : 0, __ATOMIC_RELAXED); }

/* Nothing to key: the knob transmits nothing. */
void radio_ptt_key(void)    {}
void radio_ptt_unkey(void)  {}
void radio_ptt_toggle(void) {}
void radio_ptt_force_abort(uint8_t reason) { (void)reason; }

bool radio_is_ready(void) { return P.link == RADIO_LINK_READY; }

/* The repeater's transmitter is not the knob's: never "on the air" here. */
bool radio_on_air(void) { return false; }

void radio_get_status(radio_status_t *o)
{
    if (!o) return;
    memset(o, 0, sizeof *o);
    const uint64_t t = now_ms();
    taskENTER_CRITICAL(&s_mux);
    o->link        = P.link;
    o->reflector   = true;
    /* The knob keys nothing: the repeater's transmitter is its own. Without
     * this the face warned "TX DISABLED" over a repeater free to transmit. */
    o->rx_only     = true;
    o->tg          = P.tg;
    memcpy(o->tg_name, P.name, sizeof o->tg_name);
    memcpy(o->talker, P.talker, sizeof o->talker);
    memcpy(o->talker_info, P.talker_info, sizeof o->talker_info);
    memcpy(o->last_talker, P.last, sizeof o->last_talker);
    o->talker_ms   = (P.talker[0] || P.last[0]) ? (uint32_t)((int64_t)t - P.talker_t) : 0;
    o->tg_locked   = P.block_rx;
    o->muted       = P.block_tx;
    o->smeter_dbm  = P.smeter;
    o->tx_mic_dbm  = SILENT_DB;
    memcpy(o->server, P.server, sizeof o->server);
    memcpy(o->where, P.where, sizeof o->where);
    memcpy(o->note, P.note, sizeof o->note);
    o->note_seq    = P.note_seq;
    o->tx          = P.tx;
    o->n_trx       = 1;
    o->connects    = P.connects;
    o->closes      = P.closes;
    memcpy(o->last_close, P.last_close, sizeof o->last_close);
    taskEXIT_CRITICAL(&s_mux);
    /* The voice the repeater in focus transmits, as it is played: the thin
     * line on the face. Outside s_mux: the audio's own lock. */
    o->rx_level_db = tc_audio_level();
    o->spk_muted   = tc_audio_muted();
    snprintf(o->mode, sizeof o->mode, "fm");
}
