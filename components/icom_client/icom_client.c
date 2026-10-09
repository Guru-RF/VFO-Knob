/* Icom network client: an IC-705 over WiFi, the icom firmware's radio.h.
 *
 * The radio's LAN protocol (the one RS-BA1 and wfview speak) is three UDP
 * streams: control on the port configured (50001), then CI-V and audio on
 * ports the radio hands out after login (50002 and 50003 by default). Every
 * stream carries the same 16-byte header -- length, type, sequence, our id,
 * the radio's id, all little-endian -- and keeps itself alive the same way:
 * "are you there" until the radio answers, then pings every 500 ms and idle
 * packets every 100 ms. Written from the protocol as wfview documents it
 * (packettypes.h); none of wfview's code is used.
 *
 * The session, in order: are-you-there / I-am-here / are-you-ready /
 * I-am-ready on control; login with the scrambled user name and password; a
 * token, confirmed, and renewed every minute; the radio's capabilities and
 * whether anyone else holds it; a stream request naming our local CI-V and
 * audio ports and the audio format; the radio's own ports in reply. Then the
 * CI-V and audio streams each do their own handshake, and CI-V is opened.
 *
 * Tuning is optimistic, as radio.h promises: the display moves at once, a
 * frequency set goes out at most every SET_PERIOD_MS, and what the radio
 * reports is adopted only once the knob has been quiet for QUIET_MS -- the
 * radio echoes each of our own sets straight back, and chasing those echoes
 * mid-turn would drag the dial backwards.
 */
#include "radio.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/param.h>

#include "audio_in.h"
#include "audio_out.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "kvstore.h"
#include "ptt_fsm.h"
#include "vfo_tune.h"

static const char *TAG = "icom";

#define CIV_RADIO      0xA4          /* the IC-705's CI-V address: until the
                                        server names the radio's own */
#define CIV_US         0xE0          /* a controller's                     */
#define OUR_NAME       "VFO-Knob"    /* shown by the radio as its user     */

#define PKT_CONTROL    0x10
#define PKT_PING       0x15
#define PKT_OPENCLOSE  0x16
#define PKT_AUDIO_HDR  0x18
#define PKT_TOKEN      0x40
#define PKT_STATUS     0x50
#define PKT_LOGIN_RSP  0x60
#define PKT_LOGIN      0x80
#define PKT_CONNINFO   0x90
#define PKT_CAPS       0x42
#define PKT_RADIO_CAP  0x66

#define AUDIO_CODEC    0x04          /* LPCM, one channel, 16 bit          */
#define AUDIO_FRAME    480           /* 20 ms at 24 kHz                    */
#define TX_LATENCY_MS  150

#define LOOP_MS        5
#define AYT_MS         500
#define PING_MS        500
#define IDLE_MS        100
#define TOKEN_MS       60000
#define ALIVE_MS       5000          /* silence on control = link lost     */
#define CIV_QUIET_MS   2000          /* CI-V silence: ask for it again     */
#define SET_PERIOD_MS  40            /* frequency sets, while turning      */
#define QUIET_MS       400           /* then the radio's word is final     */
#define GREET_TMO_MS   8000
#define BUSY_TMO_MS    180000        /* how long the radio may hold a stale one */
#define STREAM_DEAD_MS 5000          /* CI-V or audio silent: the session is gone */
#define SLOW_TICK_MS   220           /* one slow-poll question per tick */
#define CIV_FRESH_MS   1500          /* PTT only while CI-V is answering */

/* The X6100's server stops sending receive audio within a second unless the
 * client sends audio too -- wfview always does, its microphone or silence --
 * so that firmware keeps a stream of silence going whenever it is not
 * transmitting. An IC-705 streams without it, and is spared the traffic. */
#if VFO_RADIO_XIEGU
#define AUDIO_BOTH_WAYS 1
#else
#define AUDIO_BOTH_WAYS 0
#endif

/* The X6100's server also stops sending audio now and then, for good, while
 * its CI-V carries on unharmed -- measured: after 0.2 s in one session, after
 * 19 s in another. A new session brings it back only for as long, so there a
 * silent audio stream does not end the session: the dial carries on, without
 * the sound. On an IC-705 it means the session is gone, and it is started
 * over. */
#if VFO_RADIO_XIEGU
#define AUDIO_BEST_EFFORT 1
#else
#define AUDIO_BEST_EFFORT 0
#endif

/* Memory channels: groups 00-99 of channels 00-99 (see the memories
 * section). */
#define MEM_CHANNELS   100
#define MEM_REQ_MS     300           /* a memory read not answered: again  */
#define MEM_STEP_MS    60            /* channel selects, while turning     */
#define MEM_SETTLE_MS  3000          /* after ready, before reading them   */
/* Where the dial keeps its memory state: per firmware, so a knob switched from
 * the IC-705's firmware to the Xiegu's does not come up in the IC-705's
 * memory mode. */
#if VFO_RADIO_XIEGU
#define MEM_NVS_NS     "xiegu"
#else
#define MEM_NVS_NS     "icom"
#endif

#define KEEP_N         16            /* tracked packets kept for resends   */
#define KEEP_LEN       192

/* --------------------------------------------------------------- streams */

typedef struct {
    const char        *name;
    int                fd;
    struct sockaddr_in to;
    uint16_t           lport;
    uint32_t           my_id, remote_id;
    uint16_t           seq, ping_seq;
    bool               here, ready, idles;
    uint32_t           t_ayt, t_ping, t_idle, t_rx;
    uint8_t           *keep;        /* KEEP_N x KEEP_LEN, PSRAM; NULL = none */
    uint16_t           keep_seq[KEEP_N], keep_len[KEEP_N];
    uint8_t            keep_i;
} stream_t;

static stream_t s_ctl = { .name = "ctl", .fd = -1 };
static stream_t s_civ = { .name = "civ", .fd = -1 };
static stream_t s_aud = { .name = "aud", .fd = -1 };

/* ----------------------------------------------------------------- state */

typedef struct {
    radio_link_t link;
    tune_t     tune;
    int64_t    f_committed, f_server;
    uint32_t   t_last_input_ms, t_last_send_ms, t_ready_ms;
    bool       have_freq;
    char       mode[8];
    uint8_t    mode_byte, data_mode, filter_no, width_idx;
    char       agc[6];               /* 16 12: "fast", "mid", "slow" */
    uint8_t    preamp;               /* 16 02: 0 off, 1 P.AMP1, 2 P.AMP2 */
    bool       have_preamp;
    uint8_t    rf_gain, rf_power;    /* 14 02, 14 0A: 0-255 */
    uint8_t    squelch;              /* 14 03: 0-255, 0 open */
    uint8_t    tuner;                /* 1C 01: 0 out, 1 in the line, 2 tuning */
    bool       have_rf_gain, have_rf_power, have_tuner, have_squelch;
    int32_t    filt_lo, filt_hi, rit_hz;
    float      smeter_dbm;
    float      tx_mic_dbm, tx_fwd_w, tx_peak_w, tx_swr, tx_alc;
    bool       tx;                   /* the radio says it is transmitting */
    uint32_t   connects, closes, reconciles, rejects, unknown_cmds, sends, echoes;
    uint32_t   chronos, txa_sent, txa_failed, txa_skipped, txa_max_us;
    char       last_close[48];
    ptt_fsm_t  ptt;
    uint32_t   pending_key, pending_unkey, pending_toggle;
    uint8_t    pending_abort;
    bool       audio_suspend;
    /* Memory mode: the channel shown, and what the other tasks asked for. */
    bool       mem_mode;
    uint8_t    mem_state, mem_ch;    /* radio_mem_state_t */
    int32_t    mem_steps;            /* detents in memory mode, not yet taken */
    int8_t     pending_mem;          /* -1 none, 0 leave, 1 enter */
    int16_t    pending_group;        /* -1 none */
    /* The radio, from its capabilities; MODEL_OTHER until they come. */
    const struct model *model;
    /* A second receiver and a choice of antennas (the IC-7610). */
    uint8_t    rx;                   /* 07 D2: 0 MAIN, 1 SUB */
    bool       have_rx;
    uint8_t    ant;                  /* 12: 0 ANT1, 1 ANT2 */
    bool       ant_rx, have_ant;     /* ...receiving on the RX ANT input */
    int8_t     pending_rx;           /* -1 none */
    int8_t     pending_ant;          /* -1 none; else the antenna, | 0x10 for RX ANT */
} state_t;

static state_t      S;
static portMUX_TYPE S_LOCK = portMUX_INITIALIZER_UNLOCKED;

/* The session: owned by the task, never touched from outside. */
static struct {
    char       host[48], user[33], pass[33];
    uint16_t   port;
    uint16_t   auth_seq, tokreq, civ_seqb, aud_seqb;
    uint32_t   token;
    bool       authed, streaming, civ_open;
    uint8_t    guid[16], mac[6];
    uint16_t   commoncap;
    char       radio_name[32];
    uint8_t    civ_addr;           /* the radio's CI-V address, from the server */
    /* What the radio has refused outright, so it is not asked again: memory
     * reads (no memories over CI-V) and the modulation inputs (no such
     * setting). An IC-705 refuses neither; a radio behind wfview's server
     * that is not one may. */
    bool       no_mem, no_modin, no_probe;
    bool       probing;                  /* the model has wfview's numbers to read */
    bool       aud_quiet;          /* the radio's audio has stopped (AUDIO_BEST_EFFORT) */
    uint32_t   t_token, t_civ_rx, t_civ_open, t_session, t_retry, t_aud_rx;
    bool       waiting_busy;       /* the radio still holds our last session */
    uint32_t   backoff_ms;
    /* CI-V scheduling */
    uint32_t   t_poll_s, t_poll_tx, t_poll_ptt, t_poll_slow;
    uint8_t    slow_i;             /* the slow poll's next question */
    uint8_t    tx_meter_i;
    uint32_t   t_tx_frame;
    /* The radio's modulation inputs as the operator left them: 1A 05 01 18
     * (voice modes) and 19 (data modes). 0xFF until the radio has said. */
    uint8_t    modin_off, modin_d1;
    bool       modin_switched, modin_logged;
    /* This over's microphone, for the line logged when it ends. */
    uint16_t   over_frames, over_silent;
    int        over_peak;
    /* Memories: the dial's group, and reading it. */
    uint8_t    mem_group;
    int16_t    mem_scan;           /* the channel being read; -1 = not reading */
    uint8_t    mem_tries, mem_used;
    bool       mem_loaded;         /* s_mem holds a full read of the group */
    bool       mem_select_due;     /* go to a channel once it has */
    int16_t    mem_sent;           /* the channel the radio was put on; -1 */
    uint32_t   t_mem_req, t_mem_sel, t_mem_dirty;
} C;

/* One memory channel, as the radio holds it (see the memories section). */
typedef struct {
    int64_t  hz;
    int32_t  offset_hz;
    uint16_t tone_dhz;
    int8_t   duplex;
    bool     used;
    char     name[17];
} mem_t;
static mem_t *s_mem;                 /* MEM_CHANNELS of them, PSRAM */
static bool   s_mem_restored;        /* the dial's memory state, read from NVS */

/* While one of our overs has them switched to the network, the operator's
 * values are kept here too. RTC_NOINIT survives a crash or a watchdog reset,
 * so the next session puts them back instead of leaving the radio deaf to its
 * own microphone -- by the commands they were read with, and only to a radio
 * that has the same: another radio's numbers mean other settings. */
#define MODIN_MAGIC 0x4D4F4449u
RTC_NOINIT_ATTR static struct {
    uint32_t magic;
    uint8_t  off, d1;
    uint16_t voice, data;
} s_modin_saved;

static uint8_t  s_rx[1500];
static uint8_t *s_txa;              /* one outbound audio packet, PSRAM */

static inline uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

/* Overridden by the application; see tci_client.c. */
__attribute__((weak)) void haptic_hook(uint8_t effect, uint8_t prio)
{
    (void)effect; (void)prio;
}

/* ------------------------------------------------------------ byte order */

static void put16le(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static void put32le(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = v >> (8 * i); }
static void put16be(uint8_t *p, uint16_t v) { p[0] = v >> 8; p[1] = v; }
static void put32be(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = v >> (24 - 8 * i); }
static uint16_t get16le(const uint8_t *p) { return p[0] | p[1] << 8; }
static uint32_t get32le(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t get16be(const uint8_t *p) { return p[0] << 8 | p[1]; }

/* The login scramble: each character plus its position indexes this table.
 * A protocol constant -- the radio decodes with the same one. */
static const uint8_t SCRAMBLE[95] = {
    0x47,0x5d,0x4c,0x42,0x66,0x20,0x23,0x46,0x4e,0x57,0x45,0x3d,0x67,0x76,0x60,0x41,0x62,0x39,0x59,0x2d,0x68,0x7e,
    0x7c,0x65,0x7d,0x49,0x29,0x72,0x73,0x78,0x21,0x6e,0x5a,0x5e,0x4a,0x3e,0x71,0x2c,0x2a,0x54,0x3c,0x3a,0x63,0x4f,
    0x43,0x75,0x27,0x79,0x5b,0x35,0x70,0x48,0x6b,0x56,0x6f,0x34,0x32,0x6c,0x30,0x61,0x6d,0x7b,0x2f,0x4b,0x64,0x38,
    0x2b,0x2e,0x50,0x40,0x3f,0x55,0x33,0x37,0x25,0x77,0x24,0x26,0x74,0x6a,0x28,0x53,0x4d,0x69,0x22,0x5c,0x44,0x31,
    0x36,0x58,0x3b,0x7a,0x51,0x5f,0x52,
};

static void scramble(const char *in, uint8_t out[16])
{
    memset(out, 0, 16);
    for (int i = 0; i < 16 && in[i]; i++) {
        int p = (uint8_t)in[i] + i;
        if (p > 126) p = 32 + p % 127;
        out[i] = (p >= 32 && p <= 126) ? SCRAMBLE[p - 32] : 0;
    }
}

/* ---------------------------------------------------------- stream basics */

static void hdr(const stream_t *s, uint8_t *b, uint32_t len, uint16_t type, uint16_t seq)
{
    put32le(b + 0, len);
    put16le(b + 4, type);
    put16le(b + 6, seq);
    put32le(b + 8, s->my_id);
    put32le(b + 12, s->remote_id);
}

static bool raw_send(stream_t *s, const uint8_t *b, size_t len)
{
    return s->fd >= 0 && send(s->fd, b, len, 0) == (ssize_t)len;
}

static void control(stream_t *s, uint16_t type, uint16_t seq)
{
    uint8_t b[PKT_CONTROL];
    hdr(s, b, sizeof b, type, seq);
    raw_send(s, b, sizeof b);
}

/* A tracked packet takes the next sequence number, and ctl/civ keep it so the
 * radio can ask for it again. */
static bool tracked(stream_t *s, uint8_t *b, size_t len)
{
    put16le(b + 6, s->seq);
    if (s->keep && len <= KEEP_LEN) {
        uint8_t i = s->keep_i++ % KEEP_N;
        memcpy(s->keep + i * KEEP_LEN, b, len);
        s->keep_seq[i] = s->seq;
        s->keep_len[i] = len;
    }
    s->seq++;
    s->t_idle = now_ms();
    return raw_send(s, b, len);
}

static void resend(stream_t *s, uint16_t seq)
{
    if (!s->keep) return;
    for (int i = 0; i < KEEP_N; i++)
        if (s->keep_len[i] && s->keep_seq[i] == seq) {
            raw_send(s, s->keep + i * KEEP_LEN, s->keep_len[i]);
            return;
        }
}

static void ping(stream_t *s)
{
    uint8_t b[PKT_PING] = { 0 };
    hdr(s, b, sizeof b, 0x07, s->ping_seq);
    b[0x10] = 0x00;
    put32le(b + 0x11, now_ms());
    raw_send(s, b, sizeof b);
}

/* What every stream shares: the handshake, pings, idles and resends. Returns
 * true when the packet is fully handled here. */
static bool common(stream_t *s, const uint8_t *d, int n, uint32_t t)
{
    uint16_t type = get16le(d + 4), seq = get16le(d + 6);
    s->t_rx = t;
    if (n == PKT_CONTROL) {
        switch (type) {
        case 0x04:                                   /* I am here */
            s->remote_id = get32le(d + 8);
            s->here = true;
            control(s, 0x06, 1);                     /* are you ready */
            return true;
        case 0x06:                                   /* I am ready */
            s->remote_id = get32le(d + 8);
            s->ready = true;
            return false;                            /* the stream acts on it */
        case 0x01:                                   /* send one again */
            resend(s, seq);
            return true;
        default:
            return true;
        }
    }
    if (n == PKT_PING && type == 0x07) {
        if (d[0x10] == 0x00) {                       /* their ping: answer */
            uint8_t b[PKT_PING];
            hdr(s, b, sizeof b, 0x07, seq);
            b[0x10] = 0x01;
            memcpy(b + 0x11, d + 0x11, 4);
            raw_send(s, b, sizeof b);
        } else if (seq == s->ping_seq) {
            s->ping_seq++;
        }
        return true;
    }
    if (type == 0x01 && n > PKT_CONTROL) {           /* send several again */
        for (int i = PKT_CONTROL; i + 1 < n; i += 2) resend(s, get16le(d + i));
        return true;
    }
    return false;
}

static void stream_tick(stream_t *s, uint32_t t)
{
    if (s->fd < 0 || !s->to.sin_port) return;
    if (!s->here) {
        if (t - s->t_ayt >= AYT_MS) { control(s, 0x03, 0); s->t_ayt = t; }
        return;
    }
    if (t - s->t_ping >= PING_MS) { ping(s); s->t_ping = t; }
    if (s->idles && t - s->t_idle >= IDLE_MS) {
        uint8_t b[PKT_CONTROL];
        hdr(s, b, sizeof b, 0x00, 0);
        tracked(s, b, sizeof b);
    }
}

static int stream_open(stream_t *s, uint32_t local_ip_be)
{
    s->fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s->fd < 0) return -1;
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = 0,
                             .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (bind(s->fd, (struct sockaddr *)&a, sizeof a) < 0) return -1;
    socklen_t al = sizeof a;
    getsockname(s->fd, (struct sockaddr *)&a, &al);
    s->lport = ntohs(a.sin_port);
    const uint8_t *ip = (const uint8_t *)&local_ip_be;
    s->my_id = (uint32_t)ip[2] << 24 | (uint32_t)ip[3] << 16 | s->lport;
    s->remote_id = 0;
    s->seq = 1;                     /* the radio counts tracked packets from 1 */
    s->ping_seq = 0;
    s->here = s->ready = false;
    s->t_ayt = s->t_ping = s->t_idle = 0;
    s->t_rx = now_ms();
    memset(s->keep_len, 0, sizeof s->keep_len);
    return 0;
}

static void stream_connect(stream_t *s, uint32_t ip_be, uint16_t port)
{
    s->to = (struct sockaddr_in){ .sin_family = AF_INET, .sin_port = htons(port),
                                  .sin_addr.s_addr = ip_be };
    connect(s->fd, (struct sockaddr *)&s->to, sizeof s->to);
}

static void stream_close(stream_t *s)
{
    if (s->fd >= 0) close(s->fd);
    s->fd = -1;
    s->to.sin_port = 0;
    s->here = s->ready = false;
}

/* ------------------------------------------------------------------ CI-V */

/* Every CI-V command is answered once, and in order: with its data, or with
 * FB for done and FA for refused. So a queue of what went out says which
 * command a refusal was for. The radio's own reports (transceive) go to 00,
 * not to us, and answer nothing. */
#define AWAIT_N 32
static struct { uint8_t cmd, sub, len; } s_await[AWAIT_N];
static uint8_t s_aw_head, s_aw_n;

static void await_push(const uint8_t *body, size_t n)
{
    if (s_aw_n == AWAIT_N) {                     /* the oldest went unanswered */
        s_aw_head = (s_aw_head + 1) % AWAIT_N;
        s_aw_n--;
    }
    const unsigned i = (s_aw_head + s_aw_n) % AWAIT_N;
    s_await[i].cmd = body[0];
    s_await[i].sub = n > 1 ? body[1] : 0xFF;
    s_await[i].len = (uint8_t)n;
    s_aw_n++;
}

/* What an answer to `cmd` was for: the oldest outstanding command, or for a
 * data reply the oldest with that command -- any before it went unanswered. */
static bool await_pop(uint8_t cmd, uint8_t *c, uint8_t *sub, uint8_t *len)
{
    while (s_aw_n) {
        const unsigned i = s_aw_head;
        s_aw_head = (s_aw_head + 1) % AWAIT_N;
        s_aw_n--;
        if (cmd == 0xFA || cmd == 0xFB || s_await[i].cmd == cmd) {
            *c = s_await[i].cmd;
            *sub = s_await[i].sub;
            *len = s_await[i].len;
            return true;
        }
    }
    return false;
}

static void log_refused(uint8_t c, uint8_t sub)
{
    static uint16_t last;
    static uint32_t t_last;
    const uint16_t k = (uint16_t)(c << 8 | sub);
    const uint32_t t = (uint32_t)(esp_timer_get_time() / 1000);
    if (k == last && t - t_last < 10000) return;      /* once in a while will do */
    last = k;
    t_last = t;
    if (sub == 0xFF) ESP_LOGW(TAG, "the radio refused %02X", c);
    else             ESP_LOGW(TAG, "the radio refused %02X %02X", c, sub);
}

/* A refused READ that says a whole feature is missing, so it is not asked
 * for again this session: an IC-705 refuses neither of these, but a radio
 * behind wfview's server that only answers like one may lack them. A refused
 * set is left alone -- that can be a matter of the moment. */
static void on_refused(uint8_t c, uint8_t sub, uint8_t len)
{
    if (c == 0x1A && sub == 0x00 && !C.no_mem) {                /* 1A 00 g g c c */
        C.no_mem = true;
        C.mem_scan = -1;
        taskENTER_CRITICAL(&S_LOCK);
        S.mem_mode  = false;
        S.mem_state = RADIO_MEM_OFF;
        taskEXIT_CRITICAL(&S_LOCK);
        ESP_LOGW(TAG, "the radio has no memories over CI-V: no memory mode");
    } else if (c == 0x07 && sub == 0xD2 && len == 2) {           /* MAIN or SUB? */
        static bool said;
        if (!said) {
            said = true;
            ESP_LOGW(TAG, "the radio will not say whether MAIN or SUB is selected: "
                          "no overs from the knob, which cannot tell what it would key");
        }
    } else if (c == 0x1A && sub == 0x05 && len == 4 && C.probing &&
               !C.no_probe) {                                     /* wfview's, read */
        C.no_probe = true;
        ESP_LOGW(TAG, "the radio refuses wfview's modulation-input numbers: not read again");
    } else if (c == 0x1A && sub == 0x05 && len == 4 && !C.no_modin) {  /* read */
        C.no_modin = true;
        ESP_LOGW(TAG, "the radio has no modulation-input setting over CI-V: "
                      "overs go out on whatever input it is set to");
    }
}

static void civ_send(const uint8_t *body, size_t n)
{
    if (s_civ.fd < 0 || !C.civ_open) return;
    uint8_t b[0x15 + 32];
    size_t  len = 0x15 + 4 + n + 1;
    if (len > sizeof b) return;
    hdr(&s_civ, b, len, 0x00, 0);
    b[0x10] = 0xC1;
    put16le(b + 0x11, (uint16_t)(len - 0x15));
    put16be(b + 0x13, C.civ_seqb++);
    uint8_t *f = b + 0x15;
    f[0] = 0xFE; f[1] = 0xFE; f[2] = C.civ_addr; f[3] = CIV_US;
    memcpy(f + 4, body, n);
    f[4 + n] = 0xFD;
    tracked(&s_civ, b, len);
    await_push(body, n);
    S.sends++;
}

#define CIV(...) do { const uint8_t _b[] = { __VA_ARGS__ }; civ_send(_b, sizeof _b); } while (0)

/* The setters in radio.h run on the caller's task, and two tasks building
 * frames on one stream would tangle its sequence numbers. So they leave their
 * commands here, and the icom task sends them on its next pass. */
typedef struct { uint8_t n, b[11]; } civ_msg_t;
static QueueHandle_t s_later;

static void civ_later(const uint8_t *body, size_t n)
{
    civ_msg_t m = { .n = (uint8_t)n };
    if (!s_later || n > sizeof m.b) return;
    memcpy(m.b, body, n);
    if (xQueueSend(s_later, &m, 0) != pdTRUE) ESP_LOGW(TAG, "CI-V command dropped: queue full");
}

#define CIV_LATER(...) do { const uint8_t _b[] = { __VA_ARGS__ }; civ_later(_b, sizeof _b); } while (0)

static void civ_openclose(bool open)
{
    uint8_t b[PKT_OPENCLOSE] = { 0 };
    hdr(&s_civ, b, sizeof b, 0x00, 0);
    put16le(b + 0x10, 0x01C0);
    b[0x12] = 0;
    put16be(b + 0x13, C.civ_seqb++);
    b[0x15] = open ? 0x04 : 0x00;
    tracked(&s_civ, b, sizeof b);
}

static uint8_t bcd(unsigned v) { return (uint8_t)(((v / 10) % 10) << 4 | (v % 10)); }
static unsigned unbcd(uint8_t b) { return (b >> 4) * 10 + (b & 0x0F); }

/* Frequencies are five BCD bytes, least significant first -- six on the
 * IC-905 from 10 GHz, where five stop at 9.999 999 999 GHz (wfview's
 * icomcommander.cpp: "On the IC-905 10GHz+ uses 6 bytes for freq!"). */
static int64_t freq_from_n(const uint8_t *p, size_t n)
{
    int64_t f = 0, m = 1;
    for (size_t i = 0; i < n && i < 6; i++, m *= 100) f += unbcd(p[i]) * m;
    return f;
}
static int64_t freq_from(const uint8_t *p) { return freq_from_n(p, 5); }

/* Five bytes, or six from 10 GHz as wfview sends them: how many it wrote. */
static size_t freq_to(uint8_t *p, int64_t f)
{
    const size_t n = f >= 10000000000LL ? 6 : 5;
    for (size_t i = 0; i < n; i++, f /= 100) p[i] = bcd((unsigned)(f % 100));
    return n;
}

/* Levels and meters are 0-255 as two BCD bytes, most significant first. */
static unsigned level_from(const uint8_t *p) { return unbcd(p[0]) * 100 + unbcd(p[1]); }

/* The IC-705's own calibration points (its CI-V reference; wfview's rig file
 * carries the same): raw reading -> what the radio's meter says. */
typedef struct { uint8_t raw; float val; } cal_t;
static const cal_t CAL_S[]   = { {0,-54}, {10,-48}, {30,-36}, {60,-24}, {90,-12},
                                 {120,0}, {241,64} };          /* dB over S9 */
static const cal_t CAL_PO[]  = { {0,0}, {21,0.5f}, {43,1}, {65,1.5f}, {83,2},
                                 {95,2.5f}, {105,3}, {114,3.5f}, {124,4},
                                 {143,5}, {183,7.5f}, {213,10}, {255,12} };  /* W */
static const cal_t CAL_SWR[] = { {0,1}, {48,1.5f}, {80,2}, {120,3}, {240,6} };

static float calibrate(const cal_t *c, size_t n, unsigned raw)
{
    if (raw <= c[0].raw) return c[0].val;
    for (size_t i = 1; i < n; i++)
        if (raw <= c[i].raw)
            return c[i - 1].val + (c[i].val - c[i - 1].val) *
                   (float)(raw - c[i - 1].raw) / (float)(c[i].raw - c[i - 1].raw);
    return c[n - 1].val;
}
#define CAL(t, raw) calibrate(t, sizeof t / sizeof t[0], raw)
#define NCAL(t) (uint8_t)(sizeof t / sizeof t[0])

/* The IC-7610's S-meter is not quite the IC-705's (wfview's rig files). Its
 * Po reads the IC-705's table ten times over: 100 W where that has 10. */
static const cal_t CAL_S_7610[] = { {0,-54}, {11,-48}, {21,-42}, {34,-36}, {50,-30},
                                    {59,-24}, {75,-18}, {93,-12}, {103,-6}, {124,0},
                                    {145,10}, {160,20}, {183,30}, {204,40}, {222,50},
                                    {246,60} };

/* The IC-905's S-meter: the IC-705's up to S9, S9+60 at the top rather than
 * +64 (wfview's IC-905.rig; IC-R8600.rig has the same). */
static const cal_t CAL_S_905[] = { {0,-54}, {10,-48}, {30,-36}, {60,-24}, {90,-12},
                                   {120,0}, {241,60} };

/* ---------------------------------------------------------------- models */

/* What differs between the radios this client knows, found by the name in
 * their capabilities. The LAN protocol, and the CI-V for tuning, mode,
 * filter, AGC, preamp and meters, are the same on all of them; the rest is
 * here.
 *
 * A radio not in the table gets what is safe on any of them: tuning and
 * receive, with neither memory reads nor modulation-input switching -- its
 * overs go out on whatever input it is set to. The IC-705's numbers for those
 * are other settings on another radio. */
typedef struct model {
    const char    *name;            /* the capabilities' name starts with it */
    int64_t        f_min, f_max;    /* where the knob may tune */
    uint8_t        preamps;         /* P.AMP1 and P.AMP2, or a single one */
    int64_t        one_preamp_hz;   /* from here up a single one; 0 = nowhere */
    const cal_t   *cal_s;           /* the S-meter */
    uint8_t        n_cal_s;
    float          po_scale;        /* times CAL_PO, the IC-705's 10 W */
    /* The modulation inputs, voice and data modes: 1A 05 <hi> <lo>, and the
     * value that means the network. 0 = nothing the knob may switch. */
    uint16_t       modin_voice, modin_data;
    uint8_t        modin_lan;
    const char *const *modin_names; /* by value, for the log */
    uint8_t        n_modin_names;
    bool           memories;        /* groups of channels over 1A 00 */
    /* ...the IC-9700's: a group per band, the one it is on -- 1 2 m, 2 70 cm,
     * 3 23 cm -- of channels 1-99, in a layout of its own (on_memory). */
    bool           mem_by_band;
    uint8_t        n_rx;            /* receivers: MAIN and SUB, 07 D0/D1/D2 */
    uint8_t        n_ant;           /* antennas to choose from, 12 */
    bool           rx_ant;          /* ...each also with the RX ANT input */
    uint16_t       max_w;           /* RF power's full scale, 14 0A; 0 = say % */
    /* ...from 400 MHz and from 1 GHz, where it differs: the IC-9700's 75 W
     * on 70 cm and 10 W on 23 cm. 0 = max_w there too. */
    uint16_t       max_w_uhf, max_w_shf;
    bool           tuner;           /* an antenna tuner in the line, 1C 01 */
    /* A receiver: nothing to key, so no PTT, and none of the reads that go
     * with a transmitter -- 1C 00, RF power, RIT -- which it refuses. */
    bool           rx_only;
    bool           squelch;         /* a squelch the dial sets, 14 03 */
    /* The IC-905's: from 10 GHz a frequency is six bytes in 00, 03 and 05. */
    bool           freq6;
    /* ...its power from 2 GHz, and from 10 GHz in milliwatts -- 0.5 W, which
     * the dial shows as %: 0 = as the band below. */
    uint16_t       max_w_2g, max_mw_10g;
    bool           no_rit;          /* no 21 xx: RIT neither read nor set */
    /* wfview's modulation-input numbers on a radio nobody here has tried:
     * read and logged against modin_names, never written (modin_voice 0). */
    uint16_t       probe_voice, probe_data;
} model_t;

static const char *const MODIN_705[]  = { "MIC", "USB", "MIC+USB", "WLAN" };
static const char *const MODIN_7610[] = { "MIC", "ACC", "MIC+ACC", "USB", "MIC+USB", "LAN" };
/* By value as wfview's rig files give them: for the log only, nothing is
 * switched on these until a radio has confirmed them (probe_voice). */
static const char *const MODIN_7300MK2[] = { "MIC", "USB", "ACC", "MIC+USB", "MIC+ACC", "LAN" };
static const char *const MODIN_905[]     = { "MIC", "USB", "MIC+USB", "LAN" };
static const char *const MODIN_7760[]    = { "MIC", "USB", "LINE", "ACC", "MIC+USB", "MIC+LINE",
                                             "MIC+ACC", "MIC+USB+ACC", "MIC+LINE+ACC", "LAN" };

static const model_t MODELS[] = {
    /* 30 kHz-470 MHz; two preamps on HF and 6 m, a single one on 2 m and
     * 70 cm -- should it refuse one it answers NG and keeps what it had, and
     * the dial shows that. */
    { .name = "IC-705", .f_min = 30000, .f_max = 470000000,
      .preamps = 2, .one_preamp_hz = 100000000,
      .cal_s = CAL_S, .n_cal_s = NCAL(CAL_S), .po_scale = 1,
      .modin_voice = 0x0118, .modin_data = 0x0119, .modin_lan = 3,
      .modin_names = MODIN_705, .n_modin_names = 4,
      .memories = true, .n_rx = 1, .max_w = 10 },
    /* 30 kHz-60 MHz, two receivers, ANT1 and ANT2 each with or without the
     * RX ANT input. Its memories are not the IC-705's groups; the swipe
     * chooses the receiver and the antenna instead. */
    { .name = "IC-7610", .f_min = 30000, .f_max = 60000000,
      .preamps = 2,
      .cal_s = CAL_S_7610, .n_cal_s = NCAL(CAL_S_7610), .po_scale = 10,
      .modin_voice = 0x0091, .modin_data = 0x0092, .modin_lan = 5,
      .modin_names = MODIN_7610, .n_modin_names = 6,
      .n_rx = 2, .n_ant = 2, .rx_ant = true, .max_w = 100, .tuner = true },
    /* The IC-7760: 30 kHz-60 MHz and 200 W, two receivers -- MAIN and SUB as
     * on the IC-7610, whose commands, unprefixed, and S-meter it has; its Po
     * about the IC-705's table twenty times over (exact to 100 W and at
     * 200 W). From wfview's rig file alone, not yet tried on one: its
     * modulation inputs are only read (1A 05 01 29 and 01 30) -- an over goes
     * out on whatever input it is set to, so DATA OFF MOD and DATA1 MOD to LAN
     * on the radio for the knob's microphone -- and its four antennas, their
     * RX ANT input and its tuner are left alone until they have been: they
     * switch relays. Its memories, as the IC-7610's, are not read. */
    { .name = "IC-7760", .f_min = 30000, .f_max = 60000000,
      .preamps = 2,
      .cal_s = CAL_S_7610, .n_cal_s = NCAL(CAL_S_7610), .po_scale = 20,
      .probe_voice = 0x0129, .probe_data = 0x0130,
      .modin_names = MODIN_7760, .n_modin_names = 10,
      .n_rx = 2, .max_w = 200 },
    /* The IC-R8600, a receiver: 10 kHz-3 GHz, one preamp, ANT1-3. Its
     * memories are not the IC-705's groups; it echoes every command it is
     * sent, which civ_frame() leaves alone like any echo. */
    { .name = "IC-R8600", .f_min = 10000, .f_max = 3000000000LL,
      .preamps = 1,
      .cal_s = CAL_S, .n_cal_s = NCAL(CAL_S), .po_scale = 1,
      .n_rx = 1, .n_ant = 3, .rx_only = true, .squelch = true },
    /* The IC-9700: 2 m, 70 cm and 23 cm, an antenna for each -- nothing to
     * choose -- and one preamp. 100 W on 2 m, 75 W on 70 cm, 10 W on 23 cm:
     * POWER and the meter read in the band's watts. Its modulation inputs
     * are the IC-7610's under other numbers (wfview's rig file; read back
     * from the radio: MIC). Its memories are a group per band. It refuses
     * 07 D2, so the dial works whichever band the radio has selected, MAIN
     * or SUB. */
    { .name = "IC-9700", .f_min = 144000000, .f_max = 1300000000,
      .preamps = 1,
      .cal_s = CAL_S, .n_cal_s = NCAL(CAL_S), .po_scale = 10,
      .modin_voice = 0x0115, .modin_data = 0x0116, .modin_lan = 5,
      .modin_names = MODIN_7610, .n_modin_names = 6,
      .memories = true, .mem_by_band = true,
      .n_rx = 1, .max_w = 100, .max_w_uhf = 75, .max_w_shf = 10 },
    /* The IC-7300MK2: 30 kHz-74.8 MHz -- HF, 6 m and 4 m -- 100 W, one
     * receiver, P.AMP1 and P.AMP2. Its S-meter is the IC-705's and its Po the
     * IC-705's ten times over (wfview's rig file; 4 m may be 50 W on some
     * versions, unconfirmed). Not yet tried on one, so nothing past the
     * dial's own is switched: not its modulation inputs (wfview's 1A 05 00 84
     * and 85, only read: overs go out on whatever input it is set to), nor
     * its tuner, squelch or RX ANT. Its memories, channels 1-99 with no
     * group, are a layout on_memory() does not read. Named in full, its
     * modulation-input numbers are read (wfview's, logged against the radio's
     * menu); matched only as "IC-7300" -- an IC-7300 behind wfview's server,
     * where those numbers are other settings -- they are not. */
    { .name = "IC-7300MK2", .f_min = 30000, .f_max = 74800000,
      .preamps = 2,
      .cal_s = CAL_S, .n_cal_s = NCAL(CAL_S), .po_scale = 10,
      .probe_voice = 0x0084, .probe_data = 0x0085,
      .modin_names = MODIN_7300MK2, .n_modin_names = 6,
      .n_rx = 1, .max_w = 100 },
    { .name = "IC-7300", .f_min = 30000, .f_max = 74800000,
      .preamps = 2,
      .cal_s = CAL_S, .n_cal_s = NCAL(CAL_S), .po_scale = 10,
      .n_rx = 1, .max_w = 100 },
    /* The IC-905: 2 m, 70 cm and 23 cm at 10 W, 13 cm and 6 cm at 2 W, 3 cm
     * (10 GHz, with its CX-10G) at 0.5 W -- POWER and the meter in the band's
     * watts, the 0.5 W as %. From 10 GHz a frequency takes six bytes (freq6,
     * from wfview). One receiver, one preamp, no antenna to choose, no tuner,
     * no RIT. Not yet tried on one: its memories (a 69-byte layout of its
     * own) are not read, and its modulation inputs (wfview's 1A 05 01 26 /
     * 01 27) are only read -- DATA OFF MOD and DATA MOD to LAN on the radio
     * for the knob's microphone. */
    { .name = "IC-905", .f_min = 144000000, .f_max = 10500000000LL,
      .preamps = 1,
      .cal_s = CAL_S_905, .n_cal_s = NCAL(CAL_S_905), .po_scale = 1,
      .freq6 = true, .no_rit = true,
      .probe_voice = 0x0126, .probe_data = 0x0127,
      .modin_names = MODIN_905, .n_modin_names = 4,
      .n_rx = 1, .max_w = 10, .max_w_2g = 2, .max_mw_10g = 500 },
    /* The X6100 and X6200: 0.5-54 MHz behind an IC-705's CI-V, one preamp.
     * Their server has neither the memories nor the modulation inputs. */
    { .name = "X6", .f_min = 500000, .f_max = 54000000,
      .preamps = 1,
      .cal_s = CAL_S, .n_cal_s = NCAL(CAL_S), .po_scale = 1,
      .n_rx = 1, .max_w = 10 },
};

static const model_t MODEL_OTHER = {
    .name = "", .f_min = 30000, .f_max = 470000000,
    .preamps = 2, .one_preamp_hz = 100000000,
    .cal_s = CAL_S, .n_cal_s = NCAL(CAL_S), .po_scale = 1,
    .n_rx = 1,
};

/* The radio's model, as everything reads it: MODEL_OTHER until
 * radio_start() sets one. The dial and the supervisor ask for the status from
 * boot, before that -- and when the radio cannot be found it never runs.
 * Reading a NULL model crashed the knob at every boot with its radio off, in
 * the status and in the PTT permit it reports; turning the dial would have
 * too. */
static const model_t *model_now(void)
{
    const model_t *m = S.model;
    return m ? m : &MODEL_OTHER;
}

/* RF power's full scale on the band the radio is on, in watts. */
static float po_w_at(const model_t *m, int64_t f)
{
    if (m->max_mw_10g && f >= 10000000000LL) return m->max_mw_10g / 1000.0f;
    if (m->max_w_2g && f >= 2000000000LL)    return m->max_w_2g;
    if (m->max_w_shf && f >= 1000000000LL)   return m->max_w_shf;
    if (m->max_w_uhf && f >= 400000000LL)    return m->max_w_uhf;
    return m->max_w;
}

/* ...as the dial shows it: whole watts, 0 = in % (the IC-905's 0.5 W). */
static uint16_t max_w_at(const model_t *m, int64_t f) { return (uint16_t)po_w_at(m, f); }

/* The Po meter, times CAL_PO's 10 W: the band's full scale where it varies. */
static float po_scale_now(void)
{
    const model_t *m = model_now();
    return (m->max_w_uhf || m->max_w_shf || m->max_w_2g || m->max_mw_10g)
           ? po_w_at(m, S.f_server) / 10.0f : m->po_scale;
}

static const model_t *model_for(const char *name)
{
    for (size_t i = 0; i < sizeof MODELS / sizeof MODELS[0]; i++)
        if (strncmp(name, MODELS[i].name, strlen(MODELS[i].name)) == 0) return &MODELS[i];
    return &MODEL_OTHER;
}

/* ------------------------------------------------------------ modes */

typedef struct { const char *name; uint8_t mode, data; } mode_map_t;
/* Names are the editors' (radio.h). The IC-705 has no synchronous AM or
 * narrow-FM mode of its own: those are its AM and FM. */
static const mode_map_t MODES[] = {
    { "lsb", 0x00, 0 }, { "usb", 0x01, 0 }, { "am",  0x02, 0 }, { "sam", 0x02, 0 },
    { "cw",  0x03, 0 }, { "rtty", 0x04, 0 }, { "fm", 0x05, 0 }, { "nfm", 0x05, 0 },
    { "wfm", 0x06, 0 }, { "cwr", 0x07, 0 }, { "rttyr", 0x08, 0 }, { "dv", 0x17, 0 },
    { "digl", 0x00, 1 }, { "digu", 0x01, 1 },
    /* The IC-R8600's synchronous AM, as it reports it; "sam" set on the
     * others is their AM, the entry above. */
    { "sam", 0x11, 0 },
    /* The IC-905's and IC-9700's digital data and ATV: named on the glass,
     * not offered by it. */
    { "dd", 0x22, 0 }, { "atv", 0x23, 0 },
};

static const char *mode_name(uint8_t mode, uint8_t data)
{
    for (size_t i = 0; i < sizeof MODES / sizeof MODES[0]; i++)
        if (MODES[i].mode == mode && MODES[i].data == data) return MODES[i].name;
    return "?";
}

/* Filter width index <-> Hz (the IC-705's 1A 03): SSB, CW and RTTY run
 * 50-500 Hz in 50 Hz steps, then 600-3600 Hz in 100; AM 200-10000 in 200. */
static int32_t width_hz(uint8_t mode, uint8_t idx)
{
    if (mode == 0x02) return 200 * (idx + 1);
    if (mode == 0x05 || mode == 0x06) return 0;
    return idx <= 9 ? 50 * (idx + 1) : 600 + 100 * (idx - 10);
}

static uint8_t width_idx(uint8_t mode, int32_t hz)
{
    if (mode == 0x02) return (uint8_t)MIN(49, MAX(0, hz / 200 - 1));
    if (hz <= 500)    return (uint8_t)MAX(0, hz / 50 - 1);
    return (uint8_t)MIN(40, 10 + (hz - 600 + 50) / 100);
}

/* The glass shows the passband as edges, the way TCI reports it; the radio
 * only knows a width. Place it where an operator would expect to read it. */
static void set_edges(void)
{
    int32_t w = width_hz(S.mode_byte, S.width_idx);
    if (S.mode_byte == 0x05) w = S.filter_no == 1 ? 15000 : S.filter_no == 2 ? 10000 : 7000;
    switch (S.mode_byte) {
    case 0x00: S.filt_lo = -(w + 100); S.filt_hi = -100;    break;   /* LSB */
    case 0x01: S.filt_lo = 100;        S.filt_hi = 100 + w; break;   /* USB */
    default:   S.filt_lo = -w / 2;     S.filt_hi = w / 2;   break;
    }
}

/* ------------------------------------------------------------- PTT glue */

static void log_refusal(uint32_t missing)
{
    if (!missing) { ESP_LOGW(TAG, "PTT REFUSED by the radio, or not confirmed in time"); return; }
    ESP_LOGW(TAG, "PTT REFUSED, not ready: 0x%03lx", (unsigned long)missing);
}

static void session_end(bool polite, const char *why);

/* The knob's microphone reaches the transmitter only with the radio's
 * modulation input on WLAN. That is switched for our overs alone -- as
 * AetherSDR switches its source to TCI for a knob's -- so the radio's own
 * microphone still works whenever the radio is keyed at the radio. */
static const char *modin_name(uint8_t v)
{
    const model_t *m = model_now();
    return v < m->n_modin_names ? m->modin_names[v] : "?";
}

/* 1A 05 <hi> <lo>: read one of the radio's modulation inputs; with a value,
 * set it. */
static void modin_read(uint16_t which)
{
    CIV(0x1A, 0x05, (uint8_t)(which >> 8), (uint8_t)which);
}

static void modin_set(uint16_t which, uint8_t v)
{
    CIV(0x1A, 0x05, (uint8_t)(which >> 8), (uint8_t)which, v);
}

static void modin_to_wlan(void)
{
    const model_t *m = model_now();
    if (C.modin_switched || C.no_modin || !m->modin_voice) return;
    if (C.modin_off == 0xFF || C.modin_d1 == 0xFF) {
        ESP_LOGW(TAG, "modulation inputs not known: this over is the radio's own microphone");
        return;
    }
    if (C.modin_off == m->modin_lan && C.modin_d1 == m->modin_lan) return;
    ESP_LOGI(TAG, "modulation input %s for this over (was %s, data %s)",
             modin_name(m->modin_lan), modin_name(C.modin_off), modin_name(C.modin_d1));
    s_modin_saved.off = C.modin_off;
    s_modin_saved.d1 = C.modin_d1;
    s_modin_saved.voice = m->modin_voice;
    s_modin_saved.data = m->modin_data;
    s_modin_saved.magic = MODIN_MAGIC;
    modin_set(m->modin_voice, m->modin_lan);
    modin_set(m->modin_data, m->modin_lan);
    C.modin_switched = true;
}

static void modin_restore(void)
{
    if (!C.modin_switched) return;
    modin_set(model_now()->modin_voice, C.modin_off);
    modin_set(model_now()->modin_data, C.modin_d1);
    C.modin_switched = false;
    s_modin_saved.magic = 0;
}

static void ptt_dispatch(const ptt_out_t *o)
{
    if (o->haptic) haptic_hook(o->haptic, o->haptic_prio);
    if (o->send_key)   { modin_to_wlan(); CIV(0x1C, 0x00, 0x01); }
    if (o->send_unkey) { CIV(0x1C, 0x00, 0x00); modin_restore(); }
    /* Rungs 2 and 3: the IC-705 stops transmitting when the client that keyed
     * it over the LAN goes away, so ending the session is the stronger unkey. */
    if (o->close_socket) {
        ESP_LOGW(TAG, "PTT ladder rung 2: logging out");
        session_end(true, "PTT ladder");
    }
    if (o->destroy_socket) {
        ESP_LOGE(TAG, "PTT ladder rung 3: dropping the session");
        session_end(false, "PTT ladder");
    }
    if (o->restart) {
        ESP_LOGE(TAG, "PTT ladder rung 4: rebooting to guarantee an unkey");
        esp_restart();
    }
    if (o->entered_tx) {
        audio_in_set_active(true);
        C.over_frames = C.over_silent = 0;
        C.over_peak = 0;
        ESP_LOGW(TAG, "*** TX ***");
    }
    if (o->left_tx && C.over_frames) {
        ESP_LOGI(TAG, "over: %u audio frames, %u of them silence, microphone peak %d dBFS",
                 C.over_frames, C.over_silent,
                 C.over_peak ? (int)(20.0f * log10f((float)C.over_peak / 32767.0f)) : -99);
    }
    if (o->left_tx)    { audio_in_set_active(false); ESP_LOGI(TAG, "*** RX ***");
                         modin_restore(); }
    if (o->refused)    { log_refusal(o->missing); modin_restore(); }
}

static uint32_t ptt_permit_now(uint32_t t)
{
    uint32_t p = PERMIT_BAND | PERMIT_MODE | PERMIT_NO_OVERLAY | PERMIT_NO_FAULT |
                 PERMIT_TX_ENABLE;
    /* A second receiver only listens: an IC-7610 on its SUB transmits on the
     * MAIN's frequency, which the dial is not showing. */
    if (!model_now()->rx_only && (model_now()->n_rx < 2 || (S.have_rx && S.rx == 0)))
        p |= PERMIT_TRX;
    if ((S.link == RADIO_LINK_READY || S.link == RADIO_LINK_DEGRADED) &&
        t - S.t_ready_ms >= 500) p |= PERMIT_LINK;
    /* Liveness means CI-V answering, not just control pings: a session can
     * keep its pings while the radio has closed its streams, and a key sent
     * into that goes nowhere. */
    if (s_ctl.fd >= 0 && t - s_ctl.t_rx < ALIVE_MS && C.civ_open &&
        t - C.t_civ_rx < CIV_FRESH_MS) p |= PERMIT_PONG_FRESH;
    if (t - S.t_last_input_ms >= QUIET_MS) p |= PERMIT_NO_RECONCILE;
    return p;
}

/* ------------------------------------------------------------ CI-V input */

static void on_freq(int64_t f, uint32_t t)
{
    taskENTER_CRITICAL(&S_LOCK);
    S.f_server = f;
    if (!S.have_freq) {
        /* Authoritative on every (re)connect. */
        tune_assign(&S.tune, f);
        S.f_committed = f;
        S.have_freq = true;
    } else if (t - S.t_last_input_ms >= QUIET_MS && t - S.t_last_send_ms >= QUIET_MS &&
               f != S.tune.f_display) {
        /* Tuned on the radio itself, or a set the radio corrected. */
        tune_assign(&S.tune, f);
        S.f_committed = f;
        S.reconciles++;
        /* In memory mode that is a channel chosen on the radio's own dial:
         * find it by its frequency, so the name follows. */
        if (S.mem_mode && s_mem && s_mem[S.mem_ch].hz != f)
            for (int i = 0; i < MEM_CHANNELS; i++)
                if (s_mem[i].used && s_mem[i].hz == f) {
                    S.mem_ch   = (uint8_t)i;
                    C.mem_sent = (int16_t)i;
                    break;
                }
    } else if (f == S.f_committed) {
        S.echoes++;
    }
    taskEXIT_CRITICAL(&S_LOCK);
}

static void on_memory(const uint8_t *p, size_t n);
static void mem_nvs(bool save);

/* The radio works its other receiver now, chosen on the dial or on the radio:
 * all the dial shows is that one's, the frequency first -- the radio's to say
 * again, as on a new session. */
static void rx_changed(void)
{
    taskENTER_CRITICAL(&S_LOCK);
    S.have_freq = false;
    S.have_ant = false;
    taskEXIT_CRITICAL(&S_LOCK);
    CIV(0x03);
    CIV(0x26, 0x00);
    if (!S.mem_mode) CIV(0x1A, 0x03);
    CIV(0x16, 0x12);
    CIV(0x16, 0x02);
    CIV(0x21, 0x00);
    if (model_now()->n_ant) CIV(0x12);
}

static void civ_frame(const uint8_t *f, size_t n, uint32_t t)
{
    /* FE FE <to> <from> <cmd> [sub] <data> FD */
    if (n < 6 || f[0] != 0xFE || f[1] != 0xFE || f[n - 1] != 0xFD) return;
    if (f[3] != C.civ_addr) return;                  /* our own, echoed */
    if (f[2] != CIV_US && f[2] != 0x00) return;      /* someone else's */
    const uint8_t  cmd = f[4];
    const uint8_t *b   = f + 5;
    const size_t   bn  = n - 6;
    C.t_civ_rx = t;
    uint8_t asked = 0, asked_sub = 0xFF, asked_len = 0;
    /* A frame prefixed 29 <receiver> (the IC-7760's kind, should it report
     * that way) answers nothing we asked: popping for it would drain the
     * queue, and later refusals would go to the wrong command. */
    const bool answer = f[2] == CIV_US && cmd != 0x29 &&
                        await_pop(cmd, &asked, &asked_sub, &asked_len);

    switch (cmd) {
    case 0x00: case 0x03:                            /* frequency */
        if (bn >= 5) {
            on_freq(freq_from_n(b, model_now()->freq6 && bn >= 6 ? 6 : 5), t);
            if (S.link == RADIO_LINK_GREETING && S.have_freq) {
                S.link = RADIO_LINK_READY;
                S.t_ready_ms = t;
                ESP_LOGI(TAG, "ready: %s, %lld Hz", C.radio_name, (long long)S.f_server);
            }
        }
        return;
    case 0x01: case 0x04:                            /* mode, filter */
        if (bn >= 1) {
            S.mode_byte = b[0];
            if (bn >= 2) S.filter_no = b[1];
            /* The transceive report carries no data flag: ask for the full one.
             * And the radio keeps an AGC setting per mode. On a memory channel
             * the radio refuses a width read. */
            CIV(0x26, 0x00);
            if (!S.mem_mode) CIV(0x1A, 0x03);
            CIV(0x16, 0x12);
        }
        return;
    case 0x26:                                       /* 26 00: mode data filter */
        if (bn >= 4 && b[0] == 0x00) {
            S.mode_byte = b[1];
            S.data_mode = b[2];
            S.filter_no = b[3];
            strlcpy(S.mode, mode_name(S.mode_byte, S.data_mode), sizeof S.mode);
            set_edges();
        }
        return;
    case 0x1A:
        if (bn >= 2 && b[0] == 0x03) {               /* filter width */
            S.width_idx = (uint8_t)unbcd(b[1]);
            set_edges();
        } else if (b[0] == 0x05 && bn < 4) {
            /* The X6100's server answers 1A 05 01 18 with 1A 05 00: no
             * such setting. Stop asking, and stop warning about it. */
            if (!C.no_modin)
                ESP_LOGW(TAG, "the radio has no modulation-input setting over CI-V: "
                              "overs go out on whatever input it is set to");
            C.no_modin = true;
            if (model_now()->probe_voice && !C.no_probe)
                ESP_LOGW(TAG, "%s answers wfview's modulation-input numbers short: "
                              "they are not its own", C.radio_name);
            C.no_probe = true;
        } else if (bn >= 4 && b[0] == 0x05 && model_now()->modin_voice && !C.modin_switched) {
            const uint16_t which = (uint16_t)(b[1] << 8 | b[2]);
            if (which == model_now()->modin_voice) C.modin_off = b[3];   /* modulation inputs */
            if (which == model_now()->modin_data)  C.modin_d1 = b[3];
            if (!C.modin_logged && C.modin_off != 0xFF && C.modin_d1 != 0xFF) {
                C.modin_logged = true;
                ESP_LOGI(TAG, "modulation inputs: %s, data %s",
                         modin_name(C.modin_off), modin_name(C.modin_d1));
            }
        } else if (bn >= 4 && b[0] == 0x05 && model_now()->probe_voice) {
            /* wfview's numbers on a radio nobody here has tried: what they
             * hold, for its owner to hold against the radio's own menu. */
            const model_t *m = model_now();
            const uint16_t which = (uint16_t)(b[1] << 8 | b[2]);
            if (which == m->probe_voice) C.modin_off = b[3];
            if (which == m->probe_data)  C.modin_d1 = b[3];
            if (!C.modin_logged && C.modin_off != 0xFF && C.modin_d1 != 0xFF) {
                C.modin_logged = true;
                ESP_LOGW(TAG, "%s: DATA OFF MOD reads %02X (%s), DATA MOD %02X (%s) -- "
                              "1A 05 %04X and %04X, wfview's numbers, read only: "
                              "the knob switches neither",
                         C.radio_name, (unsigned)C.modin_off, modin_name(C.modin_off),
                         (unsigned)C.modin_d1, modin_name(C.modin_d1),
                         (unsigned)m->probe_voice, (unsigned)m->probe_data);
            }
        } else if (bn >= 5 && b[0] == 0x00) {        /* a memory channel */
            on_memory(b + 1, bn - 1);
        }
        return;
    case 0x15:                                       /* meters */
        if (bn < 3) return;
        {
            unsigned raw = level_from(b + 1);
            switch (b[0]) {
            case 0x02: S.smeter_dbm = -73.0f + calibrate(model_now()->cal_s, model_now()->n_cal_s, raw);
                       break;
            case 0x11: S.tx_fwd_w = CAL(CAL_PO, raw) * po_scale_now();
                       if (S.tx_fwd_w > S.tx_peak_w || t - C.t_poll_tx > 400)
                           S.tx_peak_w = S.tx_fwd_w;
                       break;
            case 0x12: S.tx_swr = CAL(CAL_SWR, raw); break;
            case 0x13: S.tx_alc = raw / 120.0f;      break;
            }
        }
        return;
    case 0x1C:                                       /* 1C 00: transmitting? */
        if (bn >= 2 && b[0] == 0x00) {
            bool on = b[1] == 0x01;
            ptt_out_t o;
            if (on != S.tx) ESP_LOGI(TAG, "radio reports %s", on ? "TX" : "RX");
            S.tx = on;
            ptt_fsm_event(&S.ptt, on ? PTT_EV_CONFIRM_TRUE : PTT_EV_CONFIRM_FALSE,
                          t, ptt_permit_now(t), &o);
            ptt_dispatch(&o);
        } else if (bn >= 2 && b[0] == 0x01 && b[1] <= 2) {  /* 1C 01: the tuner */
            S.tuner = b[1];
            S.have_tuner = true;
        }
        return;
    case 0x16:                                       /* functions */
        if (bn >= 2 && b[0] == 0x02 && b[1] <= 2) {  /* preamp */
            S.preamp = b[1];
            S.have_preamp = true;
        } else if (bn >= 2 && b[0] == 0x12) {        /* AGC time constant */
            static const char *AGC[] = { "", "fast", "mid", "slow" };
            strlcpy(S.agc, b[1] <= 3 ? AGC[b[1]] : "", sizeof S.agc);
        }
        return;
    case 0x07:                                       /* 07 D2: MAIN or SUB */
        if (bn >= 2 && b[0] == 0xD2 && b[1] <= 1 && model_now()->n_rx > 1) {
            const bool first = !S.have_rx;
            if (first || b[1] != S.rx) {
                if (!first) ESP_LOGI(TAG, "on the %s receiver", b[1] ? "SUB" : "MAIN");
                taskENTER_CRITICAL(&S_LOCK);
                S.rx = b[1];
                S.have_rx = true;
                taskEXIT_CRITICAL(&S_LOCK);
                if (!first) rx_changed();
            }
        }
        return;
    case 0x12:                                       /* 12 <ant> <RX ANT>: antenna */
        if (bn >= 1 && model_now()->n_ant && unbcd(b[0]) < model_now()->n_ant) {
            taskENTER_CRITICAL(&S_LOCK);
            S.ant = (uint8_t)unbcd(b[0]);
            S.ant_rx = model_now()->rx_ant && bn >= 2 && b[1] == 0x01;
            S.have_ant = true;
            taskEXIT_CRITICAL(&S_LOCK);
        }
        return;
    case 0x21:                                       /* RIT */
        if (bn >= 4 && b[0] == 0x00) {
            int32_t hz = (int32_t)(unbcd(b[1]) + 100 * unbcd(b[2]));
            S.rit_hz = b[3] ? -hz : hz;
        }
        return;
    case 0xFB:                                       /* OK */
        return;
    case 0xFA:                                       /* NG */
        S.rejects++;
        if (answer) {
            log_refused(asked, asked_sub);
            on_refused(asked, asked_sub, asked_len);
        }
        return;
    case 0x14:                                       /* levels: 0000-0255 */
        if (bn >= 3 && (b[0] == 0x02 || b[0] == 0x03 || b[0] == 0x0A)) {
            const unsigned v = unbcd(b[1]) * 100 + unbcd(b[2]);
            const uint8_t level = v > 255 ? 255 : (uint8_t)v;
            if (b[0] == 0x02)      { S.rf_gain  = level; S.have_rf_gain  = true; }
            else if (b[0] == 0x03) { S.squelch  = level; S.have_squelch  = true; }
            else                   { S.rf_power = level; S.have_rf_power = true; }
        }
        return;
    default:
        S.unknown_cmds++;
        return;
    }
}

/* ---------------------------------------------------------- the session */

static void send_auth(uint8_t *b, size_t size, uint8_t reqtype)
{
    hdr(&s_ctl, b, size, 0x00, 0);
    put32be(b + 0x10, (uint32_t)size - 0x10);
    b[0x14] = 0x01;
    b[0x15] = reqtype;
    put16be(b + 0x16, C.auth_seq++);
    put16le(b + 0x1a, C.tokreq);
    put32le(b + 0x1c, C.token);
}

static void send_login(void)
{
    uint8_t b[PKT_LOGIN] = { 0 };
    C.tokreq = (uint16_t)(esp_random() | 1);
    send_auth(b, sizeof b, 0x00);
    scramble(C.user, b + 0x40);
    scramble(C.pass, b + 0x50);
    memcpy(b + 0x60, OUR_NAME, strlen(OUR_NAME));
    tracked(&s_ctl, b, sizeof b);
}

static void send_token(uint8_t what)
{
    uint8_t b[PKT_TOKEN] = { 0 };
    send_auth(b, sizeof b, what);
    put16be(b + 0x24, 0x0798);
    tracked(&s_ctl, b, sizeof b);
}

static void send_stream_request(void)
{
    uint8_t b[PKT_CONNINFO] = { 0 };
    send_auth(b, sizeof b, 0x03);
    if (C.commoncap == 0x8010) {
        put16le(b + 0x27, 0x8010);
        memcpy(b + 0x2a, C.mac, 6);
    } else {
        memcpy(b + 0x20, C.guid, 16);
    }
    memcpy(b + 0x40, C.radio_name, strnlen(C.radio_name, 32));
    scramble(C.user, b + 0x60);
    b[0x70] = 1;                            /* receive audio */
    b[0x71] = 1;                            /* transmit audio */
    b[0x72] = AUDIO_CODEC;
    b[0x73] = AUDIO_CODEC;
    put32be(b + 0x74, AUDIO_RATE_HZ);
    put32be(b + 0x78, TX_AUDIO_RATE_HZ);
    put32be(b + 0x7c, s_civ.lport);
    put32be(b + 0x80, s_aud.lport);
    put32be(b + 0x84, TX_LATENCY_MS);
    b[0x88] = 1;
    tracked(&s_ctl, b, sizeof b);
    ESP_LOGI(TAG, "asking for CI-V and audio (local ports %u/%u)",
             (unsigned)s_civ.lport, (unsigned)s_aud.lport);
}

static void set_close_reason(const char *why)
{
    taskENTER_CRITICAL(&S_LOCK);
    strlcpy(S.last_close, why, sizeof S.last_close);
    taskEXIT_CRITICAL(&S_LOCK);
}

/* Leave. Politely -- token dropped, streams told -- whenever the radio can
 * still hear us, so it frees the session at once instead of holding it busy
 * for the next connection, which is ours. */
static void session_end(bool polite, const char *why)
{
    if (polite) {
        modin_restore();
        if (C.civ_open) civ_openclose(false);
        if (C.authed) send_token(0x01);
        control(&s_civ, 0x05, 0);
        control(&s_aud, 0x05, 0);
        control(&s_ctl, 0x05, 0);
    }
    stream_close(&s_civ);
    stream_close(&s_aud);
    stream_close(&s_ctl);
    C.authed = C.streaming = C.civ_open = false;
    audio_in_set_active(false);

    ptt_out_t o;
    uint32_t t = now_ms();
    if (S.ptt.state != PTT_IDLE) {
        ptt_fsm_abort(&S.ptt, PTT_AB_LINK_DOWN, t, &o);
        ptt_dispatch(&o);
    }
    taskENTER_CRITICAL(&S_LOCK);
    if (S.link != RADIO_LINK_DOWN) S.closes++;
    S.link = RADIO_LINK_DOWN;
    S.have_freq = false;
    S.tx = false;
    taskEXIT_CRITICAL(&S_LOCK);
    set_close_reason(why);
    C.t_retry = t + C.backoff_ms;
    C.backoff_ms = MIN(C.backoff_ms * 2, 16000);
    ESP_LOGW(TAG, "session ended: %s (retry in %lu ms)", why, (unsigned long)C.backoff_ms);
}

static bool session_begin(uint32_t t)
{
    struct in_addr radio;
    if (!inet_aton(C.host, &radio)) {
        set_close_reason("bad radio address");
        C.t_retry = t + 5000;
        return false;
    }
    /* The ids carry our own address, so find which one reaches the radio. */
    int probe = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in pa = { .sin_family = AF_INET, .sin_port = htons(C.port),
                              .sin_addr = radio };
    struct sockaddr_in me = { 0 };
    socklen_t ml = sizeof me;
    if (probe < 0 || connect(probe, (struct sockaddr *)&pa, sizeof pa) < 0 ||
        getsockname(probe, (struct sockaddr *)&me, &ml) < 0) {
        if (probe >= 0) close(probe);
        set_close_reason("no route to the radio");
        C.t_retry = t + 2000;
        return false;
    }
    close(probe);
    const uint32_t local = me.sin_addr.s_addr;

    if (stream_open(&s_ctl, local) || stream_open(&s_civ, local) ||
        stream_open(&s_aud, local)) {
        session_end(false, "no sockets");
        return false;
    }
    stream_connect(&s_ctl, radio.s_addr, C.port);
    s_ctl.idles = true;
    s_civ.idles = true;
    s_aud.idles = false;
    C.auth_seq = 0x30;
    C.token = 0;
    C.civ_seqb = C.aud_seqb = 0;
    C.authed = C.streaming = C.civ_open = false;
    C.modin_off = C.modin_d1 = 0xFF;
    C.modin_switched = C.modin_logged = false;
    C.civ_addr = CIV_RADIO;
    C.no_mem = C.no_modin = C.no_probe = C.probing = C.aud_quiet = false;
    C.waiting_busy = false;
    C.t_session = t;
    taskENTER_CRITICAL(&S_LOCK);
    S.link = RADIO_LINK_CONNECTING;
    S.have_freq = false;
    taskEXIT_CRITICAL(&S_LOCK);
    ESP_LOGI(TAG, "connecting to %s:%u", C.host, (unsigned)C.port);
    return true;
}

static void on_control(const uint8_t *d, int n, uint32_t t)
{
    const uint16_t type = get16le(d + 4);
    if (n == PKT_CONTROL && type == 0x06) {                 /* I am ready */
        taskENTER_CRITICAL(&S_LOCK);
        S.link = RADIO_LINK_GREETING;
        taskEXIT_CRITICAL(&S_LOCK);
        send_login();
        return;
    }
    if (n == PKT_LOGIN_RSP && type != 0x01) {
        uint32_t err = get32le(d + 0x30);
        if (err == 0xFEFFFFFF) {
            ESP_LOGE(TAG, "the radio refused the user name or password");
            C.backoff_ms = 30000;
            session_end(true, "wrong user name or password");
            return;
        }
        if (get16le(d + 0x1a) == C.tokreq && !C.authed) {
            C.token = get32le(d + 0x1c);
            C.authed = true;
            send_token(0x02);
            C.t_token = t;
        }
        return;
    }
    if (n == PKT_TOKEN && type != 0x01) {
        if (d[0x15] == 0x05 && d[0x14] == 0x02 && get32le(d + 0x30) == 0xFFFFFFFF) {
            ESP_LOGW(TAG, "token renewal refused; asking for the streams again");
            C.streaming = false;
            send_stream_request();
        }
        return;
    }
    if (n >= PKT_CAPS + PKT_RADIO_CAP && (n - PKT_CAPS) % PKT_RADIO_CAP == 0) {
        const uint8_t *c = d + PKT_CAPS;            /* the first radio */
        memcpy(C.guid, c, 16);
        C.commoncap = get16le(c + 7);
        memcpy(C.mac, c + 10, 6);
        memset(C.radio_name, 0, sizeof C.radio_name);
        memcpy(C.radio_name, c + 0x10, 31);
        C.civ_addr = c[0x52] ? c[0x52] : CIV_RADIO;
        const model_t *m = model_for(C.radio_name);
        ESP_LOGI(TAG, "radio: %s, CI-V 0x%02x, audio %02x%02x rx, %02x%02x tx%s",
                 C.radio_name, c[0x52], c[0x53], c[0x54], c[0x55], c[0x56],
                 !m->name[0] ? " -- not one this knob knows: receive and "
                               "tuning only, overs on its own input"
                 : !m->rx_only && !m->modin_voice
                             ? " -- its modulation input is not switched: overs carry "
                               "whatever input the radio has selected" : "");
        taskENTER_CRITICAL(&S_LOCK);
        S.model = m;
        S.have_ant = S.have_rx = false;
        S.pending_rx = S.pending_ant = -1;
        if (!m->memories) {
            /* Not in memory mode on a radio without them, whatever the dial
             * remembers from the last one. */
            S.mem_mode  = false;
            S.mem_state = RADIO_MEM_OFF;
        }
        taskEXIT_CRITICAL(&S_LOCK);
        if (!m->memories) {
            C.no_mem = true;
            C.mem_scan = -1;
        } else if (s_mem && !s_mem_restored) {
            /* Back in memory mode if that is where the dial was, on this
             * radio: the radio is still on the channel, so nothing needs
             * sending. Once per start, under the model's own keys. */
            s_mem_restored = true;
            mem_nvs(false);
            if (S.mem_mode) {
                S.mem_state = RADIO_MEM_READING;
                C.mem_sent  = S.mem_ch;
            }
        }
        if (!m->modin_voice) C.no_modin = true;
        C.probing = m->probe_voice != 0;
        return;
    }
    if (n == PKT_CONNINFO && type != 0x01) {
        uint32_t busy = get32le(d + 0x60);
        char who[17] = { 0 };
        memcpy(who, d + 0x64, 16);
        if (C.streaming) return;
        if (busy && who[0] && strcmp(who, OUR_NAME) != 0) {
            char m[48];
            snprintf(m, sizeof m, "radio in use by %s", who);
            ESP_LOGW(TAG, "%s", m);
            C.backoff_ms = 5000;
            session_end(true, m);
            return;
        }
        if (busy) {
            /* Our own previous session, which the radio has not let go of --
             * a crash or a power loss logs nothing out. It says when it does,
             * with another of these. Asking for streams before then got them
             * granted, and closed again the moment the old session expired:
             * CI-V and audio dead while the pings carried on. */
            if (!C.waiting_busy) {
                ESP_LOGW(TAG, "the radio still holds our previous session; waiting for it");
                set_close_reason("waiting for the radio to drop our last session");
            }
            C.waiting_busy = true;
            return;
        }
        C.waiting_busy = false;
        if (C.radio_name[0]) send_stream_request();
        return;
    }
    if (n == PKT_STATUS && type != 0x01) {
        uint32_t err = get32le(d + 0x30);
        if (err) {
            /* 0xFDFFFFFF, measured: the streams belong to another session,
             * which is also what our own looks like until the radio has
             * noticed it is gone. */
            char m[48];
            snprintf(m, sizeof m, err == 0xFDFFFFFF ? "radio busy with another session"
                                                     : "radio refused the streams (%08lx)",
                     (unsigned long)err);
            C.backoff_ms = MAX(C.backoff_ms, 3000);
            session_end(true, m);
            return;
        }
        if (d[0x40] == 0x01) {
            session_end(true, "the radio disconnected us");
            return;
        }
        uint16_t civ_port = get16be(d + 0x42), aud_port = get16be(d + 0x46);
        if (!C.streaming && civ_port && aud_port) {
            C.streaming = true;
            C.t_aud_rx = t;
            stream_connect(&s_civ, s_ctl.to.sin_addr.s_addr, civ_port);
            stream_connect(&s_aud, s_ctl.to.sin_addr.s_addr, aud_port);
            ESP_LOGI(TAG, "CI-V on %u, audio on %u", (unsigned)civ_port, (unsigned)aud_port);
        }
        return;
    }
}

static void civ_hello(uint32_t t)
{
    /* The radio is authoritative on every (re)connect: nothing left over from
     * before may override what it is about to say. */
    xQueueReset(s_later);
    s_aw_n = 0;                          /* nothing is outstanding on a new stream */
    C.t_mem_req = 0;                     /* a read in flight went with the session */
    C.mem_tries = 0;
    civ_openclose(true);
    C.civ_open = true;
    C.t_civ_open = t;
    C.t_civ_rx = t;
    CIV(0x03);                    /* frequency */
    CIV(0x26, 0x00);              /* mode, data, filter */
    if (!S.mem_mode) CIV(0x1A, 0x03);   /* filter width; refused on a memory */
    const model_t *m = model_now();
    if (!m->rx_only && !m->no_rit) CIV(0x21, 0x00);   /* RIT */
    CIV(0x16, 0x12);              /* AGC */
    CIV(0x16, 0x02);              /* preamp */
    CIV(0x14, 0x02);              /* RF gain */
    if (m->squelch) CIV(0x14, 0x03);    /* squelch */
    if (!m->rx_only) {
        CIV(0x14, 0x0A);          /* RF power */
        CIV(0x1C, 0x00);          /* transmitting? */
    }
    if (m->tuner) CIV(0x1C, 0x01);      /* the tuner in the line? */
    if (m->n_rx > 1) CIV(0x07, 0xD2);   /* MAIN or SUB */
    if (m->n_ant) CIV(0x12);            /* the antenna */
    if (s_modin_saved.magic == MODIN_MAGIC && m->modin_voice &&
        s_modin_saved.voice == m->modin_voice && s_modin_saved.data == m->modin_data) {
        ESP_LOGW(TAG, "putting back the modulation inputs a lost session left on the network");
        modin_set(m->modin_voice, s_modin_saved.off);
        modin_set(m->modin_data, s_modin_saved.d1);
        s_modin_saved.magic = 0;
    }
    if (m->modin_voice && !C.no_modin) {  /* as the operator has them */
        modin_read(m->modin_voice);
        modin_read(m->modin_data);
    }
    if (m->probe_voice && !C.no_probe) {  /* wfview's numbers: read, never set */
        modin_read(m->probe_voice);
        modin_read(m->probe_data);
    }
}

static void on_civ(const uint8_t *d, int n, uint32_t t)
{
    const uint16_t type = get16le(d + 4);
    if (n == PKT_CONTROL && type == 0x06) {                 /* I am ready */
        civ_hello(t);
        return;
    }
    if (n > 0x15 && type != 0x01 && get16le(d + 0x11) + 0x15 == n) {
        const uint8_t *p = d + 0x15, *end = d + n;
        while (p < end) {
            const uint8_t *fd = memchr(p, 0xFD, end - p);
            if (!fd) break;
            civ_frame(p, fd - p + 1, t);
            p = fd + 1;
        }
    }
}

static void on_audio(const uint8_t *d, int n)
{
    const uint16_t type = get16le(d + 4);
    if (n < 0x20 || type == 0x01) return;
    C.t_aud_rx = now_ms();
    if (C.aud_quiet) {
        C.aud_quiet = false;
        ESP_LOGI(TAG, "the radio's audio is back");
    }
    if (S.audio_suspend) return;
    size_t bytes = (size_t)n - PKT_AUDIO_HDR;
    audio_out_feed_pcm16((const int16_t *)(d + PKT_AUDIO_HDR), bytes / 2, 1);
}

/* 20 ms of the microphone, as one audio packet -- or, with `mic` false, 20 ms
 * of silence to keep a receive stream going (AUDIO_BOTH_WAYS). Silence too
 * when the mic has nothing yet: the radio times its playback on the stream,
 * and a gap is a click in the transmitted audio. */
static void send_tx_audio(bool mic)
{
    if (!s_txa || s_aud.fd < 0 || !s_aud.ready) return;
    int16_t *pcm = (int16_t *)(s_txa + PKT_AUDIO_HDR);
    if (!mic) {
        memset(pcm, 0, AUDIO_FRAME * 2);
    } else {
        C.over_frames++;
        if (!audio_in_take(pcm, AUDIO_FRAME)) {
            memset(pcm, 0, AUDIO_FRAME * 2);
            S.txa_skipped++;
            C.over_silent++;
        } else {
            int peak = 0;
            for (int i = 0; i < AUDIO_FRAME; i++) {
                int v = pcm[i] < 0 ? -pcm[i] : pcm[i];
                if (v > peak) peak = v;
            }
            if (peak > C.over_peak) C.over_peak = peak;
            /* The same scale AetherSDR's mic meter uses: dB below full scale. */
            S.tx_mic_dbm = peak ? 20.0f * log10f((float)peak / 32767.0f) : -60.0f;
        }
    }
    const size_t len = PKT_AUDIO_HDR + AUDIO_FRAME * 2;
    hdr(&s_aud, s_txa, len, 0x00, 0);
    put16le(s_txa + 0x10, 0x0080);
    put16be(s_txa + 0x12, C.aud_seqb++);
    put16le(s_txa + 0x14, 0);
    put16be(s_txa + 0x16, AUDIO_FRAME * 2);
    int64_t t0 = esp_timer_get_time();
    const bool ok = tracked(&s_aud, s_txa, len);
    uint32_t us = (uint32_t)(esp_timer_get_time() - t0);
    if (us > S.txa_max_us) S.txa_max_us = us;
    if (ok) S.txa_sent++;
    else    S.txa_failed++;
}

/* -------------------------------------------------------------- memories */

/* The IC-705's memories are groups 00-99 of channels 00-99. 1A 00 <group>
 * <channel> reads one -- frequency, mode, duplex and tone, a 16-character
 * name -- and a blank channel is answered short. The dial works in one group
 * at a time: it reads all of it, one request after another, when the session
 * starts and again each time memory mode is entered, so that channels edited
 * on the radio show. Selecting is 08 A0 <group> and 08 <channel>.
 *
 * The radio cannot be asked which channel, or even which mode, it is on, so
 * the dial keeps its own: the group, the channel and whether it is in memory
 * mode are remembered (NVS) and put back after a restart. */
/* The IC-9700's memory group: the band it is on -- 1 2 m, 2 70 cm, 3 23 cm;
 * 0 off its bands. It has no command to choose another. */
static uint8_t band_group(int64_t f)
{
    if (f >= 144000000LL && f <= 148000000LL)   return 1;
    if (f >= 420000000LL && f <= 450000000LL)   return 2;
    if (f >= 1240000000LL && f <= 1300000000LL) return 3;
    return 0;
}

/* The first channel of a group: the IC-705 counts from 00, the IC-9700 from 01. */
static int16_t mem_first(void) { return model_now()->mem_by_band ? 1 : 0; }

/* The dial's memory state, kept per firmware -- and, for the IC-9700, under
 * keys of its own: its channel and mode are not the IC-705's, and its group
 * is the band it is on. Read once the radio has said which it is. */
static void mem_nvs(bool save)
{
    const bool b = model_now()->mem_by_band;
    const char *k_ch = b ? "bmch" : "mch", *k_mode = b ? "bmmode" : "mmode";
    kv_handle_t h;
    if (kv_open(MEM_NVS_NS, &h) != ESP_OK) return;
    if (save) {
        kv_edit_begin(h);
        if (!b) kv_set_u8(h, "mgrp", C.mem_group);
        kv_set_u8(h, k_ch, S.mem_ch);
        kv_set_u8(h, k_mode, S.mem_mode);
        kv_edit_end(h);
        kv_commit(h);
    } else {
        uint8_t v;
        if (!b && kv_get_u8(h, "mgrp", &v) == ESP_OK && v < 100) C.mem_group = v;
        if (kv_get_u8(h, k_ch, &v) == ESP_OK && v < MEM_CHANNELS) S.mem_ch = v;
        if (kv_get_u8(h, k_mode, &v) == ESP_OK) S.mem_mode = v != 0;
    }
    kv_close(h);
}

static void mem_read_from(int16_t ch)
{
    C.mem_scan  = ch;
    C.mem_tries = 0;
    C.t_mem_req = 0;
}

/* The next programmed channel from `from`, one step in `dir`, wrapping round
 * the group. -1 if there is none. */
static int mem_next_used(int from, int dir)
{
    for (int i = 1; i <= MEM_CHANNELS; i++) {
        const int ch = ((from + dir * i) % MEM_CHANNELS + MEM_CHANNELS) % MEM_CHANNELS;
        if (s_mem[ch].used) return ch;
    }
    return -1;
}

/* Show a channel at once, as tuning does; the radio's report follows. */
static void mem_show(uint8_t ch)
{
    taskENTER_CRITICAL(&S_LOCK);
    S.mem_ch = ch;
    if (s_mem[ch].used) {
        tune_assign(&S.tune, s_mem[ch].hz);
        S.f_committed = s_mem[ch].hz;
    }
    taskEXIT_CRITICAL(&S_LOCK);
}

static void mem_put(uint8_t ch, bool enter, uint32_t t)
{
    if (enter) {
        CIV(0x08);                                   /* memory mode */
        if (!model_now()->mem_by_band)
            CIV(0x08, 0xA0, bcd(C.mem_group));       /* the dial's group */
    }
    CIV(0x08, bcd(ch / 100), bcd(ch % 100));
    C.mem_sent  = ch;
    C.t_mem_sel = t;
    S.t_last_send_ms = t;        /* a report from the channel before is stale */
    CIV(0x03);                                       /* and what it holds */
    CIV(0x26, 0x00);                 /* no width: refused on a memory channel */
}

/* Onto a channel of the group: the one last used there, else the first. */
static void mem_go(uint32_t t)
{
    if (!C.mem_used) {
        S.mem_state = RADIO_MEM_EMPTY;
        return;
    }
    const int ch = s_mem[S.mem_ch].used ? S.mem_ch : mem_next_used(MEM_CHANNELS - 1, 1);
    mem_show((uint8_t)ch);
    S.mem_state = RADIO_MEM_READY;
    mem_put((uint8_t)ch, true, t);
    ESP_LOGI(TAG, "memory mode: %02u/%02u \"%s\"", C.mem_group, ch, s_mem[ch].name);
}

static void mem_next(void)
{
    if (++C.mem_scan < MEM_CHANNELS) {
        C.mem_tries = 0;
        C.t_mem_req = 0;
        return;
    }
    C.mem_scan   = -1;
    C.mem_loaded = true;
    uint8_t used = 0;
    for (int i = 0; i < MEM_CHANNELS; i++) used += s_mem[i].used;
    C.mem_used = used;
    ESP_LOGI(TAG, "memory group %02u: %u channels programmed", C.mem_group, used);
    if (!S.mem_mode) return;
    if (C.mem_select_due) {
        C.mem_select_due = false;
        mem_go(now_ms());
    } else {
        S.mem_state = used ? RADIO_MEM_READY : RADIO_MEM_EMPTY;
    }
}

static void on_memory(const uint8_t *p, size_t n)
{
    /* The IC-705's: a two-byte group, then the channel, the select byte and
     * from 5 on the channel; its name at 99, after a second VFO's. The
     * IC-9700's (wfview's rig file): a one-byte group -- its band -- so all
     * of it one byte earlier, and the name at 51, nothing between. A blank
     * channel is answered with the select byte alone, FF: 4 bytes there. */
    const bool b = model_now()->mem_by_band;
    if (n < (b ? 4u : 5u) || !s_mem) return;
    const uint8_t *q = b ? p - 1 : p;                /* q[k]: the IC-705's offset k */
    const unsigned grp = b ? unbcd(p[0]) : unbcd(p[0]) * 100 + unbcd(p[1]);
    const unsigned ch  = unbcd(q[2]) * 100 + unbcd(q[3]);
    const size_t   len_min = b ? 67 : 115;
    const uint8_t *name = b ? p + 51 : p + 99;
    if (grp != C.mem_group || ch >= MEM_CHANNELS) return;

    mem_t m = { 0 };
    if (n >= len_min && q[4] != 0xFF) {
        /* Offsets from the payload's start: 5 frequency, 13 duplex and tone
         * mode, 15 the tone, 18 the tone squelch's, 25 the offset (100 Hz,
         * least significant first). */
        const uint8_t tm = q[13] & 0x0F;             /* 1 TONE, 2 TSQL */
        const uint8_t *tf = tm == 2 ? q + 18 : q + 15;
        m.used      = true;
        m.hz        = freq_from(q + 5);
        m.duplex    = (q[13] >> 4) == 1 ? -1 : (q[13] >> 4) == 2 ? 1 : 0;
        m.offset_hz = (int32_t)(unbcd(q[25]) + 100 * unbcd(q[26]) + 10000 * unbcd(q[27])) * 100;
        if (tm == 1 || tm == 2)
            m.tone_dhz = (uint16_t)(unbcd(tf[0]) * 10000 + unbcd(tf[1]) * 100 + unbcd(tf[2]));
        int len = 0;
        for (int i = 0; i < 16; i++) {
            const char c = (char)name[i];
            m.name[i] = (c >= 0x20 && c < 0x7F) ? c : ' ';
            if (m.name[i] != ' ') len = i + 1;
        }
        m.name[len] = 0;
        ESP_LOGI(TAG, "memory %02u/%02u: %lld Hz, shift %c%ld Hz, tone %u.%u, \"%s\"",
                 grp, ch, (long long)m.hz, m.duplex < 0 ? '-' : m.duplex > 0 ? '+' : ' ',
                 (long)m.offset_hz, m.tone_dhz / 10, m.tone_dhz % 10, m.name);
    }
    taskENTER_CRITICAL(&S_LOCK);
    s_mem[ch] = m;
    taskEXIT_CRITICAL(&S_LOCK);
    if ((int)ch == C.mem_scan) mem_next();
}

static void mem_set_group(uint8_t g);

static void mem_enter(uint32_t t)
{
    S.mem_mode = true;
    /* The IC-9700's group is the band it is on: another band, another read. */
    const uint8_t g = model_now()->mem_by_band ? band_group(S.f_server) : 0;
    if (g && g != C.mem_group) {
        mem_set_group(g);                   /* reads it, and selects when read */
    } else if (C.mem_loaded) {
        mem_go(t);
    } else {
        S.mem_state = RADIO_MEM_READING;
        C.mem_select_due = true;
    }
    if (C.mem_scan < 0) mem_read_from(mem_first());  /* catch edits on the radio */
    mem_nvs(true);
}

static void mem_leave(void)
{
    S.mem_mode  = false;
    S.mem_state = RADIO_MEM_OFF;
    C.mem_select_due = false;
    C.mem_sent  = -1;
    /* The VFO's frequency is the radio's to say: until it has, the dial is on
     * the channel's, and a turn now must not carry that over to the VFO. */
    taskENTER_CRITICAL(&S_LOCK);
    S.have_freq = false;
    taskEXIT_CRITICAL(&S_LOCK);
    CIV(0x07);                                       /* the VFO */
    CIV(0x0F, 0x10);                                 /* simplex */
    CIV(0x03);
    CIV(0x26, 0x00);
    CIV(0x1A, 0x03);
    CIV(0x16, 0x02);
    CIV(0x16, 0x12);
    ESP_LOGI(TAG, "VFO mode, simplex");
    mem_nvs(true);
}

static void mem_set_group(uint8_t g)
{
    C.mem_group  = g;
    C.mem_loaded = false;
    C.mem_used   = 0;
    taskENTER_CRITICAL(&S_LOCK);
    memset(s_mem, 0, MEM_CHANNELS * sizeof *s_mem);
    S.mem_ch = 0;                          /* the old channel was another group's */
    taskEXIT_CRITICAL(&S_LOCK);
    mem_read_from(mem_first());
    if (S.mem_mode) {
        S.mem_state = RADIO_MEM_READING;
        C.mem_select_due = true;
    }
    mem_nvs(true);
}

static void mem_task(uint32_t t)
{
    if (!s_mem || C.no_mem || !C.civ_open || S.link != RADIO_LINK_READY) return;
    const bool keyed = S.ptt.state != PTT_IDLE || S.tx;

    /* What the other tasks asked for: kept until the radio can hear it, and
     * never acted on mid-over. */
    const int16_t grp = S.pending_group;
    if (grp >= 0 && !keyed) {
        S.pending_group = -1;
        mem_set_group((uint8_t)grp);
    }
    const int8_t want = S.pending_mem;
    if (want >= 0 && !keyed) {
        S.pending_mem = -1;
        if (want) mem_enter(t);
        else      mem_leave();
    }

    /* Read the group, one channel at a time, never while transmitting -- on
     * the IC-9700, the band it is on. */
    if (!C.mem_loaded && C.mem_scan < 0 && t - S.t_ready_ms >= MEM_SETTLE_MS) {
        const uint8_t g = model_now()->mem_by_band ? band_group(S.f_server) : 0;
        if (g) C.mem_group = g;
        if (g || !model_now()->mem_by_band) mem_read_from(mem_first());
    }
    if (C.mem_scan >= 0 && !keyed &&
        (!C.t_mem_req || t - C.t_mem_req >= MEM_REQ_MS)) {
        if (C.t_mem_req && ++C.mem_tries >= 3) {
            ESP_LOGW(TAG, "memory %02u/%02d never answered", C.mem_group, C.mem_scan);
            mem_next();
        }
        if (C.mem_scan >= 0) {
            const unsigned ch = (unsigned)C.mem_scan;
            if (model_now()->mem_by_band)
                CIV(0x1A, 0x00, bcd(C.mem_group), bcd(ch / 100), bcd(ch % 100));
            else
                CIV(0x1A, 0x00, bcd(C.mem_group / 100), bcd(C.mem_group % 100),
                    bcd(ch / 100), bcd(ch % 100));
            C.t_mem_req = t ? t : 1;
        }
    }

    if (!S.mem_mode) return;

    /* The knob's detents: one programmed channel each. */
    taskENTER_CRITICAL(&S_LOCK);
    int32_t steps = S.mem_steps;
    S.mem_steps = 0;
    taskEXIT_CRITICAL(&S_LOCK);
    if (steps && S.mem_state == RADIO_MEM_READY) {
        int ch = S.mem_ch;
        for (; steps > 0 && ch >= 0; steps--) ch = mem_next_used(ch, 1);
        for (; steps < 0 && ch >= 0; steps++) ch = mem_next_used(ch, -1);
        if (ch >= 0) {
            mem_show((uint8_t)ch);
            C.t_mem_dirty = t;
        }
    }
    /* Onto the radio at most every MEM_STEP_MS: a quick turn skips channels
     * rather than queueing them. */
    if (S.mem_state == RADIO_MEM_READY && !keyed && C.mem_sent != S.mem_ch &&
        t - C.t_mem_sel >= MEM_STEP_MS)
        mem_put(S.mem_ch, C.mem_sent < 0, t);

    /* Remembered once it has stopped moving. */
    if (C.t_mem_dirty && t - C.t_mem_dirty > 3000) {
        C.t_mem_dirty = 0;
        mem_nvs(true);
    }
}

/* ----------------------------------------------- receivers and antennas */

/* The receiver and the antenna chosen on the dial: kept until the radio can
 * hear it, and never acted on mid-over -- an antenna relay switched under
 * power is the one thing here that could harm the radio. What the dial then
 * shows is the radio's answer (07 D2, 12). */
static void rx_ant_task(void)
{
    if (!C.civ_open || S.link != RADIO_LINK_READY) return;
    if (S.ptt.state != PTT_IDLE || S.tx) return;
    const model_t *m = model_now();
    const int8_t rx = S.pending_rx;
    if (rx >= 0) {
        S.pending_rx = -1;
        if (rx < m->n_rx) {
            ESP_LOGI(TAG, "to the %s receiver", rx ? "SUB" : "MAIN");
            CIV(0x07, rx ? 0xD1 : 0xD0);
            CIV(0x07, 0xD2);
        }
    }
    const int8_t a = S.pending_ant;
    if (a >= 0) {
        S.pending_ant = -1;
        const uint8_t ant = a & 0x0F;
        const bool    on_rx = (a & 0x10) && m->rx_ant;
        if (ant < m->n_ant) {
            ESP_LOGI(TAG, "to ANT%u%s", ant + 1, on_rx ? "+RX" : "");
            if (m->rx_ant) CIV(0x12, bcd(ant), on_rx ? 0x01 : 0x00);
            else           CIV(0x12, bcd(ant));
            CIV(0x12);
        }
    }
}

/* ------------------------------------------------------------- the task */

static void poll_civ(uint32_t t)
{
    if (!C.civ_open || S.link != RADIO_LINK_READY) {
        if (C.civ_open && t - C.t_civ_open > 1500 && S.link == RADIO_LINK_GREETING) {
            CIV(0x03);                              /* nudge the first report */
            C.t_civ_open = t;
        }
        return;
    }
    const bool keyed = S.ptt.state != PTT_IDLE || S.tx;
    if (keyed) {
        if (t - C.t_poll_tx >= 60) {
            static const uint8_t m[] = { 0x11, 0x12, 0x13 };
            CIV(0x15, m[C.tx_meter_i++ % 3]);
            C.t_poll_tx = t;
        }
        if (t - C.t_poll_ptt >= 150) { CIV(0x1C, 0x00); C.t_poll_ptt = t; }
    } else {
        if (t - C.t_poll_s >= 100) { CIV(0x15, 0x02); C.t_poll_s = t; }
        if (t - C.t_poll_ptt >= 1000 && !model_now()->rx_only) { CIV(0x1C, 0x00); C.t_poll_ptt = t; }
        /* What the operator may change on the radio itself. The preamp is
         * kept per band, so this is also how a band change shows. One
         * question at a time, round about every 2 s: asked all at once, an
         * IC-7610 answered the first seven and let the rest go, every time
         * -- its RF power and its tuner were never known. */
        if (t - C.t_poll_slow >= SLOW_TICK_MS && t - S.t_last_input_ms >= QUIET_MS) {
            const model_t *m = model_now();
            switch (C.slow_i++ % 9) {
            case 0: CIV(0x03); break;
            case 1: CIV(0x26, 0x00); break;
            case 2: CIV(0x16, 0x12); break;
            case 3: CIV(0x16, 0x02); break;
            case 4:
                /* Asked for once at the start, but an answer lost on the WiFi
                 * left them unknown for the whole session -- and without them
                 * no over is switched to WLAN: it goes out on the radio's own
                 * microphone, silent. So ask until they are known. */
                if (m->modin_voice && !C.no_modin && C.modin_off == 0xFF) modin_read(m->modin_voice);
                if (m->modin_voice && !C.no_modin && C.modin_d1 == 0xFF)  modin_read(m->modin_data);
                /* ...and wfview's, where the knob only reads them, the same. */
                if (m->probe_voice && !C.no_probe && C.modin_off == 0xFF) modin_read(m->probe_voice);
                if (m->probe_voice && !C.no_probe && C.modin_d1 == 0xFF)  modin_read(m->probe_data);
                break;
            case 5: if (m->n_rx > 1) CIV(0x07, 0xD2); break;
            case 6: if (m->n_ant) CIV(0x12); break;
            case 7:
                CIV(0x14, 0x02);
                if (!m->rx_only) CIV(0x14, 0x0A);
                if (m->squelch)  CIV(0x14, 0x03);
                break;
            case 8: if (m->tuner) CIV(0x1C, 0x01); break;
            }
            C.t_poll_slow = t;
        }
    }
}

static void tune_out(uint32_t t)
{
    /* In memory mode the knob selects channels (mem_task); a frequency set
     * there would take the radio off the channel. */
    if (S.link != RADIO_LINK_READY || S.ptt.state != PTT_IDLE || S.mem_mode) return;
    bool    fire = false;
    int64_t want = 0;
    taskENTER_CRITICAL(&S_LOCK);
    if (S.have_freq && S.tune.f_display != S.f_committed &&
        t - S.t_last_send_ms >= SET_PERIOD_MS) {
        want = S.tune.f_display;
        S.f_committed = want;
        S.t_last_send_ms = t;
        fire = true;
    }
    taskEXIT_CRITICAL(&S_LOCK);
    if (fire) {
        uint8_t b[7] = { 0x05 };
        civ_send(b, 1 + freq_to(b + 1, want));
    }
}

static void ptt_step(uint32_t t)
{
    ptt_out_t o;
    uint32_t  permit = ptt_permit_now(t);
    if (S.pending_toggle) {
        S.pending_toggle = 0;
        S.pending_key = (S.ptt.state == PTT_IDLE);
        S.pending_unkey = !S.pending_key;
    }
    if (S.pending_key) {
        S.pending_key = 0;
        ptt_fsm_event(&S.ptt, PTT_EV_TAP_KEY, t, permit, &o);
        ptt_dispatch(&o);
        if (o.send_key) C.t_poll_ptt = t - 1000;   /* confirm at once */
    }
    if (S.pending_unkey) {
        S.pending_unkey = 0;
        ptt_fsm_event(&S.ptt, PTT_EV_TAP_UNKEY, t, permit, &o);
        ptt_dispatch(&o);
        C.t_poll_ptt = t - 1000;
    }
    if (S.pending_abort) {
        uint8_t r = S.pending_abort;
        S.pending_abort = 0;
        ptt_fsm_abort(&S.ptt, (ptt_abort_t)r, t, &o);
        ptt_dispatch(&o);
    }
    if (S.ptt.state == PTT_ON && s_ctl.fd >= 0 && t - s_ctl.t_rx > 6000) {
        ESP_LOGE(TAG, "radio silent for %lu ms while keyed", (unsigned long)(t - s_ctl.t_rx));
        ptt_fsm_abort(&S.ptt, PTT_AB_PONG_STALE, t, &o);
        ptt_dispatch(&o);
    }
    ptt_fsm_event(&S.ptt, PTT_EV_TICK, t, permit, &o);
    ptt_dispatch(&o);
}

static void icom_task(void *arg)
{
    (void)arg;
    C.backoff_ms = 500;
    C.t_retry = now_ms();

    for (;;) {
        uint32_t t = now_ms();

        if (s_ctl.fd < 0) {
            if ((int32_t)(t - C.t_retry) >= 0) session_begin(t);
            ptt_step(t);
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        fd_set rs;
        FD_ZERO(&rs);
        int maxfd = -1;
        stream_t *all[3] = { &s_ctl, &s_civ, &s_aud };
        for (int i = 0; i < 3; i++)
            if (all[i]->fd >= 0 && all[i]->to.sin_port) {
                FD_SET(all[i]->fd, &rs);
                if (all[i]->fd > maxfd) maxfd = all[i]->fd;
            }
        struct timeval tv = { .tv_sec = 0, .tv_usec = LOOP_MS * 1000 };
        if (maxfd >= 0 && select(maxfd + 1, &rs, NULL, NULL, &tv) > 0) {
            for (int i = 0; i < 3; i++) {
                stream_t *s = all[i];
                if (s->fd < 0 || !FD_ISSET(s->fd, &rs)) continue;
                int n;
                while ((n = recv(s->fd, s_rx, sizeof s_rx, MSG_DONTWAIT)) >= PKT_CONTROL) {
                    uint32_t tn = now_ms();
                    if (common(s, s_rx, n, tn)) continue;
                    if (s == &s_ctl)      on_control(s_rx, n, tn);
                    else if (s == &s_civ) on_civ(s_rx, n, tn);
                    else                  on_audio(s_rx, n);
                    if (s_ctl.fd < 0) break;          /* the session ended */
                }
                if (s_ctl.fd < 0) break;
            }
        }
        t = now_ms();
        if (s_ctl.fd < 0) continue;

        stream_tick(&s_ctl, t);
        stream_tick(&s_civ, t);
        stream_tick(&s_aud, t);

        /* Liveness: the radio pings us every half second on control. */
        if (t - s_ctl.t_rx > ALIVE_MS) {
            session_end(false, S.link == RADIO_LINK_CONNECTING ? "radio not answering"
                                                               : "radio went silent");
            continue;
        }
        if (S.link != RADIO_LINK_READY &&
            t - C.t_session > (C.waiting_busy ? BUSY_TMO_MS : GREET_TMO_MS)) {
            session_end(true, C.waiting_busy ? "the radio kept our last session"
                                             : "no session within 8 s");
            continue;
        }
        /* Streams that stop while control carries on: the radio has closed
         * them. Start over rather than show a live dial on a dead link. */
        if (S.link == RADIO_LINK_READY && t - S.t_ready_ms > STREAM_DEAD_MS) {
            if (t - C.t_civ_rx > STREAM_DEAD_MS) { session_end(true, "CI-V went silent"); continue; }
            if (!S.audio_suspend && t - C.t_aud_rx > STREAM_DEAD_MS) {
                if (!AUDIO_BEST_EFFORT) {
                    session_end(true, "audio went silent");
                    continue;
                }
                if (!C.aud_quiet) ESP_LOGW(TAG, "the radio stopped sending audio; carrying on without it");
                C.aud_quiet = true;
            }
        }
        if (C.authed && t - C.t_token >= TOKEN_MS) { send_token(0x05); C.t_token = t; }
        if (C.civ_open && t - C.t_civ_rx > CIV_QUIET_MS) {
            civ_openclose(true);                 /* ask for CI-V again */
            C.t_civ_rx = t;
        }
        if (S.link == RADIO_LINK_READY && t - S.t_ready_ms > 30000) C.backoff_ms = 500;

        ptt_step(t);
        {
            civ_msg_t m;
            while (xQueueReceive(s_later, &m, 0) == pdTRUE) civ_send(m.b, m.n);
        }
        tune_out(t);
        mem_task(t);
        rx_ant_task();
        poll_civ(t);

        /* TX audio, every 20 ms from the moment the key goes out -- and on a
         * radio that wants audio both ways, silence the rest of the time. */
        const bool on_air = S.ptt.state == PTT_REQ_ON || S.ptt.state == PTT_ON;
        if (on_air || (AUDIO_BOTH_WAYS && S.link == RADIO_LINK_READY && !S.audio_suspend)) {
            if (t - C.t_tx_frame >= 20) {
                C.t_tx_frame = (t - C.t_tx_frame > 60) ? t : C.t_tx_frame + 20;
                if (on_air) S.chronos++;
                send_tx_audio(on_air);
            }
        } else {
            C.t_tx_frame = t;
        }
    }
}

/* Log out on the way down, so the radio is free when this knob comes back. */
static void on_restart(void)
{
    if (s_ctl.fd < 0) return;
    modin_restore();
    if (C.civ_open) civ_openclose(false);
    if (C.authed) send_token(0x01);
    control(&s_civ, 0x05, 0);
    control(&s_aud, 0x05, 0);
    control(&s_ctl, 0x05, 0);
    /* The reset follows within microseconds, and a datagram still queued in
     * the WiFi driver then never leaves: the radio kept the session and
     * refused the next one for over a minute. Give them time to go. */
    vTaskDelay(pdMS_TO_TICKS(150));
}

/* ----------------------------------------------------------------- public */

const char *radio_link_name(void) { return "LAN"; }

esp_err_t radio_start(const char *host, uint16_t port, const char *user, const char *pass)
{
    ESP_RETURN_ON_FALSE(host && user && pass, ESP_ERR_INVALID_ARG, TAG, "args");
    if (!user[0] || !pass[0]) ESP_LOGW(TAG, "no radio user name or password configured");
    strlcpy(C.host, host, sizeof C.host);
    strlcpy(C.user, user, sizeof C.user);
    strlcpy(C.pass, pass, sizeof C.pass);
    C.port = port ? port : 50001;

    memset(&S, 0, sizeof S);
    tune_init(&S.tune, 14074000, 100);
    ptt_fsm_init(&S.ptt);
    strlcpy(S.mode, "usb", sizeof S.mode);
    S.smeter_dbm = -127.0f;
    S.pending_mem = -1;
    S.pending_group = -1;
    S.pending_rx = S.pending_ant = -1;
    S.model = &MODEL_OTHER;
    C.mem_scan = -1;
    C.mem_sent = -1;
    /* Read from NVS once the radio has said which it is (its capabilities):
     * the IC-9700 keeps its own. */
    s_mem = heap_caps_calloc(MEM_CHANNELS, sizeof *s_mem, MALLOC_CAP_SPIRAM);

    s_later = xQueueCreate(16, sizeof(civ_msg_t));
    ESP_RETURN_ON_FALSE(s_later, ESP_ERR_NO_MEM, TAG, "queue");
    s_ctl.keep = heap_caps_calloc(KEEP_N, KEEP_LEN, MALLOC_CAP_SPIRAM);
    s_civ.keep = heap_caps_calloc(KEEP_N, KEEP_LEN, MALLOC_CAP_SPIRAM);
    if (!s_txa) s_txa = heap_caps_malloc(PKT_AUDIO_HDR + AUDIO_FRAME * 2, MALLOC_CAP_SPIRAM);
    if (!s_txa) ESP_LOGW(TAG, "no PSRAM for TX audio; transmitting silence");

    /* ESP-IDF has five of these, and the knob uses them all on some
     * firmwares: a refusal said, never silent. */
    const esp_err_t she = esp_register_shutdown_handler(on_restart);
    if (she != ESP_OK) ESP_LOGE(TAG, "the restart handler not registered: %s", esp_err_to_name(she));
    if (xTaskCreatePinnedToCore(icom_task, "icom", 6144, NULL, 6, NULL, 0) != pdPASS) {
        ESP_LOGE(TAG, "no internal RAM for the icom task (%u free, largest %u)",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        return ESP_ERR_NO_MEM;
    }
    S.link = RADIO_LINK_CONNECTING;
    return ESP_OK;
}

int64_t radio_tune_by(int32_t detents, uint8_t accel_mult, int32_t step_hz)
{
    uint32_t t = now_ms();
    int64_t f;
    taskENTER_CRITICAL(&S_LOCK);
    if (detents && S.mem_mode) {
        S.mem_steps += detents;                      /* channels, for mem_task */
        S.t_last_input_ms = t;
    } else if (detents) {
        if (S.tune.step_hz != step_hz) tune_set_step(&S.tune, step_hz);
        const model_t *m = model_now();
        tune_apply(&S.tune, detents, accel_mult, LOOP_MS, m->f_min, m->f_max);
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

void radio_set_mode(const char *mode)
{
    if (!mode) return;
    for (size_t i = 0; i < sizeof MODES / sizeof MODES[0]; i++)
        if (strcasecmp(MODES[i].name, mode) == 0) {
            uint8_t fil = S.filter_no ? S.filter_no : 1;
            CIV_LATER(0x26, 0x00, MODES[i].mode, MODES[i].data, fil);
            CIV_LATER(0x26, 0x00);
            CIV_LATER(0x1A, 0x03);
            CIV_LATER(0x16, 0x12);          /* the new mode's AGC */
            return;
        }
}

void radio_set_filter(int32_t lo, int32_t hi)
{
    int32_t w = hi > lo ? hi - lo : lo - hi;
    if (S.mode_byte == 0x05 || S.mode_byte == 0x06) return;   /* fixed in FM */
    CIV_LATER(0x1A, 0x03, bcd(width_idx(S.mode_byte, w)));
    CIV_LATER(0x1A, 0x03);
}

void radio_select_filter(uint8_t n)
{
    if (n < 1 || n > 3) return;
    CIV_LATER(0x26, 0x00, S.mode_byte, S.data_mode, n);
    CIV_LATER(0x26, 0x00);
    CIV_LATER(0x1A, 0x03);
}

void radio_set_agc(const char *agc)
{
    static const char *AGC[] = { "fast", "mid", "slow" };
    for (uint8_t i = 0; agc && i < 3; i++)
        if (strcasecmp(agc, AGC[i]) == 0) {
            CIV_LATER(0x16, 0x12, (uint8_t)(i + 1));
            CIV_LATER(0x16, 0x12);
            return;
        }
}

void radio_set_gain(int8_t gain)
{
    if (gain < 0 || gain > 2) return;
    CIV_LATER(0x16, 0x02, (uint8_t)gain);
    CIV_LATER(0x16, 0x02);
}

/* 0-100 % as the radio's 0000-0255, and read back. */
static void set_level(uint8_t sub, uint8_t pct)
{
    const unsigned v = (pct > 100 ? 100 : pct) * 255u / 100u;
    CIV_LATER(0x14, sub, bcd(v / 100), bcd(v % 100));
    CIV_LATER(0x14, sub);
}

void radio_set_rf_gain(uint8_t pct)  { set_level(0x02, pct); }
void radio_set_rf_power(uint8_t pct) { set_level(0x0A, pct); }
void radio_set_squelch(uint8_t pct)
{
    if (model_now()->squelch) set_level(0x03, pct);
}

void radio_set_tuner(bool on)
{
    if (!model_now()->tuner) return;
    CIV_LATER(0x1C, 0x01, on ? 0x01 : 0x00);
    CIV_LATER(0x1C, 0x01);
}

void radio_set_rit(int32_t hz)
{
    if (model_now()->no_rit) return;             /* the IC-905: none */
    int32_t a = hz < 0 ? -hz : hz;
    if (a > 9999) a = 9999;
    taskENTER_CRITICAL(&S_LOCK);
    S.rit_hz = hz < 0 ? -a : a;
    taskEXIT_CRITICAL(&S_LOCK);
    CIV_LATER(0x21, 0x00, bcd(a % 100), bcd(a / 100), hz < 0 ? 0x01 : 0x00);
    CIV_LATER(0x21, 0x01, hz ? 0x01 : 0x00);
}

void radio_goto_freq(int64_t hz)
{
    /* Only where the radio tunes: the page's API takes any frequency, and a
     * radio sent one it lacks refuses it -- the dial jumped there and back. */
    const model_t *m = model_now();
    if (hz < m->f_min || hz > m->f_max) {
        ESP_LOGW(TAG, "%lld Hz is outside where the radio tunes", (long long)hz);
        return;
    }
    uint32_t t = now_ms();
    taskENTER_CRITICAL(&S_LOCK);
    tune_assign(&S.tune, hz);
    S.t_last_input_ms = t;
    taskEXIT_CRITICAL(&S_LOCK);
}

void radio_memory_mode(bool on)
{
    if (s_mem) S.pending_mem = on ? 1 : 0;
}

void radio_memory_group(uint8_t group)
{
    /* Not on the IC-9700: its group is the band it is on. */
    if (s_mem && group < 100 && !model_now()->mem_by_band) S.pending_group = group;
}

void radio_select_rx(uint8_t rx)
{
    if (rx < model_now()->n_rx) S.pending_rx = (int8_t)rx;
}

void radio_set_antenna(uint8_t ant, bool rx_ant)
{
    if (ant < model_now()->n_ant) S.pending_ant = (int8_t)(ant | (rx_ant ? 0x10 : 0));
}

/* Not yet on the Icoms: their TUNE and tuner over CI-V. */
void radio_tune(void)     {}
void radio_atu_tune(void) {}
void radio_atu_memories(bool on) { (void)on; }

/* Nothing to ask. */
bool radio_get_choice(uint8_t i, char *title, size_t tn, char *name, size_t nn)
{
    (void)i; (void)title; (void)tn; (void)name; (void)nn;
    return false;
}
void radio_choose(uint8_t i) { (void)i; }

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
    taskENTER_CRITICAL(&S_LOCK);
    o->link       = S.link;
    o->f_display  = S.tune.f_display;
    o->f_server   = S.f_server;
    o->filt_lo    = S.filt_lo;
    o->filt_hi    = S.filt_hi;
    o->filter_no  = S.filter_no;
    o->have_gain  = S.have_preamp;
    o->gain       = (int8_t)S.preamp;
    o->gain_min   = 0;
    const model_t *m = model_now();
    o->gain_max   = m->one_preamp_hz && S.f_server >= m->one_preamp_hz ? 1 : m->preamps;
    o->gain_step  = 1;
    o->has_levels   = !m->rx_only;        /* 14 02 and 14 0A on all but a receiver */
    o->has_squelch  = m->squelch;
    o->have_squelch = S.have_squelch;
    o->squelch_pct  = (uint8_t)((S.squelch * 100u + 127u) / 255u);
    o->rx_only      = m->rx_only;
    o->no_rit       = m->no_rit;
    o->have_levels  = S.have_rf_gain && S.have_rf_power;
    o->rf_gain_pct  = (uint8_t)((S.rf_gain  * 100u + 127u) / 255u);
    o->rf_power_pct = (uint8_t)((S.rf_power * 100u + 127u) / 255u);
    o->max_w        = max_w_at(m, S.f_server);
    strlcpy(o->model, C.radio_name, sizeof o->model);
    o->f_min        = m->f_min;
    o->f_max        = m->f_max;
    o->has_tuner    = m->tuner;
    o->have_tuner   = S.have_tuner;
    o->tuner_on     = S.tuner != 0;
    o->has_memories = s_mem != NULL && !C.no_mem && m->memories;
    o->n_rx       = m->n_rx;
    o->rx         = S.rx;
    if (m->n_rx > 1 && S.rx) strlcpy(o->tx_why, "SUB: RX ONLY", sizeof o->tx_why);
    o->n_ant      = m->n_ant;
    o->has_rx_ant = m->rx_ant;
    o->ant        = S.ant;
    o->ant_rx     = S.ant_rx;
    o->have_ant   = S.have_ant;
    o->mem_state  = S.mem_mode ? S.mem_state : RADIO_MEM_OFF;
    o->mem_group  = C.mem_group;
    o->mem_band   = m->mem_by_band;
    o->mem_ch     = S.mem_ch;
    if (S.mem_mode && s_mem && s_mem[S.mem_ch].used) {
        const mem_t *m = &s_mem[S.mem_ch];
        memcpy(o->mem_name, m->name, sizeof o->mem_name);
        o->mem_duplex    = m->duplex;
        o->mem_offset_hz = m->offset_hz;
        o->mem_tone_dhz  = m->tone_dhz;
    }
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
    o->permit     = ptt_permit_now(now_ms());
    o->pong_age_ms = s_ctl.fd >= 0 ? (int32_t)(now_ms() - s_ctl.t_rx) : -1;
    taskEXIT_CRITICAL(&S_LOCK);
    strlcpy(o->mode, S.mode, sizeof o->mode);
    strlcpy(o->agc, S.agc, sizeof o->agc);
    strlcpy(o->last_close, S.last_close, sizeof o->last_close);
}
