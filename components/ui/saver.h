/* The screensavers (net_screen_t's saver), drawn straight to the panel while
 * LVGL's display refresh rests. ui.c's, from the LVGL task alone (its
 * saver timer), with the port's lock held: nothing else is on the panel. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lvgl.h"

/* LVGL's refresh paused, the glass cleared, the first frame drawn. */
void saver_start(lv_display_t *disp, uint8_t kind);
/* What is due by now: a column of rain, the clock's second. */
void saver_frame(uint32_t now_ms);
/* Every transfer finished and LVGL's refresh running again; the caller
 * invalidates the screen. */
void saver_stop(void);
bool saver_running(void);

/* ui.c's: the drifting saver's two lines, what the face shows biggest -- the
 * frequency and the mode, the talkgroup, the beam. */
void ui_saver_text(char *big, size_t nb, char *small, size_t ns);
