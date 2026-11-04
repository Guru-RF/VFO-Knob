/* The face's touch on the host (tools/lvhost): components/ui/ui.c itself,
 * built for a firmware -- the FlexRadio's or the Icoms' -- on LVGL 9.3, its
 * ESP-IDF names stubbed (stub/), the touch fed press by press on a clock of
 * its own. What it checks is the PTT slab: a press held half a second opens
 * the antennas with a buzz and keys nothing -- the receive antenna, then the
 * transmit one where the radio chooses that apart, then nothing more -- and
 * so, where it can, a tap keys once the finger has stayed off a moment, the
 * glass losing a held finger (as the CST816 does, for up to 130 ms) never
 * keying: not mid-hold, not with one read misplaced, not after the buzz,
 * when the antennas stay up. On the air any touch unkeys at once and no hold
 * opens anything; a radio with no antenna choice keys as the finger lifts,
 * as it always did. And the swipe down's row, the memory face, a web SDR's
 * reading under the S-units -- playing, and quiet where it cannot reach the
 * dial -- and pictures of each (PPM, and PNG where ImageMagick is). The
 * other faces with a web SDR -- the Xiegu's, the UberSDR's, Kiwi888's -- for
 * that reading too, between their neighbours at their widest.
 *
 * On every face that shows a Bluetooth headset's logo -- the radios', the
 * reflector's, the telephone's, the receivers' -- the device's battery
 * beside it: hidden until the device has said its charge, then its glyph
 * and its colour for it (green, yellow, red; white on the red slab), and
 * where it lands: in the slab, well inside the glass, clear of the logo,
 * the caption, the spot, the receiver's name, RAISE BOOM and the
 * telephone's halves -- each caption and state the slab has, a headset's
 * and a speaker's.
 *
 * On the UberSDR's face, a guest's time left at the slab's other end:
 * every value it can say, its words and colour, its ink in the slab and
 * inside the glass, level with the logo, clear of the spot's call however
 * long -- beside no device, a headset's battery or a speaker's. And the
 * call in the room between them: centred where it fits so, else moved
 * aside just as far as it must, whole; only one too long for the room cut
 * with dots -- as Kiwi888's slab has the receiver's name. And with no link,
 * the receiver the warning is about named under it -- NOT FOUND, then
 * CONNECTING to the next -- between the warning and the address card, its
 * panel inside the glass; a name too long, cut with dots inside the panel;
 * with none, the panel as ever.
 *
 * And on every face, the setup firmware's too, the knob's own battery, while
 * it runs on it: none until its charge is known, nor on USB power; then its
 * glyph and colour for it, as a headset's, centred at the top of the arc
 * over the S-units -- clear of the arc, its ticks and its peak, the reading
 * under it however wide, OV after it on Kiwi888's, a web SDR's line, the
 * telephone's two meters at full, the setup firmware's titles, and of what
 * comes up over the middle: an editor, a chooser, a question, a warning,
 * the address card. Not on a radio's face on the air, where the transmit
 * scale's numbers are; on the reflector's, whose arc stays the audio's.
 * Under the full-face views, the SSTV viewer and the telephone's keypad: no
 * ink of it there.
 *
 *   make slab-check OUT=dir     every face's runs, the pictures in dir */
#include "lvgl.h"
#include "esp_lvgl_port.h"
#include "hal_touch.h"
#include "panel.h"
#include "ui.h"
#if VFO_RADIO_CLOCK
#include "clockface.h"
#endif

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 360
#define H 360
/* Not every face's runs use every helper. */
#define SOME __attribute__((unused))

/* ---- the clock, the display, the touch ---- */
static uint32_t now_ms;
static uint32_t tick_cb(void) { return now_ms; }

static uint16_t fb[W * H];
static uint8_t  buf[W * 12 * 2] __attribute__((aligned(4)));
static lv_display_t *disp;

static int n_flush;                   /* LVGL's flushes, counted */
static long flush_px;                 /* ...and the pixels they sent */
static void flush_cb(lv_display_t *d, const lv_area_t *a, uint8_t *px)
{
    n_flush++;
    const int w = lv_area_get_width(a);
    flush_px += (long)w * lv_area_get_height(a);
    for (int y = a->y1; y <= a->y2; y++)
        memcpy(&fb[y * W + a->x1], px + (size_t)(y - a->y1) * w * 2, (size_t)w * 2);
    lv_display_flush_ready(d);
}

esp_err_t lvgl_port_init(const lvgl_port_cfg_t *cfg)
{
    (void)cfg;
    return ESP_OK;
}

lv_display_t *lvgl_port_add_disp(const lvgl_port_display_cfg_t *cfg)
{
    (void)cfg;
    disp = lv_display_create(W, H);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(disp, buf, NULL, sizeof buf, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, flush_cb);
    return disp;
}

lv_indev_t *lvgl_port_add_touch(const lvgl_port_touch_cfg_t *cfg)
{
    lv_indev_t *t = lv_indev_create();
    lv_indev_set_type(t, LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(t, cfg->disp);
    return t;
}

static bool     finger;
static uint16_t fx, fy;
static struct esp_lcd_touch_s { int unused; } the_touch;
esp_lcd_touch_handle_t hal_touch_handle(void) { return &the_touch; }
esp_err_t esp_lcd_touch_read_data(esp_lcd_touch_handle_t tp) { (void)tp; return ESP_OK; }
esp_err_t esp_lcd_touch_get_data(esp_lcd_touch_handle_t tp, esp_lcd_touch_point_data_t *p,
                                 uint8_t *n, uint8_t max)
{
    (void)tp;
    (void)max;
    *n = finger ? 1 : 0;
    if (finger) { p[0].x = fx; p[0].y = fy; }
    return ESP_OK;
}

esp_lcd_panel_handle_t panel_handle(void) { return NULL; }
esp_lcd_panel_io_handle_t panel_io_handle(void) { return NULL; }
static int backlight = -1;            /* the duty last set: -1, none yet */
void panel_set_brightness(uint8_t duty) { backlight = duty; }

/* The screensaver's own path to the glass: its pixels go out byte-swapped,
 * as the port sends LVGL's, and land in the same frame. */
static int panel_px;                  /* pixels sent since last asked */
esp_err_t esp_lcd_panel_draw_bitmap(esp_lcd_panel_handle_t p, int x1, int y1, int x2, int y2, const void *px)
{
    (void)p;
    const uint16_t *s = px;
    for (int y = y1; y < y2; y++)
        for (int x = x1; x < x2; x++, s++)
            if (x >= 0 && x < W && y >= 0 && y < H) fb[y * W + x] = (uint16_t)(*s << 8 | *s >> 8);
    panel_px += (x2 - x1) * (y2 - y1);
    return ESP_OK;
}
esp_err_t esp_lcd_panel_disp_on_off(esp_lcd_panel_handle_t p, bool on) { (void)p; (void)on; return ESP_OK; }

/* The time: 2026-11-04 13:52:07 UTC, 14:52 in Brussels, until a run has none. */
#include "wallclock.h"
static bool clock_set = true;
bool wallclock_now(struct tm *local, struct tm *utc, uint16_t *ms)
{
    if (!clock_set) return false;
    const time_t t = 1793800327 + now_ms / 1000;
    if (local) { const time_t l = t + 3600; gmtime_r(&l, local); }
    if (utc) gmtime_r(&t, utc);
    if (ms) *ms = (uint16_t)(now_ms % 1000);
    return true;
}
uint32_t wallclock_age_s(void) { return clock_set ? 60 : UINT32_MAX; }
bool net_prov_clock24(void) { return true; }
static uint8_t ck_face;
static bool    ck_sec = true;
uint8_t net_prov_clock_face(void) { return ck_face; }
bool net_prov_clock_seconds(void) { return ck_sec; }
void net_prov_save_clock(uint8_t face, bool seconds) { ck_face = face; ck_sec = seconds; }

/* splash.c's: no splash here, and no update screen. */
void ui_splash_start(void) {}
void ui_updating_show(void) {}
void ui_updating_reboot(void) {}
void ui_switching(const char *radio) { (void)radio; }
void ui_updating_progress(int percent) { (void)percent; }
void ui_updating_result(bool ok, const char *message) { (void)ok; (void)message; }
void ui_updating_hide(void) {}

/* ---- the face's state, as the ui task gives it every 50 ms ---- */
static ui_state_t st;
static uint32_t   next_update;

/* Time goes on 1 ms at a time: LVGL's timers, the touch read every 10 ms,
 * the state every 50. */
static void run(uint32_t ms)
{
    for (uint32_t i = 0; i < ms; i++) {
        now_ms++;
        if ((int32_t)(now_ms - next_update) >= 0) {
            next_update = now_ms + 50;
            ui_update(&st);
        }
        lv_timer_handler();
    }
}

static void down(int x, int y) { fx = (uint16_t)x; fy = (uint16_t)y; finger = true; }
static void up(void) { finger = false; }

/* A visible label with this text, anywhere on the screen. */
static bool visible(lv_obj_t *o)
{
    for (; o; o = lv_obj_get_parent(o))
        if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) return false;
    return true;
}
static bool find(lv_obj_t *o, const char *text)
{
    if (lv_obj_check_type(o, &lv_label_class) && visible(o) && !strcmp(lv_label_get_text(o), text))
        return true;
    for (uint32_t i = 0; i < lv_obj_get_child_count(o); i++)
        if (find(lv_obj_get_child(o, (int32_t)i), text)) return true;
    return false;
}
static bool shown(const char *text) { return find(lv_screen_active(), text); }
/* ...and one whose text begins so: the label itself. */
SOME static lv_obj_t *label_from(lv_obj_t *o, const char *prefix)
{
    if (lv_obj_check_type(o, &lv_label_class) && visible(o) &&
        !strncmp(lv_label_get_text(o), prefix, strlen(prefix)))
        return o;
    for (uint32_t i = 0; i < lv_obj_get_child_count(o); i++) {
        lv_obj_t *l = label_from(lv_obj_get_child(o, (int32_t)i), prefix);
        if (l) return l;
    }
    return NULL;
}

/* ---- checks ---- */
static int n_run, n_fail;
static const char *where = "";
#define CHECK(c) do { n_run++; if (!(c)) { n_fail++; printf("FAIL [%s] %s:%d: %s\n", where, __FILE__, __LINE__, #c); } } while (0)

/* What the face asked for since the last look: taken, as the ui task takes it. */
static bool ptt(void)  { return ui_take_ptt_tap(); }
static bool buzz(void) { return ui_take_slab_hold(); }
static bool commit(ui_commit_t *c) { memset(c, 0, sizeof *c); return ui_take_commit(c); }
static void quiet(void)
{
    ui_commit_t c;
    (void)ptt(); (void)buzz(); (void)commit(&c);
}

/* A tap: down `ms`, then up -- and the touch read once more (every
 * TOUCH_POLL_MS), so the release has been seen. */
#define SEEN 15
static void tap_at(int x, int y, uint32_t ms) { down(x, y); run(ms); up(); run(SEEN); }
static void settle(void) { run(400); }

static void picture(const char *dir, const char *name)
{
    lv_obj_invalidate(lv_screen_active());
    lv_refr_now(disp);
    char path[512];
    snprintf(path, sizeof path, "%s/%s.ppm", dir, name);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (int i = 0; i < W * H; i++) {
        /* The round glass: outside it, black. */
        const int x = i % W - W / 2, y = i / W - H / 2;
        const uint16_t c = x * x + y * y <= 180 * 180 ? fb[i] : 0;
        const uint8_t px[3] = { (uint8_t)((c >> 11) << 3), (uint8_t)(((c >> 5) & 63) << 2),
                                (uint8_t)((c & 31) << 3) };
        fwrite(px, 1, 3, f);
    }
    fclose(f);
    char cmd[1200];
    snprintf(cmd, sizeof cmd, "command -v magick >/dev/null && magick %s/%s.ppm %s/%s.png", dir, name, dir, name);
    if (system(cmd)) {}
}

/* The frame as the glass shows it now: no refresh first, as a screensaver
 * draws round LVGL. */
static void snap(const char *dir, const char *name)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s.ppm", dir, name);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (int i = 0; i < W * H; i++) {
        const int x = i % W - W / 2, y = i / W - H / 2;
        const uint16_t c = x * x + y * y <= 180 * 180 ? fb[i] : 0;
        const uint8_t px[3] = { (uint8_t)((c >> 11) << 3), (uint8_t)(((c >> 5) & 63) << 2),
                                (uint8_t)((c & 31) << 3) };
        fwrite(px, 1, 3, f);
    }
    fclose(f);
    char cmd[1200];
    snprintf(cmd, sizeof cmd, "command -v magick >/dev/null && magick %s/%s.ppm %s/%s.png", dir, name, dir, name);
    if (system(cmd)) {}
}

#define SLAB_X 180
#define SLAB_Y 300
#define PANEL_X 180
#define PANEL_Y 174
#define OUTSIDE_X 180
#define OUTSIDE_Y 60

/* Where the antennas are a hold away, a tap keys once the finger has stayed
 * off PTT_REARM_MS (150 ms, ui.c) -- seen by the next touch read, settled by
 * a 15 ms timer: never sooner after the lift than this, nor later than that. */
#define KEY_SOONEST 150
#define KEY_LATEST  180

/* A finger's contacts with the glass, from time 0: down over [t0, t1), at
 * x, y -- the glass losing it for a moment is a gap between two. */
typedef struct { uint32_t t0, t1; int x, y; } seg_t;
typedef struct {
    int     keys;
    int32_t key_at;                   /* the first key: ms from time 0; -1 none */
    bool    buzz, opened, open_at_end;
} seen_t;

SOME static seen_t timeline(const seg_t *s, int n, uint32_t end)
{
    seen_t r = { .key_at = -1 };
    quiet();
    for (uint32_t t = 0; t < end; t++) {
        const seg_t *on = NULL;
        for (int i = 0; i < n; i++)
            if (t >= s[i].t0 && t < s[i].t1) on = &s[i];
        if (on) down(on->x, on->y);
        else    up();
        run(1);
        if (ptt() && !r.keys++) r.key_at = (int32_t)t;
        if (buzz()) r.buzz = true;
        if (ui_edit_active()) r.opened = true;
    }
    r.open_at_end = ui_edit_active();
    return r;
}

/* ...and the face as it was: the finger off, the editor shut, the clock on. */
SOME static void after(void)
{
    up(); run(SEEN);
    if (ui_edit_active()) tap_at(OUTSIDE_X, OUTSIDE_Y, 80);
    settle(); quiet();
}

/* ---- the radios ---- */
static void base(void)
{
    memset(&st, 0, sizeof st);
    st.n_sstv = -1;
    st.rxsrc = -1;
    st.freq_hz = 14200000;
    st.step_hz = 100;
    st.mode = "usb";
    st.filt_lo = 100;
    st.filt_hi = 2800;
    st.smeter_dbm = -85.0f;
    st.link_ok = true;
    st.may_key = true;
}

/* ---- a Bluetooth device's battery, beside its logo (ui.c headset_slab) ---- */

#define SLAB_TOP 248                       /* ui.c PTT_TOP */
#define GLASS_R  168                       /* ui.c: every widget inside it */
#define ROOM     3                         /* nothing else's ink this close to the battery's */
#define NLEVELS  (int)(sizeof LEVELS / sizeof *LEVELS)

static uint16_t with_b[W * H], without_b[W * H];
static int      least_room = 99;
static const int LEVELS[] = { 100, 90, 88, 87, 80, 70, 63, 62, 60, 50, 49, 40, 38, 37, 30, 21, 20, 13, 12, 10, 0 };

/* The whole face, drawn now, into dst. */
static void render(uint16_t *dst)
{
    lv_obj_invalidate(lv_screen_active());
    lv_refr_now(disp);
    memcpy(dst, fb, sizeof fb);
}

static const char *const BATTS[5] = { LV_SYMBOL_BATTERY_EMPTY, LV_SYMBOL_BATTERY_1, LV_SYMBOL_BATTERY_2,
                                      LV_SYMBOL_BATTERY_3, LV_SYMBOL_BATTERY_FULL };

/* A battery's label, shown: a visible label with one of its glyphs -- in
 * the slab (`slab`), a device's, or over it, the knob's own. */
static lv_obj_t *battery_in(lv_obj_t *o, bool slab)
{
    if (lv_obj_check_type(o, &lv_label_class) && visible(o) && (lv_obj_get_y(o) >= SLAB_TOP) == slab)
        for (int i = 0; i < 5; i++)
            if (!strcmp(lv_label_get_text(o), BATTS[i])) return o;
    for (uint32_t i = 0; i < lv_obj_get_child_count(o); i++) {
        lv_obj_t *b = battery_in(lv_obj_get_child(o, (int32_t)i), slab);
        if (b) return b;
    }
    return NULL;
}
static lv_obj_t *battery(lv_obj_t *o) { return battery_in(o, true); }
static bool batt_shown(void) { return battery(lv_screen_active()) != NULL; }

/* Its glyph for a charge: to the nearest quarter. */
static const char *glyph_for(int pct)
{
    return BATTS[pct >= 88 ? 4 : pct >= 63 ? 3 : pct >= 38 ? 2 : pct >= 13 ? 1 : 0];
}

/* Its colour, by hue -- each face has its own green, yellow and red -- or
 * white. */
enum { IS_GREEN, IS_YELLOW, IS_RED, IS_WHITE, IS_OTHER };
static const char *const IS[] = { "green", "yellow", "red", "white", "neither" };
static int colour_is(lv_color_t c)
{
    const int r = c.red, g = c.green, b = c.blue;
    const int hi = r > g ? (r > b ? r : b) : (g > b ? g : b);
    const int lo = r < g ? (r < b ? r : b) : (g < b ? g : b);
    if (lo > 0xE0) return IS_WHITE;
    if (hi - lo < 0x40) return IS_OTHER;
    const double d = hi - lo;
    double h = hi == r ? 60.0 * fmod((g - b) / d, 6.0) : hi == g ? 60.0 * ((b - r) / d + 2.0)
                                                                  : 60.0 * ((r - g) / d + 4.0);
    if (h < 0) h += 360.0;
    return h < 15 || h >= 345 ? IS_RED : h >= 30 && h < 65 ? IS_YELLOW : h >= 90 && h < 170 ? IS_GREEN : IS_OTHER;
}

/* ...for a charge: green from half up, yellow under that, red at a fifth
 * and below. */
static int colour_for(int pct) { return pct >= 50 ? IS_GREEN : pct > 20 ? IS_YELLOW : IS_RED; }

/* No pixel but the slab's own colour on the ring d px out from the box. */
static bool ring_clear(int x1, int y1, int x2, int y2, int d, uint16_t bg)
{
    for (int y = y1 - d; y <= y2 + d; y++)
        for (int x = x1 - d; x <= x2 + d; x++) {
            if (x > x1 - d && x < x2 + d && y > y1 - d && y < y2 + d) continue;
            if (x < 0 || y < 0 || x >= W || y >= H) continue;
            if (without_b[y * W + x] != bg) return false;
        }
    return true;
}

/* What the face draws with `o` and not without it: its ink's box (false:
 * none), and the px of nothing else's ink around it, up to 40. */
typedef struct { int x1, y1, x2, y2; } box_t;
SOME static bool ink_of(lv_obj_t *o, box_t *b, int *room)
{
    render(with_b);
    lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    render(without_b);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    *b = (box_t){ W, H, -1, -1 };
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            if (with_b[y * W + x] != without_b[y * W + x]) {
                if (x < b->x1) b->x1 = x;
                if (x > b->x2) b->x2 = x;
                if (y < b->y1) b->y1 = y;
                if (y > b->y2) b->y2 = y;
            }
    if (b->x2 < 0) return false;
    const uint16_t bg = without_b[((b->y1 + b->y2) / 2) * W + (b->x1 + b->x2) / 2];
    *room = 0;
    while (*room < 40 && ring_clear(b->x1, b->y1, b->x2, b->y2, *room + 1, bg)) (*room)++;
    return true;
}

/* ...inside the glass's margin, every corner of it. */
SOME static bool in_glass(const box_t *b)
{
    const int cx[2] = { b->x1, b->x2 + 1 }, cy[2] = { b->y1, b->y2 + 1 };
    for (int i = 0; i < 4; i++) {
        const int dx = cx[i & 1] - W / 2, dy = cy[i >> 1] - H / 2;
        if (dx * dx + dy * dy > GLASS_R * GLASS_R) return false;
    }
    return true;
}

/* The battery as the face shows it now, at st.batt: its glyph and colour --
 * white on the red slab (`white`) -- and its ink, what the face draws with
 * it and not without: in the slab, inside the glass's margin, and nothing
 * else's ink within ROOM px -- the logo's, the caption's, the spot's, a
 * button's, the edge of a half. The room it has is logged; a picture of it
 * taken where `pic` names one. */
static void batt_check(const char *dir, const char *pic, bool white)
{
    lv_obj_t *b = battery(lv_screen_active());
    CHECK(b != NULL);
    if (!b) return;
    CHECK(!strcmp(lv_label_get_text(b), glyph_for(st.batt)));
    const int want = white ? IS_WHITE : colour_for(st.batt);
    const int got  = colour_is(lv_obj_get_style_text_color(b, LV_PART_MAIN));
    CHECK(got == want);
    render(with_b);
    lv_obj_add_flag(b, LV_OBJ_FLAG_HIDDEN);
    render(without_b);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_HIDDEN);
    int x1 = W, y1 = H, x2 = -1, y2 = -1;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            if (with_b[y * W + x] != without_b[y * W + x]) {
                if (x < x1) x1 = x;
                if (x > x2) x2 = x;
                if (y < y1) y1 = y;
                if (y > y2) y2 = y;
            }
    CHECK(x2 >= 0);
    if (x2 < 0) return;
    CHECK(y1 >= SLAB_TOP + 2);
    const int cx[2] = { x1, x2 + 1 }, cy[2] = { y1, y2 + 1 };
    for (int i = 0; i < 4; i++) {
        const int dx = cx[i & 1] - W / 2, dy = cy[i >> 1] - H / 2;
        CHECK(dx * dx + dy * dy <= GLASS_R * GLASS_R);
    }
    const uint16_t bg = without_b[((y1 + y2) / 2) * W + (x1 + x2) / 2];
    int room = 0;
    while (room < 40 && ring_clear(x1, y1, x2, y2, room + 1, bg)) room++;
    CHECK(room >= ROOM);
    if (room < least_room) least_room = room;
    printf("  %-62s %3d %% %-6s %s  ink %d-%d, %d-%d  %2d px clear\n", where, st.batt, IS[got],
           got == want ? "  " : "!!", x1, x2, y1, y2, room);
    if (pic) picture(dir, pic);
}

/* A headset's or a speaker's battery at every charge, and in every state
 * of a transmitting face's slab: at rest, on the air, keyed by another,
 * not able to key; a headset muted, and asking for its boom up. Pictures:
 * the three colours, a speaker's, and on the air. */
SOME static void batt_radio(const char *dir, const char *face)
{
    char pic[64];
    st.speaker = false;
    where = "no battery until the device says it";
    st.headset = true; st.have_batt = false; st.batt = 80; run(60);
    CHECK(shown(LV_SYMBOL_BLUETOOTH) && !batt_shown());
    where = "...nor once it has gone";
    st.headset = false; st.have_batt = true; run(60);
    CHECK(!shown(LV_SYMBOL_BLUETOOTH) && !batt_shown());
    for (int spk = 0; spk < 2; spk++) {
        st.headset = !spk; st.speaker = spk; st.have_batt = true;
        where = spk ? "a speaker's battery, by its charge" : "a headset's battery, by its charge";
        for (int i = 0; i < NLEVELS; i++) {
            st.batt = (uint8_t)LEVELS[i]; run(60);
            batt_check(dir, NULL, false);
        }
        st.batt = spk ? 60 : 30;
        where = spk ? "...a speaker's, on the air: white on the red slab" : "...a headset's, on the air: white";
        st.tx = st.keyed = true; run(60);
        snprintf(pic, sizeof pic, "%s-batt-%s-tx", face, spk ? "speaker" : "headset");
        batt_check(dir, pic, true);
        where = "...TX  REMOTE";
        st.keyed = false; st.tx_remote = true; run(60);
        CHECK(shown("TX  REMOTE"));
        batt_check(dir, NULL, true);
        st.tx = st.tx_remote = false;
        where = "...----, not able to key";
        st.may_key = false; run(60);
        CHECK(shown("----"));
        batt_check(dir, NULL, false);
        st.may_key = true;
        if (!spk) {
            where = "...a headset muted";
            st.headset_muted = true; run(60);
            batt_check(dir, NULL, false);
            where = "...RAISE BOOM";
            st.headset_raise = true; run(60);
            CHECK(shown("RAISE BOOM"));
            snprintf(pic, sizeof pic, "%s-batt-raise-boom", face);
            batt_check(dir, pic, false);
            st.headset_muted = st.headset_raise = false;
        }
    }
    where = "the three colours";
    st.speaker = false; st.headset = true;
    static const struct { int pct; const char *name; } C[] = { { 80, "green" }, { 40, "yellow" }, { 10, "red" } };
    for (int i = 0; i < 3; i++) {
        st.batt = (uint8_t)C[i].pct; run(60);
        snprintf(pic, sizeof pic, "%s-batt-%s", face, C[i].name);
        batt_check(dir, pic, false);
    }
    st.headset = false; st.speaker = true; st.batt = 60; run(60);
    snprintf(pic, sizeof pic, "%s-batt-speaker", face);
    batt_check(dir, pic, false);
    st.speaker = false; st.have_batt = false; run(60);
}

/* ---- the knob's own battery, top centre (ui.c knob_batt_show) ---- */

#if VFO_RADIO_EPIBEAM
#define KNOB_INK_TOP  56                   /* inside the ring of directions, 48 at the top */
#define KNOB_INK_FOOT 92                   /* ...and over the caption's 99 */
#elif VFO_RADIO_ADSB
#define KNOB_INK_TOP  34                   /* under the range's label, the padlock's from 35 */
#define KNOB_INK_FOOT 61                   /* ...within the plate the map leaves clear */
#else
#define KNOB_INK_TOP  38                   /* its ink's rows within these: under the arc */
#define KNOB_INK_FOOT 58                   /* ...and over the S-units */
#endif
#if VFO_RADIO_SVXCONNECT || VFO_RADIO_PHONE
#define ARC_STAYS 1                        /* the reflector's arc: the audio's on the air too */
#else
#define ARC_STAYS 0
#endif

static int  knob_least_room = 99;
static char knob_said[96];

static lv_obj_t *knob_battery(void) { return battery_in(lv_screen_active(), false); }

/* Any ink of it on the face: none hidden, nor under a view over the whole
 * face. */
static bool knob_inked(void)
{
    lv_obj_t *b = knob_battery();
    box_t     k;
    int       room;
    return b && ink_of(b, &k, &room);
}

/* The knob's battery as the face shows it now, at st.knob_pct: its glyph and
 * colour, as a headset's; and its ink -- centred, its rows under the arc
 * and over the S-units, inside the glass's margin, nothing else's ink
 * within ROOM px. A line for each `where`; a picture where `pic` names one. */
static void knob_check(const char *dir, const char *pic)
{
    lv_obj_t *b = knob_battery();
    CHECK(b != NULL);
    if (!b) {
        printf("  [%s] no battery of the knob's\n", where);
        return;
    }
    CHECK(!strcmp(lv_label_get_text(b), glyph_for(st.knob_pct)));
    const int got = colour_is(lv_obj_get_style_text_color(b, LV_PART_MAIN));
    CHECK(got == colour_for(st.knob_pct));
    box_t k;
    int   room = 0;
    const bool inked = ink_of(b, &k, &room);
    CHECK(inked);
    if (!inked) {
        printf("  [%s] the knob's battery, covered\n", where);
        return;
    }
    CHECK(abs(k.x1 + k.x2 - W) <= 1);
    CHECK(k.y1 >= KNOB_INK_TOP && k.y2 <= KNOB_INK_FOOT);
    CHECK(in_glass(&k));
    CHECK(room >= ROOM);
    if (room < knob_least_room) knob_least_room = room;
    if (strcmp(knob_said, where) || pic) {
        snprintf(knob_said, sizeof knob_said, "%s", where);
        printf("  %-62s %3d %% %-6s ink %d-%d, %d-%d  %2d px clear\n", where, st.knob_pct, IS[got], k.x1,
               k.x2, k.y1, k.y2, room);
    }
    if (pic) picture(dir, pic);
}

/* ...at every charge. */
static void knob_levels(const char *dir)
{
    const uint8_t was = st.knob_pct;
    for (int i = 0; i < NLEVELS; i++) {
        st.knob_pct = (uint8_t)LEVELS[i]; run(60);
        knob_check(dir, NULL);
    }
    st.knob_pct = was; run(60);
}

/* A finger's swipe from (x0, y0) to (x1, y1), 16 px each 8 ms. */
SOME static void swipe(int x0, int y0, int x1, int y1)
{
    down(x0, y0);
    const int n = (abs(x1 - x0) > abs(y1 - y0) ? abs(x1 - x0) : abs(y1 - y0)) / 16;
    for (int i = 1; i <= n; i++) {
        fx = (uint16_t)(x0 + (x1 - x0) * i / n);
        fy = (uint16_t)(y0 + (y1 - y0) * i / n);
        run(8);
    }
    up(); run(50);
}

/* How far from the middle what the face draws with `o`, and not without
 * it, reaches: its farthest pixel, rounded corners as they are drawn. */
static float ink_reach(lv_obj_t *o)
{
    render(with_b);
    lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    render(without_b);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    float far = 0;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            if (with_b[y * W + x] != without_b[y * W + x]) {
                const float dx = x + 0.5f - W / 2, dy = y + 0.5f - H / 2, d = sqrtf(dx * dx + dy * dy);
                if (d > far) far = d;
            }
    return far;
}

/* The address card's text, as app_main.c gives it: the firmware, the
 * knob's power, its addresses -- the widest a LAN's address is in the
 * card's font (Montserrat 20: its 0, 4 and 8 the widest digits, its 1 the
 * narrowest; 192.168.254.248 is 285 px on the setup line, 208.208.208.208,
 * on no LAN, 294). */
#define CARD "Icom 1.18.4\nbattery 85 %\nUSB   -\nWiFi  192.168.254.248\nsetup  http://192.168.254.248"

/* A label shown, by the start of its text. */
static lv_obj_t *card_label(void) { return label_from(lv_screen_active(), "Icom 1.18.4"); }

/* The knob's battery on this face: none until its charge is known, or on
 * USB; through every charge; and over what comes up over the middle -- a
 * warning holding the card's five lines, the card itself, a chooser, a
 * question, VOLUME's panel -- following its charge with a chooser up. On a
 * radio's face, none on the air; on the reflector's, there still. Pictures:
 * at rest, with the card, and on the air. */
SOME static void knob_sweep(const char *dir, const char *face)
{
    char pic[64];
    const ui_state_t keep = st;

    where = "the knob's battery: none until its charge is known, nor on USB";
    st.knob_batt = false; st.knob_pct = 85; run(60);
    CHECK(!knob_battery() && !knob_inked());
    st.knob_batt = true;
    where = "the knob's battery, by its charge";
    knob_levels(dir);
    snprintf(pic, sizeof pic, "%s-knob-batt", face);
    knob_check(dir, pic);

    where = "...over a warning, the card's five lines in it";
    ui_set_netinfo(CARD);
    st.warn = "NO LINK"; run(60);
    knob_check(dir, NULL);
    {
        lv_obj_t *t = label_from(lv_screen_active(), "NO LINK"), *n = card_label();
        CHECK(t && n);
        if (t && n) {
            lv_area_t at, an, ap;
            lv_obj_get_coords(t, &at);
            lv_obj_get_coords(n, &an);
            lv_obj_get_coords(lv_obj_get_parent(n), &ap);
            CHECK(an.y1 >= at.y2 + 2);              /* under the warning */
            CHECK(an.y2 <= ap.y2 - 4);              /* inside the panel */
            printf("  ...the warning's lines %d-%d, under its title's %d, in its panel's %d\n",
                   (int)an.y1, (int)an.y2, (int)at.y2, (int)ap.y2);
        }
    }
    snprintf(pic, sizeof pic, "%s-knob-batt-warning", face);
    picture(dir, pic);
    st.warn = NULL; run(60);

    where = "...under the address card, held up on the S-meter";
    down(W / 2, 66); run(700); up(); run(SEEN);
    {
        lv_obj_t *n = card_label();
        CHECK(n != NULL);
        knob_check(dir, NULL);
        if (n) {
            /* The card: its five lines, the widest address, and its rounded
             * corners all inside the glass's margin. */
            lv_area_t a;
            lv_obj_get_coords(n, &a);
            const float r = ink_reach(n);
            CHECK(r <= GLASS_R);
            printf("  ...the card %d-%d, %d-%d: its ink %.1f px from the middle at the most\n", (int)a.x1,
                   (int)a.x2, (int)a.y1, (int)a.y2, (double)r);
            snprintf(pic, sizeof pic, "%s-knob-batt-card", face);
            picture(dir, pic);
        }
    }
    tap_at(W / 2, 174, 80); settle(); quiet();
    CHECK(!card_label());

    where = "...over a chooser, and following its charge under it";
    {
        static const char T[2][12] = { "DIAL FOR", "STATION" };
        static const char N[2][24] = { "thinkstation", "OWN" };
        ui_ask_choice(T, N, 2, 0); run(60);
        CHECK(ui_choice_active());
        knob_check(dir, NULL);
        st.knob_pct = 30; run(60);
        knob_check(dir, NULL);
        st.knob_batt = false; run(60);
        CHECK(!knob_battery());
        st.knob_batt = true; st.knob_pct = 85; run(60);
        ui_ask_choice(NULL, NULL, 0, 0); run(60);
        CHECK(!ui_choice_active());
    }

    where = "...over a question";
    CHECK(ui_ask_turn("FIRMWARE?", "turn the knob for the picker\ntap to cancel; WiFi is kept"));
    run(60);
    knob_check(dir, NULL);
    run(1000);
    tap_at(OUTSIDE_X, OUTSIDE_Y, 80); settle(); quiet();
    CHECK(ui_take_update_answer() == -1);

    where = "...over VOLUME's panel, the dial turned";
    ui_volume_turn(1); run(60);
    CHECK(ui_edit_active());
    knob_check(dir, NULL);
    run(2100);
    CHECK(!ui_edit_active());

    where = ARC_STAYS ? "...on the air: the reflector's arc, and the battery, as they were"
                      : "...on the air: none, the transmit scale's numbers where it was";
    st.tx = st.keyed = true; run(60);
    if (ARC_STAYS) knob_check(dir, NULL);
    else           CHECK(!knob_battery() && !knob_inked());
    snprintf(pic, sizeof pic, "%s-knob-batt-tx", face);
    picture(dir, pic);
    st.keyed = false; st.tx_remote = true; run(60);
    if (ARC_STAYS) knob_check(dir, NULL);
    else           CHECK(!knob_battery());
    st.tx = st.tx_remote = false; run(60);
    where = "...and back in receive";
    knob_check(dir, NULL);

    st = keep;
    st.knob_batt = false; run(60);
    CHECK(!knob_battery());
}

#if !VFO_RADIO_SETUP
/* ---- the lock (ui.c klock_*): the address card's LOCK ---- */
#define LOCK_X 180                         /* the pill: ui.c KLOCK_BTN_Y 62, 44 high */
#define LOCK_Y 84
#if VFO_RX_ONLY || VFO_RADIO_PHONE
#define LOCK_KEYS 0                        /* no PTT on this slab */
#else
#define LOCK_KEYS 1
#endif

/* The padlock at the top of the arc, shown: the font's lock glyph on the
 * face itself, above the S-units -- not the pill's, nor the reflector's
 * talkgroup padlock further down. */
static lv_obj_t *padlock(void)
{
    lv_obj_t *scr = lv_screen_active();
    for (uint32_t i = 0; i < lv_obj_get_child_count(scr); i++) {
        lv_obj_t *o = lv_obj_get_child(scr, (int32_t)i);
        if (lv_obj_check_type(o, &lv_label_class) && visible(o) && lv_obj_get_y(o) < 56 &&
            !strcmp(lv_label_get_text(o), "\xEF\x80\xA3"))
            return o;
    }
    return NULL;
}
static int padlock_colour(void)
{
    lv_obj_t *l = padlock();
    return l ? colour_is(lv_obj_get_style_text_color(l, 0)) : IS_OTHER;
}

/* The card up under a finger held on the S-meter, and the pill with it. */
static void card_up(void) { down(W / 2, 66); run(700); up(); run(SEEN); }

/* Locked: the card's pill, LOCK, takes the tap once it has been up a
 * moment, not in its first; the padlock comes, amber; the glass opens
 * nothing -- the frequency, AGC, the swipes -- and keys nothing, the
 * padlock red a moment for each; on the air the slab still unkeys, and the
 * telephone's hangs up or declines, never answers. With
 * no link, the pill over the warning, to unlock there. The padlock beside
 * the knob's own battery, clear of it; a chooser puts the card away, and
 * the pill with it. UNLOCK,
 * and the face as it was. Pictures: the card's pill, the face locked, the
 * warning, the padlock beside the battery. */
SOME static void lock_sweep(const char *dir, const char *face)
{
    char pic[64];
    const ui_state_t keep = st;
    ui_set_netinfo(CARD);
    settle(); quiet();

    where = "the lock: none at first";
    CHECK(!ui_locked() && !padlock() && !shown("LOCK"));

    where = "...the card up, and its pill: LOCK";
    card_up();
    CHECK(card_label() && shown("LOCK"));
    snprintf(pic, sizeof pic, "%s-lock-card", face);
    picture(dir, pic);
    where = "...a tap on it in its first moments: nothing";
    tap_at(LOCK_X, LOCK_Y, 60);
    CHECK(!ui_locked());
    run(700);
    where = "...a tap once it has been up a moment: locked";
    tap_at(LOCK_X, LOCK_Y, 60); run(60);
    CHECK(ui_locked() && shown("UNLOCK") && padlock());
    CHECK(padlock_colour() == IS_YELLOW);
    printf("  ...the padlock %s\n", IS[padlock_colour()]);
    where = "...the card goes by itself; the pill with it";
    run(10500);
    CHECK(!card_label() && !shown("UNLOCK") && padlock());
    snprintf(pic, sizeof pic, "%s-locked", face);
    picture(dir, pic);

    where = "...a tap on the frequency: nothing, the padlock red";
    quiet();
    tap_at(PANEL_X, PANEL_Y, 80);
    CHECK(!ui_edit_active() && !ptt());
    CHECK(padlock_colour() == IS_RED);
    run(900);
    CHECK(padlock_colour() == IS_YELLOW);
    where = "...a tap on AGC's place: nothing";
    tap_at(W / 2 - 80, 80, 80); run(60);
    CHECK(!ui_edit_active());
    where = "...swipes, from either side and from the top: nothing";
    swipe(340, 180, 140, 180); run(60);
    CHECK(!ui_edit_active());
    swipe(20, 180, 220, 180); run(60);
    CHECK(!ui_edit_active());
    swipe(180, 20, 180, 220); run(60);
    CHECK(!ui_edit_active());
    where = "...a tap and a hold on the slab: no key, no antennas";
    tap_at(SLAB_X, SLAB_Y, 80); run(60);
    CHECK(!ptt());
    down(SLAB_X, SLAB_Y); run(900); up(); run(SEEN); settle();
    CHECK(!ptt() && !buzz() && !ui_edit_active());
#if LOCK_KEYS
    where = "...on the air: a tap on the slab unkeys";
    st.keyed = true; run(400); quiet();
    tap_at(SLAB_X, SLAB_Y, 80); run(60);
    CHECK(ptt());
    st.keyed = false; settle(); quiet();
#endif
#if VFO_RADIO_PHONE
    where = "...a call up: a tap on the slab hangs up";
    st.call = 3; run(400); quiet();
    tap_at(SLAB_X, SLAB_Y, 80); run(60);
    CHECK(ptt());
    where = "...a call ringing in: a tap on either half declines it";
    st.call = 2; run(400); quiet(); (void)ui_take_call_req();
    tap_at(SLAB_X + 60, SLAB_Y, 80); run(60);
    CHECK(ui_take_call_req() == 2 && !ptt());
    st.call = 0; settle(); quiet(); (void)ui_take_call_req();
#endif

    where = "...the knob's battery: the padlock left of it, clear of it";
    st.knob_batt = true; st.knob_pct = 85; run(60);
    {
        lv_obj_t *l = padlock(), *b = knob_battery();
        CHECK(l && b);
        if (l && b) {
            lv_area_t al, ab;
            lv_obj_get_coords(l, &al);
            lv_obj_get_coords(b, &ab);
            CHECK(al.x2 + ROOM <= ab.x1);
            CHECK(al.y1 >= 30 && al.y2 <= 60);
            printf("  ...the padlock %d-%d, %d-%d; the battery from %d\n", (int)al.x1, (int)al.x2,
                   (int)al.y1, (int)al.y2, (int)ab.x1);
        }
    }
    snprintf(pic, sizeof pic, "%s-locked-batt", face);
    picture(dir, pic);
    st.knob_batt = false; run(60);

    where = "...no link: the pill over the warning, to unlock there";
    st.warn = "NO LINK"; st.link_ok = false; run(100);
    CHECK(shown("UNLOCK"));
    snprintf(pic, sizeof pic, "%s-locked-warning", face);
    picture(dir, pic);
    st.warn = NULL; st.link_ok = true; run(100);
    CHECK(!shown("UNLOCK"));

    where = "...a chooser over the card: no pill under it";
    card_up();
    CHECK(shown("UNLOCK"));
    {
        static const char T[2][12] = { "DIAL FOR", "STATION" };
        static const char N[2][24] = { "thinkstation", "OWN" };
        ui_ask_choice(T, N, 2, 0); run(60);
        CHECK(ui_choice_active() && !shown("UNLOCK"));
        ui_ask_choice(NULL, NULL, 0, 0); run(60);
    }
    CHECK(!card_label() && !shown("UNLOCK"));   /* it put the card away */

    where = "...UNLOCK: the face as it was";
    card_up(); run(700);
    tap_at(LOCK_X, LOCK_Y, 60); run(60);
    CHECK(!ui_locked() && !padlock() && shown("LOCK"));
    tap_at(W / 2, 174, 80); settle(); quiet();
    CHECK(!card_label() && !shown("LOCK"));
    st = keep;
    run(100);
}
#endif

/* ...and on a radio's face, or the receiver's, over the S-units at their
 * widest and at nothing, the bar and its peak at the top of the arc, right
 * over it, a web SDR's line outside the arc and its reading, and a device's
 * battery on the slab beside it. */
SOME static void knob_radio(const char *dir, const char *face)
{
    char pic[64];
    const ui_state_t keep = st;
    st.knob_batt = true; st.knob_pct = 60;
    where = "...over the S-units at their widest, S9+60";
    st.smeter_dbm = -13.0f; run(1500);
    knob_levels(dir);
    where = "...the bar and its peak at the top of the arc";
    st.smeter_dbm = -82.0f; run(3000);
    knob_levels(dir);
    snprintf(pic, sizeof pic, "%s-knob-batt-peak", face);
    knob_check(dir, pic);
    where = "...over nothing on the S-meter";
    st.smeter_dbm = -127.0f; run(3000);
    knob_check(dir, NULL);
#if VFO_HAS_SDR
    where = "...a web SDR's line outside the arc, its reading under the S-units";
    st.n_sdr = 1;
    strcpy(st.sdr_name[0], "Web-888");
    st.rxsrc = 0; st.sdr_streaming = true; st.sdr_dbm = -82.0f; run(1500);
    knob_levels(dir);
    st.rxsrc = -1; st.sdr_streaming = false; st.n_sdr = 0; run(60);
#endif
    where = "...a headset's battery on the slab beside it";
    st.headset = true; st.have_batt = true; st.batt = 40; run(60);
    knob_levels(dir);
    batt_check(dir, NULL, false);
    snprintf(pic, sizeof pic, "%s-knob-batt-headset", face);
    knob_check(dir, pic);
    st = keep; run(60);
}

/* ---- a web SDR beside the radio ---- */
#if VFO_HAS_SDR

/* Its colour on the face (ui.c's SDR_HEX), and the amber of a word for why
 * it is not heard (C_WARN): each face's own. */
#if VFO_RADIO_ICOM
#define SDR_HEX  0x5A9BFF
#define WARN_HEX 0xFFB000
#elif VFO_RADIO_XIEGU
#define SDR_HEX  0x4DA6FF
#define WARN_HEX 0xFFD000
#elif VFO_RADIO_UBERSDR
#define SDR_HEX  0x8B7CF8
#define WARN_HEX 0xF2B544
#elif VFO_RADIO_KIWI
#define SDR_HEX  0x99C9FF
#define WARN_HEX 0xFFA500
#else
#define SDR_HEX  0x62BBFF
#define WARN_HEX 0xFFB000
#endif

/* A visible label with this text on the row of the readings under the
 * S-units (y 97). */
static lv_obj_t *on_the_row(lv_obj_t *o, const char *text)
{
    if (lv_obj_check_type(o, &lv_label_class) && visible(o) && !strcmp(lv_label_get_text(o), text)) {
        lv_area_t a;
        lv_obj_get_coords(o, &a);
        if (a.y1 <= 97 && a.y2 >= 97) return o;
    }
    for (uint32_t i = 0; i < lv_obj_get_child_count(o); i++) {
        lv_obj_t *f = on_the_row(lv_obj_get_child(o, (int32_t)i), text);
        if (f) return f;
    }
    return NULL;
}

/* The SDR's colour on its thin line, just outside the radio's S-meter. */
static int sdr_line_px(void)
{
    lv_obj_invalidate(lv_screen_active());
    lv_refr_now(disp);
    const uint16_t c = lv_color_to_u16(lv_color_hex(SDR_HEX));
    int n = 0;
    for (int i = 0; i < W * H; i++) {
        if (fb[i] != c) continue;
        const double r = hypot(i % W + 0.5 - W / 2.0, i / W + 0.5 - H / 2.0);
        n += r >= 171.0 && r <= 178.5;
    }
    return n;
}

/* Playing, the SDR's line and its S-units in its colour, under the radio's;
 * a dial it cannot reach, the line gone and "can't reach" in amber in their
 * place -- clear of the AGC and the gain either side at their widest. */
static void sdr_reading(const char *dir, const char *face, const char *agc_wide, const char *gain_wide)
{
    char name[64];
    st.n_sdr = 1;
    strcpy(st.sdr_name[0], "KiwiSDR");
    st.rxsrc = 0;
    st.sdr_streaming = true;
    st.sdr_dbm = -97.0f;
    run(1200);
    where = "a web SDR playing: its line, its S-units";
    CHECK(sdr_line_px() > 0);
    CHECK(on_the_row(lv_screen_active(), "S5") != NULL);
    snprintf(name, sizeof name, "%s-sdr", face);
    picture(dir, name);
    where = "a web SDR that cannot reach the dial: its line gone, \"can't reach\" in amber";
    st.sdr_streaming = false;
    st.sdr_trouble = true;
    strcpy(st.sdr_note, "can't reach");
    run(200);
    CHECK(sdr_line_px() == 0);
    lv_obj_t *r = on_the_row(lv_screen_active(), "can't reach");
    lv_obj_t *a = on_the_row(lv_screen_active(), agc_wide), *g = on_the_row(lv_screen_active(), gain_wide);
    CHECK(r && a && g);
    if (r && a && g) {
        lv_area_t cr, ca, cg;
        lv_obj_get_coords(r, &cr);
        lv_obj_get_coords(a, &ca);
        lv_obj_get_coords(g, &cg);
        printf("%s: \"can't reach\" %d px wide (x %d..%d), %s ends at x %d, %s starts at x %d\n", face,
               (int)lv_area_get_width(&cr), (int)cr.x1, (int)cr.x2, agc_wide, (int)ca.x2, gain_wide, (int)cg.x1);
        CHECK(cr.x1 > ca.x2 + 4 && cr.x2 < cg.x1 - 4);
        CHECK(lv_color_to_u16(lv_obj_get_style_text_color(r, LV_PART_MAIN)) ==
              lv_color_to_u16(lv_color_hex(WARN_HEX)));
    }
    snprintf(name, sizeof name, "%s-sdr-out-of-range", face);
    picture(dir, name);
    st.rxsrc = -1;
    st.n_sdr = 0;
    st.sdr_trouble = false;
    st.sdr_note[0] = 0;
    run(200);
}
#endif /* VFO_HAS_SDR */

#if VFO_RX_ONLY
/* ---- a receiver's slab, the UberSDR's or Kiwi888's: its line in its room
 * (ui.c call_place) ---- */

/* What an UberSDR's time left for a guest says for s seconds: whole
 * minutes, from a hundred of them hours, the seconds too in the last five;
 * an idle limit's last minute. */
static void left_says(char *out, size_t cap, int s, bool idle)
{
    if (idle)          snprintf(out, cap, "idle %d:%02d", s / 60, s % 60);
    else if (s < 300)  snprintf(out, cap, "%d:%02d", s / 60, s % 60);
    else if (s < 6000) snprintf(out, cap, "%d min", s / 60);
    else               snprintf(out, cap, "%d h %02d", s / 3600, s / 60 % 60);
}

/* A visible label saying exactly this. */
static lv_obj_t *label_is(lv_obj_t *o, const char *text)
{
    if (lv_obj_check_type(o, &lv_label_class) && visible(o) && !strcmp(lv_label_get_text(o), text)) return o;
    for (uint32_t i = 0; i < lv_obj_get_child_count(o); i++) {
        lv_obj_t *l = label_is(lv_obj_get_child(o, (int32_t)i), text);
        if (l) return l;
    }
    return NULL;
}

/* The label of the spot's call that says `says`: found by its start, which
 * dots never cut -- a frequency's five, so as not to be the line under it. */
static lv_obj_t *call_label(const char *says)
{
    char prefix[6];
    snprintf(prefix, sizeof prefix, "%.*s", says[0] >= '0' && says[0] <= '9' ? 5 : 3, says);
    return label_from(lv_screen_active(), prefix);
}

static int text_w(const char *t, const lv_font_t *f)
{
    lv_point_t p;
    lv_text_get_size(&p, t, f, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return p.x;
}

/* The widest the time left says in a kind of its own -- 0 whole minutes, 1
 * the last five, 2 idle, 3 hours to 99 -- measured over all it can say
 * there, once; and a digit's, the widest. */
static int widest_of(int kind)
{
    static int w[5] = { -1, -1, -1, -1, -1 };
    if (w[kind] >= 0) return w[kind];
    char t[24];
    w[kind] = 0;
    if (kind == 4) {
        for (char d = '0'; d <= '9'; d++) {
            const char one[2] = { d, 0 };
            if (text_w(one, &lv_font_montserrat_14) > w[4]) w[4] = text_w(one, &lv_font_montserrat_14);
        }
        return w[4];
    }
    const int from = kind == 0 ? 300 : kind == 3 ? 6000 : 0, to = kind == 0 ? 6000 : kind == 1 ? 300 : kind == 2 ? 61
                                                                              : 100 * 3600;
    for (int s = from; s < to; s += kind == 3 ? 60 : 1) {
        left_says(t, sizeof t, s, kind == 2);
        const int tw = text_w(t, &lv_font_montserrat_14);
        if (tw > w[kind]) w[kind] = tw;
    }
    return w[kind];
}

/* What the time left keeps from the call: the widest it can say as it says
 * it now -- the hours, a widest digit more for each beyond two. */
static int left_keeps(void)
{
    if (st.left_idle) return widest_of(2);
    if (st.left_s < 300) return widest_of(1);
    if (st.left_s < 6000) return widest_of(0);
    int w = widest_of(3);
    for (int h = st.left_s / 3600; h >= 100; h /= 10) w += widest_of(4);
    return w;
}

/* Where the device's logo's ink begins, a headset's or a speaker's: drawn
 * once for each. */
static int logo_ink(lv_obj_t *logo)
{
    static int at[2] = { -1, -1 };
    const int k = st.speaker ? 1 : 0;
    box_t b;
    int room;
    if (at[k] < 0 && ink_of(logo, &b, &room)) at[k] = b.x1;
    return at[k];
}

static int calls_centred, calls_moved, calls_cut, call_least_l = 999, call_most_r = -1;

/* The spot's call, saying `says` -- or Kiwi888's receiver's name -- where
 * the face puts it (ui.c call_place): its room from 4 px past what the time
 * left keeps (`left`, its label, when it shows), to 4 px short of the
 * device's battery, or its logo's ink -- else the slab's 290 px. In it:
 * centred where it fits so, and whole; else whole, its box its text's,
 * moved aside just as far as it must, against the end it moved from; only
 * one too long for the whole room cut with dots, the room its box. On one
 * line, always. */
static void call_check(const char *says, lv_obj_t *left)
{
    lv_obj_t *c = call_label(says);
    CHECK(c != NULL);
    if (!c) return;
    const int l = left ? lv_obj_get_x(left) + left_keeps() + 4 : W / 2 - 145;
    lv_obj_t *b = battery(lv_screen_active());
    lv_obj_t *logo = !st.headset && !st.speaker ? NULL
                     : label_is(lv_screen_active(), st.speaker ? LV_SYMBOL_VOLUME_MAX : LV_SYMBOL_BLUETOOTH);
    const int r = b ? lv_obj_get_x(b) - 4 : logo ? logo_ink(logo) - 4 : W / 2 + 145;
    const int tw = text_w(says, &lv_font_montserrat_28);
    const int x = lv_obj_get_x(c), w = lv_obj_get_width(c);
    const int half = W / 2 - l < r - W / 2 ? W / 2 - l : r - W / 2;
    const bool cut = strstr(lv_label_get_text(c), "...") != NULL;
    CHECK(x >= l && x + w <= r);
    CHECK(lv_obj_get_height(c) == lv_font_get_line_height(&lv_font_montserrat_28));
    if (tw <= 2 * half) {
        CHECK(!cut && abs(2 * x + w - W) <= 1);
        calls_centred++;
    } else if (tw <= r - l) {
        CHECK(!cut && w == tw);
        CHECK((x == l && W / 2 - tw / 2 < l) || (x + w == r && W / 2 - tw / 2 > r - tw));
        calls_moved++;
    } else {
        CHECK(cut && x == l && x + w == r);
        calls_cut++;
    }
    if (x < call_least_l) call_least_l = x;
    if (x + w > call_most_r) call_most_r = x + w;
}
#endif

#if VFO_RADIO_MULTIFLEX
static void radio(void)
{
    base();
    strcpy(st.agc, "med");
    st.have_gain = true; st.gain = 8; st.gain_min = -8; st.gain_max = 32; st.gain_step = 8;
    st.has_tune = true;
    st.has_memories = true;
    st.mem_all = true;
    st.n_ant = 6; st.ant = 0; st.have_ant = true;
    strcpy(st.ant_names, "ANT1,ANT2,RX_A,RX_B,XVTA,XVTB");
    st.n_tx_ant = 4; st.tx_ant = 0; st.have_tx_ant = true;
    strcpy(st.tx_ant_names, "ANT1,ANT2,XVTA,XVTB");
}

static void runs(const char *dir)
{
    ui_commit_t c;

    where = "a tap keys once the finger has stayed off";
    radio(); settle(); quiet();
    down(SLAB_X, SLAB_Y); run(120);
    CHECK(!ptt());
    up(); run(KEY_SOONEST - 10);
    CHECK(!ptt());                                 /* the glass may give it back yet */
    run(KEY_LATEST - KEY_SOONEST + 10);
    CHECK(ptt());
    CHECK(!ui_edit_active());
    CHECK(!buzz());
    settle(); quiet();

    where = "every tap alike, a flick or a slow one: keyed that moment after the lift";
    {
        static const uint32_t TAPS[] = { 20, 60, 100, 200, 300, 480 };
        for (size_t i = 0; i < sizeof TAPS / sizeof *TAPS; i++) {
            const seg_t s[] = { { 0, TAPS[i], SLAB_X, SLAB_Y } };
            const seen_t r = timeline(s, 1, TAPS[i] + 400);
            CHECK(r.keys == 1 && !r.buzz && !r.opened);
            CHECK(r.key_at >= (int32_t)(TAPS[i] + KEY_SOONEST) &&
                  r.key_at <= (int32_t)(TAPS[i] + KEY_LATEST));
            after();
        }
    }

    where = "held: the receive antenna, a buzz, no key";
    down(SLAB_X, SLAB_Y); run(480);
    CHECK(!ui_edit_active());
    run(40);
    CHECK(ui_edit_active());
    CHECK(buzz());
    CHECK(shown("RX ANT") && shown("ANT1"));
    picture(dir, "flex-rx-ant");
    run(300); up(); run(200);
    CHECK(!ptt());
    CHECK(ui_edit_active());

    where = "turned, and tapped on its panel: on to the transmit antenna";
    ui_edit_rotate(2); run(30);
    CHECK(shown("RX_A"));
    CHECK(!commit(&c));                            /* not live: a tap commits */
    tap_at(PANEL_X, PANEL_Y, 80); run(20);
    CHECK(commit(&c) && c.have_ant && c.ant == 2 && !c.ant_rx && !c.have_tx_ant);
    CHECK(shown("TX ANT") && shown("ANT1"));
    picture(dir, "flex-tx-ant");
    ui_edit_rotate(1); run(30);
    CHECK(shown("ANT2"));
    tap_at(PANEL_X, PANEL_Y, 80); run(20);
    CHECK(commit(&c) && c.have_tx_ant && c.tx_ant == 1 && !c.have_ant);
    where = "...and then nothing: not V/M, though the radio has memories";
    CHECK(!ui_edit_active());
    CHECK(!ptt());
    settle(); quiet();

    where = "a tap straight through: the antenna left alone";
    down(SLAB_X, SLAB_Y); run(600); up(); run(10);
    CHECK(buzz());
    tap_at(PANEL_X, PANEL_Y, 80); run(20);
    CHECK(commit(&c) && !c.have_ant);
    CHECK(shown("TX ANT"));
    where = "a tap outside the panel closes, untouched";
    tap_at(OUTSIDE_X, OUTSIDE_Y, 80); run(20);
    CHECK(!ui_edit_active());
    CHECK(!commit(&c));
    CHECK(!ptt());
    settle(); quiet();

    where = "...on the slab too: it closes the editor, and keys nothing";
    down(SLAB_X, SLAB_Y); run(600); up(); run(200);
    CHECK(ui_edit_active() && buzz());
    tap_at(SLAB_X, SLAB_Y, 100); run(300);
    CHECK(!ui_edit_active());
    CHECK(!ptt());
    CHECK(!commit(&c));
    settle(); quiet();

    where = "the glass loses the finger as it lands: the hold goes on";
    down(SLAB_X, SLAB_Y); run(40); up(); run(60);
    CHECK(!ptt());
    down(SLAB_X, SLAB_Y); run(300);
    CHECK(!ptt());
    run(130);                                      /* 530 ms from the first touch */
    CHECK(ui_edit_active() && shown("RX ANT"));
    CHECK(buzz());
    up(); run(300);
    CHECK(!ptt());
    tap_at(OUTSIDE_X, OUTSIDE_Y, 80); settle(); quiet();

    where = "...and mid-hold";
    down(SLAB_X, SLAB_Y); run(300); up(); run(50);
    CHECK(!ptt());
    down(SLAB_X, SLAB_Y); run(170);
    CHECK(ui_edit_active() && buzz());
    up(); run(300);
    CHECK(!ptt());
    tap_at(OUTSIDE_X, OUTSIDE_Y, 80); settle(); quiet();

    where = "a flick, no finger back: keyed once it has stayed off";
    down(SLAB_X, SLAB_Y); run(40); up(); run(SEEN);
    CHECK(!ptt());
    run(100);
    CHECK(!ptt());
    run(60);
    CHECK(ptt());
    CHECK(!ui_edit_active());
    settle(); quiet();

    where = "a slow tap: keyed once the finger has stayed off";
    down(SLAB_X, SLAB_Y); run(320); up(); run(SEEN);
    CHECK(!ptt());
    run(170);
    CHECK(ptt());
    settle(); quiet();

    where = "the glass loses a held finger at any moment, for up to 130 ms: never a key";
    {
        static const uint32_t AT[]  = { 30, 90, 120, 150, 200, 240, 300, 450, 480 };
        static const uint32_t FOR[] = { 35, 70, 105, 130 };
        for (size_t i = 0; i < sizeof AT / sizeof *AT; i++)
            for (size_t k = 0; k < sizeof FOR / sizeof *FOR; k++) {
                const seg_t s[] = { { 0, AT[i], SLAB_X, SLAB_Y },
                                    { AT[i] + FOR[k], 1000, SLAB_X, SLAB_Y } };
                const seen_t r = timeline(s, 2, 1400);
                CHECK(r.keys == 0 && r.buzz && r.open_at_end);
                after();
            }
    }

    where = "...and after the buzz, once or twice: the antennas stay up";
    {
        const seg_t once[] = { { 0, 670, SLAB_X, SLAB_Y }, { 730, 980, SLAB_X + 3, SLAB_Y - 2 } };
        seen_t r = timeline(once, 2, 1400);
        CHECK(r.keys == 0 && r.buzz && r.open_at_end && shown("RX ANT"));
        after();
        const seg_t twice[] = { { 0, 600, SLAB_X, SLAB_Y }, { 650, 800, SLAB_X, SLAB_Y },
                                { 860, 1000, SLAB_X - 4, SLAB_Y + 3 } };
        r = timeline(twice, 3, 1400);
        CHECK(r.keys == 0 && r.buzz && r.open_at_end && shown("RX ANT"));
        after();
    }

    where = "one read misplaced as the half second passes: the antennas a read later, no key";
    for (uint32_t g = 470; g <= 530; g += 2) {
        const seg_t s[] = { { 0, g, SLAB_X, SLAB_Y }, { g, g + 10, SLAB_X + 30, SLAB_Y },
                            { g + 10, 900, SLAB_X + 2, SLAB_Y } };
        const seen_t r = timeline(s, 3, 1300);
        CHECK(r.keys == 0 && r.buzz && r.open_at_end);
        after();
    }
    {
        /* ...or for 40 ms, and back: no key either. */
        const seg_t s[] = { { 0, 470, SLAB_X, SLAB_Y }, { 470, 510, SLAB_X + 30, SLAB_Y },
                            { 510, 900, SLAB_X + 2, SLAB_Y } };
        const seen_t r = timeline(s, 3, 1300);
        CHECK(r.keys == 0 && r.buzz && r.open_at_end);
        after();
    }

    where = "a press that wandered off and stays off: neither key nor antennas";
    {
        const seg_t s[] = { { 0, 300, SLAB_X, SLAB_Y }, { 300, 900, SLAB_X + 40, SLAB_Y } };
        const seen_t r = timeline(s, 2, 1300);
        CHECK(r.keys == 0 && !r.buzz && !r.opened);
        after();
    }

    where = "the finger given back just above the slab: the same press";
    {
        /* A tap of the slab's edge, the glass losing it as it landed. */
        const seg_t s[] = { { 0, 40, SLAB_X, 251 }, { 80, 300, SLAB_X, 245 } };
        seen_t r = timeline(s, 2, 300);
        CHECK(r.keys == 0 && !r.opened);           /* nothing while the finger is down */
        r = timeline(s, 0, KEY_LATEST + 20);
        CHECK(r.keys == 1 && !r.opened);           /* ...a tap as it lifts, not RIT */
        after();
        /* ...and held there: the antennas. */
        const seg_t held[] = { { 0, 40, SLAB_X, 251 }, { 80, 700, SLAB_X, 245 } };
        r = timeline(held, 2, 1100);
        CHECK(r.keys == 0 && r.buzz && r.open_at_end && shown("RX ANT"));
        after();
    }

    where = "a finger down elsewhere before a slab tap has settled: no key, and nothing else";
    {
        const seg_t s[] = { { 0, 60, SLAB_X, SLAB_Y }, { 120, 220, 150, 222 } };   /* RIT's */
        const seen_t r = timeline(s, 2, 700);
        CHECK(r.keys == 0 && !r.opened);
        after();
    }

    where = "two taps in a row: two keys";
    tap_at(SLAB_X, SLAB_Y, 120);
    run(KEY_LATEST);
    CHECK(ptt());
    run(30);
    tap_at(SLAB_X, SLAB_Y, 120);
    run(KEY_LATEST);
    CHECK(ptt());
    settle(); quiet();

    where = "our PTT keyed while a tap waits (the headset's button): the tap toggles, unkeying";
    tap_at(SLAB_X, SLAB_Y, 120);
    run(60);
    st.keyed = true; st.tx = true;
    run(KEY_LATEST);
    CHECK(ptt());
    st.keyed = false; st.tx = false;
    settle(); quiet();

    where = "another station on the air while a tap waits: it keys nothing";
    tap_at(SLAB_X, SLAB_Y, 120);
    run(60);
    st.tx = true;
    run(KEY_LATEST);
    CHECK(!ptt());
    st.tx = false;
    settle(); quiet();

    where = "a second tap just after the first has keyed: it unkeys";
    {
        int keys = 0;
        uint32_t keyed_at = 0;
        for (uint32_t t = 0; t < 900; t++) {
            /* two taps of 120 ms, the second 160 ms after the first lifts */
            if (t < 120 || (t >= 280 && t < 400)) down(SLAB_X, SLAB_Y);
            else                                   up();
            run(1);
            if (ptt() && !keys++) keyed_at = t;
            /* ...and the knob's PTT keyed a moment after the first key, as
             * the ui task and the radio make it */
            if (keys && t == keyed_at + 30) { st.keyed = true; st.tx = true; }
        }
        CHECK(keys == 2);
        st.keyed = false; st.tx = false;
        settle(); quiet();
    }

    where = "...closer than the glass's gap: one press, keyed once -- or, held long, the antennas";
    {
        const seg_t quick[] = { { 0, 60, SLAB_X, SLAB_Y }, { 160, 280, SLAB_X, SLAB_Y } };
        seen_t r = timeline(quick, 2, 700);
        CHECK(r.keys == 1 && !r.opened);
        after();
        const seg_t slow[] = { { 0, 300, SLAB_X, SLAB_Y }, { 400, 520, SLAB_X, SLAB_Y } };
        r = timeline(slow, 2, 900);
        CHECK(r.keys == 0 && r.buzz && r.open_at_end);
        after();
    }

    where = "a swipe from the slab: no key, no antennas";
    down(SLAB_X, SLAB_Y);
    for (int y = SLAB_Y; y > 120; y -= 12) { fy = (uint16_t)y; run(10); }
    up(); run(400);
    CHECK(!ptt());
    CHECK(!buzz());
    settle(); quiet();

    where = "on the air: a touch unkeys at once, and no hold opens anything";
    st.tx = true; st.keyed = true; run(60);
    down(SLAB_X, SLAB_Y); run(SEEN);
    CHECK(ptt());
    run(800);
    CHECK(!ui_edit_active());
    CHECK(!buzz());
    up(); run(300);
    CHECK(!ptt());
    st.tx = false; st.keyed = false; settle(); quiet();

    where = "no link: the slab is nothing";
    st.link_ok = false; run(60);
    down(SLAB_X, SLAB_Y); run(700); up(); run(300);
    CHECK(!ptt() && !buzz() && !ui_edit_active());
    st.link_ok = true; settle(); quiet();

    where = "the swipe down: the antennas in its row, then V/M";
    down(180, 70);
    for (int y = 70; y < 230; y += 16) { fy = (uint16_t)y; run(8); }
    up(); run(50);
    CHECK(ui_edit_active() && shown("RX ANT"));
    CHECK(!buzz());
    tap_at(PANEL_X, PANEL_Y, 80); run(20);
    CHECK(shown("TX ANT"));
    tap_at(PANEL_X, PANEL_Y, 80); run(20);
    CHECK(shown("V/M"));
    picture(dir, "flex-vm");
    tap_at(OUTSIDE_X, OUTSIDE_Y, 80); settle(); quiet();

    where = "the memory face: no group to choose";
    st.mem_state = 2; st.mem_ch = 7; st.freq_hz = 29620000; st.mode = "fm";
    strcpy(st.mem_name, "ON0TEN"); st.mem_duplex = -1; st.mem_offset_hz = 100000; st.mem_tone_dhz = 797;
    st.filt_lo = -8000; st.filt_hi = 8000;
    run(120);
    CHECK(shown("ON0TEN"));
    CHECK(shown("M07  29.620  -0.1  T79.7"));
    CHECK(shown("10m"));
    CHECK(shown("MEM"));
    picture(dir, "flex-memory");
    tap_at(104, 122, 80); run(30);                 /* the band's place */
    CHECK(!ui_edit_active());
    where = "...held there too, the antennas";
    run(200);                                      /* a finger back sooner is the same one */
    down(SLAB_X, SLAB_Y); run(600); up(); run(50);
    CHECK(ui_edit_active() && shown("RX ANT"));
    tap_at(OUTSIDE_X, OUTSIDE_Y, 80); settle(); quiet();
    st.mem_state = 3; st.mem_name[0] = 0; run(120);
    CHECK(shown("NO MEMORIES") && shown("on the radio"));

    radio(); run(120);
    batt_radio(dir, "flex");
    knob_sweep(dir, "flex");
    lock_sweep(dir, "flex");
    knob_radio(dir, "flex");
    where = "...the memory face";
    st.knob_batt = true; st.knob_pct = 85;
    st.mem_state = 2; st.mem_ch = 7; strcpy(st.mem_name, "ON0TEN"); run(120);
    knob_check(dir, NULL);
    st.mem_state = 0; st.knob_batt = false; run(120);

    radio();
    strcpy(st.agc, "slow");
    st.gain = 32;
    settle(); quiet();
    sdr_reading(dir, "flex", "SLOW", "+32 dB");
}
#elif VFO_RADIO_ICOM
static void ic7610(void)
{
    base();
    strcpy(st.agc, "mid");
    st.have_gain = true; st.gain = 0; st.gain_min = 0; st.gain_max = 2; st.gain_step = 1;
    st.n_rx = 2; st.rx = 0;
    st.n_ant = 2; st.ant = 0; st.has_rx_ant = true; st.have_ant = true;
    st.has_levels = true; st.have_levels = true;
    st.has_tuner = true; st.have_tuner = true;
    st.max_w = 100;
}

static void runs(const char *dir)
{
    ui_commit_t c;

    where = "IC-7610: a tap keys, once the finger has stayed off";
    ic7610(); settle(); quiet();
    tap_at(SLAB_X, SLAB_Y, 120);
    CHECK(!ptt());
    run(KEY_LATEST);
    CHECK(ptt());
    settle(); quiet();

    where = "IC-7610: the glass loses a held finger, before the buzz or after: never a key";
    {
        static const uint32_t AT[] = { 90, 150, 240, 480, 600 };
        for (size_t i = 0; i < sizeof AT / sizeof *AT; i++) {
            const seg_t s[] = { { 0, AT[i], SLAB_X, SLAB_Y }, { AT[i] + 105, 1000, SLAB_X, SLAB_Y } };
            const seen_t r = timeline(s, 2, 1400);
            CHECK(r.keys == 0 && r.buzz && r.open_at_end && shown("ANTENNA"));
            after();
        }
    }

    where = "IC-7610: held, its antenna editor, the quicker way";
    down(SLAB_X, SLAB_Y); run(520);
    CHECK(ui_edit_active() && shown("ANTENNA") && shown("ANT1"));
    CHECK(buzz());
    picture(dir, "icom-antenna");
    up(); run(300);
    CHECK(!ptt());
    ui_edit_rotate(3); run(30);
    CHECK(shown("ANT2+RX"));
    tap_at(PANEL_X, PANEL_Y, 80); run(20);
    CHECK(commit(&c) && c.have_ant && c.ant == 1 && c.ant_rx);
    CHECK(!ui_edit_active());                       /* no transmit antenna apart */
    settle(); quiet();

    where = "IC-7610: the swipe down's row as before: VFO, ANTENNA";
    down(180, 70);
    for (int y = 70; y < 230; y += 16) { fy = (uint16_t)y; run(8); }
    up(); run(50);
    CHECK(shown("VFO"));
    tap_at(PANEL_X, PANEL_Y, 80); run(20);
    CHECK(shown("ANTENNA"));
    tap_at(OUTSIDE_X, OUTSIDE_Y, 80); settle(); quiet();

    where = "IC-705: no antennas, so a hold keys as it lifts, as ever";
    base();
    strcpy(st.agc, "mid");
    st.has_memories = true;
    run(60);
    down(SLAB_X, SLAB_Y); run(900);
    CHECK(!ui_edit_active() && !buzz());
    CHECK(!ptt());
    up(); run(SEEN);
    CHECK(ptt());
    settle(); quiet();
    where = "IC-705: ...and a tap at once, with no wait";
    tap_at(SLAB_X, SLAB_Y, 60);
    CHECK(ptt());
    settle(); quiet();

    where = "IC-R8600: a tap keys nothing; held, its three antennas";
    base();
    st.rx_only = true;
    st.n_ant = 3; st.ant = 2; st.have_ant = true;
    run(60);
    tap_at(SLAB_X, SLAB_Y, 120); run(300);
    CHECK(!ptt() && !ui_edit_active());
    down(SLAB_X, SLAB_Y); run(520);
    CHECK(ui_edit_active() && shown("ANTENNA") && shown("ANT3") && buzz());
    picture(dir, "icom-r8600-antenna");
    up(); run(300);
    CHECK(!ptt());
    ui_edit_rotate(-2); run(30);
    tap_at(PANEL_X, PANEL_Y, 80); run(20);
    CHECK(commit(&c) && c.have_ant && c.ant == 0);
    CHECK(!ui_edit_active());
    settle(); quiet();

    where = "IC-R8600: RECEIVER, the battery beside the logo -- or the speaker";
    st.have_batt = true;
    for (int spk = 0; spk < 2; spk++) {
        st.headset = !spk; st.speaker = spk;
        for (int i = 0; i < NLEVELS; i++) {
            st.batt = (uint8_t)LEVELS[i]; run(60);
            CHECK(shown("RECEIVER"));
            batt_check(dir, NULL, false);
        }
        st.batt = 70; run(60);
        batt_check(dir, spk ? "icom-batt-receiver-speaker" : "icom-batt-receiver", false);
    }
    st.headset = st.speaker = st.have_batt = false;
    where = "IC-R8600: the knob's battery over RECEIVER's face";
    st.knob_batt = true; st.knob_pct = 85; run(60);
    knob_levels(dir);
    st.knob_batt = false; run(60);

    ic7610(); run(120);
    batt_radio(dir, "icom");
    knob_sweep(dir, "icom");
    lock_sweep(dir, "icom");
    knob_radio(dir, "icom");

    ic7610();
    strcpy(st.agc, "slow");
    settle(); quiet();
    sdr_reading(dir, "icom", "SLOW", "OFF");
}
#elif VFO_RADIO_SVXCONNECT || VFO_RADIO_AETHERSDR || VFO_RADIO_XIEGU
/* The reflector's face, AetherSDR's and the Xiegus': the slab as the
 * others', in each one's palette. On the reflector's, the knob's battery
 * over the talker, however long his call and where he is; on the Xiegus',
 * the web SDR's reading. */
static void runs(const char *dir)
{
    base(); run(120);
#if VFO_RADIO_SVXCONNECT
    batt_radio(dir, "svx");
    knob_sweep(dir, "svx");
    lock_sweep(dir, "svx");
    where = "...over the talker, the longest call, and where he is";
    st.knob_batt = true; st.knob_pct = 85;
    st.tg = 2622; strcpy(st.tg_name, "Belgium");
    strcpy(st.talker, "VE3ABC/VE2X"); strcpy(st.talker_info, "Toronto, Canada");
    st.rx_level_db = -3.0f; run(1500);
    knob_levels(dir);
    knob_check(dir, "svx-knob-batt-talker");
    st.talker[0] = st.talker_info[0] = 0; st.rx_level_db = -90.0f; st.knob_batt = false; run(1500);
#elif VFO_RADIO_AETHERSDR
    batt_radio(dir, "aether");
    knob_sweep(dir, "aether");
    lock_sweep(dir, "aether");
    knob_radio(dir, "aether");
#else
    batt_radio(dir, "xiegu");
    knob_sweep(dir, "xiegu");
    lock_sweep(dir, "xiegu");
    knob_radio(dir, "xiegu");
    /* The web SDR's reading alone, between the AGC and the preamp. */
    base();
    strcpy(st.agc, "slow");
    st.have_gain = true; st.gain = 0; st.gain_max = 1; st.gain_step = 1;
    settle(); quiet();
    sdr_reading(dir, "xiegu", "SLOW", "OFF");
#endif
}
#elif VFO_RADIO_PHONE
/* The telephone's slab, the call's: CALL, ----, NO SERVICE, ENDED; HANG UP
 * on the red slab; a call ringing in, DECLINE red with a headset -- or,
 * with a speaker, which is no headset, the slab in halves, the speaker
 * under ANSWER and its battery beside it there. */
static void runs(const char *dir)
{
    base(); run(120);
    static const struct { uint8_t call; bool link; uint8_t fav; const char *says; bool red; } SL[] = {
        { 0, true, 3, "CALL", false },     { 0, true, 0, "----", false }, { 0, false, 0, "NO SERVICE", false },
        { 1, true, 3, "HANG UP", true },   { 3, true, 3, "HANG UP", true }, { 4, true, 3, "ENDED", false },
        { 2, true, 3, "DECLINE", true },
    };
    char pic[64];
    st.have_batt = true;
    for (int spk = 0; spk < 2; spk++) {
        st.headset = !spk; st.speaker = spk;
        for (size_t k = 0; k < sizeof SL / sizeof *SL; k++) {
            st.call = SL[k].call; st.link_ok = SL[k].link; st.n_fav = SL[k].fav;
            strcpy(st.peer, st.call ? "Office" : "");
            const bool halves = spk && st.call == 2;
            snprintf(pic, sizeof pic, "the slab %s, %s", halves ? "in halves" : SL[k].says,
                     spk ? "a speaker's battery" : "a headset's");
            where = pic;
            for (int i = 0; i < NLEVELS; i++) {
                st.batt = (uint8_t)LEVELS[i]; run(60);
                CHECK(shown(halves ? "ANSWER" : SL[k].says));
                batt_check(dir, NULL, SL[k].red);
            }
        }
        st.call = 2; st.batt = 40; run(60);
        batt_check(dir, spk ? "tel-batt-ringing-speaker" : "tel-batt-ringing-headset", true);
        st.call = 3; st.batt = 80; run(60);
        batt_check(dir, spk ? "tel-batt-call-speaker" : "tel-batt-call-headset", true);
        st.call = 0; st.link_ok = false; st.n_fav = 0; st.batt = 10; run(60);
        batt_check(dir, spk ? "tel-batt-no-service-speaker" : "tel-batt-no-service-headset", false);
        st.link_ok = true; st.n_fav = 3;
    }
    where = "no device, a charge left over: no battery";
    st.headset = st.speaker = false; st.call = 0; run(60);
    CHECK(!batt_shown());
    st.have_batt = false; run(60);

    /* The knob's battery over the telephone's face: ringing, in a call with
     * both meters at full, their peaks at the top beside it; none under the
     * keypad; over the calls' history. */
    knob_sweep(dir, "tel");
    lock_sweep(dir, "tel");
    where = "...a call ringing in, its slab in halves";
    st.knob_batt = true; st.knob_pct = 85;
    st.call = 2; strcpy(st.peer, "Office"); strcpy(st.peer_num, "+441632960123"); run(60);
    knob_levels(dir);
    where = "...in a call, both meters at full, their peaks at the top";
    st.call = 3; st.call_ms = 167000; st.rx_level_db = 0.0f; st.tx_mic_dbm = 0.0f; run(1500);
    knob_levels(dir);
    knob_check(dir, "tel-knob-batt-call");
    where = "...the keypad, over all above the slab: no ink of it";
    swipe(180, 70, 180, 230);
    CHECK(shown("DTMF"));
    CHECK(knob_battery() && !knob_inked());
    picture(dir, "tel-knob-batt-keypad");
    tap_at(180, 14, 80); settle(); quiet();
    CHECK(!shown("DTMF"));
    knob_check(dir, NULL);
    where = "...the calls, swiped in from the left";
    st.call = 0; st.peer[0] = 0; st.rx_level_db = st.tx_mic_dbm = -90.0f; run(1500);
    {
        static const ui_call_t C[2] = { { "+447700900123", "Mum", 0, 167, UI_CALL_OUT },
                                        { "+441632960123", "Office", 0, 0, UI_CALL_MISSED } };
        CHECK(ui_set_calls(C, 2));
    }
    swipe(60, 180, 300, 180);
    CHECK(ui_edit_active());
    knob_check(dir, NULL);
    tap_at(OUTSIDE_X, OUTSIDE_Y, 80); settle(); quiet();
    CHECK(!ui_edit_active());
    st.knob_batt = false; run(60);
}
#elif VFO_RADIO_UBERSDR
/* ---- a guest's time left, at the slab's left end (ui.c left_slab) ---- */

/* What it says for s seconds (left_says), and in which colour: dim, then
 * yellow, red in the last minute and when idle. */
static int left_colour(int s, bool idle) { return idle || s < 60 ? IS_RED : s < 300 ? IS_YELLOW : IS_OTHER; }

static int left_least_room = 99, left_least_gap = 99, left_widest[3];   /* minutes and hours, the seconds, idle */
static lv_obj_t *s_call;                    /* the spot's call on the slab, as spot_is() last set it */
static char s_call_says[24];                /* ...what it says, whole */

/* The spot nearest the dial, this call, as the slab shows it -- its label. */
static void spot_is(const char *call)
{
    static ui_spot_t sp = { .hz = 14215000, .mode = "usb", .what = "DX  2m  heard 12 dB" };
    strcpy(sp.call, call);
    ui_set_spots(&sp, 1);
    run(60);
    snprintf(s_call_says, sizeof s_call_says, "%s", call);
    s_call = call_label(call);
}

/* The time left as the face shows it now, at st.left_s: its text and
 * colour, and its ink -- in the slab, inside the glass's margin, nothing
 * else's within ROOM px: the call however long (cut with dots), the line
 * under it, the count, the battery and the logo at the other end. With
 * the headset's logo there, level with it. Its box 4 px at least from the
 * call's, which keeps its room (call_check). */
static void left_check(const char *dir, const char *pic)
{
    char want[24];
    left_says(want, sizeof want, st.left_s, st.left_idle);
    lv_obj_t *l = label_is(lv_screen_active(), want);
    CHECK(l != NULL);
    if (!l) {
        printf("  [%s] no \"%s\" on the face\n", where, want);
        return;
    }
    const int got = colour_is(lv_obj_get_style_text_color(l, LV_PART_MAIN));
    CHECK(got == left_colour(st.left_s, st.left_idle));
    box_t b, logo;
    int room, lroom;
    CHECK(ink_of(l, &b, &room));
    CHECK(b.y1 >= SLAB_TOP + 2);
    CHECK(in_glass(&b));
    CHECK(room >= ROOM);
    if (room < left_least_room) left_least_room = room;
    const int k = st.left_idle ? 2 : st.left_s < 300 ? 1 : 0;
    if (b.x2 - b.x1 + 1 > left_widest[k]) left_widest[k] = b.x2 - b.x1 + 1;
    CHECK(s_call != NULL);
    if (s_call) {
        const int gap = lv_obj_get_x(s_call) - (lv_obj_get_x(l) + lv_obj_get_width(l));
        CHECK(gap >= 4);
        call_check(s_call_says, l);
        if (gap < left_least_gap) left_least_gap = gap;
    }
    lv_obj_t *bt = st.headset ? label_is(lv_screen_active(), LV_SYMBOL_BLUETOOTH) : NULL;
    if (bt && ink_of(bt, &logo, &lroom)) {
        CHECK(abs((b.y1 + b.y2) - (logo.y1 + logo.y2)) <= 3);
        if (pic) printf("  ...the logo's ink %d-%d, %d-%d\n", logo.x1, logo.x2, logo.y1, logo.y2);
    }
    if (pic) {
        printf("  %-48s %-10s %-7s ink %d-%d, %d-%d  %2d px clear%s\n", where, want, IS[got], b.x1, b.x2, b.y1,
               b.y2, room, bt ? "  level with the logo" : "");
        picture(dir, pic);
    }
}

/* Every value it can say -- each second of the last five minutes, each
 * minute's first and last to a hundred, hours to a hundred and a few with
 * more digits, an idle limit's minute -- beside the spot `call` and the
 * device as st has it. */
static void left_sweep(const char *dir, const char *call)
{
    st.have_left = true;
    st.left_idle = false;
    spot_is(call);
    for (int s = 0; s < 6000; s += s < 300 || s % 60 ? 1 : 59) {
        st.left_s = s; run(60);
        left_check(dir, NULL);
    }
    static const int MINS[] = { 0, 40, 48, 59 };      /* 48: the widest digits */
    static const int MORE[] = { 100, 111, 168, 188, 488, 999, 1000, 4888, 9999, 10000, 48888, 99999 };
    for (int i = 0; i < 99 + (int)(sizeof MORE / sizeof *MORE); i++) {
        const int h = i < 99 ? i + 1 : MORE[i - 99];
        for (size_t m = 0; m < sizeof MINS / sizeof *MINS; m++) {
            if (h * 3600 + MINS[m] * 60 < 6000) continue;
            st.left_s = h * 3600 + MINS[m] * 60; run(60);
            left_check(dir, NULL);
        }
    }
    st.left_idle = true;
    for (int s = 0; s <= 60; s++) {
        st.left_s = s; run(60);
        left_check(dir, NULL);
    }
    st.left_idle = false;
}

/* The receiver's slab: the spot nearest the dial, large, where PTT is, its
 * line under it. A long call kept clear of the device's battery, or its
 * logo, while they show: centred where it fits so, else moved aside, and
 * only one too long for its room cut with dots. At its left end a guest's
 * time left, the call kept clear of it too. */
static void runs(const char *dir)
{
    base();
    st.has_spots = true;
    st.freq_hz = 14215000;
    run(120);
    static const char *const CALLS[] = { "LU7YZ", "W1AW", "PA/ON4ABC", "VE3ABC/VE2X", "WWWWWWWWWWW", "" };
    char pic[64];
    for (size_t k = 0; k < sizeof CALLS / sizeof *CALLS; k++) {
        ui_spot_t sp = { .hz = 14215000, .mode = "usb", .what = "DX  2m  heard 12 dB" };
        strcpy(sp.call, CALLS[k]);
        ui_set_spots(&sp, 1);
        const char *says = CALLS[k][0] ? CALLS[k] : "14.215.0";
        st.headset = st.speaker = st.have_batt = false; run(60);
        snprintf(pic, sizeof pic, "the spot %s, no device", says);
        where = pic;
        call_check(says, NULL);
        for (int spk = 0; spk < 2; spk++) {
            st.headset = !spk; st.speaker = spk;
            st.have_batt = false; run(60);
            snprintf(pic, sizeof pic, "the spot %s, whole without a battery (%s)", says, spk ? "speaker" : "headset");
            where = pic;
            CHECK(shown(says) == (strcmp(says, "WWWWWWWWWWW") != 0));   /* the widest cut, even so */
            call_check(says, NULL);
            st.have_batt = true;
            snprintf(pic, sizeof pic, "the spot %s, %s", says, spk ? "a speaker's battery" : "a headset's battery");
            where = pic;
            for (int i = 0; i < NLEVELS; i++) {
                st.batt = (uint8_t)LEVELS[i]; run(60);
                batt_check(dir, NULL, false);
            }
            call_check(says, NULL);
        }
    }
    where = "no spots here";
    ui_set_spots(NULL, 0);
    st.headset = true; st.speaker = false; st.batt = 50; run(60);
    CHECK(shown("no spots or voices here"));
    batt_check(dir, NULL, false);

    where = "pictures";
    ui_spot_t sp = { .call = "LU7YZ", .hz = 14215000, .mode = "usb", .what = "DX  2m  heard 12 dB" };
    ui_set_spots(&sp, 1);
    st.batt = 80; run(60);
    batt_check(dir, "uber-batt-green", false);
    strcpy(sp.call, "VE3ABC/VE2X");
    ui_set_spots(&sp, 1);
    st.batt = 30; run(60);
    batt_check(dir, "uber-batt-long", false);
    where = "a long call while the battery shows: moved aside, whole, on one line";
    lv_obj_t *l = label_from(lv_screen_active(), "VE3ABC");
    CHECK(l && shown("VE3ABC/VE2X") && 2 * lv_obj_get_x(l) + lv_obj_get_width(l) < W &&
          lv_obj_get_height(l) == lv_font_get_line_height(&lv_font_montserrat_28));
    call_check("VE3ABC/VE2X", NULL);
    where = "a call too long for its room: on one line, cut with dots";
    strcpy(sp.call, "WWWWWWWWWWW");
    ui_set_spots(&sp, 1);
    run(60);
    l = label_from(lv_screen_active(), "WWW");
    CHECK(l && strstr(lv_label_get_text(l), "...") &&
          lv_obj_get_height(l) == lv_font_get_line_height(&lv_font_montserrat_28));
    call_check("WWWWWWWWWWW", NULL);
    strcpy(sp.call, "VE3ABC/VE2X");
    ui_set_spots(&sp, 1);
    st.headset = false; st.speaker = true; st.batt = 10; run(60);
    batt_check(dir, "uber-batt-speaker", false);
    where = "the battery gone: the spot's call whole again";
    st.have_batt = false; run(60);
    CHECK(!batt_shown());
    CHECK(shown("VE3ABC/VE2X"));
    st.speaker = false; run(60);
    CHECK(shown("VE3ABC/VE2X"));
    where = "no device, a charge left over: no battery, the call whole";
    st.have_batt = true; run(60);
    CHECK(!batt_shown() && shown("VE3ABC/VE2X"));
    st.have_batt = false; run(60);

    where = "a guest's time left: none where no limit applies";
    st.have_left = false; st.left_s = 3120; run(60);
    CHECK(!shown("52 min"));
    static const char *const SWEEP[] = { "LU7YZ", "VE3ABC/VE2X", "WWWWWWWWWWW", "14.268.0" };
    for (int dev = 0; dev < 3; dev++) {
        st.headset = dev == 1; st.speaker = dev == 2; st.have_batt = dev > 0; st.batt = 30;
        for (size_t k = 0; k < sizeof SWEEP / sizeof *SWEEP; k++) {
            snprintf(pic, sizeof pic, "the time left beside %s, %s", SWEEP[k],
                     dev == 1 ? "a headset's battery" : dev == 2 ? "a speaker's battery" : "no device");
            where = pic;
            left_sweep(dir, SWEEP[k]);
            if (dev) batt_check(dir, NULL, false);
        }
    }
    where = "...gone again: the call whole";
    st.headset = st.speaker = st.have_batt = false; st.have_left = false; run(60);
    CHECK(!shown("0:00") && shown("14.268.0"));
    ui_set_spots(&sp, 1);                          /* VE3ABC/VE2X */
    run(60);
    CHECK(shown("VE3ABC/VE2X"));

    where = "pictures of the time left";
    st.have_left = true; st.left_idle = false;
    spot_is("LU7YZ");
    st.left_s = 3125; run(60);
    left_check(dir, "uber-left-52-min");
    st.left_s = 6000; run(60);
    left_check(dir, "uber-left-hours");
    st.left_s = 299; run(60);
    left_check(dir, "uber-left-4-59");
    st.left_s = 42; run(60);
    left_check(dir, "uber-left-0-42");
    spot_is("VE3ABC/VE2X");
    st.left_s = 3125; run(60);
    left_check(dir, "uber-left-long-moved");
    st.headset = true; st.have_batt = true; st.batt = 80; run(60);
    left_check(dir, "uber-left-long-headset");
    st.headset = false; st.speaker = true; st.left_s = 299; run(60);
    left_check(dir, "uber-left-long-speaker");
    st.speaker = false; st.headset = true; st.batt = 30; st.left_idle = true; st.left_s = 42; run(60);
    left_check(dir, "uber-left-idle");
    st.headset = st.have_batt = st.have_left = st.left_idle = false; run(60);

    /* No link: the warning, the receiver it is about under it -- the one in
     * use NOT FOUND, then CONNECTING to the next -- and the address card's
     * five lines under that, in the panel, the panel inside the glass. */
    where = "no link: the receiver named under the warning";
    ui_set_netinfo(CARD);
    st.link_ok = false;
    static const struct { const char *says, *name; } NAMED[] = {
        { "NOT FOUND", "ON6URE-TEL-LAN" }, { "CONNECTING", "ON6URE-TEL" }, { "NO ANSWER", "WWWWWWWWWWWWWWWWWWWWWWW" },
    };
    for (size_t k = 0; k < sizeof NAMED / sizeof *NAMED; k++) {
        st.warn = NAMED[k].says;
        snprintf(st.warn_name, sizeof st.warn_name, "%s", NAMED[k].name);
        run(60);
        lv_obj_t *t = label_is(lv_screen_active(), NAMED[k].says), *nm = label_from(lv_screen_active(), "ON6URE"),
                 *c = card_label();
        if (!nm) nm = label_from(lv_screen_active(), "WWW");
        CHECK(t && nm && c);
        if (!t || !nm || !c) continue;
        lv_area_t at, an, ac, ap;
        lv_obj_get_coords(t, &at);
        lv_obj_get_coords(nm, &an);
        lv_obj_get_coords(c, &ac);
        lv_obj_get_coords(lv_obj_get_parent(c), &ap);
        const bool cut = strstr(lv_label_get_text(nm), "...") != NULL;
        CHECK(cut == (k == 2));                        /* only the one too long, cut */
        CHECK(lv_obj_get_height(nm) == lv_font_get_line_height(&lv_font_montserrat_20));   /* one line */
        CHECK(an.y1 >= at.y2 + 2 && ac.y1 >= an.y2 + 2);   /* under the warning, over the card */
        CHECK(an.x1 > ap.x1 + 8 && an.x2 < ap.x2 - 8);     /* inside the panel */
        CHECK(ac.y2 <= ap.y2 - 4);
        box_t ink;
        int room;
        CHECK(ink_of(nm, &ink, &room) && ink.x1 > ap.x1 + 8 && ink.x2 < ap.x2 - 8);
        const float r = ink_reach(lv_obj_get_parent(c));
        CHECK(r <= GLASS_R);
        printf("  %-10s %-24s its line %d-%d, under the warning's %d, over the card's %d-%d; the panel %d-%d, "
               "%.1f px from the middle at the most%s\n", NAMED[k].says, lv_label_get_text(nm), (int)an.y1,
               (int)an.y2, (int)at.y2, (int)ac.y1, (int)ac.y2, (int)ap.y1, (int)ap.y2, (double)r, cut ? ", cut" : "");
        if (k < 2) picture(dir, k ? "uber-warning-connecting" : "uber-warning-not-found");
    }
    where = "...with none named, the panel as ever";
    st.warn = "NO LINK";
    st.warn_name[0] = 0;
    run(60);
    {
        lv_obj_t *c = card_label();
        CHECK(c && !label_from(lv_screen_active(), "WWW") && !label_from(lv_screen_active(), "ON6URE"));
        if (c) CHECK(lv_obj_get_height(lv_obj_get_parent(c)) == 134);
    }
    st.warn = NULL;
    st.link_ok = true;
    run(60);

    /* The knob's battery over the receiver's face, a KiwiSDR's line beside
     * it; none under the SSTV viewer, whose title is there. */
    knob_sweep(dir, "uber");
    lock_sweep(dir, "uber");
    knob_radio(dir, "uber");
    where = "...the SSTV viewer, over the whole face: no ink of it";
    st.knob_batt = true; st.knob_pct = 85; st.n_sstv = 3; run(60);
    swipe(300, 180, 60, 180);
    CHECK(ui_edit_active());
    tap_at(PANEL_X, PANEL_Y, 80); run(60);
    CHECK(ui_sstv_wanted(NULL) == 0);
    CHECK(knob_battery() && !knob_inked());
    picture(dir, "uber-knob-batt-sstv");
    tap_at(180, 180, 80); settle(); quiet();
    CHECK(ui_sstv_wanted(NULL) == -1);
    knob_check(dir, NULL);
    /* The gallery gone with its receiver -- the next in the list taken, with
     * none of its own, or none yet: the viewer closes, the dial is back. */
    where = "...the SSTV viewer, its receiver's gallery gone: closed";
    swipe(300, 180, 60, 180);
    tap_at(PANEL_X, PANEL_Y, 80); run(60);
    CHECK(ui_sstv_wanted(NULL) == 0);
    st.n_sstv = -1; run(60);
    CHECK(ui_sstv_wanted(NULL) == -1 && !ui_edit_active());
    quiet();
    st.knob_batt = false; run(60);

    /* The KiwiSDR beside it, between the SNR where the AGC is -- below zero,
     * its widest -- and the noise filter's name where the gain is. */
    static const char *const NR[] = { "OFF", "NR2", "RN2", "NR4" };
    for (int k = 1; k < 4; k++) {
        base();
        st.have_snr = true;
        st.snr_db = -10.0f;
        st.have_gain = true; st.gain = (int8_t)k; st.gain_max = 3; st.n_gain_names = 4;
        for (int j = 0; j < 4; j++) strcpy(st.gain_names[j], NR[j]);
        settle(); quiet();
        char face[16];
        snprintf(face, sizeof face, "uber-%s", NR[k]);
        sdr_reading(dir, face, "-10 dB", NR[k]);
    }

    printf("%s: the time left, %d px clear of anything else at the least, its box %d px from the call's; "
           "its ink at the widest %d px (minutes, hours), %d (the seconds), %d (idle)\n", "slab_uber",
           left_least_room, left_least_gap, left_widest[0], left_widest[1], left_widest[2]);
    printf("%s: the spot's call %d times centred, %d moved aside, %d cut with dots; its box within %d-%d\n",
           "slab_uber", calls_centred, calls_moved, calls_cut, call_least_l, call_most_r);
}
#elif VFO_RADIO_SETUP
/* The setup firmware's face: its screens over the whole face, each title
 * 110 px over the middle, and the firmwares' list on the dial, a chooser;
 * the knob's own battery over all of them, clear of the titles. Its bare
 * face, before the first screen, as AetherSDR's. */
static void runs(const char *dir)
{
    base(); run(120);
    static const struct { const char *title, *text; } S[] = {
        { "VFO-KNOB", "Starting" },
        { "WIFI SETUP", "Join the WiFi network\nVFOKnob\nwith your phone, then\nchoose your network on\n"
                        "the page that opens." },
        { "WIFI SETUP", "Joining\nHomeNetwork" },
        { "WIFI SETUP", "Could not join\nHomeNetwork:\nwrong password?\nTry again on the phone." },
        { "FIRMWARE", "Looking up\nthe firmwares..." },
        { "NO WIFI", "The knob's WiFi\nwould not start.\nRestart the knob." },
    };
    char pic[96];
    where = "the bare face, before the first screen";
    st.knob_batt = true; st.knob_pct = 85; run(60);
    knob_levels(dir);
    for (size_t i = 0; i < sizeof S / sizeof *S; i++) {
        ui_setup_show(S[i].title, S[i].text); run(60);
        snprintf(pic, sizeof pic, "the knob's battery over %s: %.30s", S[i].title, S[i].text);
        for (char *c = pic; *c; c++)
            if (*c == '\n') *c = ' ';
        where = pic;
        CHECK(shown(S[i].title));
        knob_levels(dir);
    }
    where = "...over the list of firmwares, a chooser";
    {
        static const char T[3][12] = { "INSTALL", "INSTALL", "WIFI" };
        static const char N[3][24] = { "SVXConnect 1.19.0", "Icom 1.18.4", "Set up again" };
        ui_setup_show("FIRMWARE", "Turn to your radio,\nthen tap to install.");
        ui_ask_choice(T, N, 3, 0);
        ui_setup_show("FIRMWARE", "Turn to your radio,\nthen tap to install."); run(60);
        CHECK(ui_choice_active());
        knob_levels(dir);
        knob_check(dir, "setup-knob-batt");
        where = "...none on USB, the list up";
        st.knob_batt = false; run(60);
        CHECK(!knob_battery());
        st.knob_batt = true; run(60);
        knob_check(dir, NULL);
        ui_ask_choice(NULL, NULL, 0, 0); run(60);
    }
    where = "...a beta stamped BETA in the panel's corner, and only it";
    {
        static const char T[3][12] = { "INSTALL", "INSTALL", "WIFI" };
        static const char N[3][24] = { "Telephone 1.22.0", "EpiBeam 1.22.0", "Set up again" };
        static const bool B[3] = { false, true, false };
        ui_ask_choice_beta(T, N, B, 3, 0); run(60);
        CHECK(shown("Telephone 1.22.0") && !shown("BETA"));
        ui_edit_rotate(1); run(30);
        CHECK(shown("EpiBeam 1.22.0") && shown("BETA"));
        lv_obj_t *stamp = label_from(lv_screen_active(), "BETA"), *title = label_from(lv_screen_active(), "INSTALL");
        CHECK(stamp && title);
        if (stamp && title) {
            lv_area_t s, t;
            lv_obj_get_coords(stamp, &s);
            lv_obj_get_coords(title, &t);
            /* Clear of the title, and on the glass: its far corner inside the circle. */
            CHECK(s.x1 > t.x2 + 8);
            const int dx = s.x2 - 180, dy = s.y1 - 180;
            CHECK(dx * dx + dy * dy < 172 * 172);
        }
        picture(dir, "setup-beta");
        ui_edit_rotate(1); run(30);
        CHECK(shown("Set up again") && !shown("BETA"));
        where = "...and none on any other chooser";
        ui_ask_choice(T, N, 3, 1); run(60);
        CHECK(shown("EpiBeam 1.22.0") && !shown("BETA"));
        ui_ask_choice(NULL, NULL, 0, 0); run(60);
    }
    ui_setup_hide(); run(60);
    st.knob_batt = false; run(60);
}
#elif VFO_RADIO_KIWI
/* Kiwi888's face, the receiver in use on its slab -- its name, under it its
 * antenna or address, and where it is -- with no PTT and no spots. */
static void kiwi_base(void)
{
    static const char *const NR[] = { "OFF", "WDSP", "LMS", "SPEC" };
    base();
    strcpy(st.agc, "med");
    st.have_gain = true; st.gain = 0; st.gain_max = 3; st.n_gain_names = 4;
    for (int j = 0; j < 4; j++) strcpy(st.gain_names[j], NR[j]);
    st.freq_hz = 7123000; st.mode = "lsb"; st.filt_lo = -2700; st.filt_hi = -300;
    strcpy(st.server, "EchoTracer");
    st.rx_line2 = "Web-888  192.168.1.88:8077";
    st.rx_line3 = "Lombardsijde, Belgium";
}

/* The right ear's reading, between the AGC and the noise filter at their
 * widest. Then the receiver's name on the slab, a Bluetooth device's logo
 * and its battery at the slab's right end, the battery at every charge:
 * the name kept clear of them as the UberSDR's spot is -- centred where it
 * fits so, else moved aside, whole, and only one too long for its room cut
 * with dots (call_check). And the knob's own battery over the S-units, as
 * on every face: over the red peak mark at the top of the arc, the right
 * ear's line and reading, and the reading at its widest with OV after it,
 * in red. */
static void runs(const char *dir)
{
    static const char *const AGC[] = { "fast", "slow" }, *const AGC_UP[] = { "FAST", "SLOW" };
    static const char *const NR[] = { "OFF", "WDSP", "LMS", "SPEC" };
    for (int a = 0; a < 2; a++)
        for (int k = 1; k < 4; k += 2) {
            base();
            strcpy(st.agc, AGC[a]);
            st.have_gain = true; st.gain = (int8_t)k; st.gain_max = 3; st.n_gain_names = 4;
            for (int j = 0; j < 4; j++) strcpy(st.gain_names[j], NR[j]);
            settle(); quiet();
            char face[24];
            snprintf(face, sizeof face, "kiwi-%s-%s", AGC_UP[a], NR[k]);
            sdr_reading(dir, face, AGC_UP[a], NR[k]);
        }

    kiwi_base(); run(120);
    /* In turn: centred on any slab; moved aside beside a speaker's battery;
     * beside either device's; the same, a speaker's battery's room to two
     * pixels; moved aside beside a logo alone, cut beside a battery; centred
     * on a slab of its own, cut beside any device; cut on any slab. */
    static const char *const NAMES[] = { "EchoTracer", "TerraBooster", "TerraBooster 2", "KiwiSDR Bruges",
                                         "KiwiSDR ON4ABC", "Web-888 OctaLoop", "KiwiSDR ON4ABC Bruges" };
    char pic[96];
    for (size_t k = 0; k < sizeof NAMES / sizeof *NAMES; k++) {
        printf("  the receiver %-28s %3d px\n", NAMES[k], text_w(NAMES[k], &lv_font_montserrat_28));
        strcpy(st.server, NAMES[k]);
        st.headset = st.speaker = st.have_batt = false; run(60);
        snprintf(pic, sizeof pic, "the receiver %s, no device", NAMES[k]);
        where = pic;
        call_check(NAMES[k], NULL);
        for (int spk = 0; spk < 2; spk++) {
            st.headset = !spk; st.speaker = spk;
            st.have_batt = false; run(60);
            snprintf(pic, sizeof pic, "the receiver %s, beside the logo alone (%s)", NAMES[k],
                     spk ? "speaker" : "headset");
            where = pic;
            call_check(NAMES[k], NULL);
            st.have_batt = true;
            snprintf(pic, sizeof pic, "the receiver %s, %s", NAMES[k],
                     spk ? "a speaker's battery" : "a headset's battery");
            where = pic;
            for (int i = 0; i < NLEVELS; i++) {
                st.batt = (uint8_t)LEVELS[i]; run(60);
                batt_check(dir, NULL, false);
            }
            call_check(NAMES[k], NULL);
        }
    }

    where = "pictures";
    strcpy(st.server, "EchoTracer");
    st.speaker = false; st.headset = true; st.have_batt = true; st.batt = 80; run(60);
    batt_check(dir, "kiwi-batt-green", false);
    where = "a long name while the battery shows: moved aside, whole, on one line";
    strcpy(st.server, "KiwiSDR Bruges");
    st.batt = 30; run(60);
    lv_obj_t *l = label_from(lv_screen_active(), "KiwiSDR");
    CHECK(l && shown("KiwiSDR Bruges") && 2 * lv_obj_get_x(l) + lv_obj_get_width(l) < W &&
          lv_obj_get_height(l) == lv_font_get_line_height(&lv_font_montserrat_28));
    batt_check(dir, "kiwi-batt-long", false);
    call_check("KiwiSDR Bruges", NULL);
    st.headset = false; st.speaker = true; st.batt = 10; run(60);
    CHECK(shown("KiwiSDR Bruges"));
    batt_check(dir, "kiwi-batt-speaker", false);
    call_check("KiwiSDR Bruges", NULL);
    where = "the battery gone: the name whole, centred again beside the speaker";
    st.have_batt = false; run(60);
    CHECK(!batt_shown() && shown("KiwiSDR Bruges"));
    call_check("KiwiSDR Bruges", NULL);
    where = "no device, a charge left over: no battery, the name centred";
    st.speaker = false; st.have_batt = true; run(60);
    CHECK(!batt_shown() && shown("KiwiSDR Bruges"));
    call_check("KiwiSDR Bruges", NULL);
    st.have_batt = false;
    strcpy(st.server, "EchoTracer"); run(60);

    /* The knob's own battery over Kiwi888's face, the right ear's line and
     * reading beside it (knob_radio); and over the reading at its widest,
     * OV after it -- and the right ear's "can't reach" in its place. */
    knob_sweep(dir, "kiwi");
    lock_sweep(dir, "kiwi");
    knob_radio(dir, "kiwi");
    {
        const ui_state_t keep = st;
        st.knob_batt = true; st.knob_pct = 60;
        where = "...over the reading at its widest, OV after it in red";
        st.ovl = true; st.smeter_dbm = -13.0f; run(1500);
        CHECK(shown("-13 dBm  OV"));
        knob_levels(dir);
        knob_check(dir, "kiwi-knob-batt-ov");
        where = "...over the right ear's can't reach, in amber";
        st.ovl = false; st.smeter_dbm = -85.0f;
        st.n_sdr = 1; strcpy(st.sdr_name[0], "KiwiSDR");
        st.rxsrc = 0; st.sdr_trouble = true; strcpy(st.sdr_note, "can't reach"); run(1500);
        CHECK(shown("can't reach"));
        knob_levels(dir);
        st = keep; run(60);
    }

    printf("%s: the receiver's name %d times centred, %d moved aside, %d cut with dots; its box within %d-%d\n",
           "slab_kiwi", calls_centred, calls_moved, calls_cut, call_least_l, call_most_r);
}
#elif VFO_RADIO_OWRX
/* The OpenWebRX face: Kiwi888's slab and meter, the meter in the receiver's
 * own dB on its page's scale -- green, yellow from 70 % of it, red from 90
 * -- its listeners where the AGC is, its squelch where the gain is, and its
 * bands, a swipe down or a tap on the band away. */
#define OW_GREEN  0x22FF2F
#define OW_YELLOW 0xFFF720
#define OW_RED    0xFF5939
#define OW_TRACK  0x373737
#define OW_DIM    0x6E6E6E                  /* ui.c's C_DISABLED there */

static void owrx_base(void)
{
    base();
    st.freq_hz = 7074000; st.mode = "usb"; st.filt_lo = 300; st.filt_hi = 3000;
    st.step_hz = 100;
    st.meter_lo = -108.0f; st.meter_hi = 0.0f;
    st.smeter_dbm = -85.0f;
    st.users = 3; st.sq_db = -150; st.squelch_pct = 0; st.has_squelch = st.have_squelch = true;
    st.band_sel = 1; st.band_wait_s = 0; st.has_sam = true;
    strcpy(st.band_name, "40m");
    strcpy(st.server, "ON4PRA WebSDR");
    st.rx_line2 = "40m  RSPdx";
    st.rx_line3 = "OpenWebRX+ 1.2.126";
    st.n_radios = 2; st.n_radios_direct = 2; st.radio_sel = 0;
    strcpy(st.radio_name[0], "ON4PRA WebSDR");
    strcpy(st.radio_name[1], "fms.komkon.org");
}

/* The meter's colour at share f of its arc, across the middle of its band. */
static uint16_t arc_px(float f)
{
    lv_obj_invalidate(lv_screen_active());
    lv_refr_now(disp);
    const double a = (170.0 + f * 200.0) * M_PI / 180.0;
    const int x = (int)lround(180 + 164 * cos(a)), y = (int)lround(180 + 164 * sin(a));
    return fb[y * W + x];
}
static uint16_t u16(uint32_t rgb) { return lv_color_to_u16(lv_color_hex(rgb)); }

/* The editor's value: the label in the panel saying `text`. */
static lv_obj_t *value_is(const char *text) { return label_is(lv_screen_active(), text); }

static const ui_band_t BANDS9[] = {
    { "160m", "RSPdx", "1.810-2.000" },        { "40m", "RSPdx", "7.000-7.200" },
    { "20m", "RSPdx", "14.000-14.350" },       { "2m FM Repeaters", "RTL-SDR", "145.575-145.800" },
    { "70cm", "Airspy HF+ Disc", "430.000-440.000" },     /* 15: the longest an SDR's keeps */
    { "Airband", "RTL-SDR", "118.000-137.000" },
    { "PMR446", "RTL-SDR", "446.000-446.200" },
    { "Broadcast FM Wide Band", "RTL-SDR", "87.500-108.000" },  /* 22: the longest a name keeps */
    { "CB", "", "" },
};
#define N_BANDS9 ((int)(sizeof BANDS9 / sizeof *BANDS9))

static void runs(const char *dir)
{
    owrx_base(); run(1500);

    where = "the face: its dB readout, nothing under it, its listeners and its squelch";
    CHECK(shown("-85.0 dB"));
    CHECK(shown("USERS") && shown("3"));
    CHECK(shown("SQL") && shown("OPEN"));
    CHECK(!shown("-85 dBm"));
    CHECK(shown("40m") && shown("USB") && shown("2700"));
    CHECK(shown("ON4PRA WebSDR") && shown("40m  RSPdx") && shown("OpenWebRX+ 1.2.126"));
    picture(dir, "owrx-face");

    where = "the meter: green to 70 % of its scale (-85 dB of -108..0 is 21 %)";
    st.smeter_dbm = -54.0f; run(3000);                  /* 50 % */
    CHECK(arc_px(0.45f) == u16(OW_GREEN));
    CHECK(arc_px(0.58f) == u16(OW_TRACK));
    CHECK(shown("-54.0 dB"));
    where = "...yellow from 70 %, red from 90";
    st.smeter_dbm = -5.0f; run(3000);                   /* 95 % */
    CHECK(arc_px(0.65f) == u16(OW_GREEN));
    CHECK(arc_px(0.75f) == u16(OW_YELLOW));
    CHECK(arc_px(0.92f) == u16(OW_RED));
    picture(dir, "owrx-meter-red");
    where = "...on the receiver's own scale: -128..-28, -78 dB is half way";
    st.meter_lo = -128.0f; st.meter_hi = -28.0f; st.smeter_dbm = -78.0f; run(3000);
    CHECK(arc_px(0.45f) == u16(OW_GREEN));
    CHECK(arc_px(0.58f) == u16(OW_TRACK));
    where = "...a scale it cannot mean is not taken";
    st.meter_lo = -10.0f; st.meter_hi = -8.0f; run(1500);
    CHECK(arc_px(0.45f) == u16(OW_GREEN) && arc_px(0.58f) == u16(OW_TRACK));
    st.meter_lo = -108.0f; st.meter_hi = 0.0f; st.smeter_dbm = -85.0f; run(3000);
    where = "...with no link: no reading";
    st.link_ok = false; run(200);
    CHECK(shown("-- dB"));
    st.link_ok = true; run(200);

    where = "the squelch: where the gain is, tapped; its value in the receiver's dB";
    tap_at(180 + 72, 88, 80); run(100);
    CHECK(ui_edit_active() && shown("SQUELCH") && shown("OPEN"));
    ui_edit_rotate(50); run(100);
    CHECK(shown("-54 dB"));
    picture(dir, "owrx-squelch");
    {
        ui_commit_t c;
        commit(&c);                                     /* live as it turns */
        CHECK(c.have_squelch && c.squelch_pct == 50);
    }
    tap_at(OUTSIDE_X, OUTSIDE_Y, 80); settle(); quiet();
    st.squelch_pct = 50; st.sq_db = -54; run(100);
    CHECK(shown("-54 dB") && !shown("OPEN"));
    st.squelch_pct = 0; st.sq_db = -150; run(100);
    where = "...the listeners' place takes no tap";
    tap_at(180 - 72, 88, 80); run(100);
    CHECK(!ui_edit_active());
    after();

    where = "no bands said yet: the swipe down opens nothing";
    swipe(180, 70, 180, 230); run(100);
    CHECK(!ui_edit_active());
    after();

    where = "the bands: a swipe down, on the one in use";
    CHECK(ui_set_bands(BANDS9, N_BANDS9));
    run(60);
    swipe(180, 70, 180, 230); run(100);
    CHECK(ui_edit_active() && shown("BAND 2 / 9") && value_is("40m") && shown("RSPdx  7.000-7.200  -  in use"));
    where = "...the next: what a tap does there -- every other listener moved too";
    ui_edit_rotate(1); run(100);
    CHECK(value_is("20m") && shown("RSPdx  14.000-14.350  -  moves 2 more"));
    picture(dir, "owrx-band");
    {
        lv_obj_t *v = value_is("20m");
        CHECK(v && lv_color_eq(lv_obj_get_style_text_color(v, 0), lv_color_hex(0xFFFF80)));
    }
    where = "...the longest names: in the panel whole or cut with dots, the hint within it";
    int widest = 0;
    for (int i = 2; i < N_BANDS9; i++) {
        ui_edit_rotate(1); run(100);
        lv_obj_t *t = label_from(lv_screen_active(), "BAND ");
        CHECK(t != NULL);
        /* The value: the panel's middle label, by its place. */
        const int32_t px = PANEL_X - 145, pw = 290;
        bool found = false;
        for (uint32_t k = 0; t && k < lv_obj_get_child_count(lv_obj_get_parent(t)); k++) {
            lv_obj_t *o = lv_obj_get_child(lv_obj_get_parent(t), (int32_t)k);
            if (!lv_obj_check_type(o, &lv_label_class) || o == t) continue;
            lv_area_t a;
            lv_obj_get_coords(o, &a);
            CHECK(a.x1 >= px + 4 && a.x2 <= px + pw - 4);
            if (a.x2 - a.x1 > widest) widest = a.x2 - a.x1;
            found = true;
        }
        CHECK(found);
        if (i == 7) picture(dir, "owrx-band-longest");
    }
    printf("  the band chooser: its widest label %d px of the panel's 290\n", widest + 1);
    CHECK(shown("CB") && shown("moves 2 more"));
    where = "...a tap on the panel asks for it";
    tap_at(PANEL_X, PANEL_Y, 80); run(100);
    {
        ui_commit_t c;
        CHECK(commit(&c) && c.have_band && c.band == 8);
    }
    after();

    where = "...too soon after the last: greyed, how long said";
    st.band_wait_s = 7; run(100);
    tap_at(180 - 76, 122, 80); run(100);
    CHECK(ui_edit_active() && value_is("40m") && shown("RSPdx  7.000-7.200  -  in use"));
    ui_edit_rotate(-1); run(100);
    {
        lv_obj_t *v = value_is("160m");
        CHECK(v && lv_color_eq(lv_obj_get_style_text_color(v, 0), lv_color_hex(OW_DIM)));
        CHECK(shown("RSPdx  1.810-2.000  -  in 7 s"));
    }
    picture(dir, "owrx-band-wait");
    after();
    st.band_wait_s = 0; run(60);

    where = "...the one in use, tapped: no change asked";
    tap_at(180 - 76, 122, 80); run(100);
    CHECK(ui_edit_active() && value_is("40m"));
    tap_at(PANEL_X, PANEL_Y, 80); run(100);
    {
        ui_commit_t c;
        commit(&c);
        CHECK(!c.have_band);
    }
    after();

    where = "...alone on it: no others to move";
    st.users = 1; run(60);
    swipe(180, 70, 180, 230); run(100);
    ui_edit_rotate(1); run(100);
    CHECK(shown("RSPdx  14.000-14.350  -  tap to switch"));
    after();
    st.users = 3; run(60);

    where = "the band's name on the face, fitted to its place beside the mode";
    strcpy(st.band_name, "Broadcast FM Wide Band"); run(100);
    {
        lv_obj_t *b = label_from(lv_screen_active(), "Broa");
        lv_obj_t *m = label_is(lv_screen_active(), "USB");
        CHECK(b && m);
        if (b && m) {
            lv_area_t ab, am;
            lv_obj_get_coords(b, &ab);
            lv_obj_get_coords(m, &am);
            CHECK(ab.x2 - ab.x1 + 1 <= 92 && ab.x2 + 4 <= am.x1);
            CHECK(ab.x1 >= 180 - 168);
        }
    }
    picture(dir, "owrx-band-name-long");
    strcpy(st.band_name, "40m"); run(100);
    CHECK(shown("40m"));

    where = "the slab: a tap brings the receivers";
    tap_at(SLAB_X, SLAB_Y, 80); run(100);
    CHECK(ui_edit_active() && shown("RECEIVER") && shown("ON4PRA WebSDR"));
    after();
    where = "...and so does a swipe up";
    swipe(180, 230, 180, 70); run(100);
    CHECK(ui_edit_active() && shown("RECEIVER"));
    after();

    where = "the modes: SAM on OpenWebRX+ only";
    tap_at(180, 122, 80); run(100);
    CHECK(ui_edit_active() && shown("MODE"));
    ui_edit_rotate(4); run(100);
    CHECK(shown("SAM"));
    after();
    st.has_sam = false; run(60);
    tap_at(180, 122, 80); run(100);
    ui_edit_rotate(4); run(100);
    CHECK(shown("NFM") && !shown("SAM"));
    after();
    st.has_sam = true; run(60);

    where = "the filter: a sideband from 150 Hz off the carrier on OpenWebRX+";
    tap_at(180 + 76, 122, 80); run(100);
    CHECK(ui_edit_active() && shown("FILTER"));
    ui_edit_rotate(1); run(100);
    {
        ui_commit_t c;
        commit(&c);
        CHECK(c.have_filter && c.filt_lo == 150 && c.filt_hi == 150 + 3000);
    }
    after();

    /* The knob's own battery over this face, as on every face. */
    knob_sweep(dir, "owrx");
    lock_sweep(dir, "owrx");
    knob_radio(dir, "owrx");

    /* The receiver's name on the slab beside a device's logo and battery. */
    st.headset = true; st.have_batt = true; st.batt = 80; run(60);
    call_check("ON4PRA WebSDR", NULL);
    batt_check(dir, "owrx-batt", false);
    st.headset = false; st.have_batt = false; run(60);
}
#elif VFO_RADIO_WEBSDR
/* The WebSDR face: Kiwi888's slab and S-meter -- dBm, in S-units -- its own
 * squelch, on or off, where the gain is, nothing where the AGC is, and its
 * bands, or a one-band site's band plan, a swipe down away. */
static lv_obj_t *value_is(const char *text) { return label_is(lv_screen_active(), text); }

static void wsdr_base(void)
{
    base();
    st.freq_hz = 14074000; st.mode = "usb"; st.filt_lo = 300; st.filt_hi = 2700;
    st.step_hz = 100;
    st.smeter_dbm = -79.0f;
    st.users = -1; st.squelch_pct = 0; st.has_squelch = st.have_squelch = true;
    st.band_sel = 3; st.has_sam = true;
    strcpy(st.band_name, "hf");
    strcpy(st.server, "Twente");
    st.rx_line2 = "20 m  hf";
    st.rx_line3 = "WebSDR";
    st.n_radios = 2; st.n_radios_direct = 2; st.radio_sel = 0;
    strcpy(st.radio_name[0], "Twente");
    strcpy(st.radio_name[1], "Maasbree");
}

/* A one-band site's band plan, as the client names it: no SDR, a range. */
static const ui_band_t PLAN[] = {
    { "160 m", "", "1.810-1.880" }, { "80 m", "", "3.500-3.800" },   { "49 m BC", "", "5.900-6.200" },
    { "20 m", "", "14.000-14.350" }, { "10 m", "", "28.000-29.700" },
};
#define N_PLAN ((int)(sizeof PLAN / sizeof *PLAN))

static void runs(const char *dir)
{
    wsdr_base(); run(5000);                         /* the peak from the harness's first 0 dBm, falling 30 dB/s */

    where = "the face: S-units and dBm, the site's squelch, nothing where the AGC is";
    /* Eased toward the reading and cut, not rounded (ui.c): -78.99 reads -78. */
    CHECK(shown("S8") && (shown("-79 dBm") || shown("-78 dBm")));
    CHECK(shown("SQL") && shown("OPEN"));
    CHECK(!shown("AGC") && !shown("USERS"));
    CHECK(shown("hf") && shown("USB") && shown("2400"));
    CHECK(shown("Twente") && shown("20 m  hf") && shown("WebSDR"));
    picture(dir, "websdr-face");
    where = "...over S9, its dB over it";
    st.smeter_dbm = -53.0f; run(3000);
    CHECK(shown("S9+20") && shown("-53 dBm"));
    st.smeter_dbm = -79.0f; run(3000);

    where = "the squelch: where the gain is, tapped; on or off";
    tap_at(180 + 72, 88, 80); run(100);
    CHECK(ui_edit_active() && shown("SQUELCH") && shown("OPEN"));
    ui_edit_rotate(1); run(100);
    CHECK(value_is("ON"));
    {
        ui_commit_t c;
        commit(&c);
        CHECK(c.have_squelch && c.squelch_pct == 100);
    }
    ui_edit_rotate(3); run(100);
    CHECK(value_is("ON"));
    ui_edit_rotate(-1); run(100);
    CHECK(shown("OPEN"));
    {
        ui_commit_t c;
        commit(&c);
        CHECK(c.have_squelch && c.squelch_pct == 0);
    }
    picture(dir, "websdr-squelch");
    tap_at(OUTSIDE_X, OUTSIDE_Y, 80); settle(); quiet();
    st.squelch_pct = 100; run(100);
    CHECK(shown("ON") && !shown("OPEN"));
    st.squelch_pct = 0; run(100);
    where = "...the AGC's place takes no tap";
    tap_at(180 - 72, 88, 80); run(100);
    CHECK(!ui_edit_active());
    after();

    where = "no bands yet: the swipe down opens nothing";
    swipe(180, 70, 180, 230); run(100);
    CHECK(!ui_edit_active());
    after();

    where = "the band plan: a swipe down, on the range the dial is in";
    CHECK(ui_set_bands(PLAN, N_PLAN));
    run(60);
    swipe(180, 70, 180, 230); run(100);
    CHECK(ui_edit_active() && shown("BAND 4 / 5") && value_is("20 m") && shown("14.000-14.350  -  in use"));
    where = "...another: any, any time -- nobody else moved";
    ui_edit_rotate(1); run(100);
    CHECK(value_is("10 m") && shown("28.000-29.700  -  tap to switch"));
    {
        lv_obj_t *v = value_is("10 m");
        CHECK(v && !lv_color_eq(lv_obj_get_style_text_color(v, 0), lv_color_hex(0x575757)));
    }
    picture(dir, "websdr-band");
    tap_at(PANEL_X, PANEL_Y, 80); run(100);
    {
        ui_commit_t c;
        CHECK(commit(&c) && c.have_band && c.band == 4);
    }
    after();
    where = "...a broadcast band's name whole";
    swipe(180, 70, 180, 230); run(100);
    ui_edit_rotate(-1); run(100);
    CHECK(value_is("49 m BC") && shown("5.900-6.200  -  tap to switch"));
    after();

    where = "the band's name on the face: the site's";
    strcpy(st.band_name, "80m"); run(100);
    CHECK(shown("80m"));
    strcpy(st.band_name, "hf"); run(100);

    where = "the slab: a tap brings the receivers";
    tap_at(SLAB_X, SLAB_Y, 80); run(100);
    CHECK(ui_edit_active() && shown("RECEIVER") && shown("Twente"));
    after();

    where = "the modes: SAM on Twente's server only, no WFM";
    tap_at(180, 122, 80); run(100);
    CHECK(ui_edit_active() && shown("MODE"));
    ui_edit_rotate(4); run(100);
    CHECK(shown("SAM"));
    ui_edit_rotate(1); run(100);
    CHECK(shown("NFM"));
    ui_edit_rotate(1); run(100);
    CHECK(shown("NFM") && !shown("WFM"));
    after();
    st.has_sam = false; run(60);
    tap_at(180, 122, 80); run(100);
    ui_edit_rotate(4); run(100);
    CHECK(shown("NFM") && !shown("SAM"));
    after();
    st.has_sam = true; run(60);

    where = "the filter: a sideband from 300 Hz off the carrier, as its page";
    tap_at(180 + 76, 122, 80); run(100);
    CHECK(ui_edit_active() && shown("FILTER"));
    ui_edit_rotate(1); run(100);
    {
        ui_commit_t c;
        commit(&c);
        CHECK(c.have_filter && c.filt_lo == 300 && c.filt_hi == 300 + 2700);
    }
    after();
    where = "...CW around its 750 Hz tone below the carrier";
    st.mode = "cw"; st.filt_lo = -950; st.filt_hi = -550; run(100);
    tap_at(180 + 76, 122, 80); run(100);
    ui_edit_rotate(1); run(100);
    {
        ui_commit_t c;
        commit(&c);
        CHECK(c.have_filter && c.filt_lo == -750 - 250 && c.filt_hi == -750 + 250);
    }
    after();
    st.mode = "usb"; st.filt_lo = 300; st.filt_hi = 2700; run(100);

    /* The knob's own battery over this face, as on every face. */
    knob_sweep(dir, "websdr");
    lock_sweep(dir, "websdr");
    knob_radio(dir, "websdr");

    st.headset = true; st.have_batt = true; st.batt = 80; run(60);
    call_check("Twente", NULL);
    batt_check(dir, "websdr-batt", false);
    st.headset = false; st.have_batt = false; run(60);
}
#elif VFO_RADIO_EPIBEAM
/* EpiBeam's compass (ui.c EPI_FACE): an array's directions round the ring, 4
 * or 8, the beam's in the middle with its heading. A detent a direction, round
 * and round, taken once the knob has rested -- once, and where it stopped; a
 * tap on the ring that direction, nearer the middle nothing, nor any PTT or
 * swipe. The gear: a tap the card, again away; held three seconds the
 * firmware picker. Locked, the ring steers nothing and the gear still brings
 * the card, UNLOCK on it. Pictures: 4 directions and 8. */
#define DEG "\xC2\xB0"
#define GEAR_X 180                       /* ui.c BEAM_GEAR_Y: CY + 84 */
#define GEAR_Y 264
#define RING_R 155                       /* the middle of the ring of directions */

/* A tap on the ring at `deg` clockwise from north. */
static void ring_tap(int deg)
{
    const double a = deg * 3.14159265358979 / 180.0;
    tap_at(W / 2 + (int)lround(RING_R * sin(a)), H / 2 - (int)lround(RING_R * cos(a)), 80);
}
static int taken(void) { run(450); return ui_take_beam(); }

static void runs(const char *dir)
{
    base();
    st.link_ok = false;
    ui_set_netinfo("EpiBeam 1.21.8\nUSB power\nUSB   -\nWiFi  192.168.254.248\nsetup  http://192.168.254.248");
    run(120);
    (void)ui_take_beam(); (void)ui_take_beam_moved(); quiet();

    where = "4: N lit, in the middle with its heading, the four round the ring";
    CHECK(shown("4-SQUARE") && shown("N") && shown("0" DEG) && shown("EpiCenter: not connected"));
    CHECK(shown("E") && shown("S") && shown("W") && !shown("NE") && !shown("NW"));
    picture(dir, "epibeam-4");

    where = "...a detent: E at once, taken once the knob has rested -- once";
    ui_beam_turn(1); run(30);
    CHECK(ui_take_beam_moved() && shown("90" DEG));
    CHECK(ui_take_beam() == -1);
    CHECK(taken() == 1);
    CHECK(ui_take_beam() == -1 && !ui_take_beam_moved());
    where = "...at an even tick as at an odd one: never taken before the knob has rested";
    for (int i = 0; i < 4; i++) {
        ui_beam_turn(1); run(16);
        CHECK(ui_take_beam() == -1);
        run(451);
        CHECK(ui_take_beam() >= 0);
    }
    ui_beam_turn(-4);
    CHECK(taken() == 1);
    where = "...three back, past N: S";
    ui_beam_turn(-3);
    CHECK(taken() == 2 && shown("S") && shown("180" DEG));
    where = "...turned on before it rests: taken once, where it stopped";
    ui_beam_turn(1); run(200);
    ui_beam_turn(1); run(200);
    CHECK(ui_take_beam() == -1);
    run(250);
    CHECK(ui_take_beam() == 0 && shown("0" DEG));

    where = "...a tap on the ring: W; on E: E";
    ring_tap(270);
    CHECK(taken() == 3 && shown("270" DEG));
    ring_tap(85);
    CHECK(taken() == 1);
    where = "...nearer the middle, or where the slab would be: nothing, and no PTT";
    tap_at(W / 2 - 60, H / 2 - 20, 80);
    CHECK(taken() == -1);
    tap_at(W / 2 + 70, 270, 80);
    CHECK(taken() == -1 && !ptt());
    where = "...swipes: nothing";
    swipe(340, 180, 140, 180); run(60);
    swipe(180, 20, 180, 220); run(60);
    CHECK(taken() == -1 && !ui_edit_active());

    where = "the gear: a tap, the card and its click; again, away";
    tap_at(GEAR_X, GEAR_Y, 80); run(30);
    CHECK(label_from(lv_screen_active(), "EpiBeam 1.21.8") && ui_take_card_shown());
    picture(dir, "epibeam-card");
    tap_at(GEAR_X, GEAR_Y, 80); run(30);
    CHECK(!label_from(lv_screen_active(), "EpiBeam 1.21.8"));
    CHECK(taken() == -1);
    where = "...held three seconds: the firmware picker asked for, and no card";
    down(GEAR_X, GEAR_Y); run(3100);
    CHECK(ui_take_picker_request());
    up(); run(SEEN); run(60);
    CHECK(!label_from(lv_screen_active(), "EpiBeam 1.21.8") && taken() == -1);

    where = "locked, from the card's LOCK: the ring steers nothing";
    tap_at(GEAR_X, GEAR_Y, 80); run(700);
    CHECK(shown("LOCK"));
    tap_at(LOCK_X, LOCK_Y, 60); run(60);
    CHECK(ui_locked());
    run(10500);
    where = "...the padlock inside the ring, under N, clear of the caption";
    {
        lv_obj_t *lk = NULL, *scr = lv_screen_active();
        for (uint32_t i = 0; i < lv_obj_get_child_count(scr); i++) {
            lv_obj_t *o = lv_obj_get_child(scr, (int32_t)i);
            if (lv_obj_check_type(o, &lv_label_class) && visible(o) &&
                !strcmp(lv_label_get_text(o), "\xEF\x80\xA3"))
                lk = o;
        }
        box_t k;
        int room = 0;
        CHECK(lk && ink_of(lk, &k, &room));
        if (lk && ink_of(lk, &k, &room)) {
            CHECK(k.y1 >= KNOB_INK_TOP && k.y2 <= KNOB_INK_FOOT && room >= ROOM);
            printf("  %-62s ink %d-%d, %d-%d  %2d px clear\n", where, k.x1, k.x2, k.y1, k.y2, room);
        }
    }
    picture(dir, "epibeam-locked");
    ring_tap(180);
    CHECK(taken() == -1 && shown("90" DEG));
    where = "...the gear still brings the card, and UNLOCK on it";
    tap_at(GEAR_X, GEAR_Y, 80); run(700);
    CHECK(shown("UNLOCK"));
    tap_at(LOCK_X, LOCK_Y, 60); run(60);
    CHECK(!ui_locked());
    run(10500);
    ring_tap(180);
    CHECK(taken() == 2);

    where = "8: the corners too; a detent from N is NE, a tap on SW is SW";
    ui_beam_set(8, 0); run(60);
    CHECK(shown("8 DIRECTIONS") && shown("NE") && shown("SE") && shown("SW") && shown("NW") && shown("0" DEG));
    ui_beam_turn(1);
    CHECK(taken() == 1 && shown("45" DEG));
    ring_tap(225);
    CHECK(taken() == 5 && shown("225" DEG));
    picture(dir, "epibeam-8");
    where = "...set from the page while it turns: nothing owed, nothing taken";
    ui_beam_turn(1); run(30);
    ui_beam_set(4, 1);
    CHECK(taken() == -1 && shown("4-SQUARE") && shown("90" DEG));

    where = "EpiCenter linked: said so, in green";
    st.link_ok = true; run(120);
    CHECK(shown("EpiCenter") && !shown("EpiCenter: not connected"));
    st.link_ok = false; run(120);

    where = "the knob's battery, on its own power: inside the ring, clear of N lit";
    ui_beam_set(4, 0);
    st.knob_batt = true; st.knob_pct = 85; run(60);
    knob_check(dir, "epibeam-batt");
    where = "...at 15 %, red";
    st.knob_pct = 15; run(60);
    knob_check(dir, NULL);
    st.knob_pct = 85;
    where = "...and the padlock beside it, locked";
    tap_at(GEAR_X, GEAR_Y, 80); run(700);
    tap_at(LOCK_X, LOCK_Y, 60); run(60);
    CHECK(ui_locked());
    run(10500);
    knob_check(dir, "epibeam-batt-locked");
    tap_at(GEAR_X, GEAR_Y, 80); run(700);
    tap_at(LOCK_X, LOCK_Y, 60); run(60);
    CHECK(!ui_locked());
    run(10500);
    st.knob_batt = false; run(60);
    (void)ui_take_beam_moved(); quiet();
}
#elif VFO_RADIO_CLOCK
/* The Clock's face (ui.c CLOCK_FACE): clockface.c's faces, the shack clock
 * until the page or the knob chooses another. The glass is the face as
 * clockface.c draws it whole -- give or take the burn-in's pixel -- and a
 * tick sends the hands' box alone. A detent a face, round and round, kept
 * once the knob has rested -- once, and where it stopped; the page's face
 * and second hand drawn at once. A tap anywhere: the card, again away; held
 * three seconds the firmware picker; no PTT, nothing a swipe away. The
 * knob's battery only once it is low. The alarms: the next on the dial, a
 * tap on the middle their list, set with the dial and taps, switched by
 * their pill, closed by a tap elsewhere or by itself; one due rings over
 * everything, locked or not -- a tap stops it, a turn snoozes it and the
 * rest of that turn moves no face, STOP on the list ends a snooze, left
 * alone it stops; once, it is off as it rings. Pictures: each face, the
 * card, no time yet, the low battery, the alarms' list and setting, one
 * ringing, one snoozed. */
static uint16_t want[W * H];
static int ck_dx, ck_dy;
static char ck_mark[20];                 /* the dial's mark, as ui.c should have it */

/* The pixels of the round glass that differ from the face drawn whole now,
 * at the best of the burn-in's nine shifts: ck_dx, ck_dy. */
static int face_diff(uint8_t face, bool seconds)
{
    cf_opts_t o = { .face = face, .seconds = seconds, .h24 = true };
    strcpy(o.mark, ck_mark);
    lv_area_t a;
    clockface_draw(want, &o, &a);
    lv_obj_invalidate(lv_screen_active());
    lv_refr_now(disp);
    int best = W * H;
    for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++) {
            int n = 0;
            for (int y = 2; y < H - 2; y++)
                for (int x = 2; x < W - 2; x++) {
                    const int u = x - W / 2, v = y - H / 2;
                    if (u * u + v * v <= 168 * 168) n += fb[y * W + x] != want[(y - dy) * W + (x - dx)];
                }
            if (n < best) { best = n; ck_dx = dx; ck_dy = dy; }
        }
    if (best) printf("  [%s] %d pixels differ\n", where, best);
    return best;
}
/* To a tenth into the next second: its tick drawn, the Diver's hand not
 * yet a sixth on. */
static void to_tenth(void) { run(1000 - now_ms % 1000 + 100); }
/* The face the knob rested on, kept as app_main keeps it. */
static int taken(void)
{
    run(650);
    const int f = ui_take_clock_face();
    if (f >= 0) net_prov_save_clock((uint8_t)f, net_prov_clock_seconds());
    return f;
}
#define CK_CARD "Clock 1.23.2"
#define AL_FLASH_PIC 120                 /* into the ringing's first flash: the red ring on */

/* The next minute, local, from now: rung at its start. */
static int next_minute(void)
{
    struct tm lt;
    wallclock_now(&lt, NULL, NULL);
    return (lt.tm_hour * 60 + lt.tm_min + 1) % 1440;
}
static void to_minute(void)
{
    struct tm lt;
    wallclock_now(&lt, NULL, NULL);
    run((uint32_t)(60 - lt.tm_sec) * 1000u + 150u);
}

static void alarms(const char *dir)
{
    net_alarms_t al = { .snooze_min = 1, .ring_min = 1, .volume = 60 }, got;
    char hm[16];

    where = "the alarms: one on, Monday to Friday at 07:30 -- the dial carries it, with a bell";
    al.a[0] = (alarm_t){ 7, 30, ALARM_WORK, true };
    al.a[1] = (alarm_t){ 6, 0, 0, false };
    ui_alarms_set(&al); run(100);
    strcpy(ck_mark, LV_SYMBOL_BELL " 07:30");
    to_tenth();
    CHECK(face_diff(CF_SHACK, true) == 0);
    picture(dir, "clock-alarm-mark");
    where = "...on the other faces too, each where it has room";
    {
        static const struct { uint8_t f; const char *pic; } F[] = {
            { CF_DIVER, "clock-alarm-mark-diver" }, { CF_CLASSIC, "clock-alarm-mark-classic" },
            { CF_DIGITAL, "clock-alarm-mark-digital" },
        };
        for (size_t i = 0; i < sizeof F / sizeof *F; i++) {
            net_prov_save_clock(F[i].f, true);
            ui_clock_changed(); run(100);
            to_tenth();
            CHECK(face_diff(F[i].f, true) == 0);
            picture(dir, F[i].pic);
        }
        net_prov_save_clock(CF_SHACK, true);
        ui_clock_changed(); run(100);
    }

    where = "a tap on the clock's middle: the list, on the next to ring; no card";
    tap_at(180, 180, 80); run(60);
    CHECK(shown("ALARM 1") && shown("07") && shown("30") && shown("MON-FRI") && shown("ON"));
    CHECK(!label_from(lv_screen_active(), CK_CARD));
    picture(dir, "clock-alarms");
    where = "...a detent: the second, off, once; the face stays";
    ui_clock_turn(1); run(60);
    CHECK(shown("ALARM 2") && shown("06") && shown("00") && shown("ONCE") && shown("OFF"));
    run(700);
    CHECK(ui_take_clock_face() == -1);
    where = "...its pill: on, and kept";
    tap_at(180, 287, 80); run(60);
    CHECK(shown("ON") && ui_take_alarms(&got) && got.a[1].on && got.a[0].on);
    CHECK(!ui_take_alarms(&got));
    where = "...its time: the hours with the dial, a tap, the minutes, a tap, the days";
    tap_at(180, 160, 80); run(60);
    CHECK(shown("HOURS"));
    ui_clock_turn(3); run(60);
    CHECK(shown("09") && !shown("ON") && !shown("OFF"));
    tap_at(180, 160, 80); run(60);
    CHECK(shown("MINUTES"));
    ui_clock_turn(-5); run(60);
    CHECK(shown("55"));
    tap_at(100, 300, 80); run(60);
    CHECK(shown("DAYS"));
    ui_clock_turn(2); run(60);
    CHECK(shown("MON-FRI"));
    ui_clock_turn(-3); run(60);
    CHECK(shown("SUN"));
    ui_clock_turn(1); run(60);
    CHECK(shown("ONCE"));
    ui_clock_turn(2); run(60);
    picture(dir, "clock-alarm-set");
    CHECK(!ui_take_alarms(&got));
    where = "...a tap: kept, on";
    tap_at(180, 160, 80); run(60);
    CHECK(ui_take_alarms(&got) && got.a[1].hour == 9 && got.a[1].min == 55 && got.a[1].days == ALARM_WORK &&
          got.a[1].on);
    CHECK(shown("ALARM 2") && shown("ON") && shown("MON-FRI"));
    where = "...a tap elsewhere: the clock back, no card, the next on its dial";
    tap_at(180, 30, 80); run(60);
    CHECK(!shown("ALARM 2") && !label_from(lv_screen_active(), CK_CARD));
    to_tenth();
    CHECK(face_diff(CF_SHACK, true) == 0);
    where = "...left alone 30 s: the clock back by itself";
    tap_at(180, 180, 80); run(60);
    CHECK(shown("ALARM 1"));
    run(30500);
    CHECK(!shown("ALARM 1"));
    where = "...the card up: the middle puts it away, and opens nothing";
    tap_at(60, 180, 80); run(30);
    CHECK(label_from(lv_screen_active(), CK_CARD) && ui_take_card_shown());
    tap_at(180, 180, 80); run(30);
    CHECK(!label_from(lv_screen_active(), CK_CARD) && !shown("ALARM 1"));

    where = "due: it rings at its minute, over everything; once, so off as it rings";
    int m = next_minute();
    al.a[1] = got.a[1];
    al.a[2] = (alarm_t){ (uint8_t)(m / 60), (uint8_t)(m % 60), 0, true };
    ui_alarms_set(&al); run(100);
    CHECK(!ui_alarm_ringing());
    to_minute();
    snprintf(hm, sizeof hm, "%02d:%02d", m / 60, m % 60);
    CHECK(ui_alarm_ringing() && shown(hm) && shown("ONCE"));
    CHECK(ui_take_alarms(&got) && !got.a[2].on && got.a[1].on);
    run(AL_FLASH_PIC);
    picture(dir, "clock-ringing");
    where = "...a tap: stopped, and no card";
    tap_at(100, 100, 80); run(60);
    CHECK(!ui_alarm_ringing() && !shown(hm) && !label_from(lv_screen_active(), CK_CARD));

    where = "again: a turn snoozes it, SNOOZE on the dial; the rest of the turn moves no face";
    m = next_minute();
    al.a[2] = (alarm_t){ (uint8_t)(m / 60), (uint8_t)(m % 60), 0, true };
    ui_alarms_set(&al); run(100);
    to_minute();
    CHECK(ui_alarm_ringing() && ui_take_alarms(&got) && !got.a[2].on);
    ui_clock_turn(1); run(60);
    CHECK(!ui_alarm_ringing());
    ui_clock_turn(1); run(300);
    ui_clock_turn(1); run(700);
    CHECK(ui_take_clock_face() == -1);
    snprintf(ck_mark, sizeof ck_mark, "SNOOZE %02d:%02d", (m + 1) % 1440 / 60, (m + 1) % 60);
    to_tenth();
    CHECK(face_diff(CF_SHACK, true) == 0);
    picture(dir, "clock-snoozed");
    where = "...a snooze's minute on: ringing again";
    run(60000);
    CHECK(ui_alarm_ringing());
    where = "...snoozed again, the list on it: STOP ends the snooze; once, it is off";
    ui_clock_turn(1); run(1600);
    tap_at(180, 180, 80); run(60);
    CHECK(shown("ALARM 3") && shown("STOP"));
    tap_at(180, 287, 80); run(60);
    CHECK(shown("OFF") && !shown("STOP"));
    tap_at(180, 30, 80); run(60);
    run(61000);
    CHECK(!ui_alarm_ringing());
    strcpy(ck_mark, LV_SYMBOL_BELL " 07:30");
    to_tenth();
    CHECK(face_diff(CF_SHACK, true) == 0);

    where = "left alone: it stops after the page's minute";
    m = next_minute();
    al.a[2] = (alarm_t){ (uint8_t)(m / 60), (uint8_t)(m % 60), ALARM_DAILY, true };
    ui_alarms_set(&al); run(100);
    to_minute();
    CHECK(ui_alarm_ringing());
    run(61000);
    CHECK(!ui_alarm_ringing());
    CHECK(!ui_take_alarms(&got));                /* every day: still on, nothing to keep */

    where = "locked: the middle opens nothing; a ring still stops at a tap and snoozes at a turn";
    tap_at(60, 180, 80); run(700);
    tap_at(180, 84, 60); run(60);
    CHECK(ui_locked());
    run(10500);
    tap_at(180, 180, 80); run(60);
    CHECK(!shown("ALARM 1"));
    run(10500);
    m = next_minute();
    al.a[2] = (alarm_t){ (uint8_t)(m / 60), (uint8_t)(m % 60), 0, true };
    ui_alarms_set(&al); run(100);
    to_minute();
    CHECK(ui_alarm_ringing());
    tap_at(240, 240, 80); run(60);
    CHECK(!ui_alarm_ringing() && ui_locked());
    run(10500);
    tap_at(60, 180, 80); run(700);
    tap_at(180, 84, 60); run(60);
    CHECK(!ui_locked());
    run(10500);
    (void)ui_take_alarms(&got);
    al.a[2].on = false;
    ui_alarms_set(&al); run(100);
    strcpy(ck_mark, LV_SYMBOL_BELL " 07:30");
    quiet();
}

static void runs(const char *dir)
{
    base();
    ui_set_netinfo(CK_CARD "\nUSB power\nUSB   -\nWiFi  192.168.254.248\nsetup  http://192.168.254.248");
    run(1500);
    (void)ui_take_clock_face(); quiet();

    where = "the shack clock, as clockface.c draws it whole";
    to_tenth();
    CHECK(face_diff(CF_SHACK, true) == 0);
    picture(dir, "clock-shack");
    where = "...a minute of ticks: the hands' box alone sent";
    run(2000);
    flush_px = 0;
    run(60000);
    printf("  %-62s %ld px a second\n", where, flush_px / 60);
    CHECK(flush_px / 60 < W * H / 4);
    where = "...the face after them clockface.c's still";
    to_tenth();
    CHECK(face_diff(CF_SHACK, true) == 0);

    where = "a detent: the Diver at once, kept once the knob has rested -- once";
    ui_clock_turn(1); run(100);
    CHECK(ui_take_clock_face() == -1);
    CHECK(taken() == CF_DIVER);
    CHECK(ui_take_clock_face() == -1);
    to_tenth();
    CHECK(face_diff(CF_DIVER, true) == 0);
    picture(dir, "clock-diver");
    where = "...on: the classic white";
    ui_clock_turn(1);
    CHECK(taken() == CF_CLASSIC);
    to_tenth();
    CHECK(face_diff(CF_CLASSIC, true) == 0);
    picture(dir, "clock-classic");
    where = "...on: the digital";
    ui_clock_turn(1);
    CHECK(taken() == CF_DIGITAL);
    to_tenth();
    CHECK(face_diff(CF_DIGITAL, true) == 0);
    picture(dir, "clock-digital");
    where = "...on, round: the shack clock; three back: the Diver";
    ui_clock_turn(1);
    CHECK(taken() == CF_SHACK);
    ui_clock_turn(-3);
    CHECK(taken() == CF_DIVER);
    where = "...turned on before it rests: kept once, where it stopped";
    ui_clock_turn(1); run(300);
    ui_clock_turn(1); run(300);
    CHECK(ui_take_clock_face() == -1);
    CHECK(taken() == CF_DIGITAL);

    where = "the page: the classic white without its second hand, at once";
    net_prov_save_clock(CF_CLASSIC, false);
    ui_clock_changed(); run(100);
    to_tenth();
    CHECK(face_diff(CF_CLASSIC, false) == 0);
    where = "...the shack clock with it";
    net_prov_save_clock(CF_SHACK, true);
    ui_clock_changed(); run(100);
    to_tenth();
    CHECK(face_diff(CF_SHACK, true) == 0);
    CHECK(ui_take_clock_face() == -1);

    where = "a tap anywhere: the card, and its click; again, away; no PTT";
    tap_at(120, 250, 80); run(30);
    CHECK(label_from(lv_screen_active(), CK_CARD) && ui_take_card_shown());
    picture(dir, "clock-card");
    tap_at(250, 280, 80); run(30);
    CHECK(!label_from(lv_screen_active(), CK_CARD) && !ui_locked());
    CHECK(!ptt() && ui_take_clock_face() == -1);
    where = "...swipes: nothing";
    swipe(340, 180, 140, 180); run(60);
    swipe(180, 20, 180, 220); run(60);
    CHECK(!ui_edit_active() && !label_from(lv_screen_active(), CK_CARD) && ui_take_clock_face() == -1);
    where = "...held three seconds: the firmware picker asked for, and no card";
    down(180, 180); run(3100);
    CHECK(ui_take_picker_request());
    up(); run(SEEN); run(60);
    CHECK(!label_from(lv_screen_active(), CK_CARD));

    where = "the burn-in: three minutes on, the face a pixel along";
    to_tenth();
    CHECK(face_diff(CF_SHACK, true) == 0);
    {
        const int dx = ck_dx, dy = ck_dy;
        run(180000);
        to_tenth();
        CHECK(face_diff(CF_SHACK, true) == 0);
        CHECK((ck_dx != dx || ck_dy != dy) && abs(ck_dx - dx) <= 1 && abs(ck_dy - dy) <= 1);
    }

    where = "no time yet: the dial alone, NO TIME YET on it";
    clock_set = false;
    ui_clock_changed(); run(1100);
    CHECK(face_diff(CF_SHACK, true) == 0);
    picture(dir, "clock-notime");
    clock_set = true;
    ui_clock_changed(); run(1100);

    where = "the knob's battery: not over the 12 at 85 %; at 15 %, red";
    st.knob_batt = true; st.knob_pct = 85; run(60);
    CHECK(!knob_battery());
    st.knob_pct = 15; run(60);
    {
        lv_obj_t *b = knob_battery();
        CHECK(b && colour_is(lv_obj_get_style_text_color(b, LV_PART_MAIN)) == colour_for(15));
    }
    picture(dir, "clock-batt");
    st.knob_batt = false; run(60);
    quiet();
    alarms(dir);
}
#elif VFO_RADIO_WEATHER
/* The Weather's faces (ui.c WEATHER_FACE): before a forecast, the place and
 * "asking"; then NOW in the page's units, the dial on to the next 24 hours
 * and the 5 days and round, kept once the knob has rested; the icons, the
 * chart and the days drawn -- told by their colours on the glass; what went
 * wrong said at the foot; a tap the card, held the picker, no swipe; the
 * burn-in's pixel; the knob's battery between the time and the place. */
#define WX_C565(h) ((uint16_t)((((h) >> 19 & 31) << 11) | (((h) >> 10 & 63) << 5) | ((h) >> 3 & 31)))
#define WX_CARD "Weather 1.23.2"
#define WX_SUN   0xFFC83C                  /* ui.c's */
#define WX_MOON  0xE8E2C0
#define WX_CLOUD 0xDCE3EC
#define WX_BOLT  0xFFD23C
#define WX_LINE  0xFFA040
static wx_t sky;

/* Pixels of exactly that colour within a box of the glass. */
static int colour_px(uint32_t hex, int x1, int y1, int x2, int y2)
{
    const uint16_t c = WX_C565(hex);
    int n = 0;
    for (int y = y1; y <= y2; y++)
        for (int x = x1; x <= x2; x++) n += fb[y * W + x] == c;
    return n;
}

/* The rain's bars, blue at 60 % over the black. */
static int bluish_px(int x1, int y1, int x2, int y2)
{
    int n = 0;
    for (int y = y1; y <= y2; y++)
        for (int x = x1; x <= x2; x++) {
            const uint16_t c = fb[y * W + x];
            const int r = c >> 11, b = c & 31;
            n += b > 12 && b > 2 * r;
        }
    return n;
}

/* A sky over Brussels at 14:55 on 4 November: partly cloudy, rain likely in
 * the evening, a thunderstorm on the fifth day. */
static void make_sky(void)
{
    memset(&sky, 0, sizeof sky);
    sky.utc_offset_s = 3600;
    snprintf(sky.zone, sizeof sky.zone, "Europe/Brussels");
    sky.now = (wx_now_t){ .t = 1793800500, .temp_c = 9.7f, .feels_c = 5.4f, .wind_kmh = 19.1f, .gust_kmh = 38.5f,
                          .pressure_hpa = 1027.4f, .wind_deg = 58, .humidity = 63, .code = 2, .day = true };
    static const uint8_t RAIN[WX_HOURS] = { 0, 5, 0, 10, 20, 35, 60, 80, 75, 40, 10 };
    static const uint8_t CODE[WX_HOURS] = { 3, 0, 0, 2, 3, 61, 63, 65, 80, 51, 3 };
    sky.n_hours = WX_HOURS;
    for (int i = 0; i < WX_HOURS; i++)
        sky.hour[i] = (wx_hour_t){ .t = 1793800800 + 3600LL * i, .temp_c = 9.6f - 0.4f * (float)(i < 15 ? i : 30 - i),
                                   .rain_pct = RAIN[i], .code = CODE[i], .day = i < 1 || i > 16 };
    static const uint8_t DCODE[WX_DAYS] = { 3, 0, 3, 51, 95, 71, 45 };
    sky.n_days = WX_DAYS;
    for (int i = 0; i < WX_DAYS; i++)
        sky.day[i] = (wx_day_t){ .t = 1793746800 + 86400LL * i, .sunrise = 1793774364 + 86400LL * i,
                                 .sunset = 1793808740 + 86400LL * i, .max_c = 9.9f + (float)i * 0.3f,
                                 .min_c = 4.7f - (float)i, .rain_pct = (uint8_t)(i * 15), .code = DCODE[i] };
}

static int wx_taken(void) { run(650); return ui_take_weather_face(); }

static void runs(const char *dir)
{
    base();
    net_weather_t cfg = { .lat_e6 = NET_WX_HOME_LAT_E6, .lon_e6 = NET_WX_HOME_LON_E6, .place = NET_WX_HOME,
                          .temp_unit = WX_C, .wind_unit = WX_KMH, .refresh_min = 15 };
    ui_weather_set(&cfg);
    ui_set_netinfo(WX_CARD "\nUSB power\nUSB   -\nWiFi  192.168.254.248\nsetup  http://192.168.254.248");
    run(300);
    (void)ui_take_weather_face(); quiet();

    where = "before a forecast: the place, the time, asking";
    CHECK(shown(NET_WX_HOME) && shown("asking Open-Meteo...") && shown("--"));
    CHECK(label_from(lv_screen_active(), "14:5"));
    picture(dir, "weather-asking");

    where = "a forecast: now, in degrees C and km/h";
    make_sky();
    ui_weather_data(&sky); run(300);
    CHECK(shown("10\xC2\xB0" "C") && shown("feels like 5\xC2\xB0") && shown("Partly cloudy"));
    CHECK(shown("ENE  19 km/h, gusts 39"));
    CHECK(shown("humidity 63 %    1027 hPa"));
    CHECK(shown("sunrise 07:39    sunset 17:12"));
    CHECK(shown("Open-Meteo, 14:55") && !shown("asking Open-Meteo..."));
    picture(dir, "weather-now");
    where = "...its icon drawn: the sun and a cloud";
    CHECK(colour_px(WX_SUN, 52, 88, 143, 179) > 100 && colour_px(WX_CLOUD, 52, 88, 143, 179) > 300);
    where = "...the wind's arrow left of its words";
    {
        lv_obj_t *wl = label_from(lv_screen_active(), "ENE");
        lv_area_t a;
        CHECK(wl != NULL);
        if (wl) {
            lv_obj_get_coords(wl, &a);
            CHECK(colour_px(0xC0C8D4, a.x1 - 30, a.y1 - 4, a.x1 - 2, a.y2 + 4) > 20);
        }
    }

    where = "the page's units: F and knots; Beaufort";
    cfg.temp_unit = WX_F; cfg.wind_unit = WX_KN;
    ui_weather_set(&cfg); run(300);
    CHECK(shown("49\xC2\xB0" "F") && shown("feels like 42\xC2\xB0") && shown("ENE  10 kn, gusts 21"));
    cfg.wind_unit = WX_BFT;
    ui_weather_set(&cfg); run(300);
    CHECK(shown("ENE  3 Bft"));
    cfg.temp_unit = WX_C; cfg.wind_unit = WX_KMH;
    ui_weather_set(&cfg); run(300);
    CHECK(shown("10\xC2\xB0" "C"));

    where = "a long place's name: cut short, within the round where it is";
    {
        net_weather_t lc = cfg;
        snprintf(lc.place, sizeof lc.place, "Sint-Pieters-Leeuw, Flemish Brabant, Belgium");
        ui_weather_set(&lc); run(300);
        lv_obj_t *l = label_from(lv_screen_active(), "Sint-Pieters");
        CHECK(l != NULL);
        if (l) {
            lv_area_t a;
            lv_obj_get_coords(l, &a);
            /* Its glyphs, not its box: the ink between the box's ends. */
            int x1 = W, x2 = -1;
            for (int y = a.y1; y <= a.y2; y++)
                for (int x = 0; x < W; x++)
                    if (fb[y * W + x] != 0) { if (x < x1) x1 = x; if (x > x2) x2 = x; }
            CHECK(x1 >= 180 - 145 && x2 <= 180 + 145 && x2 - x1 > 200);
            /* One line: nothing between it and the icon, 88 down. */
            int below = 0;
            for (int y = a.y2 + 1; y < 88; y++)
                for (int x = 0; x < W; x++) below += fb[y * W + x] != 0;
            CHECK(a.y2 - a.y1 < 22 && below == 0);
        }
        picture(dir, "weather-longplace");
        ui_weather_set(&cfg); run(300);
        CHECK(shown(NET_WX_HOME));
    }

    where = "by night, clear: the moon, no sun";
    sky.now.day = false; sky.now.code = 0;
    ui_weather_data(&sky); run(300);
    CHECK(shown("Clear sky"));
    CHECK(colour_px(WX_MOON, 52, 88, 143, 179) > 100 && colour_px(WX_SUN, 52, 88, 143, 179) == 0);
    picture(dir, "weather-night");
    where = "each icon drawn";
    {
        static const uint8_t CODES[] = { 0, 2, 3, 45, 53, 57, 63, 73, 81, 95 };
        for (size_t i = 0; i < sizeof CODES; i++) {
            sky.now.day = true; sky.now.code = CODES[i];
            ui_weather_data(&sky); run(300);
            int ink = 0;
            for (int y = 88; y < 180; y++) for (int x = 52; x < 144; x++) ink += fb[y * W + x] != 0;
            CHECK(ink > 400);
            if (CODES[i] == 95) CHECK(colour_px(WX_BOLT, 52, 88, 143, 179) > 60);
            char name[32];
            snprintf(name, sizeof name, "weather-icon-%u", (unsigned)CODES[i]);
            picture(dir, name);
        }
        make_sky();
        ui_weather_data(&sky); run(300);
    }

    where = "a detent: the next 24 hours rolling in, kept once the knob has rested";
    ui_weather_turn(1); run(200);
    CHECK(shown("NEXT 24 HOURS") && shown("Partly cloudy"));        /* both, half way */
    {
        lv_obj_t *c = label_from(lv_screen_active(), "NEXT 24 HOURS"), *n = label_from(lv_screen_active(), "Partly cloudy");
        lv_area_t ca, na;
        if (c && n) {
            lv_obj_get_coords(c, &ca);
            lv_obj_get_coords(n, &na);
        }
        CHECK(c && n && ca.x1 > 60 && ca.x1 < 300 && na.x2 < 300);  /* the next from the right, now going left */
    }
    run(300);
    CHECK(shown("NEXT 24 HOURS") && !shown("Partly cloudy"));
    CHECK(ui_take_weather_face() == -1);
    CHECK(wx_taken() == 1);
    CHECK(shown("rain likely at 21:00, 60 %"));
    CHECK(colour_px(WX_LINE, 24, 120, 335, 250) > 150 && bluish_px(24, 120, 335, 250) > 300);
    picture(dir, "weather-hours");
    where = "...on: the 5 days; on, round: now; one back: the days";
    ui_weather_turn(1);
    CHECK(wx_taken() == 2 && shown("5 DAYS"));
    CHECK(colour_px(WX_BOLT, 40, 248, 320, 288) > 12);              /* Sunday's thunder */
    picture(dir, "weather-days");
    ui_weather_turn(1);
    CHECK(wx_taken() == 0 && shown("Partly cloudy"));
    ui_weather_turn(-1); run(200);
    {
        lv_obj_t *d = label_from(lv_screen_active(), "5 DAYS");
        lv_area_t da;
        if (d) lv_obj_get_coords(d, &da);
        CHECK(d && da.x2 < 180);                                     /* back: in from the left */
    }
    CHECK(wx_taken() == 2 && !shown("Partly cloudy"));
    where = "...a turn while one rolls: the first ends at once, the next rolls";
    ui_weather_turn(1); run(100);
    ui_weather_turn(1); run(500);
    CHECK(shown("NEXT 24 HOURS") && !shown("5 DAYS") && !shown("Partly cloudy"));
    CHECK(wx_taken() == 1);
    ui_weather_turn(1);
    CHECK(wx_taken() == 2);
    where = "...the page's face is the one the dial left";
    cfg.face = 2;
    ui_weather_set(&cfg); run(300);
    CHECK(shown("5 DAYS") && ui_take_weather_face() == -1);
    ui_weather_turn(1);
    CHECK(wx_taken() == 0);
    cfg.face = 0;
    ui_weather_set(&cfg); run(300);

    where = "the faces' own turn: every 3 s the next, round, never kept";
    cfg.cycle_s = 3;
    ui_weather_set(&cfg); run(300);
    run(2600);
    CHECK(shown("Partly cloudy"));
    run(300);
    CHECK(shown("NEXT 24 HOURS") && ui_take_weather_face() == -1);
    run(2700);
    CHECK(shown("NEXT 24 HOURS"));
    run(300);
    CHECK(shown("5 DAYS"));
    run(3000);
    CHECK(shown("Partly cloudy") && ui_take_weather_face() == -1);
    where = "...a detent: the next at once, kept, and 3 s from there";
    run(2000);
    ui_weather_turn(1); run(150);
    CHECK(shown("NEXT 24 HOURS"));
    run(2500);
    CHECK(shown("NEXT 24 HOURS") && ui_take_weather_face() == 1);
    run(500);
    CHECK(shown("5 DAYS") && ui_take_weather_face() == -1);
    where = "...under a screensaver none; the glass back, its first face its time";
    ui_screen_set(&(net_screen_t){ NET_SAVER_MATRIX, 1, 1, 1 });
    run(100);
    now_ms += 61000; ui_dim_tick(false); run(4000);
    CHECK(shown("5 DAYS"));
    tap_at(180, 180, 80); run(400);
    CHECK(shown("5 DAYS") && !label_from(lv_screen_active(), WX_CARD));
    run(2300);
    CHECK(shown("5 DAYS"));
    run(600);
    CHECK(shown("Partly cloudy"));
    ui_screen_set(&(net_screen_t){ NET_SAVER_NONE, 0, 0, 0 });
    run(200);
    quiet();
    where = "...0: never";
    cfg.cycle_s = 0;
    ui_weather_set(&cfg); run(300);
    run(10000);
    CHECK(shown("Partly cloudy") && ui_take_weather_face() == -1);

    where = "what went wrong, at the foot; and again right";
    ui_weather_state("no answer"); run(1100);
    CHECK(shown("Open-Meteo: no answer"));
    picture(dir, "weather-failing");
    ui_weather_state(""); run(1100);
    CHECK(shown("Open-Meteo, 14:55"));

    where = "a tap anywhere: the card, and its click; again, away; no PTT";
    tap_at(120, 250, 80); run(30);
    CHECK(label_from(lv_screen_active(), WX_CARD) && ui_take_card_shown());
    tap_at(250, 280, 80); run(30);
    CHECK(!label_from(lv_screen_active(), WX_CARD) && !ui_locked());
    CHECK(!ptt() && ui_take_weather_face() == -1);
    where = "...swipes: nothing";
    swipe(340, 180, 140, 180); run(60);
    swipe(180, 20, 180, 220); run(60);
    CHECK(!ui_edit_active() && !label_from(lv_screen_active(), WX_CARD) && ui_take_weather_face() == -1);
    where = "...held three seconds: the firmware picker asked for, and no card";
    down(180, 180); run(3100);
    CHECK(ui_take_picker_request());
    up(); run(SEEN); run(60);
    CHECK(!label_from(lv_screen_active(), WX_CARD));

    where = "the burn-in: three minutes on, the whole a pixel along";
    {
        lv_obj_t *c = label_from(lv_screen_active(), "Partly cloudy");
        lv_area_t a, b;
        CHECK(c != NULL);
        if (c) {
            lv_obj_get_coords(c, &a);
            run(180000);
            lv_obj_get_coords(c, &b);
            CHECK((a.x1 != b.x1 || a.y1 != b.y1) && abs(a.x1 - b.x1) <= 2 && abs(a.y1 - b.y1) <= 2);
        }
    }

    where = "the knob's battery, on its own power: between the time and the place";
    st.knob_batt = true; st.knob_pct = 85; run(60);
    knob_check(dir, "weather-batt");
    st.knob_pct = 15; run(60);
    knob_check(dir, NULL);
    st.knob_batt = false; run(60);
    quiet();
}
#elif VFO_RADIO_ADSB
/* ADS-B Radar's radar (ui.c ADSB_FACE): a sky round Brussels airport, the
 * aircraft nearest first as the client hands them over. The map under them
 * from the image's own tables; the range on the dial, a detent a range, held
 * at either end, taken once the knob has rested; an emergency and a type on
 * the page's list called out at the top and buzzed once; a tap on an
 * aircraft its card with the route, on the card away; the sweep, still under
 * a card; the filter; the gear, its card and the picker; locked, no card of
 * an aircraft; the knob's battery and the padlock clear of the map.
 * Pictures of each. */
#define GEAR_X 180                       /* ui.c AD_GEAR_Y: CY + 146 */
#define GEAR_Y 326
#define AD_R   172                       /* ui.c: the radar's edge */
#define LAT0   50.9014                   /* EBBR */
#define LON0   4.4844

typedef struct {
    const char *flight, *reg, *type, *squawk, *emerg;
    float nm, brg;                       /* where, from the middle */
    int32_t alt;                         /* ft; ADSB_ALT_NONE; -1 on the ground */
    int16_t gs, track, vrate;
    uint8_t flags;
    float seen;
} sky_t;

static const sky_t SKY[] = {
    { "BEL3PM",  "OO-SSC", "A320", "",     "",        0.6f, 200, -1,     12,  250, ADSB_VR_NONE, 0, 1 },
    { "UAE183",  "A6-EGO", "B77W", "",     "",        8,   150, 3200,  180,  250, -900, 0, 1 },
    { "BEL7JK",  "OO-SNA", "A320", "",     "",        12,  300, 12000, 310,  290, 2200, 0, 1 },
    { "",        "OO-ABC", "C172", "",     "",        15,  80,  1500,  95,   120, 0, 0, 2 },
    { "",        "",       "",     "",     "",        18,  170, 6000,  -1,   -1,  ADSB_VR_NONE, 0, 3 },
    { "WZZ1RS",  "HA-LVA", "A21N", "",     "",        20,  260, 35000, 450,  80,  0, 0, 1 },
    { "KLM1724", "PH-EZA", "E190", "",     "",        22,  20,  8500,  280,  200, -1500, 0, 1 },
    { "BAF589",  "CT-01",  "A400", "",     "",        25,  330, 18000, 330,  150, 0, ADSB_DB_MILITARY, 1 },
    { "EZY82QK", "G-EZOA", "A320", "7700", "general", 28,  240, 15000, 290,  60,  -2500, 0, 1 },
    { "RYR4TF",  "EI-DCL", "B738", "",     "",        30,  210, 37000, 455,  30,  0, 0, 1 },
    { "SWR781",  "HB-JCA", "BCS3", "",     "",        33,  130, 34000, 440,  310, 0, 0, 1 },
    { "TUI3TH",  "OO-TMA", "B38M", "",     "",        35,  60,  33000, 430,  240, 0, 0, 1 },
    { "AUA372",  "OE-LBM", "A320", "",     "",        37,  75,  30000, 420,  265, 0, 0, 1 },
    { "DLH9AB",  "D-AINA", "A20N", "",     "",        40,  100, 39000, 460,  270, 0, 0, 1 },
    { "AFR1234", "F-GRHA", "A319", "",     "",        42,  180, 28000, 400,  350, 0, 0, 40 },
    { "SAS1594", "SE-ROA", "A20N", "",     "",        44,  40,  32000, 450,  220, 0, 0, 1 },
    { "BAW412",  "G-EUXC", "A321", "",     "",        45,  290, 25000, 410,  110, 0, 0, 1 },
    { "TAP641",  "CS-TVA", "A20N", "",     "",        46,  220, 36000, 445,  40,  0, 0, 1 },
    { "EJA412",  "N412QS", "GLF6", "",     "",        47,  305, 45000, 480,  95,  0, ADSB_DB_INTERESTING, 1 },
};
#define N_SKY (int)(sizeof SKY / sizeof SKY[0])
static adsb_ac_t sky[N_SKY];

static void sky_make(void)
{
    for (int i = 0; i < N_SKY; i++) {
        const sky_t *s = &SKY[i];
        adsb_ac_t   *a = &sky[i];
        memset(a, 0, sizeof *a);
        snprintf(a->hex, sizeof a->hex, "4%05x", 0x4a10 + i * 37);
        strlcpy(a->flight, s->flight, sizeof a->flight);
        strlcpy(a->reg, s->reg, sizeof a->reg);
        strlcpy(a->type, s->type, sizeof a->type);
        strlcpy(a->squawk, s->squawk, sizeof a->squawk);
        strlcpy(a->emerg, s->emerg, sizeof a->emerg);
        a->ground = s->alt == -1;
        a->alt_ft = a->ground ? ADSB_ALT_NONE : s->alt;
        a->gs_kt = s->gs;
        a->track = s->track;
        a->vrate_fpm = s->vrate;
        a->db_flags = s->flags;
        a->seen_s = s->seen;
        const double b = s->brg * M_PI / 180.0;
        a->lat = LAT0 + s->nm * cos(b) / 60.0;
        a->lon = LON0 + s->nm * sin(b) / (60.0 * cos(LAT0 * M_PI / 180.0));
        adsb_dist_brg(LAT0, LON0, a->lat, a->lon, &a->dist_nm, &a->brg);
    }
}

/* A table of the image's, from the disk. */
static uint8_t *table(const char *name, size_t *n)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s", ADSB_TABLES, name);
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    *n = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *p = malloc(*n);
    if (p && fread(p, 1, *n, f) != *n) { free(p); p = NULL; }
    fclose(f);
    return p;
}

static net_adsb_t cfg;
static void set(void) { ui_adsb_set(&cfg); run(120); }

/* Where an aircraft of the sky is on the glass, at the range set now. */
static void sky_xy(int i, int *x, int *y)
{
    adsb_view_t v;
    adsb_view_set(&v, LAT0, LON0, adsb_range_nm(cfg.units, cfg.range), AD_R);
    float px, py;
    adsb_project(&v, sky[i].lat, sky[i].lon, &px, &py);
    *x = W / 2 + (int)lroundf(px);
    *y = H / 2 + (int)lroundf(py);
}
static int sky_find(const char *flight)
{
    for (int i = 0; i < N_SKY; i++)
        if (!strcmp(SKY[i].flight, flight)) return i;
    return -1;
}
static void sky_tap(const char *flight)
{
    int x, y;
    sky_xy(sky_find(flight), &x, &y);
    tap_at(x, y, 80);
    run(120);
}

/* Whether the face moves on its own: drawn, a while on, drawn again. */
static bool moving(void)
{
    render(with_b);
    run(200);
    render(without_b);
    return memcmp(with_b, without_b, sizeof with_b) != 0;
}

static void runs(const char *dir)
{
    base();
    size_t mn = 0, an = 0;
    const uint8_t *map = table("map.bin", &mn), *apt = table("airports.bin", &an);
    CHECK(map && apt);
    ui_adsb_tables(map, mn, apt, an);
    ui_set_netinfo("ADS-B Radar 1.22.0\nUSB power\nUSB   -\nWiFi  192.168.254.248\nsetup  http://192.168.254.248");
    sky_make();

    where = "no middle yet: an empty radar, the feed asking";
    memset(&cfg, 0, sizeof cfg);
    cfg.lat_e6 = cfg.lon_e6 = NET_ADSB_NO_POS;
    cfg.range = 4;
    cfg.opts = NET_ADSB_OPTS_DEFAULT;
    cfg.refresh_s = 10;
    strlcpy(cfg.types, "A400, C17", sizeof cfg.types);
    set();
    CHECK(shown("50 NM") && shown("asking"));
    CHECK(!moving());
    picture(dir, "adsb-nopos");

    where = "the middle on Brussels airport: the map round it, the sweep going round";
    cfg.lat_e6 = (int32_t)lround(LAT0 * 1e6);
    cfg.lon_e6 = (int32_t)lround(LON0 * 1e6);
    strlcpy(cfg.centre, "EBBR", sizeof cfg.centre);
    set();
    CHECK(moving());
    picture(dir, "adsb-map");

    where = "the sky: on the radar, the feed's name and how many at the foot";
    ui_adsb_feed("adsb.fi", true);
    ui_adsb_aircraft(sky, N_SKY);
    run(120);
    /* On the ground hidden: one fewer than the feed's. */
    char foot[48];
    snprintf(foot, sizeof foot, "adsb.fi  " LV_SYMBOL_BULLET "  %d", N_SKY - 1);
    CHECK(shown(foot));
    where = "...the emergency called out at the top, buzzed once";
    CHECK(label_from(lv_screen_active(), "EMERGENCY  EZY82QK"));
    CHECK(ui_take_adsb_alert() == ADSB_AL_EMERGENCY);
    ui_adsb_aircraft(sky, N_SKY); run(120);
    CHECK(ui_take_adsb_alert() == ADSB_AL_NONE);
    picture(dir, "adsb-radar");

    /* The sweep is painted by the face itself, only its box invalidated:
     * at eight places round a turn -- one with the trail across north --
     * what the display was sent step by step is the whole glass drawn
     * again, and stopped, it leaves not a pixel behind. */
    for (int k = 0; k < 8; k++) {
        where = "the sweep round a turn: each step's change sent, nothing of it left when stopped";
        run(450);
        memcpy(with_b, fb, sizeof fb);
        render(without_b);
        CHECK(!memcmp(with_b, without_b, sizeof fb));
        tap_at(GEAR_X, GEAR_Y, 80); run(120);                 /* the card stops it */
        render(with_b);
        ui_adsb_aircraft(sky, N_SKY); run(120);              /* the scene drawn again, from nothing */
        render(without_b);
        CHECK(!memcmp(with_b, without_b, sizeof fb));
        tap_at(GEAR_X, GEAR_Y, 80); run(120);
    }

    where = "a detent: 75 NM at once, a click, taken once the knob has rested";
    (void)ui_take_adsb_steps(); (void)ui_take_adsb_range();
    ui_adsb_turn(1); run(60);
    CHECK(shown("75 NM") && ui_take_adsb_steps() == 1);
    CHECK(ui_take_adsb_range() == -1);
    run(650);
    CHECK(ui_take_adsb_range() == 5 && ui_take_adsb_range() == -1);
    where = "...turned on before it rests: taken once, where it stopped";
    ui_adsb_turn(-1); run(300);
    ui_adsb_turn(-1); run(300);
    CHECK(ui_take_adsb_range() == -1 && shown("30 NM") && ui_take_adsb_steps() == 2);
    run(400);
    CHECK(ui_take_adsb_range() == 3);
    where = "...held at either end: 250 NM, and no click past it";
    ui_adsb_turn(20); run(60);
    CHECK(shown("250 NM") && ui_take_adsb_steps() == 1);
    ui_adsb_turn(1); run(60);
    CHECK(ui_take_adsb_steps() == 0);
    run(650);
    CHECK(ui_take_adsb_range() == 9);
    cfg.range = 9;
    picture(dir, "adsb-250");
    ui_adsb_turn(-20); run(700);
    CHECK(shown("5 NM") && ui_take_adsb_range() == 0);
    cfg.range = 0;
    picture(dir, "adsb-5");
    ui_adsb_turn(4); run(700);
    CHECK(shown("50 NM") && ui_take_adsb_range() == 4);
    cfg.range = 4;
    (void)ui_take_adsb_steps();

    where = "a tap on an aircraft: its card, its route asked for; the sweep still";
    sky_tap("UAE183");
    char cs[12] = "";
    CHECK(ui_adsb_card_callsign(cs, sizeof cs) && !strcmp(cs, "UAE183"));
    CHECK(shown("UAE183") && shown("B77W  " LV_SYMBOL_BULLET "  A6-EGO"));
    CHECK(shown("3200 ft " LV_SYMBOL_DOWN "  -900 ft/min"));
    CHECK(shown("180 kt  " LV_SYMBOL_BULLET "  250\xC2\xB0"));
    ui_adsb_route("UAE183", "", "", true); run(60);
    CHECK(shown("route ..."));
    ui_adsb_route("UAE183", "Dubai", "Brussels", false); run(60);
    CHECK(shown("Dubai  " LV_SYMBOL_RIGHT "  Brussels"));
    CHECK(!moving());
    picture(dir, "adsb-card");
    where = "...a tap on the card: away, the sweep on again";
    tap_at(W / 2, H / 2, 80); run(60);
    CHECK(!ui_adsb_card_callsign(cs, sizeof cs) && !shown("Dubai  " LV_SYMBOL_RIGHT "  Brussels"));
    CHECK(moving());
    where = "...the emergency's card: its squawk and what it is, in red";
    sky_tap("EZY82QK");
    CHECK(shown("EZY82QK") && shown("7700  EMERGENCY"));
    picture(dir, "adsb-card-emergency");
    where = "...a tap where no aircraft is: away";
    tap_at(W / 2 + 120, H / 2 - 40, 80); run(60);
    CHECK(!shown("7700  EMERGENCY"));
    where = "...left open: gone on its own after 20 s";
    sky_tap("BEL7JK");
    CHECK(shown("BEL7JK"));
    run(20500);
    CHECK(!ui_adsb_card_callsign(cs, sizeof cs));
    where = "...one without a callsign: its registration, no route to ask";
    sky_tap("");
    CHECK(shown("OO-ABC") && !ui_adsb_card_callsign(cs, sizeof cs));
    tap_at(W / 2, H / 2, 80); run(60);

    where = "the type on the page's list: gold, called out when no emergency is";
    sky[sky_find("EZY82QK")].squawk[0] = 0;
    sky[sky_find("EZY82QK")].emerg[0] = 0;
    ui_adsb_aircraft(sky, N_SKY); run(120);
    CHECK(label_from(lv_screen_active(), "A400  BAF589"));
    CHECK(ui_take_adsb_alert() == ADSB_AL_NONE);   /* seen before, as an alert */
    picture(dir, "adsb-type");
    strlcpy(sky[sky_find("EZY82QK")].squawk, "7700", 5);
    strlcpy(sky[sky_find("EZY82QK")].emerg, "general", 12);

    where = "military only: the A400, and the emergency, which always shows";
    cfg.show = ADSB_SHOW_MILITARY; set();
    ui_adsb_aircraft(sky, N_SKY); run(120);
    CHECK(shown("50 NM  " LV_SYMBOL_BULLET "  MIL"));
    CHECK(shown("adsb.fi  " LV_SYMBOL_BULLET "  2"));
    picture(dir, "adsb-mil");
    cfg.show = ADSB_SHOW_ALL;
    where = "...no airports, no countries, no sweep: the rings and the sky";
    cfg.opts = 0; set();
    ui_adsb_aircraft(sky, N_SKY); run(120);
    CHECK(!moving());
    picture(dir, "adsb-plain");
    cfg.opts = NET_ADSB_OPTS_DEFAULT;
    where = "...in kilometres: 100 km";
    cfg.units = ADSB_KM; set();
    CHECK(shown("100 km"));
    cfg.units = ADSB_NM; set();
    where = "...the feed failing: said so at the foot, in the warning's colour";
    ui_adsb_feed("HTTP 429", false); run(60);
    CHECK(shown("HTTP 429"));
    picture(dir, "adsb-down");
    ui_adsb_feed("adsb.fi", true);
    ui_adsb_aircraft(sky, N_SKY); run(120);
    (void)ui_take_adsb_alert();

    where = "a new middle from the page: the aircraft round the old one gone, and their card";
    sky_tap("UAE183");
    cfg.lat_e6 += 1000000; set();
    CHECK(shown("adsb.fi") && !label_from(lv_screen_active(), "EMERGENCY"));
    CHECK(!ui_adsb_card_callsign(cs, sizeof cs));
    where = "...the same middle again: the aircraft stay";
    cfg.lat_e6 -= 1000000; set();
    ui_adsb_aircraft(sky, N_SKY); run(120);
    set();
    CHECK(shown(foot) && label_from(lv_screen_active(), "EMERGENCY"));
    (void)ui_take_adsb_alert();

    where = "the gear: a tap, the card and its click; again, away";
    tap_at(GEAR_X, GEAR_Y, 80); run(30);
    CHECK(label_from(lv_screen_active(), "ADS-B Radar 1.22.0") && ui_take_card_shown());
    picture(dir, "adsb-gear");
    tap_at(GEAR_X, GEAR_Y, 80); run(30);
    CHECK(!label_from(lv_screen_active(), "ADS-B Radar 1.22.0"));
    where = "...held three seconds: the firmware picker asked for, and no card";
    down(GEAR_X, GEAR_Y); run(3100);
    CHECK(ui_take_picker_request());
    up(); run(SEEN); run(60);
    CHECK(!label_from(lv_screen_active(), "ADS-B Radar 1.22.0"));
    where = "...no PTT, nor a swipe that does anything";
    tap_at(W / 2, 290, 80);
    swipe(340, 180, 140, 180); run(60);
    swipe(180, 20, 180, 220); run(60);
    CHECK(!ptt() && !ui_edit_active());

    where = "locked, from the card's LOCK: a tap on an aircraft no card";
    tap_at(GEAR_X, GEAR_Y, 80); run(700);
    CHECK(shown("LOCK"));
    tap_at(LOCK_X, LOCK_Y, 60); run(60);
    CHECK(ui_locked());
    run(10500);
    sky_tap("UAE183");
    CHECK(!ui_adsb_card_callsign(cs, sizeof cs));
    where = "...the padlock under the range, clear of the map";
    {
        lv_obj_t *lk = padlock();
        box_t k;
        int room = 0;
        CHECK(lk && ink_of(lk, &k, &room));
        if (lk && ink_of(lk, &k, &room)) {
            CHECK(k.y1 >= KNOB_INK_TOP && k.y2 <= KNOB_INK_FOOT && room >= ROOM);
            printf("  %-62s ink %d-%d, %d-%d  %2d px clear\n", where, k.x1, k.x2, k.y1, k.y2, room);
        }
    }
    picture(dir, "adsb-locked");
    where = "...the gear still brings the card, and UNLOCK on it";
    tap_at(GEAR_X, GEAR_Y, 80); run(700);
    CHECK(shown("UNLOCK"));
    tap_at(LOCK_X, LOCK_Y, 60); run(60);
    CHECK(!ui_locked());
    run(10500);
    sky_tap("UAE183");
    CHECK(ui_adsb_card_callsign(cs, sizeof cs));
    tap_at(W / 2, H / 2, 80); run(60);

    where = "the knob's battery, on its own power: under the range, clear of the map";
    st.knob_batt = true; st.knob_pct = 85; run(60);
    knob_check(dir, "adsb-batt");
    where = "...at every charge";
    knob_levels(dir);
    where = "...and the padlock beside it, locked";
    tap_at(GEAR_X, GEAR_Y, 80); run(700);
    tap_at(LOCK_X, LOCK_Y, 60); run(60);
    CHECK(ui_locked());
    run(10500);
    knob_check(dir, "adsb-batt-locked");
    tap_at(GEAR_X, GEAR_Y, 80); run(700);
    tap_at(LOCK_X, LOCK_Y, 60); run(60);
    CHECK(!ui_locked());
    run(10500);
    st.knob_batt = false; run(60);
    quiet();
}
#endif

/* The backlight (GitHub #14): the page's brightness, lit at once; dimmed
 * stays dimmer than lit; dark is dark; and use lights it at the brightness
 * set. Every face shares it. */
static void brightness(void)
{
    where = "the brightness: 100 % full, 80 % the 200 it always was, lit at once";
    ui_set_brightness(100);
    CHECK(backlight == 255);
    ui_set_brightness(80);
    CHECK(backlight == 204);
    where = "...dimmed after its minutes, dark after its own";
    ui_screen_set(&(net_screen_t){ NET_SAVER_NONE, 0, 1, 1 });
    now_ms += 61000;
    ui_dim_tick(false);
    CHECK(backlight == 18);
    now_ms += 60000;
    ui_dim_tick(false);
    CHECK(backlight == 0);
    where = "...a turn lights it at the brightness set";
    ui_set_brightness(30);
    CHECK(backlight == 77);
    now_ms += 61000;
    ui_dim_tick(false);
    CHECK(backlight == 18);
    ui_note_activity();
    CHECK(backlight == 77);
    where = "...set lower than dimmed: dimmed is no brighter than lit";
    ui_set_brightness(5);
    CHECK(backlight == 13);
    now_ms += 61000;
    ui_dim_tick(false);
    CHECK(backlight == 13);
    ui_screen_set(&(net_screen_t){ NET_SAVER_NONE, 0, 0, 0 });
    ui_set_brightness(80);
}

/* The screensavers (GitHub: the third-eye's rain, the clocks): after a
 * minute of nothing, the saver on the glass at the brightness set; a minute
 * later dimmed; a minute after that dark, with the dial drawn again behind
 * it. The first touch on it does nothing but bring the dial back, and the
 * dial comes back exactly as it was. Every face shares it. */
static uint16_t dial[W * H], saver_fb[W * H];
/* The radar's sweep moves by itself, and the clock's hands: there the dial
 * is only checked to be back, not to be the picture it was. */
#if VFO_RADIO_ADSB || VFO_RADIO_CLOCK || VFO_RADIO_WEATHER
#define SAME_DIAL() (memcmp(saver_fb, fb, sizeof fb) != 0)
#else
#define SAME_DIAL() (!memcmp(dial, fb, sizeof fb))
#endif
static void screensaver(const char *dir)
{
    static const struct { uint8_t kind; const char *name; } K[] = {
        { NET_SAVER_MATRIX, "saver-matrix" }, { NET_SAVER_DIGITAL, "saver-digital" },
        { NET_SAVER_DRIFT, "saver-drift" }, { NET_SAVER_CLOCK, "saver-clock" },
    };
    ui_set_brightness(80);
    quiet();
    for (size_t k = 0; k < sizeof K / sizeof *K; k++) {
        where = K[k].name;
        ui_screen_set(&(net_screen_t){ K[k].kind, 1, 1, 1 });
        run(500);
        lv_obj_invalidate(lv_screen_active());
        lv_refr_now(disp);
        memcpy(dial, fb, sizeof fb);
        now_ms += 59000; ui_dim_tick(false); run(200);
        memset(saver_fb, 0, sizeof saver_fb);
        CHECK(SAME_DIAL());                            /* not yet */
        now_ms += 1500; ui_dim_tick(false);
        panel_px = 0;
        run(3000);
        CHECK(memcmp(dial, fb, sizeof fb) != 0);        /* the saver on the glass */
        CHECK(backlight == 204);
        /* What it costs: the pixels sent a second. */
        printf("%s: %d px/s to the glass\n", K[k].name, panel_px / 3);
        snap(dir, K[k].name);
        memcpy(saver_fb, fb, sizeof fb);
        where = "...the face changing behind it draws nothing over it";
        n_flush = 0;
        st.freq_hz += 1000;
        run(1000);
        st.freq_hz -= 1000;
        run(1000);
        CHECK(n_flush == 0);
        where = "...dimmed a minute later";
        now_ms += 60000; ui_dim_tick(false); run(200);
        CHECK(backlight == 18);
        where = "...a touch only brings the dial back, as it was";
        down(SLAB_X, SLAB_Y); run(120); up(); run(400);
        CHECK(!ptt());
        CHECK(backlight == 204);
        CHECK(SAME_DIAL());
        where = "...dark at the third minute, the dial drawn behind it";
        now_ms += 181000; ui_dim_tick(false); run(400);
        CHECK(backlight == 0);
        CHECK(SAME_DIAL());
        ui_note_activity(); run(100);
        CHECK(backlight == 204);
        quiet();
    }
    where = "the digital clock with no time yet";
    clock_set = false;
    ui_screen_set(&(net_screen_t){ NET_SAVER_DIGITAL, 1, 1, 1 });
    now_ms += 61000; ui_dim_tick(false); run(1500);
    snap(dir, "saver-notime");
    clock_set = true;
    ui_screen_set(&(net_screen_t){ NET_SAVER_NONE, 0, 0, 0 });
    run(200);
    quiet();
}

/* Which way up (GitHub #15): with the USB cable at the top, as the knob
 * comes, the glass turned 180 degrees and a finger with it; at the bottom,
 * neither -- as the harness draws. Every face shares it. */
static void turned(void)
{
    lv_indev_t *t = lv_indev_get_next(NULL);
    lv_point_t  p = { -1, -1 };
    where = "the cable at the bottom from the start: the glass as it comes, a finger where it lands";
    CHECK(lv_display_get_rotation(disp) == LV_DISPLAY_ROTATION_0);
    down(20, 180);
    run(30);
    lv_indev_get_point(t, &p);
    CHECK(p.x == 20 && p.y == 180);
    up();
    run(SEEN);
    where = "...turned to the top: the glass turned 180 degrees, and the finger with it";
    ui_set_turned(false);
    CHECK(lv_display_get_rotation(disp) == LV_DISPLAY_ROTATION_180);
    down(20, 180);
    run(30);
    lv_indev_get_point(t, &p);
    CHECK(p.x == W - 1 - 20 && p.y == H - 1 - 180);
    up();
    run(SEEN);
    where = "...and back to the bottom";
    ui_set_turned(true);
    CHECK(lv_display_get_rotation(disp) == LV_DISPLAY_ROTATION_0);
    ui_take_ptt_tap();
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : ".";
    lv_init();
    lv_tick_set_cb(tick_cb);
    /* As a knob with its USB cable at the bottom: the glass the way it
     * comes, the face drawn as the harness compares it. */
    ui_set_turned(true);
    if (ui_init() != ESP_OK) { printf("ui_init failed\n"); return 2; }
    run(100);
    runs(dir);
    brightness();
    screensaver(dir);
    turned();
    if (least_room < 99) printf("%s: the battery, %d px clear of anything else at the least\n", argv[0], least_room);
    if (knob_least_room < 99)
        printf("%s: the knob's own battery, %d px clear of anything else at the least\n", argv[0], knob_least_room);
    {
        /* LVGL's pool at its fullest, the run's whole: the knob has 64 kB of
         * it, and a pointer there is half the PC's, so the PC's figure is
         * larger. Measured 2026-11-04: every released face at most 76 kB
         * here (the Telephone); the Clock's dial drawn unbatched, 86 kB here,
         * ran the knob's out and crashed it. */
        lv_mem_monitor_t m;
        lv_mem_monitor(&m);
        printf("%s: LVGL's pool at most %zu bytes\n", argv[0], m.max_used);
        where = "LVGL's pool, the whole run";
        CHECK(m.max_used < 80 * 1024);
    }
    printf("%s: %d checks, %d failed\n", argv[0], n_run, n_fail);
    return n_fail ? 1 : 0;
}
