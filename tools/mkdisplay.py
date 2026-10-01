#!/usr/bin/env python3
"""Draw the knob as animated SVG, for the README.

The face's geometry and palette are copied from components/ui/ui.c rather than
eyeballed, so these stay honest when the firmware changes: 360x360 round glass,
meter arc from 170 to 370 degrees at radius 170 with a 12 px band, and the same
colour blocks and background-coloured notches the device actually draws. The
readouts step through the same values as the bars, formatted the way the
firmware formats them.

Around the face is the body, to scale: Waveshare's drawing gives 66 mm across,
and the cover glass (~55 mm) and the 1.8" panel (45.7 mm, the 360 px) are
measured off the same drawing. Everything outside the body is annotation --
the scale the firmware draws as bare ticks, and the dimensions -- in a neutral
grey that reads on light and dark pages alike. Nothing is added to the glass
that the device does not show.
"""
import math, os

CX = CY = 180
ARC_R0, ARC_ROT, ARC_SPAN = 170, 170, 200
BAND = 12
RC = ARC_R0 - BAND / 2                      # centreline of the band
SWR_ROT, SWR_SPAN = ARC_ROT, ARC_SPAN // 2 - 3
AUD_ROT, AUD_SPAN = ARC_ROT + ARC_SPAN // 2 + 3, ARC_SPAN // 2 - 3
PTT_TOP = 248
MIC_R = ARC_R0 - 22            # inner ring, the mic level
MIC_BAND = 5

BG, BG1, BG_TX = "#0F0F1A", "#1A2A3A", "#3A2A0E"
ACCENT, ACCENT_HI = "#00B4D8", "#00C8F0"
TEXT, TEXT2, LABEL, SUBTLE = "#C8D8E8", "#8EA8C0", "#506070", "#1A2330"
WARN, DANGER, TX_RED, TX_TEXT = "#FFB84D", "#FF4D4D", "#E01010", "#F0C890"
GREEN, DISABLED = "#4DD87A", "#3A4A5A"
RFG_GOLD = "#E9B61D"           # splash.h RFG_GOLD_HEX, the logo's gold
PWR = RFG_GOLD                 # the power bar wears it
FONT = "DejaVu Sans,Verdana,sans-serif"

RXZONES = [(-127, -121, "#1A6B47"), (-121, -109, "#1F7A52"),
           (-109,  -97, "#2F9E6A"), (-97,   -85, "#4DD87A"),
           (-85,   -73, "#9BD94A"), (-73,   -53, "#FFD24D"),
           (-53,   -33, "#FF9A3C"), (-33,   -13, "#FF4D4D")]
RXNOTCH = [-121, -109, -97, -85, -73, -53, -33]
RXTICKS = [(-121, 6, "S1"), (-109, 6, "S3"), (-97, 6, "S5"), (-85, 6, "S7"),
           (-73, 11, "S9"), (-53, 6, "+20"), (-33, 6, "+40"), (-13, 9, "+60")]
SWRZONES = [(1.0, 2.0, "#4DD87A"), (2.0, 2.5, "#FFB84D"), (2.5, 3.0, "#FF4D4D")]
# ui.c MIC_ZONE: AetherSDR's mic Level gauge, -40 to +10 dB.
MICZONES = [(-40, -10, "#4DD87A"), (-10, 0, "#FFB84D"), (0, 10, "#FF4D4D")]
# The Icom firmware's face (ui.c under VFO_RADIO_ICOM): the IC-705's own
# screen -- black, white digits, Icom blue, the S-meter blue up to S9 and red
# above it. use_palette() swaps a set in for the colours above; the drawing
# functions look them up when they are called.
PALETTES = {
    "aethersdr": dict(BG=BG, BG1=BG1, BG_TX=BG_TX, ACCENT=ACCENT, ACCENT_HI=ACCENT_HI,
                      TEXT=TEXT, TEXT2=TEXT2, LABEL=LABEL, SUBTLE=SUBTLE, WARN=WARN,
                      DANGER=DANGER, TX_RED=TX_RED, TX_TEXT=TX_TEXT, GREEN=GREEN,
                      DISABLED=DISABLED, PWR=PWR, RXZONES=RXZONES,
                      SWRZONES=SWRZONES, MICZONES=MICZONES),
    "icom": dict(BG="#000000", BG1="#141A24", BG_TX="#2A0508", ACCENT="#2F7BFF",
                 ACCENT_HI="#5A9BFF", TEXT="#FFFFFF", TEXT2="#C0C8D4",
                 LABEL="#707884", SUBTLE="#181C24", WARN="#FFB000", DANGER="#FF3030",
                 TX_RED="#E60012", TX_TEXT="#FFFFFF", GREEN="#3FA9FF",
                 DISABLED="#3A4048", PWR="#3FA9FF",
                 RXZONES=[(-127, -121, "#0D3B8C"), (-121, -109, "#1350B0"),
                          (-109,  -97, "#1A68D4"), (-97,   -85, "#2A86F2"),
                          (-85,   -73, "#46A8FF"), (-73,   -53, "#FF6A5A"),
                          (-53,   -33, "#FF4040"), (-33,   -13, "#E60012")],
                 SWRZONES=[(1.0, 2.0, "#3FA9FF"), (2.0, 2.5, "#FFB000"),
                           (2.5, 3.0, "#FF3030")],
                 MICZONES=[(-40, -10, "#3FA9FF"), (-10, 0, "#FFB000"),
                           (0, 10, "#FF3030")]),
    # The svxconnect firmware's face (ui.c under VFO_RADIO_SVXCONNECT):
    # svxconnect.app's ink and gold, green for connected. Its arc is the audio,
    # -60 to 0 dBFS, and these zones are in dBFS, not dBm.
    "svxconnect": dict(BG="#08090C", BG1="#13161D", BG_TX="#2A0C0C", ACCENT="#E5A823",
                       ACCENT_HI="#ECC34A", TEXT="#FFFFFF", TEXT2="#E2E8F0",
                       LABEL="#94A3B8", SUBTLE="#1B1F29", WARN="#D29922",
                       DANGER="#D13B3B", TX_RED="#D13B3B", TX_TEXT="#FFFFFF",
                       GREEN="#2EA043", DISABLED="#475569", PWR="#E5A823",
                       RXZONES=[(-60, -48, "#1B5E2E"), (-48, -36, "#237A3B"),
                                (-36, -24, "#2EA043"), (-24, -18, "#35B35A"),
                                (-18, -12, "#9DBD3B"), (-12,  -6, "#D8C43A"),
                                ( -6,  -3, "#E08C33"), ( -3,   0, "#D13B3B")],
                       SWRZONES=[(1.0, 2.0, "#35B35A"), (2.0, 2.5, "#D8C43A"),
                                 (2.5, 3.0, "#D13B3B")],
                       MICZONES=[(-40, -10, "#35B35A"), (-10, 0, "#D8C43A"),
                                 (0, 10, "#D13B3B")]),
    # The multiflex firmware's face (ui.c under VFO_RADIO_MULTIFLEX): the
    # Maestro's -- black, white digits, Flex blue, a blue S-meter that turns
    # red over S9, power in green, TX a red badge.
    "multiflex": dict(BG="#000000", BG1="#0C1622", BG_TX="#2A0608", ACCENT="#2A9DF4",
                      ACCENT_HI="#62BBFF", TEXT="#FFFFFF", TEXT2="#C9D2DC",
                      LABEL="#7D8792", SUBTLE="#141C26", WARN="#FFB000",
                      DANGER="#F0302C", TX_RED="#E8262B", TX_TEXT="#FFFFFF",
                      GREEN="#43B649", DISABLED="#3A424C", PWR="#43B649",
                      RXZONES=[(-127, -121, "#0A3563"), (-121, -109, "#0F4C8A"),
                               (-109,  -97, "#1666B3"), (-97,   -85, "#1F82D9"),
                               (-85,   -73, "#2A9DF4"), (-73,   -53, "#F26A6A"),
                               (-53,   -33, "#EE4444"), (-33,   -13, "#E8262B")],
                      SWRZONES=[(1.0, 2.0, "#2A9DF4"), (2.0, 2.5, "#FFB000"),
                                (2.5, 3.0, "#F0302C")],
                      MICZONES=[(-40, -10, "#2A9DF4"), (-10, 0, "#FFB000"),
                                (0, 10, "#F0302C")]),
    # The ubersdr firmware's face (ui.c under VFO_RADIO_UBERSDR): UberSDR's
    # own dark theme, its S-meter red through yellow to green as UberSDR
    # colours it. It only receives, so its transmit colours are never shown.
    "ubersdr": dict(BG="#090C12", BG1="#141A25", BG_TX="#2A0C10", ACCENT="#08A2FB",
                    ACCENT_HI="#4DB4FF", TEXT="#DFE5EE", TEXT2="#8D99AD",
                    LABEL="#5C6779", SUBTLE="#1A2130", WARN="#F2B544",
                    DANGER="#F2646A", TX_RED="#F2646A", TX_TEXT="#FFFFFF",
                    GREEN="#45D69A", DISABLED="#2F3B4E", PWR="#08A2FB",
                    RXZONES=[(-127, -121, "#F42525"), (-121, -109, "#F48C25"),
                             (-109,  -97, "#F4F425"), (-97,   -85, "#8CF425"),
                             (-85,   -73, "#25F425"), (-73,   -53, "#25F425"),
                             (-53,   -33, "#25F425"), (-33,   -13, "#25F425")],
                    SWRZONES=SWRZONES, MICZONES=MICZONES),
}


def use_palette(name):
    globals().update(PALETTES[name])


# ui.c add_tx_ticks(): value, on-screen label, 0 grey / 1 amber / 2 red.
SWRTICKS = [(1.0, "1", 0), (1.5, None, 0), (2.0, "2", 1), (2.5, None, 2), (3.0, "3", 2)]
PWR_FS, PWR_PEGS = 100, [(10, "10"), (50, "50"), (100, "100")]   # the 100 W range

# --- the body, in face pixels ---------------------------------------------
MM = 360 / 45.72                # the panel: 1.8 inches across is 360 px
BODY_R = 33.0 * MM              # 66 mm
GLASS_R = 27.5 * MM             # cover glass, off Waveshare's drawing
MARGIN = 46                     # room for the scale around the body
DIM_H = 58                      # room for the dimension line underneath
SIZE = 2 * (BODY_R + MARGIN)
OX = OY = SIZE / 2              # the face's centre on the canvas
ANNOT = "#8A94A6"               # annotation: legible on white and on #0d1117
ANNOT_HOT = "#E0902A"


def smeter_frac(dbm):
    dbm = max(-127.0, min(-13.0, dbm))
    return (0.6 * (dbm + 127) / 54) if dbm <= -73 else (0.6 + 0.4 * (dbm + 73) / 60)


def smeter_text(dbm):
    """ui.c smeter_text(), to the letter."""
    if dbm >= -73:
        return f"S9+{int((dbm + 73) / 10) * 10}"
    return f"S{max(0, min(9, int((dbm + 127) / 6)))}"


def swr_frac(s):
    return max(0.0, min(1.0, (s - 1.0) / 2.0))


def mic_frac(db):
    return max(0.0, min(1.0, (db + 40.0) / 50.0))


def pt(deg, r, cx=CX, cy=CY):
    a = math.radians(deg)
    return cx + r * math.cos(a), cy + r * math.sin(a)


def arc_path(a0, a1, r):
    x0, y0 = pt(a0, r)
    x1, y1 = pt(a1, r)
    large = 1 if abs(a1 - a0) > 180 else 0
    sweep = 1 if a1 > a0 else 0
    return f"M{x0:.2f},{y0:.2f} A{r:.2f},{r:.2f} 0 {large} {sweep} {x1:.2f},{y1:.2f}"


def arc_len(a0, a1, r):
    return abs(math.radians(a1 - a0)) * r


def block(a0, a1, colour, inner="", r=RC, band=BAND):
    return (f'<path d="{arc_path(a0, a1, r)}" fill="none" stroke="{colour}" '
            f'stroke-width="{band}">{inner}</path>')


def notch(deg):
    x0, y0 = pt(deg, ARC_R0 - 13)
    x1, y1 = pt(deg, ARC_R0 + 1)
    return (f'<line x1="{x0:.2f}" y1="{y0:.2f}" x2="{x1:.2f}" y2="{y1:.2f}" '
            f'stroke="{BG}" stroke-width="3"/>')


def tick(deg, length, colour, width=2):
    r1 = ARC_R0 - 15
    x0, y0 = pt(deg, r1 - length)
    x1, y1 = pt(deg, r1)
    return (f'<line x1="{x0:.2f}" y1="{y0:.2f}" x2="{x1:.2f}" y2="{y1:.2f}" '
            f'stroke="{colour}" stroke-width="{width}" stroke-linecap="round"/>')


def text(x, y, s, size, colour, weight=400, anchor="middle", extra="", inner=""):
    return (f'<text x="{x:.1f}" y="{y:.1f}" font-size="{size}" fill="{colour}" '
            f'font-family="{FONT}" font-weight="{weight}" text-anchor="{anchor}"'
            f'{extra}>{s}{inner}</text>')


def sweep_cover(a0, a1, values, dur, colour=SUBTLE, r=RC, band=BAND):
    """A track-coloured arc drawn backwards over the blocks, its dash length
    animated so the bar appears to fill and fall. The bar fills from a0, so
    pass the ends swapped for one that fills the other way."""
    L = arc_len(a0, a1, r)
    lens = " ; ".join(f"{L * (1 - v):.2f} {L + 10:.2f}" for v in values)
    # Open on the first value, so a renderer that ignores SMIL still shows a
    # meter with something in it rather than an empty track.
    first = L * (1 - values[0])
    return (f'<path d="{arc_path(a1, a0, r)}" fill="none" stroke="{colour}" '
            f'stroke-width="{band + 0.5}" stroke-dasharray="{first:.2f} {L + 10:.2f}">'
            f'<animate attributeName="stroke-dasharray" dur="{dur}" '
            f'repeatCount="indefinite" values="{lens}"/></path>')


def step_keys(n):
    """Key times for something that follows an n-keyframe bar: each step takes
    over halfway between two keyframes, so it names the one the bar is
    nearest to."""
    t = [k / (n - 1) for k in range(n)]
    return ";".join(f"{v:.4f}" for v in [0.0] + [(t[k] + t[k + 1]) / 2
                                                  for k in range(n - 1)])


def stepped(x, y, labels, dur, size, colour, weight=400, colours=None):
    """A readout that follows an animated bar.

    SMIL cannot animate text content, so every distinct string is stacked and
    shown in turn. The bar moves linearly between keyframes at k/(n-1); each
    label takes over halfway between two of them, so the text always names the
    keyframe the bar is nearest to."""
    n = len(labels)
    keys = step_keys(n)
    out = []
    seen = []
    for i, lab in enumerate(labels):
        key = (lab, colours[i] if colours else colour)
        if key in seen:
            continue
        seen.append(key)
        vals = ";".join("1" if (labels[j], colours[j] if colours else colour) == key
                        else "0" for j in range(n))
        out.append(text(x, y, lab, size, key[1], weight,
                        extra=f' opacity="{1 if i == 0 else 0}"',
                        inner=f'<animate attributeName="opacity" dur="{dur}" '
                              f'repeatCount="indefinite" calcMode="discrete" '
                              f'keyTimes="{keys}" values="{vals}"/>'))
    return "".join(out)


def cycling_digit(x, y, size, colour, digits, dur, weight=700):
    """SMIL cannot animate text content, so stack the glyphs and cross-fade."""
    n = len(digits)
    out = []
    for i, d in enumerate(digits):
        vals, keys = [], []
        for k in range(n + 1):
            vals.append("1" if k % n == i else "0")
            keys.append(f"{k / n:.4f}")
        out.append(
            f'<text x="{x}" y="{y}" font-size="{size}" fill="{colour}" '
            f'font-family="{FONT}" font-weight="{weight}" '
            f'text-anchor="middle" opacity="{1 if i == 0 else 0}">{d}'
            f'<animate attributeName="opacity" dur="{dur}" repeatCount="indefinite" '
            f'calcMode="discrete" values="{";".join(vals)}" '
            f'keyTimes="{";".join(keys)}"/></text>')
    return "".join(out)


def readout(digits, cycle_idx=None, cycle_vals=None, dur="6s",
            colour=TEXT, sep_colour=LABEL, underline=None, after_colour=None,
            active_colour=None, ghz=False):
    """Eight digits: three MHz with leading blanks, three kHz, two Hz.
    PITCH 33 for the first six, 28 for the Hz pair, separators 11 wide.
    From 1 GHz (ghz) four MHz, three kHz and the 100 Hz digit, the
    separators one along (ui.c dig_place). Digits after the active one
    "will roll" and are drawn in after_colour; the active one itself, when it
    is not animated, in active_colour."""
    PITCH, SMALL, SEPW, FS = 33, 28, 11, 44
    n_small, seps = (1, (3, 6)) if ghz else (2, (2, 5))
    total = (8 - n_small) * PITCH + n_small * SMALL + 2 * SEPW
    x = CX - total / 2
    out, xs = [], [0] * 8
    for i in range(8):
        w = SMALL if i >= 8 - n_small else PITCH
        cx = x + w / 2
        xs[i] = cx
        ch = digits[i]
        if i == cycle_idx and cycle_vals:
            out.append(cycling_digit(cx, 186, FS, ACCENT_HI, cycle_vals, dur))
        elif ch != " ":
            late = after_colour and underline is not None and i > underline
            c = (active_colour if active_colour and i == underline
                 else after_colour if late else colour)
            out.append(text(cx, 186, ch, FS, c, 700))
        x += w
        if i in seps:
            out.append(text(x + SEPW / 2, 186, ".", FS, sep_colour))
            x += SEPW
    if underline is not None:
        out.append(f'<rect x="{xs[underline] - (PITCH - 9) / 2:.1f}" y="196" '
                   f'width="{PITCH - 9}" height="3" fill="{ACCENT}"/>')
    return "".join(out)


def speaker(x, y, colour):
    """Font Awesome's volume-down, which is what LV_SYMBOL_VOLUME_MID draws;
    (x, y) is the left end of the baseline, 14 px type."""
    return (f'<g transform="translate({x:.1f},{y - 11:.1f})" fill="{colour}">'
            f'<path d="M0,3.5 h3 l4,-3.5 v11 l-4,-3.5 h-3 z"/>'
            f'<path d="M9,3 a3.2,3.2 0 0 1 0,5" fill="none" stroke="{colour}" '
            f'stroke-width="1.6" stroke-linecap="round"/></g>')


def microphone(x, y, colour):
    """Font Awesome's microphone, the glyph in components/ui/font_mic_14.c."""
    return (f'<g transform="translate({x:.1f},{y - 12:.1f})" fill="{colour}">'
            f'<rect x="2.5" y="0" width="5" height="8" rx="2.5"/>'
            f'<path d="M0.8,5.5 a4.2,4.2 0 0 0 8.4,0" fill="none" stroke="{colour}" '
            f'stroke-width="1.5" stroke-linecap="round"/>'
            f'<rect x="4.3" y="9.6" width="1.4" height="2.4"/>'
            f'<rect x="2.2" y="11.6" width="5.6" height="1.4" rx="0.7"/></g>')


def icon_readout(cx, y, icon, value, colour):
    """An LVGL label "<icon> 40" centred on cx: a 10-11 px glyph, a space, the
    digits."""
    w = 11 + 4 + 8.4 * len(value)
    x0 = cx - w / 2
    return icon(x0, y, colour) + text(x0 + 15, y, value, 14, colour, anchor="start")


def aux(agc, gain_caption, gain, gain_known=True):
    """AGC left of the S-unit readout and the front end's gain right of it,
    each a caption over its setting (ui.c AUX_DX); the gain greyed, with
    "--", while the radio does not report one."""
    dx = 72
    g_cap, g_val = (LABEL, TEXT2) if gain_known else (DISABLED, DISABLED)
    return (text(CX - dx, 83, "AGC", 14, LABEL) + text(CX - dx, 102, agc, 14, TEXT2)
            + text(CX + dx, 83, gain_caption, 14, g_cap)
            + text(CX + dx, 102, gain if gain_known else "--", 14, g_val))


def approx_width(s, size):
    """Montserrat's advance, roughly -- enough to set an icon beside a
    centred label the way LVGL lays out the whole line."""
    w = 0.0
    for c in s:
        w += 0.27 if c == " " else 0.62 if c.isdigit() else 0.70 if c.isupper() else 0.56
    return w * size


def bluetooth(x, y, colour, size=20):
    """Font Awesome's bluetooth-b, the rune LV_SYMBOL_BLUETOOTH draws; (x, y)
    is the left end of the baseline."""
    k = size / 20
    return (f'<path transform="translate({x:.1f},{y - 15 * k:.1f}) scale({k:.3f})" '
            f'd="M1,4 L9,12 L5,16 L5,0 L9,4 L1,12" fill="none" stroke="{colour}" '
            f'stroke-width="1.8" stroke-linejoin="round" stroke-linecap="round"/>')


def microphone28(cx, top, colour, slash=False, bg="#000000"):
    """font_btmic_28's microphone (U+F130), or struck through (U+F131),
    centred on cx with its glyph's top at `top`: microphone() twice over."""
    s = f'<g transform="translate({cx - 10:.1f},{top:.1f}) scale(2)">{microphone(0, 12, colour)}</g>'
    if slash:
        # The stroke cuts the microphone, with a gap of the slab's colour
        # along it, as Font Awesome's microphone-slash does.
        s += (f'<line x1="{cx - 15:.1f}" y1="{top - 1:.1f}" x2="{cx + 15:.1f}" y2="{top + 27:.1f}" '
              f'stroke="{bg}" stroke-width="7" stroke-linecap="round"/>'
              f'<line x1="{cx - 15:.1f}" y1="{top - 1:.1f}" x2="{cx + 15:.1f}" y2="{top + 27:.1f}" '
              f'stroke="{colour}" stroke-width="3" stroke-linecap="round"/>')
    return s


def headset_slab(name, muted=False, raise_boom=False, tx=False):
    """The PTT slab while a Bluetooth headset is connected (ui.c
    headset_slab): the Bluetooth rune and the headset's name, Montserrat 20,
    its box's top at PTT_TOP + 12; under it the microphone (font_btmic_28,
    its box's top at PTT_TOP + 44) -- struck through, in red, while the
    headset has it muted -- or, with the boom arm as the PTT and the headset
    come with it down, a red RAISE BOOM button. On the air the slab is red and
    all of it white."""
    fill = TX_RED if tx else BG1
    fg = "#FFFFFF" if tx else TEXT
    s = (f'<rect x="0" y="{PTT_TOP}" width="360" height="{360 - PTT_TOP}" fill="{fill}"/>'
         f'<line x1="0" y1="{PTT_TOP + 1}" x2="360" y2="{PTT_TOP + 1}" '
         f'stroke="{ACCENT}" stroke-width="2"/>')
    rune, gap = 13, 11                      # the glyph's advance, then two spaces
    x0 = 180 - (rune + gap + approx_width(name, 20)) / 2
    base = PTT_TOP + 12 + 18
    name = name.replace("&", "&amp;").replace("<", "&lt;")
    s += bluetooth(x0, base, fg) + text(x0 + rune + gap, base, name, 20, fg, anchor="start")
    if raise_boom:
        pw, top = approx_width("RAISE BOOM", 20) + 36, PTT_TOP + 42
        s += (f'<rect x="{180 - pw / 2:.1f}" y="{top}" width="{pw:.1f}" height="34" rx="17" '
              f'fill="{TX_RED}"/>' + text(180, top + 6 + 18, "RAISE BOOM", 20, "#FFFFFF"))
    else:
        col = "#FFFFFF" if tx else TX_RED if muted else TEXT2
        s += microphone28(180, PTT_TOP + 46, col, muted, fill)
    return s


def ptt_slab(fill, label, text_colour):
    """The slab carries a 2 px accent border along its top edge only. The
    label is Montserrat 28 in a box whose top is PTT_TOP + 14; that font's
    baseline sits 25 px down. "TX  REMOTE" has two spaces on the device."""
    return (f'<rect x="0" y="{PTT_TOP}" width="360" height="{360 - PTT_TOP}" '
            f'fill="{fill}"/>'
            f'<line x1="0" y1="{PTT_TOP + 1}" x2="360" y2="{PTT_TOP + 1}" '
            f'stroke="{ACCENT}" stroke-width="2"/>'
            + text(180, PTT_TOP + 14 + 25, label, 28, text_colour, 500,
                   extra=' xml:space="preserve"'))


# --- the body and the annotation around it ---------------------------------

def body_head(title):
    """Canvas, then the anodised bezel and the black cover glass. The face is
    drawn in its own 360 px coordinates, shifted onto the glass."""
    W, H = SIZE, SIZE + DIM_H
    return (
        f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {W:.0f} {H:.0f}" '
        f'width="{W:.0f}" height="{H:.0f}" role="img" aria-label="{title}">'
        f'<title>{title}</title><defs>'
        f'<clipPath id="glass"><circle cx="180" cy="180" r="180"/></clipPath>'
        # Anodised aluminium: lit from the top left, the way the product
        # photographs are.
        f'<linearGradient id="alu" x1="0.15" y1="0.05" x2="0.85" y2="0.95">'
        f'<stop offset="0" stop-color="#4F8BF5"/><stop offset="0.45" stop-color="#2A5FDB"/>'
        f'<stop offset="1" stop-color="#16389A"/></linearGradient>'
        f'<linearGradient id="chamfer" x1="0.2" y1="0" x2="0.8" y2="1">'
        f'<stop offset="0" stop-color="#9CC0FF"/><stop offset="1" stop-color="#1B3F9E"/>'
        f'</linearGradient>'
        f'<linearGradient id="gloss" x1="0" y1="0" x2="0.6" y2="0.7">'
        f'<stop offset="0" stop-color="#FFFFFF" stop-opacity="0.10"/>'
        f'<stop offset="0.5" stop-color="#FFFFFF" stop-opacity="0"/></linearGradient>'
        f'<marker id="arrow" viewBox="0 0 10 10" refX="10" refY="5" markerWidth="8" '
        f'markerHeight="8" orient="auto-start-reverse"><path d="M0,1 L10,5 L0,9 z" '
        f'fill="{ANNOT}"/></marker>'
        f'</defs>'
        f'<circle cx="{OX:.1f}" cy="{OY:.1f}" r="{BODY_R:.1f}" fill="url(#alu)"/>'
        f'<circle cx="{OX:.1f}" cy="{OY:.1f}" r="{BODY_R - 1:.1f}" fill="none" '
        f'stroke="#0E2A74" stroke-width="2"/>'
        f'<circle cx="{OX:.1f}" cy="{OY:.1f}" r="{GLASS_R + 2.5:.1f}" fill="none" '
        f'stroke="url(#chamfer)" stroke-width="3" opacity="0.8"/>'
        f'<circle cx="{OX:.1f}" cy="{OY:.1f}" r="{GLASS_R:.1f}" fill="#060709"/>'
        f'<g transform="translate({OX - CX:.1f},{OY - CY:.1f})">'
        f'<g clip-path="url(#glass)">')


def face_end():
    """Closes the glass clip and the shift that body_head() opened."""
    return '</g></g>'


def body_tail():
    return (f'<circle cx="{OX:.1f}" cy="{OY:.1f}" r="{GLASS_R:.1f}" '
            f'fill="url(#gloss)"/>' + dimension() + '</svg>')


def outside(deg, lab, colour=ANNOT):
    """A scale mark carried out past the body: a short leader in line with the
    tick on the glass, and its value beyond it."""
    x0, y0 = pt(deg, BODY_R + 4, OX, OY)
    x1, y1 = pt(deg, BODY_R + 12, OX, OY)
    xt, yt = pt(deg, BODY_R + 26, OX, OY)
    return (f'<line x1="{x0:.1f}" y1="{y0:.1f}" x2="{x1:.1f}" y2="{y1:.1f}" '
            f'stroke="{colour}" stroke-width="1.5" stroke-linecap="round"/>'
            + text(xt, yt + 5, lab, 14, colour, 600))


def caption(deg, lab):
    """A word for a whole arc, set clear of the body. Anchored on the side
    away from it, or a centred caption leans back over the bezel."""
    c = math.cos(math.radians(deg))
    anchor = "start" if c > 0.3 else "end" if c < -0.3 else "middle"
    x, y = pt(deg, BODY_R + (26 if anchor == "middle" else 14), OX, OY)
    return text(x, y + 5, lab, 13, ANNOT, 400, anchor,
                extra=' font-style="italic"')


def dimension():
    """66 mm across, drawn the way Waveshare's outline drawing does it; the
    height rides along in the label because a top view cannot show it."""
    y = OY + BODY_R + 30
    xl, xr = OX - BODY_R, OX + BODY_R
    label = "Ø 66 mm · 22 mm high"
    half_gap = 86
    return (f'<g stroke="{ANNOT}" stroke-width="1.2">'
            # Started clear of the scale labels that sit just below the
            # horizontal on either side.
            f'<line x1="{xl:.1f}" y1="{OY + 70:.1f}" x2="{xl:.1f}" y2="{y + 7:.1f}"/>'
            f'<line x1="{xr:.1f}" y1="{OY + 70:.1f}" x2="{xr:.1f}" y2="{y + 7:.1f}"/>'
            f'<line x1="{OX - half_gap:.1f}" y1="{y:.1f}" x2="{xl:.1f}" y2="{y:.1f}" '
            f'marker-end="url(#arrow)"/>'
            f'<line x1="{OX + half_gap:.1f}" y1="{y:.1f}" x2="{xr:.1f}" y2="{y:.1f}" '
            f'marker-end="url(#arrow)"/></g>'
            + text(OX, y + 5, label, 14, ANNOT, 600))


# --- the two faces --------------------------------------------------------

def rx_face():
    s = [body_head("VFO-Knob receiving")]
    s.append(f'<circle cx="180" cy="180" r="180" fill="{BG}"/>')
    s.append(f'<path d="{arc_path(ARC_ROT, ARC_ROT + ARC_SPAN, RC)}" fill="none" '
             f'stroke="{SUBTLE}" stroke-width="{BAND}"/>')
    for lo, hi, col in RXZONES:
        s.append(block(ARC_ROT + smeter_frac(lo) * ARC_SPAN,
                       ARC_ROT + smeter_frac(hi) * ARC_SPAN, col))
    # A signal that rises through S9, peaks at S9+20 and fades back down to the
    # noise, the way a real one does. Starts and ends on the same value so the
    # loop has no seam.
    lvl = [-86, -80, -71, -61, -52, -58, -66, -79, -94, -108, -116, -104, -95, -86]
    s.append(sweep_cover(ARC_ROT, ARC_ROT + ARC_SPAN,
                         [smeter_frac(d) for d in lvl], "6s"))
    for d in RXNOTCH:
        s.append(notch(ARC_ROT + smeter_frac(d) * ARC_SPAN))
    for d, ln, _ in RXTICKS:
        col = TEXT2 if d == -73 else (WARN if d > -73 else LABEL)
        s.append(tick(ARC_ROT + smeter_frac(d) * ARC_SPAN, ln, col,
                      3 if d == -73 else 2))
    s.append(stepped(180, 83, [smeter_text(d) for d in lvl], "6s", 20, TEXT, 700))
    s.append(stepped(180, 103, [f"{d} dBm" for d in lvl], "6s", 14, LABEL))
    s.append(text(CX - 76, 129, "40m", 20, ACCENT))
    s.append(text(CX, 129, "LSB", 20, TEXT))
    s.append(text(CX + 76, 129, "2800", 20, TEXT2))
    # 7.161.73 -- three MHz digits with two blanked, three kHz, two Hz.
    s.append(readout("  7161" "73", cycle_idx=6,
                     cycle_vals=list("7890123456"), underline=6, after_colour=TEXT2))
    s.append(text(CX - 98, 227, "100 Hz", 20, ACCENT))
    # RIT is amber only when it is set; at zero it is greyed out.
    s.append(text(CX - 24, 227, "RIT 0", 14, DISABLED))
    s.append(icon_readout(CX + 42, 227, speaker, "40", TEXT2))
    s.append(icon_readout(CX + 104, 227, microphone, "100", TEXT2))
    s.append(ptt_slab(BG1, "PTT", TEXT2))
    s.append(face_end())
    # The device draws the S-meter scale as bare ticks -- labels at this
    # diameter collided with the band row -- so the values live out here.
    for d, _, lab in RXTICKS:
        s.append(outside(ARC_ROT + smeter_frac(d) * ARC_SPAN, lab,
                         ANNOT_HOT if d > -73 else ANNOT))
    s.append(body_tail())
    return "".join(s)


def tx_face():
    s = [body_head("VFO-Knob transmitting")]
    s.append(f'<circle cx="180" cy="180" r="180" fill="{BG_TX}"/>')
    s.append(f'<circle cx="180" cy="180" r="178" fill="none" stroke="{TX_RED}" '
             f'stroke-width="4"/>')
    # SWR across the left half.
    s.append(f'<path d="{arc_path(SWR_ROT, SWR_ROT + SWR_SPAN, RC)}" fill="none" '
             f'stroke="{SUBTLE}" stroke-width="{BAND}"/>')
    for lo, hi, col in SWRZONES:
        s.append(block(SWR_ROT + swr_frac(lo) * SWR_SPAN,
                       SWR_ROT + swr_frac(hi) * SWR_SPAN, col))
    swr = [1.3, 1.2, 1.4, 1.3, 1.2, 1.5, 1.3, 1.3]
    s.append(sweep_cover(SWR_ROT, SWR_ROT + SWR_SPAN, [swr_frac(v) for v in swr], "5s"))
    for v in (1.5, 2.0, 2.5):
        s.append(notch(SWR_ROT + swr_frac(v) * SWR_SPAN))
    # ui.c add_tx_ticks(): the SWR scale is numbered on the glass.
    for v, lab, kind in SWRTICKS:
        a = SWR_ROT + swr_frac(v) * SWR_SPAN
        col = DANGER if kind == 2 else WARN if kind == 1 else LABEL
        s.append(tick(a, 10 if lab else 6, col, 3 if lab else 2))
        if lab:
            x, y = pt(a, 128)
            s.append(text(x, y + 5, lab, 14, col))
    # Forward power across the right half, auto-ranged to 100 W, with the
    # range's pegs numbered on the glass the way pwr_set_range() does.
    s.append(f'<path d="{arc_path(AUD_ROT, AUD_ROT + AUD_SPAN, RC)}" fill="none" '
             f'stroke="{SUBTLE}" stroke-width="{BAND}"/>')
    pwr = [.87, .62, .88, .70, .95, .81, .55, .70, .87]
    # One colour all the way up, the RF.Guru logo's gold. (ui.c: C_BRAND)
    s.append(block(AUD_ROT, AUD_ROT + AUD_SPAN, PWR))
    s.append(sweep_cover(AUD_ROT, AUD_ROT + AUD_SPAN, pwr, "5s"))
    for w, _ in PWR_PEGS:
        if w < PWR_FS:
            s.append(notch(AUD_ROT + w / PWR_FS * AUD_SPAN))
    for w, lab in PWR_PEGS:
        a = AUD_ROT + w / PWR_FS * AUD_SPAN
        s.append(tick(a, 9, LABEL))
        x, y = pt(a, 128)
        s.append(text(x, y + 5, lab, 14, LABEL))
    # The mic level is a thin inner ring under the power bar, filling the
    # other way so two bars on the same side are not read as one quantity:
    # -40 dB at the bottom end, +10 dB at the top, green, amber from -10 and
    # red from 0, like the SWR band. (ui.c: MIC_ZONE)
    rc_mic = MIC_R - MIC_BAND / 2
    def mic_angle(db):
        return AUD_ROT + (1 - mic_frac(db)) * AUD_SPAN
    for lo, hi, col in MICZONES:
        s.append(block(mic_angle(lo), mic_angle(hi), col, r=rc_mic, band=MIC_BAND))
    mic = [-16, -9, -22, -3, -13, -26, -7, -15, -16]
    s.append(sweep_cover(AUD_ROT + AUD_SPAN, AUD_ROT, [mic_frac(v) for v in mic],
                         "5s", r=rc_mic, band=MIC_BAND))
    # Both readouts follow their bars. SWR is coloured by its zone, power in
    # the transmit text colour with the auto-range's full scale beside it.
    s.append(stepped(180, 83, [f"SWR {v:.1f}" for v in swr], "5s", 20, TEXT, 700,
                     colours=[DANGER if v >= 2.5 else WARN if v >= 2.0 else TEXT
                              for v in swr]))
    s.append(stepped(180, 103, [f"PWR {round(f * PWR_FS)}W / {PWR_FS}W" for f in pwr],
                     "5s", 14, TX_TEXT))
    s.append(text(CX - 76, 129, "40m", 20, ACCENT))
    s.append(text(CX, 129, "LSB", 20, TEXT))
    s.append(text(CX + 76, 129, "2800", 20, TEXT2))
    s.append(readout("  7161" "73", colour=TX_TEXT, underline=6))
    s.append(text(CX - 98, 227, "100 Hz", 20, ACCENT))
    s.append(text(CX - 24, 227, "RIT 0", 14, DISABLED))
    s.append(icon_readout(CX + 42, 227, speaker, "40", TEXT2))
    s.append(icon_readout(CX + 104, 227, microphone, "100", TEXT2))
    # Solid red while we are transmitting: "TX" in ui.c.
    s.append(ptt_slab(TX_RED, "TX", "#FFFFFF"))
    s.append(face_end())
    # The glass numbers the whole watts and SWR; the half-steps it leaves as
    # bare ticks are named out here, and so is each arc.
    for v, lab, kind in SWRTICKS:
        if not lab:
            s.append(outside(SWR_ROT + swr_frac(v) * SWR_SPAN, f"{v:.1f}",
                             ANNOT_HOT))
    # Up where the canvas has room: beside the body there are only ~46 px.
    s.append(caption(SWR_ROT + SWR_SPAN * 0.50, "SWR"))
    s.append(caption(AUD_ROT + AUD_SPAN * 0.12, "power"))
    s.append(caption(AUD_ROT + AUD_SPAN * 0.47, "mic, inner ring"))
    s.append(body_tail())
    return "".join(s)


def main():
    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    out = os.path.join(here, "docs")
    os.makedirs(out, exist_ok=True)
    for name, svg in (("display-rx.svg", rx_face()), ("display-tx.svg", tx_face())):
        with open(os.path.join(out, name), "w") as f:
            f.write(svg)
        print(f"{name}: {len(svg)} bytes")


if __name__ == "__main__":
    main()
