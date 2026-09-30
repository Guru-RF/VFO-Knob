/* A web SDR as a second receiver: KiwiSDR's protocol. See sdr_rx.h.
 *
 * Written from the protocol as the Kiwi's own web client and kiwiclient speak
 * it, tried against a KiwiSDR 2 (v1.902) and a Web-888:
 *
 *   ws://host:port/ws/kiwi/<ts>/SND     KiwiSDR 1.9; older ones and the
 *   ws://host:port/<ts>/SND             Web-888 answer only this one
 *   -> SET auth t=kiwi p=<pw> [ipl=<time-limit pw>]    ("#" for no pw with ipl)
 *   <- MSG ... badp=0 ... audio_rate=12000 sample_rate=12000.000
 *   -> SET AR OK in=12000 out=24000, SET compression=1,
 *      SET mod=usb low_cut=300 high_cut=2700 freq=14074.000, SET keepalive (1 s)
 *   <- SND: flags(1) seq(4 LE) S-meter(2 BE, 0.1 dB + 127) data -- IMA-ADPCM
 *      when flags has 0x10, else 16-bit big-endian PCM
 *
 * The task's stack is in PSRAM: it never touches flash (the list is saved by
 * whoever calls sdr_save, the selection by a timer). */
#include "sdr_rx.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "audio_out.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "nvs.h"

static const char *TAG = "sdr";

#define NVS_NS     "vfo"
#define KEY_LIST   "sdrs"
#define KEY_SEL    "sdrsel"
#define KEY_BAL    "sdrbal"
#define BUF_BYTES  (16 * 1024)       /* frames up to this; bigger are skipped */
#define PCM_MAX    4096              /* samples of one SND frame, decoded */
#define OUT_MAX    (PCM_MAX * 2 + 16)
#define QUIET_MS   10000             /* nothing for this long: the session is gone */
#define ANSWER_MS  3000              /* no login answer: try the other path */

EXT_RAM_BSS_ATTR static sdr_cfg_t s_list[SDR_MAX];
static int          s_n;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static volatile int s_want = -1;
static volatile int8_t s_bal;
static volatile uint32_t s_list_gen;
static sdr_status_t s_st = { .sel = -1 };
/* Which path each receiver answered on: 0 unknown, 1 /ws/kiwi/, 2 the old. */
static uint8_t      s_path[SDR_MAX];

static struct {
    int64_t  hz;
    char     mode[8];
    int32_t  lo, hi;
    uint32_t gen;
} s_tune;

static esp_timer_handle_t s_save_timer;

#define WAIT_RADIO "waiting for the radio"

/* ------------------------------------------------------------------ list */

static void list_parse(const char *blob)
{
    int n = 0;
    const char *p = blob;
    while (p && *p && n < SDR_MAX) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        char line[200];
        if (len >= sizeof line) len = sizeof line - 1;
        memcpy(line, p, len);
        line[len] = 0;
        /* name \t host \t port \t pass \t ipl, empty fields kept */
        char *f[5] = { line, "", "", "", "" };
        int k = 1;
        for (char *q = line; *q && k < 5; q++)
            if (*q == '\t') { *q = 0; f[k++] = q + 1; }
        if (f[1][0]) {
            sdr_cfg_t *c = &s_list[n++];
            memset(c, 0, sizeof *c);
            strlcpy(c->name, f[0], sizeof c->name);
            strlcpy(c->host, f[1], sizeof c->host);
            c->port = (uint16_t)atoi(f[2]);
            if (!c->port) c->port = 8073;
            strlcpy(c->pass, f[3], sizeof c->pass);
            strlcpy(c->ipl, f[4], sizeof c->ipl);
        }
        p = eol ? eol + 1 : NULL;
    }
    s_n = n;
}

int sdr_count(void) { return s_n; }

bool sdr_get(int i, sdr_cfg_t *out)
{
    if (i < 0 || i >= s_n || !out) return false;
    taskENTER_CRITICAL(&s_lock);
    *out = s_list[i];
    taskEXIT_CRITICAL(&s_lock);
    return true;
}

static bool clean(const char *s)
{
    for (; *s; s++) if (*s == '\t' || *s == '\n' || *s == '\r') return false;
    return true;
}

esp_err_t sdr_save(const sdr_cfg_t *list, int n)
{
    if (n < 0 || n > SDR_MAX) return ESP_ERR_INVALID_ARG;
    char *blob = heap_caps_calloc(1, SDR_MAX * 200 + 1, MALLOC_CAP_SPIRAM);
    if (!blob) return ESP_ERR_NO_MEM;
    size_t o = 0;
    for (int i = 0; i < n; i++) {
        const sdr_cfg_t *c = &list[i];
        if (!c->host[0] || !clean(c->name) || !clean(c->host) || !clean(c->pass) || !clean(c->ipl))
            continue;
        o += snprintf(blob + o, SDR_MAX * 200 + 1 - o, "%s\t%s\t%u\t%s\t%s\n",
                      c->name, c->host, (unsigned)c->port, c->pass, c->ipl);
    }
    nvs_handle_t h;
    esp_err_t e = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (e == ESP_OK) {
        e = nvs_set_str(h, KEY_LIST, blob);
        if (e == ESP_OK) e = nvs_commit(h);
        nvs_close(h);
    }
    if (e == ESP_OK) {
        taskENTER_CRITICAL(&s_lock);
        list_parse(blob);
        memset(s_path, 0, sizeof s_path);
        s_list_gen++;
        if (s_want >= s_n) s_want = -1;
        taskEXIT_CRITICAL(&s_lock);
        ESP_LOGI(TAG, "%d receiver%s saved", s_n, s_n == 1 ? "" : "s");
    }
    free(blob);
    return e;
}

/* The selection and the balance, saved from the timer task: the SDR task's
 * stack is in PSRAM and may not touch flash, and the dial's task has none to
 * spare. */
static void save_cb(void *arg)
{
    (void)arg;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_i8(h, KEY_SEL, (int8_t)s_want);
    nvs_set_i8(h, KEY_BAL, s_bal);
    nvs_commit(h);
    nvs_close(h);
}

static void save_later(void)
{
    if (!s_save_timer) return;
    esp_timer_stop(s_save_timer);
    esp_timer_start_once(s_save_timer, 2 * 1000 * 1000);   /* settle first */
}

void sdr_rx_select(int i)
{
    if (i < -1 || i >= s_n) i = -1;
    if (i == s_want) return;
    s_want = i;
    ESP_LOGI(TAG, "listening to %s", i < 0 ? "the radio alone" : s_list[i].name[0] ? s_list[i].name : s_list[i].host);
    save_later();
}

int sdr_rx_selected(void) { return s_want; }

void sdr_rx_set_balance(int8_t b)
{
    if (b < -100) b = -100;
    if (b > 100)  b = 100;
    audio_out_set_balance(b);
    if (b == s_bal) return;
    s_bal = b;
    save_later();
}

int8_t sdr_rx_balance(void) { return s_bal; }

void sdr_rx_tune(int64_t hz, const char *mode, int32_t lo, int32_t hi)
{
    if (!mode) mode = "";
    taskENTER_CRITICAL(&s_lock);
    if (hz != s_tune.hz || lo != s_tune.lo || hi != s_tune.hi || strcmp(mode, s_tune.mode)) {
        s_tune.hz = hz;
        s_tune.lo = lo;
        s_tune.hi = hi;
        strlcpy(s_tune.mode, mode, sizeof s_tune.mode);
        s_tune.gen++;
    }
    taskEXIT_CRITICAL(&s_lock);
}

void sdr_rx_status(sdr_status_t *out)
{
    if (!out) return;
    taskENTER_CRITICAL(&s_lock);
    *out = s_st;
    taskEXIT_CRITICAL(&s_lock);
}

/* The trouble in a word or two, for the dial: what state says at length. */
static const char *note_of(const char *state)
{
    static const struct { const char *state, *note; } N[] = {
        { "busy", "busy" },                  { "app channels in use", "busy" },
        { "no apps allowed", "no apps" },    { "wrong password", "password?" },
        { "daily limit reached", "day limit" }, { "down", "down" },
        { "name not found", "not found" },   { "no route", "no route" },
        { "no free socket", "no socket" },
    };
    for (size_t i = 0; i < sizeof N / sizeof N[0]; i++)
        if (!strcmp(state, N[i].state)) return N[i].note;
    return "no answer";                      /* refused, closed, went quiet ... */
}

static void set_state(int sel, const char *state, const char *name)
{
    taskENTER_CRITICAL(&s_lock);
    s_st.sel = sel;
    strlcpy(s_st.state, state, sizeof s_st.state);
    if (name) strlcpy(s_st.name, name, sizeof s_st.name);
    s_st.streaming = strcmp(state, "streaming") == 0;
    /* Everything but on its way, or waiting its turn, is trouble. */
    s_st.trouble = sel >= 0 && !s_st.streaming && strcmp(state, "connecting") &&
                   strcmp(state, "logged in") && strcmp(state, WAIT_RADIO);
    strlcpy(s_st.note, s_st.trouble ? note_of(state) : "", sizeof s_st.note);
    taskEXIT_CRITICAL(&s_lock);
}

/* --------------------------------------------------------- the connection */

/* The receiver's addresses. A name can stand for several, and not all of
 * them need be the receiver -- kiwi.on4cdj.be has a second that resets -- so
 * each is tried in turn, as a browser would (CONFIG_LWIP_DNS_MAX_HOST_IP). */
typedef struct {
    struct sockaddr_in a[4];
    int n;
} addrs_t;

static const char *resolve(const char *host, uint16_t port, addrs_t *out)
{
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM }, *ai = NULL;
    char ps[8];
    snprintf(ps, sizeof ps, "%u", (unsigned)port);
    out->n = 0;
    if (getaddrinfo(host, ps, &hints, &ai) != 0 || !ai) {
        ESP_LOGW(TAG, "%s: name not found", host);
        return "name not found";
    }
    for (const struct addrinfo *p = ai; p && out->n < 4; p = p->ai_next)
        if (p->ai_family == AF_INET && p->ai_addrlen >= sizeof out->a[0])
            memcpy(&out->a[out->n++], p->ai_addr, sizeof out->a[0]);
    freeaddrinfo(ai);
    return out->n ? NULL : "name not found";
}

/* A connected socket, or -1 with `why` saying what stopped it. */
static int tcp_connect(const struct sockaddr_in *sa, int timeout_ms, const char **why)
{
    const char *w = NULL;
    int fd = socket(AF_INET, SOCK_STREAM, 0), e = errno;
    if (fd < 0) { w = "no free socket"; goto fail; }
    const int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    const int r = connect(fd, (const struct sockaddr *)sa, sizeof *sa);
    e = errno;
    if (r != 0 && e != EINPROGRESS) {
        w = e == EHOSTUNREACH || e == ENETUNREACH ? "no route" : "refused";
        goto fail;
    }
    fd_set wf;
    FD_ZERO(&wf);
    FD_SET(fd, &wf);
    struct timeval tv = { .tv_sec = timeout_ms / 1000, .tv_usec = (timeout_ms % 1000) * 1000 };
    const int sr = select(fd + 1, NULL, &wf, NULL, &tv);
    if (sr <= 0) { e = sr < 0 ? errno : 0; w = sr < 0 ? "select failed" : "no answer"; goto fail; }
    socklen_t el = sizeof e;
    e = 0;
    getsockopt(fd, SOL_SOCKET, SO_ERROR, &e, &el);
    if (e) { w = e == ECONNREFUSED ? "refused" : e == EHOSTUNREACH ? "no route" : "no answer"; goto fail; }
    fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
    struct timeval io = { .tv_sec = 5 };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &io, sizeof io);
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &io, sizeof io);
    return fd;
fail: {
        char ip[16];
        inet_ntoa_r(sa->sin_addr, ip, sizeof ip);
        ESP_LOGW(TAG, "%s:%u: %s (errno %d)", ip, (unsigned)ntohs(sa->sin_port), w, e);
    }
    if (fd >= 0) close(fd);
    if (why) *why = w;
    return -1;
}

static bool send_all(int fd, const void *b, size_t n)
{
    const uint8_t *p = b;
    while (n) {
        int k = send(fd, p, n, 0);
        if (k <= 0) return false;
        p += k;
        n -= (size_t)k;
    }
    return true;
}

/* The upgrade: true once the server has said 101. */
static bool ws_open(int fd, const char *host, uint16_t port, bool new_path)
{
    uint8_t key[16];
    esp_fill_random(key, sizeof key);
    static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char k64[25];
    int o = 0;
    for (int i = 0; i < 16; i += 3) {
        uint32_t v = key[i] << 16 | (i + 1 < 16 ? key[i + 1] << 8 : 0) | (i + 2 < 16 ? key[i + 2] : 0);
        k64[o++] = B64[v >> 18 & 63];
        k64[o++] = B64[v >> 12 & 63];
        k64[o++] = i + 1 < 16 ? B64[v >> 6 & 63] : '=';
        k64[o++] = i + 2 < 16 ? B64[v & 63] : '=';
    }
    k64[o] = 0;
    /* 32 bits of timestamp, as kiwiclient: a real Kiwi takes no more. */
    const uint32_t ts = (uint32_t)(esp_timer_get_time() / 1000000) ^ esp_random();
    char req[320];
    int n = snprintf(req, sizeof req,
        "GET %s%lu/SND HTTP/1.1\r\nHost: %s:%u\r\nUpgrade: websocket\r\n"
        "Connection: Upgrade\r\nSec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n"
        "User-Agent: VFO-Knob\r\n\r\n",
        new_path ? "/ws/kiwi/" : "/", (unsigned long)ts, host, (unsigned)port, k64);
    if (!send_all(fd, req, (size_t)n)) return false;
    /* The status line; the rest of the headers only read past, to the blank
     * line after them, where the frames begin. */
    char line[16];
    size_t got = 0;
    uint32_t tail = 0;
    for (int i = 0; i < 4096; i++) {
        char ch;
        if (recv(fd, &ch, 1, 0) != 1) return false;
        if (got < sizeof line - 1) line[got++] = ch;
        tail = tail << 8 | (uint8_t)ch;
        if (tail == 0x0D0A0D0A) {
            line[got] = 0;
            return strncmp(line, "HTTP/1.1 101", 12) == 0 || strncmp(line, "HTTP/1.0 101", 12) == 0;
        }
    }
    return false;
}

static bool ws_text(int fd, const char *t)
{
    const size_t n = strlen(t);
    if (n > 240) return false;                /* the longest is the login */
    uint8_t f[240 + 8];
    size_t h = 0;
    f[h++] = 0x81;
    if (n < 126) f[h++] = 0x80 | (uint8_t)n;
    else { f[h++] = 0x80 | 126; f[h++] = (uint8_t)(n >> 8); f[h++] = (uint8_t)n; }
    uint8_t m[4];
    esp_fill_random(m, 4);
    memcpy(f + h, m, 4);
    h += 4;
    for (size_t i = 0; i < n; i++) f[h + i] = (uint8_t)t[i] ^ m[i & 3];
    return send_all(fd, f, h + n);
}

static bool ws_pong(int fd, const uint8_t *p, size_t n)
{
    if (n > 125) n = 125;
    uint8_t f[125 + 6];
    f[0] = 0x8A;
    f[1] = 0x80 | (uint8_t)n;
    uint8_t m[4];
    esp_fill_random(m, 4);
    memcpy(f + 2, m, 4);
    for (size_t i = 0; i < n; i++) f[6 + i] = p[i] ^ m[i & 3];
    return send_all(fd, f, 6 + n);
}

/* ---------------------------------------------------------------- audio */

static const int16_t STEP[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55,
    60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307,
    337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411,
    1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358,
    5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500,
    20350, 22385, 24623, 27086, 29794, 32767 };
static const int8_t IDX[16] = { -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8 };

typedef struct {
    int32_t pred;
    int     idx;
    float   pos, step;      /* the resampler: input rate / output rate */
    int16_t prev;
} dsp_t;

static int adpcm(dsp_t *d, const uint8_t *in, size_t n, int16_t *out)
{
    int o = 0;
    for (size_t i = 0; i < n; i++) {
        for (int half = 0; half < 2; half++) {
            const uint8_t code = half ? in[i] >> 4 : in[i] & 0x0F;
            const int step = STEP[d->idx];
            int diff = step >> 3;
            if (code & 4) diff += step;
            if (code & 2) diff += step >> 1;
            if (code & 1) diff += step >> 2;
            d->pred += code & 8 ? -diff : diff;
            if (d->pred > 32767) d->pred = 32767;
            if (d->pred < -32768) d->pred = -32768;
            d->idx += IDX[code];
            if (d->idx < 0) d->idx = 0;
            if (d->idx > 88) d->idx = 88;
            out[o++] = (int16_t)d->pred;
        }
    }
    return o;
}

/* Linear interpolation from the receiver's rate to the knob's 24 kHz. */
static int resample(dsp_t *d, const int16_t *x, int n, int16_t *out)
{
    int o = 0;
    while (d->pos < n && o < OUT_MAX) {
        const int   i = (int)d->pos;
        const float f = d->pos - i;
        const float a = i == 0 ? d->prev : x[i - 1], b = x[i];
        out[o++] = (int16_t)(a + (b - a) * f);
        d->pos += d->step;
    }
    d->pos -= n;
    d->prev = x[n - 1];
    return o;
}

/* ---------------------------------------------------------------- tuning */

/* The radio's mode as the Kiwi names it, and a passband that makes sense. */
static const char *kiwi_mode(const char *m, int32_t *lo, int32_t *hi)
{
    const char *k = "usb";
    if (!strcasecmp(m, "lsb") || !strcasecmp(m, "digl")) k = "lsb";
    else if (!strcasecmp(m, "cw") || !strcasecmp(m, "cwr") ||
             !strcasecmp(m, "cwu") || !strcasecmp(m, "cwl")) k = "cw";      /* UberSDR's names */
    else if (!strcasecmp(m, "am")) k = "am";
    else if (!strcasecmp(m, "sam")) k = "sam";
    else if (!strcasecmp(m, "fm") || !strcasecmp(m, "nfm")) k = "nbfm";
    int32_t l = *lo, h = *hi;
    if (!strcmp(k, "lsb") && l > 0) { const int32_t t = l; l = -h; h = -t; }
    if (!strcmp(k, "cw")) {
        /* the Kiwi's CW is centred on a 500 Hz tone */
        int32_t w = h - l;
        if (w <= 0 || w > 3000) w = 500;
        l = 500 - w / 2;
        h = 500 + w / 2;
    }
    if (l >= h) {
        if (!strcmp(k, "lsb"))       { l = -2700; h = -300; }
        else if (!strcmp(k, "usb"))  { l = 300;   h = 2700; }
        else if (!strcmp(k, "nbfm")) { l = -6000; h = 6000; }
        else                         { l = -4900; h = 4900; }
    }
    *lo = l;
    *hi = h;
    return k;
}

static bool send_tune(int fd, uint32_t *gen)
{
    int64_t hz;
    char mode[8];
    int32_t lo, hi;
    taskENTER_CRITICAL(&s_lock);
    hz = s_tune.hz;
    strlcpy(mode, s_tune.mode, sizeof mode);
    lo = s_tune.lo;
    hi = s_tune.hi;
    *gen = s_tune.gen;
    taskEXIT_CRITICAL(&s_lock);
    if (hz <= 0) return true;
    const char *k = kiwi_mode(mode, &lo, &hi);
    char cmd[112];
    snprintf(cmd, sizeof cmd, "SET mod=%s low_cut=%ld high_cut=%ld freq=%.3f",
             k, (long)lo, (long)hi, (double)hz / 1000.0);
    return ws_text(fd, cmd);
}

/* ---------------------------------------------------------------- session */

enum { END_WANT = 0, END_ERROR = -1, END_NO_ANSWER = -2, END_BADP = -3, END_BUSY = -4,
       END_APPS_FULL = -5, END_NO_APPS = -6, END_DAY_LIMIT = -7 };

static uint8_t *s_buf;
static int16_t *s_pcm, *s_out;

/* The value of `key` in a MSG's "key=value key2=value" list, into v. */
static bool msg_val(const char *msg, const char *key, char *v, size_t cap)
{
    const size_t kl = strlen(key);
    for (const char *p = msg; (p = strstr(p, key)); p += kl) {
        if ((p == msg || p[-1] == ' ') && p[kl] == '=') {
            const char *s = p + kl + 1;
            size_t n = strcspn(s, " ");
            if (n >= cap) n = cap - 1;
            memcpy(v, s, n);
            v[n] = 0;
            return true;
        }
    }
    return false;
}

static int session(int idx, const sdr_cfg_t *c, bool new_path)
{
    addrs_t ad;
    const char *why = resolve(c->host, c->port, &ad);
    if (why) { set_state(idx, why, NULL); return END_ERROR; }
    int fd = -1;
    bool refused = false;
    for (int i = 0; i < ad.n && fd < 0; i++) {
        fd = tcp_connect(&ad.a[i], ad.n > 1 ? 3000 : 5000, &why);
        if (fd >= 0 && !ws_open(fd, c->host, c->port, new_path)) { close(fd); fd = -1; refused = true; }
    }
    if (fd < 0) {
        set_state(idx, refused ? "refused" : why, NULL);
        /* Turned away at the door: the other path may be the one. */
        return refused ? END_NO_ANSWER : END_ERROR;
    }

    char auth[128];
    snprintf(auth, sizeof auth, "SET auth t=kiwi p=%s%s%s",
             c->pass[0] ? c->pass : (c->ipl[0] ? "#" : ""), c->ipl[0] ? " ipl=" : "", c->ipl);
    ws_text(fd, auth);

    dsp_t d = { .step = 0.5f };
    int rx_chans = 0;
    bool in = false, streaming = false;
    size_t have = 0, skip = 0;
    uint32_t tgen = 0;
    int64_t t_open = esp_timer_get_time(), t_rx = t_open, t_ka = 0, t_tuned = 0;
    const uint32_t list_gen = s_list_gen;
    int end = END_ERROR;

    for (;;) {
        const int64_t now = esp_timer_get_time();
        if (s_want != idx || s_list_gen != list_gen) { end = END_WANT; break; }
        if (!in && now - t_open > ANSWER_MS * 1000LL) { end = END_NO_ANSWER; break; }
        if (now - t_rx > QUIET_MS * 1000LL) { set_state(idx, "went quiet", NULL); break; }
        if (in && now - t_ka > 1000000) { ws_text(fd, "SET keepalive"); t_ka = now; }
        if (in && s_tune.gen != tgen && now - t_tuned > 100000) {
            send_tune(fd, &tgen);
            t_tuned = now;
        }

        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(fd, &rf);
        struct timeval tv = { .tv_usec = 100000 };
        const int r = select(fd + 1, &rf, NULL, NULL, &tv);
        if (r < 0) break;
        if (r == 0) continue;
        const int k = recv(fd, s_buf + have, BUF_BYTES - have, 0);
        /* Hung up on before the login's answer: the Web-888 takes the new
         * path's upgrade and then closes -- the old path is its own. */
        if (k <= 0) { set_state(idx, "closed", NULL); if (!in) end = END_NO_ANSWER; break; }
        t_rx = now;
        if (skip) {
            const size_t s = (size_t)k < skip ? (size_t)k : skip;
            skip -= s;
            memmove(s_buf, s_buf + s, (size_t)k - s);
            have = (size_t)k - s;
        } else {
            have += (size_t)k;
        }

        /* Every complete frame in the buffer. */
        for (;;) {
            if (have < 2) break;
            const uint8_t op = s_buf[0] & 0x0F;
            size_t hl = 2, len = s_buf[1] & 0x7F;
            if (len == 126) { if (have < 4) break; len = s_buf[2] << 8 | s_buf[3]; hl = 4; }
            else if (len == 127) {
                if (have < 10) break;
                len = 0;
                for (int i = 6; i < 10; i++) len = len << 8 | s_buf[i];
                hl = 10;
            }
            if (s_buf[1] & 0x80) hl += 4;                   /* never, from a server */
            if (hl + len > BUF_BYTES) {                     /* too big: let it go by */
                skip = hl + len - have;
                have = 0;
                break;
            }
            if (have < hl + len) break;
            const uint8_t *p = s_buf + hl;

            if (op == 0x8) { set_state(idx, "closed", NULL); if (!in) end = END_NO_ANSWER; goto out; }
            if (op == 0x9) ws_pong(fd, p, len);
            if ((op == 0x1 || op == 0x2) && len >= 3) {
                if (!memcmp(p, "MSG", 3) && len > 4) {
                    char msg[384], v[48];
                    const size_t ml = len - 4 < sizeof msg - 1 ? len - 4 : sizeof msg - 1;
                    memcpy(msg, p + 4, ml);
                    msg[ml] = 0;
                    if (msg_val(msg, "badp", v, sizeof v)) {
                        if (strcmp(v, "0") != 0) {
                            set_state(idx, strcmp(v, "1") == 0 ? "wrong password" : "refused", NULL);
                            end = END_BADP;
                            goto out;
                        }
                        in = true;
                        ws_text(fd, "SET squelch=0 max=0");
                        ws_text(fd, "SET genattn=0");
                        ws_text(fd, "SET gen=0 mix=-1");
                        ws_text(fd, "SET ident_user=VFO-Knob");
                        ws_text(fd, "SET compression=1");
                        ws_text(fd, "SET agc=1 hang=0 thresh=-100 slope=6 decay=1000 manGain=50");
                        send_tune(fd, &tgen);
                        t_tuned = now;
                        set_state(idx, "logged in", NULL);
                        s_path[idx] = new_path ? 1 : 2;
                    }
                    if (msg_val(msg, "rx_chans", v, sizeof v)) rx_chans = atoi(v);
                    /* too_busy=N: all N of its channels are taken -- or, N
                     * fewer than it has, the channels its owner lets apps
                     * have rather than browsers. A Kiwi says so some seconds
                     * in, seeing no waterfall; with none for apps at all
                     * (ON4CDJ's), the knob does not come back. */
                    if (msg_val(msg, "too_busy", v, sizeof v)) {
                        const int n = atoi(v);
                        if (rx_chans > 0 && n < rx_chans) {
                            set_state(idx, n ? "app channels in use" : "no apps allowed", NULL);
                            end = n ? END_APPS_FULL : END_NO_APPS;
                        } else {
                            set_state(idx, "busy", NULL);
                            end = END_BUSY;
                        }
                        goto out;
                    }
                    if (msg_val(msg, "redirect", v, sizeof v)) {
                        set_state(idx, "busy", NULL);
                        end = END_BUSY;
                        goto out;
                    }
                    /* Its listening time per address per day, used up: the
                     * time-limit password is what lifts it. */
                    if (msg_val(msg, "ip_limit", v, sizeof v)) {
                        set_state(idx, "daily limit reached", NULL);
                        end = END_DAY_LIMIT;
                        goto out;
                    }
                    if (msg_val(msg, "down", v, sizeof v)) {
                        set_state(idx, "down", NULL);
                        end = END_BUSY;
                        goto out;
                    }
                    if (msg_val(msg, "audio_rate", v, sizeof v)) {
                        char ar[64];
                        snprintf(ar, sizeof ar, "SET AR OK in=%d out=%d", atoi(v), AUDIO_RATE_HZ);
                        ws_text(fd, ar);
                        d.step = atof(v) / AUDIO_RATE_HZ;
                    }
                    if (msg_val(msg, "sample_rate", v, sizeof v)) {
                        const float sr = atof(v);
                        if (sr > 4000.0f) d.step = sr / AUDIO_RATE_HZ;
                    }
                } else if (!memcmp(p, "SND", 3) && in && len > 10) {
                    const uint8_t flags = p[3];
                    const uint16_t sm = (uint16_t)(p[8] << 8 | p[9]);
                    const uint8_t *data = p + 10;
                    const size_t dn = len - 10;
                    int n = 0;
                    if (flags & 0x10) {
                        if (dn * 2 <= PCM_MAX) n = adpcm(&d, data, dn, s_pcm);
                    } else {
                        for (size_t i = 0; i + 1 < dn && n < PCM_MAX; i += 2)
                            s_pcm[n++] = (int16_t)(data[i] << 8 | data[i + 1]);
                    }
                    if (n > 0) {
                        const int o = resample(&d, s_pcm, n, s_out);
                        if (!streaming) {
                            streaming = true;
                            audio_out_sdr(true);
                            set_state(idx, "streaming", NULL);
                            ESP_LOGI(TAG, "streaming from %s:%u (%s path), %.0f Hz",
                                     c->host, (unsigned)c->port, new_path ? "new" : "old",
                                     (double)(d.step * AUDIO_RATE_HZ));
                        }
                        audio_out_feed_sdr(s_out, (size_t)o);
                    }
                    taskENTER_CRITICAL(&s_lock);
                    s_st.smeter_dbm = 0.1f * sm - 127.0f;
                    taskEXIT_CRITICAL(&s_lock);
                }
            }
            memmove(s_buf, s_buf + hl + len, have - hl - len);
            have -= hl + len;
        }
    }
out:
    close(fd);
    return end;
}

static void sdr_task(void *arg)
{
    (void)arg;
    uint32_t backoff = 2000;
    for (;;) {
        const int want = s_want;
        if (want < 0 || want >= s_n) {
            audio_out_sdr(false);
            set_state(-1, "off", "");
            backoff = 2000;
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        sdr_cfg_t c;
        sdr_get(want, &c);
        /* Not before the radio says where it is: the receiver would play its
         * own default frequency meanwhile. */
        if (s_tune.hz <= 0) {
            set_state(want, WAIT_RADIO, c.name[0] ? c.name : c.host);
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        set_state(want, "connecting", c.name[0] ? c.name : c.host);
        /* The path it answered on before; else the new, then the old. */
        const uint8_t known = s_path[want];
        int end = session(want, &c, known != 2);
        if (end == END_NO_ANSWER && !known) end = session(want, &c, false);
        audio_out_sdr(false);
        if (s_want != want) { backoff = 2000; continue; }
        if (end == END_NO_ANSWER) set_state(want, "no answer", NULL);
        /* A wrong password waits for a new one; busy or gone, a while. */
        /* A wrong password waits a minute, a full receiver a while; one that
         * takes no apps, until it is chosen again. */
        const uint32_t wait = end == END_BADP      ? 60000   : end == END_BUSY      ? 20000   :
                              end == END_APPS_FULL ? 120000  : end == END_DAY_LIMIT ? 1800000 :
                              end == END_NO_APPS   ? UINT32_MAX : backoff;
        if (wait == UINT32_MAX)
            ESP_LOGW(TAG, "%s:%u: %s -- not again until chosen again", c.host, (unsigned)c.port, s_st.state);
        else
            ESP_LOGW(TAG, "%s:%u: %s -- again in %lu s", c.host, (unsigned)c.port, s_st.state,
                     (unsigned long)(wait / 1000));
        const uint32_t gen = s_list_gen;
        for (uint32_t t = 0; t < wait && s_want == want && s_list_gen == gen; t += 100) {
            vTaskDelay(pdMS_TO_TICKS(100));
            if (wait == UINT32_MAX) t = 0;
        }
        backoff = backoff < 30000 ? backoff * 2 : 30000;
    }
}

esp_err_t sdr_rx_init(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t n = 0;
        if (nvs_get_str(h, KEY_LIST, NULL, &n) == ESP_OK && n > 1) {
            char *blob = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
            if (blob && nvs_get_str(h, KEY_LIST, blob, &n) == ESP_OK) list_parse(blob);
            free(blob);
        }
        int8_t sel = -1, bal = 0;
        if (nvs_get_i8(h, KEY_SEL, &sel) == ESP_OK && sel >= 0 && sel < s_n) s_want = sel;
        if (nvs_get_i8(h, KEY_BAL, &bal) == ESP_OK && bal >= -100 && bal <= 100) s_bal = bal;
        nvs_close(h);
    }
    audio_out_set_balance(s_bal);
    s_buf = heap_caps_malloc(BUF_BYTES, MALLOC_CAP_SPIRAM);
    s_pcm = heap_caps_malloc(PCM_MAX * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    s_out = heap_caps_malloc(OUT_MAX * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    ESP_RETURN_ON_FALSE(s_buf && s_pcm && s_out, ESP_ERR_NO_MEM, TAG, "buffers");
    const esp_timer_create_args_t ta = { .callback = save_cb, .name = "sdrsave" };
    esp_timer_create(&ta, &s_save_timer);
    ESP_RETURN_ON_FALSE(xTaskCreatePinnedToCoreWithCaps(sdr_task, "sdr", 6144, NULL, 5, NULL, 0,
                                                        MALLOC_CAP_SPIRAM) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "task");
    ESP_LOGI(TAG, "%d web SDR%s configured%s", s_n, s_n == 1 ? "" : "s",
             s_want >= 0 ? ", one selected" : "");
    return ESP_OK;
}

/* ------------------------------------------------------------------ test */

static const char *find(const uint8_t *b, size_t n, const char *s)
{
    const size_t sn = strlen(s);
    for (size_t i = 0; i + sn <= n; i++)
        if (!memcmp(b + i, s, sn)) return (const char *)b + i;
    return NULL;
}

static void json_esc(char *out, size_t cap, const char *s)
{
    size_t o = 0;
    for (; *s && o + 2 < cap; s++) {
        if (*s == '"' || *s == '\\') { out[o++] = '\\'; out[o++] = *s; }
        else if ((unsigned char)*s >= 0x20) out[o++] = *s;
    }
    out[o] = 0;
}

esp_err_t sdr_test(const sdr_cfg_t *c, char *json, size_t cap)
{
    if (!c || !c->host[0] || !json) return ESP_ERR_INVALID_ARG;
    char name[64] = "", sw[48] = "", users[8] = "?", umax[8] = "?";
    /* Its /status: what it calls itself, and how full it is -- from the first
     * of its addresses that takes a connection. */
    addrs_t ad;
    const char *why = resolve(c->host, c->port, &ad);
    int fd = -1, at = 0;
    for (; !why && at < ad.n; at++)
        if ((fd = tcp_connect(&ad.a[at], ad.n > 1 ? 3000 : 5000, &why)) >= 0) break;
    if (fd < 0) {
        char eh[132];
        json_esc(eh, sizeof eh, c->host);
        snprintf(json, cap, "{\"ok\":false,\"error\":\"%s:%u: %s\"}", eh, (unsigned)c->port, why);
        return ESP_OK;
    }
    char req[160];
    const int rn = snprintf(req, sizeof req, "GET /status HTTP/1.0\r\nHost: %s:%u\r\n\r\n",
                            c->host, (unsigned)c->port);
    send_all(fd, req, (size_t)rn);
    char *st = heap_caps_calloc(1, 4096, MALLOC_CAP_SPIRAM);
    if (st) {
        size_t got = 0;
        int k;
        while (got < 4095 && (k = recv(fd, st + got, 4095 - got, 0)) > 0) got += (size_t)k;
        for (char *line = strtok(st, "\r\n"); line; line = strtok(NULL, "\r\n")) {
            if (!strncmp(line, "name=", 5)) strlcpy(name, line + 5, sizeof name);
            else if (!strncmp(line, "sw_version=", 11)) strlcpy(sw, line + 11, sizeof sw);
            else if (!strncmp(line, "users=", 6)) strlcpy(users, line + 6, sizeof users);
            else if (!strncmp(line, "users_max=", 10)) strlcpy(umax, line + 10, sizeof umax);
        }
        free(st);
    }
    close(fd);

    /* Then a login, on either path, for the answer to the password. */
    const char *login = "no answer", *path = "";
    int apps = -1;                          /* the channels apps may have */
    for (int pass = 0; pass < 2 && !strcmp(login, "no answer"); pass++) {
        const bool np = pass == 0;
        fd = tcp_connect(&ad.a[at], 5000, NULL);
        if (fd < 0) break;
        if (ws_open(fd, c->host, c->port, np)) {
            char auth[128];
            snprintf(auth, sizeof auth, "SET auth t=kiwi p=%s%s%s",
                     c->pass[0] ? c->pass : (c->ipl[0] ? "#" : ""), c->ipl[0] ? " ipl=" : "", c->ipl);
            ws_text(fd, auth);
            uint8_t *b = heap_caps_malloc(BUF_BYTES, MALLOC_CAP_SPIRAM);
            size_t have = 0;
            int64_t t0 = esp_timer_get_time();
            bool in = false;
            while (b && esp_timer_get_time() - t0 < ANSWER_MS * 1000LL) {
                fd_set rf;
                FD_ZERO(&rf);
                FD_SET(fd, &rf);
                struct timeval tv = { .tv_usec = 200000 };
                if (select(fd + 1, &rf, NULL, NULL, &tv) <= 0) continue;
                const int k = recv(fd, b + have, BUF_BYTES - 1 - have, 0);
                if (k <= 0) break;
                have += (size_t)k;
                b[have] = 0;
                /* The answers are text inside the frames: look for them. */
                const char *m;
                if (!in && (m = find(b, have, "badp="))) {
                    login = m[5] == '0' ? "ok" : m[5] == '1' ? "wrong password" : "refused";
                    path = np ? "new" : "old";
                    if (m[5] != '0') break;
                    /* In: its configuration follows, and says how many of
                     * its channels apps may have (a browser's are apart). */
                    in = true;
                    t0 = esp_timer_get_time();
                }
                if (!in && find(b, have, "too_busy")) { login = "busy"; path = np ? "new" : "old"; break; }
                if (in && (m = find(b, have, "ext_api_nchans%22%3a"))) {
                    const char *d = m + 20, *e = (const char *)b + have;
                    if (d + 3 <= e && !memcmp(d, "%20", 3)) d += 3;
                    if (d < e && *d >= '0' && *d <= '9' && memchr(d, '%', (size_t)(e - d))) {
                        apps = atoi(d);
                        break;
                    }
                }
                if (have > BUF_BYTES - 1024) {          /* keep a tail: a key may straddle */
                    memmove(b, b + have - 64, 64);
                    have = 64;
                }
            }
            free(b);
        }
        close(fd);
    }

    if (apps == 0 && !strcmp(login, "ok")) login = "no apps";
    char en[128], es[96];
    json_esc(en, sizeof en, name);
    json_esc(es, sizeof es, sw);
    snprintf(json, cap,
             "{\"ok\":%s,\"name\":\"%s\",\"sw\":\"%s\",\"users\":\"%s\",\"users_max\":\"%s\","
             "\"login\":\"%s\",\"path\":\"%s\",\"apps\":%d}",
             strcmp(login, "ok") == 0 ? "true" : "false", en, es, users, umax, login, path, apps);
    return ESP_OK;
}
