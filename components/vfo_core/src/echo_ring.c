#include "echo_ring.h"

#include <string.h>

void echo_clear(echo_ring_t *r)
{
    if (!r) return;
    r->count = 0;
    /* ttl_expiries / overflows are lifetime diagnostics; do not reset. */
}

void echo_push(echo_ring_t *r, int64_t hz, uint32_t now_ms)
{
    if (!r) return;
    if (r->count == ECHO_DEPTH) {
        /* Shouldn't happen: the send gate caps us at 20/s and the TTL is
         * 1500ms, so eight in flight means the server has stopped answering. */
        memmove(&r->e[0], &r->e[1], sizeof r->e[0] * (ECHO_DEPTH - 1));
        r->count--;
        r->overflows++;
    }
    r->e[r->count].hz   = hz;
    r->e[r->count].t_ms = now_ms;
    r->count++;
}

void echo_expire(echo_ring_t *r, uint32_t now_ms, uint32_t ttl_ms)
{
    if (!r) return;
    uint8_t keep = 0;
    /* Entries are in ascending age order, so the survivors are a suffix. */
    while (keep < r->count && (uint32_t)(now_ms - r->e[keep].t_ms) > ttl_ms)
        keep++;
    if (keep == 0) return;
    r->ttl_expiries += keep;
    memmove(&r->e[0], &r->e[keep], sizeof r->e[0] * (size_t)(r->count - keep));
    r->count = (uint8_t)(r->count - keep);
}

int echo_find(const echo_ring_t *r, int64_t hz)
{
    if (!r) return -1;
    for (uint8_t i = 0; i < r->count; i++)
        if (r->e[i].hz == hz) return i;   /* oldest match wins */
    return -1;
}

void echo_drop_through(echo_ring_t *r, int idx)
{
    if (!r || idx < 0 || idx >= r->count) return;
    uint8_t drop = (uint8_t)(idx + 1);
    memmove(&r->e[0], &r->e[drop], sizeof r->e[0] * (size_t)(r->count - drop));
    r->count = (uint8_t)(r->count - drop);
}
