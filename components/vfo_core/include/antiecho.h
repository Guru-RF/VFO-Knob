/* Anti-echo classifier.
 *
 * The problem: we tune optimistically (the glass must not wait ~30-80ms for the
 * wire), the server echoes every accepted set, and the operator may also be
 * tuning at the PC. Getting this wrong produces rubber-banding, which is the
 * single most infuriating defect this device could have.
 *
 * Pure function over the echo ring plus two timestamps, so the whole thing is
 * host-testable and fuzzable against traces captured from the real server.
 */
#ifndef VFO_ANTIECHO_H
#define VFO_ANTIECHO_H

#include "echo_ring.h"

#define AE_SEND_PERIOD_MS 50u
#define AE_QUIET_MS      250u
#define AE_ECHO_TTL_MS  1500u
#define AE_SETTLE_MS      80u

typedef enum {
    AE_OUR_ECHO,   /* confirmation of something we sent -- ignore entirely   */
    AE_AMBIGUOUS,  /* too close to our own activity to judge -- defer        */
    AE_REMOTE,     /* unambiguous change made elsewhere -- adopt it          */
} ae_class_t;

/* Classify an inbound vfo: for our trx on channel 0.
 *
 * Mutates `r` on a match (drop_through) and expires stale entries first.
 *
 * The quiet window is tested against BOTH timestamps, deliberately:
 *   - t_last_send alone misses AetherSDR's ~400ms post-band-change push, which
 *     has no send of ours nearby and which we DO want to adopt.
 *   - t_last_input alone rubber-bands on a clamp echo that arrives 200ms after
 *     the last send with the operator's hand already off the knob.
 * Requiring both turns the locked-slice case into one clean spring-back
 * instead of half a dozen fights in 300ms.
 */
ae_class_t antiecho_classify(echo_ring_t *r,
                             int64_t  hz,
                             uint32_t now_ms,
                             uint32_t t_last_input_ms,
                             uint32_t t_last_send_ms);

#endif /* VFO_ANTIECHO_H */
