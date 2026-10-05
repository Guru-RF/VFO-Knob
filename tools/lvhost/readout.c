/* The frequency readout on the host (tools/lvhost): ui.c's eight digit
 * labels and two separators, placed as dig_place() places them, the step's
 * underline under its digit, between the neighbours build() gives them -- the
 * band row above; the step row and the slab below; either side the S-meter's
 * track, notches and ticks, and in transmit the hairline, the microphone's
 * ring and the scales' numbers -- in the icom firmware's colours, rendered
 * with the same LVGL 9.3 on the 360 px glass: in Montserrat 48 at the places
 * it had, and in Hack 46 (components/ui/font_hack_46.c) at its own.
 *
 * For each frequency: where the readout's ink lies, the narrowest gap between
 * two digits and between a digit and a separator, how near it comes to its
 * neighbours and to the glass's edge, and partial redraws against full ones.
 * Then every place rolled through 0-9, in each of the three layouts: how far
 * a digit's ink moves with its own value, the narrowest gap any two values
 * leave beside each other, and whether a neighbour's ink ever reaches into a
 * digit's place as the neighbours roll. Last each layout's envelope -- every
 * place at every value at once, and the underline under each place -- in
 * receive and in transmit: the nearest any frequency at all brings the
 * readout to the face and to the glass's edge, not only the eight here.
 *
 *   make readout-check OUT=dir    writes dir/readout-NN-<freq>.png, the old
 *                                 face left and the new right, the same
 *                                 enlarged (-zoom), the envelopes on the
 *                                 transmit face as dir/readout-env-*.png,
 *                                 and prints the measures; non-zero if the
 *                                 new readout has a problem */
#include "lvgl.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define W 360
#define H 360
/* ui.c's geometry. */
#define CX       180
#define CY       180
#define ARC_R0   170
#define ARC_ROT  170
#define ARC_SPAN 200
#define SWR_ROT  ARC_ROT
#define SWR_SPAN (ARC_SPAN / 2 - 3)
#define AUD_ROT  (ARC_ROT + ARC_SPAN / 2 + 3)
#define AUD_SPAN (ARC_SPAN / 2 - 3)
#define AUX_DX   72
#define PTT_TOP  248
#define N_DIG    8
/* The icom firmware's colours (ui.c, VFO_RADIO_ICOM): white digits on black. */
#define C_BG        lv_color_hex(0x000000)
#define C_BG1       lv_color_hex(0x141A24)
#define C_BG_TX     lv_color_hex(0x2A0508)
#define C_ACCENT    lv_color_hex(0x2F7BFF)
#define C_ACCENT_HI lv_color_hex(0x5A9BFF)
#define C_TEXT      lv_color_hex(0xFFFFFF)
#define C_TEXT2     lv_color_hex(0xC0C8D4)
#define C_LABEL     lv_color_hex(0x707884)
#define C_SUBTLE    lv_color_hex(0x181C24)
#define C_WARN      lv_color_hex(0xFFB000)
#define C_DANGER    lv_color_hex(0xFF3030)
#define C_TX_TEXT   lv_color_hex(0xFFFFFF)
#define C_TX_RED    lv_color_hex(0xE60012)
#define C_BRAND     lv_color_hex(0x3FA9FF)
#define SYM_MIC "\xEF\x84\xB0"

LV_FONT_DECLARE(font_hack_46);
LV_FONT_DECLARE(font_mic_14);

/* ui.c: what each place steps, below and from 1 GHz. */
static const int DIG_STEP[N_DIG] = {
    1000000, 1000000, 1000000, 100000, 10000, 1000, 100, 10,
};
static const int DIG_STEP_GHZ[N_DIG] = {
    1000000, 1000000, 1000000, 1000000, 100000, 10000, 1000, 100,
};

/* ui.c band_of() (the icom firmware's) and step_name(): the band row's band
 * and the step row's step, as the knob writes them for the frequency. */
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
    if (m >= 50000  && m <= 54000)  return "6m";
    if (m >= 70000  && m <= 70500)  return "4m";
    if (m >= 144000 && m <= 148000) return "2m";
    if (m >= 430000 && m <= 440000) return "70cm";
    if (m >= 1240000 && m <= 1300000) return "23cm";
    if (m >= 2300000 && m <= 2450000) return "13cm";
    if (m >= 5650000 && m <= 5925000) return "6cm";
    if (m >= 10000000 && m <= 10500000) return "3cm";
    return "--";
}
static const char *step_name(int hz)
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

/* ---- the readout ---- */
typedef struct {
    bool hack;                            /* the new one, or as it was */
    lv_obj_t *dig[N_DIG], *sep[2], *underline;
    int x[N_DIG];                         /* ui.c s_dig_x */
    uint8_t lay;
    int active;
} readout_t;

/* ui.c dig_place() as it was, for Montserrat 48: MMM.kkk.hh, the sub-kHz
 * pair narrower; from 1 GHz MMMM.kkk.h; from 10 GHz "10" in a place 50 wide
 * and the rest closed up. */
static void place_old(readout_t *r, uint8_t lay)
{
    const int n_small = lay ? 1 : 2, sep_a = lay ? 3 : 2, sep_b = lay ? 6 : 5;
    const int pitch = lay == 2 ? 31 : 33, small = lay == 2 ? 26 : 28,
              sepw  = lay == 2 ? 9 : 11,   first = lay == 2 ? 50 : pitch;
    const int total = first + (N_DIG - 1 - n_small) * pitch + n_small * small + 2 * sepw;
    int x = CX - total / 2, sep = 0;
    for (int i = 0; i < N_DIG; i++) {
        const int w = i == 0 ? first : i >= N_DIG - n_small ? small : pitch;
        r->x[i] = x + w / 2;
        lv_obj_align(r->dig[i], LV_ALIGN_CENTER, r->x[i] - CX, 170 - CY);
        x += w;
        if (i == sep_a || i == sep_b) {
            lv_obj_align(r->sep[sep++], LV_ALIGN_CENTER, x + sepw / 2 - CX, 170 - CY);
            x += sepw;
        }
    }
    r->lay = lay;
}

/* ui.c dig_place() now, for Hack 46: every digit the font's own advance --
 * the "10" from 10 GHz two of them -- and a separator a narrower place. */
#define DIG_PITCH 28
#define DIG_SEPW  12
#define DIG_LINE  (DIG_PITCH - 6)             /* the underline's width */
static void place_new(readout_t *r, uint8_t lay)
{
    const int sep_a = lay ? 3 : 2, sep_b = lay ? 6 : 5;
    const int first = lay == 2 ? 2 * DIG_PITCH : DIG_PITCH;
    const int total = first + (N_DIG - 1) * DIG_PITCH + 2 * DIG_SEPW;
    int x = CX - total / 2, sep = 0;
    for (int i = 0; i < N_DIG; i++) {
        const int w = i == 0 ? first : DIG_PITCH;
        r->x[i] = x + w / 2;
        lv_obj_align(r->dig[i], LV_ALIGN_CENTER, r->x[i] - CX, 170 - CY);
        x += w;
        if (i == sep_a || i == sep_b) {
            lv_obj_align(r->sep[sep++], LV_ALIGN_CENTER, x + DIG_SEPW / 2 - CX, 170 - CY);
            x += DIG_SEPW;
        }
    }
    r->lay = lay;
}

static void place(readout_t *r, uint8_t lay) { (r->hack ? place_new : place_old)(r, lay); }

static lv_obj_t *mklabel(lv_obj_t *scr, const lv_font_t *f, lv_color_t c, int x, int y, const char *t)
{
    lv_obj_t *l = lv_label_create(scr);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, c, 0);
    lv_label_set_text(l, t);
    lv_obj_align(l, LV_ALIGN_CENTER, x - CX, y - CY);
    return l;
}

/* ui.c build(): the digits, the separators, the underline. */
static void readout_build(readout_t *r, lv_obj_t *scr, bool hack)
{
    const lv_font_t *f = hack ? &font_hack_46 : &lv_font_montserrat_48;
    r->hack = hack;
    for (int i = 0; i < N_DIG; i++) r->dig[i] = mklabel(scr, f, C_TEXT, CX, 170, "0");
    for (int i = 0; i < 2; i++) r->sep[i] = mklabel(scr, f, C_LABEL, CX, 170, ".");
    place(r, 0);
    r->underline = lv_obj_create(scr);
    lv_obj_set_size(r->underline, hack ? DIG_LINE : 33 - 9, 3);
    lv_obj_set_style_bg_color(r->underline, C_ACCENT, 0);
    lv_obj_set_style_border_width(r->underline, 0, 0);
    lv_obj_set_style_radius(r->underline, 2, 0);
    lv_obj_remove_flag(r->underline, LV_OBJ_FLAG_SCROLLABLE);
    r->active = 5;
}

static int step_of(const readout_t *r, int i) { return (r->lay ? DIG_STEP_GHZ : DIG_STEP)[i]; }

/* ui.c ui_update(): the layout for the frequency (the icom firmware's three),
 * the step's place, the digits -- the leading ones blank below 100 MHz --
 * their colours, and the underline. Returns the step it took: from 1 GHz no
 * finer than 100 Hz. */
static int show(readout_t *r, int64_t f, int step, bool tx)
{
    const uint8_t lay = f >= 10000000000LL ? 2 : f >= 1000000000LL ? 1 : 0;
    if (lay != r->lay) place(r, lay);
    if (lay && step < 100) step = 100;
    for (int i = N_DIG - 1; i >= 0; i--)
        if (step_of(r, i) == step) { r->active = i; break; }
    const int mhz = (int)(f / 1000000), khz = (int)((f / 1000) % 1000), hz = (int)((f % 1000) / 10);
    int d[N_DIG] = {
        (mhz / 100) % 10, (mhz / 10) % 10, mhz % 10,
        (khz / 100) % 10, (khz / 10) % 10, khz % 10,
        (hz / 10) % 10,   hz % 10,
    };
    int lead = (mhz >= 100) ? 0 : (mhz >= 10) ? 1 : 2;
    if (r->lay) {
        const int dd[N_DIG] = {
            (mhz / 1000) % (r->lay == 2 ? 100 : 10),
            (mhz / 100) % 10, (mhz / 10) % 10, mhz % 10,
            (khz / 100) % 10,  (khz / 10) % 10,  khz % 10,        hz / 10,
        };
        memcpy(d, dd, sizeof d);
        lead = 0;
    }
    for (int i = 0; i < N_DIG; i++) {
        char b[3] = { (char)('0' + d[i] % 10), 0, 0 };
        if (d[i] >= 10) { b[0] = (char)('0' + d[i] / 10); b[1] = (char)('0' + d[i] % 10); }
        lv_label_set_text(r->dig[i], i < lead ? "" : b);
        const lv_color_t c = i == r->active ? C_ACCENT_HI : i > r->active ? C_TEXT2 : C_TEXT;
        lv_obj_set_style_text_color(r->dig[i], tx ? C_TX_TEXT : c, 0);
    }
    lv_obj_align(r->underline, LV_ALIGN_CENTER, r->x[r->active] - CX, 204 - CY);
    return step;
}

static void readout_hide(readout_t *r, bool hide)
{
    lv_obj_t *o[N_DIG + 3];
    for (int i = 0; i < N_DIG; i++) o[i] = r->dig[i];
    o[N_DIG] = r->sep[0]; o[N_DIG + 1] = r->sep[1]; o[N_DIG + 2] = r->underline;
    for (int i = 0; i < N_DIG + 3; i++) {
        if (hide) lv_obj_add_flag(o[i], LV_OBJ_FLAG_HIDDEN);
        else      lv_obj_remove_flag(o[i], LV_OBJ_FLAG_HIDDEN);
    }
}

/* ---- the face around it: what ui.c build() puts near the readout ---- */
typedef struct {
    lv_obj_t *scr, *ring, *meter, *rx_zone[8], *rx_ticks;
    lv_obj_t *swr_zone[3], *pwr_arc, *mic_zone[3], *tx_ticks;
    lv_obj_t *srd, *dbm, *aux[4], *band, *mode, *filt, *step, *rit, *vol, *mic, *ptt, *ptt_lbl;
    lv_point_precise_t pts[32][2];        /* the lines keep pointers to them */
} face_t;

static float smeter_frac(float dbm)
{
    if (dbm < -127.0f) dbm = -127.0f;
    if (dbm > -13.0f)  dbm = -13.0f;
    return dbm <= -73.0f ? 0.6f * (dbm + 127.0f) / 54.0f : 0.6f + 0.4f * (dbm + 73.0f) / 60.0f;
}
static float swr_frac(float w)
{
    if (w <= 1.0f) return 0.0f;
    const float f = (w - 1.0f) / 2.0f;
    return f > 1.0f ? 1.0f : f;
}
static float mic_frac(float db) { return (db + 40.0f) / 50.0f; }

/* An lv_arc as ui.c's meters are made: a band `width` wide inside a circle
 * of radius r, from rot over span degrees, its indicator filled to `fill`. */
static lv_obj_t *mkarc(lv_obj_t *scr, int r, int rot, int span, int width, lv_color_t track,
                       bool track_on, lv_color_t fillc, float fill, bool reverse)
{
    lv_obj_t *a = lv_arc_create(scr);
    lv_obj_set_size(a, r * 2, r * 2);
    lv_obj_center(a);
    lv_arc_set_rotation(a, rot);
    lv_arc_set_bg_angles(a, 0, span);
    lv_arc_set_range(a, 0, 1000);
    if (reverse) lv_arc_set_mode(a, LV_ARC_MODE_REVERSE);
    lv_arc_set_value(a, (int)(fill * 1000));
    lv_obj_remove_style(a, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(a, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(a, width, LV_PART_MAIN);
    lv_obj_set_style_arc_color(a, track, LV_PART_MAIN);
    if (!track_on) lv_obj_set_style_arc_opa(a, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_arc_width(a, width, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(a, fillc, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(a, false, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(a, false, LV_PART_INDICATOR);
    return a;
}
static lv_obj_t *mkgroup(lv_obj_t *scr)
{
    lv_obj_t *g = lv_obj_create(scr);
    lv_obj_set_size(g, 360, 360);
    lv_obj_set_pos(g, 0, 0);
    lv_obj_set_style_bg_opa(g, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(g, 0, 0);
    lv_obj_set_style_pad_all(g, 0, 0);
    lv_obj_remove_flag(g, LV_OBJ_FLAG_SCROLLABLE);
    return g;
}
/* A line between radii r0 and r1 at deg, as ui.c's ticks and notches. */
static void radial(lv_obj_t *g, lv_point_precise_t p[2], float deg, int r0, int r1, int width,
                   lv_color_t c, bool rounded)
{
    const float a = deg * 3.14159265f / 180.0f, co = cosf(a), sn = sinf(a);
    p[0].x = (lv_value_precise_t)(CX + r0 * co);
    p[0].y = (lv_value_precise_t)(CY + r0 * sn);
    p[1].x = (lv_value_precise_t)(CX + r1 * co);
    p[1].y = (lv_value_precise_t)(CY + r1 * sn);
    lv_obj_t *ln = lv_line_create(g);
    lv_line_set_points(ln, p, 2);
    lv_obj_set_style_line_width(ln, width, 0);
    lv_obj_set_style_line_color(ln, c, 0);
    lv_obj_set_style_line_rounded(ln, rounded, 0);
}
static void radial_label(lv_obj_t *g, float deg, const char *t, lv_color_t c)
{
    const float a = deg * 3.14159265f / 180.0f;
    lv_obj_t *l = lv_label_create(g);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(l, c, 0);
    lv_label_set_text(l, t);
    lv_obj_align(l, LV_ALIGN_CENTER, (int)(128 * cosf(a)), (int)(128 * sinf(a)));
}

static void face_build(face_t *fc, lv_obj_t *scr)
{
    static const struct { float from, to; uint32_t rgb; } RXZ[8] = {
        { -127, -121, 0x0D3B8C }, { -121, -109, 0x1350B0 }, { -109, -97, 0x1A68D4 },
        {  -97,  -85, 0x2A86F2 }, {  -85,  -73, 0x46A8FF }, {  -73, -53, 0xFF6A5A },
        {  -53,  -33, 0xFF4040 }, {  -33,  -13, 0xE60012 },
    };
    static const struct { float from, to; uint32_t rgb; } SWRZ[3] = {
        { 1.0f, 2.0f, 0x3FA9FF }, { 2.0f, 2.5f, 0xFFB000 }, { 2.5f, 3.0f, 0xFF3030 },
    };
    static const struct { float from, to; uint32_t rgb; } MICZ[3] = {
        { -40, -10, 0x3FA9FF }, { -10, 0, 0xFFB000 }, { 0, 10, 0xFF3030 },
    };
    static const float RXNOTCH[] = { -121, -109, -97, -85, -73, -53, -33 };
    static const struct { float dbm; int len, kind; } TICKS[] = {
        { -121, 6, 0 }, { -109, 6, 0 }, { -97, 6, 0 }, { -85, 6, 0 },
        { -73, 11, 1 }, { -53, 6, 2 }, { -33, 6, 2 }, { -13, 9, 2 },
    };
    lv_point_precise_t (*pts)[2] = fc->pts;
    int np = 0;
    const float level = -63.0f, swr = 1.3f, pwr = 0.5f, mic_db = -14.0f;

    fc->scr = scr;
    fc->ring = mkarc(scr, 178, 0, 360, 4, C_TX_RED, true, C_TX_RED, 0, false);
    lv_obj_set_style_arc_width(fc->ring, 0, LV_PART_INDICATOR);
    fc->meter = mkarc(scr, ARC_R0, ARC_ROT, ARC_SPAN, 12, C_SUBTLE, true, C_SUBTLE, 0, false);
    lv_obj_set_style_arc_opa(fc->meter, LV_OPA_TRANSP, LV_PART_INDICATOR);
    for (int z = 0; z < 8; z++) {
        const int a0 = (int)(smeter_frac(RXZ[z].from) * ARC_SPAN), a1 = (int)(smeter_frac(RXZ[z].to) * ARC_SPAN);
        float fill = (level - RXZ[z].from) / (RXZ[z].to - RXZ[z].from);
        fill = fill < 0 ? 0 : fill > 1 ? 1 : fill;
        fc->rx_zone[z] = mkarc(scr, ARC_R0, ARC_ROT + a0, a1 - a0, 12, C_SUBTLE, false,
                               lv_color_hex(RXZ[z].rgb), fill, false);
    }
    fc->rx_ticks = mkgroup(scr);
    for (size_t i = 0; i < sizeof RXNOTCH / sizeof RXNOTCH[0]; i++)
        radial(fc->rx_ticks, pts[np++], ARC_ROT + smeter_frac(RXNOTCH[i]) * ARC_SPAN,
               ARC_R0 - 13, ARC_R0 + 1, 3, C_BG, false);
    for (size_t i = 0; i < sizeof TICKS / sizeof TICKS[0]; i++)
        radial(fc->rx_ticks, pts[np++], ARC_ROT + smeter_frac(TICKS[i].dbm) * ARC_SPAN,
               ARC_R0 - 15 - TICKS[i].len, ARC_R0 - 15, TICKS[i].kind == 1 ? 3 : 2,
               TICKS[i].kind == 1 ? C_TEXT2 : TICKS[i].kind == 2 ? C_WARN : C_LABEL, true);

    /* Transmit: SWR left, forward power right, the microphone's ring inside. */
    fc->pwr_arc = mkarc(scr, ARC_R0, AUD_ROT, AUD_SPAN, 12, C_SUBTLE, true, C_BRAND, pwr, false);
    for (int z = 0; z < 3; z++) {
        const int a0 = (int)((1.0f - mic_frac(MICZ[z].to)) * AUD_SPAN + 0.5f);
        const int a1 = (int)((1.0f - mic_frac(MICZ[z].from)) * AUD_SPAN + 0.5f);
        float fill = (mic_db - MICZ[z].from) / (MICZ[z].to - MICZ[z].from);
        fill = fill < 0 ? 0 : fill > 1 ? 1 : fill;
        fc->mic_zone[z] = mkarc(scr, ARC_R0 - 22, AUD_ROT + a0, a1 - a0, 5, C_SUBTLE, true,
                                lv_color_hex(MICZ[z].rgb), fill, true);
    }
    for (int z = 0; z < 3; z++) {
        const int a0 = (int)(swr_frac(SWRZ[z].from) * SWR_SPAN), a1 = (int)(swr_frac(SWRZ[z].to) * SWR_SPAN);
        float fill = (swr - SWRZ[z].from) / (SWRZ[z].to - SWRZ[z].from);
        fill = fill < 0 ? 0 : fill > 1 ? 1 : fill;
        fc->swr_zone[z] = mkarc(scr, ARC_R0, SWR_ROT + a0, a1 - a0, 12, C_SUBTLE, true,
                                lv_color_hex(SWRZ[z].rgb), fill, false);
    }
    fc->tx_ticks = mkgroup(scr);
    {
        static const struct { float swr; const char *t; int kind; } T[] = {
            { 1.0f, "1", 0 }, { 1.5f, NULL, 0 }, { 2.0f, "2", 1 }, { 2.5f, NULL, 2 }, { 3.0f, "3", 2 },
        };
        for (size_t i = 0; i < sizeof T / sizeof T[0]; i++) {
            const float deg = SWR_ROT + swr_frac(T[i].swr) * SWR_SPAN;
            const lv_color_t c = T[i].kind == 2 ? C_DANGER : T[i].kind == 1 ? C_WARN : C_LABEL;
            radial(fc->tx_ticks, pts[np++], deg, ARC_R0 - 15 - (T[i].t ? 10 : 6), ARC_R0 - 15,
                   T[i].t ? 3 : 2, c, true);
            if (T[i].t) radial_label(fc->tx_ticks, deg, T[i].t, c);
        }
        static const struct { float w; const char *t; } P[] = { { 10, "10" }, { 50, "50" }, { 100, "100" } };
        for (size_t i = 0; i < 3; i++) {
            const float deg = AUD_ROT + P[i].w / 100.0f * AUD_SPAN;
            radial(fc->tx_ticks, pts[np++], deg, ARC_R0 - 24, ARC_R0 - 15, 2, C_LABEL, true);
            if (P[i].w < 100) radial(fc->tx_ticks, pts[np++], deg, ARC_R0 - 13, ARC_R0 + 1, 3, C_BG, false);
            radial_label(fc->tx_ticks, deg, P[i].t, C_LABEL);
        }
        static const float SWRNOTCH[] = { 1.5f, 2.0f, 2.5f };
        for (size_t i = 0; i < 3; i++)
            radial(fc->tx_ticks, pts[np++], SWR_ROT + swr_frac(SWRNOTCH[i]) * SWR_SPAN,
                   ARC_R0 - 13, ARC_R0 + 1, 3, C_BG, false);
    }
    lv_obj_move_foreground(fc->rx_ticks);
    lv_obj_move_foreground(fc->tx_ticks);

    fc->srd = mklabel(scr, &lv_font_montserrat_20, C_TEXT, CX, 76, "S9+10");
    fc->dbm = mklabel(scr, &lv_font_montserrat_14, C_LABEL, CX, 98, "-63 dBm");
    fc->aux[0] = mklabel(scr, &lv_font_montserrat_14, C_LABEL, CX - AUX_DX, 78, "AGC");
    fc->aux[1] = mklabel(scr, &lv_font_montserrat_14, C_TEXT2, CX - AUX_DX, 97, "MID");
    fc->aux[2] = mklabel(scr, &lv_font_montserrat_14, C_LABEL, CX + AUX_DX, 78, "P.AMP");
    fc->aux[3] = mklabel(scr, &lv_font_montserrat_14, C_TEXT2, CX + AUX_DX, 97, "OFF");
    fc->band = mklabel(scr, &lv_font_montserrat_20, C_ACCENT, CX - 76, 122, "23cm");
    fc->mode = mklabel(scr, &lv_font_montserrat_20, C_TEXT, CX, 122, "DIGU");
    fc->filt = mklabel(scr, &lv_font_montserrat_20, C_TEXT2, CX + 76, 122, "FIL2");
    fc->step = mklabel(scr, &lv_font_montserrat_20, C_ACCENT, CX - 98, 220, "100 kHz");
    fc->rit = mklabel(scr, &lv_font_montserrat_14, C_WARN, CX - 24, 222, "RIT +120");
    fc->vol = mklabel(scr, &lv_font_montserrat_14, C_TEXT2, CX + 42, 222, LV_SYMBOL_VOLUME_MID " 40");
    fc->mic = mklabel(scr, &font_mic_14, C_TEXT2, CX + 104, 222, SYM_MIC " 100");

    fc->ptt = lv_obj_create(scr);
    lv_obj_set_size(fc->ptt, 360, 360 - PTT_TOP);
    lv_obj_set_pos(fc->ptt, 0, PTT_TOP);
    lv_obj_set_style_radius(fc->ptt, 0, 0);
    lv_obj_set_style_bg_color(fc->ptt, C_BG1, 0);
    lv_obj_set_style_border_width(fc->ptt, 2, 0);
    lv_obj_set_style_border_side(fc->ptt, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_color(fc->ptt, C_ACCENT, 0);
    lv_obj_set_style_pad_all(fc->ptt, 0, 0);
    lv_obj_remove_flag(fc->ptt, LV_OBJ_FLAG_SCROLLABLE);
    fc->ptt_lbl = lv_label_create(scr);
    lv_obj_set_style_text_font(fc->ptt_lbl, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(fc->ptt_lbl, C_TEXT2, 0);
    lv_label_set_text(fc->ptt_lbl, "PTT");
    lv_obj_set_width(fc->ptt_lbl, 360);
    lv_obj_set_style_text_align(fc->ptt_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_all(fc->ptt_lbl, 0, 0);
    lv_obj_set_pos(fc->ptt_lbl, 0, PTT_TOP + 14);
}

static void vis(lv_obj_t *o, bool on)
{
    if (on) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    else    lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

/* ui.c ui_update() on the way into transmit and back. */
static void face_tx(face_t *fc, bool tx)
{
    lv_obj_set_style_bg_color(fc->scr, tx ? C_BG_TX : C_BG, 0);
    for (int i = 0; i < 4; i++) vis(fc->aux[i], !tx);
    vis(fc->meter, !tx);
    for (int z = 0; z < 8; z++) vis(fc->rx_zone[z], !tx);
    vis(fc->rx_ticks, !tx);
    for (int z = 0; z < 3; z++) { vis(fc->mic_zone[z], tx); vis(fc->swr_zone[z], tx); }
    vis(fc->pwr_arc, tx);
    vis(fc->tx_ticks, tx);
    vis(fc->ring, tx);
    lv_label_set_text(fc->srd, tx ? "SWR 1.3" : "S9+10");
    lv_label_set_text(fc->dbm, tx ? "PWR 50W / 100W" : "-63 dBm");
    lv_obj_set_style_text_color(fc->dbm, tx ? C_TX_TEXT : C_LABEL, 0);
    lv_obj_set_style_bg_color(fc->ptt, tx ? C_TX_RED : C_BG1, 0);
    lv_label_set_text(fc->ptt_lbl, tx ? "TX" : "PTT");
    lv_obj_set_style_text_color(fc->ptt_lbl, tx ? lv_color_white() : C_TEXT2, 0);
}

/* ---- two displays: the readout as it was, and Hack ---- */
typedef struct {
    lv_display_t *d;
    uint16_t fb[W * H];
    uint8_t buf[W * 12 * 2] __attribute__((aligned(4)));
    face_t face;
    readout_t ro;
} host_t;
static host_t h_old, h_new;

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
static void host_init(host_t *h, bool hack)
{
    h->d = lv_display_create(W, H);
    lv_display_set_user_data(h->d, h);
    lv_display_set_color_format(h->d, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(h->d, h->buf, NULL, sizeof h->buf, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(h->d, flush_cb);
    lv_obj_t *scr = lv_display_get_screen_active(h->d);
    lv_obj_set_style_bg_color(scr, C_BG, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    face_build(&h->face, scr);
    readout_build(&h->ro, scr, hack);
    face_tx(&h->face, false);
}
static void full(host_t *h)
{
    lv_obj_invalidate(lv_display_get_screen_active(h->d));
    lv_refr_now(h->d);
}

/* ---- measuring ---- */
typedef struct { int x1, y1, x2, y2, n; } box_t;

/* The pixels where two pictures differ: what was drawn in between. */
static box_t diff_box(const uint16_t *a, const uint16_t *b)
{
    box_t r = { W, H, -1, -1, 0 };
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            if (a[y * W + x] != b[y * W + x]) {
                if (x < r.x1) r.x1 = x;
                if (x > r.x2) r.x2 = x;
                if (y < r.y1) r.y1 = y;
                if (y > r.y2) r.y2 = y;
                r.n++;
            }
    return r;
}
static int differ_in(const uint16_t *a, const uint16_t *b, box_t bx, int grow)
{
    int n = 0;
    for (int y = bx.y1 - grow; y <= bx.y2 + grow; y++)
        for (int x = bx.x1 - grow; x <= bx.x2 + grow; x++)
            if (x >= 0 && x < W && y >= 0 && y < H) n += a[y * W + x] != b[y * W + x];
    return n;
}

/* How near the readout's ink comes to the rest of the face: the shortest
 * distance between pixel centres, one the readout drew (with != without) and
 * one of the face without it that is not the background -- 0 where they are
 * the same pixel, 1 where they touch. */
static double clearance(const uint16_t *with, const uint16_t *without, lv_color_t bg, int *wx, int *wy)
{
    const uint16_t b = lv_color_to_u16(bg);
    double best = 1e9;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            if (with[y * W + x] == without[y * W + x]) continue;
            for (int v = y - 24; v <= y + 24; v++)
                for (int u = x - 24; u <= x + 24; u++) {
                    if (u < 0 || u >= W || v < 0 || v >= H || without[v * W + u] == b) continue;
                    const double d = hypot(u - x, v - y);
                    if (d < best) { best = d; *wx = u; *wy = v; }
                }
        }
    return best;
}
/* The readout's ink nearest the glass's edge: how far inside it. */
static double glass_margin(const uint16_t *with, const uint16_t *without)
{
    double worst = 0;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            if (with[y * W + x] != without[y * W + x]) {
                const double dx = fabs(x + 0.5 - W / 2.0) + 0.5, dy = fabs(y + 0.5 - H / 2.0) + 0.5;
                const double d = hypot(dx, dy);
                if (d > worst) worst = d;
            }
    return W / 2.0 - worst;
}

/* The picture: outside the glass dimmed and tinted, its edge in magenta. */
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
            }
            fwrite(rgb, 1, 3, f);
        }
    fclose(f);
}

/* The places' ink, one label at a time: everything of the readout hidden but
 * the one label, against the face without the readout. */
static box_t alone(host_t *h, lv_obj_t *o, const uint16_t *bare, uint16_t *img)
{
    readout_hide(&h->ro, true);
    vis(o, true);
    full(h);
    if (img) memcpy(img, h->fb, sizeof h->fb);
    const box_t b = diff_box(h->fb, bare);
    vis(o, false);
    return b;
}

static const struct { const char *name; int64_t hz; int step; bool tx; } CASE[] = {
    { "7.074.000",      7074000LL,      1000,    false },
    { "14.205.000",     14205000LL,     100,     false },
    { "1.296.200.000",  1296200000LL,   1000,    false },
    { "0.475.000",      475000LL,       10,      false },
    { "11.111.111",     11111111LL,     10,      false },
    /* every place a 4, Montserrat's widest digit -- and on the air */
    { "444.444.440",    444444440LL,    10,      true  },
    /* the IC-905's 3 cm: "10" in the first place */
    { "10.368.200.000", 10368200000LL,  1000000, false },
    { "10.368.200.000", 10368200000LL,  100,     true  },
};
#define N_CASE ((int)(sizeof CASE / sizeof CASE[0]))

static void slug(char *out, size_t cap, const char *s)
{
    size_t n = 0;
    for (; *s && n + 1 < cap; s++) out[n++] = (*s >= '0' && *s <= '9') || *s == '.' ? *s : '_';
    out[n] = 0;
}

typedef struct {
    box_t ink;                            /* the digits' and separators' */
    double clear, line_clear, glass;      /* ...to the face; the underline's */
    int cx, cy, lx, ly, gap_dd, gap_ds, stale;
} meas_t;

/* One frequency on one display: the readout's ink, its gaps, its
 * clearances, partial against full. The band and the step rows say what
 * the knob would. */
static meas_t measure(host_t *h, int64_t f, int step, bool tx, uint16_t *pic)
{
    static uint16_t bare[W * H], partial[W * H], one[W * H];
    meas_t m;
    const lv_color_t bg = tx ? C_BG_TX : C_BG;
    face_tx(&h->face, tx);
    readout_hide(&h->ro, false);
    full(h);
    lv_label_set_text(h->face.step, step_name(show(&h->ro, f, step, tx)));
    lv_label_set_text(h->face.band, band_of(f));
    lv_refr_now(h->d);                        /* only what changed, as on the knob */
    memcpy(partial, h->fb, sizeof partial);
    full(h);
    m.stale = diff_box(partial, h->fb).n;
    memcpy(pic, h->fb, sizeof h->fb);
    readout_hide(&h->ro, true);
    full(h);
    memcpy(bare, h->fb, sizeof bare);
    /* The digits and separators, then the underline on its own. */
    readout_hide(&h->ro, false);
    vis(h->ro.underline, false);
    full(h);
    m.ink = diff_box(h->fb, bare);
    m.clear = clearance(h->fb, bare, bg, &m.cx, &m.cy);
    m.glass = glass_margin(h->fb, bare);
    alone(h, h->ro.underline, bare, one);
    m.line_clear = clearance(one, bare, bg, &m.lx, &m.ly);
    /* The gaps: each label's ink alone, in the order they stand. */
    box_t b[N_DIG + 2];
    int kind[N_DIG + 2], n = 0;
    const int sep_a = h->ro.lay ? 3 : 2, sep_b = h->ro.lay ? 6 : 5;
    for (int i = 0; i < N_DIG; i++) {
        b[n] = alone(h, h->ro.dig[i], bare, one);
        kind[n++] = 0;
        if (i == sep_a || i == sep_b) {
            b[n] = alone(h, h->ro.sep[i == sep_a ? 0 : 1], bare, one);
            kind[n++] = 1;
        }
    }
    m.gap_dd = m.gap_ds = 99;
    int last = -1;
    for (int k = 0; k < n; k++) {
        if (b[k].x2 < 0) continue;            /* a blank leading digit */
        if (last >= 0) {
            const int g = b[k].x1 - b[last].x2 - 1;
            int *slot = kind[k] || kind[last] ? &m.gap_ds : &m.gap_dd;
            if (g < *slot) *slot = g;
        }
        last = k;
    }
    readout_hide(&h->ro, false);
    full(h);
    return m;
}

/* Every place rolled through 0-9 in one layout, alone and between
 * neighbours. Returns the problems found (the new readout's only count). */
typedef struct { double move; int wmin, wmax, gap_dd, gap_ds, intrude; } roll_t;
static void roll(host_t *h, uint8_t lay, roll_t out[N_DIG])
{
    static uint16_t bare[W * H], pic[W * H];
    static uint16_t solo[N_DIG][10][W * H];
    box_t b[N_DIG][10], sb[2];
    place(&h->ro, lay);
    face_tx(&h->face, false);
    readout_hide(&h->ro, true);
    full(h);
    memcpy(bare, h->fb, sizeof bare);
    const int sep_a = lay ? 3 : 2, sep_b = lay ? 6 : 5;
    for (int i = 0; i < N_DIG; i++)
        for (int v = 0; v < 10; v++) {
            char t[3] = { (char)('0' + v), 0, 0 };
            if (lay == 2 && i == 0) { t[0] = '1'; t[1] = '0'; }   /* "10", always */
            lv_label_set_text(h->ro.dig[i], t);
            lv_obj_set_style_text_color(h->ro.dig[i], C_TEXT, 0);
            b[i][v] = alone(h, h->ro.dig[i], bare, solo[i][v]);
        }
    for (int s = 0; s < 2; s++) sb[s] = alone(h, h->ro.sep[s], bare, pic);
    for (int i = 0; i < N_DIG; i++) {
        roll_t *r = &out[i];
        double lo = 1e9, hi = -1e9;
        r->wmin = 99; r->wmax = 0; r->gap_dd = r->gap_ds = 99; r->intrude = 0;
        for (int v = 0; v < 10; v++) {
            const double c = (b[i][v].x1 + b[i][v].x2 + 1) / 2.0;
            const int w = b[i][v].x2 - b[i][v].x1 + 1;
            if (c < lo) lo = c;
            if (c > hi) hi = c;
            if (w < r->wmin) r->wmin = w;
            if (w > r->wmax) r->wmax = w;
            /* the next place to the right, or the separator between */
            if (i == sep_a || i == sep_b) {
                const box_t s = sb[i == sep_a ? 0 : 1];
                if (s.x1 - b[i][v].x2 - 1 < r->gap_ds) r->gap_ds = s.x1 - b[i][v].x2 - 1;
                for (int u = 0; u < 10; u++)
                    if (b[i + 1][u].x1 - s.x2 - 1 < r->gap_ds) r->gap_ds = b[i + 1][u].x1 - s.x2 - 1;
            } else if (i + 1 < N_DIG) {
                for (int u = 0; u < 10; u++)
                    if (b[i + 1][u].x1 - b[i][v].x2 - 1 < r->gap_dd) r->gap_dd = b[i + 1][u].x1 - b[i][v].x2 - 1;
            }
        }
        r->move = hi - lo;
    }
    /* Between neighbours: each place at each value, both neighbours rolled
     * through 0-9 and the separators shown -- the place's own pixels (its ink
     * alone, a pixel round it) must be what they were alone. */
    readout_hide(&h->ro, false);
    lv_obj_add_flag(h->ro.underline, LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < N_DIG; i++)
        for (int v = 0; v < 10; v++) {
            if (lay == 2 && i == 0 && v) break;
            for (int w = 0; w < 10; w++) {
                for (int k = 0; k < N_DIG; k++) {
                    char t[3] = { (char)('0' + (k == i ? v : w)), 0, 0 };
                    if (lay == 2 && k == 0) { t[0] = '1'; t[1] = '0'; }
                    lv_label_set_text(h->ro.dig[k], t);
                    lv_obj_set_style_text_color(h->ro.dig[k], C_TEXT, 0);
                }
                full(h);
                out[i].intrude += differ_in(h->fb, solo[i][v], b[i][v], 1);
            }
        }
    readout_hide(&h->ro, false);
}

/* How tall each digit stands: its ink alone, white on black, each row
 * counted by its brightest pixel -- a row the edge only half covers counts
 * half -- so heights compare to a fraction of a pixel. */
static void heights(host_t *h, double out[10])
{
    static uint16_t bare[W * H], img[W * H];
    place(&h->ro, 0);
    face_tx(&h->face, false);
    readout_hide(&h->ro, true);
    full(h);
    memcpy(bare, h->fb, sizeof bare);
    for (int v = 0; v < 10; v++) {
        const char t[2] = { (char)('0' + v), 0 };
        lv_label_set_text(h->ro.dig[3], t);
        lv_obj_set_style_text_color(h->ro.dig[3], C_TEXT, 0);
        const box_t b = alone(h, h->ro.dig[3], bare, img);
        out[v] = 0;
        for (int y = b.y1; y <= b.y2; y++) {
            int top = 0;
            for (int x = b.x1; x <= b.x2; x++) {
                const int g = (img[y * W + x] >> 5) & 63;     /* green: 6 bits */
                if (g > top) top = g;
            }
            out[v] += top / 63.0;
        }
    }
    readout_hide(&h->ro, false);
}

/* A layout's envelope on one display, in receive (out[0]) and in transmit
 * (out[1]): every place at every value drawn at once -- all the ink any
 * frequency in the layout can put down, the separators' with it -- and the
 * underline under each place in turn. The nearest any readout comes to the
 * face and to the glass, not just the eight frequencies'. `pic` gets the
 * transmit face with the envelope in white and the underlines in the accent. */
typedef struct {
    box_t ink;
    double clear, glass;                  /* the digits' and separators' */
    int cx, cy;
    double line[N_DIG];                   /* the underline under each place: */
    int over[N_DIG];                      /* ...to the face; its pixels on it */
} env_t;
static void envelope(host_t *h, uint8_t lay, env_t out[2], uint16_t *pic)
{
    static uint16_t bare[W * H], one[W * H], ink[W * H];
    static uint8_t under[W * H];
    int u, v;
    place(&h->ro, lay);
    for (int k = 0; k < 2; k++) {
        env_t *e = &out[k];
        const lv_color_t bg = k ? C_BG_TX : C_BG;
        face_tx(&h->face, k);
        readout_hide(&h->ro, true);
        full(h);
        memcpy(bare, h->fb, sizeof bare);
        memcpy(ink, bare, sizeof ink);
        memset(under, 0, sizeof under);
        for (int i = 0; i < N_DIG + 2; i++) {
            lv_obj_t *o = i < N_DIG ? h->ro.dig[i] : h->ro.sep[i - N_DIG];
            for (int d = 0; d < (i < N_DIG ? 10 : 1); d++) {
                if (i < N_DIG) {
                    char t[3] = { (char)('0' + d), 0, 0 };
                    if (lay == 2 && i == 0) {                     /* "10", always */
                        if (d) break;
                        t[0] = '1'; t[1] = '0';
                    }
                    lv_label_set_text(o, t);
                    lv_obj_set_style_text_color(o, k ? C_TX_TEXT : C_TEXT, 0);
                }
                alone(h, o, bare, one);
                for (int p = 0; p < W * H; p++)
                    if (one[p] != bare[p]) ink[p] = bare[p] == 0xFFFF ? 0xFFFE : 0xFFFF;
            }
        }
        for (int i = 0; i < N_DIG; i++) {
            lv_obj_align(h->ro.underline, LV_ALIGN_CENTER, h->ro.x[i] - CX, 204 - CY);
            alone(h, h->ro.underline, bare, one);
            e->line[i] = clearance(one, bare, bg, &u, &v);
            e->over[i] = 0;
            for (int p = 0; p < W * H; p++)
                if (one[p] != bare[p]) {
                    under[p] = 1;
                    e->over[i] += bare[p] != lv_color_to_u16(bg);
                }
        }
        e->ink = diff_box(ink, bare);
        e->clear = clearance(ink, bare, bg, &e->cx, &e->cy);
        e->glass = glass_margin(ink, bare);
        if (k)
            for (int p = 0; p < W * H; p++) pic[p] = under[p] ? lv_color_to_u16(C_ACCENT) : ink[p];
    }
    face_tx(&h->face, false);
    readout_hide(&h->ro, false);
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : ".";
    lv_init();
    lv_tick_set_cb(tick_cb);
    host_init(&h_old, false);
    host_init(&h_new, true);
    int bad = 0;
    char nm[256], s[64];
    static uint16_t pic_old[W * H], pic_new[W * H];

    printf("Hack 46: digits %d px apart, separators %d, the underline %d wide\n\n",
           DIG_PITCH, DIG_SEPW, DIG_LINE);
    printf("Clearances are between pixel centres: 1 is touching, 0 overlapping.\n\n");
    printf("frequency       step    tx      ink x    y        gap d-d d-.   digits to face   underline to face  glass stale\n");
    for (int i = 0; i < N_CASE; i++) {
        meas_t mo = measure(&h_old, CASE[i].hz, CASE[i].step, CASE[i].tx, pic_old);
        meas_t mn = measure(&h_new, CASE[i].hz, CASE[i].step, CASE[i].tx, pic_new);
        for (int k = 0; k < 2; k++) {
            const meas_t *m = k ? &mn : &mo;
            printf("%-15s %-7d %-2s %s %3d..%-3d %3d..%-3d %4d %3d  %5.1f at %3d,%-3d  %5.1f at %3d,%-3d %5.1f %4d\n",
                   k ? "" : CASE[i].name, CASE[i].step, CASE[i].tx ? "tx" : "", k ? "Hack " : "Mont.",
                   m->ink.x1, m->ink.x2, m->ink.y1, m->ink.y2, m->gap_dd, m->gap_ds,
                   m->clear, m->cx, m->cy, m->line_clear, m->lx, m->ly, m->glass, m->stale);
        }
        /* The new readout: never touching another digit, a separator, the
         * face or the glass, its underline clear of the face unless it met
         * it as nearly before, and nothing left behind. */
        if (mn.gap_dd < 2 || mn.gap_ds < 2 || mn.clear < 3.0 || mn.glass < 8.0 || mn.stale ||
            (mn.line_clear < 3.0 && mn.line_clear < mo.line_clear)) {
            printf("  PROBLEM with the new readout\n");
            bad++;
        } else if (mn.line_clear < 1.5) {
            /* In transmit the power scale's last number sits where the
             * underline's row meets the arc: under the last place, the two
             * meet -- as they did before. */
            printf("  the underline meets the face at %d,%d, as it did before\n", mn.lx, mn.ly);
        }
        slug(s, sizeof s, CASE[i].name);
        snprintf(nm, sizeof nm, "%s/readout-%02d-%s%s-old.ppm", dir, i + 1, s, CASE[i].tx ? "-tx" : "");
        ppm(nm, pic_old);
        snprintf(nm, sizeof nm, "%s/readout-%02d-%s%s-new.ppm", dir, i + 1, s, CASE[i].tx ? "-tx" : "");
        ppm(nm, pic_new);
    }

    {
        double ho[10], hn[10];
        heights(&h_old, ho);
        heights(&h_new, hn);
        printf("\nthe digits' height, px (rows weighted by their ink)\n       ");
        for (int v = 0; v < 10; v++) printf("   %d  ", v);
        printf("\nMont.  ");
        for (int v = 0; v < 10; v++) printf(" %5.2f", ho[v]);
        printf("\nHack   ");
        for (int v = 0; v < 10; v++) printf(" %5.2f", hn[v]);
        printf("\n");
    }
    static const char *LAY[3] = { "below 1 GHz: MMM.kkk.hh", "from 1 GHz: MMMM.kkk.h", "from 10 GHz: \"10\"MMM.kkk.h" };
    for (uint8_t lay = 0; lay < 3; lay++) {
        static roll_t ro[N_DIG], rn[N_DIG];
        roll(&h_old, lay, ro);
        roll(&h_new, lay, rn);
        printf("\nevery place rolled 0-9, %s (px; \".\" a gap to the separator)\n", LAY[lay]);
        printf("        ink moves     ink width     narrowest gap   neighbours' pixels\n");
        printf("place  Mont.  Hack   Mont.  Hack   to the next     in its place\n");
        printf("                                   Mont.  Hack     Mont.  Hack\n");
        for (int i = 0; i < N_DIG; i++) {
            char go[16], gn[16];
            const bool sep_next = i == (lay ? 3 : 2) || i == (lay ? 6 : 5);
            if (i == N_DIG - 1) { snprintf(go, sizeof go, "-"); snprintf(gn, sizeof gn, "-"); }
            else {
                snprintf(go, sizeof go, "%s%d", sep_next ? "." : "", sep_next ? ro[i].gap_ds : ro[i].gap_dd);
                snprintf(gn, sizeof gn, "%s%d", sep_next ? "." : "", sep_next ? rn[i].gap_ds : rn[i].gap_dd);
            }
            printf("  %d    %4.1f   %4.1f   %2d-%-2d  %2d-%-2d   %5s %5s    %5d %5d\n",
                   i, ro[i].move, rn[i].move, ro[i].wmin, ro[i].wmax, rn[i].wmin, rn[i].wmax, go, gn,
                   ro[i].intrude, rn[i].intrude);
            const int g = sep_next ? rn[i].gap_ds : rn[i].gap_dd;
            if (rn[i].intrude || (i < N_DIG - 1 && g < 2) || rn[i].move > 2.0) bad++;
        }
    }

    /* The envelopes: the worst any frequency can do, layout by layout. The
     * new readout must keep 3 px from the face and 8 from the glass, and its
     * underline, under each place, come no nearer the face nor cover more of
     * it than the old one did under the same place. */
    static const char *ENV[3] = { "mhz", "ghz", "10ghz" };
    static env_t eo[3][2], en[3][2];
    for (uint8_t lay = 0; lay < 3; lay++) {
        envelope(&h_old, lay, eo[lay], pic_old);
        envelope(&h_new, lay, en[lay], pic_new);
        snprintf(nm, sizeof nm, "%s/readout-env-%s-tx-old.ppm", dir, ENV[lay]);
        ppm(nm, pic_old);
        snprintf(nm, sizeof nm, "%s/readout-env-%s-tx-new.ppm", dir, ENV[lay]);
        ppm(nm, pic_new);
    }
    printf("\nthe envelope: every place at every value at once, the separators with them (px)\n");
    printf("layout                            ink x    y         digits to face   glass\n");
    for (int lay = 0; lay < 3; lay++)
        for (int k = 0; k < 2; k++) {
            for (int n = 0; n < 2; n++) {
                const env_t *e = n ? &en[lay][k] : &eo[lay][k];
                printf("%-27s %-2s %s %3d..%-3d %3d..%-3d  %5.1f at %3d,%-3d %5.1f\n",
                       k || n ? "" : LAY[lay], k ? "tx" : "rx", n ? "Hack " : "Mont.",
                       e->ink.x1, e->ink.x2, e->ink.y1, e->ink.y2, e->clear, e->cx, e->cy, e->glass);
            }
            if (en[lay][k].clear < 3.0 || en[lay][k].glass < 8.0) {
                printf("  PROBLEM with the new readout\n");
                bad++;
            }
        }
    printf("\nthe underline under each place: to the face, and the face's pixels it covers (px)\n");
    printf("layout                                  0        1        2        3        4        5        6        7\n");
    for (int lay = 0; lay < 3; lay++)
        for (int k = 0; k < 2; k++) {
            for (int n = 0; n < 2; n++) {
                const env_t *e = n ? &en[lay][k] : &eo[lay][k];
                printf("%-27s %-2s %s", k || n ? "" : LAY[lay], k ? "tx" : "rx", n ? "Hack " : "Mont.");
                for (int i = 0; i < N_DIG; i++) printf(" %4.1f/%-3d", e->line[i], e->over[i]);
                printf("\n");
            }
            for (int i = 0; i < N_DIG; i++) {
                const env_t *o = &eo[lay][k], *e = &en[lay][k];
                if ((e->line[i] < 3.0 && e->line[i] < o->line[i]) || e->over[i] > o->over[i]) {
                    printf("  PROBLEM with the new underline under place %d\n", i);
                    bad++;
                } else if (e->over[i]) {
                    /* In transmit the power scale's last number sits where
                     * the underline's row meets the arc. */
                    printf("  under place %d it covers %d px of the face, as it did before (%d)\n",
                           i, e->over[i], o->over[i]);
                }
            }
        }
    printf("\n%s\n", bad ? "PROBLEMS" : "all good");
    return bad ? 1 : 0;
}
