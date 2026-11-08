/* Elecraft K4 client: the k4 firmware's radio.h -- a K4 over its own remote
 * protocol, with the knob a remote client of the radio, as K4remote or
 * K4-Control are, with no computer in between.
 *
 * The K4's remote server (TCP 9205 in the clear) takes the SHA-384 of the
 * password, in lowercase hex, as the very first bytes, and from then on
 * everything -- CAT text, audio, the panadapter -- travels in one envelope
 * (k4_parse.h). The radio is told what to be with the same CAT commands its
 * serial port takes (K4 Programmer's Reference, rev. D12):
 *
 *     RDY;  the radio sends its whole state          K41;  its K4 dialect
 *     ER1;  errors with their text                   EM3;  Opus audio
 *     SLn;  how much audio jitter to ride out
 *
 * Audio is Opus both ways at 12 kHz: stereo from the radio (left Main, right
 * Sub -- the knob plays Main), mono to it, 20 ms a frame. The knob's audio
 * runs at 24 kHz, so what the radio sends is doubled up, and the microphone
 * halved. The codec has a task of its own, below this one.
 *
 * Only what a knob does is here: tune, mode, passband, AGC, RF gain, RIT,
 * the DSP switches, the S-meter and the transmit meters, PTT and a tune
 * carrier. The panadapter is never asked for, and what comes of it is
 * skipped. The sub receiver, memories, the antenna tuner and the antennas
 * are not here.
 *
 * Tried on one K4 (DC0SK's, 2026-11-07); written from the programmer's
 * reference and K4remote's notes, and run against tools/mock_k4.py
 * (tools/k4host).
 */
#include "radio.h"

#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>

#include <opus.h>

#include "audio_in.h"
#include "audio_out.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/ringbuf.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "k4_parse.h"
#include "k4_sha.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "ptt_fsm.h"
#include "vfo_tune.h"

static const char *TAG = "k4";

#define DEFAULT_PORT   9205          /* the clear port; 9204 is TLS-PSK, not spoken here */
#define LOOP_MS        5
#define CONNECT_MS     3000
#define MIC_SETTLE_MS  300           /* the mic's start-up pop, muted: the K4's ALC shows it */
#define PASSWORD_MS    3000          /* a hang-up this soon after the proof is the password */
#define AUTH_MS        6000          /* sent the password, nothing back */
#define PING_MS        1000          /* the radio drops a client silent for 10 s */
#define PING_MISS      5             /* seconds with no PONG: the link is gone */
#define PONG_FRESH_MS  3000
#define SEND_MS        50            /* tune sets, while turning */
#define QUIET_MS       300           /* then a status may move the dial */
#define SMETER_MS      100           /* the S-meter is asked for: see poll_radio() */
#define TQ_MS          150           /* while keying, "are you transmitting?" */
#define UNKEY_SETTLE_MS 1200         /* the radio's slowness to a key (0.9 s seen) and its answers in flight */
#define LATE_KEY_MS    8000          /* after our own RX;, a key of the radio's is still ours */
#define F_MIN          100000LL      /* the K4's receiver: 100 kHz... */
#define F_MAX          54000000LL    /* ...to 54 MHz (and 6 m) */
#define TUNE_MAX_MS    30000         /* a tune carrier, at most */
#define EM_MODE        3             /* Opus; the one K4remote and the radio's default use */
#define SL_TIER        3             /* the radio's default jitter allowance */

/* Opus: 12 kHz, the radio's; the knob's own audio is at AUDIO_RATE_HZ. */
#define K4_RATE        12000
#define FRAME_MAX      720           /* samples per channel the radio may send (60 ms) */
#define TX_FRAME       240           /* 20 ms, the radio's default tier */
#define OPUS_MAX       400           /* a packet of ours, to the radio */
#define RX_OPUS_MAX    1275          /* the most an Opus packet can be: the radio's, in */
#define TX_BITRATE     24000
#define TX_COMPLEXITY  1
#define RXQ_BYTES      16384         /* Opus packets waiting for the codec */
#define UP             (AUDIO_RATE_HZ / K4_RATE)

_Static_assert(AUDIO_RATE_HZ == 2 * K4_RATE, "the audio is doubled up from 12 kHz to 24");
_Static_assert(TX_AUDIO_RATE_HZ == 2 * K4_RATE, "the microphone is halved from 24 kHz to 12");

/* ----------------------------------------------------------------- state */

typedef struct {
    radio_link_t link;
    tune_t     tune;
    int64_t    f_committed, f_server;
    uint32_t   t_last_input_ms, t_last_send_ms, t_ready_ms;
    bool       have_freq, remote_pending;
    char       mode[8];
    int32_t    filt_lo, filt_hi;
    char       agc[6];
    uint8_t    dsp_has, dsp_on;
    bool       have_gain;
    int8_t     gain, gain_min, gain_max, gain_step;
    int32_t    rit_hz;
    float      smeter_dbm;
    float      tx_mic_dbm, tx_fwd_w, tx_peak_w, tx_swr, tx_alc;
    uint32_t   t_peak;
    bool       tx;                   /* the radio is transmitting */
    char       tx_why[16];           /* the radio's last refusal, as the face says it */
    uint32_t   connects, closes, reconciles, rejects, unknown_cmds, sends, echoes;
    uint32_t   chronos, txa_sent, txa_failed, txa_skipped, txa_max_us;
    uint32_t   rx_packets, rx_lost, rx_concealed, dec_max_us;
    uint32_t   send_max_ms;          /* the longest send_all, since the last report */
    uint32_t   rx_big, rx_derr;      /* packets over RX_OPUS_MAX; decodes that failed */
    int        rx_derr_code;         /* the last one's libopus error */
    char       last_close[48];
    char       link_why[16];
    char       model[16];
    ptt_fsm_t  ptt;
    uint32_t   pending_key, pending_unkey, pending_toggle, pending_tune;
    uint8_t    pending_abort;
    bool       audio_suspend;
    uint8_t    kind;                 /* KIND_* */
    uint32_t   t_keyed;
    char       note[16];
    uint32_t   note_seq;
} state_t;

enum { KIND_VOICE = 0, KIND_TUNE };

/* In PSRAM, as flex_client's are: internal RAM is what the WiFi driver sends
 * and receives from. Only tasks touch these; the lock stays internal, as the
 * atomic instruction a spinlock takes does not work on PSRAM. */
EXT_RAM_BSS_ATTR static state_t S;
static portMUX_TYPE S_LOCK = portMUX_INITIALIZER_UNLOCKED;

EXT_RAM_BSS_ATTR static struct {
    char       host[48];
    char       pass[48];
    uint16_t   port;
    int        fd;
    k4_framer_t fr;
    k4_status_t st;                  /* what the radio has said, the latest */
    bool       got_any;              /* any frame at all since connecting */
    bool       audio_seen;           /* ...and an audio one */
    bool       seeded;
    uint32_t   t_session, t_ping, t_pong, t_retry, t_smeter, t_tq, t_poll_if;
    uint8_t    pings_out;
    uint32_t   t_last_rx, loop_last, loop_gap_max;   /* data last came; the task's own pauses */
    uint32_t   backoff_ms;
    uint8_t    nr_level, ap_width;   /* kept: the radio's DSP commands want them */
    uint32_t   t_unkey;              /* when the last RX; went out */
    bool       key_confirmed;        /* the radio said it was keyed, for the key now or last out */
    uint32_t   tq_sent, tq_recv, tq_mark;   /* TQ; asked, answered, and the one asked with that RX; */
    bool       tx_asked;             /* a TX; sent and not yet answered either way */
    uint8_t    tx_seq;
} C;

typedef enum { Q_MODE, Q_FILTER, Q_AGC, Q_GAIN, Q_RIT, Q_DSP } req_kind_t;
typedef struct { uint8_t kind; int32_t a, b; char s[8]; } req_t;
static QueueHandle_t   s_req;
static RingbufHandle_t s_rxq;        /* [seq][Opus bytes], k4 -> codec */
static SemaphoreHandle_t s_wlock;    /* one writer at a time: a frame is not split */

static inline uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

/* Overridden by the application; see tci_client.c. */
__attribute__((weak)) void haptic_hook(uint8_t effect, uint8_t prio)
{
    (void)effect; (void)prio;
}

static void set_close_reason(const char *why)
{
    taskENTER_CRITICAL(&S_LOCK);
    strlcpy(S.last_close, why, sizeof S.last_close);
    taskEXIT_CRITICAL(&S_LOCK);
}

static void set_link_why(const char *why)
{
    taskENTER_CRITICAL(&S_LOCK);
    strlcpy(S.link_why, why, sizeof S.link_why);
    taskEXIT_CRITICAL(&S_LOCK);
}

static void session_end(const char *why, bool polite);
static void k4_ptt_dispatch(const ptt_out_t *o);

/* ------------------------------------------------------------------ wire */

/* Bytes to the radio, whole, under the write lock: the codec task's audio and
 * this task's commands share the socket, and a frame must not be split by the
 * other's. The socket has a send time-out (session_begin), so a stalled link
 * never holds the lock for long. */
static bool send_all(const uint8_t *b, size_t n)
{
    if (C.fd < 0) return false;
    const uint32_t t0 = now_ms();
    if (xSemaphoreTake(s_wlock, pdMS_TO_TICKS(1500)) != pdTRUE) {
        if (now_ms() - t0 > S.send_max_ms) S.send_max_ms = now_ms() - t0;
        return false;
    }
    bool ok = true;
    while (n > 0) {
        const int fd = C.fd;
        const ssize_t k = fd >= 0 ? send(fd, b, n, 0) : -1;
        if (k <= 0) { ok = false; break; }
        b += k;
        n -= (size_t)k;
    }
    xSemaphoreGive(s_wlock);
    const uint32_t took = now_ms() - t0;
    if (took > S.send_max_ms) S.send_max_ms = took;
    return ok;
}

/* One CAT command, with its ';' in the text; the frame around it is ours. */
static bool cat(const char *fmt, ...)
{
    char c[96];
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(c, sizeof c, fmt, ap);
    va_end(ap);
    if (n <= 0 || n >= (int)sizeof c) return false;
    uint8_t f[sizeof c + 16];
    const size_t m = k4_encode_cat(f, sizeof f, c);
    if (!m) return false;
    S.sends++;
    if (!send_all(f, m)) {
        session_end("the radio stopped taking commands", false);
        return false;
    }
    return true;
}

/* ------------------------------------------------------------- the radio */

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

/* The passband as lo..hi, from the width and the centre pitch the K4 keeps it
 * by: upper sideband above the carrier, lower below it (negative, as the
 * other firmwares carry it), and around it for the modes that sit on it. */
static void passband(const char *mode, int32_t bw, int32_t is, int32_t *lo, int32_t *hi)
{
    const int32_t half = bw / 2;
    if (!strcmp(mode, "usb") || !strcmp(mode, "digu")) {
        *lo = MAX(0, is - half); *hi = is + half;
    } else if (!strcmp(mode, "lsb") || !strcmp(mode, "digl")) {
        *lo = -(is + half); *hi = -MAX(0, is - half);
    } else {
        *lo = -half; *hi = half;
    }
}

static const char *refusal_for(int code, const char *text)
{
    /* The K4's own words are long ("TX inhibited"); the face has 15 letters.
     * The codes are the radio's (K4 Built-In Operating Manual): not all are
     * known, and what is not is said in the radio's own text, cut. */
    (void)code;
    static char w[16];
    size_t n = 0;
    for (const char *p = text; *p && n + 1 < sizeof w; p++)
        w[n++] = (char)((*p >= 'a' && *p <= 'z') ? *p - 32 : *p);
    w[n] = 0;
    return w;
}

static void apply_status(void)
{
    const k4_status_t *r = &C.st;
    taskENTER_CRITICAL(&S_LOCK);
    if (r->have_mode) strlcpy(S.mode, r->mode, sizeof S.mode);
    if (r->have_agc) {
        const char *a = k4_agc_name(r->agc);
        if (a) strlcpy(S.agc, a, sizeof S.agc);
    }
    if (r->have_rg) {
        S.gain = (int8_t)r->rg_db;
        S.have_gain = true;
    }
    if (r->have_rit) S.rit_hz = r->rit_on ? r->rit_hz : 0;
    if (r->have_smeter) S.smeter_dbm = r->smeter_dbm;
    if (r->have_bw || r->have_is || r->have_mode)
        passband(S.mode, r->bw_hz ? r->bw_hz : 2400, r->have_is ? r->is_hz : 1500,
                 &S.filt_lo, &S.filt_hi);
    if (r->have_nb) { S.dsp_has |= RADIO_DSP_NB; if (r->nb_on) S.dsp_on |= RADIO_DSP_NB; else S.dsp_on &= (uint8_t)~RADIO_DSP_NB; }
    if (r->have_nr) { S.dsp_has |= RADIO_DSP_NR; if (r->nr_on) S.dsp_on |= RADIO_DSP_NR; else S.dsp_on &= (uint8_t)~RADIO_DSP_NR; }
    if (r->have_na) { S.dsp_has |= RADIO_DSP_ANF; if (r->na_on) S.dsp_on |= RADIO_DSP_ANF; else S.dsp_on &= (uint8_t)~RADIO_DSP_ANF; }
    if (r->have_ap) { S.dsp_has |= RADIO_DSP_APF; if (r->ap_on) S.dsp_on |= RADIO_DSP_APF; else S.dsp_on &= (uint8_t)~RADIO_DSP_APF; }
    if (r->have_nr) C.nr_level = r->nr_level;
    if (r->have_ap) C.ap_width = r->ap_width;
    if (r->have_tm) {
        S.tx_fwd_w = r->fwd_w;
        S.tx_swr = r->swr;
        S.tx_alc = r->alc;
        const uint32_t t = now_ms();
        if (r->fwd_w >= S.tx_peak_w || t - S.t_peak > 1000) { S.tx_peak_w = r->fwd_w; S.t_peak = t; }
    }
    if (r->have_id) {
        /* ID017: a K4. The others of the family say so in their own digits. */
        strlcpy(S.model, "K4", sizeof S.model);
    }
    taskEXIT_CRITICAL(&S_LOCK);
}

/* A status that moved the transmit state is the radio's word on the key. */
static void tx_report(bool tx, uint32_t t, bool tq_reply)
{
    ptt_out_t o;
    if (tx == S.tx) {
        /* A key the radio never took -- the deadline called it refused -- has
         * no change to report when it is unkeyed: the radio was on receive all
         * along, and the ladder would climb to a reboot waiting for one. But
         * a status that merely says "receive" proves nothing: it may have been
         * made before the radio had our RX; (a late key is then still to come).
         * Only the answer to a TQ; asked together with the RX; counts -- the
         * radio answers in order -- and not before the radio's slowness (it
         * took 0.9 s to report a key) has had its time. */
        if (!tx && tq_reply && S.ptt.state == PTT_RELEASING && C.t_unkey && C.tq_recv >= C.tq_mark &&
            t - C.t_unkey >= UNKEY_SETTLE_MS) {
            ptt_fsm_event(&S.ptt, PTT_EV_CONFIRM_FALSE, t, 0, &o);
            k4_ptt_dispatch(&o);
        }
        return;
    }
    ESP_LOGI(TAG, "radio %s", tx ? "transmitting" : "receiving");
    S.tx = tx;
    C.tx_asked = false;
    if (tx) {
        /* On the air with nothing of ours keyed -- another remote client, or
         * the radio's own key -- is shown, and never unkeyed from here. */
        if (S.ptt.state != PTT_IDLE) {
            C.key_confirmed = true;
            ptt_fsm_event(&S.ptt, PTT_EV_CONFIRM_TRUE, t, 0, &o);
            k4_ptt_dispatch(&o);
        } else if (C.t_unkey && !C.key_confirmed && t - C.t_unkey < LATE_KEY_MS) {
            /* Our own key, taken after we had given up on it: it is ours to end. */
            ESP_LOGW(TAG, "the radio took a key we had given up on: unkeying");
            cat("RX;");
            cat("TQ;");
            C.tq_sent++;
        }
    } else if (S.ptt.state == PTT_ON || S.ptt.state == PTT_RELEASING) {
        ptt_fsm_event(&S.ptt, PTT_EV_CONFIRM_FALSE, t, 0, &o);
        k4_ptt_dispatch(&o);
    }
}

/* What the radio says that the knob does not read: the first of each kind, in
 * full, and any state of the transmit chain (TS: TX test, DA: voice messages
 * and the recorder, TU: tune) every time -- so that an icon on the radio can
 * be matched to what it sent. Kinds are the letters at the front. */
static void log_unknown(const char *cmd)
{
    EXT_RAM_BSS_ATTR static char seen[64][5];
    EXT_RAM_BSS_ATTR static uint8_t n_seen;
    char kind[5] = "";
    size_t k = 0;
    while (k < 4 && ((cmd[k] >= 'A' && cmd[k] <= 'Z') || cmd[k] == '$' || cmd[k] == '#')) { kind[k] = cmd[k]; k++; }
    kind[k] = 0;
    const bool chain = !strncmp(cmd, "TS", 2) || !strncmp(cmd, "DA", 2) || !strncmp(cmd, "TU", 2);
    bool known = false;
    for (uint8_t i = 0; i < n_seen; i++) if (!strcmp(seen[i], kind)) { known = true; break; }
    if (!known && n_seen < 64) {
        memcpy(seen[n_seen++], kind, sizeof seen[0]);
        ESP_LOGI(TAG, "a new kind of message from the radio: %.60s", cmd);
    } else if (chain) {
        ESP_LOGI(TAG, "radio: %.60s", cmd);
    }
}

static void on_cat_cmd(const char *cmd, uint32_t t)
{
    k4_status_t *r = &C.st;
    /* Each command is taken alone, so that a field is "heard" only once. */
    r->have_f = r->have_tx = r->have_mode = r->have_bw = r->have_smeter = r->have_tm =
        r->have_agc = r->have_rg = r->have_rit = r->have_is = r->have_nb = r->have_nr =
        r->have_na = r->have_ap = r->have_id = false;
    r->pong = false;
    r->err = 0;
    const bool cmd_is_tq = cmd[0] == 'T' && cmd[1] == 'Q';
    if (!k4_apply(r, cmd)) {
        S.unknown_cmds++;
        log_unknown(cmd);
        return;
    }
    if (r->pong) { C.t_pong = t; C.pings_out = 0; }
    if (r->have_f) on_freq(r->f_hz);
    if (r->err) {
        ESP_LOGW(TAG, "the radio says: error %d, %s", r->err, r->err_text);
        taskENTER_CRITICAL(&S_LOCK);
        strlcpy(S.tx_why, refusal_for(r->err, r->err_text), sizeof S.tx_why);
        taskEXIT_CRITICAL(&S_LOCK);
        S.rejects++;
        /* A refused key is no key: the radio will not say "transmitting", and
         * the deadline would take 1.5 s to say so. */
        if (S.ptt.state == PTT_REQ_ON && C.tx_asked) {
            ptt_out_t o;
            ptt_fsm_event(&S.ptt, PTT_EV_CONFIRM_FALSE, t, 0, &o);
            k4_ptt_dispatch(&o);
            C.tx_asked = false;
        }
        return;
    }
    const bool tq_reply = cmd_is_tq;
    if (tq_reply) C.tq_recv++;
    apply_status();
    if (r->have_tx) tx_report(r->tx, t, tq_reply);
}

static void on_cat(const uint8_t *p, size_t n, uint32_t t)
{
    size_t pos = 0;
    char c[64];
    while (k4_cat_next(p, n, &pos, c, sizeof c)) on_cat_cmd(c, t);
}

static void on_frame(void *ctx, const uint8_t *p, size_t n)
{
    (void)ctx;
    if (!n) return;
    const uint32_t t = now_ms();
    C.got_any = true;
    if (p[0] == K4_PAY_CAT) {
        on_cat(p, n, t);
    } else if (p[0] == K4_PAY_AUDIO) {
        k4_audio_t a;
        EXT_RAM_BSS_ATTR static uint8_t item[1 + RX_OPUS_MAX];
        if (!k4_audio_parse(p, n, &a) || a.n == 0) return;
        if (a.n > RX_OPUS_MAX) { S.rx_big++; return; }
        /* The first audio packet of a session, as it came: what the radio
         * is sending, if the knob does not play it. */
        if (!C.audio_seen) {
            C.audio_seen = true;
            ESP_LOGI(TAG, "first audio packet: mode %u, %u samples a channel, %u bytes, seq %u, "
                          "payload version %u, rate code %u", a.mode, a.samples, (unsigned)a.n,
                     a.seq, p[1], p[6]);
        }
        if (a.mode != K4_EM_OPUS_INT && a.mode != K4_EM_OPUS_FLOAT) {   /* PCM we did not ask for */
            S.rx_big++;
            return;
        }
        S.rx_packets++;
        item[0] = a.seq;
        memcpy(item + 1, a.data, a.n);
        if (!s_rxq || xRingbufferSend(s_rxq, item, 1 + a.n, 0) != pdTRUE) S.rx_lost++;
    }
    /* Anything else -- the panadapter -- is not asked for, and not read. */
}

/* ------------------------------------------------------------------- PTT */

static const char *tx_why(void)
{
    return S.tx_why;
}

static void log_refusal(uint32_t missing)
{
    ESP_LOGW(TAG, "PTT refused: missing permits 0x%lx (%s)", (unsigned long)missing,
             missing & PERMIT_LINK ? "link not ready" :
             missing & PERMIT_PONG_FRESH ? "radio not answering" : "other");
}

static void k4_ptt_dispatch(const ptt_out_t *o)
{
    /* The microphone and the key first, the motor last: it sits next to the
     * microphone, and must never be heard on the air. */
    if (o->refused || o->left_tx) audio_in_set_active(false);
    if (o->send_key) {
        taskENTER_CRITICAL(&S_LOCK);
        S.tx_why[0] = 0;
        taskEXIT_CRITICAL(&S_LOCK);
        C.key_confirmed = false;
        if (S.kind == KIND_TUNE) {
            ESP_LOGW(TAG, "tune carrier");
            C.tx_asked = true;
            cat("TU1;");
        } else {
            audio_in_set_active(true);
            C.tx_asked = true;
            cat("TX;");
        }
        cat("TQ;");
        C.tq_sent++;
        C.t_tq = now_ms();
    }
    if (o->send_unkey) {
        /* An RX; alone does not end a tune (the radio's TU0 does). */
        if (S.kind == KIND_TUNE) cat("TU0;");
        cat("RX;");
        cat("TQ;");
        C.tq_sent++;
        C.tq_mark = C.tq_sent;
        C.t_tq = C.t_unkey = now_ms();
    }
    /* Rungs 2 and 3: leaving. Whether a K4 drops a remote client's key with
     * its connection is not known -- K4remote also sends RX; first on a lost
     * link, and so does session_end(polite) here -- so the unkey is sent as
     * well, where the link still takes it. */
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
        ESP_LOGW(TAG, "*** TX ***%s", S.kind == KIND_TUNE ? " (tune)" : "");
    }
    if (o->left_tx) ESP_LOGI(TAG, "*** RX ***");
    if (o->refused) log_refusal(o->missing);
    if (o->haptic) haptic_hook(o->haptic, o->haptic_prio);
}

static uint32_t ptt_permit_now(uint32_t t)
{
    /* The radio says nothing of band or inhibit before a key (an error comes
     * after), so those are left to its answer; the rest are ours to know. */
    uint32_t p = PERMIT_MODE | PERMIT_NO_OVERLAY | PERMIT_NO_FAULT | PERMIT_TX_ENABLE |
                 PERMIT_BAND | PERMIT_TRX;
    if (S.link == RADIO_LINK_READY && t - S.t_ready_ms >= 500) p |= PERMIT_LINK;
    if (C.fd >= 0 && t - C.t_pong < PONG_FRESH_MS) p |= PERMIT_PONG_FRESH;
    if (t - S.t_last_input_ms >= QUIET_MS) p |= PERMIT_NO_RECONCILE;
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
    if (S.pending_tune) {
        S.pending_tune = 0;
        if (S.ptt.state == PTT_IDLE) {
            S.kind = KIND_TUNE;
            ptt_fsm_event(&S.ptt, PTT_EV_TAP_KEY, t, permit, &o);
            k4_ptt_dispatch(&o);
        }
    }
    if (S.pending_key) {
        S.pending_key = 0;
        if (S.ptt.state == PTT_IDLE) S.kind = KIND_VOICE;
        ptt_fsm_event(&S.ptt, PTT_EV_TAP_KEY, t, permit, &o);
        k4_ptt_dispatch(&o);
    }
    /* A tune carrier that runs on is stopped, as PTT would. */
    if (S.ptt.state == PTT_ON && S.kind == KIND_TUNE && t - S.t_keyed > TUNE_MAX_MS) {
        ESP_LOGW(TAG, "tune carrier: time is up");
        S.pending_unkey = 1;
    }
    if (S.pending_unkey) {
        S.pending_unkey = 0;
        ptt_fsm_event(&S.ptt, PTT_EV_TAP_UNKEY, t, permit, &o);
        k4_ptt_dispatch(&o);
    }
    if (S.pending_abort) {
        const uint8_t r = S.pending_abort;
        S.pending_abort = 0;
        ptt_fsm_abort(&S.ptt, (ptt_abort_t)r, t, &o);
        k4_ptt_dispatch(&o);
    }
    if (S.ptt.state == PTT_ON && C.fd >= 0 && t - C.t_pong > 6000) {
        ESP_LOGE(TAG, "radio silent for %lu ms while keyed", (unsigned long)(t - C.t_pong));
        ptt_fsm_abort(&S.ptt, PTT_AB_PONG_STALE, t, &o);
        k4_ptt_dispatch(&o);
    }
    /* While a key is out, ask whether the radio has it: its own report may
     * come late, and the deadline is short. */
    if (S.ptt.state != PTT_IDLE && C.fd >= 0 && t - C.t_tq >= TQ_MS) {
        C.t_tq = t;
        cat("TQ;");
        C.tq_sent++;
    }
    ptt_fsm_event(&S.ptt, PTT_EV_TICK, t, permit, &o);
    k4_ptt_dispatch(&o);
}

/* ---------------------------------------------------------------- audio */

/* The K4's remote audio input has no gain of its own to turn down (its MIC
 * setting did nothing, with the mic unplugged either): a 0.07 peak (-23 dBFS)
 * already read 7 on the ALC, whose bar ends at 8. The knob's levels were meant
 * for radios with their own TX level, so for the K4 it is turned down here,
 * -20 dB, with the operator's TX slider on top. */
#define TX_ATTEN_NUM 13              /* (a+b) * 13 / 256 = the mean * 0.1 */
#define TX_ATTEN_SHR 8

static void tx_frame(OpusEncoder *enc)
{
    EXT_RAM_BSS_ATTR static int16_t mic[2 * TX_FRAME], mono[TX_FRAME];
    EXT_RAM_BSS_ATTR static uint8_t pkt[OPUS_MAX], frame[OPUS_MAX + 32];
    if (!audio_in_take(mic, 2 * TX_FRAME)) {
        memset(mic, 0, sizeof mic);
        S.txa_skipped++;
    } else {
        int peak = 0;
        for (int i = 0; i < 2 * TX_FRAME; i++) {
            const int v = mic[i] < 0 ? -mic[i] : mic[i];
            if (v > peak) peak = v;
        }
        S.tx_mic_dbm = peak ? 20.0f * log10f((float)peak / 32767.0f) : -60.0f;
    }
    /* 24 kHz to 12: the two samples' mean, which is also a crude low-pass --
     * voice for the air has nothing above 3 kHz to keep. */
    for (int i = 0; i < TX_FRAME; i++)
        mono[i] = (int16_t)((((int)mic[2 * i] + (int)mic[2 * i + 1]) * TX_ATTEN_NUM) >> TX_ATTEN_SHR);
    const int64_t t0 = esp_timer_get_time();
    const int n = opus_encode(enc, mono, TX_FRAME, pkt, OPUS_MAX);
    const uint32_t us = (uint32_t)(esp_timer_get_time() - t0);
    if (us > S.txa_max_us) S.txa_max_us = us;
    if (n <= 0) { S.txa_failed++; return; }
    const size_t m = k4_encode_audio(frame, sizeof frame, C.tx_seq++, EM_MODE, TX_FRAME, pkt, (size_t)n);
    if (m && send_all(frame, m)) S.txa_sent++;
    else S.txa_failed++;
}

/* The radio's frame -- 12 kHz stereo, Main on the left -- to the jack: Main
 * in both ears, each sample doubled up with its midpoint to the knob's 24 kHz.
 * `prev` is the last sample, so that the first midpoint joins on. */
static void play(const int16_t *pcm, int m, int16_t *prev, int16_t *out)
{
    for (int k = 0; k < m; k++) {
        const int16_t x = pcm[2 * k];
        const int16_t mid = (int16_t)(((int)*prev + (int)x) / 2);
        out[(UP * k) * 2] = out[(UP * k) * 2 + 1] = mid;
        out[(UP * k + 1) * 2] = out[(UP * k + 1) * 2 + 1] = x;
        *prev = x;
    }
    audio_out_feed_pcm16(out, (size_t)(UP * m), 2);
}

/* The radio's audio in, the microphone out. On a task of its own, below the
 * radio's, so a slow frame never holds up PTT. */
static void codec_task(void *arg)
{
    (void)arg;
    int err;
    OpusDecoder *dec = opus_decoder_create(K4_RATE, 2, &err);
    OpusEncoder *enc = opus_encoder_create(K4_RATE, 1, OPUS_APPLICATION_VOIP, &err);
    if (!dec || !enc) {
        ESP_LOGE(TAG, "no Opus codec (%d): no audio either way", err);
        vTaskDelete(NULL);
        return;
    }
    opus_encoder_ctl(enc, OPUS_SET_BITRATE(TX_BITRATE));
    opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(TX_COMPLEXITY));
    opus_encoder_ctl(enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));

    EXT_RAM_BSS_ATTR static int16_t pcm[2 * FRAME_MAX], out[2 * UP * FRAME_MAX];
    uint8_t last = 0;
    int     last_n = TX_FRAME;
    bool    have_last = false;
    int16_t prev = 0;
    int64_t next_tx = 0;
    for (;;) {
        const bool keyed = S.ptt.state == PTT_REQ_ON || S.ptt.state == PTT_ON;
        const bool voice = keyed && S.kind == KIND_VOICE;
        size_t n = 0;
        uint8_t *it = xRingbufferReceive(s_rxq, &n, pdMS_TO_TICKS(keyed ? 1 : 20));
        if (it) {
            const uint8_t seq = it[0];
            /* In transmit the radio's stream is our own monitor at most: not
             * played, and the frames not decoded -- the encoder wants the time. */
            if (!keyed && !S.audio_suspend && n > 1) {
                if (have_last) {
                    /* Up to three lost frames concealed; more is a new start. */
                    const uint8_t missed = (uint8_t)(seq - last - 1);
                    for (uint8_t i = 0; missed <= 3 && i < missed; i++) {
                        const int m = opus_decode(dec, NULL, 0, pcm, last_n, 0);
                        if (m <= 0) break;
                        S.rx_concealed++;
                        play(pcm, m, &prev, out);
                    }
                }
                const int64_t t0 = esp_timer_get_time();
                const int m = opus_decode(dec, it + 1, (opus_int32)(n - 1), pcm, FRAME_MAX, 0);
                const uint32_t us = (uint32_t)(esp_timer_get_time() - t0);
                if (us > S.dec_max_us) S.dec_max_us = us;
                if (m > 0) {
                    last_n = m;
                    play(pcm, m, &prev, out);
                } else {
                    if (!S.rx_derr) ESP_LOGW(TAG, "Opus decode failed: %d, on %u bytes", m, (unsigned)(n - 1));
                    S.rx_derr++;
                    S.rx_derr_code = m;
                }
            }
            last = seq;
            have_last = true;
            vRingbufferReturnItem(s_rxq, it);
        }
        if (voice) {
            const int64_t now = esp_timer_get_time();
            if (!next_tx || now - next_tx > 100000) next_tx = now;   /* (re)start */
            if (now >= next_tx) {
                next_tx += 20000;
                S.chronos++;
                tx_frame(enc);
            }
        } else {
            next_tx = 0;
        }
    }
}

/* --------------------------------------------------------------- session */

static int wifi_rssi(void)
{
    wifi_ap_record_t ap;
    return esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : 0;
}

static void session_end(const char *why, bool polite)
{
    if (C.fd >= 0)
        ESP_LOGW(TAG, "session ending (%s): last data %lu ms ago, %u pings unanswered, this task's "
                      "longest pause %lu ms, longest send %lu ms, WiFi %d dBm, internal free %u "
                      "(lowest %u, block %u)",
                 why, (unsigned long)(now_ms() - C.t_last_rx), C.pings_out,
                 (unsigned long)C.loop_gap_max, (unsigned long)S.send_max_ms, wifi_rssi(),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    /* A radio that hangs up soon after the password, before it has said a
     * word, did not like it: that is what it does -- and our next send then
     * fails, so how the end is worded does not tell. A connection that dies
     * late is a bad link and says nothing of the password: a ping with 65% loss
     * had the knob ask for a password that had not changed. */
    if (C.fd >= 0 && !C.got_any && !polite &&
        (strstr(why, "closed") || strstr(why, "lost") || strstr(why, "taking"))) {
        if (now_ms() - C.t_session < PASSWORD_MS) set_link_why("PASSWORD?");
        else set_link_why("NO LINK");
    }
    if (C.fd >= 0) {
        if (polite) {
            /* A key left behind is dropped with the connection; say RX first. */
            uint8_t f[32];
            const size_t m = k4_encode_cat(f, sizeof f, "RX;");
            if (m) send_all(f, m);
        }
        const int fd = C.fd;
        C.fd = -1;                                    /* the codec reads this first */
        close(fd);
    }
    C.got_any = C.seeded = C.tx_asked = C.audio_seen = false;
    k4_framer_init(&C.fr);
    memset(&C.st, 0, sizeof C.st);
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
    S.tx = false;
    S.dsp_has = S.dsp_on = 0;
    taskEXIT_CRITICAL(&S_LOCK);
    set_close_reason(why);
    C.t_retry = t + C.backoff_ms;
    C.backoff_ms = MIN(C.backoff_ms * 2, 16000);
    ESP_LOGW(TAG, "session ended: %s (retry in %lu ms)", why, (unsigned long)C.backoff_ms);
}

static bool resolve(struct sockaddr_in *to)
{
    to->sin_family = AF_INET;
    to->sin_port = htons(C.port);
    if (inet_aton(C.host, &to->sin_addr)) return true;
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM }, *res = NULL;
    if (getaddrinfo(C.host, NULL, &hints, &res) != 0 || !res) return false;
    to->sin_addr = ((struct sockaddr_in *)res->ai_addr)->sin_addr;
    freeaddrinfo(res);
    return true;
}

/* The questions that fill the state in, after RDY;'s dump: each answered
 * with the K4's own response, taken like any other. */
static void seed(void)
{
    static const char *const Q[] = {
        "IF;", "FA;", "MD;", "BW;", "IS;", "SM;", "SMH;", "GT;", "RG;", "RO;", "PC;",
        "NB;", "NR;", "NA;", "AP;", "ID;", "TM1;",
    };
    for (size_t i = 0; i < sizeof Q / sizeof Q[0] && C.fd >= 0; i++) cat("%s", Q[i]);
}

static bool session_begin(uint32_t t)
{
    struct sockaddr_in to;
    if (!resolve(&to)) {
        set_close_reason("bad radio address");
        set_link_why("BAD ADDRESS");
        C.t_retry = t + 5000;
        return false;
    }
    const int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        set_close_reason("no socket");
        C.t_retry = t + 2000;
        return false;
    }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    struct timeval tv = { .tv_sec = CONNECT_MS / 1000, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    ESP_LOGI(TAG, "connecting to %s:%u", C.host, (unsigned)C.port);
    taskENTER_CRITICAL(&S_LOCK);
    S.link = RADIO_LINK_CONNECTING;
    taskEXIT_CRITICAL(&S_LOCK);
    if (connect(fd, (struct sockaddr *)&to, sizeof to) != 0) {
        close(fd);
        taskENTER_CRITICAL(&S_LOCK);
        S.link = RADIO_LINK_DOWN;
        taskEXIT_CRITICAL(&S_LOCK);
        set_close_reason("radio not answering");
        set_link_why("NO ANSWER");
        C.t_retry = now_ms() + C.backoff_ms;
        C.backoff_ms = MIN(C.backoff_ms * 2, 16000);
        return false;
    }
    C.fd = fd;
    k4_framer_init(&C.fr);
    C.got_any = C.seeded = false;
    C.pings_out = 0;
    C.t_session = C.t_ping = C.t_pong = C.t_smeter = C.t_poll_if = C.t_last_rx = now_ms();
    taskENTER_CRITICAL(&S_LOCK);
    S.link = RADIO_LINK_GREETING;
    S.connects++;
    taskEXIT_CRITICAL(&S_LOCK);

    /* The password's proof, raw -- before any frame. The radio closes on a
     * wrong one, which is what "closed before it said anything" is below. */
    char hex[K4_AUTH_HEX];
    k4_sha384_hex(C.pass, hex);
    if (!send_all((const uint8_t *)hex, strlen(hex))) {
        session_end("the radio stopped taking the password", false);
        return false;
    }
    /* What the radio is to be to us: its state, the K4 dialect, errors with
     * their text, Opus audio, and the jitter allowance; then the questions. */
    if (!cat("RDY;") || !cat("K41;") || !cat("ER1;") || !cat("EM%d;", EM_MODE) ||
        !cat("SL%d;", SL_TIER))
        return false;
    seed();
    C.seeded = true;
    return C.fd >= 0;
}

/* ----------------------------------------------------------------- tasks */

static void take_requests(void)
{
    req_t r;
    char b[24];
    while (xQueueReceive(s_req, &r, 0) == pdTRUE) {
        switch (r.kind) {
        case Q_MODE: {
            const char d = k4_mode_digit(r.s);
            if (d && k4_cmd_mode(b, sizeof b, d)) {
                cat("%s", b);
                taskENTER_CRITICAL(&S_LOCK);
                strlcpy(S.mode, r.s, sizeof S.mode);
                taskEXIT_CRITICAL(&S_LOCK);
                /* The width the new mode keeps comes back with its own BW. */
                cat("BW;");
                cat("IS;");
            }
            break;
        }
        case Q_FILTER: {
            const int32_t w = r.b > r.a ? r.b - r.a : r.a - r.b;
            if (k4_cmd_bw(b, sizeof b, w)) cat("%s", b);
            taskENTER_CRITICAL(&S_LOCK);
            S.filt_lo = r.a;
            S.filt_hi = r.b;
            taskEXIT_CRITICAL(&S_LOCK);
            break;
        }
        case Q_AGC: {
            const int g = k4_agc_digit(r.s);
            if (g >= 0) {
                cat("GT%d;", g);
                taskENTER_CRITICAL(&S_LOCK);
                strlcpy(S.agc, r.s, sizeof S.agc);
                taskEXIT_CRITICAL(&S_LOCK);
            }
            break;
        }
        case Q_GAIN:
            if (k4_cmd_rg(b, sizeof b, r.a)) cat("%s", b);
            S.gain = (int8_t)r.a;
            break;
        case Q_RIT:
            /* RIT is on while it has an offset, and off at zero. */
            if (k4_cmd_rit(b, sizeof b, r.a)) cat("%s", b);
            cat("RT%d;", r.a != 0);
            S.rit_hz = r.a;
            break;
        case Q_DSP: {
            const bool on = r.b != 0;
            if (r.a == RADIO_DSP_NB)       cat("NB%d;", on);
            else if (r.a == RADIO_DSP_ANF) cat("NA%d;", on);
            else if (r.a == RADIO_DSP_NR)  cat("NR%02u%d;", C.nr_level ? C.nr_level : 2u, on);
            else if (r.a == RADIO_DSP_APF) cat("AP%d%u;", on, (unsigned)C.ap_width);
            taskENTER_CRITICAL(&S_LOCK);
            if (on) S.dsp_on |= (uint8_t)r.a;
            else    S.dsp_on &= (uint8_t)~r.a;
            taskEXIT_CRITICAL(&S_LOCK);
            break;
        }
        }
    }
}

static void tune_out(uint32_t t)
{
    if (S.link != RADIO_LINK_READY || S.ptt.state != PTT_IDLE) return;
    bool fire = false;
    int64_t want = 0;
    taskENTER_CRITICAL(&S_LOCK);
    /* A change made elsewhere, adopted once the knob has been quiet. */
    if (S.remote_pending && t - S.t_last_input_ms >= QUIET_MS && t - S.t_last_send_ms >= QUIET_MS) {
        S.remote_pending = false;
        if (S.f_server != S.f_committed) {
            tune_assign(&S.tune, S.f_server);
            S.f_committed = S.f_server;
            S.reconciles++;
        }
    }
    if (S.have_freq && S.tune.f_display != S.f_committed && t - S.t_last_send_ms >= SEND_MS) {
        want = S.tune.f_display;
        S.f_committed = want;
        S.t_last_send_ms = t;
        fire = true;
    }
    taskEXIT_CRITICAL(&S_LOCK);
    if (fire) {
        char b[24];
        if (k4_cmd_freq(b, sizeof b, want)) cat("%s", b);
    }
}

/* What the radio does not push on its own, asked for: the S-meter while
 * receiving, and the status line now and then as a safety net under whatever
 * it does push. */
static void poll_radio(uint32_t t)
{
    if (!S.tx && S.ptt.state == PTT_IDLE && t - C.t_smeter >= SMETER_MS) {
        C.t_smeter = t;
        cat("SMH;");
    }
    if (t - C.t_poll_if >= 1000) {
        C.t_poll_if = t;
        cat("IF;");
    }
}

static void k4_task(void *arg)
{
    (void)arg;
    C.backoff_ms = 500;
    C.t_retry = now_ms();
    EXT_RAM_BSS_ATTR static uint8_t buf[1460];
    uint32_t rep_at = 0, rep_pkts = 0;
    for (;;) {
        uint32_t t = now_ms();
        if (C.loop_last && t - C.loop_last > C.loop_gap_max) C.loop_gap_max = t - C.loop_last;
        C.loop_last = t;
        /* Receive audio, every 10 s: what came, what was dropped or would not
         * decode, and what the jack made of it -- the first thing to read when
         * a K4 is heard by nobody. */
        if (!rep_at) rep_at = t + 10000;
        if ((int32_t)(t - rep_at) >= 0) {
            rep_at = t + 10000;
            audio_stats_t as;
            audio_out_stats(&as);
            if (C.fd >= 0)
                ESP_LOGI(TAG, "rx audio, 10 s: %lu packets (%lu in all), %lu ring full, %lu concealed, "
                              "%lu decode errors (last %d), %lu not taken; decode %lu us at most | "
                              "jack: %lu frames, %lu dropped, %lu underruns",
                         (unsigned long)(S.rx_packets - rep_pkts), (unsigned long)S.rx_packets,
                         (unsigned long)S.rx_lost, (unsigned long)S.rx_concealed,
                         (unsigned long)S.rx_derr, S.rx_derr_code, (unsigned long)S.rx_big,
                         (unsigned long)S.dec_max_us, (unsigned long)as.frames,
                         (unsigned long)as.dropped, (unsigned long)as.underruns);
            if (C.fd >= 0)
                ESP_LOGI(TAG, "link, 10 s: WiFi %d dBm; this task paused %lu ms at most, a send took %lu ms "
                              "at most; last data %lu ms ago; internal free %u, lowest %u, block %u",
                         wifi_rssi(), (unsigned long)C.loop_gap_max, (unsigned long)S.send_max_ms,
                         (unsigned long)(t - C.t_last_rx),
                         (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                         (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                         (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
            C.loop_gap_max = 0;
            S.send_max_ms = 0;
            rep_pkts = S.rx_packets;
            S.dec_max_us = 0;
        }
        if (C.fd < 0) {
            if ((int32_t)(t - C.t_retry) >= 0) session_begin(t);
            ptt_step(t);
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        fd_set rs;
        FD_ZERO(&rs);
        FD_SET(C.fd, &rs);
        struct timeval tv = { .tv_sec = 0, .tv_usec = LOOP_MS * 1000 };
        const int fd = C.fd;
        const int sel = select(fd + 1, &rs, NULL, NULL, &tv);
        if (sel > 0 && FD_ISSET(fd, &rs)) {
            const int n = recv(fd, buf, sizeof buf, MSG_DONTWAIT);
            if (n == 0) {
                session_end(C.got_any ? "the radio closed the connection"
                                      : "closed before it said anything: the password?", false);
                continue;
            }
            if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                session_end("connection lost", false);
                continue;
            }
            if (n > 0) {
                C.t_last_rx = now_ms();
                k4_framer_push(&C.fr, buf, (size_t)n, on_frame, NULL);
            }
            if (C.fd < 0) continue;
        }
        t = now_ms();

        if (!C.got_any && t - C.t_session > AUTH_MS) {
            set_link_why("NO ANSWER");
            session_end("no answer after the password", false);
            continue;
        }
        if (t - C.t_ping >= PING_MS) {
            if (C.pings_out >= PING_MISS) { session_end("the radio stopped answering", false); continue; }
            C.pings_out++;
            C.t_ping = t;
            if (!cat("PING;")) continue;
        }
        if (S.link != RADIO_LINK_READY && S.have_freq && S.mode[0]) {
            taskENTER_CRITICAL(&S_LOCK);
            S.link = RADIO_LINK_READY;
            S.t_ready_ms = t;
            S.link_why[0] = 0;
            taskEXIT_CRITICAL(&S_LOCK);
            C.backoff_ms = 500;
            ESP_LOGI(TAG, "ready: %lld Hz, %s", (long long)S.f_server, S.mode);
        }
        ptt_step(t);
        if (C.fd < 0) continue;
        take_requests();
        if (S.link == RADIO_LINK_READY) {
            poll_radio(t);
            tune_out(t);
        }
    }
}

static void on_restart(void)
{
    if (C.fd < 0) return;
    session_end("restarting", true);
    vTaskDelay(pdMS_TO_TICKS(100));
}

/* ----------------------------------------------------------------- public */

const char *radio_link_name(void) { return "K4"; }

esp_err_t radio_start(const char *host, uint16_t port, const char *user, const char *pass)
{
    audio_in_set_settle_ms(MIC_SETTLE_MS);
    (void)user;
    ESP_RETURN_ON_FALSE(host, ESP_ERR_INVALID_ARG, TAG, "args");
    strlcpy(C.host, host, sizeof C.host);
    strlcpy(C.pass, pass ? pass : "", sizeof C.pass);
    C.port = port ? port : DEFAULT_PORT;
    C.fd = -1;

    memset(&S, 0, sizeof S);
    tune_init(&S.tune, 14100000, 100);
    ptt_fsm_init(&S.ptt);
    strlcpy(S.mode, "", sizeof S.mode);
    S.smeter_dbm = -127.0f;
    S.gain_min = -60;
    S.gain_max = 0;
    S.gain_step = 5;
    C.nr_level = 2;

    s_req = xQueueCreate(16, sizeof(req_t));
    s_rxq = xRingbufferCreateWithCaps(RXQ_BYTES, RINGBUF_TYPE_NOSPLIT, MALLOC_CAP_SPIRAM);
    s_wlock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_req && s_rxq && s_wlock, ESP_ERR_NO_MEM, TAG, "buffers");

    const esp_err_t she = esp_register_shutdown_handler(on_restart);
    if (she != ESP_OK) ESP_LOGE(TAG, "the restart handler not registered: %s", esp_err_to_name(she));
    /* The codec's stack in PSRAM: Opus is deep, internal RAM is what WiFi
     * sends from, and this task touches no flash. */
    /* On core 1, with the jack's and the display's light tasks, not on core 0
     * with WiFi, lwIP and the radio's task: the K4's audio packets are 950
     * bytes of stereo Opus every 40 ms, and decoding one took up to 200 ms
     * wall-clock beside the network (2026-11-07, on a bench knob). */
    if (xTaskCreatePinnedToCoreWithCaps(codec_task, "k4codec", 16384, NULL, 5, NULL, 1,
                                        MALLOC_CAP_SPIRAM) != pdPASS)
        ESP_LOGE(TAG, "no memory for the codec task: no audio");
    if (xTaskCreatePinnedToCore(k4_task, "k4", 6144, NULL, 6, NULL, 0) != pdPASS) {
        ESP_LOGE(TAG, "no internal RAM for the k4 task (%u free, largest %u)",
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
    if (mode && k4_mode_digit(mode)) request(Q_MODE, 0, 0, mode);
}
void radio_set_filter(int32_t lo, int32_t hi) { request(Q_FILTER, lo, hi, NULL); }
void radio_select_filter(uint8_t n) { (void)n; }
void radio_set_agc(const char *agc) { if (agc) request(Q_AGC, 0, 0, agc); }
void radio_set_gain(int8_t gain) { request(Q_GAIN, gain, 0, NULL); }
void radio_set_rit(int32_t hz) { request(Q_RIT, hz, 0, NULL); }

void radio_goto_freq(int64_t hz)
{
    const uint32_t t = now_ms();
    taskENTER_CRITICAL(&S_LOCK);
    if (hz < F_MIN) hz = F_MIN;
    if (hz > F_MAX) hz = F_MAX;
    tune_assign(&S.tune, hz);
    S.t_last_input_ms = t;
    taskEXIT_CRITICAL(&S_LOCK);
}

void radio_tune(void)     { S.pending_tune = 1; }
void radio_atu_tune(void) {}
void radio_atu_memories(bool on) { (void)on; }
void radio_set_rf_gain(uint8_t pct)  { (void)pct; }
void radio_set_rf_power(uint8_t pct) { (void)pct; }
void radio_set_tuner(bool on)        { (void)on; }
void radio_set_squelch(uint8_t pct)  { (void)pct; }
void radio_set_dsp(uint8_t which, bool on) { request(Q_DSP, which, on, NULL); }

bool radio_get_choice(uint8_t i, char *title, size_t tn, char *name, size_t nn)
{
    (void)i;
    if (tn) title[0] = 0;
    if (nn) name[0] = 0;
    return false;
}
void radio_choose(uint8_t i) { (void)i; }

void radio_memory_mode(bool on)        { (void)on; }
void radio_memory_group(uint8_t group) { (void)group; }
void radio_select_rx(uint8_t rx) { (void)rx; }
void radio_set_antenna(uint8_t ant, bool rx_ant) { (void)ant; (void)rx_ant; }
void radio_set_tx_antenna(uint8_t ant) { (void)ant; }
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
    o->f_min      = F_MIN;
    o->f_max      = F_MAX;
    o->filt_lo    = S.filt_lo;
    o->filt_hi    = S.filt_hi;
    o->dsp_has    = S.dsp_has;
    o->dsp_on     = S.dsp_on & S.dsp_has;
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
    strlcpy(o->link_why, S.link_why, sizeof o->link_why);
    strlcpy(o->model, S.model, sizeof o->model);
    o->has_tune   = true;
    strlcpy(o->note, S.note, sizeof o->note);
    o->note_seq   = S.note_seq;
    strlcpy(o->mode, S.mode, sizeof o->mode);
    strlcpy(o->agc, S.agc, sizeof o->agc);
    strlcpy(o->last_close, S.last_close, sizeof o->last_close);
    taskEXIT_CRITICAL(&S_LOCK);
}
