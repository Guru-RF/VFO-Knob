/* The Clock firmware's alarms (components/vfo_core/src/alarm.c): due at its
 * minute and on its days only, once on any day; the next ring and how far
 * off, over midnight and the week's end; the dial's days and the face's
 * words for them; the times in 24 and 12 hours. */
#include "tiny.h"
#include "alarm.h"

/* A local time: 2026-11-04 was a Wednesday (tm_wday 3). */
static struct tm at(int wday, int hour, int min)
{
    struct tm t = { 0 };
    t.tm_wday = wday;
    t.tm_hour = hour;
    t.tm_min  = min;
    return t;
}

static void test_bits(void)
{
    CASE("days: Monday the lowest bit, Sunday the highest");
    CHECK_EQ(alarm_day_bit(1), ALARM_MON);
    CHECK_EQ(alarm_day_bit(0), ALARM_SUN);
    CHECK_EQ(alarm_day_bit(3), 0x04);
    CHECK_EQ(alarm_day_bit(6), 0x20);

    CASE("bounds");
    CHECK(alarm_ok(&(alarm_t){ 23, 59, ALARM_DAILY, true }));
    CHECK(!alarm_ok(&(alarm_t){ 24, 0, 0, true }));
    CHECK(!alarm_ok(&(alarm_t){ 7, 60, 0, true }));
    CHECK(!alarm_ok(&(alarm_t){ 7, 0, 0x80, true }));
}

static void test_due(void)
{
    alarm_t a[ALARM_N] = { { 0 } };
    a[2] = (alarm_t){ 7, 30, ALARM_WORK, true };
    a[5] = (alarm_t){ 9, 0, 0, true };
    a[7] = (alarm_t){ 7, 30, ALARM_WEEKEND, true };
    a[8] = (alarm_t){ 7, 30, ALARM_DAILY, false };

    CASE("due: at its minute, on its days");
    CHECK_EQ(alarm_due(a, ALARM_N, &(struct tm){ .tm_wday = 3, .tm_hour = 7, .tm_min = 30 }), 2);
    CHECK_EQ(alarm_due(a, ALARM_N, &(struct tm){ .tm_wday = 3, .tm_hour = 7, .tm_min = 31 }), -1);
    CHECK_EQ(alarm_due(a, ALARM_N, &(struct tm){ .tm_wday = 3, .tm_hour = 7, .tm_min = 29 }), -1);
    struct tm sat = at(6, 7, 30), sun = at(0, 7, 30);
    CHECK_EQ(alarm_due(a, ALARM_N, &sat), 7);
    CHECK_EQ(alarm_due(a, ALARM_N, &sun), 7);

    CASE("due: once, on any day");
    struct tm wed9 = at(3, 9, 0), sun9 = at(0, 9, 0);
    CHECK_EQ(alarm_due(a, ALARM_N, &wed9), 5);
    CHECK_EQ(alarm_due(a, ALARM_N, &sun9), 5);

    CASE("due: never while off, nor out of bounds");
    a[2].on = a[7].on = false;
    struct tm wed730 = at(3, 7, 30);
    CHECK_EQ(alarm_due(a, ALARM_N, &wed730), -1);
    a[3] = (alarm_t){ 7, 30, 0x80 | ALARM_DAILY, true };
    CHECK_EQ(alarm_due(a, ALARM_N, &wed730), -1);
}

static void test_next(void)
{
    CASE("minutes to: later today, tomorrow, the days skipped");
    alarm_t w = { 7, 30, ALARM_WORK, true };
    struct tm wed6 = at(3, 6, 0), wed8 = at(3, 8, 0), fri8 = at(5, 8, 0), sat23 = at(6, 23, 0);
    CHECK_EQ(alarm_minutes_to(&w, &wed6), 90);
    CHECK_EQ(alarm_minutes_to(&w, &wed8), 1440 - 30);
    CHECK_EQ(alarm_minutes_to(&w, &fri8), 3 * 1440 - 30);    /* to Monday */
    CHECK_EQ(alarm_minutes_to(&w, &sat23), 2 * 1440 + 7 * 60 + 30 - 23 * 60);   /* past Sunday */

    CASE("minutes to: at its own minute, the next one -- a week, or once a day");
    struct tm wed730 = at(3, 7, 30);
    CHECK_EQ(alarm_minutes_to(&(alarm_t){ 7, 30, 0x04, true }, &wed730), 7 * 1440);
    CHECK_EQ(alarm_minutes_to(&(alarm_t){ 7, 30, 0, true }, &wed730), 1440);
    CHECK_EQ(alarm_minutes_to(&(alarm_t){ 7, 31, 0, true }, &wed730), 1);

    CASE("minutes to: over midnight, and off");
    struct tm sun2359 = at(0, 23, 59);
    CHECK_EQ(alarm_minutes_to(&(alarm_t){ 0, 0, ALARM_MON, true }, &sun2359), 1);
    CHECK_EQ(alarm_minutes_to(&(alarm_t){ 0, 0, ALARM_MON, false }, &sun2359), -1);

    CASE("next: the soonest, the lowest of two alike, none while all are off");
    alarm_t a[ALARM_N] = { { 0 } };
    int m = 0;
    CHECK_EQ(alarm_next(a, ALARM_N, &wed6, &m), -1);
    a[4] = (alarm_t){ 6, 45, 0, true };
    a[1] = (alarm_t){ 6, 30, ALARM_DAILY, true };
    a[6] = (alarm_t){ 6, 30, 0, true };
    CHECK_EQ(alarm_next(a, ALARM_N, &wed6, &m), 1);
    CHECK_EQ(m, 30);
    a[1].on = false;
    CHECK_EQ(alarm_next(a, ALARM_N, &wed6, &m), 6);
    a[6].on = false;
    CHECK_EQ(alarm_next(a, ALARM_N, &wed6, &m), 4);
    CHECK_EQ(m, 45);
}

static void test_words(void)
{
    char t[24];
    CASE("the dial's days: each its own, and back");
    for (int r = 0; r < ALARM_REPS; r++) CHECK_EQ(alarm_rep_of(alarm_rep_days(r)), r);
    CHECK_EQ(alarm_rep_of(0x15), -1);
    CHECK_EQ(alarm_rep_days(ALARM_REPS), 0);
    CHECK_EQ(alarm_rep_days(-1), 0);

    CASE("the days in words");
    alarm_days_text(0, t, sizeof t);              CHECK_STR(t, "ONCE");
    alarm_days_text(ALARM_DAILY, t, sizeof t);    CHECK_STR(t, "EVERY DAY");
    alarm_days_text(ALARM_WORK, t, sizeof t);     CHECK_STR(t, "MON-FRI");
    alarm_days_text(ALARM_WEEKEND, t, sizeof t);  CHECK_STR(t, "SAT, SUN");
    alarm_days_text(0x04, t, sizeof t);           CHECK_STR(t, "WED");
    alarm_days_text(ALARM_SUN, t, sizeof t);      CHECK_STR(t, "SUN");
    alarm_days_text(0x15, t, sizeof t);           CHECK_STR(t, "MO WE FR");
    alarm_days_text(0x3F, t, sizeof t);           CHECK_STR(t, "MO TU WE TH FR SA");
    alarm_days_text(0x15, t, 6);                  CHECK_STR(t, "MO WE");   /* cut short, never mid-day */

    CASE("the time, 24 hours and 12");
    bool pm = false;
    alarm_time_text(7, 5, true, t, sizeof t, &pm);   CHECK_STR(t, "07:05"); CHECK(!pm);
    alarm_time_text(0, 0, false, t, sizeof t, &pm);  CHECK_STR(t, "12:00"); CHECK(!pm);
    alarm_time_text(12, 30, false, t, sizeof t, &pm); CHECK_STR(t, "12:30"); CHECK(pm);
    alarm_time_text(23, 59, false, t, sizeof t, &pm); CHECK_STR(t, "11:59"); CHECK(pm);
    alarm_time_text(23, 59, true, t, sizeof t, NULL); CHECK_STR(t, "23:59");
}

int main(void)
{
    test_bits();
    test_due();
    test_next();
    test_words();
    printf("%d checks, %d failed\n", t_run, t_fail);
    return t_fail ? 1 : 0;
}
