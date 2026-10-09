/* A web SDR as a second receiver: KiwiSDR's protocol. See sdr_rx.h.
 *
 * The session is components/kiwi_proto's (kiwi_sess.h), the one the kiwi
 * firmware's receiver runs on: the app path, /<ts>/SND, as kiwirecorder logs
 * in, so an owner's limit on apps holds for the knob; every refusal classed;
 * CW tuned so the station on the dial is heard at the receiver's own CW tone
 * -- 500 Hz on a KiwiSDR, or where its load_cfg centres CW, and on an
 * UberSDR's Kiwi input the carrier itself, the tone its own; a Web-888's
 * S-meter counted from its own reference; keepalive every 5 s; the audio
 * brought to 24 kHz by half-band filters and a cubic interpolator, and a
 * backlog left out in one jump. What is the second receiver's own is here:
 * the list, the one chosen, the radio it follows, and what it does when a
 * receiver will not have it -- or cannot reach the dial: quiet, and said so,
 * the session and its channel kept, until the dial is back within its range
 * (kiwi_sess.h's reach), rather than its edge played as if it were the dial.
 *
 * Its owner's limits are kept. A login refused for the day's listening limit
 * (MSG ip_limit) marks the receiver, and the mark outlives a restart
 * (kiwi_mark.h): a Kiwi bars an address for good after five such refusals.
 * So does a login left unanswered, where its day limit may count it: its
 * answer may have been such a refusal, lost on the way -- and a login's
 * answer is always waited for, whatever else happens meanwhile. A marked
 * receiver is not logged in to again on the knob's own initiative; the
 * operator choosing it again (sdr_rx_choose) is one counted try, two at
 * most. Ended for idling (inactivity_timeout) or by its owner (kiwi_kick), or
 * refused this address (badp=3, HTTP 403), it waits for such a choice too --
 * through a crash as well, as a restart nobody asked for chooses nothing. A
 * wrong password, an address that is no Kiwi, and a KiwiSDR that lets no
 * apps in -- its /status says so, or its door for apps stays shut three
 * times running -- wait for that or for the list saved. On its own the knob
 * tries one receiver at most six times in ten minutes, and reads its /status
 * once a boot before its first session, seldom after: owners drop an address
 * that polls it.
 *
 * On the kiwi firmware it is the right ear, beside that firmware's own
 * receiver in the left (the primary, sdr_rx_primary_cb), from the same list
 * and on the same dial, with the left ear's AGC, noise filter and squelch
 * (sdr_rx_settings_cb): each ear's owners' limits as they are, the marks
 * shared, and never both ears on one receiver -- a choice of the left ear's
 * is refused, and one the left ear takes meanwhile is let go and waited out.
 *
 * A receiver behind a front -- the kiwisdr.com proxy, Cloudflare -- is spoken
 * to over TLS (kiwi_sess.h), its address an https:// link; an http:// one's
 * redirect to https:// on its own host is followed, and kept (sdr_moved).
 *
 * The task's stack is in PSRAM: it never touches flash (the list is saved by
 * whoever calls sdr_save, the selection, the marks and a list a redirect
 * moved by timers). */
#include "sdr_rx.h"

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

static const char *TAG = "sdr";

#define NVS_NS     "vfo"
#define KEY_LIST   "sdrs"
#if VFO_RADIO_KIWI
/* The kiwi firmware's right ear keeps a choice of its own: a web SDR chosen
 * beside a radio on another firmware starts no second session here. */
#define KEY_SEL    "kwr"
#define KEY_SELH   "kwrh"            /* the right ear's kiwi_hp: its address */
#define KEY_BAL    "kwbal"
#else
#define KEY_SEL    "sdrsel"
#define KEY_SELH   "sdrselh"         /* the selected one's kiwi_hp: its address */
#define KEY_BAL    "sdrbal"
#endif
#define KEY_IDENT  "sdrid"           /* who the receivers' owners see */
/* Owners drop an address that polls /status. A receiver's is read once a
 * boot, before its first session; a marked one's again before a try, at
 * most once a minute however often it is chosen; and again five minutes on
 * after a session ended in a way it may explain. */
#define LOOK_GAP_US     (60 * 1000000LL)
#define STATUS_AGAIN_US (5 * 60 * 1000000LL)
#define CAP_N           6                      /* the knob's own attempts on one receiver... */
#define CAP_US          (10 * 60 * 1000000LL)  /* ...in this long */
#define STREAM_OK_US    (30 * 1000000LL)       /* streamed this long: the backoff starts over */
#define SILENT_MAX      3                      /* silent doors in a row: it lets no apps in */
#define SAVE_LATER_US   (30 * 1000000LL)       /* the choice to flash with audio playing, at the latest */
#define QUIET_US        (1000000LL)            /* silent this long: what it let through before has played */
#define LIST_LOOK_US    (2 * 1000000LL)        /* a list a redirect moved: a quiet moment looked for this often */
#define LIST_LINE        200                    /* one receiver's line in NVS, at most */
/* A task's stack for a session or the page's Test: a TLS handshake takes
 * some 9 kB of it on the PC (test/host's model), the session's own above
 * it -- in PSRAM, where room costs nothing that is scarce. */
#define TLS_STACK       16384

EXT_RAM_BSS_ATTR static sdr_cfg_t s_list[SDR_MAX];
static uint32_t     s_hp[SDR_MAX];  /* each one's kiwi_hp */
static int          s_n;
static bool         s_list_loaded;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static volatile int s_want = -1;
static volatile int8_t s_bal;
static volatile uint32_t s_list_gen;
/* The operator's acts (sdr_rx_choose), each one counted, even of the
 * receiver already chosen, and the receiver the last one chose -- and
 * whether it was held for its day limit as it was chosen: only then is the
 * choice a try. */
static volatile uint32_t s_chosen, s_chosen_hp;
static volatile bool     s_chosen_marked;
static sdr_status_t s_st = { .sel = -1 };
/* The kiwi firmware's left ear: whether it has a receiver in use, or a
 * session on it; whether audio plays, which flash waits for
 * (sdr_rx_quiet_cb); and its AGC, noise filter and squelch, this one's too
 * (sdr_rx_settings_cb). */
static bool (*volatile s_primary)(uint32_t hp, bool chosen);
static bool (*volatile s_audible)(void);
static void (*volatile s_settings)(uint8_t *agc, uint8_t *nr, uint8_t *sq_pct);
static int64_t s_t_change;                  /* the choice or the balance last changed */
/* The session's audio: frames lost (stereo, undecodable), feeds a full ring
 * let go, and both with the ring's underruns as the last report left them. */
EXT_RAM_BSS_ATTR static kiwi_counts_t s_counts;
EXT_RAM_BSS_ATTR static uint32_t      s_lost, s_rep_lost, s_rep_under;

/* What a receiver's /status said this boot, by its address. */
typedef struct {
    uint32_t      hp;
    bool          tried;            /* read, or tried, this boot */
    bool          ok;               /* ...and it answered as a Kiwi, at t_ok */
    bool          again;            /* a session since ended so that another read may explain it */
    uint8_t       end;              /* how its last session ended (kiwi_end_t) */
    int64_t       t_read, t_ok;
    kiwi_status_t st;
} look_t;
EXT_RAM_BSS_ATTR static look_t        s_look[SDR_MAX];
EXT_RAM_BSS_ATTR static kiwi_status_t s_reading;   /* a /status as it is read */
EXT_RAM_BSS_ATTR static kiwi_said_t   s_said;      /* the session's, as it goes */
/* The knob's own attempts on each receiver, the newest CAP_N. */
EXT_RAM_BSS_ATTR static struct { uint32_t hp; int64_t t[CAP_N]; uint8_t head; } s_cap[SDR_MAX];

static struct {
    int64_t  hz;
    char     mode[8];
    int32_t  lo, hi;
} s_tune;

/* The session's receiver: its place, its address and the list it was in;
 * `live` from just before its login to its end, whatever the list does; how
 * far the session has come (`st`), and whether the dial is out of its reach
 * (`out`) -- the session's task alone has those two. */
static struct { int want; uint32_t hp, gen; bool on; volatile bool live; kiwi_state_t st; bool out; } s_run;
/* Since when its audio has been silence -- its squelch closed, or the dial
 * out of its reach -- 0 while it sounds (sdr_rx_audible). */
static int64_t s_silent_since;

static esp_timer_handle_t s_save_timer;
/* The list a redirect moved -- a receiver https:// from now on -- not in
 * flash yet: written at a quiet moment by its own timer (list_cb). The list
 * in flash is written by that and by sdr_save, the web server's: one at a
 * time, each from the list it takes to its write (`s_list_writing`, under
 * s_lock), so an older list never lands in flash after a newer one. */
static esp_timer_handle_t s_list_timer;
static volatile bool      s_list_dirty;
EXT_RAM_BSS_ATTR static bool s_list_writing;

#define WAIT_RADIO   "waiting for the radio"
#define LEFT_EAR     "in the left ear"      /* the kiwi firmware's own has it: waited out */
#define OUT_OF_RANGE "out of range"         /* it cannot reach the dial: quiet, the session kept */

/* ------------------------------------------------------------------ list */

uint32_t sdr_hp(const sdr_cfg_t *c) { return c ? kiwi_hp(c->host, c->kport ? c->kport : c->port) : 0; }

bool sdr_parse_addr(const char *in, sdr_cfg_t *c)
{
    if (!c) return false;
    uint16_t port = 8073;
    bool tls = false;
    if (!kiwi_url(in, 8073, c->host, sizeof c->host, &port, &tls)) return false;
    c->port = port;
    c->tls = tls;
    c->kport = 0;
    return true;
}

bool sdr_same(const sdr_cfg_t *old, sdr_cfg_t *c)
{
    if (!old || !c || strcasecmp(old->host, c->host)) return false;
    if (old->port == c->port && old->tls == c->tls) {
        c->kport = old->kport;
        return true;
    }
    /* At the address an https:// redirect moved it from, as a page loaded
     * before the move still shows it: the move kept, and its key. */
    if (old->tls && old->kport && !c->tls && c->port == old->kport) {
        c->port = old->port;
        c->tls = true;
        c->kport = old->kport;
        return true;
    }
    return false;
}

static void list_parse(const char *blob)
{
    int n = 0;
    const char *p = blob;
    while (p && *p && n < SDR_MAX) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        char line[LIST_LINE];
        if (len >= sizeof line) len = sizeof line - 1;
        memcpy(line, p, len);
        line[len] = 0;
        /* name \t host \t port \t pass \t ipl, empty fields kept. An
         * https:// receiver has its scheme before its host, and one an
         * https:// redirect moved the port it is known by after its own
         * ("443/80"): a list from before reads as it did. */
        char *f[5] = { line, "", "", "", "" };
        int k = 1;
        for (char *q = line; *q && k < 5; q++)
            if (*q == '\t') { *q = 0; f[k++] = q + 1; }
        const bool tls = !strncasecmp(f[1], "https://", 8);
        const char *host = tls ? f[1] + 8 : !strncasecmp(f[1], "http://", 7) ? f[1] + 7 : f[1];
        if (host[0]) {
            sdr_cfg_t *c = &s_list[n++];
            memset(c, 0, sizeof *c);
            strlcpy(c->name, f[0], sizeof c->name);
            strlcpy(c->host, host, sizeof c->host);
            char *e;
            c->port = (uint16_t)strtoul(f[2], &e, 10);
            if (*e == '/') c->kport = (uint16_t)strtoul(e + 1, NULL, 10);
            c->tls = tls;
            if (!c->port) c->port = tls ? 443 : 8073;
            if (c->kport == c->port) c->kport = 0;
            strlcpy(c->pass, f[3], sizeof c->pass);
            strlcpy(c->ipl, f[4], sizeof c->ipl);
            s_hp[n - 1] = sdr_hp(c);
        }
        p = eol ? eol + 1 : NULL;
    }
    s_n = n;
}

static void list_cb(void *arg);

esp_err_t sdr_list_init(void)
{
    /* The timer that takes a list a redirect moved to flash (list_cb), with
     * the list: the kiwi firmware's left ear may move one before the right
     * ear's task is up. */
    const esp_timer_create_args_t ta = { .callback = list_cb, .name = "sdrlist" };
    if (!s_list_timer) esp_timer_create(&ta, &s_list_timer);
    if (s_list_loaded) return ESP_OK;
    char who[KIWI_IDENT_MAX] = "";
    kv_handle_t h;
    if (kv_open(NVS_NS, &h) == ESP_OK) {
        size_t n = 0;
        if (kv_get_str(h, KEY_LIST, NULL, &n) == ESP_OK && n > 1) {
            char *blob = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
            if (blob && kv_get_str(h, KEY_LIST, blob, &n) == ESP_OK) {
                taskENTER_CRITICAL(&s_lock);
                list_parse(blob);
                taskEXIT_CRITICAL(&s_lock);
            }
            free(blob);
        }
        n = sizeof who;
        if (kv_get_str(h, KEY_IDENT, who, &n) != ESP_OK) who[0] = 0;
        kv_close(h);
    }
    /* Who the owners see, for every session from the first. */
    kiwi_ident_set(who);
    if (who[0]) ESP_LOGI(TAG, "the receivers' owners see the knob as \"%s\"", who);
    s_list_loaded = true;
    return ESP_OK;
}

void sdr_ident(char *out, size_t cap) { kiwi_ident(out, cap); }

esp_err_t sdr_ident_save(const char *who)
{
    /* Printable, and no spaces at either end: the receiver lists it as it is. */
    char w[KIWI_IDENT_MAX];
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)(who ? who : ""); *p && n + 1 < sizeof w; p++)
        if (*p >= 0x20 && *p != 0x7F && (n || *p != ' ')) w[n++] = (char)*p;
    /* A UTF-8 character cut at the end is left out whole. */
    size_t k = n;
    while (k && ((unsigned char)w[k - 1] & 0xC0) == 0x80) k--;
    if (k && ((unsigned char)w[k - 1] & 0xC0) == 0xC0) {
        const unsigned char lead = (unsigned char)w[k - 1];
        const size_t want = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : 2;
        if (n - (k - 1) < want) n = k - 1;
    }
    while (n && w[n - 1] == ' ') n--;
    w[n] = 0;
    char was[KIWI_IDENT_MAX];
    kiwi_ident(was, sizeof was);
    if (!strcmp(was, w)) return ESP_OK;
    kv_handle_t h;
    esp_err_t e = kv_open(NVS_NS, &h);
    if (e == ESP_OK) {
        e = w[0] ? kv_set_str(h, KEY_IDENT, w) : kv_erase_key(h, KEY_IDENT);
        if (e == ESP_ERR_NVS_NOT_FOUND) e = ESP_OK;         /* none, and none there */
        if (e == ESP_OK) e = kv_commit_wait(h, 3000);
        if (e == ESP_ERR_TIMEOUT) e = ESP_OK;               /* on its way */
        kv_close(h);
    }
    if (e != ESP_OK) return e;
    kiwi_ident_set(w);
    ESP_LOGI(TAG, "the receivers' owners see the knob as \"%s\"", w[0] ? w : KIWI_IDENT_DEFAULT);
    return ESP_OK;
}

uint32_t sdr_list_gen(void) { return s_list_gen; }

int sdr_count(void) { return s_n; }

bool sdr_get(int i, sdr_cfg_t *out)
{
    if (i < 0 || i >= s_n || !out) return false;
    taskENTER_CRITICAL(&s_lock);
    *out = s_list[i];
    taskEXIT_CRITICAL(&s_lock);
    return true;
}

static bool clean(const char *s)
{
    for (; *s; s++) if (*s == '\t' || *s == '\n' || *s == '\r') return false;
    return true;
}

static void save_later(void);

/* The one listened to, said in the log: `i` in the list as it is now. */
static void say_sel(int i)
{
#if VFO_RADIO_KIWI
    ESP_LOGI(TAG, "the right ear: %s", i < 0 ? "off" : s_list[i].name[0] ? s_list[i].name : s_list[i].host);
#else
    ESP_LOGI(TAG, "listening to %s", i < 0 ? "the radio alone" : s_list[i].name[0] ? s_list[i].name : s_list[i].host);
#endif
}

/* The list as NVS keeps it, a line a receiver (list_parse): those with a
 * host and no tab or line break in a field. `kept[i]`: list[i]'s place among
 * those written, -1 left out (`kept` may be NULL). */
static void list_blob(const sdr_cfg_t *list, int n, char *blob, size_t cap, int *kept)
{
    size_t o = 0;
    int k = 0;
    blob[0] = 0;
    for (int i = 0; i < n; i++) {
        const sdr_cfg_t *c = &list[i];
        if (kept) kept[i] = -1;
        if (!c->host[0] || !clean(c->name) || !clean(c->host) || !clean(c->pass) || !clean(c->ipl)) continue;
        char kp[8] = "";
        if (c->kport && c->kport != c->port) snprintf(kp, sizeof kp, "/%u", (unsigned)c->kport);
        const int w = snprintf(blob + o, cap - o, "%s\t%s%s\t%u%s\t%s\t%s\n", c->name, c->tls ? "https://" : "",
                               c->host, (unsigned)c->port, kp, c->pass, c->ipl);
        if (w < 0 || (size_t)w >= cap - o) {
            blob[o] = 0;                        /* cannot happen: a line is under LIST_LINE */
            break;
        }
        o += (size_t)w;
        if (kept) kept[i] = k;
        k++;
    }
}

/* The list to the settings (kvstore): a page's save waits until it is
 * written (a timeout is as good as saved), a timer's does not. */
static esp_err_t list_nvs(const char *blob, bool wait)
{
    kv_handle_t h;
    esp_err_t e = kv_open(NVS_NS, &h);
    if (e == ESP_OK) {
        e = kv_set_str(h, KEY_LIST, blob);
        if (e == ESP_OK) e = wait ? kv_commit_wait(h, 3000) : kv_commit(h);
        if (e == ESP_ERR_TIMEOUT) e = ESP_OK;
        kv_close(h);
    }
    return e;
}

/* The list's turn to be written to flash: true, taken; false while the other
 * writer has it (s_list_writing). */
static bool list_write_take(void)
{
    taskENTER_CRITICAL(&s_lock);
    const bool got = !s_list_writing;
    if (got) s_list_writing = true;
    taskEXIT_CRITICAL(&s_lock);
    return got;
}

static void list_write_give(void)
{
    taskENTER_CRITICAL(&s_lock);
    s_list_writing = false;
    taskEXIT_CRITICAL(&s_lock);
}

esp_err_t sdr_save(const sdr_cfg_t *list, int n, int sel)
{
    if (n < 0 || n > SDR_MAX) return ESP_ERR_INVALID_ARG;
    char *blob = heap_caps_calloc(1, SDR_MAX * LIST_LINE + 1, MALLOC_CAP_SPIRAM);
    if (!blob) return ESP_ERR_NO_MEM;
    int kept[SDR_MAX];
    list_blob(list, n, blob, SDR_MAX * LIST_LINE + 1, kept);
    const int at = sel >= 0 && sel < n ? kept[sel] : -1;    /* list[sel]'s place among those kept */
    /* A marked receiver given a new time-limit password: what the operator
     * does to listen again, so it counts as choosing it again -- one counted
     * try, never a fresh count. A wrong password is refused like any login,
     * and lifting the mark for each would let mistyped ones reach the five. */
    uint32_t fresh_ipl = 0;
    for (int i = 0; i < n; i++) {
        if (!list[i].ipl[0]) continue;
        const uint32_t hp = sdr_hp(&list[i]);
        bool same = false;
        for (int j = 0; j < s_n; j++)          /* only sdr_save writes s_list's entries whole */
            if (s_hp[j] == hp && !strcmp(s_list[j].ipl, list[i].ipl)) same = true;
        if (!same && kiwi_mark_get(hp, NULL)) fresh_ipl = hp;
    }
    /* A list a redirect moved, on its way to flash (list_cb): written first,
     * and this one over it -- and that one waits until this is the list. */
    while (!list_write_take()) vTaskDelay(pdMS_TO_TICKS(10));
    const esp_err_t e = list_nvs(blob, true);
    if (e == ESP_OK) {
        taskENTER_CRITICAL(&s_lock);
        const int was = s_want;
        const uint32_t was_hp = was >= 0 && was < s_n ? s_hp[was] : 0;
        list_parse(blob);
        s_list_gen++;
        s_list_dirty = false;                   /* this list is the one in flash now */
        /* The one listened to, set with the list: no task ever reads the new
         * list with the old place, nor with none meanwhile -- the kiwi
         * firmware's left ear, its own receiver gone from the list, picking
         * the first the right ear does not keep (sdr_rx_uses). */
        s_want = at >= 0 && at < s_n ? at : -1;
        const bool moved = s_want != was || (s_want >= 0 ? s_hp[s_want] : 0) != was_hp;
        if (fresh_ipl) {
            s_chosen_hp = fresh_ipl;
            s_chosen_marked = true;
            s_chosen++;
        }
        taskEXIT_CRITICAL(&s_lock);
        list_write_give();
        ESP_LOGI(TAG, "%d receiver%s saved", s_n, s_n == 1 ? "" : "s");
        if (fresh_ipl) ESP_LOGI(TAG, "a new time-limit password for a day-limited receiver: as chosen again");
        if (moved) {
            say_sel(s_want);
            save_later();
        }
    } else {
        list_write_give();
    }
    free(blob);
    return e;
}

void sdr_moved(uint32_t hp, uint16_t tls_port)
{
    char host[64] = "";
    taskENTER_CRITICAL(&s_lock);
    for (int i = 0; hp && tls_port && i < s_n; i++) {
        sdr_cfg_t *c = &s_list[i];
        if (s_hp[i] != hp || c->tls) continue;
        /* Known by the port it had: its key, and all the knob keeps under
         * it, stay its. */
        if (!c->kport) c->kport = c->port;
        c->port = tls_port;
        c->tls = true;
        if (c->kport == c->port) c->kport = 0;
        strlcpy(host, c->host, sizeof host);
        s_list_dirty = true;
    }
    taskEXIT_CRITICAL(&s_lock);
    if (!host[0]) return;
    ESP_LOGI(TAG, "%s: https:// on port %u from now on -- to flash at a quiet moment", host, (unsigned)tls_port);
    if (s_list_timer && !esp_timer_is_active(s_list_timer)) esp_timer_start_once(s_list_timer, 1000);
}

/* A list a redirect moved, to flash -- at a quiet moment: no receiver's
 * audio playing, in either ear, and nothing on the air; looked for every
 * LIST_LOOK_US for as long as it takes, as a flash write holds the audio's
 * interrupts up. Beside a radio that is the web SDR's audio: the radio's own
 * receive audio may play on, as for the knob's other settings there. A
 * restart before then loses nothing that matters: the redirect is followed
 * again. A list being saved from the page meanwhile is the newer: this waits
 * for it, and finds nothing left to write. From the timer task, which may
 * touch flash. */
static void list_cb(void *arg)
{
    (void)arg;
    EXT_RAM_BSS_ATTR static sdr_cfg_t list[SDR_MAX];
    bool (*const audible)(void) = s_audible;
    if (sdr_rx_audible() || (audible && audible()) || kiwi_mark_busy() || !list_write_take()) {
        esp_timer_start_once(s_list_timer, LIST_LOOK_US);
        return;
    }
    char *blob = heap_caps_calloc(1, SDR_MAX * LIST_LINE + 1, MALLOC_CAP_SPIRAM);
    if (!blob) {
        list_write_give();
        esp_timer_start_once(s_list_timer, LIST_LOOK_US);
        return;
    }
    taskENTER_CRITICAL(&s_lock);
    const bool dirty = s_list_dirty;
    const int n = s_n;
    memcpy(list, s_list, sizeof list);
    s_list_dirty = false;
    taskEXIT_CRITICAL(&s_lock);
    if (dirty) {
        list_blob(list, n, blob, SDR_MAX * LIST_LINE + 1, NULL);
        const esp_err_t e = list_nvs(blob, false);
        if (e == ESP_OK) ESP_LOGI(TAG, "the receivers in flash, https:// kept");
        else ESP_LOGW(TAG, "the receivers not saved (%s): https:// kept until a restart", esp_err_to_name(e));
    }
    list_write_give();
    free(blob);
}

/* The selection -- its place and its address -- and the balance, saved from
 * the timer task: the SDR task's stack is in PSRAM and may not touch flash,
 * and the dial's task has none to spare. On the kiwi firmware, at a quiet
 * moment, or 30 s after the change while its audio plays on. */
static void save_cb(void *arg)
{
    (void)arg;
    bool (*const audible)(void) = s_audible;
    if (audible && audible() && esp_timer_get_time() - s_t_change < SAVE_LATER_US) {
        esp_timer_start_once(s_save_timer, 1000 * 1000);     /* a look again in a second */
        return;
    }
    taskENTER_CRITICAL(&s_lock);
    const int8_t   sel  = (int8_t)s_want;
    const uint32_t selh = s_want >= 0 && s_want < s_n ? s_hp[s_want] : 0;
    taskEXIT_CRITICAL(&s_lock);
    kv_handle_t h;
    if (kv_open(NVS_NS, &h) != ESP_OK) return;
    kv_edit_begin(h);
    kv_set_i8(h, KEY_SEL, sel);
    kv_set_u32(h, KEY_SELH, selh);
    kv_set_i8(h, KEY_BAL, s_bal);
    kv_edit_end(h);
    kv_commit(h);
    kv_close(h);
}

static void save_later(void)
{
    s_t_change = esp_timer_get_time();
    if (!s_save_timer) return;
    esp_timer_stop(s_save_timer);
    esp_timer_start_once(s_save_timer, 2 * 1000 * 1000);   /* settle first */
}

void sdr_rx_select(int i)
{
    if (i < -1 || i >= s_n) i = -1;
    if (i == s_want) return;
    s_want = i;
    say_sel(i);
    save_later();
}

bool sdr_rx_choose(int i)
{
    if (i < -1 || i >= s_n) i = -1;
    taskENTER_CRITICAL(&s_lock);
    const uint32_t hp = i >= 0 ? s_hp[i] : 0;
    taskEXIT_CRITICAL(&s_lock);
    /* The left ear's: never in both ears -- unless it is this one's already,
     * a list saved under both, which the left ear has meanwhile anyway. One
     * the left ear is only leaving is chosen, and waited out. */
    bool (*const primary)(uint32_t, bool) = s_primary;
    if (hp && i != s_want && primary && primary(hp, true)) {
        ESP_LOGW(TAG, "%s plays in the left ear: not in the right as well",
                 s_list[i].name[0] ? s_list[i].name : s_list[i].host);
        return false;
    }
    const bool marked = hp && kiwi_mark_get(hp, NULL);
    taskENTER_CRITICAL(&s_lock);
    s_chosen_hp = hp;
    s_chosen_marked = marked;
    s_chosen++;
    taskEXIT_CRITICAL(&s_lock);
    if (i >= 0 && i == s_want)
        ESP_LOGI(TAG, "%s chosen again", s_list[i].name[0] ? s_list[i].name : s_list[i].host);
    sdr_rx_select(i);
    return true;
}

int sdr_rx_selected(void) { return s_want; }

void sdr_rx_primary_cb(bool (*uses)(uint32_t hp, bool chosen)) { s_primary = uses; }
void sdr_rx_quiet_cb(bool (*audible)(void)) { s_audible = audible; }
void sdr_rx_settings_cb(void (*get)(uint8_t *agc, uint8_t *nr, uint8_t *sq_pct)) { s_settings = get; }

bool sdr_rx_audible(void)
{
    const int64_t now = esp_timer_get_time();
    taskENTER_CRITICAL(&s_lock);
    const bool a = s_run.on && (!s_silent_since || now - s_silent_since < QUIET_US);
    taskEXIT_CRITICAL(&s_lock);
    return a;
}

/* The left ear on this receiver -- in use there, or its session still on it. */
static bool primary_has(uint32_t hp)
{
    bool (*const primary)(uint32_t, bool) = s_primary;
    return hp && primary && primary(hp, false);
}

static look_t *look_find(uint32_t hp);

bool sdr_host_shared(int i)
{
    bool shared = false;
    taskENTER_CRITICAL(&s_lock);
    for (int j = 0; i >= 0 && i < s_n && j < s_n; j++)
        if (j != i && !strcasecmp(s_list[j].host, s_list[i].host)) shared = true;
    taskEXIT_CRITICAL(&s_lock);
    return shared;
}

bool sdr_rx_label(int i, char *out, size_t cap)
{
    sdr_cfg_t c;
    if (!out || !cap || !sdr_get(i, &c)) return false;
    char ant[48] = "";
    const uint32_t hp = sdr_hp(&c);
    taskENTER_CRITICAL(&s_lock);
    const look_t *L = look_find(hp);
    if (L && L->ok) strlcpy(ant, L->st.antenna, sizeof ant);
    taskEXIT_CRITICAL(&s_lock);
    kiwi_label(out, cap, c.name, ant, c.host, kiwi_label_port(c.port, c.tls), sdr_host_shared(i));
    return true;
}

void sdr_rx_set_balance(int8_t b)
{
    if (b < -100) b = -100;
    if (b > 100)  b = 100;
    audio_out_set_balance(b);
    if (b == s_bal) return;
    s_bal = b;
    save_later();
}

int8_t sdr_rx_balance(void) { return s_bal; }

void sdr_rx_tune(int64_t hz, const char *mode, int32_t lo, int32_t hi)
{
    if (!mode) mode = "";
    taskENTER_CRITICAL(&s_lock);
    s_tune.hz = hz;
    s_tune.lo = lo;
    s_tune.hi = hi;
    strlcpy(s_tune.mode, mode, sizeof s_tune.mode);
    taskEXIT_CRITICAL(&s_lock);
}

void sdr_rx_status(sdr_status_t *out)
{
    if (!out) return;
    taskENTER_CRITICAL(&s_lock);
    *out = s_st;
    taskEXIT_CRITICAL(&s_lock);
}

/* The trouble in a word or two, for the dial: what state says at length. */
static const char *note_of(const char *state)
{
    static const struct { const char *state, *note; } N[] = {
        { "busy", "busy" },                  { "app channels in use", "busy" },
        { "apps full, held", "busy" },       { "no apps allowed", "no apps" },
        { "wrong password", "password?" },   { "daily limit reached", "day limit" },
        { "down", "down" },                  { "name not found", "not found" },
        { "no route", "no route" },          { "no free socket", "no socket" },
        { "time up", "time up" },            { "kicked", "kicked" },
        { "refused", "refused" },            { "memory full", "memory full" },
        { "try later", "try later" },        { "not a kiwi", "not a kiwi" },
        { "one per address", "one per ip" }, { "updating", "updating" },
        { LEFT_EAR, "left ear" },            { OUT_OF_RANGE, "can't reach" },
        { "certificate not valid", "certificate" },
    };
    for (size_t i = 0; i < sizeof N / sizeof N[0]; i++)
        if (!strcmp(state, N[i].state)) return N[i].note;
    if (!strncmp(state, "moved", 5)) return "moved";        /* "moved to <host>" */
    return "no answer";                      /* closed, went quiet ... */
}

/* How a session ended, as the pages and the radio page say it. */
static const char *state_of(kiwi_end_t e)
{
    switch (e) {
    case KIWI_END_NOT_FOUND: return "name not found";
    case KIWI_END_NO_ROUTE:  return "no route";
    case KIWI_END_NO_SOCKET: return "no free socket";
    case KIWI_END_CLOSED:    return "closed";
    case KIWI_END_QUIET:     return "went quiet";
    case KIWI_END_REFUSED:   return "refused";
    case KIWI_END_NOT_KIWI:  return "not a kiwi";
    case KIWI_END_PASSWORD:  return "wrong password";
    case KIWI_END_DUP_IP:    return "one per address";
    case KIWI_END_UPDATING:  return "updating";
    case KIWI_END_TRY_LATER: return "try later";
    case KIWI_END_FULL:
    case KIWI_END_PWD_FULL:  return "busy";
    case KIWI_END_SILENT:
    case KIWI_END_APPS_FULL: return "app channels in use";
    case KIWI_END_NO_APPS:   return "no apps allowed";
    case KIWI_END_DAY_LIMIT: return "daily limit reached";
    case KIWI_END_IDLE:      return "time up";
    case KIWI_END_KICKED:    return "kicked";
    case KIWI_END_DOWN:      return "down";
    case KIWI_END_NO_FLASH:  return "memory full";
    case KIWI_END_MOVED:     return "moved";
    case KIWI_END_CERT:      return "certificate not valid";
    default:                 return "no answer";
    }
}

/* ...and where it was sent, for one that moved: "moved to <host>". */
static const char *state_to(kiwi_end_t e, const kiwi_said_t *said, char *buf, size_t cap)
{
    if (e != KIWI_END_MOVED || !said->moved.to[0]) return state_of(e);
    snprintf(buf, cap, "moved to %.38s", said->moved.to);
    return buf;
}

static void set_state(int sel, const char *state, const char *name)
{
    taskENTER_CRITICAL(&s_lock);
    s_st.sel = sel;
    strlcpy(s_st.state, state, sizeof s_st.state);
    if (name) strlcpy(s_st.name, name, sizeof s_st.name);
    s_st.streaming = strcmp(state, "streaming") == 0;
    /* Everything but on its way, or waiting its turn, is trouble. */
    s_st.trouble = sel >= 0 && !s_st.streaming && strcmp(state, "connecting") &&
                   strcmp(state, "logged in") && strcmp(state, WAIT_RADIO);
    strlcpy(s_st.note, s_st.trouble ? note_of(state) : "", sizeof s_st.note);
    taskEXIT_CRITICAL(&s_lock);
}

/* ------------------------------------------------------------- /status */

static look_t *look_find(uint32_t hp)
{
    for (int i = 0; hp && i < SDR_MAX; i++)
        if (s_look[i].hp == hp) return &s_look[i];
    return NULL;
}

/* Whether this receiver is in the list as it is now: what the knob keeps of
 * one that is not is the first to go when its tables are full. */
static bool listed(uint32_t hp)
{
    bool in = false;
    taskENTER_CRITICAL(&s_lock);
    for (int j = 0; j < s_n; j++) in |= s_hp[j] == hp;
    taskEXIT_CRITICAL(&s_lock);
    return in;
}

/* Its place: its own, else an empty one, else one of a receiver no longer
 * in the list, else the oldest read. */
static look_t *look_get(uint32_t hp)
{
    look_t *L = look_find(hp);
    if (L) return L;
    L = &s_look[0];
    for (int i = 0; i < SDR_MAX; i++) {
        if (!s_look[i].hp) { L = &s_look[i]; break; }
        if (!listed(s_look[i].hp)) { L = &s_look[i]; break; }
        if (s_look[i].t_read < L->t_read) L = &s_look[i];
    }
    taskENTER_CRITICAL(&s_lock);
    memset(L, 0, sizeof *L);
    L->hp = hp;
    taskEXIT_CRITICAL(&s_lock);
    return L;
}

/* What its /status said this boot, if it was read as a Kiwi's: for a mark,
 * with how long ago (the knob's clock). */
static const kiwi_status_t *known(const look_t *L) { return L && L->ok ? &L->st : NULL; }
static int64_t known_age(const look_t *L) { return L ? esp_timer_get_time() - L->t_ok : 0; }

/* The receiver whose /status is being read (ctx: its place) is still the
 * one chosen: another chosen meanwhile is not kept waiting. */
static bool still_wanted(void *ctx) { return s_want == (int)(intptr_t)ctx; }

/* Its /status, which may lift a day-limit mark, says what it is (a Web-888's
 * S-meter is biased otherwise, and only a KiwiSDR shuts its door for apps in
 * silence) and whether it lets apps in. A marked one's at most once a
 * minute, however often it is chosen. A read that fails leaves what an
 * earlier one said, and when; one let go of for another receiver is no
 * read at all -- its once a boot is still to come. On the kiwi firmware one
 * the left ear has read takes the place of this one's own: the first look,
 * and a marked one's within the minute (kiwi_status_kept) -- once a boot
 * between the two ears. An http:// one's redirect to https:// on its own
 * host, followed by the read, is kept from now (sdr_moved) -- and in `c`,
 * for the session that follows. */
static void look(sdr_cfg_t *c, int want, look_t *L, bool for_mark)
{
    const int64_t now = esp_timer_get_time();
    if (for_mark && L->tried && now - L->t_read < LOOK_GAP_US) return;
    int64_t at = 0;
    if ((!L->tried || for_mark) && kiwi_status_kept(L->hp, &s_reading, &at) && (!for_mark || now - at < LOOK_GAP_US)) {
        taskENTER_CRITICAL(&s_lock);
        L->tried = true;
        L->again = false;
        L->t_read = L->t_ok = at;
        L->ok = s_reading.ok;
        L->st = s_reading;
        taskEXIT_CRITICAL(&s_lock);
        ESP_LOGI(TAG, "%s:%u: its /status as read %lld s ago", c->host, (unsigned)c->port,
                 (long long)((now - at) / 1000000));
        return;
    }
    const kiwi_addr_t ad = sdr_addr(c);
    kiwi_moved_t mv;
    const char *why = kiwi_status_read(&ad, &s_reading, &mv, still_wanted, (void *)(intptr_t)want, TAG);
    if (mv.tls_port && !c->tls) {
        sdr_moved(L->hp, mv.tls_port);
        if (!c->kport) c->kport = c->port;
        c->port = mv.tls_port;
        c->tls = true;
    }
    if (why && !strcmp(why, "left")) return;
    /* Under the lock: the dial reads its antenna (sdr_rx_label). */
    taskENTER_CRITICAL(&s_lock);
    L->tried = true;
    L->again = false;
    L->t_read = now;
    if (!why) {
        L->ok = s_reading.ok;
        L->st = s_reading;
        L->t_ok = now;
    }
    taskEXIT_CRITICAL(&s_lock);
    if (!why) kiwi_status_keep(L->hp, &s_reading);
    if (why) {
        ESP_LOGW(TAG, "%s:%u: no /status (%s%s%s)", c->host, (unsigned)c->port, why, mv.to[0] ? " to " : "",
                 mv.to);
        return;
    }
    if (!s_reading.ok) {
        ESP_LOGW(TAG, "%s:%u: its /status is not a Kiwi's", c->host, (unsigned)c->port);
        return;
    }
    kiwi_mark_check(L->hp, &s_reading, TAG);
}

/* ------------------------------------------------------- the session's side */

/* The radio, as the receiver follows it: its mode by the Kiwi's name; the
 * AGC, the noise filter and the squelch the receiver's own defaults -- the
 * AGC at MED, none, open -- or on the kiwi firmware the left ear's, asked
 * outside the lock, as that ear takes one of its own. */
static void cb_ctl(void *ctx, kiwi_ctl_t *out)
{
    (void)ctx;
    char mode[8];
    taskENTER_CRITICAL(&s_lock);
    out->t.hz = s_tune.hz;
    out->t.lo = s_tune.lo;
    out->t.hi = s_tune.hi;
    strlcpy(mode, s_tune.mode, sizeof mode);
    taskEXIT_CRITICAL(&s_lock);
    strlcpy(out->t.mode, kiwi_mode_of(mode), sizeof out->t.mode);
    out->agc = KIWI_AGC_MED;
    out->nr = KIWI_NR_OFF;
    out->sq_pct = 0;
    void (*const get)(uint8_t *, uint8_t *, uint8_t *) = s_settings;
    if (get) get(&out->agc, &out->nr, &out->sq_pct);
}

/* Where it tunes: the radio owns the dial -- outside this, the receiver goes
 * quiet (cb_reach), and the pages say what it covers. */
static void cb_range(void *ctx, int64_t lo, int64_t hi)
{
    (void)ctx;
    taskENTER_CRITICAL(&s_lock);
    s_st.lo_hz = lo;
    s_st.hi_hz = hi;
    taskEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "it tunes %lld..%lld Hz", (long long)lo, (long long)hi);
}

/* The session, as the pages and the dial say it: on its way, logged in,
 * playing -- or quiet, the dial out of its receiver's reach. */
static void say_run(void)
{
    set_state(s_run.want, s_run.out ? OUT_OF_RANGE : s_run.st == KIWI_ST_CONNECTING ? "connecting"
                          : s_run.st == KIWI_ST_LOGGED_IN ? "logged in" : "streaming", NULL);
}

/* The dial out of the receiver's reach, or back within it (kiwi_sess.h's
 * reach): silence meanwhile, the ring fed at the stream's pace, and its
 * session and channel kept -- no login again, no tune at every turn of the
 * dial; back within it, the tune and the audio at once. Said once a change. */
static void cb_reach(void *ctx, bool out)
{
    (void)ctx;
    const int64_t now = esp_timer_get_time();
    taskENTER_CRITICAL(&s_lock);
    const int64_t hz = s_tune.hz, lo = s_st.lo_hz, hi = s_st.hi_hz;
    /* Out, silence from here and no reading -- back within it, S0 until its
     * first one, never the dial's of before nor a peak of nobody's. */
    if (out) {
        if (!s_silent_since) s_silent_since = now;
        s_st.smeter_dbm = -127.0f;
    } else {
        s_silent_since = 0;
    }
    taskEXIT_CRITICAL(&s_lock);
    s_run.out = out;
    if (out)
        ESP_LOGW(TAG, "%lld Hz is out of its range, %lld..%lld Hz: quiet until the dial is back", (long long)hz,
                 (long long)lo, (long long)hi);
    else
        ESP_LOGI(TAG, "%lld Hz is within its range again: tuned, and playing", (long long)hz);
    say_run();
}

/* The right ear, from its first audio on. A full ring lets the rest go:
 * lost audio, counted as such. */
static void cb_audio(void *ctx, const int16_t *pcm, size_t n)
{
    (void)ctx;
    if (!s_run.on) {
        audio_out_sdr(true);
        s_run.on = true;
    }
    if (!audio_out_feed_sdr(pcm, n)) s_lost++;
}

static size_t cb_queued(void *ctx)
{
    (void)ctx;
    return audio_out_sdr_queued();
}

/* The target the network has set: the ring waits for that much after a gap. */
static void cb_preroll(void *ctx, size_t n)
{
    (void)ctx;
    audio_out_sdr_set_preroll(n);
}

/* The times the ring ran dry, its playing stopped: what is heard as a break. */
static uint32_t cb_underruns(void *ctx)
{
    (void)ctx;
    audio_stats_t a;
    audio_out_sdr_stats(&a);
    return a.underruns;
}

static void report_from_now(void)
{
    audio_stats_t a;
    audio_out_sdr_stats(&a);
    s_rep_lost  = s_counts.dropped + s_lost;
    s_rep_under = a.underruns;
}

/* Every 30 s while it plays: how the audio came, and what the right ear's
 * ring made of it, as the kiwi firmware's own receiver says it of the left. */
static void cb_report(void *ctx, const kiwi_report_t *r)
{
    (void)ctx;
    audio_stats_t a;
    audio_out_sdr_stats(&a);
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

/* Its reading, and its squelch: closed, silence from this frame on. */
static void cb_meter(void *ctx, float dbm, bool ovl, bool squelched)
{
    (void)ctx;
    (void)ovl;
    const int64_t now = esp_timer_get_time();
    taskENTER_CRITICAL(&s_lock);
    s_st.smeter_dbm = dbm;
    if (!squelched) s_silent_since = 0;
    else if (!s_silent_since) s_silent_since = now;
    taskEXIT_CRITICAL(&s_lock);
}

static void cb_state(void *ctx, kiwi_state_t st, const kiwi_said_t *said)
{
    (void)ctx;
    (void)said;
    if (st == KIWI_ST_STREAMING) report_from_now();
    s_run.st = st;
    say_run();
}

/* On, until another receiver is chosen or the list is saved -- or, on the
 * kiwi firmware, the left ear takes this one. */
static bool cb_go_on(void *ctx)
{
    (void)ctx;
    return s_want == s_run.want && s_list_gen == s_run.gen && !primary_has(s_run.hp);
}

/* Its redirect to https:// on its own host, followed on the session's way
 * in: kept from now, under its key. */
static void cb_moved(void *ctx, uint16_t tls_port)
{
    (void)ctx;
    sdr_moved(s_run.hp, tls_port);
}

/* The receiver's clock followed with the drift trim, so the ring stays at
 * its target -- and comes back down to it once a grown one eases -- and a
 * backlog's jump to the live point. The ring's figures are audio_out's
 * (sdr_task). */
static const kiwi_link_t LINK = {
    .ctl = cb_ctl, .range = cb_range, .reach = cb_reach, .audio = cb_audio, .queued = cb_queued, .trim = true,
    .preroll = cb_preroll, .underruns = cb_underruns, .meter = cb_meter, .state = cb_state, .go_on = cb_go_on,
    .report = cb_report, .moved = cb_moved, .counts = &s_counts, .tag = "sdr",
};

/* ------------------------------------------------------------ the task */

/* The knob's own attempts on a receiver, at most CAP_N in CAP_US -- a
 * courtesy to its owner, whatever ended them; only those that reached it,
 * though: an hour of WiFi gone is no reason to leave it alone once it is
 * back. 0 when one may go now, else how long until one may. */
static int cap_at(uint32_t hp)
{
    int at = -1;
    for (int i = 0; at < 0 && i < SDR_MAX; i++)
        if (s_cap[i].hp == hp) return i;
    for (int i = 0; at < 0 && i < SDR_MAX; i++)
        if (!s_cap[i].hp || !listed(s_cap[i].hp)) at = i;
    if (at < 0) at = 0;
    memset(&s_cap[at], 0, sizeof s_cap[at]);
    s_cap[at].hp = hp;
    return at;
}

static int64_t cap_wait(uint32_t hp)
{
    const int i = cap_at(hp);
    const int64_t oldest = s_cap[i].t[s_cap[i].head], now = esp_timer_get_time();
    return oldest && now - oldest < CAP_US ? CAP_US - (now - oldest) : 0;
}

static void cap_note(uint32_t hp, int64_t t)
{
    const int i = cap_at(hp);
    s_cap[i].t[s_cap[i].head] = t;
    s_cap[i].head = (uint8_t)((s_cap[i].head + 1) % CAP_N);
}

/* Up to `ms` (0: for as long as it takes), or until the operator does
 * something: another receiver, any act of choosing, a list saved. */
static void wait_event(uint32_t ms, int want, uint32_t ch, uint32_t gen)
{
    for (uint32_t t = 0; !ms || t < ms; t += 100) {
        if (s_want != want || s_chosen != ch || s_list_gen != gen) return;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

/* What holds the receiver back until the operator chooses it again: what
 * ended its last session (kiwi_end_t), said as the pages say it, and the
 * list it was in -- a wrong password, an address that is no Kiwi, one that
 * moved, a certificate refused and no apps let go when it is saved. */
typedef struct { uint32_t hp; kiwi_end_t end; uint32_t gen; char state[48]; } hold_t;

static bool cured_by_save(kiwi_end_t e)
{
    return e == KIWI_END_PASSWORD || e == KIWI_END_NOT_KIWI || e == KIWI_END_NO_APPS || e == KIWI_END_MOVED ||
           e == KIWI_END_CERT;
}

/* Held from now -- one receiver at a time here, the chosen one -- and kept
 * through a crash as well (kiwi_boot_hold_put): a restart nobody asked for
 * chooses nothing. The last one held is let go of -- through a crash too,
 * unless the kiwi firmware's left ear has that receiver now: what holds it
 * there through a crash may be that ear's. */
static void hold_set(hold_t *h, uint32_t hp, kiwi_end_t end, uint32_t gen, const char *state)
{
    if (h->hp && h->hp != hp && !primary_has(h->hp)) kiwi_boot_hold_put(h->hp, 0);
    *h = (hold_t){ .hp = hp, .end = end, .gen = gen };
    strlcpy(h->state, state ? state : state_of(end), sizeof h->state);
    kiwi_boot_hold_put(hp, (uint8_t)end);
}

static void sdr_task(void *arg)
{
    (void)arg;
    kiwi_sess_t *ss = kiwi_sess_new();
    if (!ss) {
        ESP_LOGE(TAG, "no memory for the session's buffers");
        set_state(-1, "memory full", "");
        vTaskDeleteWithCaps(NULL);          /* made WithCaps: its stack goes with it */
    }
    /* The right ear's ring: a backlog past 80 % of it left out in one jump,
     * back to its pre-roll -- which grows while the network keeps breaking
     * the stream up, the ring with room for it. */
    kiwi_link_t link = LINK;
    link.target     = audio_out_sdr_preroll();
    link.room       = audio_out_sdr_room();
    link.target_max = audio_out_sdr_preroll_max();
    uint32_t backoff = 2000, seen = 0;  /* every choice since boot counts, even one before this ran */
    uint32_t last_hp = 0;
    uint8_t  silent_n = 0;
    bool boot = true;                   /* the first look at the selection since boot */
    hold_t hold = { 0 };
    /* What held the selected receiver back when the knob crashed holds it
     * still: a restart nobody asked for chooses nothing. */
    if (s_want >= 0 && s_want < s_n) {
        const uint8_t kept = kiwi_boot_hold_get(s_hp[s_want]);
        if (kept) {
            hold.hp  = s_hp[s_want];
            hold.end = (kiwi_end_t)kept;
            hold.gen = s_list_gen;
            strlcpy(hold.state, state_of(hold.end), sizeof hold.state);
            ESP_LOGW(TAG, "%s before the restart -- not again until chosen again", state_of(hold.end));
        }
    }
    for (;;) {
        const int want = s_want;
        if (want < 0 || want >= s_n) {
            audio_out_sdr(false);
            set_state(-1, "off", "");
            backoff = 2000;
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        sdr_cfg_t c;
        if (!sdr_get(want, &c)) continue;       /* the list shorter since: looked at again */
        char label[24] = "";
        sdr_rx_label(want, label, sizeof label);
        /* Not before the radio says where it is: the receiver would play its
         * own default frequency meanwhile. */
        if (s_tune.hz <= 0) {
            set_state(want, WAIT_RADIO, label);
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        const uint32_t hp = sdr_hp(&c);
        /* The kiwi firmware's left ear has it -- a list saved under both, the
         * left ear's own gone from it: never both ears on one receiver. Not
         * even its /status: waited out, the choice kept for when it is free. */
        if (primary_has(hp)) {
            const uint32_t g0 = s_list_gen;
            set_state(want, LEFT_EAR, label);
            audio_out_sdr(false);
            while (s_want == want && s_list_gen == g0 && primary_has(hp)) vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        /* The operator's act, if one came for this receiver since the last
         * look: one attempt through whatever held it. A boot, a list save's
         * reshuffle or a retry is no such act. A try on one held for its day
         * limit only if it was held as it was chosen: a Test refused after
         * the choice holds it until the operator chooses it again. */
        taskENTER_CRITICAL(&s_lock);
        const bool chosen = s_chosen != seen && s_chosen_hp == hp;
        const bool tries = chosen && s_chosen_marked;
        if (chosen) seen = s_chosen;
        const uint32_t ch = s_chosen, gen = s_list_gen;
        taskEXIT_CRITICAL(&s_lock);
        if (hp != last_hp || chosen) {
            backoff = 2000;
            silent_n = 0;
        }
        last_hp = hp;
        /* After a run of crashes, each soon after the last start, the
         * receivers wait: the knob's own first contact this boot -- a status
         * read, a login -- comes no sooner than kiwi_boot_hold_us(). The
         * operator's choice goes at once. */
        const int64_t hold_until = kiwi_boot_hold_us();
        if (!chosen && esp_timer_get_time() < hold_until) {
            set_state(want, "try later", label);
            while (s_want == want && s_chosen == ch && s_list_gen == gen && esp_timer_get_time() < hold_until)
                vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        const bool first = boot;
        boot = false;

        if (hold.hp == hp && (chosen || (hold.gen != gen && cured_by_save(hold.end)))) {
            kiwi_boot_hold_put(hold.hp, 0);
            hold.hp = 0;
        }
        const char *held = hold.hp == hp ? hold.state : NULL;
        look_t *L = look_get(hp);
        bool paid = false;                      /* this login is a try, counted already */
        if (!held && kiwi_mark_get(hp, NULL)) {
            /* A day-limit mark: the receiver's own word first, which may
             * lift it; then a try, only when chosen again, and counted in
             * flash before the login it allows -- a restart in between must
             * not hand it back. */
            if (chosen || first) look(&c, want, L, true);
            /* The operator may have gone on meanwhile: a try is spent on the
             * receiver chosen, never on one left. */
            if (s_want != want) continue;
            kiwi_mark_t mk;
            if (kiwi_mark_get(hp, &mk)) {
                if (!tries || !kiwi_mark_try(hp, TAG)) {
                    /* Its login unanswered, it says what it last saw. */
                    held = !(mk.flags & KIWI_MARK_UNSURE) ? "daily limit reached"
                         : L->end == KIWI_END_SILENT || L->end == KIWI_END_APPS_FULL ? "apps full, held"
                         : "no answer, held";
                    if (chosen && !tries)
                        ESP_LOGW(TAG, "%s:%u: marked after it was chosen -- not again until chosen again",
                                 c.host, (unsigned)c.port);
                } else {
                    paid = true;
                    set_state(want, "connecting", label);
                    /* In flash before the login -- unless the flash cannot
                     * keep it, and then the operator's choice goes as it is. */
                    while (!kiwi_mark_saved() && kiwi_mark_durable() && s_want == want)
                        vTaskDelay(pdMS_TO_TICKS(100));
                    if (s_want != want) continue;
                }
            }
        }
        /* Its /status: once a boot before its first session; again only five
         * minutes on, after an end it may explain. Never in a retry loop.
         * The session is on its way from here: a Test of this receiver
         * meanwhile answers from it, with no login of its own beside it. */
        if (!held && (!L->tried || (L->again && esp_timer_get_time() - L->t_read >= STATUS_AGAIN_US))) {
            set_state(want, "connecting", label);
            look(&c, want, L, false);
            if (s_want != want) continue;
        }
        /* A KiwiSDR that lets no apps in: said, with no socket opened. */
        if (!held && L->ok && L->st.kind == KIWI_KIND_KIWISDR && L->st.ext_api == 0) {
            ESP_LOGW(TAG, "%s:%u: lets no apps listen -- not again until chosen again", c.host, (unsigned)c.port);
            hold_set(&hold, hp, KIWI_END_NO_APPS, gen, NULL);
            continue;
        }
        /* A flash that cannot keep a mark: a refusal now would be forgotten
         * at the next start, and the knob would log in by itself again, at
         * every start. So on its own it logs in only where no day limit can
         * count -- its /status, read once, says -- and the operator's choice
         * goes as it is. */
        if (!held && !chosen && !kiwi_mark_durable() && kiwi_mark_may_count(hp, known(L)))
            held = "memory full";
        if (held) {
            set_state(want, held, label);
            wait_event(0, want, ch, gen);
            continue;
        }
        /* The knob's own attempts, capped; the operator's go at once. */
        if (!chosen) {
            const int64_t w = cap_wait(hp);
            if (w > 0) {
                ESP_LOGW(TAG, "%s:%u: %d attempts in 10 minutes -- again in %lld s", c.host, (unsigned)c.port,
                         CAP_N, (long long)(w / 1000000));
                set_state(want, "try later", label);
                wait_event((uint32_t)(w / 1000) + 100, want, ch, gen);
                continue;
            }
        }

        /* On its way, said before a Test is looked at: one logging in to this
         * receiver now is waited for, and its answer read -- then the mark
         * again, as a refusal of the Test's holds the session back too. Two
         * logins at once could spend two strikes at once (kiwi_test_busy). */
        set_state(want, "connecting", label);
        while (kiwi_test_busy(hp) && s_want == want && s_list_gen == gen) vTaskDelay(pdMS_TO_TICKS(100));
        if (s_want != want || s_list_gen != gen) continue;
        if (!paid && kiwi_mark_get(hp, NULL)) {
            ESP_LOGW(TAG, "%s:%u: a Test's login marked it meanwhile", c.host, (unsigned)c.port);
            continue;
        }
        /* The left ear's meanwhile: said on its way above, then looked at,
         * as the left ear does the other way round -- one of the two always
         * sees the other. */
        if (primary_has(hp)) continue;
        kiwi_boot_hold_put(hp, 0);              /* logged in to: nothing holds it, crash or not */
        taskENTER_CRITICAL(&s_lock);
        s_run.want = want;
        s_run.hp = hp;
        s_run.gen = gen;
        s_run.on = false;
        s_run.live = true;
        s_run.st = KIWI_ST_CONNECTING;
        s_run.out = false;
        s_silent_since = 0;
        s_st.lo_hz = s_st.hi_hz = 0;            /* this receiver's range is its own to say */
        s_st.smeter_dbm = -127.0f;              /* ...and its reading: S0 until its first */
        taskEXIT_CRITICAL(&s_lock);
        const int64_t t_try = esp_timer_get_time();
        const kiwi_addr_t ad = sdr_addr(&c);
        kiwi_end_t end = kiwi_sess_run(ss, &ad, known(L), &link, &s_said);
        taskENTER_CRITICAL(&s_lock);
        s_run.live = false;
        taskEXIT_CRITICAL(&s_lock);
        /* Streamed -- quiet out of range or not -- long enough. */
        const bool streamed = s_run.st == KIWI_ST_STREAMING && esp_timer_get_time() - t_try >= STREAM_OK_US;
        s_run.out = false;
        /* An attempt of its own that reached the receiver counts -- not one
         * the knob ended itself, for a list saved under it. */
        if (!chosen && s_said.connected && end != KIWI_END_WANT) cap_note(hp, t_try);
        audio_out_sdr(false);
        s_run.on = false;
        L->end = (uint8_t)end;
        /* What was chosen while it played chose the receiver as it was: no
         * attempt through what ended it. */
        taskENTER_CRITICAL(&s_lock);
        if (s_chosen_hp == hp) seen = s_chosen;
        taskEXIT_CRITICAL(&s_lock);
        char said_st[48];
        if (end != KIWI_END_WANT) set_state(want, state_to(end, &s_said, said_st, sizeof said_st), NULL);
        /* A quiet moment, for a mark at rest -- on the kiwi firmware only with
         * the left ear quiet as well (sdr_rx_quiet_cb); else it waits, 30 s
         * at most (kiwi_mark.h). */
        bool (*const audible)(void) = s_audible;
        if (!audible || !audible()) kiwi_mark_flush();
        if (streamed) backoff = 2000;
        if (end != KIWI_END_SILENT) silent_n = 0;
        if (end == KIWI_END_NO_ANSWER || end == KIWI_END_SILENT || end == KIWI_END_APPS_FULL ||
            end == KIWI_END_NO_APPS)
            L->again = true;

        /* Its login left unanswered, by a receiver whose day limit may count
         * it -- its /status, read once this boot, says: one refusal, in case
         * its answer was one that never arrived. The mark holds it from now,
         * through restarts. */
        if (s_said.unanswered && kiwi_mark_may_count(hp, known(L))) {
            kiwi_mark_set(hp, KIWI_MARK_NO_ANSWER, paid, known(L), known_age(L), TAG);
            continue;
        }
        uint32_t wait = 0;
        switch (end) {
        case KIWI_END_WANT:
            backoff = 2000;
            continue;                           /* another receiver, or the list saved: at once */
        case KIWI_END_DAY_LIMIT:
            /* Its listening time per address per day, used up: marked, and
             * held by that from now on, through restarts. Said for the login,
             * a refusal the receiver counted. */
            kiwi_mark_set(hp, s_said.limit_at_login ? KIWI_MARK_REFUSED : KIWI_MARK_MIDWAY, paid, known(L),
                          known_age(L), TAG);
            ESP_LOGW(TAG, "%s:%u: day limit -- not again until chosen again, %d tries at most", c.host,
                     (unsigned)c.port, KIWI_STRIKES_MAX);
            continue;
        case KIWI_END_SILENT:
            /* A KiwiSDR's door for apps, shut: its app channels all taken --
             * or, three times in a row, none for apps at all. */
            if (++silent_n < SILENT_MAX) {
                wait = 120000;
                break;
            }
            ESP_LOGW(TAG, "%s:%u: silent %d times -- no apps", c.host, (unsigned)c.port, silent_n);
            end = KIWI_END_NO_APPS;
            set_state(want, state_of(end), NULL);
            /* fall through */
        case KIWI_END_IDLE:
        case KIWI_END_KICKED:
        case KIWI_END_REFUSED:
        case KIWI_END_PASSWORD:
        case KIWI_END_NOT_KIWI:
        case KIWI_END_NO_APPS:
        case KIWI_END_MOVED:
        case KIWI_END_CERT:
            hold_set(&hold, hp, end, gen, state_to(end, &s_said, said_st, sizeof said_st));
            ESP_LOGW(TAG, "%s:%u: %s -- not again until chosen again", c.host, (unsigned)c.port, hold.state);
            continue;
        case KIWI_END_APPS_FULL:
            wait = 120000;
            break;
        case KIWI_END_FULL:
        case KIWI_END_PWD_FULL:
            wait = 20000;
            break;
        case KIWI_END_NOT_FOUND:
            wait = 30000;
            break;
        case KIWI_END_DUP_IP:
        case KIWI_END_UPDATING:
        case KIWI_END_DOWN:
        case KIWI_END_TRY_LATER:
            wait = 60000;
            break;
        default:                                /* no route, no answer, closed, went quiet ... */
            wait = backoff;
            backoff = backoff < 30000 ? backoff * 2 : 30000;
            break;
        }
        ESP_LOGW(TAG, "%s:%u: %s -- again in %lu s", c.host, (unsigned)c.port, state_of(end),
                 (unsigned long)(wait / 1000));
        wait_event(wait, want, ch, gen);
    }
}

esp_err_t sdr_rx_init(void)
{
    sdr_list_init();
    kiwi_mark_init(TAG);
    bool resave = false;
    kv_handle_t h;
    if (kv_open(NVS_NS, &h) == ESP_OK) {
        int8_t sel = -1, bal = 0;
        uint32_t selh = 0;
        const bool have_h = kv_get_u32(h, KEY_SELH, &selh) == ESP_OK;
        if (kv_get_i8(h, KEY_SEL, &sel) == ESP_OK && sel >= 0) {
            /* The receiver by its address, as the list may have moved under
             * the place kept; gone, none. A knob that kept no address yet
             * trusts the place, this once. */
            int i = sel < s_n && (!have_h || s_hp[sel] == selh) ? sel : -1;
            for (int k = 0; i < 0 && have_h && k < s_n; k++)
                if (s_hp[k] == selh) i = k;
            s_want = i;
            resave = !have_h || i != sel;
        }
        if (kv_get_i8(h, KEY_BAL, &bal) == ESP_OK && bal >= -100 && bal <= 100) s_bal = bal;
        kv_close(h);
    }
    audio_out_set_balance(s_bal);
    const esp_timer_create_args_t ta = { .callback = save_cb, .name = "sdrsave" };
    esp_timer_create(&ta, &s_save_timer);
    if (resave) save_later();
    /* Its stack in PSRAM: room for a TLS handshake beside the session. */
    ESP_RETURN_ON_FALSE(xTaskCreatePinnedToCoreWithCaps(sdr_task, "sdr", TLS_STACK, NULL, 5, NULL, 0,
                                                        MALLOC_CAP_SPIRAM) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "task");
    ESP_LOGI(TAG, "%d web SDR%s configured%s", s_n, s_n == 1 ? "" : "s",
             s_want >= 0 ? ", one selected" : "");
    return ESP_OK;
}

/* ------------------------------------------------------------------ test */

/* Whether this receiver is the one in use, its session on its way -- from
 * its /status read before the login on -- or playing: a Test answers from
 * that, with no second login beside it (kiwi_test's in_use). */
bool sdr_rx_in_session(uint32_t hp)
{
    taskENTER_CRITICAL(&s_lock);
    const bool p = hp && ((s_run.live && s_run.hp == hp) ||
                          (s_st.sel >= 0 && s_st.sel < s_n && s_hp[s_st.sel] == hp &&
                           (s_st.streaming || !strcmp(s_st.state, "connecting") ||
                            !strcmp(s_st.state, "logged in"))));
    taskEXIT_CRITICAL(&s_lock);
    return p;
}

bool sdr_rx_uses(uint32_t hp)
{
    taskENTER_CRITICAL(&s_lock);
    const int w = s_want;
    const bool chosen = hp && w >= 0 && w < s_n && s_hp[w] == hp;
    taskEXIT_CRITICAL(&s_lock);
    return chosen;
}

/* The Test on a task of its own (sdr_test): its stack in PSRAM, with room
 * for a TLS handshake, which the web server's has not -- and so a task that
 * never touches flash: the marks were read before it began, and kiwi_mark
 * writes from its own timer. One at a time, as the web server runs them.
 * Done, it waits to be deleted by sdr_test: a WithCaps task deleting itself
 * has another made, in internal RAM, to free it -- and aborts without it. */
EXT_RAM_BSS_ATTR static struct {
    sdr_cfg_t    c;
    bool       (*in_use)(uint32_t hp);
    char        *json;
    size_t       cap;
    const char  *tag;
    kiwi_moved_t mv;
    volatile bool done;
} s_test;

static void test_task(void *arg)
{
    (void)arg;
    const kiwi_addr_t a = sdr_addr(&s_test.c);
    kiwi_test(&a, s_test.in_use, s_test.json, s_test.cap, &s_test.mv, s_test.tag);
    s_test.done = true;
    for (;;) vTaskDelay(portMAX_DELAY);
}

esp_err_t sdr_test(const sdr_cfg_t *c, bool (*in_use)(uint32_t hp), char *json, size_t cap, const char *tag)
{
    if (!c || !c->host[0] || !json) return ESP_ERR_INVALID_ARG;
    if (!tag) tag = TAG;
    /* The marks from flash, on this task: the Test's may not touch it. */
    kiwi_mark_init(tag);
    s_test.c = *c;
    s_test.in_use = in_use ? in_use : sdr_rx_in_session;
    s_test.json = json;
    s_test.cap = cap;
    s_test.tag = tag;
    s_test.done = false;
    memset(&s_test.mv, 0, sizeof s_test.mv);
    TaskHandle_t t = NULL;
    if (xTaskCreatePinnedToCoreWithCaps(test_task, "sdrtest", TLS_STACK, NULL, 5, &t, 0,
                                        MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGE(tag, "no memory for the Test's task");
        snprintf(json, cap, "{\"ok\":false,\"error\":\"no memory for a test just now: try again in a moment\"}");
        return ESP_ERR_NO_MEM;
    }
    while (!s_test.done) vTaskDelay(pdMS_TO_TICKS(50));
    vTaskDeleteWithCaps(t);                 /* its stack and all with it */
    /* Followed to https:// on its own host: kept for the receiver at that
     * address in the list, as a session's redirect is. */
    if (s_test.mv.tls_port && !c->tls) sdr_moved(sdr_hp(c), s_test.mv.tls_port);
    return ESP_OK;
}
