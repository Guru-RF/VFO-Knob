/* The websdr firmware's receiver: radio.h for a WebSDR -- PA3FWM's server
 * software, the receivers listed on websdr.org -- over its protocol
 * (components/wsdr_proto's session, on the web SDRs' link). See wsdr.h.
 *
 * One task keeps one receiver going, as the OpenWebRX firmware's does: the
 * configuration page's list (net_prov: up to four) says which there are, the
 * operator which is in use; another is taken over at once, with no restart.
 * One that cannot be reached hands over to the next in the list, the one in
 * use after a fair chance (FAIR_TRIES); the first that plays is in use from
 * then on, saved as such. A site too busy is waited for five minutes if it is
 * the one in use, and passed by as a stand-in. What a site will not have --
 * no WebSDR there, the knob refused, a certificate, a redirect elsewhere --
 * holds it until it is chosen again or the list is saved.
 *
 * Before its first session each boot, a receiver's page is read as a
 * browser reads it: its bandinfo.js (its bands, its band plan, its idle
 * timeout), its sound script (which stream path its page opens) and its
 * title (its name). Every listener tunes independently on a WebSDR, so the
 * dial runs on from one band into the next by itself, over the gaps between
 * them, and stops only at the outer edges; BAND chooses among its bands, or
 * on a one-band site among its band plan's ranges.
 *
 * Its idle timeout is kept as its page keeps it: nothing touched that long,
 * the session is let go (IDLE), and the next touch takes it up again.
 *
 * The task's stack is in PSRAM, so it never touches flash: the dial, the
 * mode and the passband (per receiver), the squelch and the receiver in use
 * are saved by a timer, at a quiet moment -- a session over, nothing heard a
 * second -- or 30 s after the last change. */
#include "wsdr.h"
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
#include "nvs.h"
#include "owrx_proto.h"
#include "vfo_tune.h"
#include "wsdr_proto.h"
#include "wsdr_sess.h"

static const char *TAG = "wsdr";

#define NVS_NS          "vfo"
#define LOOP_MS         20                     /* the knob's detents: tune_apply's pace */
#define F_LOW           10000LL
#define F_TOP           2000000000LL           /* before a receiver has said where it is */
#define DEFAULT_HZ      7100000LL              /* 40 m, LSB */
#define FAIR_TRIES      2                      /* the one in use, not reached: tries before another */
#define STREAM_OK_US    (30 * 1000000LL)       /* streamed this long: its waits start over */
#define SAVE_QUIET_US   (2 * 1000000LL)
#define SAVE_LATER_US   (30 * 1000000LL)
#define NO_SOCKET_MS    2000                   /* the knob's own sockets all taken: soon again */
#define FOREVER         0
#define N_RX            NET_PROV_RADIOS
#define N_CHOICE        (WSDR_BANDS > WSDR_PLAN ? WSDR_BANDS : WSDR_PLAN)

static portMUX_TYPE S_LOCK = portMUX_INITIALIZER_UNLOCKED;
static struct {
    radio_link_t link;
    char     why[16];
    tune_t   tune;
    int64_t  f_min, f_max, f_sent;
    char     mode[8];
    int32_t  lo, hi;                /* the passband, from the carrier the site is sent */
    uint8_t  sq;                    /* the squelch: 0 open, else on (the site's own, on or off) */
    bool     mute;
    int      dbm10;                 /* the S-meter */
    bool     have_db;
    uint32_t want;                  /* the receiver in use, by its key */
    uint32_t at;                    /* the one tried now: the one in use, or a stand-in */
    uint32_t chosen, chosen_key;    /* the operator's acts, and the last one's receiver */
    uint32_t run_key;               /* the session's */
    int      band;                  /* the band the dial is in, in the site's list; -1 */
    uint32_t touch;                 /* the operator's acts on the dial and the rest: the idle clock */
    unsigned rate;
    bool     fresh;                 /* nothing kept for this receiver: where its page starts */
    bool     suspend;
    uint32_t connects, closes;
    bool     dirty, use_dirty;      /* settings, and the one in use, not saved yet */
} S = { .link = RADIO_LINK_DOWN, .f_min = F_LOW, .f_max = F_TOP, .band = -1, .dbm10 = -1500 };

/* The list's receivers by their keys, as of a list generation. */
static uint32_t s_keys[N_RX];
static int      s_keys_n = -1;
static uint32_t s_keys_gen;

/* Each receiver as the knob knows it this boot, by its key. */
typedef struct {
    uint32_t    key;
    wsdr_end_t  end;                /* its last session's end, NONE none */
    uint8_t     tries;              /* ...in a row */
    int64_t     not_before;
    bool        held;               /* until chosen again -- or the list saved */
    uint32_t    gen;                /* the list's, when it was held */
    uint16_t    tls_port;           /* its redirect to https:// on its own host, followed */
    bool        looked;             /* its page read: bandinfo.js, sound script, title */
    bool        v11;                /* its page opens /~~stream?v=11 */
    char        name[64];           /* its page's title */
    wsdr_info_t info;               /* its bands */
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
EXT_RAM_BSS_ATTR static wsdr_info_t    s_info;      /* the bands of the receiver the session is on */
EXT_RAM_BSS_ATTR static wsdr_choice_t  s_choices[N_CHOICE];
static int                             s_nc;
static uint32_t                        s_bands_seq;
EXT_RAM_BSS_ATTR static wsdr_counts_t  s_counts;
EXT_RAM_BSS_ATTR static uint32_t       s_lost;      /* feeds a full ring let go */
EXT_RAM_BSS_ATTR static char           s_state[48];
EXT_RAM_BSS_ATTR static char           s_last_close[48];
EXT_RAM_BSS_ATTR static char           s_name[64];  /* the one in use's, for the face */
EXT_RAM_BSS_ATTR static char           s_ident[32]; /* who the knob is in a site's list of listeners */
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

/* Each receiver's key, fresh for the list as it is now; a list saved since
 * the last look with another in use than before: that one, the operator's
 * choice (owrx_client's keys_fresh, which this is). */
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
        return;
    }
    memcpy(s_keys, k, sizeof s_keys);
    s_keys_n = n;
    s_keys_gen = g;
    if (!first && a >= 0 && a < n && k[a] && k[a] != S.want) {
        S.want = S.at = k[a];
        S.chosen++;
        S.chosen_key = k[a];
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

int wsdr_rx_active(void)
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

bool wsdr_rx_label(int i, char *out, size_t cap)
{
    net_radio_t r;
    if (!out || !cap || !net_prov_radio_get(i, &r)) return false;
    if (r.name[0]) {
        strlcpy(out, r.name, cap);
        return true;
    }
    const uint32_t key = key_of(&r);
    char nm[64] = "";
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

/* ------------------------------------------------------------ the bands */

/* The dial's band from its frequency: the one it is in still, else the
 * first that covers it. Under S_LOCK. */
static void band_locked(void)
{
    S.band = wsdr_band_of(&s_info, S.tune.f_display, S.band);
}

/* A voice's sideband where the dial comes into another band: lower below
 * 10 MHz, upper above and on 60 m -- the page's own rule. Under S_LOCK. */
static void sideband_locked(void)
{
    const char *sb = kiwi_sideband(S.tune.f_display);
    if (!sb) sb = S.tune.f_display < 10000000 ? "lsb" : "usb";     /* outside the amateur bands too */
    if (strcmp(S.mode, sb) == 0) return;
    if (strcmp(S.mode, "usb") && strcmp(S.mode, "lsb")) return;
    const int32_t lo = S.lo;
    S.lo = -S.hi;
    S.hi = -lo;
    strlcpy(S.mode, sb, sizeof S.mode);
}

/* The span the dial may cover: the site's bands from the lowest to the
 * highest; between them it runs over the gaps (place_locked). */
static void span_locked(void)
{
    if (!s_info.n_bands) {
        S.f_min = F_LOW;
        S.f_max = F_TOP;
        return;
    }
    int64_t lo = INT64_MAX, hi = INT64_MIN;
    for (int i = 0; i < s_info.n_bands; i++) {
        const wsdr_band_t *b = &s_info.band[i];
        if (b->center_hz - b->span_hz / 2 < lo) lo = b->center_hz - b->span_hz / 2;
        if (b->center_hz + b->span_hz / 2 > hi) hi = b->center_hz + b->span_hz / 2;
    }
    S.f_min = lo > F_LOW ? lo : F_LOW;
    S.f_max = hi;
}

/* The dial in a gap between two bands -- turned out of one -- on into the
 * next that way, at its near edge; none that way: back to the edge it
 * left. `up`: which way it went. Under S_LOCK. */
static void place_locked(bool up)
{
    const int was = S.band;
    band_locked();
    if (S.band < 0 && s_info.n_bands) {
        const int64_t hz = S.tune.f_display;
        int best = -1;
        int64_t edge = 0;
        for (int i = 0; i < s_info.n_bands; i++) {
            const wsdr_band_t *b = &s_info.band[i];
            const int64_t lo = b->center_hz - b->span_hz / 2, hi = b->center_hz + b->span_hz / 2;
            if (up && lo > hz && (best < 0 || lo < edge)) best = i, edge = lo;
            if (!up && hi < hz && (best < 0 || hi > edge)) best = i, edge = hi;
        }
        if (best < 0 && was >= 0) {
            const wsdr_band_t *b = &s_info.band[was];
            best = was;
            edge = up ? b->center_hz + b->span_hz / 2 : b->center_hz - b->span_hz / 2;
        }
        if (best >= 0) {
            tune_assign(&S.tune, edge);
            S.band = best;
        }
    }
    if (S.band != was && S.band >= 0) sideband_locked();
}

/* BAND's list: the site's bands, or its band plan's ranges where it has one
 * band only. Under S_LOCK. */
static void choices_locked(void)
{
    s_nc = 0;
    if (s_info.n_bands > 1 || !s_info.n_plan) {
        for (int i = 0; i < s_info.n_bands && s_nc < N_CHOICE; i++) {
            const wsdr_band_t *b = &s_info.band[i];
            wsdr_choice_t *c = &s_choices[s_nc++];
            strlcpy(c->name, b->name[0] ? b->name : "band", sizeof c->name);
            c->lo = b->center_hz - b->span_hz / 2;
            c->hi = b->center_hz + b->span_hz / 2;
            c->plan = false;
        }
    } else {
        for (int i = 0; i < s_info.n_plan && s_nc < N_CHOICE; i++) {
            const wsdr_range_t *r = &s_info.plan[i];
            if (wsdr_band_of(&s_info, (r->lo_hz + r->hi_hz) / 2, -1) < 0) continue;  /* not on its band */
            wsdr_choice_t *c = &s_choices[s_nc++];
            wsdr_range_name(r, c->name, sizeof c->name, NULL);
            c->lo = r->lo_hz;
            c->hi = r->hi_hz;
            c->plan = true;
        }
        /* In frequency order, as the page's own list reads. */
        for (int i = 1; i < s_nc; i++)
            for (int j = i; j > 0 && s_choices[j].lo < s_choices[j - 1].lo; j--) {
                const wsdr_choice_t t = s_choices[j];
                s_choices[j] = s_choices[j - 1];
                s_choices[j - 1] = t;
            }
    }
    s_bands_seq++;
}

/* The receiver the session is on: its bands the dial's from now on, the
 * dial on one of them -- kept where it was, else where the site's page
 * starts a listener. Under S_LOCK. */
static void bands_from_locked(const rx_t *x)
{
    s_info = x->info;
    span_locked();
    choices_locked();
    if (S.fresh || wsdr_band_of(&s_info, S.tune.f_display, -1) < 0) {
        /* The site's own start where it gives one, else its first band's. */
        int64_t hz = s_info.ini_hz;
        if (hz <= 0 || wsdr_band_of(&s_info, hz, -1) < 0) hz = s_info.band[0].vfo_hz;
        tune_assign(&S.tune, hz);
        const wsdr_mode_t *m = wsdr_mode_named(s_info.ini_mode);
        if (S.fresh && m) {
            strlcpy(S.mode, m->name, sizeof S.mode);
            S.lo = m->lo;
            S.hi = m->hi;
        }
        S.band = -1;
        band_locked();
        sideband_locked();
    } else {
        band_locked();
    }
    S.fresh = false;
}

/* ------------------------------------------------------------ remembered */

/* Nothing heard now: no session playing, or muted. Under S_LOCK. */
static bool quiet_locked(void)
{
    return S.link != RADIO_LINK_READY || S.mute;
}

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

/* ...and back on it, where it was kept. False, for a receiver nothing was
 * kept for. Under S_LOCK. */
static bool unkeep_locked(uint32_t key)
{
    for (int i = 0; key && i < N_RX; i++) {
        const kept_t *k = &s_kept[i];
        if (k->key != key || k->hz < F_LOW) continue;
        tune_assign(&S.tune, k->hz);
        if (wsdr_mode_named(k->mod)) strlcpy(S.mode, k->mod, sizeof S.mode);
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
    if (!quiet_locked() && since < SAVE_LATER_US) {
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
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "wst", kept, sizeof kept);
    nvs_set_u8(h, "wsq", sq);
    nvs_commit(h);
    nvs_close(h);
    if (use >= 0 && use != net_prov_radio_active()) {
        const esp_err_t e = net_prov_radio_activate(use);
        if (e != ESP_OK) ESP_LOGE(TAG, "receiver %d in use, not saved: %s", use, esp_err_to_name(e));
    }
}

/* A setting changed -- an act of the operator's, which is also the idle
 * timeout's clock: into flash at a quiet moment. */
static void changed(void)
{
    taskENTER_CRITICAL(&S_LOCK);
    S.dirty = true;
    S.touch++;
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

/* Who the knob is in a site's list of listeners: the one name every web
 * receiver is told (NVS "sdrid", the Kiwi firmwares' too); "" none. */
static void ident_load(void)
{
    size_t n = sizeof s_ident;
    nvs_handle_t h;
    s_ident[0] = 0;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_str(h, "sdrid", s_ident, &n) != ESP_OK) s_ident[0] = 0;
        nvs_close(h);
    }
}

static void load(void)
{
    uint8_t sq = 0;
    size_t n = sizeof s_kept;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_blob(h, "wst", s_kept, &n) != ESP_OK || n != sizeof s_kept) memset(s_kept, 0, sizeof s_kept);
        nvs_get_u8(h, "wsq", &sq);
        nvs_close(h);
    }
    ident_load();
    keys_fresh();
    const int a = net_prov_radio_active();
    const wsdr_mode_t *lsb = wsdr_mode_named("lsb");
    taskENTER_CRITICAL(&S_LOCK);
    tune_init(&S.tune, DEFAULT_HZ, 1000);
    strlcpy(S.mode, "lsb", sizeof S.mode);
    S.lo = lsb->lo;
    S.hi = lsb->hi;
    S.sq = sq ? 1 : 0;
    if (a >= 0 && a < s_keys_n && s_keys[a]) S.want = s_keys[a];
    for (int i = 0; !S.want && i < s_keys_n; i++) S.want = s_keys[i];
    S.at = S.want;
    S.fresh = !unkeep_locked(S.want);
    const int i = place_of(S.want);
    taskEXIT_CRITICAL(&S_LOCK);
    ESP_LOGI(TAG, "%lld Hz %s %ld..%ld, squelch %s; %d receiver%s, %s; listed as \"%s\"", (long long)S.tune.f_display,
             S.mode, (long)S.lo, (long)S.hi, S.sq ? "on" : "off", s_keys_n, s_keys_n == 1 ? "" : "s",
             i >= 0 ? "one in use" : "none in use", s_ident);
}

/* ------------------------------------------------------------ the face */

static void set_link(radio_link_t l, const char *why, const char *state)
{
    taskENTER_CRITICAL(&S_LOCK);
    S.link = l;
    strlcpy(S.why, why ? why : "", sizeof S.why);
    if (state) strlcpy(s_state, state, sizeof s_state);
    if (l != RADIO_LINK_READY) {
        S.have_db = false;
        S.rate = 0;
    }
    taskEXIT_CRITICAL(&S_LOCK);
}

/* The pages' word for an end: the face's, in lower case. */
static void note_of(wsdr_end_t e, char *out, size_t cap)
{
    const char *w = wsdr_end_word(e);
    size_t i = 0;
    for (; w[i] && i + 1 < cap; i++) out[i] = (char)(w[i] >= 'A' && w[i] <= 'Z' ? w[i] + 32 : w[i]);
    out[i] = 0;
}

static void down(wsdr_end_t e, const char *state)
{
    char n[24];
    note_of(e, n, sizeof n);
    set_link(RADIO_LINK_DOWN, wsdr_end_word(e), state && state[0] ? state : n);
}

/* ------------------------------------------------------------ its page */

/* The receiver whose page is being read (ctx: its key) is still the one
 * wanted: another chosen meanwhile is not kept waiting. */
static bool still_wanted(void *ctx)
{
    taskENTER_CRITICAL(&S_LOCK);
    const bool w = S.at == (uint32_t)(uintptr_t)ctx;
    taskEXIT_CRITICAL(&S_LOCK);
    return w;
}

/* Its page, once a boot: its bands, its stream path, its title. NONE when
 * it answered as a WebSDR, else why not -- WANT when another receiver was
 * chosen meanwhile. */
static wsdr_end_t look(rx_t *x, wsdr_where_t *w)
{
    void *k = (void *)(uintptr_t)x->key;
    uint16_t tp = 0;
    EXT_RAM_BSS_ATTR static wsdr_info_t in;
    const wsdr_end_t e = wsdr_info_read(w, &in, &tp, still_wanted, k, TAG);
    if (tp && !w->tls) {
        ESP_LOGI(TAG, "%s: on https:// from now on, port %u", w->host, (unsigned)tp);
        x->tls_port = tp;
        w->tls = true;
        w->port = tp;
    }
    if (e != WSDR_END_NONE) {
        if (e != WSDR_END_WANT)
            ESP_LOGW(TAG, "%s:%u%s: no bandinfo.js (%s)", w->host, (unsigned)w->port, w->prefix, wsdr_end_word(e));
        return e;
    }
    bool v11 = true;                    /* every site but Twente: a distributed server */
    const wsdr_end_t pe = wsdr_path_read(w, &v11, still_wanted, k, TAG);
    if (pe == WSDR_END_WANT) return pe;
    if (pe != WSDR_END_NONE) v11 = true;
    char title[64] = "";
    if (wsdr_title_read(w, title, sizeof title, still_wanted, k, TAG) == WSDR_END_WANT) return WSDR_END_WANT;
    taskENTER_CRITICAL(&S_LOCK);
    x->info = in;
    x->v11 = v11;
    x->looked = true;
    if (title[0]) strlcpy(x->name, title, sizeof x->name);
    taskEXIT_CRITICAL(&S_LOCK);
    ESP_LOGI(TAG, "%s:%u%s: \"%s\", %d band%s%s%s, %s, idle timeout %s", w->host, (unsigned)w->port, w->prefix,
             title, in.n_bands, in.n_bands == 1 ? "" : "s", in.n_plan ? ", a band plan of " : "",
             in.n_plan ? (in.n_plan > 9 ? "many" : "a few") : "", v11 ? "/~~stream?v=11" : "/~~stream",
             in.idle_ms ? "on" : "none");
    for (int i = 0; i < in.n_bands; i++)
        ESP_LOGI(TAG, "  band %d %s: %lld +- %lld Hz, step %.3f Hz, %ld Hz wide at most", i, in.band[i].name,
                 (long long)in.band[i].center_hz, (long long)(in.band[i].span_hz / 2), in.band[i].step_hz,
                 (long)in.band[i].maxbw_hz);
    return WSDR_END_NONE;
}

/* ------------------------------------------------------------ the session's side */

static void cb_ctl(void *ctx, wsdr_ctl_t *out)
{
    (void)ctx;
    taskENTER_CRITICAL(&S_LOCK);
    const wsdr_mode_t *m = wsdr_mode_named(S.mode);
    out->t.dial_hz = S.tune.f_display;
    out->t.band = S.band >= 0 ? S.band : 0;
    out->t.mode = m ? m->mode : WSDR_M_SSB;
    out->t.cw = !strcmp(S.mode, "cw");
    out->t.lo = S.lo;
    out->t.hi = S.hi;
    if (S.band >= 0 && S.band < s_info.n_bands) out->b = s_info.band[S.band];
    else memset(&out->b, 0, sizeof out->b);
    out->squelch = S.sq != 0;
    out->mute = S.mute;
    out->touch = S.touch;
    strlcpy(out->name, s_ident, sizeof out->name);
    S.f_sent = S.tune.f_display;
    taskEXIT_CRITICAL(&S_LOCK);
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

static void cb_meter(void *ctx, int dbm10)
{
    (void)ctx;
    taskENTER_CRITICAL(&S_LOCK);
    S.dbm10 = dbm10;
    S.have_db = true;
    taskEXIT_CRITICAL(&S_LOCK);
}

EXT_RAM_BSS_ATTR static uint32_t s_rep_lost, s_rep_under;

static void report_from_now(void)
{
    audio_stats_t a;
    audio_out_stats(&a);
    s_rep_lost  = s_lost;
    s_rep_under = a.underruns;
}

/* Every 30 s while it plays: how the audio came, and what the ring made of
 * it. */
static void cb_report(void *ctx, const wl_report_t *r)
{
    (void)ctx;
    audio_stats_t a;
    audio_out_stats(&a);
    ESP_LOGI(TAG, "ring %lu ms of %lu, trim %.5f, blocks %lu, lost %lu, underruns %lu, breaks %lu, jumps %lu "
             "(%lu ms left out); gap %lu ms, held %lu; free internal %u",
             (unsigned long)r->level_ms, (unsigned long)r->target_ms, (double)r->trim, (unsigned long)r->blocks,
             (unsigned long)(s_lost - s_rep_lost), (unsigned long)(a.underruns - s_rep_under),
             (unsigned long)r->breaks, (unsigned long)r->jumps, (unsigned long)r->left_ms,
             (unsigned long)r->gap_ms, (unsigned long)r->held,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    s_rep_lost  = s_lost;
    s_rep_under = a.underruns;
}

static void cb_state(void *ctx, wsdr_state_t st, unsigned rate)
{
    (void)ctx;
    if (st == WSDR_ST_CONNECTING) {
        set_link(RADIO_LINK_CONNECTING, NULL, "connecting");
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
    taskENTER_CRITICAL(&S_LOCK);
    S.rate = rate;
    taskEXIT_CRITICAL(&S_LOCK);
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

static void cb_moved(void *ctx, uint16_t tls_port)
{
    (void)ctx;
    taskENTER_CRITICAL(&S_LOCK);
    rx_t *x = rx_find(S.run_key);
    if (x) x->tls_port = tls_port;
    taskEXIT_CRITICAL(&S_LOCK);
}

static const wsdr_link_t LINK = {
    .ctl = cb_ctl, .audio = cb_audio,
    .ring = { .queued = cb_queued, .trim = true, .preroll = cb_preroll, .underruns = cb_underruns, .tag = "wsdr" },
    .meter = cb_meter, .state = cb_state, .go_on = cb_go_on, .report = cb_report, .moved = cb_moved,
    .counts = &s_counts, .tag = "wsdr",
};

/* ------------------------------------------------------------ the task */

/* Up to `ms` (FOREVER: for as long as it takes), or until the operator does
 * something -- another receiver, any act of choosing, a touch where `touch`
 * is watched -- or the list is saved. True when one came. */
static bool wait_event(uint32_t ms, uint32_t at, uint32_t ch, uint32_t gen, const uint32_t *touch)
{
    for (uint32_t t = 0; ms == FOREVER || t < ms; t += 100) {
        vTaskDelay(pdMS_TO_TICKS(100));
        taskENTER_CRITICAL(&S_LOCK);
        const bool ev = S.at != at || S.chosen != ch || (touch && S.touch != *touch);
        taskEXIT_CRITICAL(&S_LOCK);
        if (ev || net_prov_radios_gen() != gen) return true;
    }
    return false;
}

/* The next receiver after `key` in the list, in turn, that has an address
 * and is not held: the one tried from now. False when there is none. */
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
        const bool same = S.at == key;
        if (same) S.at = c;
        taskEXIT_CRITICAL(&S_LOCK);
        return same;
    }
    return false;
}

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
static bool unreached(wsdr_end_t e)
{
    return e == WSDR_END_NOT_FOUND || e == WSDR_END_NO_ROUTE || e == WSDR_END_NO_ANSWER || e == WSDR_END_QUIET ||
           e == WSDR_END_DOWN || e == WSDR_END_PROTOCOL || e == WSDR_END_CLOSED;
}

/* How a try at `x` ended (not WANT, not IDLE): marked -- its wait, or held
 * -- and the next to try chosen. */
static void ended(rx_t *x, wsdr_end_t e, bool streamed, uint32_t gen)
{
    taskENTER_CRITICAL(&S_LOCK);
    const bool in_use = x->key == S.want;
    taskEXIT_CRITICAL(&S_LOCK);
    if (streamed) x->tries = 0;
    if (x->tries < 255) x->tries++;
    x->end = e;
    const uint32_t ms = wsdr_retry_ms(e, x->tries);
    if (!ms) {
        x->held = true;
        x->gen = gen;
    } else {
        const uint32_t jit = esp_random() % (ms / 4 + 1);
        x->not_before = esp_timer_get_time() + (int64_t)(ms + jit) * 1000;
    }
    /* Another's turn: one held; a stand-in that did not play; the one in use
     * not reached, after its fair chance. The one in use too busy keeps its
     * turn. */
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
    wsdr_sess_t *ss = wsdr_sess_new();
    if (!ss) {
        ESP_LOGE(TAG, "no memory for the receiver's buffers");
        set_link(RADIO_LINK_DOWN, "NO MEMORY", "no memory");
        vTaskDeleteWithCaps(NULL);
    }
    wsdr_link_t link = LINK;
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
        if (!owrx_url(r.host, &u)) {
            set_link(RADIO_LINK_DOWN, "NO RECEIVER", "no address");
            while (net_prov_radios_gen() == gen) vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
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
        if (x->held && x->gen != gen) x->held = false;      /* a list saved: a new address may cure it */
        if (key != last) {
            /* Another receiver: the dial it was left on there. */
            taskENTER_CRITICAL(&S_LOCK);
            keep_locked(last);
            S.fresh = !unkeep_locked(key);
            S.band = -1;
            s_name[0] = 0;
            memset(&s_info, 0, sizeof s_info);
            s_nc = 0;
            s_bands_seq++;
            S.f_min = F_LOW;
            S.f_max = F_TOP;
            taskEXIT_CRITICAL(&S_LOCK);
            save_now();
            last = key;
        }
        taskENTER_CRITICAL(&S_LOCK);
        if (x->name[0]) strlcpy(s_name, x->name, sizeof s_name);
        taskEXIT_CRITICAL(&S_LOCK);
        if (x->held) {
            down(x->end, NULL);
            wait_event(FOREVER, key, ch, gen, NULL);
            continue;
        }
        const int64_t wait = x->not_before - esp_timer_get_time();
        if (wait > 0) {
            char st[48], w[16];
            note_of(x->end, w, sizeof w);
            snprintf(st, sizeof st, "%.15s, again in %lld s", w, (long long)((wait + 999999) / 1000000));
            down(x->end, st);
            wait_event((uint32_t)(wait / 1000) + 1, key, ch, gen, NULL);
            continue;
        }
        if (x->tls_port) {
            u.tls = true;
            u.port = x->tls_port;
        }
        wsdr_where_t w = { .host = u.host, .prefix = u.path, .port = u.port, .tls = u.tls, .key = owrx_key(&u) };

        /* Its page, once a boot, before its first session. */
        set_link(RADIO_LINK_CONNECTING, NULL, "connecting");
        if (!x->looked) {
            const wsdr_end_t e = look(x, &w);
            if (e == WSDR_END_WANT || moved_from(key)) continue;
            if (e == WSDR_END_NO_SOCKET) {
                vTaskDelay(pdMS_TO_TICKS(NO_SOCKET_MS));
                continue;
            }
            if (e != WSDR_END_NONE) {
                down(e, NULL);
                ended(x, e, false, gen);
                continue;
            }
        }

        taskENTER_CRITICAL(&S_LOCK);
        bands_from_locked(x);
        if (x->name[0]) strlcpy(s_name, x->name, sizeof s_name);
        S.run_key = key;
        S.connects++;
        s_tls = w.tls;
        taskEXIT_CRITICAL(&S_LOCK);
        ESP_LOGI(TAG, "on %s: %lld Hz %s", S.band >= 0 ? s_info.band[S.band].name : "?", (long long)S.tune.f_display,
                 S.mode);
        audio_out_flush();
        s_t_stream = 0;
        const wsdr_end_t end = wsdr_sess_run(ss, &w, x->v11, x->info.idle_ms, &link);
        const bool streamed = s_t_stream && esp_timer_get_time() - s_t_stream >= STREAM_OK_US;
        audio_out_flush();
        taskENTER_CRITICAL(&S_LOCK);
        S.run_key = 0;
        S.closes++;
        if (S.chosen_key == key) seen = S.chosen;
        taskEXIT_CRITICAL(&S_LOCK);
        strlcpy(s_last_close, wsdr_end_word(end), sizeof s_last_close);
        save_now();
        if (end == WSDR_END_WANT) {
            set_link(RADIO_LINK_CONNECTING, NULL, "connecting");
            continue;
        }
        if (end == WSDR_END_NO_SOCKET) {
            down(end, NULL);
            vTaskDelay(pdMS_TO_TICKS(NO_SOCKET_MS));
            continue;
        }
        if (end == WSDR_END_IDLE) {
            /* As its page: let go, and taken up again at the next touch. */
            down(end, "idle: turn the dial");
            x->tries = 0;
            taskENTER_CRITICAL(&S_LOCK);
            const uint32_t t = S.touch;
            taskEXIT_CRITICAL(&S_LOCK);
            wait_event(FOREVER, key, ch, gen, &t);
            continue;
        }
        down(end, NULL);
        ended(x, end, streamed, gen);
    }
}

/* ------------------------------------------------------------- wsdr.h */

bool wsdr_rx_use(int i, bool chosen)
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
    if (moved || chosen) changed();
    char l[24];
    if (wsdr_rx_label(i, l, sizeof l))
        ESP_LOGI(TAG, "receiver %d (%s)%s%s", i, l, moved ? " in use" : "", !moved && chosen ? " chosen again" : "");
    return true;
}

bool wsdr_audible(void)
{
    taskENTER_CRITICAL(&S_LOCK);
    const bool a = !quiet_locked();
    taskEXIT_CRITICAL(&S_LOCK);
    return a;
}

bool wsdr_rx_state(int i, wsdr_rx_state_t *out)
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
        if (out->held || out->wait_s) strlcpy(out->why, wsdr_end_word(x->end), sizeof out->why);
        strlcpy(out->name, x->name, sizeof out->name);
        out->tls_port = x->tls_port;
    }
    if (key == S.at && S.link == RADIO_LINK_DOWN && !strcmp(S.why, "IDLE")) strlcpy(out->why, "IDLE", sizeof out->why);
    taskEXIT_CRITICAL(&S_LOCK);
    return true;
}

/* ------------------------------------------------------------- the Test */

/* On a task of its own: its stack in PSRAM, with room for a TLS handshake,
 * which the web server's has not. One at a time, as the web server runs
 * them. Done, it waits to be deleted. */
EXT_RAM_BSS_ATTR static struct {
    owrx_url_t    u;
    wsdr_info_t   info;
    bool          v11;
    char          title[64];
    wsdr_end_t    end;
    uint16_t      tls_port;
    volatile bool done;
} s_test;

static void test_task(void *arg)
{
    (void)arg;
    wsdr_where_t w = { .host = s_test.u.host, .prefix = s_test.u.path, .port = s_test.u.port, .tls = s_test.u.tls,
                       .key = owrx_key(&s_test.u) };
    s_test.end = wsdr_info_read(&w, &s_test.info, &s_test.tls_port, NULL, NULL, "wsdr test");
    if (s_test.tls_port) {
        w.tls = true;
        w.port = s_test.tls_port;
    }
    if (s_test.end == WSDR_END_NONE) {
        s_test.v11 = true;
        wsdr_path_read(&w, &s_test.v11, NULL, NULL, "wsdr test");
        wsdr_title_read(&w, s_test.title, sizeof s_test.title, NULL, NULL, "wsdr test");
    }
    s_test.done = true;
    for (;;) vTaskDelay(portMAX_DELAY);
}

bool wsdr_test(const char *addr, wsdr_test_t *out)
{
    if (!out) return false;
    memset(out, 0, sizeof *out);
    if (!addr || !owrx_url(addr + strspn(addr, " "), &s_test.u)) {
        strlcpy(out->error, "NO ADDRESS", sizeof out->error);
        return false;
    }
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
    s_test.title[0] = 0;
    s_test.done = false;
    TaskHandle_t t = NULL;
    if (xTaskCreatePinnedToCoreWithCaps(test_task, "wstest", 16384, NULL, 5, &t, 0, MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGE(TAG, "no memory for the Test's task");
        strlcpy(out->error, "NO MEMORY", sizeof out->error);
        return false;
    }
    while (!s_test.done) vTaskDelay(pdMS_TO_TICKS(50));
    vTaskDeleteWithCaps(t);
    if (s_test.tls_port && !s_test.u.tls) {
        s_test.u.tls = true;
        s_test.u.port = s_test.tls_port;
    }
    if (tp || s_test.tls_port) owrx_url_text(&s_test.u, out->url, sizeof out->url);
    if (s_test.end != WSDR_END_NONE) {
        strlcpy(out->error, wsdr_end_word(s_test.end), sizeof out->error);
        ESP_LOGW(TAG, "test %s:%u%s: %s", s_test.u.host, (unsigned)s_test.u.port, s_test.u.path, out->error);
        return false;
    }
    out->ok = true;
    strlcpy(out->name, s_test.title, sizeof out->name);
    out->bands = s_test.info.n_bands;
    out->plan = s_test.info.n_bands == 1 ? s_test.info.n_plan : 0;
    out->idle_min = (int)(s_test.info.idle_ms / 60000);
    out->v11 = s_test.v11;
    ESP_LOGI(TAG, "test %s:%u%s: \"%s\", %d bands, idle %d min, %s", s_test.u.host, (unsigned)s_test.u.port,
             s_test.u.path, out->name, out->bands, out->idle_min, out->v11 ? "?v=11" : "plain");
    return true;
}

void wsdr_now(wsdr_now_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof *out);
    const int i = wsdr_rx_active();
    net_radio_t r;
    const bool have = i >= 0 && net_prov_radio_get(i, &r);
    taskENTER_CRITICAL(&S_LOCK);
    strlcpy(out->name, s_name, sizeof out->name);
    out->known = s_info.n_bands > 0;
    out->rate = S.rate;
    out->idle_min = (int)(s_info.idle_ms / 60000);
    strlcpy(out->state, s_state, sizeof out->state);
    out->bands_seq = s_bands_seq;
    const rx_t *x = rx_find(S.at);
    out->sam = x && x->looked && !x->v11;
    out->band_sel = -1;
    const int64_t hz = S.tune.f_display;
    for (int k = 0; k < s_nc; k++)
        if (hz >= s_choices[k].lo && hz <= s_choices[k].hi) {
            out->band_sel = k;
            break;
        }
    if (S.band >= 0 && S.band < s_info.n_bands) {
        const wsdr_band_t *b = &s_info.band[S.band];
        strlcpy(out->band, b->name, sizeof out->band);
        out->lo = b->center_hz - b->span_hz / 2;
        out->hi = b->center_hz + b->span_hz / 2;
    }
    const bool connecting = S.link == RADIO_LINK_CONNECTING || S.link == RADIO_LINK_GREETING;
    const bool on_it = S.link == RADIO_LINK_READY;
    const int sel = out->band_sel;
    char plan[24] = "";
    if (sel >= 0 && s_choices[sel].plan) strlcpy(plan, s_choices[sel].name, sizeof plan);
    taskEXIT_CRITICAL(&S_LOCK);
    if (have) strlcpy(out->url, r.host, sizeof out->url);
    /* The slab: under its name, the band (and the plan's range in it); then
     * that it is a WebSDR, or on its way. */
    if (plan[0]) snprintf(out->line2, sizeof out->line2, "%.20s  %.20s", plan, out->band);
    else         strlcpy(out->line2, out->band, sizeof out->line2);
    if (connecting)  strlcpy(out->line3, "connecting...", sizeof out->line3);
    else if (on_it)  strlcpy(out->line3, "WebSDR", sizeof out->line3);
}

int wsdr_bands(wsdr_choice_t *out, int max)
{
    taskENTER_CRITICAL(&S_LOCK);
    const int n = s_nc < max ? s_nc : max;
    if (out && n > 0) memcpy(out, s_choices, (size_t)n * sizeof *out);
    taskEXIT_CRITICAL(&S_LOCK);
    return n;
}

bool wsdr_band_choose(int i)
{
    bool ok = false;
    char name[24] = "";
    int64_t hz = 0;
    taskENTER_CRITICAL(&S_LOCK);
    if (i >= 0 && i < s_nc) {
        const wsdr_choice_t *c = &s_choices[i];
        strlcpy(name, c->name, sizeof name);
        if (c->plan) {
            bool ham = false;
            const wsdr_range_t rg = { c->lo, c->hi };
            char nm[24];
            wsdr_range_name(&rg, nm, sizeof nm, &ham);
            hz = (c->lo + c->hi) / 2;
            tune_assign(&S.tune, hz);
            if (!ham) {                                 /* a broadcast band: AM */
                const wsdr_mode_t *m = wsdr_mode_named("am");
                strlcpy(S.mode, m->name, sizeof S.mode);
                S.lo = m->lo;
                S.hi = m->hi;
            }
        } else {
            const wsdr_band_t *b = &s_info.band[i];
            hz = b->vfo_hz;
            tune_assign(&S.tune, hz);
        }
        band_locked();
        sideband_locked();
        ok = true;
    }
    taskEXIT_CRITICAL(&S_LOCK);
    if (ok) {
        ESP_LOGI(TAG, "band %d (%s): %lld Hz", i, name, (long long)hz);
        changed();
    }
    return ok;
}

void wsdr_ident(char *out, size_t cap)
{
    if (!out || !cap) return;
    taskENTER_CRITICAL(&S_LOCK);
    strlcpy(out, s_ident, cap);
    taskEXIT_CRITICAL(&S_LOCK);
}

esp_err_t wsdr_ident_save(const char *who)
{
    char w[sizeof s_ident];
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)(who ? who : ""); *p && n + 1 < sizeof w; p++)
        if (*p >= 0x20 && *p != 0x7F && (n || *p != ' ')) w[n++] = (char)*p;
    size_t k = n;                               /* a UTF-8 character cut at the end: left out whole */
    while (k && ((unsigned char)w[k - 1] & 0xC0) == 0x80) k--;
    if (k && ((unsigned char)w[k - 1] & 0xC0) == 0xC0) {
        const unsigned char lead = (unsigned char)w[k - 1];
        const size_t want = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : 2;
        if (n - (k - 1) < want) n = k - 1;
    }
    while (n && w[n - 1] == ' ') n--;
    w[n] = 0;
    char was[sizeof s_ident];
    wsdr_ident(was, sizeof was);
    if (!strcmp(was, w)) return ESP_OK;
    nvs_handle_t h;
    esp_err_t e = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (e == ESP_OK) {
        e = w[0] ? nvs_set_str(h, "sdrid", w) : nvs_erase_key(h, "sdrid");
        if (e == ESP_ERR_NVS_NOT_FOUND) e = ESP_OK;
        if (e == ESP_OK) e = nvs_commit(h);
        nvs_close(h);
    }
    if (e != ESP_OK) return e;
    taskENTER_CRITICAL(&S_LOCK);
    strlcpy(s_ident, w, sizeof s_ident);        /* the session sends it at once, with a tuning */
    taskEXIT_CRITICAL(&S_LOCK);
    ESP_LOGI(TAG, "listed at the sites as \"%s\"", w);
    return ESP_OK;
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
    const esp_timer_create_args_t ta = { .callback = save_cb, .name = "wssave" };
    if (!s_save_t) esp_timer_create(&ta, &s_save_t);
    ESP_RETURN_ON_FALSE(xTaskCreatePinnedToCoreWithCaps(task, "wsdr", 16384, NULL, 5, NULL, 0,
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
        place_locked(detents > 0);
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
    if (moved) place_locked(S.tune.f_display > was);
    taskEXIT_CRITICAL(&S_LOCK);
    if (moved) changed();
}

void radio_audio_suspend(bool suspend)
{
    S.suspend = suspend;
    ESP_LOGW(TAG, "audio %s", suspend ? "suspended" : "resumed");
}

void radio_get_status(radio_status_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof *out);
    char label[24] = "";
    const int i = wsdr_rx_active();
    if (i >= 0) wsdr_rx_label(i, label, sizeof label);
    taskENTER_CRITICAL(&S_LOCK);
    out->link       = S.link;
    out->f_display  = S.tune.f_display;
    out->f_server   = S.f_sent;
    strlcpy(out->mode, S.mode, sizeof out->mode);
    out->filt_lo    = S.lo;
    out->filt_hi    = S.hi;
    /* dBm, as calibrated as the site's owner made it. */
    out->smeter_dbm = S.have_db && S.link == RADIO_LINK_READY ? (float)S.dbm10 / 10.0f : -150.0f;
    strlcpy(out->model, "WebSDR", sizeof out->model);
    out->has_squelch  = out->have_squelch = true;
    out->squelch_pct  = S.sq ? 100 : 0;
    strlcpy(out->link_why, S.why, sizeof out->link_why);
    out->f_min      = S.f_min;
    out->f_max      = S.f_max < F_TOP ? S.f_max : 0;
    out->rx_only    = true;
    out->connects   = S.connects;
    out->closes     = S.closes;
    out->sends      = s_counts.sent;
    out->echoes     = s_counts.msgs;
    out->rejects    = s_counts.dropped + s_lost;
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

/* A mode the site plays -- AM sync on Twente's server alone, AM elsewhere --
 * with its page's passband. Under S_LOCK. */
static void mode_locked(const char *want)
{
    const rx_t *x = rx_find(S.run_key ? S.run_key : S.at);
    const bool newer = x && x->looked && !x->v11;
    if (want && !strcmp(want, "sam") && !newer) want = "am";
    if (want && !strcmp(want, "fm")) want = "nfm";
    const wsdr_mode_t *m = wsdr_mode_named(want);
    if (!m) return;
    strlcpy(S.mode, m->name, sizeof S.mode);
    S.lo = m->lo;
    S.hi = m->hi;
}

void radio_set_mode(const char *mode)
{
    if (!mode) return;
    char before[8];
    taskENTER_CRITICAL(&S_LOCK);
    strlcpy(before, S.mode, sizeof before);
    mode_locked(mode);
    const bool other = strcmp(before, S.mode) != 0;
    taskEXIT_CRITICAL(&S_LOCK);
    if (other) changed();
}

void radio_set_filter(int32_t lo, int32_t hi)
{
    if (lo >= hi || lo < -20000 || hi > 20000) return;
    taskENTER_CRITICAL(&S_LOCK);
    S.lo = lo;
    S.hi = hi;
    taskEXIT_CRITICAL(&S_LOCK);
    changed();
}

void radio_goto_freq(int64_t hz)
{
    taskENTER_CRITICAL(&S_LOCK);
    if (hz < S.f_min) hz = S.f_min;
    if (hz > S.f_max) hz = S.f_max;
    const int64_t was = S.tune.f_display;
    tune_assign(&S.tune, hz);
    place_locked(hz >= was);
    sideband_locked();
    taskEXIT_CRITICAL(&S_LOCK);
    changed();
}

/* The site's own squelch -- on its modulation, not its level: on or off. */
void radio_set_squelch(uint8_t pct)
{
    taskENTER_CRITICAL(&S_LOCK);
    const uint8_t v = pct ? 1 : 0;
    const bool other = S.sq != v;
    S.sq = v;
    taskEXIT_CRITICAL(&S_LOCK);
    if (other) changed();
}

/* Muted, the site is asked to send silence: a byte a block, not its audio. */
void radio_mute(bool muted)
{
    taskENTER_CRITICAL(&S_LOCK);
    const bool other = S.mute != muted;
    S.mute = muted;
    taskEXIT_CRITICAL(&S_LOCK);
    if (other) changed();
}

/* Nothing of these on a WebSDR. */
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
