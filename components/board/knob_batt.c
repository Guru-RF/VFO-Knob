/* The knob's own battery, from its 5 V rail. See knob_batt.h. */
#include "knob_batt.h"

#include <stddef.h>

/* A 4.2 V Li-ion cell (LiCoO2, NMC): its open-circuit voltage against its
 * charge, as cell makers' data sheets draw it -- 4.20 V full, 4.10 at 90 %,
 * 4.00 at 80, 3.92 at 70, 3.87 at 60, 3.84 at 50, 3.80 at 40, 3.77 at 30,
 * 3.74 at 20, 3.69 at 10, 3.61 at 5 -- each 130 mV lower on the knob's
 * rail: its load and the switch take that (4,080-4,100 mV measured just off
 * a full charge, against the cell's 4.20 V at rest). Full from 4,050 mV, so
 * a cell just charged reads full a while, as a phone's does; empty at
 * 3,400, where the 3V3 regulator gives out, and the knob with it. The shape
 * is the cell makers'; a run-down on the knob is yet to confirm it. */
static const struct { int16_t mv; uint8_t pct; } CURVE[] = {
    { 4050, 100 }, { 3970, 90 }, { 3870, 80 }, { 3790, 70 }, { 3740, 60 }, { 3710, 50 },
    { 3670, 40 },  { 3640, 30 }, { 3610, 20 }, { 3560, 10 }, { 3480, 5 },  { 3400, 0 },
};

/* The charge shown moves once the smoothed charge is 2 % past the halfway
 * mark to the next five: either way while it settles, then down only. */
#define PAST 4.5f

float knob_batt_charge(float mv)
{
    if (mv >= CURVE[0].mv) return 100.0f;
    for (size_t i = 1; i < sizeof CURVE / sizeof *CURVE; i++)
        if (mv >= CURVE[i].mv) {
            const float f = (mv - CURVE[i].mv) / (float)(CURVE[i - 1].mv - CURVE[i].mv);
            return CURVE[i].pct + f * (float)(CURVE[i - 1].pct - CURVE[i].pct);
        }
    return 0.0f;
}

/* A charge, as shown: the nearest five. */
static int8_t fives(float pct)
{
    const int v = (int)((pct + 2.5f) / 5.0f) * 5;
    return (int8_t)(v < 0 ? 0 : v > 100 ? 100 : v);
}

/* No readings in a row. */
static void row_clear(knob_batt_t *b)
{
    b->n   = 0;
    b->sum = 0;
}

/* On the battery from the readings in a row that found it: its charge shown
 * at once, from their mean, and settling from there. */
static void on_battery(knob_batt_t *b)
{
    b->src       = KNOB_PWR_BATTERY;
    b->smooth_mv = (float)b->sum / (float)b->n;
    b->taken     = b->n;
    b->pct       = fives(knob_batt_charge(b->smooth_mv));
    row_clear(b);
}

void knob_batt_init(knob_batt_t *b)
{
    *b = (knob_batt_t){ .src = KNOB_PWR_UNKNOWN, .pct = -1, .mv = -1 };
}

bool knob_batt_feed(knob_batt_t *b, int mv, uint32_t now_ms)
{
    if (mv < KNOB_RAIL_MIN_MV || mv > KNOB_RAIL_MAX_MV) return false;
    const uint8_t  src  = b->src;
    const int8_t   pct  = b->pct;
    const uint32_t dt   = b->mv >= 0 ? now_ms - b->at_ms : 0;    /* unsigned: across a wrap too */
    const bool     cell = mv < KNOB_USB_OFF_MV;                  /* a rail a cell can give */
    b->mv    = (int16_t)mv;
    b->at_ms = now_ms;
    switch (b->src) {
    case KNOB_PWR_UNKNOWN:
        /* Settling: KNOB_BATT_SETTLE readings in a row on one side of
         * KNOB_USB_OFF_MV -- a cable plugged in or pulled meanwhile starts
         * it again. Over it USB; under it the battery. */
        if (b->n && (b->sum / b->n < KNOB_USB_OFF_MV) != cell) row_clear(b);
        b->sum += mv;
        if (++b->n < KNOB_BATT_SETTLE) break;
        if (cell) {
            on_battery(b);
        } else {
            b->src = KNOB_PWR_USB;
            row_clear(b);
        }
        break;
    case KNOB_PWR_USB:
        /* Unplugged: KNOB_BATT_CONFIRM readings in a row under the line. */
        if (!cell) {
            row_clear(b);
            break;
        }
        b->sum += mv;
        if (++b->n >= KNOB_BATT_CONFIRM) on_battery(b);
        break;
    default:
        /* Plugged in: KNOB_BATT_CONFIRM readings in a row from
         * KNOB_USB_ON_MV up. One over what a cell gives is none of the
         * cell's either. */
        b->n = mv >= KNOB_USB_ON_MV ? (uint8_t)(b->n + 1) : 0;
        if (b->n >= KNOB_BATT_CONFIRM) {
            b->src = KNOB_PWR_USB;
            b->pct = -1;
            row_clear(b);
            break;
        }
        if (!cell) break;
        /* The rail: the mean of the first KNOB_BATT_FREE readings, then
         * smoothed over KNOB_BATT_TAU_MS. The charge shown: either way
         * while those come and the nearest five at their end, then down
         * only. */
        const bool settling = b->taken < KNOB_BATT_FREE;
        if (settling) b->taken++;
        const float w = settling ? 1.0f / (float)b->taken : (float)dt / (float)(KNOB_BATT_TAU_MS + dt);
        b->smooth_mv += ((float)mv - b->smooth_mv) * w;
        const float c = knob_batt_charge(b->smooth_mv);
        if (settling && b->taken == KNOB_BATT_FREE) b->pct = fives(c);
        else if (c < b->pct - PAST || (settling && c > b->pct + PAST)) b->pct = fives(c);
        break;
    }
    return b->src != src || b->pct != pct;
}
