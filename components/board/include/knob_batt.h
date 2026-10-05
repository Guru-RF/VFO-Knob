/* The knob's own battery, from its 5 V rail (board_power_mv): on USB or on
 * the battery, and how full that is. Plain C, with nothing of ESP-IDF's:
 * test/host tests it.
 *
 * USB or the battery. On USB the rail is the cable's 5 V less a diode:
 * 4,460-4,560 mV on the battery knob (2026-10-05), its cell near full.
 * Unplugged it is the cell's, through a switch -- no boost -- under the
 * knob's own load: 4,080-4,100 mV just off a full charge. A Li-ion cell is
 * never above 4.20 V, and the switch and the load only take from that: under
 * KNOB_USB_OFF_MV the rail can be the battery's, and over it it cannot. From
 * KNOB_USB_ON_MV up it is USB's, 100 mV over the cell at its fullest with no
 * load at all; between the two a rail stays on the side it was. A USB port
 * at its spec's lowest, 4.75 V, gives about that past the diode, a weak one
 * or a long cable less: such a rail is taken for the battery -- nothing in
 * it tells the two apart. A plug or an unplug takes KNOB_BATT_CONFIRM
 * readings in a row past the other line, so that one caught in a dip or a
 * spike changes nothing. At boot the first KNOB_BATT_SETTLE readings settle
 * it, all on one side of KNOB_USB_OFF_MV: over it USB, as no cell under the
 * knob's load reads that.
 *
 * Its charge, on the battery only: on USB the rail is the cable's, and the
 * cell's cannot be read. The curve (knob_batt.c) is a 4.2 V Li-ion cell's
 * open-circuit voltage against its charge, as cell makers draw it, less the
 * 130 mV the knob's load and the switch take, ending at 3.40 V, where the
 * 3V3 regulator gives out. Shown in fives at once, from the readings that
 * found the battery, and settling over its first KNOB_BATT_FREE readings
 * there: the rail is their mean, the charge shown moves up or down once
 * that is 2 % past the halfway mark to the next five, and at their end it
 * is the nearest five. After that the rail is smoothed over some 30 s, so
 * that a WiFi burst or the backlight does not move it, and the charge shown
 * only goes down, once 2 % past the halfway mark -- the cell never charges
 * itself; only a lighter load, or a cell at rest, lifts the rail. */
#ifndef KNOB_BATT_H
#define KNOB_BATT_H

#include <stdbool.h>
#include <stdint.h>

enum { KNOB_PWR_UNKNOWN = 0, KNOB_PWR_USB, KNOB_PWR_BATTERY };

#define KNOB_USB_ON_MV    4300       /* from here up, USB */
#define KNOB_USB_OFF_MV   4200       /* under it, the battery: no cell gives more */
#define KNOB_RAIL_MIN_MV  3000       /* no rail that runs the knob reads outside these: */
#define KNOB_RAIL_MAX_MV  5600       /* ...a reading there is the ADC's, and not taken */
#define KNOB_BATT_SETTLE  5          /* readings on one side, at boot, before a word */
#define KNOB_BATT_CONFIRM 2          /* readings in a row past the other line: a plug, an unplug */
#define KNOB_BATT_FREE    30         /* readings on the battery while its charge settles */
#define KNOB_BATT_TAU_MS  30000      /* then the charge's smoothing, its time constant */

typedef struct {
    uint8_t  src;           /* KNOB_PWR_*: UNKNOWN until the first readings have settled */
    int8_t   pct;           /* the charge shown, 0-100 in fives; -1 none: on USB, or not settled */
    int16_t  mv;            /* the last reading taken, mV; -1 none yet */
    float    smooth_mv;     /* on the battery: the rail, smoothed */
    /* The gauge's own. */
    uint8_t  n;             /* readings in a row: settling, or past the other line, */
    uint8_t  taken;         /* readings on the battery, up to KNOB_BATT_FREE: its charge settling */
    int32_t  sum;           /* ...the readings in a row, summed */
    uint32_t at_ms;         /* when the last reading came */
} knob_batt_t;

void knob_batt_init(knob_batt_t *b);

/* One reading of the rail, mV, taken at now_ms (any clock in ms, wrapping
 * or not). True when what is shown changed: USB or the battery, or the
 * charge. A reading outside KNOB_RAIL_MIN_MV-KNOB_RAIL_MAX_MV is not taken. */
bool knob_batt_feed(knob_batt_t *b, int mv, uint32_t now_ms);

/* The curve: the charge, 0-100 %, of a cell whose rail reads mv under the
 * knob's load. */
float knob_batt_charge(float mv);

#endif /* KNOB_BATT_H */
