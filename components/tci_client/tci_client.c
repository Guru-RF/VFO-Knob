/* TCI v2.0 client for AetherSDR.
 *
 * Connects out to ws://<host>:50001 and speaks the same protocol AetherSDR
 * already serves to WSJT-X. Design notes that matter:
 *
 *  - Tuning is OPTIMISTIC. The wire round trip is 30-80 ms with an unbounded
 *    tail (it waits on someone else's Qt event loop), while the glass and the
 *    motor are 10-20 ms. Neither may wait for the wire, so the display leads
 *    and the anti-echo classifier reconciles afterwards.
 *
 *  - There is NO outbound queue, deliberately. The server closes the socket
 *    after 64 queued commands, so a queue would turn an enthusiastic flick
 *    into a disconnection. Instead the sender polls for a difference and is
 *    hard-capped at 20 Hz, which makes that failure structurally impossible
 *    rather than merely unlikely.
 *
 *  - The greeting is authoritative on every (re)connect. Pushing our stale
 *    pre-dropout frequency at a rig the operator has since retuned would be
 *    the rudest possible bug.
 */
#include "radio.h"

#include <string.h>

#include "antiecho.h"
#include "audio_in.h"
#include "audio_out.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "lwip/sockets.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ptt_fsm.h"
#include "tci_parse.h"
#include "vfo_tune.h"

static const char *TAG = "tci";

#define RX_CAP        2048
#define SEND_GATE_MS  5       /* how often we look for work */
#define GREET_TMO_MS  4000    /* a socket that accepts but never greets is a
                                 real failure mode when AetherSDR is starting */
#define AGC_POLL_MS   3000    /* see the AGC poll in tx_task */

typedef struct {
    radio_link_t link;
    tune_t     tune;
    accel_t    accel;
    echo_ring_t echo;
    int64_t    f_committed, f_server;
    uint32_t   t_last_input_ms, t_last_send_ms, t_ready_ms;
    bool       reconcile_armed;
    char       mode[8];
    char       agc[6];
    uint32_t   t_agc_poll;
    int32_t    filt_lo, filt_hi, rit_hz;
    float      smeter_dbm;
    float      tx_mic_dbm, tx_fwd_w, tx_peak_w, tx_swr, tx_alc;
    uint32_t   tx_sensor_frames, tx_sensor_log_ms;
    bool       slice_locked, tx;
    uint8_t    my_trx, n_trx;
    uint32_t   connects, closes, reconciles, rejects, unknown_cmds, sends, echoes;
    char       last_close[48];
    /* --- PTT --- */
    ptt_fsm_t  ptt;
    int64_t    last_pong_us;
    bool       tx_enable_seen;
    bool       have_chan_sensors;   /* the opt-in, higher-precision stream */
    bool       need_sensors_enable;
    bool       need_audio_start;
    bool       audio_on;
    uint32_t   t_audio_start_ms;   /* when audio_start went out, 0 if not yet */
    uint8_t    audio_kills;        /* links dropped right after audio_start */
    bool       audio_blocked;      /* stop asking; audio is what breaks us */
    bool       audio_suspend;      /* held off while something else needs the link */
    uint32_t   pending_key, pending_unkey, pending_toggle;
    uint8_t    pending_abort;
    uint32_t   chronos, txa_sent, txa_failed, txa_skipped, txa_max_us;
} state_t;

static state_t     S;
static portMUX_TYPE S_LOCK = portMUX_INITIALIZER_UNLOCKED;
static esp_websocket_client_handle_t s_ws;
static char        s_rx[RX_CAP];
static size_t      s_rx_len;
static int64_t     s_greet_deadline_us;
static bool        s_last_remote_tx;
static int64_t     s_retry_at_us;      /* 0 = connected or connecting */
/* Audio reassembly, in PSRAM. Allocated once; never on the hot path. */
#define AUD_CAP 12288
static uint8_t    *s_aud;
static size_t      s_aud_len;
/* Outbound TX_AUDIO frame: 64-byte header + TX_CHRONO_FRAMES int16 samples.
 * PSRAM, because internal RAM is the contended resource here. */
#define TXA_BYTES  (64 + TX_CHRONO_FRAMES * 2)
static uint8_t    *s_txa;
/* The TCI connection's socket, found on connect; -1 while there is none. */
static int         s_fd = -1;
static uint16_t    s_port;
static uint32_t    s_backoff_ms = 250;

static inline uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

/* Overridden by the application so this component stays free of a haptic
 * dependency; PTT confirmations are the one place haptics are load-bearing,
 * because the protocol gives the operator no other signal. */
__attribute__((weak)) void haptic_hook(uint8_t effect, uint8_t prio)
{
    (void)effect; (void)prio;
}

static void send_cmd(const char *fmt, ...);

/* Say why a key was refused. This used to print the FSM's reason field, which
 * belongs to the last abort, not to the refusal: a link that had quietly died
 * read "PTT REFUSED (operator)". */
static void log_refusal(uint32_t missing)
{
    static const struct { uint32_t bit; const char *name; } P[] = {
        { PERMIT_LINK, "link" },           { PERMIT_TRX, "trx" },
        { PERMIT_TX_ENABLE, "tx-enable" }, { PERMIT_NO_OVERLAY, "overlay" },
        { PERMIT_NO_FAULT, "fault" },      { PERMIT_PONG_FRESH, "pong" },
        { PERMIT_NO_RECONCILE, "reconcile" },
        { PERMIT_BAND, "band" },           { PERMIT_MODE, "mode" },
    };
    if (!missing) {
        ESP_LOGW(TAG, "PTT REFUSED by AetherSDR, or not confirmed in time");
        return;
    }
    char   buf[80] = "";
    size_t n = 0;
    for (size_t i = 0; i < sizeof P / sizeof P[0] && n < sizeof buf; i++)
        if (missing & P[i].bit)
            n += snprintf(buf + n, sizeof buf - n, " %s", P[i].name);
    ESP_LOGW(TAG, "PTT REFUSED, not ready:%s", buf);
}

/* Perform whatever the FSM decided. Rungs 2-4 tear down our own socket, which
 * is a MORE reliable unkey than any command we can send: AetherSDR unkeys
 * unconditionally when a PTT-owning client disconnects, whereas a trx:false may
 * never be dispatched at all. */
static void ptt_dispatch(const ptt_out_t *o)
{
    if (o->haptic) haptic_hook(o->haptic, o->haptic_prio);

    /* ",tci" names our TX_AUDIO as the source. Without it AetherSDR keys a
     * voice mode like a foot switch: the radio transmits its own mic input,
     * no TX_CHRONO ever arrives, and the knob's microphone is never sent.
     * Only digital modes default to TCI audio. */
    if (o->send_key)   send_cmd("trx:%u,true,tci;", (unsigned)S.my_trx);
    if (o->send_unkey) send_cmd("trx:%u,false;",    (unsigned)S.my_trx);

    if (o->close_socket) {
        ESP_LOGW(TAG, "PTT ladder rung 2: closing the socket");
        esp_websocket_client_close(s_ws, pdMS_TO_TICKS(300));
    }
    if (o->destroy_socket) {
        ESP_LOGE(TAG, "PTT ladder rung 3: destroying the transport");
        esp_websocket_client_stop(s_ws);
    }
    if (o->restart) {
        ESP_LOGE(TAG, "PTT ladder rung 4: rebooting to guarantee an unkey");
        esp_restart();
    }
    /* The microphone runs only while keyed. A live mic when the operator has
     * not asked to transmit is a privacy bug, not just a wasted buffer. */
    if (o->entered_tx) { audio_in_set_active(true);  ESP_LOGW(TAG, "*** TX ***"); }
    if (o->left_tx)    { audio_in_set_active(false); ESP_LOGI(TAG, "*** RX ***"); }
    if (o->refused)    log_refusal(o->missing);
}

/* --------------------------------------------------------------- inbound */

static void apply_fact(const tci_fact_t *f)
{
    switch (f->kind) {

    case TCI_READY:
        taskENTER_CRITICAL(&S_LOCK);
        S.link       = RADIO_LINK_READY;
        S.t_ready_ms = now_ms();
        S.have_chan_sensors   = false;
        S.need_sensors_enable = true;
        S.need_audio_start    = !S.audio_blocked;
        S.audio_on            = false;
        S.t_audio_start_ms    = 0;
        /* The greeting is authoritative: adopt the rig's frequency wholesale
         * and forget anything we thought we knew. */
        tune_assign(&S.tune, S.f_server);
        S.f_committed = S.f_server;
        echo_clear(&S.echo);
        taskEXIT_CRITICAL(&S_LOCK);
        ESP_LOGI(TAG, "ready: trx=%u f=%lld mode=%s filt=%ld..%ld%s",
                 (unsigned)S.my_trx, (long long)S.f_server, S.mode,
                 (long)S.filt_lo, (long)S.filt_hi,
                 S.slice_locked ? " LOCKED" : "");
        break;

    case TCI_TRX_COUNT:
        S.n_trx = (uint8_t)f->i0;
        break;

    case TCI_ACTIVE_SLICE:
        /* We can only FOLLOW focus: the server ignores active_slice SETs and
         * set_in_focus is a stub, so a slice selector is not implementable. */
        if (f->i0 >= 0) S.my_trx = (uint8_t)f->i0;
        break;

    case TCI_VFO: {
        if (f->trx != S.my_trx || f->channel != 0) break;
        uint32_t t = now_ms();
        taskENTER_CRITICAL(&S_LOCK);
        S.f_server = f->hz;
        if (S.link != RADIO_LINK_READY) {           /* still in the greeting */
            taskEXIT_CRITICAL(&S_LOCK);
            break;
        }
        ae_class_t cls = antiecho_classify(&S.echo, f->hz, t,
                                           S.t_last_input_ms, S.t_last_send_ms);
        if (cls == AE_OUR_ECHO) {
            S.echoes++;
            taskEXIT_CRITICAL(&S_LOCK);
            break;
        }
        if (cls == AE_AMBIGUOUS) {
            S.reconcile_armed = true;             /* decide within 250 ms */
            taskEXIT_CRITICAL(&S_LOCK);
            break;
        }
        /* Unambiguous remote change: the operator tuned at the PC. */
        tune_assign(&S.tune, f->hz);
        S.f_committed = f->hz;
        echo_clear(&S.echo);
        taskEXIT_CRITICAL(&S_LOCK);
        ESP_LOGI(TAG, "remote tune -> %lld", (long long)f->hz);
        break;
    }

    case TCI_MODULATION:
        if (f->trx == S.my_trx) strlcpy(S.mode, f->s0, sizeof S.mode);
        break;

    case TCI_RX_FILTER_BAND:
        if (f->trx == S.my_trx) { S.filt_lo = f->i0; S.filt_hi = f->i1; }
        break;

    case TCI_AGC_MODE:
        if (f->trx == S.my_trx) strlcpy(S.agc, f->s0, sizeof S.agc);
        break;

    case TCI_RIT_OFFSET:
        if (f->trx == S.my_trx) S.rit_hz = f->i0;
        break;

    case TCI_LOCK:
        if (f->trx == S.my_trx) S.slice_locked = f->b0;
        break;

    case TCI_TRX:
        S.tx = f->b0;
        /* One frame, three meanings, disambiguated purely by our own state.
         * In IDLE an unsolicited trx:true is someone ELSE keying -- possibly
         * our own dead previous session -- and we must NOT try to unkey it,
         * because a non-owner's trx:false only touches its own handle. */
        {
            ptt_out_t o;
            ptt_fsm_event(&S.ptt, f->b0 ? PTT_EV_CONFIRM_TRUE
                                        : PTT_EV_CONFIRM_FALSE,
                          now_ms(), 0, &o);
            ptt_dispatch(&o);
            /* Log once per transition. This is genuinely worth knowing --
             * it means the desktop, a foot switch or another client is
             * transmitting -- but it was one line per frame, which buried
             * everything else. The UI follows the state either way. */
            if (f->b0 != s_last_remote_tx) {
                s_last_remote_tx = f->b0;
                if (f->b0 && S.ptt.state == PTT_IDLE)
                    ESP_LOGW(TAG, "transmitting, keyed elsewhere (MOX, another "
                                  "client, or a foot switch)");
            }
        }
        break;

    case TCI_TX_ENABLE:
        if (f->trx == S.my_trx) S.tx_enable_seen = f->b0;
        break;

    case TCI_TX_SENSORS:
        /* tx_sensors:0,<mic_dbm>,<fwd_w>,<peak_w>,<swr>,<alc_dbfs>;
         * Field 2 is "peak" but carries the same cached value as fwd, so it is
         * never rendered. */
        S.tx_mic_dbm = f->f0;
        S.tx_fwd_w   = f->f1;
        S.tx_peak_w  = f->f2;
        S.tx_swr     = f->f3;
        S.tx_alc     = f->f4;
        S.tx_sensor_frames++;
        /* Rate-limited: the stream is 5 Hz and only flows while transmitting,
         * so one line a second is enough to see whether it is arriving at all
         * and what range the mic figure actually uses. */
        {
            uint32_t t = now_ms();
            if (t - S.tx_sensor_log_ms > 1000) {
                S.tx_sensor_log_ms = t;
                ESP_LOGI(TAG, "tx_sensors mic=%.1f fwd=%.1f swr=%.2f alc=%.1f",
                         (double)f->f0, (double)f->f1,
                         (double)f->f3, (double)f->f4);
            }
        }
        break;

    case TCI_RX_CHANNEL_SENSORS:
        if (f->trx == S.my_trx) {
            S.have_chan_sensors = true;      /* 0.1 dB resolution */
            S.smeter_dbm = f->f0;
        }
        break;

    case TCI_RX_SMETER:
        /* Broadcast unconditionally by the server, but integer and truncated
         * toward zero. Use it whenever the finer stream is not flowing.
         *
         * This previously read "only if smeter_dbm == 0", meaning it latched
         * on the very first sample and the meter never moved again. It looked
         * correct against the mock only because the mock sends the opt-in
         * stream without being asked. */
        if (f->trx == S.my_trx && !S.have_chan_sensors)
            S.smeter_dbm = (float)f->i0;
        break;

    case TCI_UNKNOWN:
        S.unknown_cmds++;   /* named by consume(); M15 requires this to be 0 */
        break;

    default:
        break;
    }
}

static void consume(const char *data, size_t len)
{
    if (s_rx_len + len > RX_CAP) { s_rx_len = 0; return; }   /* bounded, no malloc */
    memcpy(s_rx + s_rx_len, data, len);
    s_rx_len += len;

    size_t start = 0;
    for (size_t i = 0; i < s_rx_len; i++) {
        if (s_rx[i] != ';') continue;
        tci_fact_t f;
        /* Split on ';' regardless of framing. The server sends one command per
         * frame today, but relying on that is exactly what a proxy breaks. */
        if (tci_parse(s_rx + start, i - start, &f)) {
            if (f.kind == TCI_UNKNOWN) {
                /* Name it. "zero unparsed commands over a full session" is an
                 * M15 acceptance criterion, and a bare counter cannot tell you
                 * WHICH field the research missed. */
                size_t n = i - start;
                if (n > 63) n = 63;
                char raw[64];
                memcpy(raw, s_rx + start, n);
                raw[n] = '\0';
                ESP_LOGW(TAG, "UNPARSED: %s;", raw);
            }
            apply_fact(&f);
        }
        start = i + 1;
    }
    if (start) {
        memmove(s_rx, s_rx + start, s_rx_len - start);
        s_rx_len -= start;
    }
}

/* The client keeps its socket to itself, so find it: the connection whose
 * peer is the TCI port and whose own end is ephemeral. Checking our end rules
 * out a browser on the configuration page that happens to connect from a
 * source port equal to the TCI port. */
static int find_tci_socket(void)
{
    for (int fd = LWIP_SOCKET_OFFSET;
         fd < LWIP_SOCKET_OFFSET + CONFIG_LWIP_MAX_SOCKETS; fd++) {
        struct sockaddr_in peer, self;
        socklen_t pn = sizeof peer, sn = sizeof self;
        if (getpeername(fd, (struct sockaddr *)&peer, &pn) != 0 ||
            peer.sin_family != AF_INET || ntohs(peer.sin_port) != s_port)
            continue;
        if (getsockname(fd, (struct sockaddr *)&self, &sn) != 0 ||
            ntohs(self.sin_port) < 0xC000)      /* lwIP's ephemeral range */
            continue;
        return fd;
    }
    return -1;
}

/* True when a frame can be written without waiting. lwIP reports a TCP socket
 * writable only while more than TCP_SNDLOWAT (2.8 kB here) of its send buffer
 * is free, which is well over one 1 kB frame. */
static bool socket_has_room(void)
{
    if (s_fd < 0) return true;              /* not found: send as before */
    fd_set w;
    FD_ZERO(&w);
    FD_SET(s_fd, &w);
    struct timeval now = { 0 };
    return select(s_fd + 1, NULL, &w, NULL, &now) > 0;
}

/* Answer one TX_CHRONO with one TX_AUDIO frame.
 *
 * Transmit is paced by the server: it asks every 21.33 ms and we answer, which
 * is why this is driven from the receive path rather than from a timer of our
 * own. Answering late shows up as a chrono stall server-side. If the
 * microphone has not produced enough samples we still send a full frame,
 * padded with silence -- a gap in transmitted audio is worse than quiet.
 *
 * It must never make the receive path wait, though. This runs on the
 * WebSocket task, the only reader of the socket, so a send that blocks holds
 * up every inbound frame behind it: RX audio, and the trx:false confirming an
 * unkey. At 4 kB a frame they did, a little more on every over, until a long
 * over's confirmation arrived after the PTT ladder had given up on it and the
 * knob restarted itself. A send that times out is worse again: the client
 * drops the connection. So a frame goes out only when the socket has room for
 * it, and is skipped otherwise -- 21 ms of silence is the cheapest thing that
 * can go wrong here. */
static void send_tx_audio(uint32_t receiver)
{
    S.chronos++;
    if (!s_txa || !s_ws) return;
    if (!socket_has_room()) { S.txa_skipped++; return; }

    tci_audio_hdr_t *h = (tci_audio_hdr_t *)s_txa;
    memset(h, 0, sizeof *h);
    h->receiver    = receiver;
    h->sample_rate = TX_AUDIO_RATE_HZ;   /* server resamples 1:1 from 24 kHz */
    h->format      = TCI_AUDIO_FMT_INT16;
    h->length      = TX_CHRONO_FRAMES;
    h->type        = TCI_AUDIO_TYPE_TX;
    h->channels    = 1;

    int16_t *pcm = (int16_t *)(s_txa + sizeof *h);
    if (!audio_in_take(pcm, TX_CHRONO_FRAMES))
        memset(pcm, 0, TX_CHRONO_FRAMES * sizeof(int16_t));

    const int64_t t0 = esp_timer_get_time();
    const int n = esp_websocket_client_send_bin(s_ws, (const char *)s_txa,
                                                TXA_BYTES, pdMS_TO_TICKS(50));
    const uint32_t us = (uint32_t)(esp_timer_get_time() - t0);
    if (us > S.txa_max_us) S.txa_max_us = us;
    if (n == TXA_BYTES) S.txa_sent++;
    else                S.txa_failed++;
}

/* The link is gone, however we found out. Runs on the WebSocket task. */
static void link_lost(void)
{
    /* If the link keeps dying within a moment of asking for audio, then
     * audio is what is killing it -- give up on audio rather than
     * reconnect forever. A knob that tunes without RX audio is useful; one
     * stuck in a reconnect loop is not. Seen on the USB-NCM transport,
     * where the WebSocket frame parser loses sync ~40 ms after
     * audio_start; the same code is stable over WiFi. */
    uint32_t tnow = now_ms();
    if (S.t_audio_start_ms && (tnow - S.t_audio_start_ms) < 2000 &&
        !S.audio_blocked) {
        if (++S.audio_kills >= 3) {
            S.audio_blocked = true;
            ESP_LOGE(TAG, "link died %u times just after audio_start -- "
                          "disabling RX audio for this session",
                     (unsigned)S.audio_kills);
        }
    }
    S.closes++;
    S.link = RADIO_LINK_DOWN;
    S.tx   = false;
    s_fd   = -1;

    /* AetherSDR calls abortTciPtt() when a PTT-owning client disconnects,
     * so the radio is already unkeyed. Collapse our own state to match
     * rather than continuing to climb a ladder against a dead socket. */
    if (S.ptt.state != PTT_IDLE) {
        ESP_LOGW(TAG, "link lost while %s -- server fails closed",
                 ptt_state_name(S.ptt.state));
        ptt_fsm_init(&S.ptt);
        audio_in_set_active(false);     /* never leave the mic live */
    }
    /* Schedule our own reconnect. We own this rather than the component,
     * because a safety abort deliberately CLOSES the socket, and an
     * explicit close disables the component's auto-reconnect -- which
     * would leave the knob permanently offline after the one event where
     * it most needs to come back. */
    if (!s_retry_at_us) {
        s_retry_at_us = esp_timer_get_time() + (int64_t)s_backoff_ms * 1000;
        ESP_LOGI(TAG, "reconnect in %u ms", (unsigned)s_backoff_ms);
    }
    ESP_LOGW(TAG, "link down%s%s", S.last_close[0] ? ": " : "", S.last_close);
}

static void ws_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base;
    esp_websocket_event_data_t *e = data;

    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
        S.connects++;
        S.link = RADIO_LINK_GREETING;
        s_retry_at_us = 0;
        s_rx_len = 0;
        s_greet_deadline_us = esp_timer_get_time() + GREET_TMO_MS * 1000;
        s_fd = find_tci_socket();
        if (s_fd < 0)
            ESP_LOGW(TAG, "TCI socket not found; TX audio sends may block");
        ESP_LOGI(TAG, "connected, awaiting greeting");
        break;

    case WEBSOCKET_EVENT_DATA:
        if (e->op_code == 0x0A) {                       /* pong */
            S.last_pong_us = esp_timer_get_time();
            break;
        }
        if (e->op_code == 0x08) {                       /* close */
            if (e->data_len > 2) {
                size_t n = e->data_len - 2;
                if (n >= sizeof S.last_close) n = sizeof S.last_close - 1;
                memcpy(S.last_close, e->data_ptr + 2, n);
                S.last_close[n] = '\0';
            }
            break;
        }
        if (e->op_code == 0x02 || (e->op_code == 0x00 && s_aud_len)) {
            /* Binary: RX audio, delivered in buffer_size chunks. Reassembled
             * into PSRAM so the socket buffer can stay small. */
            if (!s_aud) break;
            if (e->payload_offset == 0) s_aud_len = 0;
            if (s_aud_len + e->data_len <= AUD_CAP) {
                memcpy(s_aud + s_aud_len, e->data_ptr, e->data_len);
                s_aud_len += e->data_len;
            } else {
                s_aud_len = 0;                  /* oversize: drop the frame */
                break;
            }
            if (e->payload_offset + e->data_len >= (size_t)e->payload_len) {
                const tci_audio_hdr_t *h = (const tci_audio_hdr_t *)s_aud;
                if (s_aud_len >= sizeof *h) {
                    if (h->type == TCI_AUDIO_TYPE_RX)      audio_out_feed(s_aud, s_aud_len);
                    else if (h->type == TCI_AUDIO_TYPE_CHRONO) send_tx_audio(h->receiver);
                }
                s_aud_len = 0;
            }
            break;
        }
        if (e->op_code != 0x01 && e->op_code != 0x00) break;   /* text/continuation */
        if (e->data_len > 0) consume(e->data_ptr, e->data_len);
        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_CLOSED:
        link_lost();
        break;

    case WEBSOCKET_EVENT_FINISH:
        /* The client's task has ended. DISCONNECTED or CLOSED has usually said
         * so already, but not when a close is cut short: if the handshake
         * overruns its timeout, esp_websocket_client_close() stops the task
         * outright and FINISH is the only event there is. Rung 2 of the PTT
         * ladder is exactly such a close. Unhandled, the knob went on
         * believing the link was READY -- no reconnect, no pongs, every key
         * refused -- until it was power-cycled. */
        if (S.link != RADIO_LINK_DOWN) link_lost();
        break;

    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGW(TAG, "websocket error");
        break;
    default:
        break;
    }
}

/* Every bit required before a key is accepted. Band and mode are pinned set in
 * v1 (no band plan yet) so adding the table later does not touch the FSM. */
static uint32_t ptt_permit_now(uint32_t t)
{
    uint32_t p = PERMIT_BAND | PERMIT_MODE | PERMIT_NO_OVERLAY | PERMIT_NO_FAULT;

    if (S.link == RADIO_LINK_READY || S.link == RADIO_LINK_DEGRADED) {
        if (t - S.t_ready_ms >= 500) p |= PERMIT_LINK;
    }
    if (S.n_trx > 0 || S.my_trx == 0)       p |= PERMIT_TRX;
    if (S.tx_enable_seen)                   p |= PERMIT_TX_ENABLE;
    if (!S.reconcile_armed)                 p |= PERMIT_NO_RECONCILE;

    /* A pong within 4 s. Before the first pong arrives we allow it, otherwise
     * PTT would be unavailable for the first ping interval after connecting. */
    /* Comfortably more than two ping intervals, so an ordinary missed pong
     * does not make the transmitter unavailable mid-QSO. */
    if (!S.last_pong_us ||
        (esp_timer_get_time() - S.last_pong_us) / 1000 < 5000) p |= PERMIT_PONG_FRESH;

    return p;
}

/* --------------------------------------------------------------- outbound */

static void send_cmd(const char *fmt, ...)
{
    char buf[96];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n <= 0 || !s_ws) return;
    esp_websocket_client_send_text(s_ws, buf, n, pdMS_TO_TICKS(200));
    S.sends++;
}

static void tx_task(void *arg)
{
    (void)arg;
    TickType_t next = xTaskGetTickCount();

    for (;;) {
        vTaskDelayUntil(&next, pdMS_TO_TICKS(SEND_GATE_MS));
        uint32_t t = now_ms();

        /* Reconnect supervisor. */
        if (s_retry_at_us && esp_timer_get_time() >= s_retry_at_us) {
            s_retry_at_us = 0;
            S.link = RADIO_LINK_CONNECTING;
            esp_websocket_client_stop(s_ws);          /* idempotent */
            if (esp_websocket_client_start(s_ws) != ESP_OK) {
                s_backoff_ms = s_backoff_ms < 8000 ? s_backoff_ms * 2 : 8000;
                s_retry_at_us = esp_timer_get_time() + (int64_t)s_backoff_ms * 1000;
            } else {
                s_backoff_ms = s_backoff_ms < 8000 ? s_backoff_ms * 2 : 8000;
            }
            continue;
        }
        /* Reset the backoff once the link has been solid for a while, so a
         * long healthy session does not inherit a previous bad patch's delay. */
        if (S.link == RADIO_LINK_READY && (t - S.t_ready_ms) > 30000)
            s_backoff_ms = 250;

        if (S.link == RADIO_LINK_GREETING &&
            esp_timer_get_time() > s_greet_deadline_us) {
            ESP_LOGW(TAG, "no greeting within %d ms, reconnecting", GREET_TMO_MS);
            esp_websocket_client_close(s_ws, pdMS_TO_TICKS(200));
            continue;
        }
        /* --- PTT: intents, deadlines and the ladder --------------------- */
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
            }
            if (S.pending_unkey) {
                S.pending_unkey = 0;
                ptt_fsm_event(&S.ptt, PTT_EV_TAP_UNKEY, t, permit, &o);
                ptt_dispatch(&o);
            }
            if (S.pending_abort) {
                uint8_t r = S.pending_abort;
                S.pending_abort = 0;
                ptt_fsm_abort(&S.ptt, (ptt_abort_t)r, t, &o);
                ptt_dispatch(&o);
            }

            /* Link liveness is PONG-only, never traffic: rx_smeter is
             * suppressed below -200 dBm, so a rig with no radio attached emits
             * nothing and a traffic watchdog would false-unkey every over. */
            if (S.ptt.state == PTT_ON && S.last_pong_us) {
                int64_t age = (esp_timer_get_time() - S.last_pong_us) / 1000;
                /* Three consecutive missed 2 s pings. Tighter than this and a
                 * momentary WiFi hiccup cuts the operator off mid-word, which
                 * is its own kind of unsafe. */
                if (age > 6000) {
                    ESP_LOGE(TAG, "pong stale (%lld ms) while keyed", (long long)age);
                    ptt_fsm_abort(&S.ptt, PTT_AB_PONG_STALE, t, &o);
                    ptt_dispatch(&o);
                }
            }

            ptt_fsm_event(&S.ptt, PTT_EV_TICK, t, permit, &o);
            ptt_dispatch(&o);
        }

        if (S.link != RADIO_LINK_READY && S.link != RADIO_LINK_DEGRADED) continue;
        /* Settle after a reconnect before pushing anything. */
        if (t - S.t_ready_ms < 500) continue;

        /* Ask for the finer sensor stream once the link settles. Sent from
         * here rather than from the receive handler, which runs on the
         * WebSocket task and must not re-enter the transport. */
        if (S.need_sensors_enable) {
            S.need_sensors_enable = false;
            send_cmd("rx_sensors_enable:true;");
            /* Opt-in, like the RX sensors. Without it there is no mic level
             * and no SWR, which are exactly what you want to see while the
             * transmitter is running. */
            send_cmd("tx_sensors_enable:true;");
            ESP_LOGI(TAG, "sensor streams requested (rx + tx)");
        }

        /* Audio is declared on the SAME trx we control. effectiveTrx()
         * redirects a PTT request for trx 0 to the client's declared audio
         * receiver, so declaring a different one would key a slice the
         * operator never addressed, on that slice's band and antenna. Same
         * receiver means the redirect is a no-op. */
        /* Give the link back while a firmware image is being pushed in. RX
         * audio is a continuous ~96 kB/s inbound stream on the same socket
         * and the same USB pipe as the upload; with both running the upload
         * broke midway and the audio counters showed thousands of dropped
         * frames. Nobody needs to listen to the radio while updating it. */
        if (S.audio_suspend && S.audio_on) {
            send_cmd("audio_stop:%u;", (unsigned)S.my_trx);
            S.audio_on = false;
            S.need_audio_start = false;
            ESP_LOGW(TAG, "RX audio suspended");
        }

        if (S.need_audio_start && !S.need_sensors_enable && !S.audio_suspend) {
            S.need_audio_start = false;
            send_cmd("audio_samplerate:%d;", AUDIO_RATE_HZ);
            send_cmd("audio_stream_sample_type:int16;");
            send_cmd("audio_start:%u;", (unsigned)S.my_trx);
            S.audio_on = true;
            S.t_audio_start_ms = t;
            ESP_LOGI(TAG, "RX audio requested on trx %u at %d Hz",
                     (unsigned)S.my_trx, AUDIO_RATE_HZ);
        }

        /* No vfo: traffic while keyed. You are not tuning during a
         * transmission, and an empty server queue guarantees the trx:false
         * that ends the over is never stuck behind anything. */
        if (S.ptt.state != PTT_IDLE) continue;

        /* AetherSDR sends agc_mode in its greeting, and afterwards only when
         * another TCI client changes it -- never for a change made on the
         * desktop. So ask now and then; the reply comes to us alone. */
        if (t - S.t_agc_poll >= AGC_POLL_MS) {
            S.t_agc_poll = t;
            send_cmd("agc_mode:%u;", (unsigned)S.my_trx);
        }

        bool     fire = false;
        int64_t  want = 0;
        uint32_t period = (S.link == RADIO_LINK_DEGRADED) ? 100 : AE_SEND_PERIOD_MS;

        taskENTER_CRITICAL(&S_LOCK);
        /* Deferred reconcile: both quiet windows must have expired. */
        if (S.reconcile_armed &&
            (t - S.t_last_input_ms) >= AE_QUIET_MS &&
            (t - S.t_last_send_ms)  >= AE_QUIET_MS) {
            if (S.f_server != S.f_committed) {
                tune_assign(&S.tune, S.f_server);
                S.f_committed = S.f_server;
                echo_clear(&S.echo);
                S.reconciles++;
                S.rejects++;         /* the only rejection signal that exists */
            }
            S.reconcile_armed = false;
        }

        if (S.tune.f_display != S.f_committed) {
            bool settle = (t - S.t_last_input_ms) >= AE_SETTLE_MS;
            if (settle || (t - S.t_last_send_ms) >= period) {
                if (S.slice_locked) {
                    /* Predictable spring-back: refuse locally and put nothing
                     * on the wire at all. */
                    tune_assign(&S.tune, S.f_committed);
                    S.rejects++;
                } else {
                    want = S.tune.f_display;
                    S.f_committed   = want;
                    S.t_last_send_ms = t;
                    echo_push(&S.echo, want, t);
                    fire = true;
                }
            }
        }
        taskEXIT_CRITICAL(&S_LOCK);

        if (fire) send_cmd("vfo:%u,0,%lld;", (unsigned)S.my_trx, (long long)want);
    }
}

/* ----------------------------------------------------------------- public */

int64_t radio_tune_by(int32_t detents, uint8_t accel_mult, int32_t step_hz)
{
    uint32_t t = now_ms();
    int64_t f;
    taskENTER_CRITICAL(&S_LOCK);
    if (detents) {
        if (S.tune.step_hz != step_hz) tune_set_step(&S.tune, step_hz);
        tune_apply(&S.tune, detents, accel_mult, SEND_GATE_MS, 1000LL, 75000000LL);
        S.t_last_input_ms = t;
        S.reconcile_armed = false;  /* operator intent beats a pending reconcile */
    }
    f = S.tune.f_display;
    taskEXIT_CRITICAL(&S_LOCK);
    return f;
}

void radio_audio_suspend(bool suspend)
{
    if (S.audio_suspend == suspend) return;
    S.audio_suspend = suspend;
    /* Resuming re-arms the request; the tx task sends it on its next pass. */
    if (!suspend && !S.audio_blocked) S.need_audio_start = true;
    ESP_LOGW(TAG, "RX audio %s", suspend ? "suspend requested" : "resume requested");
}

void radio_set_step(int32_t step_hz)
{
    taskENTER_CRITICAL(&S_LOCK);
    tune_set_step(&S.tune, step_hz);
    taskEXIT_CRITICAL(&S_LOCK);
}

void radio_set_mode(const char *mode)
{
    if (mode && *mode) send_cmd("modulation:%u,%s;", (unsigned)S.my_trx, mode);
}

void radio_set_filter(int32_t lo, int32_t hi)
{
    send_cmd("rx_filter_band:%u,%ld,%ld;", (unsigned)S.my_trx, (long)lo, (long)hi);
}

/* AetherSDR's filters are passband edges, not presets. */
void radio_select_filter(uint8_t n) { (void)n; }

void radio_set_agc(const char *agc)
{
    if (!agc || !*agc) return;
    /* A client's own SET is not echoed back to it, so show it at once. The
     * poll corrects it if AetherSDR disagrees -- but not straight away, when
     * the SET, which AetherSDR applies on its next pass, may not have landed. */
    strlcpy(S.agc, agc, sizeof S.agc);
    S.t_agc_poll = now_ms();
    send_cmd("agc_mode:%u,%s;", (unsigned)S.my_trx, agc);
}

/* AetherSDR's TCI has no RF gain -- the panadapter's rfgain, -8 to +32 dB --
 * to read or to set (see TODO.md). have_gain stays false, so the dial shows
 * RF.G greyed out until it does. */
void radio_set_gain(int8_t gain) { (void)gain; }

void radio_set_rit(int32_t hz)
{
    /* Confirmed on no path whatsoever, so set the local value optimistically
     * and let a later GET correct it if the rig disagrees. */
    taskENTER_CRITICAL(&S_LOCK);
    S.rit_hz = hz;
    taskEXIT_CRITICAL(&S_LOCK);
    send_cmd("rit_offset:%u,%ld;", (unsigned)S.my_trx, (long)hz);
    send_cmd("rit_enable:%u,%s;", (unsigned)S.my_trx, hz ? "true" : "false");
}

void radio_goto_freq(int64_t hz)
{
    /* Route through the same model the knob uses, so the echo ring sees it and
     * the jump is not mistaken for a remote change. */
    uint32_t t = now_ms();
    taskENTER_CRITICAL(&S_LOCK);
    tune_assign(&S.tune, hz);
    S.t_last_input_ms = t;
    S.reconcile_armed = false;
    taskEXIT_CRITICAL(&S_LOCK);
}

/* TCI has no way to select a memory channel. */
void radio_memory_mode(bool on)        { (void)on; }
void radio_memory_group(uint8_t group) { (void)group; }

/* The slice is AetherSDR's to choose, and TCI has no antenna selection. */
void radio_select_rx(uint8_t rx)               { (void)rx; }
void radio_set_antenna(uint8_t ant, bool rx_ant) { (void)ant; (void)rx_ant; }

/* TCI has no tune carrier or tuner to start from here. */
void radio_tune(void)     {}
void radio_atu_tune(void) {}
void radio_atu_memories(bool on) { (void)on; }
void radio_set_rf_gain(uint8_t pct)  { (void)pct; }
void radio_set_rf_power(uint8_t pct) { (void)pct; }
void radio_set_tuner(bool on)        { (void)on; }
void radio_set_squelch(uint8_t pct)  { (void)pct; }

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
    memset(o, 0, sizeof *o);          /* what TCI has no word for stays zero */
    taskENTER_CRITICAL(&S_LOCK);
    o->link       = S.link;
    o->f_display  = S.tune.f_display;
    o->f_server   = S.f_server;
    o->filt_lo    = S.filt_lo;
    o->filt_hi    = S.filt_hi;
    o->rit_hz     = S.rit_hz;
    o->smeter_dbm = S.smeter_dbm;
    o->tx_mic_dbm = S.tx_mic_dbm;
    o->tx_fwd_w   = S.tx_fwd_w;
    o->tx_peak_w  = S.tx_peak_w;
    o->tx_swr     = S.tx_swr;
    o->tx_alc     = S.tx_alc;
    o->slice_locked = S.slice_locked;
    o->tx         = S.tx;
    o->my_trx     = S.my_trx;
    o->n_trx      = S.n_trx;
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
    o->pong_age_ms = S.last_pong_us
        ? (int32_t)((esp_timer_get_time() - S.last_pong_us) / 1000) : -1;
    taskEXIT_CRITICAL(&S_LOCK);
    strlcpy(o->mode, S.mode, sizeof o->mode);
    strlcpy(o->agc, S.agc, sizeof o->agc);
    strlcpy(o->last_close, S.last_close, sizeof o->last_close);
}

/* How much of a WebSocket receive buffer internal RAM can spare. */
static int ws_buffer_size(void)
{
    size_t big = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    if (big > 48000) return 8192;
    if (big > 24000) return 4096;
    return 2048;
}

const char *radio_link_name(void) { return "TCI"; }

esp_err_t radio_start(const char *host, uint16_t port,
                      const char *user, const char *pass)
{
    (void)user; (void)pass;       /* AetherSDR's TCI server has no login */
    char uri[96];
    snprintf(uri, sizeof uri, "ws://%s:%u/", host, (unsigned)port);
    s_port = port;

    memset(&S, 0, sizeof S);
    tune_init(&S.tune, 14074000, 100);
    accel_init(&S.accel);
    ptt_fsm_init(&S.ptt);
    strlcpy(S.mode, "usb", sizeof S.mode);

    if (!s_aud) {
        s_aud = heap_caps_malloc(AUD_CAP, MALLOC_CAP_SPIRAM);
        if (!s_aud) ESP_LOGW(TAG, "no PSRAM for audio reassembly; RX audio off");
    }
    if (!s_txa) s_txa = heap_caps_malloc(TXA_BYTES, MALLOC_CAP_SPIRAM);
    s_aud_len = 0;

    esp_websocket_client_config_t cfg = {
        .uri                    = uri,
        /* We own reconnect. Two reasons: the close REASON string is the
         * richest error channel this protocol has, and an explicit close --
         * which the PTT safety ladder performs deliberately -- suppresses the
         * component's own auto-reconnect entirely. */
        .disable_auto_reconnect = true,
        .network_timeout_ms     = 5000,
        /* 2 s, not the default 10. Liveness is PONG-based (traffic cannot be
         * used: rx_smeter is suppressed below -200 dBm, so a rig with no radio
         * attached is silent). At a 10 s ping interval the freshness test below
         * would block PTT for six seconds out of every ten. */
        .ping_interval_sec      = 2,
        .pingpong_timeout_sec   = 8,
        .task_prio              = 6,
        .task_stack             = 6144,
        /* Core 0, beside lwIP: the client left to itself floats, and it
         * follows the display to core 1 -- where it takes 40% of the core
         * receiving audio, 60% transmitting, and starves the idle task. */
        .task_core_id_set       = true,
        .task_core_id           = 0,
        /* Sized to what internal RAM can actually spare right now.
         *
         * 2 kB was forced by the WiFi path, where LVGL, the WiFi driver and
         * I2S leave barely enough for this client's task stack. On the USB
         * path WiFi has been shut down by the time we get here, which frees
         * about 40 kB -- and the small buffer turned out to matter: within
         * 70 ms of audio_start the frame parser lost sync ("Non-zero RSV bits
         * detected") and the link entered a reconnect loop. Audio over the
         * cable arrives far faster than over WiFi, so a whole frame no longer
         * fits between reads. Fragments are still reassembled in PSRAM. */
        .buffer_size            = ws_buffer_size(),
    };
    s_ws = esp_websocket_client_init(&cfg);
    ESP_RETURN_ON_FALSE(s_ws, ESP_FAIL, TAG, "ws init");
    esp_err_t err = esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY,
                                                  ws_event, NULL);
    /* Set the state BEFORE starting. esp_websocket_client_start() launches the
     * connection on its own task, and over the USB cable the handshake and the
     * TCI greeting can both land before this function reaches its next few
     * statements -- about 30 ms, measured. Assigning CONNECTING afterwards
     * then overwrote the READY the receive handler had already set, and since
     * "ready;" arrives exactly once the link never recovered: a healthy,
     * ESTABLISHED socket stuck at CONNECTING forever, sending nothing. Over
     * WiFi the handshake was always slower than these instructions, so it
     * never showed. */
    S.link = RADIO_LINK_CONNECTING;
    if (err == ESP_OK) err = esp_websocket_client_start(s_ws);
    if (err != ESP_OK) {
        /* Destroy the handle before returning. Leaving it allocated leaked an
         * entire client per attempt, and because the caller retries every 2 s
         * the device walked itself down to 23 bytes of internal RAM and stayed
         * there -- a far worse failure than the one being retried. */
        esp_websocket_client_destroy(s_ws);
        s_ws = NULL;
        S.link = RADIO_LINK_DOWN;
        ESP_LOGE(TAG, "websocket start failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Check this. An unchecked failure here is the nastiest outcome the client
     * has: the socket connects, the greeting parses, the S-meter moves and the
     * status line says READY -- while nothing is ever transmitted, because
     * every outbound command goes through tx_task. The knob looks connected
     * and simply does not tune. It is also the allocation most likely to fail,
     * being 4 kB of internal RAM asked for last. */
    if (xTaskCreatePinnedToCore(tx_task, "tci_tx", 4096, NULL, 8, NULL, 0)
        != pdPASS) {
        ESP_LOGE(TAG, "no internal RAM for tci_tx (%u free, largest %u) -- "
                      "refusing to run receive-only",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        esp_websocket_client_stop(s_ws);
        esp_websocket_client_destroy(s_ws);
        s_ws = NULL;
        S.link = RADIO_LINK_DOWN;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "connecting to %s", uri);
    return ESP_OK;
}
