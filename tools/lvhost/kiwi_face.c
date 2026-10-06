/* The kiwi face's new parts on the host (tools/lvhost), as ui.c builds them on
 * the kiwi firmware -- its palette, the S-meter arc with its notches and
 * ticks, the readout over it, the AGC and the noise filter either side, the
 * editor panel -- rendered with the same LVGL 9.3:
 *
 *   - the S-meter in one vu_band object with its peak held in red, as the
 *     receivers' own page marks it, against the lv_arc stack the other faces
 *     still draw, its peak LED given the same red: pixel by pixel, state by
 *     state -- the kiwi face is where phase 3 of tools/lvhost/VU-PLAN.txt
 *     starts, and its look must not change, but for what VU-PLAN accepted:
 *     the bar's end a degree off where a zone's arc rounded its value down;
 *   - vu_band's partial redraws against full ones on a walk of the signal
 *     (stale pixels: a sector not invalidated), and what each redraws;
 *   - the top of the face: the S-units, the reading in dBm with OV after it
 *     in red while the receiver's ADC overloads, AGC MED and NR WDSP;
 *   - the NOISE FILTER and SQUELCH editors through fit_text, over the face;
 *   - the right ear (KIWI-PLAN step 8): its S-meter, the thin line in the
 *     page's link blue just outside the left ear's with its peak held, its
 *     reading in blue where the dBm is; the RIGHT EAR chooser (OFF, a
 *     receiver, the left ear's dimmed with "in the left ear"), the RECEIVER
 *     chooser's right-ear one dimmed alike, and BALANCE's LEFT, L | R, RIGHT;
 *   - the right ear's receiver short of the dial: its line gone, "can't
 *     reach" in amber where its reading was, clear of AGC SLOW and NR WDSP.
 *
 *   make kiwi-check OUT=dir    writes dir/kiwi-*.ppm and .png; exit 1 when
 *                              the two meters differ or a redraw left pixels
 *                              stale, a value is not in the font it should
 *                              be, or the right ear's line is not where it
 *                              should be */
#include "lvgl.h"
#include "vu_band.h"
#include "fit_text.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define W 360
#define H 360
#define CX 180
#define CY 180
#define ARC_R0 170
#define ARC_ROT 170
#define ARC_SPAN 200
#define LED_DEG 3
#define AUX_DX 72
#define RX_ZONES 8
/* ui.c's kiwi palette (VFO_RADIO_KIWI). */
#define C_BG        lv_color_hex(0x000000)
#define C_BG1       lv_color_hex(0x373737)
#define C_ACCENT    lv_color_hex(0xFFFF50)
#define C_ACCENT_HI lv_color_hex(0xFFFF80)
#define C_TEXT      lv_color_hex(0xFFFFFF)
#define C_TEXT2     lv_color_hex(0xD3D3D3)
#define C_LABEL     lv_color_hex(0x909090)
#define C_SUBTLE    lv_color_hex(0x262626)
#define C_DISABLED  lv_color_hex(0x575757)
#define SDR_HEX     0x99C9FF                 /* the page's link blue: the right ear */
#define C_SDR       lv_color_hex(SDR_HEX)
#define SDR_R       176                      /* its S-meter: a line just outside the left ear's */
#define C_WARN      lv_color_hex(0xFFA500)
#define C_DANGER    lv_color_hex(0xFF3030)
#define KIWI_PEAK_HEX 0xFF0000

static const struct { float from, to; uint32_t rgb; } RXZONES[RX_ZONES] = {
    { -127.0f, -121.0f, 0x00FF00 }, { -121.0f, -109.0f, 0x00FF00 },
    { -109.0f,  -97.0f, 0x00FF00 }, {  -97.0f,  -85.0f, 0x00FF00 },
    {  -85.0f,  -73.0f, 0x00FF00 }, {  -73.0f,  -53.0f, 0x00FF00 },
    {  -53.0f,  -33.0f, 0x00FF00 }, {  -33.0f,  -13.0f, 0x00FF00 },
};
static const float RXNOTCH[] = { -121.0f, -109.0f, -97.0f, -85.0f, -73.0f, -53.0f, -33.0f };

static float smeter_frac(float dbm)
{
    if (dbm < -127.0f) dbm = -127.0f;
    if (dbm > -13.0f)  dbm = -13.0f;
    return (dbm <= -73.0f) ? 0.6f * (dbm + 127.0f) / 54.0f : 0.6f + 0.4f * (dbm + 73.0f) / 60.0f;
}
static int rx_zone_of(float dbm)
{
    for (int z = 0; z < RX_ZONES - 1; z++) if (dbm < RXZONES[z].to) return z;
    return RX_ZONES - 1;
}
static void smeter_text(float dbm, char *out, size_t n)
{
    if (dbm >= -73.0f) snprintf(out, n, "S9+%d", (int)((dbm + 73.0f) / 10.0f) * 10);
    else {
        int s = (int)((dbm + 127.0f) / 6.0f);
        if (s < 0) s = 0;
        if (s > 9) s = 9;
        snprintf(out, n, "S%d", s);
    }
}

/* ---- the marks over the arc, as ui.c's add_rx_notches() and add_ticks() ---- */
static lv_obj_t *mkgroup(lv_obj_t *scr)
{
    lv_obj_t *g = lv_obj_create(scr);
    lv_obj_set_size(g, 360, 360);
    lv_obj_set_pos(g, 0, 0);
    lv_obj_set_style_bg_opa(g, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(g, 0, 0);
    lv_obj_set_style_pad_all(g, 0, 0);
    lv_obj_remove_flag(g, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(g, LV_OBJ_FLAG_CLICKABLE);
    return g;
}
static lv_obj_t *marks(lv_obj_t *scr)
{
    static const struct { float dbm; uint8_t len, kind; } TICKS[] = {
        { -121, 6, 0 }, { -109, 6, 0 }, { -97, 6, 0 }, { -85, 6, 0 },
        { -73, 11, 1 }, { -53, 6, 2 }, { -33, 6, 2 }, { -13, 9, 2 },
    };
    /* LVGL keeps the points: one set per screen. */
    static lv_point_precise_t np[3][sizeof RXNOTCH / sizeof RXNOTCH[0]][2];
    static lv_point_precise_t tp[3][sizeof TICKS / sizeof TICKS[0]][2];
    static int used;
    const int k = used++ % 3;
    lv_obj_t *g = mkgroup(scr);
    for (size_t i = 0; i < sizeof RXNOTCH / sizeof RXNOTCH[0]; i++) {
        const float a = (ARC_ROT + smeter_frac(RXNOTCH[i]) * ARC_SPAN) * 3.14159265f / 180.0f;
        const float c = cosf(a), sn = sinf(a);
        const int r1 = ARC_R0 + 1, r0 = ARC_R0 - 13;
        np[k][i][0].x = (lv_value_precise_t)(CX + r0 * c);
        np[k][i][0].y = (lv_value_precise_t)(CY + r0 * sn);
        np[k][i][1].x = (lv_value_precise_t)(CX + r1 * c);
        np[k][i][1].y = (lv_value_precise_t)(CY + r1 * sn);
        lv_obj_t *ln = lv_line_create(g);
        lv_obj_set_style_line_width(ln, 3, 0);
        lv_obj_set_style_line_color(ln, C_BG, 0);
        lv_obj_set_style_line_rounded(ln, false, 0);
        lv_line_set_points(ln, np[k][i], 2);
    }
    for (size_t i = 0; i < sizeof TICKS / sizeof TICKS[0]; i++) {
        const float a = (ARC_ROT + smeter_frac(TICKS[i].dbm) * ARC_SPAN) * 3.14159265f / 180.0f;
        const float c = cosf(a), sn = sinf(a);
        const int r1 = ARC_R0 - 15, r0 = r1 - TICKS[i].len;
        tp[k][i][0].x = (lv_value_precise_t)(CX + r0 * c);
        tp[k][i][0].y = (lv_value_precise_t)(CY + r0 * sn);
        tp[k][i][1].x = (lv_value_precise_t)(CX + r1 * c);
        tp[k][i][1].y = (lv_value_precise_t)(CY + r1 * sn);
        lv_obj_t *ln = lv_line_create(g);
        lv_line_set_points(ln, tp[k][i], 2);
        lv_obj_set_style_line_width(ln, TICKS[i].kind == 1 ? 3 : 2, 0);
        lv_obj_set_style_line_color(ln, TICKS[i].kind == 1 ? C_TEXT2 : TICKS[i].kind == 2 ? C_WARN : C_LABEL, 0);
        lv_obj_set_style_line_rounded(ln, true, 0);
    }
    return g;
}

/* ---- the S-meter as the other faces draw it: a track and eight zone arcs,
 * and the peak LED's arcs on top -- here in the kiwi face's red ---- */
typedef struct {
    lv_obj_t *track, *zone[RX_ZONES], *led[RX_ZONES];
    int16_t   val[RX_ZONES];
    int       lit, at;
} old_t;

static void old_build(lv_obj_t *scr, old_t *m)
{
    m->track = lv_arc_create(scr);
    lv_obj_set_size(m->track, ARC_R0 * 2, ARC_R0 * 2);
    lv_obj_center(m->track);
    lv_arc_set_rotation(m->track, ARC_ROT);
    lv_arc_set_bg_angles(m->track, 0, ARC_SPAN);
    lv_arc_set_range(m->track, 0, 1000);
    lv_arc_set_value(m->track, 0);
    lv_obj_remove_style(m->track, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(m->track, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(m->track, 12, LV_PART_MAIN);
    lv_obj_set_style_arc_color(m->track, C_SUBTLE, LV_PART_MAIN);
    lv_obj_set_style_arc_width(m->track, 12, LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(m->track, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(m->track, false, LV_PART_MAIN);
    for (int z = 0; z < RX_ZONES; z++) {
        lv_obj_t *b = lv_arc_create(scr);
        lv_obj_set_size(b, ARC_R0 * 2, ARC_R0 * 2);
        lv_obj_center(b);
        const int a0 = (int)(smeter_frac(RXZONES[z].from) * ARC_SPAN);
        const int a1 = (int)(smeter_frac(RXZONES[z].to) * ARC_SPAN);
        lv_arc_set_rotation(b, ARC_ROT + a0);
        lv_arc_set_bg_angles(b, 0, a1 - a0);
        lv_arc_set_range(b, 0, 1000);
        lv_arc_set_value(b, 0);
        lv_obj_remove_style(b, NULL, LV_PART_KNOB);
        lv_obj_remove_flag(b, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_arc_opa(b, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_arc_width(b, 12, LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(b, lv_color_hex(RXZONES[z].rgb), LV_PART_INDICATOR);
        lv_obj_set_style_arc_rounded(b, false, LV_PART_INDICATOR);
        lv_obj_set_style_arc_rounded(b, false, LV_PART_MAIN);
        m->zone[z] = b;
        m->val[z] = 0;
    }
    /* led_build(): one arc a zone, all but the lit one collapsed. */
    for (int z = 0; z < RX_ZONES; z++) {
        lv_obj_t *a = lv_arc_create(scr);
        lv_obj_set_size(a, ARC_R0 * 2, ARC_R0 * 2);
        lv_obj_center(a);
        lv_arc_set_rotation(a, ARC_ROT);
        lv_arc_set_bg_angles(a, 0, 0);
        lv_obj_remove_style(a, NULL, LV_PART_KNOB);
        lv_obj_remove_flag(a, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_arc_width(a, 12, LV_PART_MAIN);
        lv_obj_set_style_arc_color(a, lv_color_hex(KIWI_PEAK_HEX), LV_PART_MAIN);
        lv_obj_set_style_arc_rounded(a, false, LV_PART_MAIN);
        lv_obj_set_style_arc_opa(a, LV_OPA_TRANSP, LV_PART_INDICATOR);
        m->led[z] = a;
    }
    m->lit = -1;
    m->at = 0;
}
static void old_set(old_t *m, float disp, float pk)
{
    /* led_set() */
    int at = 0, z = -1;
    const float f = smeter_frac(pk);
    if (f > 0.0f) {
        z = rx_zone_of(pk);
        at = (int)(f * ARC_SPAN + 0.5f) - LED_DEG;
        if (at < 0) at = 0;
        if (at > ARC_SPAN - LED_DEG) at = ARC_SPAN - LED_DEG;
    }
    if (z != m->lit || at != m->at) {
        if (m->lit >= 0 && m->lit != z) lv_arc_set_bg_angles(m->led[m->lit], 0, 0);
        if (z >= 0) lv_arc_set_bg_angles(m->led[z], at, at + LED_DEG);
        m->lit = z;
        m->at = at;
    }
    lv_arc_set_value(m->track, (int)(smeter_frac(disp) * 1000));
    for (int k = 0; k < RX_ZONES; k++) {
        float v = (disp - RXZONES[k].from) / (RXZONES[k].to - RXZONES[k].from);
        if (v < 0.0f) v = 0.0f;
        if (v > 1.0f) v = 1.0f;
        const int16_t x = (int16_t)(v * 1000.0f);
        if (x == m->val[k]) continue;
        m->val[k] = x;
        lv_arc_set_value(m->zone[k], x);
    }
}

/* ---- the kiwi face's: one object ---- */
static void new_build(lv_obj_t *scr, vu_band_t *b)
{
    int16_t a0[RX_ZONES], a1[RX_ZONES];
    uint32_t rgb[RX_ZONES];
    for (int z = 0; z < RX_ZONES; z++) {
        a0[z]  = (int16_t)(smeter_frac(RXZONES[z].from) * ARC_SPAN);
        a1[z]  = (int16_t)(smeter_frac(RXZONES[z].to) * ARC_SPAN);
        rgb[z] = RXZONES[z].rgb;
    }
    vu_band_build(b, scr, CX, CY, ARC_ROT, ARC_SPAN, ARC_R0, 12, false, RX_ZONES, a0, a1, rgb, C_SUBTLE, LED_DEG);
    vu_band_led_color(b, KIWI_PEAK_HEX);
}
static void new_set(vu_band_t *b, float disp, float pk)
{
    vu_band_set(b, smeter_frac(disp), smeter_frac(pk), rx_zone_of(pk));
}

/* ---- the top of the face, as ui.c's build(): readout and aux ---- */
typedef struct { lv_obj_t *srd, *dbm, *agc_cap, *agc_val, *nr_cap, *nr_val; } top_t;

static lv_obj_t *mklabel(lv_obj_t *scr, const lv_font_t *f, lv_color_t c, int x, int y, const char *txt)
{
    lv_obj_t *l = lv_label_create(scr);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, c, 0);
    lv_label_set_text(l, txt);
    lv_obj_align(l, LV_ALIGN_CENTER, x - CX, y - CY);
    return l;
}
static void top_build(lv_obj_t *scr, top_t *t)
{
    t->srd     = mklabel(scr, &lv_font_montserrat_20, C_TEXT, CX, 76, "S0");
    t->dbm     = mklabel(scr, &lv_font_montserrat_14, C_LABEL, CX, 98, "-127 dBm");
    t->agc_cap = mklabel(scr, &lv_font_montserrat_14, C_LABEL, CX - AUX_DX, 78, "AGC");
    t->agc_val = mklabel(scr, &lv_font_montserrat_14, C_TEXT2, CX - AUX_DX, 97, "MED");
    t->nr_cap  = mklabel(scr, &lv_font_montserrat_14, C_LABEL, CX + AUX_DX, 78, "NR");
    t->nr_val  = mklabel(scr, &lv_font_montserrat_14, C_TEXT2, CX + AUX_DX, 97, "OFF");
}
/* ui_update()'s readout on the kiwi face: the peak in S-units, and in dBm --
 * "  OV" after it, all in the danger red, while the ADC overloads. */
static void top_set(top_t *t, float pk, bool ov, const char *nr)
{
    char tb[24];
    smeter_text(pk, tb, sizeof tb);
    lv_label_set_text(t->srd, tb);
    snprintf(tb, sizeof tb, ov ? "%d dBm  OV" : "%d dBm", (int)pk);
    lv_label_set_text(t->dbm, tb);
    lv_obj_set_style_text_color(t->dbm, ov ? C_DANGER : C_LABEL, 0);
    lv_label_set_text(t->nr_val, nr);
}

/* ---- the editor panel, as ui.c builds it ---- */
typedef struct { lv_obj_t *panel, *title, *value, *hint; } panel_t;
static void panel_build(lv_obj_t *scr, panel_t *p)
{
    p->panel = lv_obj_create(scr);
    lv_obj_set_size(p->panel, EDIT_W, EDIT_H);
    lv_obj_align(p->panel, LV_ALIGN_CENTER, 0, -6);
    lv_obj_set_style_radius(p->panel, 18, 0);
    lv_obj_set_style_bg_color(p->panel, C_BG1, 0);
    lv_obj_set_style_bg_opa(p->panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(p->panel, C_ACCENT, 0);
    lv_obj_set_style_border_width(p->panel, 2, 0);
    lv_obj_remove_flag(p->panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(p->panel, LV_OBJ_FLAG_CLICKABLE);
    p->title = lv_label_create(p->panel);
    lv_obj_set_style_text_font(p->title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(p->title, C_LABEL, 0);
    lv_obj_align(p->title, LV_ALIGN_TOP_MID, 0, 2);
    p->value = lv_label_create(p->panel);
    lv_obj_set_style_text_font(p->value, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(p->value, C_ACCENT_HI, 0);
    lv_obj_align(p->value, LV_ALIGN_CENTER, 0, 4);
    p->hint = lv_label_create(p->panel);
    lv_obj_set_style_text_font(p->hint, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(p->hint, C_LABEL, 0);
    lv_obj_align(p->hint, LV_ALIGN_BOTTOM_MID, 0, -2);
    lv_label_set_text(p->hint, "turn to choose  -  tap to accept");
}
/* edit_render(): ED_GAIN with names is NOISE FILTER, ED_SQUELCH OPEN or n%. */
static const lv_font_t *panel_set(panel_t *p, const char *title, const char *v)
{
    lv_label_set_text(p->title, title);
    return fit_text(p->value, v, &lv_font_montserrat_48, EDIT_ROOM(EDIT_W));
}

/* edit_render() for a list of names (ED_RXSRC, ED_RADIO): the wider panel,
 * a name from 28 pt -- RIGHT EAR's OFF, as RX's LOCAL, from 48 -- its colour
 * dimmed where the other ear has it, and the hint saying so. */
static const lv_font_t *panel_names(panel_t *p, const char *title, const char *v, bool word, bool other,
                                    const char *hint)
{
    lv_obj_set_width(p->panel, EDIT_W_NAME);
    lv_label_set_text(p->title, title);
    lv_obj_set_style_text_color(p->value, other ? C_DISABLED : C_ACCENT_HI, 0);
    lv_label_set_text(p->hint, hint);
    return fit_text(p->value, v, word ? &lv_font_montserrat_48 : &lv_font_montserrat_28, EDIT_ROOM(EDIT_W_NAME));
}

/* ...and a value's (BALANCE): the narrower panel, from 48 pt. */
static const lv_font_t *panel_value(panel_t *p, const char *title, const char *v)
{
    lv_obj_set_width(p->panel, EDIT_W);
    lv_label_set_text(p->title, title);
    lv_obj_set_style_text_color(p->value, C_ACCENT_HI, 0);
    lv_label_set_text(p->hint, "turn to choose  -  tap to accept");
    return fit_text(p->value, v, &lv_font_montserrat_48, EDIT_ROOM(EDIT_W));
}

/* ---- the right ear's S-meter, as ui.c builds it under VFO_HAS_SDR: a thin
 * arc on the same scale just outside the left ear's, and its peak held a
 * second on it, one zone in the same blue (led_build, led_set) ---- */
typedef struct { lv_obj_t *line, *led; int at; } sdr_t;

static void sdr_build(lv_obj_t *scr, sdr_t *s)
{
    s->line = lv_arc_create(scr);
    lv_obj_set_size(s->line, SDR_R * 2, SDR_R * 2);
    lv_obj_center(s->line);
    lv_arc_set_rotation(s->line, ARC_ROT);
    lv_arc_set_bg_angles(s->line, 0, ARC_SPAN);
    lv_arc_set_range(s->line, 0, 1000);
    lv_arc_set_value(s->line, 0);
    lv_obj_remove_style(s->line, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(s->line, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(s->line, 4, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s->line, C_SUBTLE, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s->line, 4, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s->line, C_SDR, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(s->line, false, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(s->line, false, LV_PART_INDICATOR);
    s->led = lv_arc_create(scr);
    lv_obj_set_size(s->led, SDR_R * 2, SDR_R * 2);
    lv_obj_center(s->led);
    lv_arc_set_rotation(s->led, ARC_ROT);
    lv_arc_set_bg_angles(s->led, 0, 0);
    lv_obj_remove_style(s->led, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(s->led, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(s->led, 4, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s->led, C_SDR, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(s->led, false, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(s->led, LV_OPA_TRANSP, LV_PART_INDICATOR);
}
static void sdr_show(sdr_t *s, bool on)
{
    if (on) { lv_obj_remove_flag(s->line, LV_OBJ_FLAG_HIDDEN); lv_obj_remove_flag(s->led, LV_OBJ_FLAG_HIDDEN); }
    else    { lv_obj_add_flag(s->line, LV_OBJ_FLAG_HIDDEN);    lv_obj_add_flag(s->led, LV_OBJ_FLAG_HIDDEN); }
}
static void sdr_set(sdr_t *s, float disp, float pk)
{
    lv_arc_set_value(s->line, (int)(smeter_frac(disp) * 1000.0f));
    int at = (int)(smeter_frac(pk) * ARC_SPAN + 0.5f) - LED_DEG;
    if (at < 0) at = 0;
    if (at > ARC_SPAN - LED_DEG) at = ARC_SPAN - LED_DEG;
    lv_arc_set_bg_angles(s->led, at, at + LED_DEG);
}

/* ---- the host display ---- */
static uint16_t fb[W * H];
static uint8_t drawbuf[W * 12 * 2] __attribute__((aligned(4)));
static long inv_n, inv_px;

static uint32_t tick_cb(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}
static void flush_cb(lv_display_t *d, const lv_area_t *a, uint8_t *px)
{
    const int w = lv_area_get_width(a);
    for (int y = a->y1; y <= a->y2; y++)
        memcpy(&fb[y * W + a->x1], px + (size_t)(y - a->y1) * w * 2, (size_t)w * 2);
    lv_display_flush_ready(d);
}
static void inv_cb(lv_event_t *e)
{
    const lv_area_t *a = lv_event_get_param(e);
    inv_n++;
    inv_px += lv_area_get_size(a);
}
static void full(lv_display_t *d, lv_obj_t *scr, uint16_t *out)
{
    lv_obj_invalidate(scr);
    lv_refr_now(d);
    if (out) memcpy(out, fb, sizeof fb);
}
/* The picture, the glass's edge marked, outside it dimmed. */
static void ppm(const char *dir, const char *name, const uint16_t *img)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return; }
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            const uint16_t p = img[y * W + x];
            uint8_t rgb[3] = { (uint8_t)(((p >> 11) & 31) * 255 / 31), (uint8_t)(((p >> 5) & 63) * 255 / 63),
                               (uint8_t)((p & 31) * 255 / 31) };
            const double d = hypot(x + 0.5 - W / 2.0, y + 0.5 - H / 2.0);
            if (d > 180.0) { rgb[0] = (uint8_t)(48 + rgb[0] / 4); rgb[1] = (uint8_t)(48 + rgb[1] / 4);
                             rgb[2] = (uint8_t)(48 + rgb[2] / 4); }
            fwrite(rgb, 1, 3, f);
        }
    fclose(f);
}
static int differ(const uint16_t *a, const uint16_t *b, lv_area_t *box)
{
    int n = 0;
    box->x1 = W; box->y1 = H; box->x2 = -1; box->y2 = -1;
    for (int i = 0; i < W * H; i++) {
        if (a[i] == b[i]) continue;
        n++;
        const int x = i % W, y = i / W;
        if (x < box->x1) box->x1 = x;
        if (x > box->x2) box->x2 = x;
        if (y < box->y1) box->y1 = y;
        if (y > box->y2) box->y2 = y;
    }
    return n;
}
/* Differing pixels further than `slack` degrees from `deg` on the arc (from
 * ARC_ROT): what is not the bar's end rounded otherwise. */
static int off_end(const uint16_t *a, const uint16_t *b, float deg, float slack)
{
    int n = 0;
    for (int i = 0; i < W * H; i++) {
        if (a[i] == b[i]) continue;
        float t = atan2f((float)(i / W) + 0.5f - CY, (float)(i % W) + 0.5f - CX) * 180.0f / 3.14159265f;
        if (t < 0) t += 360.0f;
        float at = t - ARC_ROT;
        if (at < -90.0f) at += 360.0f;
        if (fabsf(at - deg) > slack) n++;
    }
    return n;
}

/* Pixels of a colour, within a box: the peak mark's red is there. */
static int count(const uint16_t *img, lv_color_t c)
{
    const uint16_t v = lv_color_to_u16(c);
    int n = 0;
    for (int i = 0; i < W * H; i++) n += img[i] == v;
    return n;
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : ".";
    lv_init();
    lv_tick_set_cb(tick_cb);
    lv_display_t *d = lv_display_create(W, H);
    lv_display_set_color_format(d, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(d, drawbuf, NULL, sizeof drawbuf, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(d, flush_cb);
    lv_display_add_event_cb(d, inv_cb, LV_EVENT_INVALIDATE_AREA, NULL);

    lv_obj_t *scr_old = lv_obj_create(NULL), *scr_new = lv_obj_create(NULL);
    lv_obj_t *scrs[2] = { scr_old, scr_new };
    for (int i = 0; i < 2; i++) {
        lv_obj_remove_flag(scrs[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_color(scrs[i], C_BG, 0);
        lv_obj_set_style_bg_opa(scrs[i], LV_OPA_COVER, 0);
        lv_obj_set_style_pad_all(scrs[i], 0, 0);
        lv_obj_set_style_border_width(scrs[i], 0, 0);
    }
    /* In ui.c's order: the meter, then the marks lifted over it. */
    static old_t om;
    static vu_band_t nb;
    old_build(scr_old, &om);
    lv_obj_move_foreground(marks(scr_old));
    new_build(scr_new, &nb);
    lv_obj_move_foreground(marks(scr_new));
    int bad = 0;

    /* 1. The look: the same states both ways, full redraws, compared. */
    static const float ST[][2] = {          /* the bar, the peak held (dBm) */
        { -127, -127 }, { -120, -118 }, { -110, -97 }, { -97, -97 }, { -85, -73 }, { -80, -73 },
        { -73, -60 },   { -66, -63 },   { -50, -40 },  { -40, -33 }, { -30, -20 }, { -13, -13 },
        { -100, -20 },  { -126.5f, -126 }, { -73.2f, -72.8f },
    };
    static uint16_t A[W * H], B[W * H];
    int worst = 0;
    for (size_t i = 0; i < sizeof ST / sizeof ST[0]; i++) {
        old_set(&om, ST[i][0], ST[i][1]);
        new_set(&nb, ST[i][0], ST[i][1]);
        lv_screen_load(scr_old); full(d, scr_old, A);
        lv_screen_load(scr_new); full(d, scr_new, B);
        lv_area_t box;
        const int n = differ(A, B, &box);
        const int red = count(B, lv_color_hex(KIWI_PEAK_HEX));
        const float end = (float)(int)(smeter_frac(ST[i][0]) * ARC_SPAN);
        const int elsewhere = off_end(A, B, end, 1.5f);
        printf("state %2zu: bar %7.1f, peak %7.1f dBm: %4d px differ, %d of them off the bar's end; "
               "%3d px of the red mark\n", i, ST[i][0], ST[i][1], n, elsewhere, red);
        if (elsewhere > worst) worst = elsewhere;
        /* A peak on the scale shows its mark; none at the floor. */
        if ((smeter_frac(ST[i][1]) > 0.0f) != (red > 0)) {
            printf("  the red mark %s\n", red ? "where there is no peak" : "missing");
            bad++;
        }
        if (i == 7 || i == 12) {
            char nm[64];
            snprintf(nm, sizeof nm, "kiwi-meter-%02zu-stack.ppm", i);
            ppm(dir, nm, A);
            snprintf(nm, sizeof nm, "kiwi-meter-%02zu-band.ppm", i);
            ppm(dir, nm, B);
        }
    }
    printf("look: %d px differ between the stack and the band but at the bar's end (a degree, VU-PLAN)\n",
           worst);
    if (worst) bad++;

    /* 2. Partial redraws against full ones, on a walk of the signal: what
     *    each draws, and nothing left stale. */
    srand(11);
    float lvl = -127, pk = -127;
    long stale = 0, steps = 3000;
    long n_inv[2] = { 0, 0 }, px_inv[2] = { 0, 0 };
    for (int pass = 0; pass < 2; pass++) {
        lv_obj_t *scr = scrs[pass];
        lv_screen_load(scr);
        full(d, scr, A);
        srand(11);
        lvl = pk = -127;
        inv_n = inv_px = 0;
        for (long t = 0; t < steps; t++) {
            /* SSB: syllables and pauses, the peak held a while, then falling. */
            const bool talk = (t / 29) % 3 != 0;
            const float target = talk ? -95.0f + (float)(rand() % 600) / 10.0f : -121.0f;
            lvl = target > lvl ? target : lvl + (target - lvl) * 0.35f;
            if (lvl > pk) pk = lvl; else if (t % 20 > 10) pk -= 1.5f;
            if (pk < lvl) pk = lvl;
            if (pass == 0) old_set(&om, lvl, pk);
            else           new_set(&nb, lvl, pk);
            lv_refr_now(d);
            if (pass == 1 && t % 10 == 0) {
                memcpy(B, fb, sizeof fb);
                const long keep_n = inv_n, keep_px = inv_px;
                full(d, scr, A);
                inv_n = keep_n;
                inv_px = keep_px;
                lv_area_t box;
                const int n = differ(A, B, &box);
                if (n) {
                    stale += n;
                    printf("  step %ld: %d px stale in %d,%d-%d,%d\n", t, n, (int)box.x1, (int)box.y1,
                           (int)box.x2, (int)box.y2);
                }
            }
        }
        n_inv[pass] = inv_n;
        px_inv[pass] = inv_px;
    }
    printf("walk of %ld steps: the stack invalidated %ld areas, %ld px; the band %ld areas, %ld px (%.1fx fewer); "
           "%ld px stale\n", steps, n_inv[0], px_inv[0], n_inv[1], px_inv[1],
           px_inv[1] ? (double)px_inv[0] / (double)px_inv[1] : 0.0, stale);
    if (stale) bad++;

    /* 3. The top of the face, and the editors over it. */
    static top_t top;
    top_build(scr_new, &top);
    static panel_t pan;
    panel_build(scr_new, &pan);
    lv_obj_add_flag(pan.panel, LV_OBJ_FLAG_HIDDEN);
    lv_screen_load(scr_new);
    static const struct { const char *name; float bar, pk; bool ov; const char *nr; const char *ed, *edv; } SHOT[] = {
        { "kiwi-top.ppm",          -70, -63, false, "WDSP", NULL, NULL },
        { "kiwi-top-ov.ppm",       -20, -13, true,  "WDSP", NULL, NULL },
        { "kiwi-nr.ppm",           -70, -63, false, "WDSP", "NOISE FILTER", "WDSP" },
        { "kiwi-nr-off.ppm",       -70, -63, false, "OFF",  "NOISE FILTER", "OFF" },
        { "kiwi-squelch-open.ppm", -70, -63, false, "WDSP", "SQUELCH", "OPEN" },
        { "kiwi-squelch-30.ppm",   -70, -63, false, "WDSP", "SQUELCH", "30%" },
        { "kiwi-squelch-100.ppm",  -70, -63, false, "WDSP", "SQUELCH", "100%" },
    };
    for (size_t i = 0; i < sizeof SHOT / sizeof SHOT[0]; i++) {
        new_set(&nb, SHOT[i].bar, SHOT[i].pk);
        top_set(&top, SHOT[i].pk, SHOT[i].ov, SHOT[i].nr);
        if (SHOT[i].ed) {
            lv_obj_remove_flag(pan.panel, LV_OBJ_FLAG_HIDDEN);
            const lv_font_t *f = panel_set(&pan, SHOT[i].ed, SHOT[i].edv);
            if (f != &lv_font_montserrat_48) {
                printf("%s: \"%s\" did not fit at 48 pt\n", SHOT[i].name, SHOT[i].edv);
                bad++;
            }
        } else {
            lv_obj_add_flag(pan.panel, LV_OBJ_FLAG_HIDDEN);
        }
        full(d, scr_new, B);
        ppm(dir, SHOT[i].name, B);
        /* The reading in red with OV, in the readout's place only. */
        lv_area_t c;
        lv_obj_get_coords(top.dbm, &c);
        const int w = (int)lv_area_get_width(&c);
        printf("%-24s readout \"%s\" %d px wide (x %d..%d)%s\n", SHOT[i].name, lv_label_get_text(top.dbm), w,
               (int)c.x1, (int)c.x2, SHOT[i].ov ? ", in red" : "");
        /* Clear of the AGC and NR columns either side (their text centred at
         * CX -+ 72, at most "MED" and "WDSP" wide). */
        lv_area_t l, r;
        lv_obj_get_coords(top.agc_val, &l);
        lv_obj_get_coords(top.nr_val, &r);
        if (c.x1 <= l.x2 + 4 || c.x2 >= r.x1 - 4) {
            printf("  the readout runs into the aux columns (%d..%d, %d..%d)\n", (int)l.x2, (int)r.x1,
                   (int)c.x1, (int)c.x2);
            bad++;
        }
    }

    /* 4. The right ear: its line and its peak just outside the left ear's
     *    meter, its reading in blue where the dBm is; then RIGHT EAR,
     *    RECEIVER and BALANCE over the face. */
    static sdr_t sd;
    sdr_build(scr_new, &sd);
    lv_obj_move_foreground(pan.panel);              /* an editor over the line, as on the knob */
    sdr_show(&sd, true);
    new_set(&nb, -85, -79);
    top_set(&top, -79, false, "WDSP");
    sdr_set(&sd, -97, -91);
    {
        char tb[16];
        smeter_text(-91, tb, sizeof tb);
        lv_label_set_text(top.dbm, tb);
        lv_obj_set_style_text_color(top.dbm, C_SDR, 0);
    }
    lv_obj_add_flag(pan.panel, LV_OBJ_FLAG_HIDDEN);
    full(d, scr_new, B);
    ppm(dir, "kiwi-right.ppm", B);
    {
        /* Its blue: the reading under the S-units, and the line between the
         * left ear's meter (out to 170) and the glass's edge (180) -- and
         * nowhere else. */
        const uint16_t blue = lv_color_to_u16(C_SDR);
        int line = 0, label = 0, astray = 0;
        for (int i = 0; i < W * H; i++) {
            if (fb[i] != blue) continue;
            const double r = hypot(i % W + 0.5 - CX, i / W + 0.5 - CY);
            if (r >= 171.0 && r <= 178.5) line++;
            else if (r < 120.0) label++;
            else astray++;
        }
        printf("kiwi-right.ppm: the right ear's line %d px, its reading %d px, %d px of its blue elsewhere\n",
               line, label, astray);
        if (!line || !label || astray) bad++;
    }
    static const struct { const char *name, *title, *v; bool word, other; const char *hint; } ED[] = {
        { "kiwi-right-ear.ppm",        "RIGHT EAR", "TerraBooster",  false, false, "turn to choose  -  tap to accept" },
        { "kiwi-right-off.ppm",        "RIGHT EAR", "OFF",           true,  false, "turn to choose  -  tap to accept" },
        { "kiwi-right-refused.ppm",    "RIGHT EAR", "EchoTracer",    false, true,  "in the left ear" },
        { "kiwi-receiver-refused.ppm", "RECEIVER",  "OctaLoop Mini", false, true,  "in the right ear" },
    };
    lv_obj_remove_flag(pan.panel, LV_OBJ_FLAG_HIDDEN);
    for (size_t i = 0; i < sizeof ED / sizeof ED[0]; i++) {
        const lv_font_t *f = panel_names(&pan, ED[i].title, ED[i].v, ED[i].word, ED[i].other, ED[i].hint);
        const lv_font_t *want = ED[i].word ? &lv_font_montserrat_48 : &lv_font_montserrat_28;
        full(d, scr_new, B);
        ppm(dir, ED[i].name, B);
        printf("%-26s %s \"%s\" at %d pt%s, \"%s\"\n", ED[i].name, ED[i].title, lv_label_get_text(pan.value),
               f == &lv_font_montserrat_48 ? 48 : f == &lv_font_montserrat_28 ? 28 : 20,
               ED[i].other ? ", dimmed" : "", ED[i].hint);
        if (f != want) bad++;
    }
    static const char *const BAL[] = { "LEFT", "L | R", "RIGHT" };
    for (size_t i = 0; i < sizeof BAL / sizeof BAL[0]; i++) {
        char nm[48];
        snprintf(nm, sizeof nm, "kiwi-balance-%zu.ppm", i);
        const lv_font_t *f = panel_value(&pan, "BALANCE", BAL[i]);
        full(d, scr_new, B);
        ppm(dir, nm, B);
        printf("%-26s BALANCE \"%s\" at %d pt\n", nm, BAL[i], f == &lv_font_montserrat_48 ? 48 : 28);
        if (f != &lv_font_montserrat_48) bad++;
    }

    /* 5. The right ear's receiver cannot reach the dial: its line gone, and
     *    in its reading's place "can't reach" in amber, as the other words
     *    for why it is not heard -- clear of the AGC and the noise filter at
     *    their widest. */
    lv_obj_add_flag(pan.panel, LV_OBJ_FLAG_HIDDEN);
    sdr_show(&sd, false);
    lv_label_set_text(top.agc_val, "SLOW");
    lv_label_set_text(top.nr_val, "WDSP");
    lv_label_set_text(top.dbm, "can't reach");
    lv_obj_set_style_text_color(top.dbm, C_WARN, 0);
    full(d, scr_new, B);
    ppm(dir, "kiwi-right-out.ppm", B);
    {
        const uint16_t blue = lv_color_to_u16(C_SDR), amber = lv_color_to_u16(C_WARN);
        int blue_n = 0, amber_n = 0;
        for (int i = 0; i < W * H; i++) {
            blue_n += fb[i] == blue;
            amber_n += fb[i] == amber && hypot(i % W + 0.5 - CX, i / W + 0.5 - CY) < 120.0;
        }
        lv_area_t c, l, r;
        lv_obj_get_coords(top.dbm, &c);
        lv_obj_get_coords(top.agc_val, &l);
        lv_obj_get_coords(top.nr_val, &r);
        printf("kiwi-right-out.ppm: \"%s\" %d px wide (x %d..%d), SLOW ends at x %d, WDSP starts at x %d; "
               "%d px of amber in it, %d px of the right ear's blue anywhere\n", lv_label_get_text(top.dbm),
               (int)lv_area_get_width(&c), (int)c.x1, (int)c.x2, (int)l.x2, (int)r.x1, amber_n, blue_n);
        if (blue_n || !amber_n || c.x1 <= l.x2 + 4 || c.x2 >= r.x1 - 4) bad++;
    }
    printf("%s\n", bad ? "FAILED" : "ok");
    return bad ? 1 : 0;
}
