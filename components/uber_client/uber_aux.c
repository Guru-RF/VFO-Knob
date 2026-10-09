/* The UberSDR's spots, voice activity and SSTV pictures -- everything but
 * the audio, on a task of its own so that a download never holds it up.
 *
 *   wss /ws/dxcluster?user_session_id=<uuid>   the DX cluster's spots and the
 *        CW skimmer's, once subscribed; the server pings, and wants pongs
 *   GET  /api/bands                            the receiver's own band list
 *   GET  /api/noisefloor/voice-activity?band=  voices heard on a band now, with
 *                                              the cluster's name where it has one
 *   GET  /addon/sstv/api/images?limit=         its SSTV gallery, newest first
 *   GET  /addon/sstv/images/<file>             a picture, as PNG
 *
 * The task's stack is in PSRAM: nothing here touches flash. It runs on core
 * 1, below the face, and starts only once the audio's session has been up a
 * few seconds: a TLS handshake here costs a second or two of software
 * crypto, and at a start every other one -- the session's, the update
 * check's, a second receiver's -- runs on core 0 at once; with this task's
 * there too, core 0's idle task starved past the watchdog's 5 s (a restart
 * a few seconds into the ubersdr firmware, 2026-10-09). */
#include "uber_json.h"
#include "uber_png.h"
#include "uber_priv.h"
#include "radio.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "uber-aux";

static portMUX_TYPE A_LOCK = portMUX_INITIALIZER_UNLOCKED;

static int64_t now_s(void) { return esp_timer_get_time() / 1000000; }

/* One connection for all of this task's requests, kept open between them --
 * to the receiver the session is on, as this task last took it (uber_rx). */
static uconn_t s_conn;
EXT_RAM_BSS_ATTR static uhost_t s_rx;

/* ------------------------------------------------------------------ bands */

typedef struct {
    int64_t lo, hi;
    char    label[8];
} band_t;

/* IARU Region 1, until the receiver says otherwise. */
static const band_t IARU[] = {
    { 1810000,  2000000,  "160m" }, { 3500000,  3800000,  "80m" }, { 5351500,  5366500,  "60m" },
    { 7000000,  7200000,  "40m" },  { 10100000, 10150000, "30m" }, { 14000000, 14350000, "20m" },
    { 18068000, 18168000, "17m" },  { 21000000, 21450000, "15m" }, { 24890000, 24990000, "12m" },
    { 28000000, 29700000, "10m" },  { 50000000, 52000000, "6m" },
};
#define BANDS_MAX 24
EXT_RAM_BSS_ATTR static band_t s_band[BANDS_MAX];
static int s_nband;

bool uber_band_of(int64_t hz, int64_t *lo, int64_t *hi, char *label, size_t cap)
{
    bool found = false;
    taskENTER_CRITICAL(&A_LOCK);
    const band_t *b = s_nband ? s_band : IARU;
    const int n = s_nband ? s_nband : (int)(sizeof IARU / sizeof IARU[0]);
    for (int i = 0; i < n && !found; i++) {
        if (hz >= b[i].lo && hz <= b[i].hi) {
            if (lo) *lo = b[i].lo;
            if (hi) *hi = b[i].hi;
            if (label) strlcpy(label, b[i].label, cap);
            found = true;
        }
    }
    taskEXIT_CRITICAL(&A_LOCK);
    return found;
}

static void read_bands(void)
{
    char *b = heap_caps_malloc(8192, MALLOC_CAP_SPIRAM), why[32];
    size_t n = 0;
    if (b && unet_http_keep(&s_conn, &s_rx, "GET", "/api/bands", NULL, b, 8191, &n, 10000, why, sizeof why) == 200) {
        const char *e = b + n, *it = b, *v;
        band_t t[BANDS_MAX];
        int k = 0;
        while (k < BANDS_MAX && (v = jnext(&it, e))) {
            char g[16] = "";
            jo_str(v, e, "group", g, sizeof g);
            if (!strcasecmp(g, "blocked")) continue;
            t[k].lo = (int64_t)jo_num(v, e, "start", 0);
            t[k].hi = (int64_t)jo_num(v, e, "end", 0);
            if (jo_str(v, e, "label", t[k].label, sizeof t[k].label) && t[k].hi > t[k].lo) k++;
        }
        if (k) {
            taskENTER_CRITICAL(&A_LOCK);
            memcpy(s_band, t, sizeof t[0] * k);
            s_nband = k;
            taskEXIT_CRITICAL(&A_LOCK);
            ESP_LOGI(TAG, "%d bands from the receiver", k);
        }
    }
    free(b);
}

/* What a spot is to be tuned in: the cluster's comment says, where it does,
 * and otherwise the band plan (IARU Region 1): CW at the bottom of a band,
 * the digital modes above, voice above those -- the lower sideband below
 * 10 MHz but on 60 m. "" for a digital mode: no voice to hear. */
const char *uber_mode_for(uint32_t hz, const char *comment)
{
    char c[64] = "";
    size_t i = 0;
    for (; comment && comment[i] && i + 1 < sizeof c; i++) c[i] = (char)toupper((unsigned char)comment[i]);
    c[i] = 0;
    static const char *DIGI[] = { "FT8", "FT4", "FT-8", "JS8", "PSK", "RTTY", "WSPR", "JT65", "JT9",
                                  "MSK144", "Q65", "OLIVIA", "DIGI" };
    for (size_t k = 0; k < sizeof DIGI / sizeof DIGI[0]; k++) if (strstr(c, DIGI[k])) return "";
    const char *ssb = hz < 10000000 && !(hz >= 5250000 && hz <= 5450000) ? "lsb" : "usb";
    const char *w = strstr(c, "CW");
    if (w && (w == c || !isalpha((unsigned char)w[-1])) && !isalpha((unsigned char)w[2])) return "cwu";
    if (strstr(c, "SSB") || strstr(c, "USB") || strstr(c, "LSB")) return ssb;
    /* kHz: the top of the CW segment, and of the digital one after it. */
    static const struct { uint32_t lo, cw, dig, hi; } PLAN[] = {
        { 1810,  1838,  1843,  2000 },  { 3500,  3570,  3600,  3800 },  { 5351,  5354,  5354,  5367 },
        { 7000,  7040,  7060,  7200 },  { 10100, 10130, 10150, 10150 }, { 14000, 14070, 14100, 14350 },
        { 18068, 18095, 18111, 18168 }, { 21000, 21070, 21151, 21450 }, { 24890, 24915, 24940, 24990 },
        { 28000, 28070, 28300, 29700 }, { 50000, 50100, 50100, 54000 },
    };
    const uint32_t k = hz / 1000;
    for (size_t b = 0; b < sizeof PLAN / sizeof PLAN[0]; b++) {
        if (k < PLAN[b].lo || k > PLAN[b].hi) continue;
        if (k < PLAN[b].cw) return "cwu";
        if (k < PLAN[b].dig) return "";
        /* FT8 and FT4 where they sit in the voice segments: 7074, 7047.5. */
        static const uint32_t FT[] = { 7074, 7047, 5357, 3573, 1840 };
        for (size_t f = 0; f < sizeof FT / sizeof FT[0]; f++) if (k >= FT[f] && k <= FT[f] + 3) return "";
        if (k >= 29500 && k <= 29700) return "fm";
        return ssb;
    }
    return ssb;
}

/* ------------------------------------------------------------------ spots */

#define SPOTS_MAX 128
typedef struct {
    char     call[12];
    uint32_t hz;
    char     kind;
    int8_t   snr;
    uint8_t  wpm;
    char     mode[5];
    int64_t  t;                   /* Unix s, the spot's own */
} spot_t;
EXT_RAM_BSS_ATTR static spot_t s_spot[SPOTS_MAX];
static int      s_nspot;
static uint32_t s_spot_seq;

/* 2026-09-30T19:18:58.278Z, or with an offset: Unix seconds, 0 if not. */
static int64_t rfc3339(const char *s)
{
    int Y, M, D, h, m, sec;
    if (sscanf(s, "%4d-%2d-%2dT%2d:%2d:%2d", &Y, &M, &D, &h, &m, &sec) != 6) return 0;
    /* days from civil (Howard Hinnant's) */
    Y -= M <= 2;
    const int era = (Y >= 0 ? Y : Y - 399) / 400;
    const unsigned yoe = (unsigned)(Y - era * 400);
    const unsigned doy = (153 * (M + (M > 2 ? -3 : 9)) + 2) / 5 + D - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    int64_t t = ((int64_t)era * 146097 + doe - 719468) * 86400 + h * 3600 + m * 60 + sec;
    const char *z = s + 19;
    while (*z == '.' || isdigit((unsigned char)*z)) z++;
    if ((*z == '+' || *z == '-') && isdigit((unsigned char)z[1])) {
        const int oh = atoi(z + 1), om = z[3] == ':' ? atoi(z + 4) : 0;
        t -= (*z == '+' ? 1 : -1) * (oh * 3600 + om * 60);
    }
    return t;
}

static void add_spot(char kind, const char *call, uint32_t hz, const char *comment, int snr, int wpm,
                     int64_t t)
{
    if (!call[0] || !hz) return;
    const char *mode = kind == 'C' ? "cwu" : uber_mode_for(hz, comment);
    taskENTER_CRITICAL(&A_LOCK);
    int slot = -1, oldest = 0;
    for (int i = 0; i < s_nspot; i++) {
        if (!strcasecmp(s_spot[i].call, call) &&
            (s_spot[i].hz > hz ? s_spot[i].hz - hz : hz - s_spot[i].hz) < 1000) {
            slot = i;
            break;
        }
        if (s_spot[i].t < s_spot[oldest].t) oldest = i;
    }
    if (slot < 0) slot = s_nspot < SPOTS_MAX ? s_nspot++ : oldest;
    spot_t *s = &s_spot[slot];
    strlcpy(s->call, call, sizeof s->call);
    s->hz = hz;
    s->kind = kind;
    s->snr = (int8_t)(snr < -99 ? -99 : snr > 99 ? 99 : snr);
    s->wpm = (uint8_t)(wpm < 0 ? 0 : wpm > 99 ? 99 : wpm);
    strlcpy(s->mode, mode, sizeof s->mode);
    s->t = t;
    s_spot_seq++;
    taskEXIT_CRITICAL(&A_LOCK);
}

/* Spots age out: the cluster's after half an hour, the skimmer's sooner. */
static void expire_spots(void)
{
    const int64_t now = uber_server_now();
    if (!now) return;
    taskENTER_CRITICAL(&A_LOCK);
    for (int i = 0; i < s_nspot;) {
        const int64_t age = now - s_spot[i].t;
        if (s_spot[i].t && age > (s_spot[i].kind == 'C' ? 600 : 1800)) {
            s_spot[i] = s_spot[--s_nspot];
            s_spot_seq++;
        } else {
            i++;
        }
    }
    taskEXIT_CRITICAL(&A_LOCK);
}

static void on_dx(const char *j, size_t n)
{
    const char *e = j + n;
    char type[24];
    if (!jo_str(j, e, "type", type, sizeof type)) return;
    const bool dx = !strcmp(type, "dx_spot"), cw = !strcmp(type, "cw_spot");
    if (!dx && !cw) {
        if (!strcmp(type, "subscription_status") || !strcmp(type, "status"))
            ESP_LOGD(TAG, "%.*s", (int)n, j);
        return;
    }
    const char *d = jkey(j, e, "data");
    char call[16] = "", comment[64] = "", ts[40] = "";
    jo_str(d, e, "dx_call", call, sizeof call);
    jo_str(d, e, "comment", comment, sizeof comment);
    jo_str(d, e, "time", ts, sizeof ts);
    const uint32_t hz = (uint32_t)(jo_num(d, e, "frequency", 0) + 0.5);
    add_spot(cw ? 'C' : 'D', call, hz, comment, (int)jo_num(d, e, "snr", 0),
             (int)jo_num(d, e, "wpm", 0), rfc3339(ts));
}

/* ----------------------------------------------------------------- voices */

/* Voices UberSDR's own detector hears on the dial's band just now -- a
 * station talking, named where the cluster has spotted it there. Asked for
 * every few seconds while the dial is on a band, in a voice mode. */
#define VOICE_MAX 32
typedef struct {
    uint32_t hz;                  /* its dial frequency, as the detector puts it */
    char     mode[5];
    char     call[12];            /* "" when nobody has named it */
    int8_t   snr;
} voice_t;
EXT_RAM_BSS_ATTR static voice_t s_voice[VOICE_MAX];
static int      s_nvoice;
static char     s_voice_band[8];

static void read_voice(const char *band)
{
    char path[80], why[32];
    snprintf(path, sizeof path, "/api/noisefloor/voice-activity?band=%s", band);
    char *b = heap_caps_malloc(32768, MALLOC_CAP_SPIRAM);
    size_t n = 0;
    if (b && unet_http_keep(&s_conn, &s_rx, "GET", path, NULL, b, 32767, &n, 8000, why, sizeof why) == 200) {
        const char *e = b + n, *it = jkey(b, e, "activities"), *v;
        voice_t t[VOICE_MAX];
        int k = 0;
        while (it && k < VOICE_MAX && (v = jnext(&it, e))) {
            memset(&t[k], 0, sizeof t[k]);
            t[k].hz = (uint32_t)jo_num(v, e, "estimated_dial_freq", 0);
            if (!jo_str(v, e, "mode", t[k].mode, sizeof t[k].mode) || !t[k].mode[0])
                strlcpy(t[k].mode, t[k].hz < 10000000 ? "lsb" : "usb", sizeof t[k].mode);
            for (char *m = t[k].mode; *m; m++) *m = (char)tolower((unsigned char)*m);   /* "LSB" */
            jo_str(v, e, "dx_callsign", t[k].call, sizeof t[k].call);
            const double snr = jo_num(v, e, "snr", 0);
            t[k].snr = (int8_t)(snr < -99 ? -99 : snr > 99 ? 99 : snr);
            if (t[k].hz) k++;
        }
        taskENTER_CRITICAL(&A_LOCK);
        const bool changed = k != s_nvoice || memcmp(s_voice, t, sizeof t[0] * k) ||
                             strcmp(s_voice_band, band);
        memcpy(s_voice, t, sizeof t[0] * k);
        s_nvoice = k;
        strlcpy(s_voice_band, band, sizeof s_voice_band);
        if (changed) s_spot_seq++;
        taskEXIT_CRITICAL(&A_LOCK);
        ESP_LOGD(TAG, "voices on %s: %d", band, k);
    }
    free(b);
}

/* ------------------------------------------------------------ on the band */

int uber_spots(uber_spot_t *out, int max, uint32_t *seq)
{
    int64_t dial, lo, hi;
    char mode[6], label[8];
    uber_dial(&dial, mode, sizeof mode);
    const bool cw = !strncmp(mode, "cw", 2);
    if (seq) *seq = s_spot_seq;
    if (!out || max <= 0 || !uber_band_of(dial, &lo, &hi, label, sizeof label)) return 0;
    const int64_t now = uber_server_now();
    /* Everything on the band that suits the mode: the cluster's spots, the
     * skimmer's in CW, and in a voice mode the voices heard now -- each
     * station once, a voice folded into the spot of it a few hundred hertz
     * off, which it marks as heard. */
    EXT_RAM_BSS_ATTR static uber_spot_t all[SPOTS_MAX + VOICE_MAX];
    int n = 0;
    taskENTER_CRITICAL(&A_LOCK);
    for (int i = 0; i < s_nspot; i++) {
        const spot_t *s = &s_spot[i];
        if (s->hz < lo || s->hz > hi || !s->mode[0]) continue;
        if (cw != !strncmp(s->mode, "cw", 2)) continue;
        uber_spot_t *o = &all[n++];
        memset(o, 0, sizeof *o);
        strlcpy(o->call, s->call, sizeof o->call);
        o->hz = s->hz;
        strlcpy(o->mode, s->mode, sizeof o->mode);
        o->kind = s->kind;
        o->snr = s->snr;
        o->wpm = s->wpm;
        const int64_t age = now && s->t ? now - s->t : 0;
        o->age_s = (uint16_t)(age < 0 ? 0 : age > 65535 ? 65535 : age);
    }
    const int nspots = n;
    if (!cw && !strcmp(label, s_voice_band)) {
        for (int i = 0; i < s_nvoice; i++) {
            const voice_t *v = &s_voice[i];
            if (v->hz < lo || v->hz > hi) continue;
            bool spotted = false;
            for (int k = 0; k < nspots; k++)
                if (llabs((int64_t)all[k].hz - v->hz) <= 1000) {
                    all[k].heard = true;
                    all[k].snr = v->snr;
                    spotted = true;
                }
            if (spotted) continue;
            uber_spot_t *o = &all[n++];
            memset(o, 0, sizeof *o);
            strlcpy(o->call, v->call, sizeof o->call);
            o->hz = v->hz;
            strlcpy(o->mode, v->mode, sizeof o->mode);
            o->kind = 'V';
            o->snr = v->snr;
            o->heard = true;
        }
    }
    taskEXIT_CRITICAL(&A_LOCK);
    /* The ones nearest the dial, then in frequency order. */
    for (int i = 1; i < n; i++) {
        const uber_spot_t t = all[i];
        const int64_t d = llabs((int64_t)t.hz - dial);
        int k = i - 1;
        while (k >= 0 && llabs((int64_t)all[k].hz - dial) > d) { all[k + 1] = all[k]; k--; }
        all[k + 1] = t;
    }
    if (n > max) n = max;
    for (int i = 1; i < n; i++) {
        const uber_spot_t t = all[i];
        int k = i - 1;
        while (k >= 0 && all[k].hz > t.hz) { all[k + 1] = all[k]; k--; }
        all[k + 1] = t;
    }
    memcpy(out, all, sizeof out[0] * n);
    return n;
}

/* ------------------------------------------------------------------ SSTV */

#define SSTV_MAX   30
#define SSTV_W     260           /* the viewer's frame: see ui.c */
#define SSTV_H     208
#define PNG_MAX    (1536 * 1024)
typedef struct {
    char     file[72];
    char     mode[8];
    char     audio[5];
    char     call[12];
    char     at[8];              /* HH:MM, UTC */
    uint32_t hz;
    int8_t   snr;
} sstv_t;
EXT_RAM_BSS_ATTR static sstv_t s_sstv[SSTV_MAX];
static int      s_nsstv = -1;    /* -1: not read (or none to read) */
static int64_t  s_sstv_at;
static volatile int s_want = -1;
static volatile uint32_t s_want_gen;   /* the viewer's request: see uber.h */
static struct {
    uber_sstv_t img;
    uint32_t shown;
    char     file[72];           /* the picture published */
    uint32_t gen;                /* the request it answered */
    int      slot;               /* where its pixels are, -1 for none */
} V = { .slot = -1 };

/* Pictures decoded, kept: the one on the glass, the one offered to it, and
 * the ones fetched ahead of the knob -- so turning to the next is instant. */
#define SLOTS 4
EXT_RAM_BSS_ATTR static struct {
    char     file[72];
    uint16_t w, h;
    uint32_t used;
} s_slot[SLOTS];
static uint16_t *s_slot_px[SLOTS];
/* The one last offered, which may never reach the glass, and the one the
 * glass draws: it keeps drawing a picture, dimmed under "fetching...", until
 * it takes the next, however many it let go meanwhile. The face's task
 * sets s_drawn: uber_sstv_shown on a take, uber_sstv_want(-1) on closing. */
static int       s_offered = -1;
static volatile int s_drawn = -1;
static uint32_t  s_clock;
static char      s_failed[72];   /* not fetched ahead again */

static int slot_of(const char *file)
{
    for (int i = 0; i < SLOTS; i++) if (s_slot[i].file[0] && !strcmp(s_slot[i].file, file)) return i;
    return -1;
}

/* The least recently used, but never one the glass may be showing. */
static int slot_free(void)
{
    int best = -1;
    for (int i = 0; i < SLOTS; i++) {
        if (i == s_offered || i == s_drawn) continue;
        if (best < 0 || s_slot[i].used < s_slot[best].used) best = i;
    }
    return best;
}

int uber_sstv_count(void)
{
    uber_info_t in;
    uber_info(&in);
    return in.sstv ? (s_nsstv < 0 ? 0 : s_nsstv) : -1;
}

void uber_sstv_want(int idx, uint32_t gen)
{
    /* Closed, the viewer has let its picture go: its slot is free again. */
    if (idx < 0) s_drawn = -1;
    s_want_gen = gen;
    s_want = idx;
}

bool uber_sstv_get(uber_sstv_t *out, uint32_t after_seq)
{
    bool r = false;
    taskENTER_CRITICAL(&A_LOCK);
    if (V.img.seq != after_seq) {
        *out = V.img;
        r = true;
    }
    taskEXIT_CRITICAL(&A_LOCK);
    return r;
}

void uber_sstv_shown(uint32_t seq, bool taken)
{
    taskENTER_CRITICAL(&A_LOCK);
    if (taken && seq == V.img.seq) s_drawn = V.slot;
    V.shown = seq;
    taskEXIT_CRITICAL(&A_LOCK);
}

int uber_sstv_files(char (*out)[72], int max)
{
    int n = 0;
    taskENTER_CRITICAL(&A_LOCK);
    for (; n < s_nsstv && n < max; n++) strlcpy(out[n], s_sstv[n].file, sizeof out[n]);
    taskEXIT_CRITICAL(&A_LOCK);
    return n;
}

void uber_base_url(char *out, size_t cap)
{
    uhost_t h;
    uber_rx(&h);
    if (h.port == (h.tls ? 443 : 80))
        snprintf(out, cap, "%s://%s", h.tls ? "https" : "http", h.host);
    else
        snprintf(out, cap, "%s://%s:%u", h.tls ? "https" : "http", h.host, (unsigned)h.port);
}

static void read_sstv_list(void)
{
    const size_t cap = 48 * 1024;
    char *b = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM), why[32];
    size_t n = 0;
    char path[96];
    snprintf(path, sizeof path, "/addon/sstv/api/images?limit=%d&offset=0&complete=1", SSTV_MAX);
    const int st = b ? unet_http_keep(&s_conn, &s_rx, "GET", path, NULL, b, cap - 1, &n, 10000, why, sizeof why) : -1;
    s_sstv_at = now_s();
    if (st == 200) {
        const char *e = b + n, *it = b, *v;
        int k = 0;
        EXT_RAM_BSS_ATTR static sstv_t t[SSTV_MAX];
        while (k < SSTV_MAX && (v = jnext(&it, e))) {
            sstv_t *x = &t[k];
            memset(x, 0, sizeof *x);
            if (!jo_str(v, e, "file", x->file, sizeof x->file) || !x->file[0]) continue;
            jo_str(v, e, "sstv_mode", x->mode, sizeof x->mode);
            jo_str(v, e, "audio_mode", x->audio, sizeof x->audio);
            jo_str(v, e, "callsign", x->call, sizeof x->call);
            char ts[40] = "";
            jo_str(v, e, "rx_end", ts, sizeof ts);
            if (strlen(ts) >= 16) snprintf(x->at, sizeof x->at, "%.5s", ts + 11);
            x->hz = (uint32_t)jo_num(v, e, "frequency_hz", 0);
            const double snr = jo_num(v, e, "snr_avg_db", 0);
            x->snr = (int8_t)(snr < -99 ? -99 : snr > 99 ? 99 : snr);
            k++;
        }
        taskENTER_CRITICAL(&A_LOCK);
        memcpy(s_sstv, t, sizeof t[0] * k);
        s_nsstv = k;
        taskEXIT_CRITICAL(&A_LOCK);
        ESP_LOGI(TAG, "SSTV gallery: %d pictures", k);
    } else {
        ESP_LOGW(TAG, "SSTV gallery: %s", st > 0 ? "refused" : why);
    }
    free(b);
}

static void publish(int idx, uint32_t gen, const sstv_t *x, int slot, const char *fail)
{
    char up[6] = "";
    for (int i = 0; i < 5 && x->audio[i]; i++) up[i] = (char)toupper((unsigned char)x->audio[i]);
    taskENTER_CRITICAL(&A_LOCK);
    V.img.px = slot >= 0 ? s_slot_px[slot] : NULL;
    V.img.w = slot >= 0 ? s_slot[slot].w : 0;
    V.img.h = slot >= 0 ? s_slot[slot].h : 0;
    V.img.idx = idx;
    V.img.n = s_nsstv;
    V.img.failed = fail != NULL;
    snprintf(V.img.title, sizeof V.img.title, "%s  %d / %d", x->mode, idx + 1, s_nsstv);
    if (fail) snprintf(V.img.caption, sizeof V.img.caption, "%s", fail);
    else if (x->call[0])
        snprintf(V.img.caption, sizeof V.img.caption, "%s  %lu.%03lu  %sZ", x->call,
                 (unsigned long)(x->hz / 1000000), (unsigned long)(x->hz / 1000 % 1000), x->at);
    else
        snprintf(V.img.caption, sizeof V.img.caption, "%lu.%03lu %s  %sZ  %d dB",
                 (unsigned long)(x->hz / 1000000), (unsigned long)(x->hz / 1000 % 1000), up, x->at,
                 x->snr);
    strlcpy(V.file, x->file, sizeof V.file);
    V.gen  = gen;
    V.slot = slot;
    V.img.seq++;
    taskEXIT_CRITICAL(&A_LOCK);
    if (slot >= 0) {
        s_offered = slot;
        s_slot[slot].used = ++s_clock;
    }
}

/* Picture idx, downloaded and decoded into a free slot: the slot, or -1 with
 * why. */
static int fetch_picture(int idx, char *why, size_t wn)
{
    sstv_t x;
    taskENTER_CRITICAL(&A_LOCK);
    x = s_sstv[idx];
    taskEXIT_CRITICAL(&A_LOCK);
    const int slot = slot_free();
    if (slot < 0) {
        snprintf(why, wn, "busy");
        return -1;
    }
    if (!s_slot_px[slot]) s_slot_px[slot] = heap_caps_malloc(SSTV_W * SSTV_H * 2, MALLOC_CAP_SPIRAM);
    uint8_t *png = heap_caps_malloc(PNG_MAX, MALLOC_CAP_SPIRAM);
    if (!png || !s_slot_px[slot]) {
        free(png);
        snprintf(why, wn, "no memory");
        return -1;
    }
    char path[112];
    snprintf(path, sizeof path, "/addon/sstv/images/%s", x.file);
    size_t n = 0;
    const int64_t t0 = esp_timer_get_time();
    const int st = unet_http_keep(&s_conn, &s_rx, "GET", path, NULL, (char *)png, PNG_MAX, &n, 15000,
                                  why, wn);
    const int64_t t1 = esp_timer_get_time();
    s_slot[slot].file[0] = 0;                   /* until it holds this one */
    int w = 0, h = 0, r = -1;
    if (st != 200 || n < 64) {
        ESP_LOGW(TAG, "%s: %d %s", x.file, st, st < 0 ? why : "");
        if (st > 0) snprintf(why, wn, "not found");
    } else if (upng_decode(png, n, s_slot_px[slot], SSTV_W, SSTV_H, &w, &h, why, wn) != ESP_OK) {
        ESP_LOGW(TAG, "%s: %s", x.file, why);
    } else {
        ESP_LOGI(TAG, "%s: %u bytes in %lld ms, %dx%d in %lld ms", x.file, (unsigned)n,
                 (long long)((t1 - t0) / 1000), w, h, (long long)((esp_timer_get_time() - t1) / 1000));
        strlcpy(s_slot[slot].file, x.file, sizeof s_slot[slot].file);
        s_slot[slot].w = (uint16_t)w;
        s_slot[slot].h = (uint16_t)h;
        s_slot[slot].used = ++s_clock;
        r = slot;
    }
    free(png);
    return r;
}

/* The viewer: the picture it wants, from the cache or fetched; and while it
 * rests on one, the next one older, then the next newer, fetched ahead. */
static void sstv_step(void)
{
    /* The face writes the request's gen before its idx, so an idx read
     * first comes with its own gen or a newer one; either way a pair that
     * moves on meanwhile is answered again on the next pass. */
    const int want = s_want;
    const uint32_t gen = s_want_gen;
    if (want < 0 || want >= s_nsstv || V.shown != V.img.seq) return;
    char why[40];
    /* A new request has the picture offered again even when it is the one
     * offered last: the viewer, reopened or turned back to it, waits on
     * "fetching..." for it, and without this waited for good. So does a
     * gallery of another size: the title counts it ("M2  2 / 5"). */
    if (V.img.idx != want || V.gen != gen || V.img.n != s_nsstv || strcmp(V.file, s_sstv[want].file) ||
        !V.img.seq) {
        int slot = slot_of(s_sstv[want].file);
        if (slot < 0) slot = fetch_picture(want, why, sizeof why);
        sstv_t x = s_sstv[want];
        publish(want, gen, &x, slot, slot < 0 ? why : NULL);
        return;
    }
    for (int d = 1; d >= -1; d -= 2) {
        const int k = want + d;
        if (k < 0 || k >= s_nsstv || slot_of(s_sstv[k].file) >= 0 || !strcmp(s_failed, s_sstv[k].file))
            continue;
        if (fetch_picture(k, why, sizeof why) < 0) strlcpy(s_failed, s_sstv[k].file, sizeof s_failed);
        return;                                  /* one a pass: the knob may have moved */
    }
}

/* ------------------------------------------------------------------- task */

static bool s_started;

/* Another receiver, handed over to: nothing of the last one's -- the
 * connection, its bands, spots and voices, its gallery and the pictures
 * fetched from it. The glass may still be drawing one: its pixels stay. */
static void forget(void)
{
    unet_close(&s_conn);
    taskENTER_CRITICAL(&A_LOCK);
    s_nband = 0;
    s_nspot = 0;
    s_nvoice = 0;
    s_voice_band[0] = 0;
    s_spot_seq++;
    s_nsstv = -1;
    for (int i = 0; i < SLOTS; i++) s_slot[i].file[0] = 0;
    V.file[0] = 0;
    taskEXIT_CRITICAL(&A_LOCK);
    s_sstv_at = 0;
    s_failed[0] = 0;
}

static void aux_task(void *arg)
{
    (void)arg;
    uws_t dx = { 0 };
    bool dx_open = false;
    uint32_t dx_gen = 0;
    int64_t dx_retry = 0, t_voice = 0, t_expire = 0, t_bands = 0;
    char voice_band[8] = "";
    int dx_backoff = 10;
    int64_t up_since = 0;               /* the session READY since; 0 not */
    uint32_t rx_gen = uber_rx(&s_rx);
    for (;;) {
        if (!radio_is_ready()) {
            up_since = 0;
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        if (!up_since) up_since = now_s();
        if (now_s() - up_since < 5) {   /* its handshakes done first */
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        /* The session gone on to another receiver: this task with it. */
        const uint32_t g = uber_rx(&s_rx);
        if (g != rx_gen) {
            rx_gen = g;
            if (dx_open) uws_close(&dx);
            dx_open = false;
            forget();
            dx_retry = t_voice = t_bands = 0;
            dx_backoff = 10;
            voice_band[0] = 0;
            ESP_LOGI(TAG, "on to %s:%u", s_rx.host, (unsigned)s_rx.port);
        }
        uber_info_t in;
        uber_info(&in);
        if (!in.known) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        const int64_t now = now_s();
        if (!s_nband && now >= t_bands) {
            read_bands();
            t_bands = now + 300;
        }

        /* The spots' socket, under the session's UUID. */
        char uuid[37];
        uint32_t gen;
        const bool reg = uber_session_id(uuid, sizeof uuid, &gen);
        if (dx_open && gen != dx_gen) {
            uws_close(&dx);
            dx_open = false;
        }
        if (in.spots && !dx_open && reg && now >= dx_retry) {
            char path[96], why[64];
            snprintf(path, sizeof path, "/ws/dxcluster?user_session_id=%s", uuid);
            if (uws_open(&dx, &s_rx, path, 16 * 1024, 10000, why, sizeof why)) {
                dx_open = true;
                dx_gen = gen;
                dx_backoff = 10;
                uws_text(&dx, "{\"type\":\"subscribe_dx_spots\"}");
                uws_text(&dx, "{\"type\":\"subscribe_cw_spots\"}");
                ESP_LOGI(TAG, "spots: subscribed");
            } else {
                ESP_LOGW(TAG, "spots: %s; again in %d s", why, dx_backoff);
                dx_retry = now + dx_backoff;
                dx_backoff = dx_backoff < 60 ? dx_backoff * 2 : 120;
            }
        }
        if (dx_open) {
            /* Everything waiting, then on. */
            for (int i = 0; i < 64; i++) {
                uint8_t op;
                const uint8_t *p;
                size_t n;
                const int r = uws_recv(&dx, i ? 0 : 100, &op, &p, &n);
                if (r < 0) {
                    ESP_LOGW(TAG, "spots: socket closed (%u); again in %d s", (unsigned)dx.close_code,
                             dx_backoff);
                    uws_close(&dx);
                    dx_open = false;
                    dx_retry = now + dx_backoff;
                    break;
                }
                if (r == 0) break;
                if (op == 0x1) on_dx((const char *)p, n);
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(100));
        }

        /* The voices on the dial's band, in a voice mode: every ten seconds,
         * and at once on another band. */
        if (in.voice && reg) {
            int64_t dial;
            char mode[6], label[8];
            uber_dial(&dial, mode, sizeof mode);
            if (strncmp(mode, "cw", 2) && uber_band_of(dial, NULL, NULL, label, sizeof label) &&
                (now - t_voice >= 10 || strcmp(label, voice_band))) {
                t_voice = now;
                strlcpy(voice_band, label, sizeof voice_band);
                read_voice(label);
            }
        }
        if (now - t_expire >= 10) {
            t_expire = now;
            expire_spots();
        }

        /* SSTV: the list now and then, and fresh for the viewer. */
        const int want = s_want;
        if (in.sstv && (s_nsstv < 0 ? now - s_sstv_at >= 30
                                    : now - s_sstv_at >= (want >= 0 ? 60 : 600)))
            read_sstv_list();
        (void)want;
        if (in.sstv) sstv_step();
    }
}

void uber_aux_start(void)
{
    if (s_started) return;
    s_started = xTaskCreatePinnedToCoreWithCaps(aux_task, "uberaux", 12288, NULL, 3, NULL, 1,
                                                MALLOC_CAP_SPIRAM) == pdPASS;
    if (!s_started) ESP_LOGE(TAG, "no task for the spots");
}
