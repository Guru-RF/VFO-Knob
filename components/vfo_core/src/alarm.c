#include "alarm.h"

#include <stdio.h>
#include <string.h>

bool alarm_ok(const alarm_t *a)
{
    return a->hour < 24 && a->min < 60 && !(a->days & ~ALARM_DAILY);
}

uint8_t alarm_day_bit(int wday)
{
    /* Monday the lowest bit: Sunday, 0 in struct tm, the highest. */
    return (uint8_t)(1u << ((wday + 6) % 7));
}

int alarm_due(const alarm_t *a, int n, const struct tm *lt)
{
    for (int i = 0; i < n; i++) {
        if (!a[i].on || !alarm_ok(&a[i])) continue;
        if (a[i].hour != lt->tm_hour || a[i].min != lt->tm_min) continue;
        if (!a[i].days || (a[i].days & alarm_day_bit(lt->tm_wday))) return i;
    }
    return -1;
}

int alarm_minutes_to(const alarm_t *a, const struct tm *lt)
{
    if (!a->on || !alarm_ok(a)) return -1;
    const int now = lt->tm_hour * 60 + lt->tm_min, at = a->hour * 60 + a->min;
    /* Today, the days after it, and today a week on: the first of its days
     * still to come. */
    for (int d = 0; d <= 7; d++) {
        const int m = d * 1440 + at - now;
        if (m <= 0) continue;
        if (!a->days || (a->days & alarm_day_bit((lt->tm_wday + d) % 7))) return m;
    }
    return -1;
}

int alarm_next(const alarm_t *a, int n, const struct tm *lt, int *minutes)
{
    int best = -1, least = 0;
    for (int i = 0; i < n; i++) {
        const int m = alarm_minutes_to(&a[i], lt);
        if (m > 0 && (best < 0 || m < least)) { best = i; least = m; }
    }
    if (minutes) *minutes = least;
    return best;
}

static const uint8_t REP[ALARM_REPS] = {
    0, ALARM_DAILY, ALARM_WORK, ALARM_WEEKEND, 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40,
};

uint8_t alarm_rep_days(int rep) { return rep >= 0 && rep < ALARM_REPS ? REP[rep] : 0; }

int alarm_rep_of(uint8_t days)
{
    for (int i = 0; i < ALARM_REPS; i++)
        if (REP[i] == days) return i;
    return -1;
}

void alarm_days_text(uint8_t days, char *out, size_t n)
{
    static const char *const ONE[7] = { "MON", "TUE", "WED", "THU", "FRI", "SAT", "SUN" };
    static const char *const TWO[7] = { "MO", "TU", "WE", "TH", "FR", "SA", "SU" };
    if (!n) return;
    days &= ALARM_DAILY;
    if (!days)                 { snprintf(out, n, "ONCE");      return; }
    if (days == ALARM_DAILY)   { snprintf(out, n, "EVERY DAY"); return; }
    if (days == ALARM_WORK)    { snprintf(out, n, "MON-FRI");   return; }
    if (days == ALARM_WEEKEND) { snprintf(out, n, "SAT, SUN");  return; }
    const int rep = alarm_rep_of(days);
    if (rep >= 4) { snprintf(out, n, "%s", ONE[rep - 4]); return; }
    size_t at = 0;
    out[0] = 0;
    for (int d = 0; d < 7; d++) {
        if (!(days & (1u << d))) continue;
        const int k = snprintf(out + at, n - at, "%s%s", at ? " " : "", TWO[d]);
        if (k < 0 || (size_t)k >= n - at) { out[at] = 0; return; }
        at += (size_t)k;
    }
}

void alarm_time_text(uint8_t hour, uint8_t min, bool h24, char *out, size_t n, bool *pm)
{
    hour %= 24;
    min %= 60;
    if (pm) *pm = hour >= 12;
    if (h24) snprintf(out, n, "%02u:%02u", (unsigned)hour, (unsigned)min);
    else     snprintf(out, n, "%u:%02u", (unsigned)(hour % 12 ? hour % 12 : 12), (unsigned)min);
}
