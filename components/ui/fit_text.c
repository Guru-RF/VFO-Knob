/* The editor panel's value, fitted to it: see fit_text.h. */
#include "fit_text.h"

#include <stdbool.h>
#include <string.h>

/* The steps down, largest first. */
static const lv_font_t *const STEP[] = {
    &lv_font_montserrat_48, &lv_font_montserrat_28, &lv_font_montserrat_20,
};
#define N_STEP ((int)(sizeof STEP / sizeof STEP[0]))

const lv_font_t *fit_text(lv_obj_t *label, const char *txt, const lv_font_t *font, int32_t room)
{
    if (!txt) txt = "";
    const uint32_t len = (uint32_t)strlen(txt);
    /* The font asked for, then each step smaller than it. */
    int i = 0;
    while (i < N_STEP && lv_font_get_line_height(STEP[i]) >= lv_font_get_line_height(font)) i++;
    const lv_font_t *f = font;
    int32_t w = lv_text_get_width(txt, len, f, 0);
    while (w > room && i < N_STEP) {
        f = STEP[i++];
        w = lv_text_get_width(txt, len, f, 0);
    }
    const bool dots = w > room;

    lv_obj_set_style_text_font(label, f, 0);
    /* Centred in its box, whichever box it gets: every value the same, so
     * none carries anything over from a cut one before it. */
    if (lv_obj_get_style_text_align(label, LV_PART_MAIN) != LV_TEXT_ALIGN_CENTER)
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    const lv_label_long_mode_t mode = dots ? LV_LABEL_LONG_MODE_DOTS : LV_LABEL_LONG_MODE_WRAP;
    if (lv_label_get_long_mode(label) != mode) lv_label_set_long_mode(label, mode);
    if (dots) {
        /* LVGL puts the dots where the text runs past the label's height: so
         * one line of it, room wide. */
        lv_obj_set_size(label, room, lv_font_get_line_height(f));
    } else {
        /* As wide as the text, as it always was: centred on the panel. */
        lv_obj_set_size(label, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    }
    lv_label_set_text(label, txt);
    return f;
}
