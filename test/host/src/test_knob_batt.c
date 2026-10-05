/* The knob's own battery from its 5 V rail (components/board/knob_batt.c):
 * USB or the battery, settled at boot and changed on two readings in a row
 * past the other line, a plug or an unplug; on the battery its charge, by
 * the curve, shown in fives -- settling either way for its first half
 * minute, then smoothed, steady through WiFi's bursts and the backlight,
 * never back up on a run-down, and empty at 3.4 V. The readings are the
 * battery knob's (2026-10-05): 4,460-4,560 mV on USB, 4,080-4,100 just off
 * a full charge. */
#include "tiny.h"
#include "knob_batt.h"

#include <math.h>

static knob_batt_t b;
static uint32_t    now;

/* One reading a second, as the knob takes them. */
static bool feed(int mv)
{
    now += 1000;
    return knob_batt_feed(&b, mv, now);
}

static void fresh(void)
{
    knob_batt_init(&b);
    now = 0;
}

/* On the battery at a steady rail, settled. */
static void on_battery_at(int mv)
{
    fresh();
    for (int i = 0; i < KNOB_BATT_SETTLE; i++) feed(mv);
    for (int i = 0; i < 300; i++) feed(mv);
}

/* A repeatable noise, +-amp mV. */
static uint32_t seed = 12345;
static int noise(int amp)
{
    seed = seed * 1103515245u + 12345u;
    return (int)((seed >> 16) % (uint32_t)(2 * amp + 1)) - amp;
}

static void test_curve(void)
{
    CASE("the curve, at its points");
    static const struct { int mv, pct; } P[] = {
        { 4050, 100 }, { 3970, 90 }, { 3870, 80 }, { 3790, 70 }, { 3740, 60 }, { 3710, 50 },
        { 3670, 40 },  { 3640, 30 }, { 3610, 20 }, { 3560, 10 }, { 3480, 5 },  { 3400, 0 },
    };
    for (size_t i = 0; i < sizeof P / sizeof *P; i++) CHECK_EQ(lroundf(knob_batt_charge((float)P[i].mv)), P[i].pct);

    CASE("the curve: full from 4,050 mV, empty under 3,400, between them in a line");
    CHECK_EQ(lroundf(knob_batt_charge(4096)), 100);                  /* just off a full charge */
    CHECK_EQ(lroundf(knob_batt_charge(4500)), 100);
    CHECK_EQ(lroundf(knob_batt_charge(3399)), 0);
    CHECK_EQ(lroundf(knob_batt_charge(3000)), 0);
    CHECK_EQ(lroundf(knob_batt_charge(3725)), 55);
    CHECK_EQ(lroundf(knob_batt_charge(3920)), 85);

    CASE("the curve: never less charge for more volts");
    float last = -1;
    int   ups  = 0;
    for (int mv = 3300; mv <= 4300; mv++) {
        const float c = knob_batt_charge((float)mv);
        if (c < last) ups++;
        last = c;
    }
    CHECK_EQ(ups, 0);
}

static void test_settle(void)
{
    CASE("at boot: nothing said until the readings have settled");
    fresh();
    for (int i = 0; i < KNOB_BATT_SETTLE - 1; i++) {
        CHECK(!feed(4090));
        CHECK_EQ(b.src, KNOB_PWR_UNKNOWN);
        CHECK_EQ(b.pct, -1);
        CHECK_EQ(b.mv, 4090);
    }
    CHECK(feed(4085));
    CHECK_EQ(b.src, KNOB_PWR_BATTERY);
    CHECK_EQ(b.pct, 100);

    CASE("at boot on USB");
    fresh();
    for (int i = 0; i < KNOB_BATT_SETTLE - 1; i++) CHECK(!feed(4500 + 10 * i));
    CHECK(feed(4463));
    CHECK_EQ(b.src, KNOB_PWR_USB);
    CHECK_EQ(b.pct, -1);

    CASE("at boot on a battery half used: its charge from the readings' mean");
    fresh();
    static const int R[] = { 3700, 3720, 3730, 3700, 3700 };     /* mean 3710: 50 % */
    for (int i = 0; i < KNOB_BATT_SETTLE; i++) feed(R[i]);
    CHECK_EQ(b.src, KNOB_PWR_BATTERY);
    CHECK_EQ(b.pct, 50);

    CASE("plugged in while settling: settled on USB, five readings after");
    fresh();
    feed(4090);
    feed(4090);
    for (int i = 0; i < KNOB_BATT_SETTLE - 1; i++) {
        feed(4520);
        CHECK_EQ(b.src, KNOB_PWR_UNKNOWN);
    }
    CHECK(feed(4520));
    CHECK_EQ(b.src, KNOB_PWR_USB);

    CASE("...and unplugged while settling: on the battery, the USB readings not in its charge");
    fresh();
    feed(4520);
    feed(4520);
    feed(4520);
    for (int i = 0; i < KNOB_BATT_SETTLE - 1; i++) feed(3740);
    CHECK_EQ(b.src, KNOB_PWR_UNKNOWN);
    CHECK(feed(3740));
    CHECK_EQ(b.src, KNOB_PWR_BATTERY);
    CHECK_EQ(b.pct, 60);

    CASE("at boot, the line: under 4,200 mV the battery, from it USB -- no cell under load reads that");
    static const struct { int mv, src; } L[] = {
        { KNOB_USB_OFF_MV - 1, KNOB_PWR_BATTERY }, { KNOB_USB_OFF_MV, KNOB_PWR_USB },
        { KNOB_USB_OFF_MV + 1, KNOB_PWR_USB },     { 4250, KNOB_PWR_USB },     /* a weak USB supply */
    };
    for (size_t i = 0; i < sizeof L / sizeof *L; i++) {
        fresh();
        for (int k = 0; k < KNOB_BATT_SETTLE; k++) feed(L[i].mv);
        CHECK_EQ(b.src, L[i].src);
        CHECK_EQ(b.pct, L[i].src == KNOB_PWR_BATTERY ? 100 : -1);
    }
}

static void test_plug(void)
{
    CASE("unplugged: the battery on the second reading under 4,200 mV, its charge from the two");
    fresh();
    for (int i = 0; i < KNOB_BATT_SETTLE; i++) feed(4500);
    CHECK(!feed(3880));
    CHECK_EQ(b.src, KNOB_PWR_USB);
    CHECK(feed(3920));
    CHECK_EQ(b.src, KNOB_PWR_BATTERY);
    CHECK_EQ(b.pct, 85);                                          /* 83 %, shown in fives */

    CASE("plugged in: USB on the second reading from 4,300 mV, no charge");
    CHECK(!feed(4470));
    CHECK_EQ(b.src, KNOB_PWR_BATTERY);
    CHECK_EQ(b.pct, 85);
    CHECK(feed(4470));
    CHECK_EQ(b.src, KNOB_PWR_USB);
    CHECK_EQ(b.pct, -1);
    CHECK(!feed(4560));

    CASE("one reading past the line is neither: a dip on USB, a spike on the battery");
    for (int i = 0; i < 20; i++) {
        CHECK(!feed(i % 2 ? 4500 : 3700));
        CHECK_EQ(b.src, KNOB_PWR_USB);
    }
    on_battery_at(3740);
    for (int i = 0; i < 20; i++) {
        CHECK(!feed(i % 2 ? 3740 : 4500));
        CHECK_EQ(b.src, KNOB_PWR_BATTERY);
        CHECK_EQ(b.pct, 60);
    }
    CHECK(!feed(4500));                                           /* ...nor two with a rail between them */
    CHECK(!feed(4250));
    CHECK(!feed(4500));
    CHECK_EQ(b.src, KNOB_PWR_BATTERY);

    CASE("on USB, a rail that sags stays USB, down to 4,200 mV");
    fresh();
    for (int i = 0; i < KNOB_BATT_SETTLE; i++) feed(4500);
    for (int mv = KNOB_USB_ON_MV - 1; mv >= KNOB_USB_OFF_MV; mv -= 7) {
        CHECK(!feed(mv));
        CHECK_EQ(b.src, KNOB_PWR_USB);
    }
    CHECK(!feed(KNOB_USB_OFF_MV - 1));
    CHECK(feed(KNOB_USB_OFF_MV - 1));
    CHECK_EQ(b.src, KNOB_PWR_BATTERY);
    CHECK_EQ(b.pct, 100);

    CASE("on the battery, a rail that swells stays the battery, up to 4,300 mV");
    for (int mv = 4100; mv < KNOB_USB_ON_MV; mv += 7) {
        CHECK(!feed(mv));
        CHECK_EQ(b.src, KNOB_PWR_BATTERY);
        CHECK_EQ(b.pct, 100);
    }
    CHECK(!feed(KNOB_USB_ON_MV));
    CHECK(feed(KNOB_USB_ON_MV));
    CHECK_EQ(b.src, KNOB_PWR_USB);

    CASE("on the battery, a rail over what a cell gives is none of the cell's: the charge stays");
    on_battery_at(3740);
    const float was = b.smooth_mv;
    for (int i = 0; i < 300; i++) {
        CHECK(!feed(4210 + (i * 37) % 80));                       /* 4,210-4,289 */
        CHECK_EQ(b.src, KNOB_PWR_BATTERY);
        CHECK_EQ(b.pct, 60);
    }
    CHECK(fabsf(b.smooth_mv - was) < 0.01f);

    CASE("a USB port at its lowest, 4.75 V less the diode -- 4,300 mV, give or take 25 -- for an hour");
    fresh();                                                      /* from boot: USB throughout */
    int flips = 0, batt = 0;
    for (int i = 0; i < 3600; i++) {
        if (feed(4300 + noise(25)) && i >= KNOB_BATT_SETTLE) flips++;
        if (b.src == KNOB_PWR_BATTERY) batt++;
    }
    CHECK_EQ(b.src, KNOB_PWR_USB);
    CHECK_EQ(flips, 0);
    CHECK_EQ(batt, 0);
    on_battery_at(3740);                                          /* plugged into one: seen, and kept */
    int seen = -1;
    flips = 0;
    for (int i = 0; i < 3600; i++) {
        const bool changed = feed(4300 + noise(25));
        if (b.src == KNOB_PWR_USB && seen < 0) seen = i;
        else if (changed) flips++;
        if (seen < 0) CHECK_EQ(b.pct, 60);                        /* the cell's charge until then */
    }
    CHECK(seen >= 1 && seen < 30);
    CHECK_EQ(flips, 0);
    CHECK_EQ(b.src, KNOB_PWR_USB);

    CASE("USB as measured, a reading in thirty 300 mV low: never the battery");
    fresh();
    for (int i = 0; i < 3600; i++) {
        feed(i % 30 == 17 ? 4180 : 4460 + (i * 37) % 101);
        if (i >= KNOB_BATT_SETTLE) CHECK_EQ(b.src, KNOB_PWR_USB);
    }

    CASE("the measured readings: USB 4,460-4,560 mV, the battery 4,080-4,100");
    fresh();
    for (int i = 0; i < 600; i++) {
        feed(4460 + (i * 37) % 101);
        CHECK_EQ(b.src, i < KNOB_BATT_SETTLE - 1 ? KNOB_PWR_UNKNOWN : KNOB_PWR_USB);
    }
    for (int i = 0; i < 600; i++) {
        feed(4080 + (i * 13) % 21);
        CHECK_EQ(b.src, i < KNOB_BATT_CONFIRM - 1 ? KNOB_PWR_USB : KNOB_PWR_BATTERY);
        CHECK_EQ(b.pct, i < KNOB_BATT_CONFIRM - 1 ? -1 : 100);
    }

    CASE("a reading no rail that runs the knob could give: not taken");
    on_battery_at(3740);
    static const int BAD[] = { -1, 0, 1200, KNOB_RAIL_MIN_MV - 1, KNOB_RAIL_MAX_MV + 1, 6200 };
    for (size_t i = 0; i < sizeof BAD / sizeof *BAD; i++) {
        CHECK(!feed(BAD[i]));
        CHECK_EQ(b.mv, 3740);
        CHECK_EQ(b.src, KNOB_PWR_BATTERY);
        CHECK_EQ(b.pct, 60);
    }
    fresh();
    for (int i = 0; i < 20; i++) CHECK(!feed(-1));
    CHECK_EQ(b.src, KNOB_PWR_UNKNOWN);
    CHECK_EQ(b.mv, -1);
}

static void test_steady(void)
{
    CASE("WiFi's bursts: a reading in thirty 100 mV low, for an hour -- the charge stays");
    on_battery_at(3740);
    for (int i = 0; i < 3600; i++) {
        CHECK(!feed(i % 30 == 7 ? 3640 : 3740 + noise(10)));
        CHECK_EQ(b.pct, 60);
    }

    CASE("...and one deep dip, 300 mV: the charge stays");
    on_battery_at(3740);
    for (int i = 0; i < 5; i++) {
        CHECK(!feed(3440));
        CHECK(!feed(3740));
        for (int k = 0; k < 120; k++) feed(3740);
        CHECK_EQ(b.pct, 60);
    }

    CASE("the backlight off, a lighter load: the rail 25 mV up for ten minutes -- the charge stays");
    for (int i = 0; i < 600; i++) {
        CHECK(!feed(3765 + noise(8)));
        CHECK_EQ(b.pct, 60);
    }
    CHECK(knob_batt_charge(b.smooth_mv) > 64);                    /* it moved, and did not show */

    CASE("...and on again: the charge stays");
    for (int i = 0; i < 600; i++) {
        CHECK(!feed(3740 + noise(8)));
        CHECK_EQ(b.pct, 60);
    }

    CASE("settled, a far lighter load: the rail 100 mV up for an hour -- the charge shown never rises");
    for (int i = 0; i < 3600; i++) {
        CHECK(!feed(3840 + noise(8)));
        CHECK_EQ(b.pct, 60);
    }
    CHECK(knob_batt_charge(b.smooth_mv) > 75);

    CASE("smoothed over some 30 s: half way in about 20, nearly all in 2 min");
    on_battery_at(3740);
    for (int i = 0; i < 21; i++) feed(3710);
    CHECK(b.smooth_mv < 3727 && b.smooth_mv > 3723);
    for (int i = 0; i < 100; i++) feed(3710);
    CHECK(b.smooth_mv < 3712);
    CHECK_EQ(b.pct, 50);

    CASE("hovering at a five's halfway mark: no flicker, where rounding alone would");
    on_battery_at(3718);                                          /* 52.7 %: shown 55 */
    CHECK_EQ(b.pct, 55);
    int changes = 0, rounded = 0, was = 55;
    for (int i = 0; i < 3600; i++) {
        if (feed(3717 + noise(12))) changes++;
        const int r = (int)((knob_batt_charge(b.smooth_mv) + 2.5f) / 5.0f) * 5;
        if (r != was) rounded++;
        was = r;
    }
    CHECK_EQ(changes, 0);
    CHECK_EQ(b.pct, 55);
    CHECK(rounded > 0);

    CASE("a reading or two late, or in the same ms: still smoothed, nothing jumps");
    on_battery_at(3740);
    CHECK(!knob_batt_feed(&b, 3700, now));                        /* no time gone: no weight */
    CHECK(b.smooth_mv > 3739.9f);
    now += 10000;
    CHECK(!knob_batt_feed(&b, 3740, now));
    CHECK_EQ(b.pct, 60);

    CASE("the ms clock wrapping: as any other second");
    fresh();
    now = UINT32_MAX - 40500;
    for (int i = 0; i < KNOB_BATT_FREE; i++) feed(3740);         /* settled, before the wrap */
    for (int i = 0; i < 30; i++) feed(3710);                      /* across it */
    CHECK(now < 60000);
    CHECK(b.smooth_mv < 3725 && b.smooth_mv > 3715);
}

static void test_settling(void)
{
    CASE("unplugged on a reading 100 mV low: low at first, up as the readings come, its own five at the half minute");
    fresh();
    for (int i = 0; i < KNOB_BATT_SETTLE; i++) feed(4500);
    feed(3640);                                                   /* a WiFi burst */
    feed(3740);                                                   /* 60 % */
    CHECK_EQ(b.src, KNOB_PWR_BATTERY);
    CHECK_EQ(b.pct, 45);
    int last = b.pct, downs = 0;
    for (int taken = KNOB_BATT_CONFIRM + 1; taken <= KNOB_BATT_FREE; taken++) {
        feed(3740);
        if (b.pct < last) downs++;
        last = b.pct;
        if (taken == 10) CHECK_EQ(b.pct, 55);                     /* the mean 3,730: 57 % */
    }
    CHECK_EQ(downs, 0);
    CHECK_EQ(b.pct, 60);
    for (int i = 0; i < 600; i++) feed(3740 + noise(8));
    CHECK_EQ(b.pct, 60);

    CASE("...and at half the charge, 17 mV low: green at the half minute, not yellow");
    fresh();
    for (int i = 0; i < KNOB_BATT_SETTLE; i++) feed(4500);
    feed(3695);
    for (int i = 1; i < KNOB_BATT_FREE; i++) feed(3712);          /* 50.7 % */
    CHECK_EQ(b.pct, 50);
    for (int i = 0; i < 600; i++) feed(3712);
    CHECK_EQ(b.pct, 50);

    CASE("...and booted on a reading 100 mV low: the same");
    fresh();
    feed(3640);
    for (int i = 1; i < KNOB_BATT_FREE; i++) feed(3740);
    CHECK_EQ(b.pct, 60);

    CASE("while it settles, a halfway mark does not flicker it; at the half minute, the nearest five");
    fresh();
    for (int i = 0; i < KNOB_BATT_SETTLE; i++) feed(4500);
    int changes = 0;
    for (int i = 0; i < KNOB_BATT_FREE - 1; i++)
        if (feed(3717 + noise(12)) && i >= KNOB_BATT_CONFIRM) changes++;   /* 52.3 %, by 50 and 55 */
    CHECK_EQ(changes, 0);
    feed(3717);
    CHECK_EQ(b.pct, (int)((knob_batt_charge(b.smooth_mv) + 2.5f) / 5.0f) * 5);

    CASE("the half minute over: down only, by the smoothed rail");
    on_battery_at(3790);                                          /* 70 % */
    CHECK_EQ(b.pct, 70);
    for (int i = 0; i < 300; i++) feed(3760);                     /* 64 % */
    CHECK_EQ(b.pct, 65);
    for (int i = 0; i < 300; i++) feed(3790);
    CHECK_EQ(b.pct, 65);
}

static void test_rundown(void)
{
    CASE("a run-down, three hours from just off a full charge to the regulator's end");
    fresh();
    const int    secs = 3 * 3600;
    int          last = 100, ups = 0, odd = 0, far = 0, greens = 0, yellows = 0, reds = 0;
    int          colour = -1;                                     /* 0 green, 1 yellow, 2 red */
    for (int t = 0; t < secs; t++) {
        const float rail = 4100.0f - 720.0f * (float)t / (float)secs;   /* to 3,380 */
        const int   mv   = (int)rail + noise(15) - (t % 30 == 11 ? 90 : 0);
        feed(mv);
        if (b.src != KNOB_PWR_BATTERY) continue;
        if (b.pct > last) ups++;
        if (b.pct % 5 || b.pct < 0 || b.pct > 100) odd++;
        if (t > 60 && fabsf((float)b.pct - knob_batt_charge(rail)) > 7.5f) far++;
        const int c = b.pct >= 50 ? 0 : b.pct > 20 ? 1 : 2;
        if (c != colour) {
            if (c == 0) greens++;
            if (c == 1) yellows++;
            if (c == 2) reds++;
            colour = c;
        }
        last = b.pct;
    }
    CHECK_EQ(ups, 0);                                             /* never back up */
    CHECK_EQ(odd, 0);                                             /* always a five */
    CHECK_EQ(far, 0);                                             /* within a step and a half */
    CHECK_EQ(greens, 1);                                          /* each colour once, in order */
    CHECK_EQ(yellows, 1);
    CHECK_EQ(reds, 1);
    CHECK_EQ(b.pct, 0);
}

T_MAIN({
    test_curve();
    test_settle();
    test_plug();
    test_steady();
    test_settling();
    test_rundown();
})
