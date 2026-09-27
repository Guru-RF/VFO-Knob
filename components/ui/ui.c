#include "ui.h"
#include "board_pins.h"
#include "hal_touch.h"
#include "panel.h"

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

static const char *TAG = "ui";

/* --- AetherSDR "Default Dark" palette ------------------------------------
 * Taken from resources/themes/default-dark.json so the knob reads as an
 * extension of the desktop rather than a separate device. Note TX is AMBER
 * here, not red -- that is AetherSDR's convention and worth matching, since
 * the operator already reads amber as "on the air". */
#define C_BG        lv_color_hex(0x0F0F1A)   /* background.app   */
#define C_BG1       lv_color_hex(0x1A2A3A)   /* background.1     */
#define C_BG_TX     lv_color_hex(0x3A2A0E)   /* background.tx    */
#define C_ACCENT    lv_color_hex(0x00B4D8)   /* accent           */
#define C_ACCENT_HI lv_color_hex(0x00C8F0)   /* accent.bright    */
#define C_TEXT      lv_color_hex(0xC8D8E8)   /* text.primary     */
#define C_TEXT2     lv_color_hex(0x8EA8C0)   /* text.secondary   */
#define C_LABEL     lv_color_hex(0x506070)   /* text.label       */
#define C_DISABLED  lv_color_hex(0x3A4A5A)   /* text.disabled    */
#define C_SUBTLE    lv_color_hex(0x1A2330)   /* border.subtle    */
#define C_WARN      lv_color_hex(0xFFB84D)   /* accent.warning   */
#define C_DANGER    lv_color_hex(0xFF4D4D)   /* accent.danger    */
#define C_TX_BORDER lv_color_hex(0xD08020)   /* tx.mox.border    */
#define C_TX_TEXT   lv_color_hex(0xF0C890)   /* tx.mox.text      */
#define C_PEAK      lv_color_hex(0xE6F0FA)   /* meter.peak       */
/* AetherSDR's own TX tint is a muted amber. On a 45 mm face that is not
 * emphatic enough for "you are radiating", so the slab uses a saturated red
 * while the finer TX details keep the theme's amber. */
#define C_TX_RED    lv_color_hex(0xE01010)

/* The theme's own meter.bar gradient runs green -> amber -> red but only
 * reaches red at 95% of full scale. On an S-meter that is roughly S9+53, so a
 * genuinely strong signal still read green. meter_color() below keeps the
 * theme's colours but moves the thresholds to where an operator expects them.
 */

#define CX 180
#define CY 180
#define ARC_R0   170      /* meter outer radius */
#define ARC_ROT  170      /* LVGL 0deg = 3 o'clock; 170..370 spans the top */
#define ARC_SPAN 200

/* The PTT slab runs full width and all the way to the bottom edge; the round
 * glass clips it into a chord, which is the intended shape. Making it the
 * largest target on the face is deliberate -- with toggle PTT, stopping a
 * transmission must never require aim. */
#define PTT_TOP 248

#define N_DIG 8
static const int DIG_STEP[N_DIG] = {
    1000000, 1000000, 1000000, 100000, 10000, 1000, 100, 10,
};

static lv_obj_t *s_scr, *s_dig[N_DIG], *s_sep[2], *s_underline;
static lv_obj_t *s_band, *s_mode, *s_filt, *s_step_lbl, *s_srd;
static lv_obj_t *s_meter, *s_ring, *s_ptt, *s_ptt_lbl, *s_status;
static lv_obj_t *s_dbm, *s_rit, *s_vol;
static lv_obj_t *s_edit_panel, *s_edit_title, *s_edit_value, *s_edit_hint;

typedef enum { ED_NONE = 0, ED_BAND, ED_MODE, ED_FILTER, ED_RIT, ED_VOL } edit_t;
static edit_t  s_edit;
static int     s_edit_idx;
static int32_t s_edit_rit;
static bool    s_edit_lsb;   /* passband sits below the carrier */
static uint8_t s_volume = 40;
static ui_commit_t s_commit;
static bool    s_have_commit;

/* Option lists. Modes come from AetherSDR's own modulations_list; the filter
 * widths are the common SSB/CW/digi set rather than a continuous range,
 * because a rotary picking from a short list is far quicker than one
 * scrubbing through hundreds of values. */
static const char *MODES[] = { "usb","lsb","cw","cwr","am","sam","fm","nfm",
                               "digu","digl","rtty" };
static const int32_t FILTERS[] = { 250, 500, 700, 1000, 1500, 1800, 2100,
                                   2400, 2700, 3000, 3600, 6000 };
static const struct { const char *name; int64_t hz; } BANDS[] = {
    { "160m",  1840000 }, { "80m",   3700000 }, { "60m",   5355000 },
    { "40m",   7100000 }, { "30m",  10130000 }, { "20m",  14100000 },
    { "17m",  18120000 }, { "15m",  21200000 }, { "12m",  24940000 },
    { "10m",  28400000 }, { "6m",   50200000 },
};
#define NELEM(a) ((int)(sizeof (a) / sizeof (a)[0]))
static int   s_dig_x[N_DIG];
static int   s_active_dig = 5;
static int32_t s_step_req;
static bool  s_ptt_tap, s_was_tx;
static float s_meter_disp = -127.0f;
static lv_display_t *s_disp;
/* The panel is mounted upside down relative to the USB-C port: with the cable
 * at the top, the image needs 180 degrees. Software rotation, because this
 * panel honours MADCTL MX but not MY so the controller cannot do a full turn.
 * Applied through esp_lvgl_port, which rotates TOUCH with it. */
#define UI_ROT_DEFAULT 2               /* 2 = 180 degrees */
static uint8_t s_rot = UI_ROT_DEFAULT;

/* Green below S7, amber approaching S9, red at S9 and above.
 *
 * The theme's own bar gradient only reaches red at 95% of full scale, which on
 * an S-meter is about S9+53 -- so a genuinely strong signal still showed green.
 * An operator reads "over S9" as the meaningful threshold, so that is where the
 * colour changes. */
static lv_color_t meter_color(float frac)
{
    if (frac >= 0.60f) return lv_color_hex(0xE8553C);   /* S9 and above */
    if (frac >= 0.47f) return lv_color_hex(0xE8B94C);   /* approaching S9 */
    if (frac >= 0.25f) return lv_color_hex(0x6CC56A);
    return lv_color_hex(0x2F9E6A);
}

/* S0 = -127 dBm, S9 = -73, S9+60 = -13, and S9 sits at 60% of the scale --
 * matching AetherSDR's own s-meter-v1.json. A linear ring would look wrong
 * next to the desktop. */
static float smeter_frac(float dbm)
{
    if (dbm < -127.0f) dbm = -127.0f;
    if (dbm > -13.0f)  dbm = -13.0f;
    return (dbm <= -73.0f) ? 0.6f * (dbm + 127.0f) / 54.0f
                           : 0.6f + 0.4f * (dbm + 73.0f) / 60.0f;
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

static const char *band_of(int64_t hz)
{
    const int64_t m = hz / 1000;
    if (m >= 1810   && m <= 2000)   return "160m";
    if (m >= 3500   && m <= 3800)   return "80m";
    if (m >= 5351   && m <= 5367)   return "60m";
    if (m >= 7000   && m <= 7200)   return "40m";
    if (m >= 10100  && m <= 10150)  return "30m";
    if (m >= 14000  && m <= 14350)  return "20m";
    if (m >= 18068  && m <= 18168)  return "17m";
    if (m >= 21000  && m <= 21450)  return "15m";
    if (m >= 24890  && m <= 24990)  return "12m";
    if (m >= 28000  && m <= 29700)  return "10m";
    if (m >= 50000  && m <= 52000)  return "6m";
    return "--";
}

/* --- field editors -------------------------------------------------------- */

static void edit_render(void)
{
    if (s_edit == ED_NONE) {
        lv_obj_add_flag(s_edit_panel, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_remove_flag(s_edit_panel, LV_OBJ_FLAG_HIDDEN);

    char v[16];
    const char *title = "";
    switch (s_edit) {
    case ED_BAND:
        title = "BAND";
        snprintf(v, sizeof v, "%s", BANDS[s_edit_idx].name);
        break;
    case ED_MODE: {
        title = "MODE";
        const char *m = MODES[s_edit_idx];
        size_t n = strlen(m); if (n > 7) n = 7;
        for (size_t i = 0; i < n; i++)
            v[i] = (m[i] >= 'a' && m[i] <= 'z') ? (char)(m[i] - 32) : m[i];
        v[n] = 0;
        break;
    }
    case ED_FILTER:
        title = "FILTER";
        snprintf(v, sizeof v, "%ld Hz", (long)FILTERS[s_edit_idx]);
        break;
    case ED_RIT:
        title = "RIT";
        snprintf(v, sizeof v, "%+ld Hz", (long)s_edit_rit);
        break;
    case ED_VOL:
        title = "VOLUME";
        snprintf(v, sizeof v, "%d", s_volume);
        break;
    default: return;
    }
    lv_label_set_text(s_edit_title, title);
    lv_label_set_text(s_edit_value, v);
}

static int index_of_mode(const char *m)
{
    for (int i = 0; i < NELEM(MODES); i++)
        if (m && strcasecmp(MODES[i], m) == 0) return i;
    return 0;
}

static int nearest_filter(int32_t w)
{
    int best = 0;
    int32_t bd = 1 << 30;
    for (int i = 0; i < NELEM(FILTERS); i++) {
        int32_t d = FILTERS[i] - w; if (d < 0) d = -d;
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}

static int nearest_band(int64_t hz)
{
    int best = 0;
    int64_t bd = (int64_t)1 << 60;
    for (int i = 0; i < NELEM(BANDS); i++) {
        int64_t d = BANDS[i].hz - hz; if (d < 0) d = -d;
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}

static void edit_open(edit_t what, const ui_state_t *st)
{
    s_edit = what;
    switch (what) {
    case ED_BAND:   s_edit_idx = nearest_band(st->freq_hz); break;
    case ED_MODE:   s_edit_idx = index_of_mode(st->mode);   break;
    case ED_FILTER:
        s_edit_idx = nearest_filter(st->filt_hi - st->filt_lo);
        /* Remember which side of the carrier this mode uses. Applying a
         * positive passband to LSB mutes the radio, which reads as a hardware
         * fault rather than a filter setting. */
        s_edit_lsb = (st->filt_hi <= 0) ||
                     (st->mode && (strcasecmp(st->mode, "lsb") == 0 ||
                                   strcasecmp(st->mode, "cwr") == 0 ||
                                   strcasecmp(st->mode, "digl") == 0));
        break;
    case ED_RIT:    s_edit_rit = st->rit_hz; break;
    default: break;
    }
    edit_render();
}

static void edit_commit(void)
{
    memset(&s_commit, 0, sizeof s_commit);
    switch (s_edit) {
    case ED_BAND:
        s_commit.have_freq = true;
        s_commit.freq_hz   = BANDS[s_edit_idx].hz;
        break;
    case ED_MODE:
        s_commit.have_mode = true;
        strlcpy(s_commit.mode, MODES[s_edit_idx], sizeof s_commit.mode);
        break;
    case ED_FILTER: {
        s_commit.have_filter = true;
        int32_t w = FILTERS[s_edit_idx];
        if (s_edit_lsb) { s_commit.filt_lo = -w;  s_commit.filt_hi = -100; }
        else            { s_commit.filt_lo = 100; s_commit.filt_hi =  w;   }
        break;
    }
    case ED_RIT:
        s_commit.have_rit = true;
        s_commit.rit_hz   = s_edit_rit;
        break;
    default: break;      /* volume is local-only for now */
    }
    s_have_commit = (s_edit != ED_NONE && s_edit != ED_VOL);
    s_edit = ED_NONE;
    edit_render();
}

bool ui_edit_active(void) { return s_edit != ED_NONE; }

void ui_edit_rotate(int32_t detents)
{
    if (s_edit == ED_NONE || !detents) return;
    if (!lvgl_port_lock(20)) return;
    switch (s_edit) {
    case ED_BAND:
        s_edit_idx += detents;
        if (s_edit_idx < 0) s_edit_idx = 0;
        if (s_edit_idx >= NELEM(BANDS)) s_edit_idx = NELEM(BANDS) - 1;
        break;
    case ED_MODE:
        s_edit_idx += detents;
        if (s_edit_idx < 0) s_edit_idx = 0;
        if (s_edit_idx >= NELEM(MODES)) s_edit_idx = NELEM(MODES) - 1;
        break;
    case ED_FILTER:
        s_edit_idx += detents;
        if (s_edit_idx < 0) s_edit_idx = 0;
        if (s_edit_idx >= NELEM(FILTERS)) s_edit_idx = NELEM(FILTERS) - 1;
        break;
    case ED_RIT:
        s_edit_rit += detents * 10;
        if (s_edit_rit >  9990) s_edit_rit =  9990;
        if (s_edit_rit < -9990) s_edit_rit = -9990;
        break;
    case ED_VOL: {
        int v = s_volume + detents * 2;
        if (v < 0)   v = 0;
        if (v > 100) v = 100;
        s_volume = (uint8_t)v;
        break;
    }
    default: break;
    }
    edit_render();
    lvgl_port_unlock();
}

bool ui_take_commit(ui_commit_t *out)
{
    if (!s_have_commit || !out) return false;
    *out = s_commit;
    s_have_commit = false;
    return true;
}

uint8_t ui_volume(void) { return s_volume; }

/* --- touch --------------------------------------------------------------- */

static int nearest_digit(int x)
{
    int best = 0, bd = 1 << 30;
    for (int i = 0; i < N_DIG; i++) {
        int d = x - s_dig_x[i];
        if (d < 0) d = -d;
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}

/* The last state ui_update() saw, so the editors can open on the current
 * value. The touch callback runs on the LVGL task and cannot ask the client. */
static ui_state_t s_last;

static void touch_cb(lv_event_t *e)
{
    (void)e;
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);

    /* An editor is open: ANY tap accepts. Commitment on the imprecise input,
     * selection on the precise one. */
    if (s_edit != ED_NONE) { edit_commit(); return; }

    if (p.y >= PTT_TOP) { s_ptt_tap = true; return; }   /* the whole bottom slab */

    /* band | mode | filter */
    if (p.y >= 104 && p.y < 140) {
        if      (p.x < CX - 38) edit_open(ED_BAND,   &s_last);
        else if (p.x > CX + 38) edit_open(ED_FILTER, &s_last);
        else                    edit_open(ED_MODE,   &s_last);
        return;
    }
    /* frequency digits -> step decade */
    if (p.y >= 144 && p.y < 212) {
        s_active_dig = nearest_digit(p.x);
        s_step_req   = DIG_STEP[s_active_dig];
        return;
    }
    /* step | rit | volume */
    if (p.y >= 212 && p.y < PTT_TOP) {
        if      (p.x > CX + 40) edit_open(ED_VOL, &s_last);
        else if (p.x > CX - 20) edit_open(ED_RIT, &s_last);
        return;
    }
}

/* --- build --------------------------------------------------------------- */

static lv_obj_t *mklabel(const lv_font_t *f, lv_color_t c, int x, int y,
                         const char *txt)
{
    lv_obj_t *l = lv_label_create(s_scr);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, c, 0);
    lv_label_set_text(l, txt);
    lv_obj_align(l, LV_ALIGN_CENTER, x - CX, y - CY);
    return l;
}

/* Scale ticks are LINES, not text. Seven scattered labels at this diameter
 * collided with the band/mode row and made the face look cluttered; short
 * radial marks read as a scale instantly and cost nothing. The precise value
 * lives in the numeric S-readout instead. */
static void add_ticks(void)
{
    static const struct { float dbm; uint8_t len; uint8_t kind; } TICKS[] = {
        { -121, 6, 0 }, { -109, 6, 0 }, { -97, 6, 0 }, { -85, 6, 0 },
        { -73, 11, 1 },                                  /* S9 -- the landmark */
        { -53, 6, 2 }, { -33, 6, 2 }, { -13, 9, 2 },
    };
    static lv_point_precise_t pts[sizeof TICKS / sizeof TICKS[0]][2];

    for (size_t i = 0; i < sizeof TICKS / sizeof TICKS[0]; i++) {
        float a = (ARC_ROT + smeter_frac(TICKS[i].dbm) * ARC_SPAN)
                  * 3.14159265f / 180.0f;
        float c = cosf(a), sn = sinf(a);
        int r1 = ARC_R0 - 15, r0 = r1 - TICKS[i].len;
        pts[i][0].x = (lv_value_precise_t)(CX + r0 * c);
        pts[i][0].y = (lv_value_precise_t)(CY + r0 * sn);
        pts[i][1].x = (lv_value_precise_t)(CX + r1 * c);
        pts[i][1].y = (lv_value_precise_t)(CY + r1 * sn);

        lv_obj_t *ln = lv_line_create(s_scr);
        lv_line_set_points(ln, pts[i], 2);
        lv_obj_set_style_line_width(ln, TICKS[i].kind == 1 ? 3 : 2, 0);
        lv_obj_set_style_line_color(ln,
            TICKS[i].kind == 1 ? C_TEXT2 : TICKS[i].kind == 2 ? C_WARN : C_LABEL, 0);
        lv_obj_set_style_line_rounded(ln, true, 0);
    }
}

static void build(void)
{
    s_scr = lv_screen_active();
    lv_obj_set_style_bg_color(s_scr, C_BG, 0);
    lv_obj_remove_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);

    /* TX hairline: a complete ring, which peripheral vision catches instantly
     * and which shares no geometry with anything shown in receive. */
    s_ring = lv_arc_create(s_scr);
    lv_obj_set_size(s_ring, 356, 356);
    lv_obj_center(s_ring);
    lv_arc_set_bg_angles(s_ring, 0, 360);
    lv_obj_remove_style(s_ring, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(s_ring, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(s_ring, 4, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_ring, C_BG, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_ring, 0, LV_PART_INDICATOR);

    s_meter = lv_arc_create(s_scr);
    lv_obj_set_size(s_meter, ARC_R0 * 2, ARC_R0 * 2);
    lv_obj_center(s_meter);
    lv_arc_set_rotation(s_meter, ARC_ROT);
    lv_arc_set_bg_angles(s_meter, 0, ARC_SPAN);
    lv_arc_set_range(s_meter, 0, 1000);
    lv_arc_set_value(s_meter, 0);
    lv_obj_remove_style(s_meter, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(s_meter, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(s_meter, 12, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_meter, C_SUBTLE, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_meter, 12, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_meter, C_ACCENT, LV_PART_INDICATOR);
    add_ticks();

    /* Signal, as a number as well as an arc: an arc shows trend, a number
     * lets you report a readable signal report. */
    s_srd  = mklabel(&lv_font_montserrat_20, C_TEXT,  CX, 76,  "S0");
    s_dbm  = mklabel(&lv_font_montserrat_14, C_LABEL, CX, 98,  "-127 dBm");

    s_band = mklabel(&lv_font_montserrat_20, C_ACCENT, CX - 76, 122, "--");
    s_mode = mklabel(&lv_font_montserrat_20, C_TEXT,   CX,      122, "USB");
    s_filt = mklabel(&lv_font_montserrat_20, C_TEXT2,  CX + 76, 122, "0");

    const int PITCH = 33, SMALL = 21, SEPW = 13;
    int total = 6 * PITCH + 2 * SMALL + 2 * SEPW;
    int x = CX - total / 2;
    int sep = 0;
    for (int i = 0; i < N_DIG; i++) {
        bool small = (i >= 6);
        int w = small ? SMALL : PITCH;
        s_dig_x[i] = x + w / 2;
        s_dig[i] = mklabel(small ? &lv_font_montserrat_28 : &lv_font_montserrat_48,
                           C_TEXT, s_dig_x[i], small ? 178 : 170, "0");
        x += w;
        if (i == 2 || i == 5) {
            s_sep[sep++] = mklabel(&lv_font_montserrat_48, C_LABEL,
                                   x + SEPW / 2, 170, ".");
            x += SEPW;
        }
    }

    s_underline = lv_obj_create(s_scr);
    lv_obj_set_size(s_underline, PITCH - 9, 3);
    lv_obj_set_style_bg_color(s_underline, C_ACCENT, 0);
    lv_obj_set_style_border_width(s_underline, 0, 0);
    lv_obj_set_style_radius(s_underline, 2, 0);
    lv_obj_remove_flag(s_underline, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_underline, LV_OBJ_FLAG_CLICKABLE);

    s_step_lbl = mklabel(&lv_font_montserrat_20, C_ACCENT, CX - 72, 220, "1 kHz");
    s_rit      = mklabel(&lv_font_montserrat_14, C_WARN,   CX + 12, 222, "");
    s_vol      = mklabel(&lv_font_montserrat_20, C_TEXT2,  CX + 72, 220,
                         LV_SYMBOL_VOLUME_MID " 40");
    s_status   = mklabel(&lv_font_montserrat_14, C_LABEL,  CX,      240, "");

    /* Full width, hard to the bottom edge. The circle clips it to a chord. */
    s_ptt = lv_obj_create(s_scr);
    lv_obj_set_size(s_ptt, 360, 360 - PTT_TOP);
    lv_obj_align(s_ptt, LV_ALIGN_TOP_LEFT, 0, PTT_TOP);
    lv_obj_set_style_radius(s_ptt, 0, 0);
    lv_obj_set_style_bg_color(s_ptt, C_BG1, 0);
    lv_obj_set_style_border_width(s_ptt, 0, 0);
    lv_obj_set_style_pad_all(s_ptt, 0, 0);
    lv_obj_remove_flag(s_ptt, LV_OBJ_FLAG_SCROLLABLE);
    /* An lv_obj is CLICKABLE by default, so the slab swallowed every tap and
     * the screen-level handler never ran -- PTT simply did nothing. Same trap
     * applies to the editor panel below. */
    lv_obj_remove_flag(s_ptt, LV_OBJ_FLAG_CLICKABLE);
    /* Parented to the SCREEN, not the slab: as a child it inherited the
     * container's box model and would not sit centred. */
    s_ptt_lbl = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_ptt_lbl, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(s_ptt_lbl, C_TEXT2, 0);
    lv_label_set_text(s_ptt_lbl, "PTT");
    lv_obj_set_style_text_align(s_ptt_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_ptt_lbl, LV_ALIGN_TOP_MID, 0, PTT_TOP + 12);
    lv_obj_remove_flag(s_ptt_lbl, LV_OBJ_FLAG_CLICKABLE);

    /* Editor overlay: hidden until a field is tapped. */
    s_edit_panel = lv_obj_create(s_scr);
    lv_obj_set_size(s_edit_panel, 250, 132);
    lv_obj_align(s_edit_panel, LV_ALIGN_CENTER, 0, -6);
    lv_obj_set_style_radius(s_edit_panel, 18, 0);
    lv_obj_set_style_bg_color(s_edit_panel, C_BG1, 0);
    lv_obj_set_style_bg_opa(s_edit_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_edit_panel, C_ACCENT, 0);
    lv_obj_set_style_border_width(s_edit_panel, 2, 0);
    lv_obj_remove_flag(s_edit_panel, LV_OBJ_FLAG_SCROLLABLE);
    /* Must not be clickable: "tap anywhere to accept" has to include tapping
     * the panel itself, which is the obvious place to tap. */
    lv_obj_remove_flag(s_edit_panel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_edit_panel, LV_OBJ_FLAG_HIDDEN);

    s_edit_title = lv_label_create(s_edit_panel);
    lv_obj_set_style_text_font(s_edit_title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_edit_title, C_LABEL, 0);
    lv_obj_align(s_edit_title, LV_ALIGN_TOP_MID, 0, 2);
    lv_label_set_text(s_edit_title, "");

    s_edit_value = lv_label_create(s_edit_panel);
    lv_obj_set_style_text_font(s_edit_value, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(s_edit_value, C_ACCENT_HI, 0);
    lv_obj_align(s_edit_value, LV_ALIGN_CENTER, 0, 4);
    lv_label_set_text(s_edit_value, "");

    s_edit_hint = lv_label_create(s_edit_panel);
    lv_obj_set_style_text_font(s_edit_hint, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_edit_hint, C_LABEL, 0);
    lv_obj_align(s_edit_hint, LV_ALIGN_BOTTOM_MID, 0, -2);
    lv_label_set_text(s_edit_hint, "turn to choose  -  tap to accept");

    lv_obj_add_event_cb(s_scr, touch_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_flag(s_scr, LV_OBJ_FLAG_CLICKABLE);
}

esp_err_t ui_init(void)
{
    const lvgl_port_cfg_t pc = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_RETURN_ON_ERROR(lvgl_port_init(&pc), TAG, "lvgl port");

    lvgl_port_display_cfg_t dc = {
        .io_handle     = panel_io_handle(),
        .panel_handle  = panel_handle(),
        /* 2 x 16 lines of internal DMA memory (23 kB). Every bound here was
         * MEASURED on hardware, not chosen:
         *   40 lines                        -> WebSocket task could not spawn
         *   24 lines                        -> TCP connects timed out
         *   32 lines + WiFi buffers in PSRAM-> WiFi could not init its static
         *                                      RX descriptors
         * 16 lines leaves ~38 kB internal free with WiFi and TCI both up.
         * Partial rendering means buffer height costs latency only on large
         * redraws, and the readout is per-digit precisely so those are rare.
         * Never a full framebuffer; never PSRAM for flush buffers. */
        .buffer_size   = BOARD_LCD_H_RES * 16,
        .double_buffer = true,
        .hres          = BOARD_LCD_H_RES,
        .vres          = BOARD_LCD_V_RES,
        /* No static rotation. This panel honours MADCTL MX but not MY, so
         * neither the controller nor esp_lcd_panel_mirror() can do a full
         * turn; rotation is applied at runtime instead (console 'r'), which
         * also rotates TOUCH so the two cannot disagree. A mirrored display
         * with un-mirrored touch still "works" -- just on the digit opposite
         * the one you aimed at. */
        .flags         = { .buff_dma = true, .sw_rotate = true,
                           /* RGB565 goes out byte-swapped on this SPI panel.
                            * Without this the background renders grey-green
                            * instead of near-black and every anti-aliased
                            * glyph edge picks up red/cyan fringing -- which
                            * reads as "pixelated", not as "wrong colour". */
                           .swap_bytes = true },
    };
    lv_display_t *disp = lvgl_port_add_disp(&dc);
    ESP_RETURN_ON_FALSE(disp, ESP_FAIL, TAG, "add disp");

    s_disp = disp;
    lv_display_set_rotation(disp, LV_DISPLAY_ROTATION_180);
    const lvgl_port_touch_cfg_t tc = { .disp = disp, .handle = hal_touch_handle() };
    ESP_RETURN_ON_FALSE(lvgl_port_add_touch(&tc), ESP_FAIL, TAG, "add touch");

    lvgl_port_lock(0);
    build();
    lvgl_port_unlock();
    ESP_LOGI(TAG, "LVGL up; free internal %u, largest DMA %u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
    return ESP_OK;
}

static const char *step_name(int32_t hz)
{
    switch (hz) {
    case 10:      return "10 Hz";
    case 100:     return "100 Hz";
    case 1000:    return "1 kHz";
    case 10000:   return "10 kHz";
    case 100000:  return "100 kHz";
    case 1000000: return "1 MHz";
    }
    return "-";
}

void ui_update(const ui_state_t *st)
{
    if (!st || !s_scr) return;
    if (!lvgl_port_lock(20)) return;      /* never block the caller */
    s_last = *st;                         /* editors open on the live value */

    /* While an editor is open its panel owns the screen; leave the rest of the
     * face alone so the value the operator is choosing does not jitter. */
    if (s_edit != ED_NONE) { lvgl_port_unlock(); return; }

    int64_t f = st->freq_hz < 0 ? 0 : st->freq_hz;
    int mhz = (int)(f / 1000000);
    int khz = (int)((f / 1000) % 1000);
    int hz  = (int)((f % 1000) / 10);
    int d[N_DIG] = {
        (mhz / 100) % 10, (mhz / 10) % 10, mhz % 10,
        (khz / 100) % 10, (khz / 10) % 10, khz % 10,
        (hz / 10) % 10,   hz % 10,
    };
    int lead = (mhz >= 100) ? 0 : (mhz >= 10) ? 1 : 2;

    for (int i = 0; i < N_DIG; i++) {
        char b[2] = { (char)('0' + d[i]), 0 };
        const char *txt = (i < lead) ? "" : b;
        if (strcmp(lv_label_get_text(s_dig[i]), txt) != 0)
            lv_label_set_text(s_dig[i], txt);
        lv_color_t c = (i == s_active_dig) ? C_ACCENT_HI
                     : (i > s_active_dig)  ? C_LABEL     /* these will roll */
                                           : C_TEXT;
        lv_obj_set_style_text_color(s_dig[i], st->tx ? C_TX_TEXT : c, 0);
    }
    lv_obj_align(s_underline, LV_ALIGN_CENTER,
                 s_dig_x[s_active_dig] - CX, 204 - CY);

    lv_label_set_text(s_band, band_of(f));
    if (st->mode) {
        char up[8];
        size_t n = strlen(st->mode); if (n > 7) n = 7;
        for (size_t i = 0; i < n; i++) {
            char ch = st->mode[i];
            up[i] = (ch >= 'a' && ch <= 'z') ? (char)(ch - 32) : ch;
        }
        up[n] = 0;
        lv_label_set_text(s_mode, up);
    }
    lv_label_set_text_fmt(s_filt, "%ld", (long)(st->filt_hi - st->filt_lo));
    lv_label_set_text(s_step_lbl, step_name(st->step_hz));

    /* RIT is always shown so it is always tappable, but greyed at zero: RIT
     * silently non-zero is a classic way to lose a QSO, so when it IS set it
     * has to stand out. */
    if (st->rit_hz) {
        lv_label_set_text_fmt(s_rit, "RIT %+ld", (long)st->rit_hz);
        lv_obj_set_style_text_color(s_rit, C_WARN, 0);
    } else {
        lv_label_set_text(s_rit, "RIT 0");
        lv_obj_set_style_text_color(s_rit, C_DISABLED, 0);
    }

    lv_label_set_text_fmt(s_vol, LV_SYMBOL_VOLUME_MID " %u", (unsigned)s_volume);

    lv_label_set_text(s_status,
        !st->link_ok     ? "NO LINK" :
        st->slice_locked ? "LOCKED"  : "");
    lv_obj_set_style_text_color(s_status,
        !st->link_ok ? C_DANGER : C_WARN, 0);

    /* Attack instantly, decay slowly: a meter that falls as fast as it rises
     * is unreadable, and the sample rate is only 5 Hz. */
    if (st->smeter_dbm > s_meter_disp) s_meter_disp = st->smeter_dbm;
    else s_meter_disp += (st->smeter_dbm - s_meter_disp) * 0.35f;

    float frac = smeter_frac(s_meter_disp);
    lv_arc_set_value(s_meter, (int)(frac * 1000));
    lv_obj_set_style_arc_color(s_meter,
        st->tx ? C_TX_BORDER : meter_color(frac), LV_PART_INDICATOR);
    char sbuf[10];
    smeter_text(s_meter_disp, sbuf, sizeof sbuf);
    lv_label_set_text(s_srd, st->tx ? "TX" : sbuf);
    lv_label_set_text_fmt(s_dbm, "%d dBm", (int)s_meter_disp);
    lv_obj_set_style_text_color(s_srd, st->tx ? C_TX_TEXT : C_TEXT, 0);

    if (st->tx != s_was_tx) {
        s_was_tx = st->tx;
        lv_obj_set_style_bg_color(s_scr, st->tx ? C_BG_TX : C_BG, 0);
        lv_obj_set_style_arc_color(s_ring, st->tx ? C_TX_RED : C_BG, LV_PART_MAIN);
        /* Unmissable: the whole bottom slab goes solid red. With toggle PTT
         * you can walk away from it, so it has to shout. */
        lv_obj_set_style_bg_color(s_ptt, st->tx ? C_TX_RED : C_BG1, 0);
        lv_obj_set_style_text_color(s_ptt_lbl,
            st->tx ? lv_color_white() : C_TEXT2, 0);
    }
    if (st->tx)
        lv_label_set_text_fmt(s_ptt_lbl, "TX  %lu",
                              (unsigned long)(st->tot_remain_ms / 1000));
    else
        lv_label_set_text(s_ptt_lbl, st->may_key ? "PTT" : "----");

    lvgl_port_unlock();
}

void ui_cycle_rotation(void)
{
    static const lv_display_rotation_t R[4] = {
        LV_DISPLAY_ROTATION_0,   LV_DISPLAY_ROTATION_90,
        LV_DISPLAY_ROTATION_180, LV_DISPLAY_ROTATION_270,
    };
    if (!s_disp) return;
    s_rot = (uint8_t)((s_rot + 1) & 3);
    if (lvgl_port_lock(200)) {
        lv_display_set_rotation(s_disp, R[s_rot]);
        lv_obj_invalidate(lv_screen_active());
        lvgl_port_unlock();
    }
}

uint8_t ui_rotation(void) { return s_rot; }

int32_t ui_take_step_request(void) { int32_t v = s_step_req; s_step_req = 0; return v; }
bool    ui_take_ptt_tap(void)      { bool v = s_ptt_tap;     s_ptt_tap = false; return v; }
