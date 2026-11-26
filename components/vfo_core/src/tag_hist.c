/* Who was heard, one entry per over (tag_hist.h). */
#include "tag_hist.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tag.h"

void tag_hist_init(tag_hist_t *h)
{
    memset(h, 0, sizeof *h);
    h->head = -1;
}

int tag_hist_count(const tag_hist_t *h) { return h->n; }

const tag_entry_t *tag_hist_at(const tag_hist_t *h, int i)
{
    if (i < 0 || i >= h->n) return NULL;
    return &h->e[(h->head - i + TAG_HIST_N) % TAG_HIST_N];
}

/* A callsign into its field: TAG_CALL_LEN at most, and ended. */
static void put_call(char dst[TAG_CALL_LEN + 1], const char *src)
{
    size_t i = 0;
    for (; i < TAG_CALL_LEN && src[i]; i++) dst[i] = src[i];
    dst[i] = 0;
}

static tag_entry_t *newest(tag_hist_t *h) { return h->n ? &h->e[h->head] : NULL; }

static tag_entry_t *add(tag_hist_t *h)
{
    h->head = (h->head + 1) % TAG_HIST_N;
    if (h->n < TAG_HIST_N) h->n++;
    tag_entry_t *e = &h->e[h->head];
    memset(e, 0, sizeof *e);
    return e;
}

tag_hist_r tag_hist_reading(tag_hist_t *h, const char *call, bool local, bool sure, int64_t now_ms, int64_t now_unix)
{
    tag_entry_t *e = newest(h);
    if (e && e->now_start && e->local == local && !strcmp(e->call, call) && now_ms - e->last_ms <= TAG_HIST_GAP_MS) {
        e->last_ms = now_ms;
        if (now_unix) e->last_unix = now_unix;
        if (e->reads < UINT16_MAX) e->reads++;
        return TAG_HIST_SAME;
    }
    int64_t first_ms = now_ms, first_unix = now_unix;
    uint16_t reads = 1;
    const bool again = h->wait.have && h->wait.local == local && !strcmp(h->wait.call, call) &&
                       now_ms - h->wait.ms <= TAG_HIST_CONFIRM_MS;
    if (!sure && !again) {
        h->wait.have = true;
        h->wait.local = local;
        put_call(h->wait.call, call);
        h->wait.ms = now_ms;
        h->wait.unix_s = now_unix;
        return TAG_HIST_WAIT;
    }
    if (again) {                                             /* it was heard from the first reading on */
        first_ms = h->wait.ms;
        first_unix = h->wait.unix_s;
        reads = 2;
    }
    h->wait.have = false;
    e = add(h);
    put_call(e->call, call);
    e->local = local;
    e->now_start = true;
    e->first_ms = first_ms;
    e->last_ms = now_ms;
    e->first_unix = first_unix;
    e->last_unix = now_unix;
    e->reads = reads;
    return TAG_HIST_NEW;
}

void tag_hist_holding(tag_hist_t *h, int64_t now_ms, int64_t now_unix)
{
    tag_entry_t *e = newest(h);
    if (!e || !e->now_start || now_ms - e->last_ms > TAG_HIST_GAP_MS) return;   /* gone already */
    e->last_ms = now_ms;
    if (now_unix) e->last_unix = now_unix;
}

void tag_hist_clock(tag_hist_t *h, int64_t now_ms, int64_t now_unix)
{
    if (!now_unix) return;
    for (int i = 0; i < h->n; i++) {
        tag_entry_t *e = (tag_entry_t *)tag_hist_at(h, i);
        if (!e->now_start) break;                            /* older starts: nothing to go by */
        if (!e->first_unix) e->first_unix = now_unix - (now_ms - e->first_ms) / 1000;
        if (!e->last_unix) e->last_unix = now_unix - (now_ms - e->last_ms) / 1000;
    }
}

size_t tag_hist_save(const tag_hist_t *h, char *buf, size_t cap)
{
    size_t at = 0;
    if (cap) buf[0] = 0;
    for (int i = h->n - 1; i >= 0; i--) {
        const tag_entry_t *e = tag_hist_at(h, i);
        char line[80];
        const int k = snprintf(line, sizeof line, "%" PRId64 " %" PRId64 " %u %c %s\n", e->first_unix,
                               e->last_unix, (unsigned)e->reads, e->local ? 'L' : 'N', e->call);
        if (k <= 0 || at + (size_t)k + 1 > cap) break;
        memcpy(buf + at, line, (size_t)k + 1);
        at += (size_t)k;
    }
    return at;
}

int tag_hist_load(tag_hist_t *h, const char *buf)
{
    tag_hist_init(h);
    int got = 0;
    for (const char *p = buf; p && *p;) {
        const char *nl = strchr(p, '\n');
        char line[80];
        const size_t len = nl ? (size_t)(nl - p) : strlen(p);
        if (len < sizeof line) {
            memcpy(line, p, len);
            line[len] = 0;
            long long f, l;
            unsigned r;
            char w, call[16];
            if (sscanf(line, "%lld %lld %u %c %15s", &f, &l, &r, &w, call) == 5 && (w == 'L' || w == 'N') &&
                tag_call_ok(call) && f >= 0 && l >= f) {
                tag_entry_t *e = add(h);
                put_call(e->call, call);
                e->local = w == 'L';
                e->first_unix = f;
                e->last_unix = l;
                e->reads = (uint16_t)(r > UINT16_MAX ? UINT16_MAX : r);
                got++;
            }
        }
        p = nl ? nl + 1 : NULL;
    }
    return got;
}
