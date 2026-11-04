/* The clock faces: the Clock firmware's and the clock screensaver's, drawn by
 * the knob's own code into a 360x360 RGB565 picture. The dial is kept apart
 * and drawn once a day; a tick puts back only the box the hands left and
 * draws them anew in it. From the LVGL task alone: it draws with LVGL's
 * software renderer, on canvases never shown. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"

enum { CF_SHACK, CF_DIVER, CF_CLASSIC, CF_DIGITAL, CF_N };

typedef struct {
    uint8_t face;       /* CF_* */
    bool    seconds;    /* a second hand: the Diver's sweeps, the others tick */
    bool    h24;        /* the digital face's hours */
    char    mark[20];   /* a line the dial carries, the Clock's next alarm: "" none */
} cf_opts_t;

/* The face for now into `px` (360 x 360, stride 360). `dirty` is the box that
 * changed since the last call, the whole picture after clockface_reset or a
 * change of face. False when nothing changed: nothing to send. */
bool clockface_draw(uint16_t *px, const cf_opts_t *o, lv_area_t *dirty);
/* The next call: the whole face, dial and all. */
void clockface_reset(void);
/* How long until the hands next move, ms. */
uint32_t clockface_wait_ms(const cf_opts_t *o);
