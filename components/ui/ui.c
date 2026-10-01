#include "ui.h"
#include "splash.h"
#include "board_pins.h"
#include "hal_touch.h"
#include "panel.h"
#include "net_prov.h"

#include "esp_attr.h"
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

/* LVGL's symbol set has no microphone, so the mic-gain readout used the music
 * note. font_mic_14 carries the one glyph and falls back to Montserrat 14 for
 * everything else. U+F130 in UTF-8. */
LV_FONT_DECLARE(font_mic_14);
/* The PTT slab's, while a Bluetooth headset is connected: its microphone,
 * live or muted (font_btmic_28.c). */
LV_FONT_DECLARE(font_btmic_28);
#define SYM_HS_MIC  "\xEF\x84\xB0"                  /* U+F130 */
#define SYM_HS_MUTE "\xEF\x84\xB1"                  /* U+F131 */
#define SYM_MIC "\xEF\x84\xB0"

#if VFO_RADIO_ICOM
/* --- Icom palette ----------------------------------------------------------
 * The IC-705's own screen: black, white digits, the mode in Icom blue, the
 * S-meter blue up to S9 and red above it, power in blue, TX in Icom red. The
 * layout is the AetherSDR face's, unchanged; the colours say at a glance which
 * radio this knob is for. */
#define C_BG        lv_color_hex(0x000000)
#define C_BG1       lv_color_hex(0x141A24)   /* panels, the PTT slab    */
#define C_BG_TX     lv_color_hex(0x2A0508)   /* a red tint on the air   */
#define C_ACCENT    lv_color_hex(0x2F7BFF)   /* Icom blue               */
#define C_ACCENT_HI lv_color_hex(0x5A9BFF)
#define C_TEXT      lv_color_hex(0xFFFFFF)   /* white digits            */
#define C_TEXT2     lv_color_hex(0xC0C8D4)
#define C_LABEL     lv_color_hex(0x707884)
#define C_DISABLED  lv_color_hex(0x3A4048)
#define C_SUBTLE    lv_color_hex(0x181C24)
#define C_WARN      lv_color_hex(0xFFB000)
#define C_DANGER    lv_color_hex(0xFF3030)
#define C_TX_BORDER lv_color_hex(0xE60012)   /* Icom red                */
#define C_TX_TEXT   lv_color_hex(0xFFFFFF)
#define C_PEAK      lv_color_hex(0xFFFFFF)
#define C_TX_RED    lv_color_hex(0xE60012)
#define C_GREEN     lv_color_hex(0x3FA9FF)   /* Icom's meters are blue  */
#define PWR_HEX     0x3FA9FF                 /* ...its Po meter too     */
#elif VFO_RADIO_MULTIFLEX
/* --- Maestro palette -------------------------------------------------------
 * FlexRadio's Maestro: black, white digits, the slice in Flex blue, meter
 * scales in blue that turn red over the top, power in green, and TX a red
 * badge. The layout is the other faces'; the colours say which radio this
 * knob is for. */
#define C_BG        lv_color_hex(0x000000)
#define C_BG1       lv_color_hex(0x0C1622)   /* panels, the PTT slab    */
#define C_BG_TX     lv_color_hex(0x2A0608)   /* a red tint on the air   */
#define C_ACCENT    lv_color_hex(0x2A9DF4)   /* the slice's blue        */
#define C_ACCENT_HI lv_color_hex(0x62BBFF)
#define C_TEXT      lv_color_hex(0xFFFFFF)   /* white digits            */
#define C_TEXT2     lv_color_hex(0xC9D2DC)
#define C_LABEL     lv_color_hex(0x7D8792)
#define C_DISABLED  lv_color_hex(0x3A424C)
#define C_SUBTLE    lv_color_hex(0x141C26)
#define C_WARN      lv_color_hex(0xFFB000)
#define C_DANGER    lv_color_hex(0xF0302C)
#define C_TX_BORDER lv_color_hex(0xE8262B)   /* the TX badge            */
#define C_TX_TEXT   lv_color_hex(0xFFFFFF)
#define C_PEAK      lv_color_hex(0xFFFFFF)
#define C_TX_RED    lv_color_hex(0xE8262B)
#define C_GREEN     lv_color_hex(0x43B649)   /* multiFLEX green         */
#define PWR_HEX     0x43B649                 /* Po in green, as on the Maestro */
#elif VFO_RADIO_XIEGU
/* --- Xiegu palette ---------------------------------------------------------
 * The X6100 and X6200 highlight in orange. Black, white digits, an orange
 * accent, and an S-meter in orange up to S9 and red above it. The layout is
 * the other faces', unchanged; the colours say which radio this knob is for. */
#define C_BG        lv_color_hex(0x000000)
#define C_BG1       lv_color_hex(0x1E1A16)   /* panels, the PTT slab    */
#define C_BG_TX     lv_color_hex(0x2A0A05)   /* a red tint on the air   */
#define C_ACCENT    lv_color_hex(0xFF8C1A)   /* Xiegu orange            */
#define C_ACCENT_HI lv_color_hex(0xFFA64D)
#define C_TEXT      lv_color_hex(0xFFFFFF)   /* white digits            */
#define C_TEXT2     lv_color_hex(0xD2CCC4)
#define C_LABEL     lv_color_hex(0x7E7770)
#define C_DISABLED  lv_color_hex(0x3E3A36)
#define C_SUBTLE    lv_color_hex(0x1E1C1A)
#define C_WARN      lv_color_hex(0xFFD000)
#define C_DANGER    lv_color_hex(0xFF3B30)
#define C_TX_BORDER lv_color_hex(0xFF3B30)
#define C_TX_TEXT   lv_color_hex(0xFFFFFF)
#define C_PEAK      lv_color_hex(0xFFFFFF)
#define C_TX_RED    lv_color_hex(0xE8200C)
#define C_GREEN     lv_color_hex(0xFF8C1A)   /* the meters are orange   */
#define PWR_HEX     0xFF8C1A                 /* ...Po as well           */
#define SDR_HEX     0x4DA6FF                 /* a web SDR is blue on every face */
#elif VFO_RADIO_UBERSDR
/* --- UberSDR palette -------------------------------------------------------
 * UberSDR's own dark theme, from its v2 page: ink, its accent blue, its dim
 * and faint text, and the S-meter from red to green as UberSDR colours it.
 * A KiwiSDR beside it is in the theme's violet. It only receives: nothing
 * here is ever drawn in transmit. */
#define C_BG        lv_color_hex(0x090C12)   /* --bg                    */
#define C_BG1       lv_color_hex(0x141A25)   /* --surface-2: the slab   */
#define C_BG_TX     lv_color_hex(0x2A0C10)
#define C_ACCENT    lv_color_hex(0x08A2FB)   /* --accent                */
#define C_ACCENT_HI lv_color_hex(0x4DB4FF)   /* --ch                    */
#define C_TEXT      lv_color_hex(0xDFE5EE)   /* --text                  */
#define C_TEXT2     lv_color_hex(0x8D99AD)   /* --text-dim              */
#define C_LABEL     lv_color_hex(0x5C6779)   /* --text-faint            */
#define C_DISABLED  lv_color_hex(0x2F3B4E)   /* --border-strong         */
#define C_SUBTLE    lv_color_hex(0x1A2130)   /* --surface-3: the tracks */
#define C_WARN      lv_color_hex(0xF2B544)   /* --warn                  */
#define C_DANGER    lv_color_hex(0xF2646A)   /* --bad                   */
#define C_TX_BORDER lv_color_hex(0xF2646A)
#define C_TX_TEXT   lv_color_hex(0xFFFFFF)
#define C_PEAK      lv_color_hex(0xDFE5EE)
#define C_TX_RED    lv_color_hex(0xF2646A)
#define C_GREEN     lv_color_hex(0x45D69A)   /* --good                  */
#define PWR_HEX     0x08A2FB
#define SDR_HEX     0x8B7CF8                 /* --violet: a KiwiSDR beside it */
#elif VFO_RADIO_SVXCONNECT
/* --- SvxConnect palette -----------------------------------------------------
 * svxconnect.app's ink and gold, with the status colours the SvxConnect
 * clients use: green connected, amber (re)connecting, red transmitting. */
#define C_BG        lv_color_hex(0x08090C)   /* ink-950                 */
#define C_BG1       lv_color_hex(0x13161D)   /* ink-800: the PTT slab   */
#define C_BG_TX     lv_color_hex(0x2A0C0C)   /* a red tint on the air   */
#define C_ACCENT    lv_color_hex(0xE5A823)   /* gold-400                */
#define C_ACCENT_HI lv_color_hex(0xECC34A)   /* gold-300                */
#define C_TEXT      lv_color_hex(0xFFFFFF)
#define C_TEXT2     lv_color_hex(0xE2E8F0)   /* slate-200               */
#define C_LABEL     lv_color_hex(0x94A3B8)   /* slate-400               */
#define C_DISABLED  lv_color_hex(0x475569)   /* slate-600               */
#define C_SUBTLE    lv_color_hex(0x1B1F29)   /* ink-700                 */
#define C_WARN      lv_color_hex(0xD29922)   /* busy, reconnecting      */
#define C_DANGER    lv_color_hex(0xD13B3B)   /* transmit                */
#define C_TX_BORDER lv_color_hex(0xD13B3B)
#define C_TX_TEXT   lv_color_hex(0xFFFFFF)
#define C_PEAK      lv_color_hex(0xFFFFFF)
#define C_TX_RED    lv_color_hex(0xD13B3B)
#define C_GREEN     lv_color_hex(0x2EA043)   /* connected               */
#define PWR_HEX     0xE5A823
#else
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
#define C_GREEN     lv_color_hex(0x4DD87A)   /* accent.success */
/* Not from the theme: the power bar wears the RF.Guru logo's gold. */
#define PWR_HEX     RFG_GOLD_HEX
#endif
#define C_BRAND     lv_color_hex(PWR_HEX)

/* The svxconnect firmware's face: a reflector's talkgroup where a radio's
 * frequency is, and the audio level on the arc. */
#if VFO_RADIO_SVXCONNECT
#define REFLECTOR_FACE 1
LV_FONT_DECLARE(font_svx_icons_24);
#define SYM_LOCK    "\xEF\x80\xA3"                  /* U+F023 */
#define SYM_UNLOCK  "\xEF\x8F\x81"                  /* U+F3C1 */
#define SYM_MUTED   "\xEF\x9A\xA9"                  /* U+F6A9 */
#define SYM_SOUND   "\xEF\x80\xA8"                  /* U+F028 */
#else
#define REFLECTOR_FACE 0
#endif
/* A radio's readouts the reflector face has no use for. */
#define RADIO_ONLY __attribute__((unused))
/* A receiver's face (the ubersdr firmware): no PTT, no microphone, no RIT.
 * The slab holds the spots and voices on the band, the AGC's place the SNR,
 * the gain's the noise filter; a swipe from the right, the SSTV pictures. */
#if VFO_RX_ONLY
#define RX_FACE 1
#else
#define RX_FACE 0
#endif
/* A web SDR beside the radio: blue -- the Icom's and the Maestro's own. */
#ifndef SDR_HEX
#if VFO_RADIO_ICOM
#define SDR_HEX     0x5A9BFF                 /* C_ACCENT_HI */
#elif VFO_RADIO_MULTIFLEX
#define SDR_HEX     0x62BBFF                 /* C_ACCENT_HI */
#else
#define SDR_HEX     0x4DA6FF
#endif
#endif
#define C_SDR       lv_color_hex(SDR_HEX)
#define SDR_R       176       /* its S-meter: a line just outside the radio's */

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

/* In transmit the arc splits: SWR on the LEFT half, forward power on the
 * right, with the mic level as a thin inner ring. */
#define SWR_ROT  ARC_ROT
#define SWR_SPAN (ARC_SPAN / 2 - 3)
#define AUD_ROT  (ARC_ROT + ARC_SPAN / 2 + 3)
#define AUD_SPAN (ARC_SPAN / 2 - 3)

/* The PTT slab runs full width and all the way to the bottom edge; the round
 * glass clips it into a chord, which is the intended shape. Making it the
 * largest target on the face is deliberate -- with toggle PTT, stopping a
 * transmission must never require aim. */
#define PTT_TOP   248
/* The whole bottom slab is PTT. There was briefly a TUNE button in the left
 * corner; it was removed because it keys the transmitter and had none of the
 * safeguards the PTT path has -- no timeout, no drop on link loss. If tune
 * returns it needs all of that first. */
#define PTT_LEFT  0
#define PTT_RIGHT 360

#define N_DIG 8
static const int DIG_STEP[N_DIG] = {
    1000000, 1000000, 1000000, 100000, 10000, 1000, 100, 10,
};

static lv_obj_t *s_scr, *s_dig[N_DIG], *s_sep[2], *s_underline;
static lv_obj_t *s_band, *s_mode, *s_filt, *s_step_lbl, *s_srd;
static lv_obj_t *s_meter, *s_ring, *s_ptt, *s_ptt_lbl;
static lv_obj_t *s_hs_name, *s_hs_mic, *s_hs_raise;  /* the slab while a headset is connected */
static lv_obj_t *s_hs_bt;                   /* a receiver's: the headset's logo beside the spots */
static lv_obj_t *s_warn_panel, *s_warn_net;

/* "Update?" -- see ui_ask_update(). */
#define ASK_MS     10000
#define ASK_ARM_MS 800     /* a tap sooner than this was aimed at what is under it */
static lv_obj_t     *s_ask_panel, *s_ask_title, *s_ask_hint;
static bool          s_asking;
static uint32_t      s_ask_since;
static volatile int  s_ask_answer;       /* 1 yes, -1 no, 0 none */
static volatile bool s_ask_knob;         /* the knob turned while asking */
static volatile bool s_ask_turn;         /* ui_ask_turn(): a turn is the yes */
static bool          s_ask_restarts;     /* a yes restarts first: see ui.h */
static volatile uint32_t s_turned_at;    /* when a turn said that yes */
#define TURN_SPENT_MS 2000   /* ...and the turns after it tune nothing either */
static lv_obj_t *s_dbm, *s_rit, *s_vol, *s_mic, *s_warn;
#if VFO_HAS_SDR
static lv_obj_t *s_sdr_arc;
#endif
static lv_obj_t *s_agc_cap, *s_agc_val, *s_gain_cap, *s_gain_val;
/* A receiver's slab: the nearest spot or voice, what and where it is, and
 * how many there are. */
static lv_obj_t *s_spot_sub, *s_spot_n;
#if REFLECTOR_FACE
/* The reflector face's lock and mute, in the talkgroup's row. */
static lv_obj_t *s_lock_icon, *s_mute_icon;
#endif
static volatile bool s_lock_tap, s_mute_tap;

/* Memory mode: the channel in the frequency readout's place -- its name large,
 * and under it the channel number, frequency, shift and tone. */
static lv_obj_t *s_mem_big, *s_mem_small;
static bool      s_mem_face;

/* AGC and the front end's gain, either side of the S-unit readout: centred
 * AUX_DX from the middle, and tapped anywhere from AUX_IN to AUX_OUT out and
 * from AUX_TOP down to the band row. */
#define AUX_DX   72
#define AUX_IN   40
#define AUX_OUT  118
#define AUX_TOP  60
static lv_obj_t *s_pwr_arc, *s_rx_ticks, *s_tx_ticks;

/* Mic level on AetherSDR's own scale -- the P/CW applet's Level gauge: -40 to
 * +10 dB, amber from -10 and red from 0 -- so the knob and the desktop read
 * alike. It was auto-ranging, which always looked nearly full and so could
 * not say whether the drive was right. */
#define MIC_DB_MIN (-40.0f)
#define MIC_DB_MAX  (10.0f)
#define MIC_ZONES 3
static const struct { float from, to; uint32_t rgb; } MIC_ZONE[MIC_ZONES] = {
#if VFO_RADIO_ICOM
    { -40.0f, -10.0f, 0x3FA9FF },   /* blue  */
    { -10.0f,   0.0f, 0xFFB000 },   /* amber */
    {   0.0f,  10.0f, 0xFF3030 },   /* red   */
#elif VFO_RADIO_MULTIFLEX
    { -40.0f, -10.0f, 0x2A9DF4 },   /* blue  */
    { -10.0f,   0.0f, 0xFFB000 },   /* amber */
    {   0.0f,  10.0f, 0xF0302C },   /* red   */
#elif VFO_RADIO_XIEGU
    { -40.0f, -10.0f, 0xFF8C1A },   /* orange */
    { -10.0f,   0.0f, 0xFFD000 },   /* yellow */
    {   0.0f,  10.0f, 0xFF3B30 },   /* red    */
#elif VFO_RADIO_SVXCONNECT
    { -40.0f, -10.0f, 0x35B35A },   /* SvxConnect's meter: green  */
    { -10.0f,   0.0f, 0xD8C43A },   /* yellow */
    {   0.0f,  10.0f, 0xD13B3B },   /* red    */
#else
    { -40.0f, -10.0f, 0x4DD87A },   /* green */
    { -10.0f,   0.0f, 0xFFB84D },   /* amber */
    {   0.0f,  10.0f, 0xFF4D4D },   /* red   */
#endif
};
static lv_obj_t *s_mic_zone[MIC_ZONES];
static float mic_frac(float db) { return (db - MIC_DB_MIN) / (MIC_DB_MAX - MIC_DB_MIN); }

/* Peak hold with decay for the transmit meters. In SSB the level is speech:
 * it jumps between syllables and pauses, and a meter that follows it sample by
 * sample is a flicker nobody can read. Hold each peak for a second, then let
 * it fall at a steady rate -- as the S-meter already does. */
#define PEAK_HOLD_MS 1000
typedef struct { float v; uint32_t t_peak, t_last; } peak_t;
static peak_t s_mic_pk, s_pwr_pk, s_swr_pk;
static peak_t s_sig_pk = { .v = -127.0f };       /* the S-meter, from the floor */

static float peak_hold(peak_t *p, float x, float fall_per_s)
{
    const uint32_t now = lv_tick_get();
    const float    dt  = (float)(now - p->t_last) / 1000.0f;
    p->t_last = now;
    if (x >= p->v) {
        p->v      = x;
        p->t_peak = now;
    } else if (now - p->t_peak > PEAK_HOLD_MS) {
        p->v -= fall_per_s * dt;
        if (p->v < x) p->v = x;
    }
    return p->v;
}

static void peak_reset(peak_t *p, float v)
{
    p->v = v;
    p->t_peak = p->t_last = lv_tick_get();
}

/* The bars themselves follow the signal: up at once, most of the way back
 * down within a third of a second. The peak is shown separately, below. */
static float release(float *d, float x)
{
    if (x > *d) *d = x;
    else        *d += (x - *d) * 0.35f;
    return *d;
}

/* Peak LEDs: one block per meter that hangs at the recent peak while the bar
 * underneath goes down, then falls away to meet it, as on a VU meter.
 *
 * It moves only by changing its span. An arc invalidates just the span it
 * leaves and the one it enters, whereas a rotation or a style write redraws
 * the whole screen-sized object -- the mistake that once starved a core. So
 * its colour comes from one arc per zone, with all but the lit one collapsed
 * to nothing: an arc with no span draws nothing at all. */
#define LED_DEG 3
#define LED_MAX 8
typedef struct {
    lv_obj_t *arc[LED_MAX];
    int       n, lit, at;     /* zones; the lit one (-1: none); its start angle */
    int       span;           /* the meter's span, degrees */
    bool      reverse;        /* fills from the far end (the mic ring) */
} peak_led_t;
static peak_led_t s_sig_led, s_swr_led, s_pwr_led, s_mic_led;
#if VFO_HAS_SDR
static peak_led_t s_sdr_led;         /* the web SDR's, on its thin line */
#endif

static void led_build(peak_led_t *l, int rot, int span, int r, int width,
                      bool reverse, const uint32_t *rgb, int n)
{
    l->n = n; l->lit = -1; l->at = 0; l->span = span; l->reverse = reverse;
    for (int i = 0; i < n; i++) {
        lv_obj_t *a = lv_arc_create(s_scr);
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

/* Light the block at fraction f of the scale, in zone z; f <= 0 puts it out. */
static void led_set(peak_led_t *l, float f, int z)
{
    if (!l->n) return;                      /* never built */
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
    l->at  = at;
}

static void led_show(peak_led_t *l, bool on)
{
    for (int i = 0; i < l->n; i++) {
        if (on) lv_obj_remove_flag(l->arc[i], LV_OBJ_FLAG_HIDDEN);
        else    lv_obj_add_flag(l->arc[i], LV_OBJ_FLAG_HIDDEN);
    }
}
static float s_pwr_peak;
static int   s_pwr_range = -1;

/* Four ranges, each with the pegs an operator actually reads. The bar switches
 * between them and the printed pegs switch with it, so the scale is never
 * ambiguous. */
#define PWR_RANGES 4
#define PWR_PEGS   3
static const struct {
    float fs;
    float peg[PWR_PEGS];
    const char *lbl[PWR_PEGS];
} PWR[PWR_RANGES] = {
    {   10.0f, {    1,    5,   10 }, { "1",   "5",   "10"  } },
    {  100.0f, {   10,   50,  100 }, { "10",  "50",  "100" } },
    { 1000.0f, {  100,  500, 1000 }, { ".1k", ".5k", "1k"  } },
    { 2000.0f, { 1000, 1500, 2000 }, { "1k",  "1.5k","2k"  } },
};
static lv_obj_t *s_pwr_tick[PWR_PEGS];
static lv_obj_t *s_pwr_notch[PWR_PEGS];
static lv_point_precise_t s_pwr_npts[PWR_PEGS][2];

/* Defined with the other meter drawing further down; needed here because the
 * power pegs move with the auto-range and take their notches with them. */
static void      notch_points(float deg, lv_point_precise_t out[2]);
static lv_obj_t *mknotch(lv_obj_t *parent);
static lv_obj_t *s_pwr_lbl[PWR_PEGS];
static lv_point_precise_t s_pwr_pts[PWR_PEGS][2];

static void pwr_set_range(int r)
{
    if (r == s_pwr_range) return;
    s_pwr_range = r;
    for (int i = 0; i < PWR_PEGS; i++) {
        float frac = PWR[r].peg[i] / PWR[r].fs;
        float a = (AUD_ROT + frac * AUD_SPAN) * 3.14159265f / 180.0f;
        float c = cosf(a), sn = sinf(a);
        int r1 = ARC_R0 - 15, r0 = r1 - 9;
        s_pwr_pts[i][0].x = (lv_value_precise_t)(CX + r0 * c);
        s_pwr_pts[i][0].y = (lv_value_precise_t)(CY + r0 * sn);
        s_pwr_pts[i][1].x = (lv_value_precise_t)(CX + r1 * c);
        s_pwr_pts[i][1].y = (lv_value_precise_t)(CY + r1 * sn);
        lv_line_set_points(s_pwr_tick[i], s_pwr_pts[i], 2);
        /* The top peg sits at full scale, which is the end of the arc: a notch
         * there would cut nothing and only nibble the end cap. */
        if (s_pwr_notch[i]) {
            if (frac < 0.995f) {
                notch_points(AUD_ROT + frac * AUD_SPAN, s_pwr_npts[i]);
                lv_line_set_points(s_pwr_notch[i], s_pwr_npts[i], 2);
                lv_obj_remove_flag(s_pwr_notch[i], LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(s_pwr_notch[i], LV_OBJ_FLAG_HIDDEN);
            }
        }
        lv_label_set_text(s_pwr_lbl[i], PWR[r].lbl[i]);
        /* 128, not 139: the last peg sits at the very end of the arc and its
         * label ran off the edge of the round glass -- "10" lost its zero. */
        lv_obj_align(s_pwr_lbl[i], LV_ALIGN_CENTER,
                     (int)(128 * c), (int)(128 * sn));
    }
}
#define SWR_ZONES 3
static const struct { float from, to; uint32_t rgb; } ZONES[SWR_ZONES] = {
#if VFO_RADIO_ICOM
    { 1.0f, 2.0f, 0x3FA9FF },   /* blue  */
    { 2.0f, 2.5f, 0xFFB000 },   /* amber */
    { 2.5f, 3.0f, 0xFF3030 },   /* red   */
#elif VFO_RADIO_MULTIFLEX
    { 1.0f, 2.0f, 0x2A9DF4 },   /* blue  */
    { 2.0f, 2.5f, 0xFFB000 },   /* amber */
    { 2.5f, 3.0f, 0xF0302C },   /* red   */
#elif VFO_RADIO_XIEGU
    { 1.0f, 2.0f, 0xFF8C1A },   /* orange */
    { 2.0f, 2.5f, 0xFFD000 },   /* yellow */
    { 2.5f, 3.0f, 0xFF3B30 },   /* red    */
#elif VFO_RADIO_SVXCONNECT
    { 1.0f, 2.0f, 0x35B35A },
    { 2.0f, 2.5f, 0xD8C43A },
    { 2.5f, 3.0f, 0xD13B3B },
#else
    { 1.0f, 2.0f, 0x4DD87A },   /* green */
    { 2.0f, 2.5f, 0xFFB84D },   /* amber */
    { 2.5f, 3.0f, 0xFF4D4D },   /* red   */
#endif
};
static lv_obj_t *s_swr_zone[SWR_ZONES];

/* The receive meter gets the same treatment as SWR: bands laid end to end that
 * fill independently, so a strong signal is read as "how far into the red"
 * rather than by recognising a colour. S9 is -73 dBm and sits at 0.6 of the
 * arc, matching AetherSDR's own scale.
 *
 * The bar still sweeps the whole arc -- the bands only decide what colour each
 * part of that sweep is, so the fill is continuous and steps through the
 * palette as the signal climbs. */
/* Blocks, not a wash. The boundaries are the S-unit marks, and a notch in the
 * background colour is drawn across the band at each one -- so where the bar
 * has reached, the notches read as gaps cut into it, and where it has not they
 * vanish into the unfilled track. That is what separates the blocks; no
 * gradient is involved and the arcs are square-ended so the segments butt up
 * against each other cleanly. */
#define RX_ZONES 8
static const struct { float from, to; uint32_t rgb; } RXZONES[RX_ZONES] = {
#if VFO_RADIO_ICOM
    { -127.0f, -121.0f, 0x0D3B8C },   /* S0 to S1: blue, deepening  */
    { -121.0f, -109.0f, 0x1350B0 },   /* S1 to S3   */
    { -109.0f,  -97.0f, 0x1A68D4 },   /* S3 to S5   */
    {  -97.0f,  -85.0f, 0x2A86F2 },   /* S5 to S7   */
    {  -85.0f,  -73.0f, 0x46A8FF },   /* S7 to S9   */
    {  -73.0f,  -53.0f, 0xFF6A5A },   /* S9 to +20: red over S9, as on the radio */
    {  -53.0f,  -33.0f, 0xFF4040 },   /* +20 to +40 */
    {  -33.0f,  -13.0f, 0xE60012 },   /* +40 to +60 */
#elif VFO_RADIO_MULTIFLEX
    { -127.0f, -121.0f, 0x0A3563 },   /* S0 to S1: Flex blue, brightening */
    { -121.0f, -109.0f, 0x0F4C8A },   /* S1 to S3   */
    { -109.0f,  -97.0f, 0x1666B3 },   /* S3 to S5   */
    {  -97.0f,  -85.0f, 0x1F82D9 },   /* S5 to S7   */
    {  -85.0f,  -73.0f, 0x2A9DF4 },   /* S7 to S9   */
    {  -73.0f,  -53.0f, 0xF26A6A },   /* S9 to +20: red over S9, as on the Maestro */
    {  -53.0f,  -33.0f, 0xEE4444 },   /* +20 to +40 */
    {  -33.0f,  -13.0f, 0xE8262B },   /* +40 to +60 */
#elif VFO_RADIO_XIEGU
    { -127.0f, -121.0f, 0x5C2A00 },   /* S0 to S1: orange, brightening */
    { -121.0f, -109.0f, 0x7F3A00 },   /* S1 to S3   */
    { -109.0f,  -97.0f, 0xA64E00 },   /* S3 to S5   */
    {  -97.0f,  -85.0f, 0xD46A0A },   /* S5 to S7   */
    {  -85.0f,  -73.0f, 0xFF8C1A },   /* S7 to S9   */
    {  -73.0f,  -53.0f, 0xFF6A3D },   /* S9 to +20: red over S9 */
    {  -53.0f,  -33.0f, 0xFF4A2E },   /* +20 to +40 */
    {  -33.0f,  -13.0f, 0xE8200C },   /* +40 to +60 */
#elif VFO_RADIO_UBERSDR
    /* UberSDR's own S-meter colours: hsl(0..120, 90%, 55%) from S1 to S9,
     * red through yellow to green, and green all the way up from there. */
    { -127.0f, -121.0f, 0xF42525 },   /* S0 to S1   */
    { -121.0f, -109.0f, 0xF48C25 },   /* S1 to S3   */
    { -109.0f,  -97.0f, 0xF4F425 },   /* S3 to S5   */
    {  -97.0f,  -85.0f, 0x8CF425 },   /* S5 to S7   */
    {  -85.0f,  -73.0f, 0x25F425 },   /* S7 to S9   */
    {  -73.0f,  -53.0f, 0x25F425 },   /* S9 to +20  */
    {  -53.0f,  -33.0f, 0x25F425 },   /* +20 to +40 */
    {  -33.0f,  -13.0f, 0x25F425 },   /* +40 to +60 */
#elif VFO_RADIO_SVXCONNECT
    /* Not an S-meter: the audio level, -60 to 0 dBFS, in SvxConnect's meter
     * colours -- green, then yellow from -12 dB, red in the last 3. */
    {  -60.0f,  -48.0f, 0x1B5E2E },
    {  -48.0f,  -36.0f, 0x237A3B },
    {  -36.0f,  -24.0f, 0x2EA043 },
    {  -24.0f,  -18.0f, 0x35B35A },
    {  -18.0f,  -12.0f, 0x9DBD3B },
    {  -12.0f,   -6.0f, 0xD8C43A },
    {   -6.0f,   -3.0f, 0xE08C33 },
    {   -3.0f,    0.0f, 0xD13B3B },
#else
    { -127.0f, -121.0f, 0x1A6B47 },   /* S0 to S1   */
    { -121.0f, -109.0f, 0x1F7A52 },   /* S1 to S3   */
    { -109.0f,  -97.0f, 0x2F9E6A },   /* S3 to S5   */
    {  -97.0f,  -85.0f, 0x4DD87A },   /* S5 to S7   */
    {  -85.0f,  -73.0f, 0x9BD94A },   /* S7 to S9   */
    {  -73.0f,  -53.0f, 0xFFD24D },   /* S9 to +20  */
    {  -53.0f,  -33.0f, 0xFF9A3C },   /* +20 to +40 */
    {  -33.0f,  -13.0f, 0xFF4D4D },   /* +40 to +60 */
#endif
};

/* Every boundary gets a notch, which means every printed tick gets one -- the
 * ends excepted, since they are the ends. */
#if VFO_RADIO_SVXCONNECT
static const float RXNOTCH[] = { -48.0f, -36.0f, -24.0f, -18.0f, -12.0f,
                                  -6.0f, -3.0f };
#else
static const float RXNOTCH[] = { -121.0f, -109.0f, -97.0f, -85.0f, -73.0f,
                                  -53.0f, -33.0f };
#endif


static lv_obj_t *s_rx_zone[RX_ZONES];
static int16_t   s_rx_val[RX_ZONES];   /* last value written, to skip redraws */
static lv_obj_t *s_edit_panel, *s_edit_title, *s_edit_value, *s_edit_hint;

/* Tap the meter arc to see where the knob actually is on the network. The one
 * question a headless box cannot answer for itself, and the display is the
 * only channel that needs nothing else already working. */
static lv_obj_t *s_netinfo;
static char      s_netinfo_text[128] = "no network yet";
static uint32_t  s_netinfo_until;        /* lv_tick at which it hides again */
#define NETINFO_MS 10000

typedef enum { ED_NONE = 0, ED_BAND, ED_MODE, ED_FILTER, ED_AGC, ED_GAIN,
               ED_GROUP, ED_RIT, ED_VOL, ED_MIC, ED_RX, ED_ANT, ED_MENU,
               ED_CHOICE, ED_RFGAIN, ED_POWER, ED_TUNER, ED_RXSRC,
               ED_BALANCE, ED_RADIO, ED_VM, ED_SPOT, ED_SSTV } edit_t;
static edit_t  s_edit;
static int     s_edit_idx;
static int     s_edit_from, s_edit_n;  /* where the receiver's opened; how many */
static bool    s_edit_moved;           /* the knob has turned since it opened */
/* The swipe menu (ED_MENU): its items, UI_ACT_*, as the radio offers them;
 * MEM's light, and when it was last tapped -- until the radio agrees, the
 * light is the tap's. */
static uint8_t  s_menu[3], s_menu_n;
static bool     s_mem_lit;
static uint32_t s_mem_tapped;
/* The client's question (ED_CHOICE), and its answer until taken. */
static char     s_ch_title[UI_CHOICES][12], s_ch_name[UI_CHOICES][24];
static volatile int s_ch_answer = -1;
/* The last state ui_update() saw, so the editors can open on the current
 * value. The touch callback runs on the LVGL task and cannot ask the client. */
static ui_state_t s_last;
/* The spots on the band (ui_set_spots), and the chooser's own copy of them,
 * which holds still while it is open. */
EXT_RAM_BSS_ATTR static ui_spot_t s_spots[UI_SPOTS_MAX];
EXT_RAM_BSS_ATTR static ui_spot_t s_spot_snap[UI_SPOTS_MAX];
static uint8_t   s_nspots, s_nsnap;
/* The SSTV viewer (a receiver's face): see sv_open(). */
static lv_obj_t      *s_sv, *s_sv_img, *s_sv_title, *s_sv_cap, *s_sv_wait;
static lv_image_dsc_t s_sv_dsc;
static bool           s_sv_open;
static int            s_sv_idx;
static void sv_title(void);
static void spot_lines(const ui_spot_t *sp, char *l1, size_t n1, char *l2, size_t n2);
static int32_t s_edit_rit;
static int8_t  s_edit_gain, s_edit_gmin, s_edit_gmax, s_edit_gstep;
static int     s_edit_pct;             /* RF GAIN and POWER, 0-100 */
static int     s_edit_bal;             /* BALANCE, -100..100 */
static bool    s_edit_lsb;   /* passband sits below the carrier */
static uint8_t s_volume = 40;
static uint8_t s_micgain = 100;
static ui_commit_t s_commit;
static bool    s_have_commit;

/* Option lists. Modes come from AetherSDR's own modulations_list; the filter
 * widths are the common SSB/CW/digi set rather than a continuous range,
 * because a rotary picking from a short list is far quicker than one
 * scrubbing through hundreds of values. */
#if VFO_RADIO_ICOM
/* The IC-705's own modes (it has no synchronous AM or narrow FM of its own),
 * and its bands, which run on to 2 m and 70 cm. Its AGC is FAST, MID or SLOW,
 * and the gain beside the S-meter is its preamp. */
static const char *MODES[] = { "usb","lsb","cw","cwr","am","fm","rtty",
                               "digu","digl" };
static const char *AGCS[]  = { "fast","mid","slow" };
#define GAIN_CAPTION "P.AMP"
#elif VFO_RADIO_MULTIFLEX
/* The FLEX-6000's own modes and AGC, as its API names them. The gain beside
 * the S-meter is the panadapter's RF gain, which the API carries (TCI does
 * not: the AetherSDR firmware shows it greyed). */
static const char *MODES[] = { "usb","lsb","cw","am","sam","fm","nfm",
                               "digu","digl","rtty" };
static const char *AGCS[]  = { "fast","med","slow","off" };
#define GAIN_CAPTION "RF.G"
#elif VFO_RADIO_XIEGU
/* The X6100 and X6200 are HF and 6 m radios behind an IC-705's CI-V, so
 * the IC-705's names, less the modes they lack; the preamp is their PRE. */
static const char *MODES[] = { "usb","lsb","cw","cwr","am","fm","digu","digl" };
static const char *AGCS[]  = { "fast","mid","slow" };
#define GAIN_CAPTION "PRE"
#elif VFO_RADIO_UBERSDR
/* UberSDR's own modes, by its own names. It has no AGC to choose -- its SNR
 * is shown where the AGC is -- and the gain beside the S-meter is its noise
 * filter, by name (n_gain_names). */
static const char *MODES[] = { "usb","lsb","cwu","cwl","am","sam","fm","nfm" };
static const char *AGCS[]  = { "" };
#define GAIN_CAPTION "FIL"
#elif VFO_RADIO_SVXCONNECT
/* A reflector has no modes, AGC or gain; the tables stay for the editors'
 * sake, which the reflector face never opens. */
static const char *MODES[] = { "fm" };
static const char *AGCS[]  = { "fast" };
#define GAIN_CAPTION ""
#else
/* AetherSDR passes FlexRadio's AGC settings through by name, and the gain
 * beside the S-meter is the panadapter's RF gain. */
static const char *MODES[] = { "usb","lsb","cw","cwr","am","sam","fm","nfm",
                               "digu","digl","rtty" };
static const char *AGCS[]  = { "fast","med","slow","off" };
#define GAIN_CAPTION "RF.G"
#endif
#if VFO_RADIO_UBERSDR
/* Its passbands, by mode: the widths UberSDR allows each (CW +-500 Hz, voice
 * up to 6 kHz, AM +-6 kHz, FM +-8 kHz). */
static const int32_t F_SSB[] = { 1800, 2100, 2400, 2700, 3000, 3600, 4200, 5000 };
static const int32_t F_CW[]  = { 100, 200, 300, 400, 500, 800, 1000 };
static const int32_t F_AM[]  = { 4000, 6000, 8000, 10000, 12000 };
static const int32_t F_FM[]  = { 10000, 12000, 16000 };
static const int32_t *FILTERS = F_SSB;
static int           N_FILTERS = (int)(sizeof F_SSB / sizeof F_SSB[0]);

static void filters_for(const char *m)
{
#define USE(a) do { FILTERS = a; N_FILTERS = (int)(sizeof a / sizeof a[0]); } while (0)
    if (m && !strncasecmp(m, "cw", 2))                          USE(F_CW);
    else if (m && (!strcasecmp(m, "am") || !strcasecmp(m, "sam"))) USE(F_AM);
    else if (m && (!strcasecmp(m, "fm") || !strcasecmp(m, "nfm"))) USE(F_FM);
    else                                                         USE(F_SSB);
#undef USE
}

/* A passband on both sides of the carrier, not one. */
static bool filter_centred(const char *m)
{
    return m && (!strncasecmp(m, "cw", 2) || !strcasecmp(m, "am") || !strcasecmp(m, "sam") ||
                 !strcasecmp(m, "fm") || !strcasecmp(m, "nfm"));
}
#else
static const int32_t FILTERS[] = { 250, 500, 700, 1000, 1500, 1800, 2100,
                                   2400, 2700, 3000, 3600, 6000 };
#define N_FILTERS ((int)(sizeof FILTERS / sizeof FILTERS[0]))
#endif
/* A radio with filter presets (the IC-705's FIL1-3) is offered those instead
 * of widths: its widths belong to each preset, set on the radio. */
#define N_PRESETS 3
static bool s_edit_presets;
static const struct { const char *name; int64_t hz; } BANDS[] = {
    { "160m",  1840000 }, { "80m",   3700000 }, { "60m",   5355000 },
    { "40m",   7100000 }, { "30m",  10130000 }, { "20m",  14100000 },
    { "17m",  18120000 }, { "15m",  21200000 }, { "12m",  24940000 },
    { "10m",  28400000 },
#if !VFO_RADIO_UBERSDR
    { "6m",   50200000 },               /* an UberSDR stops at 30 MHz */
#endif
#if VFO_RADIO_ICOM
    { "2m",  144300000 }, { "70cm", 432200000 },
#endif
};
#define NELEM(a) ((int)(sizeof (a) / sizeof (a)[0]))
static int   s_dig_x[N_DIG];
static int   s_active_dig = 5;
static int32_t s_step_req;
static bool  s_ptt_tap, s_was_tx;
static bool  s_ptt_armed;            /* a press on the slab, in receive: see touch_cb */
/* A PTT press counts only once the finger has been up this long (touch_cb).
 * A flicker is a few tens of ms; a deliberate second tap is well over this. */
#define PTT_REARM_MS 150
static uint32_t s_released_at;       /* lv_tick of the last release */
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
/* LVGL's built-in snprintf does NOT handle %f unless LV_SPRINTF_USE_FLOAT is
 * enabled, and silently emits a literal "f" instead -- which is exactly how
 * "SWR 11.5" reached the glass as "SWR f". Format fixed-point by hand rather
 * than depend on a config flag, and keep float printf out of the binary. */
static void fmt1(char *out, size_t n, const char *pre, float v, const char *suf)
{
    if (v < 0) v = 0;
    int t = (int)(v * 10.0f + 0.5f);
    snprintf(out, n, "%s%d.%d%s", pre, t / 10, t % 10, suf);
}

/* S0 = -127 dBm, S9 = -73, S9+60 = -13, and S9 sits at 60% of the scale --
 * matching AetherSDR's own s-meter-v1.json. A linear ring would look wrong
 * next to the desktop. */
static float smeter_frac(float dbm)
{
#if VFO_RADIO_SVXCONNECT
    /* The reflector face's arc is the audio level: -60 to 0 dBFS, evenly. */
    if (dbm < -60.0f) dbm = -60.0f;
    if (dbm > 0.0f)   dbm = 0.0f;
    return (dbm + 60.0f) / 60.0f;
#else
    if (dbm < -127.0f) dbm = -127.0f;
    if (dbm > -13.0f)  dbm = -13.0f;
    return (dbm <= -73.0f) ? 0.6f * (dbm + 127.0f) / 54.0f
                           : 0.6f + 0.4f * (dbm + 73.0f) / 60.0f;
#endif
}

RADIO_ONLY static void smeter_text(float dbm, char *out, size_t n)
{
    if (dbm >= -73.0f) snprintf(out, n, "S9+%d", (int)((dbm + 73.0f) / 10.0f) * 10);
    else {
        int s = (int)((dbm + 127.0f) / 6.0f);
        if (s < 0) s = 0;
        if (s > 9) s = 9;
        snprintf(out, n, "S%d", s);
    }
}

RADIO_ONLY static const char *band_of(int64_t hz)
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
    if (m >= 144000 && m <= 148000) return "2m";
    if (m >= 430000 && m <= 440000) return "70cm";
    return "--";
}

/* Upper case for the glass: modes and AGC settings arrive as "usb", "mid". */
static void upcase(const char *in, char *out, size_t n)
{
    size_t i = 0;
    for (; in && in[i] && i + 1 < n; i++)
        out[i] = (in[i] >= 'a' && in[i] <= 'z') ? (char)(in[i] - 32) : in[i];
    out[i] = 0;
}

/* The gain as the radio puts it: the IC-705's preamp is OFF, 1 or 2 -- or ON
 * where it has only the one -- and AetherSDR's RF gain is in dB. */
static void gain_text(int g, int gmax, char *out, size_t n)
{
#if VFO_RADIO_ICOM || VFO_RADIO_XIEGU
    if (g <= 0)         snprintf(out, n, "OFF");
    else if (gmax <= 1) snprintf(out, n, "ON");
    else                snprintf(out, n, "%d", g);
#else
    (void)gmax;
    if (g) snprintf(out, n, "%+d dB", g);
    else   snprintf(out, n, "0 dB");
#endif
}

/* The gain's name where its steps have one (the ubersdr firmware's filter). */
static void gain_label(const ui_state_t *st, int g, char *out, size_t n)
{
    if (st->n_gain_names && g >= 0 && g < st->n_gain_names) snprintf(out, n, "%s", st->gain_names[g]);
    else                                                    gain_text(g, st->gain_max, out, n);
}

/* 145.6375, 438.625: MHz to the 100 Hz digit, which FM channels need and
 * nothing finer. */
static void mhz_text(int64_t hz, char *out, size_t n)
{
    if (hz < 0) hz = 0;
    const long mhz = (long)(hz / 1000000), khz = (long)(hz / 1000 % 1000);
    const int  d   = (int)(hz % 1000 / 100);
    if (d) snprintf(out, n, "%ld.%03ld%d", mhz, khz, d);
    else   snprintf(out, n, "%ld.%03ld", mhz, khz);
}

/* A repeater's shift as it is usually said: -0.6, +7.6, -1.25. */
static void shift_text(int8_t dup, int32_t hz, char *out, size_t n)
{
    if (!dup) { out[0] = 0; return; }
    const long khz = (long)(hz / 1000) % 100000;
    char frac[12];
    snprintf(frac, sizeof frac, "%03ld", khz % 1000);
    for (int i = 2; i > 0 && frac[i] == '0'; i--) frac[i] = 0;
    snprintf(out, n, "%c%ld.%s", dup < 0 ? '-' : '+', khz / 1000, frac);
}

/* What the memory face says: a channel, or why there is none yet. */
RADIO_ONLY static void mem_texts(const ui_state_t *st, char *big, size_t nb, char *small, size_t ns)
{
    if (st->mem_state == UI_MEM_READING) {
        snprintf(big, nb, "MEMORIES");
        snprintf(small, ns, "reading group %02u", (unsigned)st->mem_group);
        return;
    }
    if (st->mem_state == UI_MEM_EMPTY) {
        snprintf(big, nb, "NO MEMORIES");
        snprintf(small, ns, "in group %02u", (unsigned)st->mem_group);
        return;
    }
    char f[24], sh[32] = "", tn[16] = "", t[24];
    mhz_text(st->freq_hz, f, sizeof f);
    shift_text(st->mem_duplex, st->mem_offset_hz, t, sizeof t);
    if (t[0]) snprintf(sh, sizeof sh, "  %s", t);
    if (st->mem_tone_dhz)
        snprintf(tn, sizeof tn, "  T%u.%u", st->mem_tone_dhz / 10u, st->mem_tone_dhz % 10u);
    if (st->mem_name[0]) {
        snprintf(big, nb, "%s", st->mem_name);
        snprintf(small, ns, "M%02u  %s%s%s", (unsigned)st->mem_ch, f, sh, tn);
    } else {                          /* no name: the frequency is the headline */
        snprintf(big, nb, "%s", f);
        snprintf(small, ns, "M%02u%s%s", (unsigned)st->mem_ch, sh, tn);
    }
}

/* --- field editors -------------------------------------------------------- */

/* The antenna editor's choices: each antenna, then each again with the RX ANT
 * input -- ANT1, ANT2, ANT1+RX, ANT2+RX on an IC-7610. */
static int ant_choices(const ui_state_t *st)
{
    return st->n_ant * (st->has_rx_ant ? 2 : 1);
}

static int ant_index(const ui_state_t *st)
{
    return st->ant + (st->ant_rx ? st->n_ant : 0);
}

static void edit_render(void)
{
    if (s_edit == ED_NONE) {
        lv_obj_add_flag(s_edit_panel, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_remove_flag(s_edit_panel, LV_OBJ_FLAG_HIDDEN);

    char v[24];
    const char *title = "";
    lv_color_t vcolor = C_ACCENT_HI;
    switch (s_edit) {
    case ED_BAND:
        title = "BAND";
        snprintf(v, sizeof v, "%s", BANDS[s_edit_idx].name);
        break;
    case ED_MODE:
        title = "MODE";
        upcase(MODES[s_edit_idx], v, 8);
        break;
    case ED_FILTER:
        title = "FILTER";
        if (s_edit_presets) snprintf(v, sizeof v, "FIL%d", s_edit_idx + 1);
        else                snprintf(v, sizeof v, "%ld Hz", (long)FILTERS[s_edit_idx]);
        break;
    case ED_AGC:
        title = "AGC";
        upcase(AGCS[s_edit_idx], v, sizeof v);
        break;
    case ED_GAIN:
        title = s_last.n_gain_names ? "NOISE FILTER" : GAIN_CAPTION;
        gain_label(&s_last, s_edit_gain, v, sizeof v);
        break;
    case ED_GROUP:
        title = "MEMORY GROUP";
        snprintf(v, sizeof v, "%02d", s_edit_idx);
        break;
    case ED_RIT:
        title = "RIT";
        snprintf(v, sizeof v, "%+ld Hz", (long)s_edit_rit);
        break;
    case ED_VOL:
        title = "VOLUME";
        snprintf(v, sizeof v, "%d", s_volume);
        break;
    case ED_MIC:
        title = "MIC GAIN";
        snprintf(v, sizeof v, "%d", s_micgain);
        break;
    case ED_RX:
        title = "VFO";
        snprintf(v, sizeof v, "%s", s_edit_idx ? "SUB" : "MAIN");
        break;
    case ED_MENU: {
        static const char *NAME[] = { "", "TUNE", "ATU", "MEM" };
        const uint8_t a = s_menu[s_edit_idx];
        title = "MENU";
        /* The tuner's memories light when they are on, as the button on
         * SmartSDR's TX panel does, and are dimmed when off; a tap turns
         * them the other way. */
        snprintf(v, sizeof v, "%s", NAME[a]);
        vcolor = a == UI_ACT_MEM && !s_mem_lit ? C_DISABLED : C_ACCENT_HI;
        break;
    }
    case ED_CHOICE:
        title = s_ch_title[s_edit_idx];
        snprintf(v, sizeof v, "%s", s_ch_name[s_edit_idx]);
        break;
    case ED_ANT: {
        const int n = s_last.n_ant ? s_last.n_ant : 1;
        title = "ANTENNA";
        snprintf(v, sizeof v, "ANT%d%s", s_edit_idx % n + 1, s_edit_idx >= n ? "+RX" : "");
        break;
    }
    case ED_RFGAIN:
        title = "RF GAIN";
        snprintf(v, sizeof v, "%d%%", s_edit_pct);
        break;
    case ED_POWER:
        title = "POWER";
        /* In watts where the radio's full scale is known: the IC-7610's
         * 100 W, the IC-705's 10 W. */
        if (s_last.max_w) snprintf(v, sizeof v, "%d W", (s_edit_pct * s_last.max_w + 50) / 100);
        else              snprintf(v, sizeof v, "%d%%", s_edit_pct);
        break;
    case ED_TUNER:
        title = "TUNER";
        snprintf(v, sizeof v, "%s", s_edit_idx ? "ON" : "OFF");
        vcolor = s_edit_idx ? C_ACCENT_HI : C_DISABLED;
        break;
    case ED_RXSRC:
        title = "RX";
        snprintf(v, sizeof v, "%s", s_edit_idx ? s_last.sdr_name[s_edit_idx - 1] : "LOCAL");
        break;
    case ED_RADIO:
        title = "RADIO";
        snprintf(v, sizeof v, "%s", s_last.radio_name[s_edit_idx]);
        break;
    case ED_VM:
        /* Icom's own name for it: the V/M key. */
        title = "V/M";
        snprintf(v, sizeof v, "%s", s_edit_idx ? "MEMORY" : "VFO");
        break;
    case ED_BALANCE:
        title = "BALANCE";
        /* 0: the radio left and the SDR right; towards an end, that one alone
         * -- on a receiver's face, the UberSDR left and a KiwiSDR right. */
        if (s_edit_bal <= -100)     snprintf(v, sizeof v, "%s", RX_FACE ? "UBER" : "RADIO");
        else if (s_edit_bal >= 100) snprintf(v, sizeof v, "%s", RX_FACE ? "KIWI" : "SDR");
        else if (s_edit_bal == 0)   snprintf(v, sizeof v, "L | R");
        else if (s_edit_bal < 0)    snprintf(v, sizeof v, LV_SYMBOL_LEFT " %d", -s_edit_bal);
        else                        snprintf(v, sizeof v, "%d " LV_SYMBOL_RIGHT, s_edit_bal);
        break;
    case ED_SPOT: {
        static char t[24];
        char l2[56];
        snprintf(t, sizeof t, "SPOT %d / %d", s_edit_idx + 1, (int)s_nsnap);
        title = t;
        spot_lines(&s_spot_snap[s_edit_idx], v, sizeof v, l2, sizeof l2);
        break;
    }
    case ED_SSTV:
        title = "SSTV";
        if (s_last.n_sstv > 0) snprintf(v, sizeof v, "%d PICTURES", s_last.n_sstv);
        else                   snprintf(v, sizeof v, "NONE YET");
        vcolor = s_last.n_sstv > 0 ? C_ACCENT_HI : C_DISABLED;
        break;
    default: return;
    }
    lv_label_set_text(s_edit_title, title);
    /* A station's name wants more room than a mode or a width. */
    lv_obj_set_style_text_font(s_edit_value,
                               s_edit == ED_CHOICE || s_edit == ED_RADIO || s_edit == ED_VM ||
                               s_edit == ED_SPOT || s_edit == ED_SSTV || (s_edit == ED_RXSRC && s_edit_idx)
                               ? &lv_font_montserrat_28 : &lv_font_montserrat_48, 0);
    lv_label_set_text(s_edit_value, v);
    lv_obj_set_style_text_color(s_edit_value, vcolor, 0);
    /* The radio chooser says how each is reached: directly, or through a
     * service -- the same FlexRadio can be both. */
    if (s_edit == ED_RADIO) {
        char h[40];
        /* A receiver is reached however its address says: nothing to add. */
        if (RX_FACE) snprintf(h, sizeof h, "tap to switch");
        else         snprintf(h, sizeof h, "%s  -  tap to switch",
                              s_edit_idx < s_last.n_radios_direct ? "LAN" : s_last.radio_via);
        lv_label_set_text(s_edit_hint, h);
    } else if (s_edit == ED_SPOT) {
        /* Where, in what, and what it is: 14.205.0 USB  DX 4m */
        char l1[24], h[56];
        spot_lines(&s_spot_snap[s_edit_idx], l1, sizeof l1, h, sizeof h);
        lv_label_set_text(s_edit_hint, h);
    } else if (s_edit == ED_SSTV) {
        lv_label_set_text(s_edit_hint, s_last.n_sstv > 0 ? "tap to look  -  the knob turns them"
                                                         : "the receiver's gallery is empty");
    } else if (strcmp(lv_label_get_text(s_edit_hint), "turn to choose  -  tap to accept")) {
        lv_label_set_text(s_edit_hint, "turn to choose  -  tap to accept");
    }
}

static int index_of_mode(const char *m)
{
    for (int i = 0; i < NELEM(MODES); i++)
        if (m && strcasecmp(MODES[i], m) == 0) return i;
    return 0;
}

static int index_of_agc(const char *a)
{
    for (int i = 0; i < NELEM(AGCS); i++)
        if (a && strcasecmp(AGCS[i], a) == 0) return i;
    return 0;
}

static int nearest_filter(int32_t w)
{
    int best = 0;
    int32_t bd = 1 << 30;
    for (int i = 0; i < N_FILTERS; i++) {
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

static void netinfo_show(bool on);

static void edit_open(edit_t what, const ui_state_t *st)
{
    s_edit = what;
    netinfo_show(false);                  /* it would peek out from behind */
    /* ...and a warning -- NO LINK, with the radio in use switched off --
     * would cover it: it waits until the editor closes. */
    if (s_warn_panel) lv_obj_add_flag(s_warn_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_edit_panel);
    switch (what) {
    case ED_BAND:   s_edit_idx = nearest_band(st->freq_hz); break;
    case ED_MODE:   s_edit_idx = index_of_mode(st->mode);   break;
    case ED_FILTER:
#if VFO_RADIO_UBERSDR
        filters_for(st->mode);
#endif
        s_edit_presets = st->filter_no != 0;
        if (s_edit_presets) {
            s_edit_idx = st->filter_no - 1;
            break;
        }
        s_edit_idx = nearest_filter(st->filt_hi - st->filt_lo);
        /* Remember which side of the carrier this mode uses. Applying a
         * positive passband to LSB mutes the radio, which reads as a hardware
         * fault rather than a filter setting. */
        s_edit_lsb = (st->filt_hi <= 0) ||
                     (st->mode && (strcasecmp(st->mode, "lsb") == 0 ||
                                   strcasecmp(st->mode, "cwr") == 0 ||
                                   strcasecmp(st->mode, "digl") == 0));
        break;
    case ED_AGC:    s_edit_idx = index_of_agc(st->agc); break;
    case ED_GROUP:  s_edit_idx = st->mem_group; break;
    case ED_GAIN:
        s_edit_gain  = st->gain;
        s_edit_gmin  = st->gain_min;
        s_edit_gmax  = st->gain_max;
        s_edit_gstep = st->gain_step > 0 ? st->gain_step : 1;
        break;
    case ED_RIT:    s_edit_rit = st->rit_hz; break;
    case ED_RX:
        s_edit_idx = st->rx;
        s_edit_n   = st->n_rx;
        break;
    case ED_ANT:
        s_edit_idx = ant_index(st);
        s_edit_n   = ant_choices(st);
        break;
    case ED_MENU:
        s_menu_n = 0;
        if (st->has_tune) s_menu[s_menu_n++] = UI_ACT_TUNE;
        if (st->has_atu)  s_menu[s_menu_n++] = UI_ACT_ATU;
        if (st->has_atu)  s_menu[s_menu_n++] = UI_ACT_MEM;
        s_edit_n   = s_menu_n;
        /* Opened on MEM, the one item that does not transmit: TUNE and the
         * tuner are a turn away, and then a tap on the panel. */
        s_edit_idx   = s_menu_n - 1;
        s_mem_lit    = st->atu_mem;
        s_mem_tapped = 0;
        break;
    case ED_RXSRC:
        s_edit_n   = 1 + st->n_sdr;
        s_edit_idx = st->rxsrc >= 0 && st->rxsrc < st->n_sdr ? st->rxsrc + 1 : 0;
        break;
    case ED_BALANCE: s_edit_bal = st->balance; break;
    case ED_SPOT: {
        /* Opened on the spot nearest the dial. */
        memcpy(s_spot_snap, s_spots, sizeof s_spots[0] * s_nspots);
        s_nsnap    = s_nspots;
        s_edit_n   = s_nsnap;
        s_edit_idx = 0;
        int64_t best = INT64_MAX;
        for (int i = 0; i < s_nsnap; i++) {
            const int64_t d = llabs((int64_t)s_spot_snap[i].hz - st->freq_hz);
            if (d < best) { best = d; s_edit_idx = i; }
        }
        break;
    }
    case ED_RADIO:
        s_edit_n   = st->n_radios;
        s_edit_idx = st->radio_sel >= 0 && st->radio_sel < st->n_radios ? st->radio_sel : 0;
        break;
    case ED_VM:
        s_edit_n   = 2;
        s_edit_idx = st->mem_state != UI_MEM_OFF;
        break;
    case ED_RFGAIN: s_edit_pct = st->rf_gain_pct;  break;
    case ED_POWER:  s_edit_pct = st->rf_power_pct; break;
    case ED_TUNER:
        s_edit_idx = st->tuner_on ? 1 : 0;
        s_edit_n   = 2;
        break;
    default: break;
    }
    s_edit_from  = s_edit_idx;
    s_edit_moved = false;
    edit_render();
}

/* Filter, AGC, gain and RIT go to the radio as the knob turns them, and a
 * tap only closes them; volume and mic gain are the knob's own and apply as
 * they change. The rest -- band, mode, group, VFO, antenna, the menu -- act
 * on a tap on their panel, and a tap anywhere else closes them untouched. */
static bool edit_live(edit_t e)
{
    return e == ED_FILTER || e == ED_AGC || e == ED_GAIN || e == ED_RIT ||
           e == ED_VOL || e == ED_MIC || e == ED_RFGAIN || e == ED_POWER ||
           e == ED_TUNER || e == ED_BALANCE;
}

/* What the open editor's value asks of the radio, into s_commit. */
static void edit_fill(void)
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
        if (s_edit_presets) {
            s_commit.have_filter_no = true;
            s_commit.filter_no = (uint8_t)(s_edit_idx + 1);
            break;
        }
        s_commit.have_filter = true;
        int32_t w = FILTERS[s_edit_idx];
#if VFO_RADIO_UBERSDR
        /* UberSDR's edges: 50 Hz off the carrier for a sideband, as its own
         * defaults are, and centred on it for everything else. */
        if (filter_centred(s_last.mode)) { s_commit.filt_lo = -w / 2; s_commit.filt_hi = w / 2; }
        else if (s_edit_lsb)             { s_commit.filt_lo = -w;     s_commit.filt_hi = -50; }
        else                             { s_commit.filt_lo = 50;     s_commit.filt_hi = w;   }
        break;
#endif
        if (s_edit_lsb) { s_commit.filt_lo = -w;  s_commit.filt_hi = -100; }
        else            { s_commit.filt_lo = 100; s_commit.filt_hi =  w;   }
        break;
    }
    case ED_AGC:
        s_commit.have_agc = true;
        strlcpy(s_commit.agc, AGCS[s_edit_idx], sizeof s_commit.agc);
        break;
    case ED_GAIN:
        s_commit.have_gain = true;
        s_commit.gain      = s_edit_gain;
        break;
    case ED_GROUP:
        s_commit.have_mem_group = true;
        s_commit.mem_group      = (uint8_t)s_edit_idx;
        break;
    case ED_RIT:
        s_commit.have_rit = true;
        s_commit.rit_hz   = s_edit_rit;
        break;
    case ED_RX:
        s_commit.have_rx = s_edit_idx != s_edit_from;
        s_commit.rx      = (uint8_t)s_edit_idx;
        break;
    case ED_MENU:
        s_commit.action = s_menu[s_edit_idx];
        break;
    case ED_RFGAIN:
        s_commit.have_rf_gain = true;
        s_commit.rf_gain_pct  = (uint8_t)s_edit_pct;
        break;
    case ED_POWER:
        s_commit.have_rf_power = true;
        s_commit.rf_power_pct  = (uint8_t)s_edit_pct;
        break;
    case ED_TUNER:
        s_commit.have_tuner = true;
        s_commit.tuner_on   = s_edit_idx == 1;
        break;
    case ED_RXSRC:
        s_commit.have_rxsrc = true;
        s_commit.rxsrc      = (int8_t)(s_edit_idx - 1);
        break;
    case ED_BALANCE:
        s_commit.have_balance = true;
        s_commit.balance      = (int8_t)s_edit_bal;
        break;
    case ED_RADIO:
        s_commit.have_radio = true;
        s_commit.radio      = (int8_t)s_edit_idx;
        break;
    case ED_VM:
        s_commit.have_vm = true;
        s_commit.vm_mem  = s_edit_idx == 1;
        break;
    case ED_SPOT:
        s_commit.have_spot = s_nsnap > 0;
        s_commit.spot_hz   = s_spot_snap[s_edit_idx].hz;
        strlcpy(s_commit.spot_mode, s_spot_snap[s_edit_idx].mode, sizeof s_commit.spot_mode);
        break;
    case ED_ANT: {
        /* Only when turned to: until then the editor follows the radio, and
         * a tap straight through leaves the antenna alone. */
        const int n = s_last.n_ant ? s_last.n_ant : 1;
        s_commit.have_ant = s_edit_moved;
        s_commit.ant      = (uint8_t)(s_edit_idx % n);
        s_commit.ant_rx   = s_edit_idx >= n;
        break;
    }
    default: break;      /* volume and mic gain are the knob's own */
    }
}

static void edit_close(void)
{
    s_edit = ED_NONE;
    edit_render();
}

static void edit_commit(void)
{
    edit_fill();
    s_have_commit = s_edit != ED_NONE && s_edit != ED_VOL && s_edit != ED_MIC;
    edit_close();
}

/* A live editor's value, to the radio now. The ui task takes the newest
 * every 50 ms, so a fast turn is not a flood of commands. */
static void edit_publish(void)
{
    if (s_edit == ED_VOL || s_edit == ED_MIC) return;
    edit_fill();
    s_commit.live = true;
    s_have_commit = true;
}

bool ui_edit_active(void) { return s_edit != ED_NONE || s_sv_open; }

void ui_edit_rotate(int32_t detents)
{
    if ((s_edit == ED_NONE && !s_sv_open) || !detents) return;
    if (!lvgl_port_lock(20)) return;
    /* The SSTV viewer: the next picture, or the one before. */
    if (s_sv_open) {
        int i = s_sv_idx + (detents > 0 ? 1 : -1);
        if (i >= s_last.n_sstv) i = s_last.n_sstv - 1;
        if (i < 0) i = 0;
        if (i != s_sv_idx) {
            s_sv_idx = i;
            sv_title();
        }
        lvgl_port_unlock();
        return;
    }
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
    case ED_FILTER: {
        const int n = s_edit_presets ? N_PRESETS : N_FILTERS;
        s_edit_idx += detents;
        if (s_edit_idx < 0) s_edit_idx = 0;
        if (s_edit_idx >= n) s_edit_idx = n - 1;
        break;
    }
    case ED_AGC:
        s_edit_idx += detents;
        if (s_edit_idx < 0) s_edit_idx = 0;
        if (s_edit_idx >= NELEM(AGCS)) s_edit_idx = NELEM(AGCS) - 1;
        break;
    case ED_GROUP:
        s_edit_idx += detents;
        if (s_edit_idx < 0)  s_edit_idx = 0;
        if (s_edit_idx > 99) s_edit_idx = 99;
        break;
    case ED_RFGAIN:
    case ED_POWER:
        s_edit_pct += detents;
        if (s_edit_pct < 0)   s_edit_pct = 0;
        if (s_edit_pct > 100) s_edit_pct = 100;
        break;
    case ED_BALANCE:
        s_edit_bal += detents * 10;
        if (s_edit_bal < -100) s_edit_bal = -100;
        if (s_edit_bal > 100)  s_edit_bal = 100;
        break;
    case ED_RXSRC:
    case ED_RADIO:
    case ED_VM:
    case ED_SPOT:
    case ED_RX:
    case ED_ANT:
    case ED_MENU:
    case ED_CHOICE:
    case ED_TUNER:
        s_edit_idx += detents;
        if (s_edit_idx < 0)         s_edit_idx = 0;
        if (s_edit_idx >= s_edit_n) s_edit_idx = s_edit_n - 1;
        s_edit_moved = true;
        break;
    case ED_GAIN: {
        int g = s_edit_gain + detents * s_edit_gstep;
        if (g < s_edit_gmin) g = s_edit_gmin;
        if (g > s_edit_gmax) g = s_edit_gmax;
        s_edit_gain = (int8_t)g;
        break;
    }
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
    case ED_MIC: {
        /* Up to 200%: the PDM element is quiet, and the alternative to gain
         * here is asking the operator to shout at a knob. */
        int v = s_micgain + detents * 5;
        if (v < 0)   v = 0;
        if (v > 200) v = 200;
        s_micgain = (uint8_t)v;
        break;
    }
    default: break;
    }
    if (edit_live(s_edit)) edit_publish();
    edit_render();
    lvgl_port_unlock();
}

void ui_ask_choice(const char titles[][12], const char names[][24], uint8_t n, uint8_t def)
{
    if (!lvgl_port_lock(50)) return;
    if (!n || !titles || !names) {
        if (s_edit == ED_CHOICE) edit_close();
    } else {
        if (n > UI_CHOICES) n = UI_CHOICES;
        for (uint8_t i = 0; i < n; i++) {
            strlcpy(s_ch_title[i], titles[i], sizeof s_ch_title[i]);
            strlcpy(s_ch_name[i], names[i], sizeof s_ch_name[i]);
        }
        /* The question comes first: over any editor, and over a warning
         * that would otherwise sit on top of it. */
        s_edit       = ED_CHOICE;
        s_edit_n     = n;
        s_edit_idx   = def < n ? def : 0;
        s_edit_moved = false;
        s_ch_answer  = -1;
        if (s_warn_panel) lv_obj_add_flag(s_warn_panel, LV_OBJ_FLAG_HIDDEN);
        netinfo_show(false);
        edit_render();
        lv_obj_move_foreground(s_edit_panel);
    }
    lvgl_port_unlock();
}

bool ui_choice_active(void) { return s_edit == ED_CHOICE; }

int ui_take_choice(void)
{
    const int a = s_ch_answer;
    s_ch_answer = -1;
    return a;
}

bool ui_take_commit(ui_commit_t *out)
{
    if (!s_have_commit || !out) return false;
    /* A live editor writes these from the knob task, under the port lock. */
    if (!lvgl_port_lock(20)) return false;
    const bool have = s_have_commit;
    if (have) *out = s_commit;
    s_have_commit = false;
    lvgl_port_unlock();
    return have;
}

uint8_t ui_volume(void)  { return s_volume; }
uint8_t ui_mic_gain(void) { return s_micgain; }

void ui_set_netinfo(const char *text)
{
    if (!text) return;
    /* Copied under the port lock because the card may be on screen and the
     * label points straight at this buffer. */
    if (!lvgl_port_lock(20)) return;
    strlcpy(s_netinfo_text, text, sizeof s_netinfo_text);
    if (s_netinfo && !lv_obj_has_flag(s_netinfo, LV_OBJ_FLAG_HIDDEN))
        lv_label_set_text(s_netinfo, s_netinfo_text);
    lvgl_port_unlock();
}

void ui_set_levels(uint8_t volume, uint8_t mic_gain)
{
    if (volume   <= 100) s_volume  = volume;
    if (mic_gain <= 200) s_micgain = mic_gain;
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

/* The addresses come up on a long press on the meter arc. A tap there used
 * to bring them up, and the arc is where a hand reaching for the dial lands:
 * they kept appearing by themselves. They come up the moment the press is
 * long enough, with a click from the motor, so the finger knows it can let
 * go -- not when it does, which left it guessing. */
#define NETINFO_HOLD_MS 600
static uint32_t s_pressed_at;
static bool          s_press_card;        /* this press may bring the card up */
static volatile bool s_card_shown;        /* ...and did: the click is owed */

/* The firmware picker -- the setup firmware, with the WiFi kept -- is a
 * second step behind the addresses: with them on screen, a press on the
 * S-meter or on the addresses themselves, held three seconds, when the motor
 * buzzes. Two fingers would have been the obvious sign, but the CST816
 * reports one touch only; two deliberate holds in a row are not something a
 * hand does by accident, and a turn of the knob must still say yes. Five
 * seconds was too long: counted by hand, most holds let go at 4 to 4.9 s --
 * and the question comes up under the finger, where it cannot be seen. With
 * something wrong -- no link, as on a knob with another radio's firmware --
 * the warning panel carries the addresses in the card's place, and counts as
 * the card. */
#define PICKER_HOLD_MS 3000
static bool          s_press_picker;      /* this press may ask for the picker */
static volatile bool s_picker_req;

/* Taps act when the finger lifts, not when it lands -- all but PTT and the
 * update question. A swipe starts with a press too, and acting on the press
 * would first take it for a tap on whatever it started on: a swipe begun on
 * the mode would open the mode's editor. So the press is only noted, and the
 * release decides: a gesture on the way, or a finger that wandered, means it
 * was not a tap. */
#define TAP_SLOP 24                       /* px a tap may wander */
static lv_point_t    s_press_pt;
static bool          s_press_tap;         /* this press may still be a tap */
static bool          s_gestured;          /* ...and this one became a gesture */

static bool shown_at(lv_obj_t *o, lv_point_t p)
{
    if (!o || lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) return false;
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    return p.x >= a.x1 && p.x <= a.x2 && p.y >= a.y1 && p.y <= a.y2;
}

/* The addresses on screen: the card, or the warning panel in its place. */
static bool addresses_up(void)
{
    return (s_netinfo && !lv_obj_has_flag(s_netinfo, LV_OBJ_FLAG_HIDDEN)) ||
           !lv_obj_has_flag(s_warn_panel, LV_OBJ_FLAG_HIDDEN);
}

static void netinfo_show(bool on)
{
    if (!s_netinfo) return;
    if (on) {
        lv_label_set_text(s_netinfo, s_netinfo_text);
        lv_obj_clear_flag(s_netinfo, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_netinfo);
        s_netinfo_until = lv_tick_get() + NETINFO_MS;
    } else {
        lv_obj_add_flag(s_netinfo, LV_OBJ_FLAG_HIDDEN);
        s_netinfo_until = 0;
    }
}

/* AGC and the gain either side of the S-unit readout, where a tap opens
 * their editors: receive settings, shown only in receive, and like every
 * editor only with a link. The rest of the arc is the addresses'. */
static bool aux_spot(lv_point_t p)
{
    if (REFLECTOR_FACE || p.y < AUX_TOP || p.y >= 104 || !s_last.link_ok || s_last.tx)
        return false;
    const int dx = p.x - CX;
    /* A receiver's SNR, where the AGC is, is only a reading. */
    return (dx <= -AUX_IN && dx >= -AUX_OUT && !RX_FACE) ||
           (dx >= AUX_IN && dx <= AUX_OUT && s_last.have_gain);
}

/* --- the SSTV viewer (a receiver's face) ---------------------------------- */

static void sv_close(void)
{
    if (!s_sv_open) return;
    s_sv_open = false;
    lv_obj_add_flag(s_sv, LV_OBJ_FLAG_HIDDEN);
    lv_image_set_src(s_sv_img, NULL);
    lv_image_cache_drop(&s_sv_dsc);
}

static void sv_title(void)
{
    char t[24];
    snprintf(t, sizeof t, "%d / %d", s_sv_idx + 1, (int)s_last.n_sstv);
    lv_label_set_text(s_sv_title, t);
    lv_label_set_text(s_sv_wait, "fetching...");
    lv_obj_remove_flag(s_sv_wait, LV_OBJ_FLAG_HIDDEN);
    /* The last picture stays, dimmed, until the next is there. */
    lv_obj_set_style_image_opa(s_sv_img, LV_OPA_30, 0);
}

static void sv_open(void)
{
    if (!s_sv) {
        /* Over the whole face, black as the receiver's page: the picture in
         * the middle, what it is above it, where and when under it. */
        s_sv = lv_obj_create(s_scr);
        lv_obj_set_size(s_sv, 360, 360);
        lv_obj_center(s_sv);
        lv_obj_set_style_bg_color(s_sv, C_BG, 0);
        lv_obj_set_style_bg_opa(s_sv, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(s_sv, 0, 0);
        lv_obj_set_style_radius(s_sv, 0, 0);
        lv_obj_set_style_pad_all(s_sv, 0, 0);
        lv_obj_remove_flag(s_sv, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(s_sv, LV_OBJ_FLAG_CLICKABLE);
        s_sv_img = lv_image_create(s_sv);
        lv_obj_align(s_sv_img, LV_ALIGN_CENTER, 0, 0);
        lv_obj_remove_flag(s_sv_img, LV_OBJ_FLAG_CLICKABLE);
        s_sv_title = lv_label_create(s_sv);
        lv_obj_set_style_text_font(s_sv_title, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(s_sv_title, C_ACCENT_HI, 0);
        lv_obj_align(s_sv_title, LV_ALIGN_CENTER, 0, 52 - CY);
        s_sv_cap = lv_label_create(s_sv);
        lv_obj_set_style_text_font(s_sv_cap, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(s_sv_cap, C_TEXT2, 0);
        lv_obj_set_width(s_sv_cap, 250);
        lv_obj_set_style_text_align(s_sv_cap, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(s_sv_cap, LV_LABEL_LONG_DOT);
        lv_obj_align(s_sv_cap, LV_ALIGN_CENTER, 0, 304 - CY);
        /* Over a picture, on a pill of its own: grey on a picture could not
         * be read. */
        s_sv_wait = lv_label_create(s_sv);
        lv_obj_set_style_text_font(s_sv_wait, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(s_sv_wait, C_TEXT, 0);
        lv_obj_set_style_bg_color(s_sv_wait, C_BG1, 0);
        lv_obj_set_style_bg_opa(s_sv_wait, LV_OPA_90, 0);
        lv_obj_set_style_border_color(s_sv_wait, C_ACCENT, 0);
        lv_obj_set_style_border_width(s_sv_wait, 2, 0);
        lv_obj_set_style_radius(s_sv_wait, 14, 0);
        lv_obj_set_style_pad_hor(s_sv_wait, 16, 0);
        lv_obj_set_style_pad_ver(s_sv_wait, 8, 0);
        lv_obj_align(s_sv_wait, LV_ALIGN_CENTER, 0, 0);
    }
    s_sv_open = true;
    s_sv_idx  = 0;
    netinfo_show(false);
    lv_obj_add_flag(s_warn_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_sv_img, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_sv_cap, "");
    sv_title();
    lv_obj_remove_flag(s_sv, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_sv);
}

int ui_sstv_wanted(void) { return s_sv_open ? s_sv_idx : -1; }

void ui_sstv_show(const uint16_t *px, int w, int h, int idx, const char *title,
                  const char *caption, bool failed)
{
    if (!s_scr || !lvgl_port_lock(50)) return;
    /* Only the one the knob is on: one it has turned past is let go. */
    if (s_sv_open && idx == s_sv_idx) {
        lv_image_set_src(s_sv_img, NULL);
        lv_image_cache_drop(&s_sv_dsc);
        if (failed || !px || w <= 0 || h <= 0) {
            lv_obj_add_flag(s_sv_img, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(s_sv_wait, caption && caption[0] ? caption : "not to be had");
            lv_obj_remove_flag(s_sv_wait, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(s_sv_cap, "");
        } else {
            memset(&s_sv_dsc, 0, sizeof s_sv_dsc);
            s_sv_dsc.header.magic  = LV_IMAGE_HEADER_MAGIC;
            s_sv_dsc.header.cf     = LV_COLOR_FORMAT_RGB565;
            s_sv_dsc.header.w      = (uint32_t)w;
            s_sv_dsc.header.h      = (uint32_t)h;
            s_sv_dsc.header.stride = (uint32_t)w * 2;
            s_sv_dsc.data_size     = (uint32_t)(w * h * 2);
            s_sv_dsc.data          = (const uint8_t *)px;
            lv_image_set_src(s_sv_img, &s_sv_dsc);
            lv_obj_align(s_sv_img, LV_ALIGN_CENTER, 0, 0);
            lv_obj_set_style_image_opa(s_sv_img, LV_OPA_COVER, 0);
            lv_obj_remove_flag(s_sv_img, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_sv_wait, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(s_sv_cap, caption ? caption : "");
        }
        if (title && title[0]) lv_label_set_text(s_sv_title, title);
    }
    lvgl_port_unlock();
}

void ui_set_spots(const ui_spot_t *spots, uint8_t n)
{
    if (!lvgl_port_lock(20)) return;
    if (n > UI_SPOTS_MAX) n = UI_SPOTS_MAX;
    if (n && spots) memcpy(s_spots, spots, sizeof s_spots[0] * n);
    s_nspots = spots ? n : 0;
    lvgl_port_unlock();
}

/* A spot's two lines: its call -- or, for a voice nobody has named, its
 * frequency -- and under it where and what it is. */
static void spot_lines(const ui_spot_t *sp, char *l1, size_t n1, char *l2, size_t n2)
{
    char f[16], m[6];
    snprintf(f, sizeof f, "%lu.%03lu.%lu", (unsigned long)(sp->hz / 1000000),
             (unsigned long)(sp->hz / 1000 % 1000), (unsigned long)(sp->hz / 100 % 10));
    upcase(sp->mode, m, sizeof m);
    if (sp->call[0]) {
        snprintf(l1, n1, "%.11s", sp->call);
        snprintf(l2, n2, "%s %s  %.23s", f, m, sp->what);
    } else {
        snprintf(l1, n1, "%s", f);
        snprintf(l2, n2, "%s  %.23s", m, sp->what);
    }
}

/* A tap at p, the finger down for `held` ms. */
static void tap(lv_point_t p, uint32_t held)
{
    (void)held;
    /* The SSTV viewer: any tap, anywhere, and the dial is back. */
    if (s_sv_open) {
        sv_close();
        return;
    }
    /* An editor is open: ANY tap accepts. Commitment on the imprecise input,
     * selection on the precise one. */
    if (s_edit == ED_CHOICE) {
        /* A question that needs an answer: a tap on the panel gives it, a
         * tap anywhere else is ignored. */
        lv_area_t a;
        lv_obj_get_coords(s_edit_panel, &a);
        if (p.x < a.x1 || p.x > a.x2 || p.y < a.y1 || p.y > a.y2) return;
        s_ch_answer = s_edit_idx;
        edit_close();
        return;
    }
    if (s_edit != ED_NONE) {
        const edit_t was = s_edit;
        /* A live editor has done its work as it turned: any tap closes it --
         * but RF GAIN, tapped on its panel, goes on to POWER, as the swipe
         * down's receiver goes on to its antenna. */
        if (edit_live(was)) {
            /* ...as BALANCE, first while a web SDR is chosen, goes on to
             * RF GAIN. */
            if (was == ED_BALANCE && shown_at(s_edit_panel, p) &&
                s_last.has_levels && s_last.have_levels) {
                edit_open(ED_RFGAIN, &s_last);
                return;
            }
            if (was == ED_RFGAIN && shown_at(s_edit_panel, p)) {
                edit_open(ED_POWER, &s_last);
                return;
            }
            edit_close();
            return;
        }
        /* The panel is the button: a tap on it accepts, anywhere else closes
         * it untouched. */
        lv_area_t a;
        lv_obj_get_coords(s_edit_panel, &a);
        if (p.x < a.x1 || p.x > a.x2 || p.y < a.y1 || p.y > a.y2) { edit_close(); return; }
        /* The tuner's memories toggle in place: the menu stays, MEM lit or
         * dimmed as they now are. Each tap says which, not "the other way",
         * so two quick ones cannot leave the radio out of step. */
        if (was == ED_MENU && s_menu[s_edit_idx] == UI_ACT_MEM) {
            s_mem_lit    = !s_mem_lit;
            s_mem_tapped = lv_tick_get() | 1;
            memset(&s_commit, 0, sizeof s_commit);
            s_commit.action  = UI_ACT_MEM;
            s_commit.atu_mem = s_mem_lit;
            s_have_commit = true;
            edit_render();
            return;
        }
        /* SSTV's panel is a door: a tap on it opens the viewer. */
        if (was == ED_SSTV) {
            edit_close();
            if (s_last.n_sstv > 0) sv_open();
            return;
        }
        edit_commit();
        /* The swipe's editors come in a row: what is heard beside the radio
         * -- LOCAL or a web SDR -- then the radio's own receiver, then its
         * antenna. The radio plays on in the left ear with an SDR in the
         * right, so its choices follow either way. */
        if (was == ED_RXSRC && s_last.mem_state == UI_MEM_OFF) {
            if (s_last.n_rx > 1)  { edit_open(ED_RX, &s_last);  return; }
            if (s_last.n_ant)     { edit_open(ED_ANT, &s_last); return; }
        }
        if (was == ED_RX && s_last.n_ant) { edit_open(ED_ANT, &s_last); return; }
        /* ...and last, on a radio with memories, V/M. */
        if ((was == ED_RXSRC || was == ED_RX || was == ED_ANT) && s_last.has_memories)
            edit_open(ED_VM, &s_last);
        return;
    }

    /* The address card goes away with a tap on the card itself, and that tap
     * does nothing else. Like any other control on the face it answers only
     * to a tap on itself: a tap beside it goes to whatever is there. */
    if (s_netinfo && !lv_obj_has_flag(s_netinfo, LV_OBJ_FLAG_HIDDEN)) {
        lv_area_t a;
        lv_obj_get_coords(s_netinfo, &a);
        if (p.x >= a.x1 && p.x <= a.x2 && p.y >= a.y1 && p.y <= a.y2) {
            netinfo_show(false);
            return;
        }
    }

#if REFLECTOR_FACE
    /* The lock left of the talkgroup, the mute right of it: toggles, with or
     * without a link. */
    if (p.y >= 104 && p.y < 140) {
        if (p.x < CX - 38) { s_lock_tap = true; return; }
        if (p.x > CX + 38) { s_mute_tap = true; return; }
    }
#endif
    /* AGC left of the S-unit readout, the gain right of it. */
    if (aux_spot(p)) {
        edit_open(p.x < CX ? ED_AGC : ED_GAIN, &s_last);
        return;
    }

    /* The meter arc: a long press shows the addresses. */
    if (p.y < 104) {
        if (held >= NETINFO_HOLD_MS) netinfo_show(true);
        return;
    }

    /* With no link there is nothing behind any of these: opening an editor
     * would let the operator choose a mode or a filter that goes nowhere. The
     * warning panel is the only thing on screen that means anything, so leave
     * it alone -- the long press on the meter still works, since the
     * addresses are what you want. */
    if (!s_last.link_ok) return;

    const bool mem = s_last.mem_state != UI_MEM_OFF;
    /* band | mode | filter -- in memory mode the band's place holds the group.
     * A reflector's talkgroup is chosen with the dial, not an editor. */
    if (p.y >= 104 && p.y < 140) {
        if (REFLECTOR_FACE) return;
        if      (p.x < CX - 38) edit_open(mem ? ED_GROUP : ED_BAND, &s_last);
        else if (p.x > CX + 38) edit_open(ED_FILTER, &s_last);
        else                    edit_open(ED_MODE,   &s_last);
        return;
    }
    /* frequency digits -> step decade; a channel has no digits to pick */
    if (p.y >= 144 && p.y < 212) {
        if (mem || REFLECTOR_FACE) return;
        s_active_dig = nearest_digit(p.x);
        s_step_req   = DIG_STEP[s_active_dig];
        return;
    }
    /* A receiver's slab: all the spots and voices, on the dial. */
    if (RX_FACE && p.y >= PTT_TOP) {
        if (s_nspots > 0) edit_open(ED_SPOT, &s_last);
        return;
    }
    /* step | volume, on a receiver */
    if (RX_FACE && p.y >= 208 && p.y < PTT_TOP) {
        if (p.x > CX) edit_open(ED_VOL, &s_last);
        return;
    }
    /* step | rit | volume | mic */
    if (p.y >= 208 && p.y < PTT_TOP) {
        if      (p.x > CX + 74) edit_open(ED_MIC, &s_last);
        else if (p.x > CX + 12) edit_open(ED_VOL, &s_last);
        else if (p.x > CX - 56 && !REFLECTOR_FACE) edit_open(ED_RIT, &s_last);
        return;
    }
}

/* Still pressed. On the S-meter, as soon as the press is long enough, the
 * address card comes up and the motor clicks. With the addresses already
 * up, three seconds ask for the firmware picker, and they make way for the
 * question. Either once for the press, which is then no tap. */
static void pressing_cb(lv_event_t *e)
{
    (void)e;
    if (s_edit != ED_NONE || s_asking) return;
    const uint32_t held = lv_tick_elaps(s_pressed_at);
    if (s_press_card && held >= NETINFO_HOLD_MS) {
        s_press_card = false;
        /* Not for a finger on its way somewhere: a swipe, or a drag. */
        lv_point_t q = s_press_pt;
        lv_indev_t *indev = lv_indev_active();
        if (indev) lv_indev_get_point(indev, &q);
        if (s_gestured || LV_ABS(q.x - s_press_pt.x) > TAP_SLOP ||
            LV_ABS(q.y - s_press_pt.y) > TAP_SLOP) return;
        s_press_tap  = false;
        netinfo_show(true);
        s_card_shown = true;
        return;
    }
    if (!s_press_picker || held < PICKER_HOLD_MS) return;
    s_press_picker = false;
    s_press_tap    = false;
    netinfo_show(false);
    s_picker_req   = true;
}

static void release_cb(lv_event_t *e)
{
    (void)e;
    s_released_at = lv_tick_get();
    /* A long press is rare enough to log, and the touch controller cutting
     * one short shows only here. */
    const uint32_t held = lv_tick_elaps(s_pressed_at);
    if (held >= 2000)
        ESP_LOGI(TAG, "press at %d,%d%s released after %u ms",
                 (int)s_press_pt.x, (int)s_press_pt.y,
                 s_press_picker ? " (towards the picker)" : "", (unsigned)held);
    s_press_picker = false;
    s_press_card   = false;
    /* A press on the PTT slab in receive: a tap keys, a swipe does not. */
    if (s_ptt_armed) {
        s_ptt_armed = false;
        lv_point_t q = s_press_pt;
        lv_indev_t *indev = lv_indev_active();
        if (indev) lv_indev_get_point(indev, &q);
        if (s_gestured || LV_ABS(q.x - s_press_pt.x) > TAP_SLOP ||
            LV_ABS(q.y - s_press_pt.y) > TAP_SLOP) {
            ESP_LOGI(TAG, "PTT not keyed: a %s from the slab, %d,%d to %d,%d",
                     s_gestured ? "swipe" : "drag", (int)s_press_pt.x, (int)s_press_pt.y,
                     (int)q.x, (int)q.y);
            return;
        }
        s_ptt_tap = true;
        return;
    }
    const bool was_tap = s_press_tap && !s_gestured;
    s_press_tap = false;
    if (!was_tap) return;
    lv_indev_t *indev = lv_indev_active();
    if (indev) {
        lv_point_t q;
        lv_indev_get_point(indev, &q);
        if (LV_ABS(q.x - s_press_pt.x) > TAP_SLOP ||
            LV_ABS(q.y - s_press_pt.y) > TAP_SLOP) return;   /* a drag */
    }
    tap(s_press_pt, lv_tick_elaps(s_pressed_at));
}

/* A swipe. Down chooses what is heard: LOCAL or a web SDR, and then, on a
 * radio with a second receiver or a choice of antennas, their editors -- the
 * receiver, and a tap later the antenna -- and last, with memories, V/M. Up
 * chooses the radio. From the left, the levels; from the right, the tuner. */
static void gesture_cb(lv_event_t *e)
{
    (void)e;
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    s_gestured = true;
    const lv_dir_t dir = lv_indev_get_gesture_dir(indev);
    if (s_edit != ED_NONE || s_asking || s_last.tx || s_sv_open) return;
    /* Up: another radio, where the knob knows more than one -- with or
     * without a link, since the one in use may be switched off. */
    if (dir == LV_DIR_TOP) {
        if (s_last.n_radios > 1) edit_open(ED_RADIO, &s_last);
        return;
    }
    if (!s_last.link_ok) return;
    /* From the left: with a web SDR chosen, the balance first -- the one
     * turned most -- then RF gain and power. From the right: the tuner, or
     * the FlexRadio's TUNE/ATU/MEM. */
    if (dir == LV_DIR_RIGHT) {
        if (s_last.rxsrc >= 0)                       edit_open(ED_BALANCE, &s_last);
        else if (s_last.has_levels && s_last.have_levels) edit_open(ED_RFGAIN, &s_last);
        return;
    }
    if (dir == LV_DIR_LEFT) {
        /* A receiver's: its SSTV pictures, where it keeps them. */
        if (RX_FACE) {
            if (s_last.n_sstv >= 0) edit_open(ED_SSTV, &s_last);
            return;
        }
        if (s_last.has_tuner && s_last.have_tuner)  edit_open(ED_TUNER, &s_last);
        else if (s_last.has_tune || s_last.has_atu) edit_open(ED_MENU, &s_last);
        return;
    }
    if (dir != LV_DIR_BOTTOM) return;
    /* Down: the receiver -- LOCAL or a web SDR -- and after it, a second
     * receiver's and the antennas' choice (see tap()). */
#if VFO_HAS_SDR
    if (s_last.n_sdr > 0) {
        edit_open(ED_RXSRC, &s_last);
        return;
    }
#endif
    if (s_last.mem_state == UI_MEM_OFF && s_last.n_rx > 1) {
        edit_open(ED_RX, &s_last);
        return;
    }
    if (s_last.mem_state == UI_MEM_OFF && s_last.n_ant) {
        edit_open(ED_ANT, &s_last);
        return;
    }
    if (s_last.has_memories) edit_open(ED_VM, &s_last);
}

static void touch_cb(lv_event_t *e)
{
    (void)e;
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    ui_note_activity();
    s_press_pt   = p;
    s_pressed_at = lv_tick_get();
    s_press_tap  = false;
    s_gestured   = false;
    s_ptt_armed  = false;
    /* With the addresses up, on the S-meter or on them: see PICKER_HOLD_MS. */
    s_press_picker = addresses_up() &&
                     (p.y < 104 || shown_at(s_netinfo, p) || shown_at(s_warn_panel, p));
    /* Or, with them not up yet, on the S-meter: see NETINFO_HOLD_MS. */
    s_press_card = p.y < 104 && !s_press_picker && !aux_spot(p);

    /* A question is up: this tap answers it and goes nowhere else. On the
     * panel is yes; anywhere else is no -- the operator was reaching for
     * something, and PTT above all must not be keyed by answering. */
    if (s_asking) {
        lv_area_t a;
        lv_obj_get_coords(s_ask_panel, &a);
        const bool on = p.x >= a.x1 && p.x <= a.x2 && p.y >= a.y1 && p.y <= a.y2;
        /* The question appears over the most-touched part of the face, so a
         * tap already on its way when it popped up was not an answer. */
        if (on && lv_tick_elaps(s_ask_since) < ASK_ARM_MS) return;
        /* A question the knob answers comes up under a finger held on the
         * arc, and a finger lifting can flicker: a press straight after a
         * release is still that finger, not a no. */
        if (s_ask_turn && lv_tick_elaps(s_released_at) < PTT_REARM_MS) return;
        lv_obj_add_flag(s_ask_panel, LV_OBJ_FLAG_HIDDEN);
        s_asking = false;
        s_ptt_tap = false;
        /* Yes: the update screen at once. It is a separate screen, so PTT is
         * out of reach from this moment until the restart -- not whenever the
         * network task next looks. (The port lock is recursive.) A question
         * the knob answers takes only a no from a tap. */
        const bool yes = on && !s_ask_turn;
        if (yes) {
            if (s_ask_restarts) ui_updating_reboot();
            else                ui_updating_show();
        }
        s_ask_answer = yes ? 1 : -1;
        return;
    }

    /* PTT keeps acting on the press: the whole slab, with a link, and not
     * while an editor is open, when a tap there accepts the edit. With no
     * link the slab would arm a transmitter we cannot reach. */
    if (!RX_FACE && p.y >= PTT_TOP && s_edit == ED_NONE && s_last.link_ok) {
        /* One tap, one toggle. A light touch can flicker -- press, release,
         * press within a single tap -- and on a toggle each extra press
         * undoes the one before: keyed and unkeyed in one tap, which reads as
         * "PTT needs a hard press". So a press counts only once the finger
         * has been off the glass for a moment. Nothing else on the face
         * toggles, so nothing else needs this. */
        const uint32_t up = lv_tick_elaps(s_released_at);
        if (up < PTT_REARM_MS) {
            ESP_LOGI(TAG, "PTT press ignored: finger up only %u ms", (unsigned)up);
            return;
        }
        /* On the air, the press unkeys, at once. In receive it keys only
         * once the finger lifts without having moved: a swipe up begun on
         * the slab -- memory mode -- would otherwise key the transmitter on
         * its way, before anything could know it was a swipe. A tap's
         * length, a tenth of a second, is all keying waits. */
        if (s_last.tx || s_last.keyed) {
            s_ptt_tap = true;
            return;
        }
        /* A headset's button is the PTT while one is connected: the glass
         * only ever unkeys then, never keys. */
        if (s_last.headset) return;
        s_ptt_armed = true;
        return;
    }

    /* Everything else: see release_cb. */
    s_press_tap = true;
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
static lv_obj_t *mkgroup(void)
{
    lv_obj_t *g = lv_obj_create(s_scr);
    lv_obj_set_size(g, 360, 360);
    lv_obj_set_pos(g, 0, 0);
    lv_obj_set_style_bg_opa(g, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(g, 0, 0);
    lv_obj_set_style_pad_all(g, 0, 0);
    lv_obj_remove_flag(g, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(g, LV_OBJ_FLAG_CLICKABLE);
    return g;
}

static float swr_frac(float w)
{
    if (w <= 1.0f) return 0.0f;
    float f = (w - 1.0f) / 2.0f;
    return f > 1.0f ? 1.0f : f;
}

/* Scale for the transmit half: SWR ticks and labels on the right, and a label
 * for the audio meter on the left. Without a scale the SWR arc is just a
 * coloured bar, and 2.5 looks much like 1.5. */
static void add_tx_ticks(void)
{
    static const struct { float swr; const char *t; uint8_t kind; } T[] = {
        { 1.0f, "1",   0 }, { 1.5f, NULL, 0 }, { 2.0f, "2", 1 },
        { 2.5f, NULL, 2 }, { 3.0f, "3",  2 },
    };
    static lv_point_precise_t pts[5][2];

    for (size_t i = 0; i < sizeof T / sizeof T[0]; i++) {
        float a = (SWR_ROT + swr_frac(T[i].swr) * SWR_SPAN) * 3.14159265f / 180.0f;
        float c = cosf(a), sn = sinf(a);
        int r1 = ARC_R0 - 15, r0 = r1 - (T[i].t ? 10 : 6);
        pts[i][0].x = (lv_value_precise_t)(CX + r0 * c);
        pts[i][0].y = (lv_value_precise_t)(CY + r0 * sn);
        pts[i][1].x = (lv_value_precise_t)(CX + r1 * c);
        pts[i][1].y = (lv_value_precise_t)(CY + r1 * sn);

        lv_color_t col = T[i].kind == 2 ? C_DANGER
                       : T[i].kind == 1 ? C_WARN : C_LABEL;
        lv_obj_t *ln = lv_line_create(s_tx_ticks);
        lv_line_set_points(ln, pts[i], 2);
        lv_obj_set_style_line_width(ln, T[i].t ? 3 : 2, 0);
        lv_obj_set_style_line_color(ln, col, 0);
        lv_obj_set_style_line_rounded(ln, true, 0);

        if (T[i].t) {
            lv_obj_t *l = lv_label_create(s_tx_ticks);
            lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
            lv_obj_set_style_text_color(l, col, 0);
            lv_label_set_text(l, T[i].t);
            lv_obj_align(l, LV_ALIGN_CENTER,
                         (int)(128 * c), (int)(128 * sn));
        }
    }

    /* No "SWR" / "AUDIO" captions. At this diameter they land in the same
     * radial band as the tick marks and overlap them, and they are redundant:
     * only one half carries a numbered scale, and the centre already reads out
     * the SWR figure and the forward power in words. */
}

/* Created separately from the pegs because they belong on top of the power
 * arc, which does not exist yet when the pegs are made. */
static void add_pwr_notches(void)
{
    for (int i = 0; i < PWR_PEGS; i++) s_pwr_notch[i] = mknotch(s_tx_ticks);
    int r = s_pwr_range;
    s_pwr_range = -1;          /* force pwr_set_range to redo the geometry */
    pwr_set_range(r);
}

static void add_pwr_pegs(void)
{
    for (int i = 0; i < PWR_PEGS; i++) {
        s_pwr_tick[i] = lv_line_create(s_tx_ticks);
        lv_obj_set_style_line_width(s_pwr_tick[i], 2, 0);
        lv_obj_set_style_line_color(s_pwr_tick[i], C_LABEL, 0);
        lv_obj_set_style_line_rounded(s_pwr_tick[i], true, 0);

        s_pwr_lbl[i] = lv_label_create(s_tx_ticks);
        lv_obj_set_style_text_font(s_pwr_lbl[i], &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(s_pwr_lbl[i], C_LABEL, 0);
        lv_label_set_text(s_pwr_lbl[i], "");
    }
}

/* Cuts across the meter band at each S-unit boundary, in the background
 * colour. Static: on the filled part of the bar they are gaps, on the unfilled
 * track they disappear, so nothing has to be recoloured as the signal moves. */
/* One cut across a meter band, in the background colour, at a given angle.
 * Runs a pixel proud at both ends so no anti-aliased sliver joins two blocks. */
static void notch_points(float deg, lv_point_precise_t out[2])
{
    float a = deg * 3.14159265f / 180.0f;
    float c = cosf(a), sn = sinf(a);
    int r1 = ARC_R0 + 1, r0 = ARC_R0 - 13;
    out[0].x = (lv_value_precise_t)(CX + r0 * c);
    out[0].y = (lv_value_precise_t)(CY + r0 * sn);
    out[1].x = (lv_value_precise_t)(CX + r1 * c);
    out[1].y = (lv_value_precise_t)(CY + r1 * sn);
}

static lv_obj_t *mknotch(lv_obj_t *parent)
{
    lv_obj_t *ln = lv_line_create(parent);
    lv_obj_set_style_line_width(ln, 3, 0);
    lv_obj_set_style_line_color(ln, C_BG, 0);
    lv_obj_set_style_line_rounded(ln, false, 0);
    return ln;
}

static void add_rx_notches(void)
{
    static lv_point_precise_t pts[sizeof RXNOTCH / sizeof RXNOTCH[0]][2];

    for (size_t i = 0; i < sizeof RXNOTCH / sizeof RXNOTCH[0]; i++) {
        notch_points(ARC_ROT + smeter_frac(RXNOTCH[i]) * ARC_SPAN, pts[i]);
        lv_line_set_points(mknotch(s_rx_ticks), pts[i], 2);
    }
}

static void add_ticks(void)
{
    static const struct { float dbm; uint8_t len; uint8_t kind; } TICKS[] = {
#if VFO_RADIO_SVXCONNECT
        { -48, 6, 0 }, { -36, 6, 0 }, { -24, 6, 0 }, { -18, 6, 0 },
        { -12, 11, 1 },                        /* -12 dBFS -- where yellow starts */
        { -6, 6, 2 }, { -3, 6, 2 }, { 0, 9, 2 },
#else
        { -121, 6, 0 }, { -109, 6, 0 }, { -97, 6, 0 }, { -85, 6, 0 },
        { -73, 11, 1 },                                  /* S9 -- the landmark */
        { -53, 6, 2 }, { -33, 6, 2 }, { -13, 9, 2 },
#endif
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

        lv_obj_t *ln = lv_line_create(s_rx_ticks);
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
    /* The screen carries default padding, and lv_obj_set_pos() is relative to
     * the parent's CONTENT area -- so x=0 was not the left edge and a
     * full-width label was not centred. This is why the caption kept looking
     * off no matter how it was aligned. */
    lv_obj_set_style_pad_all(s_scr, 0, 0);
    lv_obj_set_style_border_width(s_scr, 0, 0);

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
    /* s_meter keeps the unfilled track; the coloured fill is the zone arcs
     * below, so its own indicator must not draw or the two would overlap. */
    lv_obj_set_style_arc_width(s_meter, 12, LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(s_meter, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(s_meter, false, LV_PART_MAIN);

    for (size_t z = 0; z < RX_ZONES; z++) {
        lv_obj_t *b = lv_arc_create(s_scr);
        lv_obj_set_size(b, ARC_R0 * 2, ARC_R0 * 2);
        lv_obj_center(b);
        float zlo = RXZONES[z].from, zhi = RXZONES[z].to;
        int a0 = (int)(smeter_frac(zlo) * ARC_SPAN);
        int a1 = (int)(smeter_frac(zhi) * ARC_SPAN);
        lv_arc_set_rotation(b, ARC_ROT + a0);
        lv_arc_set_bg_angles(b, 0, a1 - a0);
        lv_arc_set_range(b, 0, 1000);
        lv_arc_set_value(b, 0);
        lv_obj_remove_style(b, NULL, LV_PART_KNOB);
        lv_obj_remove_flag(b, LV_OBJ_FLAG_CLICKABLE);
        /* No background of its own: s_meter already draws the track. */
        lv_obj_set_style_arc_opa(b, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_arc_width(b, 12, LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(b, lv_color_hex(RXZONES[z].rgb),
                                   LV_PART_INDICATOR);
        /* Square ends: rounded caps make neighbouring blocks overlap into
         * lozenges and round off the leading edge of the bar. */
        lv_obj_set_style_arc_rounded(b, false, LV_PART_INDICATOR);
        lv_obj_set_style_arc_rounded(b, false, LV_PART_MAIN);
        s_rx_zone[z] = b;
        s_rx_val[z]  = 0;
    }
#if VFO_HAS_SDR
    /* A web SDR's S-meter: a thin line just outside the radio's, on its
     * scale, in the SDR's blue -- clear of the notches and the ticks, and
     * over the hairline, which only shows in transmit, when this does not. */
    s_sdr_arc = lv_arc_create(s_scr);
    lv_obj_set_size(s_sdr_arc, SDR_R * 2, SDR_R * 2);
    lv_obj_center(s_sdr_arc);
    lv_arc_set_rotation(s_sdr_arc, ARC_ROT);
    lv_arc_set_bg_angles(s_sdr_arc, 0, ARC_SPAN);
    lv_arc_set_range(s_sdr_arc, 0, 1000);
    lv_arc_set_value(s_sdr_arc, 0);
    lv_obj_remove_style(s_sdr_arc, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(s_sdr_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(s_sdr_arc, 4, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_sdr_arc, C_SUBTLE, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_sdr_arc, 4, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_sdr_arc, C_SDR, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(s_sdr_arc, false, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(s_sdr_arc, false, LV_PART_INDICATOR);
    lv_obj_add_flag(s_sdr_arc, LV_OBJ_FLAG_HIDDEN);
#endif
    s_rx_ticks = mkgroup();
    s_tx_ticks = mkgroup();
    add_rx_notches();
    add_ticks();
    add_tx_ticks();
    add_pwr_pegs();
    pwr_set_range(0);
    lv_obj_add_flag(s_tx_ticks, LV_OBJ_FLAG_HIDDEN);

    /* In transmit the arc changes meaning entirely: MIC level across the left
     * half, SWR across the right. Two separate arcs rather than one repurposed
     * one, so the split is visible at a glance and neither has to share a
     * scale with the other. */
    /* Forward power takes the right-hand outer arc. Auto-ranged, because this
     * has to read sensibly at 5 W and at 2.5 kW without the operator picking a
     * scale. */
    s_pwr_arc = lv_arc_create(s_scr);
    lv_obj_set_size(s_pwr_arc, ARC_R0 * 2, ARC_R0 * 2);
    lv_obj_center(s_pwr_arc);
    lv_arc_set_rotation(s_pwr_arc, AUD_ROT);
    lv_arc_set_bg_angles(s_pwr_arc, 0, AUD_SPAN);
    lv_arc_set_range(s_pwr_arc, 0, 1000);
    lv_arc_set_value(s_pwr_arc, 0);
    lv_obj_remove_style(s_pwr_arc, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(s_pwr_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(s_pwr_arc, 12, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_pwr_arc, C_SUBTLE, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(s_pwr_arc, false, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(s_pwr_arc, false, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(s_pwr_arc, 12, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_pwr_arc, C_BRAND, LV_PART_INDICATOR);
    lv_obj_add_flag(s_pwr_arc, LV_OBJ_FLAG_HIDDEN);

    /* Mic level: a thin inner ring. Demoted deliberately -- it is a nice-to-
     * have next to SWR and power, and it should not compete with them. Built
     * like the SWR meter, from arcs laid end to end that fill independently,
     * so the bar runs green, then amber, then red.
     *
     * Fills from the bottom end upward, opposite to the power bar above it.
     * Two bars growing the same way on the same side invite being read as one
     * quantity. -40 dB is the bottom end, +10 dB the top. */
    for (size_t z = 0; z < MIC_ZONES; z++) {
        lv_obj_t *b = lv_arc_create(s_scr);
        lv_obj_set_size(b, (ARC_R0 - 22) * 2, (ARC_R0 - 22) * 2);
        lv_obj_center(b);
        int a0 = (int)((1.0f - mic_frac(MIC_ZONE[z].to))   * AUD_SPAN + 0.5f);
        int a1 = (int)((1.0f - mic_frac(MIC_ZONE[z].from)) * AUD_SPAN + 0.5f);
        lv_arc_set_rotation(b, AUD_ROT + a0);
        lv_arc_set_bg_angles(b, 0, a1 - a0);
        lv_arc_set_range(b, 0, 1000);
        lv_arc_set_value(b, 0);
        lv_arc_set_mode(b, LV_ARC_MODE_REVERSE);
        lv_obj_remove_style(b, NULL, LV_PART_KNOB);
        lv_obj_remove_flag(b, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_arc_width(b, 5, LV_PART_MAIN);
        lv_obj_set_style_arc_color(b, C_SUBTLE, LV_PART_MAIN);
        lv_obj_set_style_arc_width(b, 5, LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(b, lv_color_hex(MIC_ZONE[z].rgb), LV_PART_INDICATOR);
        lv_obj_set_style_arc_rounded(b, false, LV_PART_INDICATOR);
        lv_obj_set_style_arc_rounded(b, false, LV_PART_MAIN);
        lv_obj_add_flag(b, LV_OBJ_FLAG_HIDDEN);
        s_mic_zone[z] = b;
    }

    /* Three arcs laid end to end, each filling independently. The bar is
     * therefore green up to 2.0, continues amber to 2.5 and red beyond --
     * rather than one bar that changes colour all at once. Reading it is then
     * a glance at how far into the red it has gone, not a colour lookup. */
    for (size_t z = 0; z < SWR_ZONES; z++) {
        lv_obj_t *b = lv_arc_create(s_scr);
        lv_obj_set_size(b, ARC_R0 * 2, ARC_R0 * 2);
        lv_obj_center(b);
        int a0 = (int)(swr_frac(ZONES[z].from) * SWR_SPAN);
        int a1 = (int)(swr_frac(ZONES[z].to)   * SWR_SPAN);
        lv_arc_set_rotation(b, SWR_ROT + a0);
        lv_arc_set_bg_angles(b, 0, a1 - a0);
        lv_arc_set_range(b, 0, 1000);
        lv_arc_set_value(b, 0);
        lv_obj_remove_style(b, NULL, LV_PART_KNOB);
        lv_obj_remove_flag(b, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_arc_width(b, 12, LV_PART_MAIN);
        lv_obj_set_style_arc_color(b, C_SUBTLE, LV_PART_MAIN);
        lv_obj_set_style_arc_width(b, 12, LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(b, lv_color_hex(ZONES[z].rgb), LV_PART_INDICATOR);
        lv_obj_set_style_arc_rounded(b, false, LV_PART_INDICATOR);
        lv_obj_set_style_arc_rounded(b, false, LV_PART_MAIN);
        lv_obj_add_flag(b, LV_OBJ_FLAG_HIDDEN);
        s_swr_zone[z] = b;
    }

    /* Peak LEDs: above the bars they mark, below the notches that cut every
     * band into blocks (both tick groups are lifted after this). */
    {
        uint32_t rx[RX_ZONES], mic[MIC_ZONES], swr[SWR_ZONES];
        for (size_t z = 0; z < RX_ZONES; z++)  rx[z]  = RXZONES[z].rgb;
        for (size_t z = 0; z < MIC_ZONES; z++) mic[z] = MIC_ZONE[z].rgb;
        for (size_t z = 0; z < SWR_ZONES; z++) swr[z] = ZONES[z].rgb;
        static const uint32_t pwr[1] = { PWR_HEX };             /* the bar's */
        led_build(&s_sig_led, ARC_ROT, ARC_SPAN, ARC_R0, 12, false, rx, RX_ZONES);
        led_build(&s_swr_led, SWR_ROT, SWR_SPAN, ARC_R0, 12, false, swr, SWR_ZONES);
        led_build(&s_pwr_led, AUD_ROT, AUD_SPAN, ARC_R0, 12, false, pwr, 1);
        led_build(&s_mic_led, AUD_ROT, AUD_SPAN, ARC_R0 - 22, 5, true, mic, MIC_ZONES);
        led_show(&s_swr_led, false);
        led_show(&s_pwr_led, false);
        led_show(&s_mic_led, false);
#if VFO_HAS_SDR
        /* The web SDR's line holds its peak the same way. */
        static const uint32_t sdr[1] = { SDR_HEX };
        led_build(&s_sdr_led, ARC_ROT, ARC_SPAN, SDR_R, 4, false, sdr, 1);
        led_show(&s_sdr_led, false);
#endif
    }

    /* Notch the SWR band at each printed mark, exactly as the receive meter is
     * notched. 1.0 and 3.0 are the ends of the arc and are left alone.
     *
     * These are created after the transmit arcs, and s_tx_ticks was created
     * before them, so the group has to be lifted or the notches would be drawn
     * underneath the bar they are supposed to cut. */
    {
        static const float SWRNOTCH[] = { 1.5f, 2.0f, 2.5f };
        static lv_point_precise_t np[sizeof SWRNOTCH / sizeof SWRNOTCH[0]][2];
        for (size_t i = 0; i < sizeof SWRNOTCH / sizeof SWRNOTCH[0]; i++) {
            notch_points(SWR_ROT + swr_frac(SWRNOTCH[i]) * SWR_SPAN, np[i]);
            lv_line_set_points(mknotch(s_tx_ticks), np[i], 2);
        }
    }
    add_pwr_notches();
    lv_obj_move_foreground(s_rx_ticks);
    lv_obj_move_foreground(s_tx_ticks);

    /* Signal, as a number as well as an arc: an arc shows trend, a number
     * lets you report a readable signal report. */
    s_srd  = mklabel(&lv_font_montserrat_20, C_TEXT,  CX, 76,  "S0");
    s_dbm  = mklabel(&lv_font_montserrat_14, C_LABEL, CX, 98,  "-127 dBm");

    /* Either side of it, a caption and the setting under it: the AGC, and
     * the front end's gain. Tapping either opens its editor. */
    s_agc_cap  = mklabel(&lv_font_montserrat_14, C_LABEL, CX - AUX_DX, 78, RX_FACE ? "SNR" : "AGC");
    s_agc_val  = mklabel(&lv_font_montserrat_14, C_DISABLED, CX - AUX_DX, 97, "--");
    s_gain_cap = mklabel(&lv_font_montserrat_14, C_DISABLED, CX + AUX_DX, 78, GAIN_CAPTION);
    s_gain_val = mklabel(&lv_font_montserrat_14, C_DISABLED, CX + AUX_DX, 97, "--");
#if REFLECTOR_FACE
    /* A reflector has no AGC or gain: a lock for the talkgroup and a mute for
     * its audio take their places, each toggled by a tap. */
    lv_obj_add_flag(s_agc_cap, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_agc_val, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_gain_cap, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_gain_val, LV_OBJ_FLAG_HIDDEN);
    /* In the talkgroup's row, where a radio's band and filter are: the line
     * above belongs to the talker, whose callsign needs all of its width. */
    s_lock_icon = mklabel(&font_svx_icons_24, C_LABEL, CX - 76, 122, SYM_UNLOCK);
    s_mute_icon = mklabel(&font_svx_icons_24, C_LABEL, CX + 76, 122, SYM_SOUND);
#endif

    s_band = mklabel(&lv_font_montserrat_20, C_ACCENT, CX - 76, 122, "--");
    s_mode = mklabel(&lv_font_montserrat_20, C_TEXT,   CX,      122, "USB");
    s_filt = mklabel(&lv_font_montserrat_20, C_TEXT2,  CX + 76, 122, "0");

    const int PITCH = 33, SMALL = 28, SEPW = 11;
    int total = 6 * PITCH + 2 * SMALL + 2 * SEPW;
    int x = CX - total / 2;
    int sep = 0;
    for (int i = 0; i < N_DIG; i++) {
        bool small = (i >= 6);
        int w = small ? SMALL : PITCH;
        s_dig_x[i] = x + w / 2;
        /* The 100 Hz and 10 Hz digits were montserrat_28 AND dimmed, which
         * together made them unreadable. Same size as the rest now; only the
         * colour marks them as below the tuning step. */
        s_dig[i] = mklabel(&lv_font_montserrat_48,
                           C_TEXT, s_dig_x[i], 170, "0");
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

    /* The memory face, in the digits' place. A fixed width, so a sixteen-
     * character name ends in dots rather than off the glass. */
    s_mem_big = mklabel(&lv_font_montserrat_28, C_TEXT, CX, 160, "");
    lv_obj_set_width(s_mem_big, 300);
    /* Portals name talkgroups like "145.450 ON0ORA-S Simplex Club Opwijk":
     * on the reflector face a name too long for the line goes round in a
     * loop. A memory's name is at most 16 characters, and fits. */
    lv_label_set_long_mode(s_mem_big, REFLECTOR_FACE ? LV_LABEL_LONG_SCROLL_CIRCULAR
                                                     : LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(s_mem_big, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_mem_big, LV_ALIGN_CENTER, 0, 160 - CY);
    s_mem_small = mklabel(&lv_font_montserrat_20, C_TEXT2, CX, 196, "");
    lv_obj_add_flag(s_mem_big, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_mem_small, LV_OBJ_FLAG_HIDDEN);

    s_step_lbl = mklabel(&lv_font_montserrat_20, C_ACCENT, CX - 98, 220, "1 kHz");
    s_rit      = mklabel(&lv_font_montserrat_14, C_WARN,   CX - 24, 222, "RIT 0");
    if (REFLECTOR_FACE) {
        lv_obj_add_flag(s_rit, LV_OBJ_FLAG_HIDDEN);         /* no RIT on a reflector */
        /* The link's state, where the step is: moved in towards the middle,
         * into the room the RIT leaves, so it does not hang off the edge. */
        lv_obj_align(s_step_lbl, LV_ALIGN_CENTER, -56, 220 - CY);
    }
    s_vol      = mklabel(&lv_font_montserrat_14, C_TEXT2,  CX + 42, 222,
                         LV_SYMBOL_VOLUME_MID " 40");
    s_mic      = mklabel(&font_mic_14,           C_TEXT2,  CX + 104, 222,
                         SYM_MIC " 100");
    if (RX_FACE) {
        /* A receiver has no RIT to set and no microphone: the step and the
         * volume share the row. */
        lv_obj_add_flag(s_rit, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_mic, LV_OBJ_FLAG_HIDDEN);
        lv_obj_align(s_step_lbl, LV_ALIGN_CENTER, -56, 220 - CY);
        lv_obj_align(s_vol, LV_ALIGN_CENTER, 56, 222 - CY);
    }

    /* Full width, hard to the bottom edge. The circle clips it to a chord.
     */
    s_ptt = lv_obj_create(s_scr);
    lv_obj_set_size(s_ptt, PTT_RIGHT - PTT_LEFT, 360 - PTT_TOP);
    lv_obj_set_pos(s_ptt, PTT_LEFT, PTT_TOP);
    lv_obj_set_style_radius(s_ptt, 0, 0);
    lv_obj_set_style_bg_color(s_ptt, C_BG1, 0);
    lv_obj_set_style_border_width(s_ptt, 2, 0);
    lv_obj_set_style_border_side(s_ptt, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_color(s_ptt, C_ACCENT, 0);
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
    /* Absolute position and an explicit full width. Auto-sized labels centre
     * on their own content, which shifts as the text changes between "PTT",
     * "----" and "TX 118" -- so the caption appeared to wander. */
    lv_obj_set_width(s_ptt_lbl, PTT_RIGHT - PTT_LEFT);
    lv_obj_set_style_text_align(s_ptt_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_all(s_ptt_lbl, 0, 0);
    lv_obj_set_pos(s_ptt_lbl, PTT_LEFT, PTT_TOP + 14);
    lv_obj_remove_flag(s_ptt_lbl, LV_OBJ_FLAG_CLICKABLE);
    if (!RX_FACE) {
        /* A Bluetooth headset: its button is the PTT then, and the slab is
         * the headset's -- its name, and under it its microphone, struck
         * through in red while it is muted. */
        s_hs_name = lv_label_create(s_scr);
        lv_obj_set_style_text_font(s_hs_name, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(s_hs_name, C_TEXT, 0);
        lv_obj_set_style_text_align(s_hs_name, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(s_hs_name, LV_LABEL_LONG_DOT);
        lv_obj_set_width(s_hs_name, 280);
        lv_obj_set_pos(s_hs_name, CX - 140, PTT_TOP + 12);
        lv_obj_remove_flag(s_hs_name, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(s_hs_name, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_hs_name, "");
        s_hs_mic = lv_label_create(s_scr);
        lv_obj_set_style_text_font(s_hs_mic, &font_btmic_28, 0);
        lv_obj_set_style_text_color(s_hs_mic, C_TEXT2, 0);
        lv_obj_set_style_text_align(s_hs_mic, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(s_hs_mic, 60);
        lv_obj_set_pos(s_hs_mic, CX - 30, PTT_TOP + 44);
        lv_obj_remove_flag(s_hs_mic, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(s_hs_mic, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_hs_mic, SYM_HS_MIC);
        /* The boom arm as the PTT, and the headset come with it down: a red
         * button asks for it up -- nothing keys until it has been. */
        s_hs_raise = lv_label_create(s_scr);
        lv_obj_set_style_text_font(s_hs_raise, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(s_hs_raise, lv_color_white(), 0);
        lv_obj_set_style_bg_color(s_hs_raise, C_TX_RED, 0);
        lv_obj_set_style_bg_opa(s_hs_raise, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(s_hs_raise, 18, 0);
        lv_obj_set_style_pad_hor(s_hs_raise, 18, 0);
        lv_obj_set_style_pad_ver(s_hs_raise, 6, 0);
        lv_label_set_text(s_hs_raise, "RAISE BOOM");
        lv_obj_align(s_hs_raise, LV_ALIGN_TOP_MID, 0, PTT_TOP + 42);
        lv_obj_remove_flag(s_hs_raise, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(s_hs_raise, LV_OBJ_FLAG_HIDDEN);
    }
    if (RX_FACE) {
        /* No PTT: the spot or voice nearest the dial, large, where PTT's
         * caption is; where and what it is under it; and how many there are
         * on the band in the narrow chord at the bottom. */
        lv_obj_set_style_text_color(s_ptt_lbl, C_TEXT, 0);
        lv_label_set_long_mode(s_ptt_lbl, LV_LABEL_LONG_DOT);
        lv_obj_set_width(s_ptt_lbl, 290);
        lv_obj_set_pos(s_ptt_lbl, CX - 145, PTT_TOP + 8);
        lv_label_set_text(s_ptt_lbl, "");
        s_spot_sub = lv_label_create(s_scr);
        lv_obj_set_style_text_font(s_spot_sub, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(s_spot_sub, C_TEXT2, 0);
        lv_obj_set_style_text_align(s_spot_sub, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(s_spot_sub, LV_LABEL_LONG_DOT);
        lv_obj_set_width(s_spot_sub, 246);
        lv_obj_set_pos(s_spot_sub, CX - 123, PTT_TOP + 44);
        lv_obj_remove_flag(s_spot_sub, LV_OBJ_FLAG_CLICKABLE);
        lv_label_set_text(s_spot_sub, "");
        s_spot_n = lv_label_create(s_scr);
        lv_obj_set_style_text_font(s_spot_n, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(s_spot_n, C_LABEL, 0);
        lv_obj_set_style_text_align(s_spot_n, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(s_spot_n, 190);
        lv_obj_set_pos(s_spot_n, CX - 95, PTT_TOP + 64);
        lv_obj_remove_flag(s_spot_n, LV_OBJ_FLAG_CLICKABLE);
        lv_label_set_text(s_spot_n, "");
        /* A Bluetooth headset connected: only its logo, at the right end of
         * the slab, level with the spot -- the spots keep the slab. */
        s_hs_bt = lv_label_create(s_scr);
        lv_obj_set_style_text_font(s_hs_bt, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(s_hs_bt, C_ACCENT, 0);
        lv_obj_set_pos(s_hs_bt, CX + 126, PTT_TOP + 12);
        lv_obj_remove_flag(s_hs_bt, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(s_hs_bt, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_hs_bt, LV_SYMBOL_BLUETOOTH);
    }

    /* Editor overlay: hidden until a field is tapped. */
    /* Network address card. Same treatment as the editor panel, and equally
     * not clickable -- the tap that dismisses it lands on the screen. */
    s_netinfo = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_netinfo, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_netinfo, C_ACCENT_HI, 0);
    lv_obj_set_style_text_align(s_netinfo, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_bg_color(s_netinfo, C_BG1, 0);
    lv_obj_set_style_bg_opa(s_netinfo, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_netinfo, C_ACCENT, 0);
    lv_obj_set_style_border_width(s_netinfo, 2, 0);
    lv_obj_set_style_radius(s_netinfo, 14, 0);
    lv_obj_set_style_pad_all(s_netinfo, 12, 0);
    lv_obj_align(s_netinfo, LV_ALIGN_CENTER, 0, -6);
    lv_obj_remove_flag(s_netinfo, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_netinfo, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_netinfo, s_netinfo_text);

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

    /* AetherSDR owns the band plan. When it refuses a key -- out of band, a
     * locked slice, TX disabled -- the protocol gives no reason, only a
     * trx:false. This banner and the refusal haptic are the whole of the
     * operator's feedback, so they have to be unmissable. */
    /* One warning, in the same shape as the editors -- an operator already
     * reads a panel in the middle of the dial as the device saying something
     * -- but in the danger colour. It used to be said twice, as a label over
     * the readout and again under it, which is worse than saying it once:
     * two copies of "NO LINK" invite a look for two different faults. */
    /* Tall enough for the card's four lines -- the firmware, then the
     * addresses -- under the warning. */
    s_warn_panel = lv_obj_create(s_scr);
    lv_obj_set_size(s_warn_panel, 268, 134);
    lv_obj_align(s_warn_panel, LV_ALIGN_CENTER, 0, -6);
    lv_obj_set_style_radius(s_warn_panel, 18, 0);
    lv_obj_set_style_bg_color(s_warn_panel, C_BG1, 0);
    lv_obj_set_style_bg_opa(s_warn_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_warn_panel, C_DANGER, 0);
    lv_obj_set_style_border_width(s_warn_panel, 2, 0);
    lv_obj_set_style_pad_all(s_warn_panel, 0, 0);
    lv_obj_remove_flag(s_warn_panel, LV_OBJ_FLAG_SCROLLABLE);
    /* Not clickable, for the same reason the editor panel is not: a tap
     * anywhere must still reach the screen handler. */
    lv_obj_remove_flag(s_warn_panel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_warn_panel, LV_OBJ_FLAG_HIDDEN);

    s_warn = lv_label_create(s_warn_panel);
    lv_obj_set_style_text_font(s_warn, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(s_warn, C_DANGER, 0);
    lv_label_set_text(s_warn, "");
    lv_obj_align(s_warn, LV_ALIGN_TOP_MID, 0, 8);

    /* The addresses live inside the warning, not on a card behind it. With no
     * link they are the single most useful thing on the screen -- they are how
     * you reach the configuration page to fix whatever is wrong -- and having
     * them peek out from under the panel was worse than not showing them. */
    s_warn_net = lv_label_create(s_warn_panel);
    lv_obj_set_style_text_font(s_warn_net, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_warn_net, C_TEXT2, 0);
    lv_obj_set_style_text_align(s_warn_net, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_warn_net, "");
    lv_obj_align(s_warn_net, LV_ALIGN_BOTTOM_MID, 0, -8);

    /* The update question: the same panel as the warning, in the accent
     * colour -- it is an offer, not a fault. */
    s_ask_panel = lv_obj_create(s_scr);
    lv_obj_set_size(s_ask_panel, 268, 116);
    lv_obj_align(s_ask_panel, LV_ALIGN_CENTER, 0, -6);
    lv_obj_set_style_radius(s_ask_panel, 18, 0);
    lv_obj_set_style_bg_color(s_ask_panel, C_BG1, 0);
    lv_obj_set_style_bg_opa(s_ask_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_ask_panel, C_ACCENT, 0);
    lv_obj_set_style_border_width(s_ask_panel, 2, 0);
    lv_obj_set_style_pad_all(s_ask_panel, 0, 0);
    lv_obj_remove_flag(s_ask_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_ask_panel, LV_OBJ_FLAG_CLICKABLE);   /* screen handler */
    lv_obj_add_flag(s_ask_panel, LV_OBJ_FLAG_HIDDEN);

    s_ask_title = lv_label_create(s_ask_panel);
    lv_obj_set_style_text_font(s_ask_title, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(s_ask_title, C_ACCENT_HI, 0);
    lv_label_set_text(s_ask_title, "");

    s_ask_hint = lv_label_create(s_ask_panel);
    lv_obj_set_style_text_font(s_ask_hint, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_ask_hint, C_TEXT2, 0);
    lv_obj_set_style_text_align(s_ask_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_ask_hint, "");

    lv_obj_add_event_cb(s_scr, touch_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_scr, pressing_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(s_scr, release_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(s_scr, gesture_cb, LV_EVENT_GESTURE, NULL);
    lv_obj_add_flag(s_scr, LV_OBJ_FLAG_CLICKABLE);
}

bool ui_ask_update(const char *version, const char *running, bool restart_first)
{
    if (!s_scr || !version) return false;
    if (!lvgl_port_lock(200)) return false;
    /* Tags may or may not carry a "v"; the dial shows the number. */
    if (*version == 'v') version++;
    if (running && *running == 'v') running++;
    lv_label_set_text_fmt(s_ask_title, "UPDATE %s", version);
    lv_obj_align(s_ask_title, LV_ALIGN_TOP_MID, 0, 16);
    lv_label_set_text_fmt(s_ask_hint, "tap here to install\nnow running %s",
                          running ? running : "?");
    lv_obj_align(s_ask_hint, LV_ALIGN_BOTTOM_MID, 0, -14);
    lv_obj_add_flag(s_warn_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_ask_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_ask_panel);
    s_ask_restarts = restart_first;
    s_ask_turn   = false;
    s_ask_knob   = false;
    s_ask_answer = 0;
    s_ask_since  = lv_tick_get();
    s_asking     = true;
    lvgl_port_unlock();
    ui_note_activity();              /* a dimmed dial would hide the question */
    return true;
}

bool ui_ask_turn(const char *title, const char *hint)
{
    if (!s_scr || !title) return false;
    if (!lvgl_port_lock(200)) return false;
    lv_label_set_text(s_ask_title, title);
    lv_obj_align(s_ask_title, LV_ALIGN_TOP_MID, 0, 16);
    lv_label_set_text(s_ask_hint, hint ? hint : "turn the knob for yes");
    lv_obj_align(s_ask_hint, LV_ALIGN_BOTTOM_MID, 0, -14);
    lv_obj_add_flag(s_warn_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_ask_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_ask_panel);
    s_ask_turn   = true;
    s_ask_knob   = false;
    s_ask_answer = 0;
    s_ask_since  = lv_tick_get();
    s_asking     = true;
    lvgl_port_unlock();
    ui_note_activity();
    return true;
}

bool ui_take_card_shown(void)
{
    const bool r = s_card_shown;
    s_card_shown = false;
    return r;
}

bool ui_take_picker_request(void)
{
    const bool r = s_picker_req;
    s_picker_req = false;
    return r;
}

/* --- the setup firmware's screen ------------------------------------------ */

static lv_obj_t *s_setup, *s_setup_title, *s_setup_text;

void ui_setup_show(const char *title, const char *text)
{
    if (!s_scr || !lvgl_port_lock(200)) return;
    if (!s_setup) {
        /* Over the whole face: the setup firmware has no radio to show. */
        s_setup = lv_obj_create(s_scr);
        lv_obj_set_size(s_setup, 360, 360);
        lv_obj_center(s_setup);
        lv_obj_set_style_bg_color(s_setup, C_BG, 0);
        lv_obj_set_style_bg_opa(s_setup, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(s_setup, 0, 0);
        lv_obj_set_style_radius(s_setup, 0, 0);
        lv_obj_remove_flag(s_setup, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(s_setup, LV_OBJ_FLAG_CLICKABLE);
        s_setup_title = lv_label_create(s_setup);
        lv_obj_set_style_text_font(s_setup_title, &lv_font_montserrat_28, 0);
        lv_obj_set_style_text_color(s_setup_title, C_ACCENT, 0);
        s_setup_text = lv_label_create(s_setup);
        lv_obj_set_width(s_setup_text, 280);
        lv_label_set_long_mode(s_setup_text, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_font(s_setup_text, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(s_setup_text, C_TEXT, 0);
        lv_obj_set_style_text_align(s_setup_text, LV_TEXT_ALIGN_CENTER, 0);
    }
    lv_label_set_text(s_setup_title, title ? title : "");
    lv_label_set_text(s_setup_text, text ? text : "");
    /* The title above where a chooser's panel comes (its top is 108 px down),
     * and the text under that panel while one is up, or it is hidden. */
    lv_obj_align(s_setup_title, LV_ALIGN_CENTER, 0, -110);
    const bool panel = s_edit != ED_NONE || s_asking;
    lv_obj_align(s_setup_text, LV_ALIGN_CENTER, 0, panel ? 96 : 20);
    lv_obj_remove_flag(s_setup, LV_OBJ_FLAG_HIDDEN);
    /* Under a question or an editor, if one is up. */
    lv_obj_move_foreground(s_setup);
    if (s_edit != ED_NONE) lv_obj_move_foreground(s_edit_panel);
    if (s_asking) lv_obj_move_foreground(s_ask_panel);
    lvgl_port_unlock();
    ui_note_activity();
}

int ui_take_update_answer(void)
{
    const int a = s_ask_answer;
    if (a) s_ask_answer = 0;
    return a;
}

bool ui_ask_knob_moved(void)
{
    if (s_asking && s_ask_turn) {
        /* Not in its first moments: that turn began before it was asked. */
        if (lv_tick_elaps(s_ask_since) >= ASK_ARM_MS) {
            s_turned_at = lv_tick_get();
            s_ask_knob  = true;
        }
        return true;
    }
    if (s_asking) s_ask_knob = true;
    return s_turned_at && lv_tick_elaps(s_turned_at) < TURN_SPENT_MS;
}

esp_err_t ui_init(void)
{
    lvgl_port_cfg_t pc = ESP_LVGL_PORT_INIT_CONFIG();
    /* Core 1, with the rest of the display and input work, as sdkconfig
     * intends: lwIP is quarantined on core 0. Left to itself LVGL ran on core
     * 0, beside a WebSocket task that takes 40-60% of it just receiving audio
     * -- measured at 2-7% idle while transmitting, with core 1 90% idle.
     * The display's SPI interrupt is on the same core, and has to be: see
     * PANEL_CORE. */
    pc.task_affinity = PANEL_CORE;
    ESP_RETURN_ON_ERROR(lvgl_port_init(&pc), TAG, "lvgl port");

    lvgl_port_display_cfg_t dc = {
        .io_handle     = panel_io_handle(),
        .panel_handle  = panel_handle(),
        /* 2 x 12 lines of internal DMA memory (17 kB). Every bound here was
         * MEASURED on hardware, not chosen:
         *   40 lines                        -> WebSocket task could not spawn
         *   24 lines                        -> TCP connects timed out
         *   32 lines + WiFi buffers in PSRAM-> WiFi could not init its static
         *                                      RX descriptors
         *   16 lines + microphone       -> WebSocket client could not allocate
         * 12 lines is what fits once the display, WiFi, BOTH I2S channels and
         * the TCI client are all resident.
         * Partial rendering means buffer height costs latency only on large
         * redraws, and the readout is per-digit precisely so those are rare.
         * Never a full framebuffer; never PSRAM for flush buffers. */
        .buffer_size   = BOARD_LCD_H_RES * 12,
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
    /* Raw lv_* calls must hold the port lock. lvgl_port_init() has already
     * started the LVGL task by this point, so rotating unguarded races its
     * render pass: main spins forever inside lv_inv_area() and the task
     * watchdog reboots the device before the UI is ever drawn. It was benign
     * for a long time purely because the timing happened not to collide. */
    lvgl_port_lock(0);
    lv_display_set_rotation(disp, LV_DISPLAY_ROTATION_180);
    lvgl_port_unlock();
    const lvgl_port_touch_cfg_t tc = { .disp = disp, .handle = hal_touch_handle() };
    lv_indev_t *touch = lvgl_port_add_touch(&tc);
    ESP_RETURN_ON_FALSE(touch, ESP_FAIL, TAG, "add touch");
    /* LVGL reads once per refresh period (16 ms) by default. A brisk tap can
     * fall between slower reads, which reads as "press firmly". */
    lvgl_port_lock(0);
    lv_timer_set_period(lv_indev_get_read_timer(touch), TOUCH_POLL_MS);
    lvgl_port_unlock();

    lvgl_port_lock(0);
    build();
    {
        /* LVGL's objects live in its own fixed pool, not the heap. */
        lv_mem_monitor_t mm;
        lv_mem_monitor(&mm);
        ESP_LOGI(TAG, "LVGL pool %u%% used, %u bytes free, largest %u",
                 (unsigned)mm.used_pct, (unsigned)mm.free_size,
                 (unsigned)mm.free_biggest_size);
    }
    /* Straight after build(), so the splash covers a screen that is already
     * finished rather than one still being assembled. */
    ui_splash_start();
    lvgl_port_unlock();
    ESP_LOGI(TAG, "LVGL up; free internal %u, largest DMA %u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
    return ESP_OK;
}

RADIO_ONLY static const char *step_name(int32_t hz)
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

/* Which zone a value falls in, for the colour of its peak LED. */
static int rx_zone_of(float dbm)
{
    for (int z = 0; z < RX_ZONES - 1; z++) if (dbm < RXZONES[z].to) return z;
    return RX_ZONES - 1;
}
static int mic_zone_of(float db)
{
    for (int z = 0; z < MIC_ZONES - 1; z++) if (db < MIC_ZONE[z].to) return z;
    return MIC_ZONES - 1;
}
static int swr_zone_of(float w)
{
    for (int z = 0; z < SWR_ZONES - 1; z++) if (w < ZONES[z].to) return z;
    return SWR_ZONES - 1;
}
static float s_mic_bar, s_pwr_bar, s_swr_bar;       /* the transmit bars */

/* Write only what changed. LVGL invalidates an object on every text or style
 * write, even one that sets the value it already has, and then redraws
 * everything underneath -- here, arcs the size of the screen. The power arc's
 * colour, written every tick, redrew the whole transmit face twenty times a
 * second. The mic ring's extra arcs made each of those redraws dearer, until
 * LVGL no longer left the idle task on its core a moment to run, and the task
 * watchdog reset the knob mid-over. */
static void set_text(lv_obj_t *o, const char *s)
{
    const char *cur = lv_label_get_text(o);
    if (cur && strcmp(cur, s) == 0) return;
    lv_label_set_text(o, s);
}

static void set_text_color(lv_obj_t *o, lv_color_t c)
{
    if (lv_color_eq(lv_obj_get_style_text_color(o, LV_PART_MAIN), c)) return;
    lv_obj_set_style_text_color(o, c, 0);
}

/* The slab while a headset is connected: in place of PTT's caption, which the
 * glass no longer keys. On the air the slab is red, and all of it white. */
static void vis(lv_obj_t *o, bool on)
{
    if (on == !lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) return;
    if (on) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    else    lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

static void headset_slab(const ui_state_t *st)
{
    if (s_hs_bt) vis(s_hs_bt, st->headset);
    if (!s_hs_name) return;
    const bool on = st->headset;
    vis(s_ptt_lbl, !on);
    vis(s_hs_name, on);
    vis(s_hs_mic, on && !st->headset_raise);
    vis(s_hs_raise, on && st->headset_raise);
    if (!on) return;
    char t[48];
    snprintf(t, sizeof t, LV_SYMBOL_BLUETOOTH "  %s", st->headset_name[0] ? st->headset_name : "headset");
    set_text(s_hs_name, t);
    set_text_color(s_hs_name, st->tx ? lv_color_white() : C_TEXT);
    set_text(s_hs_mic, st->headset_muted ? SYM_HS_MUTE : SYM_HS_MIC);
    set_text_color(s_hs_mic, st->tx ? lv_color_white() : st->headset_muted ? C_TX_RED : C_TEXT2);
}

void ui_update(const ui_state_t *st)
{
    if (!st || !s_scr) return;
    if (!lvgl_port_lock(20)) return;      /* never block the caller */
    s_last = *st;                         /* editors open on the live value */

    /* No countdown: after ten seconds, a turn of the knob, or the radio
     * keying up, the question simply goes away and the answer is no -- but
     * to a question the knob answers (ui_ask_turn), a turn is the yes. */
    if (s_asking && (s_ask_knob || st->tx ||
                     lv_tick_elaps(s_ask_since) >= ASK_MS)) {
        lv_obj_add_flag(s_ask_panel, LV_OBJ_FLAG_HIDDEN);
        s_asking = false;
        s_ask_answer = s_ask_turn && s_ask_knob && !st->tx ? 1 : -1;
    }

    /* MEM's light follows the radio, but not for a moment after a tap: the
     * radio has not heard it yet. */
    if (s_edit == ED_MENU && st->atu_mem != s_mem_lit &&
        (!s_mem_tapped || lv_tick_elaps(s_mem_tapped) > 1500)) {
        s_mem_lit = st->atu_mem;
        edit_render();
    }

    /* The antenna editor follows the radio until the knob turns it: opened
     * straight after a change of receiver, it shows the last one's at first. */
    if (s_edit == ED_ANT && !s_edit_moved && st->have_ant && ant_index(st) != s_edit_idx) {
        s_edit_idx = ant_index(st);
        edit_render();
    }

    /* While an editor is open its panel owns the screen; leave the rest of the
     * face alone so the value the operator is choosing does not jitter -- nor,
     * behind the SSTV viewer, redraw a picture from PSRAM for a moving meter. */
    if (s_edit != ED_NONE || s_sv_open) { lvgl_port_unlock(); return; }

    /* The address card times out on its own: it covers the frequency, and an
     * operator who walked away should come back to a working dial. Not under
     * a finger holding on towards the firmware picker. */
    if (s_netinfo_until && lv_tick_get() > s_netinfo_until && !s_press_picker) {
        lv_obj_add_flag(s_netinfo, LV_OBJ_FLAG_HIDDEN);
        s_netinfo_until = 0;
    }

    int64_t f = st->freq_hz < 0 ? 0 : st->freq_hz;

    /* Memory mode swaps the frequency readout for the channel -- and a
     * reflector never has one. */
    const bool mem = REFLECTOR_FACE || st->mem_state != UI_MEM_OFF;
    if (mem != s_mem_face) {
        s_mem_face = mem;
        lv_obj_t *vfo[N_DIG + 3];
        for (int i = 0; i < N_DIG; i++) vfo[i] = s_dig[i];
        vfo[N_DIG] = s_sep[0]; vfo[N_DIG + 1] = s_sep[1]; vfo[N_DIG + 2] = s_underline;
        for (int i = 0; i < N_DIG + 3; i++) {
            if (mem) lv_obj_add_flag(vfo[i], LV_OBJ_FLAG_HIDDEN);
            else     lv_obj_remove_flag(vfo[i], LV_OBJ_FLAG_HIDDEN);
        }
        if (mem) {
            lv_obj_remove_flag(s_mem_big, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(s_mem_small, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_mem_big, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_mem_small, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (mem) {
        char big[40], small[80];
#if REFLECTOR_FACE
        /* The talkgroup's name, as the reflector's portal gives it, and the
         * reflector under it. */
        if (st->tg_name[0])  snprintf(big, sizeof big, "%s", st->tg_name);
        else if (st->tg)     snprintf(big, sizeof big, "TG %lu", (unsigned long)st->tg);
        else                 snprintf(big, sizeof big, "MONITOR");
        snprintf(small, sizeof small, "%s", st->server);
#else
        mem_texts(st, big, sizeof big, small, sizeof small);
#endif
        set_text(s_mem_big, big);
        set_text(s_mem_small, small);
        set_text_color(s_mem_big, st->tx ? C_TX_TEXT : C_TEXT);
    }

    int mhz = (int)(f / 1000000);
    int khz = (int)((f / 1000) % 1000);
    int hz  = (int)((f % 1000) / 10);
    int d[N_DIG] = {
        (mhz / 100) % 10, (mhz / 10) % 10, mhz % 10,
        (khz / 100) % 10, (khz / 10) % 10, khz % 10,
        (hz / 10) % 10,   hz % 10,
    };
    int lead = (mhz >= 100) ? 0 : (mhz >= 10) ? 1 : 2;

    for (int i = 0; i < N_DIG && !mem; i++) {
        char b[2] = { (char)('0' + d[i]), 0 };
        const char *txt = (i < lead) ? "" : b;
        if (strcmp(lv_label_get_text(s_dig[i]), txt) != 0)
            lv_label_set_text(s_dig[i], txt);
        lv_color_t c = (i == s_active_dig) ? C_ACCENT_HI
                     : (i > s_active_dig)  ? C_TEXT2     /* these will roll */
                                           : C_TEXT;
        set_text_color(s_dig[i], st->tx ? C_TX_TEXT : c);
    }
    static int s_underline_dig = -1;
    if (s_underline_dig != s_active_dig) {
        s_underline_dig = s_active_dig;
        lv_obj_align(s_underline, LV_ALIGN_CENTER,
                     s_dig_x[s_active_dig] - CX, 204 - CY);
    }

    char tb[24];
#if REFLECTOR_FACE
    /* The talkgroup where band, mode and filter are, and the link where the
     * step is. */
    set_text(s_band, "");
    set_text(s_filt, "");
    if (st->tg) snprintf(tb, sizeof tb, "TG %lu", (unsigned long)st->tg);
    else        snprintf(tb, sizeof tb, "TG --");
    set_text(s_mode, tb);
    set_text_color(s_mode, C_ACCENT);
    set_text(s_step_lbl, st->link_ok ? "connected" : st->connecting ? "connecting"
                                                                  : "disconnected");
    set_text_color(s_step_lbl, st->link_ok ? C_GREEN : st->connecting ? C_WARN : C_DANGER);
    set_text(s_lock_icon, st->tg_locked ? SYM_LOCK : SYM_UNLOCK);
    set_text_color(s_lock_icon, st->tg_locked ? C_WARN : C_LABEL);
    set_text(s_mute_icon, st->muted ? SYM_MUTED : SYM_SOUND);
    set_text_color(s_mute_icon, st->muted ? C_DANGER : C_LABEL);
#else
    if (mem) {
        char g[8];
        snprintf(g, sizeof g, "G%02u", (unsigned)st->mem_group);
        set_text(s_band, g);
    } else if (st->n_rx > 1 && st->rx) {
        /* The second receiver, which only listens: marked, since the band is
         * where the eye goes to see what the dial is on. */
        snprintf(tb, sizeof tb, "SUB %s", band_of(f));
        set_text(s_band, tb);
    } else {
        set_text(s_band, band_of(f));
    }
    if (st->mode) {
        char up[8];
        upcase(st->mode, up, sizeof up);
        set_text(s_mode, up);
    }
    if (st->filter_no) snprintf(tb, sizeof tb, "FIL%u", (unsigned)st->filter_no);
    else               snprintf(tb, sizeof tb, "%ld", (long)(st->filt_hi - st->filt_lo));
    set_text(s_filt, tb);
    set_text(s_step_lbl, mem ? "MEM" : step_name(st->step_hz));
#endif

    /* Greyed out while the radio has not said -- which for AetherSDR's RF
     * gain is always: its TCI carries none. */
    if (RX_FACE) {
        /* The SNR, in UberSDR's colours for it: red at 0 dB, green from 15. */
        if (st->have_snr) {
            snprintf(tb, sizeof tb, "%d dB", (int)lroundf(st->snr_db));
            float f = st->snr_db / 15.0f;
            if (f < 0.0f) f = 0.0f;
            if (f > 1.0f) f = 1.0f;
            set_text(s_agc_val, tb);
            set_text_color(s_agc_val, lv_color_hsv_to_rgb((uint16_t)(f * 120.0f), 85, 96));
        } else {
            set_text(s_agc_val, "--");
            set_text_color(s_agc_val, C_DISABLED);
        }
    } else {
        upcase(st->agc, tb, 8);
        set_text(s_agc_val, tb[0] ? tb : "--");
        set_text_color(s_agc_val, tb[0] ? C_TEXT2 : C_DISABLED);
    }
    if (st->have_gain) gain_label(st, st->gain, tb, sizeof tb);
    set_text(s_gain_val, st->have_gain ? tb : "--");
    set_text_color(s_gain_val, st->have_gain ? C_TEXT2 : C_DISABLED);
    set_text_color(s_gain_cap, st->have_gain ? C_LABEL : C_DISABLED);

    /* RIT is always shown so it is always tappable, but greyed at zero: RIT
     * silently non-zero is a classic way to lose a QSO, so when it IS set it
     * has to stand out. */
    if (REFLECTOR_FACE || RX_FACE) {
        /* no RIT */
    } else if (st->rit_hz) {
        snprintf(tb, sizeof tb, "RIT %+ld", (long)st->rit_hz);
        set_text(s_rit, tb);
        set_text_color(s_rit, C_WARN);
    } else {
        set_text(s_rit, "RIT 0");
        set_text_color(s_rit, C_DISABLED);
    }

    snprintf(tb, sizeof tb, LV_SYMBOL_VOLUME_MID " %u", (unsigned)s_volume);
    set_text(s_vol, tb);
    snprintf(tb, sizeof tb, SYM_MIC " %u", (unsigned)s_micgain);
    set_text(s_mic, tb);

    if (st->warn && st->warn[0] && !s_asking && s_edit == ED_NONE) {
        if (strcmp(lv_label_get_text(s_warn), st->warn) != 0) {
            lv_label_set_text(s_warn, st->warn);
            lv_obj_align(s_warn, LV_ALIGN_TOP_MID, 0, 8);
        }
        if (strcmp(lv_label_get_text(s_warn_net), s_netinfo_text) != 0) {
            lv_label_set_text(s_warn_net, s_netinfo_text);
            lv_obj_align(s_warn_net, LV_ALIGN_BOTTOM_MID, 0, -8);
        }
        if (lv_obj_has_flag(s_warn_panel, LV_OBJ_FLAG_HIDDEN)) {
            lv_obj_remove_flag(s_warn_panel, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(s_warn_panel);
        }
        /* The address card and the warning would otherwise stack. */
        if (s_netinfo) lv_obj_add_flag(s_netinfo, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_warn_panel, LV_OBJ_FLAG_HIDDEN);
    }

    /* The bar follows the signal; the peak LED hangs a second above it and
     * then falls away at 30 dB/s -- about five S-units a second. The readouts
     * give the peak: in SSB that is the figure worth reading. */
#if REFLECTOR_FACE
    /* The whole arc is the audio: what is heard in receive, the microphone in
     * transmit. */
    release(&s_meter_disp, st->tx ? st->tx_mic_dbm : st->rx_level_db);
#else
    release(&s_meter_disp, st->smeter_dbm);
#endif
    const float sig_pk = peak_hold(&s_sig_pk, s_meter_disp, 30.0f);
    if (!st->tx || REFLECTOR_FACE)
        led_set(&s_sig_led, smeter_frac(sig_pk), rx_zone_of(sig_pk));

    float frac = smeter_frac(s_meter_disp);
    lv_arc_set_value(s_meter, (int)(frac * 1000));
    /* Each band fills only across its own span: full once the signal is past
     * its top, empty until the signal reaches its bottom. */
    for (size_t z = 0; z < RX_ZONES; z++) {
        float lo = RXZONES[z].from, hi = RXZONES[z].to;
        float f = (s_meter_disp - lo) / (hi - lo);
        if (f < 0.0f) f = 0.0f;
        if (f > 1.0f) f = 1.0f;
        int16_t v = (int16_t)(f * 1000.0f);
        /* Only write what changed. An arc's bounding box is the whole screen,
         * so a redundant set on sixteen of them would invalidate the display
         * sixteen times a frame for nothing. In practice one segment moves. */
        if (v == s_rx_val[z]) continue;
        s_rx_val[z] = v;
        lv_arc_set_value(s_rx_zone[z], v);
    }
#if REFLECTOR_FACE
    /* Who is talking, where the S-units are: now, or dimmed, the last one. */
    {
        char who[40];
        const unsigned secs = (unsigned)(st->talker_ms / 1000u);
        if (st->tx) {
            snprintf(who, sizeof who, "%d dB", (int)sig_pk);
            set_text(s_srd, who);
            set_text(s_dbm, "microphone");
            set_text_color(s_srd, C_TX_TEXT);
            set_text_color(s_dbm, C_TX_TEXT);
        } else if (st->talker[0]) {
            set_text(s_srd, st->talker);
            if (st->talker_info[0]) snprintf(who, sizeof who, "%s", st->talker_info);
            else                    snprintf(who, sizeof who, "%us", secs);
            set_text(s_dbm, who);
            set_text_color(s_srd, C_TEXT);
            set_text_color(s_dbm, C_GREEN);
        } else if (st->last_talker[0]) {
            set_text(s_srd, st->last_talker);
            if (secs < 60) snprintf(who, sizeof who, "%us ago", secs);
            else           snprintf(who, sizeof who, "%um ago", secs / 60u);
            set_text(s_dbm, who);
            set_text_color(s_srd, C_LABEL);
            set_text_color(s_dbm, C_LABEL);
        } else {
            set_text(s_srd, "--");
            set_text(s_dbm, "");
            set_text_color(s_srd, C_LABEL);
        }
    }
#else
    char sbuf[10];
    smeter_text(sig_pk, sbuf, sizeof sbuf);
#if VFO_HAS_SDR
    /* A web SDR playing beside the radio: its S-meter, the thin blue line
     * outside the radio's, and its reading where the dBm is -- in blue, which
     * says whose it is. Not in transmit, when the SDR is silent. */
    static float   s_sdr_disp = -127.0f;
    static peak_t  s_sdr_pk = { .v = -127.0f };
    static int16_t s_sdr_val;
    const bool sdr_on = st->rxsrc >= 0 && st->sdr_streaming && !st->tx;
    if (sdr_on == lv_obj_has_flag(s_sdr_arc, LV_OBJ_FLAG_HIDDEN)) {
        if (sdr_on) lv_obj_remove_flag(s_sdr_arc, LV_OBJ_FLAG_HIDDEN);
        else        lv_obj_add_flag(s_sdr_arc, LV_OBJ_FLAG_HIDDEN);
        led_set(&s_sdr_led, 0.0f, -1);
        led_show(&s_sdr_led, sdr_on);
    }
    float sdr_pk = -127.0f;
    if (sdr_on) {
        release(&s_sdr_disp, st->sdr_dbm);
        /* Its peak held a second above the line, then falling at 30 dB/s
         * to meet it, as the radio's does. */
        sdr_pk = peak_hold(&s_sdr_pk, s_sdr_disp, 30.0f);
        led_set(&s_sdr_led, smeter_frac(sdr_pk), 0);
        const int16_t v = (int16_t)(smeter_frac(s_sdr_disp) * 1000.0f);
        if (v != s_sdr_val) {               /* only what changed: see s_rx_val */
            s_sdr_val = v;
            lv_arc_set_value(s_sdr_arc, v);
        }
    } else {
        s_sdr_disp = -127.0f;
        peak_reset(&s_sdr_pk, -127.0f);
    }
#endif
    if (!st->tx) {
        set_text(s_srd, sbuf);
#if VFO_HAS_SDR
        if (st->rxsrc >= 0) {
            /* Playing, its S-units; on its way, dots; not to be had, why. */
            if (sdr_on)                smeter_text(sdr_pk, tb, sizeof tb);
            else if (st->sdr_trouble)  snprintf(tb, sizeof tb, "%s", st->sdr_note);
            else                       snprintf(tb, sizeof tb, "...");
            set_text(s_dbm, tb);
            set_text_color(s_dbm, sdr_on ? C_SDR : st->sdr_trouble ? C_WARN : C_LABEL);
        } else
#endif
        {
            /* A receiver's level is its own, in dB below full scale. */
            snprintf(tb, sizeof tb, RX_FACE ? "%d dBFS" : "%d dBm", (int)sig_pk);
            set_text(s_dbm, tb);
            set_text_color(s_dbm, C_LABEL);
        }
    }
    set_text_color(s_srd, st->tx ? C_TX_TEXT : C_TEXT);
#endif

    if (st->tx && !REFLECTOR_FACE) {
        /* Each over starts from nothing rather than from the last one's
         * held peaks. */
        if (!s_was_tx) {
            s_mic_bar = MIC_DB_MIN - 60.0f;
            s_pwr_bar = 0.0f;
            s_swr_bar = 1.0f;
            peak_reset(&s_mic_pk, MIC_DB_MIN);
            peak_reset(&s_pwr_pk, 0.0f);
            peak_reset(&s_swr_pk, 1.0f);
        }

        /* MIC: the radio's mic level as AetherSDR reports it -- the value its
         * Level gauge draws as the bar -- on that gauge's scale. At rest it
         * sits around -95, off the bottom. Peak LED falls at 20 dB/s. */
        const float mv     = release(&s_mic_bar, st->tx_mic_dbm);
        const float mic_pk = peak_hold(&s_mic_pk, mv, 20.0f);
        led_set(&s_mic_led, mic_frac(mic_pk), mic_zone_of(mic_pk));
        for (size_t z = 0; z < MIC_ZONES; z++) {
            float lo = MIC_ZONE[z].from, hi = MIC_ZONE[z].to;
            float f = (mv <= lo) ? 0.0f : (mv >= hi) ? 1.0f : (mv - lo) / (hi - lo);
            lv_arc_set_value(s_mic_zone[z], (int)(f * 1000));
        }

        /* POWER: auto-ranged onto a sensible ladder rather than a continuous
         * scale, so the full-scale figure is always a number an operator
         * recognises. Holds the highest range reached and decays out of it,
         * otherwise the scale would jump about mid-over. */
        float pw = st->tx_peak_w;
        if (pw > s_pwr_peak) s_pwr_peak = pw;
        else                 s_pwr_peak *= 0.997f;     /* decay out of a range */
        int r = PWR_RANGES - 1;
        for (int i = 0; i < PWR_RANGES; i++) {
            if (s_pwr_peak <= PWR[i].fs) { r = i; break; }
        }
        pwr_set_range(r);
        float fs = PWR[r].fs;
        /* The peak LED falls at half the range per second, and the readout
         * gives the peak: in SSB that is the power of the last words, the
         * figure that means something. */
        const float pw_bar  = release(&s_pwr_bar, pw);
        const float pw_held = peak_hold(&s_pwr_pk, pw_bar, 0.5f * fs);
        float pf = fs > 0 ? pw_bar / fs : 0.0f;
        if (pf < 0) pf = 0;
        if (pf > 1) pf = 1;
        /* One colour, the logo's gold. It used to turn amber above 90% of the
         * range, which beside gold would be no signal at all -- and the range
         * steps up by itself once the power passes full scale. */
        lv_arc_set_value(s_pwr_arc, (int)(pf * 1000));
        const float pk_f = fs > 0 ? pw_held / fs : 0.0f;
        led_set(&s_pwr_led, pk_f, 0);

        /* SWR only counts with real forward power behind it: in a speech
         * pause the radio's figure is noise, and the peak would latch it. The
         * bar follows; the LED and the readout hold the peak, falling at 1.0
         * per second. */
        const float w_bar = release(&s_swr_bar,
                                    st->tx_fwd_w >= 1.0f ? st->tx_swr : 1.0f);
        const float w = peak_hold(&s_swr_pk, w_bar, 1.0f);
        led_set(&s_swr_led, swr_frac(w), swr_zone_of(w));
        for (size_t z = 0; z < SWR_ZONES; z++) {
            float lo = ZONES[z].from, hi = ZONES[z].to;
            float f = (w_bar <= lo) ? 0.0f : (w_bar >= hi) ? 1.0f
                                    : (w_bar - lo) / (hi - lo);
            lv_arc_set_value(s_swr_zone[z], (int)(f * 1000));
        }

        /* The REAL SWR figure, not clamped to the scale. The bar pegs at 3.0
         * because that is where the scale ends, but 11.5 is exactly what an
         * operator needs to see -- it is the difference between a poor match
         * and nothing connected. */
        char b[20];
        fmt1(b, sizeof b, "SWR ", w, "");
        set_text(s_srd, b);
        set_text_color(s_srd, w >= 2.5f ? C_DANGER : w >= 2.0f ? C_WARN : C_TEXT);
        /* Show the full scale alongside the reading. An auto-ranging bar
         * without its scale is misleading: half-deflection could be 50 W or
         * 1250 W. Anywhere on the face is too crowded for a separate caption,
         * so it goes here. */
        /* PWR 1.4kW below a kilowatt boundary, PWR 850W above it -- whichever
         * reads more naturally, with the auto-range's full scale alongside so
         * the bar is never ambiguous. */
        char pb[28];
        if (pw_held >= 1000.0f) {
            char t[16];
            fmt1(t, sizeof t, "", pw_held / 1000.0f, "kW");
            snprintf(pb, sizeof pb, "PWR %s / %dW", t, (int)fs);
        } else {
            snprintf(pb, sizeof pb, "PWR %dW / %dW",
                     (int)(pw_held + 0.5f), (int)fs);
        }
        set_text(s_dbm, pb);
        /* C_LABEL is dark grey, which all but vanishes against the amber TX
         * background -- which is why these readouts could not be found. */
        set_text_color(s_dbm, C_TX_TEXT);
    }

    if (st->tx != s_was_tx) {
        s_was_tx = st->tx;
        lv_obj_set_style_bg_color(s_scr, st->tx ? C_BG_TX : C_BG, 0);
        /* Swap the meter set wholesale. AGC and gain are receive settings
         * and make way for the transmit readouts, which are wider. A
         * reflector's arc stays the arc: only the colours change. */
        lv_obj_t *aux[] = { s_agc_cap, s_agc_val, s_gain_cap, s_gain_val };
        for (size_t i = 0; i < sizeof aux / sizeof aux[0] && !REFLECTOR_FACE; i++) {
            if (st->tx) lv_obj_add_flag(aux[i], LV_OBJ_FLAG_HIDDEN);
            else        lv_obj_remove_flag(aux[i], LV_OBJ_FLAG_HIDDEN);
        }
        if (REFLECTOR_FACE) {
            /* the arc, the notches and the peak LED stay as they are */
        } else if (st->tx) {
            lv_obj_add_flag(s_meter, LV_OBJ_FLAG_HIDDEN);
            for (size_t z = 0; z < RX_ZONES; z++)
                lv_obj_add_flag(s_rx_zone[z], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_rx_ticks, LV_OBJ_FLAG_HIDDEN);
            led_show(&s_sig_led, false);
            led_show(&s_mic_led, true);
            led_show(&s_pwr_led, true);
            led_show(&s_swr_led, true);
            for (size_t z = 0; z < MIC_ZONES; z++)
                lv_obj_remove_flag(s_mic_zone[z], LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(s_pwr_arc, LV_OBJ_FLAG_HIDDEN);
            s_pwr_peak = 0.0f;              /* re-range for each over */
            lv_obj_remove_flag(s_tx_ticks, LV_OBJ_FLAG_HIDDEN);
            for (size_t z = 0; z < SWR_ZONES; z++)
                lv_obj_remove_flag(s_swr_zone[z], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(s_meter, LV_OBJ_FLAG_HIDDEN);
            for (size_t z = 0; z < RX_ZONES; z++)
                lv_obj_remove_flag(s_rx_zone[z], LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(s_rx_ticks, LV_OBJ_FLAG_HIDDEN);
            led_show(&s_sig_led, true);
            led_show(&s_mic_led, false);
            led_show(&s_pwr_led, false);
            led_show(&s_swr_led, false);
            for (size_t z = 0; z < MIC_ZONES; z++)
                lv_obj_add_flag(s_mic_zone[z], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_pwr_arc, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_tx_ticks, LV_OBJ_FLAG_HIDDEN);
            for (size_t z = 0; z < SWR_ZONES; z++)
                lv_obj_add_flag(s_swr_zone[z], LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_set_style_arc_color(s_ring, st->tx ? C_TX_RED : C_BG, LV_PART_MAIN);
        /* Unmissable: the whole bottom slab goes solid red. With toggle PTT
         * you can walk away from it, so it has to shout. */
        lv_obj_set_style_bg_color(s_ptt, st->tx ? C_TX_RED : C_BG1, 0);
        lv_obj_set_style_text_color(s_ptt_lbl,
            st->tx ? lv_color_white() : C_TEXT2, 0);
    }
    headset_slab(st);
    if (RX_FACE) {
        /* The spot or voice nearest the dial: green while it is heard, bright
         * when the dial is on it, dimmer when it is only a pointer. */
        if (!st->has_spots) {
            set_text(s_ptt_lbl, "");
            set_text(s_spot_sub, "");
            set_text(s_spot_n, "");
        } else if (!s_nspots) {
            set_text(s_ptt_lbl, "");
            set_text(s_spot_sub, "no spots or voices here");
            set_text_color(s_spot_sub, C_LABEL);
            set_text(s_spot_n, "");
        } else {
            int k = 0;
            for (int i = 1; i < s_nspots; i++)
                if (llabs((int64_t)s_spots[i].hz - f) < llabs((int64_t)s_spots[k].hz - f)) k = i;
            const ui_spot_t *sp = &s_spots[k];
            char l1[24], l2[56], l3[24];
            spot_lines(sp, l1, sizeof l1, l2, sizeof l2);
            set_text(s_ptt_lbl, l1);
            set_text_color(s_ptt_lbl, sp->heard ? C_GREEN : llabs((int64_t)sp->hz - f) < 500 ? C_TEXT : C_TEXT2);
            set_text(s_spot_sub, l2);
            set_text_color(s_spot_sub, C_TEXT2);
            snprintf(l3, sizeof l3, "%u on %s", (unsigned)s_nspots, band_of(f));
            set_text(s_spot_n, l3);
        }
    } else if (st->tx_remote)
        /* Keyed by the desktop, a foot switch or another client. Our trx:false
         * would only touch our own producer handle, so tapping cannot stop it
         * and the caption must not imply otherwise. */
        set_text(s_ptt_lbl, "TX  REMOTE");
    else if (st->tx)
        set_text(s_ptt_lbl, "TX");
    else
#if VFO_PTT_DRY_RUN
        set_text(s_ptt_lbl, st->may_key ? "PTT TEST" : "----");
#else
        set_text(s_ptt_lbl, st->may_key ? "PTT" : "----");
#endif

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
bool    ui_take_ptt_tap(void)      { bool v = s_ptt_tap;     s_ptt_tap  = false; return v; }
bool    ui_take_lock_tap(void)     { bool v = s_lock_tap;    s_lock_tap = false; return v; }
bool    ui_take_mute_tap(void)     { bool v = s_mute_tap;    s_mute_tap = false; return v; }


/* ------------------------------------------------------------------- dim */

/* A knob on a desk spends most of its life being looked at, not touched, so
 * "idle" has to mean nothing happened at all -- no detent, no tap, and no
 * transmit. Dimming rather than blanking: the frequency stays readable across
 * the room, which is the whole point of the thing, while an OLED-ish panel
 * left at full brightness for days is asking for trouble. */
#define DIM_FULL  200
#define DIM_LOW    18
#define DIM_BLANK   0

/* Two stages, because they answer different questions. Dimming says "nobody is
 * using this" and still leaves the frequency readable from across the room,
 * which is most of what a dial is for. Blanking says "nobody is in the room",
 * and is what actually spares the panel overnight. */
enum { LVL_FULL = 0, LVL_DIM, LVL_BLANK };

static uint32_t s_dim_after_ms   =  5u * 60u * 1000u;
static uint32_t s_blank_after_ms = 10u * 60u * 1000u;
static uint32_t s_last_use_ms;
static uint8_t  s_level = LVL_FULL;

static void set_level(uint8_t level)
{
    if (level == s_level) return;
    s_level = level;
    panel_set_brightness(level == LVL_BLANK ? DIM_BLANK
                       : level == LVL_DIM   ? DIM_LOW
                                            : DIM_FULL);
}

uint32_t ui_last_use(void) { return s_last_use_ms; }

void ui_note_activity(void)
{
    /* Reachable from the configuration page, which is serving long before the
     * display exists. lv_tick_get() before lv_init() is not survivable. */
    if (!s_scr) return;
    s_last_use_ms = lv_tick_get();
    set_level(LVL_FULL);
}

void ui_dim_set_minutes(uint16_t dim_minutes, uint16_t blank_minutes)
{
    s_dim_after_ms   = (uint32_t)dim_minutes   * 60u * 1000u;
    s_blank_after_ms = (uint32_t)blank_minutes * 60u * 1000u;
    ui_note_activity();        /* no-op until the display is up */
}

void ui_dim_tick(bool transmitting)
{
    if (!s_scr) return;
    if (transmitting) { ui_note_activity(); return; }

    const uint32_t idle = lv_tick_elaps(s_last_use_ms);
    /* Checked darkest-first so the stages cannot fight when blank <= dim. */
    if (s_blank_after_ms && idle >= s_blank_after_ms)    set_level(LVL_BLANK);
    else if (s_dim_after_ms && idle >= s_dim_after_ms)   set_level(LVL_DIM);
    else                                                 set_level(LVL_FULL);
}
