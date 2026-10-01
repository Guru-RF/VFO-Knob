/* FlexRadio client: the multiflex firmware's radio.h -- a FLEX-6000/8000 over
 * its own API, with the knob as one of the radio's MultiFlex stations.
 *
 * The API is text over TCP 4992. The radio greets with its protocol version
 * (V1.4.0.0) and our handle (H08893C28); commands go out as C<seq>|<command>
 * and are answered R<seq>|<hex code>|<text>; the state of everything we
 * subscribe to arrives as S<handle>|<object> key=value ... lines. Meter
 * readings, the panadapter and audio travel over UDP as VITA-49 packets, to
 * the port named with `client udpport`; our microphone goes the other way,
 * to the radio's UDP 4991.
 *
 * At boot the knob asks, on the dial, what to be -- when there is a choice:
 *
 *  - a station of its own (`client gui <uuid>`), like SmartSDR, AetherSDR or
 *    a Maestro beside it: the radio gives a new station a panadapter and a
 *    slice, and gives a known one back what it had -- so the uuid is kept
 *    (NVS), and the knob comes back on its own slice, where it was. Its own
 *    audio both ways, its own transmit slice;
 *  - or the dial and PTT for one already there (`client bind client_id=`):
 *    no slice or audio of its own, the station's active slice under the
 *    dial, and PTT keying the station's transmitter -- its microphone, its
 *    settings, like a FlexControl on its computer. Chosen from the stations
 *    `sub client all` lists; remembered, and offered first next time.
 *
 * What the dial works is what carries our handle -- or the station's -- as
 * client_handle; everything else is left alone. Measured against a FLEX-6600
 * on SmartSDR 4.2.20:
 *
 *  - each station has its own transmit settings, and ours start with the
 *    microphone on the network (mic_selection=PC);
 *  - the radio does not echo a client's own changes back to it -- a
 *    `slice tune`, `filt`, AGC, RIT or gain set is answered R|0| and nothing
 *    more -- so the dial shows what it asked for at once, and what arrives in
 *    a status is a change made elsewhere. A mode change is the exception: it
 *    brings the mode's filter and AGC back with it.
 *
 * Audio is Opus both ways, as SmartSDR's own remote audio is: the radio sends
 * 10 ms CELT frames of 24 kHz stereo (40-50 kbit/s, where the uncompressed
 * stream is 1.4 Mbit/s and crackled on WiFi), and takes the microphone the
 * same way, in mono. The codec has a task of its own, below this one.
 *
 * Written from the protocol as AetherSDR speaks it (src/core, src/models);
 * none of its code is used.
 */
#include "radio.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>

#include <opus.h>

#include "audio_in.h"
#include "audio_out.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_tls.h"
#include "smartlink.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/ringbuf.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "nvs.h"
#include <fcntl.h>
#include "ptt_fsm.h"
#include "vfo_tune.h"

static const char *TAG = "flex";

#define API_PORT       4992
#define TX_PORT        4991          /* where the radio takes our audio */
#define OUR_PROGRAM    "VFO-Knob"
#define OUR_STATION    "VFO-Knob"    /* how the other stations see us */
#define NVS_NS         "flex"

#define LOOP_MS        5
#define CONNECT_MS     3000
#define GREET_MS       5000          /* connected, but no V/H lines */
#define SETUP_MS       10000         /* registered, but no slice of ours */
#define PING_MS        1000          /* AetherSDR's keepalive interval */
#define PING_MISS      5             /* unanswered pings: the link is gone */
#define ADOPT_MS       400           /* after `slice list`: statuses settle */
#define LIST_MS        700           /* after `sub client all`: the stations settle */
#define ASK_MS         30000         /* the question unanswered: its default */
#define N_STATIONS     (RADIO_CHOICES - 1)
#define N_SLC          8             /* slices a radio has, at most */
#define PONG_FRESH_MS  3000
#define SEND_MS        50            /* tune sets, while turning */
#define QUIET_MS       300           /* then a status may move the dial */
#define F_MIN          30000LL       /* the FLEX-6000s receive 30 kHz... */
#define F_MAX          54000000LL    /* ...to 54 MHz */
#define LINE_CAP       8192          /* the longest status line kept */
#define N_METERS       64
#define N_PENDING      32

/* Opus: the radio's frame, and ours. */
#define FRAME          240           /* 10 ms at 24 kHz */
#define OPUS_MAX       400           /* bytes of one encoded frame, and room */
#define TX_BITRATE     48000
#define TX_COMPLEXITY  0             /* CELT, mono: at 2 it took 5.4 ms of each
                                        10 ms frame on the S3, worst 14 */
#define RXQ_BYTES      8192          /* Opus packets waiting for the codec */
#define TUNE_MAX_MS    30000         /* a tune carrier, at most */
#define ATU_MAX_MS     20000         /* a tuner's cycle, at most */

/* VITA-49 packet class codes (the low half of the class id's second word). */
#define PCC_METER      0x8002
#define PCC_OPUS       0x8005

/* ----------------------------------------------------------------- state */

typedef struct {
    radio_link_t link;
    tune_t     tune;
    int64_t    f_committed, f_server;
    uint32_t   t_last_input_ms, t_last_send_ms, t_ready_ms;
    bool       have_freq, remote_pending;
    char       mode[8];              /* the editors' names, lower case */
    int32_t    filt_lo, filt_hi;
    char       agc[6];
    bool       have_gain;
    int8_t     gain, gain_min, gain_max, gain_step;
    int32_t    rit_hz;
    float      smeter_dbm;
    float      tx_mic_dbm, tx_fwd_w, tx_peak_w, tx_swr, tx_alc;
    uint32_t   t_peak;
    bool       tx;                   /* the radio is transmitting, whoever keyed it */
    bool       tx_ours;              /* ...and it is our slice */
    bool       tx_allowed;           /* the interlock's own word */
    char       tx_reason[24];        /* ...and its reason when not */
    bool       tx_slice;             /* our slice is our transmit slice */
    uint32_t   connects, closes, reconciles, rejects, unknown_cmds, sends, echoes;
    uint32_t   chronos, txa_sent, txa_failed, txa_skipped, txa_max_us;
    uint32_t   rx_packets, rx_lost, rx_concealed, dec_max_us;
    char       last_close[48];
    ptt_fsm_t  ptt;
    uint32_t   pending_key, pending_unkey, pending_toggle;
    uint8_t    pending_abort;
    bool       audio_suspend;
    /* What a key is for: an over (the microphone), the tune carrier, or the
     * tuner's cycle -- chosen when it is asked for. */
    uint8_t    kind;                 /* KIND_* */
    uint32_t   t_keyed;              /* on the air since */
    uint32_t   pending_tune, pending_atu;
    bool       has_atu;              /* the radio has a tuner, and it is enabled */
    bool       atu_mem;              /* ...and recalls its memories */
    char       note[16];             /* radio.h: said once, for a moment */
    uint32_t   note_seq;
    /* The question at boot (radio_get_choice): our own station, or the dial
     * for one already on the radio. */
    uint8_t    n_choices, choice_default;
    uint32_t   choices_seq;
    char       ch_title[RADIO_CHOICES][12], ch_name[RADIO_CHOICES][24];
    int8_t     pending_choice;       /* -1 none */
} state_t;

enum { KIND_VOICE = 0, KIND_TUNE, KIND_ATU };


static state_t      S;
static portMUX_TYPE S_LOCK = portMUX_INITIALIZER_UNLOCKED;

/* One meter the radio has defined: where it measures, and what. */
typedef struct {
    uint16_t id;
    int16_t  num;
    char     src[6], nam[10];
} meter_t;

typedef enum {
    P_NONE = 0, P_PING, P_GUI, P_BIND, P_UDPPORT, P_SLICE_LIST, P_PAN_CREATE, P_SLICE_CREATE,
    P_CLIENT_IP,
    P_RFGAIN_INFO, P_RX_STREAM, P_TX_STREAM, P_XMIT_ON,
} pend_t;

/* The session: owned by the task, never touched from outside -- save the
 * few words the codec task reads to send the microphone (radio, ufd,
 * tx_stream), each written once per session. */
static struct {
    char       host[48];
    uint16_t   port;
    struct sockaddr_in radio;
    int        fd, ufd;
    /* By SmartLink: the API over TLS (fd is its socket), and UDP to the
     * radio's public port -- registered until its first packet, then kept
     * open with a ping (wan_udp). */
    bool       wan;
    esp_tls_t *tls;
    struct sockaddr_in udp_to;
    bool       udp_ok, ip_answered;
    uint32_t   t_udp, t_udp_first;
    uint16_t   uport;
    uint32_t   handle;               /* ours, from the H line */
    char       uuid[40];             /* our station's id, kept in NVS */
    uint32_t   seq;
    struct { uint32_t seq; pend_t what; } pend[N_PENDING];
    char      *line;                 /* LINE_CAP, PSRAM */
    size_t     line_n;
    bool       line_skip;            /* inside a line too long to keep */
    bool       greeted, registered, listed;
    int        slice;                /* ours; -1 = none yet */
    uint32_t   pan, waterfall;       /* our panadapter, and its waterfall */
    uint32_t   rx_stream, tx_stream;
    bool       creating, streams_asked, tx_claimed;
    bool       pan_tamed, wf_tamed;
    uint32_t   t_session, t_listed, t_ping, t_pong, t_retry;
    uint8_t    pings_out;
    uint32_t   backoff_ms;
    meter_t   *meters;               /* N_METERS, PSRAM */
    uint8_t    n_meters;
    uint16_t   m_level, m_fwd, m_swr, m_alc;
    /* This over's microphone, for the line logged when it ends. */
    uint16_t   over_frames, over_silent;
    int        over_peak;
    uint32_t   enc_us_sum;
    /* The other stations on the radio, and what this knob is: a station of
     * its own, or the dial for one of them (bound: its handle). The choice
     * holds across reconnects; the question is asked again only when the
     * station we dial for leaves. */
    struct station { uint32_t handle; char id[40], name[24]; } st[N_STATIONS], q[N_STATIONS];
    uint8_t    n_st, n_q;
    bool       decided, own, asking, registering, station_left;
    char       pick_id[40], pick_name[24];
    uint32_t   bound;
    uint32_t   t_hello, t_asked;
    /* A tuner cycle we started, until the tuner says how it went -- which
     * may come after the radio is back on receive. */
    bool       atu_waiting;
    uint32_t   t_atu;
} C;

/* What the other tasks asked for, carried out by the flex task. */
typedef enum { Q_MODE, Q_FILTER, Q_AGC, Q_GAIN, Q_RIT, Q_ATU_MEM } req_kind_t;
typedef struct { uint8_t kind; int32_t a, b; char s[8]; } req_t;
static QueueHandle_t   s_req;
static RingbufHandle_t s_rxq;        /* [count][Opus bytes], flex -> codec */
static uint8_t s_udp[1600];

static inline uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static void put32be(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

/* Overridden by the application; see tci_client.c. */
__attribute__((weak)) void haptic_hook(uint8_t effect, uint8_t prio)
{
    (void)effect; (void)prio;
}

/* --------------------------------------------------------------- parsing */

/* The value of `key` in a space-separated key=value list, NUL-terminated in
 * `out`. The key must start a word: "tx" is not "rit_tx". */
static bool kv(const char *line, const char *key, char *out, size_t cap)
{
    const size_t kl = strlen(key);
    for (const char *p = line; (p = strstr(p, key)) != NULL; p += kl) {
        if ((p == line || p[-1] == ' ') && p[kl] == '=') {
            const char *v = p + kl + 1;
            size_t n = strcspn(v, " ");
            if (n >= cap) n = cap - 1;
            memcpy(out, v, n);
            out[n] = 0;
            return true;
        }
    }
    return false;
}

static bool kv_long(const char *line, const char *key, long *out)
{
    char v[24];
    if (!kv(line, key, v, sizeof v)) return false;
    *out = strtol(v, NULL, 0);
    return true;
}

static bool kv_hex(const char *line, const char *key, uint32_t *out)
{
    char v[24];
    if (!kv(line, key, v, sizeof v)) return false;
    *out = (uint32_t)strtoul(v, NULL, 16);
    return true;
}

/* "14.100000" MHz to Hz, without float: 24 bits of mantissa would round a
 * 54 MHz frequency to several Hz. */
static bool kv_mhz(const char *line, const char *key, int64_t *hz)
{
    char v[24];
    if (!kv(line, key, v, sizeof v)) return false;
    int64_t mhz = strtoll(v, NULL, 10), frac = 0;
    const char *d = strchr(v, '.');
    int n = 0;
    if (d)
        for (d++; *d >= '0' && *d <= '9' && n < 6; d++, n++) frac = frac * 10 + (*d - '0');
    for (; n < 6; n++) frac *= 10;
    *hz = mhz * 1000000 + frac;
    return true;
}

static void mhz_text(int64_t hz, char *out, size_t cap)
{
    snprintf(out, cap, "%lld.%06lld", (long long)(hz / 1000000), (long long)(hz % 1000000));
}

/* ----------------------------------------------------------------- modes */

/* The editors' names (radio.h) and the API's. */
static const struct { const char *ours, *api; } MODES[] = {
    { "usb", "USB" }, { "lsb", "LSB" }, { "cw", "CW" }, { "am", "AM" },
    { "sam", "SAM" }, { "fm", "FM" }, { "nfm", "NFM" }, { "digu", "DIGU" },
    { "digl", "DIGL" }, { "rtty", "RTTY" }, { "dfm", "DFM" }, { "fdv", "FDV" },
};

static const char *mode_api(const char *ours)
{
    for (size_t i = 0; i < sizeof MODES / sizeof MODES[0]; i++)
        if (strcasecmp(MODES[i].ours, ours) == 0) return MODES[i].api;
    return NULL;
}

/* ---------------------------------------------------------------- the API */

static void set_close_reason(const char *why)
{
    taskENTER_CRITICAL(&S_LOCK);
    strlcpy(S.last_close, why, sizeof S.last_close);
    taskEXIT_CRITICAL(&S_LOCK);
}

static void session_end(const char *why, bool polite);

/* The API's bytes: on the socket, or through TLS by SmartLink. */
static bool api_send(const char *b, int n)
{
    if (!C.tls) return send(C.fd, b, n, 0) == n;
    const uint32_t t0 = now_ms();
    while (n > 0) {
        const ssize_t k = esp_tls_conn_write(C.tls, b, (size_t)n);
        if (k == ESP_TLS_ERR_SSL_WANT_WRITE || k == ESP_TLS_ERR_SSL_WANT_READ) {
            if (now_ms() - t0 > CONNECT_MS) return false;
            vTaskDelay(1);
            continue;
        }
        if (k <= 0) return false;
        b += k;
        n -= (int)k;
    }
    return true;
}

/* Send one command; `what` names the reply's handler. */
static uint32_t cmd(pend_t what, const char *fmt, ...)
{
    if (C.fd < 0) return 0;
    char b[192];
    const uint32_t seq = ++C.seq;
    int n = snprintf(b, sizeof b, "C%lu|", (unsigned long)seq);
    va_list ap;
    va_start(ap, fmt);
    n += vsnprintf(b + n, sizeof b - n - 1, fmt, ap);
    va_end(ap);
    if (n >= (int)sizeof b - 1) n = sizeof b - 2;
    b[n++] = '\n';
    if (!api_send(b, n)) {
        session_end("the radio stopped taking commands", false);
        return 0;
    }
    for (int i = 0; i < N_PENDING; i++)
        if (C.pend[i].what == P_NONE) {
            C.pend[i].seq = seq;
            C.pend[i].what = what;
            break;
        }
    S.sends++;
    return seq;
}

static pend_t pending_take(uint32_t seq)
{
    for (int i = 0; i < N_PENDING; i++)
        if (C.pend[i].what != P_NONE && C.pend[i].seq == seq) {
            const pend_t w = C.pend[i].what;
            C.pend[i].what = P_NONE;
            return w;
        }
    return P_NONE;
}

/* ---------------------------------------------------------------- meters */

static void meters_map(void)
{
    C.m_level = C.m_fwd = C.m_swr = C.m_alc = 0;
    for (int i = 0; i < C.n_meters; i++) {
        const meter_t *m = &C.meters[i];
        const bool tx = strncmp(m->src, "TX-", 3) == 0;
        if (C.slice >= 0 && strcmp(m->src, "SLC") == 0 && m->num == C.slice &&
            strcmp(m->nam, "LEVEL") == 0) C.m_level = m->id;
        else if (tx && strcmp(m->nam, "FWDPWR") == 0) C.m_fwd = m->id;
        else if (tx && strcmp(m->nam, "SWR") == 0)    C.m_swr = m->id;
        else if (tx && strcmp(m->nam, "ALC") == 0)    C.m_alc = m->id;
    }
}

static meter_t *meter_slot(uint16_t id)
{
    for (int i = 0; i < C.n_meters; i++)
        if (C.meters[i].id == id) return &C.meters[i];
    if (C.n_meters == N_METERS) return NULL;
    meter_t *m = &C.meters[C.n_meters++];
    memset(m, 0, sizeof *m);
    m->id = id;
    return m;
}

/* "meter 7.src=SLC#7.num=0#7.nam=LEVEL#7.unit=dBm#..." -- several meters to
 * a line, their fields '#'-separated; or "meter 7 removed". */
static void on_meter_status(char *body)
{
    if (strstr(body, " removed")) {
        const uint16_t id = (uint16_t)atoi(body);
        for (int i = 0; i < C.n_meters; i++)
            if (C.meters[i].id == id) {
                C.meters[i] = C.meters[--C.n_meters];
                break;
            }
        meters_map();
        return;
    }
    char *save = NULL;
    for (char *tok = strtok_r(body, "#", &save); tok; tok = strtok_r(NULL, "#", &save)) {
        char *dot = strchr(tok, '.'), *eq = strchr(tok, '=');
        if (!dot || !eq || eq < dot) continue;
        meter_t *m = meter_slot((uint16_t)atoi(tok));
        if (!m) continue;
        *eq = 0;
        const char *k = dot + 1, *v = eq + 1;
        if (strcmp(k, "src") == 0)      strlcpy(m->src, v, sizeof m->src);
        else if (strcmp(k, "nam") == 0) strlcpy(m->nam, v, sizeof m->nam);
        else if (strcmp(k, "num") == 0) m->num = (int16_t)atoi(v);
    }
    meters_map();
}

/* Readings: pairs of a meter's id and its value, the ones we use all in
 * units the radio scales by 128 (dBm, dBFS, SWR). */
static void on_meters(const uint8_t *p, size_t n, uint32_t t)
{
    for (size_t i = 0; i + 4 <= n; i += 4) {
        const uint16_t id  = (uint16_t)(p[i] << 8 | p[i + 1]);
        const int16_t  raw = (int16_t)(p[i + 2] << 8 | p[i + 3]);
        if (!id) continue;
        const float v = raw / 128.0f;
        if (id == C.m_level) {
            S.smeter_dbm = v;
        } else if (id == C.m_fwd) {
            S.tx_fwd_w = powf(10.0f, v / 10.0f) / 1000.0f;
            if (S.tx_fwd_w > S.tx_peak_w || t - S.t_peak > 400) {
                S.tx_peak_w = S.tx_fwd_w;
                S.t_peak = t;
            }
        } else if (id == C.m_swr) {
            S.tx_swr = v;
        } else if (id == C.m_alc) {
            S.tx_alc = v;
        }
    }
}

/* ------------------------------------------------------------------ UDP */

static void on_udp(const uint8_t *p, int n, uint32_t t)
{
    if (n < 28) return;
    const uint32_t w0 = (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3];
    const unsigned type = w0 >> 28;
    const bool     cls = (w0 >> 27) & 1, trailer = (w0 >> 26) & 1;
    const unsigned tsi = (w0 >> 22) & 3, tsf = (w0 >> 20) & 3;
    size_t hdr = 4;
    if (type == 1 || type == 3) hdr += 4;             /* stream id */
    if (cls) hdr += 8;
    if (tsi) hdr += 4;
    if (tsf) hdr += 8;
    /* The datagram's own length, not the header's size in words: an Opus
     * frame is not padded to one. */
    size_t len = (size_t)n;
    if (trailer && len >= 4) len -= 4;
    if (!cls || len <= hdr) return;
    const uint32_t sid = (uint32_t)p[4] << 24 | p[5] << 16 | p[6] << 8 | p[7];
    const uint16_t pcc = (uint16_t)(p[14] << 8 | p[15]);
    const uint8_t *pay = p + hdr;
    const size_t   pn  = len - hdr;

    if (pcc == PCC_METER) {
        on_meters(pay, pn, t);
    } else if (pcc == PCC_OPUS && sid == C.rx_stream && pn <= OPUS_MAX && s_rxq) {
        uint8_t item[1 + OPUS_MAX];
        item[0] = (w0 >> 16) & 0xF;                   /* the packet count, for losses */
        memcpy(item + 1, pay, pn);
        S.rx_packets++;
        if (xRingbufferSend(s_rxq, item, 1 + pn, 0) != pdTRUE) S.rx_lost++;
    }
    /* Anything else is the hidden panadapter's. */
}

/* ------------------------------------------------------------ the slice */

/* The radio does not echo our own tunes, so a frequency in a status is a
 * change made elsewhere -- another station on our slice, or the radio. It
 * is adopted once the knob is quiet, never mid-turn. */
static void on_freq(int64_t f)
{
    taskENTER_CRITICAL(&S_LOCK);
    S.f_server = f;
    if (!S.have_freq) {
        /* Authoritative on every (re)connect. */
        tune_assign(&S.tune, f);
        S.f_committed = f;
        S.have_freq = true;
    } else if (f != S.f_committed) {
        S.remote_pending = true;                      /* adopted in tune_out */
    } else {
        S.echoes++;
    }
    taskEXIT_CRITICAL(&S_LOCK);
}

/* Every slice the dial may work -- our own station's, or the station's we
 * dial for -- as its statuses have left it. A status carries only what
 * changed, so the rest is kept here: that is how the dial can move to
 * another of the station's slices when its operator clicks one, and show
 * all of it at once. The knob's own settings are written in too, since the
 * radio does not echo them. */
typedef struct {
    bool     known;
    int64_t  hz;
    char     mode[8], agc[6];
    int32_t  lo, hi;
    long     rit_on, rit_freq;
    bool     tx;
    uint32_t pan;
} slc_t;
static slc_t s_slc[N_SLC];

/* Whose slices the dial works: our own station's, or the one we dial for. */
static uint32_t owner_wanted(void) { return C.bound ? C.bound : C.handle; }

static void lose_slice(void)
{
    C.slice = -1;
    taskENTER_CRITICAL(&S_LOCK);
    S.have_freq = false;
    taskEXIT_CRITICAL(&S_LOCK);
}

static void follow_slice(int n)
{
    const slc_t *c = &s_slc[n];
    C.slice = n;
    if (c->pan) C.pan = c->pan;
    taskENTER_CRITICAL(&S_LOCK);
    S.have_freq = false;                              /* its frequency, as the radio has it */
    strlcpy(S.mode, c->mode, sizeof S.mode);
    strlcpy(S.agc, c->agc, sizeof S.agc);
    S.filt_lo  = c->lo;
    S.filt_hi  = c->hi;
    S.rit_hz   = c->rit_on ? (int32_t)c->rit_freq : 0;
    S.tx_slice = c->tx;
    taskEXIT_CRITICAL(&S_LOCK);
    if (c->hz) on_freq(c->hz);
    meters_map();
    ESP_LOGI(TAG, "%s slice %d, on panadapter 0x%08lx", C.bound ? "dialling" : "our",
             n, (unsigned long)C.pan);
}

static void on_slice_status(int n, const char *line)
{
    if (n < 0 || n >= N_SLC) return;
    slc_t *c = &s_slc[n];
    uint32_t owner = 0;
    const bool has_owner = kv_hex(line, "client_handle", &owner);
    long v;
    if (has_owner && owner != owner_wanted()) {
        c->known = false;
        if (n == C.slice) { ESP_LOGW(TAG, "slice %d went to another station", n); lose_slice(); }
        return;
    }
    if (kv_long(line, "in_use", &v) && v == 0) {
        c->known = false;
        if (n == C.slice) { ESP_LOGW(TAG, "slice %d was closed", n); lose_slice(); }
        return;
    }
    if (has_owner) c->known = true;
    if (!c->known) return;

    /* What the status carries, into the slice's record. */
    int64_t hz;
    char s[16];
    const bool f = kv_mhz(line, "RF_frequency", &hz);
    if (f) c->hz = hz;
    const bool m = kv(line, "mode", s, sizeof s);
    if (m) {
        size_t i = 0;
        for (; s[i] && i < sizeof c->mode - 1; i++) c->mode[i] = (char)(s[i] | 0x20);
        c->mode[i] = 0;
    }
    const bool lo = kv_long(line, "filter_lo", &v);
    if (lo) c->lo = (int32_t)v;
    const bool hi = kv_long(line, "filter_hi", &v);
    if (hi) c->hi = (int32_t)v;
    const bool agc = kv(line, "agc_mode", s, sizeof s);
    if (agc) strlcpy(c->agc, s, sizeof c->agc);
    const bool ro = kv_long(line, "rit_on", &v);
    if (ro) c->rit_on = v;
    const bool rf = kv_long(line, "rit_freq", &v);
    if (rf) c->rit_freq = v;
    const bool tx = kv_long(line, "tx", &v);
    if (tx) c->tx = v != 0;
    uint32_t pan;
    if (kv_hex(line, "pan", &pan) && pan) c->pan = pan;

    /* Which slice the dial works: our own station's first; when dialling for
     * a station, the one it activates -- where its operator last clicked. */
    long act;
    if (C.slice < 0 || (C.bound && n != C.slice && kv_long(line, "active", &act) && act)) {
        follow_slice(n);
        return;
    }
    if (n != C.slice) return;
    /* Only what this status says: the rest may be ours, not yet echoed. */
    if (f) on_freq(hz);
    taskENTER_CRITICAL(&S_LOCK);
    if (m)   strlcpy(S.mode, c->mode, sizeof S.mode);
    if (agc) strlcpy(S.agc, c->agc, sizeof S.agc);
    if (lo)  S.filt_lo = c->lo;
    if (hi)  S.filt_hi = c->hi;
    if (ro || rf) S.rit_hz = c->rit_on ? (int32_t)c->rit_freq : 0;
    if (tx)  S.tx_slice = c->tx;
    taskEXIT_CRITICAL(&S_LOCK);
}

static void on_pan_status(uint32_t id, const char *line)
{
    uint32_t owner = 0;
    const bool has_owner = kv_hex(line, "client_handle", &owner);
    if (has_owner && owner != owner_wanted()) return;
    /* The radio sends a new station's panadapter before any slice on it. */
    if (!C.pan && has_owner && C.own) C.pan = id;
    if (id != C.pan) return;
    uint32_t wf;
    if (kv_hex(line, "waterfall", &wf) && wf) C.waterfall = wf;
    long g;
    if (kv_long(line, "rfgain", &g)) {
        S.gain = (int8_t)g;
        S.have_gain = true;
    }
}

/* -------------------------------------------------------------- PTT glue */

static void log_refusal(uint32_t missing)
{
    if (!missing) { ESP_LOGW(TAG, "PTT REFUSED by the radio, or not confirmed in time"); return; }
    if (missing & PERMIT_BAND) ESP_LOGW(TAG, "PTT REFUSED: %s", S.tx_reason);
    else if (missing & PERMIT_TRX)
        ESP_LOGW(TAG, "PTT REFUSED: %s", S.tx && !S.tx_ours ? "another station is transmitting"
                                         : !S.tx_slice ? "our slice is not our transmit slice"
                                         : !C.tx_stream ? "no microphone stream yet"
                                         : S.tx_reason);
    else ESP_LOGW(TAG, "PTT REFUSED, not ready: 0x%03lx", (unsigned long)missing);
}

static void over_log(void)
{
    if (!C.over_frames) return;
    ESP_LOGI(TAG, "over: %u frames, %u of them silence, microphone peak %d dBFS, "
                  "Opus %lu us a frame (worst %lu)",
             C.over_frames, C.over_silent,
             C.over_peak ? (int)(20.0f * log10f((float)C.over_peak / 32767.0f)) : -99,
             (unsigned long)(C.enc_us_sum / C.over_frames), (unsigned long)S.txa_max_us);
}

static void ptt_dispatch(const ptt_out_t *o)
{
    /* The microphone and the key first, the motor last: it sits next to the
     * microphone, and must never be heard on the air. */
    if (o->refused || o->left_tx) audio_in_set_active(false);
    if (o->send_key) {
        switch (S.kind) {
        case KIND_TUNE:
            ESP_LOGW(TAG, "tune carrier");
            cmd(P_XMIT_ON, "transmit tune 1");
            break;
        case KIND_ATU:
            ESP_LOGW(TAG, "tuner: tuning");
            C.atu_waiting = true;
            C.t_atu = now_ms();
            cmd(P_XMIT_ON, "atu start");
            break;
        default:
            C.over_frames = C.over_silent = 0;
            C.over_peak = 0;
            C.enc_us_sum = 0;
            S.txa_max_us = 0;
            /* Dialling for a station, its own microphone is on the air. */
            if (C.own) audio_in_set_active(true);
            cmd(P_XMIT_ON, "xmit 1");
            break;
        }
    }
    if (o->send_unkey) {
        /* A tuner stopped halfway is stopped the way its carrier is. */
        if (S.kind != KIND_VOICE) cmd(P_NONE, "transmit tune 0");
        cmd(P_NONE, "xmit 0");
    }
    /* Rungs 2 and 3: a station that leaves stops transmitting. */
    if (o->close_socket) {
        ESP_LOGW(TAG, "PTT ladder rung 2: leaving the radio");
        session_end("PTT ladder", true);
    }
    if (o->destroy_socket) {
        ESP_LOGE(TAG, "PTT ladder rung 3: dropping the connection");
        session_end("PTT ladder", false);
    }
    if (o->restart) {
        ESP_LOGE(TAG, "PTT ladder rung 4: rebooting to guarantee an unkey");
        esp_restart();
    }
    if (o->entered_tx) {
        S.t_keyed = now_ms();
        ESP_LOGW(TAG, "*** TX ***%s", S.kind == KIND_TUNE ? " (tune)" : S.kind == KIND_ATU ? " (tuner)" : "");
    }
    if (o->left_tx) {
        ESP_LOGI(TAG, "*** RX ***");
        if (S.kind == KIND_VOICE) over_log();
    }
    if (o->refused) log_refusal(o->missing);
    /* The tuner unkeys itself when it is done: that is its end, not the
     * radio taking the key away, and wants no refusal buzz. */
    const bool quiet = S.kind == KIND_ATU && o->left_tx && S.ptt.reason == PTT_AB_REMOTE;
    if (o->haptic && !quiet) haptic_hook(o->haptic, o->haptic_prio);
}

static uint32_t ptt_permit_now(uint32_t t);

/* The interlock says who, if anyone, is on the air. Only TRANSMITTING is
 * RF; the states around it (PTT_REQUESTED, UNKEY_REQUESTED, *_DELAY) are on
 * the way in or out, and say nothing yet. */
static void on_interlock(const char *line, uint32_t t)
{
    char st[24], src[8] = "";
    uint32_t who = 0;
    long allowed;
    if (kv_long(line, "tx_allowed", &allowed)) {
        S.tx_allowed = allowed != 0;
        char r[24] = "";
        kv(line, "reason", r, sizeof r);
        taskENTER_CRITICAL(&S_LOCK);
        strlcpy(S.tx_reason, r, sizeof S.tx_reason);
        taskEXIT_CRITICAL(&S_LOCK);
    }
    if (!kv(line, "state", st, sizeof st)) return;
    if (strstr(st, "REQUESTED") || strstr(st, "DELAY")) return;
    kv_hex(line, "tx_client_handle", &who);
    kv(line, "source", src, sizeof src);
    const bool on_air = strcmp(st, "TRANSMITTING") == 0;
    const bool keying = S.ptt.state != PTT_IDLE;
    /* Ours by our handle -- or, while we are keying, by none (the tune
     * carrier and the tuner report none of their own) or by the station we
     * dial for, whose transmitter it is. The same station keyed from its own
     * PTT, with nothing of ours keyed, is not ours: never unkeyed from here. */
    (void)src;
    const bool ours = on_air && (who == C.handle ||
                                 ((who == 0 || (C.bound && who == C.bound)) && keying));
    if (on_air != S.tx || ours != S.tx_ours)
        ESP_LOGI(TAG, "radio %s%s", on_air ? "transmitting" : "receiving",
                 on_air ? (ours ? " (us)" : " (another station)") : "");
    S.tx = on_air;
    S.tx_ours = ours;

    ptt_out_t o;
    if (ours) {
        ptt_fsm_event(&S.ptt, PTT_EV_CONFIRM_TRUE, t, ptt_permit_now(t), &o);
        ptt_dispatch(&o);
        if (S.ptt.state == PTT_IDLE && who == C.handle) {
            /* Our slice on the air with nothing of ours keyed: a key that
             * outlived what sent it. It is ours, so stop it. */
            ESP_LOGE(TAG, "the radio transmits for us unasked: unkeying");
            cmd(P_NONE, "xmit 0");
        }
    } else if (!on_air && (S.ptt.state == PTT_ON || S.ptt.state == PTT_RELEASING)) {
        /* Back in receive. While the key is only asked for, a READY says
         * nothing: the xmit's reply or the deadline decides that. */
        ptt_fsm_event(&S.ptt, PTT_EV_CONFIRM_FALSE, t, ptt_permit_now(t), &o);
        ptt_dispatch(&o);
    }
}

/* ------------------------------------------------------------- stations */

/* The stations on the radio, from `sub client all`: a GUI client has a
 * client_id. We are not among them -- neither this session, nor an old one
 * of ours the radio has yet to let go. */
static void on_client_status(const char *body)
{
    const uint32_t h = (uint32_t)strtoul(body + 7, NULL, 16);
    if (strstr(body, " disconnected")) {
        if (strstr(body, "duplicate_client_id=1") && h == C.handle)
            ESP_LOGE(TAG, "another client is using our station id");
        for (int i = 0; i < C.n_st; i++)
            if (C.st[i].handle == h) {
                C.st[i] = C.st[--C.n_st];
                break;
            }
        if (C.bound && h == C.bound) {
            ESP_LOGW(TAG, "%s has left the radio", C.pick_name);
            C.station_left = true;
        }
        return;
    }
    if (!strstr(body, " connected")) return;
    char id[40] = "", name[24] = "";
    if (!kv(body, "client_id", id, sizeof id) || !id[0]) return;      /* not a station */
    if (h == C.handle || strcmp(id, C.uuid) == 0) return;            /* ourselves */
    if (!kv(body, "station", name, sizeof name) || !name[0]) kv(body, "program", name, sizeof name);
    int i = 0;
    while (i < C.n_st && C.st[i].handle != h) i++;
    if (i == C.n_st) {
        if (C.n_st == N_STATIONS) return;
        C.n_st++;
    }
    C.st[i].handle = h;
    strlcpy(C.st[i].id, id, sizeof C.st[i].id);
    strlcpy(C.st[i].name, name, sizeof C.st[i].name);
}

static void pick_save(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, "pickid", C.pick_id);
    nvs_set_str(h, "pickname", C.pick_name);
    nvs_commit(h);
    nvs_close(h);
}

static void pick_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t n = sizeof C.pick_id;
    if (nvs_get_str(h, "pickid", C.pick_id, &n) != ESP_OK) C.pick_id[0] = 0;
    n = sizeof C.pick_name;
    if (nvs_get_str(h, "pickname", C.pick_name, &n) != ESP_OK) C.pick_name[0] = 0;
    nvs_close(h);
}

/* Ask on the dial: our own station, or the dial for one already on the
 * radio -- the last choice first, found by its id or else its name. */
static void ask_station(uint32_t t)
{
    uint8_t n = 1, def = 0;
    taskENTER_CRITICAL(&S_LOCK);
    strlcpy(S.ch_title[0], "STATION", sizeof S.ch_title[0]);
    strlcpy(S.ch_name[0], "OWN", sizeof S.ch_name[0]);
    for (int i = 0; i < C.n_st && n < RADIO_CHOICES; i++, n++) {
        C.q[n - 1] = C.st[i];
        strlcpy(S.ch_title[n], "DIAL FOR", sizeof S.ch_title[n]);
        strlcpy(S.ch_name[n], C.st[i].name, sizeof S.ch_name[n]);
        if (C.pick_id[0] && strcmp(C.st[i].id, C.pick_id) == 0) def = n;
    }
    if (!def && C.pick_name[0])
        for (uint8_t k = 1; k < n; k++)
            if (strcmp(S.ch_name[k], C.pick_name) == 0) { def = k; break; }
    S.n_choices = n;
    S.choice_default = def;
    S.choices_seq++;
    S.pending_choice = -1;
    taskEXIT_CRITICAL(&S_LOCK);
    C.n_q = n - 1;
    C.asking = true;
    C.t_asked = t;
    ESP_LOGI(TAG, "asking: our own station, or the dial for one of %u", C.n_q);
}

static void decide(int a)
{
    taskENTER_CRITICAL(&S_LOCK);
    S.n_choices = 0;
    S.pending_choice = -1;
    taskEXIT_CRITICAL(&S_LOCK);
    C.asking = false;
    C.decided = true;
    C.own = a <= 0 || a > C.n_q;
    if (C.own) {
        C.pick_id[0] = C.pick_name[0] = 0;
        ESP_LOGI(TAG, "our own station");
    } else {
        strlcpy(C.pick_id, C.q[a - 1].id, sizeof C.pick_id);
        strlcpy(C.pick_name, C.q[a - 1].name, sizeof C.pick_name);
        ESP_LOGI(TAG, "the dial for %s", C.pick_name);
    }
    pick_save();
}

static bool station_here(const char *id)
{
    for (int i = 0; i < C.n_st; i++)
        if (strcmp(C.st[i].id, id) == 0) return true;
    return false;
}

/* The stations have been listed: become our own station, or bind to the
 * one chosen -- asking first, at boot, when there is anyone to choose. A
 * reconnect keeps the choice without asking. */
static void choose_step(uint32_t t)
{
    if (!C.greeted || C.registering) return;
    if (C.wan) {
        /* By SmartLink: registered first -- after `client ip`, which gives
         * the radio the moment it needs -- and as our own station. The
         * others on the radio are not listed before registering there, so
         * the knob does not offer to be the dial for one of them. */
        if (!C.ip_answered && t - C.t_hello < 3000) return;
        C.decided = true;
        C.own = true;
    } else if (t - C.t_hello < LIST_MS) {
        return;
    } else if (C.asking) {
        int8_t a = S.pending_choice;
        if (a < 0 && t - C.t_asked < ASK_MS) return;
        if (a < 0) {
            a = (int8_t)S.choice_default;
            ESP_LOGI(TAG, "no answer: the one it offered first");
        }
        decide(a);
    } else if (!C.decided || (!C.own && !station_here(C.pick_id))) {
        if (C.n_st) { ask_station(t); return; }
        decide(0);                        /* nobody else on the radio */
    }
    C.registering = true;
    C.t_session = t;                      /* the setup's deadline runs from here */
    cmd(P_NONE, "client program " OUR_PROGRAM);
    if (C.own) cmd(P_GUI, "client gui %s", C.uuid);
    else       cmd(P_BIND, "client bind client_id=%s", C.pick_id);
}

/* Registered, or bound: what we follow, and the port our meters come to. */
static void subscribe(void)
{
    if (C.own) cmd(P_NONE, "client station " OUR_STATION);
    cmd(P_NONE, "keepalive enable");
    cmd(P_NONE, "sub slice all");
    cmd(P_NONE, "sub pan all");
    cmd(P_NONE, "sub tx all");
    cmd(P_NONE, "sub meter all");
    cmd(P_NONE, "sub atu all");
    /* By SmartLink the radio learns our UDP address from the packets
     * themselves (wan_udp): `client udpport` is for the LAN. */
    if (C.wan) return;
    /* A byte first, as AetherSDR does, so a stateful firewall on the way
     * lets the radio's packets back in; then the port itself. */
    for (int i = 0; i < 3; i++) {
        struct sockaddr_in to = C.radio;
        to.sin_port = htons(API_PORT);
        sendto(C.ufd, "", 1, 0, (struct sockaddr *)&to, sizeof to);
    }
    cmd(P_UDPPORT, "client udpport %u", (unsigned)C.uport);
}

static void note(const char *text)
{
    taskENTER_CRITICAL(&S_LOCK);
    strlcpy(S.note, text, sizeof S.note);
    S.note_seq++;
    taskEXIT_CRITICAL(&S_LOCK);
}

/* "atu status=TUNE_SUCCESSFUL atu_enabled=1 memories_enabled=0 using_mem=0" */
static void on_atu_status(const char *line)
{
    long en;
    if (kv_long(line, "atu_enabled", &en)) S.has_atu = en != 0;
    if (kv_long(line, "memories_enabled", &en)) S.atu_mem = en != 0;
    char st[24];
    if (!kv(line, "status", st, sizeof st)) return;
    if (!C.atu_waiting || strcmp(st, "TUNE_IN_PROGRESS") == 0 ||
        strcmp(st, "TUNE_NOT_STARTED") == 0) return;
    C.atu_waiting = false;
    if (strcmp(st, "TUNE_SUCCESSFUL") == 0 || strcmp(st, "TUNE_OK") == 0) {
        ESP_LOGI(TAG, "tuner: tuned (%s)", st);
    } else {
        ESP_LOGW(TAG, "tuner: %s", st);
        note(strcmp(st, "TUNE_ABORTED") == 0 ? "ATU ABORTED" : "ATU FAILED");
    }
}

static void on_status(char *body, uint32_t t)
{
    if (strncmp(body, "slice ", 6) == 0) {
        on_slice_status(atoi(body + 6), body);
    } else if (strncmp(body, "display pan ", 12) == 0) {
        on_pan_status((uint32_t)strtoul(body + 12, NULL, 16), body);
    } else if (strncmp(body, "interlock ", 10) == 0) {
        on_interlock(body, t);
    } else if (strncmp(body, "meter ", 6) == 0) {
        on_meter_status(body + 6);
    } else if (strncmp(body, "atu ", 4) == 0) {
        on_atu_status(body);
    } else if (strncmp(body, "client ", 7) == 0) {
        on_client_status(body);
    }
}

/* ------------------------------------------------------------- session */

static void uuid_save(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, "uuid", C.uuid);
    nvs_commit(h);
    nvs_close(h);
}

static void uuid_load(void)
{
    nvs_handle_t h;
    size_t n = sizeof C.uuid;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_str(h, "uuid", C.uuid, &n) != ESP_OK) C.uuid[0] = 0;
        nvs_close(h);
    }
    if (C.uuid[0]) return;
    uint8_t r[16];
    esp_fill_random(r, sizeof r);
    r[6] = (r[6] & 0x0F) | 0x40;                      /* version 4 */
    r[8] = (r[8] & 0x3F) | 0x80;                      /* RFC 4122 variant */
    snprintf(C.uuid, sizeof C.uuid,
             "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X",
             r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7],
             r[8], r[9], r[10], r[11], r[12], r[13], r[14], r[15]);
    uuid_save();
    ESP_LOGI(TAG, "a new station id: %s", C.uuid);
}

/* Leave. Politely -- our streams given back, FlexLib's goodbye byte -- when
 * the radio can still hear us; the radio frees the rest of a station itself
 * when its connection closes, and keeps its slice for when it comes back. */
static void session_end(const char *why, bool polite)
{
    if (C.fd >= 0) {
        if (polite) {
            char b[64];
            const uint32_t ids[2] = { C.rx_stream, C.tx_stream };
            for (int i = 0; i < 2; i++)
                if (ids[i]) {
                    int n = snprintf(b, sizeof b, "C%lu|stream remove 0x%08lX\n",
                                     (unsigned long)++C.seq, (unsigned long)ids[i]);
                    api_send(b, n);
                }
            api_send("\x04", 1);
        }
        if (C.tls) esp_tls_conn_destroy(C.tls);     /* its socket with it */
        else       close(C.fd);
        C.tls = NULL;
    }
    C.tx_stream = 0;                                  /* the codec reads this first */
    if (C.ufd >= 0) close(C.ufd);
    C.fd = C.ufd = -1;
    C.udp_ok = false;
    C.greeted = C.registered = C.listed = C.creating = C.streams_asked = C.tx_claimed = false;
    C.pan_tamed = C.wf_tamed = false;
    /* Who is on the radio is listed afresh; what we chose to be is kept. */
    C.registering = C.asking = false;
    C.bound = 0;
    C.n_st = 0;
    memset(s_slc, 0, sizeof s_slc);
    taskENTER_CRITICAL(&S_LOCK);
    S.n_choices = 0;
    S.pending_choice = -1;
    taskEXIT_CRITICAL(&S_LOCK);
    C.slice = -1;
    C.pan = C.waterfall = C.rx_stream = 0;
    C.n_meters = 0;
    C.m_level = C.m_fwd = C.m_swr = C.m_alc = 0;
    memset(C.pend, 0, sizeof C.pend);
    const uint32_t t = now_ms();
    if (S.ptt.state != PTT_IDLE) {
        ptt_out_t o;
        ptt_fsm_abort(&S.ptt, PTT_AB_LINK_DOWN, t, &o);
        audio_in_set_active(false);
        if (o.restart) esp_restart();
    }
    taskENTER_CRITICAL(&S_LOCK);
    if (S.link != RADIO_LINK_DOWN) S.closes++;
    S.link = RADIO_LINK_DOWN;
    S.have_freq = false;
    S.have_gain = false;
    S.tx = S.tx_ours = false;
    taskEXIT_CRITICAL(&S_LOCK);
    set_close_reason(why);
    C.t_retry = t + C.backoff_ms;
    C.backoff_ms = MIN(C.backoff_ms * 2, 16000);
    ESP_LOGW(TAG, "session ended: %s (retry in %lu ms)", why, (unsigned long)C.backoff_ms);
}

/* By SmartLink: a UDP port first, for the server to tell the radio; then the
 * brokering and the radio's TLS (sl_open, which blocks for up to 40 s). Not
 * retried sooner than 5 s, and backing off to a minute: FlexRadio's service
 * is not ours to hammer. */
static bool wan_begin(uint32_t t)
{
    int u = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in me = { .sin_family = AF_INET };
    socklen_t ml = sizeof me;
    if (u < 0 || bind(u, (struct sockaddr *)&me, sizeof me) != 0) {
        if (u >= 0) close(u);
        set_close_reason("no UDP socket");
        C.t_retry = t + 2000;
        return false;
    }
    getsockname(u, (struct sockaddr *)&me, &ml);
    C.uport = ntohs(me.sin_port);
    taskENTER_CRITICAL(&S_LOCK);
    S.link = RADIO_LINK_CONNECTING;
    taskEXIT_CRITICAL(&S_LOCK);
    ESP_LOGI(TAG, "connecting through SmartLink (our UDP port %u)", (unsigned)C.uport);
    sl_link_t l;
    char why[96];
    if (sl_open(C.uport, &l, why, sizeof why) != ESP_OK) {
        close(u);
        taskENTER_CRITICAL(&S_LOCK);
        S.link = RADIO_LINK_DOWN;
        taskEXIT_CRITICAL(&S_LOCK);
        set_close_reason(why);
        C.backoff_ms = MAX(C.backoff_ms, 5000);
        C.t_retry = now_ms() + C.backoff_ms;
        ESP_LOGW(TAG, "SmartLink: %s (again in %lu s)", why, (unsigned long)(C.backoff_ms / 1000));
        C.backoff_ms = MIN(C.backoff_ms * 2, 60000);
        return false;
    }
    int fd = -1;
    esp_tls_get_conn_sockfd(l.tls, &fd);
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    C.tls = l.tls;
    C.fd = fd;
    C.ufd = u;
    C.udp_to = l.udp;
    C.radio = l.udp;
    C.udp_ok = false;
    C.t_udp = C.t_udp_first = 0;
    C.line_n = 0;
    C.line_skip = false;
    C.seq = 1;                                   /* 1 was `wan validate` */
    C.handle = 0;
    C.pings_out = 0;
    C.t_session = C.t_ping = C.t_pong = now_ms();
    taskENTER_CRITICAL(&S_LOCK);
    S.link = RADIO_LINK_GREETING;
    S.connects++;
    taskEXIT_CRITICAL(&S_LOCK);
    return true;
}

/* The radio's UDP by SmartLink: `udp_register` every 50 ms until its first
 * packet -- a second between them after five -- then `ping` every 5 s, so
 * the NATs on the way keep the path open. */
static void wan_udp(uint32_t t)
{
    if (!C.wan || !C.handle || C.ufd < 0) return;
    if (!C.t_udp_first) C.t_udp_first = t;
    const uint32_t every = C.udp_ok ? 5000 : t - C.t_udp_first < 5000 ? 50 : 1000;
    if (C.t_udp && t - C.t_udp < every) return;
    C.t_udp = t;
    char b[64];
    const int n = snprintf(b, sizeof b, "client %s handle=0x%lX",
                           C.udp_ok ? "ping" : "udp_register", (unsigned long)C.handle);
    sendto(C.ufd, b, n, 0, (struct sockaddr *)&C.udp_to, sizeof C.udp_to);
}

static bool session_begin(uint32_t t)
{
    if (C.wan) return wan_begin(t);
    C.radio.sin_family = AF_INET;
    C.radio.sin_port = htons(C.port);
    if (!inet_aton(C.host, &C.radio.sin_addr)) {
        set_close_reason("bad radio address");
        C.t_retry = t + 5000;
        return false;
    }
    C.fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (C.fd < 0) {
        set_close_reason("no socket");
        C.t_retry = t + 2000;
        return false;
    }
    int one = 1;
    setsockopt(C.fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    struct timeval tv = { .tv_sec = CONNECT_MS / 1000, .tv_usec = 0 };
    setsockopt(C.fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    ESP_LOGI(TAG, "connecting to %s:%u", C.host, (unsigned)C.port);
    taskENTER_CRITICAL(&S_LOCK);
    S.link = RADIO_LINK_CONNECTING;
    taskEXIT_CRITICAL(&S_LOCK);
    if (connect(C.fd, (struct sockaddr *)&C.radio, sizeof C.radio) != 0) {
        close(C.fd);
        C.fd = -1;
        taskENTER_CRITICAL(&S_LOCK);
        S.link = RADIO_LINK_DOWN;
        taskEXIT_CRITICAL(&S_LOCK);
        set_close_reason("radio not answering");
        C.t_retry = now_ms() + C.backoff_ms;
        C.backoff_ms = MIN(C.backoff_ms * 2, 16000);
        return false;
    }
    /* The UDP side: on the address the radio sees us at, a port of our own. */
    struct sockaddr_in me = { 0 };
    socklen_t ml = sizeof me;
    getsockname(C.fd, (struct sockaddr *)&me, &ml);
    int u = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    me.sin_port = 0;
    if (u < 0 || bind(u, (struct sockaddr *)&me, sizeof me) != 0) {
        if (u >= 0) close(u);
        session_end("no UDP socket", false);
        return false;
    }
    getsockname(u, (struct sockaddr *)&me, &ml);
    C.uport = ntohs(me.sin_port);
    C.ufd = u;
    C.line_n = 0;
    C.line_skip = false;
    C.seq = 0;
    C.handle = 0;
    C.pings_out = 0;
    C.t_session = C.t_ping = C.t_pong = now_ms();
    taskENTER_CRITICAL(&S_LOCK);
    S.link = RADIO_LINK_GREETING;
    S.connects++;
    taskEXIT_CRITICAL(&S_LOCK);
    return true;
}

/* Registered, subscribed, UDP named: now a slice of our own. The radio has
 * one for a known station id -- or for a new one, the default it gives every
 * new station -- and says so in the statuses that follow `sub slice all`.
 * Only if none came is one made: a panadapter first, then the slice on it. */
static void setup_step(uint32_t t)
{
    if (!C.own || !C.listed || C.slice >= 0 || C.creating) return;
    if (t - C.t_listed < ADOPT_MS) return;
    if (C.pan) {
        ESP_LOGI(TAG, "no slice of ours: making one on our panadapter");
        cmd(P_SLICE_CREATE, "slice create pan=0x%08lX freq=14.100000 antenna=ANT1 mode=USB",
            (unsigned long)C.pan);
    } else {
        ESP_LOGI(TAG, "no slice of ours: making a panadapter and one on it");
        cmd(P_PAN_CREATE, "display panafall create x=50 y=20");
    }
    C.creating = true;
}

/* Our panadapter is only there because a slice needs one: nobody sees it,
 * so it may send as little as the radio allows. */
static void tame_pan(void)
{
    if (!C.pan_tamed && C.pan) {
        C.pan_tamed = true;
        cmd(P_RFGAIN_INFO, "display pan rfgain_info 0x%08lX", (unsigned long)C.pan);
        /* A station we dial for keeps its panadapter as its operator set it. */
        if (!C.own) return;
        cmd(P_NONE, "display pan set 0x%08lX xpixels=50 ypixels=20", (unsigned long)C.pan);
        cmd(P_NONE, "display pan set 0x%08lX fps=1", (unsigned long)C.pan);
    }
    if (!C.wf_tamed && C.waterfall && C.own) {
        C.wf_tamed = true;
        cmd(P_NONE, "display panafall set 0x%08lX line_duration=1", (unsigned long)C.waterfall);
    }
}

static void on_reply(uint32_t seq, uint32_t code, const char *body, uint32_t t)
{
    const pend_t what = pending_take(seq);
    if (what == P_PING) {
        C.pings_out = 0;
        C.t_pong = t;
        return;
    }
    /* By SmartLink, command 1 was `wan validate`: the radio answers it with
     * 500000B1 and carries on (AetherSDR never reads that answer). */
    if (C.wan && seq == 1) {
        ESP_LOGI(TAG, "SmartLink: wan validate answered %08lX", (unsigned long)code);
        return;
    }
    if (what == P_CLIENT_IP) {
        C.ip_answered = true;
        ESP_LOGI(TAG, "SmartLink: the radio sees us at %s", code ? "?" : body);
        return;
    }
    /* A 1xxxxxxx code is a warning (the radio does not know our program's
     * name), not a failure. */
    if (code && (code >> 28) != 1) {
        ESP_LOGW(TAG, "command %lu refused: %08lX %s", (unsigned long)seq,
                 (unsigned long)code, body);
        S.rejects++;
        switch (what) {
        case P_GUI:
            C.backoff_ms = 30000;
            session_end("the radio refused us as a station", true);
            break;
        case P_BIND:
            C.decided = false;                /* ask again, with who is here then */
            session_end("the station could not be joined", true);
            break;
        case P_PAN_CREATE:
        case P_SLICE_CREATE:
            C.backoff_ms = 30000;
            session_end("the radio has no slice free for us", true);
            break;
        case P_UDPPORT:
            session_end("the radio refused our UDP port", true);
            break;
        case P_XMIT_ON: {
            ptt_out_t o;
            ptt_fsm_event(&S.ptt, PTT_EV_CONFIRM_FALSE, t, ptt_permit_now(t), &o);
            ptt_dispatch(&o);
            break;
        }
        default:
            break;
        }
        return;
    }
    switch (what) {
    case P_GUI:
        if (strlen(body) == 36 && strcmp(body, C.uuid) != 0) {
            strlcpy(C.uuid, body, sizeof C.uuid);    /* the radio chose our id */
            uuid_save();
        }
        C.registered = true;
        ESP_LOGI(TAG, "a station on the radio, handle 0x%08lX", (unsigned long)C.handle);
        subscribe();
        break;
    case P_BIND: {
        /* "0x38BB362E": the station's handle, whose slices we now work. */
        const uint32_t h = (uint32_t)strtoul(body, NULL, 16);
        for (int i = 0; !h && i < C.n_st; i++)
            if (strcmp(C.st[i].id, C.pick_id) == 0) C.bound = C.st[i].handle;
        if (h) C.bound = h;
        C.registered = true;
        ESP_LOGI(TAG, "the dial for %s, handle 0x%08lX", C.pick_name, (unsigned long)C.bound);
        subscribe();
        break;
    }
    case P_UDPPORT:
        cmd(P_SLICE_LIST, "slice list");
        break;
    case P_SLICE_LIST:
        C.listed = true;
        C.t_listed = t;
        break;
    case P_PAN_CREATE: {
        /* "0x40000000,0x42000000": the panadapter, then its waterfall. */
        const char *p = strncmp(body, "pan=", 4) == 0 ? body + 4 : body;
        C.pan = (uint32_t)strtoul(p, NULL, 16);
        const char *c = strchr(body, ',');
        if (c) C.waterfall = (uint32_t)strtoul(c + 1, NULL, 16);
        ESP_LOGI(TAG, "our panadapter: 0x%08lX", (unsigned long)C.pan);
        cmd(P_SLICE_CREATE, "slice create pan=0x%08lX freq=14.100000 antenna=ANT1 mode=USB",
            (unsigned long)C.pan);
        break;
    }
    case P_SLICE_CREATE:
        C.creating = false;
        if (C.slice < 0) {
            C.slice = atoi(body);
            meters_map();
            ESP_LOGI(TAG, "our slice: %d (new)", C.slice);
        }
        break;
    case P_RFGAIN_INFO: {
        /* "low,high,step" in dB: -8,32,8 on a FLEX-6600. */
        long lo, hi, st;
        if (sscanf(body, "%ld,%ld,%ld", &lo, &hi, &st) == 3 && hi > lo && st > 0) {
            S.gain_min = (int8_t)lo;
            S.gain_max = (int8_t)hi;
            S.gain_step = (int8_t)st;
        }
        break;
    }
    case P_RX_STREAM:
    case P_TX_STREAM: {
        const char *p = strncmp(body, "stream=", 7) == 0 ? body + 7 : body;
        const uint32_t id = (uint32_t)strtoul(p, NULL, 16);
        if (what == P_RX_STREAM) C.rx_stream = id;
        else                     C.tx_stream = id;
        ESP_LOGI(TAG, "%s audio: stream 0x%08lX", what == P_RX_STREAM ? "receive" : "transmit",
                 (unsigned long)id);
        break;
    }
    default:
        break;
    }
}

static void on_line(char *l, uint32_t t)
{
    switch (l[0]) {
    case 'V':
        ESP_LOGI(TAG, "radio API %s", l + 1);
        break;
    case 'H':
        C.handle = (uint32_t)strtoul(l + 1, NULL, 16);
        C.greeted = true;
        C.t_hello = t;
        /* By SmartLink the radio takes nothing before `client gui` but
         * `client ip` (and hangs up on the rest), as AetherSDR found; on
         * the LAN, who is here, before we are anyone. */
        C.ip_answered = false;
        if (C.wan) cmd(P_CLIENT_IP, "client ip");
        else       cmd(P_NONE, "sub client all");
        break;
    case 'R': {
        char *bar1 = strchr(l, '|');
        if (!bar1) break;
        char *bar2 = strchr(bar1 + 1, '|');
        const uint32_t seq = (uint32_t)strtoul(l + 1, NULL, 10);
        const uint32_t code = (uint32_t)strtoul(bar1 + 1, NULL, 16);
        on_reply(seq, code, bar2 ? bar2 + 1 : "", t);
        break;
    }
    case 'S': {
        char *bar = strchr(l, '|');
        if (bar) on_status(bar + 1, t);
        break;
    }
    case 'M':
        ESP_LOGI(TAG, "radio says: %s", l + 1);
        break;
    default:
        S.unknown_cmds++;
        break;
    }
}

static void on_tcp(const char *d, int n, uint32_t t)
{
    for (int i = 0; i < n; i++) {
        const char ch = d[i];
        if (ch == '\n' || ch == '\r') {
            if (!C.line_skip && C.line_n) {
                C.line[C.line_n] = 0;
                on_line(C.line, t);
                if (C.fd < 0) return;
            }
            C.line_n = 0;
            C.line_skip = false;
        } else if (!C.line_skip) {
            if (C.line_n < LINE_CAP - 1) {
                C.line[C.line_n++] = ch;
            } else {
                ESP_LOGW(TAG, "a status line longer than %d bytes: skipped", LINE_CAP);
                C.line_skip = true;
                C.line_n = 0;
            }
        }
    }
}

/* ---------------------------------------------------------- the codec */

/* 10 ms of the microphone, as Opus, to the radio. Silence while the mic has
 * nothing yet: the radio plays the stream as it comes, and a gap is a click
 * on the air. */
static void tx_frame(OpusEncoder *enc)
{
    static int16_t mic[FRAME];
    static uint8_t pkt[28 + OPUS_MAX];
    C.over_frames++;
    if (!audio_in_take(mic, FRAME)) {
        memset(mic, 0, sizeof mic);
        S.txa_skipped++;
        C.over_silent++;
    } else {
        int peak = 0;
        for (int i = 0; i < FRAME; i++) {
            const int v = mic[i] < 0 ? -mic[i] : mic[i];
            if (v > peak) peak = v;
        }
        if (peak > C.over_peak) C.over_peak = peak;
        /* The same scale the other firmwares' mic meters use: dB below full. */
        S.tx_mic_dbm = peak ? 20.0f * log10f((float)peak / 32767.0f) : -60.0f;
    }
    const int64_t t0 = esp_timer_get_time();
    const int n = opus_encode(enc, mic, FRAME, pkt + 28, OPUS_MAX);
    const uint32_t us = (uint32_t)(esp_timer_get_time() - t0);
    C.enc_us_sum += us;
    if (us > S.txa_max_us) S.txa_max_us = us;
    const uint32_t sid = C.tx_stream;
    const int fd = C.ufd;
    if (n <= 0 || !sid || fd < 0) { S.txa_failed++; return; }

    /* VITA-49 as SmartSDR sends it: IF data with a stream id, class id and
     * timestamps (all zero), the packet count, and the size in words -- the
     * datagram itself exactly as long as the Opus frame, unpadded. */
    static uint8_t count;
    const size_t len = 28 + (size_t)n;
    put32be(pkt, 0x38D00000u | (uint32_t)(count++ & 0xF) << 16 | (uint32_t)((len + 3) / 4));
    put32be(pkt + 4, sid);
    put32be(pkt + 8, 0x00001C2D);                    /* FlexRadio's OUI */
    put32be(pkt + 12, 0x534C0000u | PCC_OPUS);
    memset(pkt + 16, 0, 12);
    struct sockaddr_in to = C.radio;
    if (C.wan) to = C.udp_to;                    /* its public port, by SmartLink */
    else       to.sin_port = htons(TX_PORT);
    if (sendto(fd, pkt, len, 0, (struct sockaddr *)&to, sizeof to) == (int)len) S.txa_sent++;
    else S.txa_failed++;
}

/* The radio's audio in, the microphone out, both as 10 ms Opus frames. On a
 * task of its own, below the API's, so a slow frame never holds up PTT. */
static void codec_task(void *arg)
{
    (void)arg;
    int err;
    OpusDecoder *dec = opus_decoder_create(AUDIO_RATE_HZ, 2, &err);
    OpusEncoder *enc = opus_encoder_create(TX_AUDIO_RATE_HZ, 1,
                                           OPUS_APPLICATION_RESTRICTED_LOWDELAY, &err);
    if (!dec || !enc) {
        ESP_LOGE(TAG, "no Opus codec (%d): no audio either way", err);
        vTaskDelete(NULL);
    }
    /* CELT only (the low-delay application), as the radio's own stream is,
     * in 10 ms frames; mono, which the radio's stereo decoder takes as is. */
    opus_encoder_ctl(enc, OPUS_SET_BITRATE(TX_BITRATE));
    opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(TX_COMPLEXITY));
    opus_encoder_ctl(enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));

    static int16_t pcm[2 * 2 * FRAME];
    uint8_t last = 0;
    bool    have_last = false;
    int64_t next_tx = 0;
    for (;;) {
        const bool keyed = S.ptt.state == PTT_REQ_ON || S.ptt.state == PTT_ON;
        const bool voice = keyed && S.kind == KIND_VOICE && C.tx_stream;
        size_t n = 0;
        uint8_t *it = xRingbufferReceive(s_rxq, &n, pdMS_TO_TICKS(keyed ? 1 : 20));
        if (it) {
            const uint8_t count = it[0];
            /* In transmit the radio's stream is our own monitor at most:
             * not played, and the frames not decoded -- the encoder wants
             * the time. */
            if (!keyed && !S.audio_suspend && n > 1) {
                if (have_last) {
                    /* Up to three lost frames concealed; more is a new start. */
                    const uint8_t missed = (uint8_t)((count - last - 1) & 0xF);
                    for (uint8_t i = 0; missed <= 3 && i < missed; i++) {
                        const int m = opus_decode(dec, NULL, 0, pcm, 2 * FRAME, 0);
                        if (m > 0) audio_out_feed_pcm16(pcm, (size_t)m, 2);
                        S.rx_concealed++;
                    }
                }
                const int64_t t0 = esp_timer_get_time();
                const int m = opus_decode(dec, it + 1, (opus_int32)(n - 1), pcm, 2 * FRAME, 0);
                const uint32_t us = (uint32_t)(esp_timer_get_time() - t0);
                if (us > S.dec_max_us) S.dec_max_us = us;
                if (m > 0) audio_out_feed_pcm16(pcm, (size_t)m, 2);
            }
            last = count;
            have_last = true;
            vRingbufferReturnItem(s_rxq, it);
        }
        if (voice) {
            const int64_t now = esp_timer_get_time();
            if (!next_tx || now - next_tx > 60000) next_tx = now;   /* (re)start */
            if (now >= next_tx) {
                next_tx += 10000;
                S.chronos++;
                tx_frame(enc);
            }
        } else {
            next_tx = 0;
        }
    }
}

/* ---------------------------------------------------------- the task */

/* Why the radio will not transmit here, in the words a refusal shows. */
static const char *tx_why(void)
{
    if (S.tx && !S.tx_ours) return "TX IN USE";
    if (!S.tx_allowed) {
        if (strstr(S.tx_reason, "BAND") || strstr(S.tx_reason, "PA_RANGE")) return "OUT OF BAND";
        if (strstr(S.tx_reason, "INHIBIT")) return "TX INHIBITED";
        if (strstr(S.tx_reason, "NO_TX")) return "NO TX SLICE";
    }
    if (!S.tx_slice) return "NO TX SLICE";
    return "";
}

static uint32_t ptt_permit_now(uint32_t t)
{
    /* TX_ENABLE is kept for the radio's own "transmitter disabled", which
     * this API does not report: out of band is only PTT not offered (BAND),
     * not a warning over the dial. */
    uint32_t p = PERMIT_MODE | PERMIT_NO_OVERLAY | PERMIT_NO_FAULT | PERMIT_TX_ENABLE;
    if (S.link == RADIO_LINK_READY && t - S.t_ready_ms >= 500) p |= PERMIT_LINK;
    if (C.fd >= 0 && t - C.t_pong < PONG_FRESH_MS) p |= PERMIT_PONG_FRESH;
    if (t - S.t_last_input_ms >= QUIET_MS) p |= PERMIT_NO_RECONCILE;
    const bool band = strstr(S.tx_reason, "BAND") || strstr(S.tx_reason, "PA_RANGE");
    if (S.tx_allowed || !band) p |= PERMIT_BAND;
    /* Our slice transmits, nobody else is on the air, the microphone has a
     * stream to go to, and the interlock has nothing else against it. */
    if (S.tx_slice && (C.tx_stream || C.bound) && !(S.tx && !S.tx_ours) && (S.tx_allowed || band))
        p |= PERMIT_TRX;
    return p;
}

static void ptt_step(uint32_t t)
{
    ptt_out_t o;
    const uint32_t permit = ptt_permit_now(t);
    if (S.pending_toggle) {
        S.pending_toggle = 0;
        S.pending_key = (S.ptt.state == PTT_IDLE);
        S.pending_unkey = !S.pending_key;
    }
    if (S.pending_tune || S.pending_atu) {
        const uint8_t k = S.pending_tune ? KIND_TUNE : KIND_ATU;
        S.pending_tune = S.pending_atu = 0;
        if (S.ptt.state == PTT_IDLE) {
            S.kind = k;
            ptt_fsm_event(&S.ptt, PTT_EV_TAP_KEY, t, permit, &o);
            ptt_dispatch(&o);
        }
    }
    if (S.pending_key) {
        S.pending_key = 0;
        if (S.ptt.state == PTT_IDLE) S.kind = KIND_VOICE;
        ptt_fsm_event(&S.ptt, PTT_EV_TAP_KEY, t, permit, &o);
        ptt_dispatch(&o);
    }
    /* A tune carrier or a tuner that runs on is stopped, as PTT would. */
    if (S.ptt.state == PTT_ON && S.kind != KIND_VOICE &&
        t - S.t_keyed > (S.kind == KIND_TUNE ? TUNE_MAX_MS : ATU_MAX_MS)) {
        ESP_LOGW(TAG, "%s: time is up", S.kind == KIND_TUNE ? "tune carrier" : "tuner");
        S.pending_unkey = 1;
    }
    if (S.pending_unkey) {
        S.pending_unkey = 0;
        ptt_fsm_event(&S.ptt, PTT_EV_TAP_UNKEY, t, permit, &o);
        ptt_dispatch(&o);
    }
    if (S.pending_abort) {
        const uint8_t r = S.pending_abort;
        S.pending_abort = 0;
        ptt_fsm_abort(&S.ptt, (ptt_abort_t)r, t, &o);
        ptt_dispatch(&o);
    }
    if (S.ptt.state == PTT_ON && C.fd >= 0 && t - C.t_pong > 6000) {
        ESP_LOGE(TAG, "radio silent for %lu ms while keyed", (unsigned long)(t - C.t_pong));
        ptt_fsm_abort(&S.ptt, PTT_AB_PONG_STALE, t, &o);
        ptt_dispatch(&o);
    }
    ptt_fsm_event(&S.ptt, PTT_EV_TICK, t, permit, &o);
    ptt_dispatch(&o);
}

/* Settings the radio does not echo back to their sender: the dial shows
 * them as asked. A mode change does come back, with its filter and AGC. */
static void take_requests(void)
{
    req_t r;
    while (xQueueReceive(s_req, &r, 0) == pdTRUE) {
        if (C.slice < 0 && r.kind != Q_ATU_MEM) continue;
        switch (r.kind) {
        case Q_MODE:
            cmd(P_NONE, "slice set %d mode=%s", C.slice, r.s);
            break;
        case Q_FILTER:
            cmd(P_NONE, "filt %d %ld %ld", C.slice, (long)r.a, (long)r.b);
            S.filt_lo = s_slc[C.slice].lo = r.a;
            S.filt_hi = s_slc[C.slice].hi = r.b;
            break;
        case Q_AGC:
            cmd(P_NONE, "slice set %d agc_mode=%s", C.slice, r.s);
            strlcpy(s_slc[C.slice].agc, r.s, sizeof s_slc[C.slice].agc);
            taskENTER_CRITICAL(&S_LOCK);
            strlcpy(S.agc, r.s, sizeof S.agc);
            taskEXIT_CRITICAL(&S_LOCK);
            break;
        case Q_RIT:
            cmd(P_NONE, "slice set %d rit_on=%d rit_freq=%ld", C.slice, r.a ? 1 : 0, (long)r.a);
            s_slc[C.slice].rit_on = r.a != 0;
            s_slc[C.slice].rit_freq = r.a;
            S.rit_hz = r.a;
            break;
        case Q_GAIN:
            if (!C.pan) break;
            cmd(P_NONE, "display pan set 0x%08lX rfgain=%ld", (unsigned long)C.pan, (long)r.a);
            S.gain = (int8_t)r.a;
            break;
        case Q_ATU_MEM:
            cmd(P_NONE, "atu set memories_enabled=%d", r.a ? 1 : 0);
            S.atu_mem = r.a != 0;
            ESP_LOGI(TAG, "tuner memories %s", r.a ? "on" : "off");
            break;
        }
    }
}

static void tune_out(uint32_t t)
{
    if (C.slice < 0 || S.link != RADIO_LINK_READY || S.ptt.state != PTT_IDLE) return;
    bool fire = false;
    int64_t want = 0;
    taskENTER_CRITICAL(&S_LOCK);
    /* A change made elsewhere, adopted once the knob has been quiet. */
    if (S.remote_pending && t - S.t_last_input_ms >= QUIET_MS &&
        t - S.t_last_send_ms >= QUIET_MS) {
        S.remote_pending = false;
        if (S.f_server != S.f_committed) {
            tune_assign(&S.tune, S.f_server);
            S.f_committed = S.f_server;
            S.reconciles++;
        }
    }
    if (S.have_freq && S.tune.f_display != S.f_committed &&
        t - S.t_last_send_ms >= SEND_MS) {
        want = S.tune.f_display;
        S.f_committed = want;
        S.t_last_send_ms = t;
        fire = true;
    }
    taskEXIT_CRITICAL(&S_LOCK);
    if (fire) {
        char mhz[24];
        mhz_text(want, mhz, sizeof mhz);
        cmd(P_NONE, "slice tune %d %s", C.slice, mhz);
    }
}

static void flex_task(void *arg)
{
    (void)arg;
    C.backoff_ms = 500;
    C.t_retry = now_ms();
    static char tcp[1460];

    for (;;) {
        uint32_t t = now_ms();
        if (C.fd < 0) {
            if ((int32_t)(t - C.t_retry) >= 0) session_begin(t);
            ptt_step(t);
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        fd_set rs;
        FD_ZERO(&rs);
        FD_SET(C.fd, &rs);
        FD_SET(C.ufd, &rs);
        /* TLS may hold bytes already read off the socket: no waiting then. */
        const bool held = C.tls && esp_tls_get_bytes_avail(C.tls) > 0;
        struct timeval tv = { .tv_sec = 0, .tv_usec = held ? 0 : LOOP_MS * 1000 };
        const int sel = select(MAX(C.fd, C.ufd) + 1, &rs, NULL, NULL, &tv);
        if (sel > 0 || held) {
            if (sel > 0 && FD_ISSET(C.ufd, &rs)) {
                int n;
                while ((n = recv(C.ufd, s_udp, sizeof s_udp, MSG_DONTWAIT)) > 0) {
                    if (C.wan && !C.udp_ok) {
                        C.udp_ok = true;
                        ESP_LOGI(TAG, "SmartLink: the radio's UDP arrives");
                    }
                    on_udp(s_udp, n, now_ms());
                }
            }
            if (C.tls && (held || (sel > 0 && FD_ISSET(C.fd, &rs)))) {
                for (int i = 0; i < 8 && C.fd >= 0; i++) {
                    const ssize_t n = esp_tls_conn_read(C.tls, tcp, sizeof tcp);
                    if (n == ESP_TLS_ERR_SSL_WANT_READ || n == ESP_TLS_ERR_SSL_WANT_WRITE) break;
                    if (n == 0) { session_end("the radio closed the connection", false); break; }
                    if (n < 0)  { session_end("connection lost", false); break; }
                    on_tcp(tcp, (int)n, now_ms());
                    if (C.fd < 0 || esp_tls_get_bytes_avail(C.tls) <= 0) break;
                }
                if (C.fd < 0) continue;
            } else if (sel > 0 && FD_ISSET(C.fd, &rs)) {
                const int n = recv(C.fd, tcp, sizeof tcp, MSG_DONTWAIT);
                if (n == 0) { session_end("the radio closed the connection", false); continue; }
                if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                    session_end("connection lost", false);
                    continue;
                }
                if (n > 0) on_tcp(tcp, n, now_ms());
                if (C.fd < 0) continue;
            }
        }
        t = now_ms();

        if (!C.greeted && t - C.t_session > GREET_MS) { session_end("no greeting", false); continue; }
        if (C.registered && C.slice < 0 && S.link != RADIO_LINK_READY &&
            t - C.t_session > SETUP_MS) {
            session_end("no slice within 10 s", true);
            continue;
        }
        if (C.registered && t - C.t_ping >= PING_MS) {
            if (C.pings_out >= PING_MISS) { session_end("the radio stopped answering", false); continue; }
            C.pings_out++;
            C.t_ping = t;
            cmd(P_PING, "ping");
            if (C.fd < 0) continue;
        }

        /* The station we dial for has gone: ask again, with who is here. */
        if (C.station_left) {
            C.station_left = false;
            C.decided = false;
            session_end("the station we dial for left", true);
            continue;
        }
        wan_udp(t);
        choose_step(t);
        if (C.fd < 0) continue;
        setup_step(t);
        /* Dialling for a station whose slice we lost: any other of its own. */
        if (C.bound && C.slice < 0)
            for (int n = 0; n < N_SLC; n++)
                if (s_slc[n].known) { follow_slice(n); break; }
        if (C.slice >= 0) {
            tame_pan();
            /* Audio, and the transmit slice, only as a station of our own: a
             * station we dial for has its own audio, and its own say in
             * which slice transmits. */
            if (C.own && !C.streams_asked) {
                C.streams_asked = true;
                cmd(P_RX_STREAM, "stream create type=remote_audio_rx compression=opus");
                cmd(P_TX_STREAM, "stream create type=remote_audio_tx compression=opus");
            }
            if (C.own && !C.tx_claimed && S.have_freq) {
                C.tx_claimed = true;
                if (!S.tx_slice) cmd(P_NONE, "slice set %d tx=1", C.slice);
            }
            if (S.link != RADIO_LINK_READY && S.have_freq) {
                taskENTER_CRITICAL(&S_LOCK);
                S.link = RADIO_LINK_READY;
                S.t_ready_ms = t;
                taskEXIT_CRITICAL(&S_LOCK);
                C.backoff_ms = 500;
                ESP_LOGI(TAG, "ready: slice %d, %lld Hz, %s", C.slice,
                         (long long)S.f_server, S.mode);
            }
        } else if (S.link == RADIO_LINK_READY) {
            /* Our slice went away under us: find or make another. */
            taskENTER_CRITICAL(&S_LOCK);
            S.link = RADIO_LINK_GREETING;
            taskEXIT_CRITICAL(&S_LOCK);
            C.listed = false;
            C.creating = false;
            C.tx_claimed = false;
            if (C.own) cmd(P_SLICE_LIST, "slice list");
        }
        if (C.fd < 0) continue;

        ptt_step(t);
        take_requests();
        tune_out(t);
    }
}

/* Leave on the way down, so the radio frees our streams at once. */
static void on_restart(void)
{
    if (C.fd < 0) return;
    session_end("restarting", true);
    vTaskDelay(pdMS_TO_TICKS(100));
}

/* ----------------------------------------------------------------- public */

const char *radio_link_name(void) { return "API"; }

esp_err_t radio_start(const char *host, uint16_t port, const char *user, const char *pass)
{
    (void)user;
    (void)pass;
    ESP_RETURN_ON_FALSE(host, ESP_ERR_INVALID_ARG, TAG, "args");
    strlcpy(C.host, host, sizeof C.host);
    C.port = port ? port : API_PORT;
    C.fd = C.ufd = -1;
    C.slice = -1;

    memset(&S, 0, sizeof S);
    tune_init(&S.tune, 14100000, 100);
    ptt_fsm_init(&S.ptt);
    strlcpy(S.mode, "usb", sizeof S.mode);
    S.smeter_dbm = -127.0f;
    S.gain_min = -8;
    S.gain_max = 32;
    S.gain_step = 8;
    S.tx_allowed = true;
    S.pending_choice = -1;

    uuid_load();
    pick_load();
    /* The radio in use: this one by its address, or one by SmartLink. */
    sl_init();
    char serial[24];
    sl_active(serial, sizeof serial);
    C.wan = serial[0] && sl_enabled();
    if (C.wan) ESP_LOGI(TAG, "the radio in use is reached through SmartLink (%s)", serial);
    C.line = heap_caps_malloc(LINE_CAP, MALLOC_CAP_SPIRAM);
    C.meters = heap_caps_calloc(N_METERS, sizeof *C.meters, MALLOC_CAP_SPIRAM);
    s_req = xQueueCreate(16, sizeof(req_t));
    s_rxq = xRingbufferCreateWithCaps(RXQ_BYTES, RINGBUF_TYPE_NOSPLIT, MALLOC_CAP_SPIRAM);
    ESP_RETURN_ON_FALSE(C.line && C.meters && s_req && s_rxq, ESP_ERR_NO_MEM, TAG, "buffers");

    esp_register_shutdown_handler(on_restart);
    /* The codec's stack in PSRAM: Opus is deep, internal RAM is what WiFi
     * sends from, and this task touches no flash. */
    if (xTaskCreatePinnedToCoreWithCaps(codec_task, "fxcodec", 16384, NULL, 5, NULL, 0,
                                        MALLOC_CAP_SPIRAM) != pdPASS)
        ESP_LOGE(TAG, "no memory for the codec task: no audio");
    if (xTaskCreatePinnedToCore(flex_task, "flex", 6144, NULL, 6, NULL, 0) != pdPASS) {
        ESP_LOGE(TAG, "no internal RAM for the flex task (%u free, largest %u)",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        return ESP_ERR_NO_MEM;
    }
    S.link = RADIO_LINK_CONNECTING;
    return ESP_OK;
}

int64_t radio_tune_by(int32_t detents, uint8_t accel_mult, int32_t step_hz)
{
    const uint32_t t = now_ms();
    int64_t f;
    taskENTER_CRITICAL(&S_LOCK);
    if (detents) {
        if (S.tune.step_hz != step_hz) tune_set_step(&S.tune, step_hz);
        tune_apply(&S.tune, detents, accel_mult, LOOP_MS, F_MIN, F_MAX);
        S.t_last_input_ms = t;
    }
    f = S.tune.f_display;
    taskEXIT_CRITICAL(&S_LOCK);
    return f;
}

void radio_set_step(int32_t step_hz)
{
    taskENTER_CRITICAL(&S_LOCK);
    tune_set_step(&S.tune, step_hz);
    taskEXIT_CRITICAL(&S_LOCK);
}

void radio_audio_suspend(bool suspend)
{
    S.audio_suspend = suspend;
    ESP_LOGW(TAG, "RX audio %s", suspend ? "suspended" : "resumed");
}

static void request(req_kind_t kind, int32_t a, int32_t b, const char *s)
{
    req_t r = { .kind = (uint8_t)kind, .a = a, .b = b };
    if (s) strlcpy(r.s, s, sizeof r.s);
    if (!s_req || xQueueSend(s_req, &r, 0) != pdTRUE) ESP_LOGW(TAG, "request dropped: queue full");
}

void radio_set_mode(const char *mode)
{
    const char *api = mode ? mode_api(mode) : NULL;
    if (api) request(Q_MODE, 0, 0, api);
}

void radio_set_filter(int32_t lo, int32_t hi) { request(Q_FILTER, lo, hi, NULL); }

/* The Flex has no filter presets over the API; the editors offer widths. */
void radio_select_filter(uint8_t n) { (void)n; }

void radio_set_agc(const char *agc)
{
    static const char *AGC[] = { "off", "slow", "med", "fast" };
    for (size_t i = 0; agc && i < sizeof AGC / sizeof AGC[0]; i++)
        if (strcasecmp(agc, AGC[i]) == 0) { request(Q_AGC, 0, 0, AGC[i]); return; }
}

void radio_set_gain(int8_t gain)
{
    if (gain < S.gain_min || gain > S.gain_max) return;
    request(Q_GAIN, gain, 0, NULL);
}

void radio_set_rit(int32_t hz)
{
    if (hz > 9999) hz = 9999;
    if (hz < -9999) hz = -9999;
    request(Q_RIT, hz, 0, NULL);
}

void radio_goto_freq(int64_t hz)
{
    const uint32_t t = now_ms();
    taskENTER_CRITICAL(&S_LOCK);
    tune_assign(&S.tune, hz);
    S.t_last_input_ms = t;
    taskEXIT_CRITICAL(&S_LOCK);
}

void radio_tune(void)     { S.pending_tune = 1; }
void radio_atu_tune(void) { if (S.has_atu) S.pending_atu = 1; }
void radio_atu_memories(bool on) { if (S.has_atu) request(Q_ATU_MEM, on, 0, NULL); }
void radio_set_rf_gain(uint8_t pct)  { (void)pct; }
void radio_set_rf_power(uint8_t pct) { (void)pct; }
void radio_set_tuner(bool on)        { (void)on; }
void radio_set_squelch(uint8_t pct)  { (void)pct; }

bool radio_get_choice(uint8_t i, char *title, size_t tn, char *name, size_t nn)
{
    bool ok = false;
    taskENTER_CRITICAL(&S_LOCK);
    if (i < S.n_choices) {
        strlcpy(title, S.ch_title[i], tn);
        strlcpy(name, S.ch_name[i], nn);
        ok = true;
    }
    taskEXIT_CRITICAL(&S_LOCK);
    return ok;
}

void radio_choose(uint8_t i)
{
    if (i < S.n_choices) S.pending_choice = (int8_t)i;
}

/* Not yet on the Flex: its memory channels. */
void radio_memory_mode(bool on)        { (void)on; }
void radio_memory_group(uint8_t group) { (void)group; }

/* Not yet on the Flex: its receive antennas on the swipe. */
void radio_select_rx(uint8_t rx)                 { (void)rx; }
void radio_set_antenna(uint8_t ant, bool rx_ant) { (void)ant; (void)rx_ant; }

/* A radio has no talkgroup to lock or mute. */
void radio_tg_lock(bool locked) { (void)locked; }
void radio_mute(bool muted)     { (void)muted; }

void radio_ptt_key(void)    { S.pending_key = 1; }
void radio_ptt_unkey(void)  { S.pending_unkey = 1; }
void radio_ptt_toggle(void) { S.pending_toggle = 1; }
void radio_ptt_force_abort(uint8_t reason) { S.pending_abort = reason; }

bool radio_is_ready(void)
{
    return S.link == RADIO_LINK_READY || S.link == RADIO_LINK_DEGRADED;
}

bool radio_on_air(void) { return S.tx || S.ptt.state != PTT_IDLE; }

void radio_get_status(radio_status_t *o)
{
    if (!o) return;
    memset(o, 0, sizeof *o);
    const uint32_t t = now_ms();
    taskENTER_CRITICAL(&S_LOCK);
    o->link       = S.link;
    o->f_display  = S.tune.f_display;
    o->f_server   = S.f_server;
    o->filt_lo    = S.filt_lo;
    o->filt_hi    = S.filt_hi;
    o->have_gain  = S.have_gain;
    o->gain       = S.gain;
    o->gain_min   = S.gain_min;
    o->gain_max   = S.gain_max;
    o->gain_step  = S.gain_step;
    o->rit_hz     = S.rit_hz;
    o->smeter_dbm = S.smeter_dbm;
    o->tx_mic_dbm = S.tx_mic_dbm;
    o->tx_fwd_w   = S.tx_fwd_w;
    o->tx_peak_w  = S.tx_peak_w;
    o->tx_swr     = S.tx_swr;
    o->tx_alc     = S.tx_alc;
    o->tx         = S.tx;
    o->n_trx      = 1;
    o->connects   = S.connects;
    o->closes     = S.closes;
    o->reconciles = S.reconciles;
    o->rejects    = S.rejects;
    o->unknown_cmds = S.unknown_cmds;
    o->sends      = S.sends;
    o->echoes     = S.echoes;
    o->chronos    = S.chronos;
    o->txa_sent   = S.txa_sent;
    o->txa_failed = S.txa_failed;
    o->txa_skipped = S.txa_skipped;
    o->txa_max_us = S.txa_max_us;
    o->ptt_state  = (uint8_t)S.ptt.state;
    o->ptt_rung   = S.ptt.rung;
    o->ptt_reason = (uint8_t)S.ptt.reason;
    o->ptt_refusals = S.ptt.refusals;
    o->permit     = ptt_permit_now(t);
    o->pong_age_ms = C.fd >= 0 ? (int32_t)(t - C.t_pong) : -1;
    strlcpy(o->tx_why, tx_why(), sizeof o->tx_why);
    o->has_tune   = true;
    o->has_atu    = S.has_atu;
    strlcpy(o->note, S.note, sizeof o->note);
    o->note_seq   = S.note_seq;
    o->atu_mem    = S.atu_mem;
    o->n_choices  = S.n_choices;
    o->choice_default = S.choice_default;
    o->choices_seq = S.choices_seq;
    strlcpy(o->mode, S.mode, sizeof o->mode);
    strlcpy(o->agc, S.agc, sizeof o->agc);
    strlcpy(o->last_close, S.last_close, sizeof o->last_close);
    taskEXIT_CRITICAL(&S_LOCK);
}

/* --- SmartLink's radios, beside the configured ones (radio.h) --------- */

int radio_found_count(void)
{
    sl_init();
    return sl_enabled() ? sl_count() : 0;
}

bool radio_found_get(int i, char *name, size_t cap)
{
    sl_init();
    sl_radio_t r;
    if (!sl_enabled() || !sl_get(i, &r)) {
        if (cap) name[0] = 0;
        return false;
    }
    strlcpy(name, r.name[0] ? r.name : r.serial, cap);
    return true;
}

const char *radio_found_via(void) { return "SmartLink"; }

int radio_found_active(void)
{
    sl_init();                          /* asked before anything else loads it */
    if (!sl_enabled()) return -1;
    char a[24];
    sl_active(a, sizeof a);
    if (!a[0]) return -1;
    for (int i = 0; i < sl_count(); i++) {
        sl_radio_t r;
        if (sl_get(i, &r) && !strcmp(r.serial, a)) return i;
    }
    return -1;
}

esp_err_t radio_found_use(int i)
{
    sl_init();
    if (i < 0) return sl_set_active("");
    sl_radio_t r;
    if (!sl_get(i, &r)) return ESP_ERR_INVALID_ARG;
    return sl_set_active(r.serial);
}

/* The configuration page's SmartLink endpoints (webcfg's hook). */
size_t radio_web_endpoints(const httpd_uri_t **out);
size_t radio_web_endpoints(const httpd_uri_t **out)
{
    sl_init();
    return sl_web_endpoints(out);
}
