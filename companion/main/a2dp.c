/* A speaker. See a2dp.h.
 *
 * The stack codes the SBC itself (Bluedroid's internal codec), from 16-bit
 * stereo PCM at 44.1 kHz, which every A2DP sink must take. It asks for that
 * PCM on its BTC task, a 30 ms tick of its own clock at a time, one SBC
 * frame (128 samples, 512 bytes) a call: data_cb(), below. The knob's one
 * channel goes into both: a single speaker box keeps no stereo, and both
 * channels the same cost the joint-stereo coder nothing.
 *
 * What it makes waits in the stack's queue until its BTU task takes a packet
 * of frames for the air -- and when the link stalls (the speaker at the edge
 * of reach), the queue grows by a 4 kB buffer a packet, up to 27 of them:
 * more memory than this chip has. So when the internal heap is under
 * A2DP_HEAP_FLOOR the queue is dropped -- the stack's own flush, as it does
 * while a stream stops: late audio is no use to anyone listening -- and the
 * stream goes on at its pace. Not made at all instead, the frames would be
 * owed: the stack counts every one it asked for and did not get, and asks
 * for them all again, twice as fast, once memory is back
 * (btc_media_aa_prep_sbc_2_send). A wrap of the stack's queue reader (as
 * air.c wraps four of its functions, CMakeLists.txt) counts what leaves it,
 * for the reports only.
 *
 * Its volume: AVRCP, the speaker's remote control, this chip its controller
 * as a phone is. A speaker that takes its volume from its source (absolute
 * volume, AVRCP 1.4 on: it lists volume changes among its events) is set to
 * the knob's VOLUME as the knob says (a2dp_volume) -- as it comes, the
 * knob's last word kept for it -- and tells of its own changes -- its
 * buttons, its knob -- which the knob's VOLUME follows; the knob then sends
 * it its audio before the VOLUME, its own amplifier the volume now. Its
 * first answer must not be louder than asked, and its sets must not go
 * unanswered, or the knob scales what it sends again (BTL_AV_REFUSED) until
 * its next VOLUME. Only the speaker's own remote control counts: the stack
 * keeps one, whoever's it is. Never set unasked: a knob that does not say
 * leaves it where it is, and scales what it sends, as with a speaker that
 * does not take it. Nor anything else of AVRCP: its play and pause keys are
 * refused ("not implemented"), as by a source that has none.
 *
 * The stack builds its A2DP sink beside the source, whatever the build, and
 * the sink's SBC decoder keeps 9.7 kB of this chip's static RAM for itself.
 * A source never uses it, and the RAM is wanted: below, a wrap of each of the
 * sink's entry points that other parts of the stack call (CMakeLists.txt)
 * leaves the sink's code unreferenced, and the linker drops it with its
 * RAM. */
#include "a2dp.h"

#include <stdio.h>
#include <string.h>

#include "bt_link_proto.h"
#include "esp_a2dp_api.h"
#include "esp_avrc_api.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hfp.h"
#include "link.h"

/* Bluedroid's own headers, for the wraps' types (CMakeLists.txt) and its
 * queue's drop flag. */
#include "btc_a2dp_sink.h"
#include "btc_a2dp_source.h"
#include "btc_av.h"
#include "osi/allocator.h"

static const char *TAG = "a2dp";

/* The stack's queue holds 4112-byte buffers, 1-3 in flight on a good link:
 * 24 kB left is room for those and the rest of the chip, a stalled link
 * having taken some 12-15 of them by then. */
#define A2DP_HEAP_FLOOR (24 * 1024)
/* The stack drops its queue at the end of the packet being made while its
 * flag is up; a packet has 15 SBC frames at most, so 16 on it has. */
#define FLUSH_FRAMES 16

/* SBC frames, counted on the BTC task (made, dropped, most) and the BTU task
 * (sent), each by its one writer; read by the main task's reports. The
 * reset only takes where they stand. */
static volatile uint32_t s_made, s_sent, s_dropped, s_most;
static uint32_t          s_made0, s_sent0, s_dropped0;
static volatile uint32_t s_spf;             /* stereo samples an SBC frame: 128, 16 blocks of 8 subbands */
static int               s_flush;           /* frames to go with the stack's drop flag up; 0, down */
static TaskHandle_t      s_btc;             /* the task the stack asks for PCM on */

/* Made, and neither taken for the air nor dropped: in the stack's queue. A
 * packet's worth off at most just after a drop, and none less than none. */
static uint32_t waiting(void)
{
    const int32_t w = (int32_t)((s_made - s_made0) - (s_sent - s_sent0) - (s_dropped - s_dropped0));
    return w > 0 ? (uint32_t)w : 0;
}

/* ---- on the stack's BTC task --------------------------------------------- */

/* The stack's: len bytes of 16-bit stereo PCM for its SBC coder -- one SBC
 * frame's, a call. Never a log, a lock or a UART here: the stack's task
 * never waits. */
static int32_t data_cb(uint8_t *buf, int32_t len)
{
    if (!buf || len <= 0) return 0;         /* -1: the stack flushes -- nothing is kept here */
    if (!s_btc) s_btc = xTaskGetCurrentTaskHandle();
    if (s_flush && --s_flush == 0) {
        /* Dropped by now: all that was made and not taken for the air. */
        btc_a2dp_source_set_tx_flush(false);
        s_dropped += waiting();
    }
    if (!s_flush && heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) < A2DP_HEAP_FLOOR) {
        /* Memory short: the queue goes. This frame is made all the same --
         * one not made would be owed (see the top) -- and goes with it. */
        s_flush = FLUSH_FRAMES;
        btc_a2dp_source_set_tx_flush(true);
    }
    static int16_t mono[256];
    const int32_t  smp = len / 4;
    for (int32_t done = 0; done < smp;) {
        const int32_t n = smp - done < 256 ? smp - done : 256;
        hfp_dn_pull(mono, (size_t)n);
        /* The one channel in both, byte by byte: the stack's buffer need not
         * be aligned for us. */
        uint8_t *o = buf + 4 * done;
        for (int32_t i = 0; i < n; i++, o += 4) {
            const uint16_t v = (uint16_t)mono[i];
            o[0] = o[2] = (uint8_t)v;
            o[1] = o[3] = (uint8_t)(v >> 8);
        }
        done += n;
    }
    s_made++;
    s_spf = (uint32_t)smp;
    const uint32_t w = waiting();
    if (w > s_most) s_most = w;
    return smp * 4;
}

static void cb(esp_a2d_cb_event_t ev, esp_a2d_cb_param_t *p)
{
    switch (ev) {
    case ESP_A2D_CONNECTION_STATE_EVT: {
        const esp_a2d_connection_state_t st = p->conn_stat.state;
        if (st == ESP_A2D_CONNECTION_STATE_DISCONNECTING) break;
        hfp_av_conn(p->conn_stat.remote_bda,
                    st == ESP_A2D_CONNECTION_STATE_CONNECTED    ? BTL_LINK_CONNECTED
                    : st == ESP_A2D_CONNECTION_STATE_CONNECTING ? BTL_LINK_CONNECTING
                                                                : BTL_LINK_IDLE,
                    p->conn_stat.audio_mtu);
        break;
    }
    case ESP_A2D_AUDIO_STATE_EVT:
        hfp_av_audio(p->audio_stat.remote_bda, p->audio_stat.state == ESP_A2D_AUDIO_STATE_STARTED);
        break;
    case ESP_A2D_MEDIA_CTRL_ACK_EVT: {
        /* One request at a time, the stack's rule: a check, then the start;
         * or a suspend. Busy is a no. */
        const esp_a2d_media_ctrl_t cmd = p->media_ctrl_stat.cmd;
        const bool ok = p->media_ctrl_stat.status == ESP_A2D_MEDIA_CTRL_ACK_SUCCESS;
        if (cmd == ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY) {
            if (ok) esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_START);
            else hfp_av_media(A2DP_START, false);
        } else if (cmd == ESP_A2D_MEDIA_CTRL_START) {
            hfp_av_media(A2DP_START, ok);
        } else if (cmd == ESP_A2D_MEDIA_CTRL_SUSPEND) {
            hfp_av_media(A2DP_SUSPEND, ok);
        }
        break;
    }
    case ESP_A2D_AUDIO_CFG_EVT: {
        /* What the sink took of what this chip offers: 44.1 kHz only. */
        const esp_a2d_cie_sbc_t *s = &p->audio_cfg.mcc.cie.sbc_info;
        ESP_LOGI(TAG, "SBC as the speaker takes it: %s kHz, %s, bitpool %u-%u",
                 s->samp_freq == ESP_A2D_SBC_CIE_SF_44K ? "44.1" : s->samp_freq == ESP_A2D_SBC_CIE_SF_48K ? "48" : "?",
                 s->ch_mode == ESP_A2D_SBC_CIE_CH_MODE_JOINT_STEREO ? "joint stereo"
                 : s->ch_mode == ESP_A2D_SBC_CIE_CH_MODE_STEREO     ? "stereo"
                 : s->ch_mode == ESP_A2D_SBC_CIE_CH_MODE_MONO       ? "mono"
                                                                    : "dual channel",
                 (unsigned)s->min_bitpool, (unsigned)s->max_bitpool);
        break;
    }
    case ESP_A2D_REPORT_SNK_DELAY_VALUE_EVT:
        /* On the stack's BTU task, this one. */
        hfp_av_delay(p->a2d_report_delay_value_stat.delay_value);
        break;
    case ESP_A2D_PROF_STATE_EVT:
        if (p->a2d_prof_stat.init_state == ESP_A2D_INIT_SUCCESS)
            ESP_LOGI(TAG, "A2DP source up: SBC, 44.1 kHz, the knob's one channel in both");
        else
            ESP_LOGW(TAG, "A2DP source down");
        break;
    default:
        ESP_LOGD(TAG, "a2dp event %d", ev);
        break;
    }
}

/* ---- on the stack's BTU task --------------------------------------------- */

/* The stack's queue reader, called from bta_av_co.c, defined in
 * btc_a2dp_source.c: each packet of SBC frames it takes for the air. Typed
 * after the stack's declaration, as air.c's are. */
extern __typeof__(btc_a2dp_source_audio_readbuf) __real_btc_a2dp_source_audio_readbuf;
__typeof__(btc_a2dp_source_audio_readbuf) __wrap_btc_a2dp_source_audio_readbuf;
BT_HDR *__wrap_btc_a2dp_source_audio_readbuf(void)
{
    BT_HDR *pkt = __real_btc_a2dp_source_audio_readbuf();
    if (pkt) s_sent += pkt->layer_specific;  /* the SBC frames in it */
    return pkt;
}

/* ---- its volume: AVRCP, the speaker's remote control ---------------------- */

/* The stack's transaction labels: one for its events, one for the notice of
 * a change, and the sets taking turns among the rest -- consecutive commands
 * want different ones, and a late answer to one set is never another's. */
enum { TL_CAPS = 1, TL_NOTE = 2, TL_SET0 = 3, TL_SETS = 12 };

/* A set unanswered this long was refused -- the stack tells only of one
 * taken -- or lost; so many in a row, the dial turning or not, and the knob
 * scales what it sends again, as for a speaker that keeps its own. */
#define SET_ANSWER_US 1500000
#define SET_TRIES     3
/* Its remote control up this long with no word of its events: it has none
 * to tell (no AVRCP 1.3 metadata), and no volume of its own to set. */
#define EVENTS_US     4000000
/* Its first answer to the knob, the volume it set: a step of its own -- 8 to
 * 16 of them over the range -- at most this above the one asked, or the knob
 * does not take it at its word: its audio goes at full level only to a
 * speaker seen to turn itself down. Any quieter is no harm. */
#define NEAR          16
/* Word of its volume this soon after a set, and this near a volume sent or
 * answered, is that set's own echo -- late, or in a step of its own -- not a
 * turn of its own: taken for one, the dial would snap back at every step. A
 * small turn of its own right after the knob's is missed so. */
#define ECHO_US       1500000

/* Lines for the main loop to say (a2dp_tick), and the state to report: never
 * on the stack's task. A connection's lines wait for its speaker's name (its
 * remote control may come before its A2DP); FAR, GAVE_UP and OTHER are said
 * once a connection, GAVE_UP again after a set taken. */
enum { SAY_FEATS = 1, SAY_TAKES = 2, SAY_NOT = 4, SAY_SILENT = 8, SAY_SET = 16, SAY_OWN = 32,
       SAY_GAVE_UP = 64, SAY_FAR = 128, SAY_OTHER = 256, SAY_STATE = 512 };
#define SAY_LINES (SAY_STATE - 1)
#define SAY_ONCE  (SAY_FAR | SAY_GAVE_UP | SAY_OTHER)

/* The BTC task (the stack's answers), link_rx (the knob's VOLUME and audio)
 * and the main loop (a2dp_tick) all come here, under s_vmux. */
static portMUX_TYPE s_vmux = portMUX_INITIALIZER_UNLOCKED;
static struct {
    bool          up;                   /* a remote control is connected -- */
    uint8_t       bda[6];               /* ...this device's */
    volatile bool mine;                 /* ...the speaker's, as hfp.c last named it */
    bool          asked;                /* ...its events asked for, no answer yet */
    bool          takes;                /* it lists volume changes among them: absolute volume */
    volatile bool set;                  /* a set of the knob's taken since the knob last started */
    bool          refused;              /* ...or not: a first answer louder than asked, or SET_TRIES
                                           unanswered -- until the knob's next VOLUME */
    bool          knob_av;              /* the knob's hello says it sets a speaker's volume */
    bool          busy;                 /* a set sent, not answered */
    bool          pending;              /* the knob's VOLUME, not sent yet */
    uint8_t       knob;                 /* the knob's VOLUME, 0-100, as it last said; 0xFF none since it started */
    uint8_t       want;                 /* ...as the speaker's, 0-127 */
    uint8_t       sent, sent2;          /* the last two sets sent, 0-127 (0xFF none): their echoes are no turn */
    uint8_t       now;                  /* its volume as it last said, 0-127; 0xFF none */
    uint8_t       was;                  /* ...as it first said: its own, before the knob's -- for its line */
    uint8_t       first, first_said;    /* the set it answered first, and what it said, for its line */
    uint8_t       tries;                /* sets unanswered in a row */
    uint8_t       tl;                   /* the next set's label, of TL_SETS */
    uint8_t       turns;                /* its own changes, since this chip started */
    uint8_t       epoch, busy_epoch;    /* the knob's starts; the one the set under way was sent in */
    uint16_t      say, told;            /* SAY_*: lines to say; the once-a-connection ones said */
    uint16_t      tg_feat;              /* its remote control's features, for the line */
    uint32_t      feat;
    int64_t       up_us, busy_us, set_us;   /* connected; the set under way sent; a set last sent or answered */
} s_vol = { .now = 0xFF, .was = 0xFF, .sent = 0xFF, .sent2 = 0xFF, .knob = 0xFF };

/* Is the remote control the speaker's -- spk, its address from hfp.c, NULL
 * none? Kept for the calls without one: the stack's answers, the knob's
 * audio. (s_vmux held.) */
static bool mine_locked(const uint8_t *spk)
{
    s_vol.mine = s_vol.up && spk && !memcmp(spk, s_vol.bda, sizeof s_vol.bda);
    return s_vol.mine;
}

/* The knob's VOLUME, if one is to go and nothing else is under way, to the
 * speaker's own remote control: taken for sending, by whoever finds it --
 * the knob's command, the speaker's answer, the main loop -- and sent
 * outside the lock. (s_vmux held.) */
static bool claim_locked(uint8_t *vol, uint8_t *tl)
{
    if (!s_vol.mine || !s_vol.takes || !s_vol.pending || s_vol.busy || s_vol.refused || s_vol.knob > 100)
        return false;
    s_vol.pending    = false;
    s_vol.busy       = true;
    s_vol.busy_us    = s_vol.set_us = esp_timer_get_time();
    s_vol.busy_epoch = s_vol.epoch;
    s_vol.sent2      = s_vol.sent;
    s_vol.sent       = s_vol.want;
    *vol = s_vol.want;
    *tl  = (uint8_t)(TL_SET0 + s_vol.tl);
    s_vol.tl = (uint8_t)((s_vol.tl + 1) % TL_SETS);
    return true;
}

/* v within NEAR of b, a volume said or sent (0xFF: none). */
static bool near_to(uint8_t v, uint8_t b)
{
    return b <= 127 && (v > b ? v - b : b - v) <= NEAR;
}

/* Its volume, as it tells of it: the notice of a change, or -- the stack's
 * INTERIM, below -- where it stands as the next notice is asked for. A turn
 * of its own -- its buttons, its knob -- for the knob's VOLUME to follow,
 * only while it has the knob's, with no set of ours under way and no echo of
 * one; before that it is its own, only noted. (s_vmux held.) */
static void heard_locked(uint8_t v, int64_t t)
{
    if (!s_vol.up || !s_vol.takes) return;
    const bool echo = s_vol.busy || (t - s_vol.set_us < ECHO_US && (near_to(v, s_vol.sent) ||
                                                                    near_to(v, s_vol.sent2) ||
                                                                    near_to(v, s_vol.now)));
    if (s_vol.set && !echo && v != s_vol.now) {
        s_vol.turns++;
        s_vol.say |= SAY_OWN;
    }
    if (s_vol.was == 0xFF) s_vol.was = v;
    if (v != s_vol.now) s_vol.say |= SAY_STATE;  /* where it plays: the knob's own sums use it */
    s_vol.now = v;
}

/* The stack's, on its BTC task: its remote control's answers. Nothing for
 * the knob here: the main loop says what came (a2dp_tick). */
static void rc_cb(esp_avrc_ct_cb_event_t ev, esp_avrc_ct_cb_param_t *p)
{
    uint8_t vol = 0, tl = 0;
    bool    go  = false;
    const int64_t t = esp_timer_get_time();
    switch (ev) {
    case ESP_AVRC_CT_CONNECTION_STATE_EVT: {
        const bool up = p->conn_stat.connected;
        portENTER_CRITICAL(&s_vmux);
        s_vol.up      = up;
        memcpy(s_vol.bda, p->conn_stat.remote_bda, sizeof s_vol.bda);
        s_vol.mine    = false;              /* until hfp.c names the speaker: a tick */
        s_vol.asked   = up;
        s_vol.takes   = false;
        s_vol.set     = false;
        s_vol.refused = false;
        s_vol.busy    = false;
        /* The knob's VOLUME, if it has said one since it started: set as
         * soon as this one says it takes it. */
        s_vol.pending = s_vol.knob <= 100;
        s_vol.now     = s_vol.was = 0xFF;
        s_vol.sent    = s_vol.sent2 = 0xFF;
        s_vol.tries   = 0;
        s_vol.up_us   = t;
        s_vol.say     = SAY_STATE;          /* the lines of the one before go with it */
        s_vol.told    = 0;
        portEXIT_CRITICAL(&s_vmux);
        /* What it can tell of: its volume among it, or not. */
        if (up) esp_avrc_ct_send_get_rn_capabilities_cmd(TL_CAPS);
        break;
    }
    case ESP_AVRC_CT_REMOTE_FEATURES_EVT:
        portENTER_CRITICAL(&s_vmux);
        s_vol.feat    = p->rmt_feats.feat_mask;
        s_vol.tg_feat = p->rmt_feats.tg_feat_flag;
        s_vol.say    |= SAY_FEATS;
        portEXIT_CRITICAL(&s_vmux);
        break;
    case ESP_AVRC_CT_GET_RN_CAPABILITIES_RSP_EVT: {
        esp_avrc_rn_evt_cap_mask_t evs = p->get_rn_caps_rsp.evt_set;
        const bool takes = esp_avrc_rn_evt_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_TEST, &evs,
                                                              ESP_AVRC_RN_VOLUME_CHANGE);
        portENTER_CRITICAL(&s_vmux);
        const bool up = s_vol.up;
        if (up) {
            s_vol.asked = false;
            s_vol.takes = takes;
            s_vol.say  |= takes ? SAY_TAKES | SAY_STATE : SAY_NOT;
            go = claim_locked(&vol, &tl);
        }
        portEXIT_CRITICAL(&s_vmux);
        /* Its own changes from now on: a notice each, asked for again after
         * each one -- its first answer, at once, its volume as it stands. */
        if (up && takes) esp_avrc_ct_send_register_notification_cmd(TL_NOTE, ESP_AVRC_RN_VOLUME_CHANGE, 0);
        break;
    }
    case ESP_AVRC_CT_CHANGE_NOTIFY_EVT: {
        if (p->change_ntf.event_id != ESP_AVRC_RN_VOLUME_CHANGE) break;
        portENTER_CRITICAL(&s_vmux);
        const bool up = s_vol.up && s_vol.takes;
        heard_locked(p->change_ntf.event_parameter.volume & 0x7F, t);
        portEXIT_CRITICAL(&s_vmux);
        if (up) esp_avrc_ct_send_register_notification_cmd(TL_NOTE, ESP_AVRC_RN_VOLUME_CHANGE, 0);
        break;
    }
    case ESP_AVRC_CT_SET_ABSOLUTE_VOLUME_RSP_EVT: {
        /* Taken: the volume it set, a step of its own near the one asked,
         * maybe. A set sent before the knob last started sets nothing: the
         * knob's VOLUME may be another since. Nor a first answer louder than
         * asked -- unless a VOLUME of the knob's waits, which goes next and
         * whose answer decides. Later answers are its volume, whatever they
         * say: the knob turns down itself what it hears is louder. */
        const uint8_t v = p->set_volume_rsp.volume & 0x7F;
        portENTER_CRITICAL(&s_vmux);
        if (s_vol.up && s_vol.takes) {
            const bool fresh = s_vol.busy_epoch == s_vol.epoch && s_vol.knob <= 100;
            if (fresh && !s_vol.set && !s_vol.refused) {
                s_vol.first      = s_vol.sent;
                s_vol.first_said = v;
                if ((int)v <= (int)s_vol.sent + NEAR) {
                    s_vol.set   = true;
                    s_vol.told &= (uint16_t)~SAY_GAVE_UP;
                    s_vol.say  |= SAY_SET;
                } else if (!s_vol.pending) {
                    s_vol.refused = true;
                    s_vol.say    |= SAY_FAR;
                }
            }
            s_vol.busy   = false;
            s_vol.tries  = 0;
            s_vol.now    = v;
            s_vol.set_us = t;
            s_vol.say   |= SAY_STATE;
            go = claim_locked(&vol, &tl);   /* the knob turned on meanwhile */
        }
        portEXIT_CRITICAL(&s_vmux);
        break;
    }
    case ESP_AVRC_CT_PROF_STATE_EVT:
        if (p->avrc_ct_init_stat.state == ESP_AVRC_INIT_SUCCESS)
            ESP_LOGI(TAG, "AVRCP up: a speaker that takes it is set to the knob's VOLUME");
        else
            ESP_LOGW(TAG, "AVRCP: state %d", (int)p->avrc_ct_init_stat.state);
        break;
    default:
        ESP_LOGD(TAG, "avrc event %d", ev);
        break;
    }
    if (go) esp_avrc_ct_send_set_absolute_volume_cmd(tl, vol);
}

/* The stack's reader of a remote control's answers, called from btc_avrc.c,
 * defined in avrc_pars_ct.c. Asked for the next notice of its volume, a
 * speaker answers at once with its volume as it stands (INTERIM) -- which
 * the stack drops: at its connect the volume it has, and after each notice
 * any change since, a second press of its button or its knob turned on.
 * Heard here, as a notice is. Typed after the stack's declaration, as
 * air.c's wraps are. */
extern __typeof__(AVRC_ParsResponse) __real_AVRC_ParsResponse;
__typeof__(AVRC_ParsResponse) __wrap_AVRC_ParsResponse;
tAVRC_STS __wrap_AVRC_ParsResponse(tAVRC_MSG *p_msg, tAVRC_RESPONSE *p_result)
{
    const tAVRC_STS st = __real_AVRC_ParsResponse(p_msg, p_result);
    if (st == AVRC_STS_NO_ERROR && p_msg && p_result && p_msg->hdr.opcode == AVRC_OP_VENDOR &&
        p_msg->hdr.ctype == AVRC_RSP_INTERIM && p_result->pdu == AVRC_PDU_REGISTER_NOTIFICATION &&
        p_result->reg_notif.event_id == AVRC_EVT_VOLUME_CHANGE) {
        portENTER_CRITICAL(&s_vmux);
        heard_locked(p_result->reg_notif.param.volume & 0x7F, esp_timer_get_time());
        portEXIT_CRITICAL(&s_vmux);
    }
    return st;
}

void a2dp_volume(uint8_t pct, const uint8_t *spk)
{
    if (pct > 100) pct = 100;
    uint8_t vol = 0, tl = 0;
    portENTER_CRITICAL(&s_vmux);
    mine_locked(spk);
    if (pct != s_vol.knob) {
        s_vol.knob = pct;
        s_vol.want = btl_av_from_knob(pct);
        /* A VOLUME of its own: a speaker that refused the last tries again. */
        if (s_vol.refused) {
            s_vol.refused = false;
            s_vol.say    |= SAY_STATE;
        }
        /* Where it is already, as near as the knob can say -- a turn of its
         * own, which the knob took and says back: nothing to set. Unless a
         * set is under way, which will move it: this one after it. */
        s_vol.pending = s_vol.busy || !(s_vol.set && s_vol.now != 0xFF && btl_av_to_knob(s_vol.now) == pct);
    }
    const bool go = claim_locked(&vol, &tl);
    portEXIT_CRITICAL(&s_vmux);
    if (go) esp_avrc_ct_send_set_absolute_volume_cmd(tl, vol);
}

uint8_t a2dp_volume_state(const uint8_t *spk, uint8_t *vol, uint8_t *turns)
{
    portENTER_CRITICAL(&s_vmux);
    const uint8_t av = !mine_locked(spk) || !s_vol.takes
                           ? 0
                           : BTL_AV_TAKES | (s_vol.set ? BTL_AV_SET : 0) | (s_vol.refused ? BTL_AV_REFUSED : 0);
    *vol   = av ? s_vol.now : 0xFF;
    *turns = s_vol.turns;
    portEXIT_CRITICAL(&s_vmux);
    return av;
}

bool a2dp_volume_full(void) { return s_vol.set && s_vol.mine; }

bool a2dp_volume_settled(const uint8_t *spk)
{
    portENTER_CRITICAL(&s_vmux);
    const bool mine = mine_locked(spk);
    const bool done = !s_vol.knob_av ||
                      (mine && !s_vol.asked && (!s_vol.takes || s_vol.set || s_vol.refused));
    portEXIT_CRITICAL(&s_vmux);
    return done;
}

void a2dp_knob_started(void)
{
    portENTER_CRITICAL(&s_vmux);
    const bool was = s_vol.set;
    s_vol.epoch++;
    s_vol.set     = false;
    s_vol.pending = false;
    s_vol.knob    = 0xFF;               /* none from this start: nothing set, nor tried again, until it says */
    if (was) s_vol.say |= SAY_STATE;
    portEXIT_CRITICAL(&s_vmux);
}

void a2dp_knob_av(bool av)
{
    portENTER_CRITICAL(&s_vmux);
    s_vol.knob_av = av;
    portEXIT_CRITICAL(&s_vmux);
}

static const char *rc_bda_str(const uint8_t *b, char *s)
{
    snprintf(s, 18, "%02x:%02x:%02x:%02x:%02x:%02x", b[0], b[1], b[2], b[3], b[4], b[5]);
    return s;
}

void a2dp_tick(const char *speaker, const uint8_t *spk)
{
    const int64_t t = esp_timer_get_time();
    uint8_t vol = 0, tl = 0, rc[6];
    portENTER_CRITICAL(&s_vmux);
    const bool mine = mine_locked(spk);
    if (s_vol.busy && t - s_vol.busy_us > SET_ANSWER_US) {
        s_vol.busy = false;
        if (++s_vol.tries < SET_TRIES) {
            s_vol.pending = s_vol.knob <= 100;      /* once more: its latest */
        } else {
            /* Its sets go unanswered: the knob scales what it sends again,
             * as for a speaker that keeps its own. Its next VOLUME tries
             * again. */
            s_vol.tries   = 0;
            s_vol.pending = false;
            s_vol.set     = false;
            s_vol.refused = true;
            s_vol.say    |= SAY_GAVE_UP | SAY_STATE;
        }
    }
    if (s_vol.up && s_vol.asked && t - s_vol.up_us > EVENTS_US) {
        s_vol.asked = false;
        s_vol.say  |= SAY_SILENT;
    }
    /* Another device's remote control, the speaker's A2DP up: it has the
     * only one there is, and the speaker's own volume is not set. */
    if (s_vol.up && spk && !mine) s_vol.say |= SAY_OTHER;
    const bool go = claim_locked(&vol, &tl);
    /* Its lines, with its name: a speaker's only -- a headset's own A2DP,
     * idle, has a remote control too, which nothing here sets -- and kept
     * until its A2DP is up, as its remote control may come first. */
    const uint16_t ready = !speaker ? 0 : s_vol.say & (mine ? SAY_LINES : spk ? SAY_OTHER : 0);
    const uint16_t say   = (uint16_t)(ready & ~(s_vol.told & SAY_ONCE)) | (s_vol.say & SAY_STATE);
    s_vol.told |= ready & SAY_ONCE;
    s_vol.say  &= (uint16_t)~(ready | SAY_STATE);
    const uint8_t  now  = s_vol.now, was = s_vol.was, first = s_vol.first, said = s_vol.first_said;
    const uint32_t feat = s_vol.feat;
    const uint16_t tg   = s_vol.tg_feat;
    const bool     kav  = s_vol.knob_av;
    memcpy(rc, s_vol.bda, sizeof rc);
    portEXIT_CRITICAL(&s_vmux);
    if (go) esp_avrc_ct_send_set_absolute_volume_cmd(tl, vol);
    const char *n = speaker && speaker[0] ? speaker : "the speaker";
    if (say & SAY_FEATS)
        link_log("%s's remote control (AVRCP): metadata %s, an amplifier's controls %s", n,
                 (feat & ESP_AVRC_FEAT_META_DATA) ? "yes" : "no", (tg & ESP_AVRC_FEAT_FLAG_CAT2) ? "yes" : "no");
    if (say & SAY_TAKES) {
        if (kav) link_log("%s takes its volume from the knob (AVRCP absolute volume)", n);
        else link_log("%s could take its volume from the knob (AVRCP absolute volume), but this knob's firmware "
                      "scales its sound itself: its own controls set its volume", n);
    }
    if (say & (SAY_NOT | SAY_SILENT))
        link_log("%s %s: its own controls set its volume, the knob scales its sound", n,
                 (say & SAY_NOT) ? "lists no volume of its own to set" : "tells nothing of its volume");
    if (say & SAY_SET) {
        if (was <= 127)
            link_log("%s's volume is the knob's now: VOLUME %u %% set as %u of 127, it says %u (it was at %u)", n,
                     (unsigned)btl_av_to_knob(first), (unsigned)first, (unsigned)said, (unsigned)was);
        else
            link_log("%s's volume is the knob's now: VOLUME %u %% set as %u of 127, it says %u", n,
                     (unsigned)btl_av_to_knob(first), (unsigned)first, (unsigned)said);
    }
    if (say & SAY_FAR)
        link_log("%s says %u of 127 for the %u asked: louder, not taken at its word -- the knob scales its sound, "
                 "its next VOLUME tries again", n, (unsigned)said, (unsigned)first);
    if (say & SAY_OWN)
        link_log("%s's own volume: %u of 127 -- the knob's VOLUME %u %%", n, (unsigned)now,
                 (unsigned)btl_av_to_knob(now));
    if (say & SAY_GAVE_UP)
        link_log("%s answered none of %d volume sets: the knob scales its sound, its next VOLUME tries again", n,
                 SET_TRIES);
    if (say & SAY_OTHER) {
        char b[18];
        link_log("the remote control (AVRCP) up is %s's, not %s's: %s keeps its own volume, the knob scales its sound",
                 rc_bda_str(rc, b), n, n);
    }
    if (say & SAY_STATE) hfp_report_state();
}

/* ---- the stack's A2DP sink, left out ------------------------------------- */

/* Each of the sink's entry points (btc_a2dp_sink.c) that the rest of the
 * stack calls, put in front of with the linker's --wrap (CMakeLists.txt):
 * none of the real ones is called from anywhere then, and the linker drops
 * the sink -- its decoder's 9.7 kB of static RAM with it. Every caller asks
 * for the sink's work only with a peer that is a source (btc_av.c's
 * peer_sep), or with the sink started (esp_a2d_sink_init(), never called
 * here): a speaker is a sink, this chip its source. Typed after the stack's
 * own declarations, as air.c's wraps are. */
__typeof__(btc_a2dp_sink_startup) __wrap_btc_a2dp_sink_startup;
bool __wrap_btc_a2dp_sink_startup(void) { return false; }   /* no sink to start */

__typeof__(btc_a2dp_sink_shutdown) __wrap_btc_a2dp_sink_shutdown;
void __wrap_btc_a2dp_sink_shutdown(void) {}

__typeof__(btc_a2dp_sink_on_idle) __wrap_btc_a2dp_sink_on_idle;
void __wrap_btc_a2dp_sink_on_idle(void) {}

__typeof__(btc_a2dp_sink_on_stopped) __wrap_btc_a2dp_sink_on_stopped;
void __wrap_btc_a2dp_sink_on_stopped(tBTA_AV_SUSPEND *p_av) { (void)p_av; }

__typeof__(btc_a2dp_sink_on_suspended) __wrap_btc_a2dp_sink_on_suspended;
void __wrap_btc_a2dp_sink_on_suspended(tBTA_AV_SUSPEND *p_av) { (void)p_av; }

__typeof__(btc_a2dp_sink_set_rx_flush) __wrap_btc_a2dp_sink_set_rx_flush;
void __wrap_btc_a2dp_sink_set_rx_flush(BOOLEAN enable) { (void)enable; }

__typeof__(btc_a2dp_sink_reset_decoder) __wrap_btc_a2dp_sink_reset_decoder;
void __wrap_btc_a2dp_sink_reset_decoder(UINT8 *p_av) { (void)p_av; }

__typeof__(btc_a2dp_sink_reg_data_cb) __wrap_btc_a2dp_sink_reg_data_cb;
void __wrap_btc_a2dp_sink_reg_data_cb(esp_a2d_sink_data_cb_t callback) { (void)callback; }

/* A packet for a sink to play: dropped, as the sink drops what comes while
 * it is not started -- the buffer is the callee's. */
__typeof__(btc_a2dp_sink_enque_buf) __wrap_btc_a2dp_sink_enque_buf;
UINT8 __wrap_btc_a2dp_sink_enque_buf(BT_HDR *p_buf)
{
    osi_free(p_buf);
    return 0;
}

/* ---- for hfp.c ------------------------------------------------------------ */

void a2dp_init(void)
{
    /* Its remote control first: the stack takes AVRCP only before A2DP
     * (btc_avrc.c), and its callback before the answer to its start. */
    esp_avrc_ct_register_callback(rc_cb);
    esp_err_t e = esp_avrc_ct_init();
    if (e != ESP_OK) ESP_LOGE(TAG, "AVRCP not started: %s -- a speaker's own controls set its volume", esp_err_to_name(e));
    esp_a2d_register_callback(cb);
    esp_a2d_source_register_data_callback(data_cb);
    e = esp_a2d_source_init();
    if (e != ESP_OK) ESP_LOGE(TAG, "A2DP source not started: %s", esp_err_to_name(e));
}

void a2dp_connect(const uint8_t bda[6])
{
    esp_bd_addr_t a;
    memcpy(a, bda, sizeof a);
    esp_a2d_source_connect(a);
}

void a2dp_disconnect(const uint8_t bda[6])
{
    esp_bd_addr_t a;
    memcpy(a, bda, sizeof a);
    esp_a2d_source_disconnect(a);
}

void a2dp_start(void) { esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY); }

void a2dp_suspend(void) { esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_SUSPEND); }

void a2dp_counts_reset(void)
{
    /* A drop of the stream before, still under way: over, its flag down. */
    if (s_flush) {
        s_flush = 0;
        btc_a2dp_source_set_tx_flush(false);
    }
    s_made0    = s_made;
    s_sent0    = s_sent;
    s_dropped0 = s_dropped;
    s_most     = 0;
}

void a2dp_counts(a2dp_counts_t *out)
{
    out->made           = s_made - s_made0;
    out->sent           = s_sent - s_sent0;
    out->dropped        = s_dropped - s_dropped0;
    out->waiting        = waiting();
    out->waiting_max    = s_most;
    out->spf            = s_spf ? s_spf : 128;
    /* The least the BTC task ever had left: the SBC coder's work is on it
     * now, besides the stack's own. */
    out->btc_stack_free = s_btc ? (uint32_t)uxTaskGetStackHighWaterMark(s_btc) : 0;
}
