/* The openwebrx firmware's receiver: radio.h for an OpenWebRX or an
 * OpenWebRX+, over their protocol (components/owrx_proto's session, on the
 * web SDRs' link). See owrx.h.
 *
 * One task keeps one receiver going. The configuration page's list (net_prov:
 * up to four) says which there are, the operator which is in use; another is
 * taken over at once, the session ended and the next one begun, with no
 * restart. One that cannot be reached -- its name not found, no route, no
 * answer, silent -- hands over to the next in the list, in turn, the one in
 * use after a fair chance (FAIR_TRIES); the first that plays is in use from
 * then on, saved as such. One that answers only to refuse keeps its turn if
 * it is the one in use (full, no SDR running: its own word, and it is waited
 * for as its own page waits), and is passed by if it is a stand-in. What its
 * owner will not have -- the address banned, a front that refuses, no
 * OpenWebRX there, a certificate, a redirect elsewhere -- holds it until it
 * is chosen again, or the list is saved (all but a ban). Every wait grows as
 * the receivers' own page's does (owrx_retry_ms), with some jitter.
 *
 * Its status.json is read once a boot, before its first session: its name,
 * its version, and where each of its bands is, for the BAND list. A band is
 * chosen from that list -- never by turning: the dial stops at the edge of
 * the one the receiver is on -- and never within OWRX_SWITCH_GAP_MS of the
 * last, or of the session's start (OpenWebRX+ bans quick switchers for twelve
 * hours). A band another listener chooses is followed, the dial moved onto
 * it by the receiver's own rule (owrx_retune).
 *
 * The task's stack is in PSRAM, so it never touches flash: the dial, the
 * mode and the passband (per receiver), the squelch and the receiver in use
 * are saved by a timer, at a quiet moment -- a session over, the squelch
 * closed a second -- or 30 s after the last change. */
#include "owrx.h"
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
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "kiwi_proto.h"
#include "net_prov.h"
#include "kvstore.h"
#include "owrx_proto.h"
#include "owrx_sess.h"
#include "vfo_tune.h"

static const char *TAG = "owrx";

#define NVS_NS          "vfo"
#define LOOP_MS         20                     /* the knob's detents: tune_apply's pace */
#define F_LOW           10000LL
#define F_TOP           2000000000LL           /* before a receiver has said where it is */
#define DEFAULT_HZ      7100000LL              /* 40 m, LSB */
#define FAIR_TRIES      2                      /* the one in use, not reached: tries before another */
#define STREAM_OK_US    (30 * 1000000LL)       /* streamed this long: its waits start over */
#define SAVE_QUIET_US   (2 * 1000000LL)
#define SAVE_LATER_US   (30 * 1000000LL)
#define SQ_QUIET_US     (1000000LL)            /* the squelch closed this long: a quiet moment */
#define NO_SOCKET_MS    2000                   /* the knob's own sockets all taken: soon again */
#define FOREVER         0
#define N_RX            NET_PROV_RADIOS

static portMUX_TYPE S_LOCK = portMUX_INITIALIZER_UNLOCKED;
static struct {
    radio_link_t link;
    char     why[16];
    tune_t   tune;
    int64_t  f_min, f_max, f_sent;
    char     mode[8];
    int32_t  lo, hi;                /* the passband, around the dial */
    uint8_t  sq;                    /* the squelch, 0-100 %: 0 open */
    float    db;                    /* the S-meter, its dB */
    bool     have_db, shut;         /* ...under the squelch */
    int64_t  shut_since;
    bool     sq_told;               /* ...closed long enough, and the quiet moment taken */
    float    m_lo, m_hi;            /* the meter's scale */
    bool     plus;
    uint32_t want;                  /* the receiver in use, by its key */
    uint32_t at;                    /* the one tried now: the one in use, or a stand-in */
    uint32_t chosen, chosen_key;    /* the operator's acts, and the last one's receiver */
    uint32_t run_key;               /* the session's */
    uint32_t band_seq;
    char     band[OWRX_ID_MAX];     /* the band asked for */
    int64_t  t_band, t_conn;        /* the last asked for; the session's greeting */
    int      band_sel;              /* in s_bands; -1 */
    char     band_name[48];
    int      users, users_max;
    char     note[16];
    uint32_t note_seq;
    bool     fresh;                 /* nothing kept for this receiver: its band's own start */
    bool     suspend;
    uint32_t connects, closes;
    bool     dirty, use_dirty;      /* settings, and the one in use, not saved yet */
} S = { .link = RADIO_LINK_DOWN, .f_min = F_LOW, .f_max = F_TOP, .m_lo = -108.0f, .m_hi = 0.0f,
        .band_sel = -1, .users = -1, .users_max = -1 };

/* The list's receivers by their keys, as of a list generation. */
static uint32_t s_keys[N_RX];
static int      s_keys_n = -1;
static uint32_t s_keys_gen;

/* Each receiver as the knob knows it this boot, by its key. */
typedef struct {
    uint32_t   key;
    owrx_end_t end;                 /* its last session's end, NONE none */
    uint8_t    tries;               /* ...in a row */
    int64_t    not_before;
    bool       held;                /* until chosen again -- or the list saved, but for a ban */
    uint32_t   gen;                 /* the list's, when it was held */
    uint16_t   tls_port;            /* its redirect to https:// on its own host, followed */
    bool       looked;              /* its status.json read, or tried */
    bool       ok;                  /* ...and it answered as OpenWebRX */
    char       name[48], version[24];
} rx_t;

/* The dial, the mode and the passband each receiver was left on. */
typedef struct {
    uint32_t key;
    int64_t  hz;
    char     mod[8];
    int32_t  lo, hi;
} kept_t;

EXT_RAM_BSS_ATTR static rx_t           s_rx[N_RX];
EXT_RAM_BSS_ATTR static kept_t         s_kept[N_RX];
EXT_RAM_BSS_ATTR static owrx_said_t    s_said;      /* the session's, as it goes */
EXT_RAM_BSS_ATTR static owrx_said_t    s_said_j;    /* ...its bands, joined to the status */
EXT_RAM_BSS_ATTR static owrx_status_t  s_st;        /* the status.json of... */
static uint32_t                        s_st_key;    /* ...this receiver */
EXT_RAM_BSS_ATTR static owrx_band_info_t s_bands[OWRX_BANDS];
EXT_RAM_BSS_ATTR static char           s_band_ids[OWRX_BANDS][OWRX_ID_MAX];   /* ...each one's, to ask for it */
static int                             s_nb;
static uint32_t                        s_bands_seq;
EXT_RAM_BSS_ATTR static owrx_counts_t  s_counts;
EXT_RAM_BSS_ATTR static uint32_t       s_lost;      /* feeds a full ring let go */
EXT_RAM_BSS_ATTR static char           s_state[48];
EXT_RAM_BSS_ATTR static char           s_last_close[48];
EXT_RAM_BSS_ATTR static char           s_name[48], s_version[24];   /* the one in use's, for the face */
static bool                            s_tls;       /* the session's link: WSS */

static esp_timer_handle_t s_save_t;
static int64_t            s_t_stream;
static int64_t            s_t_change;

/* ------------------------------------------------------------ the list */

static uint32_t key_of(const net_radio_t *r)
{
    owrx_url_t u;
    return r->host[0] && owrx_url(r->host, &u) ? owrx_key(&u) : 0;
}

/* Each receiver's key, fresh for the list as it is now. A list saved since
 * the last look: the one it has in use is the one in use -- the operator's
 * choice when it is another than before. Any task: the face's, the page's
 * and the session's each read the list into a copy of their own, and one
 * that read it before a save never puts its keys over those of one that
 * read it after. */
static void keys_fresh(void)
{
    const uint32_t g = net_prov_radios_gen();
    taskENTER_CRITICAL(&S_LOCK);
    const bool ok = s_keys_n >= 0 && s_keys_gen == g;
    taskEXIT_CRITICAL(&S_LOCK);
    if (ok) return;
    net_radio_t r;
    uint32_t k[N_RX] = { 0 };
    int n = net_prov_radio_count();
    if (n > N_RX) n = N_RX;
    for (int i = 0; i < n; i++)
        if (net_prov_radio_get(i, &r)) k[i] = key_of(&r);
    const int a = net_prov_radio_active();
    taskENTER_CRITICAL(&S_LOCK);
    const bool first = s_keys_n < 0;
    if (!first && (s_keys_gen == g || (int32_t)(s_keys_gen - g) > 0)) {
        taskEXIT_CRITICAL(&S_LOCK);
        return;                                 /* another's look, as fresh or fresher */
    }
    memcpy(s_keys, k, sizeof s_keys);
    s_keys_n = n;
    s_keys_gen = g;
    if (!first && a >= 0 && a < n && k[a] && k[a] != S.want) {
        S.want = S.at = k[a];
        S.chosen++;
        S.chosen_key = k[a];
        /* On the way to it from now: never the last one's state under its
         * name. */
        S.link = RADIO_LINK_CONNECTING;
        S.why[0] = 0;
        strlcpy(s_state, "connecting", sizeof s_state);
    }
    taskEXIT_CRITICAL(&S_LOCK);
}

/* Its place in the list, -1 gone. Under S_LOCK. */
static int place_of(uint32_t key)
{
    for (int i = 0; key && i < s_keys_n; i++)
        if (s_keys[i] == key) return i;
    return -1;
}

static bool listed(uint32_t key)
{
    keys_fresh();
    taskENTER_CRITICAL(&S_LOCK);
    const bool in = place_of(key) >= 0;
    taskEXIT_CRITICAL(&S_LOCK);
    return in;
}

int owrx_rx_active(void)
{
    keys_fresh();
    taskENTER_CRITICAL(&S_LOCK);
    const int i = place_of(S.want);
    taskEXIT_CRITICAL(&S_LOCK);
    return i;
}

static rx_t *rx_find(uint32_t key)
{
    for (int i = 0; key && i < N_RX; i++)
        if (s_rx[i].key == key) return &s_rx[i];
    return NULL;
}

/* Its place: an empty one, else one of a receiver no longer in the list. */
static rx_t *rx_get(uint32_t key)
{
    rx_t *x = rx_find(key);
    if (x) return x;
    for (int i = 0; !x && i < N_RX; i++)
        if (!s_rx[i].key) x = &s_rx[i];
    for (int i = 0; !x && i < N_RX; i++)
        if (!listed(s_rx[i].key)) x = &s_rx[i];
    if (!x) x = &s_rx[0];
    taskENTER_CRITICAL(&S_LOCK);
    memset(x, 0, sizeof *x);
    x->key = key;
    taskEXIT_CRITICAL(&S_LOCK);
    return x;
}

bool owrx_rx_label(int i, char *out, size_t cap)
{
    net_radio_t r;
    if (!out || !cap || !net_prov_radio_get(i, &r)) return false;
    if (r.name[0]) {
        strlcpy(out, r.name, cap);
        return true;
    }
    const uint32_t key = key_of(&r);
    char nm[48] = "";
    taskENTER_CRITICAL(&S_LOCK);
    const rx_t *x = rx_find(key);
    if (x && x->name[0]) strlcpy(nm, x->name, sizeof nm);
    taskEXIT_CRITICAL(&S_LOCK);
    if (nm[0]) {
        strlcpy(out, nm, cap);
        return true;
    }
    owrx_url_t u;
    strlcpy(out, r.host[0] && owrx_url(r.host, &u) ? u.host : r.host[0] ? net_prov_host_shown(r.host) : "NO ADDRESS",
            cap);
    return true;
}

/* ------------------------------------------------------------ remembered */

/* Nothing heard now: no session playing, or its squelch closed long enough
 * that the ring holds none of what it let through. Under S_LOCK. */
static bool quiet_locked(int64_t now)
{
    return S.link != RADIO_LINK_READY || (S.shut && now - S.shut_since >= SQ_QUIET_US);
}

/* The dial, the mode and the passband, kept for the receiver at `key`.
 * Under S_LOCK. */
static void keep_locked(uint32_t key)
{
    if (!key) return;
    kept_t *k = NULL;
    for (int i = 0; !k && i < N_RX; i++)
        if (s_kept[i].key == key) k = &s_kept[i];
    for (int i = 0; !k && i < N_RX; i++)
        if (!s_kept[i].key || place_of(s_kept[i].key) < 0) k = &s_kept[i];
    if (!k) k = &s_kept[0];
    k->key = key;
    k->hz = S.tune.f_display;
    strlcpy(k->mod, S.mode, sizeof k->mod);
    k->lo = S.lo;
    k->hi = S.hi;
}

/* ...and back on it, where it was kept: the receiver's span puts it right
 * if its band has moved since (owrx_retune). False, the dial as it is, for
 * a receiver nothing was kept for. Under S_LOCK. */
static bool unkeep_locked(uint32_t key)
{
    for (int i = 0; key && i < N_RX; i++) {
        const kept_t *k = &s_kept[i];
        if (k->key != key || k->hz < F_LOW) continue;
        tune_assign(&S.tune, k->hz);
        if (owrx_mode_find(k->mod)) strlcpy(S.mode, k->mod, sizeof S.mode);
        if (k->lo < k->hi) {
            S.lo = k->lo;
            S.hi = k->hi;
        }
        return true;
    }
    return false;
}

static void save_cb(void *arg)
{
    (void)arg;
    taskENTER_CRITICAL(&S_LOCK);
    const int64_t now = esp_timer_get_time(), since = now - s_t_change;
    if (!quiet_locked(now) && since < SAVE_LATER_US) {
        taskEXIT_CRITICAL(&S_LOCK);
        esp_timer_start_once(s_save_t, (uint64_t)(SAVE_LATER_US - since));
        return;
    }
    keep_locked(S.at ? S.at : S.want);
    EXT_RAM_BSS_ATTR static kept_t kept[N_RX];
    memcpy(kept, s_kept, sizeof kept);
    const uint8_t sq = S.sq;
    const int use = S.use_dirty ? place_of(S.want) : -1;
    S.dirty = S.use_dirty = false;
    taskEXIT_CRITICAL(&S_LOCK);
    kv_handle_t h;
    if (kv_open(NVS_NS, &h) != ESP_OK) return;
    kv_edit_begin(h);
    kv_set_blob(h, "owt", kept, sizeof kept);
    kv_set_u8(h, "owq", sq);
    kv_edit_end(h);
    kv_commit(h);
    kv_close(h);
    /* The one in use, as the list keeps it: where the knob starts next. */
    if (use >= 0 && use != net_prov_radio_active()) {
        const esp_err_t e = net_prov_radio_activate(use);
        if (e != ESP_OK) ESP_LOGE(TAG, "receiver %d in use, not saved: %s", use, esp_err_to_name(e));
    }
}

/* A setting changed: into flash at a quiet moment -- soon with nothing
 * playing; while it plays 30 s after the last change at the latest, since a
 * flash write holds the audio's interrupts up a moment. */
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

static void save_now(void)
{
    taskENTER_CRITICAL(&S_LOCK);
    const bool dirty = S.dirty || S.use_dirty;
    taskEXIT_CRITICAL(&S_LOCK);
    if (!dirty || !s_save_t) return;
    esp_timer_stop(s_save_t);
    esp_timer_start_once(s_save_t, 1000);
}

static void load(void)
{
    uint8_t sq = 0;
    size_t n = sizeof s_kept;
    kv_handle_t h;
    if (kv_open(NVS_NS, &h) == ESP_OK) {
        if (kv_get_blob(h, "owt", s_kept, &n) != ESP_OK || n != sizeof s_kept) memset(s_kept, 0, sizeof s_kept);
        kv_get_u8(h, "owq", &sq);
        kv_close(h);
    }
    keys_fresh();
    const int a = net_prov_radio_active();
    taskENTER_CRITICAL(&S_LOCK);
    tune_init(&S.tune, DEFAULT_HZ, 1000);
    strlcpy(S.mode, "lsb", sizeof S.mode);
    owrx_passband(owrx_mode_find("lsb"), false, &S.lo, &S.hi);
    S.sq = sq <= 100 ? sq : 0;
    /* The list's own in use, else the first with an address. */
    if (a >= 0 && a < s_keys_n && s_keys[a]) S.want = s_keys[a];
    for (int i = 0; !S.want && i < s_keys_n; i++) S.want = s_keys[i];
    S.at = S.want;
    S.fresh = !unkeep_locked(S.want);
    const int i = place_of(S.want);
    taskEXIT_CRITICAL(&S_LOCK);
    ESP_LOGI(TAG, "%lld Hz %s %ld..%ld, squelch %u%%; %d receiver%s, %s", (long long)S.tune.f_display, S.mode,
             (long)S.lo, (long)S.hi, (unsigned)S.sq, s_keys_n, s_keys_n == 1 ? "" : "s",
             i >= 0 ? "one in use" : "none in use");
}

/* ------------------------------------------------------------ the face */

static void set_link(radio_link_t l, const char *why, const char *state)
{
    taskENTER_CRITICAL(&S_LOCK);
    S.link = l;
    strlcpy(S.why, why ? why : "", sizeof S.why);
    if (state) strlcpy(s_state, state, sizeof s_state);
    if (l != RADIO_LINK_READY) S.have_db = false;
    taskEXIT_CRITICAL(&S_LOCK);
}

/* The pages' word for an end: the face's, in lower case. */
static void note_of(owrx_end_t e, char *out, size_t cap)
{
    const char *w = owrx_end_word(e);
    size_t i = 0;
    for (; w[i] && i + 1 < cap; i++) out[i] = (char)(w[i] >= 'A' && w[i] <= 'Z' ? w[i] + 32 : w[i]);
    out[i] = 0;
}

static void down(owrx_end_t e, const char *state)
{
    char n[24];
    note_of(e, n, sizeof n);
    set_link(RADIO_LINK_DOWN, owrx_end_word(e), state && state[0] ? state : n);
}

/* The bands from the session's last word, joined to the receiver's
 * status.json where it is the same receiver's: for the BAND list. */
static void bands_from(const owrx_said_t *said, uint32_t key)
{
    s_said_j = *said;
    int placed = 0;
    if (s_st_key == key && s_st.ok) placed = owrx_join(&s_said_j, &s_st);
    EXT_RAM_BSS_ATTR static owrx_band_info_t b[OWRX_BANDS];
    const int n = s_said_j.n_bands;
    for (int i = 0; i < n; i++) {
        const owrx_band_t *o = &s_said_j.bands[i];
        const size_t l = o->label < sizeof o->name ? o->label : 0;
        strlcpy(b[i].name, o->name + l, sizeof b[i].name);
        snprintf(b[i].sdr, sizeof b[i].sdr, "%.*s", (int)(l > 0 ? l - 1 : 0), o->name);
        b[i].lo = o->center > 0 && o->rate > 0 ? o->center - o->rate / 2 : 0;
        b[i].hi = o->center > 0 && o->rate > 0 ? o->center + o->rate / 2 : 0;
    }
    const int sel = owrx_band_now(&s_said_j);
    taskENTER_CRITICAL(&S_LOCK);
    memcpy(s_bands, b, sizeof s_bands);
    for (int i = 0; i < n; i++) strlcpy(s_band_ids[i], s_said_j.bands[i].id, sizeof s_band_ids[i]);
    s_nb = n;
    s_bands_seq++;
    S.band_sel = sel;
    /* Its config may have come before its list: the band's name from it. */
    if (sel >= 0) strlcpy(S.band_name, b[sel].name, sizeof S.band_name);
    taskEXIT_CRITICAL(&S_LOCK);
    ESP_LOGI(TAG, "%d bands%s, %d placed by its status.json", n,
             said->bands_seen > n ? " kept (more not)" : "", placed);
}

/* ------------------------------------------------------------ status.json */

/* The receiver whose status.json is being read (ctx: its key) is still the
 * one wanted: another chosen meanwhile is not kept waiting. */
static bool still_wanted(void *ctx)
{
    taskENTER_CRITICAL(&S_LOCK);
    const bool w = S.at == (uint32_t)(uintptr_t)ctx;
    taskEXIT_CRITICAL(&S_LOCK);
    return w;
}

/* Its status.json, once a boot: its name, its version, its bands' places.
 * NONE when it answered (as OpenWebRX or not: the session says which), else
 * why not -- WANT when another receiver was chosen meanwhile. */
static owrx_end_t look(rx_t *x, owrx_url_t *u)
{
    uint16_t tp = 0;
    const owrx_end_t e = owrx_status_read(u, x->key, &s_st, &tp, still_wanted, (void *)(uintptr_t)x->key, TAG);
    if (tp && !u->tls) {
        ESP_LOGI(TAG, "%s: on https:// from now on, port %u", u->host, (unsigned)tp);
        x->tls_port = tp;
        u->tls = true;
        u->port = tp;
    }
    if (e == OWRX_END_WANT) return e;
    taskENTER_CRITICAL(&S_LOCK);
    x->looked = true;
    if (e == OWRX_END_NONE && s_st.ok) {
        x->ok = true;
        strlcpy(x->name, s_st.name, sizeof x->name);
        strlcpy(x->version, s_st.version, sizeof x->version);
    }
    taskEXIT_CRITICAL(&S_LOCK);
    s_st_key = e == OWRX_END_NONE && s_st.ok ? x->key : 0;
    if (e != OWRX_END_NONE)
        ESP_LOGW(TAG, "%s:%u%s: no status.json (%s)", u->host, (unsigned)u->port, u->path, owrx_end_word(e));
    else if (!s_st.ok)
        ESP_LOGW(TAG, "%s:%u%s: its status.json is not OpenWebRX's", u->host, (unsigned)u->port, u->path);
    else
        ESP_LOGI(TAG, "%s:%u%s: %s, %s %s; %u SDR%s, %u bands; %d listeners at most", u->host, (unsigned)u->port,
                 u->path, s_st.name, owrx_plus_version(s_st.version) ? "OpenWebRX+" : "OpenWebRX", s_st.version,
                 (unsigned)s_st.n_sdrs, s_st.n_sdrs == 1 ? "" : "s", (unsigned)s_st.profiles, s_st.max_clients);
    return e;
}

/* ------------------------------------------------------------ the session's side */

static void cb_ctl(void *ctx, owrx_ctl_t *out)
{
    (void)ctx;
    taskENTER_CRITICAL(&S_LOCK);
    out->t.hz = S.tune.f_display;
    strlcpy(out->t.mod, S.mode, sizeof out->t.mod);
    out->t.lo = S.lo;
    out->t.hi = S.hi;
    out->t.sq = -150;
    out->sq_pct = S.sq;
    out->band_seq = S.band_seq;
    strlcpy(out->band, S.band, sizeof out->band);
    S.f_sent = S.tune.f_display;
    taskEXIT_CRITICAL(&S_LOCK);
}

/* Where it listens has changed: the dial kept on the span, or moved to the
 * band's own start, in its mode (owrx_retune); the dial stops at the span's
 * edges from now on. */
static void cb_span(void *ctx, const owrx_said_t *said)
{
    (void)ctx;
    float lo, hi;
    owrx_meter_range(said, &lo, &hi);
    owrx_tune_t t = { 0 };
    taskENTER_CRITICAL(&S_LOCK);
    /* A receiver the knob has kept nothing for starts where its own page
     * starts a listener: the band's start, in its mode. */
    t.hz = S.fresh ? 0 : S.tune.f_display;
    S.fresh = false;
    strlcpy(t.mod, S.mode, sizeof t.mod);
    t.lo = S.lo;
    t.hi = S.hi;
    taskEXIT_CRITICAL(&S_LOCK);
    const bool moved = owrx_retune(said, &t);
    const int sel = owrx_band_now(said);
    char name[48] = "";
    taskENTER_CRITICAL(&S_LOCK);
    /* By its own name, as the list joined to its status.json has it -- the
     * session's says the SDR's first. */
    if (sel >= 0 && sel < s_nb) strlcpy(name, s_bands[sel].name, sizeof name);
    else if (sel >= 0)          strlcpy(name, said->bands[sel].name, sizeof name);
    S.f_min = said->center - said->rate / 2;
    S.f_max = said->center + said->rate / 2;
    if (moved) {
        tune_assign(&S.tune, t.hz);
        strlcpy(S.mode, t.mod, sizeof S.mode);
        S.lo = t.lo;
        S.hi = t.hi;
    }
    S.m_lo = lo;
    S.m_hi = hi;
    S.plus = said->plus;
    S.band_sel = sel;
    strlcpy(S.band_name, name, sizeof S.band_name);
    taskEXIT_CRITICAL(&S_LOCK);
    ESP_LOGI(TAG, "on %s%s%lld +- %ld Hz; the dial %s %lld Hz %s", name, name[0] ? ", " : "",
             (long long)said->center, (long)(said->rate / 2), moved ? "moved to" : "kept at", (long long)t.hz, t.mod);
    if (moved) changed();
}

static void cb_told(void *ctx, uint32_t ev, const owrx_said_t *said)
{
    (void)ctx;
    taskENTER_CRITICAL(&S_LOCK);
    const uint32_t key = S.run_key;
    if (ev & OWRX_EV_CLIENTS) S.users = said->clients;
    if (ev & OWRX_EV_CONFIG) {
        S.users_max = said->max_clients;
        S.plus = said->plus;
    }
    if (ev & (OWRX_EV_NAME | OWRX_EV_HELLO)) {
        if (said->name[0]) strlcpy(s_name, said->name, sizeof s_name);
        if (said->version[0]) strlcpy(s_version, said->version, sizeof s_version);
        rx_t *x = rx_find(key);
        if (x && said->name[0]) strlcpy(x->name, said->name, sizeof x->name);
        if (x && said->version[0]) strlcpy(x->version, said->version, sizeof x->version);
    }
    /* A band refused: OpenWebRX+'s "This profile is locked". */
    if ((ev & OWRX_EV_LOG) && strstr(said->log, "locked")) {
        strlcpy(S.note, "BAND LOCKED", sizeof S.note);
        S.note_seq++;
    }
    taskEXIT_CRITICAL(&S_LOCK);
    if (ev & OWRX_EV_BANDS) bands_from(said, key);
    if (ev & OWRX_EV_LOG) ESP_LOGI(TAG, "it says: %s", said->log);
}

static void cb_audio(void *ctx, const int16_t *pcm, size_t n)
{
    (void)ctx;
    /* A full ring lets the rest go: lost audio, counted as such. */
    if (!S.suspend && !audio_out_feed_pcm16(pcm, n, 1)) s_lost++;
}

static size_t cb_queued(void *ctx)
{
    (void)ctx;
    return audio_out_queued();
}

static void cb_preroll(void *ctx, size_t n)
{
    (void)ctx;
    audio_out_set_preroll(n);
}

static uint32_t cb_underruns(void *ctx)
{
    (void)ctx;
    audio_stats_t a;
    audio_out_stats(&a);
    return a.underruns;
}

static void cb_meter(void *ctx, float db)
{
    (void)ctx;
    const int64_t now = esp_timer_get_time();
    taskENTER_CRITICAL(&S_LOCK);
    const float sq = S.sq ? S.m_lo + (S.m_hi - S.m_lo) * (float)S.sq / 100.0f : -150.0f;
    const bool shut = S.sq && db < sq;
    if (shut && !S.shut) S.shut_since = now;
    S.db = db;
    S.have_db = true;
    S.shut = shut;
    const bool quiet = shut && !S.sq_told && quiet_locked(now);
    if (quiet || !shut) S.sq_told = quiet;
    taskEXIT_CRITICAL(&S_LOCK);
    if (quiet) save_now();
}

EXT_RAM_BSS_ATTR static uint32_t s_rep_lost, s_rep_under;

static void report_from_now(void)
{
    audio_stats_t a;
    audio_out_stats(&a);
    s_rep_lost  = s_counts.lost + s_lost;
    s_rep_under = a.underruns;
}

/* Every 30 s while it plays: how the audio came, and what the ring made of
 * it. */
static void cb_report(void *ctx, const wl_report_t *r)
{
    (void)ctx;
    audio_stats_t a;
    audio_out_stats(&a);
    const uint32_t lost = s_counts.lost + s_lost;
    ESP_LOGI(TAG, "ring %lu ms of %lu, trim %.5f, blocks %lu, lost %lu, underruns %lu, breaks %lu, jumps %lu "
             "(%lu ms left out); gap %lu ms, held %lu; free internal %u",
             (unsigned long)r->level_ms, (unsigned long)r->target_ms, (double)r->trim, (unsigned long)r->blocks,
             (unsigned long)(lost - s_rep_lost), (unsigned long)(a.underruns - s_rep_under),
             (unsigned long)r->breaks, (unsigned long)r->jumps, (unsigned long)r->left_ms,
             (unsigned long)r->gap_ms, (unsigned long)r->held,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    s_rep_lost  = lost;
    s_rep_under = a.underruns;
}

static void cb_state(void *ctx, owrx_state_t st, const owrx_said_t *said)
{
    (void)ctx;
    (void)said;
    if (st == OWRX_ST_CONNECTING) {
        /* No warning while it connects: the slab says so. */
        set_link(RADIO_LINK_CONNECTING, NULL, "connecting");
        return;
    }
    if (st == OWRX_ST_CONNECTED) {
        taskENTER_CRITICAL(&S_LOCK);
        S.t_conn = esp_timer_get_time();
        taskEXIT_CRITICAL(&S_LOCK);
        set_link(RADIO_LINK_GREETING, NULL, "connected");
        return;
    }
    /* Playing: a stand-in that plays is the one in use from now on. */
    s_t_stream = esp_timer_get_time();
    report_from_now();
    bool took = false;
    taskENTER_CRITICAL(&S_LOCK);
    if (S.run_key && S.run_key != S.want) {
        S.want = S.run_key;
        S.use_dirty = took = true;
    }
    taskEXIT_CRITICAL(&S_LOCK);
    set_link(RADIO_LINK_READY, NULL, "streaming");
    if (took) {
        ESP_LOGI(TAG, "a stand-in plays: in use from now on");
        changed();
    }
}

/* On, until another receiver is chosen, or the list saved under it lost it. */
static bool cb_go_on(void *ctx)
{
    (void)ctx;
    keys_fresh();
    taskENTER_CRITICAL(&S_LOCK);
    const bool on = S.at == S.run_key && place_of(S.run_key) >= 0;
    taskEXIT_CRITICAL(&S_LOCK);
    return on;
}

/* Its redirect to https:// on its own host, followed: kept for this boot,
 * under its key. */
static void cb_moved(void *ctx, uint16_t tls_port)
{
    (void)ctx;
    taskENTER_CRITICAL(&S_LOCK);
    rx_t *x = rx_find(S.run_key);
    if (x) x->tls_port = tls_port;
    taskEXIT_CRITICAL(&S_LOCK);
}

static const owrx_link_t LINK = {
    .ctl = cb_ctl, .span = cb_span, .told = cb_told, .audio = cb_audio,
    .ring = { .queued = cb_queued, .trim = true, .preroll = cb_preroll, .underruns = cb_underruns, .tag = "owrx" },
    .meter = cb_meter, .state = cb_state, .go_on = cb_go_on, .report = cb_report, .moved = cb_moved,
    .counts = &s_counts, .tag = "owrx",
};

/* ------------------------------------------------------------ the task */

/* Up to `ms` (FOREVER: for as long as it takes), or until the operator does
 * something -- another receiver, any act of choosing -- or the list is
 * saved. True when one came. */
static bool wait_event(uint32_t ms, uint32_t at, uint32_t ch, uint32_t gen)
{
    for (uint32_t t = 0; ms == FOREVER || t < ms; t += 100) {
        vTaskDelay(pdMS_TO_TICKS(100));
        taskENTER_CRITICAL(&S_LOCK);
        const bool ev = S.at != at || S.chosen != ch;
        taskEXIT_CRITICAL(&S_LOCK);
        if (ev || net_prov_radios_gen() != gen) return true;
    }
    return false;
}

/* The next receiver after `key` in the list, in turn, wrapping round, that
 * has an address and is not held: the one tried from now. False when there
 * is none but this one. */
static bool hand_over(uint32_t key)
{
    keys_fresh();
    taskENTER_CRITICAL(&S_LOCK);
    const int n = s_keys_n, from = place_of(key);
    uint32_t keys[N_RX];
    memcpy(keys, s_keys, sizeof keys);
    taskEXIT_CRITICAL(&S_LOCK);
    for (int k = 1; k < n; k++) {
        const uint32_t c = keys[((from < 0 ? 0 : from) + k) % n];
        if (!c || c == key) continue;
        const rx_t *x = rx_find(c);
        if (x && x->held) continue;
        taskENTER_CRITICAL(&S_LOCK);
        const bool same = S.at == key;      /* the operator has not gone elsewhere meanwhile */
        if (same) S.at = c;
        taskEXIT_CRITICAL(&S_LOCK);
        return same;
    }
    return false;
}

/* The receiver to try now -- the one tried last, the one in use, else the
 * first with an address -- its place in the list, -1 with none. */
static int pick(uint32_t *key)
{
    keys_fresh();
    taskENTER_CRITICAL(&S_LOCK);
    if (place_of(S.want) < 0) {
        S.want = 0;
        for (int i = 0; !S.want && i < s_keys_n; i++) S.want = s_keys[i];
        S.use_dirty = S.want != 0;
    }
    if (place_of(S.at) < 0) S.at = S.want;
    const int i = place_of(S.at);
    *key = i >= 0 ? S.at : 0;
    taskEXIT_CRITICAL(&S_LOCK);
    return i;
}

static bool moved_from(uint32_t key)
{
    taskENTER_CRITICAL(&S_LOCK);
    const bool m = S.at != key;
    taskEXIT_CRITICAL(&S_LOCK);
    return m;
}

/* Not reached at all, as against an answer that refused. */
static bool unreached(owrx_end_t e)
{
    return e == OWRX_END_NOT_FOUND || e == OWRX_END_NO_ROUTE || e == OWRX_END_NO_ANSWER || e == OWRX_END_QUIET ||
           e == OWRX_END_DOWN || e == OWRX_END_PROTOCOL || e == OWRX_END_CLOSED;
}

/* How a try at `x` ended (not WANT): marked -- its wait, or held -- and the
 * next to try chosen. */
static void ended(rx_t *x, owrx_end_t e, bool streamed, uint32_t gen)
{
    taskENTER_CRITICAL(&S_LOCK);
    const bool in_use = x->key == S.want;
    taskEXIT_CRITICAL(&S_LOCK);
    if (streamed) x->tries = 0;
    if (x->tries < 255) x->tries++;
    x->end = e;
    const uint32_t ms = owrx_retry_ms(e, x->tries);
    if (!ms) {
        x->held = true;
        x->gen = gen;
    } else {
        const uint32_t jit = esp_random() % (ms / 4 + 1);
        x->not_before = esp_timer_get_time() + (int64_t)(ms + jit) * 1000;
    }
    /* Another's turn: one held; a stand-in that did not play; the one in use
     * not reached, after its fair chance. The one in use that refused (full,
     * no SDR) keeps its turn. */
    const bool next = x->held || !in_use || (unreached(e) && !streamed && x->tries >= FAIR_TRIES);
    char w[24];
    note_of(e, w, sizeof w);
    if (next && hand_over(x->key)) {
        ESP_LOGW(TAG, "%s -- on to the next receiver", w);
        return;
    }
    if (x->held) ESP_LOGW(TAG, "%s -- not again until chosen again", w);
    else         ESP_LOGW(TAG, "%s -- again in %lu s", w, (unsigned long)((ms + 999) / 1000));
}

static void task(void *arg)
{
    (void)arg;
    owrx_sess_t *ss = owrx_sess_new();
    if (!ss) {
        ESP_LOGE(TAG, "no memory for the receiver's buffers");
        set_link(RADIO_LINK_DOWN, "NO MEMORY", "no memory");
        vTaskDeleteWithCaps(NULL);
    }
    owrx_link_t link = LINK;
    link.ring.target     = audio_out_preroll();
    link.ring.room       = audio_out_room();
    link.ring.target_max = audio_out_preroll_max();
    uint32_t seen = 0, last = 0;

    for (;;) {
        const uint32_t gen = net_prov_radios_gen();
        uint32_t key;
        const int i = pick(&key);
        EXT_RAM_BSS_ATTR static net_radio_t r;
        if (i < 0 || !net_prov_radio_get(i, &r)) {
            set_link(RADIO_LINK_DOWN, "NO RECEIVER", "no receiver");
            while (net_prov_radios_gen() == gen) vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        owrx_url_t u;
        if (!owrx_url(r.host, &u)) {                /* pick() takes none without an address */
            set_link(RADIO_LINK_DOWN, "NO RECEIVER", "no address");
            while (net_prov_radios_gen() == gen) vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        /* The operator's act for it since the last look: an attempt now,
         * through whatever held it. */
        taskENTER_CRITICAL(&S_LOCK);
        const bool chosen = S.chosen != seen && S.chosen_key == key;
        seen = S.chosen;
        const uint32_t ch = S.chosen;
        taskEXIT_CRITICAL(&S_LOCK);
        rx_t *x = rx_get(key);
        if (chosen) {
            x->held = false;
            x->tries = 0;
            x->not_before = 0;
        }
        /* A list saved lets go of what a new address may cure; never a ban. */
        if (x->held && x->gen != gen && x->end != OWRX_END_BANNED) x->held = false;
        if (key != last) {
            /* Another receiver: the dial it was left on there. */
            taskENTER_CRITICAL(&S_LOCK);
            keep_locked(last);
            S.fresh = !unkeep_locked(key);
            S.users = S.users_max = -1;
            S.band_sel = -1;
            S.band_name[0] = 0;
            s_name[0] = s_version[0] = 0;
            s_nb = 0;
            s_bands_seq++;
            S.f_min = F_LOW;
            S.f_max = F_TOP;
            taskEXIT_CRITICAL(&S_LOCK);
            save_now();
            last = key;
        }
        taskENTER_CRITICAL(&S_LOCK);
        if (x->name[0]) strlcpy(s_name, x->name, sizeof s_name);
        if (x->version[0]) strlcpy(s_version, x->version, sizeof s_version);
        taskEXIT_CRITICAL(&S_LOCK);
        if (x->held) {
            down(x->end, NULL);
            wait_event(FOREVER, key, ch, gen);
            continue;
        }
        const int64_t wait = x->not_before - esp_timer_get_time();
        if (wait > 0) {
            char st[48], w[16];
            note_of(x->end, w, sizeof w);
            snprintf(st, sizeof st, "%.15s, again in %lld s", w, (long long)((wait + 999999) / 1000000));
            down(x->end, st);
            wait_event((uint32_t)(wait / 1000) + 1, key, ch, gen);
            continue;
        }
        if (x->tls_port) {
            u.tls = true;
            u.port = x->tls_port;
        }

        /* Its status.json, once a boot, before its first session. One not
         * reached is a session not reached. */
        set_link(RADIO_LINK_CONNECTING, NULL, "connecting");
        if (!x->looked) {
            const owrx_end_t e = look(x, &u);
            if (e == OWRX_END_WANT || moved_from(key)) continue;
            if (e == OWRX_END_NO_SOCKET) {
                vTaskDelay(pdMS_TO_TICKS(NO_SOCKET_MS));
                continue;
            }
            if (e != OWRX_END_NONE && e != OWRX_END_NOT_OWRX) {
                x->looked = false;                  /* read again when it is reached */
                down(e, NULL);
                ended(x, e, false, gen);
                continue;
            }
        }

        taskENTER_CRITICAL(&S_LOCK);
        S.run_key = key;
        S.connects++;
        S.shut = S.sq_told = false;
        S.t_conn = 0;
        s_tls = u.tls;
        taskEXIT_CRITICAL(&S_LOCK);
        audio_out_flush();
        s_t_stream = 0;
        const owrx_end_t end = owrx_sess_run(ss, &u, key, &link, &s_said);
        const bool streamed = s_t_stream && esp_timer_get_time() - s_t_stream >= STREAM_OK_US;
        audio_out_flush();
        taskENTER_CRITICAL(&S_LOCK);
        S.run_key = 0;
        S.closes++;
        if (S.chosen_key == key) seen = S.chosen;    /* chosen while it played: as it was */
        taskEXIT_CRITICAL(&S_LOCK);
        snprintf(s_last_close, sizeof s_last_close, "%s%s%.38s", owrx_end_word(end),
                 s_said.reason[0] ? ": " : "", s_said.reason);
        save_now();
        if (end == OWRX_END_WANT) {
            set_link(RADIO_LINK_CONNECTING, NULL, "connecting");
            continue;
        }
        if (end == OWRX_END_NO_SOCKET) {
            down(end, NULL);
            vTaskDelay(pdMS_TO_TICKS(NO_SOCKET_MS));
            continue;
        }
        down(end, s_said.reason[0] ? s_said.reason : NULL);
        ended(x, end, streamed, gen);
    }
}

/* ------------------------------------------------------------- owrx.h */

bool owrx_rx_use(int i, bool chosen)
{
    keys_fresh();
    bool moved = false, ok = false;
    taskENTER_CRITICAL(&S_LOCK);
    if (i >= 0 && i < s_keys_n && s_keys[i]) {
        ok = true;
        moved = S.want != s_keys[i] || S.at != s_keys[i];
        S.use_dirty |= S.want != s_keys[i];
        S.want = S.at = s_keys[i];
        if (moved) {
            /* On the way to it from now: never the last one's state under
             * its name. */
            S.link = RADIO_LINK_CONNECTING;
            S.why[0] = 0;
            strlcpy(s_state, "connecting", sizeof s_state);
        }
        if (chosen) {
            S.chosen++;
            S.chosen_key = s_keys[i];
        }
    }
    taskEXIT_CRITICAL(&S_LOCK);
    if (!ok) return false;
    if (moved) changed();
    char l[24];
    if (owrx_rx_label(i, l, sizeof l))
        ESP_LOGI(TAG, "receiver %d (%s)%s%s", i, l, moved ? " in use" : "", !moved && chosen ? " chosen again" : "");
    return true;
}

bool owrx_audible(void)
{
    const int64_t now = esp_timer_get_time();
    taskENTER_CRITICAL(&S_LOCK);
    const bool a = !quiet_locked(now);
    taskEXIT_CRITICAL(&S_LOCK);
    return a;
}

bool owrx_rx_state(int i, owrx_rx_state_t *out)
{
    if (!out) return false;
    memset(out, 0, sizeof *out);
    keys_fresh();
    const int64_t now = esp_timer_get_time();
    taskENTER_CRITICAL(&S_LOCK);
    const uint32_t key = i >= 0 && i < s_keys_n ? s_keys[i] : 0;
    if (!key) {
        taskEXIT_CRITICAL(&S_LOCK);
        return false;
    }
    out->in_use = key == S.want;
    out->playing = key == S.run_key && S.link == RADIO_LINK_READY;
    const rx_t *x = rx_find(key);
    if (x) {
        out->held = x->held;
        if (!x->held && x->not_before > now) out->wait_s = (int)((x->not_before - now + 999999) / 1000000);
        if (out->held || out->wait_s) strlcpy(out->why, owrx_end_word(x->end), sizeof out->why);
        strlcpy(out->name, x->name, sizeof out->name);
        strlcpy(out->version, x->version, sizeof out->version);
        out->tls_port = x->tls_port;
    }
    /* The one the session is on: what its hello said, where its
     * status.json did not. */
    if (key == S.at) {
        if (!out->name[0]) strlcpy(out->name, s_name, sizeof out->name);
        if (!out->version[0]) strlcpy(out->version, s_version, sizeof out->version);
        out->plus = S.plus;
    }
    out->plus |= owrx_plus_version(out->version);
    taskEXIT_CRITICAL(&S_LOCK);
    return true;
}

/* ------------------------------------------------------------- the Test */

/* On a task of its own (owrx_test): its stack in PSRAM, with room for a TLS
 * handshake, which the web server's has not. One at a time, as the web
 * server runs them. Done, it waits to be deleted: a WithCaps task deleting
 * itself has another made, in internal RAM, to free it. */
EXT_RAM_BSS_ATTR static struct {
    owrx_url_t    u;
    owrx_status_t st;
    owrx_end_t    end;
    uint16_t      tls_port;
    volatile bool done;
} s_test;

static void test_task(void *arg)
{
    (void)arg;
    s_test.end = owrx_status_read(&s_test.u, owrx_key(&s_test.u), &s_test.st, &s_test.tls_port, NULL, NULL,
                                  "owrx test");
    s_test.done = true;
    for (;;) vTaskDelay(portMAX_DELAY);
}

bool owrx_test(const char *addr, owrx_test_t *out)
{
    if (!out) return false;
    memset(out, 0, sizeof *out);
    out->max_clients = -1;
    if (!addr || !owrx_url(addr + strspn(addr, " "), &s_test.u)) {
        strlcpy(out->error, "NO ADDRESS", sizeof out->error);
        return false;
    }
    /* One this boot found on https:// on its own host: asked there. */
    const uint32_t key = owrx_key(&s_test.u);
    taskENTER_CRITICAL(&S_LOCK);
    const rx_t *x = rx_find(key);
    const uint16_t tp = x && !s_test.u.tls ? x->tls_port : 0;
    taskEXIT_CRITICAL(&S_LOCK);
    if (tp) {
        s_test.u.tls = true;
        s_test.u.port = tp;
    }
    s_test.tls_port = 0;
    s_test.done = false;
    TaskHandle_t t = NULL;
    if (xTaskCreatePinnedToCoreWithCaps(test_task, "owtest", 16384, NULL, 5, &t, 0, MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGE(TAG, "no memory for the Test's task");
        strlcpy(out->error, "NO MEMORY", sizeof out->error);
        return false;
    }
    while (!s_test.done) vTaskDelay(pdMS_TO_TICKS(50));
    vTaskDeleteWithCaps(t);                 /* its stack and all with it */
    if (s_test.tls_port && !s_test.u.tls) {
        s_test.u.tls = true;
        s_test.u.port = s_test.tls_port;
    }
    if (tp || s_test.tls_port) owrx_url_text(&s_test.u, out->url, sizeof out->url);
    const owrx_status_t *st = &s_test.st;
    if (s_test.end != OWRX_END_NONE || !st->ok) {
        strlcpy(out->error, owrx_end_word(s_test.end != OWRX_END_NONE ? s_test.end : OWRX_END_NOT_OWRX),
                sizeof out->error);
        ESP_LOGW(TAG, "test %s:%u%s: %s", s_test.u.host, (unsigned)s_test.u.port, s_test.u.path, out->error);
        return false;
    }
    out->ok = true;
    strlcpy(out->name, st->name, sizeof out->name);
    strlcpy(out->version, st->version, sizeof out->version);
    out->plus = owrx_plus_version(st->version);
    out->sdrs = st->n_sdrs;
    out->bands = st->profiles;
    out->max_clients = st->max_clients;
    ESP_LOGI(TAG, "test %s:%u%s: %s, %s %s, %d SDRs, %d bands", s_test.u.host, (unsigned)s_test.u.port,
             s_test.u.path, out->name, out->plus ? "OpenWebRX+" : "OpenWebRX", out->version, out->sdrs, out->bands);
    return true;
}

/* Seconds until another band may be asked for; under S_LOCK. */
static int band_wait_locked(int64_t now)
{
    if (S.link != RADIO_LINK_READY || !S.t_conn) return OWRX_SWITCH_GAP_MS / 1000;
    const int64_t from = S.t_band > S.t_conn ? S.t_band : S.t_conn;
    const int64_t left = from + OWRX_SWITCH_GAP_MS * 1000LL - now;
    return left > 0 ? (int)((left + 999999) / 1000000) : 0;
}

void owrx_info(owrx_info_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof *out);
    const int i = owrx_rx_active();
    net_radio_t r;
    const bool have = i >= 0 && net_prov_radio_get(i, &r);
    const int64_t now = esp_timer_get_time();
    taskENTER_CRITICAL(&S_LOCK);
    strlcpy(out->name, s_name, sizeof out->name);
    strlcpy(out->version, s_version, sizeof out->version);
    out->known = s_name[0] || s_version[0];
    out->plus = S.plus || owrx_plus_version(s_version);
    out->users = S.users;
    out->users_max = S.users_max;
    strlcpy(out->band, S.band_name, sizeof out->band);
    strlcpy(out->state, s_state, sizeof out->state);
    out->meter_lo = S.m_lo;
    out->meter_hi = S.m_hi;
    out->sq_db = S.sq ? (int16_t)(S.m_lo + (S.m_hi - S.m_lo) * (float)S.sq / 100.0f) : -150;
    if (S.f_max < F_TOP) {
        out->center = (S.f_min + S.f_max) / 2;
        out->rate = (int32_t)(S.f_max - S.f_min);
    }
    out->band_sel = S.band_sel;
    out->band_wait_s = band_wait_locked(now);
    out->bands_seq = s_bands_seq;
    const bool connecting = S.link == RADIO_LINK_CONNECTING || S.link == RADIO_LINK_GREETING;
    const bool on_it = S.link == RADIO_LINK_READY;
    const int sel = S.band_sel;
    char sdr[40] = "";
    if (sel >= 0 && sel < s_nb) strlcpy(sdr, s_bands[sel].sdr, sizeof sdr);
    taskEXIT_CRITICAL(&S_LOCK);
    if (have) strlcpy(out->url, r.host, sizeof out->url);
    /* The slab: under its name, the band and its SDR; under that its
     * software -- or that it is on its way. */
    if (out->band[0] && sdr[0]) snprintf(out->line2, sizeof out->line2, "%.28s  %.17s", out->band, sdr);
    else                        strlcpy(out->line2, out->band, sizeof out->line2);
    if (connecting)
        strlcpy(out->line3, "connecting...", sizeof out->line3);
    else if (on_it && out->version[0])
        snprintf(out->line3, sizeof out->line3, "%s %s", out->plus ? "OpenWebRX+" : "OpenWebRX",
                 out->version[0] == 'v' ? out->version + 1 : out->version);
    else if (on_it)
        strlcpy(out->line3, out->plus ? "OpenWebRX+" : "OpenWebRX", sizeof out->line3);
}

int owrx_bands(owrx_band_info_t *out, int max)
{
    taskENTER_CRITICAL(&S_LOCK);
    const int n = s_nb < max ? s_nb : max;
    if (out && n > 0) memcpy(out, s_bands, (size_t)n * sizeof *out);
    taskEXIT_CRITICAL(&S_LOCK);
    return n;
}

bool owrx_band_choose(int i)
{
    const int64_t now = esp_timer_get_time();
    bool ok = false;
    int wait = 0;
    char id[OWRX_ID_MAX] = "", name[48] = "";
    taskENTER_CRITICAL(&S_LOCK);
    wait = band_wait_locked(now);
    if (i >= 0 && i < s_nb && i != S.band_sel && !wait && s_band_ids[i][0]) {
        strlcpy(id, s_band_ids[i], sizeof id);
        strlcpy(name, s_bands[i].name, sizeof name);
        strlcpy(S.band, id, sizeof S.band);
        S.band_seq++;
        S.t_band = now;
        ok = true;
    }
    taskEXIT_CRITICAL(&S_LOCK);
    if (ok) ESP_LOGI(TAG, "band %d (%s) asked for", i, name);
    else    ESP_LOGW(TAG, "band %d refused%s", i, wait ? ": too soon after the last" : "");
    return ok;
}

/* ------------------------------------------------------------- radio.h */

const char *radio_link_name(void) { return s_tls ? "WSS" : "WS"; }

esp_err_t radio_start(const char *host, uint16_t port, const char *user, const char *pass)
{
    /* Its receivers are the page's list, each with its own address. */
    (void)host;
    (void)port;
    (void)user;
    (void)pass;
    load();
    const esp_timer_create_args_t ta = { .callback = save_cb, .name = "owsave" };
    if (!s_save_t) esp_timer_create(&ta, &s_save_t);
    /* Its stack in PSRAM: room for a TLS handshake beside the session. */
    ESP_RETURN_ON_FALSE(xTaskCreatePinnedToCoreWithCaps(task, "owrx", 16384, NULL, 5, NULL, 0,
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
    /* Its audio is still decoded meanwhile: only not played. */
    S.suspend = suspend;
    ESP_LOGW(TAG, "audio %s", suspend ? "suspended" : "resumed");
}

void radio_get_status(radio_status_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof *out);
    char label[24] = "";
    const int i = owrx_rx_active();
    if (i >= 0) owrx_rx_label(i, label, sizeof label);
    taskENTER_CRITICAL(&S_LOCK);
    out->link       = S.link;
    out->f_display  = S.tune.f_display;
    out->f_server   = S.f_sent;
    strlcpy(out->mode, S.mode, sizeof out->mode);
    out->filt_lo    = S.lo;
    out->filt_hi    = S.hi;
    /* Not dBm: the receiver's own dB, which the face shows as such. */
    out->smeter_dbm = S.have_db && S.link == RADIO_LINK_READY ? S.db : -150.0f;
    strlcpy(out->model, S.plus || owrx_plus_version(s_version) ? "OpenWebRX+" : "OpenWebRX", sizeof out->model);
    out->has_squelch  = out->have_squelch = true;
    out->squelch_pct  = S.sq;
    strlcpy(out->link_why, S.why, sizeof out->link_why);
    out->f_min      = S.f_min;
    out->f_max      = S.f_max < F_TOP ? S.f_max : 0;
    out->rx_only    = true;
    strlcpy(out->note, S.note, sizeof out->note);
    out->note_seq   = S.note_seq;
    out->connects   = S.connects;
    out->closes     = S.closes;
    out->sends      = s_counts.sent;
    out->echoes     = s_counts.texts + s_counts.audio;
    out->rejects    = s_counts.lost + s_lost;
    strlcpy(out->last_close, s_last_close, sizeof out->last_close);
    taskEXIT_CRITICAL(&S_LOCK);
    strlcpy(out->server, label, sizeof out->server);
}

bool radio_is_ready(void) { return S.link == RADIO_LINK_READY; }
bool radio_on_air(void) { return false; }

/* Receive only: there is nothing to key. */
void radio_ptt_key(void) {}
void radio_ptt_unkey(void) {}
void radio_ptt_toggle(void) {}
void radio_ptt_force_abort(uint8_t reason) { (void)reason; }

/* A mode this receiver plays -- SAM is OpenWebRX+'s alone, AM upstream --
 * with the passband it opens with there, or from one sideband to the other
 * the same passband the other side round. Under S_LOCK. */
static void mode_locked(const char *want, bool mirror)
{
    const owrx_mode_t *m = owrx_mode_find(owrx_mode_of(want, S.tune.f_display, S.plus));
    if (!m) return;
    const bool flip = mirror && ((!strcmp(S.mode, "usb") && !strcmp(m->mod, "lsb")) ||
                                 (!strcmp(S.mode, "lsb") && !strcmp(m->mod, "usb")));
    if (flip && S.lo < S.hi) {
        const int32_t lo = S.lo;
        S.lo = -S.hi;
        S.hi = -lo;
    } else {
        owrx_passband(m, S.plus, &S.lo, &S.hi);
    }
    strlcpy(S.mode, m->mod, sizeof S.mode);
}

void radio_set_mode(const char *mode)
{
    if (!mode) return;
    char before[8];
    taskENTER_CRITICAL(&S_LOCK);
    strlcpy(before, S.mode, sizeof before);
    mode_locked(mode, false);
    const bool other = strcmp(before, S.mode) != 0;
    taskEXIT_CRITICAL(&S_LOCK);
    if (other) changed();
}

void radio_set_filter(int32_t lo, int32_t hi)
{
    if (lo >= hi || lo < -100000 || hi > 100000) return;
    taskENTER_CRITICAL(&S_LOCK);
    S.lo = lo;
    S.hi = hi;
    taskEXIT_CRITICAL(&S_LOCK);
    changed();
}

void radio_goto_freq(int64_t hz)
{
    taskENTER_CRITICAL(&S_LOCK);
    /* Within the band the receiver is on: another is chosen as a band. */
    if (hz < S.f_min) hz = S.f_min;
    if (hz > S.f_max) hz = S.f_max;
    tune_assign(&S.tune, hz);
    /* A band's own sideband, for a voice: lower below 10 MHz, upper on 60 m. */
    const char *sb = kiwi_sideband(hz);
    if (sb && (!strcmp(S.mode, "usb") || !strcmp(S.mode, "lsb")) && strcmp(S.mode, sb)) mode_locked(sb, true);
    taskEXIT_CRITICAL(&S_LOCK);
    changed();
}

/* The squelch, 0-100 % of the meter's scale, 0 open. Closed, the receiver
 * sends nothing (csdr's squelch stops the audio), and the session fills the
 * silence; it is a quiet moment. */
void radio_set_squelch(uint8_t pct)
{
    if (pct > 100) pct = 100;
    taskENTER_CRITICAL(&S_LOCK);
    const bool other = S.sq != pct;
    S.sq = pct;
    taskEXIT_CRITICAL(&S_LOCK);
    if (other) changed();
}

/* Nothing of these on an OpenWebRX. */
void radio_set_agc(const char *agc) { (void)agc; }
void radio_set_gain(int8_t gain) { (void)gain; }
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
bool radio_get_choice(uint8_t i, char *title, size_t tn, char *name, size_t nn)
{
    (void)i;
    (void)title;
    (void)tn;
    (void)name;
    (void)nn;
    return false;
}
void radio_choose(uint8_t i) { (void)i; }
