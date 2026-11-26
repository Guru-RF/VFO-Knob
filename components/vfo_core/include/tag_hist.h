/* Who was heard: the callsign tag's readings (tag.h) as one entry per over,
 * the newest first, kept on the SD card across restarts (tag_hist.c).
 *
 * TailChaser's rule: the same callsign read again in the frames that follow is
 * the same over. A new entry when the callsign or the local flag changes, or
 * when the tag has been gone for more than about 10 s. "Gone" is the tag's
 * start no longer standing out (tag_stats_t.holding), not merely no reading:
 * under speech a reading can take a while, and the over goes on.
 *
 * A reading that is not sure (tag_reading_t.sure: read under speech, where a
 * CRC can now and then come right by chance) makes no entry of its own: it
 * waits up to TAG_HIST_CONFIRM_MS for a second reading of the same callsign --
 * the tail after the over gives one. It does last the entry on the air when it
 * is that entry's callsign. */
#ifndef TAG_HIST_H
#define TAG_HIST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tag_format.h"

#define TAG_HIST_N       100
#define TAG_HIST_GAP_MS      10000
#define TAG_HIST_CONFIRM_MS  30000

typedef struct {
    char     call[TAG_CALL_LEN + 1];
    bool     local;
    bool     now_start;     /* heard since this start: the ms below mean something */
    int64_t  first_ms, last_ms;          /* the knob's clock since it started */
    int64_t  first_unix, last_unix;      /* wall time; 0: not known (no NTP then) */
    uint16_t reads;
} tag_entry_t;

typedef struct {
    tag_entry_t e[TAG_HIST_N];           /* a ring, e[head] the newest */
    int head, n;
    struct { bool have, local; char call[TAG_CALL_LEN + 1]; int64_t ms, unix_s; } wait;   /* not yet believed */
} tag_hist_t;

typedef enum { TAG_HIST_SAME, TAG_HIST_NEW, TAG_HIST_WAIT } tag_hist_r;

void tag_hist_init(tag_hist_t *h);
/* A reading at now: the entry on the air lasts (SAME), a new one begins (NEW),
 * or it waits to be read again (WAIT). now_unix 0: no wall time. */
tag_hist_r tag_hist_reading(tag_hist_t *h, const char *call, bool local, bool sure, int64_t now_ms, int64_t now_unix);
/* The newest entry's tag is still on the air: the over lasts. */
void tag_hist_holding(tag_hist_t *h, int64_t now_ms, int64_t now_unix);
/* Wall time came (NTP after the first readings): this start's entries get it. */
void tag_hist_clock(tag_hist_t *h, int64_t now_ms, int64_t now_unix);
int  tag_hist_count(const tag_hist_t *h);
const tag_entry_t *tag_hist_at(const tag_hist_t *h, int i);   /* 0 the newest; NULL past the end */

/* For the card: a line an entry, the oldest first, "first last reads L|N CALL";
 * the length written (cut short entries are left out whole). Loading takes
 * what it can read and says how many. */
size_t tag_hist_save(const tag_hist_t *h, char *buf, size_t cap);
int    tag_hist_load(tag_hist_t *h, const char *buf);

#endif /* TAG_HIST_H */
