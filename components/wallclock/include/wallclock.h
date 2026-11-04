/* The knob's wall clock: the time from the internet, in the owner's zone.
 *
 * The board has no clock chip, so there is no time at all until an NTP server
 * has answered once after a start, and the time drifts on the crystal between
 * answers (asked for again every hour). Every firmware starts it: the clock
 * screensaver, the Clock and the Weather firmwares show the time, and
 * SVXConnect and the Telephone date their certificates and calls by it.
 *
 * Any task may call these; none blocks. */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

/* SNTP from pool.ntp.org, once; again is nothing. After esp_netif_init. */
void wallclock_start(void);

/* The zone as a POSIX rule ("CET-1CEST,M3.5.0,M10.5.0/3"): the page turns the
 * owner's IANA name into one from its table (www/tz.txt). */
void wallclock_set_zone(const char *rule);

/* Seconds since the last answer, UINT32_MAX before the first. */
uint32_t wallclock_age_s(void);

/* An answer within the last day: a clock older than that is shown as stale,
 * as Analog_Clock-esp32 hides its hands. */
static inline bool wallclock_fresh(void) { return wallclock_age_s() <= 24u * 3600u; }

/* Now, local and UTC, and the milliseconds into the second (any of them may
 * be NULL): false before the first answer, with nothing filled in. */
bool wallclock_now(struct tm *local, struct tm *utc, uint16_t *ms);
