/* Echo ring — tracks frequencies we put on the wire so their confirmations can
 * be told apart from a genuine tune by the operator at the PC.
 *
 * Two non-obvious properties, each of which fixes a real bug:
 *
 *  - drop_through(), not remove(). AetherSDR coalesces no-op tunes, so a
 *    command that lands on the frequency the slice already holds emits no
 *    broadcast and its echo never arrives. Removing only the match leaves that
 *    orphan in the ring, where it later false-matches a genuine operator tune
 *    to the same frequency. Dropping the match AND everything older clears it.
 *    Per-connection FIFO ordering (dispatchText -> acknowledgeText, strictly
 *    one command at a time) is what makes "older" well defined.
 *
 *  - A TTL. TciProtocol::cmdVfo has several silent-drop exits where no echo
 *    will ever arrive, so without expiry an entry can outlive its usefulness
 *    and mis-classify a later remote change.
 */
#ifndef VFO_ECHO_RING_H
#define VFO_ECHO_RING_H

#include <stdbool.h>
#include <stdint.h>

#define ECHO_DEPTH 8

typedef struct {
    struct { int64_t hz; uint32_t t_ms; } e[ECHO_DEPTH];
    uint8_t  count;          /* entries in use; index 0 is always the OLDEST */
    uint32_t ttl_expiries;   /* diagnostic, surfaced on the status shade */
    uint32_t overflows;
} echo_ring_t;

void echo_clear(echo_ring_t *r);

/* Append. If full, the oldest entry is discarded (and counted). */
void echo_push(echo_ring_t *r, int64_t hz, uint32_t now_ms);

/* Drop entries older than ttl_ms. Call before find(). */
void echo_expire(echo_ring_t *r, uint32_t now_ms, uint32_t ttl_ms);

/* Index of the OLDEST entry matching hz, or -1. Oldest-first matters: echoes
 * arrive in the order the commands were dispatched. */
int  echo_find(const echo_ring_t *r, int64_t hz);

/* Remove entry `idx` and every entry older than it. */
void echo_drop_through(echo_ring_t *r, int idx);

#endif /* VFO_ECHO_RING_H */
