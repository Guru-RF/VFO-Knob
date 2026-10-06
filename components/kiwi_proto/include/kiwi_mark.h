/* Day-limit marks: the receivers that refused the knob for their day limit,
 * kept through restarts. Up to 8, keyed by host:port (kiwi_hp -- the port a
 * receiver is known by, which an https:// redirect does not change), in NVS
 * "kdl" of namespace "vfo", 16 bytes each, shared by every firmware.
 *
 * A Kiwi bars an address for good after five logins it refused for its day
 * limit, and a Web-888 never forgets them until it restarts. So a marked
 * receiver is never logged in to on the knob's own initiative -- not at boot,
 * not after a list save, not on a retry -- and the operator choosing it again
 * is one counted try, the knob's refusals never more than KIWI_STRIKES_MAX.
 * A login the receiver left unanswered counts as one too, where it has time
 * limits: the knob cannot tell it from a refusal whose answer was lost.
 *
 * The mark goes on the receiver's own word that its count was cleared: it
 * has restarted, or a KiwiSDR's day has passed (kiwi_mark_cleared). Short of
 * that it can only rest -- the receiver no longer held back, its strikes
 * kept, as the receiver keeps its own count: once its owner has taken the
 * time limits away (kiwi_mark_lifted), who may put them back before a
 * restart, or once a session has streamed long enough, as a time-limit
 * password lets the knob in without clearing anything.
 *
 * Flash is written by the module's own esp_timer: callers may be tasks whose
 * stacks are in PSRAM. A set, a try or a lift goes at once, though a busy
 * hook holds it while the radio is on the air -- all but a refusal at login,
 * which the receiver counted; a rest waits for a quiet moment
 * (kiwi_mark_flush), or 30 s at most. A knob whose flash cannot take the
 * table says so (kiwi_mark_durable): its marks would not outlive a restart.
 */
#ifndef KIWI_MARK_H
#define KIWI_MARK_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "kiwi_proto.h"

/* The table from NVS, once; any number of calls, from any task that may read
 * flash (not one whose stack is in PSRAM). The first, at boot, also looks at
 * how the last boot ended (kiwi_boot_hold_us).
 *
 * `tag`, here and below: the caller's, for the log (NULL: "kiwi") -- "sdr"
 * for the web SDR beside a radio, so a mark's lines go with its session's.
 * The first call's is the table's own, for what it says of itself: the
 * marks it found at boot, a flash that will not take them. */
esp_err_t kiwi_mark_init(const char *tag);

/* The mark that holds this receiver back, if any (`out` may be NULL): not one
 * at rest. */
bool kiwi_mark_get(uint32_t hp, kiwi_mark_t *out);

/* Any mark on it, one at rest too. */
bool kiwi_mark_find(uint32_t hp, kiwi_mark_t *out);

/* How the receiver turned the knob away. */
typedef enum {
    KIWI_MARK_MIDWAY,       /* ip_limit in a session it had taken: it counts none */
    KIWI_MARK_REFUSED,      /* ip_limit to the login: a refusal it counted */
    KIWI_MARK_NO_ANSWER,    /* the login went unanswered: maybe one it counted */
} kiwi_mark_how_t;

/* It turned the knob away. `paid`: this login was already counted, a try
 * the operator spent on it (kiwi_mark_try); any other refusal -- a Test, a
 * login beside another, the first -- is one strike more. `st`: its /status
 * as read this boot, `st_age_us` ago, or NULL. Saved at once, and a refusal
 * even on the air. */
void kiwi_mark_set(uint32_t hp, kiwi_mark_how_t how, bool paid, const kiwi_status_t *st, int64_t st_age_us,
                   const char *tag);

/* The operator chose a held receiver again: one more strike, counted and
 * saved before the login. False when its strikes are used up: held. True,
 * and nothing counted, when nothing holds it. */
bool kiwi_mark_try(uint32_t hp, const char *tag);

/* A fresh /status of it: the mark goes if that proves its count cleared; it
 * rests, its strikes kept, where the receiver has no time limits now -- the
 * hourglass it showed, gone; or no hourglass at all, where the mark is only
 * a login left unanswered. A mark it does not end learns from it what it
 * did not know yet. */
void kiwi_mark_check(uint32_t hp, const kiwi_status_t *st, const char *tag);

/* It streamed long enough: no limit now. It holds nothing back from now on,
 * its strikes kept; in flash at the next kiwi_mark_flush() or 30 s later. */
void kiwi_mark_clear(uint32_t hp, const char *tag);

/* Whether a login to this receiver could be one it refuses for its day limit
 * and counts: its /status (`st`, NULL if none was read) shows time limits or
 * was not a Kiwi's to be read, or it has refused the knob for its day limit
 * before. Only then is a login left unanswered counted, and only then does a
 * full flash keep the knob from logging in on its own. */
bool kiwi_mark_may_count(uint32_t hp, const kiwi_status_t *st);

/* The last set, try or lift is in flash. */
bool kiwi_mark_saved(void);

/* A quiet moment (a session over): a rest waiting for one goes now. */
void kiwi_mark_flush(void);

/* While this says true -- an over, a call -- flash waits: a write stops the
 * audio's interrupts for up to ~100 ms. kiwi_mark_busy() asks it now (false
 * with none), for others that keep the same rule. */
void kiwi_mark_busy_cb(bool (*busy)(void));
bool kiwi_mark_busy(void);

/* The marks reach flash: there was room for the table at boot, and the last
 * write took. False on a knob whose NVS has filled up: a mark would be lost
 * at the next restart, so the knob logs in to a receiver with time limits
 * only when the operator chooses it. */
bool kiwi_mark_durable(void);

/* When the knob may first contact a receiver on its own this boot (by
 * esp_timer_get_time): 0 after a normal start. After a crash, a watchdog or
 * a brownout that cut the last boot short, again and again, later each time
 * -- a knob in a restart loop would otherwise read a receiver's status page
 * and log in at the loop's pace, and owners drop an address that polls. */
int64_t kiwi_boot_hold_us(void);

/* A receiver held back until the operator chooses it again -- time up,
 * kicked, refused ... (`why` a kiwi_end_t; 0: no longer) -- kept in RTC
 * memory through a crash, a watchdog or a brownout: a restart nobody asked
 * for is no choice, and the knob does not come back by itself. Forgotten at
 * power-on and at a restart asked for. Up to four; after kiwi_mark_init. */
void    kiwi_boot_hold_put(uint32_t hp, uint8_t why);
/* The hold kept for it through the last restart, or 0. */
uint8_t kiwi_boot_hold_get(uint32_t hp);

#endif /* KIWI_MARK_H */
