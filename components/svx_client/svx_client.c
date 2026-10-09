/* The svxconnect firmware's radio: an SvxLink reflector, over WiFi.
 *
 * SVXConnect-CLI's reflector client (MIT, (c) 2026 Joeri Van Dooren) on the
 * knob. Its protocol, talkgroup manager, node info and codec are shared as
 * they are (svx/); what the CLI does with files, OpenSSL and miniaudio is done
 * here with NVS, mbedTLS and the knob's own speaker and microphone. radio.h
 * maps onto a reflector like this:
 *
 *   the dial        steps through the switchable talkgroups
 *   the frequency   is the talkgroup's name, from the reflector's portal
 *   the S-meter     is the received audio, and who is talking
 *   AGC's place     the talkgroup lock: no switching, by the dial or priority
 *   the gain's      mute
 *   PTT             keys the talkgroup. The reflector's TalkerStart for our
 *                   own callsign confirms it; on a busy talkgroup, where the
 *                   reflector gives us no floor, the key is refused.
 *
 * One task does it all -- control connection, audio channel, Opus -- on core
 * 0 beside lwIP, leaving core 1 to the dial and the display.
 */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <time.h>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "kvstore.h"

#include "audio_in.h"
#include "audio_out.h"
#include "ptt_fsm.h"
#include "radio.h"

#include "audio/codec.h"
#include "common/proto.h"
#include "common/util.h"
#include "reflector/nodeinfo.h"
#include "tg/tgmanager.h"

#include "svx_client.h"
#include "svx_feed.h"
#include "svx_net.h"
#include "svx_pki.h"
#include "svx_portal.h"
#include "svx_udp.h"

static const char *TAG = "svx";

#define CONNECT_MS        10000     /* per address                          */
#define STEP_MS           15000     /* per handshake frame                  */
#define TLS_MS            10000     /* the handshake; it takes under a second */
#define TCP_SILENCE_MS    30000     /* nothing on the control connection    */
#define UDP_SILENCE_MS    60000     /* nothing on audio, once it has worked */
#define ENROLL_RETRY_MS   30000     /* while the sysop has not signed yet   */
#define FRAME_CAP          4096     /* the largest frame we send: a request */
#define FRESH_MS          20000     /* the reflector answered this recently */
#define LEVELS               64     /* received levels, one per 20 ms frame */
#define SILENT_DB        -90.0f

static const uint16_t BACKOFF_S[] = { 3, 3, 5, 10, 20, 30, 60 };

/* `ms` or more since `then`. Signed: `then` may be later than a `now` read
 * before the I/O that set it, and an unsigned difference would wrap to 49
 * days of silence. */
static inline bool since(uint64_t now, uint64_t then, uint32_t ms)
{
    return (int64_t)(now - then) >= (int64_t)ms;
}

typedef enum {
    END_NONE = 0,       /* still going */
    END_FAILED,         /* could not connect, or the protocol broke: back off */
    END_CLOSED,         /* a session that worked has ended: reconnect soon */
    END_CERT_STORED,    /* a certificate arrived: log in with it now */
    END_CERT_REFUSED,   /* ours was refused: the next login asks for a new one */
    END_AWAIT_SYSOP,    /* the request is with the reflector: ask again later */
    END_RELOAD,         /* the settings changed */
} end_t;

/* ------------------------------------------------------------------ state */

/* The reflector task's own. Several kB of talkgroup bookkeeping: PSRAM. */
EXT_RAM_BSS_ATTR static struct {
    svx_settings_t set;
    svx_config  cfg;
    char        host[64];
    uint16_t    port;
    char        server[72];         /* host:port after the SRV lookup */
    tg_manager  tgm;
    svx_codec  *codec;
    svx_link_t  link;
    svx_udp_t   udp;
    bool        up;                 /* logged in: frames may be sent */
    uint64_t    t_ready, t_tcp_rx, t_hb;
    bool        udp_seen;
    int         backoff;
    uint64_t    t_retry;
    uint8_t     frame[FRAME_CAP];
    int         nodes;
    char        phase[48], why[80], refl_err[80];
    /* transmit */
    ptt_fsm_t   ptt;
    bool        tx_on;
    uint64_t    t_next_frame, t_tx_start;
    uint32_t    over_frames, over_silent, enc_max_us, enc_total_us;
    float       mic_db;
    /* receive */
    int16_t     pcm[1280];          /* up to 80 ms decoded */
    float       lvl[LEVELS];
    uint32_t    lvl_n;
    uint64_t    t_audio;
    uint32_t    rx_packets, rx_concealed;
    bool        muted;
} C;

/* What radio_get_status() reports, and the requests other tasks leave for the
 * reflector task. */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static struct {
    radio_link_t link;
    uint32_t tg;
    char     tg_name[32], talker[16], talker_info[32], last_talker[16], server[40];
    uint64_t talker_t;              /* the talker's start, or the last one's stop */
    bool     locked, muted, tx;
    float    rx_db, mic_db;
    uint8_t  ptt_state, ptt_rung, ptt_reason;
    uint32_t ptt_refusals, permit;
    uint32_t connects, closes, rejects, sends, txa_sent, txa_failed, txa_max_us;
    int32_t  pong_age_ms;
    char     last_close[48];
} P;

static volatile int32_t s_detents;
static volatile uint8_t s_lock_req;         /* 0 none, 1 unlock, 2 lock */
static volatile bool    s_mute_req, s_mute_val;
static volatile bool    s_key, s_unkey, s_toggle;
static volatile uint8_t s_abort;
static volatile bool    s_reload, s_enroll_start, s_enroll_stop;
static volatile bool    s_started;

/* Overridden by the application; see tci_client.c. */
__attribute__((weak)) void haptic_hook(uint8_t effect, uint8_t prio)
{
    (void)effect; (void)prio;
}

/* ------------------------------------------------------------- settings */

#define NS "svx"

static void kv_str(kv_handle_t h, const char *k, char *out, size_t cap, const char *dflt)
{
    size_t n = cap;
    if (kv_get_str(h, k, out, &n) != ESP_OK) snprintf(out, cap, "%s", dflt);
}

/* From the settings in RAM (kvstore): any stack will do. */
static void settings_load(svx_settings_t *s)
{
    memset(s, 0, sizeof *s);
    /* The CLI's defaults. */
    s->linger_s = 30;
    s->idle_s = 60;
    s->roger = true;
    s->agc = true;
    s->tx_timeout_s = 120;
    s->feed = true;

    kv_handle_t h;
    if (kv_open(NS, &h) != ESP_OK) return;
    kv_str(h, "call",  s->call,     sizeof s->call, "");
    kv_str(h, "email", s->email,    sizeof s->email, "");
    kv_str(h, "loc",   s->location, sizeof s->location, "");
    kv_str(h, "lat",   s->lat,      sizeof s->lat, "");
    kv_str(h, "lon",   s->lon,      sizeof s->lon, "");
    kv_str(h, "sw",    s->sw,       sizeof s->sw, "");
    kv_str(h, "mon",   s->mon,      sizeof s->mon, "");
    uint8_t b;
    uint16_t w;
    kv_get_u32(h, "deftg", &s->default_tg);
    if (kv_get_u8(h, "lock", &b) == ESP_OK)    s->lock_on_start = b;
    if (kv_get_u16(h, "linger", &w) == ESP_OK) s->linger_s = w;
    if (kv_get_u16(h, "idle", &w) == ESP_OK)   s->idle_s = w;
    if (kv_get_u8(h, "roger", &b) == ESP_OK)   s->roger = b;
    if (kv_get_u8(h, "agc", &b) == ESP_OK)     s->agc = b;
    if (kv_get_u16(h, "txto", &w) == ESP_OK)   s->tx_timeout_s = w;
    if (kv_get_u8(h, "feed", &b) == ESP_OK)    s->feed = b;
    kv_close(h);
}

/* The fifteen reach the medium together; the page waits until they have. */
static esp_err_t settings_store(const svx_settings_t *s)
{
    kv_handle_t h;
    esp_err_t e = kv_open(NS, &h);
    if (e != ESP_OK) return e;
    kv_edit_begin(h);
    e = kv_set_str(h, "call", s->call);
    if (e == ESP_OK) e = kv_set_str(h, "email", s->email);
    if (e == ESP_OK) e = kv_set_str(h, "loc", s->location);
    if (e == ESP_OK) e = kv_set_str(h, "lat", s->lat);
    if (e == ESP_OK) e = kv_set_str(h, "lon", s->lon);
    if (e == ESP_OK) e = kv_set_str(h, "sw", s->sw);
    if (e == ESP_OK) e = kv_set_str(h, "mon", s->mon);
    if (e == ESP_OK) e = kv_set_u32(h, "deftg", s->default_tg);
    if (e == ESP_OK) e = kv_set_u8(h, "lock", s->lock_on_start);
    if (e == ESP_OK) e = kv_set_u16(h, "linger", s->linger_s);
    if (e == ESP_OK) e = kv_set_u16(h, "idle", s->idle_s);
    if (e == ESP_OK) e = kv_set_u8(h, "roger", s->roger);
    if (e == ESP_OK) e = kv_set_u8(h, "agc", s->agc);
    if (e == ESP_OK) e = kv_set_u16(h, "txto", s->tx_timeout_s);
    if (e == ESP_OK) e = kv_set_u8(h, "feed", s->feed);
    kv_edit_end(h);
    if (e == ESP_OK) e = kv_commit_wait(h, 3000);
    kv_close(h);
    return e == ESP_ERR_TIMEOUT ? ESP_OK : e;
}

/* svx_config from the settings: the form the shared modules take. */
static void cfg_build(void)
{
    const svx_settings_t *s = &C.set;
    svx_config *c = &C.cfg;
    memset(c, 0, sizeof *c);
    snprintf(c->callsign, sizeof c->callsign, "%s", s->call);
    str_upper(c->callsign);
    snprintf(c->email, sizeof c->email, "%s", s->email);
    snprintf(c->reflector, sizeof c->reflector, "%s", C.host);
    c->port = C.port;
    c->latitude  = s->lat[0] ? strtod(s->lat, NULL) : 0.0;
    c->longitude = s->lon[0] ? strtod(s->lon, NULL) : 0.0;
    snprintf(c->location, sizeof c->location, "%s", s->location);
    c->n_switchable = tglist_parse(s->sw, c->switchable, SVX_MAX_TG);
    c->n_monitored  = tglist_parse(s->mon, c->monitored, SVX_MAX_TG);
    if (c->n_switchable < 0) c->n_switchable = 0;
    if (c->n_monitored < 0)  c->n_monitored = 0;
    /* Nothing chosen: the talkgroups the reflector's portal names, so the dial
     * has something to step through on a fresh knob. */
    if (!c->n_switchable && !c->n_monitored) {
        uint32_t ids[SVX_MAX_TG];
        int n = svx_portal_ids(ids, SVX_MAX_TG);
        for (int i = 0; i < n; i++) c->switchable[i].id = ids[i];
        c->n_switchable = n;
    }
    c->default_tg         = (int)s->default_tg;
    c->lock_on_start      = s->lock_on_start;
    c->linger_seconds     = CLAMP((int)s->linger_s, 10, 300);
    c->idle_seconds       = CLAMP((int)s->idle_s, 0, 3600);
    c->mic_agc            = s->agc;
    c->mic_agc_target_pct = 30;
    c->tail_trim_ms       = 0;
    c->roger_beep         = s->roger;
    c->roger_beep_min_sec = 3;
    c->tx_timeout_sec     = s->tx_timeout_s;
}

void svx_settings_get(svx_settings_t *s)
{
    settings_load(s);
}

static bool valid_call(const char *c)
{
    size_t n = strlen(c);
    if (n < 3 || n > 20) return false;
    for (; *c; c++)
        if (!((*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '-' || *c == '/'))
            return false;
    return true;
}

esp_err_t svx_settings_set(const svx_settings_t *in, const char **why)
{
    static const char *none = "";
    *why = none;
    svx_settings_t s = *in;
    str_upper(s.call);
    if (s.call[0] && !valid_call(s.call)) {
        *why = "a callsign is letters, digits, '-' and '/'";
        return ESP_ERR_INVALID_ARG;
    }
    svx_tg_entry tmp[SVX_MAX_TG];
    if (tglist_parse(s.sw, tmp, SVX_MAX_TG) < 0 || tglist_parse(s.mon, tmp, SVX_MAX_TG) < 0) {
        *why = "talkgroups are numbers, each with up to three '+' for priority";
        return ESP_ERR_INVALID_ARG;
    }
    if ((s.lat[0] && fabs(strtod(s.lat, NULL)) > 90.0) ||
        (s.lon[0] && fabs(strtod(s.lon, NULL)) > 180.0)) {
        *why = "latitude is -90 to 90, longitude -180 to 180";
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t e = settings_store(&s);
    if (e != ESP_OK) { *why = "could not be stored"; return e; }
    s_reload = true;
    return ESP_OK;
}

/* ------------------------------------------------------------ published */

static void set_phase(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void set_phase(const char *fmt, ...)
{
    char b[48];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    if (strcmp(b, C.phase) != 0) ESP_LOGI(TAG, "%s", b);
    taskENTER_CRITICAL(&s_mux);
    memcpy(C.phase, b, sizeof b);
    taskEXIT_CRITICAL(&s_mux);
}

static void set_why(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void set_why(const char *fmt, ...)
{
    char b[80];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    ESP_LOGW(TAG, "%s", b);
    taskENTER_CRITICAL(&s_mux);
    memcpy(C.why, b, sizeof b);
    strlcpy(P.last_close, b, sizeof P.last_close);
    taskEXIT_CRITICAL(&s_mux);
}

static void set_link(radio_link_t l, const char *face)
{
    taskENTER_CRITICAL(&s_mux);
    P.link = l;
    if (face) strlcpy(P.server, face, sizeof P.server);
    taskEXIT_CRITICAL(&s_mux);
}

void svx_state(svx_state_t *o)
{
    memset(o, 0, sizeof *o);
    taskENTER_CRITICAL(&s_mux);
    memcpy(o->phase, C.phase, sizeof o->phase);
    memcpy(o->why, C.why, sizeof o->why);
    memcpy(o->server, C.server, sizeof o->server);
    o->nodes = C.nodes;
    o->tg    = P.tg;
    o->up    = P.link == RADIO_LINK_READY;
    taskEXIT_CRITICAL(&s_mux);
}

/* ---------------------------------------------------------------- frames */

static bool send_frame(size_t len)
{
    if (C.link.fd < 0) return false;
    if (svx_link_send(&C.link, C.frame, len, 3000) != 0) {
        ESP_LOGW(TAG, "send failed");
        return false;
    }
    P.sends++;
    return true;
}

static void send_heartbeat(void)
{
    size_t n;
    if (proto_build_heartbeat(C.frame, FRAME_CAP, &n) == 0) send_frame(n);
    C.t_hb = now_ms();
}

static void send_csr(void)
{
    char *csr = svx_pki_csr_pem();
    size_t n;
    if (csr && proto_build_client_csr(C.frame, FRAME_CAP, &n, csr, strlen(csr)) == 0 &&
        send_frame(n))
        ESP_LOGI(TAG, "certificate request sent for %s <%s>", C.cfg.callsign, C.cfg.email);
    else
        ESP_LOGE(TAG, "the reflector asked for a certificate request, and there is none");
    free(csr);
}

/* ---------------------------------------------------------------- audio */

static void level_push(const int16_t *pcm, int n)
{
    for (int o = 0; o < n; o += SVX_FRAME) {
        const int k = MIN(SVX_FRAME, n - o);
        const float pk = codec_peak(pcm + o, k);
        C.lvl[C.lvl_n++ % LEVELS] = pk > 1e-5f ? 20.0f * log10f(pk) : SILENT_DB;
    }
    C.t_audio = now_ms();
}

/* The level of what the speaker is playing now, not of the newest packet:
 * the playback buffer holds a tenth of a second or more, and a meter that
 * ran ahead of the voice would look broken. */
static float level_now(uint64_t t)
{
    const uint32_t queued = C.muted ? 0 : (uint32_t)(audio_out_queued() / SVX_FRAME);
    if (since(t, C.t_audio, 250) && queued == 0) return SILENT_DB;
    const uint32_t behind = MIN(queued, LEVELS - 1);
    if (behind >= C.lvl_n) return SILENT_DB;
    return C.lvl[(C.lvl_n - 1 - behind) % LEVELS];
}

static void play(const int16_t *pcm, int n)
{
    level_push(pcm, n);
    if (!C.muted) audio_out_feed_pcm16(pcm, (size_t)n, 1);
}

static void on_audio(const uint8_t *opus, size_t len, int gap)
{
    if (C.tx_on || !C.codec) return;     /* never into our own microphone */
    /* Conceal what was lost BEFORE decoding what arrived, so the decoder's
     * state stays continuous. */
    for (int i = 0; i < MIN(gap, 8); i++) {
        int n = codec_decode(C.codec, NULL, 0, C.pcm, SVX_FRAME);
        if (n > 0) play(C.pcm, n);
        C.rx_concealed++;
    }
    int n = codec_decode(C.codec, opus, (int)len, C.pcm, (int)(sizeof C.pcm / sizeof C.pcm[0]));
    if (n > 0) play(C.pcm, n);
    C.rx_packets++;
}

/* 800 Hz for 125 ms with 10 ms ramps, SVXConnect-CLI's roger beep; `count`
 * of them 62.5 ms apart. The volume is the speaker's. */
static void beep(int count)
{
    static int16_t tone[2000];
    static bool made;
    if (!made) {
        made = true;
        for (int i = 0; i < 2000; i++) {
            float env = 1.0f;
            if (i < 160)         env = i / 160.0f;
            else if (i >= 1840)  env = (2000 - i) / 160.0f;
            tone[i] = (int16_t)(0.35f * 32767.0f * env * sinf(2.0f * (float)M_PI * 800.0f * i / SVX_RATE));
        }
    }
    if (C.muted || C.tx_on) return;
    static const int16_t gap[1000] = { 0 };
    for (int b = 0; b < count; b++) {
        if (b) audio_out_feed_pcm16(gap, 1000, 1);
        audio_out_feed_pcm16(tone, 2000, 1);
    }
    audio_out_kick();
}

/* ---------------------------------------------------- talkgroup manager */

static void cb_select(void *u, uint32_t tg, int gate)
{
    (void)u;
    if (gate) {
        /* A real change of channel: what is buffered is the old one's. */
        audio_out_flush();
        if (C.ptt.state != PTT_IDLE) {
            ESP_LOGW(TAG, "talkgroup changed while keyed: unkeying");
            s_abort = PTT_AB_OPERATOR;
        }
    }
    if (!C.up) return;
    size_t n;
    if (proto_build_select_tg(C.frame, FRAME_CAP, &n, tg) == 0) send_frame(n);
    /* And at once on UDP, as the CLI does: the reflector ties the audio
     * channel to the talkgroup by it. */
    svx_udp_send(&C.udp, UDP_MSG_HEARTBEAT, NULL, 0);
}

static void cb_monitor(void *u, const uint32_t *ids, size_t n)
{
    (void)u;
    if (!C.up) return;
    size_t len;
    if (proto_build_tg_monitor(C.frame, FRAME_CAP, &len, ids, MIN(n, (size_t)SVX_MAX_TG)) == 0)
        send_frame(len);
}

static void cb_beep(void *u, int count) { (void)u; beep(count); }
static void cb_changed(void *u) { (void)u; }
static void cb_tail_trim(void *u, int ms) { (void)u; (void)ms; }   /* tail_trim_ms is 0 */

static void tgm_start(void)
{
    const tgm_callbacks cb = {
        .select_tg = cb_select, .set_monitor = cb_monitor, .beep = cb_beep,
        .changed = cb_changed, .tail_trim = cb_tail_trim,
    };
    tgm_init(&C.tgm, &C.cfg, &cb);
}

/* The dial: detents through the switchable list, all of a turn at once. The
 * lock does not stop it: the lock holds you where you put yourself, against a
 * busier talkgroup and the drop to monitoring when it is quiet -- and the dial
 * is you, so the talkgroup it turns to is the one held, locked as the last. */
static void dial(int32_t d)
{
    const svx_config *c = &C.cfg;
    if (!d || !c->n_switchable) return;
    if (C.ptt.state != PTT_IDLE) return;
    int idx = -1;
    for (int i = 0; i < c->n_switchable; i++)
        if (c->switchable[i].id == tgm_selected(&C.tgm)) { idx = i; break; }
    const int n = c->n_switchable;
    if (idx < 0) idx = d > 0 ? -1 : n;           /* from monitoring: the ends */
    idx = ((idx + d) % n + n) % n;
    tgm_select(&C.tgm, c->switchable[idx].id);
}

/* -------------------------------------------------------------------- PTT */

static uint32_t permit_now(uint64_t t)
{
    uint32_t p = PERMIT_TRX | PERMIT_NO_OVERLAY | PERMIT_NO_FAULT | PERMIT_NO_RECONCILE;
    if (C.codec) p |= PERMIT_TX_ENABLE;        /* no Opus, nothing to send */
    if (C.up && since(t, C.t_ready, 300) && C.udp.keyed_tx) p |= PERMIT_LINK;
    if (C.up && !since(t, C.t_tcp_rx, FRESH_MS)) p |= PERMIT_PONG_FRESH;
    /* A talkgroup to talk on: monitoring only has none. */
    const uint32_t sel = tgm_selected(&C.tgm);
    if (sel) p |= PERMIT_BAND;
    /* ...and nobody else on it: the reflector gives the floor to one. */
    const tgm_talker *tk = sel ? tgm_talker_on(&C.tgm, sel) : NULL;
    if (!tk || str_ieq(tk->full, C.cfg.callsign)) p |= PERMIT_MODE;
    return p;
}

static void tx_start(void)
{
    if (C.tx_on) return;
    if (C.codec) codec_reset(C.codec);
    codec_set_agc(C.codec, C.cfg.mic_agc, C.cfg.mic_agc_target_pct);
    audio_out_flush();                   /* nothing playing into the microphone */
    audio_in_set_active(true);
    C.tx_on = true;
    C.t_tx_start = C.t_next_frame = now_ms();
    C.over_frames = C.over_silent = C.enc_max_us = C.enc_total_us = 0;
    C.mic_db = SILENT_DB;
    tgm_note_local_tx(&C.tgm, C.t_tx_start);
    ESP_LOGI(TAG, "keying TG %lu", (unsigned long)tgm_selected(&C.tgm));
}

static void tx_stop(void)
{
    if (!C.tx_on) return;
    C.tx_on = false;
    audio_in_set_active(false);
    /* The end of the over, on the audio channel: the reflector passes it on
     * so everyone's playout drains instead of waiting for more. */
    if (C.up && C.over_frames) svx_udp_send(&C.udp, UDP_MSG_FLUSH_SAMPLES, NULL, 0);
    if (C.over_frames)
        ESP_LOGI(TAG, "over: %lu frames (%lu silent), %lu ms; Opus %lu us a frame, %lu at most",
                 (unsigned long)C.over_frames, (unsigned long)C.over_silent,
                 (unsigned long)(now_ms() - C.t_tx_start),
                 (unsigned long)(C.enc_total_us / C.over_frames),
                 (unsigned long)C.enc_max_us);
    C.mic_db = SILENT_DB;
}

static void ptt_dispatch(const ptt_out_t *o);

static void ptt_event(ptt_ev_t ev)
{
    ptt_out_t o;
    ptt_fsm_event(&C.ptt, ev, (uint32_t)now_ms(), permit_now(now_ms()), &o);
    ptt_dispatch(&o);
}

static void ptt_dispatch(const ptt_out_t *o)
{
    /* The microphone closes before anything else happens, the motor last:
     * it sits next to the microphone, and must never be heard on the air. */
    if (o->refused || o->left_tx) tx_stop();
    if (o->send_key) tx_start();
    if (o->restart) {
        ESP_LOGE(TAG, "PTT ladder rung 4: rebooting");
        esp_restart();
    }
    if (o->entered_tx) ESP_LOGW(TAG, "*** TX *** on TG %lu", (unsigned long)tgm_selected(&C.tgm));
    if (o->left_tx) ESP_LOGI(TAG, "*** RX ***");
    if (o->refused) {
        const uint32_t m = o->missing;
        if (!m)                     ESP_LOGW(TAG, "PTT REFUSED: the reflector gave us no floor");
        else if (m & PERMIT_LINK)   ESP_LOGW(TAG, "PTT REFUSED: not connected");
        else if (m & PERMIT_BAND)   ESP_LOGW(TAG, "PTT REFUSED: no talkgroup selected");
        else if (m & PERMIT_MODE)   ESP_LOGW(TAG, "PTT REFUSED: someone else is talking");
        else                        ESP_LOGW(TAG, "PTT REFUSED, not ready: 0x%03lx", (unsigned long)m);
        /* Not connected: try now, rather than at the end of the backoff. */
        if ((m & PERMIT_LINK) && !C.up) C.t_retry = 0;
    }
    /* The haptic gate reads this: it must see the state the machine is in
     * now, not the one the last publish() saw. */
    taskENTER_CRITICAL(&s_mux);
    P.ptt_state = (uint8_t)C.ptt.state;
    P.tx = C.ptt.state == PTT_ON;
    taskEXIT_CRITICAL(&s_mux);
    if (o->haptic) haptic_hook(o->haptic, o->haptic_prio);
    if (o->send_unkey || o->close_socket || o->destroy_socket) {
        /* Our transmission is our own audio: it ends the moment we stop
         * sending it, so the unkey is confirmed here and now. */
        tx_stop();
        ptt_event(PTT_EV_CONFIRM_FALSE);
    }
}

static void ptt_step(uint64_t t)
{
    ptt_out_t o;
    const uint32_t permit = permit_now(t);
    if (s_toggle) {
        s_toggle = false;
        if (C.ptt.state == PTT_IDLE) s_key = true; else s_unkey = true;
    }
    if (s_key) {
        s_key = false;
        ptt_fsm_event(&C.ptt, PTT_EV_TAP_KEY, (uint32_t)t, permit, &o);
        ptt_dispatch(&o);
    }
    if (s_unkey) {
        s_unkey = false;
        ptt_fsm_event(&C.ptt, PTT_EV_TAP_UNKEY, (uint32_t)t, permit, &o);
        ptt_dispatch(&o);
    }
    if (s_abort) {
        const uint8_t r = s_abort;
        s_abort = 0;
        ptt_fsm_abort(&C.ptt, (ptt_abort_t)r, (uint32_t)t, &o);
        ptt_dispatch(&o);
    }
    if (C.ptt.state == PTT_ON && C.cfg.tx_timeout_sec > 0 &&
        since(t, C.t_tx_start, (uint32_t)C.cfg.tx_timeout_sec * 1000)) {
        ESP_LOGW(TAG, "transmit timeout after %d s", C.cfg.tx_timeout_sec);
        ptt_fsm_abort(&C.ptt, PTT_AB_OPERATOR, (uint32_t)t, &o);
        ptt_dispatch(&o);
    }
    ptt_fsm_event(&C.ptt, PTT_EV_TICK, (uint32_t)t, permit, &o);
    ptt_dispatch(&o);
}

/* 20 ms of microphone per Opus packet, paced by the clock the microphone
 * shares with esp_timer. */
static void tx_pump(uint64_t t)
{
    static uint8_t opus[SVX_MAX_OPUS];
    while (C.tx_on && (int64_t)(t - C.t_next_frame) >= 0) {
        int16_t *pcm = C.pcm;
        if (!audio_in_take(pcm, SVX_FRAME)) {
            memset(pcm, 0, SVX_FRAME * sizeof *pcm);   /* priming: quiet, not a gap */
            C.over_silent++;
        }
        codec_agc(C.codec, pcm, SVX_FRAME);
        const float pk = codec_peak(pcm, SVX_FRAME);
        C.mic_db = pk > 1e-5f ? 20.0f * log10f(pk) : SILENT_DB;

        const int64_t t0 = esp_timer_get_time();
        const int n = codec_encode(C.codec, pcm, SVX_FRAME, opus, sizeof opus);
        const uint32_t us = (uint32_t)(esp_timer_get_time() - t0);
        C.enc_total_us += us;
        if (us > C.enc_max_us) C.enc_max_us = us;
        if (n > 0 && svx_udp_send(&C.udp, UDP_MSG_AUDIO, opus, (size_t)n) == 0) {
            C.over_frames++;
            P.txa_sent++;
        } else {
            P.txa_failed++;
        }
        if (us > P.txa_max_us) P.txa_max_us = us;
        C.t_next_frame += 1000 * SVX_FRAME / SVX_RATE;
        /* Far behind -- a stall somewhere -- is not made up in a burst. */
        if ((int64_t)(t - C.t_next_frame) > 200) C.t_next_frame = t;
    }
}

/* ------------------------------------------------------- requests, face */

static void requests(uint64_t t)
{
    const int32_t d = __atomic_exchange_n(&s_detents, 0, __ATOMIC_RELAXED);
    dial(d);
    if (s_lock_req) {
        tgm_set_lock(&C.tgm, s_lock_req == 2);
        s_lock_req = 0;
    }
    if (s_mute_req) {
        s_mute_req = false;
        C.muted = s_mute_val;
        if (C.muted) audio_out_flush();
        ESP_LOGI(TAG, "%s", C.muted ? "muted" : "unmuted");
    }
    ptt_step(t);
    tx_pump(t);
}

static void publish(uint64_t t)
{
    const uint32_t sel = tgm_selected(&C.tgm);
    const tgm_talker *tk = sel ? tgm_talker_on(&C.tgm, sel) : NULL;
    const tgm_recent *last = NULL;
    for (int i = 0; i < C.tgm.n_recent && !tk; i++)
        if (C.tgm.recent[i].tg == sel) { last = &C.tgm.recent[i]; break; }
    const char *name = svx_portal_name(sel);
    const float rx = level_now(t);
    char where[32] = "";
    if (tk) svx_feed_where(tk->full, where, sizeof where);
    const uint32_t permit = permit_now(t);

    taskENTER_CRITICAL(&s_mux);
    P.tg = sel;
    strlcpy(P.tg_name, name ? name : "", sizeof P.tg_name);
    P.talker[0] = P.last_talker[0] = 0;
    memcpy(P.talker_info, where, sizeof P.talker_info);
    if (tk) {
        strlcpy(P.talker, tk->full, sizeof P.talker);
        P.talker_t = tk->start_ms;
    } else if (last) {
        strlcpy(P.last_talker, last->full, sizeof P.last_talker);
        P.talker_t = last->stop_ms;
    }
    P.locked     = tgm_locked(&C.tgm);
    P.muted      = C.muted;
    P.rx_db      = rx;
    P.mic_db     = C.mic_db;
    P.ptt_state  = (uint8_t)C.ptt.state;
    P.ptt_rung   = C.ptt.rung;
    P.ptt_reason = (uint8_t)C.ptt.reason;
    P.ptt_refusals = C.ptt.refusals;
    P.permit     = permit;
    P.pong_age_ms = C.up ? (int32_t)(t - C.t_tcp_rx) : -1;
    P.tx         = C.ptt.state == PTT_ON;
    taskEXIT_CRITICAL(&s_mux);
}

/* -------------------------------------------------------------- the login */

static end_t cert_arrived(const uint8_t *p, size_t len)
{
    switch (svx_pki_store_cert(p, len, C.cfg.callsign, time(NULL))) {
    case PKI_PUSH_STORED:
        set_phase("certificate received");
        return END_CERT_STORED;
    case PKI_PUSH_EMPTY:
        return END_AWAIT_SYSOP;
    case PKI_PUSH_SAME:
        /* The one we have. Fine if it is usable; if it is the one that was
         * refused, the two ends disagree and asking again will not help. */
        if (svx_pki_present(C.cfg.callsign, time(NULL))) return END_CERT_STORED;
        set_why("the reflector still considers our old certificate valid");
        return END_FAILED;
    case PKI_PUSH_REJECTED:
        set_why("the reflector sent a certificate that does not fit this station");
        return END_FAILED;
    default:
        set_why("the certificate could not be stored");
        return END_FAILED;
    }
}

/* Protocol greeting, CA bundle, STARTTLS, login: END_NONE when logged in and
 * the audio channel is keyed. */
static end_t handshake(bool present)
{
    uint16_t type;
    uint8_t *p;
    size_t   len, n;
    int      r;

    if (proto_build_proto_ver(C.frame, FRAME_CAP, &n) != 0 || !send_frame(n)) {
        set_why("could not greet the reflector");
        return END_FAILED;
    }
    for (bool tls = false; !tls; ) {
        r = svx_link_frame(&C.link, &type, &p, &len, STEP_MS);
        if (r <= 0) {
            set_why(r ? "the reflector closed the connection" : "no answer from the reflector");
            return END_FAILED;
        }
        switch (type) {
        case MSG_HEARTBEAT:
            send_heartbeat();
            break;
        case MSG_CA_INFO:
            if (proto_build_ca_bundle_req(C.frame, FRAME_CAP, &n) == 0) send_frame(n);
            break;
        case MSG_CA_BUNDLE_RESPONSE: {
            const size_t cap = len + len / 16 + 16;
            char *pem = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
            if (pem && proto_parse_pem_blob(p, len, pem, cap) == 0) svx_pki_store_ca(pem);
            free(pem);
            if (proto_build_start_enc_req(C.frame, FRAME_CAP, &n) == 0) send_frame(n);
            break;
        }
        case MSG_START_ENCRYPTION:
            tls = true;
            break;
        case MSG_PROTO_VER_DOWNGRADE: {
            uint16_t ma = 0, mi = 0;
            proto_parse_proto_ver_downgrade(p, len, &ma, &mi);
            set_why("the reflector speaks protocol %u.%u, not 3.0", ma, mi);
            return END_FAILED;
        }
        case MSG_ERROR: {
            char e[80] = "";
            proto_parse_error(p, len, e, sizeof e);
            set_why("reflector: %s", e);
            return END_FAILED;
        }
        default:
            break;
        }
    }

    char *crt = present ? svx_pki_crt_pem() : NULL;
    char *key = present ? svx_pki_key_pem() : NULL;
    char *ca  = present ? svx_pki_ca_pem() : NULL;
    r = svx_link_start_tls(&C.link, crt, key, ca, TLS_MS);
    free(crt);
    free(ca);
    if (key) { memset(key, 0, strlen(key)); free(key); }
    if (r == -2) {
        svx_pki_refused();
        set_why("the reflector refused our certificate (alert %d)", C.link.alert);
        return END_CERT_REFUSED;
    }
    if (r != 0) {
        set_why("the TLS handshake failed");
        /* Back off like any failure: the reflector allows one new connection
         * per ten seconds per address -- shared with every other SvxLink
         * client behind the same router. */
        return END_FAILED;
    }

    bool got_info = false, got_udp = false, sent_csr = false;
    while (!(got_info && got_udp)) {
        r = svx_link_frame(&C.link, &type, &p, &len, STEP_MS);
        if (r <= 0) {
            if (sent_csr) return END_AWAIT_SYSOP;
            set_why(r ? "the reflector closed the connection" : "no answer from the reflector");
            return END_FAILED;
        }
        switch (type) {
        case MSG_HEARTBEAT:
            send_heartbeat();
            break;
        case MSG_AUTH_CHALLENGE: {
            /* The certificate is the authentication; the digest is zeros. */
            static const uint8_t zero[20];
            if (proto_build_auth_response(C.frame, FRAME_CAP, &n, C.cfg.callsign, zero) == 0)
                send_frame(n);
            break;
        }
        case MSG_AUTH_OK:
            ESP_LOGI(TAG, "authenticated as %s", C.cfg.callsign);
            break;
        case MSG_SERVER_INFO: {
            uint16_t id = 0;
            int nodes = 0, opus = 0;
            if (proto_parse_server_info(p, len, &id, &nodes, &opus) != 0) {
                set_why("the reflector's ServerInfo does not parse");
                return END_FAILED;
            }
            C.nodes = nodes;
            if (svx_udp_open(&C.udp, C.link.addr, C.link.port) != 0) {
                set_why("no socket for the audio channel");
                return END_FAILED;
            }
            svx_udp_make_tx_key(&C.udp, id);
            char *json = malloc(1024);
            size_t jl = json ? nodeinfo_build_json(json, 1024, &C.cfg) : 0;
            const bool ok = json && proto_build_node_info(C.frame, FRAME_CAP, &n,
                                        C.udp.tx_iv, 6, C.udp.tx_key, 16, json, jl) == 0 &&
                            send_frame(n);
            free(json);
            if (!ok) { set_why("could not send NodeInfo"); return END_FAILED; }
            ESP_LOGI(TAG, "client %u, %d nodes online%s", id, nodes, opus ? "" : ", no Opus offered");
            got_info = true;
            break;
        }
        case MSG_START_UDP_ENCRYPTION: {
            uint8_t iv4[4], key16[16];
            const int k = proto_parse_start_udp_encryption(p, len, iv4, key16);
            if (!got_info || k < 0 ||
                (k == 1 ? svx_udp_rx_shared(&C.udp) : svx_udp_rx_key(&C.udp, iv4, key16)) != 0) {
                set_why("the audio channel could not be keyed");
                return END_FAILED;
            }
            /* Datagram 0, at once: it binds this flow to the login, and opens
             * the way back through the NAT. */
            svx_udp_send(&C.udp, UDP_MSG_HEARTBEAT, NULL, 0);
            got_udp = true;
            break;
        }
        case MSG_CLIENT_CSR_REQUEST:
            send_csr();
            sent_csr = true;
            svx_pki_set_pending(true, time(NULL));
            set_phase("certificate request delivered");
            break;
        case MSG_CLIENT_CERT:
            return cert_arrived(p, len);
        case MSG_ERROR: {
            char e[80] = "";
            proto_parse_error(p, len, e, sizeof e);
            set_why("reflector: %s", e);
            return sent_csr ? END_AWAIT_SYSOP : END_FAILED;
        }
        case MSG_PROTO_VER_DOWNGRADE:
            set_why("the reflector wants an older protocol");
            return END_FAILED;
        default:
            break;
        }
    }
    return END_NONE;
}

/* ------------------------------------------------------------ the session */

static end_t on_tcp(uint16_t type, const uint8_t *p, size_t len, uint64_t t)
{
    uint32_t tg;
    char call[32];
    switch (type) {
    case MSG_HEARTBEAT:
        send_heartbeat();
        break;
    case MSG_TALKER_START:
        if (proto_parse_talker(p, len, &tg, call, sizeof call) != 0) break;
        ESP_LOGI(TAG, "TG %lu: %s", (unsigned long)tg, call);
        tgm_on_talker_start(&C.tgm, tg, call);
        if (tg == tgm_selected(&C.tgm)) {
            const bool mine = str_ieq(call, C.cfg.callsign);
            if (mine && C.ptt.state == PTT_REQ_ON) {
                ptt_event(PTT_EV_CONFIRM_TRUE);
            } else if (!mine && C.ptt.state != PTT_IDLE && C.ptt.state != PTT_RELEASING) {
                ESP_LOGW(TAG, "%s has the floor: unkeying", call);
                s_abort = PTT_AB_REMOTE;
            }
        }
        break;
    case MSG_TALKER_STOP:
        if (proto_parse_talker(p, len, &tg, call, sizeof call) != 0) break;
        ESP_LOGI(TAG, "TG %lu: %s stopped", (unsigned long)tg, call);
        tgm_on_talker_stop(&C.tgm, tg, call);
        /* The reflector ended our over: its own timeout, or a sysop. */
        if (str_ieq(call, C.cfg.callsign) && C.ptt.state == PTT_ON)
            ptt_event(PTT_EV_CONFIRM_FALSE);
        audio_out_kick();                /* the rest of the over plays out */
        break;
    case MSG_NODE_JOINED:
        C.nodes++;
        break;
    case MSG_NODE_LEFT:
        if (C.nodes > 0) C.nodes--;
        break;
    case MSG_ERROR: {
        /* Usually the reason for the close that follows. */
        proto_parse_error(p, len, C.refl_err, sizeof C.refl_err);
        ESP_LOGW(TAG, "reflector: %s", C.refl_err);
        break;
    }
    case MSG_PROTO_VER_DOWNGRADE:
        set_why("the reflector wants an older protocol");
        return END_FAILED;
    case MSG_CLIENT_CSR_REQUEST:
        send_csr();
        break;
    case MSG_CLIENT_CERT:
        /* A renewal, pushed by the reflector at two thirds of the old one's
         * life. It ignores the rest of this session once it has sent it. */
        ESP_LOGI(TAG, "the reflector sent a new certificate");
        return cert_arrived(p, len) == END_CERT_STORED ? END_CERT_STORED : END_FAILED;
    default:
        break;
    }
    (void)t;
    return END_NONE;
}

static void on_udp(uint16_t type, const uint8_t *body, size_t len, int gap)
{
    switch (type) {
    case UDP_MSG_AUDIO:
        on_audio(body, len, gap);
        break;
    case UDP_MSG_FLUSH_SAMPLES:
    case UDP_MSG_ALL_SAMPLES_FLUSHED:
        audio_out_kick();
        break;
    default:
        break;
    }
}

static end_t session(void)
{
    const uint64_t t0 = now_ms();
    C.up = true;
    C.t_ready = C.t_tcp_rx = C.t_hb = t0;
    C.udp_seen = false;
    C.refl_err[0] = 0;
    C.backoff = 0;
    C.why[0] = 0;
    P.connects++;
    set_phase("connected");
    set_link(RADIO_LINK_READY, C.host);
    svx_pki_set_pending(false, time(NULL));     /* logged in: nothing to ask */
    tgm_after_connect(&C.tgm);
    if (C.set.feed) svx_feed_start(C.host);     /* where the talkers are, if it can say */

    end_t end = END_NONE;
    while (end == END_NONE) {
        uint64_t t = now_ms();
        int wait = 20;
        if (C.tx_on) wait = (int)CLAMP((int64_t)(C.t_next_frame - t), 0, 20);
        if (svx_link_readable(&C.link)) wait = 0;
        fd_set rs;
        FD_ZERO(&rs);
        FD_SET(C.link.fd, &rs);
        FD_SET(C.udp.fd, &rs);
        struct timeval tv = { .tv_sec = 0, .tv_usec = wait * 1000 };
        select(MAX(C.link.fd, C.udp.fd) + 1, &rs, NULL, NULL, &tv);
        t = now_ms();

        const bool closed = svx_link_pump(&C.link) < 0;
        uint16_t type;
        uint8_t *p;
        size_t   len;
        int k = 0;
        while (end == END_NONE && (k = svx_link_next(&C.link, &type, &p, &len)) > 0) {
            C.t_tcp_rx = t;
            end = on_tcp(type, p, len, t);
        }
        if (end != END_NONE) break;
        if (k < 0) { set_why("the reflector sent a frame too large to be real"); end = END_FAILED; break; }
        if (closed) {
            if (C.refl_err[0]) set_why("reflector: %s", C.refl_err);
            else               set_why("the reflector closed the connection");
            end = END_CLOSED;
            break;
        }

        const uint8_t *b;
        int gap;
        while (svx_udp_recv(&C.udp, &type, &b, &len, &gap) > 0) {
            C.udp_seen = true;
            on_udp(type, b, len, gap);
        }

        if (since(t, C.t_hb, SVX_TCP_HEARTBEAT_MS)) send_heartbeat();
        if (since(t, (uint64_t)C.udp.t_tx, SVX_UDP_HEARTBEAT_MS) &&
            svx_udp_send(&C.udp, UDP_MSG_HEARTBEAT, NULL, 0) != 0 && C.udp.sent_initial &&
            C.udp.tx_ctr == 0) {
            set_why("the audio channel's counter ran out: logging in again");
            end = END_CLOSED;
            break;
        }
        if (since(t, C.t_tcp_rx, TCP_SILENCE_MS)) {
            set_why("nothing from the reflector for %d s", TCP_SILENCE_MS / 1000);
            end = END_FAILED;
            break;
        }
        /* Only once audio has worked: some networks never pass a datagram,
         * and the control connection alone is still worth keeping. */
        if (C.udp_seen && since(t, (uint64_t)C.udp.t_rx, UDP_SILENCE_MS)) {
            set_why("nothing on the audio channel for %d s", UDP_SILENCE_MS / 1000);
            end = END_FAILED;
            break;
        }
        if (s_reload) { end = END_RELOAD; break; }

        tgm_tick(&C.tgm, t);
        if (C.tx_on) tgm_note_local_tx(&C.tgm, t);
        requests(t);
        publish(t);
    }

    C.up = false;
    if (C.ptt.state != PTT_IDLE) {
        ptt_out_t o;
        ptt_fsm_abort(&C.ptt, PTT_AB_LINK_DOWN, (uint32_t)now_ms(), &o);
        ptt_dispatch(&o);
    }
    tx_stop();
    audio_out_kick();
    /* The feed goes with the session, and comes back after the next login:
     * with it connected, the certificate login's second flight never got an
     * answer -- 0 of 5 re-logins with it, 6 of 6 without. */
    svx_feed_stop();
    P.closes++;
    ESP_LOGI(TAG, "session over: %lu audio packets in, %lu concealed, %lu lost, %lu failed "
             "authentication; %lu out; stack %u bytes never used, internal RAM %u free",
             (unsigned long)C.rx_packets, (unsigned long)C.rx_concealed,
             (unsigned long)C.udp.n_lost, (unsigned long)C.udp.n_auth_fail,
             (unsigned long)C.udp.n_tx, (unsigned)uxTaskGetStackHighWaterMark(NULL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    return end;
}

/* One attempt, from DNS to the end of the session. */
static end_t attempt(bool present)
{
    char host[64];
    uint16_t port = C.port;
    snprintf(host, sizeof host, "%s", C.host);
    set_link(RADIO_LINK_CONNECTING, C.host);
    set_phase("looking up %s", C.host);
    const int srv = svx_srv_lookup(C.host, host, sizeof host, &port);
    if (srv > 0) ESP_LOGI(TAG, "SRV: %s is %s:%u", C.host, host, port);
    else if (srv < 0) ESP_LOGW(TAG, "no answer to the SRV lookup; trying %s:%u", host, port);

    taskENTER_CRITICAL(&s_mux);
    snprintf(C.server, sizeof C.server, "%s:%u", host, port);
    taskEXIT_CRITICAL(&s_mux);
    set_phase("connecting to %s", host);
    if (svx_link_open(&C.link, host, port, CONNECT_MS) != 0) {
        svx_link_close(&C.link);
        set_why("cannot reach %s:%u", host, port);
        return END_FAILED;
    }
    set_link(RADIO_LINK_GREETING, NULL);
    set_phase(present ? "logging in" : "asking for a certificate");
    end_t end = handshake(present);
    if (end == END_NONE) end = session();
    svx_udp_close(&C.udp);
    svx_link_close(&C.link);
    return end;
}

/* ----------------------------------------------------------------- task */

static bool have_ip(void)
{
    esp_netif_t *n = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip;
    return n && esp_netif_get_ip_info(n, &ip) == ESP_OK && ip.ip.addr;
}

static void idle_wait(uint64_t until)
{
    /* The dial, lock and mute still work while there is no link. */
    for (;;) {
        const uint64_t t = now_ms();
        requests(t);
        publish(t);
        if (t >= until || s_reload || s_enroll_start || s_enroll_stop) return;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

static void load_all(void)
{
    settings_load(&C.set);
    if (!C.set.feed) svx_feed_stop();
    cfg_build();
    tgm_start();
    ESP_LOGI(TAG, "station %s, reflector %s, %d switchable and %d monitored talkgroups",
             C.cfg.callsign[0] ? C.cfg.callsign : "(no callsign)", C.host,
             C.cfg.n_switchable, C.cfg.n_monitored);
}

static void svx_task(void *arg)
{
    (void)arg;
    esp_sntp_config_t sc = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_netif_sntp_init(&sc);
    svx_portal_load_cache(C.host);
    load_all();
    C.codec = codec_open();
    if (!C.codec) ESP_LOGE(TAG, "no Opus: no audio either way");

    for (;;) {
        if (s_reload) {
            s_reload = false;
            load_all();
            C.t_retry = 0;
            C.backoff = 0;
        }
        if (s_enroll_stop) {
            s_enroll_stop = false;
            svx_pki_set_pending(false, time(NULL));
        }
        const time_t now = time(NULL);
        if (!C.cfg.callsign[0]) {
            set_link(RADIO_LINK_DOWN, "set up on the web page");
            set_phase("no callsign: set up on the web page");
            idle_wait(now_ms() + 1000);
            continue;
        }
        if (!have_ip()) {
            set_link(RADIO_LINK_DOWN, C.host);
            set_phase("waiting for WiFi");
            idle_wait(now_ms() + 500);
            continue;
        }
        /* Names for the talkgroups: once a day, before a login rather than
         * during a session, since it holds up this task for a moment. */
        if (svx_portal_due()) {
            set_phase("reading the talkgroup names");
            if (svx_portal_fetch(C.host) && !C.set.sw[0] && !C.set.mon[0]) load_all();
        }

        /* The certificate: present one that works; ask for one if asked to,
         * or if ours has expired or was refused. */
        if (s_enroll_start) {
            s_enroll_start = false;
            svx_pki_set_pending(true, now);
            C.t_retry = 0;
        }
        pki_info_t pi;
        svx_pki_info(C.cfg.callsign, now, &pi);
        const bool present = svx_pki_present(C.cfg.callsign, now);
        const bool renew = pi.state == PKI_EXPIRED || pi.state == PKI_REFUSED;
        if (!present && !pi.pending && !renew) {
            set_link(RADIO_LINK_DOWN, "no certificate");
            set_phase(pi.state == PKI_WRONG_CALL ? "the certificate is for another callsign"
                                                 : "no certificate: request one on the web page");
            idle_wait(now_ms() + 1000);
            continue;
        }
        if (!present) {
            if (!pi.have_key) {
                set_link(RADIO_LINK_DOWN, "making a key");
                set_phase("making a key (this takes a while)");
                const esp_err_t ke = svx_pki_make_key();
                if (ke != ESP_OK) {
                    set_why(ke == ESP_ERR_INVALID_STATE ? "no key while the SD card holding the station's key is away"
                                                        : "could not make a key");
                    idle_wait(now_ms() + 10000);
                    continue;
                }
            }
            if (svx_pki_make_csr(C.cfg.callsign, C.cfg.email) != ESP_OK) {
                set_why("could not make a certificate request");
                idle_wait(now_ms() + 10000);
                continue;
            }
        }

        if (now_ms() < C.t_retry) {
            idle_wait(C.t_retry);
            continue;
        }

        const end_t end = attempt(present);
        uint32_t delay_ms;
        switch (end) {
        case END_CERT_STORED:
        case END_RELOAD:
            delay_ms = 0;
            break;
        case END_CERT_REFUSED:
            delay_ms = 3000;
            break;
        case END_AWAIT_SYSOP:
            set_phase("waiting for the sysop to sign the certificate");
            delay_ms = ENROLL_RETRY_MS;
            break;
        default:
            delay_ms = BACKOFF_S[MIN(C.backoff, (int)(sizeof BACKOFF_S / sizeof BACKOFF_S[0]) - 1)] * 1000u;
            C.backoff++;
            break;
        }
        C.t_retry = now_ms() + delay_ms;
        if (end == END_AWAIT_SYSOP) set_link(RADIO_LINK_DOWN, "waiting for the sysop");
        else                        set_link(RADIO_LINK_DOWN, C.host);
        if (delay_ms) set_phase("%s; again in %lu s",
                                end == END_AWAIT_SYSOP ? "waiting for the sysop" : "disconnected",
                                (unsigned long)(delay_ms / 1000));
    }
}

/* -------------------------------------------------------------- enrolment */

esp_err_t svx_enroll(bool start, const char **why)
{
    static const char *none = "";
    *why = none;
    if (!start) { s_enroll_stop = true; return ESP_OK; }
    svx_settings_t s;
    settings_load(&s);
    if (!s.call[0])  { *why = "set the callsign first";  return ESP_ERR_INVALID_STATE; }
    if (!s.email[0]) { *why = "the request needs an email address"; return ESP_ERR_INVALID_STATE; }
    s_enroll_start = true;
    return ESP_OK;
}

void svx_forget(void)
{
    svx_pki_forget();
    s_reload = true;
}

/* ------------------------------------------------------------- web page */

/* The station's endpoints (svx_web.c), which webcfg registers behind its
 * login in place of its weak default of none. */
extern const httpd_uri_t svx_web_uris[];
extern const size_t      svx_web_uris_n;

size_t radio_web_endpoints(const httpd_uri_t **out);
size_t radio_web_endpoints(const httpd_uri_t **out)
{
    *out = svx_web_uris;
    return svx_web_uris_n;
}

/* ---------------------------------------------------------------- radio.h */

const char *radio_link_name(void) { return "Reflector"; }

esp_err_t radio_start(const char *host, uint16_t port, const char *user, const char *pass)
{
    (void)user; (void)pass;          /* the station's settings are the client's own */
    ESP_RETURN_ON_FALSE(host, ESP_ERR_INVALID_ARG, TAG, "host");
    if (s_started) return ESP_OK;
    memset(&C, 0, sizeof C);
    snprintf(C.host, sizeof C.host, "%s", host);
    C.port = port ? port : SVX_DEFAULT_PORT;
    C.link.fd = C.udp.fd = -1;
    ptt_fsm_init(&C.ptt);
    C.mic_db = SILENT_DB;
    memset(&P, 0, sizeof P);
    P.rx_db = P.mic_db = SILENT_DB;
    /* The credentials load themselves on first use, on the reflector task:
     * reading them here, on the caller's small stack, overflowed it. */

    /* Core 0 with lwIP. A TLS handshake and an Opus encoder need room, and
     * the stack is in PSRAM: internal RAM is what WiFi sends from, and with
     * 12 kB of it gone to this stack the certificate login's large packets
     * could not be sent. Its settings are in RAM (kvstore): no flash from it. */
    if (xTaskCreatePinnedToCoreWithCaps(svx_task, "svx", 16384, NULL, 6, NULL, 0,
                                        MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGE(TAG, "no PSRAM for the reflector task");
        return ESP_ERR_NO_MEM;
    }
    s_started = true;
    P.link = RADIO_LINK_CONNECTING;
    return ESP_OK;
}

int64_t radio_tune_by(int32_t detents, uint8_t accel_mult, int32_t step_hz)
{
    (void)accel_mult; (void)step_hz;     /* one talkgroup a detent, however fast */
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

void radio_tg_lock(bool locked) { s_lock_req = locked ? 2 : 1; }
void radio_mute(bool muted)     { s_mute_val = muted; s_mute_req = true; }

void radio_ptt_key(void)    { s_key = true; }
void radio_ptt_unkey(void)  { s_unkey = true; }
void radio_ptt_toggle(void) { s_toggle = true; }
void radio_ptt_force_abort(uint8_t reason) { s_abort = reason; }

bool radio_is_ready(void) { return P.link == RADIO_LINK_READY; }

/* What the haptic gate reads: the published state, as ptt_dispatch leaves it. */
bool radio_on_air(void) { return P.tx || P.ptt_state != PTT_IDLE; }

void radio_get_status(radio_status_t *o)
{
    if (!o) return;
    memset(o, 0, sizeof *o);
    const uint64_t t = now_ms();
    taskENTER_CRITICAL(&s_mux);
    o->link        = P.link;
    o->reflector   = true;
    o->tg          = P.tg;
    memcpy(o->tg_name, P.tg_name, sizeof o->tg_name);
    memcpy(o->talker, P.talker, sizeof o->talker);
    memcpy(o->talker_info, P.talker_info, sizeof o->talker_info);
    memcpy(o->last_talker, P.last_talker, sizeof o->last_talker);
    o->talker_ms   = (P.talker[0] || P.last_talker[0]) ? (uint32_t)(t - P.talker_t) : 0;
    o->tg_locked   = P.locked;
    o->muted       = P.muted;
    o->rx_level_db = P.rx_db;
    o->tx_mic_dbm  = P.mic_db;
    o->smeter_dbm  = P.rx_db;
    memcpy(o->server, P.server, sizeof o->server);
    o->tx          = P.tx;
    o->n_trx       = 1;
    o->ptt_state   = P.ptt_state;
    o->ptt_rung    = P.ptt_rung;
    o->ptt_reason  = P.ptt_reason;
    o->ptt_refusals = P.ptt_refusals;
    o->permit      = P.permit;
    o->pong_age_ms = P.pong_age_ms;
    o->connects    = P.connects;
    o->closes      = P.closes;
    o->rejects     = P.rejects;
    o->sends       = P.sends;
    o->txa_sent    = P.txa_sent;
    o->txa_failed  = P.txa_failed;
    o->txa_max_us  = P.txa_max_us;
    memcpy(o->last_close, P.last_close, sizeof o->last_close);
    taskEXIT_CRITICAL(&s_mux);
    snprintf(o->mode, sizeof o->mode, "fm");
}
