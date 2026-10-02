/* An arc meter drawn by one object: its track, the zones lit up to the bar,
 * and a peak LED -- in place of a stack of screen-sized lv_arc objects.
 *
 * Why: every lv_arc covers its whole circle's box, so each redrawn pixel
 * anywhere inside the ring went through every arc of the stack -- 35 on the
 * telephone's face -- and a peak LED changing zone repainted 91% of the
 * glass. The call's VU took 53-64% of core 1 (2026-10-02). Here one object
 * draws the same arcs with lv_draw_arc, each clipped to small cells along
 * the ring, and an update invalidates only the sectors that changed.
 *
 * Pure LVGL (9.3): nothing of ESP-IDF, so a host build can render it beside
 * the lv_arc stack it replaces and compare them pixel by pixel. */
#ifndef VU_BAND_H
#define VU_BAND_H

#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"

#define VU_BAND_ZONES 8
#define VU_BAND_CELLS 32

typedef struct {
    lv_obj_t  *obj;
    int16_t    cx, cy;             /* the circle's centre, screen pixels */
    int16_t    rot, span;          /* where the meter starts, LVGL degrees; its extent */
    int16_t    r, w;               /* outer radius, width */
    bool       mirror;             /* fills from rot + span back towards rot */
    uint8_t    nz, ncell;
    int16_t    a0[VU_BAND_ZONES], a1[VU_BAND_ZONES];   /* degrees from the filling end */
    lv_color_t zc[VU_BAND_ZONES], track;
    int16_t    led_deg;            /* the peak LED's length, degrees */
    lv_area_t  zbox[VU_BAND_ZONES];
    lv_area_t  cell[VU_BAND_CELLS];
    lv_area_t  ledbox;
    int16_t    bar;                /* lit up to here, degrees from the filling end */
    int16_t    led;                /* the LED from here, ...; < 0: out */
    int8_t     led_z;              /* ...in this zone's colour */
} vu_band_t;

/* Build it on `parent` (the screen): the circle at (cx, cy), radius r, width
 * w, from absolute angle rot over span degrees, filling from rot -- or, with
 * mirror, from rot + span. Zones z = 0..nz-1 run a0[z]..a1[z] degrees from the
 * filling end, in rgb[z]; the unlit track in `track`. */
void vu_band_build(vu_band_t *b, lv_obj_t *parent, int cx, int cy, int rot, int span,
                   int r, int w, bool mirror, int nz, const int16_t *a0, const int16_t *a1,
                   const uint32_t *rgb, lv_color_t track, int led_deg);

/* The bar at bar_f and the peak LED at pk_f, both 0..1 of the scale, the LED
 * in zone pk_z (< 0, or pk_f <= 0: out). Writes -- and redraws -- only what
 * changed. */
void vu_band_set(vu_band_t *b, float bar_f, float pk_f, int pk_z);

void vu_band_show(vu_band_t *b, bool on);

#endif
