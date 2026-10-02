/* An arc meter drawn by one object: see vu_band.h. */
#include "vu_band.h"

#include <math.h>
#include <stdlib.h>

#define CELL_ROWS  16          /* the strips the ring is cut into, to clip each arc to */
#define PIECE_MIN  16          /* an invalidated sweep is cut into pieces of this... */
#define PIECES_MAX 6           /* ...but never into more than these */

static int imin(int a, int b) { return a < b ? a : b; }
static int imax(int a, int b) { return a > b ? a : b; }

/* isect() is LVGL's private API in 9.3. */
static bool isect(lv_area_t *r, const lv_area_t *a, const lv_area_t *b)
{
    r->x1 = imax(a->x1, b->x1);
    r->y1 = imax(a->y1, b->y1);
    r->x2 = imin(a->x2, b->x2);
    r->y2 = imin(a->y2, b->y2);
    return r->x1 <= r->x2 && r->y1 <= r->y2;
}

/* Degrees from the filling end to absolute LVGL degrees, as a range lo..hi. */
static void abs_range(const vu_band_t *b, int lo, int hi, int *from, int *to)
{
    if (b->mirror) { *from = b->rot + b->span - hi; *to = b->rot + b->span - lo; }
    else           { *from = b->rot + lo;           *to = b->rot + hi; }
}

/* The box of the ring's sector lo..hi (degrees from the filling end): its four
 * corners, and the outer edge's extreme at each right angle inside it. With
 * the anti-aliased edge's pixel and 2 more. */
static void sector_box(const vu_band_t *b, int lo, int hi, lv_area_t *out)
{
    int A, B;
    abs_range(b, lo, hi, &A, &B);
    const float ro = (float)b->r + 1.0f, ri = (float)(b->r - b->w) - 1.0f;
    float x1 = 1e9f, y1 = 1e9f, x2 = -1e9f, y2 = -1e9f;
    const float k = 3.14159265f / 180.0f;
#define PT(rad, deg) do {                                              \
        const float t_ = (float)(deg) * k;                             \
        const float x_ = (float)b->cx + (rad) * cosf(t_);              \
        const float y_ = (float)b->cy + (rad) * sinf(t_);              \
        if (x_ < x1) x1 = x_;                                          \
        if (x_ > x2) x2 = x_;                                          \
        if (y_ < y1) y1 = y_;                                          \
        if (y_ > y2) y2 = y_;                                          \
    } while (0)
    PT(ro, A); PT(ri, A); PT(ro, B); PT(ri, B);
    for (int q = ((A + 89) / 90) * 90; q <= B; q += 90) PT(ro, q);
#undef PT
    out->x1 = (int32_t)floorf(x1) - 2;
    out->y1 = (int32_t)floorf(y1) - 2;
    out->x2 = (int32_t)ceilf(x2) + 2;
    out->y2 = (int32_t)ceilf(y2) + 2;
}

/* Is absolute angle a (0..360) within from..to, from <= to, either past 360? */
static bool angle_in(float a, float from, float to)
{
    float d = a - from;
    while (d < 0.0f)    d += 360.0f;
    while (d >= 360.0f) d -= 360.0f;
    return d <= to - from;
}

/* The cells: the object's box in strips of CELL_ROWS rows, each narrowed to
 * where the ring's pixels inside the meter's angles are -- left of the
 * centre and right of it separately, for a meter that crosses the top. */
static void build_cells(vu_band_t *b, const lv_area_t *box)
{
    int A, B;
    abs_range(b, 0, b->span, &A, &B);
    const float ro = (float)b->r + 1.5f, ri = (float)(b->r - b->w) - 1.5f;
    b->ncell = 0;
    for (int y0 = box->y1; y0 <= box->y2; y0 += CELL_ROWS) {
        const int y1 = imin(y0 + CELL_ROWS - 1, box->y2);
        int lx1 = 1 << 20, lx2 = -1, rx1 = 1 << 20, rx2 = -1;
        for (int y = y0; y <= y1; y++) {
            const float dy = (float)y + 0.5f - (float)b->cy;
            for (int x = box->x1; x <= box->x2; x++) {
                const float dx = (float)x + 0.5f - (float)b->cx;
                const float d  = sqrtf(dx * dx + dy * dy);
                if (d < ri || d > ro) continue;
                float a = atan2f(dy, dx) * 180.0f / 3.14159265f;
                if (a < 0.0f) a += 360.0f;
                if (!angle_in(a, (float)A - 1.0f, (float)B + 1.0f)) continue;
                if (x < b->cx) { if (x < lx1) lx1 = x; if (x > lx2) lx2 = x; }
                else           { if (x < rx1) rx1 = x; if (x > rx2) rx2 = x; }
            }
        }
        /* Two touching halves are one cell. */
        if (lx2 >= 0 && rx2 >= 0 && rx1 <= lx2 + 2) { lx2 = rx2; rx2 = -1; }
        const int xs[2][2] = { { lx1, lx2 }, { rx1, rx2 } };
        for (int s = 0; s < 2; s++) {
            if (xs[s][1] < 0 || b->ncell >= VU_BAND_CELLS) continue;
            lv_area_t *c = &b->cell[b->ncell++];
            c->x1 = imax(xs[s][0] - 1, box->x1);
            c->x2 = imin(xs[s][1] + 1, box->x2);
            c->y1 = y0;
            c->y2 = y1;
        }
    }
}

static void arc(lv_layer_t *layer, lv_draw_arc_dsc_t *d, const vu_band_t *b,
                lv_color_t c, int lo, int hi)
{
    if (hi <= lo) return;
    int from, to;
    abs_range(b, lo, hi, &from, &to);
    d->color       = c;
    d->start_angle = from;
    d->end_angle   = to;
    lv_draw_arc(layer, d);
}

/* Track, then the zones lit up to the bar, then the LED -- the old stack's
 * order -- each clipped to the cells the redrawn area meets. The cells do not
 * overlap, so every pixel is drawn once by each arc that covers it. Never
 * invalidate from here (LVGL asserts). */
static void draw_cb(lv_event_t *e)
{
    vu_band_t  *b     = lv_event_get_user_data(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    const lv_area_t clip0 = layer->_clip_area;
    lv_draw_arc_dsc_t d;
    lv_draw_arc_dsc_init(&d);
    d.base.layer = layer;
    /* The opacity it inherits -- the screen fading in after the splash --
     * as lv_obj_init_draw_arc_dsc() would give an lv_arc. */
    d.opa = layer->opa;
    if (d.opa <= LV_OPA_MIN) return;
    d.center.x = b->cx;
    d.center.y = b->cy;
    d.radius   = (uint16_t)b->r;
    d.width    = b->w;
    d.rounded  = 0;
    for (int c = 0; c < b->ncell; c++) {
        lv_area_t cc, t;
        if (!isect(&cc, &b->cell[c], &clip0)) continue;
        layer->_clip_area = cc;
        arc(layer, &d, b, b->track, 0, b->span);
        for (int z = 0; z < b->nz && b->a0[z] < b->bar; z++)
            if (isect(&t, &cc, &b->zbox[z]))
                arc(layer, &d, b, b->zc[z], b->a0[z], imin(b->bar, b->a1[z]));
        if (b->led >= 0 && b->led_z >= 0 && isect(&t, &cc, &b->ledbox))
            arc(layer, &d, b, b->zc[b->led_z], b->led, b->led + b->led_deg);
    }
    layer->_clip_area = clip0;
}

/* Redraw the sweep lo..hi, in pieces: one box over a long sweep would take in
 * the ring's hole, and every label inside it. */
static void band_inv(vu_band_t *b, int lo, int hi)
{
    if (hi <= lo) return;
    int piece = (hi - lo + PIECES_MAX - 1) / PIECES_MAX;
    if (piece < PIECE_MIN) piece = PIECE_MIN;
    for (int a = lo; a < hi; a += piece) {
        lv_area_t box;
        sector_box(b, a, imin(a + piece, hi), &box);
        lv_obj_invalidate_area(b->obj, &box);
    }
}

void vu_band_build(vu_band_t *b, lv_obj_t *parent, int cx, int cy, int rot, int span,
                   int r, int w, bool mirror, int nz, const int16_t *a0, const int16_t *a1,
                   const uint32_t *rgb, lv_color_t track, int led_deg)
{
    b->cx = (int16_t)cx; b->cy = (int16_t)cy;
    b->rot = (int16_t)rot; b->span = (int16_t)span;
    b->r = (int16_t)r; b->w = (int16_t)w;
    b->mirror = mirror;
    b->nz = (uint8_t)(nz > VU_BAND_ZONES ? VU_BAND_ZONES : nz);
    b->track = track;
    b->led_deg = (int16_t)led_deg;
    b->bar = 0;
    b->led = -1;
    b->led_z = -1;
    for (int z = 0; z < b->nz; z++) {
        b->a0[z] = a0[z];
        b->a1[z] = a1[z];
        b->zc[z] = lv_color_hex(rgb[z]);
        sector_box(b, a0[z], a1[z], &b->zbox[z]);
    }
    lv_area_t box;
    sector_box(b, 0, span, &box);
    build_cells(b, &box);

    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(o, box.x1, box.y1);
    lv_obj_set_size(o, lv_area_get_width(&box), lv_area_get_height(&box));
    lv_obj_add_event_cb(o, draw_cb, LV_EVENT_DRAW_MAIN, b);
    b->obj = o;
}

void vu_band_set(vu_band_t *b, float bar_f, float pk_f, int pk_z)
{
    if (!b->obj) return;
    if (bar_f < 0.0f) bar_f = 0.0f;
    if (bar_f > 1.0f) bar_f = 1.0f;
    const int bar = (int)(bar_f * (float)b->span);
    /* The LED's rule as the old stack had it (led_set): its far edge at the
     * peak, rounded, kept on the scale. */
    int led = -1, z = -1;
    if (pk_f > 0.0f && pk_z >= 0 && pk_z < b->nz) {
        if (pk_f > 1.0f) pk_f = 1.0f;
        led = (int)(pk_f * (float)b->span + 0.5f) - b->led_deg;
        if (led < 0) led = 0;
        if (led > b->span - b->led_deg) led = b->span - b->led_deg;
        z = pk_z;
    }
    if (bar != b->bar) {
        band_inv(b, imin(bar, b->bar), imax(bar, b->bar));
        b->bar = (int16_t)bar;
    }
    if (led != b->led || z != b->led_z) {
        const int was = b->led;
        if (was >= 0 && led >= 0 && abs(led - was) <= b->led_deg) {
            band_inv(b, imin(led, was), imax(led, was) + b->led_deg);   /* one sweep */
        } else {
            if (was >= 0) band_inv(b, was, was + b->led_deg);
            if (led >= 0) band_inv(b, led, led + b->led_deg);
        }
        b->led   = (int16_t)led;
        b->led_z = (int8_t)z;
        if (led >= 0) sector_box(b, led, led + b->led_deg, &b->ledbox);
    }
}

void vu_band_show(vu_band_t *b, bool on)
{
    if (!b->obj) return;
    if (on == !lv_obj_has_flag(b->obj, LV_OBJ_FLAG_HIDDEN)) return;
    if (on) lv_obj_remove_flag(b->obj, LV_OBJ_FLAG_HIDDEN);
    else    lv_obj_add_flag(b->obj, LV_OBJ_FLAG_HIDDEN);
}
