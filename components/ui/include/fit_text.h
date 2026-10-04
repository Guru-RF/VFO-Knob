/* The editor panel and its value, fitted to it.
 *
 * One panel serves every editor and chooser (ui.c edit_render): the mode,
 * the filter, the radio, and the setup firmware's list to install from, each
 * a name and a version -- "SVXConnect 1.18.10" -- with more firmwares, and
 * longer versions, to come. A value too wide for its usual font steps down
 * to a smaller one; too wide even in the smallest, it is cut with dots.
 * Before, the panel clipped it mid-glyph.
 *
 * Pure LVGL (9.3), as vu_band: the PC check (tools/lvhost/chooser.c) builds
 * the panel with these numbers and calls this same function. */
#ifndef FIT_TEXT_H
#define FIT_TEXT_H

#include <stdint.h>
#include "lvgl.h"

/* Centred, 6 px above the middle, in two widths. A value (the mode, the
 * filter, the volume, the mic gain, the power...) keeps the panel it always
 * had: opened during an over, it leaves the mic ring beside it in sight. A
 * list of names (the firmwares to install, the radios, the receivers, the
 * spots, the pictures, the calls) gets more room, its corners still 24 px
 * inside the round glass. */
#define EDIT_W      250
#define EDIT_W_NAME 290
#define EDIT_H      132
/* A value's room: its panel less 5 px each side. */
#define EDIT_ROOM(w) ((w) - 10)

/* Sets txt on label in `font` (Montserrat 48 or 28) where it is at most room
 * px wide there, or else in the first smaller one it fits: 48, then 28, then
 * 20. Too wide even in 20, it is set in 20, room wide on one line, cut with
 * dots. The widths are LVGL's own (lv_text_get_width, no letter space); the
 * text is centred in the label either way. Returns the font it is in. */
const lv_font_t *fit_text(lv_obj_t *label, const char *txt, const lv_font_t *font, int32_t room);

#endif
