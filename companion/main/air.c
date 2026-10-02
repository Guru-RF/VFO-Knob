/* The air between this chip and the headset. See air.h.
 *
 * The knob measures its own side to the last packet -- the RTP, the UART,
 * this chip's buffers -- and a connection could still sound robotic in the
 * headset, every call on it, with all of that clean. What is left is the air
 * between here and the headset, the stack and the controller on this side of
 * it, or the headset itself; and the stack keeps those to itself. So these
 * are its numbers, taken where it has them: four of its functions, each
 * called from another object file than its own, have the linker's --wrap
 * (CMakeLists.txt) put a few counts in front of them. All four run on the
 * stack's BTU task, which owns what they read. Two do more than count: the
 * audio's open and close keep the link out of sniff for the call (below).
 *
 * From the headset. Each eSCO interval the controller hands the host one
 * packet with its verdict: good; damaged -- heard, with errors retransmission
 * did not mend; or not heard at all, in its slot or the retransmission window
 * after it, and that packet comes all the same, emptied (Bluedroid turns the
 * controller's erroneous data reporting on at start-up). The stack covers a
 * damaged or empty packet with a concealed frame, and that frame earns the
 * headset its answer like any other -- each frame heard from it is one sent
 * to it (hfp.c) -- so the air's losses on the way here cost the knob sound,
 * never the headset its frames. What does cost it one is a packet that made
 * no sound at all: one that never came, which the controller drops only when
 * it had no buffer to take it in (its way to the host was behind) or the link
 * was going down -- "N packets of M due" -- or one the decoder dropped, for a
 * header or length it could not read, a codec reset, or coming before the
 * first good frame -- "made sound" under the packets.
 *
 * An mSBC packet's H2 header numbers it, 0 to 3. Each good one's number is
 * held against the count of packets since the last: one out of turn is a
 * frame the headset itself skipped or sent twice, or a packet with no H2 --
 * the headset's trouble, not the air's. (A packet the headset sent and the
 * air lost does not: the count goes on past it.)
 *
 * To the headset. The controller reports each packet of ours it sent (HCI
 * Number Of Completed Packets: Bluedroid turns sync flow control on), one a
 * slot; a slot it had none for, the headset heard nothing from us in and had
 * to conceal. So "slots filled" under the due: frames made too late (the pump
 * or the stack held up), too few (the packets that made no sound, above), or
 * too few of the controller's SCO buffers ours. Bluedroid lets the controller
 * hold as many packets as it has SCO buffers and counts each back as it is
 * sent -- but an ACL link that drops under a live SCO (btm_sco_acl_removed())
 * forgets those in flight, never to count them back, and every call after it
 * has fewer: the audio link line gives how many are free at each open, when
 * none are in flight -- all, unless some went so. "Queued" is what was made
 * and not yet sent: the head start, two or three; nought is no margin left.
 * Whether the headset heard what was sent, only the headset knows.
 *
 * The link: the eSCO parameters the call got, who is master, and the SCO
 * buffers, read as the stack opens its audio path in bta_ag_sco_co_open() --
 * on the BTU task: BTM's tables have no lock, and its list of ACL links,
 * which BTM_GetRole() walks, has its entries freed by that task: walked from
 * any other, such as the one the hands-free events come on, it could step on
 * a link just gone. The close, in bta_ag_sco_co_close(): the last numbers
 * are counted to it.
 *
 * The signal: the controller's RSSI for the link, through the public API.
 *
 * All counted in packets. An mSBC frame is one, but two with its T1 settings
 * (30 bytes a packet). Should the audio link line ever show those, suspect
 * the stack before the air: IDF 5.5.5 hands the decoder its one-byte length
 * as a four-byte one (bta_ag_co.c:696, :302), whose other three bytes are
 * whatever the stack last left there -- nought, and no T1 frame decodes --
 * and these wrappers, a call deeper on the stack, change what is left there.
 * T2's 60 bytes decode whatever those three bytes are.
 *
 * Not here: the AFH channel map, how many of the 79 channels the link hops
 * on. Bluedroid has no call for HCI Read AFH Channel Map, and hands no
 * answer to it back to anyone: it would take a command built by hand and
 * pushed past the stack into its HCI layer. */
#include "air.h"

#include <stdio.h>
#include <string.h>

#include "esp_bt.h"
#include "esp_gap_bt_api.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "link.h"

/* Bluedroid's own headers, which the bt component keeps to itself: their
 * directories are this component's private ones (CMakeLists.txt). They read
 * the same sdkconfig and project-wide definitions the stack was built with,
 * so the structures here are the stack's own -- btm_cb's too. */
#include "bta/bta_ag_co.h"
#include "bta/bta_sys.h"
#include "btm_int.h"
#include "stack/btm_api.h"
#include "stack/l2c_api.h"

/* The wrapped functions, typed after the stack's declarations of them: should
 * an IDF update change those, this is a compile error, not a stack called
 * with the wrong arguments. */
extern __typeof__(bta_ag_sco_co_in_data) __real_bta_ag_sco_co_in_data;
extern __typeof__(bta_ag_sco_co_open) __real_bta_ag_sco_co_open;
extern __typeof__(bta_ag_sco_co_close) __real_bta_ag_sco_co_close;
extern __typeof__(btm_sco_process_num_completed_pkts) __real_btm_sco_process_num_completed_pkts;
__typeof__(bta_ag_sco_co_in_data) __wrap_bta_ag_sco_co_in_data;
__typeof__(bta_ag_sco_co_open) __wrap_bta_ag_sco_co_open;
__typeof__(bta_ag_sco_co_close) __wrap_bta_ag_sco_co_close;
__typeof__(btm_sco_process_num_completed_pkts) __wrap_btm_sco_process_num_completed_pkts;

/* The controller's verdicts on the headset's packets, in its own numbering
 * (tBTM_SCO_DATA_FLAG), and the counts after them. */
enum {
    N_GOOD    = BTM_SCO_DATA_CORRECT,
    N_DAMAGED = BTM_SCO_DATA_PAR_ERR,       /* heard, with errors */
    N_UNHEARD = BTM_SCO_DATA_NONE,          /* nothing heard: an empty packet */
    N_PART    = BTM_SCO_DATA_PAR_LOST,      /* partly heard: for packets made of several, not here */
    N_SIZE,                                 /* not the size the stack expects: mSBC takes those as bad */
    N_SEQ,                                  /* a good one's H2 number out of turn, or no H2 */
    N_SENT,                                 /* ours the controller sent: slots filled */
    N_COUNTS
};

#define MSBC_T2 60                          /* bytes an mSBC packet: a whole frame, H2 first */

/* Written on the BTU task only. The lock keeps a report from reading half a
 * link's opening; the counts themselves are bumped without it. */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static struct {
    uint32_t       n[N_COUNTS];             /* since the link opened */
    uint32_t       t0_ms, t1_ms;            /* when it opened, and closed */
    bool           closed;
    uint32_t       gen;                     /* links opened since boot */
    uint16_t       handle;                  /* its HCI handle, for the sent */
    uint8_t        size;                    /* the packets the stack expects and makes, bytes */
    uint8_t        air_mode;                /* the stack's: CVSD, or transparent for mSBC */
    uint8_t        seq;                     /* the H2 number the next packet should carry */
    bool           seq_known;
    bool           known;                   /* the stack had a link to read */
    tBTM_ESCO_DATA esco;
    uint8_t        role;                    /* ours: BTM_ROLE_MASTER, _SLAVE or _UNDEFINED */
    uint16_t       bufs_free, bufs;         /* the controller's SCO buffers: free for us, of all */
    bool           awake;                   /* sniff taken out of the link's policy for the call */
} s_air;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

/* ---- on the stack's BTU task -------------------------------------------- */

/* With each of the headset's packets, some 133 a second: a count or two, and
 * on to the stack. */
void __wrap_bta_ag_sco_co_in_data(BT_HDR *p_buf, tBTM_SCO_DATA_FLAG status)
{
    s_air.n[status & 3]++;
    /* The HCI header first: the handle (2 bytes), the length (1). Then, in
     * mSBC, H2: 0x01, and 0x08 with the number's two bits twice each above. */
    const uint8_t *h  = (const uint8_t *)(p_buf + 1) + p_buf->offset;
    const bool     h2 = s_air.size == MSBC_T2 && s_air.air_mode == BTM_SCO_AIR_MODE_TRANSPNT;
    const uint8_t  due = s_air.seq;
    if (h2) s_air.seq = (due + 1) & 3;
    if (p_buf->len < 3 || h[2] != s_air.size) {
        s_air.n[N_SIZE]++;
    } else if (h2 && status == BTM_SCO_DATA_CORRECT && p_buf->len >= 5) {
        const unsigned b = h[4] >> 4;
        if (h[3] != 0x01 || (h[4] & 0x0f) != 0x08 || ((b ^ (b >> 1)) & 5)) {
            s_air.n[N_SEQ]++;               /* no H2 */
        } else {
            const uint8_t sn = (uint8_t)((b & 1) | ((b >> 1) & 2));
            if (s_air.seq_known && sn != due) s_air.n[N_SEQ]++;
            s_air.seq       = (sn + 1) & 3;
            s_air.seq_known = true;
        }
    }
    __real_bta_ag_sco_co_in_data(p_buf, status);
}

/* With each HCI Number Of Completed Packets, ACL links' and ours: our
 * link's, counted -- the event's pairs read as the stack reads them, a
 * handle and a count, two bytes each, after the number of pairs. */
void __wrap_btm_sco_process_num_completed_pkts(UINT8 *p, UINT8 evt_len)
{
    if (evt_len >= 1) {
        unsigned pairs = p[0];
        if (pairs > (evt_len - 1u) / 4) pairs = (evt_len - 1u) / 4;
        for (const uint8_t *q = p + 1; pairs--; q += 4)
            if ((((unsigned)q[1] << 8 | q[0]) & 0x0fff) == s_air.handle)
                s_air.n[N_SENT] += (unsigned)q[3] << 8 | q[2];
    }
    __real_btm_sco_process_num_completed_pkts(p, evt_len);
}

/* As the stack opens its audio path on a new link: what the link is, and the
 * counts from nought. */
void __wrap_bta_ag_sco_co_open(UINT16 handle, tBTM_SCO_AIR_MODE_TYPE air_mode, UINT8 inout_pkt_size,
                               UINT16 event)
{
    tBTM_ESCO_DATA d;
    memset(&d, 0, sizeof d);
    UINT16 hci   = BTM_INVALID_HCI_HANDLE;
    UINT8  role  = BTM_ROLE_UNDEFINED;
    bool   known = false;
    /* There is one: the stack is built for one (BTM_MAX_SCO_LINKS), as the
     * controller is (CONFIG_BTDM_CTRL_BR_EDR_MAX_SYNC_CONN). */
    for (UINT16 i = 0; i < BTM_MAX_SCO_LINKS && hci == BTM_INVALID_HCI_HANDLE; i++)
        if ((hci = BTM_ReadScoHandle(i)) != BTM_INVALID_HCI_HANDLE)
            known = BTM_ReadEScoLinkParms(i, &d) == BTM_SUCCESS;
    if (known) BTM_GetRole(d.bd_addr, &role);
    /* Awake for the call. Bluedroid's power manager sends the link to sniff
     * 7 s into a call (bta_dm_cfg.c, "sco open": SNIFF3, every 31-94 ms),
     * and 7 s after each burst of AT commands, which leaves a call "idle"
     * (SNIFF, every 250-500 ms); and the headset may ask for it too. The
     * eSCO slots go on in sniff, but the ACL around them -- and with it the
     * radios' power control -- is heard only at the anchors. On 2026-10-02,
     * a headset 30 cm away: through a call in sniff its signal here sank
     * from 11 to 20 dB under the controller's target and its losses climbed
     * from 1 to 10 %, where a ringing left awake had climbed to 4 dB under.
     * So sniff comes out of the link's policy, the stack's own way
     * (bta_dm_policy_cback): its power manager then never asks
     * (bta_dm_pm_set_mode), the controller turns the headset's asking down,
     * and a link already asleep is woken. Not one on its way to sleep this
     * very moment -- asked a few ms before, still active to BTM, which then
     * sends no wake: that sleep would last the call. hfp.c logs a sniff
     * during a call, should one ever come. It goes back in at the close. */
    if (known) bta_sys_clear_policy(BTA_ID_AG, HCI_ENABLE_SNIFF_MODE, d.bd_addr);
    const uint16_t bufs_free = (uint16_t)btm_cb.sco_cb.xmit_window_size;
    const uint16_t bufs      = btm_cb.sco_cb.num_lm_sco_bufs;
    const uint32_t t0        = now_ms();
    portENTER_CRITICAL(&s_mux);
    memset(s_air.n, 0, sizeof s_air.n);
    s_air.t0_ms     = t0;
    s_air.closed    = false;
    s_air.gen++;
    s_air.handle    = hci == BTM_INVALID_HCI_HANDLE ? 0xffff : hci & 0x0fff;
    s_air.size      = inout_pkt_size;
    s_air.air_mode  = air_mode;
    s_air.seq_known = false;
    s_air.known     = known;
    s_air.esco      = d;
    s_air.role      = role;
    s_air.bufs_free = bufs_free;
    s_air.bufs      = bufs;
    s_air.awake     = known;
    portEXIT_CRITICAL(&s_mux);
    __real_bta_ag_sco_co_open(handle, air_mode, inout_pkt_size, event);
}

void air_prefer_master(void)
{
    /* Which side is master is settled by who calls whom: a connection this
     * chip makes, it runs; one the headset makes -- switched on, it calls the
     * phone it knows -- the headset runs, as Bluedroid accepts it as slave
     * (l2c_main.c). The master hops the channels its own receiver finds clean
     * (AFH), and this one sits beside the knob's WiFi. On 2026-10-02, 30 cm
     * apart, with the link kept awake for the call: run by the headset, its
     * signal here sank from 7 to 19 dB under the controller's target within
     * half a minute and 1.5 to 2.6 % of its frames were lost; run by this
     * chip, it rose from 17 to 9 dB under and 0.3 to 1.9 % were lost. So
     * this chip accepts asking to be master: a role switch in the accept
     * (l2c_link.c), which a headset may refuse -- then it runs the link, as
     * before. Set once at start-up, read on the stack's task when a
     * connection comes in. */
    L2CA_SetDesireRole(HCI_ROLE_MASTER);
}

/* As the stack closes it: when, for the last report's due. */
void __wrap_bta_ag_sco_co_close(void)
{
    const uint32_t t1 = now_ms();
    BD_ADDR peer;
    bool    awake;
    portENTER_CRITICAL(&s_mux);
    if (!s_air.closed) {
        s_air.closed = true;
        s_air.t1_ms  = t1;
    }
    awake       = s_air.awake;
    s_air.awake = false;
    memcpy(peer, s_air.esco.bd_addr, sizeof peer);
    portEXIT_CRITICAL(&s_mux);
    /* Sniff back in the link's policy, for the hours between calls. A link
     * already gone has no policy left to give it back to: the stack looks
     * for it, does not find it, and the next link starts from the default. */
    if (awake) bta_sys_set_policy(BTA_ID_AG, HCI_ENABLE_SNIFF_MODE, peer);
    __real_bta_ag_sco_co_close();
}

/* ---- the reports, on the main task ---------------------------------------- */

/* The signal: how far the link's RSSI is from the controller's golden receive
 * range (HCI Read RSSI, for BR/EDR) -- the last answer, and the first after
 * the audio opened. */
#define NO_SIGNAL INT16_MIN
static volatile int16_t s_rssi = NO_SIGNAL, s_rssi_open = NO_SIGNAL;
static volatile bool    s_rssi_opening;     /* the next answer is the open's */
static esp_bd_addr_t    s_bda;              /* whose */

/* The link the audio is on, as air_opened() found it, and how far the
 * reports have counted it. */
static struct {
    uint32_t gen;                           /* its s_air.gen; 0: never seen opening */
    uint32_t prev[N_COUNTS], prev_in, prev_out, prev_ms;
    bool     first;                         /* not reported yet */
    bool     last;                          /* reported to its close */
} s_rep;

/* A report's numbers, from air_take() to its lines. */
static struct {
    bool     valid, closed, first, h2;
    uint32_t d[N_COUNTS], in, out, ms, due; /* in the window: since the report before */
    int32_t  queued;                        /* made and not yet sent, now */
    int16_t  rssi, rssi_open;
} s_shot;

/* Packets a frame of the stack's: two in mSBC's T1, else one. */
static uint32_t per_frame(uint8_t air_mode, uint8_t size)
{
    return air_mode == BTM_SCO_AIR_MODE_TRANSPNT && size == MSBC_T2 / 2 ? 2 : 1;
}

/* Microseconds as milliseconds, with no more decimals than they need. */
static const char *ms_str(char *s, size_t n, unsigned us)
{
    int k = snprintf(s, n, "%u.%03u", us / 1000, us % 1000);
    if (k >= (int)n) k = (int)n - 1;
    while (k > 0 && s[k - 1] == '0') s[--k] = 0;
    if (k > 0 && s[k - 1] == '.') s[--k] = 0;
    return s;
}

void air_opened(const uint8_t *bda)
{
    static uint32_t last;                   /* the link the audio before was on */
    memcpy(s_bda, bda, sizeof s_bda);
    s_rssi = s_rssi_open = NO_SIGNAL;
    s_rssi_opening = true;
    esp_bt_gap_read_rssi_delta(s_bda);

    uint32_t gen, t0;
    portENTER_CRITICAL(&s_mux);
    gen = s_air.gen;
    t0  = s_air.t0_ms;
    portEXIT_CRITICAL(&s_mux);
    /* A link opened since the audio before: this one's. Else the stack's
     * audio path was never seen opening, and nothing here is of this one. */
    memset(&s_rep, 0, sizeof s_rep);
    s_rep.gen     = gen != last ? gen : 0;
    s_rep.prev_ms = t0;
    s_rep.first   = true;
    last          = gen;
}

void air_log_link(void)
{
    tBTM_ESCO_DATA d;
    bool known, awake;
    uint8_t role;
    uint16_t bufs_free, bufs;
    portENTER_CRITICAL(&s_mux);
    awake     = s_air.awake;
    known     = s_air.known;
    d         = s_air.esco;
    role      = s_air.role;
    bufs_free = s_air.bufs_free;
    bufs      = s_air.bufs;
    portEXIT_CRITICAL(&s_mux);
    if (!s_rep.gen || !known) {
        link_log("audio link: %s", !s_rep.gen ? "not seen opening, no numbers for it" : "not in the stack's table");
        return;
    }
    /* HFP's mSBC T2 is 2-EV3, 60 bytes every 12 slots (7.5 ms); its T1 is EV3,
     * 30 bytes every 6. A slot is 625 us. */
    static const char *const coding[] = { "u-law", "A-law", "CVSD", "transparent" };
    char ms[12], every[40], size[32], window[32];
    if (d.tx_interval)
        snprintf(every, sizeof every, "every %s ms (%u slots)", ms_str(ms, sizeof ms, d.tx_interval * 625u),
                 (unsigned)d.tx_interval);
    else strlcpy(every, "at an interval not given", sizeof every);
    if (d.rx_pkt_len == d.tx_pkt_len) snprintf(size, sizeof size, "%u bytes each way", (unsigned)d.rx_pkt_len);
    else snprintf(size, sizeof size, "%u bytes in, %u out", (unsigned)d.rx_pkt_len, (unsigned)d.tx_pkt_len);
    if (d.retrans_window) snprintf(window, sizeof window, "retransmit window %u slots", (unsigned)d.retrans_window);
    else strlcpy(window, "no retransmits", sizeof window);
    link_log("audio link: %s, %s %s, %s, %s air, %s, %u of %u SCO buffers free",
             d.link_type == BTM_LINK_TYPE_ESCO ? "eSCO" : d.link_type == BTM_LINK_TYPE_SCO ? "SCO" : "?",
             size, every, window,
             d.air_mode < 4 ? coding[d.air_mode] : "?",
             role == BTM_ROLE_MASTER  ? "companion master"
             : role == BTM_ROLE_SLAVE ? "headset master"
                                      : "master unknown",
             (unsigned)bufs_free, (unsigned)bufs);
    /* Its own line: the one above is at the length a line may have. */
    esp_power_level_t lo, hi;
    if (esp_bredr_tx_power_get(&lo, &hi) == ESP_OK)
        link_log("audio link: %s, transmitting %+d to %+d dBm",
                 awake ? "kept awake for the call" : "sniff allowed", -12 + 3 * (int)lo, -12 + 3 * (int)hi);
    else
        link_log("audio link: %s, transmit power not read", awake ? "kept awake for the call" : "sniff allowed");
}

void air_signal(bool ok, int8_t delta)
{
    const int16_t r = ok ? delta : NO_SIGNAL;
    s_rssi = r;
    if (s_rssi_opening) {
        s_rssi_opening = false;
        s_rssi_open    = r;
    }
}

void air_ask_signal(void)
{
    esp_bd_addr_t a;
    memcpy(a, s_bda, sizeof a);
    esp_bt_gap_read_rssi_delta(a);
}

bool air_take(uint32_t frames_in, uint32_t frames_out)
{
    uint32_t n[N_COUNTS], t1, gen;
    uint8_t interval, size, mode;
    bool closed;
    portENTER_CRITICAL(&s_mux);
    memcpy(n, s_air.n, sizeof n);
    gen      = s_air.gen;
    closed   = s_air.closed;
    t1       = s_air.t1_ms;
    interval = s_air.known ? s_air.esco.tx_interval : 0;
    size     = s_air.size;
    mode     = s_air.air_mode;
    portEXIT_CRITICAL(&s_mux);
    /* Of the audio's link, or of none: another opened since, and its counts
     * are not this one's. */
    s_shot.valid = s_rep.gen && gen == s_rep.gen;
    if (!s_shot.valid) return true;
    /* A report due as the link closed, before hfp.c heard it had: the last
     * already, and the one at the close would be of nothing. */
    if (s_rep.last) return false;
    s_rep.last = closed;
    const uint32_t now = closed ? t1 : now_ms();
    const uint32_t ppf = per_frame(mode, size);
    s_shot.closed = closed;
    s_shot.first  = s_rep.first;
    s_shot.h2     = size == MSBC_T2 && mode == BTM_SCO_AIR_MODE_TRANSPNT;
    s_rep.first   = false;
    for (int i = 0; i < N_COUNTS; i++) {
        s_shot.d[i]  = n[i] - s_rep.prev[i];
        s_rep.prev[i] = n[i];
    }
    s_shot.in      = (frames_in - s_rep.prev_in) * ppf;
    s_shot.out     = (frames_out - s_rep.prev_out) * ppf;
    s_rep.prev_in  = frames_in;
    s_rep.prev_out = frames_out;
    s_shot.ms      = now - s_rep.prev_ms;
    s_rep.prev_ms  = now;
    /* What should have come, and gone, by the clock: a packet each way each
     * interval. */
    const uint32_t step_us = interval * 625u;
    s_shot.due    = step_us ? (uint32_t)(((uint64_t)s_shot.ms * 1000 + step_us / 2) / step_us) : 0;
    s_shot.queued = (int32_t)(frames_out * ppf - n[N_SENT]);
    s_shot.rssi      = s_rssi;
    s_shot.rssi_open = s_rssi_open;
    return true;
}

/* "30 s", or, the last report of a link, "last 12 s". */
static const char *window_str(char *s, size_t n)
{
    snprintf(s, n, "%s%lu s", s_shot.closed ? "last " : "", (unsigned long)((s_shot.ms + 500) / 1000));
    return s;
}

/* "3990 of 4000 due", or without the interval "3990" and the noun. */
static const char *count_str(char *s, size_t n, uint32_t count, const char *noun)
{
    if (s_shot.due) snprintf(s, n, "%lu of %lu due", (unsigned long)count, (unsigned long)s_shot.due);
    else snprintf(s, n, "%lu%s", (unsigned long)count, noun);
    return s;
}

void air_log_from(void)
{
    if (!s_shot.valid) {
        link_log("air: not measured -- the stack's audio path was never seen opening");
        return;
    }
    char win[16], got[32], odd[48] = "", seq[32] = "";
    /* Never seen on this controller, whose packets are one air packet each,
     * the size the link's: worth the room only if they come. */
    if (s_shot.d[N_PART]) snprintf(odd, sizeof odd, ", %lu part heard", (unsigned long)s_shot.d[N_PART]);
    if (s_shot.d[N_SIZE])
        snprintf(odd + strlen(odd), sizeof odd - strlen(odd), ", %lu wrong size", (unsigned long)s_shot.d[N_SIZE]);
    if (s_shot.h2) snprintf(seq, sizeof seq, "; %lu out of sequence", (unsigned long)s_shot.d[N_SEQ]);
    link_log("from the headset, %s: %s, %lu good, %lu damaged, %lu not heard%s; %lu made sound%s",
             window_str(win, sizeof win),
             count_str(got, sizeof got, s_shot.d[N_GOOD] + s_shot.d[N_DAMAGED] + s_shot.d[N_UNHEARD] + s_shot.d[N_PART],
                       " packets"),
             (unsigned long)s_shot.d[N_GOOD], (unsigned long)s_shot.d[N_DAMAGED],
             (unsigned long)s_shot.d[N_UNHEARD], odd, (unsigned long)s_shot.in, seq);
}

/* "3 dB under the golden range", or briefly "3 dB under" -- how the
 * controller counts its dB is its own affair: the spec asks only that it tell
 * inside the range from over and under it, and this one's thresholds are not
 * published. Power control keeps the headset in the range where it can: a
 * number under it that lasts is a headset at its strongest and still faint. */
static const char *sig_str(char *s, size_t n, int r, bool brief)
{
    if (r == NO_SIGNAL) strlcpy(s, "not read", n);
    else if (r == 0) strlcpy(s, brief ? "in it" : "in the golden range", n);
    else snprintf(s, n, "%d dB %s%s", r < 0 ? -r : r, r < 0 ? "under" : "over", brief ? "" : " the golden range");
    return s;
}

void air_log_to(void)
{
    if (!s_shot.valid) return;
    char win[16], filled[32], sig[40], open[40] = "";
    /* The first report of a link has the signal at its open too, when the
     * answer since differs. */
    if (s_shot.first && s_shot.rssi_open != NO_SIGNAL && s_shot.rssi_open != s_shot.rssi) {
        char o[24];
        snprintf(open, sizeof open, ", at the open %s", sig_str(o, sizeof o, s_shot.rssi_open, true));
    }
    link_log("to the headset, %s: %lu packets made, slots filled %s, %ld queued; signal %s%s",
             window_str(win, sizeof win), (unsigned long)s_shot.out, count_str(filled, sizeof filled, s_shot.d[N_SENT], ""),
             (long)(s_shot.queued > 0 ? s_shot.queued : 0), sig_str(sig, sizeof sig, s_shot.rssi, false), open);
}
