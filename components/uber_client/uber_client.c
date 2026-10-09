/* UberSDR client: the ubersdr firmware's radio.h -- a web receiver, listened
 * to through its own protocol, as its own web page listens (ka9q_ubersdr
 * 0.1.66; ubersdr-ntp's client for the sequence that is known to work):
 *
 *   GET  /api/description        what it is and what it has
 *   POST /connection             {"user_session_id":<uuid>,"password":<pw>}
 *   wss  /ws?frequency=..&mode=..&bandwidthLow=..&bandwidthHigh=..
 *            &format=opus&version=4&user_session_id=<uuid>[&password=..]
 *            (ws, in the clear, to a receiver on the LAN)
 *   <-   binary: an Opus packet of 20 ms behind a small header -- flags, a
 *        time stamp, on a resync the rate, and the signal and noise power
 *   ->   {"type":"tune",...} {"type":"set_dsp",...} {"type":"ping"}
 *
 * Receive only: nothing here keys anything, and PTT is refused.
 *
 * The knob may list up to four receivers (net_prov's: the configuration
 * page's list). The client starts with the one in use; one it cannot reach
 * -- its description not read: the name not found, no answer, refused, TLS
 * failed, only its tunnel answering -- hands over to the next in the list,
 * in turn, wrapping round, the one in use after a fair chance: two tries,
 * some seconds apart. The first that answers plays, and is the one in use
 * from then on. The one in use that answers but will not have us -- full,
 * busy, its day spent, a password refused, its time up -- keeps its turn:
 * that is the receiver's word, not a receiver gone. A stand-in that answers
 * so, one the operator never chose, is passed by for the next. No restart:
 * the session and the spots' task go on to the next.
 *
 * The session task's stack is in PSRAM, so it never touches flash: what is
 * kept across a restart (the frequency, the mode, the filter, the receiver
 * in use) is saved by a timer. See uber_aux.c for the spots and the SSTV
 * pictures. */
#include "radio.h"
#include "uber.h"
#include "uber_json.h"
#include "uber_priv.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <opus.h>

#include "audio_out.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "net_prov.h"
#include "kvstore.h"
#include "ptt_fsm.h"
#include "vfo_tune.h"

static const char *TAG = "uber";

#define LOOP_MS        20
#define QUIET_MS       10000      /* no audio this long: the session is gone */
#define TUNE_GAP_MS    50         /* the server paces radiod at 25 ms: coalesce */
#define DSP_SETTLE_MS  500        /* the filter editor rests this long first */
#define DSP_GAP_MS     1100       /* ...and the server wants 1 s between starts */
#define PING_GAP_MS    10000      /* activity pings, at most this often */
#define WS_RX_BYTES    (16 * 1024)
#define PCM_MAX        2880       /* 120 ms at 24 kHz: Opus's longest */
#define IDLE_WARN_S    60         /* an idle limit's last minute: on the face */
#define OVERRUN_S      60         /* listening this long past the count: it was not the receiver's */
#define FAIR_TRIES     2          /* the receiver in use, tried before the next ... */
#define FAIR_GAP_MS    4000       /* ...this far apart: a blip of WiFi or DNS passes */
#define SAID_MS        2000       /* a receiver not reached, said this long before the next */

/* Its modes, and the passband each opens with: UberSDR's own. */
static const struct { const char *m; int16_t lo, hi; } MODES[] = {
    { "usb",    50,  2700 }, { "lsb", -2700,  -50 }, { "cwu",  -200,  200 },
    { "cwl",  -200,   200 }, { "am",  -5000, 5000 }, { "sam", -5000, 5000 },
    { "fm",  -8000,  8000 }, { "nfm", -5000, 5000 },
};
#define N_MODES ((int)(sizeof MODES / sizeof MODES[0]))

/* The receiver: the session task's own to change, under S_LOCK, between
 * sessions; the spots' task and the page have it by uber_rx(). */
static uhost_t g_uh;
static char s_pass[33];

/* The knob's list of receivers (net_prov): the one the client is on, by its
 * place in the list, its name as the dial has it and its address as the
 * list has it -- -1 for a receiver not from the list, which the client keeps
 * to. s_in_use: the one in use as the client knows it -- the one it started
 * with, then each it saved (use_cb) -- not one chosen on the page meanwhile,
 * for the next boot. s_from: the one in use as a hand-over began, -1 while
 * none goes on. s_rx_gen moves on with each hand-over. */
static int      s_at = -1, s_from = -1;
static volatile int s_in_use = -1;
EXT_RAM_BSS_ATTR static char s_at_name[24];
EXT_RAM_BSS_ATTR static char s_at_host[64];
static uint16_t s_at_port;
static uint32_t s_rx_gen;

/* What the receiver says of itself, and its noise filters. */
static uber_info_t I;
static int64_t     s_fmin = 10000, s_fmax = 30000000;
static int64_t     s_fdef = 14175000;
static char        s_mdef[6] = "usb";
static uint8_t     s_n_nr;
static char        s_nr[RADIO_GAIN_NAMES - 1][6];

static portMUX_TYPE S_LOCK = portMUX_INITIALIZER_UNLOCKED;
static struct {
    radio_link_t link;
    tune_t   tune;
    int64_t  f_server;
    char     mode[6];
    int32_t  lo, hi;
    uint32_t gen;                 /* the dial moved: the receiver is told */
    uint32_t t_input;             /* when the dial last moved it */
    int8_t   nr_want, nr_on;      /* 0 off, else 1 + the filter's place */
    uint32_t t_nr_want;
    float    power, snr;          /* dBFS, dB */
    bool     have_power, have_snr;
    uint32_t snr_seq;             /* each reading */
    char     note[16];
    uint32_t note_seq;
    char     why[16];             /* why there is no link */
    bool     time_up;             /* the session was ended: the operator says when again */
    bool     relisten;            /* ...and did */
    uint32_t choices_seq;
    char     uuid[37];
    uint32_t uuid_gen;
    /* The session's limits, as the receiver counts them (uber_time_left):
     * when its first socket opened, our last word to it, the day's
     * allowance -- its end while a socket is open, else what is left.
     * Each registration says the day's before a socket can open. */
    int64_t  t_first, t_said, day_end;
    int32_t  day_left;            /* -1: none */
    int32_t  idle_s;              /* an idle limit that can end it first; 0 none */
    bool     overrun;             /* it outlived the count: the count is not shown */
    char     ended;               /* the socket closed on 0:00, whose: it stays there */
    int64_t  srv_ns, srv_at;      /* the receiver's clock, and when we read it */
    uint32_t connects, closes, frames, dropped, texts;
    char     last_close[48];
    bool     activity;
    bool     suspend;
} S = { .link = RADIO_LINK_DOWN };

static esp_timer_handle_t s_save_t;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

/* Another receiver in the list to hand over to. */
static bool others(void) { return s_at >= 0 && net_prov_radio_count() > 1; }

static int mode_index(const char *m)
{
    for (int i = 0; i < N_MODES; i++) if (m && !strcasecmp(MODES[i].m, m)) return i;
    return -1;
}

/* The radio's names, as the dial and the page may use them. */
static const char *mode_of(const char *m)
{
    if (!m) return NULL;
    if (mode_index(m) >= 0) return MODES[mode_index(m)].m;
    if (!strcasecmp(m, "cw") || !strcasecmp(m, "digu")) return !strcasecmp(m, "cw") ? "cwu" : "usb";
    if (!strcasecmp(m, "cwr")) return "cwl";
    if (!strcasecmp(m, "digl")) return "lsb";
    return NULL;
}

static void note(const char *n)
{
    taskENTER_CRITICAL(&S_LOCK);
    strlcpy(S.note, n, sizeof S.note);
    S.note_seq++;
    taskEXIT_CRITICAL(&S_LOCK);
}

static void set_link(radio_link_t l, const char *why)
{
    /* With other receivers listed, the face names the one it is about: on
     * the way to it, CONNECTING. */
    if ((!why || !why[0]) && (l == RADIO_LINK_CONNECTING || l == RADIO_LINK_GREETING) && others())
        why = "CONNECTING";
    taskENTER_CRITICAL(&S_LOCK);
    S.link = l;
    strlcpy(S.why, why ? why : "", sizeof S.why);
    taskEXIT_CRITICAL(&S_LOCK);
}

/* ------------------------------------------------------------ remembered */

#define NVS_NS "vfo"

static void save_cb(void *arg)
{
    (void)arg;
    int64_t f;
    char m[6], nr[6] = "";
    int32_t lo, hi;
    taskENTER_CRITICAL(&S_LOCK);
    f = S.tune.f_display;
    strlcpy(m, S.mode, sizeof m);
    lo = S.lo;
    hi = S.hi;
    if (S.nr_want > 0 && S.nr_want <= s_n_nr) strlcpy(nr, s_nr[S.nr_want - 1], sizeof nr);
    taskEXIT_CRITICAL(&S_LOCK);
    kv_handle_t h;
    if (kv_open(NVS_NS, &h) != ESP_OK) return;
    kv_edit_begin(h);
    kv_set_i64(h, "ubf", f);
    kv_set_str(h, "ubm", m);
    kv_set_i32(h, "ubl", lo);
    kv_set_i32(h, "ubh", hi);
    kv_set_str(h, "ubn", nr);
    kv_edit_end(h);
    kv_commit(h);
    kv_close(h);
}

static void save_later(void)
{
    if (!s_save_t) return;
    esp_timer_stop(s_save_t);
    esp_timer_start_once(s_save_t, 3 * 1000 * 1000);
}

static char s_nr_saved[6];             /* the filter at the last boot, by name */

static void load(void)
{
    kv_handle_t h;
    int64_t f = 0;
    char m[8] = "";
    int32_t lo = 0, hi = 0;
    if (kv_open(NVS_NS, &h) == ESP_OK) {
        size_t n = sizeof m;
        kv_get_i64(h, "ubf", &f);
        kv_get_str(h, "ubm", m, &n);
        kv_get_i32(h, "ubl", &lo);
        kv_get_i32(h, "ubh", &hi);
        n = sizeof s_nr_saved;
        kv_get_str(h, "ubn", s_nr_saved, &n);
        kv_close(h);
    }
    taskENTER_CRITICAL(&S_LOCK);
    tune_init(&S.tune, f > 0 ? f : 0, 1000);
    const int mi = mode_index(m);
    strlcpy(S.mode, mi >= 0 ? MODES[mi].m : "", sizeof S.mode);
    S.lo = lo;
    S.hi = hi;
    if (mi < 0 || lo >= hi) {
        S.lo = mi >= 0 ? MODES[mi].lo : 0;
        S.hi = mi >= 0 ? MODES[mi].hi : 0;
    }
    taskEXIT_CRITICAL(&S_LOCK);
}

/* ------------------------------------------------------------ description */

static bool read_description(char *why, size_t wn)
{
    const size_t cap = 24 * 1024;
    char *b = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
    if (!b) {
        snprintf(why, wn, "no memory");
        return false;
    }
    size_t n = 0;
    const int st = unet_http(&g_uh, "GET", "/api/description", NULL, b, cap - 1, &n, 12000, why, wn);
    bool ok = false;
    /* Its JSON -- a page of HTML instead is no description: a tunnel's,
     * say, for a receiver not there. */
    if (st == 200 && n > 2 && b[strspn(b, " \t\r\n")] == '{') {
        const char *e = b + n;
        uber_info_t in = { .known = true };
        const char *rcv = jkey(b, e, "receiver");
        jo_str(rcv, e, "name", in.name, sizeof in.name);
        jo_str(rcv, e, "callsign", in.callsign, sizeof in.callsign);
        jo_str(rcv, e, "location", in.location, sizeof in.location);
        jo_str(b, e, "version", in.version, sizeof in.version);
        in.max_clients   = (int)jo_num(b, e, "max_clients", 0);
        in.available     = (int)jo_num(b, e, "available_clients", 0);
        in.max_session_s = (int)jo_num(b, e, "max_session_time", 0);
        in.spots = jo_bool(b, e, "dx_cluster") || jo_bool(b, e, "cw_skimmer");
        in.voice = jo_bool(b, e, "noise_floor");
        const char *tr = jkey(b, e, "tuning_range");
        const int64_t fmin = (int64_t)jo_num(tr, e, "min_frequency", 10000);
        const int64_t fmax = (int64_t)jo_num(tr, e, "max_frequency", 30000000);
        const int64_t fdef = (int64_t)jo_num(b, e, "default_frequency", 14175000);
        char mdef[8] = "";
        jo_str(b, e, "default_mode", mdef, sizeof mdef);
        /* The noise filters it runs, in its order: the dial offers those. */
        uint8_t nn = 0;
        char nr[RADIO_GAIN_NAMES - 1][6];
        const char *dsp = jkey(b, e, "dsp");
        if (jo_bool(dsp, e, "enabled")) {
            const char *it = jkey(dsp, e, "filters"), *v;
            while (it && nn < RADIO_GAIN_NAMES - 1 && (v = jnext(&it, e)))
                if (jstr(v, e, nr[nn], sizeof nr[nn]) && nr[nn][0]) nn++;
        }
        const char *it = jkey(b, e, "addons"), *v;
        char a[16];
        while (it && (v = jnext(&it, e)))
            if (jstr(v, e, a, sizeof a) && !strcmp(a, "sstv")) in.sstv = true;
        taskENTER_CRITICAL(&S_LOCK);
        I = in;
        s_fmin = fmin > 0 ? fmin : 10000;
        s_fmax = fmax > s_fmin ? fmax : 30000000;
        s_fdef = fdef >= s_fmin && fdef <= s_fmax ? fdef : 14175000;
        if (mode_index(mdef) >= 0) strlcpy(s_mdef, MODES[mode_index(mdef)].m, sizeof s_mdef);
        s_n_nr = nn;
        memcpy(s_nr, nr, sizeof s_nr);
        taskEXIT_CRITICAL(&S_LOCK);
        ESP_LOGI(TAG, "%s (%s, %s) %s: %d of %d free, %d s a session; spots %s, voice %s, "
                      "SSTV %s, %u noise filters",
                 in.name, in.callsign, in.location, in.version, in.available, in.max_clients,
                 in.max_session_s, in.spots ? "yes" : "no", in.voice ? "yes" : "no",
                 in.sstv ? "yes" : "no", (unsigned)nn);
        ok = true;
    } else if (st > 0) {
        if (st == 200) snprintf(why, wn, "no description");
        else           snprintf(why, wn, "HTTP %d", st);
    }
    free(b);
    return ok;
}

void uber_info(uber_info_t *out)
{
    if (!out) return;
    taskENTER_CRITICAL(&S_LOCK);
    *out = I;
    taskEXIT_CRITICAL(&S_LOCK);
}

/* ---------------------------------------------------------------- session */

static void new_uuid(void)
{
    uint8_t r[16];
    esp_fill_random(r, sizeof r);
    r[6] = (r[6] & 0x0F) | 0x40;                   /* version 4 */
    r[8] = (r[8] & 0x3F) | 0x80;                   /* variant */
    char u[37];
    snprintf(u, sizeof u, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], r[8], r[9], r[10], r[11], r[12],
             r[13], r[14], r[15]);
    taskENTER_CRITICAL(&S_LOCK);
    strlcpy(S.uuid, u, sizeof S.uuid);
    S.uuid_gen++;
    /* A new session, to the receiver: its time counts from its first socket. */
    S.t_first = 0;
    S.overrun = false;
    S.ended = 0;
    taskEXIT_CRITICAL(&S_LOCK);
}

bool uber_session_id(char *out, size_t cap, uint32_t *gen)
{
    taskENTER_CRITICAL(&S_LOCK);
    const bool ok = S.uuid[0] && S.link == RADIO_LINK_READY;
    strlcpy(out, ok ? S.uuid : "", cap);
    if (gen) *gen = S.uuid_gen;
    taskEXIT_CRITICAL(&S_LOCK);
    return ok;
}

int64_t uber_server_now(void)
{
    int64_t ns, at;
    taskENTER_CRITICAL(&S_LOCK);
    ns = S.srv_ns;
    at = S.srv_at;
    taskEXIT_CRITICAL(&S_LOCK);
    if (!ns) return 0;
    return (ns + (esp_timer_get_time() - at) * 1000) / 1000000000LL;
}

void uber_dial(int64_t *hz, char *mode, size_t cap)
{
    taskENTER_CRITICAL(&S_LOCK);
    if (hz) *hz = S.tune.f_display;
    if (mode) strlcpy(mode, S.mode, cap);
    taskEXIT_CRITICAL(&S_LOCK);
}

/* ------------------------------------------------------------ time left */

/* How UberSDR ends a guest's session (ka9q_ubersdr 0.1.66: session.go,
 * main.go, ip_daily_time.go), and so how it is counted here:
 *  - max_session_time runs from the moment the receiver first saw the
 *    session's UUID on a socket, by the clock: a reconnect under the same
 *    UUID, the dial, a ping -- nothing moves it. Checked every second; then
 *    the UUID is shut out for an hour (TIME UP), and only a new one, LISTEN
 *    AGAIN's, counts afresh. The receiver says the limit, never the time
 *    used.
 *  - max_daily_time_per_ip, where its owner set one: an address's time in
 *    the last 24 hours, counted while one of its sockets is open, checked
 *    every 30 s. /connection says what is left of it.
 *  - session_timeout: a socket that has said nothing for so long. Every
 *    message counts, a ping too, and the knob pings only while someone uses
 *    it (radio_user_activity). /connection says it -- or, where there is
 *    none, the session's limit in its place, which can never end a session
 *    first.
 * A private address and the password are let past all three. */

/* The time left in us, and whose (S_LOCK held): INT64_MAX where no limit
 * applies; below zero once the count has run out. */
static int64_t left_us(int64_t now, char *why)
{
    int64_t l = INT64_MAX;
    *why = 0;
    if (S.time_up || S.overrun || !S.t_first) return l;
    /* Run out as the socket closed: 0:00 until the receiver says TIME UP,
     * or another socket opens after all. */
    if (S.ended) {
        *why = S.ended;
        return 0;
    }
    if (I.max_session_s > 0) {
        l = S.t_first + I.max_session_s * 1000000LL - now;
        *why = 'S';
    }
    const int64_t d = S.day_end ? S.day_end - now : S.day_left >= 0 ? S.day_left * 1000000LL : INT64_MAX;
    if (d < l) {
        l = d;
        *why = 'D';
    }
    /* The idle limit only in its last minute: a touch gives it back. */
    if (S.idle_s && S.link == RADIO_LINK_READY) {
        const int64_t i = S.t_said + S.idle_s * 1000000LL - now;
        if (i < l && i < IDLE_WARN_S * 1000000LL) {
            l = i;
            *why = 'I';
        }
    }
    return l;
}

int uber_time_left(char *why)
{
    const int64_t now = esp_timer_get_time();
    char k;
    taskENTER_CRITICAL(&S_LOCK);
    const int64_t l = left_us(now, &k);
    taskEXIT_CRITICAL(&S_LOCK);
    if (why) *why = k;
    if (l == INT64_MAX) return -1;
    /* Whole seconds, rounded up as UberSDR's page counts them: 0:00 is the end. */
    return l > 0 ? (int)((l + 999999) / 1000000) : 0;
}

/* What a refusal means for the next try -- and R_GONE, none: the receiver
 * not reached at all (the name not found, no answer, refused, TLS failed, or
 * only its tunnel answering for it). */
enum { R_OK = 0, R_RETRY, R_WAIT, R_REREGISTER, R_NEW_UUID, R_TIME_UP, R_STOP, R_GONE };

/* A receiver not reached, by unet's word for why: whether it was the knob's
 * own trouble -- no network, no free socket, no memory -- which no other
 * receiver would mend, or the receiver's own word ("HTTP 429": busy). */
static bool own_trouble(const char *w)
{
    return !net_prov_is_connected() || !strcmp(w, "no free socket") || !strcmp(w, "no memory") ||
           !strcmp(w, "HTTP 429");
}

static int classify(int status, const char *text, char *why, size_t wn)
{
    char t[96];
    size_t i = 0;
    for (; text && text[i] && i + 1 < sizeof t; i++) t[i] = (char)tolower((unsigned char)text[i]);
    t[i] = 0;
    /* The password's two answers come with a 403 too: theirs before it. */
    if (strstr(t, "requires a password"))    { snprintf(why, wn, "PASSWORD?");     return R_STOP; }
    if (strstr(t, "invalid bypass password")){ snprintf(why, wn, "WRONG PASSWORD"); return R_STOP; }
    if (strstr(t, "banned") || strstr(t, "access denied") || status == 401 || status == 403)
                                             { snprintf(why, wn, "REFUSED");       return R_STOP; }
    if (status == 410 || strstr(t, "terminated")) { snprintf(why, wn, "TIME UP"); return R_TIME_UP; }
    if (strstr(t, "invalid session"))        { snprintf(why, wn, "NO LINK");       return R_REREGISTER; }
    if (strstr(t, "user_session_id") || strstr(t, "no active audio session"))
                                             { snprintf(why, wn, "NO LINK");       return R_NEW_UUID; }
    if (strstr(t, "daily time limit"))       { snprintf(why, wn, "DAY LIMIT");     return R_WAIT; }
    if (strstr(t, "maximum") || status == 503){ snprintf(why, wn, "RECEIVER FULL"); return R_WAIT; }
    if (status == 429 || strstr(t, "rate limit") || strstr(t, "too many"))
                                             { snprintf(why, wn, "BUSY");          return R_WAIT; }
    snprintf(why, wn, "NO LINK");
    return R_RETRY;
}

static int register_session(char *why, size_t wn)
{
    char body[160], ans[512], uuid[37];
    taskENTER_CRITICAL(&S_LOCK);
    strlcpy(uuid, S.uuid, sizeof uuid);
    taskEXIT_CRITICAL(&S_LOCK);
    if (s_pass[0]) {
        char pw[80];
        size_t o = 0;
        for (const char *p = s_pass; *p && o + 2 < sizeof pw; p++) {
            if (*p == '"' || *p == '\\') pw[o++] = '\\';
            pw[o++] = *p;
        }
        pw[o] = 0;
        snprintf(body, sizeof body, "{\"user_session_id\":\"%s\",\"password\":\"%s\"}", uuid, pw);
    } else {
        snprintf(body, sizeof body, "{\"user_session_id\":\"%s\"}", uuid);
    }
    size_t n = 0;
    char w[32];
    const int st = unet_http(&g_uh, "POST", "/connection", body, ans, sizeof ans - 1, &n, 12000, w, sizeof w);
    if (st < 0) {
        /* Not reached: where the list has another, its turn may come. */
        snprintf(why, wn, "%s", !strcmp(w, "name not found") ? "NOT FOUND" : "NO ANSWER");
        ESP_LOGW(TAG, "/connection: %s", w);
        return own_trouble(w) ? R_RETRY : R_GONE;
    }
    const char *e = ans + n;
    if (st == 200 && jo_bool(ans, e, "allowed")) {
        const bool byp = jo_bool(ans, e, "bypassed");
        const int  mst = byp ? 0 : (int)jo_num(ans, e, "max_session_time", 0);
        /* An idle limit only where it can end a session before its limit
         * does: where there is none, UberSDR says the session's limit. */
        int idle = byp ? 0 : (int)jo_num(ans, e, "session_timeout", 0);
        if (idle < 0 || (mst && idle >= mst)) idle = 0;
        const int day = byp ? -1 : (int)jo_num(ans, e, "daily_time_remaining_secs", -1);
        taskENTER_CRITICAL(&S_LOCK);
        I.bypassed = byp;
        I.max_session_s = mst;
        S.idle_s   = idle;
        S.day_left = day >= 0 ? day : -1;
        taskEXIT_CRITICAL(&S_LOCK);
        char lim[96] = "";
        size_t o = 0;
        if (mst)      o += snprintf(lim + o, sizeof lim - o, ", %d s a session", mst);
        if (idle)     o += snprintf(lim + o, sizeof lim - o, ", idle after %d s", idle);
        if (day >= 0) snprintf(lim + o, sizeof lim - o, ", %d s left today", day);
        ESP_LOGI(TAG, "session %.8s registered%s%s", uuid, byp ? ", bypassed" : lim[0] ? " -- a guest" : "",
                 lim);
        return R_OK;
    }
    char reason[96] = "";
    jo_str(ans, e, "reason", reason, sizeof reason);
    ESP_LOGW(TAG, "/connection: %d %s", st, reason);
    /* No answer of an UberSDR's -- it says "allowed", always, a full one's
     * 503 too -- but of what stands in front of one, its tunnel or a proxy,
     * the receiver behind it gone ("502 Bad Gateway", "503 Service
     * Unavailable"): not reached. A refusal by its status alone is still the
     * receiver's: its ban comes without "allowed" (403, a page or
     * {"error":...}), and so may a rate limit (429). */
    if (!jkey(ans, e, "allowed") && st != 401 && st != 403 && st != 410 && st != 429) {
        snprintf(why, wn, "NO ANSWER");
        return R_GONE;
    }
    return classify(st, reason, why, wn);
}

/* ------------------------------------------------------------ the stream */

static uws_t s_ws;
static OpusDecoder *s_dec;
static int16_t *s_pcm;

typedef struct {
    int64_t ts_ns;
    int16_t power, noise;           /* centi-dB; -32768 none */
} v4_t;

/* UberSDR's version-4 header in front of an Opus packet: see pcm_v4_header.go. */
static bool v4(const uint8_t *p, size_t n, size_t *off, v4_t *h, bool *abs_ts, bool *quality)
{
    size_t o = 0;
    if (!n) return false;
    const uint8_t fl = p[o++];
    if (fl & 0xFC) return false;
    *abs_ts = fl & 2;
    *quality = fl & 1;
    if (fl & 2) {
        if (n < o + 8) return false;
        uint64_t t = 0;
        for (int i = 7; i >= 0; i--) t = t << 8 | p[o + i];
        h->ts_ns = (int64_t)t;
        o += 8;
    } else {
        uint64_t u = 0;
        for (int sh = 0; o < n; sh += 7) {
            const uint8_t b = p[o++];
            u |= (uint64_t)(b & 0x7F) << sh;
            if (!(b & 0x80)) break;
            if (sh > 63) return false;
        }
        h->ts_ns += (int64_t)(u >> 1) ^ -(int64_t)(u & 1);
    }
    if (fl & 2) {
        while (o < n && (p[o] & 0x80)) o++;         /* the rate: the decoder needs none */
        o += 2;                                     /* its last byte, and the channels */
    }
    if (fl & 1) {
        if (n < o + 4) return false;
        h->power = (int16_t)(p[o] | p[o + 1] << 8);
        h->noise = (int16_t)(p[o + 2] | p[o + 3] << 8);
        o += 4;
    }
    if (o > n) return false;
    *off = o;
    return true;
}

static void on_audio(const uint8_t *p, size_t n, v4_t *h)
{
    /* PCM (IQ modes, or the server's fallback) is not asked for: let it go. */
    if (n >= 4 && (!memcmp(p, "PCM4", 4) || !memcmp(p, "\x28\xB5\x2F\xFD", 4))) {
        S.dropped++;
        return;
    }
    size_t off;
    bool abs_ts, q;
    if (!v4(p, n, &off, h, &abs_ts, &q)) {
        S.dropped++;
        return;
    }
    if (abs_ts || q) {
        taskENTER_CRITICAL(&S_LOCK);
        if (abs_ts) {
            S.srv_ns = h->ts_ns;
            S.srv_at = esp_timer_get_time();
        }
        if (q) {
            S.have_power = h->power != -32768;
            S.have_snr   = h->power != -32768 && h->noise != -32768;
            if (S.have_power) S.power = h->power / 100.0f;
            if (S.have_snr)   S.snr = (h->power - h->noise) / 100.0f;
            S.snr_seq++;
        }
        taskEXIT_CRITICAL(&S_LOCK);
    }
    if (n <= off || S.suspend) return;
    const int m = opus_decode(s_dec, p + off, (opus_int32)(n - off), s_pcm, PCM_MAX, 0);
    if (m > 0) audio_out_feed_pcm16(s_pcm, (size_t)m, 1);
    else       S.dropped++;
    S.frames++;
}

static int s_tunes_out;                    /* tunes sent, their statuses not yet back */
static int8_t s_nr_sent;                   /* the filter last asked for, on this socket */

/* The stream's rate, which the noise filter is built for. */
static int rate_of(const char *m)
{
    return !strcmp(m, "am") || !strcmp(m, "sam") || !strcmp(m, "fm") || !strcmp(m, "nfm") ? 24000 : 12000;
}

static void on_text(const char *j, size_t n, char *close_why, size_t cwn)
{
    const char *e = j + n;
    char type[24];
    if (!jo_str(j, e, "type", type, sizeof type)) return;
    S.texts++;
    if (!strcmp(type, "status")) {
        const int64_t f = (int64_t)jo_num(j, e, "frequency", 0);
        char m[8] = "";
        jo_str(j, e, "mode", m, sizeof m);
        if (s_tunes_out > 0) s_tunes_out--;
        taskENTER_CRITICAL(&S_LOCK);
        if (f > 0) S.f_server = f;
        /* The receiver's word, once nothing of ours is on its way: a
         * frequency it clamped, a mode it would not take. */
        if (!s_tunes_out && f > 0 && f != S.tune.f_display && now_ms() - S.t_input > 1500)
            tune_assign(&S.tune, f);
        if (!s_tunes_out && mode_index(m) >= 0 && strcmp(m, S.mode)) strlcpy(S.mode, m, sizeof S.mode);
        taskEXIT_CRITICAL(&S_LOCK);
    } else if (!strcmp(type, "error")) {
        char err[96] = "";
        jo_str(j, e, "error", err, sizeof err);
        ESP_LOGW(TAG, "receiver: %s", err);
        /* Before a close, the reason for it. */
        strlcpy(close_why, err, cwn);
        if (strcasestr(err, "rate limit")) note("SLOW DOWN");
        else if (strcasestr(err, "out of valid range") || strcasestr(err, "out of range")) note("OUT OF RANGE");
    } else if (!strcmp(type, "dsp_status")) {
        const char *info = jkey(j, e, "info");
        char f[8] = "";
        jo_str(info, e, "filter", f, sizeof f);
        int8_t on = 0;
        if (jo_bool(info, e, "enabled"))
            for (int i = 0; i < s_n_nr; i++) if (!strcmp(f, s_nr[i])) on = (int8_t)(i + 1);
        taskENTER_CRITICAL(&S_LOCK);
        S.nr_on = on;
        taskEXIT_CRITICAL(&S_LOCK);
        ESP_LOGI(TAG, "noise filter %s", on ? f : "off");
    } else if (!strcmp(type, "dsp_error")) {
        const char *info = jkey(j, e, "info");
        char code[16] = "", msg[64] = "";
        jo_str(info, e, "code", code, sizeof code);
        jo_str(info, e, "message", msg, sizeof msg);
        ESP_LOGW(TAG, "noise filter refused: %s %s", code, msg);
        note(!strcmp(code, "CAPACITY") ? "FILTERS FULL" : "FILTER BUSY");
        /* The dial goes back to what runs; turned again, it asks again. */
        taskENTER_CRITICAL(&S_LOCK);
        S.nr_want = S.nr_on;
        taskEXIT_CRITICAL(&S_LOCK);
        s_nr_sent = S.nr_on;
    }
}

/* A message to the receiver on the audio socket: any of them is the
 * activity its idle limit counts. */
static bool say(const char *msg)
{
    const int64_t now = esp_timer_get_time();
    taskENTER_CRITICAL(&S_LOCK);
    S.t_said = now;
    taskEXIT_CRITICAL(&S_LOCK);
    return uws_text(&s_ws, msg);
}

static bool send_tune(const char *why_log)
{
    int64_t f;
    char m[6];
    int32_t lo, hi;
    taskENTER_CRITICAL(&S_LOCK);
    f = S.tune.f_display;
    strlcpy(m, S.mode, sizeof m);
    lo = S.lo;
    hi = S.hi;
    taskEXIT_CRITICAL(&S_LOCK);
    char msg[160];
    /* Integers only: the server reads the message into typed fields, and a
     * number it cannot take ends the session. */
    snprintf(msg, sizeof msg,
             "{\"type\":\"tune\",\"frequency\":%lld,\"mode\":\"%s\",\"bandwidthLow\":%ld,"
             "\"bandwidthHigh\":%ld}", (long long)f, m, (long)lo, (long)hi);
    ESP_LOGD(TAG, "%s: %s", why_log, msg);
    s_tunes_out++;
    return say(msg);
}

static bool send_dsp(int8_t want)
{
    char msg[112];
    if (want > 0 && want <= s_n_nr)
        snprintf(msg, sizeof msg, "{\"type\":\"set_dsp\",\"enabled\":true,\"filter\":\"%s\",\"params\":{}}",
                 s_nr[want - 1]);
    else
        snprintf(msg, sizeof msg, "{\"type\":\"set_dsp\",\"enabled\":false}");
    ESP_LOGI(TAG, "noise filter -> %s", want > 0 ? s_nr[want - 1] : "off");
    return say(msg);
}

/* The knob turned or the glass touched (radio.h): an UberSDR with an idle
 * timeout counts only that as listening, as its own page does -- a ping, at
 * most every PING_GAP_MS -- and it gives an idle limit's last minute back
 * (uber_time_left). */
void radio_user_activity(void) { S.activity = true; }

/* The time left as counted here, once a second while streaming: an idle
 * limit's last minute said as it begins and ends. A session listened to a
 * minute past its count -- the receiver's clock not ours after all (it was
 * restarted, say) -- is counted no more: no time shown, rather than a
 * wrong one. */
static void count_check(bool *idle_said)
{
    const int64_t now = esp_timer_get_time();
    char k;
    taskENTER_CRITICAL(&S_LOCK);
    const int64_t l = left_us(now, &k);
    const bool over = l != INT64_MAX && l < -OVERRUN_S * 1000000LL;
    if (over && k == 'I') S.idle_s = 0;
    else if (over)        S.overrun = true;
    taskEXIT_CRITICAL(&S_LOCK);
    if (over) {
        if (k == 'I') *idle_said = false;
        ESP_LOGW(TAG, "still listening %d s past the %s limit as counted here: counted no more",
                 (int)(-l / 1000000), k == 'I' ? "idle" : k == 'D' ? "day's" : "session's");
        return;
    }
    if ((k == 'I') != *idle_said) {
        *idle_said = k == 'I';
        if (*idle_said) ESP_LOGI(TAG, "idle: the receiver ends the session in %d s, unless the knob is used",
                                 (int)((l + 999999) / 1000000));
        else            ESP_LOGI(TAG, "the knob used: idle no more");
    }
}

/* One session on the audio socket, until it ends: why, as the refusal
 * classes have it. */
static int stream(char *why, size_t wn)
{
    char uuid[37], path[384], pw[100] = "";
    int64_t f;
    char m[6];
    int32_t lo, hi;
    taskENTER_CRITICAL(&S_LOCK);
    strlcpy(uuid, S.uuid, sizeof uuid);
    if (S.tune.f_display < s_fmin || S.tune.f_display > s_fmax) tune_init(&S.tune, s_fdef, S.tune.step_hz ? S.tune.step_hz : 1000);
    if (mode_index(S.mode) < 0) {
        const int mi = mode_index(s_mdef);
        strlcpy(S.mode, s_mdef, sizeof S.mode);
        S.lo = MODES[mi < 0 ? 0 : mi].lo;
        S.hi = MODES[mi < 0 ? 0 : mi].hi;
    }
    f = S.tune.f_display;
    strlcpy(m, S.mode, sizeof m);
    lo = S.lo;
    hi = S.hi;
    taskEXIT_CRITICAL(&S_LOCK);
    if (s_pass[0]) {
        size_t o = 0;
        o += snprintf(pw, sizeof pw, "&password=");
        for (const char *p = s_pass; *p && o + 4 < sizeof pw; p++) {
            if (isalnum((unsigned char)*p) || strchr("-_.~", *p)) pw[o++] = *p;
            else o += snprintf(pw + o, sizeof pw - o, "%%%02X", (unsigned char)*p);
        }
        pw[o] = 0;
    }
    snprintf(path, sizeof path,
             "/ws?frequency=%lld&mode=%s&bandwidthLow=%ld&bandwidthHigh=%ld&format=opus&version=4"
             "&user_session_id=%s%s", (long long)f, m, (long)lo, (long)hi, uuid, pw);
    char w[96];
    if (!uws_open(&s_ws, &g_uh, path, WS_RX_BYTES, 12000, w, sizeof w)) {
        ESP_LOGW(TAG, "audio socket: %s", w);
        strlcpy(S.last_close, w, sizeof S.last_close);
        return classify(atoi(w), w, why, wn);
    }
    ESP_LOGI(TAG, "audio socket open: %lld Hz %s %ld..%ld", (long long)f, m, (long)lo, (long)hi);
    /* The receiver's clocks: the session's from its first socket on, the
     * day's while one is open, the idle one from each word we send. */
    const int64_t t_open = esp_timer_get_time();
    taskENTER_CRITICAL(&S_LOCK);
    const bool first = !S.t_first;
    if (first) S.t_first = t_open;
    S.day_end = S.day_left >= 0 ? t_open + S.day_left * 1000000LL : 0;
    S.ended = 0;
    const int mst = I.max_session_s;
    taskEXIT_CRITICAL(&S_LOCK);
    if (first && mst) ESP_LOGI(TAG, "a guest's session: %d s from now", mst);
    say("{\"type\":\"get_status\"}");
    set_link(RADIO_LINK_GREETING, NULL);
    S.connects++;
    s_tunes_out = 0;

    v4_t h = { .power = -32768, .noise = -32768 };
    uint32_t sent_gen, t_tuned = 0, t_dsp = 0, t_ping = now_ms(), t_audio = now_ms(), t_count = 0;
    taskENTER_CRITICAL(&S_LOCK);
    sent_gen = S.gen;
    S.nr_on = 0;
    taskEXIT_CRITICAL(&S_LOCK);
    s_nr_sent = 0;
    int nr_rate = 0;
    bool streaming = false, idle_said = false;
    char close_why[96] = "";
    int end = R_RETRY;
    audio_out_flush();

    for (;;) {
        uint8_t op;
        const uint8_t *p;
        size_t n;
        const int r = uws_recv(&s_ws, LOOP_MS, &op, &p, &n);
        const uint32_t t = now_ms();
        if (r < 0) {
            if (s_ws.close_code) snprintf(w, sizeof w, "closed (%u)", (unsigned)s_ws.close_code);
            else                 snprintf(w, sizeof w, "connection lost");
            ESP_LOGW(TAG, "audio socket: %s%s%s", w, close_why[0] ? ": " : "", close_why);
            strlcpy(S.last_close, close_why[0] ? close_why : w, sizeof S.last_close);
            end = close_why[0] ? classify(0, close_why, why, wn) : R_RETRY;
            if (end == R_RETRY) snprintf(why, wn, "NO LINK");
            break;
        }
        if (r == 1 && op == 0x2) {
            on_audio(p, n, &h);
            t_audio = t;
            if (!streaming) {
                streaming = true;
                set_link(RADIO_LINK_READY, NULL);
                ESP_LOGI(TAG, "streaming");
            }
        } else if (r == 1 && op == 0x1) {
            on_text((const char *)p, n, close_why, sizeof close_why);
        }
        if (t - t_audio > QUIET_MS) {
            ESP_LOGW(TAG, "audio socket: nothing for %d s", QUIET_MS / 1000);
            strlcpy(S.last_close, "went quiet", sizeof S.last_close);
            snprintf(why, wn, "NO LINK");
            end = R_RETRY;
            break;
        }
        /* The dial: the newest of it, no oftener than the server paces. */
        if (S.gen != sent_gen && t - t_tuned >= TUNE_GAP_MS) {
            taskENTER_CRITICAL(&S_LOCK);
            sent_gen = S.gen;
            taskEXIT_CRITICAL(&S_LOCK);
            if (!send_tune("tune")) { snprintf(why, wn, "NO LINK"); break; }
            t_tuned = t;
            save_later();
        }
        /* The noise filter: once the editor rests on one, and not sooner
         * than the server takes another. A mode change can change the rate,
         * which the filter is built for: it goes again after one. */
        int8_t want;
        uint32_t t_want;
        int rate;
        taskENTER_CRITICAL(&S_LOCK);
        want = S.nr_want;
        t_want = S.t_nr_want;
        rate = rate_of(S.mode);
        taskEXIT_CRITICAL(&S_LOCK);
        if (s_nr_sent > 0 && rate != nr_rate) s_nr_sent = -1;
        if (streaming && want != s_nr_sent && t - t_want >= DSP_SETTLE_MS && t - t_dsp >= DSP_GAP_MS) {
            if (!send_dsp(want)) { snprintf(why, wn, "NO LINK"); break; }
            s_nr_sent = want;
            nr_rate = rate;
            t_dsp = t;
            save_later();
        }
        /* Someone is listening: a ping, the one thing an idle timer counts. */
        if (S.activity && t - t_ping >= PING_GAP_MS) {
            S.activity = false;
            t_ping = t;
            say("{\"type\":\"ping\"}");
        }
        /* The time left, once a second, for the log. */
        if (streaming && t - t_count >= 1000) {
            t_count = t;
            count_check(&idle_said);
        }
    }
    uws_close(&s_ws);
    S.closes++;
    /* A count run out stays at 0:00 (left_us). The day's clock stops with
     * the socket: what is left of it, until /connection says again. */
    const int64_t t_close = esp_timer_get_time();
    char k;
    taskENTER_CRITICAL(&S_LOCK);
    if (left_us(t_close, &k) <= 0) S.ended = k;
    if (S.day_end) {
        S.day_left = S.day_end > t_close ? (int32_t)((S.day_end - t_close) / 1000000) : 0;
        S.day_end = 0;
    }
    taskEXIT_CRITICAL(&S_LOCK);
    set_link(RADIO_LINK_DOWN, why);
    return end;
}

/* ---------------------------------------------------------- the receivers */

/* An address as the configuration page keeps it, into `u`: https://name,
 * through the tunnel, over TLS; http://host, a receiver on the LAN, in the
 * clear -- whatever the port. A name alone, as knobs kept it before: TLS on
 * 443, in the clear on any other. The port the address has, else the one
 * given, else its scheme's. */
static bool parse_host(uhost_t *u, const char *host, uint16_t port)
{
    uhost_t t = { 0 };
    const char *h = strstr(host, "://");
    const bool scheme = h != NULL;
    const bool tls    = scheme && (!strncasecmp(host, "https", 5) || !strncasecmp(host, "wss", 3));
    h = h ? h + 3 : host;
    const size_t hl = strcspn(h, ":/ ");
    if (!hl || hl >= sizeof t.host) return false;
    memcpy(t.host, h, hl);
    t.port = h[hl] == ':' ? (uint16_t)atoi(h + hl + 1) : 0;
    if (!t.port) t.port = port ? port : scheme && !tls ? 80 : 443;
    t.tls  = scheme ? tls : t.port == 443;
    *u = t;
    return true;
}

/* The receivers in the list with an address: a round, every one tried. */
static int listed(void)
{
    int k = 0;
    net_radio_t r;
    uhost_t u;
    for (int i = 0; i < net_prov_radio_count(); i++)
        if (net_prov_radio_get(i, &r) && r.host[0] && parse_host(&u, r.host, r.port)) k++;
    return k;
}

/* Receiver i of the list, at `u`, the one the client is on now: its address
 * and its password, its description read afresh, a session of its own;
 * nothing of the last one's kept but the dial, and the noise filter where
 * this one runs it too -- the one chosen on the last, or, its filters never
 * read (not reached at all), the one the last boot or hop left. Between
 * sessions: nothing plays. */
static void take(int i, const net_radio_t *r, const uhost_t *u)
{
    taskENTER_CRITICAL(&S_LOCK);
    if (s_n_nr) {
        if (S.nr_want > 0 && S.nr_want <= s_n_nr) strlcpy(s_nr_saved, s_nr[S.nr_want - 1], sizeof s_nr_saved);
        else                                     s_nr_saved[0] = 0;
    }
    g_uh = *u;
    memset(&I, 0, sizeof I);
    s_n_nr = 0;
    S.nr_want = S.nr_on = 0;
    S.f_server = 0;
    S.have_power = S.have_snr = false;
    S.srv_ns = S.srv_at = 0;
    S.idle_s = 0;
    S.day_left = -1;
    S.day_end = 0;
    s_at = i;
    strlcpy(s_at_name, r->name[0] ? r->name : net_prov_host_shown(r->host), sizeof s_at_name);
    strlcpy(s_at_host, r->host, sizeof s_at_host);
    s_at_port = r->port;
    s_rx_gen++;
    /* On the way to it: never the last one's state under its name. */
    S.link = RADIO_LINK_CONNECTING;
    strlcpy(S.why, "CONNECTING", sizeof S.why);
    taskEXIT_CRITICAL(&S_LOCK);
    strlcpy(s_pass, r->pass, sizeof s_pass);
    new_uuid();
}

/* The next receiver in the list after the one the client is on, in turn,
 * wrapping round, that has an address: the one the client is on now. */
static bool hand_over(void)
{
    const int n = net_prov_radio_count();
    for (int k = 1; k < n; k++) {
        const int i = (s_at + k) % n;
        net_radio_t r;
        uhost_t u;
        if (!net_prov_radio_get(i, &r) || !r.host[0] || !parse_host(&u, r.host, r.port)) continue;
        if (s_from < 0) s_from = s_in_use;
        take(i, &r, &u);
        return true;
    }
    return false;
}

/* The receiver handed over to, saved as the one in use by a timer: the
 * session's stack is in PSRAM, which must not touch flash. The timer's own
 * stack is small: nothing of the list goes on it. */
static esp_timer_handle_t s_use_t;
enum { USE_DUE = 0, USE_SAVED, USE_OTHER, USE_FAILED };
static volatile int  s_use_at = -1;
static volatile int  s_use_res;
static volatile esp_err_t s_use_err;
static volatile unsigned  s_use_stack;      /* the timer task's stack never used, after the write */
static int           s_use_from;
EXT_RAM_BSS_ATTR static char s_use_host[64];
static uint16_t      s_use_port;

/* On the timer's task: the one in use from now on -- the list keeps its
 * order -- unless another was chosen meanwhile, on the dial or the page, or
 * the list no longer holds it where it was. What came of it: s_use_res. */
static void use_cb(void *arg)
{
    (void)arg;
    EXT_RAM_BSS_ATTR static net_radio_t r;
    const int i = s_use_at;
    if (i >= 0 && net_prov_radio_active() == s_use_from && net_prov_radio_get(i, &r) &&
        !strcmp(r.host, s_use_host) && r.port == s_use_port) {
        s_use_err   = net_prov_radio_activate(i);
        s_use_stack = (unsigned)uxTaskGetStackHighWaterMark(NULL);
        /* The list's one in use, as the client goes by it from now on --
         * moved in RAM even where the write failed. */
        if (net_prov_radio_active() == i) s_in_use = i;
        s_use_res = s_use_err == ESP_OK ? USE_SAVED : USE_FAILED;
    } else {
        s_use_res = USE_OTHER;
    }
    s_use_at = -1;
}

/* Handed over to, and it lets the knob in: the one in use from now on, so
 * that the knob starts with it next time -- saved now, before its session:
 * the UberSDR not playing yet. (A KiwiSDR beside it may be: the firmware
 * only receives -- no over for a flash write to hold up -- and the dial's
 * own settings are saved while it plays too.) One that answers only to
 * turn the knob away, or whose session is never reached, is not: the one
 * in use stays what it was until another plays. */
static void in_use_now(void)
{
    const int from = s_from;
    if (from < 0) return;
    s_from = -1;
    if (s_at < 0 || s_at == from || !s_use_t) return;
    s_use_res = USE_DUE;
    s_use_from = from;
    strlcpy(s_use_host, s_at_host, sizeof s_use_host);
    s_use_port = s_at_port;
    s_use_at = s_at;
    if (esp_timer_start_once(s_use_t, 1) != ESP_OK) s_use_at = -1;
    for (int k = 0; k < 150 && s_use_at >= 0; k++) vTaskDelay(pdMS_TO_TICKS(20));
    switch (s_use_res) {
    case USE_SAVED:
        ESP_LOGI(TAG, "%s lets the knob in: in use from now on, saved (the timer's stack: %u bytes never used)",
                 s_at_name, s_use_stack);
        break;
    case USE_FAILED:
        ESP_LOGE(TAG, "%s lets the knob in: in use from now on, but not saved (%s)", s_at_name,
                 esp_err_to_name(s_use_err));
        break;
    case USE_OTHER:
        ESP_LOGW(TAG, "%s lets the knob in: not saved as the one in use -- another chosen meanwhile, or the "
                      "list changed", s_at_name);
        break;
    default:
        ESP_LOGW(TAG, "%s lets the knob in: not saved as the one in use -- the timer did not run", s_at_name);
        break;
    }
}

/* The receiver the client is on not reached, on a try: with others in the
 * list, the one in use again after FAIR_GAP_MS, the first time (`missed`,
 * the tries in a row); else the next in turn, its state said a moment
 * first -- and once every one has had its turn since one last answered
 * (`tried`), the backoff first, growing round by round as a single
 * receiver's does. The pause, taken here. */
static void not_reached(int *missed, int *tried, uint32_t *backoff)
{
    char was[24];
    strlcpy(was, s_at_name, sizeof was);
    if (s_at == s_in_use && ++*missed < FAIR_TRIES) {
        ESP_LOGW(TAG, "%s not reached: again in %d s before another", was, FAIR_GAP_MS / 1000);
        vTaskDelay(pdMS_TO_TICKS(FAIR_GAP_MS));
        return;
    }
    *missed = 0;
    if (++*tried >= listed()) {
        *tried = 0;
        ESP_LOGW(TAG, "%s not reached, nor any other in the list: again in %lu s", was,
                 (unsigned long)(*backoff / 1000));
        vTaskDelay(pdMS_TO_TICKS(*backoff));
        *backoff = *backoff < 30000 ? *backoff * 2 : 60000;
    } else {
        vTaskDelay(pdMS_TO_TICKS(SAID_MS));
    }
    if (hand_over())
        ESP_LOGW(TAG, "%s not reached: on to %s at %s://%s:%u", was, s_at_name, g_uh.tls ? "https" : "http",
                 g_uh.host, (unsigned)g_uh.port);
}

int uber_receiver(char *name, size_t cap)
{
    const bool named = others();
    taskENTER_CRITICAL(&S_LOCK);
    const int at = named ? s_at : -1;
    if (name && cap) strlcpy(name, named ? s_at_name : "", cap);
    taskEXIT_CRITICAL(&S_LOCK);
    return at;
}

uint32_t uber_rx(uhost_t *out)
{
    taskENTER_CRITICAL(&S_LOCK);
    *out = g_uh;
    const uint32_t g = s_rx_gen;
    taskEXIT_CRITICAL(&S_LOCK);
    return g;
}

static void session_task(void *arg)
{
    (void)arg;
    uint32_t backoff = 2000;
    char why[24] = "NO LINK";
    s_dec = opus_decoder_create(AUDIO_RATE_HZ, 1, NULL);
    s_pcm = heap_caps_malloc(PCM_MAX * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!s_dec || !s_pcm) {
        ESP_LOGE(TAG, "no memory for the audio decoder");
        set_link(RADIO_LINK_DOWN, "NO MEMORY");
        vTaskDelete(NULL);
    }
    new_uuid();
    uber_aux_start();
    /* After a socket closed on 0:00, one quick try -- another only after a
     * new session, or one that ran a while: a receiver that lets the
     * session go on (restarted, its clock not ours) while its socket fails
     * gets the backoff, as anything else, not a try every moment. */
    bool quick = true;
    /* The receiver not reached, tries in a row; the receivers tried since
     * one last answered (not_reached). */
    int missed = 0, tried = 0;
    for (;;) {
        set_link(RADIO_LINK_CONNECTING, NULL);
        if (!I.known) {
            char w[32] = "";
            if (!read_description(w, sizeof w)) {
                set_link(RADIO_LINK_DOWN, !strcmp(w, "name not found") ? "NOT FOUND" : "NO ANSWER");
                if (others() && !own_trouble(w)) {
                    ESP_LOGW(TAG, "%s, %s:%u: no description (%s)", s_at_name, g_uh.host, (unsigned)g_uh.port, w);
                    not_reached(&missed, &tried, &backoff);
                    continue;
                }
                ESP_LOGW(TAG, "%s:%u: no description (%s); again in %lu s", g_uh.host,
                         (unsigned)g_uh.port, w, (unsigned long)(backoff / 1000));
                vTaskDelay(pdMS_TO_TICKS(backoff));
                backoff = backoff < 30000 ? backoff * 2 : 60000;
                continue;
            }
            missed = tried = 0;
            /* The noise filter chosen at the last boot -- or on the receiver
             * handed over from -- if it runs here too. */
            taskENTER_CRITICAL(&S_LOCK);
            for (int i = 0; i < s_n_nr; i++)
                if (s_nr_saved[0] && !strcmp(s_nr_saved, s_nr[i])) S.nr_want = (int8_t)(i + 1);
            taskEXIT_CRITICAL(&S_LOCK);
        }
        int r = register_session(why, sizeof why);
        bool spent = false;
        /* A stand-in -- handed over to, not the one in use -- that answers
         * only to turn the knob away (full, busy, its day spent, a password)
         * is passed by for the next, as one not reached, its refusal said a
         * moment first: the operator's own keeps its turn. */
        if ((r == R_WAIT || r == R_STOP) && s_at != s_in_use && others()) {
            set_link(RADIO_LINK_DOWN, why);
            ESP_LOGW(TAG, "%s: %s -- a stand-in, so on to the next", s_at_name, why);
            not_reached(&missed, &tried, &backoff);
            continue;
        }
        /* It answered, even to refuse: it keeps its turn. */
        if (r != R_GONE && r != R_RETRY) missed = tried = 0;
        if (r == R_OK) {
            /* Handed over to, it lets the knob in: in use from now on. */
            in_use_now();
            const int64_t t0 = esp_timer_get_time();
            r = stream(why, sizeof why);
            /* A session that ran a while earns a quick return; one closed
             * on 0:00, at once -- the receiver ended it, most likely, and
             * says so: TIME UP. */
            if (esp_timer_get_time() - t0 > 30 * 1000000LL) {
                backoff = 2000;
                quick = true;
            }
            spent = r == R_RETRY && uber_time_left(NULL) == 0;
        } else {
            set_link(RADIO_LINK_DOWN, why);
        }
        switch (r) {
        case R_GONE:
            /* Not reached: with others in the list, the next one's turn may
             * come; a single receiver, again after the backoff, as ever. */
            if (others()) {
                not_reached(&missed, &tried, &backoff);
                continue;
            }
            break;
        case R_TIME_UP:
            /* Ended by the receiver -- its time limit, or its owner: another
             * session only when the operator asks, as its own page does. */
            ESP_LOGW(TAG, "session ended by the receiver: waiting for the operator");
            taskENTER_CRITICAL(&S_LOCK);
            S.time_up = true;
            S.relisten = false;
            S.choices_seq++;
            taskEXIT_CRITICAL(&S_LOCK);
            while (!S.relisten) vTaskDelay(pdMS_TO_TICKS(200));
            new_uuid();
            taskENTER_CRITICAL(&S_LOCK);
            S.time_up = false;
            taskEXIT_CRITICAL(&S_LOCK);
            backoff = 2000;
            quick = true;
            continue;
        case R_NEW_UUID:
            new_uuid();
            quick = true;
            break;
        case R_REREGISTER:
            break;
        case R_STOP:
            ESP_LOGW(TAG, "refused (%s): again in 5 min, or with a new setting", why);
            vTaskDelay(pdMS_TO_TICKS(5 * 60 * 1000));
            continue;
        case R_WAIT:
            ESP_LOGW(TAG, "%s: again in 30 s", why);
            vTaskDelay(pdMS_TO_TICKS(30000));
            continue;
        default:
            break;
        }
        if (spent && quick) {
            quick = false;
            ESP_LOGI(TAG, "closed on 0:00: again at once");
            continue;
        }
        ESP_LOGI(TAG, "again in %lu s", (unsigned long)(backoff / 1000));
        vTaskDelay(pdMS_TO_TICKS(backoff));
        backoff = backoff < 30000 ? backoff * 2 : 30000;
    }
}

/* ------------------------------------------------------------- radio.h */

const char *radio_link_name(void) { return g_uh.tls ? "WSS" : "WS"; }

esp_err_t radio_start(const char *host, uint16_t port, const char *user, const char *pass)
{
    (void)user;
    if (!host || !parse_host(&g_uh, host, port)) return ESP_ERR_INVALID_ARG;
    strlcpy(s_pass, pass ? pass : "", sizeof s_pass);
    /* The one in use in the knob's list, as this is: the others are handed
     * over to when it cannot be reached. (Static: the supervisor's stack,
     * this one's caller, has run out before.) */
    EXT_RAM_BSS_ATTR static net_radio_t r;
    const int a = net_prov_radio_active();
    if (net_prov_radio_get(a, &r) && !strcmp(r.host, host)) {
        s_at = s_in_use = a;
        strlcpy(s_at_name, r.name[0] ? r.name : net_prov_host_shown(r.host), sizeof s_at_name);
        strlcpy(s_at_host, r.host, sizeof s_at_host);
        s_at_port = r.port;
    }
    load();
    const esp_timer_create_args_t ta = { .callback = save_cb, .name = "ubsave" };
    esp_timer_create(&ta, &s_save_t);
    const esp_timer_create_args_t tu = { .callback = use_cb, .name = "ubuse" };
    esp_timer_create(&tu, &s_use_t);
    ESP_LOGI(TAG, "UberSDR at %s://%s:%u%s", g_uh.tls ? "https" : "http", g_uh.host,
             (unsigned)g_uh.port, s_pass[0] ? ", with its password" : "");
    if (others())
        ESP_LOGI(TAG, "%s, in use, of %d receivers in the list: the next in turn when it cannot be reached",
                 s_at_name, net_prov_radio_count());
    ESP_RETURN_ON_FALSE(xTaskCreatePinnedToCoreWithCaps(session_task, "uber", 16384, NULL, 5, NULL, 0,
                                                        MALLOC_CAP_SPIRAM) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "task");
    return ESP_OK;
}

int64_t radio_tune_by(int32_t detents, uint8_t accel_mult, int32_t step_hz)
{
    int64_t f;
    taskENTER_CRITICAL(&S_LOCK);
    if (detents) {
        if (S.tune.step_hz != step_hz) tune_set_step(&S.tune, step_hz);
        tune_apply(&S.tune, detents, accel_mult, LOOP_MS, s_fmin, s_fmax);
        S.gen++;
        S.t_input = now_ms();
        S.activity = true;
    }
    f = S.tune.f_display;
    taskEXIT_CRITICAL(&S_LOCK);
    return f;
}

void radio_set_step(int32_t step_hz)
{
    taskENTER_CRITICAL(&S_LOCK);
    const int64_t was = S.tune.f_display;
    tune_set_step(&S.tune, step_hz);
    /* The dial lands on the new step's grid: the receiver goes with it. */
    if (S.tune.f_display != was) {
        S.gen++;
        S.t_input = now_ms();
    }
    taskEXIT_CRITICAL(&S_LOCK);
}

void radio_audio_suspend(bool suspend)
{
    S.suspend = suspend;
    ESP_LOGW(TAG, "audio %s", suspend ? "suspended" : "resumed");
}

void radio_get_status(radio_status_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof *out);
    taskENTER_CRITICAL(&S_LOCK);
    out->link        = S.link;
    out->ptt_state   = PTT_IDLE;
    out->f_display   = S.tune.f_display;
    out->f_server    = S.f_server;
    strlcpy(out->mode, S.mode, sizeof out->mode);
    out->filt_lo     = S.lo;
    out->filt_hi     = S.hi;
    out->smeter_dbm  = S.have_power ? S.power : -127.0f;
    out->have_snr    = S.have_snr && S.link == RADIO_LINK_READY;
    out->snr_db      = S.snr;
    /* The noise filter, as a gain with names: OFF, then the receiver's. */
    out->have_gain   = s_n_nr > 0;
    out->gain        = S.nr_want;
    out->gain_min    = 0;
    out->gain_max    = (int8_t)s_n_nr;
    out->gain_step   = 1;
    out->n_gain_names = s_n_nr ? (uint8_t)(s_n_nr + 1) : 0;
    if (s_n_nr) {
        strlcpy(out->gain_names[0], "OFF", sizeof out->gain_names[0]);
        for (int i = 0; i < s_n_nr; i++)
            for (int k = 0; k < 5 && s_nr[i][k]; k++)
                out->gain_names[i + 1][k] = (char)toupper((unsigned char)s_nr[i][k]);
    }
    snprintf(out->model, sizeof out->model, "UberSDR");
    out->f_max       = s_fmax;
    strlcpy(out->note, S.note, sizeof out->note);
    out->note_seq    = S.note_seq;
    strlcpy(out->link_why, S.why, sizeof out->link_why);
    out->n_choices   = S.time_up ? 1 : 0;
    out->choices_seq = S.choices_seq;
    strlcpy(out->server, I.name, sizeof out->server);
    out->connects    = S.connects;
    out->closes      = S.closes;
    out->sends       = S.texts;
    out->echoes      = S.frames;
    out->rejects     = S.dropped;
    strlcpy(out->last_close, S.last_close, sizeof out->last_close);
    taskEXIT_CRITICAL(&S_LOCK);
}

bool radio_is_ready(void) { return S.link == RADIO_LINK_READY; }
bool radio_on_air(void) { return false; }

/* Receive only: there is nothing to key. */
void radio_ptt_key(void) {}
void radio_ptt_unkey(void) {}
void radio_ptt_toggle(void) {}
void radio_ptt_force_abort(uint8_t reason) { (void)reason; }

static void set_mode_locked(int mi)
{
    strlcpy(S.mode, MODES[mi].m, sizeof S.mode);
    S.lo = MODES[mi].lo;
    S.hi = MODES[mi].hi;
    S.gen++;
    S.t_input = now_ms();
    /* The filter is built for the stream's rate, which a mode can change. */
    if (S.nr_want) S.t_nr_want = now_ms();
}

void radio_set_mode(const char *mode)
{
    const int mi = mode_index(mode_of(mode));
    if (mi < 0) return;
    taskENTER_CRITICAL(&S_LOCK);
    if (strcmp(S.mode, MODES[mi].m)) set_mode_locked(mi);
    taskEXIT_CRITICAL(&S_LOCK);
}

void radio_set_filter(int32_t lo, int32_t hi)
{
    if (lo >= hi || lo < -12000 || hi > 12000) return;
    taskENTER_CRITICAL(&S_LOCK);
    S.lo = lo;
    S.hi = hi;
    S.gen++;
    S.t_input = now_ms();
    taskEXIT_CRITICAL(&S_LOCK);
}

void radio_select_filter(uint8_t n) { (void)n; }
void radio_set_rit(int32_t hz) { (void)hz; }
void radio_set_agc(const char *agc) { (void)agc; }

void radio_set_gain(int8_t gain)
{
    if (gain < 0) gain = 0;
    if (gain > s_n_nr) gain = (int8_t)s_n_nr;
    taskENTER_CRITICAL(&S_LOCK);
    if (S.nr_want != gain) {
        S.nr_want = gain;
        S.t_nr_want = now_ms();
    }
    taskEXIT_CRITICAL(&S_LOCK);
}

void radio_goto_freq(int64_t hz)
{
    if (hz < s_fmin || hz > s_fmax) return;
    taskENTER_CRITICAL(&S_LOCK);
    tune_assign(&S.tune, hz);
    /* A band's own sideband, for a voice mode: lower below 10 MHz. */
    if (!strcmp(S.mode, "usb") || !strcmp(S.mode, "lsb")) {
        char lbl[8];
        int64_t blo, bhi;
        if (uber_band_of(hz, &blo, &bhi, lbl, sizeof lbl) && strcmp(lbl, "60m")) {
            const int mi = mode_index(hz < 10000000 ? "lsb" : "usb");
            if (strcmp(S.mode, MODES[mi].m)) set_mode_locked(mi);
        }
    }
    S.gen++;
    S.t_input = now_ms();
    taskEXIT_CRITICAL(&S_LOCK);
}

void uber_tune_to(uint32_t hz, const char *mode)
{
    if (hz < s_fmin || hz > s_fmax) return;
    const int mi = mode_index(mode_of(mode));
    taskENTER_CRITICAL(&S_LOCK);
    tune_assign(&S.tune, hz);
    if (mi >= 0 && strcmp(S.mode, MODES[mi].m)) set_mode_locked(mi);
    S.gen++;
    S.t_input = now_ms();
    taskEXIT_CRITICAL(&S_LOCK);
}

void radio_memory_mode(bool on) { (void)on; }
void radio_memory_group(uint8_t group) { (void)group; }
void radio_select_rx(uint8_t rx) { (void)rx; }
void radio_set_antenna(uint8_t ant, bool rx_ant) { (void)ant; (void)rx_ant; }
void radio_tune(void) {}
void radio_atu_tune(void) {}
void radio_atu_memories(bool on) { (void)on; }
void radio_set_rf_gain(uint8_t pct) { (void)pct; }
void radio_set_rf_power(uint8_t pct) { (void)pct; }
void radio_set_tuner(bool on) { (void)on; }
void radio_set_squelch(uint8_t pct) { (void)pct; }
void radio_tg_lock(bool locked) { (void)locked; }
void radio_mute(bool muted) { (void)muted; }

/* The one question: after the receiver ended the session, another? */
bool radio_get_choice(uint8_t i, char *title, size_t tn, char *name, size_t nn)
{
    if (i || !S.time_up) return false;
    strlcpy(title, "TIME UP", tn);
    strlcpy(name, "LISTEN AGAIN", nn);
    return true;
}

void radio_choose(uint8_t i)
{
    if (i == 0 && S.time_up) {
        ESP_LOGI(TAG, "listening again, as asked");
        S.relisten = true;
    }
}
