/* The device -- a headset (classic Bluetooth's hands-free profile, this chip
 * as its audio gateway) or a speaker (A2DP, a2dp.c) -- one at a time. See
 * hfp.h.
 *
 * A headset: the phone's side of the hands-free profile. A headset only
 * opens its audio for a call, so while the knob wants it, there is one -- a
 * call that is never dialled, as a phone's own internet calls are. The
 * headset's button hangs up a call; the knob takes that as its PTT, and the
 * call goes on.
 *
 * Audio comes and goes through the host (HCI): the stack codes the mSBC (or
 * CVSD) and hands us 16-bit PCM at the headset's rate. The headset's packets
 * set the pace: each one heard from it is one sent to it, so what we send can
 * neither run ahead of the air nor fall behind it. The knob's audio is
 * converted to that rate on its way in, a little fast or slow as its own
 * clock needs (rs.h), and the microphone to the knob's rate on its way out.
 *
 * A speaker: an ear only. It gets the knob's audio over A2DP, converted to
 * 44.1 kHz the same way, at the pace of the stack's own media tick; the knob
 * keys its own microphone. Which a device is, kind.h: its class and services
 * at the scan, its hanging up every call's audio at once, or the page's
 * choice -- and a knob that does not say it knows speakers is given none:
 * every device is a headset to it, as before. */
#include "hfp.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "a2dp.h"
#include "air.h"
#include "batt.h"
#include "bt_link_proto.h"
#include "esp_bt.h"
#include "esp_bt_device.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#include "esp_heap_caps.h"
#include "esp_hf_ag_api.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "kind.h"
#include "link.h"
#include "nvs.h"
#include "rs.h"
#include "upd.h"
#include "bta/bta_ag_api.h"
#include "stack/btm_api.h"

/* bta_ag_int.h's, not on a component's include path: the stack's handle for
 * a headset, which BTA_AgSetCodec() takes, and its override of the link
 * settings it asks for (bta_ag_sco.c). */
extern UINT16 bta_ag_idx_by_bdaddr(BD_ADDR peer_addr);
extern void   bta_ag_set_esco_param(BOOLEAN set_reset, tBTM_ESCO_PARAMS *param);

static const char *TAG = "hfp";

#define DEVICE_NAME "VFO-Knob"
#define NVS_NS      "hfp"

/* How long the knob's audio waits in the downlink, as its target: enough to
 * ride out the knob sending in 10-20 ms lumps. */
#define DN_TARGET_MS 60
#define DN_MAX_MS    200

/* A speaker's stream, from its A2DP's connect, waits at most this long for
 * its volume to be the knob's (a2dp_volume_settled): its remote control up
 * -- the stack opens it 3.5 s on, if the speaker has not -- its events
 * asked, the knob's VOLUME set. Before, it would play at its own volume --
 * the loud one the knob's VOLUME was to tame -- until then. */
#define AV_HOLD_US 5000000

static SemaphoreHandle_t s_mx;              /* the state below */
static struct {
    uint8_t       link, audio;
    esp_bd_addr_t conn;                     /* the one the link (being made, or up) is with */
    esp_bd_addr_t bda;                      /* connected, being called, or the one to call */
    char          name[32];
    bool          have;                     /* bda is set */
    esp_bd_addr_t mem;                      /* the remembered headset (NVS) */
    char          mem_name[32];
    bool          remembered;
    uint8_t       spk, mic;
    bool          scanning;
    bool          want_audio;               /* the knob listens through the headset */
    uint32_t      dn_rate, up_rate;
    bool          user_off;                 /* the knob hung up: not called again until asked */
    bool          call;                     /* the call the audio rides on */
    bool          audio_pending;
    int           audio_tries, page_fails;
    int64_t       next_page_us, next_audio_us;
    int           conns, conn_audios;       /* headset links since boot; audio opens on this one */
    int64_t       conn_us;                  /* when this one was made */
    int64_t       audio_us;                 /* when its audio opened, 0 while closed */
    bool          audio_msbc;               /* ...in mSBC */
    int           quick;                    /* audio it closed again at once, in a row */
    bool          narrow;                   /* CVSD for it: its mSBC never stays open */
    /* What bda is, and what goes with a speaker. link above is always the
     * hands-free link -- a headset's, or a speaker's own; audio is the open
     * audio of the device's kind: a headset's call, a speaker's stream. */
    uint8_t       kind, kind_why, svc;      /* BTL_KIND_*, BTL_KWHY_*, BTL_SVC_* */
    bool          knob_known;               /* a HELLO from the knob since this chip started */
    bool          knob_speakers;            /* ...saying SPEAKERS: kinds apply */
    uint8_t       av;                       /* the A2DP link: BTL_LINK_* */
    esp_bd_addr_t av_conn;                  /* ...with whom */
    int64_t       av_since_us;              /* ...since when, while it is being made */
    bool          av_away;                  /* ...its page went unanswered: away, not a refusal */
    bool          av_trial;                 /* a speaker by its drops since this chip started, its A2DP never up since */
    int           av_fails;                 /* ...A2DP calls it answered and refused since: the fallback's test */
    bool          held;                     /* it has held a call's audio open: a headset, whatever drops later */
    bool          media_pending;            /* a start or suspend of its stream asked, not answered */
    uint8_t       media_cmd;                /* ...which: A2DP_START or A2DP_SUSPEND */
    bool          suspend_asked;            /* a suspend asked, its stream's close not come yet */
    int           media_tries;              /* the sink's own suspends soon after a start, in a row */
    int           media_fails;              /* starts refused, in a row */
    bool          rekind;                   /* links going down for a kind change: called again in 1 s */
    bool          told_av, told_hf;         /* the once-a-connection lines */
    bool          told_delay;               /* ...and the once-a-stream one */
    uint16_t      mtu;                      /* the A2DP link's packets, bytes */
    uint16_t      sink_delay;               /* the sink's delay report, 1/10 ms; 0 none */
    /* Its battery, as its hands-free link last said (batt.h) -- a headset's,
     * or a speaker's own: unknown until a report comes, forgotten when that
     * link closes, or another device is the one (batt_forget_locked). */
    uint8_t       batt;                     /* BTL_BATT_*: NONE, unknown */
    uint8_t       batt_pct;                 /* ...its charge, 0-100 % */
} S = { .spk = 10, .mic = 10, .dn_rate = 24000, .up_rate = 24000 };

/* A headset whose audio closes as soon as it is open is called with CVSD on
 * plain packets instead, from the second such close in a row, remembered
 * here until the chip restarts; the audio reopened every 2 s for ever
 * before. A JLab Pop Party (2026-10-04) says it carries the EDR links the
 * stack asks for -- 2-EV3, mSBC's T2 and CVSD's S4 alike -- and the
 * controller drops each about 150 ms after it is up (reason 0x16, with no
 * disconnect sent by the host): EV3 or HV3, no EDR, is what any headset
 * carries. */
#define QUICK_US   1500000                  /* an audio closed within this is "at once" */
#define NARROW_MAX 4
static esp_bd_addr_t s_narrow[NARROW_MAX];
static int           s_nnarrow;

static bool narrow_known(const uint8_t *bda)
{
    for (int i = 0; i < s_nnarrow; i++)
        if (!memcmp(s_narrow[i], bda, 6)) return true;
    return false;
}

static void narrow_add(const uint8_t *bda)
{
    if (narrow_known(bda)) return;
    if (s_nnarrow < NARROW_MAX) s_nnarrow++;
    memmove(s_narrow[1], s_narrow[0], (size_t)(s_nnarrow - 1) * sizeof s_narrow[0]);
    memcpy(s_narrow[0], bda, 6);
}

/* Its next audio in CVSD (the stack negotiates that codec, +BCS:1, before it
 * opens one), on EV3 or HV3 packets, no EDR. The link settings are the
 * stack's for every headset: they go back to its own when any other one
 * connects -- the chip has one at a time. */
static void narrow_set(const uint8_t *bda)
{
    static tBTM_ESCO_PARAMS plain = {
        .tx_bw          = BTM_64KBITS_RATE,
        .rx_bw          = BTM_64KBITS_RATE,
        .max_latency    = 10,
        .voice_contfmt  = BTM_VOICE_SETTING_CVSD,
        .packet_types   = BTM_SCO_PKT_TYPES_MASK_HV1 | BTM_SCO_PKT_TYPES_MASK_HV3 |
                          BTM_SCO_PKT_TYPES_MASK_EV3 | BTM_SCO_PKT_TYPES_MASK_NO_2_EV3 |
                          BTM_SCO_PKT_TYPES_MASK_NO_3_EV3 | BTM_SCO_PKT_TYPES_MASK_NO_2_EV5 |
                          BTM_SCO_PKT_TYPES_MASK_NO_3_EV5,
        .retrans_effort = BTM_ESCO_RETRANS_POWER,
    };
    bta_ag_set_esco_param(TRUE, &plain);
    const UINT16 h = bta_ag_idx_by_bdaddr((uint8_t *)bda);
    if (h) BTA_AgSetCodec(h, BTA_AG_CODEC_CVSD);
}
static bool s_hold;                         /* an update coming in: the device not called (s_mx) */

#define LOCK()   xSemaphoreTake(s_mx, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(s_mx)

/* A speaker to this knob: its verdict, unless the knob said hello without
 * SPEAKERS -- then every device is a headset, as before. Before its first
 * hello, a speaker is one, but is neither called nor said to be connected
 * (hfp_tick, hfp_report_state). The one place the kind is read. (s_mx.) */
static bool spk_locked(void)
{
    return S.kind == BTL_KIND_SPEAKER && !(S.knob_known && !S.knob_speakers);
}

/* The device's battery, forgotten: its hands-free link, which says it, went
 * -- a speaker's A2DP may play on, but no word of its charge comes over
 * that -- or another device is the one now. The next link says it again.
 * (s_mx.) */
static void batt_forget_locked(void)
{
    S.batt     = BTL_BATT_NONE;
    S.batt_pct = 0;
}

/* What a scan found, for the names it learns later and the connect after. */
#define FOUND_MAX 16
static btl_found_t s_found[FOUND_MAX];
static int         s_nfound;
static int         s_name_next;             /* the next nameless device to ask */

static btl_found_t *found_get(const uint8_t *bda)
{
    for (int i = 0; i < s_nfound; i++)
        if (!memcmp(s_found[i].bda, bda, 6)) return &s_found[i];
    return NULL;
}

/* A found device's kind: its verdict kept here, else what its class and
 * services make of it. */
static void found_kind(btl_found_t *f)
{
    if (kind_lookup(f->bda, &f->kind, &f->kind_why)) return;
    f->kind     = kind_classify(f->cod, f->svc);
    f->kind_why = f->cod || f->svc ? BTL_KWHY_CLASS : BTL_KWHY_NONE;
}

/* S.kind, kind_why and svc for bda, the device now: whenever bda changes.
 * Its verdict kept here, else what this boot's scan made of it, else a
 * headset with nothing known -- as every device was before. A fallback's
 * trial was the device before's. (s_mx.) */
static void target_kind_locked(void)
{
    btl_found_t *f = S.have ? found_get(S.bda) : NULL;
    S.svc      = f ? f->svc : 0;
    S.av_trial = false;
    S.av_fails = 0;
    S.held     = S.have && kind_held(S.bda);
    if (S.have && kind_lookup(S.bda, &S.kind, &S.kind_why)) return;
    if (f) {
        found_kind(f);
        S.kind     = f->kind;
        S.kind_why = f->kind_why;
    } else {
        S.kind     = BTL_KIND_HEADSET;
        S.kind_why = BTL_KWHY_NONE;
    }
}

/* Audio. */
static rs_t           s_dn, s_up;           /* knob -> device, headset -> knob */
static TaskHandle_t   s_pump;
static volatile bool  s_audio_on, s_mic_on, s_first;
static volatile bool  s_av_on;              /* ...the speaker's stream: the converter is the stack's media tick's */
static volatile bool  s_delay_news;         /* the sink reported its delay: the main loop says so */
static volatile uint16_t s_delay_value;     /* ...this, in 1/10 ms */
static uint32_t       s_session;            /* audio opens since boot (s_mx) */
static struct {                             /* the last one's, for its reports (s_mx) */
    esp_bd_addr_t bda;
    int           conns, audios;
    int64_t       conn_us;
    int64_t       audio_us, close_us;       /* a speaker's: its stream opened, and closed */
} s_open;
static volatile int   s_knob_hellos;        /* the knob's, asking: since boot */
static volatile int32_t s_credit;           /* bytes heard from the headset, not yet answered */
static bool           s_primed;
static float          s_avg;                /* the downlink's fill, smoothed */
static uint32_t       s_dn_target, s_dn_max;
static uint32_t       s_under, s_skips, s_frames_in, s_frames_out;

static bool same(const uint8_t *a, const uint8_t *b) { return memcmp(a, b, 6) == 0; }

static const char *bda_str(const uint8_t *b, char *s)
{
    sprintf(s, "%02x:%02x:%02x:%02x:%02x:%02x", b[0], b[1], b[2], b[3], b[4], b[5]);
    return s;
}

static void save_mem(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    if (S.remembered) {
        nvs_set_blob(h, "bda", S.mem, 6);
        nvs_set_str(h, "name", S.mem_name);
    } else {
        nvs_erase_key(h, "bda");
        nvs_erase_key(h, "name");
    }
    nvs_commit(h);
    nvs_close(h);
}

static void load_mem(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t n = 6;
    if (nvs_get_blob(h, "bda", S.mem, &n) == ESP_OK && n == 6) {
        S.remembered = true;
        n = sizeof S.mem_name;
        if (nvs_get_str(h, "name", S.mem_name, &n) != ESP_OK) S.mem_name[0] = 0;
        memcpy(S.bda, S.mem, 6);
        strlcpy(S.name, S.mem_name, sizeof S.name);
        S.have = true;
    }
    nvs_close(h);
}

/* How far behind what it is sent a speaker plays: its own buffer, as it
 * reports it (150 ms, a common one, without), the downlink's target, and up
 * to a media tick. */
static uint16_t delay_ms_locked(void)
{
    return (uint16_t)((S.sink_delay ? S.sink_delay / 10 : 150) + DN_TARGET_MS + 30);
}

void hfp_report_state(void)
{
    btl_state_t s;
    memset(&s, 0, sizeof s);
    LOCK();
    const bool spk = spk_locked();
    if (!spk) {
        s.link  = S.link;
        s.audio = S.audio;
    } else {
        /* Its A2DP link is its link; its own hands-free link alone, a link
         * on the way. Nothing, to a knob that has not said it knows
         * speakers: it would take the device for a headset. */
        s.link  = !S.knob_speakers        ? BTL_LINK_IDLE
                  : S.av != BTL_LINK_IDLE   ? S.av
                  : S.link != BTL_LINK_IDLE ? BTL_LINK_CONNECTING
                                            : BTL_LINK_IDLE;
        s.audio = S.knob_speakers ? S.audio : BTL_AUDIO_NONE;
    }
    /* The kind of the links still up, even with the device forgotten, so
     * the knob never takes a speaker's going for a headset's; what is
     * known of it, only while there is one. */
    s.kind     = spk ? BTL_KIND_SPEAKER : BTL_KIND_HEADSET;
    s.kind_why = S.have ? S.kind_why : BTL_KWHY_NONE;
    s.svc      = S.have ? S.svc : 0;
    s.delay_ms = s.audio == BTL_AUDIO_SBC_44K ? delay_ms_locked() : 0;
    /* Its battery, as it last said, where it has. */
    if (S.have && S.batt != BTL_BATT_NONE) {
        s.batt     = S.batt;
        s.batt_pct = S.batt_pct;
    }
    /* A speaker's own volume, while its A2DP is up: whether it takes the
     * knob's, and has it (a2dp.c) -- its own remote control, none other. */
    uint8_t av_vol, av_turns;
    const uint8_t av = a2dp_volume_state(spk && S.knob_speakers && S.av == BTL_LINK_CONNECTED ? S.av_conn : NULL,
                                         &av_vol, &av_turns);
    if (av) {
        s.av        = av;
        s.av_volume = av_vol;
        s.av_turns  = av_turns;
    }
    if (S.have) memcpy(s.bda, S.bda, 6);
    s.rssi  = 0;
    s.spk   = S.spk;
    s.mic   = S.mic;
    strlcpy(s.name, S.name, sizeof s.name);
    s.remembered = S.remembered && S.have && same(S.bda, S.mem);
    s.scanning   = S.scanning;
    UNLOCK();
    link_send(BTL_EVT_STATE, &s, sizeof s);
}

/* A speaker's own hands-free link sends its buttons too: the knob takes them
 * from a headset only. */
static void button(uint8_t b, const char *what)
{
    LOCK();
    const bool spk = spk_locked();
    UNLOCK();
    link_log("%s button: %s", spk ? "speaker" : "headset", what);
    link_send(BTL_EVT_BUTTON, &b, 1);
}

/* ---- the call the audio rides on ------------------------------------- */

static void call_up(void)
{
    if (!S.call) {
        S.call = true;
        esp_hf_ag_ciev_report(S.bda, ESP_HF_IND_TYPE_CALL, ESP_HF_CALL_STATUS_CALL_IN_PROGRESS);
    }
    esp_hf_ag_audio_connect(S.bda);
}

/* A call's audio is a headset's: a speaker's stream is not the hands-free
 * profile's to close. */
static bool sco_open_locked(void)
{
    return S.audio == BTL_AUDIO_CVSD_8K || S.audio == BTL_AUDIO_MSBC_16K;
}

static void call_down(void)
{
    if (sco_open_locked()) esp_hf_ag_audio_disconnect(S.bda);
    if (S.call) {
        S.call = false;
        esp_hf_ag_ciev_report(S.bda, ESP_HF_IND_TYPE_CALL, ESP_HF_CALL_STATUS_NO_CALLS);
    }
}

/* The open audio, gone with its link or its kind: the converters stand
 * aside. (s_mx.) */
static void audio_gone_locked(void)
{
    if (S.audio == BTL_AUDIO_SBC_44K) s_open.close_us = esp_timer_get_time();
    s_audio_on = false;
    s_av_on    = false;
    S.audio    = BTL_AUDIO_NONE;
    S.audio_us = 0;
}

/* The device is to be something else now -- the page said so, it showed
 * itself a speaker, or the knob's firmware changed -- and was it before
 * (`was`, spk_locked() before the change). One rule: its links all go down,
 * and it is called again as its new kind a second later. (s_mx.) */
static void rekind_locked(bool was, int64_t now)
{
    if (spk_locked() == was) return;
    if (S.link == BTL_LINK_IDLE && S.av == BTL_LINK_IDLE) return;
    call_down();
    if (S.audio != BTL_AUDIO_NONE) audio_gone_locked();
    if (S.link != BTL_LINK_IDLE) esp_hf_ag_slc_disconnect(S.conn);
    if (S.av != BTL_LINK_IDLE) a2dp_disconnect(S.av_conn);
    S.rekind        = true;
    S.next_page_us  = now + 1000000;
    S.audio_tries   = 0;
    S.audio_pending = false;
    S.media_pending = false;
    S.suspend_asked = false;
    S.media_tries   = 0;
    S.media_fails   = 0;
    S.quick         = 0;
}

/* ---- audio ------------------------------------------------------------ */

/* One frame for the device's ear: n samples at its rate. */
static void dn_frame(int16_t *out, size_t n)
{
    const uint32_t fill = rs_fill(&s_dn);
    if (!s_primed) {
        /* Start, or start again after running dry, only with a cushion. */
        if (fill < s_dn_target) { memset(out, 0, n * 2); return; }
        s_primed = true;
        s_avg    = (float)fill;
    }
    if (fill > s_dn_max) {
        /* Far behind -- the knob sent a burst after a stall: catch up at once. */
        rs_drop(&s_dn, s_dn_target);
        s_avg = (float)s_dn_target;
        s_skips++;
    }
    /* The fill, smoothed over about a second, steers the rate: what is above
     * the target is played off over some four seconds, never more than half
     * a percent fast or slow -- a pitch nobody hears. A call's frames 1/128
     * of the way each, as ever; a speaker's by their length, a second's
     * worth whatever the SBC frame -- 1/330 of it for one of 2.9 ms. */
    const float k = s_av_on ? (float)n / (0.96f * (float)s_dn.out_rate) : 1.0f / 128.0f;
    s_avg += ((float)rs_fill(&s_dn) - s_avg) * k;
    double c = ((double)s_avg - (double)s_dn_target) / (4.0 * (double)s_dn.in_rate);
    if (c > 0.005) c = 0.005;
    if (c < -0.005) c = -0.005;
    rs_nudge(&s_dn, c);
    if (!rs_pull(&s_dn, out, n)) {
        memset(out, 0, n * 2);
        s_primed = false;
        s_under++;
    }
}

/* The stack's: the headset's microphone, decoded. Its pace is the air's.
 * Not a speaker's call audio, which is closed as it opens: the downlink is
 * its stream's then. */
static void hf_incoming(const uint8_t *buf, uint32_t sz)
{
    if (!s_audio_on || s_av_on) return;
    int16_t pcm[256];
    const uint32_t n = sz > sizeof pcm ? sizeof pcm : sz;
    memcpy(pcm, buf, n);                    /* buf need not be aligned */
    rs_push(&s_up, pcm, n / 2);
    s_frames_in++;
    /* Two frames ahead at the start, then one for one. */
    int32_t c = __atomic_add_fetch(&s_credit, (int32_t)(s_first ? 3 * sz : sz), __ATOMIC_RELAXED);
    s_first = false;
    if (c > (int32_t)(8 * sz)) __atomic_store_n(&s_credit, (int32_t)(8 * sz), __ATOMIC_RELAXED);
    if (s_pump) xTaskNotifyGive(s_pump);
}

/* The stack's: a frame for the headset, if one is due. */
static uint32_t hf_outgoing(uint8_t *p, uint32_t sz)
{
    if (!s_audio_on || s_av_on || sz > 512) return 0;
    if (__atomic_load_n(&s_credit, __ATOMIC_RELAXED) < (int32_t)sz) return 0;
    __atomic_sub_fetch(&s_credit, (int32_t)sz, __ATOMIC_RELAXED);
    int16_t pcm[256];
    dn_frame(pcm, sz / 2);
    memcpy(p, pcm, sz);
    s_frames_out++;
    return sz;
}

/* Hands the stack its frames, and the microphone to the knob -- off the
 * stack's own task, which must never wait on the UART. Nor this one on the
 * console: its reports are the main task's (reports(), below). */
static void pump_task(void *arg)
{
    (void)arg;
    static int16_t out[512];
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(200));
        if (!s_audio_on || s_av_on) continue;       /* a headset's call only */
        esp_hf_ag_outgoing_data_ready();
        size_t n;
        while ((n = rs_pull_some(&s_up, out, sizeof out / sizeof out[0])) > 0)
            if (s_mic_on) link_send(BTL_AUDIO_UP, out, (uint16_t)(n * 2));
    }
}

/* The device's rate of an open audio, BTL_AUDIO_*: 8, 16 or 44.1 kHz. */
static uint32_t air_rate(uint8_t audio)
{
    return audio == BTL_AUDIO_SBC_44K ? 44100 : audio == BTL_AUDIO_MSBC_16K ? 16000 : 8000;
}

/* The converters, for the device's rate and the knob's, emptied: the
 * microphone's only for a headset -- a speaker has none, and each filter
 * takes its tens of milliseconds to design. */
static void audio_rates(uint32_t air, bool mic)
{
    rs_init(&s_dn, S.dn_rate, air);
    if (mic) rs_init(&s_up, air, S.up_rate);
    s_dn_target = S.dn_rate * DN_TARGET_MS / 1000;
    s_dn_max    = S.dn_rate * DN_MAX_MS / 1000;
    s_primed    = false;
}

static void audio_open(bool msbc)
{
    s_audio_on = false;
    s_av_on    = false;
    __atomic_store_n(&s_credit, 0, __ATOMIC_RELAXED);
    audio_rates(msbc ? 16000 : 8000, true);
    s_first  = true;
    s_under = s_skips = s_frames_in = s_frames_out = 0;
    s_session++;
    esp_hf_ag_register_data_callback(hf_incoming, hf_outgoing);
    s_audio_on = true;
}

/* A speaker's stream open: the downlink converted to 44.1 kHz, for the
 * stack's media tick to pull (hfp_dn_pull). No microphone. */
static void spk_open(void)
{
    s_audio_on = false;
    s_av_on    = true;
    audio_rates(44100, false);
    s_under = s_skips = s_frames_in = s_frames_out = 0;
    a2dp_counts_reset();
    s_session++;
    s_audio_on = true;
}

void hfp_dn_pull(int16_t *out, size_t n)
{
    /* Never an empty answer: the stream stays whole, as a call's does --
     * silence while the converter is rebuilt, or before it is open. */
    if (s_audio_on && s_av_on) dn_frame(out, n);
    else memset(out, 0, n * sizeof *out);
}

/* ---- scanning --------------------------------------------------------- */

static bool is_audio(uint32_t cod) { return esp_bt_gap_get_cod_major_dev(cod) == ESP_BT_COD_MAJOR_DEV_AV; }

/* An audio device, by its class or the services it lists. */
static bool found_audio(const btl_found_t *f)
{
    return is_audio(f->cod) || (f->svc & (BTL_SVC_HFP | BTL_SVC_HSP | BTL_SVC_A2DP));
}

static btl_found_t *found_slot(const uint8_t *bda)
{
    for (int i = 0; i < s_nfound; i++)
        if (same(s_found[i].bda, bda)) return &s_found[i];
    if (s_nfound == FOUND_MAX) return NULL;
    btl_found_t *f = &s_found[s_nfound++];
    memset(f, 0, sizeof *f);
    memcpy(f->bda, bda, 6);
    f->rssi = -127;
    return f;
}

static void on_found(const esp_bt_gap_cb_param_t *p)
{
    btl_found_t *f = found_slot(p->disc_res.bda);
    if (!f) return;
    char eir_name[32] = "";
    for (int i = 0; i < p->disc_res.num_prop; i++) {
        const esp_bt_gap_dev_prop_t *q = &p->disc_res.prop[i];
        switch (q->type) {
        case ESP_BT_GAP_DEV_PROP_COD:
            memcpy(&f->cod, q->val, 4);
            break;
        case ESP_BT_GAP_DEV_PROP_RSSI:
            f->rssi = *(const int8_t *)q->val;
            break;
        case ESP_BT_GAP_DEV_PROP_BDNAME: {
            const int n = q->len < 31 ? q->len : 31;
            memcpy(f->name, q->val, n);
            f->name[n] = 0;
            break;
        }
        case ESP_BT_GAP_DEV_PROP_EIR: {
            uint8_t len = 0;
            uint8_t *nm = esp_bt_gap_resolve_eir_data(q->val, ESP_BT_EIR_TYPE_CMPL_LOCAL_NAME, &len);
            if (!nm) nm = esp_bt_gap_resolve_eir_data(q->val, ESP_BT_EIR_TYPE_SHORT_LOCAL_NAME, &len);
            if (nm) {
                const int n = len < 31 ? len : 31;
                memcpy(eir_name, nm, n);
                eir_name[n] = 0;
            }
            /* The services it lists: what it is, beside its class. */
            f->svc = kind_eir_services(q->val);
            break;
        }
        default:
            break;
        }
    }
    if (!f->name[0] && eir_name[0]) strlcpy(f->name, eir_name, sizeof f->name);
    /* What it would be on a connect: its verdict kept here, else its class
     * and services' -- as the page shows it. */
    found_kind(f);
    link_send(BTL_EVT_FOUND, f, sizeof *f);
}

/* After the scan, the names it did not hear, one audio device at a time. */
static bool ask_next_name(void)
{
    for (; s_name_next < s_nfound; s_name_next++) {
        btl_found_t *f = &s_found[s_name_next];
        if (!f->name[0] && (found_audio(f) || f->kind == BTL_KIND_SPEAKER)) {
            esp_bt_gap_read_remote_name(f->bda);
            s_name_next++;
            return true;
        }
    }
    return false;
}

static void scan_over(void)
{
    LOCK();
    S.scanning = false;
    UNLOCK();
    link_send(BTL_EVT_SCAN_DONE, NULL, 0);
    hfp_report_state();
    int headsets = 0, speakers = 0;
    for (int i = 0; i < s_nfound; i++) {
        if (s_found[i].kind == BTL_KIND_SPEAKER) speakers++;
        else if (found_audio(&s_found[i])) headsets++;
    }
    link_log("scan done: %d devices: %d headset%s, %d speaker%s", s_nfound, headsets, headsets == 1 ? "" : "s",
             speakers, speakers == 1 ? "" : "s");
}

/* ---- the stack's events ------------------------------------------------- */

static void gap_cb(esp_bt_gap_cb_event_t ev, esp_bt_gap_cb_param_t *p)
{
    char b[18];
    switch (ev) {
    case ESP_BT_GAP_DISC_RES_EVT:
        on_found(p);
        break;
    case ESP_BT_GAP_DISC_STATE_CHANGED_EVT:
        if (p->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STARTED) {
            LOCK();
            S.scanning = true;
            UNLOCK();
            hfp_report_state();
        } else if (p->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STOPPED) {
            s_name_next = 0;
            if (!ask_next_name()) scan_over();
        }
        break;
    case ESP_BT_GAP_READ_REMOTE_NAME_EVT: {
        const bool ok = p->read_rmt_name.stat == ESP_BT_STATUS_SUCCESS;
        bool scanning;
        LOCK();
        scanning = S.scanning;
        if (ok && S.have && same(p->read_rmt_name.bda, S.bda) && !S.name[0]) {
            strlcpy(S.name, (const char *)p->read_rmt_name.rmt_name, sizeof S.name);
            if (S.remembered && same(S.bda, S.mem)) {
                strlcpy(S.mem_name, S.name, sizeof S.mem_name);
                save_mem();
            }
        }
        UNLOCK();
        if (ok)
            for (int i = 0; i < s_nfound; i++)
                if (same(s_found[i].bda, p->read_rmt_name.bda)) {
                    strlcpy(s_found[i].name, (const char *)p->read_rmt_name.rmt_name, sizeof s_found[i].name);
                    link_send(BTL_EVT_FOUND, &s_found[i], sizeof s_found[i]);
                }
        if (scanning && !ask_next_name()) scan_over();
        else hfp_report_state();
        break;
    }
    case ESP_BT_GAP_AUTH_CMPL_EVT:
        if (p->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS)
            link_log("paired with %s (%s)", p->auth_cmpl.device_name, bda_str(p->auth_cmpl.bda, b));
        else
            link_log("pairing with %s failed: %d", bda_str(p->auth_cmpl.bda, b), p->auth_cmpl.stat);
        break;
    case ESP_BT_GAP_CFM_REQ_EVT: {
        /* Just Works: neither side has a screen. Only the headset we call,
         * or the one we know. */
        bool ok;
        LOCK();
        ok = (S.have && same(p->cfm_req.bda, S.bda)) || (S.remembered && same(p->cfm_req.bda, S.mem));
        UNLOCK();
        esp_bt_gap_ssp_confirm_reply(p->cfm_req.bda, ok);
        if (!ok) link_log("pairing refused: %s", bda_str(p->cfm_req.bda, b));
        break;
    }
    case ESP_BT_GAP_KEY_REQ_EVT:
        esp_bt_gap_ssp_passkey_reply(p->key_req.bda, false, 0);
        break;
    case ESP_BT_GAP_READ_RSSI_DELTA_EVT:
        air_signal(p->read_rssi_delta.stat == ESP_BT_STATUS_SUCCESS, p->read_rssi_delta.rssi_delta);
        break;
    case ESP_BT_GAP_ACL_CONN_CMPL_STAT_EVT:
        /* No link to the speaker being called: its page went unanswered --
         * away, or switched off. Its A2DP's failure, which follows, is no
         * refusal (hfp_av_conn). */
        if (p->acl_conn_cmpl_stat.stat != ESP_BT_STATUS_SUCCESS) {
            LOCK();
            if (S.av == BTL_LINK_CONNECTING && same(p->acl_conn_cmpl_stat.bda, S.av_conn)) S.av_away = true;
            UNLOCK();
        }
        break;
    case ESP_BT_GAP_MODE_CHG_EVT:
        /* A call keeps the link awake (air.c); one that sleeps all the same,
         * or is woken as its audio opens, says so. The interval says whose
         * sleep: 31-94 ms is the stack's own for a call, its policy never
         * cleared; 250-500 ms its own between calls, asked a moment before
         * this one opened; anything else most likely the headset's. */
        if (s_audio_on) {
            /* A speaker's stream too: the stack keeps it awake while it
             * plays (its AV power policy), so a sleep is news there as well. */
            const esp_bt_pm_mode_t m = p->mode_chg.mode;
            const char *during = s_av_on ? "while the speaker played" : "during the call";
            if (m == ESP_BT_PM_MD_SNIFF)
                link_log("the link went to sniff %s, every %u ms", during,
                         (unsigned)(p->mode_chg.interval * 5u / 8u));
            else
                link_log("the link went %s %s", m == ESP_BT_PM_MD_ACTIVE ? "active" : "to another mode", during);
        }
        break;
    case ESP_BT_GAP_PIN_REQ_EVT: {
        /* An old headset: they all take 0000. */
        esp_bt_pin_code_t pin;
        memset(pin, '0', sizeof pin);
        esp_bt_gap_pin_reply(p->pin_req.bda, true, p->pin_req.min_16_digit ? 16 : 4, pin);
        break;
    }
    default:
        ESP_LOGD(TAG, "gap event %d", ev);
        break;
    }
}

static void on_connection(const uint8_t *bda, esp_hf_connection_state_t st)
{
    char b[18];
    const int64_t now = esp_timer_get_time();
    LOCK();
    /* A stranger calling us -- we are connectable for our own headset -- or
     * the end of one: not our link. */
    const bool ours = (S.have && same(bda, S.bda)) || (S.remembered && same(bda, S.mem));
    if (st == ESP_HF_CONNECTION_STATE_CONNECTED && !ours) {
        UNLOCK();
        link_log("refused a connection from %s", bda_str(bda, b));
        esp_hf_ag_slc_disconnect((uint8_t *)bda);
        return;
    }
    if (st == ESP_HF_CONNECTION_STATE_DISCONNECTED && (S.link == BTL_LINK_IDLE || !same(bda, S.conn))) {
        UNLOCK();
        ESP_LOGI(TAG, "%s: disconnected", bda_str(bda, b));
        return;
    }
    switch (st) {
    case ESP_HF_CONNECTION_STATE_CONNECTING:
    case ESP_HF_CONNECTION_STATE_CONNECTED:
        if (!ours) break;
        S.link = BTL_LINK_CONNECTING;
        memcpy(S.conn, bda, 6);
        break;
    case ESP_HF_CONNECTION_STATE_SLC_CONNECTED: {
        if (!S.have || !same(bda, S.bda)) { /* our remembered device, calling us */
            memcpy(S.bda, bda, 6);
            strlcpy(S.name, S.mem_name, sizeof S.name);
            S.have = true;
            target_kind_locked();
            batt_forget_locked();
        }
        S.link          = BTL_LINK_CONNECTED;
        memcpy(S.conn, bda, 6);
        if (spk_locked()) {
            /* A speaker's own hands-free link (the JLab opens one: AT+BAC,
             * AT+XAPL, AT+IPHONEACCEV). Kept -- refused, it is only asked
             * again, and its volume and battery come over it -- but never a
             * call on it. It is in reach: its A2DP next, unless it opens
             * that itself. */
            if (S.av == BTL_LINK_IDLE) S.next_page_us = now + 2000000;
            S.user_off = false;
            const bool tell      = !S.told_hf;
            const bool need_name = !S.name[0];
            S.told_hf = true;
            char name[32];
            strlcpy(name, S.name, sizeof name);
            UNLOCK();
            upd_headset_came();
            esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
            if (need_name) esp_bt_gap_read_remote_name((uint8_t *)bda);
            if (tell) link_log("%s opened the hands-free profile too: kept, never a call", name[0] ? name : "the speaker");
            hfp_report_state();
            return;
        }
        S.rekind        = false;
        S.page_fails    = 0;
        S.user_off      = false;
        S.audio_tries   = 0;
        S.audio_pending = false;
        S.next_audio_us = now + 800000;     /* a moment for the headset to settle */
        S.conns++;
        S.conn_us       = now;
        S.conn_audios   = 0;
        S.audio_us      = 0;
        S.quick         = 0;
        S.narrow        = narrow_known(bda);
        if (!S.remembered || !same(S.mem, S.bda) || strcmp(S.mem_name, S.name)) {
            memcpy(S.mem, S.bda, 6);
            strlcpy(S.mem_name, S.name, sizeof S.mem_name);
            S.remembered = true;
            save_mem();
        }
        /* Its verdict, kept from its first connection on: a scan later
         * changes nothing for a device in use. Not for a knob that knows
         * headsets only, whose table stays as it was. */
        if (S.knob_speakers) kind_store(S.bda, S.kind, S.kind_why);
        const bool need_name = !S.name[0];
        char name[32];
        strlcpy(name, S.name, sizeof name);
        const bool narrow = S.narrow;
        UNLOCK();
        /* After the SLC: its AT+BAC, during it, chose mSBC again. Any other
         * headset gets the stack's own link settings back. */
        if (narrow) narrow_set(bda);
        else bta_ag_set_esco_param(FALSE, NULL);
        upd_headset_came();                 /* a firmware on trial: proven enough to keep */
        esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
        if (need_name) esp_bt_gap_read_remote_name((uint8_t *)bda);
        link_log("headset connected: %s (%s)", name[0] ? name : "?", bda_str(bda, b));
        hfp_report_state();
        return;
    }
    case ESP_HF_CONNECTION_STATE_DISCONNECTED: {
        const uint8_t was = S.link;
        S.link    = BTL_LINK_IDLE;
        S.call    = false;
        S.told_hf = false;
        /* A headset's call went with it; a speaker's stream goes on. */
        if (sco_open_locked()) audio_gone_locked();
        /* The link its battery came over: the battery goes with it. */
        if (same(S.conn, S.bda)) batt_forget_locked();
        if (!same(S.conn, S.bda)) {
            /* Left for another one, which the knob asked for: call it now. */
            S.next_page_us = now + 300000;
            UNLOCK();
            hfp_report_state();
            return;
        }
        if (spk_locked()) {
            /* A speaker's own hands-free link: its A2DP link decides -- a
             * second from now, if this one went down for it to be called
             * as a speaker. */
            if (S.rekind && S.av == BTL_LINK_IDLE) S.next_page_us = now + 1000000;
            UNLOCK();
            ESP_LOGI(TAG, "%s: its hands-free link closed", bda_str(bda, b));
            hfp_report_state();
            return;
        }
        if (S.rekind) {
            /* Down to be called again as a headset. */
            S.next_page_us = now + 1000000;
            UNLOCK();
            hfp_report_state();
            return;
        }
        if (was == BTL_LINK_CONNECTED) {
            /* Switched off, or out of reach: call it again in a while. */
            S.next_page_us = now + 10000000;
            UNLOCK();
            link_log("headset gone: %s", bda_str(bda, b));
            hfp_report_state();
            return;
        }
        /* A call that was not answered. Back off: 10 s, 20, 40, then a minute. */
        S.page_fails++;
        link_log("no answer from %s (%d)", bda_str(bda, b), S.page_fails);
        const int64_t back = S.page_fails >= 4 ? 60000000 : 10000000LL << (S.page_fails - 1);
        S.next_page_us = now + back;
        if (S.remembered && !same(S.bda, S.mem)) {
            /* A new headset that did not answer: back to the one we know. */
            memcpy(S.bda, S.mem, 6);
            strlcpy(S.name, S.mem_name, sizeof S.name);
            target_kind_locked();
        } else if (!S.remembered && S.page_fails >= 2) {
            S.have = false;                 /* a new one, never reached: give up */
        }
        break;
    }
    default:
        break;
    }
    UNLOCK();
    hfp_report_state();
}

static void on_audio_state(esp_hf_audio_state_t st, const uint8_t *bda)
{
    char b[18];
    LOCK();
    switch (st) {
    case ESP_HF_AUDIO_STATE_CONNECTED:
    case ESP_HF_AUDIO_STATE_CONNECTED_MSBC: {
        if (spk_locked()) {
            /* A speaker never has a call's audio: it would take the knob's
             * audio off its stream. */
            char name[32];
            strlcpy(name, S.name, sizeof name);
            UNLOCK();
            esp_hf_ag_audio_disconnect((uint8_t *)bda);
            link_log("%s opened a call's audio by itself: closed -- a speaker", name[0] ? name : bda_str(bda, b));
            return;
        }
        const bool msbc = st == ESP_HF_AUDIO_STATE_CONNECTED_MSBC;
        S.audio         = msbc ? BTL_AUDIO_MSBC_16K : BTL_AUDIO_CVSD_8K;
        S.audio_pending = false;
        S.audio_tries   = 0;
        memcpy(s_open.bda, bda, 6);         /* for the reports, with audio_open()'s count */
        s_open.conns    = S.conns;
        s_open.conn_us  = S.conn_us;
        s_open.audios   = ++S.conn_audios;
        S.audio_us      = esp_timer_get_time();
        S.audio_msbc    = msbc;
        audio_open(msbc);
        UNLOCK();
        link_log("audio open: %s", msbc ? "mSBC, 16 kHz" : "CVSD, 8 kHz");
        hfp_report_state();
        return;
    }
    case ESP_HF_AUDIO_STATE_DISCONNECTED: {
        const int64_t now = esp_timer_get_time();
        if (spk_locked() && !sco_open_locked()) {
            /* A speaker's, closed as it opened: its stream is what is open. */
            UNLOCK();
            ESP_LOGI(TAG, "%s: a call's audio closed", bda_str(bda, b));
            return;
        }
        /* The headset's own doing, at once: the knob still wanted it and the
         * call was still up (call_down() takes the call down first). */
        const bool quick = S.audio_us && now - S.audio_us < QUICK_US && S.want_audio && S.call;
        S.quick    = quick ? S.quick + 1 : 0;
        S.audio_us = 0;
        bool narrow_now = false;
        if (quick && !S.narrow && S.quick >= 2) {
            S.narrow   = true;
            S.quick    = 0;
            narrow_now = true;
            narrow_add(bda);
        }
        /* Closing at once even so: ask less and less often, a minute apart
         * at most, rather than every 2 s for ever. */
        int64_t wait = 2000000;
        if (S.quick >= 2) wait <<= S.quick - 1 < 5 ? S.quick - 1 : 5;
        if (wait > 60000000) wait = 60000000;
        s_audio_on      = false;
        S.audio         = BTL_AUDIO_NONE;
        S.audio_pending = false;
        S.next_audio_us = now + wait;
        /* Narrowed, and closing at once all the same: no call's audio stays
         * open on it in any codec -- a speaker that calls itself a headset
         * (the JLab Pop Party, 2026-10-04: every link closed by it 85-100 ms
         * after it opened, mSBC and CVSD alike). To a knob that keeps its
         * own microphone with one, it is a speaker: its music profile next.
         * Never over the page's choice, nor for a device whose complete
         * service list has no A2DP, nor one that had no A2DP to play to --
         * nor a headset that has held a call's audio before: its drops are
         * something else's doing, a phone of its own taking it, say. */
        const bool to_spk = quick && S.narrow && S.quick >= 2 && S.knob_speakers && !S.held &&
                            S.kind_why != BTL_KWHY_USER && S.kind_why != BTL_KWHY_NO_A2DP &&
                            !((S.svc & BTL_SVC_KNOWN) && !(S.svc & BTL_SVC_A2DP));
        if (to_spk) {
            const bool was = spk_locked();
            S.kind     = BTL_KIND_SPEAKER;
            S.kind_why = BTL_KWHY_DROPS;
            S.av_trial = true;                  /* its A2DP's test, below (hfp_av_conn) */
            S.av_fails = 0;
            kind_store(S.bda, S.kind, S.kind_why);
            rekind_locked(was, now);
        }
        char name[32];
        strlcpy(name, S.name, sizeof name);
        const int q = S.quick;
        UNLOCK();
        if (to_spk) {
            link_log("%s hangs up a call's audio at once, mSBC and CVSD alike: a speaker -- calling it again as one",
                     name[0] ? name : "the headset");
        } else if (narrow_now) {
            narrow_set(bda);
            link_log("%s drops its audio at once: CVSD, 8 kHz, on plain EV3/HV3 links for it from now on",
                     name[0] ? name : "the headset");
        } else if (q >= 2) {
            link_log("audio closed at once again (%d in a row): next try in %d s", q, (int)(wait / 1000000));
        } else {
            link_log("audio closed");
        }
        hfp_report_state();
        return;
    }
    default:
        break;
    }
    UNLOCK();
}

/* The commands the stack does not know (batt.h): the device's battery.
 * Apple's AT+XAPL is answered as an iPhone answers it, wanting the battery
 * alone; its AT+IPHONEACCEV, and the hands-free profile's AT+BIEV, are
 * taken: OK, each. Anything else gets ERROR, as the stack answers what it
 * does not know itself -- never esp_hf_ag_unknown_at_send() with no answer,
 * whatever its comment says: it refuses a NULL and sends nothing, and the
 * device waited out its own time for an answer that never came. AT+BIEV
 * only ever comes unasked: the stack offers a headset no indicators (its
 * +BRSF leaves the bit out, and it has no AT+BIND). A speaker's own
 * hands-free link alike: its battery counts, and it carries no call. */
static void unknown_at(uint8_t *bda, const char *at)
{
    char b[18];
    batt_at_t a;
    batt_at_parse(at, &a);
    if (!a.ok) {
        ESP_LOGI(TAG, "unknown AT: %s", at ? at : "");
        esp_hf_ag_cmee_send(bda, ESP_HF_AT_RESPONSE_CODE_ERR, ESP_HF_CME_OPERATION_NOT_SUPPORTED);
        return;
    }
    if (a.cmd == BATT_AT_XAPL) esp_hf_ag_unknown_at_send(bda, BATT_XAPL_ANSWER);
    esp_hf_ag_cmee_send(bda, ESP_HF_AT_RESPONSE_CODE_OK, ESP_HF_CME_AG_FAILURE);
    LOCK();
    const bool it    = S.have && same(bda, S.bda);
    const bool first = it && S.batt == BTL_BATT_NONE;
    const bool news  = it && a.pct >= 0 && (first || S.batt_pct != a.pct);
    if (news) {
        S.batt     = a.cmd == BATT_AT_BIEV ? BTL_BATT_HFP : BTL_BATT_APPLE;
        S.batt_pct = (uint8_t)a.pct;
    }
    char name[32];
    strlcpy(name, it && S.name[0] ? S.name : bda_str(bda, b), sizeof name);
    UNLOCK();
    if (a.cmd == BATT_AT_XAPL) {
        link_log("%s speaks Apple's AT+XAPL (%s, features %d): answered as an iPhone%s", name, a.id, a.features,
                 (a.features & BATT_XAPL_BATTERY) ? ", for its battery" : " -- it reports no battery");
        return;
    }
    if (a.pct < 0) {
        ESP_LOGI(TAG, "%s: %s -- no battery in it", name, at);
        return;
    }
    if (!it) {
        ESP_LOGI(TAG, "%s: battery %d %% -- not the device's: not taken", name, a.pct);
        return;
    }
    if (!news) return;
    if (first)
        link_log("%s reports its battery: %d %% (%s)", name, a.pct,
                 a.cmd == BATT_AT_BIEV ? "the hands-free profile's indicator" : "Apple's AT+IPHONEACCEV");
    else
        link_log("%s: battery %d %%", name, a.pct);
    hfp_report_state();
}

static void hf_cb(esp_hf_cb_event_t ev, esp_hf_cb_param_t *p)
{
    switch (ev) {
    case ESP_HF_CONNECTION_STATE_EVT:
        on_connection(p->conn_stat.remote_bda, p->conn_stat.state);
        break;
    case ESP_HF_AUDIO_STATE_EVT:
        on_audio_state(p->audio_stat.state, p->audio_stat.remote_addr);
        break;
    case ESP_HF_CIND_RESPONSE_EVT:
        esp_hf_ag_cind_response(p->cind_rep.remote_addr,
                                S.call ? ESP_HF_CALL_STATUS_CALL_IN_PROGRESS : ESP_HF_CALL_STATUS_NO_CALLS,
                                ESP_HF_CALL_SETUP_STATUS_IDLE, ESP_HF_NETWORK_STATE_AVAILABLE, 5,
                                ESP_HF_ROAMING_STATUS_INACTIVE, 5, ESP_HF_CALL_HELD_STATUS_NONE);
        break;
    case ESP_HF_COPS_RESPONSE_EVT:
        esp_hf_ag_cops_response(p->cops_rep.remote_addr, DEVICE_NAME);
        break;
    case ESP_HF_CLCC_RESPONSE_EVT:
        if (S.call)
            esp_hf_ag_clcc_response(p->clcc_rep.remote_addr, 1, ESP_HF_CURRENT_CALL_DIRECTION_OUTGOING,
                                    ESP_HF_CURRENT_CALL_STATUS_ACTIVE, ESP_HF_CURRENT_CALL_MODE_VOICE,
                                    ESP_HF_CURRENT_CALL_MPTY_TYPE_SINGLE, NULL, ESP_HF_CALL_ADDR_TYPE_UNKNOWN);
        esp_hf_ag_clcc_response(p->clcc_rep.remote_addr, 0, 0, 0, 0, 0, NULL, 0);    /* OK */
        break;
    case ESP_HF_CNUM_RESPONSE_EVT:
        esp_hf_ag_cmee_send(p->cnum_rep.remote_addr, ESP_HF_AT_RESPONSE_CODE_OK, ESP_HF_CME_AG_FAILURE);
        break;
    case ESP_HF_UNAT_RESPONSE_EVT:
        unknown_at(p->unat_rep.remote_addr, p->unat_rep.unat);
        break;
    case ESP_HF_VOLUME_CONTROL_EVT: {
        LOCK();
        if (p->volume_control.type == ESP_HF_VOLUME_TYPE_SPK) S.spk = (uint8_t)p->volume_control.volume;
        else                                                  S.mic = (uint8_t)p->volume_control.volume;
        const uint8_t v[2] = { S.spk, S.mic };
        UNLOCK();
        link_send(BTL_EVT_VOLUME, v, 2);
        break;
    }
    /* The buttons. In a call, the main one hangs up; the stack has said OK.
     * The call goes on: the press is the knob's to read. */
    case ESP_HF_CHUP_RESPONSE_EVT:
        button(BTL_BTN_HANGUP, "hang up");
        break;
    case ESP_HF_ATA_RESPONSE_EVT:
        button(BTL_BTN_ANSWER, "answer");
        break;
    case ESP_HF_DIAL_EVT:
        /* The stack leaves the answer to us: there is no number to call. */
        esp_hf_ag_cmee_send(p->out_call.remote_addr, ESP_HF_AT_RESPONSE_CODE_ERR, ESP_HF_CME_AG_FAILURE);
        if (!p->out_call.num_or_loc) button(BTL_BTN_REDIAL, "redial");
        else link_log("headset dials %s: refused", p->out_call.num_or_loc);
        break;
    case ESP_HF_BVRA_RESPONSE_EVT:
        button(p->vra_rep.value ? BTL_BTN_VOICE_ON : BTL_BTN_VOICE_OFF,
               p->vra_rep.value ? "voice dial" : "voice dial off");
        break;
    case ESP_HF_WBS_RESPONSE_EVT:
        ESP_LOGI(TAG, "headset codec: %s", p->wbs_rep.codec == ESP_HF_WBS_YES ? "mSBC" : "CVSD");
        break;
    case ESP_HF_BCS_RESPONSE_EVT:
        ESP_LOGI(TAG, "codec chosen: %d", p->bcs_rep.mode);
        break;
    case ESP_HF_NREC_RESPONSE_EVT:
        ESP_LOGI(TAG, "headset echo cancelling: %d", p->nrec.state);
        break;
    case ESP_HF_PROF_STATE_EVT:
        ESP_LOGI(TAG, "hands-free profile %s", p->prof_stat.state == ESP_HF_INIT_SUCCESS ? "up" : "state change");
        break;
    default:
        ESP_LOGD(TAG, "hf event %d", ev);
        break;
    }
}

/* ---- a speaker's A2DP (a2dp.c's events) ----------------------------------- */

/* A2DP calls unanswered: as a headset's -- 10 s, 20, 40, then a minute --
 * but 2 s for the first two while its own hands-free link says it is in
 * reach. (s_mx.) */
static int64_t av_backoff_locked(void)
{
    const int f = S.page_fails < 1 ? 1 : S.page_fails;
    if (S.link != BTL_LINK_IDLE && f <= 2) return 2000000;
    return f >= 4 ? 60000000 : 10000000LL << (f - 1);
}

void hfp_av_conn(const uint8_t *bda, uint8_t link, uint16_t mtu)
{
    char b[18];
    const int64_t now = esp_timer_get_time();
    LOCK();
    /* A stranger calling us -- we are connectable for our own device -- is
     * refused once it is up, as on the hands-free side. */
    const bool ours = (S.have && same(bda, S.bda)) || (S.remembered && same(bda, S.mem));
    if (link == BTL_LINK_CONNECTING) {
        const bool news = ours && S.av == BTL_LINK_IDLE;
        if (news) {                         /* it calling us; ours is CONNECTING already */
            S.av          = BTL_LINK_CONNECTING;
            memcpy(S.av_conn, bda, 6);
            S.av_since_us = now;
            S.av_away     = false;
        }
        UNLOCK();
        if (news) hfp_report_state();
        return;
    }
    if (link == BTL_LINK_CONNECTED) {
        if (!ours) {
            UNLOCK();
            link_log("refused a connection from %s", bda_str(bda, b));
            a2dp_disconnect(bda);
            return;
        }
        if (!S.have || !same(bda, S.bda)) { /* our remembered device, calling us */
            memcpy(S.bda, bda, 6);
            strlcpy(S.name, S.mem_name, sizeof S.name);
            S.have = true;
            target_kind_locked();
            batt_forget_locked();
        }
        S.av            = BTL_LINK_CONNECTED;
        memcpy(S.av_conn, bda, 6);
        S.mtu           = mtu;
        S.media_pending = false;
        char name[32];
        strlcpy(name, S.name, sizeof name);
        if (!spk_locked()) {
            /* A headset's own: now that this chip is a music source too, a
             * headset may open A2DP to it, as to a phone. Accepted, never
             * started -- its calls are what it is for. */
            const bool tell = !S.told_av;
            S.told_av = true;
            UNLOCK();
            if (tell) link_log("%s opened A2DP too: not used -- a headset", name[0] ? name : bda_str(bda, b));
            return;
        }
        S.rekind        = false;
        S.page_fails    = 0;
        S.av_fails      = 0;
        S.av_trial      = false;            /* a speaker indeed */
        S.user_off      = false;
        S.told_av       = true;
        S.media_tries   = 0;
        S.media_fails   = 0;
        S.next_audio_us = now + 500000;     /* a moment for the speaker to settle */
        S.conns++;
        S.conn_us       = now;
        S.conn_audios   = 0;
        S.audio_us      = 0;
        if (!S.remembered || !same(S.mem, S.bda) || strcmp(S.mem_name, S.name)) {
            memcpy(S.mem, S.bda, 6);
            strlcpy(S.mem_name, S.name, sizeof S.mem_name);
            S.remembered = true;
            save_mem();
        }
        if (S.knob_speakers) kind_store(S.bda, S.kind, S.kind_why);
        const bool need_name = !S.name[0];
        UNLOCK();
        upd_headset_came();                 /* a firmware on trial: proven enough to keep */
        esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
        if (need_name) esp_bt_gap_read_remote_name((uint8_t *)bda);
        link_log("speaker connected: %s (%s), packets of %u bytes", name[0] ? name : "?", bda_str(bda, b),
                 (unsigned)mtu);
        hfp_report_state();
        return;
    }
    /* Gone, or never made. */
    if (S.av == BTL_LINK_IDLE || !same(bda, S.av_conn)) {
        UNLOCK();
        ESP_LOGI(TAG, "%s: A2DP disconnected", bda_str(bda, b));
        return;
    }
    const uint8_t was = S.av;
    S.av            = BTL_LINK_IDLE;
    S.told_av       = false;
    S.told_delay    = false;
    S.media_pending = false;
    S.suspend_asked = false;
    S.sink_delay    = 0;
    if (S.audio == BTL_AUDIO_SBC_44K) audio_gone_locked();     /* its stream went with it */
    if (!same(S.av_conn, S.bda)) {
        /* Left for another one, which the knob asked for: call it now. */
        S.next_page_us = now + 300000;
        UNLOCK();
        hfp_report_state();
        return;
    }
    if (!spk_locked()) {
        /* A headset's own: its hands-free link decides. */
        UNLOCK();
        ESP_LOGI(TAG, "%s: its A2DP closed", bda_str(bda, b));
        return;
    }
    if (S.rekind) {
        /* Down to be called again as a speaker. */
        S.next_page_us = now + 1000000;
        UNLOCK();
        hfp_report_state();
        return;
    }
    if (was == BTL_LINK_CONNECTED) {
        /* Switched off, or out of reach: call it again in a while. */
        S.next_page_us = now + 10000000;
        UNLOCK();
        link_log("speaker gone: %s", bda_str(bda, b));
        hfp_report_state();
        return;
    }
    char name[32];
    strlcpy(name, S.name, sizeof name);
    /* Its page unanswered: away, or switched off. Answered, it took no
     * A2DP: it has none, or plays for another already. */
    const bool away = S.av_away;
    if (S.av_trial && !away && ++S.av_fails >= 2) {
        /* Taken for a speaker since this chip started, for hanging up its
         * call's audio, and in reach -- but no A2DP from it, twice: a
         * headset after all, for good; the page can say otherwise. Never
         * for a verdict an earlier start came to: a speaker switched off
         * then is only away. */
        const bool was_spk = spk_locked();
        S.kind         = BTL_KIND_HEADSET;
        S.kind_why     = BTL_KWHY_NO_A2DP;
        S.av_trial     = false;
        S.page_fails   = 0;
        kind_store(S.bda, S.kind, S.kind_why);
        rekind_locked(was_spk, now);
        S.next_page_us = now + 1000000;
        UNLOCK();
        link_log("%s has no A2DP to play to: a headset after all", name[0] ? name : bda_str(bda, b));
        hfp_report_state();
        return;
    }
    /* A call that was not answered, or not taken. */
    S.page_fails++;
    S.next_page_us  = now + av_backoff_locked();
    const int fails = S.page_fails;
    if (S.remembered && !same(S.bda, S.mem)) {
        /* A new one that did not answer: back to the one we know. */
        memcpy(S.bda, S.mem, 6);
        strlcpy(S.name, S.mem_name, sizeof S.name);
        target_kind_locked();
    } else if (!S.remembered && S.page_fails >= 2) {
        S.have = false;                     /* a new one, never reached: give up */
    }
    UNLOCK();
    /* In reach, and no A2DP time after time: a headset its class made a
     * speaker of -- a car kit, say. Use as, on the page, says otherwise. */
    if (away) link_log("no answer from %s (%d)", bda_str(bda, b), fails);
    else link_log("%s is in reach but took no A2DP (%d)", name[0] ? name : bda_str(bda, b), fails);
    hfp_report_state();
}

void hfp_av_audio(const uint8_t *bda, bool started)
{
    char b[18];
    const int64_t now = esp_timer_get_time();
    LOCK();
    if (S.av != BTL_LINK_CONNECTED || !same(bda, S.av_conn)) {
        UNLOCK();
        return;
    }
    if (started) {
        /* Ours opened at its start's answer, which comes first -- or, no
         * longer wanted by then, is being suspended. One the sink started
         * itself the stack stops at once (btc_av.c): this chip is the
         * source, and starts its own. */
        const bool open = S.audio == BTL_AUDIO_SBC_44K, ours = S.suspend_asked;
        UNLOCK();
        if (!open && !ours) ESP_LOGI(TAG, "%s started its stream itself: the stack stops it", bda_str(bda, b));
        return;
    }
    /* Stopped. A suspend of ours is answered as the stack stops the stream,
     * before this (btc_a2dp_source_aa_stop_tx): asked is told here, not by
     * the answer. Wanted again meanwhile -- the telephone's next call --
     * it starts again at once. */
    const bool asked = S.suspend_asked;
    S.suspend_asked  = false;
    if (asked && S.want_audio) S.next_audio_us = 0;
    if (S.audio != BTL_AUDIO_SBC_44K) {     /* nothing of ours was open */
        UNLOCK();
        return;
    }
    const bool brief = now - S.audio_us < 10000000;
    audio_gone_locked();
    int again_s = 0;
    if (!asked && S.want_audio && spk_locked()) {
        /* The sink's own doing: started again in a moment -- later and later
         * if it keeps stopping soon after. */
        S.media_tries = brief ? S.media_tries + 1 : 1;
        int64_t wait = 2000000LL << (S.media_tries - 1 < 5 ? S.media_tries - 1 : 5);
        if (wait > 60000000) wait = 60000000;
        S.next_audio_us = now + wait;
        again_s         = (int)(wait / 1000000);
    }
    UNLOCK();
    if (again_s) link_log("the speaker stopped its audio itself: again in %d s", again_s);
    else link_log("speaker audio closed");
    hfp_report_state();
}

void hfp_av_media(uint8_t cmd, bool ok)
{
    const int64_t now = esp_timer_get_time();
    LOCK();
    if (cmd == A2DP_SUSPEND) {
        /* Its stream's own event, after this, says what closed
         * (hfp_av_audio). Refused, nothing closes. */
        if (S.media_pending && S.media_cmd == A2DP_SUSPEND) S.media_pending = false;
        if (!ok) S.suspend_asked = false;
        UNLOCK();
        return;
    }
    if (S.media_cmd == A2DP_START) S.media_pending = false;
    if (!ok) {
        S.media_fails++;
        int64_t wait = 2000000LL << (S.media_fails - 1 < 5 ? S.media_fails - 1 : 5);
        if (wait > 60000000) wait = 60000000;
        S.next_audio_us = now + wait;
        const int fails = S.media_fails;
        UNLOCK();
        link_log("speaker audio would not start (%d): again in %d s", fails, (int)(wait / 1000000));
        return;
    }
    /* Started: a start answered is a stream going -- one the stack had
     * started already too, which says so without an event of its own. */
    if (S.audio == BTL_AUDIO_SBC_44K || S.av != BTL_LINK_CONNECTED) {
        UNLOCK();                           /* open already, or its link gone since */
        return;
    }
    if (!spk_locked() || !S.want_audio) {
        /* ...and no longer wanted: the knob's call ended meanwhile, or the
         * device is a headset now. */
        S.media_pending = true;
        S.media_cmd     = A2DP_SUSPEND;
        S.suspend_asked = true;
        S.next_audio_us = now + 4000000;
        UNLOCK();
        a2dp_suspend();
        return;
    }
    S.audio         = BTL_AUDIO_SBC_44K;
    S.media_fails   = 0;
    S.suspend_asked = false;
    S.told_delay    = false;
    memcpy(s_open.bda, S.av_conn, 6);       /* for the reports, with spk_open()'s count */
    s_open.conns    = S.conns;
    s_open.conn_us  = S.conn_us;
    s_open.audios   = ++S.conn_audios;
    s_open.audio_us = now;
    S.audio_us      = now;
    spk_open();
    const unsigned long rate = S.dn_rate;
    /* Its first on this connection: how long it waited for its volume to be
     * the knob's (AV_HOLD_US). */
    const unsigned long after = S.conn_audios == 1 ? (unsigned long)((now - S.conn_us) / 100000) : 0;
    UNLOCK();
    if (after)
        link_log("speaker audio open: SBC, 44.1 kHz, the knob's %lu Hz in both channels, %lu.%lu s after it connected",
                 rate, after / 10, after % 10);
    else
        link_log("speaker audio open: SBC, 44.1 kHz, the knob's %lu Hz in both channels", rate);
    hfp_report_state();
}

/* On the stack's BTU task: the value only, for the main loop to say. */
void hfp_av_delay(uint16_t v)
{
    s_delay_value = v;
    s_delay_news  = true;
}

/* ---- the knob's commands ------------------------------------------------ */

/* "A2DP, hands-free", or "not listed". */
static const char *svc_str(char *s, size_t n, uint8_t svc)
{
    static const struct { uint8_t bit; const char *name; } k[] = {
        { BTL_SVC_A2DP, "A2DP" }, { BTL_SVC_HFP, "hands-free" }, { BTL_SVC_HSP, "headset" },
        { BTL_SVC_AVRCP, "remote control" },
    };
    s[0] = 0;
    for (size_t i = 0; i < sizeof k / sizeof k[0]; i++)
        if (svc & k[i].bit) snprintf(s + strlen(s), n - strlen(s), "%s%s", s[0] ? ", " : "", k[i].name);
    if (!s[0]) strlcpy(s, "not listed", n);
    else if (!(svc & BTL_SVC_KNOWN)) strlcat(s, " (a part list)", n);
    return s;
}

/* Why a device is what it is, in words: "its class, loudspeaker; its
 * services: A2DP, hands-free". f, its scan answer, if this boot heard one. */
static const char *why_str(char *s, size_t n, uint8_t why, const btl_found_t *f)
{
    char sv[48];
    switch (why) {
    case BTL_KWHY_CLASS:
        if (f) snprintf(s, n, "its class, %s; its services: %s", kind_minor_str(f->cod), svc_str(sv, sizeof sv, f->svc));
        else strlcpy(s, "its class and services, when a scan heard it", n);
        break;
    case BTL_KWHY_DROPS:   strlcpy(s, "it hangs up a call's audio at once", n); break;
    case BTL_KWHY_NO_A2DP: strlcpy(s, "it has no A2DP to play to", n);          break;
    case BTL_KWHY_USER:    strlcpy(s, "set on the configuration page", n);      break;
    default:               strlcpy(s, "nothing known of it yet", n);            break;
    }
    return s;
}

void hfp_on_frame(uint8_t type, const uint8_t *p, uint16_t n)
{
    char b[18];
    switch (type) {
    case BTL_AUDIO_DN:
    case BTL_AUDIO_DN_FULL:
        /* FULL is the knob's audio before its VOLUME, for a speaker whose own
         * volume is the knob's -- and nothing else: a headset, or a speaker
         * no longer set, would play it at full level. Dropped there, a
         * moment's silence until the knob has heard. */
        if (s_audio_on && (type == BTL_AUDIO_DN || (s_av_on && a2dp_volume_full()))) {
            static int16_t pcm[BTL_MAX_PAYLOAD / 2];
            memcpy(pcm, p, n & ~1u);        /* the frame's payload is not aligned */
            rs_push(&s_dn, pcm, n / 2);
        }
        break;
    case BTL_CMD_SCAN: {
        const int secs = n ? p[0] : 10;
        int len = (secs * 100 + 64) / 128;  /* in 1.28 s */
        if (len < 1) len = 1;
        if (len > 30) len = 30;
        s_nfound = 0;
        if (S.scanning) esp_bt_gap_cancel_discovery();
        esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, (uint8_t)len, 0);
        link_log("scanning for %.0f s", len * 1.28);
        break;
    }
    case BTL_CMD_CONNECT: {
        if (n < 6) break;
        if (S.scanning) esp_bt_gap_cancel_discovery();
        LOCK();
        const int64_t now  = esp_timer_get_time();
        const bool hf_up   = S.link != BTL_LINK_IDLE;
        const bool av_up   = S.av != BTL_LINK_IDLE;
        const bool other   = (hf_up || av_up) && S.have && !same(S.bda, p);
        const bool was_spk = spk_locked();
        esp_bd_addr_t old, old_av;
        memcpy(old, S.bda, 6);
        memcpy(old_av, S.av_conn, 6);
        /* The one left: its audio goes with its links, below, and its
         * battery now -- it is not this one's. */
        if (other && S.audio != BTL_AUDIO_NONE) audio_gone_locked();
        if (!S.have || !same(S.bda, p)) batt_forget_locked();
        memcpy(S.bda, p, 6);
        S.have = true;
        S.name[0] = 0;
        for (int i = 0; i < s_nfound; i++)
            if (same(s_found[i].bda, p)) strlcpy(S.name, s_found[i].name, sizeof S.name);
        if (!S.name[0] && S.remembered && same(S.mem, p)) strlcpy(S.name, S.mem_name, sizeof S.name);
        target_kind_locked();
        S.user_off     = false;
        S.page_fails   = 0;
        S.av_fails     = 0;
        S.next_page_us = 0;                 /* the tick calls it now */
        /* The one connected, asked for again: a scan since may have said
         * what it is. */
        if (!other) rekind_locked(was_spk, now);
        char name[32], why[96];
        strlcpy(name, S.name, sizeof name);
        const bool spk = S.kind == BTL_KIND_SPEAKER, speakers = S.knob_speakers;
        why_str(why, sizeof why, S.kind_why, found_get(p));
        UNLOCK();
        if (other) {
            link_log("leaving %s for %s", bda_str(old, b), name[0] ? name : "?");
            if (hf_up) esp_hf_ag_slc_disconnect(old);
            if (av_up) a2dp_disconnect(old_av);
        }
        if (speakers) link_log("%s (%s): a %s -- %s", name[0] ? name : "?", bda_str(p, b), spk ? "speaker" : "headset", why);
        hfp_report_state();
        break;
    }
    case BTL_CMD_DISCONNECT: {
        LOCK();
        S.user_off = true;
        const bool spk = spk_locked();
        if (S.link != BTL_LINK_IDLE && S.have) {
            call_down();
            esp_hf_ag_slc_disconnect(S.bda);
        }
        if (S.av != BTL_LINK_IDLE && S.have) a2dp_disconnect(S.av_conn);
        UNLOCK();
        link_log("%s hung up by the knob", spk ? "speaker" : "headset");
        break;
    }
    case BTL_CMD_FORGET: {
        if (n < 6) break;
        LOCK();
        const bool cur = S.have && same(S.bda, p);
        if (cur && S.link != BTL_LINK_IDLE) {
            call_down();
            esp_hf_ag_slc_disconnect(S.bda);
        }
        if (S.av != BTL_LINK_IDLE && same(S.av_conn, p)) a2dp_disconnect(S.av_conn);
        if (S.remembered && same(S.mem, p)) {
            S.remembered = false;
            save_mem();
        }
        if (cur) {
            S.have = false;
            batt_forget_locked();
        }
        /* Its verdict too: a scan makes of it what its class and services
         * say, as for a device never met. */
        kind_forget(p);
        btl_found_t *f = found_get(p);
        if (f) found_kind(f);
        UNLOCK();
        esp_bt_gap_remove_bond_device((uint8_t *)p);
        if (!S.remembered) esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
        link_log("forgot %s", bda_str(p, b));
        hfp_report_state();
        break;
    }
    case BTL_CMD_AUDIO: {
        if (n < 9) break;
        uint32_t dn, up;
        memcpy(&dn, p + 1, 4);
        memcpy(&up, p + 5, 4);
        if (dn < 8000 || dn > 48000) dn = 24000;
        if (up < 8000 || up > 48000) up = 24000;
        LOCK();
        const bool wanted = S.want_audio;
        S.want_audio = p[0] != 0;
        S.audio_tries = 0;
        if (S.want_audio && !wanted && spk_locked()) {
            /* A speaker's stream wanted anew -- the telephone's next call:
             * at once, the sink's stops before forgiven. */
            S.media_tries = S.media_fails = 0;
            if (!S.media_pending) S.next_audio_us = 0;
        }
        const bool changed = dn != S.dn_rate || up != S.up_rate;
        S.dn_rate = dn;
        S.up_rate = up;
        const uint8_t audio = S.audio;
        UNLOCK();
        /* Another firmware on the knob, another rate -- with the device's
         * audio open all the while: the converters again. The stack's
         * callbacks stand aside for a moment while they are rebuilt. */
        if (changed && audio != BTL_AUDIO_NONE && s_audio_on) {
            s_audio_on = false;
            vTaskDelay(pdMS_TO_TICKS(20));
            audio_rates(air_rate(audio), audio != BTL_AUDIO_SBC_44K);
            s_audio_on = true;
        }
        if (changed) link_log("the knob's audio: %lu Hz to the device, %lu Hz back",
                              (unsigned long)dn, (unsigned long)up);
        break;
    }
    case BTL_CMD_MIC:
        s_mic_on = n && p[0];
        break;
    case BTL_CMD_VOLUME:
        if (n < 2) break;
        LOCK();
        S.spk = p[0] > 15 ? 15 : p[0];
        S.mic = p[1] > 15 ? 15 : p[1];
        /* A headset's gains. A speaker's own hands-free link would set its
         * call volume, which nothing plays through. */
        if (S.link == BTL_LINK_CONNECTED && !spk_locked()) {
            esp_hf_ag_volume_control(S.bda, ESP_HF_VOLUME_CONTROL_TARGET_SPK, S.spk);
            esp_hf_ag_volume_control(S.bda, ESP_HF_VOLUME_CONTROL_TARGET_MIC, S.mic);
        }
        UNLOCK();
        break;
    case BTL_CMD_STATE:
        hfp_report_state();
        break;
    case BTL_CMD_AV_VOLUME: {
        /* The knob's VOLUME, kept for a speaker that takes it as its own --
         * the one whose A2DP is up, at once, if it does; never a headset:
         * its own A2DP, idle, plays nothing. */
        if (n < 1) break;
        esp_bd_addr_t spk;
        LOCK();
        const bool is = spk_locked() && S.knob_speakers && S.av == BTL_LINK_CONNECTED;
        memcpy(spk, S.av_conn, sizeof spk);
        UNLOCK();
        a2dp_volume(p[0], is ? spk : NULL);
        break;
    }
    case BTL_CMD_KIND: {
        /* The page's choice for a device, kept: nothing automatic changes it
         * after. The one connected is called again as that. */
        if (n < 7) break;
        const uint8_t kind = p[6] ? BTL_KIND_SPEAKER : BTL_KIND_HEADSET;
        kind_store(p, kind, BTL_KWHY_USER);
        LOCK();
        const int64_t now = esp_timer_get_time();
        btl_found_t *f = found_get(p);
        if (f) found_kind(f);
        char name[32] = "";
        bool again = false;
        if (S.have && same(S.bda, p)) {
            const bool was = spk_locked();
            S.kind      = kind;
            S.kind_why  = BTL_KWHY_USER;
            S.av_fails  = 0;
            S.av_trial  = false;
            again       = (S.link != BTL_LINK_IDLE || S.av != BTL_LINK_IDLE) && spk_locked() != was;
            rekind_locked(was, now);
            strlcpy(name, S.name, sizeof name);
        } else if (f) {
            strlcpy(name, f->name, sizeof name);
        } else if (S.remembered && same(S.mem, p)) {
            strlcpy(name, S.mem_name, sizeof name);
        }
        UNLOCK();
        if (f) link_send(BTL_EVT_FOUND, f, sizeof *f);
        link_log("%s (%s) set to a %s by the knob%s", name[0] ? name : "?", bda_str(p, b),
                 kind == BTL_KIND_SPEAKER ? "speaker" : "headset", again ? " -- calling it again as one" : "");
        hfp_report_state();
        break;
    }
    default:
        break;
    }
}

/* ---- the reports -------------------------------------------------------- */

/* The audio's numbers, every 30 s from each audio open and once more at its
 * close, with the air's (air.c); at each open, the link it got and the
 * history behind it. All from the main task, the lowest, a line a tick: a
 * line is longer than the console UART's 128-byte FIFO, and the console
 * waits on it a few milliseconds a line, more for one right after another
 * -- never in the pump, which feeds the headset. A speaker's stream has
 * lines of its own: no air of a call to measure, but the stack's queue. */
enum { J_AUDIO = 1, J_FROM = 2, J_TO = 4, J_LINK = 8, J_HISTORY = 16, J_SPK = 32, J_SPK_LINK = 64 };   /* in this order */
static unsigned s_jobs;
static struct {                             /* the audio's numbers, as taken */
    uint32_t in, out, fill, target, under, skips, bad;
    long     ppm;
} s_snap;
static struct {                             /* the history of the audio open */
    int     conns, audios;
    int64_t conn_us;
} s_hist;
static struct {                             /* a speaker's numbers, as taken: since the report before */
    uint32_t made, due, under, skips, dropped, fill, target, waiting, most, ms, stack;
    long     ppm;
    bool     closed;
} s_spk;
static struct {                             /* ...where its counts stood then */
    uint32_t made, under, skips, dropped;
    int64_t  t_us;
} s_spk_prev;

void hfp_knob_hello(void)
{
    s_knob_hellos++;
    /* Started afresh, its VOLUME may be another: a speaker's is not the
     * knob's until it says it again. */
    a2dp_knob_started();
}

/* 42 s, 17 min, 5 h 12 min. */
static const char *dur_str(char *s, size_t n, int64_t us)
{
    const unsigned long t = (unsigned long)(us / 1000000);
    if (t < 60)        snprintf(s, n, "%lu s", t);
    else if (t < 3600) snprintf(s, n, "%lu min", t / 60);
    else               snprintf(s, n, "%lu h %lu min", t / 3600, t / 60 % 60);
    return s;
}

/* Counted since the audio opened; the bad frames are the UART's, from the
 * knob since this chip started -- not the air's. */
static void log_audio(void)
{
    link_log("audio: %lu in, %lu out, fill %lu (target %lu), %lu dry, %lu skips, %ld ppm, "
             "%lu bad frames on the wire from the knob since boot",
             (unsigned long)s_snap.in, (unsigned long)s_snap.out, (unsigned long)s_snap.fill,
             (unsigned long)s_snap.target, (unsigned long)s_snap.under, (unsigned long)s_snap.skips,
             s_snap.ppm, (unsigned long)s_snap.bad);
}

/* What a bad connection asks of its past: a fresh one, or one of many on a
 * chip up all night through the knob's restarts? Memory running short? The
 * knob's hellos are its starts, and its losses of this chip (15 s unheard). */
static void log_history(void)
{
    char up[24], age[24];
    const int64_t  now    = esp_timer_get_time();
    const uint32_t caps   = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    const int      hellos = s_knob_hellos;
    link_log("history: up %s, %d knob hello%s, connection %d (%s old), audio %d on it; "
             "heap %u kB free, %u kB in one piece, %u kB lowest",
             dur_str(up, sizeof up, now), hellos, hellos == 1 ? "" : "s", s_hist.conns,
             dur_str(age, sizeof age, now - s_hist.conn_us), s_hist.audios,
             (unsigned)(heap_caps_get_free_size(caps) / 1024),
             (unsigned)(heap_caps_get_largest_free_block(caps) / 1024),
             (unsigned)(heap_caps_get_minimum_free_size(caps) / 1024));
}

/* A speaker's, in the window since the report before: the SBC frames made of
 * the knob's audio, against the 44.1 kHz the clock says were due; the
 * downlink's as a call's; what waits in the stack's queue, made and not yet
 * taken for the air -- the link keeping up, or not -- and what the queue
 * dropped, memory running short (a2dp.c). */
static void log_spk(void)
{
    link_log("speaker audio, %s%lu s: %lu frames made of %lu due, %lu dry, %lu skips, fill %lu (target %lu), "
             "%+ld ppm; %lu waiting (most %lu), %lu dropped for memory",
             s_spk.closed ? "last " : "", (unsigned long)((s_spk.ms + 500) / 1000), (unsigned long)s_spk.made,
             (unsigned long)s_spk.due, (unsigned long)s_spk.under, (unsigned long)s_spk.skips,
             (unsigned long)s_spk.fill, (unsigned long)s_spk.target, s_spk.ppm, (unsigned long)s_spk.waiting,
             (unsigned long)s_spk.most, (unsigned long)s_spk.dropped);
}

/* Its link: the packets it takes, its own delay, and the stack's BTC task,
 * which codes the SBC now besides its own work, with the stack it has; and
 * the heap, which the stack's queue of packets lives on (a2dp.c). */
static void log_spk_link(void)
{
    LOCK();
    const unsigned mtu = S.mtu, delay = S.sink_delay;
    UNLOCK();
    const uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    char d[32], st[40];
    if (delay) snprintf(d, sizeof d, "it plays %u ms behind", delay / 10);
    else strlcpy(d, "no delay report", sizeof d);
    if (s_spk.stack) snprintf(st, sizeof st, "BTC stack %lu bytes never used", (unsigned long)s_spk.stack);
    else strlcpy(st, "BTC stack not measured yet", sizeof st);
    link_log("speaker link: SBC 44.1 kHz, packets of %u bytes, %s, %s; heap %u kB free, %u kB lowest", mtu, d, st,
             (unsigned)(heap_caps_get_free_size(caps) / 1024), (unsigned)(heap_caps_get_minimum_free_size(caps) / 1024));
}

/* A speaker's stream opened at t0: its counts start from nought there
 * (spk_open). */
static void spk_start(int64_t t0)
{
    a2dp_counts_t c;
    a2dp_counts(&c);
    memset(&s_spk_prev, 0, sizeof s_spk_prev);
    s_spk_prev.t_us = t0;
    s_spk.stack     = c.btc_stack_free;
}

/* Its numbers, taken now, up to `end` -- its close, for the last. */
static void spk_take(bool closed, int64_t end)
{
    a2dp_counts_t c;
    a2dp_counts(&c);
    const uint32_t under = s_under, skips = s_skips;
    const uint32_t ms    = (uint32_t)((end - s_spk_prev.t_us) / 1000);
    s_spk.closed  = closed;
    s_spk.ms      = ms;
    s_spk.made    = c.made - s_spk_prev.made;
    s_spk.due     = (uint32_t)(((uint64_t)ms * 441 + 5 * c.spf) / (10 * c.spf));   /* ms x 44.1 / its frame */
    s_spk.under   = under - s_spk_prev.under;
    s_spk.skips   = skips - s_spk_prev.skips;
    s_spk.dropped = c.dropped - s_spk_prev.dropped;
    s_spk.fill    = rs_fill(&s_dn);
    s_spk.target  = s_dn_target;
    s_spk.ppm     = lround((s_dn.step / s_dn.nominal - 1.0) * 1e6);
    s_spk.waiting = c.waiting;
    s_spk.most    = c.waiting_max;
    s_spk.stack   = c.btc_stack_free;
    s_spk_prev.made    = c.made;
    s_spk_prev.under   = under;
    s_spk_prev.skips   = skips;
    s_spk_prev.dropped = c.dropped;
    s_spk_prev.t_us    = end;
    s_jobs |= J_SPK | J_SPK_LINK;
}

/* The numbers, taken now, all at once; their lines follow, one a tick. */
static void report(void)
{
    s_snap.in     = s_frames_in;
    s_snap.out    = s_frames_out;
    s_snap.fill   = rs_fill(&s_dn);
    s_snap.target = s_dn_target;
    s_snap.under  = s_under;
    s_snap.skips  = s_skips;
    s_snap.ppm    = lround((s_dn.step / s_dn.nominal - 1.0) * 1e6);
    s_snap.bad    = link_bad_frames();
    if (air_take(s_snap.in, s_snap.out)) s_jobs |= J_AUDIO | J_FROM | J_TO;
}

static void reports(void)
{
    static uint32_t seen;                   /* the audio open reported on */
    static bool     open, asked, spk;
    static int64_t  t_stat;
    LOCK();
    const bool     up  = S.audio != BTL_AUDIO_NONE;
    const bool     sbc = S.audio == BTL_AUDIO_SBC_44K;
    const uint32_t ses = s_session;
    const __typeof__(s_open) o = s_open;
    UNLOCK();
    const int64_t now = esp_timer_get_time();
    if (open && (!up || ses != seen)) {
        /* Closed: the part since the last report -- unless it opened again
         * already, and the counts are the new one's. */
        open = false;
        if (ses == seen) {
            if (spk) spk_take(true, o.close_us > s_spk_prev.t_us ? o.close_us : now);
            else report();
        }
    }
    if (up && ses != seen) {
        seen           = ses;
        open           = true;
        asked          = false;
        spk            = sbc;
        t_stat         = now;
        s_hist.conns   = o.conns;
        s_hist.audios  = o.audios;
        s_hist.conn_us = o.conn_us;
        if (spk) {
            spk_start(o.audio_us);
            s_jobs |= J_SPK_LINK | J_HISTORY;
        } else {
            air_opened(o.bda);
            s_jobs |= J_LINK | J_HISTORY;
        }
    }
    if (open) {
        /* Every 30 s from the open; a call's signal asked a second before,
         * to be fresh in the report. */
        if (!spk && !asked && now - t_stat >= 29000000) {
            asked = true;
            air_ask_signal();
        }
        if (now - t_stat >= 30000000) {
            t_stat = now;
            asked  = false;
            if (spk) spk_take(false, now);
            else report();
        }
    }
    const unsigned j = s_jobs & -s_jobs;    /* the first due */
    s_jobs &= ~j;
    switch (j) {
    case J_AUDIO:    log_audio();    break;
    case J_FROM:     air_log_from(); break;
    case J_TO:       air_log_to();   break;
    case J_LINK:     air_log_link(); break;
    case J_HISTORY:  log_history();  break;
    case J_SPK:      log_spk();      break;
    case J_SPK_LINK: log_spk_link(); break;
    default:                         break;
    }
}

/* ---- an update of this chip's firmware coming in (upd.c) ---------------- */

/* No link to a device -- hands-free or A2DP -- no scan, no audio of either
 * kind, nothing asked of either. */
static bool idle_locked(void)
{
    return S.link == BTL_LINK_IDLE && !S.scanning && S.audio == BTL_AUDIO_NONE && !S.audio_pending && !S.call &&
           S.av == BTL_LINK_IDLE && !S.media_pending;
}

bool hfp_idle(void)
{
    LOCK();
    const bool idle = idle_locked();
    UNLOCK();
    return idle;
}

bool hfp_try_hold(void)
{
    LOCK();
    const bool idle = idle_locked();
    if (idle) s_hold = true;
    UNLOCK();
    return idle;
}

void hfp_hold(bool on)
{
    LOCK();
    s_hold = on;
    if (!on) {
        /* Never sooner than it was due: a call held back goes out a
         * second from now, one due later keeps its time. */
        const int64_t soon = esp_timer_get_time() + 1000000;
        if (S.next_page_us < soon) S.next_page_us = soon;
    }
    UNLOCK();
}

bool hfp_audio_open(void)
{
    LOCK();
    const bool open = S.audio != BTL_AUDIO_NONE;
    UNLOCK();
    return open;
}

/* ---- now and then ------------------------------------------------------- */

/* The sink's delay report, come on the stack's BTU task: said here, once a
 * stream, and to the knob with the state while one plays. */
static void delay_news(void)
{
    if (!s_delay_news) return;
    s_delay_news = false;
    const uint16_t v = s_delay_value;
    LOCK();
    if (S.av == BTL_LINK_IDLE) {            /* a report from a link gone since */
        UNLOCK();
        return;
    }
    const bool changed = v != S.sink_delay;
    const bool tell    = !S.told_delay;
    const bool open    = S.audio == BTL_AUDIO_SBC_44K;
    S.sink_delay = v;
    S.told_delay = true;
    UNLOCK();
    if (tell) link_log("the speaker plays %u ms behind what it is sent (its own report)", (unsigned)(v / 10));
    if (open && changed) hfp_report_state();
}

/* A speaker's volume (a2dp.c): its sets unanswered, its lines -- said with
 * its name, and only for a speaker, its own remote control. */
static void volume_tick(void)
{
    char name[32];
    esp_bd_addr_t spk;
    LOCK();
    const bool is = spk_locked() && S.knob_speakers && S.av == BTL_LINK_CONNECTED;
    strlcpy(name, S.name, sizeof name);
    memcpy(spk, S.av_conn, sizeof spk);
    UNLOCK();
    a2dp_tick(is ? name : NULL, is ? spk : NULL);
}

void hfp_tick(void)
{
    char b[18];
    reports();
    /* The verdicts to NVS, never while audio is open: a flash write holds
     * both cores' caches a moment. */
    if (S.audio == BTL_AUDIO_NONE) kind_flush();
    delay_news();
    volume_tick();
    const int64_t now = esp_timer_get_time();
    LOCK();
    /* A headset whose call's audio has stayed open: one for sure. Marked,
     * so that drops of its audio later -- a phone of its own taking it, say
     * -- never make it a speaker (on_audio_state). Only for a knob that
     * knows speakers: the table of one that does not stays as it was. */
    if (S.knob_speakers && !spk_locked() && !S.held && sco_open_locked() && S.audio_us &&
        now - S.audio_us >= QUICK_US) {
        S.held = true;
        kind_set_held(S.bda, S.kind, S.kind_why);
        char name[32];
        strlcpy(name, S.name, sizeof name);
        UNLOCK();
        link_log("%s held a call's audio: never taken for a speaker by its drops", name[0] ? name : "the headset");
        LOCK();
    }
    const bool spk = spk_locked();
    /* Call the device, as a phone does its own when it comes in reach --
     * not while an update comes in: the transfer would only stop for it. A
     * headset on its hands-free link; a speaker on A2DP, once the knob has
     * said it knows speakers. */
    if (S.have && !S.scanning && !S.user_off && now >= S.next_page_us && !s_hold) {
        if (!spk && S.link == BTL_LINK_IDLE) {
            S.link         = BTL_LINK_CONNECTING;
            memcpy(S.conn, S.bda, 6);
            S.next_page_us = now + 30000000;    /* until the answer says otherwise */
            S.rekind       = false;
            char name[32];
            strlcpy(name, S.name, sizeof name);
            esp_bd_addr_t bda;
            memcpy(bda, S.bda, 6);
            UNLOCK();
            link_log("calling %s (%s)", name[0] ? name : "the headset", bda_str(bda, b));
            esp_hf_ag_slc_connect(bda);
            hfp_report_state();
            return;
        }
        if (spk && S.av == BTL_LINK_IDLE && S.knob_speakers) {
            S.av           = BTL_LINK_CONNECTING;
            memcpy(S.av_conn, S.bda, 6);
            S.av_since_us  = now;
            S.av_away      = false;
            S.next_page_us = now + 30000000;    /* until the answer says otherwise */
            S.rekind       = false;
            char name[32];
            strlcpy(name, S.name, sizeof name);
            esp_bd_addr_t bda;
            memcpy(bda, S.bda, 6);
            UNLOCK();
            link_log("calling the speaker %s (%s)", name[0] ? name : "?", bda_str(bda, b));
            a2dp_connect(bda);
            hfp_report_state();
            return;
        }
    }
    /* An A2DP link being made, and no word of it in 30 s: the stack lost the
     * request (one that came while its state machine had no place for it).
     * Taken as a call not answered. */
    if (S.av == BTL_LINK_CONNECTING && now - S.av_since_us > 30000000) {
        S.av = BTL_LINK_IDLE;
        S.page_fails++;
        S.next_page_us = now + av_backoff_locked();
        const int fails = S.page_fails;
        esp_bd_addr_t bda;
        memcpy(bda, S.av_conn, 6);
        UNLOCK();
        link_log("no answer from %s (%d): no word of its A2DP in 30 s", bda_str(bda, b), fails);
        hfp_report_state();
        return;
    }
    if (!spk) {
        /* Its audio, while the knob wants it -- a few tries, then the knob asks. */
        if (S.audio_pending && now > S.next_audio_us) S.audio_pending = false;
        if (S.link == BTL_LINK_CONNECTED && S.want_audio && S.audio == BTL_AUDIO_NONE &&
            !S.audio_pending && S.audio_tries < 4 && now >= S.next_audio_us) {
            S.audio_tries++;
            S.audio_pending = true;
            S.next_audio_us = now + 4000000;
            call_up();
        }
        if (S.link == BTL_LINK_CONNECTED && !S.want_audio && S.call) call_down();
        UNLOCK();
        return;
    }
    /* A speaker: never a call; its stream while the knob wants audio --
     * always on a radio, the telephone for its calls -- and only once the
     * knob has said it knows speakers: before its hello, the device is
     * neither called nor said to be there, and plays nothing. And not at its
     * own volume first: one that may take the knob's has it before it plays
     * -- or AV_HOLD_US after it connected, whatever it is. */
    if (S.call) call_down();
    if (S.media_pending && now > S.next_audio_us) S.media_pending = false;     /* an answer lost */
    if (S.av == BTL_LINK_CONNECTED && S.knob_speakers && S.want_audio && S.audio == BTL_AUDIO_NONE &&
        !S.media_pending && now >= S.next_audio_us &&
        (now - S.conn_us >= AV_HOLD_US || a2dp_volume_settled(S.av_conn))) {
        S.media_pending = true;
        S.media_cmd     = A2DP_START;
        S.next_audio_us = now + 4000000;
        UNLOCK();
        a2dp_start();
        return;
    }
    if (S.av == BTL_LINK_CONNECTED && !S.want_audio && S.audio != BTL_AUDIO_NONE && !S.media_pending) {
        S.media_pending = true;
        S.media_cmd     = A2DP_SUSPEND;
        S.suspend_asked = true;
        S.next_audio_us = now + 4000000;
        UNLOCK();
        a2dp_suspend();
        return;
    }
    UNLOCK();
}

void hfp_knob_flags(uint8_t flags)
{
    const bool speakers = flags & BTL_HELLO_SPEAKERS;
    LOCK();
    const bool first = !S.knob_known;
    const bool fell  = S.knob_speakers && !speakers;
    const bool was   = spk_locked();
    S.knob_known     = true;
    S.knob_speakers  = speakers;
    /* A speaker to a knob that does not know speakers -- or the other way
     * round -- is called again as what it is to this one. */
    rekind_locked(was, esp_timer_get_time());
    UNLOCK();
    if (!speakers && (first || fell))
        link_log("this knob's firmware plays to headsets only: every device is a headset to it");
    /* One that plays to speakers but sets none's volume: they keep their
     * own, never set from here unasked -- where their own controls left it,
     * or a knob's firmware that did set it. Said when it starts so. */
    static int8_t vol_was = -1;
    const int8_t  vol     = speakers && (flags & BTL_HELLO_AV_VOLUME);
    a2dp_knob_av(vol);
    if (speakers && vol != vol_was && !vol)
        link_log("this knob's firmware scales a speaker's sound itself: a speaker keeps its own volume, where its "
                 "own controls -- or a knob's firmware before this -- left it");
    vol_was = speakers ? vol : -1;
}

void hfp_init(void)
{
    s_mx = xSemaphoreCreateMutex();
    /* The verdicts first: the remembered device's is among them. */
    kind_load();
    load_mem();
    target_kind_locked();

    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_BLE));
    esp_bt_controller_config_t bc = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&bc));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT));
    /* What the headset hears is this chip's transmit power, which the
     * controller keeps between 0 and +3 dBm unless told -- the low end of
     * Bluetooth -- and the link has little to spare: a headset 30 cm away
     * came in here up to 20 dB under the controller's target (2026-10-02).
     * That is its signal, not ours, but the path loses as much either way,
     * so ours reaches it as faintly. +3 to +9 dBm: the floor where the
     * ceiling was, the ceiling the chip's most; the controller's power
     * control works between them. What it gains is heard in the headset
     * only: the reports' numbers are all of the way here, which the
     * headset's own power decides. Before anything transmits, as the call
     * asks. */
    esp_err_t pe = esp_bredr_tx_power_set(ESP_PWR_LVL_P3, ESP_PWR_LVL_P9);
    if (pe != ESP_OK) ESP_LOGW(TAG, "transmit power left as it was: %s", esp_err_to_name(pe));
    esp_bluedroid_config_t cfg = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bluedroid_init_with_cfg(&cfg));
    ESP_ERROR_CHECK(esp_bluedroid_enable());
    air_prefer_master();

    esp_bt_gap_register_callback(gap_cb);
    esp_bt_gap_set_device_name(DEVICE_NAME);
    /* A phone, to a headset: its class of device says so. */
    esp_bt_cod_t cod = {
        .major   = ESP_BT_COD_MAJOR_DEV_PHONE,
        .minor   = 0x03,                    /* smartphone */
        .service = ESP_BT_COD_SRVC_AUDIO | ESP_BT_COD_SRVC_TELEPHONY,
    };
    esp_bt_gap_set_cod(cod, ESP_BT_SET_COD_ALL);
    esp_bt_io_cap_t iocap = ESP_BT_IO_CAP_NONE;
    esp_bt_gap_set_security_param(ESP_BT_SP_IOCAP_MODE, &iocap, sizeof iocap);
    esp_bt_pin_code_t pin;
    memset(pin, '0', sizeof pin);
    esp_bt_gap_set_pin(ESP_BT_PIN_TYPE_FIXED, 4, pin);

    esp_hf_ag_register_callback(hf_cb);
    esp_hf_ag_init();
    /* A speaker's music source beside it: one more profile on the same
     * links. */
    a2dp_init();
    /* Reachable for our own device, which calls the phone it knows when it
     * is switched on; never discoverable: headsets and speakers do not look
     * for phones. */
    esp_bt_gap_set_scan_mode(S.remembered ? ESP_BT_CONNECTABLE : ESP_BT_NON_CONNECTABLE,
                             ESP_BT_NON_DISCOVERABLE);
    S.next_page_us = esp_timer_get_time() + 1500000;

    xTaskCreatePinnedToCore(pump_task, "pump", 4096, NULL, 13, &s_pump, 1);
    char b[18];
    esp_power_level_t lo = ESP_PWR_LVL_N0, hi = ESP_PWR_LVL_P3;
    esp_bredr_tx_power_get(&lo, &hi);
    ESP_LOGI(TAG, "Bluetooth up as %s (%s), transmitting %+d to %+d dBm%s%s", DEVICE_NAME,
             bda_str(esp_bt_dev_get_address(), b), -12 + 3 * (int)lo, -12 + 3 * (int)hi,
             !S.remembered ? "" : S.kind == BTL_KIND_SPEAKER ? ", speaker " : ", headset ",
             S.remembered ? S.mem_name : "");
}
