/* The editor panel on the host (tools/lvhost): built as ui.c builds it -- its
 * size from fit_text.h, the same styles, the setup firmware's colours -- with
 * every value set through components/ui/fit_text.c, rendered with the same
 * LVGL 9.3 on the 360 px glass. For each value: the font it got, its width,
 * whether it was cut with dots, where its ink lies in the panel; partial
 * redraws against full ones (a value that shrank leaves nothing behind); and,
 * for a value the 250 px panel held, its pixels against that panel as
 * edit_render drew it before -- they must not differ. Then the panel's
 * closest approach to the glass's edge, and a sweep of made-up names.
 *
 *   make chooser && ./chooser DIR     writes DIR/chooser-NN-*.ppm, the glass
 *                                     marked (and DIR/before-NN-*.ppm: the
 *                                     250 px panel, for a value it cut), and
 *                                     DIR/fit.tsv: each value, the font and
 *                                     the text it got, which tools/mkdocs.py's
 *                                     fit() must give too */
#include "lvgl.h"
#include "fit_text.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define W 360
#define H 360
#define OLD_W 250                         /* the panel before */
#define OLD_ROOM (OLD_W - 10)
/* The setup firmware's colours: ui.c's default palette. */
#define C_BG        lv_color_hex(0x0F0F1A)
#define C_BG1       lv_color_hex(0x1A2A3A)
#define C_ACCENT    lv_color_hex(0x00B4D8)
#define C_ACCENT_HI lv_color_hex(0x00C8F0)
#define C_LABEL     lv_color_hex(0x506070)
#define HINT "turn to choose  -  tap to accept"

static const struct { const char *title, *value; bool name; } CASE[] = {
    { "INSTALL", "SVXConnect 1.18.3",       true },
    { "INSTALL", "SVXConnect 1.18.10",      true },
    { "INSTALL", "Telephone 1.18.10",       true },
    { "INSTALL", "AetherSDR 1.18.10",       true },
    { "INSTALL", "FlexRadio 1.18.10",       true },
    { "INSTALL", "UberSDR 1.18.10",         true },
    { "WIFI",    "Set up again",            true },
    /* A station's name comes as a question's choice (ED_CHOICE, 23
     * characters at most): the radio list's own are 15. */
    { "RADIO",   "Lombardsijde FLEX-6600",  true },
    { "RADIO",   "WWWWWWWWWWWWWWWWWWWWWWW", true },
    { "MODE",    "USB",                     false },
    { "FILTER",  "FIL1",                    false },
    { "ANTENNA", "ANT1+RX",                 false },
    /* A FlexRadio's slice names its antennas: RX ANT and TX ANT, a value's
     * panel, as ANTENNA. */
    { "RX ANT",  "RX_A",                    false },
    { "TX ANT",  "XVTB",                    false },
    { "RF.G",    "+8 dB",                   false },
    { "BALANCE", "L | R",                   false },
    { "POWER",   "50 W",                    false },
};
#define N_CASE ((int)(sizeof CASE / sizeof CASE[0]))

/* ---- two displays: the panel now, and as it was ---- */
typedef struct {
    lv_display_t *d;
    uint16_t fb[W * H];
    uint8_t buf[W * 12 * 2] __attribute__((aligned(4)));
} host_t;
static host_t h_now, h_old;

static uint32_t tick_cb(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}
static void flush_cb(lv_display_t *d, const lv_area_t *a, uint8_t *px)
{
    host_t *h = lv_display_get_user_data(d);
    const int w = lv_area_get_width(a);
    for (int y = a->y1; y <= a->y2; y++)
        memcpy(&h->fb[y * W + a->x1], px + (size_t)(y - a->y1) * w * 2, (size_t)w * 2);
    lv_display_flush_ready(d);
}
static lv_obj_t *host_init(host_t *h)
{
    h->d = lv_display_create(W, H);
    lv_display_set_user_data(h->d, h);
    lv_display_set_color_format(h->d, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(h->d, h->buf, NULL, sizeof h->buf, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(h->d, flush_cb);
    /* ui.c build(): the screen. */
    lv_obj_t *scr = lv_display_get_screen_active(h->d);
    lv_obj_set_style_bg_color(scr, C_BG, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    return scr;
}
static void full(host_t *h)
{
    lv_obj_invalidate(lv_display_get_screen_active(h->d));
    lv_refr_now(h->d);
}

/* ---- the panel: ui.c build()'s s_edit_panel and its labels, w wide ---- */
typedef struct { lv_obj_t *panel, *title, *value, *hint; } panel_t;

static void panel_build(panel_t *p, lv_obj_t *scr, int32_t w)
{
    p->panel = lv_obj_create(scr);
    lv_obj_set_size(p->panel, w, EDIT_H);
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
    lv_label_set_text(p->title, "");

    p->value = lv_label_create(p->panel);
    lv_obj_set_style_text_font(p->value, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(p->value, C_ACCENT_HI, 0);
    lv_obj_align(p->value, LV_ALIGN_CENTER, 0, 4);
    lv_label_set_text(p->value, "");

    p->hint = lv_label_create(p->panel);
    lv_obj_set_style_text_font(p->hint, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(p->hint, C_LABEL, 0);
    lv_obj_align(p->hint, LV_ALIGN_BOTTOM_MID, 0, -2);
    lv_label_set_text(p->hint, HINT);
}

static const lv_font_t *usual(bool name) { return name ? &lv_font_montserrat_28 : &lv_font_montserrat_48; }
static int pt(const lv_font_t *f)
{
    return f == &lv_font_montserrat_48 ? 48 : f == &lv_font_montserrat_28 ? 28 :
           f == &lv_font_montserrat_20 ? 20 : 0;
}
static int32_t width(const char *s, const lv_font_t *f)
{
    return lv_text_get_width(s, (uint32_t)strlen(s), f, 0);
}

/* edit_render, now: a list of names in the wider panel, and the value
 * through fit_text(). */
static int32_t panel_w(bool name) { return name ? EDIT_W_NAME : EDIT_W; }
static const lv_font_t *set_now(panel_t *p, const char *title, const char *v, bool name)
{
    lv_label_set_text(p->title, title);
    lv_obj_set_width(p->panel, panel_w(name));
    const lv_font_t *f = fit_text(p->value, v, usual(name), EDIT_ROOM(panel_w(name)));
    lv_obj_set_style_text_color(p->value, C_ACCENT_HI, 0);
    return f;
}
/* edit_render, before: the usual font, whatever the width. */
static void set_then(panel_t *p, const char *title, const char *v, bool name)
{
    lv_label_set_text(p->title, title);
    lv_obj_set_style_text_font(p->value, usual(name), 0);
    lv_label_set_text(p->value, v);
    lv_obj_set_style_text_color(p->value, C_ACCENT_HI, 0);
}

/* The ink in rows y1..y2 inside the panel's border: what is not its fill. */
static bool ink(const uint16_t *fb, const lv_area_t *pan, int y1, int y2, lv_area_t *box)
{
    const uint16_t bg = lv_color_to_u16(C_BG1);
    box->x1 = W; box->x2 = -1; box->y1 = y1; box->y2 = y2;
    for (int y = y1; y <= y2; y++)
        for (int x = pan->x1 + 2; x <= pan->x2 - 2; x++)
            if (fb[y * W + x] != bg) {
                if (x < box->x1) box->x1 = x;
                if (x > box->x2) box->x2 = x;
            }
    return box->x2 >= 0;
}
static int differ(const uint16_t *a, const uint16_t *b, int x1, int y1, int x2, int y2)
{
    int n = 0;
    for (int y = y1; y <= y2; y++)
        for (int x = x1; x <= x2; x++) n += a[y * W + x] != b[y * W + x];
    return n;
}

/* The glass: the panel's outline against the circle, and its pixels. */
static double outline_margin(const lv_area_t *a, int r)
{
    /* The farthest point of a rounded rectangle from the glass's centre is
     * on a corner's arc: its centre's distance, plus the radius. */
    double worst = 0;
    const double cx[2] = { a->x1 + r, a->x2 + 1 - r }, cy[2] = { a->y1 + r, a->y2 + 1 - r };
    for (int i = 0; i < 2; i++)
        for (int j = 0; j < 2; j++) {
            const double d = hypot(cx[i] - W / 2.0, cy[j] - H / 2.0) + r;
            if (d > worst) worst = d;
        }
    return W / 2.0 - worst;
}
static double pixel_margin(const uint16_t *fb)
{
    const uint16_t bg = lv_color_to_u16(C_BG);
    double worst = 0;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            if (fb[y * W + x] != bg) {
                /* the pixel's far corner */
                const double dx = fabs(x + 0.5 - W / 2.0) + 0.5, dy = fabs(y + 0.5 - H / 2.0) + 0.5;
                const double d = hypot(dx, dy);
                if (d > worst) worst = d;
            }
    return W / 2.0 - worst;
}

/* The picture: outside the glass dimmed and tinted, its edge in magenta, and
 * dashed 16 px inside it the closest a panel may come. */
static void ppm(const char *name, const uint16_t *img)
{
    FILE *f = fopen(name, "wb");
    if (!f) { perror(name); return; }
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            const uint16_t p = img[y * W + x];
            uint8_t rgb[3] = { (uint8_t)(((p >> 11) & 31) * 255 / 31),
                               (uint8_t)(((p >> 5) & 63) * 255 / 63),
                               (uint8_t)((p & 31) * 255 / 31) };
            const double d = hypot(x + 0.5 - W / 2.0, y + 0.5 - H / 2.0);
            if (fabs(d - 180.0) < 0.75) {
                rgb[0] = 255; rgb[1] = 0; rgb[2] = 255;
            } else if (d > 180.0) {
                rgb[0] = (uint8_t)(64 + rgb[0] / 4); rgb[1] = (uint8_t)(rgb[1] / 4); rgb[2] = (uint8_t)(rgb[2] / 4);
            } else if (fabs(d - 164.0) < 0.5 && (int)(atan2(y + 0.5 - H / 2.0, x + 0.5 - W / 2.0) * 60) % 2) {
                rgb[0] = 128; rgb[1] = 64; rgb[2] = 128;
            }
            fwrite(rgb, 1, 3, f);
        }
    fclose(f);
}
/* A line of fit.tsv: the usual size, the size taken, the width shown, the
 * text shown, the value. */
static void tsv(FILE *f, const char *v, bool name, const panel_t *p)
{
    const lv_font_t *font = lv_obj_get_style_text_font(p->value, 0);
    const char *shown = lv_label_get_text(p->value);
    if (f) fprintf(f, "%d\t%d\t%d\t%s\t%s\n", pt(usual(name)), pt(font), (int)width(shown, font), shown, v);
}
static void slug(char *out, size_t cap, const char *s)
{
    size_t n = 0;
    for (; *s && n + 1 < cap; s++)
        out[n++] = ((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') ||
                    *s == '.' || *s == '-') ? *s : '_';
    out[n] = 0;
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : ".";
    lv_init();
    lv_tick_set_cb(tick_cb);
    lv_obj_t *scr_now = host_init(&h_now), *scr_then = host_init(&h_old);
    static panel_t pnew, pold;
    panel_build(&pnew, scr_now, EDIT_W);
    panel_build(&pold, scr_then, OLD_W);
    full(&h_now);
    full(&h_old);

    static uint16_t partial[W * H];
    char nm[256];
    snprintf(nm, sizeof nm, "%s/fit.tsv", dir);
    FILE *fits = fopen(nm, "w");
    if (!fits) perror(nm);
    int bad = 0;
    lv_area_t pan;
    printf("value                    usual font width dots  shown                    label x     ink x    gap L/R  off  ink  y1 base  stale before\n");
    for (int i = 0; i < N_CASE; i++) {
        const char *v = CASE[i].value;
        const lv_font_t *f = set_now(&pnew, CASE[i].title, v, CASE[i].name);
        set_then(&pold, CASE[i].title, v, CASE[i].name);
        /* What the knob draws: only what changed, over the last frame. */
        lv_refr_now(h_now.d);
        memcpy(partial, h_now.fb, sizeof partial);
        full(&h_now);
        full(&h_old);
        const int stale = differ(partial, h_now.fb, 0, 0, W - 1, H - 1);

        const int32_t w = width(v, f);
        const bool dots = lv_label_get_long_mode(pnew.value) == LV_LABEL_LONG_MODE_DOTS;
        lv_area_t lab, box;
        lv_obj_get_coords(pnew.panel, &pan);
        lv_obj_get_coords(pnew.value, &lab);
        const bool inked = ink(h_now.fb, &pan, lab.y1, lab.y2, &box);
        const int gl = box.x1 - pan.x1, gr = pan.x2 - box.x2;
        /* Centred: the label's box on the panel's middle. The ink's own
         * middle is the glyphs' -- an L's stem, a W's slant -- and only shown. */
        const double off = ((lab.x1 + lab.x2) - (pan.x1 + pan.x2)) / 2.0;
        const double ink_off = ((box.x1 + box.x2) - (pan.x1 + pan.x2)) / 2.0;
        /* Before: the 250 px panel, and where its value was cut. */
        lv_area_t old_pan, old_lab;
        lv_obj_get_coords(pold.panel, &old_pan);
        lv_obj_get_coords(pold.value, &old_lab);
        const int32_t w0 = width(v, usual(CASE[i].name));
        char before[48];
        if (w0 <= OLD_ROOM) {
            /* It fitted: the same font, the same box, the same pixels. */
            const int n = differ(h_now.fb, h_old.fb, old_pan.x1 + 2, lab.y1, old_pan.x2 - 2, lab.y2);
            const bool same = f == usual(CASE[i].name) && lab.x1 == old_lab.x1 && lab.x2 == old_lab.x2 &&
                              lab.y1 == old_lab.y1 && lab.y2 == old_lab.y2 && n == 0;
            snprintf(before, sizeof before, "%s", same ? "same" : "CHANGED");
            if (!same) bad++;
        } else {
            /* Past its room; past its border -- drawn over the ends, and
             * clipped at its edge -- it was cut. */
            snprintf(before, sizeof before, "was %d px: %s", (int)w0, w0 > OLD_W - 4 ? "cut" : "tight");
        }
        /* After dots, the next value is as wide as its text again. */
        const bool reset = dots || (lv_obj_get_style_width(pnew.value, 0) == LV_SIZE_CONTENT &&
                                    lv_obj_get_style_height(pnew.value, 0) == LV_SIZE_CONTENT &&
                                    lv_obj_get_width(pnew.value) == w &&
                                    strcmp(lv_label_get_text(pnew.value), v) == 0);
        const bool inside = lab.x1 >= pan.x1 + 5 && lab.x2 <= pan.x2 - 5 && gl >= 3 && gr >= 3;
        if (!reset || !inside || stale || !inked || fabs(off) > 0.5) bad++;
        printf("%-24s %4d %4d %5d %-5s %-24s %3d..%-3d %3d..%-3d %3d/%-3d %4.1f %4.1f %3d %3d %5d  %s%s%s%s\n",
               v, pt(usual(CASE[i].name)), pt(f), (int)w, dots ? "yes" : "no", lv_label_get_text(pnew.value),
               (int)lab.x1, (int)lab.x2, (int)box.x1, (int)box.x2, gl, gr, off, ink_off, (int)lab.y1,
               (int)(lab.y1 + lv_font_get_line_height(f) - f->base_line), stale, before,
               reset ? "" : "  NOT RESET", inside ? "" : "  OUTSIDE THE ROOM",
               fabs(off) > 0.5 ? "  OFF-CENTRE" : "");

        tsv(fits, v, CASE[i].name, &pnew);
        char s[64];
        slug(s, sizeof s, v);
        snprintf(nm, sizeof nm, "%s/chooser-%02d-%s.ppm", dir, i + 1, s);
        ppm(nm, h_now.fb);
        if (w0 > OLD_ROOM) {
            snprintf(nm, sizeof nm, "%s/before-%02d-%s.ppm", dir, i + 1, s);
            ppm(nm, h_old.fb);
        }
    }

    /* The panel's own lines, for the guides' pictures (tools/mkdocs.py), and
     * how near the glass's edge it comes: the wider one, a list of names. */
    set_now(&pnew, "INSTALL", CASE[0].value, true);
    full(&h_now);
    lv_obj_get_coords(pnew.panel, &pan);
    lv_area_t a;
    lv_obj_get_coords(pnew.panel, &a);
    lv_area_t c;
    lv_obj_get_content_coords(pnew.panel, &c);
    printf("\npanel %d,%d-%d,%d (%dx%d), content %d,%d-%d,%d\n", (int)a.x1, (int)a.y1, (int)a.x2, (int)a.y2,
           (int)lv_area_get_width(&a), (int)lv_area_get_height(&a), (int)c.x1, (int)c.y1, (int)c.x2, (int)c.y2);
    lv_obj_get_coords(pnew.title, &a);
    printf("title y %d..%d, baseline %d\n", (int)a.y1, (int)a.y2,
           (int)(a.y1 + lv_font_montserrat_20.line_height - lv_font_montserrat_20.base_line));
    lv_obj_get_coords(pnew.hint, &a);
    printf("hint  y %d..%d, baseline %d\n", (int)a.y1, (int)a.y2,
           (int)(a.y1 + lv_font_montserrat_14.line_height - lv_font_montserrat_14.base_line));
    printf("the glass: the panel's outline %.1f px inside its edge, its last pixel %.1f px\n",
           outline_margin(&pan, 18), pixel_margin(h_now.fb));

    /* A sweep: made-up names of 1 to 23 characters, wide letters and narrow,
     * each after the one before -- inside the room, centred, nothing left
     * behind, every time. */
    static const char CH[] = "WMQOGDHNUAKXBRSZEPCFLTYVJIwmqogdhnuakxbrsezpcfltyvjiI1234567890 .-+|/";
    srand(7);
    int n_sweep = 0, worst_gap = 99, n_dots = 0, n_fail = 0, pts[49] = { 0 };
    double worst_off = 0;
    for (int k = 0; k < 600; k++) {
        char v[24];
        const int len = 1 + rand() % 23;
        for (int j = 0; j < len; j++) v[j] = CH[rand() % (int)(sizeof CH - 1)];
        v[len] = 0;
        const bool name = k % 3 != 0;
        const lv_font_t *f = set_now(&pnew, "SWEEP", v, name);
        lv_refr_now(h_now.d);
        memcpy(partial, h_now.fb, sizeof partial);
        full(&h_now);
        const int stale = differ(partial, h_now.fb, 0, 0, W - 1, H - 1);
        lv_area_t lab, box;
        lv_obj_get_coords(pnew.panel, &pan);
        lv_obj_get_coords(pnew.value, &lab);
        const bool dots = lv_label_get_long_mode(pnew.value) == LV_LABEL_LONG_MODE_DOTS;
        if (!ink(h_now.fb, &pan, lab.y1, lab.y2, &box)) continue;   /* all spaces */
        n_sweep++;
        n_dots += dots;
        tsv(fits, v, name, &pnew);
        pts[pt(f)]++;
        const int gl = box.x1 - pan.x1, gr = pan.x2 - box.x2;
        if (gl < worst_gap) worst_gap = gl;
        if (gr < worst_gap) worst_gap = gr;
        /* Centred: the label's box -- the ink of a space or an 'A' at an end
         * is not where the advance ends. */
        const double off = ((lab.x1 + lab.x2) - (pan.x1 + pan.x2)) / 2.0;
        if (fabs(off) > fabs(worst_off)) worst_off = off;
        const bool ok = stale == 0 && lab.x1 >= pan.x1 + 5 && lab.x2 <= pan.x2 - 5 && gl >= 3 && gr >= 3 &&
                        fabs(off) <= 0.5 &&
                        lv_area_get_width(&pan) == panel_w(name) &&
                        (dots ? f == &lv_font_montserrat_20 && width(v, f) > EDIT_ROOM(panel_w(name))
                              : width(v, f) <= EDIT_ROOM(panel_w(name)) &&
                                lv_obj_get_width(pnew.value) == width(v, f));
        if (!ok) {
            n_fail++;
            if (n_fail <= 5)
                printf("sweep FAIL \"%s\": %d pt, %s, label %d..%d, ink %d..%d, stale %d\n", v, pt(f),
                       dots ? "dots" : "whole", (int)lab.x1, (int)lab.x2, (int)box.x1, (int)box.x2, stale);
        }
    }
    printf("sweep: %d names, %d in 48, %d in 28, %d in 20, %d with dots; closest ink to the panel's edge %d px,"
           " most off-centre %.1f px; %d failed\n", n_sweep, pts[48], pts[28], pts[20], n_dots, worst_gap,
           worst_off, n_fail);
    if (fits) fclose(fits);
    printf("%s\n", bad || n_fail ? "PROBLEMS" : "all good");
    return bad || n_fail ? 1 : 0;
}
