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

/* meter.bar.fillGradient from the same theme. */
static const struct { float at; uint32_t rgb; } METER_STOPS[] = {
    { 0.00f, 0x2F9E6A }, { 0.55f, 0x6CC56A }, { 0.80f, 0xE8B94C },
    { 0.95f, 0xE8553C }, { 1.00f, 0xF2362A },
};

#define CX 180
#define CY 180
#define ARC_R0   170      /* meter outer radius */
#define ARC_ROT  170      /* LVGL 0deg = 3 o'clock; 170..370 spans the top */
#define ARC_SPAN 200

#define N_DIG 8
static const int DIG_STEP[N_DIG] = {
    1000000, 1000000, 1000000, 100000, 10000, 1000, 100, 10,
};

static lv_obj_t *s_scr, *s_dig[N_DIG], *s_sep[2], *s_underline;
static lv_obj_t *s_band, *s_mode, *s_filt, *s_step_lbl, *s_srd;
static lv_obj_t *s_meter, *s_ring, *s_ptt, *s_ptt_lbl, *s_status;
static lv_obj_t *s_dbm, *s_rit, *s_pip;
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

static lv_color_t meter_color(float frac)
{
    for (size_t i = 1; i < sizeof METER_STOPS / sizeof METER_STOPS[0]; i++) {
        if (frac <= METER_STOPS[i].at) return lv_color_hex(METER_STOPS[i - 1].rgb);
    }
    return lv_color_hex(METER_STOPS[4].rgb);
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

static void touch_cb(lv_event_t *e)
{
    (void)e;
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);

    if (p.y > 246) { s_ptt_tap = true; return; }   /* forgiving to hit */
    if (p.y > 132 && p.y < 224) {
        s_active_dig = nearest_digit(p.x);
        s_step_req   = DIG_STEP[s_active_dig];
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

    s_step_lbl = mklabel(&lv_font_montserrat_20, C_ACCENT, CX - 44, 218, "1 kHz");
    s_rit      = mklabel(&lv_font_montserrat_14, C_WARN,   CX + 52, 220, "");
    s_status   = mklabel(&lv_font_montserrat_14, C_LABEL,  CX,      240, "");

    s_ptt = lv_obj_create(s_scr);
    lv_obj_set_size(s_ptt, 186, 56);
    lv_obj_align(s_ptt, LV_ALIGN_CENTER, 0, 100);
    lv_obj_set_style_radius(s_ptt, 28, 0);
    lv_obj_set_style_bg_color(s_ptt, C_BG1, 0);
    lv_obj_set_style_border_color(s_ptt, C_SUBTLE, 0);
    lv_obj_set_style_border_width(s_ptt, 2, 0);
    lv_obj_remove_flag(s_ptt, LV_OBJ_FLAG_SCROLLABLE);
    s_ptt_lbl = lv_label_create(s_ptt);
    lv_obj_set_style_text_font(s_ptt_lbl, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(s_ptt_lbl, C_TEXT2, 0);
    lv_label_set_text(s_ptt_lbl, "PTT");
    lv_obj_center(s_ptt_lbl);

    /* Link pip: small, low in the face, out of the way until it matters. */
    s_pip = lv_obj_create(s_scr);
    lv_obj_set_size(s_pip, 10, 10);
    lv_obj_align(s_pip, LV_ALIGN_CENTER, 0, 138);
    lv_obj_set_style_radius(s_pip, 5, 0);
    lv_obj_set_style_border_width(s_pip, 0, 0);
    lv_obj_set_style_bg_color(s_pip, C_DISABLED, 0);
    lv_obj_remove_flag(s_pip, LV_OBJ_FLAG_SCROLLABLE);

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

    /* RIT only appears when it is doing something. A chip reading "RIT 0" is
     * just noise, but RIT silently non-zero is a classic way to lose a QSO. */
    if (st->rit_hz) lv_label_set_text_fmt(s_rit, "RIT %+ld", (long)st->rit_hz);
    else            lv_label_set_text(s_rit, "");

    lv_obj_set_style_bg_color(s_pip,
        !st->link_ok ? C_DANGER : st->tx ? C_TX_BORDER : C_ACCENT, 0);

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
        lv_obj_set_style_arc_color(s_ring, st->tx ? C_TX_BORDER : C_BG, LV_PART_MAIN);
        lv_obj_set_style_bg_color(s_ptt, st->tx ? C_BG_TX : C_BG1, 0);
        lv_obj_set_style_border_color(s_ptt,
            st->tx ? C_TX_BORDER : C_SUBTLE, 0);
        lv_obj_set_style_text_color(s_ptt_lbl,
            st->tx ? C_TX_TEXT : C_TEXT2, 0);
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
