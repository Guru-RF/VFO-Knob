/* What the TailChaser Tag firmware shows: the decoder's state and who was
 * heard, as one copy handed from its client (components/tctag_client) to the
 * face (ui.c TAG_FACE) and the page. */
#ifndef TAG_VIEW_H
#define TAG_VIEW_H

#include <stdbool.h>
#include <stdint.h>

#include "tag.h"
#include "tag_hist.h"

typedef struct {
    tag_stats_t st;
    tag_hist_t  h;              /* who was heard: tag_hist_at(&h, 0) the newest */
    uint32_t    gen;            /* changes with every change to h */
    bool        listening;      /* the microphone is on, the decoder fed */
    int64_t     now_ms;         /* the client's clock (tag_entry_t's ms) when taken */
    int64_t     now_unix;       /* ...and the wall time then; 0 before NTP */
    uint32_t    lost;           /* microphone blocks the decoder was too late for */
    uint8_t     cpu;            /* percent of one core the decoder takes */
} tag_view_t;

#endif /* TAG_VIEW_H */
