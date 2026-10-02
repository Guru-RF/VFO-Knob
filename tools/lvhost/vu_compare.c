/* The phone's split VU on the host (tools/lvhost): the lv_arc stack ui.c drew beside
 * vu_band.c, rendered with the same LVGL 9.3 and compared pixel by pixel; and
 * vu_band's partial redraws checked against full ones (stale pixels = a sector
 * not invalidated). Prints the counts; writes PPMs of chosen states. */
#include "lvgl.h"
#include "vu_band.h"
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
#define SWR_ROT ARC_ROT
#define SWR_SPAN (ARC_SPAN / 2 - 3)
#define AUD_ROT (ARC_ROT + ARC_SPAN / 2 + 3)
#define LED_DEG 3
#define RX_ZONES 8
#define C_BG     lv_color_hex(0x08090C)
#define C_SUBTLE lv_color_hex(0x1B1F29)

static const struct { float from, to; uint32_t rgb; } RXZONES[RX_ZONES] = {
    {  -60.0f,  -48.0f, 0x1B5E2E }, {  -48.0f,  -36.0f, 0x237A3B },
    {  -36.0f,  -24.0f, 0x2EA043 }, {  -24.0f,  -18.0f, 0x35B35A },
    {  -18.0f,  -12.0f, 0x9DBD3B }, {  -12.0f,   -6.0f, 0xD8C43A },
    {   -6.0f,   -3.0f, 0xE08C33 }, {   -3.0f,    0.0f, 0xD13B3B },
};
static float smeter_frac(float db)
{
    if (db < -60.0f) db = -60.0f;
    if (db > 0.0f) db = 0.0f;
    return (db + 60.0f) / 60.0f;
}
static int rx_zone_of(float db)
{
    for (int z = 0; z < RX_ZONES - 1; z++) if (db < RXZONES[z].to) return z;
    return RX_ZONES - 1;
}

/* ---- ui.c's stack, as it is today (copied) ---- */
typedef struct {
    lv_obj_t *arc[8];
    int n, lit, at, span;
    bool reverse;
} peak_led_t;

static void led_build(lv_obj_t *scr, peak_led_t *l, int rot, int span, int r, int width,
                      bool reverse, const uint32_t *rgb, int n)
{
    l->n = n; l->lit = -1; l->at = 0; l->span = span; l->reverse = reverse;
    for (int i = 0; i < n; i++) {
        lv_obj_t *a = lv_arc_create(scr);
        lv_obj_set_size(a, r * 2, r * 2);
        lv_obj_center(a);
        lv_arc_set_rotation(a, rot);
        lv_arc_set_bg_angles(a, 0, 0);
        lv_obj_remove_style(a, NULL, LV_PART_KNOB);
        lv_obj_remove_flag(a, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_arc_width(a, width, LV_PART_MAIN);
        lv_obj_set_style_arc_color(a, lv_color_hex(rgb[i]), LV_PART_MAIN);
        lv_obj_set_style_arc_rounded(a, false, LV_PART_MAIN);
        lv_obj_set_style_arc_opa(a, LV_OPA_TRANSP, LV_PART_INDICATOR);
        l->arc[i] = a;
    }
}
static void led_set(peak_led_t *l, float f, int z)
{
    int at = 0;
    if (f > 0.0f && z >= 0 && z < l->n) {
        if (f > 1.0f) f = 1.0f;
        const int pos = (int)((l->reverse ? 1.0f - f : f) * l->span + 0.5f);
        at = l->reverse ? pos : pos - LED_DEG;
        if (at < 0) at = 0;
        if (at > l->span - LED_DEG) at = l->span - LED_DEG;
    } else {
        z = -1;
    }
    if (z == l->lit && at == l->at) return;
    if (l->lit >= 0 && l->lit != z) lv_arc_set_bg_angles(l->arc[l->lit], 0, 0);
    if (z >= 0) lv_arc_set_bg_angles(l->arc[z], at, at + LED_DEG);
    l->lit = z;
    l->at = at;
}
typedef struct {
    lv_obj_t *zone[RX_ZONES];
    int16_t val[RX_ZONES];
    peak_led_t led;
} old_vu_t;

static void old_build(lv_obj_t *scr, old_vu_t *v, int side)
{
    const bool mirror = side == 1;
    const int rot = side ? AUD_ROT : SWR_ROT;
    lv_obj_t *t = lv_arc_create(scr);
    lv_obj_set_size(t, ARC_R0 * 2, ARC_R0 * 2);
    lv_obj_center(t);
    lv_arc_set_rotation(t, rot);
    lv_arc_set_bg_angles(t, 0, SWR_SPAN);
    lv_obj_remove_style(t, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(t, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(t, 12, LV_PART_MAIN);
    lv_obj_set_style_arc_color(t, C_SUBTLE, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(t, false, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(t, LV_OPA_TRANSP, LV_PART_INDICATOR);
    uint32_t rgb[RX_ZONES];
    for (int z = 0; z < RX_ZONES; z++) {
        const int a0 = (int)(smeter_frac(RXZONES[z].from) * SWR_SPAN);
        const int a1 = (int)(smeter_frac(RXZONES[z].to) * SWR_SPAN);
        lv_obj_t *b = lv_arc_create(scr);
        lv_obj_set_size(b, ARC_R0 * 2, ARC_R0 * 2);
        lv_obj_center(b);
        lv_arc_set_rotation(b, mirror ? rot + SWR_SPAN - a1 : rot + a0);
        lv_arc_set_bg_angles(b, 0, a1 - a0);
        if (mirror) lv_arc_set_mode(b, LV_ARC_MODE_REVERSE);
        lv_arc_set_range(b, 0, 1000);
        lv_arc_set_value(b, 0);
        lv_obj_remove_style(b, NULL, LV_PART_KNOB);
        lv_obj_remove_flag(b, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_arc_opa(b, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_arc_width(b, 12, LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(b, lv_color_hex(RXZONES[z].rgb), LV_PART_INDICATOR);
        lv_obj_set_style_arc_rounded(b, false, LV_PART_INDICATOR);
        lv_obj_set_style_arc_rounded(b, false, LV_PART_MAIN);
        v->zone[z] = b;
        v->val[z] = 0;
        rgb[z] = RXZONES[z].rgb;
    }
    led_build(scr, &v->led, rot, SWR_SPAN, ARC_R0, 12, mirror, rgb, RX_ZONES);
}
static void old_set(old_vu_t *v, float disp, float pk)
{
    led_set(&v->led, smeter_frac(pk), rx_zone_of(pk));
    for (int z = 0; z < RX_ZONES; z++) {
        float f = (disp - RXZONES[z].from) / (RXZONES[z].to - RXZONES[z].from);
        if (f < 0.0f) f = 0.0f;
        if (f > 1.0f) f = 1.0f;
        const int16_t x = (int16_t)(f * 1000.0f);
        if (x == v->val[z]) continue;
        v->val[z] = x;
        lv_arc_set_value(v->zone[z], x);
    }
}

/* ---- the new one ---- */
static void new_build(lv_obj_t *scr, vu_band_t *b, int side)
{
    int16_t a0[RX_ZONES], a1[RX_ZONES];
    uint32_t rgb[RX_ZONES];
    for (int z = 0; z < RX_ZONES; z++) {
        a0[z] = (int16_t)(smeter_frac(RXZONES[z].from) * SWR_SPAN);
        a1[z] = (int16_t)(smeter_frac(RXZONES[z].to) * SWR_SPAN);
        rgb[z] = RXZONES[z].rgb;
    }
    vu_band_build(b, scr, CX, CY, side ? AUD_ROT : SWR_ROT, SWR_SPAN, ARC_R0, 12, side == 1,
                  RX_ZONES, a0, a1, rgb, C_SUBTLE, LED_DEG);
}
static void new_set(vu_band_t *b, float disp, float pk)
{
    vu_band_set(b, smeter_frac(disp), smeter_frac(pk), rx_zone_of(pk));
}

/* ---- the host display ---- */
static uint16_t fb[W * H];
static uint8_t drawbuf[W * 12 * 2] __attribute__((aligned(4)));
static long inv_n, inv_px;
static double render_ms;

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
static void refresh(lv_display_t *d)
{
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    lv_refr_now(d);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    render_ms += (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
}
static void full(lv_display_t *d, lv_obj_t *scr, uint16_t *out)
{
    lv_obj_invalidate(scr);
    refresh(d);
    memcpy(out, fb, sizeof fb);
}
static void ppm(const char *name, const uint16_t *img)
{
    FILE *f = fopen(name, "wb");
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (int i = 0; i < W * H; i++) {
        const uint16_t p = img[i];
        const uint8_t rgb[3] = { (uint8_t)(((p >> 11) & 31) * 255 / 31),
                                 (uint8_t)(((p >> 5) & 63) * 255 / 63),
                                 (uint8_t)((p & 31) * 255 / 31) };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}
/* Pixels that differ, and the largest step in any 565 channel. */
static int differ(const uint16_t *a, const uint16_t *b, int *maxstep, lv_area_t *box)
{
    int n = 0;
    *maxstep = 0;
    box->x1 = W; box->y1 = H; box->x2 = -1; box->y2 = -1;
    for (int i = 0; i < W * H; i++) {
        if (a[i] == b[i]) continue;
        n++;
        const int dr = abs(((a[i] >> 11) & 31) - ((b[i] >> 11) & 31));
        const int dg = abs(((a[i] >> 5) & 63) - ((b[i] >> 5) & 63)) / 2;
        const int db = abs((a[i] & 31) - (b[i] & 31));
        const int m = dr > dg ? (dr > db ? dr : db) : (dg > db ? dg : db);
        if (m > *maxstep) *maxstep = m;
        const int x = i % W, y = i / W;
        if (x < box->x1) box->x1 = x;
        if (x > box->x2) box->x2 = x;
        if (y < box->y1) box->y1 = y;
        if (y > box->y2) box->y2 = y;
    }
    return n;
}

int main(void)
{
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
    }
    static old_vu_t ov[2];
    static vu_band_t nv[2];
    for (int s = 0; s < 2; s++) { old_build(scr_old, &ov[s], s); new_build(scr_new, &nv[s], s); }

    /* 1. The look: states both ways, full redraws, compared. */
    static const float ST[][4] = {   /* rx disp, rx peak, tx disp, tx peak (dBFS) */
        { -90, -90, -90, -90 }, { -50, -45, -55, -50 }, { -40, -30, -33, -28 },
        { -24, -18, -20, -14 }, { -15, -9, -12, -7 },   { -6, -2, -5, -1 },
        { 0, 0, 0, 0 },         { -30, -5, -45, -3 },   { -47.9f, -47.9f, -36.1f, -35.9f },
        { -12.0f, -3.0f, -6.0f, -0.5f }, { -59, -58, -59.5f, -59.4f },
    };
    static uint16_t A[W * H], B[W * H];
    int worst_n = 0, worst_step = 0;
    for (size_t i = 0; i < sizeof ST / sizeof ST[0]; i++) {
        for (int s = 0; s < 2; s++) {
            old_set(&ov[s], ST[i][2 * s], ST[i][2 * s + 1]);
            new_set(&nv[s], ST[i][2 * s], ST[i][2 * s + 1]);
        }
        lv_screen_load(scr_old); full(d, scr_old, A);
        lv_screen_load(scr_new); full(d, scr_new, B);
        int step; lv_area_t box;
        const int n = differ(A, B, &step, &box);
        printf("state %2zu: %5d px differ (largest step %d/31) in %d,%d-%d,%d\n", i, n, step,
               (int)box.x1, (int)box.y1, (int)box.x2, (int)box.y2);
        if (n > worst_n) worst_n = n;
        if (step > worst_step) worst_step = step;
        char nm[64];
        if (i == 4 || i == 7) {
            snprintf(nm, sizeof nm, "old%zu.ppm", i); ppm(nm, A);
            snprintf(nm, sizeof nm, "new%zu.ppm", i); ppm(nm, B);
            for (int k = 0; k < W * H; k++) A[k] = (A[k] == B[k]) ? 0 : 0xFFFF;
            snprintf(nm, sizeof nm, "diff%zu.ppm", i); ppm(nm, A);
        }
    }
    printf("look: worst %d px differ, largest step %d/31\n", worst_n, worst_step);

    /* 2. Partial redraws against full ones, on a speech-like walk; and what
     *    each redraws, against the old stack's. */
    srand(7);
    float lvl[2] = { -60, -60 }, pk[2] = { -60, -60 };
    long stale = 0, steps = 2000;
    long n_inv[2] = { 0, 0 }, px_inv[2] = { 0, 0 };
    double ms[2] = { 0, 0 };
    for (int pass = 0; pass < 2; pass++) {             /* 0: old, 1: new */
        lv_obj_t *scr = scrs[pass];
        lv_screen_load(scr);
        full(d, scr, A);
        srand(7);
        lvl[0] = lvl[1] = -60; pk[0] = pk[1] = -60;
        inv_n = inv_px = 0;
        render_ms = 0;
        for (long t = 0; t < steps; t++) {
            for (int s = 0; s < 2; s++) {
                /* talk in bursts, pauses between; peaks held a while */
                const bool talking = ((t / 37 + s) % 3) != 0;
                const float target = talking ? -30.0f + (float)(rand() % 280) / 10.0f : -62.0f;
                lvl[s] = target > lvl[s] ? target : lvl[s] + (target - lvl[s]) * 0.35f;
                if (lvl[s] > pk[s]) pk[s] = lvl[s]; else if (t % 20 > 10) pk[s] -= 1.5f;
                if (pk[s] < lvl[s]) pk[s] = lvl[s];
                if (pass == 0) old_set(&ov[s], lvl[s], pk[s]);
                else           new_set(&nv[s], lvl[s], pk[s]);
            }
            refresh(d);
            if (pass == 1 && t % 10 == 0) {               /* stale pixels? */
                memcpy(B, fb, sizeof fb);
                const long keep_n = inv_n, keep_px = inv_px;
                const double keep_ms = render_ms;
                full(d, scr, A);
                inv_n = keep_n; inv_px = keep_px; render_ms = keep_ms;
                int step; lv_area_t box;
                const int n = differ(A, B, &step, &box);
                if (n) {
                    stale++;
                    if (stale <= 5)
                        printf("STALE at step %ld: %d px (step %d) in %d,%d-%d,%d\n", t, n, step,
                               (int)box.x1, (int)box.y1, (int)box.x2, (int)box.y2);
                }
            }
        }
        n_inv[pass] = inv_n; px_inv[pass] = inv_px; ms[pass] = render_ms;
    }
    printf("walk of %ld steps: old stack %ld areas, %ld px invalidated, %.0f ms rendering\n",
           steps, n_inv[0], px_inv[0], ms[0]);
    printf("                   new band  %ld areas, %ld px invalidated, %.0f ms rendering\n",
           n_inv[1], px_inv[1], ms[1]);
    printf("stale checks failed: %ld of %ld\n", stale, steps / 10);
    return 0;
}
