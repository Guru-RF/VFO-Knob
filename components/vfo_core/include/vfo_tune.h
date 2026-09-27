/* Tuning model: step, acceleration and the anchor quantiser.
 *
 * Two ideas carry the feel of the knob:
 *
 *  1. Step is ABSOLUTE, acceleration is MOMENTARY. Tapping a digit chooses the
 *     decade you are working in and it stays chosen; velocity only multiplies
 *     it. The alternative (velocity picks the step) means the operator cannot
 *     choose a resolution and keep it -- the knob decides, and it feels like
 *     it is guessing.
 *
 *  2. f_display == f_anchor + step_units * step_hz, always. Without this you
 *     spin fast, stop, and land on 14074003 Hz, and the digits below your step
 *     flicker forever. Re-latching the anchor on every step change is what
 *     makes the inactive digits sit still.
 */
#ifndef VFO_TUNE_H
#define VFO_TUNE_H

#include <stdbool.h>
#include <stdint.h>

/* Velocity below which the next detent is guaranteed to be exactly one step.
 * This is the promise that you can always land on an exact frequency. */
#define ACCEL_REZERO_MS 180u

#define TUNE_MAX_STEP_EFF_HZ   25000   /* per detent */
#define TUNE_MAX_RATE_HZ_PER_S 600000  /* a violent flick must not cross 3 bands */

typedef struct {
    float    v_detents;        /* EWMA, detents/second */
    uint32_t t_last_ms;
    uint8_t  band;             /* 0..5, retained for hysteresis */
} accel_t;

void    accel_init(accel_t *a);

/* Feed one detent batch; returns the acceleration multiplier (1,2,5,10,25,50).
 * Bands step up at 8/16/28/45/70 det/s and back down at 80% of each, so a
 * velocity hovering on a boundary does not chatter between multipliers. */
uint8_t accel_update(accel_t *a, int32_t detents, uint32_t now_ms);

/* Round f down onto the step grid. */
int64_t tune_quantise(int64_t f, int32_t step_hz);

typedef struct {
    int64_t f_display;
    int64_t f_anchor;
    int32_t step_units;
    int32_t step_hz;
} tune_t;

void tune_init(tune_t *t, int64_t f, int32_t step_hz);

/* Re-latch the anchor onto the new grid. f_display is unchanged. */
void tune_set_step(tune_t *t, int32_t new_step_hz);

/* Adopt an externally-authoritative frequency (greeting, or a remote change). */
void tune_assign(tune_t *t, int64_t f);

/* Apply detents. Returns the new f_display.
 *
 * TUNE_MAX_STEP_EFF_HZ caps the ACCELERATION multiplier, not the operator's
 * chosen step: with step_hz = 100 kHz one detent still moves 100 kHz, but
 * velocity cannot inflate it. TUNE_MAX_RATE_HZ_PER_S caps overall travel
 * (while always permitting at least one whole detent), and the result is
 * clamped to [lo_hz, hi_hz]. */
int64_t tune_apply(tune_t *t, int32_t detents, uint8_t accel_mult,
                   uint32_t dt_ms, int64_t lo_hz, int64_t hi_hz);

#endif /* VFO_TUNE_H */
