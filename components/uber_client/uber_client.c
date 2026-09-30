/* UberSDR client: the ubersdr firmware's radio.h -- a web receiver, listened
 * to through its own protocol, as its own web page listens (ka9q_ubersdr
 * 0.1.66; ubersdr-ntp's client for the sequence that is known to work):
 *
 *   GET  /api/description        what it is and what it has
 *   POST /connection             {"user_session_id":<uuid>,"password":<pw>}
 *   wss  /ws?frequency=..&mode=..&bandwidthLow=..&bandwidthHigh=..
 *            &format=opus&version=4&user_session_id=<uuid>[&password=..]
 *   <-   binary: an Opus packet of 20 ms behind a small header -- flags, a
 *        time stamp, on a resync the rate, and the signal and noise power
 *   ->   {"type":"tune",...} {"type":"set_dsp",...} {"type":"ping"}
 *
 * Receive only: nothing here keys anything, and PTT is refused.
 *
 * The session task's stack is in PSRAM, so it never touches flash: what is
 * kept across a restart (the frequency, the mode, the filter) is saved by a
 * timer. See uber_aux.c for the spots and the SSTV pictures. */
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
#include "nvs.h"
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

/* Its modes, and the passband each opens with: UberSDR's own. */
static const struct { const char *m; int16_t lo, hi; } MODES[] = {
    { "usb",    50,  2700 }, { "lsb", -2700,  -50 }, { "cwu",  -200,  200 },
    { "cwl",  -200,   200 }, { "am",  -5000, 5000 }, { "sam", -5000, 5000 },
    { "fm",  -8000,  8000 }, { "nfm", -5000, 5000 },
};
#define N_MODES ((int)(sizeof MODES / sizeof MODES[0]))

uhost_t g_uh;
static char s_pass[33];

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
    int64_t  srv_ns, srv_at;      /* the receiver's clock, and when we read it */
    uint32_t connects, closes, frames, dropped, texts;
    char     last_close[48];
    bool     activity;
    bool     suspend;
} S = { .link = RADIO_LINK_DOWN };

static esp_timer_handle_t s_save_t;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

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
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_i64(h, "ubf", f);
    nvs_set_str(h, "ubm", m);
    nvs_set_i32(h, "ubl", lo);
    nvs_set_i32(h, "ubh", hi);
    nvs_set_str(h, "ubn", nr);
    nvs_commit(h);
    nvs_close(h);
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
    nvs_handle_t h;
    int64_t f = 0;
    char m[8] = "";
    int32_t lo = 0, hi = 0;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t n = sizeof m;
        nvs_get_i64(h, "ubf", &f);
        nvs_get_str(h, "ubm", m, &n);
        nvs_get_i32(h, "ubl", &lo);
        nvs_get_i32(h, "ubh", &hi);
        n = sizeof s_nr_saved;
        nvs_get_str(h, "ubn", s_nr_saved, &n);
        nvs_close(h);
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
    if (!b) return false;
    size_t n = 0;
    const int st = unet_http(&g_uh, "GET", "/api/description", NULL, b, cap - 1, &n, 12000, why, wn);
    bool ok = false;
    if (st == 200 && n > 2) {
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
        snprintf(why, wn, "HTTP %d", st);
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

/* What a refusal means for the next try. */
enum { R_OK = 0, R_RETRY, R_WAIT, R_REREGISTER, R_NEW_UUID, R_TIME_UP, R_STOP };

static int classify(int status, const char *text, char *why, size_t wn)
{
    char t[96];
    size_t i = 0;
    for (; text && text[i] && i + 1 < sizeof t; i++) t[i] = (char)tolower((unsigned char)text[i]);
    t[i] = 0;
    if (strstr(t, "banned") || strstr(t, "access denied") || status == 401 || status == 403)
                                             { snprintf(why, wn, "REFUSED");       return R_STOP; }
    if (strstr(t, "requires a password"))    { snprintf(why, wn, "PASSWORD?");     return R_STOP; }
    if (strstr(t, "invalid bypass password")){ snprintf(why, wn, "WRONG PASSWORD"); return R_STOP; }
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
        snprintf(why, wn, "NO ANSWER");
        ESP_LOGW(TAG, "/connection: %s", w);
        return R_RETRY;
    }
    const char *e = ans + n;
    if (st == 200 && jo_bool(ans, e, "allowed")) {
        const bool byp = jo_bool(ans, e, "bypassed");
        const int  mst = (int)jo_num(ans, e, "max_session_time", 0);
        taskENTER_CRITICAL(&S_LOCK);
        I.bypassed = byp;
        I.max_session_s = byp ? 0 : mst;
        taskEXIT_CRITICAL(&S_LOCK);
        ESP_LOGI(TAG, "session %.8s registered%s%s", uuid, byp ? ", bypassed" : "",
                 !byp && mst ? " (time-limited)" : "");
        return R_OK;
    }
    char reason[96] = "";
    jo_str(ans, e, "reason", reason, sizeof reason);
    ESP_LOGW(TAG, "/connection: %d %s", st, reason);
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
    return uws_text(&s_ws, msg);
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
    return uws_text(&s_ws, msg);
}


void uber_activity(void) { S.activity = true; }

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
    uws_text(&s_ws, "{\"type\":\"get_status\"}");
    set_link(RADIO_LINK_GREETING, NULL);
    S.connects++;
    s_tunes_out = 0;

    v4_t h = { .power = -32768, .noise = -32768 };
    uint32_t sent_gen, t_tuned = 0, t_dsp = 0, t_ping = now_ms(), t_audio = now_ms();
    taskENTER_CRITICAL(&S_LOCK);
    sent_gen = S.gen;
    S.nr_on = 0;
    taskEXIT_CRITICAL(&S_LOCK);
    s_nr_sent = 0;
    int nr_rate = 0;
    bool streaming = false;
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
            uws_text(&s_ws, "{\"type\":\"ping\"}");
        }
    }
    uws_close(&s_ws);
    S.closes++;
    set_link(RADIO_LINK_DOWN, why);
    return end;
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
    for (;;) {
        set_link(RADIO_LINK_CONNECTING, NULL);
        if (!I.known) {
            char w[32];
            if (!read_description(w, sizeof w)) {
                ESP_LOGW(TAG, "%s:%u: no description (%s); again in %lu s", g_uh.host,
                         (unsigned)g_uh.port, w, (unsigned long)(backoff / 1000));
                set_link(RADIO_LINK_DOWN, !strcmp(w, "name not found") ? "NOT FOUND" : "NO ANSWER");
                vTaskDelay(pdMS_TO_TICKS(backoff));
                backoff = backoff < 30000 ? backoff * 2 : 60000;
                continue;
            }
            /* The noise filter chosen at the last boot, if it still runs. */
            taskENTER_CRITICAL(&S_LOCK);
            for (int i = 0; i < s_n_nr; i++)
                if (s_nr_saved[0] && !strcmp(s_nr_saved, s_nr[i])) S.nr_want = (int8_t)(i + 1);
            taskEXIT_CRITICAL(&S_LOCK);
        }
        int r = register_session(why, sizeof why);
        if (r == R_OK) {
            const int64_t t0 = esp_timer_get_time();
            r = stream(why, sizeof why);
            /* A session that ran a while earns a quick return. */
            if (esp_timer_get_time() - t0 > 30 * 1000000LL) backoff = 2000;
        } else {
            set_link(RADIO_LINK_DOWN, why);
        }
        switch (r) {
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
            taskENTER_CRITICAL(&S_LOCK);
            S.time_up = false;
            taskEXIT_CRITICAL(&S_LOCK);
            new_uuid();
            backoff = 2000;
            continue;
        case R_NEW_UUID:
            new_uuid();
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
    if (!host || !host[0]) return ESP_ERR_INVALID_ARG;
    /* https://name/ as pasted, or name, or name:port. */
    const char *h = strstr(host, "://");
    const bool https = h && !strncasecmp(host, "https", 5);
    h = h ? h + 3 : host;
    const size_t hl = strcspn(h, ":/ ");
    if (!hl || hl >= sizeof g_uh.host) return ESP_ERR_INVALID_ARG;
    memcpy(g_uh.host, h, hl);
    g_uh.host[hl] = 0;
    g_uh.port = h[hl] == ':' ? (uint16_t)atoi(h + hl + 1) : port ? port : (https ? 443 : 80);
    g_uh.tls  = g_uh.port == 443 || https;
    strlcpy(s_pass, pass ? pass : "", sizeof s_pass);
    load();
    const esp_timer_create_args_t ta = { .callback = save_cb, .name = "ubsave" };
    esp_timer_create(&ta, &s_save_t);
    ESP_LOGI(TAG, "UberSDR at %s://%s:%u%s", g_uh.tls ? "https" : "http", g_uh.host,
             (unsigned)g_uh.port, s_pass[0] ? ", with its password" : "");
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
