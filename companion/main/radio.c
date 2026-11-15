/* A radio's serial link. See radio.h.
 *
 * The knob asks for a link (CMD_RADIO OPEN): this chip asks the radio's SDP
 * which RFCOMM channel the service's class UUID is on, opens it -- pairing
 * first if need be, Just Works, the radio's pairing window open -- and from
 * then on carries its bytes both ways: the radio's to the knob as RS_DATA,
 * the knob's to the radio, each way as bt_link_stream.h numbers them. One
 * link, one try: a link that fails or closes stays closed until the knob
 * asks again, as its WebSocket's reconnects do.
 *
 * While radio mode is on, the headset's side (hfp.c) calls no device and
 * lets go of the one it has: the radio is this chip's one device. */
#include "radio.h"

#include <stdio.h>
#include <string.h>

#include "bt_link_proto.h"
#include "bt_link_stream.h"
#include "bta/bta_api.h"
#include "esp_gap_bt_api.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_sdp_api.h"
#include "esp_spp_api.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hfp.h"
#include "link.h"
#include "stack/btm_api.h"

static const char *TAG = "radio";

#define RING         8192             /* each way, bytes: a power of two */
#define WRITE_MAX    990              /* one esp_spp_write: what an RFCOMM frame carries with 3-DH5 */
#define INFLIGHT_MAX (4 * WRITE_MAX)  /* written, not yet confirmed by the stack */
#define SUPER_TOUT   3200             /* the link's supervision timeout: 2 s, in 0.625 ms slots */
#define TICK_MS      5

static SemaphoreHandle_t s_mx;        /* R and the streams */
static TaskHandle_t      s_task;
static struct {
    bool     mode;                    /* radio mode */
    bool     want;                    /* an OPEN not started yet */
    uint8_t  bda[6], uuid[16], scn_given;
    bool     have_uuid;
    uint8_t  state, conn, scn, why;
    int16_t  err;
    uint16_t opens;
    uint32_t to_radio, from_radio;
    uint32_t handle;                  /* the link's, from its CL_INIT on; 0 none */
    bool     closing;                 /* the knob's close, or ours, under way */
    uint8_t  close_why;               /* ...why, for the CLOSE_EVT */
    bool     cong;                    /* the stack says wait */
    uint32_t inflight;
    bool     sdp_ready, sdp_busy;
    bool     rings;                   /* allocated, on the first radio mode */
} R;
static btl_rs_tx_t s_to_knob;         /* the radio's bytes, on their way to the knob */
static btl_rs_rx_t s_from_knob;       /* the knob's, on their way to the radio */

#define LOCK()   xSemaphoreTake(s_mx, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(s_mx)

static bool same(const uint8_t *a, const uint8_t *b) { return memcmp(a, b, 6) == 0; }

static const char *bda_str(const uint8_t *a, char *b)
{
    snprintf(b, 18, "%02x:%02x:%02x:%02x:%02x:%02x", a[0], a[1], a[2], a[3], a[4], a[5]);
    return b;
}

static void wake(void)
{
    if (s_task) xTaskNotifyGive(s_task);
}

static void report(void)
{
    btl_radio_state_t s = { 0 };
    LOCK();
    s.mode  = R.mode;
    /* An OPEN taken and not started yet -- the task starts it within a
     * tick, or once this link has closed -- is LOOKING to the knob: an IDLE
     * now would read as the OPEN's end. */
    s.state = R.want && R.state == BTL_RS_IDLE ? BTL_RS_LOOKING : R.state;
    s.conn  = R.conn;
    memcpy(s.bda, R.bda, 6);
    s.scn        = R.scn;
    s.why        = R.why;
    s.err        = R.err;
    s.opens      = R.opens;
    s.to_radio   = R.to_radio;
    s.from_radio = R.from_radio;
    UNLOCK();
    link_send(BTL_EVT_RADIO, &s, sizeof s);
}

static bool knob_send(void *ctx, uint8_t type, const void *p, uint16_t n)
{
    (void)ctx;
    return link_send(type, p, n);
}

/* The rings, once: only a knob with radio mode ever has them. (R's lock.) */
static bool rings_locked(void)
{
    if (R.rings) return true;
    uint8_t *a = heap_caps_malloc(RING, MALLOC_CAP_8BIT), *b = heap_caps_malloc(RING, MALLOC_CAP_8BIT);
    if (!a || !b) {
        heap_caps_free(a);
        heap_caps_free(b);
        return false;
    }
    s_to_knob.buf   = a;
    s_to_knob.cap   = RING;
    s_from_knob.buf = b;
    s_from_knob.cap = RING;
    R.rings         = true;
    return true;
}

/* Ends the link, whatever it is doing. (Not under R's lock.) */
static void close_link(uint8_t why)
{
    LOCK();
    R.want = false;
    if (R.state == BTL_RS_IDLE) {
        UNLOCK();
        return;
    }
    if (R.state == BTL_RS_LOOKING) {
        /* SDP cannot be called off: its answer, when it comes, finds the
         * state moved on and is dropped. */
        R.state = BTL_RS_IDLE;
        R.why   = why;
        R.err   = 0;
        UNLOCK();
        report();
        return;
    }
    R.closing   = true;
    R.close_why = why;
    const uint32_t h = R.handle;
    UNLOCK();
    /* Before its CL_INIT there is no handle yet: its OPEN finds it closing. */
    if (h) esp_spp_disconnect(h);
}

/* Failed before it was open. (Not under R's lock.) */
static void failed(uint8_t why, int err)
{
    LOCK();
    R.state  = BTL_RS_IDLE;
    R.why    = why;
    R.err    = (int16_t)err;
    R.handle = 0;
    R.closing = false;
    UNLOCK();
    report();
}

static void sdp_cb(esp_sdp_cb_event_t ev, esp_sdp_cb_param_t *p)
{
    char b[18];
    switch (ev) {
    case ESP_SDP_INIT_EVT:
        LOCK();
        R.sdp_ready = p->init.status == ESP_SDP_SUCCESS;
        UNLOCK();
        if (p->init.status != ESP_SDP_SUCCESS) ESP_LOGE(TAG, "SDP did not start: %d", p->init.status);
        break;
    case ESP_SDP_SEARCH_COMP_EVT: {
        int scn = -1;
        if (p->search.status == ESP_SDP_SUCCESS)
            for (int i = 0; i < p->search.record_count && scn <= 0; i++)
                scn = p->search.records[i].hdr.rfcomm_channel_number;
        LOCK();
        R.sdp_busy = false;
        const bool ours = R.state == BTL_RS_LOOKING && same(p->search.remote_addr, R.bda);
        uint8_t bda[6];
        memcpy(bda, R.bda, 6);
        const uint8_t given = R.scn_given;
        UNLOCK();
        wake();
        if (!ours) break;
        if (scn <= 0) {
            /* An answer with no such service: none to open, whatever channel
             * was given. No answer at all: the given one, should the radio
             * be there after all. */
            if (p->search.status == ESP_SDP_SUCCESS || !given) {
                link_log("radio %s: %s", bda_str(bda, b),
                         p->search.status == ESP_SDP_SUCCESS ? "no such service (its TCI over Bluetooth off?)"
                                                             : "its SDP did not answer");
                failed(p->search.status == ESP_SDP_SUCCESS ? BTL_RSWHY_NO_SERVICE : BTL_RSWHY_SDP,
                       p->search.status);
                break;
            }
            link_log("radio %s: its SDP did not answer (%d); channel %u, as given", bda_str(bda, b),
                     p->search.status, given);
            scn = given;
        }
        LOCK();
        if (R.state != BTL_RS_LOOKING) {      /* closed meanwhile */
            UNLOCK();
            break;
        }
        R.state = BTL_RS_CONNECTING;
        R.scn   = (uint8_t)scn;
        UNLOCK();
        report();
        const esp_err_t e = esp_spp_connect(ESP_SPP_SEC_AUTHENTICATE | ESP_SPP_SEC_ENCRYPT, ESP_SPP_ROLE_MASTER, (uint8_t)scn, bda);
        if (e != ESP_OK) {
            link_log("radio %s: the link would not start: %s", bda_str(bda, b), esp_err_to_name(e));
            failed(BTL_RSWHY_BUSY, e);
        }
        break;
    }
    default:
        break;
    }
}

static void spp_cb(esp_spp_cb_event_t ev, esp_spp_cb_param_t *p)
{
    char b[18];
    switch (ev) {
    case ESP_SPP_INIT_EVT:
        if (p->init.status != ESP_SPP_SUCCESS) ESP_LOGE(TAG, "SPP did not start: %d", p->init.status);
        break;
    case ESP_SPP_CL_INIT_EVT:
        if (p->cl_init.status != ESP_SPP_SUCCESS) {
            failed(BTL_RSWHY_BUSY, p->cl_init.status);
            break;
        }
        LOCK();
        R.handle = p->cl_init.handle;
        UNLOCK();
        break;
    case ESP_SPP_OPEN_EVT: {
        if (p->open.status != ESP_SPP_SUCCESS) {
            link_log("radio %s: the link did not open (%d): not paired, refused, or away",
                     bda_str(p->open.rem_bda, b), p->open.status);
            failed(BTL_RSWHY_REFUSED, p->open.status);
            break;
        }
        LOCK();
        const bool wanted = R.state == BTL_RS_CONNECTING && !R.closing;
        R.handle = p->open.handle;
        if (wanted) {
            R.state    = BTL_RS_OPEN;
            R.conn     = (uint8_t)(R.conn % 255 + 1);
            R.opens++;
            R.why      = BTL_RSWHY_NONE;
            R.err      = 0;
            R.cong     = false;
            R.inflight = 0;
            btl_rs_tx_reset(&s_to_knob);
            btl_rs_rx_reset(&s_from_knob);
        }
        const uint8_t conn = R.conn, scn = R.scn;
        UNLOCK();
        if (!wanted) {                        /* closed while it opened */
            esp_spp_disconnect(p->open.handle);
            break;
        }
        /* A radio gone out of reach is let go in 2 s, not the stack's 20:
         * the knob's next try comes that much sooner. Only while this chip
         * is the link's master, though -- the stack sends nothing as its
         * slave -- and an X6200's BlueZ takes that role (2026-11-13): its
         * own timeout holds then. */
        BTM_SetLinkSuperTout(p->open.rem_bda, SUPER_TOUT);
        link_log("radio %s: link %u open, channel %u", bda_str(p->open.rem_bda, b), conn, scn);
        report();
        wake();
        break;
    }
    case ESP_SPP_CLOSE_EVT: {
        LOCK();
        if (R.state == BTL_RS_IDLE || (R.handle && p->close.handle != R.handle)) {
            UNLOCK();
            break;
        }
        const bool was_open = R.state == BTL_RS_OPEN;
        R.state   = BTL_RS_IDLE;
        R.why     = R.closing ? R.close_why : was_open ? BTL_RSWHY_REMOTE : BTL_RSWHY_REFUSED;
        R.err     = (int16_t)p->close.port_status;
        R.closing = false;
        R.handle  = 0;
        const uint8_t conn = R.conn, why = R.why;
        UNLOCK();
        link_log("radio: link %u closed (%s, port status 0x%lx)", conn,
                 why == BTL_RSWHY_KNOB ? "by the knob" : why == BTL_RSWHY_OVERFLOW ? "overflow" : "by the radio or the air",
                 (unsigned long)p->close.port_status);
        report();
        break;
    }
    case ESP_SPP_DATA_IND_EVT: {
        bool over = false;
        LOCK();
        if (R.state == BTL_RS_OPEN && p->data_ind.handle == R.handle) {
            const uint32_t took = btl_rs_tx_put(&s_to_knob, p->data_ind.data, p->data_ind.len);
            R.from_radio += took;
            over = took < p->data_ind.len && !R.closing;
            if (over) {
                R.closing   = true;
                R.close_why = BTL_RSWHY_OVERFLOW;
            }
        }
        const uint32_t h = R.handle;
        UNLOCK();
        /* A byte dropped is a broken WebSocket: the link ends, and the knob
         * starts afresh. */
        if (over) esp_spp_disconnect(h);
        wake();
        break;
    }
    case ESP_SPP_CONG_EVT:
        LOCK();
        R.cong = p->cong.cong;
        UNLOCK();
        wake();
        break;
    case ESP_SPP_WRITE_EVT:
        LOCK();
        R.inflight = (uint32_t)p->write.len >= R.inflight ? 0 : R.inflight - (uint32_t)p->write.len;
        R.cong     = p->write.cong;
        UNLOCK();
        wake();
        break;
    default:
        break;
    }
}

static void radio_task(void *arg)
{
    (void)arg;
    static uint8_t out[WRITE_MAX];
    char b[18];
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(TICK_MS));
        const int64_t now = esp_timer_get_time();
        LOCK();
        /* An OPEN asked: SDP first, one search at a time. */
        if (R.mode && R.want && R.state == BTL_RS_IDLE && R.sdp_ready && !R.sdp_busy) {
            R.want     = false;
            R.state    = BTL_RS_LOOKING;
            R.why      = BTL_RSWHY_NONE;
            R.err      = 0;
            R.scn      = 0;
            R.closing  = false;
            R.sdp_busy = true;
            uint8_t bda[6];
            memcpy(bda, R.bda, 6);
            esp_bt_uuid_t u = { .len = ESP_UUID_LEN_128 };
            memcpy(u.uuid.uuid128, R.uuid, 16);
            UNLOCK();
            link_log("radio %s: looking for its service", bda_str(bda, b));
            report();
            const esp_err_t e = esp_sdp_search_record(bda, u);
            if (e != ESP_OK) {
                LOCK();
                R.sdp_busy = false;
                UNLOCK();
                failed(BTL_RSWHY_BUSY, e);
            }
            continue;
        }
        if (R.state != BTL_RS_OPEN || R.closing) {
            UNLOCK();
            continue;
        }
        /* The knob's bytes to the radio, as far as the stack takes them. */
        while (!R.cong && R.inflight + WRITE_MAX <= INFLIGHT_MAX) {
            const uint32_t n = btl_rs_rx_read(&s_from_knob, out, sizeof out);
            if (!n) break;
            R.inflight += n;
            R.to_radio += n;
            const uint32_t h = R.handle;
            UNLOCK();
            /* It copies them before it returns. */
            esp_spp_write(h, (int)n, out);
            LOCK();
            if (R.state != BTL_RS_OPEN) break;
        }
        if (R.state == BTL_RS_OPEN) {
            btl_rs_rx_pump(&s_from_knob, R.conn, now, knob_send, NULL);
            btl_rs_tx_pump(&s_to_knob, R.conn, now, knob_send, NULL);
        }
        UNLOCK();
    }
}

void radio_on_frame(uint8_t type, const uint8_t *p, uint16_t n)
{
    switch (type) {
    case BTL_CMD_RADIO: {
        btl_radio_cmd_t c = { 0 };
        memcpy(&c, p, n < sizeof c ? n : sizeof c);
        switch (c.op) {
        case BTL_RADIO_OFF: {
            LOCK();
            const bool was = R.mode;
            R.mode = false;
            UNLOCK();
            close_link(BTL_RSWHY_KNOB);
            if (was) {
                hfp_radio(false);
                link_log("radio mode off: headsets and speakers as before");
            }
            break;
        }
        case BTL_RADIO_ON:
        case BTL_RADIO_OPEN: {
            LOCK();
            const bool was = R.mode;
            const bool ok  = rings_locked();
            R.mode = ok;
            if (ok && n >= 1 + 6 + 16) {
                memcpy(R.uuid, c.uuid, 16);
                R.have_uuid = true;
            }
            bool other = false;
            if (ok && c.op == BTL_RADIO_OPEN) {
                const bool busy = R.state != BTL_RS_IDLE;
                other = busy && !same(R.bda, c.bda);
                /* The same radio, asked again while its link is up or on its
                 * way: nothing new -- unless that link is closing, the knob's
                 * CLOSE just before: a new one once it has. */
                if (!busy || other || R.closing) {
                    memcpy(R.bda, c.bda, 6);
                    R.scn_given = c.scn;
                    R.want      = true;
                }
            }
            UNLOCK();
            if (!ok) {
                link_log("radio mode: no memory for its buffers");
                break;
            }
            if (!was) {
                hfp_radio(true);
                link_log("radio mode on: no headset nor speaker is called");
            }
            if (other) {
                close_link(BTL_RSWHY_KNOB);
                LOCK();
                R.want = true;                /* the new one, once this has closed */
                UNLOCK();
            }
            wake();
            break;
        }
        case BTL_RADIO_CLOSE:
            close_link(BTL_RSWHY_KNOB);
            break;
        default:
            break;
        }
        report();
        break;
    }
    case BTL_RS_DATA: {
        uint8_t  conn;
        uint32_t off;
        if (!btl_rs_parse_data(p, n, &conn, &off)) break;
        LOCK();
        if (R.state == BTL_RS_OPEN && conn == R.conn) btl_rs_rx_data(&s_from_knob, off, p + 5, n - 5u);
        UNLOCK();
        wake();
        break;
    }
    case BTL_RS_ACK: {
        uint8_t  conn;
        uint32_t next;
        uint16_t room;
        if (!btl_rs_parse_ack(p, n, &conn, &next, &room)) break;
        LOCK();
        if (R.state == BTL_RS_OPEN && conn == R.conn)
            btl_rs_tx_ack(&s_to_knob, next, room, esp_timer_get_time());
        UNLOCK();
        wake();
        break;
    }
    default:
        break;
    }
}

void radio_knob_hello(void)
{
    /* The knob started afresh: a link it had is gone with what it knew of
     * it -- its stream's offsets, its WebSocket. Closed, and asked again. */
    close_link(BTL_RSWHY_KNOB);
    report();
}

bool radio_mode(void)
{
    if (!s_mx) return false;
    LOCK();
    const bool m = R.mode;
    UNLOCK();
    return m;
}

bool radio_owns(const uint8_t *bda)
{
    if (!s_mx) return false;                  /* before radio_init(): a headset's pairing, not ours */
    LOCK();
    const bool own = R.mode && same(R.bda, bda);
    UNLOCK();
    return own;
}

/* Every pairing with the radio asks for General Bonding. The stack, pairing
 * by itself for a link that needs it -- the first, or again after the radio
 * said it has no key for us -- asks for no bonding (btm_sec.c, hard-coded),
 * and the radio's BlueZ then keeps the key in memory only: an X6200 that
 * restarted had forgotten the knob (2026-11-13). Asked for bonding, BlueZ
 * keeps it -- and pairs only while its pairing window is open, where a
 * pairing without bonding it takes at any time. Called from bta_dm_act.c
 * (the stack's task) for each IO capability request, ours or the radio's;
 * the MITM bit stays as the stack set it: neither side has a screen, Just
 * Works. (CMakeLists.txt's --wrap.) */
void __real_bta_dm_co_io_req(BD_ADDR bd_addr, tBTA_IO_CAP *p_io_cap, tBTA_OOB_DATA *p_oob_data,
                             tBTA_AUTH_REQ *p_auth_req, BOOLEAN is_orig);
void __wrap_bta_dm_co_io_req(BD_ADDR bd_addr, tBTA_IO_CAP *p_io_cap, tBTA_OOB_DATA *p_oob_data,
                             tBTA_AUTH_REQ *p_auth_req, BOOLEAN is_orig)
{
    __real_bta_dm_co_io_req(bd_addr, p_io_cap, p_oob_data, p_auth_req, is_orig);
    if (radio_owns(bd_addr)) *p_auth_req = BTA_AUTH_GEN_BOND | (*p_auth_req & BTA_AUTH_SP_YES);
}

void radio_paired(const uint8_t *bda, int stat)
{
    /* The radio would not pair: it has no key for us -- else no pairing --
     * and its pairing window is shut. The stack has let go of ours already
     * (btm_sec.c, on the radio's "key missing"); its store is emptied too,
     * so that a restart of this chip does not try that key again. Away
     * (page timeout) or gone mid-way says nothing of the key: kept. */
    if (stat != ESP_BT_STATUS_AUTH_REJECTED || !radio_owns(bda)) return;
    char    b[18];
    uint8_t a[6];
    memcpy(a, bda, 6);
    esp_bt_gap_remove_bond_device(a);
    link_log("radio %s: it would not pair (its pairing window shut): our old key for it dropped",
             bda_str(bda, b));
}

uint8_t radio_eir_svc(const uint8_t *eir)
{
    if (!s_mx) return 0;
    LOCK();
    const bool have = R.have_uuid;
    uint8_t    want[16];
    /* An EIR lists a 128-bit UUID least significant byte first. */
    for (int i = 0; i < 16; i++) want[i] = R.uuid[15 - i];
    UNLOCK();
    if (!have) return 0;
    const esp_bt_eir_type_t types[] = { ESP_BT_EIR_TYPE_CMPL_128BITS_UUID, ESP_BT_EIR_TYPE_INCMPL_128BITS_UUID };
    for (int t = 0; t < 2; t++) {
        uint8_t  len = 0;
        uint8_t *u   = esp_bt_gap_resolve_eir_data((uint8_t *)eir, types[t], &len);
        for (int i = 0; u && i + 16 <= len; i += 16)
            if (!memcmp(u + i, want, 16)) return BTL_SVC_RADIO;
    }
    return 0;
}

void radio_init(void)
{
    s_mx = xSemaphoreCreateMutex();
    const esp_spp_cfg_t cfg = { .mode = ESP_SPP_MODE_CB, .enable_l2cap_ertm = false };
    ESP_ERROR_CHECK(esp_spp_register_callback(spp_cb));
    ESP_ERROR_CHECK(esp_spp_enhanced_init(&cfg));
    ESP_ERROR_CHECK(esp_sdp_register_callback(sdp_cb));
    ESP_ERROR_CHECK(esp_sdp_init());
    /* Beside the link's receiver and the audio's pump, a notch below them:
     * the stream is not in a hurry the way a headset's ear is. */
    xTaskCreatePinnedToCore(radio_task, "radio", 3072, NULL, 11, &s_task, 1);
}
