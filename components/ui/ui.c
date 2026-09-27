#include "ui.h"
#include "board_pins.h"
#include "hal_touch.h"
#include "panel.h"

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "ui";

/* Palette. Saturated accent on near-black: identity comes from colour and
 * shape, not from contrast ratio. */
#define C_BG        lv_color_hex(0x0B0D0F)
#define C_BG_TX     lv_color_hex(0x140A09)
#define C_FG        lv_color_hex(0xE8EDF2)
#define C_DIM       lv_color_hex(0x6B7580)
#define C_ACCENT    lv_color_hex(0x4DD4AC)
#define C_TX        lv_color_hex(0xFF4A3D)
#define C_WARN      lv_color_hex(0xE8C547)
#define C_TRACK     lv_color_hex(0x22262B)

#define CX 180
#define CY 180

/* MMM.kkk.hh -- 8 digits, 10 Hz resolution, plus two separators. */
#define N_DIG 8
static const int DIG_STEP[N_DIG] = {
    1000000, 1000000, 1000000,   /* MHz: anything above 1 MHz/detent is silly */
    100000, 10000, 1000,         /* kHz */
    100, 10,                     /* Hz  */
};

static lv_obj_t *s_scr;
static lv_obj_t *s_dig[N_DIG];
static lv_obj_t *s_sep[2];
static lv_obj_t *s_underline;
static lv_obj_t *s_mode, *s_filt, *s_step_lbl, *s_meter, *s_ring;
static lv_obj_t *s_ptt, *s_ptt_lbl, *s_lock_lbl;

static int   s_dig_x[N_DIG];
static int   s_active_dig = 5;      /* 1 kHz by default */
static int32_t s_step_req;
static bool  s_ptt_tap;
static bool  s_was_tx;

/* --- geometry ----------------------------------------------------------- */

/* Digit pitch is ~34 px = 4.3 mm, well under the CST816's ~2 mm centroid
 * error, so a tap is snapped to the NEAREST digit centre across the whole
 * readout band. That leaves no dead zones and no misses -- only off-by-one
 * decade errors, which the underline makes obvious and one more tap fixes. */
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

    /* Bottom region: PTT. Forgiving to hit, because stopping a transmission
     * must never require aim. */
    if (p.y > 250) { s_ptt_tap = true; return; }

    /* Readout band: choose the step decade. */
    if (p.y > 140 && p.y < 225) {
        int i = nearest_digit(p.x);
        s_active_dig = i;
        s_step_req   = DIG_STEP[i];
    }
}

/* --- build -------------------------------------------------------------- */

static lv_obj_t *mklabel(lv_obj_t *par, const lv_font_t *f, lv_color_t c,
                         int x, int y, const char *txt)
{
    lv_obj_t *l = lv_label_create(par);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, c, 0);
    lv_label_set_text(l, txt);
    lv_obj_align(l, LV_ALIGN_TOP_MID, x - CX, y);
    return l;
}

static void build(void)
{
    s_scr = lv_screen_active();
    lv_obj_set_style_bg_color(s_scr, C_BG, 0);
    lv_obj_remove_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);

    /* TX hairline: a complete ring all the way round, which peripheral vision
     * catches instantly and which shares no geometry with anything in RX. */
    s_ring = lv_arc_create(s_scr);
    lv_obj_set_size(s_ring, 358, 358);
    lv_obj_center(s_ring);
    lv_arc_set_bg_angles(s_ring, 0, 360);
    lv_arc_set_value(s_ring, 0);
    lv_obj_remove_style(s_ring, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(s_ring, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(s_ring, 3, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_ring, C_BG, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_ring, 0, LV_PART_INDICATOR);

    /* S-meter across the top 200 degrees. */
    s_meter = lv_arc_create(s_scr);
    lv_obj_set_size(s_meter, 320, 320);
    lv_obj_center(s_meter);
    lv_arc_set_rotation(s_meter, 170);
    lv_arc_set_bg_angles(s_meter, 0, 200);
    lv_arc_set_range(s_meter, 0, 100);
    lv_arc_set_value(s_meter, 0);
    lv_obj_remove_style(s_meter, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(s_meter, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(s_meter, 16, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_meter, C_TRACK, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_meter, 16, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_meter, C_ACCENT, LV_PART_INDICATOR);

    s_mode = mklabel(s_scr, &lv_font_montserrat_28, C_ACCENT, CX - 60, 96, "USB");
    s_filt = mklabel(s_scr, &lv_font_montserrat_20, C_DIM,    CX + 60, 104, "2.4k");

    /* Frequency: fixed pitch, one label per digit. */
    const int PITCH = 34, SEPW = 14;
    int order[N_DIG + 2];      /* 0..7 digits, -1 separators */
    int k = 0;
    for (int i = 0; i < 3; i++) order[k++] = i;
    order[k++] = -1;
    for (int i = 3; i < 6; i++) order[k++] = i;
    order[k++] = -1;
    for (int i = 6; i < N_DIG; i++) order[k++] = i;

    int total = N_DIG * PITCH + 2 * SEPW;
    int x = CX - total / 2;
    int sep = 0;
    for (int i = 0; i < k; i++) {
        if (order[i] < 0) {
            s_sep[sep++] = mklabel(s_scr, &lv_font_montserrat_48, C_DIM,
                                   x + SEPW / 2, 150, ".");
            x += SEPW;
        } else {
            int d = order[i];
            s_dig_x[d] = x + PITCH / 2;
            s_dig[d] = mklabel(s_scr, &lv_font_montserrat_48, C_FG,
                               s_dig_x[d], 144, "0");
            x += PITCH;
        }
    }

    /* The underline is what actually reads at a glance; a coloured digit
     * changing 8 -> 1 is easy to lose. */
    s_underline = lv_obj_create(s_scr);
    lv_obj_set_size(s_underline, PITCH - 8, 4);
    lv_obj_set_style_bg_color(s_underline, C_ACCENT, 0);
    lv_obj_set_style_border_width(s_underline, 0, 0);
    lv_obj_set_style_radius(s_underline, 2, 0);
    lv_obj_remove_flag(s_underline, LV_OBJ_FLAG_SCROLLABLE);

    s_step_lbl = mklabel(s_scr, &lv_font_montserrat_20, C_DIM, CX, 214, "1 kHz");
    s_lock_lbl = mklabel(s_scr, &lv_font_montserrat_20, C_WARN, CX, 238, "");

    /* PTT pill. */
    s_ptt = lv_obj_create(s_scr);
    lv_obj_set_size(s_ptt, 200, 66);
    lv_obj_align(s_ptt, LV_ALIGN_TOP_MID, 0, 262);
    lv_obj_set_style_radius(s_ptt, 33, 0);
    lv_obj_set_style_bg_color(s_ptt, C_TRACK, 0);
    lv_obj_set_style_border_color(s_ptt, C_DIM, 0);
    lv_obj_set_style_border_width(s_ptt, 2, 0);
    lv_obj_remove_flag(s_ptt, LV_OBJ_FLAG_SCROLLABLE);
    s_ptt_lbl = lv_label_create(s_ptt);
    lv_obj_set_style_text_font(s_ptt_lbl, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(s_ptt_lbl, C_FG, 0);
    lv_label_set_text(s_ptt_lbl, "PTT");
    lv_obj_center(s_ptt_lbl);

    lv_obj_add_event_cb(s_scr, touch_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_flag(s_scr, LV_OBJ_FLAG_CLICKABLE);
}

esp_err_t ui_init(void)
{
    const lvgl_port_cfg_t pc = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_RETURN_ON_ERROR(lvgl_port_init(&pc), TAG, "lvgl port");

    lvgl_port_display_cfg_t dc = {
        .io_handle     = NULL,
        .panel_handle  = panel_handle(),
        /* 2 x 24 lines = 34.6 kB of internal DMA memory. 40 lines (57.6 kB)
         * starved the WebSocket task of the internal RAM it needs to spawn --
         * measured, not guessed. Never a full framebuffer (259 kB will not
         * coexist with WiFi) and never PSRAM, which Espressif measure at
         * 2.5-4x slower for flush buffers. */
        .buffer_size   = BOARD_LCD_H_RES * 24,
        .double_buffer = true,
        .hres          = BOARD_LCD_H_RES,
        .vres          = BOARD_LCD_V_RES,
        .flags = { .buff_dma = true },
    };
    dc.io_handle = panel_io_handle();
    lv_display_t *disp = lvgl_port_add_disp(&dc);
    ESP_RETURN_ON_FALSE(disp, ESP_FAIL, TAG, "add disp");

    const lvgl_port_touch_cfg_t tc = {
        .disp   = disp,
        .handle = hal_touch_handle(),
    };
    ESP_RETURN_ON_FALSE(lvgl_port_add_touch(&tc), ESP_FAIL, TAG, "add touch");

    lvgl_port_lock(0);
    build();
    lvgl_port_unlock();
    ESP_LOGI(TAG, "LVGL up, 2 x 24 line buffers; free internal %u, largest DMA %u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
    return ESP_OK;
}

/* --- update ------------------------------------------------------------- */

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
    return "?";
}

void ui_update(const ui_state_t *st)
{
    if (!st || !s_scr) return;
    if (!lvgl_port_lock(20)) return;      /* never block the caller */

    int64_t f = st->freq_hz;
    if (f < 0) f = 0;
    int mhz = (int)(f / 1000000);
    int khz = (int)((f / 1000) % 1000);
    int hz  = (int)((f % 1000) / 10);
    int d[N_DIG] = {
        (mhz / 100) % 10, (mhz / 10) % 10, mhz % 10,
        (khz / 100) % 10, (khz / 10) % 10, khz % 10,
        (hz / 10) % 10,   hz % 10,
    };

    /* Leading zeros above the MHz digit are blanked, not drawn as '0'. */
    int lead = (mhz >= 100) ? 0 : (mhz >= 10) ? 1 : 2;

    for (int i = 0; i < N_DIG; i++) {
        char buf[2] = { (char)('0' + d[i]), 0 };
        const char *txt = (i < lead) ? " " : buf;
        if (strcmp(lv_label_get_text(s_dig[i]), txt) != 0)
            lv_label_set_text(s_dig[i], txt);
        /* Digits below the active step will roll, so dim them. */
        lv_obj_set_style_text_color(s_dig[i],
            i < lead ? C_BG : (i > s_active_dig ? C_DIM : C_FG), 0);
    }

    lv_obj_align(s_underline, LV_ALIGN_TOP_MID,
                 s_dig_x[s_active_dig] - CX, 206);

    lv_label_set_text(s_step_lbl, step_name(st->step_hz));
    if (st->mode) {
        char up[8];
        size_t n = strlen(st->mode);
        if (n > 7) n = 7;
        for (size_t i = 0; i < n; i++)
            up[i] = (char)(st->mode[i] >= 'a' && st->mode[i] <= 'z'
                           ? st->mode[i] - 32 : st->mode[i]);
        up[n] = 0;
        lv_label_set_text(s_mode, up);
    }
    lv_label_set_text_fmt(s_filt, "%ld", (long)((st->filt_hi - st->filt_lo)));

    lv_label_set_text(s_lock_lbl,
        !st->link_ok      ? "NO LINK" :
        st->slice_locked  ? "LOCKED"  : "");

    /* S-meter: S9 sits at 60% of the arc, matching AetherSDR's own scale.
     * A linear ring would look wrong beside the desktop. */
    double c = st->smeter_dbm;
    if (c < -127) c = -127;
    if (c > -13)  c = -13;
    double frac = (c <= -73.0) ? 0.6 * (c + 127.0) / 54.0
                               : 0.6 + 0.4 * (c + 73.0) / 60.0;
    lv_arc_set_value(s_meter, (int)(frac * 100));

    if (st->tx != s_was_tx) {
        s_was_tx = st->tx;
        lv_obj_set_style_bg_color(s_scr, st->tx ? C_BG_TX : C_BG, 0);
        lv_obj_set_style_arc_color(s_ring, st->tx ? C_TX : C_BG, LV_PART_MAIN);
        lv_obj_set_style_bg_color(s_ptt, st->tx ? C_TX : C_TRACK, 0);
        lv_obj_set_style_arc_color(s_meter, st->tx ? C_TX : C_ACCENT,
                                   LV_PART_INDICATOR);
    }
    if (st->tx)
        lv_label_set_text_fmt(s_ptt_lbl, "TX %lus",
                              (unsigned long)(st->tot_remain_ms / 1000));
    else
        lv_label_set_text(s_ptt_lbl, st->may_key ? "PTT" : "---");

    lvgl_port_unlock();
}

int32_t ui_take_step_request(void)
{
    int32_t v = s_step_req;
    s_step_req = 0;
    return v;
}

bool ui_take_ptt_tap(void)
{
    bool v = s_ptt_tap;
    s_ptt_tap = false;
    return v;
}
