/* Echo ring + anti-echo classifier.
 *
 * These cases are the ones that actually bite on real hardware; each maps to a
 * specific behaviour of AetherSDR's TciServer. */
#include "tiny.h"
#include "antiecho.h"

static void test_ring_basics(void)
{
    CASE("ring basics");
    echo_ring_t r; echo_clear(&r); memset(&r, 0, sizeof r);

    echo_push(&r, 100, 1000);
    echo_push(&r, 200, 1010);
    echo_push(&r, 300, 1020);
    CHECK_EQ(r.count, 3);
    CHECK_EQ(echo_find(&r, 200), 1);
    CHECK_EQ(echo_find(&r, 999), -1);

    /* drop_through removes the match AND everything older. */
    echo_drop_through(&r, 1);
    CHECK_EQ(r.count, 1);
    CHECK_EQ(r.e[0].hz, 300);

    CASE("ring overflow");
    memset(&r, 0, sizeof r);
    for (int i = 0; i < ECHO_DEPTH + 3; i++) echo_push(&r, 1000 + i, 1000);
    CHECK_EQ(r.count, ECHO_DEPTH);
    CHECK_EQ(r.overflows, 3);
    CHECK_EQ(r.e[0].hz, 1003);            /* oldest three evicted */

    CASE("ring ttl");
    memset(&r, 0, sizeof r);
    echo_push(&r, 100, 1000);
    echo_push(&r, 200, 2000);
    echo_expire(&r, 2600, AE_ECHO_TTL_MS);  /* 100 is 1600ms old, 200 is 600 */
    CHECK_EQ(r.count, 1);
    CHECK_EQ(r.e[0].hz, 200);
    CHECK_EQ(r.ttl_expiries, 1);
}

static void test_our_echo(void)
{
    CASE("our own echo is ignored");
    echo_ring_t r; memset(&r, 0, sizeof r);

    echo_push(&r, 14074000, 1000);
    ae_class_t c = antiecho_classify(&r, 14074000, 1020, 1000, 1000);
    CHECK_EQ(c, AE_OUR_ECHO);
    CHECK_EQ(r.count, 0);
}

static void test_coalesced_noop(void)
{
    /* AetherSDR coalesces a tune that lands where the slice already is: no
     * broadcast, so that echo NEVER arrives. If we only removed the exact
     * match, the orphan would sit in the ring and later false-match a genuine
     * operator tune to the same frequency. drop_through is what prevents it. */
    CASE("coalesced no-op leaves no landmine");
    echo_ring_t r; memset(&r, 0, sizeof r);

    echo_push(&r, 14074000, 1000);   /* echoed normally      */
    echo_push(&r, 14074010, 1050);   /* no-op, never echoed  */
    echo_push(&r, 14074020, 1100);   /* echoed normally      */

    ae_class_t c = antiecho_classify(&r, 14074020, 1150, 1100, 1100);
    CHECK_EQ(c, AE_OUR_ECHO);
    CHECK_EQ(r.count, 0);            /* the orphan went with it */

    /* Now the operator tunes to 14074010 at the PC, well after we went quiet.
     * With a plain remove this would have been swallowed as "our echo". */
    c = antiecho_classify(&r, 14074010, 5000, 1100, 1100);
    CHECK_EQ(c, AE_REMOTE);
}

static void test_quiet_windows(void)
{
    CASE("dual quiet window");
    echo_ring_t r; memset(&r, 0, sizeof r);

    /* Recent operator input -> ambiguous, decide later. */
    CHECK_EQ(antiecho_classify(&r, 7100000, 1100, 1000, 500), AE_AMBIGUOUS);
    /* Recent send of ours -> also ambiguous. */
    CHECK_EQ(antiecho_classify(&r, 7100000, 1100, 500, 1000), AE_AMBIGUOUS);
    /* Both quiet -> unambiguous remote change. */
    CHECK_EQ(antiecho_classify(&r, 7100000, 2000, 1000, 1000), AE_REMOTE);

    /* Exactly on the boundary counts as quiet. */
    CHECK_EQ(antiecho_classify(&r, 7100000, 1000 + AE_QUIET_MS, 1000, 1000),
             AE_REMOTE);
}

static void test_band_change_push(void)
{
    /* A band change at the PC produces a push ~400ms later with no send of
     * ours nearby. Testing t_last_send alone would misclassify it; we want to
     * adopt this one. */
    CASE("remote band-change push is adopted");
    echo_ring_t r; memset(&r, 0, sizeof r);
    CHECK_EQ(antiecho_classify(&r, 3573000, 1400, 900, 900), AE_REMOTE);
}

static void test_stale_entry_expires(void)
{
    CASE("silently-dropped command expires");
    echo_ring_t r; memset(&r, 0, sizeof r);
    echo_push(&r, 14074000, 1000);       /* server drops this silently */

    /* Much later the operator tunes there deliberately. The TTL must have
     * cleared our stale entry or we would swallow a real remote change. */
    ae_class_t c = antiecho_classify(&r, 14074000, 1000 + AE_ECHO_TTL_MS + 50,
                                     1000, 1000);
    CHECK_EQ(c, AE_REMOTE);
    CHECK_EQ(r.ttl_expiries, 1);
}

T_MAIN({
    test_ring_basics();
    test_our_echo();
    test_coalesced_noop();
    test_quiet_windows();
    test_band_change_push();
    test_stale_entry_expires();
})
