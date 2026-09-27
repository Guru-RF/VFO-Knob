#include "vfo_tune.h"

#include <stdlib.h>
#include <string.h>

/* Band boundaries in detents/second. Each rung is x1.75 in velocity and x2.2
 * in multiplier, so effective Hz/s grows roughly as v^1.35 -- superlinear
 * enough to feel like leaning into it, shallow enough that you do not
 * overshoot by a decade. Down-boundaries are 80% of up, so a velocity sitting
 * on a threshold does not chatter between multipliers. */
static const float   ACCEL_UP[5]   = { 8.0f, 16.0f, 28.0f, 45.0f, 70.0f };
static const uint8_t ACCEL_MULT[6] = { 1, 2, 5, 10, 25, 50 };

/* Beyond this the anchor is re-latched to keep step_units from growing without
 * bound over a long session. f_display is unchanged, so this is invisible. */
#define UNITS_RELATCH 1000000

void accel_init(accel_t *a)
{
    if (!a) return;
    memset(a, 0, sizeof *a);
}

uint8_t accel_update(accel_t *a, int32_t detents, uint32_t now_ms)
{
    if (!a) return 1;
    if (detents == 0) return ACCEL_MULT[a->band];

    uint32_t dt = now_ms - a->t_last_ms;   /* wraparound-safe */
    a->t_last_ms = now_ms;

    if (dt > ACCEL_REZERO_MS) {
        /* The promise: after any spin, the very next slow click is exactly one
         * step. Without this you cannot land on an exact frequency and the
         * knob feels like it is guessing. */
        a->v_detents = 0.0f;
        a->band      = 0;
        return ACCEL_MULT[0];
    }
    if (dt == 0) dt = 1;

    int32_t n = detents < 0 ? -detents : detents;
    float inst = 1000.0f * (float)n / (float)dt;
    a->v_detents += (inst - a->v_detents) * 0.45f;   /* EWMA, tau ~2.2 detents */

    uint8_t b = a->band;
    while (b < 5 && a->v_detents >= ACCEL_UP[b])            b++;
    while (b > 0 && a->v_detents <  ACCEL_UP[b - 1] * 0.8f) b--;
    a->band = b;
    return ACCEL_MULT[b];
}

int64_t tune_quantise(int64_t f, int32_t step_hz)
{
    if (step_hz <= 0) return f;
    int64_t s = step_hz;
    int64_t q = f / s;
    if ((f % s) != 0 && f < 0) q--;      /* floor, not truncate */
    return q * s;
}

void tune_init(tune_t *t, int64_t f, int32_t step_hz)
{
    if (!t) return;
    t->step_hz    = step_hz > 0 ? step_hz : 1;
    t->f_anchor   = tune_quantise(f, t->step_hz);
    t->step_units = 0;
    t->f_display  = t->f_anchor;
}

void tune_set_step(tune_t *t, int32_t new_step_hz)
{
    if (!t || new_step_hz <= 0) return;
    t->step_hz    = new_step_hz;
    t->f_anchor   = tune_quantise(t->f_display, new_step_hz);
    t->step_units = 0;
    t->f_display  = t->f_anchor;
}

void tune_assign(tune_t *t, int64_t f)
{
    if (!t) return;
    t->f_anchor   = tune_quantise(f, t->step_hz);
    t->step_units = 0;
    t->f_display  = f;
    /* f_display is the authoritative remote value even if it is off-grid; the
     * next step change re-latches it. Keeping it exact matters more than the
     * grid, because this value came from the radio. */
}

int64_t tune_apply(tune_t *t, int32_t detents, uint8_t accel_mult,
                   uint32_t dt_ms, int64_t lo_hz, int64_t hi_hz)
{
    if (!t || detents == 0) return t ? t->f_display : 0;
    if (accel_mult < 1) accel_mult = 1;

    /* Work in step units so f_display stays exactly on the grid.
     *
     * The cap applies to ACCELERATION ONLY, never to the step the operator
     * chose. Tapping the 100 kHz digit means one detent moves 100 kHz even
     * though that exceeds TUNE_MAX_STEP_EFF_HZ -- they asked for it. What the
     * cap prevents is velocity silently multiplying it to 5 MHz. */
    int32_t mult = accel_mult;
    if ((int64_t)t->step_hz * mult > TUNE_MAX_STEP_EFF_HZ) {
        mult = (int32_t)(TUNE_MAX_STEP_EFF_HZ / t->step_hz);
        if (mult < 1) mult = 1;   /* base step always gets through */
    }
    int64_t units = (int64_t)detents * mult;

    /* Overall rate clamp, so a violent flick cannot cross three bands. Always
     * permit at least one whole detent, otherwise a large step_hz would be
     * unable to move at all. */
    if (dt_ms > 0) {
        int64_t max_hz = (int64_t)TUNE_MAX_RATE_HZ_PER_S * (int64_t)dt_ms / 1000;
        int64_t max_units = max_hz / t->step_hz;
        int64_t floor_units = detents < 0 ? -detents : detents;
        if (max_units < floor_units) max_units = floor_units;
        if (units >  max_units) units =  max_units;
        if (units < -max_units) units = -max_units;
    }

    t->step_units += (int32_t)units;
    t->f_display   = t->f_anchor + (int64_t)t->step_units * t->step_hz;

    if (t->f_display < lo_hz) {
        t->f_display = lo_hz;
        t->f_anchor  = lo_hz;
        t->step_units = 0;
    } else if (t->f_display > hi_hz) {
        t->f_display = hi_hz;
        t->f_anchor  = hi_hz;
        t->step_units = 0;
    } else if (t->step_units > UNITS_RELATCH || t->step_units < -UNITS_RELATCH) {
        t->f_anchor   = t->f_display;
        t->step_units = 0;
    }
    return t->f_display;
}
