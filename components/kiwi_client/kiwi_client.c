/* The kiwi firmware's receiver: radio.h for a KiwiSDR or a Web-888, over
 * KiwiSDR's protocol (components/kiwi_proto). See kiwi.h.
 *
 * One task keeps one receiver going: the configuration page's list
 * (components/sdr_rx) says which there are, the operator which is in use.
 * Another is taken over at once, the session ended and the next one begun,
 * with no restart. What each one says when it will not have the knob is
 * kept to, as its owner set it (the plan's section 5):
 *
 *   day limit           marked, through restarts (kiwi_mark.h): never again
 *                       on the knob's own, and only twice more when chosen
 *   login unanswered    the same, where it has time limits: it may have
 *                       been a refusal whose answer was lost
 *   time up, kicked     LISTEN AGAIN is asked; nothing until it is answered
 *   refused             not again until chosen again
 *   password?, not a    not again until chosen again or the list is saved
 *   kiwi, no apps,
 *   moved, certificate
 *   apps full (its      120 s; three silent doors in a row: no apps
 *   door silent)
 *   full                30 s, then every 60 s
 *   no answer ...       2, 5, 10, 30, 60 s, back to 2 once it has streamed
 *
 * and the knob's own attempts on one receiver -- those that reached it --
 * are capped at six in ten minutes. A flash that cannot keep a mark keeps
 * the knob from logging in on its own to one with time limits; a run of
 * crashes keeps it from any receiver a while (kiwi_boot_hold_us), and what
 * held one back before a crash holds it still (kiwi_boot_hold_get). Its
 * /status is read once a boot before its first session, and rarely after:
 * owners drop an address that polls it. Its idle timer hears of a listener
 * only when someone has touched or turned the knob (radio_user_activity), a
 * minute apart at most.
 *
 * The task's stack is in PSRAM, so it never touches flash: the dial, the
 * mode, the filter, the AGC, the noise filter, the squelch and the receiver
 * in use are saved by a timer,
 * at a quiet moment -- a session over, the next receiver connecting, the
 * squelch closed a second, what it let through played -- or 30 s after the
 * last change.
 *
 * A receiver behind a front -- the kiwisdr.com proxy, Cloudflare -- is spoken
 * to over TLS, its address an https:// link; an http:// one that answers
 * with a redirect to https:// on its own host is followed at once, and keeps
 * https from then on (sdr_moved: the list in RAM at once, in flash at a quiet
 * moment), under the key it had: its marks and holds stay its.
 *
 * The audio goes into the ring at a quarter of a second, a frame and a half:
 * the session follows the receiver's clock with the drift trim, so the ring
 * stays there for hours, and a backlog -- the network holding the stream
 * up, then handing it over at once -- is left out in one jump, where the
 * stall had silenced it already, back at the live point. Where the network
 * keeps breaking the stream up, the ring is kept fuller, up to 0.7 s, until
 * it has been calm a while (kiwi_sess.h's target_max). Every 30 s the log
 * says how it came (cb_report).
 *
 * This is the left ear. A second receiver from the same list may play in
 * the right one (components/sdr_rx, following this dial, and this ear's AGC,
 * noise filter and squelch): the two never have one receiver at once -- each
 * ear refuses the other's, and the one that finds the other on its receiver
 * waits for it to let go. This ear's dial is kept within its receiver's
 * range (cb_range), so it never plays the edge of it as if it were the dial
 * -- save in CW less than its tone over a converter's bottom edge, where the
 * carrier stays at the edge and the station is heard below its tone. The
 * right ear's receiver may not reach that dial, and goes quiet then. */
#include "kiwi.h"
#include "radio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "audio_out.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "kiwi_mark.h"
#include "kiwi_proto.h"
#include "kiwi_sess.h"
#include "kvstore.h"
#include "sdr_rx.h"
#include "vfo_tune.h"

static const char *TAG = "kiwi";

#define NVS_NS          "vfo"
#define LOOP_MS         20                     /* the knob's detents: tune_apply's pace */
#define F_LOW           10000LL                /* nothing below 10 kHz, whatever the offset */
#define F_TOP           30000000LL             /* before a receiver has said: a KiwiSDR's */
#define DEFAULT_HZ      7100000LL              /* 40 m, LSB */
#define STATUS_AGAIN_US (5 * 60 * 1000000LL)   /* a /status again, after an end it may explain */
#define LOOK_GAP_US     (60 * 1000000LL)       /* a marked one's, however often it is chosen */
#define CAP_N           6                      /* the knob's own attempts on one receiver... */
#define CAP_US          (10 * 60 * 1000000LL)  /* ...in this long */
#define STREAM_OK_US    (30 * 1000000LL)       /* streamed this long: the backoff starts over */
#define SILENT_MAX      3                      /* silent doors in a row: it lets no apps in */
#define SAVE_QUIET_US   (2 * 1000000LL)        /* a change with nothing playing */
#define SAVE_LATER_US   (30 * 1000000LL)       /* ...or while it plays: the quiet moment's fallback */
#define OVL_US          (1000000LL)
#define SQ_QUIET_US     (1000000LL)            /* the squelch closed this long: the ring holds none of
                                                  what it let through */
#define FOREVER         0

static portMUX_TYPE S_LOCK = portMUX_INITIALIZER_UNLOCKED;
static struct {
    radio_link_t link;
    char     why[16];
    tune_t   tune;
    int64_t  f_min, f_max, f_sent;
    char     mode[6];
    int32_t  lo, hi;                /* the passband, relative to the dial */
    uint8_t  agc;                   /* KIWI_AGC_* */
    uint8_t  nr;                    /* the noise filter, KIWI_NR_* */
    uint8_t  sq;                    /* the squelch, 0-100 %: 0 open */
    uint32_t t_input;
    uint32_t want_hp;               /* the receiver in use, by its address */
    uint32_t chosen, chosen_hp;     /* the operator's acts, and the last one's receiver */
    bool     chosen_marked;         /* ...held for its day limit as it was chosen: a try */
    uint32_t run_hp;                /* the session's */
    float    dbm;
    bool     have_dbm, squelched, suspend;
    bool     sq_told;               /* ...closed long enough, and the quiet moment taken */
    int64_t  sq_since;              /* ...since when */
    int64_t  ovl_until;
    uint8_t  choice;                /* 0, or KIWI_END_IDLE / _KICKED: LISTEN AGAIN asked */
    bool     relisten;              /* ...and answered */
    bool     activity;              /* a touch or a turn, not yet told the receiver */
    uint32_t choices_seq;
    uint32_t connects, closes;
    bool     dirty;                 /* settings not saved yet */
} S = { .link = RADIO_LINK_DOWN, .agc = KIWI_AGC_MED, .f_min = F_LOW, .f_max = F_TOP };

/* The noise filter's steps, as the face and the pages name them: the
 * receivers' own three, their page's names. */
static const char *const NR_NAME[] = { "OFF", "WDSP", "LMS", "SPEC" };

/* Each receiver's kiwi_hp, as of a list generation. */
static uint32_t s_hps[SDR_MAX];
static int      s_hps_n = -1;
static uint32_t s_hps_gen;

/* What a receiver's /status said this boot, by its address. */
typedef struct {
    uint32_t hp;
    bool     tried;             /* read, or tried, this boot */
    bool     ok;                /* ...and it answered as a Kiwi, at t_ok */
    bool     again;             /* a session since ended so that another read may explain it */
    uint8_t  end;               /* how its last session ended (kiwi_end_t) */
    int64_t  t_read, t_ok;
    kiwi_status_t st;
} look_t;

/* What holds a receiver back until it is chosen again. */
typedef struct {
    uint32_t   hp;
    kiwi_end_t end;             /* REFUSED, PASSWORD, NOT_KIWI, NO_APPS, IDLE, KICKED, MOVED, CERT */
    uint32_t   gen;             /* the list's generation when it was set */
    char       state[48];       /* ...said as the pages say it: "moved to <host>" */
} hold_t;

/* The knob's own attempts on a receiver, the newest CAP_N. */
typedef struct {
    uint32_t hp;
    int64_t  t[CAP_N];
    uint8_t  head;              /* the oldest */
} cap_t;

EXT_RAM_BSS_ATTR static look_t        s_look[SDR_MAX];
EXT_RAM_BSS_ATTR static hold_t        s_hold[SDR_MAX];
EXT_RAM_BSS_ATTR static cap_t         s_cap[SDR_MAX];
EXT_RAM_BSS_ATTR static kiwi_said_t   s_said;      /* the session's, as it goes */
EXT_RAM_BSS_ATTR static kiwi_said_t   s_said_ui;   /* ...its last word, for the face and the pages */
EXT_RAM_BSS_ATTR static kiwi_status_t s_read;      /* a /status as it is read */
EXT_RAM_BSS_ATTR static kiwi_counts_t s_counts;
EXT_RAM_BSS_ATTR static uint32_t      s_lost;      /* feeds a full ring let go */
EXT_RAM_BSS_ATTR static char          s_state[48];  /* in a word or two, for the pages */
EXT_RAM_BSS_ATTR static char          s_last_close[48];
/* The session's receiver, as it started: a list saved under it ends it only
 * if it went, or its passwords changed. */
EXT_RAM_BSS_ATTR static struct { uint32_t gen; char pass[32], ipl[32]; } s_run;

static esp_timer_handle_t s_save_t;
static int64_t            s_t_stream;     /* when the session began to stream; 0 not */
static int64_t            s_t_change;     /* the last setting changed, not yet in flash */

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

/* ------------------------------------------------------------ the list */

/* Each receiver's address key, fresh for the list as it is now. */
static void hps_fresh(void)
{
    const uint32_t g = sdr_list_gen();
    taskENTER_CRITICAL(&S_LOCK);
    const bool ok = s_hps_n >= 0 && s_hps_gen == g;
    taskEXIT_CRITICAL(&S_LOCK);
    if (ok) return;
    uint32_t h[SDR_MAX] = { 0 };
    int n = sdr_count();
    if (n > SDR_MAX) n = SDR_MAX;
    for (int i = 0; i < n; i++) {
        sdr_cfg_t c;
        if (sdr_get(i, &c)) h[i] = sdr_hp(&c);
    }
    taskENTER_CRITICAL(&S_LOCK);
    memcpy(s_hps, h, sizeof s_hps);
    s_hps_n = n;
    s_hps_gen = g;
    taskEXIT_CRITICAL(&S_LOCK);
}

/* Its place in the list, -1 gone. Under S_LOCK. */
static int place_of(uint32_t hp)
{
    for (int i = 0; hp && i < s_hps_n; i++)
        if (s_hps[i] == hp) return i;
    return -1;
}

int kiwi_rx_active(void)
{
    hps_fresh();
    taskENTER_CRITICAL(&S_LOCK);
    const int i = place_of(S.want_hp);
    taskEXIT_CRITICAL(&S_LOCK);
    return i;
}

/* Whether this receiver is in the list as it is now: what the knob keeps of
 * one that is not is the first to go when its tables are full. */
static bool listed(uint32_t hp)
{
    hps_fresh();
    taskENTER_CRITICAL(&S_LOCK);
    const bool in = place_of(hp) >= 0;
    taskEXIT_CRITICAL(&S_LOCK);
    return in;
}

/* The first receiver the right ear does not have, else the first -- which
 * the right ear then lets go of. `hps`, `n`: the list's addresses; not
 * under S_LOCK, as sdr_rx takes a lock of its own. */
static int first_free(const uint32_t *hps, int n)
{
    for (int k = 0; k < n; k++)
        if (!sdr_rx_uses(hps[k])) return k;
    return 0;
}

bool kiwi_rx_in_session(uint32_t hp)
{
    taskENTER_CRITICAL(&S_LOCK);
    const bool in = hp && hp == S.want_hp && S.link != RADIO_LINK_DOWN;
    taskEXIT_CRITICAL(&S_LOCK);
    return in;
}

static look_t *look_find(uint32_t hp)
{
    for (int i = 0; hp && i < SDR_MAX; i++)
        if (s_look[i].hp == hp) return &s_look[i];
    return NULL;
}

bool kiwi_rx_label(int i, char *out, size_t cap)
{
    sdr_cfg_t c;
    if (!out || !cap || !sdr_get(i, &c)) return false;
    char ant[48] = "";
    const uint32_t hp = sdr_hp(&c);
    taskENTER_CRITICAL(&S_LOCK);
    const look_t *L = look_find(hp);
    if (L && L->ok) strlcpy(ant, L->st.antenna, sizeof ant);
    taskEXIT_CRITICAL(&S_LOCK);
    /* Its antenna not read here: as the right ear names it, which may have
     * read it -- the same name in both ears' choosers. */
    if (!ant[0]) return sdr_rx_label(i, out, cap);
    kiwi_label(out, cap, c.name, ant, c.host, kiwi_label_port(c.port, c.tls), sdr_host_shared(i));
    return true;
}

static hold_t *hold_find(uint32_t hp)
{
    for (int i = 0; hp && i < SDR_MAX; i++)
        if (s_hold[i].hp == hp) return &s_hold[i];
    return NULL;
}

bool kiwi_rx_hold(int i, char *word, size_t cap)
{
    sdr_cfg_t c;
    if (!sdr_get(i, &c)) return false;
    const uint32_t hp = sdr_hp(&c);
    taskENTER_CRITICAL(&S_LOCK);
    const hold_t *h = hold_find(hp);
    const kiwi_end_t e = h ? h->end : KIWI_END_NONE;
    taskEXIT_CRITICAL(&S_LOCK);
    if (e == KIWI_END_NONE) return false;
    if (word && cap) strlcpy(word, kiwi_end_note(e), cap);
    return true;
}

/* ------------------------------------------------------------ remembered */

/* The right ear playing (components/sdr_rx) -- its squelch, which is this
 * ear's, or a dial its receiver cannot reach, not keeping it silent a second
 * by now: asked before S_LOCK is taken, as that takes a lock of its own. */
static bool right_playing(void)
{
    return sdr_rx_audible();
}

/* This ear's AGC, noise filter and squelch, which the right ear plays with
 * too: one antenna heard against another fairly. Asked by its session at
 * every pass, as this one's asks cb_ctl (sdr_rx_settings_cb). */
static void right_settings(uint8_t *agc, uint8_t *nr, uint8_t *sq)
{
    taskENTER_CRITICAL(&S_LOCK);
    *agc = S.agc;
    *nr = S.nr;
    *sq = S.sq;
    taskEXIT_CRITICAL(&S_LOCK);
}

/* Nothing heard now: no session playing, or its squelch closed long enough
 * that the ring holds none of what it let through -- and nothing in the
 * right ear either (`right`: right_playing()). Under S_LOCK. */
static bool quiet_locked(int64_t now, bool right)
{
    return !right && (S.link != RADIO_LINK_READY || (S.squelched && now - S.sq_since >= SQ_QUIET_US));
}

static void save_cb(void *arg)
{
    (void)arg;
    int64_t f;
    char m[6];
    int32_t lo, hi;
    uint8_t agc, nr, sq;
    uint32_t hp;
    int8_t at;
    const bool right = right_playing();
    taskENTER_CRITICAL(&S_LOCK);
    /* Playing now, though it was not when the change came -- a change made
     * while the receiver connected: not in its first seconds of audio, but
     * 30 s after the change, or at a quiet moment before that. */
    const int64_t now = esp_timer_get_time(), since = now - s_t_change;
    if (!quiet_locked(now, right) && since < SAVE_LATER_US) {
        taskEXIT_CRITICAL(&S_LOCK);
        esp_timer_start_once(s_save_t, (uint64_t)(SAVE_LATER_US - since));
        return;
    }
    f = S.tune.f_display;
    strlcpy(m, S.mode, sizeof m);
    lo = S.lo;
    hi = S.hi;
    agc = S.agc;
    nr = S.nr;
    sq = S.sq;
    hp = S.want_hp;
    at = (int8_t)place_of(hp);
    S.dirty = false;
    taskEXIT_CRITICAL(&S_LOCK);
    kv_handle_t h;
    if (kv_open(NVS_NS, &h) != ESP_OK) return;
    kv_edit_begin(h);
    kv_set_i64(h, "kwf", f);
    kv_set_str(h, "kwm", m);
    kv_set_i32(h, "kwl", lo);
    kv_set_i32(h, "kwh", hi);
    kv_set_u8(h, "kwa", agc);
    kv_set_u8(h, "kwn", nr);
    kv_set_u8(h, "kwq", sq);
    if (hp) {
        kv_set_i8(h, "kwrx", at);
        kv_set_u32(h, "kwrxh", hp);
    }
    kv_edit_end(h);
    kv_commit(h);
    kv_close(h);
}

/* A setting changed: into flash at a quiet moment -- soon, with nothing
 * playing; while it plays, 30 s after the last change at the latest, since
 * a flash write stops the audio's interrupts for a moment. */
static void changed(void)
{
    taskENTER_CRITICAL(&S_LOCK);
    S.dirty = true;
    s_t_change = esp_timer_get_time();
    const bool playing = S.link == RADIO_LINK_READY;
    taskEXIT_CRITICAL(&S_LOCK);
    if (!s_save_t) return;
    esp_timer_stop(s_save_t);
    esp_timer_start_once(s_save_t, playing ? SAVE_LATER_US : SAVE_QUIET_US);
}

/* A quiet moment: whatever waits, now. */
static void save_now(void)
{
    taskENTER_CRITICAL(&S_LOCK);
    const bool dirty = S.dirty;
    taskEXIT_CRITICAL(&S_LOCK);
    if (!dirty || !s_save_t) return;
    esp_timer_stop(s_save_t);
    esp_timer_start_once(s_save_t, 1000);
}

static void load(void)
{
    int64_t f = 0;
    char m[8] = "";
    int32_t lo = 0, hi = 0;
    uint8_t agc = KIWI_AGC_MED, nr = KIWI_NR_OFF, sq = 0;
    int8_t at = -1;
    uint32_t hp = 0;
    bool have_hp = false;
    kv_handle_t h;
    if (kv_open(NVS_NS, &h) == ESP_OK) {
        size_t n = sizeof m;
        kv_get_i64(h, "kwf", &f);
        kv_get_str(h, "kwm", m, &n);
        kv_get_i32(h, "kwl", &lo);
        kv_get_i32(h, "kwh", &hi);
        kv_get_u8(h, "kwa", &agc);
        kv_get_u8(h, "kwn", &nr);
        kv_get_u8(h, "kwq", &sq);
        kv_get_i8(h, "kwrx", &at);
        have_hp = kv_get_u32(h, "kwrxh", &hp) == ESP_OK;
        kv_close(h);
    }
    hps_fresh();
    uint32_t hps[SDR_MAX];
    taskENTER_CRITICAL(&S_LOCK);
    const int n_hps = s_hps_n;
    memcpy(hps, s_hps, sizeof hps);
    taskEXIT_CRITICAL(&S_LOCK);
    const int free_k = first_free(hps, n_hps);          /* sdr_rx's lock: not under ours */
    const kiwi_mode_t *k = kiwi_mode_find(m);
    taskENTER_CRITICAL(&S_LOCK);
    tune_init(&S.tune, f >= F_LOW && f <= 2000000000LL ? f : DEFAULT_HZ, 1000);
    /* A Web-888's 6 m, kept: the dial stays there until a receiver says
     * where it tunes. */
    if (S.tune.f_display > S.f_max) S.f_max = S.tune.f_display;
    if (!k) k = kiwi_mode_find("lsb");
    strlcpy(S.mode, k->m, sizeof S.mode);
    S.lo = lo < hi ? lo : k->lo;
    S.hi = lo < hi ? hi : k->hi;
    S.agc = agc <= KIWI_AGC_SLOW ? agc : KIWI_AGC_MED;
    S.nr  = nr <= KIWI_NR_SPEC ? nr : KIWI_NR_OFF;
    S.sq  = sq <= 100 ? sq : 0;
    /* The receiver by its address, as the list may have moved under the
     * place kept; gone, the first the right ear does not have. Kept by place
     * alone, trusted once. */
    if (have_hp && place_of(hp) >= 0) S.want_hp = hp;
    else if (!have_hp && at >= 0 && at < s_hps_n) S.want_hp = s_hps[at];
    else if (s_hps_n > 0) S.want_hp = s_hps[free_k < s_hps_n ? free_k : 0];
    const int i = place_of(S.want_hp);
    taskEXIT_CRITICAL(&S_LOCK);
    ESP_LOGI(TAG, "%lld Hz %s %ld..%ld, AGC %s, NR %s, squelch %u%%; %d receiver%s, %s",
             (long long)S.tune.f_display, S.mode, (long)S.lo, (long)S.hi,
             S.agc == KIWI_AGC_FAST ? "fast" : S.agc == KIWI_AGC_SLOW ? "slow" : "med", NR_NAME[S.nr],
             (unsigned)S.sq, s_hps_n, s_hps_n == 1 ? "" : "s", i >= 0 ? "one in use" : "none in use");
}

/* ------------------------------------------------------------ the face */

static void set_link(radio_link_t l, const char *why, const char *state)
{
    taskENTER_CRITICAL(&S_LOCK);
    S.link = l;
    strlcpy(S.why, why ? why : "", sizeof S.why);
    if (state) strlcpy(s_state, state, sizeof s_state);
    if (l != RADIO_LINK_READY) S.have_dbm = false;
    taskEXIT_CRITICAL(&S_LOCK);
}

/* Down, and why: the face's word and the pages' -- `state` where they say
 * more ("moved to <host>"; NULL: the word). */
static void down_as(kiwi_end_t e, const char *state)
{
    set_link(RADIO_LINK_DOWN, kiwi_end_word(e), state && state[0] ? state : kiwi_end_note(e));
}

static void down(kiwi_end_t e) { down_as(e, NULL); }

/* ------------------------------------------------------------ /status */

static look_t *look_get(uint32_t hp)
{
    look_t *L = look_find(hp);
    if (L) return L;
    /* A place of its own: an empty one, else one of a receiver no longer in
     * the list, else the oldest read. */
    L = &s_look[0];
    for (int i = 0; i < SDR_MAX; i++) {
        if (!s_look[i].hp) { L = &s_look[i]; break; }
        if (!listed(s_look[i].hp)) { L = &s_look[i]; break; }
        if (s_look[i].t_read < L->t_read) L = &s_look[i];
    }
    taskENTER_CRITICAL(&S_LOCK);
    memset(L, 0, sizeof *L);
    L->hp = hp;
    taskEXIT_CRITICAL(&S_LOCK);
    return L;
}

/* What its /status said this boot, if it was read as a Kiwi's: for a mark,
 * with how long ago (the knob's clock). */
static const kiwi_status_t *known(const look_t *L) { return L && L->ok ? &L->st : NULL; }
static int64_t known_age(const look_t *L) { return L ? esp_timer_get_time() - L->t_ok : 0; }

/* The receiver whose /status is being read (ctx: its kiwi_hp) is still the
 * one wanted: another chosen meanwhile is not kept waiting. */
static bool still_wanted(void *ctx)
{
    taskENTER_CRITICAL(&S_LOCK);
    const bool w = S.want_hp == (uint32_t)(uintptr_t)ctx;
    taskEXIT_CRITICAL(&S_LOCK);
    return w;
}

/* Its /status, which may lift a day-limit mark, says what it is (a Web-888's
 * S-meter is biased otherwise), whether it lets apps in, and its antenna. A
 * marked one's at most once a minute, however often it is chosen. A read
 * that fails leaves what an earlier one said, and when; one let go of for
 * another receiver is no read at all -- its once a boot is still to come.
 * One the right ear has read takes the place of this ear's own: the first
 * look, and a marked one's within the minute (kiwi_status_kept) -- once a
 * boot between the two ears. An http:// one's redirect to https:// on its
 * own host, followed by the read, is kept from now (sdr_moved) -- and in
 * `c`, for the session that follows. */
static void look(sdr_cfg_t *c, uint32_t hp, bool for_mark)
{
    look_t *L = look_get(hp);
    const int64_t now = esp_timer_get_time();
    if (for_mark && L->tried && now - L->t_read < LOOK_GAP_US) return;
    int64_t at = 0;
    if ((!L->tried || for_mark) && kiwi_status_kept(hp, &s_read, &at) && (!for_mark || now - at < LOOK_GAP_US)) {
        taskENTER_CRITICAL(&S_LOCK);
        L->tried = true;
        L->again = false;
        L->t_read = L->t_ok = at;
        L->ok = s_read.ok;
        L->st = s_read;
        taskEXIT_CRITICAL(&S_LOCK);
        ESP_LOGI(TAG, "%s:%u: its /status as read %lld s ago", c->host, (unsigned)c->port,
                 (long long)((now - at) / 1000000));
        return;
    }
    const kiwi_addr_t ad = sdr_addr(c);
    kiwi_moved_t mv;
    const char *why = kiwi_status_read(&ad, &s_read, &mv, still_wanted, (void *)(uintptr_t)hp, TAG);
    if (mv.tls_port && !c->tls) {
        sdr_moved(hp, mv.tls_port);
        if (!c->kport) c->kport = c->port;
        c->port = mv.tls_port;
        c->tls = true;
    }
    if (why && !strcmp(why, "left")) return;
    taskENTER_CRITICAL(&S_LOCK);
    L->tried = true;
    L->again = false;
    L->t_read = now;
    if (!why) {
        L->ok = s_read.ok;
        L->st = s_read;
        L->t_ok = now;
    }
    taskEXIT_CRITICAL(&S_LOCK);
    if (!why) kiwi_status_keep(hp, &s_read);
    if (why) {
        ESP_LOGW(TAG, "%s:%u: no /status (%s%s%s)", c->host, (unsigned)c->port, why, mv.to[0] ? " to " : "",
                 mv.to);
        return;
    }
    if (!s_read.ok) {
        ESP_LOGW(TAG, "%s:%u: its /status is not a Kiwi's", c->host, (unsigned)c->port);
        return;
    }
    ESP_LOGI(TAG, "%s:%u: %s, %s; %d of %d in use%s%s", c->host, (unsigned)c->port,
             s_read.antenna[0] ? s_read.antenna : s_read.name, s_read.sw, s_read.users, s_read.users_max,
             s_read.tlimits ? ", time limits" : "", s_read.ext_api == 0 ? ", no apps" : "");
    kiwi_mark_check(hp, &s_read, TAG);
}

/* ------------------------------------------------------------ the session's side */

static void cb_ctl(void *ctx, kiwi_ctl_t *out)
{
    (void)ctx;
    taskENTER_CRITICAL(&S_LOCK);
    out->t.hz = S.tune.f_display;
    strlcpy(out->t.mode, S.mode, sizeof out->t.mode);
    out->t.lo = S.lo;
    out->t.hi = S.hi;
    out->agc = S.agc;
    out->nr = S.nr;
    out->sq_pct = S.sq;
    S.f_sent = S.tune.f_display;
    taskEXIT_CRITICAL(&S_LOCK);
}

/* Where this receiver tunes: the dial is kept inside, and nothing saved for it. */
static void cb_range(void *ctx, int64_t lo, int64_t hi)
{
    (void)ctx;
    const int64_t fmin = lo > F_LOW ? lo : F_LOW;
    const int64_t fmax = hi > fmin ? hi : fmin + 1000000;
    taskENTER_CRITICAL(&S_LOCK);
    S.f_min = fmin;
    S.f_max = fmax;
    if (S.tune.f_display < fmin) tune_assign(&S.tune, fmin);
    else if (S.tune.f_display > fmax) tune_assign(&S.tune, fmax);
    taskEXIT_CRITICAL(&S_LOCK);
    ESP_LOGI(TAG, "it tunes %lld..%lld Hz", (long long)fmin, (long long)fmax);
}

static void cb_audio(void *ctx, const int16_t *pcm, size_t n)
{
    (void)ctx;
    /* A full ring lets the rest go: lost audio, counted as such (rej=). */
    if (!S.suspend && !audio_out_feed_pcm16(pcm, n, 1)) s_lost++;
}

/* The ring's level, for the session to keep it near its target. */
static size_t cb_queued(void *ctx)
{
    (void)ctx;
    return audio_out_queued();
}

/* The target the network has set: the ring waits for that much after a gap. */
static void cb_preroll(void *ctx, size_t n)
{
    (void)ctx;
    audio_out_set_preroll(n);
}

/* The times the ring ran dry, its playing stopped: what is heard as a break. */
static uint32_t cb_underruns(void *ctx)
{
    (void)ctx;
    audio_stats_t a;
    audio_out_stats(&a);
    return a.underruns;
}

static void cb_meter(void *ctx, float dbm, bool ovl, bool squelched)
{
    (void)ctx;
    const int64_t now = esp_timer_get_time();
    const bool right = squelched && right_playing();
    taskENTER_CRITICAL(&S_LOCK);
    if (squelched && !S.squelched) S.sq_since = now;
    S.dbm = dbm;
    S.have_dbm = true;
    S.squelched = squelched;
    if (ovl) S.ovl_until = now + OVL_US;
    /* The squelch closed, and long enough that what it let through has
     * played -- the right ear silent too: a quiet moment, once a closing. */
    const bool quiet = squelched && !S.sq_told && quiet_locked(now, right);
    if (quiet || !squelched) S.sq_told = quiet;
    taskEXIT_CRITICAL(&S_LOCK);
    if (quiet) {
        save_now();
        kiwi_mark_flush();
    }
}

/* The lost audio and the underruns as the last report left them: each
 * report says those of its own 30 s. The session's task alone. */
EXT_RAM_BSS_ATTR static uint32_t s_rep_lost, s_rep_under;

static void report_from_now(void)
{
    audio_stats_t a;
    audio_out_stats(&a);
    s_rep_lost  = s_counts.dropped + s_lost;
    s_rep_under = a.underruns;
}

/* Every 30 s while it plays: how the audio came, and what the ring made of
 * it -- the level against the target the network has set, the breaks it
 * could not ride out; the receiver's sequence numbers tell its own drops
 * from the network's delays; a jump is a backlog left out in one piece. */
static void cb_report(void *ctx, const kiwi_report_t *r)
{
    (void)ctx;
    audio_stats_t a;
    audio_out_stats(&a);
    const uint32_t lost = s_counts.dropped + s_lost;
    ESP_LOGI(TAG, "ring %lu ms of %lu, trim %.5f, frames %lu, dropped %lu, underruns %lu, breaks %lu, jumps %lu "
             "(%lu ms left out); gap %lu ms, seq lost %lu, held %lu",
             (unsigned long)r->level_ms, (unsigned long)r->target_ms, (double)r->trim, (unsigned long)r->frames,
             (unsigned long)(lost - s_rep_lost), (unsigned long)(a.underruns - s_rep_under),
             (unsigned long)r->breaks, (unsigned long)r->jumps, (unsigned long)r->left_ms,
             (unsigned long)r->gap_ms, (unsigned long)r->lost, (unsigned long)r->held);
    s_rep_lost  = lost;
    s_rep_under = a.underruns;
}

static void cb_state(void *ctx, kiwi_state_t st, const kiwi_said_t *said)
{
    (void)ctx;
    if (st == KIWI_ST_CONNECTING) {
        /* No warning while it connects: the slab says so. */
        set_link(RADIO_LINK_CONNECTING, NULL, "connecting");
        return;
    }
    taskENTER_CRITICAL(&S_LOCK);
    s_said_ui = *said;
    taskEXIT_CRITICAL(&S_LOCK);
    if (st == KIWI_ST_LOGGED_IN) {
        set_link(RADIO_LINK_GREETING, NULL, "logged in");
    } else {
        s_t_stream = esp_timer_get_time();
        report_from_now();
        set_link(RADIO_LINK_READY, NULL, "streaming");
    }
}

/* On, until another receiver is chosen, or the list saved under it lost it
 * or changed its passwords. */
static bool cb_go_on(void *ctx)
{
    (void)ctx;
    taskENTER_CRITICAL(&S_LOCK);
    const bool same = S.want_hp == S.run_hp;
    taskEXIT_CRITICAL(&S_LOCK);
    if (!same) return false;
    const uint32_t g = sdr_list_gen();
    if (g == s_run.gen) return true;
    s_run.gen = g;
    hps_fresh();
    taskENTER_CRITICAL(&S_LOCK);
    const int i = place_of(S.run_hp);
    taskEXIT_CRITICAL(&S_LOCK);
    sdr_cfg_t c;
    if (i < 0 || !sdr_get(i, &c)) return false;
    return !strcmp(c.pass, s_run.pass) && !strcmp(c.ipl, s_run.ipl);
}

/* Someone at the knob since the last time this was asked: taken, so the
 * receiver hears of each touch once. */
static bool cb_ack(void *ctx)
{
    (void)ctx;
    taskENTER_CRITICAL(&S_LOCK);
    const bool a = S.activity;
    S.activity = false;
    taskEXIT_CRITICAL(&S_LOCK);
    return a;
}

/* Its redirect to https:// on its own host, followed on the session's way
 * in: kept from now, under its key -- the session's receiver's. */
static void cb_moved(void *ctx, uint16_t tls_port)
{
    (void)ctx;
    taskENTER_CRITICAL(&S_LOCK);
    const uint32_t hp = S.run_hp;
    taskEXIT_CRITICAL(&S_LOCK);
    sdr_moved(hp, tls_port);
}

/* The ring's target is its pre-roll, a frame and a half: the level it holds
 * on average as the frames come, a whole one at a time -- more, up to the
 * ring's pre-roll at its most, while the network keeps breaking the stream
 * up (the task sets the numbers). */
static const kiwi_link_t LINK = {
    .ctl = cb_ctl, .range = cb_range, .audio = cb_audio, .queued = cb_queued, .trim = true,
    .preroll = cb_preroll, .underruns = cb_underruns, .meter = cb_meter, .state = cb_state, .go_on = cb_go_on,
    .ack = cb_ack, .report = cb_report, .moved = cb_moved, .counts = &s_counts, .tag = "kiwi",
};

/* ------------------------------------------------------------ the task */

/* Up to `ms` (FOREVER: for as long as it takes), or until the operator does
 * something: another receiver, any act of choosing, a list saved, LISTEN
 * AGAIN. True when one came. */
static bool wait_event(uint32_t ms, uint32_t hp, uint32_t ch, uint32_t gen)
{
    for (uint32_t t = 0; ms == FOREVER || t < ms; t += 100) {
        vTaskDelay(pdMS_TO_TICKS(100));
        taskENTER_CRITICAL(&S_LOCK);
        const bool ev = S.want_hp != hp || S.chosen != ch || S.relisten;
        taskEXIT_CRITICAL(&S_LOCK);
        if (ev || sdr_list_gen() != gen) return true;
    }
    return false;
}

/* Held from now: a list saved since lets go of what a new setting may cure.
 * A place of its own: an empty one, else one of a receiver no longer in the
 * list -- never another's that is. Kept through a crash as well
 * (kiwi_boot_hold_put): a restart nobody asked for chooses nothing. */
static void hold_set(uint32_t hp, kiwi_end_t end, const char *state)
{
    const uint32_t gen = sdr_list_gen();
    hold_t *h = hold_find(hp);
    for (int i = 0; !h && i < SDR_MAX; i++)
        if (!s_hold[i].hp) h = &s_hold[i];
    for (int i = 0; !h && i < SDR_MAX; i++)
        if (!listed(s_hold[i].hp)) h = &s_hold[i];
    if (!h) h = &s_hold[0];
    const uint32_t gone = h->hp != hp ? h->hp : 0;
    taskENTER_CRITICAL(&S_LOCK);
    h->hp = hp;
    h->end = end;
    h->gen = gen;
    strlcpy(h->state, state ? state : "", sizeof h->state);
    taskEXIT_CRITICAL(&S_LOCK);
    kiwi_boot_hold_put(gone, 0);
    kiwi_boot_hold_put(hp, (uint8_t)end);
}

static void hold_drop(hold_t *h)
{
    const uint32_t hp = h->hp;
    taskENTER_CRITICAL(&S_LOCK);
    memset(h, 0, sizeof *h);
    taskEXIT_CRITICAL(&S_LOCK);
    kiwi_boot_hold_put(hp, 0);
}

/* LISTEN AGAIN, asked while the receiver in use is held for time up or a
 * kick -- and no longer once it is not. */
static void ask(kiwi_end_t e)
{
    const uint8_t c = e == KIWI_END_IDLE || e == KIWI_END_KICKED ? (uint8_t)e : 0;
    taskENTER_CRITICAL(&S_LOCK);
    if (S.choice != c) {
        S.choice = c;
        S.choices_seq++;
    }
    if (!c) S.relisten = false;
    taskEXIT_CRITICAL(&S_LOCK);
}

/* The knob's own attempts on a receiver, at most CAP_N in CAP_US -- a
 * courtesy to its owner, whatever ended them; only those that reached it,
 * though: an hour of WiFi gone is no reason to leave it alone once it is
 * back. 0 when one may go now, else how long until one may. */
static cap_t *cap_get(uint32_t hp)
{
    cap_t *c = NULL;
    for (int i = 0; i < SDR_MAX; i++)
        if (s_cap[i].hp == hp) return &s_cap[i];
    for (int i = 0; !c && i < SDR_MAX; i++)
        if (!s_cap[i].hp) c = &s_cap[i];
    for (int i = 0; !c && i < SDR_MAX; i++)
        if (!listed(s_cap[i].hp)) c = &s_cap[i];
    if (!c) c = &s_cap[0];
    memset(c, 0, sizeof *c);
    c->hp = hp;
    return c;
}

static int64_t cap_wait(uint32_t hp)
{
    const cap_t *c = cap_get(hp);
    const int64_t oldest = c->t[c->head], now = esp_timer_get_time();
    return oldest && now - oldest < CAP_US ? CAP_US - (now - oldest) : 0;
}

/* An attempt made at `t` that reached the receiver. */
static void cap_note(uint32_t hp, int64_t t)
{
    cap_t *c = cap_get(hp);
    c->t[c->head] = t;
    c->head = (uint8_t)((c->head + 1) % CAP_N);
}

/* The receiver in use: by its address, wherever the list has it now; gone,
 * the first the right ear does not have. -1 with none. */
static int pick(uint32_t *hp)
{
    hps_fresh();
    uint32_t hps[SDR_MAX];
    taskENTER_CRITICAL(&S_LOCK);
    const bool gone = place_of(S.want_hp) < 0;
    const int n = s_hps_n;
    memcpy(hps, s_hps, sizeof hps);
    taskEXIT_CRITICAL(&S_LOCK);
    const int k = gone ? first_free(hps, n) : 0;     /* sdr_rx's lock: not under ours */
    taskENTER_CRITICAL(&S_LOCK);
    int i = place_of(S.want_hp);
    if (i < 0 && s_hps_n > 0) {
        i = k < s_hps_n ? k : 0;
        S.want_hp = s_hps[i];
        S.dirty = true;
    }
    *hp = i >= 0 ? S.want_hp : 0;
    taskEXIT_CRITICAL(&S_LOCK);
    return i;
}

/* Whether the operator has gone on to another receiver meanwhile. */
static bool moved_from(uint32_t hp)
{
    taskENTER_CRITICAL(&S_LOCK);
    const bool m = S.want_hp != hp;
    taskEXIT_CRITICAL(&S_LOCK);
    return m;
}

static void task(void *arg)
{
    (void)arg;
    kiwi_sess_t *ss = kiwi_sess_new();
    if (!ss) {
        ESP_LOGE(TAG, "no memory for the receiver's buffers");
        set_link(RADIO_LINK_DOWN, "NO MEMORY", "no memory");
        vTaskDeleteWithCaps(NULL);          /* made WithCaps: its stack goes with it */
    }
    kiwi_link_t link = LINK;
    link.target     = audio_out_preroll();
    link.room       = audio_out_room();
    link.target_max = audio_out_preroll_max();
    uint32_t seen = 0;              /* the operator's acts, looked at */
    uint32_t last_hp = 0;           /* the receiver last looked at... */
    char     last_ipl[32] = "";     /* ...and its time-limit password then */
    uint8_t  backoff = 0, full_n = 0, nf_n = 0, silent_n = 0;
    bool     boot = true;
    static const uint16_t BACKOFF_S[] = { 2, 5, 10, 30, 60 };

    /* What held a receiver back when the knob crashed holds it still: time
     * up and kicked ask LISTEN AGAIN as before, the rest wait to be chosen. */
    {
        hps_fresh();
        uint32_t hps[SDR_MAX];
        taskENTER_CRITICAL(&S_LOCK);
        const int n = s_hps_n;
        memcpy(hps, s_hps, sizeof hps);
        taskEXIT_CRITICAL(&S_LOCK);
        for (int i = 0; i < n; i++) {
            const uint8_t why = kiwi_boot_hold_get(hps[i]);
            if (!why) continue;
            ESP_LOGW(TAG, "receiver %d: %s before the restart -- not again until chosen again", i,
                     kiwi_end_note((kiwi_end_t)why));
            hold_set(hps[i], (kiwi_end_t)why, NULL);
        }
    }

    for (;;) {
        const uint32_t gen = sdr_list_gen();
        uint32_t hp;
        const int i = pick(&hp);
        sdr_cfg_t c;
        if (i < 0 || !sdr_get(i, &c)) {
            /* None: the page's job first, its address on the face. */
            ask(KIWI_END_NONE);
            set_link(RADIO_LINK_DOWN, "NO RECEIVER", "no receiver");
            while (sdr_list_gen() == gen) vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        /* The operator's act for it since the last look -- LISTEN AGAIN is
         * one -- else none: a boot, a list saved, a retry never are. A try
         * on one held for its day limit only if it was held as it was
         * chosen: a Test refused after the choice holds it until the
         * operator chooses it again. */
        taskENTER_CRITICAL(&S_LOCK);
        bool chosen = (S.chosen != seen && S.chosen_hp == hp) || S.relisten;
        bool tries = chosen && S.chosen_marked;
        seen = S.chosen;
        S.relisten = false;
        const uint32_t ch = S.chosen;
        taskEXIT_CRITICAL(&S_LOCK);
        /* A new time-limit password for a day-limited receiver: what the
         * operator does to listen again, so as chosen again -- one counted
         * try, never a fresh count, as a mistyped one is refused like any. */
        if (hp == last_hp && c.ipl[0] && strcmp(c.ipl, last_ipl) && kiwi_mark_get(hp, NULL)) {
            ESP_LOGI(TAG, "a new time-limit password for a day-limited receiver: as chosen again");
            chosen = tries = true;
        }
        if (hp != last_hp || chosen) backoff = full_n = nf_n = silent_n = 0;
        if (hp != last_hp) save_now();          /* the receiver in use, before its audio */
        last_hp = hp;
        strlcpy(last_ipl, c.ipl, sizeof last_ipl);

        /* Held back until chosen again; a list saved lets go of what a new
         * setting may cure. */
        hold_t *h = hold_find(hp);
        if (h && (chosen || (h->gen != gen && (h->end == KIWI_END_PASSWORD || h->end == KIWI_END_NOT_KIWI ||
                                               h->end == KIWI_END_NO_APPS || h->end == KIWI_END_MOVED ||
                                               h->end == KIWI_END_CERT)))) {
            hold_drop(h);
            h = NULL;
        }
        if (h) {
            ask(h->end);
            down_as(h->end, h->state);
            wait_event(FOREVER, hp, ch, gen);
            continue;
        }
        ask(KIWI_END_NONE);

        /* After a run of crashes, each soon after the last start, the
         * receivers wait for the knob: its own first contact this boot -- a
         * status read, a login -- comes no sooner than kiwi_boot_hold_us().
         * The operator's choice goes at once. */
        const int64_t hold_until = kiwi_boot_hold_us();
        if (!chosen && esp_timer_get_time() < hold_until) {
            down(KIWI_END_TRY_LATER);
            wait_event((uint32_t)((hold_until - esp_timer_get_time()) / 1000) + 100, hp, ch, gen);
            continue;
        }
        const bool first = boot;
        boot = false;

        /* A day-limit mark: the receiver's own word first, which may lift it;
         * then a try, only when chosen again, counted in flash before the
         * login it allows -- a restart in between must not hand it back. */
        look_t *L = look_get(hp);
        bool paid = false;              /* this login is a try, counted already */
        if (kiwi_mark_get(hp, NULL)) {
            if (chosen || first) look(&c, hp, true);
            /* The operator may have gone on meanwhile: a try is spent on the
             * receiver chosen, never on one left. */
            if (moved_from(hp)) continue;
            kiwi_mark_t mk;
            if (kiwi_mark_get(hp, &mk)) {
                if (!tries || !kiwi_mark_try(hp, TAG)) {
                    /* Its login unanswered, it says what it last said: no
                     * answer, apps full. A refusal: DAY LIMIT. */
                    const bool unsure = mk.flags & KIWI_MARK_UNSURE;
                    const kiwi_end_t e = !unsure ? KIWI_END_DAY_LIMIT
                                       : L->end > KIWI_END_WANT ? (kiwi_end_t)L->end : KIWI_END_NO_ANSWER;
                    ESP_LOGW(TAG, "%s:%u: %s%s -- not again until chosen again, %d tries at most", c.host,
                             (unsigned)c.port, unsure ? "its login went unanswered" : "day limit",
                             chosen && !tries ? ", marked after it was chosen" : "", KIWI_STRIKES_MAX);
                    down(e);
                    wait_event(FOREVER, hp, ch, gen);
                    continue;
                }
                paid = true;
                set_link(RADIO_LINK_CONNECTING, NULL, "connecting");
                /* In flash before the login -- unless the flash cannot keep
                 * it, and then the operator's choice goes as it is. */
                while (!kiwi_mark_saved() && kiwi_mark_durable() && !moved_from(hp)) vTaskDelay(pdMS_TO_TICKS(100));
                if (moved_from(hp)) continue;
            }
        }

        /* Its /status: once a boot before its first session; again only five
         * minutes on, after an end it may explain. Never in a retry loop.
         * The session is on its way from here: a Test of this receiver
         * meanwhile answers from it, with no login of its own beside it. */
        if (!L->tried || (L->again && esp_timer_get_time() - L->t_read >= STATUS_AGAIN_US)) {
            set_link(RADIO_LINK_CONNECTING, NULL, "connecting");
            look(&c, hp, false);
            if (moved_from(hp)) continue;
        }
        if (L->ok && L->st.kind == KIWI_KIND_KIWISDR && L->st.ext_api == 0) {
            /* A KiwiSDR that lets no apps in: said, with no socket opened. */
            ESP_LOGW(TAG, "%s:%u: lets no apps listen -- not again until chosen again", c.host, (unsigned)c.port);
            hold_set(hp, KIWI_END_NO_APPS, NULL);
            continue;
        }

        /* A flash that cannot keep a mark: a refusal now would be forgotten
         * at the next start, and the knob would log in by itself again, at
         * every start. So on its own it logs in only where no day limit can
         * count; the operator's choice goes as it is. */
        if (!chosen && !kiwi_mark_durable() && kiwi_mark_may_count(hp, known(L))) {
            ESP_LOGW(TAG, "%s:%u: the flash keeps no day-limit mark -- logged in to only when chosen",
                     c.host, (unsigned)c.port);
            down(KIWI_END_NO_FLASH);
            wait_event(FOREVER, hp, ch, gen);
            continue;
        }

        /* The knob's own attempts, capped; the operator's go at once. */
        if (!chosen) {
            const int64_t w = cap_wait(hp);
            if (w > 0) {
                ESP_LOGW(TAG, "%s:%u: %d attempts in 10 minutes -- again in %lld s", c.host, (unsigned)c.port,
                         CAP_N, (long long)(w / 1000000));
                down(KIWI_END_TRY_LATER);
                wait_event((uint32_t)(w / 1000) + 100, hp, ch, gen);
                continue;
            }
        }
        /* On its way, said before a Test is looked at: one logging in to this
         * receiver now is waited for, and its answer read -- then the mark
         * again, as a refusal of the Test's holds the session back too. Two
         * logins at once could spend two strikes at once (kiwi_test_busy).
         * So is the right ear's session on it, which lets go of it for this
         * ear (sdr_rx_primary_cb): one receiver is never in both ears. The
         * session's receiver is said first (S.run_hp, what the right ear
         * looks at), and only then the right ear's looked at -- as the right
         * ear says its own before it looks here: one of the two always sees
         * the other, even with this ear moved off it meanwhile. */
        set_link(RADIO_LINK_CONNECTING, NULL, "connecting");
        taskENTER_CRITICAL(&S_LOCK);
        S.run_hp = hp;
        taskEXIT_CRITICAL(&S_LOCK);
        while ((kiwi_test_busy(hp) || sdr_rx_in_session(hp)) && !moved_from(hp)) vTaskDelay(pdMS_TO_TICKS(100));
        const bool marked = !moved_from(hp) && !paid && kiwi_mark_get(hp, NULL);
        if (moved_from(hp) || marked) {
            if (marked) ESP_LOGW(TAG, "%s:%u: a Test's login marked it meanwhile", c.host, (unsigned)c.port);
            taskENTER_CRITICAL(&S_LOCK);
            S.run_hp = 0;
            taskEXIT_CRITICAL(&S_LOCK);
            continue;
        }
        const int64_t t_try = esp_timer_get_time();
        kiwi_boot_hold_put(hp, 0);              /* logged in to: nothing holds it, crash or not */

        audio_out_flush();
        taskENTER_CRITICAL(&S_LOCK);
        S.connects++;
        S.squelched = S.sq_told = false;    /* the squelch is this session's to say */
        /* The receiver's idle timer starts at the login: a touch from
         * before it is no news to this session. */
        S.activity = false;
        taskEXIT_CRITICAL(&S_LOCK);
        s_run.gen = gen;
        strlcpy(s_run.pass, c.pass, sizeof s_run.pass);
        strlcpy(s_run.ipl, c.ipl, sizeof s_run.ipl);
        s_t_stream = 0;
        const kiwi_addr_t ad = sdr_addr(&c);
        const kiwi_end_t end = kiwi_sess_run(ss, &ad, known(L), &link, &s_said);
        const int64_t streamed = s_t_stream ? esp_timer_get_time() - s_t_stream : 0;
        if (!chosen && s_said.connected) cap_note(hp, t_try);
        L->end = (uint8_t)end;
        audio_out_flush();
        taskENTER_CRITICAL(&S_LOCK);
        S.run_hp = 0;
        S.closes++;
        s_said_ui = s_said;
        /* What was chosen while it played chose it as it was: no attempt
         * through what ended it. */
        if (S.chosen_hp == hp) seen = S.chosen;
        taskEXIT_CRITICAL(&S_LOCK);
        /* Kicked, its owner's message; moved, where to. */
        const char *more = end == KIWI_END_KICKED ? s_said.kick : end == KIWI_END_MOVED ? s_said.moved.to : "";
        snprintf(s_last_close, sizeof s_last_close, "%s%s%.38s", end == KIWI_END_MOVED && more[0] ? "moved to"
                 : kiwi_end_note(end), more[0] ? (end == KIWI_END_MOVED ? " " : ": ") : "", more);
        if (end == KIWI_END_WANT) set_link(RADIO_LINK_CONNECTING, NULL, "connecting");
        else                      down_as(end, end == KIWI_END_MOVED ? s_last_close : NULL);
        /* A quiet moment, with the right ear quiet too: a cleared mark, and
         * the settings, to flash -- save_cb looks at both ears itself. A mark
         * at rest waits otherwise, 30 s at most (kiwi_mark.h). */
        if (!right_playing()) kiwi_mark_flush();
        save_now();
        if (streamed >= STREAM_OK_US) backoff = 0;
        if (end != KIWI_END_SILENT) silent_n = 0;
        if (end == KIWI_END_NO_ANSWER || end == KIWI_END_SILENT || end == KIWI_END_APPS_FULL ||
            end == KIWI_END_NO_APPS)
            L->again = true;

        /* Its login left unanswered, by a receiver whose day limit may count
         * it: one refusal, in case its answer was one that never arrived.
         * Held from now, as any mark holds -- through restarts. */
        if (s_said.unanswered && kiwi_mark_may_count(hp, known(L))) {
            kiwi_mark_set(hp, KIWI_MARK_NO_ANSWER, paid, known(L), known_age(L), TAG);
            continue;
        }

        uint32_t wait_s = 0;
        switch (end) {
        case KIWI_END_WANT:
            continue;                           /* another receiver, at once */
        case KIWI_END_DAY_LIMIT:
            /* Marked: held by that from now on, through restarts. */
            kiwi_mark_set(hp, s_said.limit_at_login ? KIWI_MARK_REFUSED : KIWI_MARK_MIDWAY, paid, known(L),
                          known_age(L), TAG);
            continue;
        case KIWI_END_IDLE:
        case KIWI_END_KICKED:
        case KIWI_END_REFUSED:
        case KIWI_END_PASSWORD:
        case KIWI_END_NOT_KIWI:
        case KIWI_END_NO_APPS:
        case KIWI_END_MOVED:
        case KIWI_END_CERT:
            ESP_LOGW(TAG, "%s:%u: %s -- not again until chosen again", c.host, (unsigned)c.port,
                     end == KIWI_END_MOVED ? s_last_close : kiwi_end_note(end));
            hold_set(hp, end, end == KIWI_END_MOVED ? s_last_close : NULL);
            continue;
        case KIWI_END_SILENT:
            /* A KiwiSDR's door for apps, shut: its app channels all taken --
             * or, three times in a row, none for apps at all. */
            if (++silent_n >= SILENT_MAX) {
                ESP_LOGW(TAG, "%s:%u: silent %d times -- no apps, not again until chosen again", c.host,
                         (unsigned)c.port, silent_n);
                hold_set(hp, KIWI_END_NO_APPS, NULL);
                continue;
            }
            wait_s = 120;
            break;
        case KIWI_END_APPS_FULL:
            wait_s = 120;
            break;
        case KIWI_END_FULL:
        case KIWI_END_PWD_FULL:
            wait_s = full_n++ ? 60 : 30;
            break;
        case KIWI_END_NOT_FOUND:
            wait_s = nf_n++ ? 60 : 30;
            break;
        case KIWI_END_DUP_IP:
        case KIWI_END_UPDATING:
        case KIWI_END_DOWN:
        case KIWI_END_TRY_LATER:
            wait_s = 60;
            break;
        default:                                /* no route, no answer, closed, quiet ... */
            wait_s = BACKOFF_S[backoff];
            if (backoff + 1 < (int)(sizeof BACKOFF_S / sizeof BACKOFF_S[0])) backoff++;
            break;
        }
        ESP_LOGW(TAG, "%s:%u: %s -- again in %lu s", c.host, (unsigned)c.port, kiwi_end_note(end),
                 (unsigned long)wait_s);
        wait_event(wait_s * 1000, hp, ch, gen);
    }
}

/* ------------------------------------------------------------- kiwi.h */

bool kiwi_rx_use(int i, bool chosen)
{
    hps_fresh();
    /* Held for its day limit as it is chosen: then the choice is a try. */
    uint32_t hp = 0;
    taskENTER_CRITICAL(&S_LOCK);
    if (i >= 0 && i < s_hps_n) hp = s_hps[i];
    const bool mine = hp && hp == S.want_hp;
    taskEXIT_CRITICAL(&S_LOCK);
    if (!hp) return false;
    /* The right ear's: one receiver is never in both ears -- unless it is
     * this ear's already, a list saved under both. One the right ear is only
     * leaving is taken, and waited out. */
    if (!mine && sdr_rx_uses(hp)) {
        ESP_LOGW(TAG, "receiver %d plays in the right ear: not in the left as well", i);
        return false;
    }
    const bool marked = chosen && kiwi_mark_get(hp, NULL);
    bool moved = false;
    taskENTER_CRITICAL(&S_LOCK);
    if (i >= 0 && i < s_hps_n) {
        moved = S.want_hp != s_hps[i];
        S.want_hp = s_hps[i];
        if (chosen) {
            S.chosen++;
            S.chosen_hp = s_hps[i];
            S.chosen_marked = marked && hp == s_hps[i];
        }
    }
    taskEXIT_CRITICAL(&S_LOCK);
    if (moved) changed();
    char l[24];
    if (kiwi_rx_label(i, l, sizeof l))
        ESP_LOGI(TAG, "receiver %d (%s)%s%s", i, l, moved ? " in use" : "", !moved && chosen ? " chosen again" : "");
    return true;
}

/* The receiver in use -- and, not `chosen` only, the one the session still
 * has, on its way off it (sdr_rx's primary): the right ear never logs in to
 * it beside this ear. */
static bool uses(uint32_t hp, bool chosen)
{
    taskENTER_CRITICAL(&S_LOCK);
    const bool u = hp && (hp == S.want_hp || (!chosen && hp == S.run_hp));
    taskEXIT_CRITICAL(&S_LOCK);
    return u;
}

bool kiwi_audible(void)
{
    const int64_t now = esp_timer_get_time();
    const bool right = right_playing();
    taskENTER_CRITICAL(&S_LOCK);
    const bool a = !quiet_locked(now, right);
    taskEXIT_CRITICAL(&S_LOCK);
    return a;
}

void kiwi_info(kiwi_info_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof *out);
    out->users = out->users_max = out->ext_api = out->strikes = -1;
    const int i = kiwi_rx_active();
    sdr_cfg_t c;
    const bool have = i >= 0 && sdr_get(i, &c);
    taskENTER_CRITICAL(&S_LOCK);
    const uint32_t hp = S.want_hp;
    const look_t *L = look_find(hp);
    if (L && L->ok) {
        const kiwi_status_t *st = &L->st;
        out->known = true;
        strlcpy(out->sw, st->sw, sizeof out->sw);
        strlcpy(out->antenna, st->antenna, sizeof out->antenna);
        strlcpy(out->loc, st->loc, sizeof out->loc);
        out->users = st->users;
        out->users_max = st->users_max;
        out->ext_api = st->ext_api;
        strlcpy(out->model, st->kind == KIWI_KIND_WEB888 ? "Web-888" : st->kind == KIWI_KIND_KIWISDR ? "KiwiSDR" : "",
                sizeof out->model);
    }
    if (!out->model[0] && S.run_hp == hp && s_said_ui.version_maj > 0)
        strlcpy(out->model, s_said_ui.version_maj >= 2000 ? "Web-888" : "KiwiSDR", sizeof out->model);
    out->rate = s_said_ui.rate > 0 ? s_said_ui.rate : s_said_ui.audio_rate;
    out->offset_khz = s_said_ui.offset_khz;
    out->f_max = S.f_max;
    out->ovl = S.ovl_until > esp_timer_get_time();
    strlcpy(out->state, s_state, sizeof out->state);
    const bool connecting = S.link == RADIO_LINK_CONNECTING || S.link == RADIO_LINK_GREETING;
    taskEXIT_CRITICAL(&S_LOCK);
    /* The slab: under its name, its antenna -- unless the name is its antenna
     * already, as the dial would name it ("EchoTracer", the Lombardsijde
     * receivers' "RF.Guru EchoTracer") -- else its model and address, or
     * only its model where the name above is its address whole; and under
     * that where it is, or that it is on its way. */
    if (have) {
        const bool shared = sdr_host_shared(i);
        /* An https:// one on its own port is its name alone. */
        const uint16_t lp = kiwi_label_port(c.port, c.tls);
        const bool bare = !lp;
        char lbl[24], ant[24], addr[24];
        kiwi_label(lbl, sizeof lbl, c.name, out->antenna, c.host, lp, shared);
        kiwi_label(ant, sizeof ant, "", out->antenna, c.host, lp, shared);
        const int an = bare ? snprintf(addr, sizeof addr, "%s", c.host)
                            : snprintf(addr, sizeof addr, "%s:%u", c.host, (unsigned)c.port);
        const char *a = out->antenna;
        while (*a == ' ') a++;
        if (*a && c.name[0] && strcasecmp(lbl, ant))
            strlcpy(out->line2, out->antenna, sizeof out->line2);
        else if (an > 0 && (size_t)an < sizeof addr && !strcmp(lbl, addr))
            strlcpy(out->line2, out->model, sizeof out->line2);
        else if (bare)
            snprintf(out->line2, sizeof out->line2, "%s%s%.30s", out->model, out->model[0] ? "  " : "", c.host);
        else
            snprintf(out->line2, sizeof out->line2, "%s%s%.24s:%u", out->model, out->model[0] ? "  " : "",
                     c.host, (unsigned)c.port);
    }
    strlcpy(out->line3, connecting ? "connecting..." : out->loc, sizeof out->line3);
    kiwi_mark_t m;
    if (hp && kiwi_mark_get(hp, &m)) {
        out->strikes = m.strikes;
        out->held = m.strikes >= KIWI_STRIKES_MAX;
        out->unsure = m.flags & KIWI_MARK_UNSURE;
    }
}

/* ------------------------------------------------------------- radio.h */

const char *radio_link_name(void) { return "WS"; }

esp_err_t radio_start(const char *host, uint16_t port, const char *user, const char *pass)
{
    /* Its receivers are the page's list, each with its own address. */
    (void)host;
    (void)port;
    (void)user;
    (void)pass;
    /* The right ear keeps off this ear's receiver: told so before this ear
     * has one (load()), so it never plays on with one it was not told of.
     * It plays with this ear's AGC, noise filter and squelch. */
    sdr_rx_primary_cb(uses);
    sdr_rx_settings_cb(right_settings);
    sdr_list_init();
    kiwi_mark_init(TAG);
    load();
    const esp_timer_create_args_t ta = { .callback = save_cb, .name = "kwsave" };
    if (!s_save_t) esp_timer_create(&ta, &s_save_t);
    /* Its stack in PSRAM: room for a TLS handshake -- some 9 kB on the PC
     * (test/host's model) -- beside the session's own. */
    ESP_RETURN_ON_FALSE(xTaskCreatePinnedToCoreWithCaps(task, "kiwi", 16384, NULL, 5, NULL, 0,
                                                        MALLOC_CAP_SPIRAM) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "task");
    return ESP_OK;
}

int64_t radio_tune_by(int32_t detents, uint8_t accel_mult, int32_t step_hz)
{
    int64_t f;
    taskENTER_CRITICAL(&S_LOCK);
    if (detents) {
        if (S.tune.step_hz != step_hz) tune_set_step(&S.tune, step_hz);
        tune_apply(&S.tune, detents, accel_mult, LOOP_MS, S.f_min, S.f_max);
        S.t_input = now_ms();
    }
    f = S.tune.f_display;
    taskEXIT_CRITICAL(&S_LOCK);
    if (detents) changed();
    return f;
}

void radio_set_step(int32_t step_hz)
{
    taskENTER_CRITICAL(&S_LOCK);
    const int64_t was = S.tune.f_display;
    tune_set_step(&S.tune, step_hz);
    const bool moved = S.tune.f_display != was;
    taskEXIT_CRITICAL(&S_LOCK);
    if (moved) changed();
}

void radio_audio_suspend(bool suspend)
{
    /* Its frames are still decoded meanwhile: only not played. */
    S.suspend = suspend;
    ESP_LOGW(TAG, "audio %s", suspend ? "suspended" : "resumed");
}

void radio_get_status(radio_status_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof *out);
    char label[24] = "";
    const int i = kiwi_rx_active();
    if (i >= 0) kiwi_rx_label(i, label, sizeof label);
    char model[12] = "";
    taskENTER_CRITICAL(&S_LOCK);
    const look_t *L = look_find(S.want_hp);
    if (L && L->ok && L->st.kind)
        strlcpy(model, L->st.kind == KIWI_KIND_WEB888 ? "Web-888" : "KiwiSDR", sizeof model);
    else if (s_said_ui.version_maj > 0)
        strlcpy(model, s_said_ui.version_maj >= 2000 ? "Web-888" : "KiwiSDR", sizeof model);
    out->link       = S.link;
    out->f_display  = S.tune.f_display;
    out->f_server   = S.f_sent;
    strlcpy(out->mode, S.mode, sizeof out->mode);
    out->filt_lo    = S.lo;
    out->filt_hi    = S.hi;
    out->smeter_dbm = S.have_dbm && S.link == RADIO_LINK_READY ? S.dbm : -127.0f;
    strlcpy(out->agc, S.agc == KIWI_AGC_FAST ? "fast" : S.agc == KIWI_AGC_SLOW ? "slow" : "med", sizeof out->agc);
    /* The noise filter, right of the S-meter, as a gain whose steps have
     * names: OFF, then the receivers' three. The squelch, a swipe from the
     * right. Both the knob's own to say, link or none. */
    out->have_gain    = true;
    out->gain         = (int8_t)S.nr;
    out->gain_min     = 0;
    out->gain_max     = KIWI_NR_SPEC;
    out->gain_step    = 1;
    out->n_gain_names = (uint8_t)(sizeof NR_NAME / sizeof NR_NAME[0]);
    for (size_t k = 0; k < sizeof NR_NAME / sizeof NR_NAME[0]; k++)
        strlcpy(out->gain_names[k], NR_NAME[k], sizeof out->gain_names[k]);
    out->has_squelch  = out->have_squelch = true;
    out->squelch_pct  = S.sq;
    strlcpy(out->link_why, S.why, sizeof out->link_why);
    out->f_min      = S.f_min;
    out->f_max      = S.f_max;
    out->rx_only    = true;
    out->n_choices  = S.choice ? 1 : 0;
    out->choices_seq = S.choices_seq;
    out->connects   = S.connects;
    out->closes     = S.closes;
    out->sends      = s_counts.sent;
    out->echoes     = s_counts.frames;
    /* Lost audio only: frames not a Kiwi's, stereo or undecodable, and
     * feeds a full ring let go -- not the receiver's settings at its login,
     * too big for the buffer and let go by unread (s_counts.big). */
    out->rejects    = s_counts.dropped + s_lost;
    strlcpy(out->last_close, s_last_close, sizeof out->last_close);
    taskEXIT_CRITICAL(&S_LOCK);
    strlcpy(out->model, model, sizeof out->model);
    strlcpy(out->server, label, sizeof out->server);
}

bool radio_is_ready(void) { return S.link == RADIO_LINK_READY; }
bool radio_on_air(void) { return false; }

/* A finger on the glass, a turn of the knob (radio.h): someone is listening,
 * which a receiver whose owner limits idle listening wants to hear -- and
 * hears from the session, a minute apart at most (cb_ack). */
void radio_user_activity(void)
{
    taskENTER_CRITICAL(&S_LOCK);
    S.activity = true;
    taskEXIT_CRITICAL(&S_LOCK);
}

/* Receive only: there is nothing to key. */
void radio_ptt_key(void) {}
void radio_ptt_unkey(void) {}
void radio_ptt_toggle(void) {}
void radio_ptt_force_abort(uint8_t reason) { (void)reason; }

/* A mode with the passband it opens with -- or, from one sideband to the
 * other, the same passband the other side round. Under S_LOCK. */
static void mode_locked(const kiwi_mode_t *k, bool mirror)
{
    const bool flip = mirror && ((!strcmp(S.mode, "usb") && !strcmp(k->m, "lsb")) ||
                                 (!strcmp(S.mode, "lsb") && !strcmp(k->m, "usb")));
    if (flip && S.lo < S.hi) {
        const int32_t lo = S.lo;
        S.lo = -S.hi;
        S.hi = -lo;
    } else {
        S.lo = k->lo;
        S.hi = k->hi;
    }
    strlcpy(S.mode, k->m, sizeof S.mode);
    S.t_input = now_ms();
}

void radio_set_mode(const char *mode)
{
    const kiwi_mode_t *k = kiwi_mode_find(kiwi_mode_of(mode));
    if (!k) return;
    taskENTER_CRITICAL(&S_LOCK);
    const bool other = strcmp(S.mode, k->m) != 0;
    if (other) mode_locked(k, false);
    taskEXIT_CRITICAL(&S_LOCK);
    if (other) changed();
}

void radio_set_filter(int32_t lo, int32_t hi)
{
    if (lo >= hi || lo < -12000 || hi > 12000) return;
    taskENTER_CRITICAL(&S_LOCK);
    S.lo = lo;
    S.hi = hi;
    S.t_input = now_ms();
    taskEXIT_CRITICAL(&S_LOCK);
    changed();
}

void radio_set_agc(const char *agc)
{
    if (!agc) return;
    const int a = !strcasecmp(agc, "fast") ? KIWI_AGC_FAST
                : !strcasecmp(agc, "med") || !strcasecmp(agc, "mid") ? KIWI_AGC_MED
                : !strcasecmp(agc, "slow") ? KIWI_AGC_SLOW : -1;
    if (a < 0) return;
    taskENTER_CRITICAL(&S_LOCK);
    const bool other = S.agc != a;
    S.agc = (uint8_t)a;
    taskEXIT_CRITICAL(&S_LOCK);
    if (other) changed();
}

void radio_goto_freq(int64_t hz)
{
    taskENTER_CRITICAL(&S_LOCK);
    if (hz < S.f_min) hz = S.f_min;
    if (hz > S.f_max) hz = S.f_max;
    tune_assign(&S.tune, hz);
    /* A band's own sideband, for a voice: lower below 10 MHz, and upper on
     * 60 m; off the bands the mode stays as it is. */
    const char *sb = kiwi_sideband(hz);
    if (sb && (!strcmp(S.mode, "usb") || !strcmp(S.mode, "lsb")) && strcmp(S.mode, sb))
        mode_locked(kiwi_mode_find(sb), true);
    S.t_input = now_ms();
    taskEXIT_CRITICAL(&S_LOCK);
    changed();
}

/* The noise filter (the gain's place, right of the S-meter): 0 off, then
 * WDSP, LMS and spectral, as the receivers' page offers them. Sent once the
 * choice has rested half a second (kiwi_sess.c): the knob turns through
 * them, and each is a dozen commands. */
void radio_set_gain(int8_t gain)
{
    if (gain < 0) gain = 0;
    if (gain > KIWI_NR_SPEC) gain = KIWI_NR_SPEC;
    taskENTER_CRITICAL(&S_LOCK);
    const bool other = S.nr != (uint8_t)gain;
    S.nr = (uint8_t)gain;
    taskEXIT_CRITICAL(&S_LOCK);
    if (other) changed();
}

/* The squelch, 0-100 %, 0 open: in NBFM the receiver's own scale, in the
 * other modes up to 40 dB over its noise (kiwi_squelch_cmd). Closed, its
 * frames come on, silenced (kiwi_snd_audio), and it is a quiet moment. */
void radio_set_squelch(uint8_t pct)
{
    if (pct > 100) pct = 100;
    taskENTER_CRITICAL(&S_LOCK);
    const bool other = S.sq != pct;
    S.sq = pct;
    taskEXIT_CRITICAL(&S_LOCK);
    if (other) changed();
}

/* Nothing of these on a web receiver. */
void radio_select_filter(uint8_t n) { (void)n; }
void radio_set_rit(int32_t hz) { (void)hz; }
void radio_memory_mode(bool on) { (void)on; }
void radio_memory_group(uint8_t group) { (void)group; }
void radio_select_rx(uint8_t rx) { (void)rx; }
void radio_set_antenna(uint8_t ant, bool rx_ant) { (void)ant; (void)rx_ant; }
void radio_tune(void) {}
void radio_atu_tune(void) {}
void radio_atu_memories(bool on) { (void)on; }
void radio_set_rf_gain(uint8_t pct) { (void)pct; }
void radio_set_rf_power(uint8_t pct) { (void)pct; }
void radio_set_tuner(bool on) { (void)on; }
void radio_tg_lock(bool locked) { (void)locked; }
void radio_mute(bool muted) { (void)muted; }

/* The one question: the receiver ended the session for idling, or its owner
 * sent the knob away -- another? Not without the operator, as its own page
 * does not. */
bool radio_get_choice(uint8_t i, char *title, size_t tn, char *name, size_t nn)
{
    taskENTER_CRITICAL(&S_LOCK);
    const uint8_t c = S.choice;
    taskEXIT_CRITICAL(&S_LOCK);
    if (i || !c) return false;
    strlcpy(title, c == KIWI_END_IDLE ? "TIME UP" : "KICKED", tn);
    strlcpy(name, "LISTEN AGAIN", nn);
    return true;
}

void radio_choose(uint8_t i)
{
    taskENTER_CRITICAL(&S_LOCK);
    const uint32_t hp = S.want_hp;
    taskEXIT_CRITICAL(&S_LOCK);
    const bool marked = hp && kiwi_mark_get(hp, NULL);   /* as kiwi_rx_use: then a try */
    taskENTER_CRITICAL(&S_LOCK);
    const bool asked = i == 0 && S.choice;
    if (asked) {
        S.relisten = true;
        S.chosen_marked = marked && hp == S.want_hp;
    }
    taskEXIT_CRITICAL(&S_LOCK);
    if (asked) ESP_LOGI(TAG, "listening again, as asked");
}
