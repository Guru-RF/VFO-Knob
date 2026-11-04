/* The clock faces (clockface.h): four of the knob's own, after looking at
 * MilovdZee's Analog_Clock-esp32 and Alan Wang's Saiko watch (ideas only:
 * the faces here are drawn by this code, nothing of theirs is in it).
 *
 *   Shack    local time on the hands, UTC on a 24-hour ring with its own
 *            red hand, the date: for the radio room
 *   Diver    a turning bezel, lume, a day-date window at 3, the second hand
 *            sweeping six steps a second
 *   Classic  a white station dial, black bars, a red second hand
 *   Digital  big digits, the date, UTC
 *
 * The dial is drawn once a day into a picture of its own; each tick copies
 * back only the box the moving hands covered and draws every hand again,
 * clipped to it. */
#include "clockface.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "wallclock.h"

LV_FONT_DECLARE(font_hack_96);

#define S 360
#define C 180

static uint16_t *s_dial;                 /* the dial alone */
static lv_obj_t *s_dial_cv, *s_frame_cv; /* canvases never shown */
static bool      s_full = true;
static const uint16_t *s_last_px;        /* the picture drawn into last */
static int       s_key = -1;             /* what the dial was drawn for */
static char      s_mark[sizeof ((cf_opts_t *)0)->mark];   /* ...and the mark it carries */

/* A canvas's layer draws nothing until it is finished, and until then each
 * draw waits in LVGL's pool -- 64 kB on the knob, where the shack clock's
 * dial, some 150 draws at once, ran it out (a NULL from lv_malloc, written
 * through in circ_calc_aa4). So a batch at a time, as the radar's map. */
#ifndef CF_BATCH                         /* tools/lvhost/clockfaces sets it, to compare */
#define CF_BATCH 16
#endif
static lv_obj_t *s_layer_cv;             /* the canvas being drawn into */
static int       s_queued;               /* draws waiting in its layer */

static void layer_begin(lv_obj_t *cv, lv_layer_t *l)
{
    s_layer_cv = cv;
    s_queued = 0;
    lv_canvas_init_layer(cv, l);
}

/* After each draw: a full batch drawn, the layer begun again, its clip kept. */
static void queued(lv_layer_t *l)
{
    if (++s_queued < CF_BATCH) return;
    const lv_area_t clip = l->_clip_area, phy = l->phy_clip_area;
    lv_canvas_finish_layer(s_layer_cv, l);
    lv_canvas_init_layer(s_layer_cv, l);
    l->_clip_area = clip;
    l->phy_clip_area = phy;
    s_queued = 0;
}

/* ---- drawing ---- */

static lv_point_precise_t polar(float deg, float r)
{
    const float a = deg * (float)M_PI / 180.0f;
    return (lv_point_precise_t){ C + r * sinf(a), C - r * cosf(a) };
}

static void disc(lv_layer_t *l, float cx, float cy, int r, uint32_t hex)
{
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = lv_color_hex(hex);
    d.radius   = LV_RADIUS_CIRCLE;
    const lv_area_t a = { (int)lroundf(cx) - r, (int)lroundf(cy) - r, (int)lroundf(cx) + r, (int)lroundf(cy) + r };
    lv_draw_rect(l, &d, &a);
    queued(l);
}

static void ring(lv_layer_t *l, int r, int w, uint32_t hex)
{
    lv_draw_arc_dsc_t d;
    lv_draw_arc_dsc_init(&d);
    d.center = (lv_point_t){ C, C };
    d.radius = (uint16_t)r;
    d.width = w;
    d.start_angle = 0;
    d.end_angle = 360;
    d.color = lv_color_hex(hex);
    lv_draw_arc(l, &d);
    queued(l);
}

/* A line out from the middle at `deg` (0 at 12, clockwise), r0 to r1. */
static void ray(lv_layer_t *l, float deg, float r0, float r1, int w, uint32_t hex, bool round)
{
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.p1 = polar(deg, r0);
    d.p2 = polar(deg, r1);
    d.width = w;
    d.color = lv_color_hex(hex);
    d.round_start = d.round_end = round;
    lv_draw_line(l, &d);
    queued(l);
}

static void text_at(lv_layer_t *l, const char *t, const lv_font_t *f, uint32_t hex, float cx, float cy)
{
    lv_point_t sz;
    lv_text_get_size(&sz, t, f, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    lv_draw_label_dsc_t d;
    lv_draw_label_dsc_init(&d);
    d.text = t;
    d.text_local = 1;        /* drawn at the layer's end: the text copied, its buffer reused meanwhile */
    d.font = f;
    d.color = lv_color_hex(hex);
    const int x = (int)lroundf(cx) - sz.x / 2, y = (int)lroundf(cy) - sz.y / 2;
    const lv_area_t a = { x, y, x + sz.x, y + sz.y };
    lv_draw_label(l, &d, &a);
    queued(l);
}

static const char *const WDAY[7]   = { "SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT" };
static const char *const MONTH[12] = { "JAN", "FEB", "MAR", "APR", "MAY", "JUN",
                                       "JUL", "AUG", "SEP", "OCT", "NOV", "DEC" };

#define LUME     0xE4EDD3
#define LUME_RIM 0x8E9686

/* ---- the dials ---- */

static void dial_shack(lv_layer_t *l, const struct tm *lt, bool have, const char *mark)
{
    disc(l, C, C, 179, 0x0B1424);
    ring(l, 179, 2, 0x2A3A55);
    for (int i = 0; i < 60; i++) {
        if (i % 5) ray(l, i * 6.0f, 161, 171, 2, 0x8A9BB5, false);
        else       ray(l, i * 6.0f, 150, 171, 4, 0xFFFFFF, false);
    }
    for (int h = 1; h <= 12; h++) {
        char n[4];
        snprintf(n, sizeof n, "%d", h);
        const lv_point_precise_t p = polar(h * 30.0f, 131);
        text_at(l, n, &lv_font_montserrat_28, 0xFFFFFF, p.x, p.y);
    }
    /* UTC, 24 hours round: 0 at the top, noon at the bottom. */
    ring(l, 112, 1, 0x2E4A73);
    for (int h = 0; h < 24; h++) {
        ray(l, h * 15.0f, 107, 113, 2, 0x5A9BFF, false);
        char n[4];
        snprintf(n, sizeof n, "%d", h);
        const lv_point_precise_t p = polar(h * 15.0f, 97);
        text_at(l, n, &lv_font_montserrat_14, 0x7FA9E8, p.x, p.y);
    }
    /* The mark, where there is one, in the name's place: inside the 24-hour
     * ring there is room for one line there and nowhere else. */
    if (mark[0]) text_at(l, mark, &lv_font_montserrat_14, 0xFFB000, C, C - 48);
    else         text_at(l, "VFO KNOB", &lv_font_montserrat_14, 0x8A9BB5, C, C - 48);
    if (have) {
        char d[24];
        snprintf(d, sizeof d, "%s %d %s", WDAY[lt->tm_wday], lt->tm_mday, MONTH[lt->tm_mon]);
        text_at(l, d, &lv_font_montserrat_14, 0xFFFFFF, C, C + 46);
        text_at(l, "UTC", &lv_font_montserrat_14, 0xFF5A36, C, C + 64);
    }
}

static void dial_diver(lv_layer_t *l, const struct tm *lt, bool have)
{
    disc(l, C, C, 179, 0x060708);
    /* The bezel: 0 to 60, a triangle at 0 with its lume pip. */
    ring(l, 179, 29, 0x1B1E23);
    for (int i = 1; i < 60; i++) {
        if (i % 10 == 0) {
            char n[4];
            snprintf(n, sizeof n, "%d", i);
            const lv_point_precise_t p = polar(i * 6.0f, 165);
            text_at(l, n, &lv_font_montserrat_14, 0xE8ECF0, p.x, p.y);
        } else if (i % 5 == 0) ray(l, i * 6.0f, 158, 172, 4, 0xE8ECF0, false);
        else                   ray(l, i * 6.0f, 163, 171, 2, 0xA8ADB4, false);
    }
    {
        lv_draw_triangle_dsc_t t;
        lv_draw_triangle_dsc_init(&t);
        t.color = lv_color_hex(0xE8ECF0);
        t.p[0] = polar(0, 152);
        t.p[1] = (lv_point_precise_t){ C - 10, C - 176 };
        t.p[2] = (lv_point_precise_t){ C + 10, C - 176 };
        lv_draw_triangle(l, &t);
        queued(l);
        disc(l, C, C - 168, 3, LUME);
    }
    ring(l, 150, 2, 0x30343A);
    /* The indices, lume: dots, bars at 6 and 9, a double bar at 12, and at 3
     * the day-date window. */
    for (int h = 1; h <= 12; h++) {
        if (h == 3) continue;
        if (h == 12) {
            ray(l, -3.2f, 112, 142, 9, LUME_RIM, false); ray(l, -3.2f, 113, 141, 6, LUME, false);
            ray(l, 3.2f, 112, 142, 9, LUME_RIM, false);  ray(l, 3.2f, 113, 141, 6, LUME, false);
        } else if (h == 6 || h == 9) {
            ray(l, h * 30.0f, 110, 143, 13, LUME_RIM, false);
            ray(l, h * 30.0f, 111, 142, 10, LUME, false);
        } else {
            const lv_point_precise_t p = polar(h * 30.0f, 128);
            disc(l, p.x, p.y, 10, LUME_RIM);
            disc(l, p.x, p.y, 8, LUME);
        }
    }
    {
        lv_draw_rect_dsc_t r;
        lv_draw_rect_dsc_init(&r);
        r.bg_color = lv_color_hex(0xF4F4F0);
        r.radius = 3;
        r.border_width = 2;
        r.border_color = lv_color_hex(0x8C9096);
        const lv_area_t a = { C + 82, C - 13, C + 142, C + 13 };
        lv_draw_rect(l, &r, &a);
        queued(l);
        if (have) {
            char d[12];
            snprintf(d, sizeof d, "%s %d", WDAY[lt->tm_wday], lt->tm_mday);
            text_at(l, d, &lv_font_montserrat_14, 0x101214, C + 112, C);
        }
    }
    text_at(l, "VFO KNOB", &lv_font_montserrat_14, 0xE8ECF0, C, C - 62);
    text_at(l, "AUTOMATIC", &lv_font_montserrat_14, 0xFF5A36, C, C + 56);
}

static void dial_classic(lv_layer_t *l, const struct tm *lt, bool have)
{
    disc(l, C, C, 179, 0xF2F2EE);
    ring(l, 179, 4, 0x303030);
    for (int i = 0; i < 60; i++) {
        if (i % 5) ray(l, i * 6.0f, 161, 171, 3, 0x1A1A1A, false);
        else       ray(l, i * 6.0f, 128, 171, 10, 0x1A1A1A, false);
    }
    if (have) {
        char d[12];
        snprintf(d, sizeof d, "%s %d", WDAY[lt->tm_wday], lt->tm_mday);
        text_at(l, d, &lv_font_montserrat_14, 0x6A6A6A, C, C + 74);
    }
}

/* ---- the hands ---- */

typedef struct {
    lv_point_precise_t a, b;     /* tail, tip */
    int      w;
    uint32_t hex, rim;           /* rim: drawn 3 px wider under it; 0 none */
    bool     round;
    int      dot_r;              /* a disc on it at dot_at: the diver's lume */
    float    dot_at;
} hand_t;

#define N_HANDS 4                /* hour, minute, a third (UTC), second */
static hand_t s_was[N_HANDS];
static bool   s_was_on[N_HANDS];

static hand_t hand(float deg, float tail, float len, int w, uint32_t hex, uint32_t rim, bool round)
{
    return (hand_t){ polar(deg + 180.0f, tail), polar(deg, len), w, hex, rim, round, 0, 0 };
}

static lv_area_t hand_box(const hand_t *h)
{
    const int m = h->w / 2 + (h->rim ? 3 : 0) + 3 + h->dot_r;
    lv_area_t a = {
        (int32_t)floorf(fminf(h->a.x, h->b.x)) - m, (int32_t)floorf(fminf(h->a.y, h->b.y)) - m,
        (int32_t)ceilf(fmaxf(h->a.x, h->b.x)) + m,  (int32_t)ceilf(fmaxf(h->a.y, h->b.y)) + m,
    };
    return a;
}

static void hand_draw(lv_layer_t *l, const hand_t *h)
{
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.p1 = h->a;
    d.p2 = h->b;
    d.round_start = d.round_end = h->round;
    if (h->rim) {
        d.width = h->w + 3;
        d.color = lv_color_hex(h->rim);
        lv_draw_line(l, &d);
        queued(l);
    }
    d.width = h->w;
    d.color = lv_color_hex(h->hex);
    lv_draw_line(l, &d);
    queued(l);
    if (h->dot_r) {
        const float t = h->dot_at;
        const float x = h->a.x + (h->b.x - h->a.x) * t, y = h->a.y + (h->b.y - h->a.y) * t;
        disc(l, x, y, h->dot_r + 2, h->hex);
        disc(l, x, y, h->dot_r, LUME);
    }
}

static void join(lv_area_t *box, const lv_area_t *a)
{
    if (box->x1 > box->x2) { *box = *a; return; }
    box->x1 = LV_MIN(box->x1, a->x1); box->y1 = LV_MIN(box->y1, a->y1);
    box->x2 = LV_MAX(box->x2, a->x2); box->y2 = LV_MAX(box->y2, a->y2);
}

/* The hands for this moment, and the cap over them (as hand 0's dot). */
static int hands_for(const cf_opts_t *o, const struct tm *lt, const struct tm *ut, uint16_t ms, bool fresh,
                     hand_t *h, bool *on)
{
    const int sec = lt->tm_sec;
    const float hr  = (lt->tm_hour % 12) * 30.0f + lt->tm_min * 0.5f;
    const float mn  = lt->tm_min * 6.0f + (sec / 10) * 1.0f;          /* a degree each 10 s */
    const bool sweep = o->face == CF_DIVER;
    const float sc  = sec * 6.0f + (sweep ? (ms / 167) * 1.0f : 0.0f);  /* six steps a second */
    const uint32_t grey = 0x707884;
    memset(on, 0, N_HANDS * sizeof *on);
    switch (o->face) {
    case CF_SHACK:
        h[0] = hand(hr, 14, 80, 8, fresh ? 0xFFFFFF : grey, 0, true);
        h[1] = hand(mn, 18, 138, 5, fresh ? 0xFFFFFF : grey, 0, true);
        h[2] = hand((ut->tm_hour + ut->tm_min / 60.0f) * 15.0f, 0, 114, 2, fresh ? 0xFF5A36 : grey, 0, true);
        h[3] = hand(sc, 30, 152, 2, fresh ? 0xFFB000 : grey, 0, true);
        on[0] = on[1] = on[2] = true;
        on[3] = o->seconds;
        break;
    case CF_DIVER:
        h[0] = hand(hr, 0, 84, 12, fresh ? LUME : grey, 0x2A2E33, true);
        h[1] = hand(mn, 0, 128, 8, fresh ? LUME : grey, 0x2A2E33, true);
        h[3] = hand(sc, 34, 142, 3, fresh ? 0xE03A2E : grey, 0, true);
        h[3].dot_r = 5; h[3].dot_at = (34.0f + 98.0f) / (34.0f + 142.0f);
        on[0] = on[1] = true;
        on[3] = o->seconds;
        break;
    default:   /* CF_CLASSIC */
        h[0] = hand(hr, 24, 96, 14, fresh ? 0x1A1A1A : grey, 0, false);
        h[1] = hand(mn, 30, 150, 10, fresh ? 0x1A1A1A : grey, 0, false);
        h[3] = hand(sc, 42, 142, 3, fresh ? 0xD7261E : grey, 0, true);
        on[0] = on[1] = true;
        on[3] = o->seconds;
        break;
    }
    return N_HANDS;
}

static void cap(lv_layer_t *l, uint8_t face)
{
    switch (face) {
    case CF_SHACK:   disc(l, C, C, 6, 0xFFFFFF); disc(l, C, C, 2, 0x0B1424); break;
    case CF_DIVER:   disc(l, C, C, 7, 0xC0C4C8); break;
    default:         disc(l, C, C, 5, 0xD7261E); break;
    }
}

static bool same(const hand_t *a, const hand_t *b)
{
    return a->a.x == b->a.x && a->a.y == b->a.y && a->b.x == b->b.x && a->b.y == b->b.y && a->hex == b->hex;
}

/* ---- the digital face ---- */

static const lv_area_t DIGITAL_BOX = { C - 160, C - 76, C + 159, C + 99 };

static void digital(lv_layer_t *l, const cf_opts_t *o, const struct tm *lt, const struct tm *ut, bool have,
                    bool fresh)
{
    lv_draw_rect_dsc_t r;
    lv_draw_rect_dsc_init(&r);
    r.bg_color = lv_color_hex(0x000000);
    lv_draw_rect(l, &r, &DIGITAL_BOX);
    queued(l);
    char big[8], line[32], utc[16];
    if (!have) {
        strcpy(big, "--:--");
        strcpy(line, "NO TIME YET");
        utc[0] = 0;
    } else {
        int hh = lt->tm_hour;
        if (!o->h24) hh = hh % 12 ? hh % 12 : 12;
        snprintf(big, sizeof big, o->h24 ? "%02d%c%02d" : "%d%c%02d", hh, lt->tm_sec % 2 ? ' ' : ':', lt->tm_min);
        snprintf(line, sizeof line, "%s %d %s%s", WDAY[lt->tm_wday], lt->tm_mday, MONTH[lt->tm_mon],
                 o->h24 ? "" : lt->tm_hour < 12 ? "  AM" : "  PM");
        snprintf(utc, sizeof utc, "%02d:%02d UTC", ut->tm_hour, ut->tm_min);
    }
    text_at(l, big, &font_hack_96, fresh ? 0xFFFFFF : 0x707884, C, C - 22);
    text_at(l, line, &lv_font_montserrat_20, fresh ? 0xC0C8D4 : 0x707884, C, C + 34);
    if (utc[0]) text_at(l, utc, &lv_font_montserrat_20, fresh ? 0x5A9BFF : 0x707884, C, C + 58);
    if (o->mark[0]) text_at(l, o->mark, &lv_font_montserrat_20, 0x707884, C, C + 88);
}

/* ---- the face ---- */

void clockface_reset(void) { s_full = true; }

uint32_t clockface_wait_ms(const cf_opts_t *o)
{
    struct tm lt;
    uint16_t ms;
    if (!wallclock_now(&lt, NULL, &ms)) return 1000;
    if (o->face == CF_DIVER && o->seconds) return 167 - ms % 167;
    if (o->seconds || o->face == CF_DIGITAL) return 1000 - ms;
    return (uint32_t)(60 - lt.tm_sec) * 1000u - ms;
}

static lv_obj_t *canvas_on(lv_obj_t **cv, uint16_t *px)
{
    if (!*cv) *cv = lv_canvas_create(NULL);
    lv_canvas_set_buffer(*cv, px, S, S, LV_COLOR_FORMAT_RGB565);
    return *cv;
}

bool clockface_draw(uint16_t *px, const cf_opts_t *o, lv_area_t *dirty)
{
    struct tm lt = { 0 }, ut = { 0 };
    uint16_t ms = 0;
    const bool have  = wallclock_now(&lt, &ut, &ms);
    const bool fresh = have && wallclock_fresh();
    const uint8_t face = o->face < CF_N ? o->face : CF_SHACK;
    /* Another picture than the last -- the Clock firmware's face, then the
     * screensaver's -- is drawn whole: the hands' last places are the
     * other's. */
    if (px != s_last_px) { s_last_px = px; s_full = true; }

    if (face == CF_DIGITAL) {
        lv_layer_t l;
        layer_begin(canvas_on(&s_frame_cv, px), &l);
        if (s_full) {
            lv_draw_rect_dsc_t r;
            lv_draw_rect_dsc_init(&r);
            r.bg_color = lv_color_hex(0x000000);
            const lv_area_t all = { 0, 0, S - 1, S - 1 };
            lv_draw_rect(&l, &r, &all);
            queued(&l);
        }
        digital(&l, o, &lt, &ut, have, fresh);
        lv_canvas_finish_layer(s_frame_cv, &l);
        *dirty = s_full ? (lv_area_t){ 0, 0, S - 1, S - 1 } : DIGITAL_BOX;
        s_full = false;
        s_key = -1;
        return true;
    }

    /* The dial again on a new day, a new face, or the time come or gone. */
    const int key = face | (have ? 0x10 : 0) | (have ? lt.tm_yday << 5 : 0);
    if (!s_dial) s_dial = heap_caps_malloc((size_t)S * S * 2, MALLOC_CAP_SPIRAM);
    if (!s_dial) return false;
    if (key != s_key || strcmp(o->mark, s_mark)) {
        lv_layer_t l;
        layer_begin(canvas_on(&s_dial_cv, s_dial), &l);
        lv_draw_rect_dsc_t r;
        lv_draw_rect_dsc_init(&r);
        r.bg_color = lv_color_hex(0x000000);
        const lv_area_t all = { 0, 0, S - 1, S - 1 };
        lv_draw_rect(&l, &r, &all);
        queued(&l);
        switch (face) {
        case CF_SHACK:  dial_shack(&l, &lt, have, o->mark); break;
        case CF_DIVER:  dial_diver(&l, &lt, have);   break;
        default:        dial_classic(&l, &lt, have); break;
        }
        /* Where the date goes: inside the shack clock's UTC ring, under the
         * others' AUTOMATIC or in their lower half. */
        if (!have) text_at(&l, "NO TIME YET", &lv_font_montserrat_20, face == CF_CLASSIC ? 0x6A6A6A : 0xC0C8D4,
                           C, face == CF_SHACK ? C + 50 : C + 90);
        /* The mark where the other dials have room: under the diver's
         * AUTOMATIC, under the classic's date. */
        if (o->mark[0] && face != CF_SHACK)
            text_at(&l, o->mark, &lv_font_montserrat_14, face == CF_DIVER ? 0xE8ECF0 : 0x6A6A6A,
                    C, face == CF_DIVER ? C + 82 : C + 100);
        lv_canvas_finish_layer(s_dial_cv, &l);
        s_key = key;
        strlcpy(s_mark, o->mark, sizeof s_mark);
        s_full = true;
    }

    hand_t h[N_HANDS];
    bool on[N_HANDS];
    if (have) hands_for(o, &lt, &ut, ms, fresh, h, on);
    else memset(on, 0, sizeof on);

    lv_area_t box = { 1, 1, 0, 0 };
    if (s_full) {
        box = (lv_area_t){ 0, 0, S - 1, S - 1 };
    } else {
        for (int i = 0; i < N_HANDS; i++) {
            if (on[i] == s_was_on[i] && (!on[i] || same(&h[i], &s_was[i]))) continue;
            if (s_was_on[i]) { const lv_area_t a = hand_box(&s_was[i]); join(&box, &a); }
            if (on[i])       { const lv_area_t a = hand_box(&h[i]);     join(&box, &a); }
        }
        if (box.x1 > box.x2) return false;
        box.x1 = LV_MAX(box.x1, 0) & ~1;
        box.y1 = LV_MAX(box.y1, 0);
        box.x2 = LV_MIN(box.x2 | 1, S - 1);
        box.y2 = LV_MIN(box.y2, S - 1);
    }
    /* The dial back under the box, every hand drawn again inside it. */
    for (int y = box.y1; y <= box.y2; y++)
        memcpy(px + y * S + box.x1, s_dial + y * S + box.x1, (size_t)(box.x2 - box.x1 + 1) * 2);
    lv_layer_t l;
    layer_begin(canvas_on(&s_frame_cv, px), &l);
    l._clip_area = box;
    for (int i = 0; i < N_HANDS; i++) if (on[i]) hand_draw(&l, &h[i]);
    if (have) cap(&l, face);
    lv_canvas_finish_layer(s_frame_cv, &l);
    memcpy(s_was, h, sizeof s_was);
    memcpy(s_was_on, on, sizeof s_was_on);
    *dirty = box;
    s_full = false;
    return true;
}
