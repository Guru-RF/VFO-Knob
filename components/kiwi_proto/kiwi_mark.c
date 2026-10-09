/* Day-limit marks: a table in RAM, kept in the knob's settings (kvstore: the
 * SD card, or NVS without one) by an esp_timer. See kiwi_mark.h. */
#include "kiwi_mark.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "kvstore.h"

/* The table's own lines go out under the tag of its first caller, at boot --
 * "sdr" beside a radio, "kiwi" on the kiwi firmware; a mark's under that of
 * whoever set, tried, checked or cleared it. */
static const char *s_tag = "kiwi";
#define TAG_OF(t) ((t) ? (t) : s_tag)

#define NVS_NS      "vfo"
#define KEY_MARKS   "kdl"
#define MARK_MAX    8
#define SOON_US     1000                    /* a set, a try, a lift */
#define CLEAR_US    (30 * 1000000LL)        /* a rest, failing a quiet moment */
#define RETRY_US    (30 * 1000000LL)        /* the write failed: a full NVS, say */
#define LOOK_US     (20 * 1000)             /* handed to the settings: written yet? */
#define WRITE_US    (5 * 1000000LL)         /* ...and not after this long: failing */
#define FRESH_US    (120 * 1000000LL)       /* a /status this recent still tells its boot */
/* NVS entries (32 bytes each) the table must find free to be sure of its
 * place: all eight marks are a 128-byte blob, six entries, and the old copy
 * stays until the new one is written. */
#define ROOM_ENTRIES 16
/* A boot that lasts this long was no part of a restart loop... */
#define YOUNG_US    (5 * 60 * 1000000LL)
/* ...and in one, the knob's own first contact with a receiver waits this
 * long from the second crash on, twice as long each time after, at most
 * the last. */
#define HOLD_FIRST_US (30 * 1000000LL)
#define HOLD_MAX_US   (10 * 60 * 1000000LL)

EXT_RAM_BSS_ATTR static kiwi_mark_t s_tab[MARK_MAX];
static portMUX_TYPE       s_mux = portMUX_INITIALIZER_UNLOCKED;
static esp_timer_handle_t s_timer;
static volatile int       s_init;           /* 0 never, 1 loading, 2 ready */
static uint8_t            s_seq;
/* Generations: every change moves s_gen; s_must is the newest that has to
 * reach flash before a login, s_urgent the newest that goes even on the air,
 * s_saved what flash holds. */
static volatile uint32_t  s_gen, s_must, s_urgent, s_saved;
static bool (*volatile s_busy)(void);
static bool               s_failing;        /* said once, not every retry */
static volatile bool      s_room;           /* NVS had room for the table, and the last write took */
static uint32_t           s_pend;           /* the generation handed to the settings, not yet written */
static int64_t            s_pend_at;        /* when; 0 none */

/* How the boots before this one ended, in RTC memory: a crash, a watchdog or
 * a brownout restart leaves it as it was, power-on fills it with noise --
 * hence the magic and the check. Built for test/host, a file stands in. It
 * also keeps the receivers held back until chosen again (time up, kicked,
 * refused ...), for a restart nobody asked for. */
#define BOOT_MAGIC 0x4B424F50u
#define BOOT_HOLDS 4
typedef struct {
    uint32_t magic, young, loops;
    uint32_t hold_hp[BOOT_HOLDS];           /* kiwi_hp; 0 a free place */
    uint8_t  hold_why[BOOT_HOLDS];          /* kiwi_end_t */
    uint32_t check;
} boot_rec_t;
#ifdef KIWI_RTC_SHIM
void *shim_rtc(size_t n);
#define BOOT_REC ((boot_rec_t *)shim_rtc(sizeof(boot_rec_t)))
#else
RTC_NOINIT_ATTR static boot_rec_t s_boot_rec;
#define BOOT_REC (&s_boot_rec)
#endif
static int64_t            s_hold_until;
static esp_timer_handle_t s_young_t;
static uint8_t            s_hold_next;      /* the holds all taken: whose place goes next */

/* FNV-1a over everything but the check itself. */
static uint32_t boot_check(const boot_rec_t *r)
{
    uint32_t h = 2166136261u;
    const uint8_t *p = (const uint8_t *)r;
    for (size_t i = 0; i < offsetof(boot_rec_t, check); i++) h = (h ^ p[i]) * 16777619u;
    return h ^ ~BOOT_MAGIC;
}

static kiwi_mark_t *find(uint32_t hp)
{
    for (int i = 0; hp && i < MARK_MAX; i++)
        if (s_tab[i].hp == hp) return &s_tab[i];
    return NULL;
}

/* Which of two marks to let go of first, the table being full: one at rest
 * before one that holds a receiver back, then the fewer strikes, then the
 * older. */
static bool sooner_gone(const kiwi_mark_t *a, const kiwi_mark_t *b)
{
    const bool ra = a->flags & KIWI_MARK_REST, rb = b->flags & KIWI_MARK_REST;
    if (ra != rb) return ra;
    if (a->strikes != b->strikes) return a->strikes < b->strikes;
    return (uint8_t)(s_seq - a->seq) > (uint8_t)(s_seq - b->seq);
}

/* Its mark, or a new one (`*fresh`): in an empty place, else in that of the
 * one that matters least, which goes into `*gone`. Under s_mux. */
static kiwi_mark_t *find_or_new(uint32_t hp, bool *fresh, kiwi_mark_t *gone)
{
    memset(gone, 0, sizeof *gone);
    kiwi_mark_t *m = find(hp);
    *fresh = !m;
    if (m) return m;
    int best = -1;
    for (int i = 0; i < MARK_MAX; i++) {
        if (!s_tab[i].hp) { best = i; break; }
        if (best < 0 || sooner_gone(&s_tab[i], &s_tab[best])) best = i;
    }
    m = &s_tab[best];
    *gone = *m;
    memset(m, 0, sizeof *m);
    m->hp = hp;
    return m;
}

/* Said once the table's lock is let go: a mark that still held a receiver
 * back, lost to a full table, is worth knowing about. */
static void say_gone(const kiwi_mark_t *g, const char *tag)
{
    if (!g->hp) return;
    if (g->strikes && !(g->flags & KIWI_MARK_REST))
        ESP_LOGW(tag, "day-limit marks full: the mark of %08lx (%u refused logins) is let go",
                 (unsigned long)g->hp, (unsigned)g->strikes);
    else
        ESP_LOGI(tag, "day-limit marks full: %08lx's, at rest, is let go", (unsigned long)g->hp);
}

/* Room for the table: the SD card holds the settings, or NVS has the room. */
static bool nvs_room(kv_handle_t h)
{
    if (kv_on_card(h)) return true;
    nvs_stats_t st;
    return nvs_get_stats(NULL, &st) == ESP_OK && st.available_entries >= ROOM_ENTRIES;
}

static void save_failed(esp_err_t e)
{
    if (!s_failing) ESP_LOGE(s_tag, "day-limit marks not saved (%s): again every 30 s", esp_err_to_name(e));
    s_failing = true;
    s_room = false;
    esp_timer_start_once(s_timer, RETRY_US);
}

/* The table to the settings, then -- a timer's look every 20 ms, never a
 * wait -- until they have written it: only then is it saved. The settings
 * keep a write to NVS off the air themselves; to the card it costs the audio
 * nothing. */
static void save_cb(void *arg)
{
    (void)arg;
    kv_handle_t h;
    esp_err_t e = kv_open(NVS_NS, &h);
    if (e != ESP_OK) {
        save_failed(e);
        return;
    }
    if (s_pend_at) {
        if (!kv_saved(h)) {
            if (esp_timer_get_time() - s_pend_at < WRITE_US) esp_timer_start_once(s_timer, LOOK_US);
            else {
                s_pend_at = 0;
                save_failed(ESP_ERR_TIMEOUT);
            }
            return;
        }
        s_pend_at = 0;
        if (s_failing) ESP_LOGW(s_tag, "day-limit marks saved after all");
        s_failing = false;
        s_room = nvs_room(h);
        s_saved = s_pend;
        if (s_saved == s_gen) return;
    }
    kiwi_mark_t tab[MARK_MAX];
    int n = 0;
    portENTER_CRITICAL(&s_mux);
    for (int i = 0; i < MARK_MAX; i++)
        if (s_tab[i].hp) tab[n++] = s_tab[i];
    const uint32_t gen = s_gen;
    portEXIT_CRITICAL(&s_mux);
    e = n ? kv_set_blob(h, KEY_MARKS, tab, n * sizeof tab[0]) : kv_erase_key(h, KEY_MARKS);
    if (e == ESP_ERR_NVS_NOT_FOUND) e = ESP_OK;         /* none, and none there */
    /* A refusal the receiver counted goes even on the air: lost to a power
     * cut, it would cost one more. */
    if (e == ESP_OK) e = (int32_t)(gen - s_urgent) <= 0 ? kv_commit_now(h) : kv_commit(h);
    kv_close(h);
    /* Not written, so not saved: a try waits without logging in, rather
     * than spend a strike a restart could forget -- and the knob stops
     * logging in on its own where a refusal could not be kept. */
    if (e != ESP_OK) {
        save_failed(e);
        return;
    }
    s_pend = gen;
    s_pend_at = esp_timer_get_time();
    esp_timer_start_once(s_timer, LOOK_US);
    ESP_LOGD(s_tag, "%d day-limit mark%s handed to the settings", n, n == 1 ? "" : "s");
}

/* Now: stop whatever was set, and start it again; twice more if another
 * task started it in between (it then saves this too, but maybe later). */
static void save_soon(void)
{
    if (!s_timer) return;
    for (int i = 0; i < 3; i++) {
        esp_timer_stop(s_timer);
        if (esp_timer_start_once(s_timer, SOON_US) == ESP_OK) return;
    }
}

/* Five minutes up: this boot is no part of a restart loop. */
static void young_cb(void *arg)
{
    (void)arg;
    portENTER_CRITICAL(&s_mux);
    boot_rec_t *r = BOOT_REC;
    r->young = 0;
    r->check = boot_check(r);
    portEXIT_CRITICAL(&s_mux);
}

/* How the last boot ended. A crash, a watchdog or a brownout while it was
 * still young is one more round of a loop -- a firmware that dies at a
 * receiver's answer, a battery that sags at WiFi's first burst -- and from
 * the second such round on, the receivers wait before the knob contacts
 * them on its own: 30 s, 60, 120 ... ten minutes at most. The holds kept
 * outlive only such a restart: a power-on, or one asked for, starts over. */
static void boot_seen(void)
{
    boot_rec_t *r = BOOT_REC;
    const esp_reset_reason_t why = esp_reset_reason();
    const bool crash = why == ESP_RST_PANIC || why == ESP_RST_INT_WDT || why == ESP_RST_TASK_WDT ||
                       why == ESP_RST_WDT || why == ESP_RST_BROWNOUT || why == ESP_RST_CPU_LOCKUP;
    const bool kept = r->magic == BOOT_MAGIC && r->check == boot_check(r);
    const uint32_t loops = kept && crash && r->young ? r->loops + 1 : 0;
    if (!kept || !crash) {
        memset(r->hold_hp, 0, sizeof r->hold_hp);
        memset(r->hold_why, 0, sizeof r->hold_why);
    }
    for (int i = 0; i < BOOT_HOLDS; i++)
        if (r->hold_hp[i])
            ESP_LOGW(s_tag, "%08lx: held through the restart (%s) until chosen again", (unsigned long)r->hold_hp[i],
                     kiwi_end_note((kiwi_end_t)r->hold_why[i]));
    r->magic = BOOT_MAGIC;
    r->young = 1;
    r->loops = loops;
    r->check = boot_check(r);
    if (loops >= 2) {
        const uint32_t sh = loops - 2 < 5 ? loops - 2 : 5;
        int64_t hold = HOLD_FIRST_US << sh;
        if (hold > HOLD_MAX_US) hold = HOLD_MAX_US;
        s_hold_until = hold;
        ESP_LOGW(s_tag, "restarted by a crash %lu times in a row, each soon after the last: "
                 "the receivers wait %lld s for the knob", (unsigned long)loops, (long long)(hold / 1000000));
    }
    const esp_timer_create_args_t ta = { .callback = young_cb, .name = "kiwiboot" };
    if (esp_timer_create(&ta, &s_young_t) == ESP_OK) esp_timer_start_once(s_young_t, YOUNG_US);
}

esp_err_t kiwi_mark_init(const char *tag)
{
    portENTER_CRITICAL(&s_mux);
    const int was = s_init;
    if (!was) s_init = 1;
    portEXIT_CRITICAL(&s_mux);
    if (was) {
        while (s_init != 2) vTaskDelay(1);       /* another task is loading it */
        return ESP_OK;
    }
    if (tag) s_tag = tag;
    boot_seen();
    int n = 0;
    kv_handle_t h;
    if (kv_open(NVS_NS, &h) == ESP_OK) {
        size_t len = 0;
        if (kv_get_blob(h, KEY_MARKS, NULL, &len) == ESP_OK && len && len % sizeof(kiwi_mark_t) == 0) {
            kiwi_mark_t *all = malloc(len);
            if (all && kv_get_blob(h, KEY_MARKS, all, &len) == ESP_OK) {
                /* A larger table, from some other firmware: its first ones. */
                for (size_t i = 0; i < len / sizeof *all && n < MARK_MAX; i++) {
                    if (!all[i].hp) continue;
                    s_tab[n] = all[i];
                    if ((int8_t)(all[i].seq - s_seq) > 0) s_seq = all[i].seq;
                    n++;
                }
            }
            free(all);
        }
        s_room = nvs_room(h);
        kv_close(h);
    }
    const esp_timer_create_args_t ta = { .callback = save_cb, .name = "kiwimark" };
    const esp_err_t e = esp_timer_create(&ta, &s_timer);
    for (int i = 0; i < n; i++)
        ESP_LOGW(s_tag, "day-limit mark %08lx: %u of %d refused logins%s%s", (unsigned long)s_tab[i].hp,
                 (unsigned)s_tab[i].strikes, KIWI_STRIKES_MAX,
                 s_tab[i].flags & KIWI_MARK_UNSURE ? ", one maybe" : "",
                 s_tab[i].flags & KIWI_MARK_REST ? ", at rest" : "");
    if (!s_room)
        ESP_LOGE(s_tag, "NVS is full: a day-limit mark would not outlive a restart -- receivers with "
                      "time limits are logged in to only when chosen");
    s_init = 2;
    return e;
}

bool kiwi_mark_get(uint32_t hp, kiwi_mark_t *out)
{
    portENTER_CRITICAL(&s_mux);
    const kiwi_mark_t *m = find(hp);
    if (m && (m->flags & KIWI_MARK_REST)) m = NULL;
    if (m && out) *out = *m;
    portEXIT_CRITICAL(&s_mux);
    return m != NULL;
}

bool kiwi_mark_find(uint32_t hp, kiwi_mark_t *out)
{
    portENTER_CRITICAL(&s_mux);
    const kiwi_mark_t *m = find(hp);
    if (m && out) *out = *m;
    portEXIT_CRITICAL(&s_mux);
    return m != NULL;
}

void kiwi_mark_set(uint32_t hp, kiwi_mark_how_t how, bool paid, const kiwi_status_t *st, int64_t st_age_us,
                   const char *tag)
{
    if (!hp) return;
    const bool counted = how != KIWI_MARK_MIDWAY;
    const bool times = st && st->ok && st->timed;
    bool fresh;
    kiwi_mark_t gone;
    portENTER_CRITICAL(&s_mux);
    kiwi_mark_t *m = find_or_new(hp, &fresh, &gone);
    m->flags &= (uint8_t)~KIWI_MARK_REST;           /* it holds the receiver back again */
    /* A login a try already paid for is counted; any other is one more --
     * a Test beside a session, two logins at once each cost the knob one. */
    if (counted && (!paid || !m->strikes) && m->strikes < 255) m->strikes++;
    /* An ip_limit, whenever it came, is a day limit for sure. */
    if (how != KIWI_MARK_NO_ANSWER) m->flags &= (uint8_t)~KIWI_MARK_UNSURE;
    else if (fresh)                 m->flags |= KIWI_MARK_UNSURE;
    if (st && st->ok && st->kind)    m->kind = st->kind;
    if (st && st->ok && st->tlimits) m->flags |= KIWI_MARK_HG;
    /* An ip_limit while its /status showed no hourglass: whatever it showed
     * before, its hourglass gone proves nothing of this receiver -- only a
     * restart, or a KiwiSDR's day, lifts this mark. */
    else if (st && st->ok && how != KIWI_MARK_NO_ANSWER) m->flags &= (uint8_t)~KIWI_MARK_HG;
    if (times) {
        /* Its clock now: the date it said, plus the knob's time since. */
        const uint32_t now = st->date_utc + (uint32_t)(st_age_us / 1000000);
        if (now > m->mark_utc) m->mark_utc = now;
        /* When it had started -- only from a read just before: had it
         * restarted since an older one, a later read would seem to prove a
         * restart after the mark. Unknown, the next read fills it in. */
        if (!m->rx_boot && st_age_us < FRESH_US) m->rx_boot = st->date_utc - st->uptime_s;
    } else {
        /* When it refused is not known: the next read says, later than the
         * refusal -- a day is counted from then, never from an older one. */
        m->mark_utc = 0;
    }
    m->seq = ++s_seq;
    const uint8_t strikes = m->strikes;
    s_must = ++s_gen;
    if (counted) s_urgent = s_must;
    portEXIT_CRITICAL(&s_mux);
    say_gone(&gone, TAG_OF(tag));
    save_soon();
    ESP_LOGW(TAG_OF(tag), "day-limit mark %08lx: %s, %u of %d refused logins", (unsigned long)hp,
             how == KIWI_MARK_REFUSED ? "refused at login"
             : how == KIWI_MARK_NO_ANSWER ? "its login left unanswered, counted in case"
             : "ended mid-session", (unsigned)strikes, KIWI_STRIKES_MAX);
}

bool kiwi_mark_try(uint32_t hp, const char *tag)
{
    bool ok = true, counted = false, held = false;
    uint8_t strikes = 0;
    portENTER_CRITICAL(&s_mux);
    kiwi_mark_t *m = find(hp);
    if (m && !(m->flags & KIWI_MARK_REST)) {
        held = true;
        ok = m->strikes < KIWI_STRIKES_MAX;
        if (ok) {
            m->strikes++;
            m->seq  = ++s_seq;
            s_must  = ++s_gen;
            counted = true;
        }
        strikes = m->strikes;
    }
    portEXIT_CRITICAL(&s_mux);
    if (counted) save_soon();
    if (held) ESP_LOGW(TAG_OF(tag), "day-limit mark %08lx: %s (%u of %d)", (unsigned long)hp,
                       ok ? "chosen again, one more try" : "chosen again, but held", (unsigned)strikes,
                       KIWI_STRIKES_MAX);
    return ok;
}

void kiwi_mark_check(uint32_t hp, const kiwi_status_t *st, const char *tag)
{
    if (!st || !st->ok) return;
    bool lifted = false, rested = false, learnt = false;
    uint8_t kept = 0;
    portENTER_CRITICAL(&s_mux);
    kiwi_mark_t *m = find(hp);
    if (m && kiwi_mark_cleared(m, st)) {
        /* Its count cleared since: nothing of the mark is left to keep. */
        memset(m, 0, sizeof *m);
        lifted = true;
    } else if (m) {
        if (st->kind && m->kind != st->kind) { m->kind = st->kind; learnt = true; }
        /* Its time limits taken away -- the hourglass it showed, gone: it
         * holds nothing back from now on. Its strikes stay, as the receiver
         * keeps its count until it restarts, and its owner may put the
         * limits back before that. */
        if (kiwi_mark_lifted(m, st) && !(m->flags & KIWI_MARK_REST)) {
            m->flags |= KIWI_MARK_REST;
            kept = m->strikes;
            rested = true;
        }
        if (st->tlimits && !(m->flags & KIWI_MARK_HG)) { m->flags |= KIWI_MARK_HG; learnt = true; }
        /* Only a login left unanswered, by a receiver with no time limits:
         * nothing it could have refused the knob for. It holds nothing back
         * from now on, the strike kept in case. */
        if (!st->tlimits && (m->flags & KIWI_MARK_UNSURE) && !(m->flags & KIWI_MARK_REST)) {
            m->flags |= KIWI_MARK_REST;
            kept = m->strikes;
            rested = true;
        }
        /* Unknown times start from this read: later than the mark, maybe
         * after a restart that then goes unseen -- held longer, never less. */
        if (st->timed) {
            if (!m->rx_boot)  { m->rx_boot = st->date_utc - st->uptime_s; learnt = true; }
            if (!m->mark_utc) { m->mark_utc = st->date_utc; learnt = true; }
        }
    }
    if (lifted || rested || learnt) s_must = ++s_gen;
    portEXIT_CRITICAL(&s_mux);
    if (lifted || rested || learnt) save_soon();
    if (lifted) ESP_LOGI(TAG_OF(tag), "day-limit mark %08lx lifted: its count was cleared since", (unsigned long)hp);
    if (rested) ESP_LOGI(TAG_OF(tag), "day-limit mark %08lx rests: no time limits now, its %u refused logins kept",
                         (unsigned long)hp, (unsigned)kept);
}

void kiwi_mark_clear(uint32_t hp, const char *tag)
{
    bool was = false, gone = false;
    uint8_t strikes = 0;
    portENTER_CRITICAL(&s_mux);
    kiwi_mark_t *m = find(hp);
    if (m && !(m->flags & KIWI_MARK_REST)) {
        strikes = m->strikes;
        /* What the receiver has counted it keeps until it restarts -- or, a
         * KiwiSDR, for a day: a time-limit password lets the knob in without
         * clearing anything. So the strikes stay, the hold goes. */
        if (strikes) m->flags |= KIWI_MARK_REST;
        else { memset(m, 0, sizeof *m); gone = true; }
        s_gen++;
        was = true;
    }
    portEXIT_CRITICAL(&s_mux);
    if (!was) return;
    if (gone) ESP_LOGI(TAG_OF(tag), "day-limit mark %08lx cleared: it streams", (unsigned long)hp);
    else      ESP_LOGI(TAG_OF(tag), "day-limit mark %08lx rests: it streams, its %u refused logins kept",
                       (unsigned long)hp, (unsigned)strikes);
    if (s_timer && !esp_timer_is_active(s_timer)) esp_timer_start_once(s_timer, CLEAR_US);
}

bool kiwi_mark_may_count(uint32_t hp, const kiwi_status_t *st)
{
    if (!st || !st->ok || st->tlimits) return true;
    /* No hourglass: no time limits -- unless it has turned the knob away
     * for its day limit before, whatever its /status said. */
    kiwi_mark_t m;
    return kiwi_mark_find(hp, &m) && !(m.flags & KIWI_MARK_UNSURE);
}

bool kiwi_mark_saved(void) { return (int32_t)(s_saved - s_must) >= 0; }

void kiwi_mark_flush(void)
{
    if (s_saved != s_gen) save_soon();
}

void kiwi_mark_busy_cb(bool (*busy)(void)) { s_busy = busy; }

bool kiwi_mark_busy(void)
{
    bool (*const busy)(void) = s_busy;
    return busy && busy();
}

bool kiwi_mark_durable(void) { return s_room; }

int64_t kiwi_boot_hold_us(void) { return s_hold_until; }

void kiwi_boot_hold_put(uint32_t hp, uint8_t why)
{
    if (!hp || s_init != 2) return;          /* the record is boot_seen()'s first */
    portENTER_CRITICAL(&s_mux);
    boot_rec_t *r = BOOT_REC;
    int at = -1;
    for (int i = 0; at < 0 && i < BOOT_HOLDS; i++)
        if (r->hold_hp[i] == hp) at = i;
    for (int i = 0; at < 0 && why && i < BOOT_HOLDS; i++)
        if (!r->hold_hp[i]) at = i;
    if (at < 0 && why) at = s_hold_next++ % BOOT_HOLDS;
    if (at >= 0) {
        r->hold_hp[at]  = why ? hp : 0;
        r->hold_why[at] = why;
        r->check = boot_check(r);
    }
    portEXIT_CRITICAL(&s_mux);
}

uint8_t kiwi_boot_hold_get(uint32_t hp)
{
    uint8_t why = 0;
    if (!hp || s_init != 2) return 0;
    portENTER_CRITICAL(&s_mux);
    const boot_rec_t *r = BOOT_REC;
    for (int i = 0; i < BOOT_HOLDS; i++)
        if (r->hold_hp[i] == hp) why = r->hold_why[i];
    portEXIT_CRITICAL(&s_mux);
    return why;
}
