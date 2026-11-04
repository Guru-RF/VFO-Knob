/* The screensavers: drawn straight to the panel while LVGL's refresh rests.
 *
 * Through LVGL the rain would cost the whole screen's refresh every frame,
 * ~45 ms at ~1.5 ms a 360x12 chunk. Here each column's moving part is
 * composed in PSRAM and sent alone, ~7 kB a step. The pixels go out through
 * LVGL's own two draw buffers in turn -- internal, DMA-capable, and idle while
 * its refresh rests -- byte-swapped as the port sends them and turned with
 * the glass. esp_lcd's draw_bitmap sets the window first, and that waits for
 * every transfer still on the wire, so the buffer about to be filled is never
 * the one being sent.
 *
 * Ideas, not code: the rain after Jekyllz's ESP32-third-eye (no licence);
 * the stale clock after MilovdZee's Analog_Clock-esp32. */
#include "saver.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "board_pins.h"
#include "clockface.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "net_prov.h"
#include "panel.h"
#include "src/display/lv_display_private.h"   /* LVGL's draw buffers, borrowed */
#include "wallclock.h"

LV_FONT_DECLARE(font_matrix_18);
LV_FONT_DECLARE(font_hack_96);
LV_FONT_DECLARE(font_hack_46);

#define SCR   BOARD_LCD_H_RES        /* the glass, square and round */
#define R_VIS 178                    /* what of it a box may reach  */

static lv_display_t *s_disp;
static lv_timer_t   *s_refr;         /* LVGL's refresh, paused */
static bool          s_on, s_turned;
static uint8_t       s_kind;

/* xorshift32: the rain's randomness, the same every run on the PC. */
static uint32_t s_rng = 0x2545F491u;
static uint32_t rnd(uint32_t n)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return n ? s_rng % n : 0;
}

/* ---- to the panel ---- */

static uint16_t *s_dma[2];           /* LVGL's draw buffers */
static uint32_t  s_dma_px;           /* pixels in each */
static uint8_t   s_dma_next;

static inline uint16_t swap16(uint16_t v) { return (uint16_t)(v << 8 | v >> 8); }

/* Every transfer under way finished: esp_lcd sends a command only once the
 * pixels before it are out. LVGL 9.3 does not wait for its last flush at the
 * end of a refresh, so this comes first; and last, before LVGL renders into
 * its buffers again. */
static void fence(void) { esp_lcd_panel_disp_on_off(panel_handle(), true); }

/* A rectangle of the screen as the face sees it, `src` w wide with `stride`
 * pixels a row (NULL: all `fill`), to the panel in as few transfers as
 * LVGL's buffers allow. */
static void put(int x, int y, int w, int h, const uint16_t *src, int stride, uint16_t fill)
{
    if (w <= 0 || h <= 0) return;
    const int per = (int)(s_dma_px / (uint32_t)w);
    for (int r = 0; r < h; r += per) {
        const int n = h - r < per ? h - r : per;
        uint16_t *d = s_dma[s_dma_next];
        s_dma_next ^= 1;
        for (int j = 0; j < n; j++) {
            /* Turned, the last row first and each row backwards: 180 deg. */
            uint16_t *o = d + (size_t)(s_turned ? n - 1 - j : j) * w;
            if (!src) {
                for (int i = 0; i < w; i++) o[i] = swap16(fill);
                continue;
            }
            const uint16_t *s = src + (size_t)(r + j) * stride;
            if (s_turned) for (int i = 0; i < w; i++) o[w - 1 - i] = swap16(s[i]);
            else          for (int i = 0; i < w; i++) o[i] = swap16(s[i]);
        }
        int px = x, py = y + r;
        if (s_turned) { px = SCR - x - w; py = SCR - (y + r) - n; }
        esp_lcd_panel_draw_bitmap(panel_handle(), px, py, px + w, py + n, d);
    }
}

static inline uint16_t rgb565(uint32_t hex)
{
    return (uint16_t)(((hex >> 8) & 0xF800) | ((hex >> 5) & 0x07E0) | ((hex >> 3) & 0x001F));
}

/* ---- a picture LVGL draws into, never shown ---- */

/* One canvas with no parent -- a screen never loaded -- its buffer the
 * picture of the moment, in PSRAM. Drawing into it is LVGL's software
 * renderer alone; the display is not involved. */
static lv_obj_t *s_cv;

typedef struct {
    uint16_t *px;
    int       w, h;
} pic_t;

static bool pic_open(pic_t *p, int w, int h)
{
    if (!p->px || p->w != w || p->h != h) {
        heap_caps_free(p->px);
        p->px = heap_caps_malloc((size_t)w * h * 2, MALLOC_CAP_SPIRAM);
        p->w = p->px ? w : 0;
        p->h = p->px ? h : 0;
    }
    if (!p->px) return false;
    if (!s_cv) s_cv = lv_canvas_create(NULL);
    lv_canvas_set_buffer(s_cv, p->px, w, h, LV_COLOR_FORMAT_RGB565);
    memset(p->px, 0, (size_t)w * h * 2);
    return true;
}

static void pic_text(lv_layer_t *l, const char *txt, const lv_font_t *f, uint32_t hex,
                     int x, int y, int w, lv_text_align_t align)
{
    lv_draw_label_dsc_t d;
    lv_draw_label_dsc_init(&d);
    d.text  = txt;
    d.text_local = 1;        /* drawn at the layer's end: the text copied */
    d.font  = f;
    d.color = lv_color_hex(hex);
    d.align = align;
    const lv_area_t a = { x, y, x + w - 1, y + lv_font_get_line_height(f) - 1 };
    lv_draw_label(l, &d, &a);
}

static int text_w(const char *txt, const lv_font_t *f)
{
    lv_point_t sz;
    lv_text_get_size(&sz, txt, f, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return sz.x;
}

/* ---- Matrix rain ---- */

#define MX_CW   15
#define MX_CH   20
#define MX_COLS (SCR / MX_CW)
#define MX_ROWS (SCR / MX_CH)

/* The film's set: the half-width katakana, the digits and a few signs. */
static const char MX_SIGNS[] = ":.=*+-<>|";
#define MX_GLYPHS ((0xFF9D - 0xFF66 + 1) + 10 + (int)sizeof MX_SIGNS - 1)

typedef struct {
    int16_t  head;               /* the head's row; < 0, still above the glass */
    uint8_t  len;                /* the cells of its trail */
    uint8_t  top, bot;           /* the rows the round glass shows of it */
    uint16_t period;             /* ms a step */
    uint32_t due;
    uint8_t  glyph[MX_ROWS];
} mx_col_t;

EXT_RAM_BSS_ATTR static mx_col_t s_mx[MX_COLS];
static uint8_t  *s_mx_atlas;     /* each glyph's coverage, mirrored, MX_CW x MX_CH */
static uint16_t *s_mx_strip;     /* a column composed, MX_CW x SCR */

static uint32_t mx_code(int g)
{
    if (g < 0xFF9D - 0xFF66 + 1) return 0xFF66 + (uint32_t)g;
    g -= 0xFF9D - 0xFF66 + 1;
    return g < 10 ? (uint32_t)('0' + g) : (uint32_t)MX_SIGNS[g - 10];
}

/* Each glyph drawn once, white on black, its green read back as coverage and
 * kept mirrored, as the film shows them. */
static bool mx_atlas(void)
{
    if (s_mx_atlas) return true;
    uint8_t *a = heap_caps_malloc((size_t)MX_GLYPHS * MX_CW * MX_CH, MALLOC_CAP_SPIRAM);
    static pic_t p;
    if (!a || !pic_open(&p, MX_CW, MX_CH)) { heap_caps_free(a); return false; }
    for (int g = 0; g < MX_GLYPHS; g++) {
        const uint32_t c = mx_code(g);
        char u[4];
        if (c < 0x80) { u[0] = (char)c; u[1] = 0; }
        else {
            u[0] = (char)(0xE0 | c >> 12);
            u[1] = (char)(0x80 | (c >> 6 & 0x3F));
            u[2] = (char)(0x80 | (c & 0x3F));
            u[3] = 0;
        }
        memset(p.px, 0, (size_t)MX_CW * MX_CH * 2);
        lv_layer_t l;
        lv_canvas_init_layer(s_cv, &l);
        pic_text(&l, u, &font_matrix_18, 0xFFFFFF, 0, -1, MX_CW, LV_TEXT_ALIGN_CENTER);
        lv_canvas_finish_layer(s_cv, &l);
        uint8_t *o = a + (size_t)g * MX_CW * MX_CH;
        for (int y = 0; y < MX_CH; y++)
            for (int x = 0; x < MX_CW; x++) {
                const uint16_t v = p.px[y * MX_CW + x];
                o[y * MX_CW + (MX_CW - 1 - x)] = (uint8_t)(((v >> 5) & 0x3F) * 255 / 63);
            }
    }
    heap_caps_free(p.px);
    p.px = NULL;
    s_mx_atlas = a;
    return true;
}

static void mx_reset(mx_col_t *c, uint32_t now, bool first)
{
    c->head   = (int16_t)-(int)rnd(first ? MX_ROWS * 2 : MX_ROWS / 2);
    c->len    = (uint8_t)(6 + rnd(11));
    c->period = (uint16_t)(45 + rnd(100));
    c->due    = now + rnd(c->period);
    for (int r = 0; r < MX_ROWS; r++) c->glyph[r] = (uint8_t)rnd(MX_GLYPHS);
}

static bool mx_start(uint32_t now)
{
    if (!mx_atlas()) return false;
    if (!s_mx_strip) s_mx_strip = heap_caps_malloc((size_t)MX_CW * SCR * 2, MALLOC_CAP_SPIRAM);
    if (!s_mx_strip) return false;
    for (int i = 0; i < MX_COLS; i++) {
        mx_col_t *c = &s_mx[i];
        /* The rows whose middle the glass shows, for this column's middle. */
        const float dx = (float)(i * MX_CW + MX_CW / 2) - SCR / 2.0f;
        const float half = dx * dx < (SCR / 2.0f) * (SCR / 2.0f)
                         ? sqrtf((SCR / 2.0f) * (SCR / 2.0f) - dx * dx) : 0.0f;
        int top = MX_ROWS, bot = -1;
        for (int r = 0; r < MX_ROWS; r++) {
            const float dy = (float)(r * MX_CH + MX_CH / 2) - SCR / 2.0f;
            if (fabsf(dy) <= half + MX_CH / 2) { if (r < top) top = r; bot = r; }
        }
        c->top = (uint8_t)(top < MX_ROWS ? top : 0);
        c->bot = (uint8_t)(bot >= 0 ? bot : 0);
        mx_reset(c, now, true);
    }
    return true;
}

/* The head pale, the trail fading to dark green, the cell after it erased. */
static void mx_draw(int i)
{
    mx_col_t *c = &s_mx[i];
    int r0 = c->head - c->len - 1, r1 = c->head;
    if (r0 < c->top) r0 = c->top;
    if (r1 > c->bot) r1 = c->bot;
    if (r0 > r1) return;
    for (int r = r0; r <= r1; r++) {
        const int d = c->head - r;
        uint32_t cr, cg, cb;
        if (d == 0)          { cr = 200; cg = 255; cb = 200; }
        else if (d > c->len) { cr = 0;   cg = 0;   cb = 0;   }
        else {
            cg = 255 - (uint32_t)(d - 1) * 215 / c->len;
            cr = 0;
            cb = cg / 4;
        }
        const uint8_t *g = s_mx_atlas + (size_t)c->glyph[r] * MX_CW * MX_CH;
        uint16_t *o = s_mx_strip + (size_t)(r - r0) * MX_CH * MX_CW;
        for (int k = 0; k < MX_CW * MX_CH; k++) {
            const uint32_t a = g[k];
            o[k] = (uint16_t)(((cr * a) >> 11) << 11 | ((cg * a) >> 10) << 5 | ((cb * a) >> 11));
        }
    }
    put(i * MX_CW, r0 * MX_CH, MX_CW, (r1 - r0 + 1) * MX_CH, s_mx_strip, MX_CW, 0);
}

static void mx_frame(uint32_t now)
{
    for (int i = 0; i < MX_COLS; i++) {
        mx_col_t *c = &s_mx[i];
        bool moved = false;
        for (int k = 0; k < 2 && (int32_t)(now - c->due) >= 0; k++) {
            c->due += c->period;
            c->head++;
            if (c->head >= 0 && c->head < MX_ROWS) c->glyph[c->head] = (uint8_t)rnd(MX_GLYPHS);
            /* Now and then a character in the trail turns over. */
            const int t = c->head - 1 - (int)rnd(c->len);
            if (t >= 0 && t < MX_ROWS && !rnd(3)) c->glyph[t] = (uint8_t)rnd(MX_GLYPHS);
            moved = true;
        }
        if ((int32_t)(now - c->due) >= 0) c->due = now + c->period;   /* fell behind: catch up */
        if (!moved) continue;
        mx_draw(i);
        if (c->head - c->len - 1 > c->bot) mx_reset(c, now, false);
    }
}

/* ---- a box drifting round the glass: the clock, the frequency ---- */

typedef struct {
    pic_t    pic;
    int      x, y;               /* the box's top left */
    int8_t   vx, vy;
    int      shown_w, shown_h;   /* what is on the glass, to clear on a resize */
    int      shown_x, shown_y;
    uint32_t due;
} drift_t;

static drift_t s_dr;

/* Inside the round glass whole, corners and all. */
static bool fits(int x, int y, int w, int h)
{
    const int c = SCR / 2;
    const int fx = x + w - c > c - x ? x + w - c : c - x;   /* the farthest corner */
    const int fy = y + h - c > c - y ? y + h - c : c - y;
    return fx * fx + fy * fy <= R_VIS * R_VIS;
}

/* One pixel a step, bouncing off the edge; a box too big for the glass
 * stays in its middle. */
static void drift_move(int w, int h)
{
    drift_t *d = &s_dr;
    if (!fits((SCR - w) / 2, (SCR - h) / 2, w, h)) { d->x = (SCR - w) / 2; d->y = (SCR - h) / 2; return; }
    if (!fits(d->x, d->y, w, h)) { d->x = (SCR - w) / 2; d->y = (SCR - h) / 2; }
    if (!fits(d->x + d->vx, d->y, w, h)) d->vx = (int8_t)-d->vx;
    if (!fits(d->x, d->y + d->vy, w, h)) d->vy = (int8_t)-d->vy;
    if (fits(d->x + d->vx, d->y + d->vy, w, h)) { d->x += d->vx; d->y += d->vy; }
}

/* The picture to the glass where the box now is; what it no longer covers
 * of the last one, black. A step of a pixel is covered by the picture's own
 * black margin. */
static void drift_show(void)
{
    drift_t *d = &s_dr;
    const int w = d->pic.w, h = d->pic.h;
    if (d->shown_w && (d->shown_w != w || d->shown_h != h ||
                       d->shown_x > d->x + 2 || d->shown_x < d->x - 2 ||
                       d->shown_y > d->y + 2 || d->shown_y < d->y - 2))
        put(d->shown_x, d->shown_y, d->shown_w, d->shown_h, NULL, 0, 0);
    put(d->x, d->y, w, h, d->pic.px, w, 0);
    d->shown_x = d->x; d->shown_y = d->y;
    d->shown_w = w;    d->shown_h = h;
}

static const char *const WDAY[7]  = { "SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT" };
static const char *const MONTH[12] = { "JAN", "FEB", "MAR", "APR", "MAY", "JUN",
                                       "JUL", "AUG", "SEP", "OCT", "NOV", "DEC" };

#define MARGIN 2

/* Big HH:MM, the colon blinking with the seconds; under it the date and the
 * UTC time. Grey once the time is a day old; "--:--" before there is any. */
static void digital_frame(uint32_t now)
{
    drift_t *d = &s_dr;
    if ((int32_t)(now - d->due) < 0) return;
    d->due = now + 1000;
    struct tm lt, ut;
    uint16_t ms = 0;
    const bool have = wallclock_now(&lt, &ut, &ms);
    if (have) d->due = now + 1000 - ms;       /* the next second, on the second */
    const bool fresh = have && wallclock_fresh();
    char big[8], line[40], utc[24];
    const bool h24 = net_prov_clock24();
    if (!have) {
        strcpy(big, "--:--");
        strcpy(line, "NO TIME YET");
        utc[0] = 0;
    } else {
        int hh = lt.tm_hour;
        if (!h24) hh = hh % 12 ? hh % 12 : 12;
        snprintf(big, sizeof big, h24 ? "%02d%c%02d" : "%d%c%02d", hh, lt.tm_sec % 2 ? ' ' : ':', lt.tm_min);
        snprintf(line, sizeof line, "%s %d %s%s", WDAY[lt.tm_wday], lt.tm_mday, MONTH[lt.tm_mon],
                 h24 ? "" : lt.tm_hour < 12 ? "  AM" : "  PM");
        snprintf(utc, sizeof utc, "%02d:%02d UTC", ut.tm_hour, ut.tm_min);
    }
    const int bw = text_w("00:00", &font_hack_96);
    const int bh = lv_font_get_line_height(&font_hack_96);
    const int sh = lv_font_get_line_height(&lv_font_montserrat_20);
    const int w = bw + 2 * MARGIN, h = bh + 6 + sh + 2 + sh + 2 * MARGIN;
    drift_move(w, h);
    if (!pic_open(&d->pic, w, h)) return;
    lv_layer_t l;
    lv_canvas_init_layer(s_cv, &l);
    pic_text(&l, big, &font_hack_96, fresh ? 0xFFFFFF : 0x707884, MARGIN, MARGIN, bw, LV_TEXT_ALIGN_CENTER);
    pic_text(&l, line, &lv_font_montserrat_20, fresh ? 0xC0C8D4 : 0x707884, MARGIN, MARGIN + bh + 6, bw,
             LV_TEXT_ALIGN_CENTER);
    if (utc[0])
        pic_text(&l, utc, &lv_font_montserrat_20, fresh ? 0x5A9BFF : 0x707884, MARGIN,
                 MARGIN + bh + 6 + sh + 2, bw, LV_TEXT_ALIGN_CENTER);
    lv_canvas_finish_layer(s_cv, &l);
    drift_show();
}

/* What the face shows biggest, in its own digits where it is a frequency,
 * moving a pixel every half second. */
static void freq_frame(uint32_t now)
{
    drift_t *d = &s_dr;
    if ((int32_t)(now - d->due) < 0) return;
    d->due = now + 500;
    char big[40] = "", small[40] = "";
    ui_saver_text(big, sizeof big, small, sizeof small);
    bool digits = big[0] != 0;
    for (const char *c = big; *c; c++) if ((*c < '0' || *c > '9') && *c != '.') digits = false;
    const lv_font_t *fb = digits ? &font_hack_46 : &lv_font_montserrat_48;
    const lv_font_t *fs = &lv_font_montserrat_20;
    int bw = text_w(big, fb), sw = small[0] ? text_w(small, fs) : 0;
    if (bw > SCR - 40) bw = SCR - 40;
    const int iw = bw > sw ? bw : sw;
    const int bh = lv_font_get_line_height(fb), sh = small[0] ? 4 + lv_font_get_line_height(fs) : 0;
    const int w = iw + 2 * MARGIN, h = bh + sh + 2 * MARGIN;
    drift_move(w, h);
    if (!pic_open(&d->pic, w, h)) return;
    lv_layer_t l;
    lv_canvas_init_layer(s_cv, &l);
    pic_text(&l, big, fb, 0xFFFFFF, MARGIN, MARGIN, iw, LV_TEXT_ALIGN_CENTER);
    if (small[0]) pic_text(&l, small, fs, 0xC0C8D4, MARGIN, MARGIN + bh + 4, iw, LV_TEXT_ALIGN_CENTER);
    lv_canvas_finish_layer(s_cv, &l);
    drift_show();
}

/* ---- the analog clock: the page's Clock face, the Clock firmware's ----
 * Its analog faces; the shack clock's while that face is the digital one,
 * which the Digital clock saver has already, drifting. */

static uint16_t *s_ck;           /* the face, 360 x 360 */
static uint32_t  s_ck_due;

static bool ck_start(uint32_t now)
{
    if (!s_ck) s_ck = heap_caps_malloc((size_t)SCR * SCR * 2, MALLOC_CAP_SPIRAM);
    if (!s_ck) return false;
    clockface_reset();
    s_ck_due = now;
    return true;
}

static void ck_frame(uint32_t now)
{
    if ((int32_t)(now - s_ck_due) < 0) return;
    const uint8_t face = net_prov_clock_face();
    const cf_opts_t o = { .face = face < CF_DIGITAL ? face : CF_SHACK, .seconds = net_prov_clock_seconds(),
                          .h24 = net_prov_clock24() };
    lv_area_t a;
    if (clockface_draw(s_ck, &o, &a))
        put(a.x1, a.y1, a.x2 - a.x1 + 1, a.y2 - a.y1 + 1, s_ck + a.y1 * SCR + a.x1, SCR, 0);
    s_ck_due = now + clockface_wait_ms(&o);
}

/* ---- start, frames, stop ---- */

void saver_start(lv_display_t *disp, uint8_t kind)
{
    if (s_on || kind == NET_SAVER_NONE) return;
    s_disp = disp;
    s_kind = kind;
    s_turned = lv_display_get_rotation(disp) == LV_DISPLAY_ROTATION_180;
    s_dma[0] = disp->buf_1 ? (uint16_t *)disp->buf_1->data : NULL;
    s_dma[1] = disp->buf_2 ? (uint16_t *)disp->buf_2->data : s_dma[0];
    s_dma_px = disp->buf_1 ? disp->buf_1->data_size / 2 : 0;
    if (!s_dma[0] || s_dma_px < SCR) return;
    /* Invalidation off as well as the refresh timer paused: LVGL 9.3 resumes
     * a paused refresh at the next invalidation, and a meter or a label
     * changing behind the saver would be drawn over it. The dial is drawn
     * again whole when the saver goes. */
    lv_display_enable_invalidation(disp, false);
    s_refr = lv_display_get_refr_timer(disp);
    if (s_refr) lv_timer_pause(s_refr);
    s_on = true;
    fence();
    s_rng ^= lv_tick_get();
    const uint32_t now = lv_tick_get();
    put(0, 0, SCR, SCR, NULL, 0, 0);
    s_dr.x = SCR; s_dr.y = SCR;               /* off the glass: drift_move centres it */
    s_dr.vx = 1;  s_dr.vy = 1;
    s_dr.shown_w = 0;
    s_dr.due = now;
    if (kind == NET_SAVER_MATRIX && !mx_start(now)) s_kind = NET_SAVER_DIGITAL;
    if (kind == NET_SAVER_CLOCK && !ck_start(now))  s_kind = NET_SAVER_DIGITAL;
    saver_frame(now);
}

void saver_frame(uint32_t now_ms)
{
    if (!s_on) return;
    switch (s_kind) {
    case NET_SAVER_MATRIX: mx_frame(now_ms);      break;
    case NET_SAVER_DRIFT:  freq_frame(now_ms);    break;
    case NET_SAVER_CLOCK:  ck_frame(now_ms);      break;
    default:               digital_frame(now_ms); break;
    }
}

void saver_stop(void)
{
    if (!s_on) return;
    s_on = false;
    fence();
    lv_display_enable_invalidation(s_disp, true);
    if (s_refr) lv_timer_resume(s_refr);
}

bool saver_running(void) { return s_on; }
