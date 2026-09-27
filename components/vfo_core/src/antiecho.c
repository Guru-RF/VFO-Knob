#include "antiecho.h"

/* Unsigned wraparound-safe elapsed time. */
static inline uint32_t since(uint32_t now, uint32_t then) { return now - then; }

ae_class_t antiecho_classify(echo_ring_t *r,
                             int64_t  hz,
                             uint32_t now_ms,
                             uint32_t t_last_input_ms,
                             uint32_t t_last_send_ms)
{
    echo_expire(r, now_ms, AE_ECHO_TTL_MS);

    int i = echo_find(r, hz);
    if (i >= 0) {
        echo_drop_through(r, i);
        return AE_OUR_ECHO;
    }

    if (since(now_ms, t_last_input_ms) < AE_QUIET_MS ||
        since(now_ms, t_last_send_ms)  < AE_QUIET_MS)
        return AE_AMBIGUOUS;

    return AE_REMOTE;
}
