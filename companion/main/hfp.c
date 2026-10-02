/* The headset. See hfp.h.
 *
 * Classic Bluetooth's hands-free profile, with this chip as the audio gateway:
 * the phone's side. A headset only opens its audio for a call, so while the
 * knob wants it, there is one -- a call that is never dialled, as a phone's
 * own internet calls are. The headset's button hangs up a call; the knob
 * takes that as its PTT, and the call goes on.
 *
 * Audio comes and goes through the host (HCI): the stack codes the mSBC (or
 * CVSD) and hands us 16-bit PCM at the headset's rate. The headset's packets
 * set the pace: each one heard from it is one sent to it, so what we send can
 * neither run ahead of the air nor fall behind it. The knob's audio is
 * converted to that rate on its way in, a little fast or slow as its own
 * clock needs (rs.h), and the microphone to the knob's rate on its way out. */
#include "hfp.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "air.h"
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
#include "link.h"
#include "nvs.h"
#include "rs.h"
#include "upd.h"

static const char *TAG = "hfp";

#define DEVICE_NAME "VFO-Knob"
#define NVS_NS      "hfp"

/* How long the knob's audio waits in the downlink, as its target: enough to
 * ride out the knob sending in 10-20 ms lumps. */
#define DN_TARGET_MS 60
#define DN_MAX_MS    200

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
} S = { .spk = 10, .mic = 10, .dn_rate = 24000, .up_rate = 24000 };
static bool s_hold;                         /* an update coming in: the headset not called (s_mx) */

#define LOCK()   xSemaphoreTake(s_mx, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(s_mx)

/* What a scan found, for the names it learns later and the connect after. */
#define FOUND_MAX 16
static btl_found_t s_found[FOUND_MAX];
static int         s_nfound;
static int         s_name_next;             /* the next nameless headset to ask */

/* Audio. */
static rs_t           s_dn, s_up;           /* knob -> headset, headset -> knob */
static TaskHandle_t   s_pump;
static volatile bool  s_audio_on, s_mic_on, s_first;
static uint32_t       s_session;            /* audio opens since boot (s_mx) */
static struct {                             /* the last one's, for its reports (s_mx) */
    esp_bd_addr_t bda;
    int           conns, audios;
    int64_t       conn_us;
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

void hfp_report_state(void)
{
    btl_state_t s;
    memset(&s, 0, sizeof s);
    LOCK();
    s.link  = S.link;
    s.audio = S.audio;
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

static void button(uint8_t b, const char *what)
{
    link_log("headset button: %s", what);
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

static void call_down(void)
{
    if (S.audio != BTL_AUDIO_NONE) esp_hf_ag_audio_disconnect(S.bda);
    if (S.call) {
        S.call = false;
        esp_hf_ag_ciev_report(S.bda, ESP_HF_IND_TYPE_CALL, ESP_HF_CALL_STATUS_NO_CALLS);
    }
}

/* ---- audio ------------------------------------------------------------ */

/* One frame for the headset's ear: n samples at its rate. */
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
     * a percent fast or slow -- a pitch nobody hears. */
    s_avg += ((float)rs_fill(&s_dn) - s_avg) * (1.0f / 128.0f);
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

/* The stack's: the headset's microphone, decoded. Its pace is the air's. */
static void hf_incoming(const uint8_t *buf, uint32_t sz)
{
    if (!s_audio_on) return;
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
    if (!s_audio_on || sz > 512) return 0;
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
        if (!s_audio_on) continue;
        esp_hf_ag_outgoing_data_ready();
        size_t n;
        while ((n = rs_pull_some(&s_up, out, sizeof out / sizeof out[0])) > 0)
            if (s_mic_on) link_send(BTL_AUDIO_UP, out, (uint16_t)(n * 2));
    }
}

/* The converters, for the headset's rate and the knob's, emptied. */
static void audio_rates(bool msbc)
{
    const uint32_t air = msbc ? 16000 : 8000;
    rs_init(&s_dn, S.dn_rate, air);
    rs_init(&s_up, air, S.up_rate);
    s_dn_target = S.dn_rate * DN_TARGET_MS / 1000;
    s_dn_max    = S.dn_rate * DN_MAX_MS / 1000;
    s_primed    = false;
}

static void audio_open(bool msbc)
{
    s_audio_on = false;
    __atomic_store_n(&s_credit, 0, __ATOMIC_RELAXED);
    audio_rates(msbc);
    s_first  = true;
    s_under = s_skips = s_frames_in = s_frames_out = 0;
    s_session++;
    esp_hf_ag_register_data_callback(hf_incoming, hf_outgoing);
    s_audio_on = true;
}

/* ---- scanning --------------------------------------------------------- */

static bool is_audio(uint32_t cod) { return esp_bt_gap_get_cod_major_dev(cod) == ESP_BT_COD_MAJOR_DEV_AV; }

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
            break;
        }
        default:
            break;
        }
    }
    if (!f->name[0] && eir_name[0]) strlcpy(f->name, eir_name, sizeof f->name);
    link_send(BTL_EVT_FOUND, f, sizeof *f);
}

/* After the scan, the names it did not hear, one headset at a time. */
static bool ask_next_name(void)
{
    for (; s_name_next < s_nfound; s_name_next++) {
        btl_found_t *f = &s_found[s_name_next];
        if (!f->name[0] && is_audio(f->cod)) {
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
    int audio = 0;
    for (int i = 0; i < s_nfound; i++) audio += is_audio(s_found[i].cod);
    link_log("scan done: %d devices, %d of them audio", s_nfound, audio);
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
    case ESP_BT_GAP_MODE_CHG_EVT:
        /* A call keeps the link awake (air.c); one that sleeps all the same,
         * or is woken as its audio opens, says so. The interval says whose
         * sleep: 31-94 ms is the stack's own for a call, its policy never
         * cleared; 250-500 ms its own between calls, asked a moment before
         * this one opened; anything else most likely the headset's. */
        if (s_audio_on) {
            const esp_bt_pm_mode_t m = p->mode_chg.mode;
            if (m == ESP_BT_PM_MD_SNIFF)
                link_log("the link went to sniff during the call, every %u ms",
                         (unsigned)(p->mode_chg.interval * 5u / 8u));
            else
                link_log("the link went %s during the call", m == ESP_BT_PM_MD_ACTIVE ? "active" : "to another mode");
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
        if (!same(bda, S.bda)) {            /* our remembered headset, calling us */
            memcpy(S.bda, bda, 6);
            strlcpy(S.name, S.mem_name, sizeof S.name);
            S.have = true;
        }
        S.link          = BTL_LINK_CONNECTED;
        memcpy(S.conn, bda, 6);
        S.page_fails    = 0;
        S.user_off      = false;
        S.audio_tries   = 0;
        S.audio_pending = false;
        S.next_audio_us = now + 800000;     /* a moment for the headset to settle */
        S.conns++;
        S.conn_us       = now;
        S.conn_audios   = 0;
        if (!S.remembered || !same(S.mem, S.bda) || strcmp(S.mem_name, S.name)) {
            memcpy(S.mem, S.bda, 6);
            strlcpy(S.mem_name, S.name, sizeof S.mem_name);
            S.remembered = true;
            save_mem();
        }
        const bool need_name = !S.name[0];
        char name[32];
        strlcpy(name, S.name, sizeof name);
        UNLOCK();
        upd_headset_came();                 /* a firmware on trial: proven enough to keep */
        esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
        if (need_name) esp_bt_gap_read_remote_name((uint8_t *)bda);
        link_log("headset connected: %s (%s)", name[0] ? name : "?", bda_str(bda, b));
        hfp_report_state();
        return;
    }
    case ESP_HF_CONNECTION_STATE_DISCONNECTED: {
        const uint8_t was = S.link;
        S.link  = BTL_LINK_IDLE;
        S.audio = BTL_AUDIO_NONE;
        S.call  = false;
        s_audio_on = false;
        if (!same(S.conn, S.bda)) {
            /* Left for another one, which the knob asked for: call it now. */
            S.next_page_us = now + 300000;
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
    LOCK();
    switch (st) {
    case ESP_HF_AUDIO_STATE_CONNECTED:
    case ESP_HF_AUDIO_STATE_CONNECTED_MSBC: {
        const bool msbc = st == ESP_HF_AUDIO_STATE_CONNECTED_MSBC;
        S.audio         = msbc ? BTL_AUDIO_MSBC_16K : BTL_AUDIO_CVSD_8K;
        S.audio_pending = false;
        S.audio_tries   = 0;
        memcpy(s_open.bda, bda, 6);         /* for the reports, with audio_open()'s count */
        s_open.conns    = S.conns;
        s_open.conn_us  = S.conn_us;
        s_open.audios   = ++S.conn_audios;
        audio_open(msbc);
        UNLOCK();
        link_log("audio open: %s", msbc ? "mSBC, 16 kHz" : "CVSD, 8 kHz");
        hfp_report_state();
        return;
    }
    case ESP_HF_AUDIO_STATE_DISCONNECTED:
        s_audio_on      = false;
        S.audio         = BTL_AUDIO_NONE;
        S.audio_pending = false;
        S.next_audio_us = esp_timer_get_time() + 2000000;
        UNLOCK();
        link_log("audio closed");
        hfp_report_state();
        return;
    default:
        break;
    }
    UNLOCK();
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
        ESP_LOGI(TAG, "unknown AT: %s", p->unat_rep.unat ? p->unat_rep.unat : "");
        esp_hf_ag_unknown_at_send(p->unat_rep.remote_addr, NULL);
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

/* ---- the knob's commands ------------------------------------------------ */

void hfp_on_frame(uint8_t type, const uint8_t *p, uint16_t n)
{
    char b[18];
    switch (type) {
    case BTL_AUDIO_DN:
        if (s_audio_on) {
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
        const bool busy = S.link != BTL_LINK_IDLE;
        const bool other = busy && S.have && !same(S.bda, p);
        esp_bd_addr_t old;
        memcpy(old, S.bda, 6);
        memcpy(S.bda, p, 6);
        S.have = true;
        S.name[0] = 0;
        for (int i = 0; i < s_nfound; i++)
            if (same(s_found[i].bda, p)) strlcpy(S.name, s_found[i].name, sizeof S.name);
        if (!S.name[0] && S.remembered && same(S.mem, p)) strlcpy(S.name, S.mem_name, sizeof S.name);
        S.user_off     = false;
        S.page_fails   = 0;
        S.next_page_us = 0;                 /* the tick calls it now */
        UNLOCK();
        if (other) {
            link_log("leaving %s for %s", bda_str(old, b), S.name);
            esp_hf_ag_slc_disconnect(old);
        }
        hfp_report_state();
        break;
    }
    case BTL_CMD_DISCONNECT:
        LOCK();
        S.user_off = true;
        if (S.link != BTL_LINK_IDLE && S.have) {
            call_down();
            esp_hf_ag_slc_disconnect(S.bda);
        }
        UNLOCK();
        link_log("headset hung up by the knob");
        break;
    case BTL_CMD_FORGET: {
        if (n < 6) break;
        LOCK();
        const bool cur = S.have && same(S.bda, p);
        if (cur && S.link != BTL_LINK_IDLE) {
            call_down();
            esp_hf_ag_slc_disconnect(S.bda);
        }
        if (S.remembered && same(S.mem, p)) {
            S.remembered = false;
            save_mem();
        }
        if (cur) S.have = false;
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
        S.want_audio = p[0] != 0;
        S.audio_tries = 0;
        const bool changed = dn != S.dn_rate || up != S.up_rate;
        S.dn_rate = dn;
        S.up_rate = up;
        const uint8_t audio = S.audio;
        UNLOCK();
        /* Another firmware on the knob, another rate -- with the headset's
         * audio open all the while: the converters again. The stack's
         * callbacks stand aside for a moment while they are rebuilt. */
        if (changed && audio != BTL_AUDIO_NONE && s_audio_on) {
            s_audio_on = false;
            vTaskDelay(pdMS_TO_TICKS(20));
            audio_rates(audio == BTL_AUDIO_MSBC_16K);
            s_audio_on = true;
        }
        if (changed) link_log("the knob's audio: %lu Hz to the headset, %lu Hz back",
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
        if (S.link == BTL_LINK_CONNECTED) {
            esp_hf_ag_volume_control(S.bda, ESP_HF_VOLUME_CONTROL_TARGET_SPK, S.spk);
            esp_hf_ag_volume_control(S.bda, ESP_HF_VOLUME_CONTROL_TARGET_MIC, S.mic);
        }
        UNLOCK();
        break;
    case BTL_CMD_STATE:
        hfp_report_state();
        break;
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
 * -- never in the pump, which feeds the headset. */
enum { J_AUDIO = 1, J_FROM = 2, J_TO = 4, J_LINK = 8, J_HISTORY = 16 };   /* in this order */
static unsigned s_jobs;
static struct {                             /* the audio's numbers, as taken */
    uint32_t in, out, fill, target, under, skips, bad;
    long     ppm;
} s_snap;
static struct {                             /* the history of the audio open */
    int     conns, audios;
    int64_t conn_us;
} s_hist;

void hfp_knob_hello(void) { s_knob_hellos++; }

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
    static bool     open, asked;
    static int64_t  t_stat;
    LOCK();
    const bool     up  = S.audio != BTL_AUDIO_NONE;
    const uint32_t ses = s_session;
    const __typeof__(s_open) o = s_open;
    UNLOCK();
    const int64_t now = esp_timer_get_time();
    if (open && (!up || ses != seen)) {
        /* Closed: the part since the last report -- unless it opened again
         * already, and the counts are the new one's. */
        open = false;
        if (ses == seen) report();
    }
    if (up && ses != seen) {
        seen           = ses;
        open           = true;
        asked          = false;
        t_stat         = now;
        s_hist.conns   = o.conns;
        s_hist.audios  = o.audios;
        s_hist.conn_us = o.conn_us;
        air_opened(o.bda);
        s_jobs |= J_LINK | J_HISTORY;
    }
    if (open) {
        /* Every 30 s from the open; the signal asked a second before, to be
         * fresh in the report. */
        if (!asked && now - t_stat >= 29000000) {
            asked = true;
            air_ask_signal();
        }
        if (now - t_stat >= 30000000) {
            t_stat = now;
            asked  = false;
            report();
        }
    }
    const unsigned j = s_jobs & -s_jobs;    /* the first due */
    s_jobs &= ~j;
    switch (j) {
    case J_AUDIO:   log_audio();    break;
    case J_FROM:    air_log_from(); break;
    case J_TO:      air_log_to();   break;
    case J_LINK:    air_log_link(); break;
    case J_HISTORY: log_history();  break;
    default:                        break;
    }
}

/* ---- an update of this chip's firmware coming in (upd.c) ---------------- */

static bool idle_locked(void)
{
    return S.link == BTL_LINK_IDLE && !S.scanning && S.audio == BTL_AUDIO_NONE && !S.audio_pending && !S.call;
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

void hfp_tick(void)
{
    char b[18];
    reports();
    const int64_t now = esp_timer_get_time();
    LOCK();
    /* Call the headset, as a phone does its own when it comes in reach --
     * not while an update comes in: the transfer would only stop for it. */
    if (S.link == BTL_LINK_IDLE && S.have && !S.scanning && !S.user_off && now >= S.next_page_us && !s_hold) {
        S.link         = BTL_LINK_CONNECTING;
        memcpy(S.conn, S.bda, 6);
        S.next_page_us = now + 30000000;    /* until the answer says otherwise */
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
}

void hfp_init(void)
{
    s_mx = xSemaphoreCreateMutex();
    load_mem();

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
    /* Reachable for our own headset, which calls the phone it knows when it
     * is switched on; never discoverable: headsets do not look for phones. */
    esp_bt_gap_set_scan_mode(S.remembered ? ESP_BT_CONNECTABLE : ESP_BT_NON_CONNECTABLE,
                             ESP_BT_NON_DISCOVERABLE);
    S.next_page_us = esp_timer_get_time() + 1500000;

    xTaskCreatePinnedToCore(pump_task, "pump", 4096, NULL, 13, &s_pump, 1);
    char b[18];
    esp_power_level_t lo = ESP_PWR_LVL_N0, hi = ESP_PWR_LVL_P3;
    esp_bredr_tx_power_get(&lo, &hi);
    ESP_LOGI(TAG, "Bluetooth up as %s (%s), transmitting %+d to %+d dBm%s%s", DEVICE_NAME,
             bda_str(esp_bt_dev_get_address(), b), -12 + 3 * (int)lo, -12 + 3 * (int)hi,
             S.remembered ? ", headset " : "", S.remembered ? S.mem_name : "");
}
