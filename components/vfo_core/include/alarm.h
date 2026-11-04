/* The Clock firmware's alarms (VFO_RADIO=clock): ten, each a time of day in
 * the owner's zone, the days it rings on, and on or off. Which one is due at
 * a minute, which rings next and in how long, and the days in words. Plain C,
 * so test/host checks it on the PC (test_alarm). */
#ifndef ALARM_H
#define ALARM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#define ALARM_N 10

/* The days, a bit each, Monday the lowest. None: once, the next time its
 * hour comes, and then off. */
#define ALARM_MON     0x01
#define ALARM_SUN     0x40
#define ALARM_DAILY   0x7F
#define ALARM_WORK    0x1F          /* Monday to Friday */
#define ALARM_WEEKEND 0x60

typedef struct {
    uint8_t hour, min;              /* local, 0-23 and 0-59 */
    uint8_t days;                   /* ALARM_* bits; 0, once */
    bool    on;
} alarm_t;

/* Its time and days within bounds. */
bool alarm_ok(const alarm_t *a);

/* struct tm's day of the week (0, Sunday) as an alarm's day bit. */
uint8_t alarm_day_bit(int wday);

/* The first alarm on that rings at the minute `lt` (local time): its index,
 * -1 none. */
int alarm_due(const alarm_t *a, int n, const struct tm *lt);

/* Minutes from the minute `lt` to the alarm's next ring, 1 to a week: one
 * at this very minute is a week away, or a day once. -1 while it is off. */
int alarm_minutes_to(const alarm_t *a, const struct tm *lt);

/* The alarm that rings next, -1 none on; its minutes away in *minutes. The
 * lowest index first, of two at the same time. */
int alarm_next(const alarm_t *a, int n, const struct tm *lt, int *minutes);

/* The days the dial goes through, in turn: once, every day, Monday to Friday,
 * Saturday and Sunday, then each day alone. */
#define ALARM_REPS 11
uint8_t alarm_rep_days(int rep);
/* The days as one of those, -1 when they are none of them (set on the page). */
int     alarm_rep_of(uint8_t days);

/* The days in words, as the face writes them: "ONCE", "EVERY DAY",
 * "MON-FRI", "SAT, SUN", "WED", or for days set on the page each by its two
 * letters, "MO WE FR". Fits in 24 bytes. */
void alarm_days_text(uint8_t days, char *out, size_t n);

/* The time as the face writes it: "07:30", or in 12 hours "7:30" with *pm
 * set for the afternoon (pm may be NULL). Fits in 6 bytes. */
void alarm_time_text(uint8_t hour, uint8_t min, bool h24, char *out, size_t n, bool *pm);

#endif
