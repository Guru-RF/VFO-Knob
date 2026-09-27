#include "tiny.h"
#include "vfo_tune.h"

#define LO 1000LL
#define HI 75000000LL

static void test_quantise(void)
{
    CASE("quantise");
    CHECK_EQ(tune_quantise(14074003, 10),   14074000);
    CHECK_EQ(tune_quantise(14074000, 1000), 14074000);
    CHECK_EQ(tune_quantise(14074999, 1000), 14074000);
    CHECK_EQ(tune_quantise(14074003, 1),    14074003);
}

static void test_anchor_invariant(void)
{
    /* Without the anchor you stop on 14074003 and the digits below your step
     * flicker forever. */
    CASE("anchor keeps the grid exact");
    tune_t t; tune_init(&t, 14074003, 10);
    CHECK_EQ(t.f_display, 14074000);

    for (int i = 0; i < 7; i++) tune_apply(&t, 1, 1, 100, LO, HI);
    CHECK_EQ(t.f_display, 14074070);
    CHECK_EQ(t.f_anchor + (int64_t)t.step_units * t.step_hz, t.f_display);

    /* Changing step re-latches the anchor onto the new grid. */
    tune_set_step(&t, 1000);
    CHECK_EQ(t.f_display, 14074000);
    CHECK_EQ(t.step_units, 0);

    tune_apply(&t, 3, 1, 100, LO, HI);
    CHECK_EQ(t.f_display, 14077000);
}

static void test_accel_rezero(void)
{
    /* The promise: after any spin, the very next slow click is exactly one
     * step. Without it you can never land on an exact frequency. */
    CASE("velocity re-zero");
    accel_t a; accel_init(&a);

    uint32_t now = 1000;
    uint8_t m = 1;
    for (int i = 0; i < 30; i++) { now += 15; m = accel_update(&a, 1, now); }
    CHECK(m > 1);                         /* a real spin accelerated */

    now += ACCEL_REZERO_MS + 10;          /* operator paused */
    m = accel_update(&a, 1, now);
    CHECK_EQ(m, 1);                       /* next click is exactly one step */
}

static void test_accel_hysteresis(void)
{
    CASE("acceleration hysteresis");
    accel_t a; accel_init(&a);
    uint32_t now = 1000;

    /* Slow and steady stays at x1. */
    uint8_t m = 1;
    for (int i = 0; i < 20; i++) { now += 150; m = accel_update(&a, 1, now); }
    CHECK_EQ(m, 1);

    /* ~100 det/s should reach the top rung. */
    accel_init(&a); now = 1000;
    for (int i = 0; i < 40; i++) { now += 10; m = accel_update(&a, 1, now); }
    CHECK_EQ(m, 50);

    /* Easing off must not chatter straight back down at the same threshold. */
    for (int i = 0; i < 3; i++) { now += 16; m = accel_update(&a, 1, now); }
    CHECK(m >= 25);
}

static void test_clamps(void)
{
    CASE("acceleration cap does not override the chosen step");
    tune_t t; tune_init(&t, 14000000, 100000);   /* operator tapped 100 kHz */
    /* x50 would be 5 MHz/detent. Acceleration is suppressed entirely, but the
     * step the operator asked for still gets through untouched. */
    tune_apply(&t, 1, 50, 100, LO, HI);
    CHECK_EQ(t.f_display, 14100000);

    CASE("acceleration is capped at a fine step");
    tune_init(&t, 14000000, 10);                 /* 10 Hz step */
    tune_apply(&t, 1, 50, 100, LO, HI);
    CHECK_EQ(t.f_display, 14000500);             /* 10 x 50 = 500 Hz, under cap */
    tune_init(&t, 14000000, 1000);               /* 1 kHz step */
    tune_apply(&t, 1, 50, 100, LO, HI);
    CHECK_EQ(t.f_display - 14000000, TUNE_MAX_STEP_EFF_HZ);  /* 50 k -> 25 k */

    CASE("range clamp");
    tune_init(&t, HI - 50, 100);
    tune_apply(&t, 100, 10, 500, LO, HI);
    CHECK_EQ(t.f_display, HI);
    CHECK_EQ(t.f_anchor + (int64_t)t.step_units * t.step_hz, t.f_display);

    tune_init(&t, LO + 50, 100);
    tune_apply(&t, -100, 10, 500, LO, HI);
    CHECK_EQ(t.f_display, LO);
}

static void test_assign(void)
{
    CASE("remote assign keeps exact value");
    tune_t t; tune_init(&t, 14074000, 100);
    tune_assign(&t, 14074003);            /* off-grid remote value */
    CHECK_EQ(t.f_display, 14074003);      /* rendered exactly as the rig says */
    tune_set_step(&t, 100);
    CHECK_EQ(t.f_display, 14074000);      /* re-latches on the next step change */
}

T_MAIN({
    test_quantise();
    test_anchor_invariant();
    test_accel_rezero();
    test_accel_hysteresis();
    test_clamps();
    test_assign();
})
