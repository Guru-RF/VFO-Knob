#!/usr/bin/env python3
"""Draw the pictures for the per-firmware guides in docs/.

Each picture is one screen of the knob, drawn from the firmware's own texts,
positions and palette: the setup screens (ui_setup_show), the chooser panel
(ED_CHOICE), the question panel (ui_ask_turn), the address card and the
update screen (splash.c), in the coordinates ui.c gives them. The body, the
palettes and the text helper are tools/mkdisplay.py's, so the guides and the
README's faces cannot drift apart.

Plain SVG -- no script, no web font, ids prefixed per file -- so a picture
works in the repository, as an <img> on a website, and inline beside others.

    python3 tools/mkdocs.py            every firmware's pictures
    python3 tools/mkdocs.py setup      one firmware's

When a screen changes in the firmware, change it here too and run this; the
guides (docs/<firmware>.md) say what the pictures show.
"""
import functools
import math
import os
import sys

sys.dont_write_bytecode = True             # no __pycache__ left in tools/
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mkdisplay as md  # noqa: E402  (the body, the palettes, text())
import mkrender as mr   # noqa: E402  (the reflector's face and its icons)

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PAD = 44                                  # room around the body for gestures
W = H = 2 * md.BODY_R + 2 * PAD
OX = OY = W / 2
GESTURE = "#E0902A"                       # mkdisplay's ANNOT_HOT: a hand's doing
VERSION = "1.19.0"                        # what the address card says it runs
POWER = "on USB power"                    # ...the knob's own power, under it
ADDRESSES = "USB   -\nWiFi  192.168.1.40\nsetup  http://192.168.1.40"


def esc(s):
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


def lines(cx, cy, text, size, colour, lh, weight=400, anchor="middle"):
    """A multi-line LVGL label centred on (cx, cy): its lines lh apart, the
    first baseline where the font's ascent puts it."""
    rows = text.split("\n")
    top = cy - len(rows) * lh / 2
    return "".join(md.text(cx, top + i * lh + lh * 0.78, esc(r), size, colour, weight, anchor)
                   for i, r in enumerate(rows))


# --- the editor panel's value, fitted as the firmware fits it ------------------

EDIT_W, EDIT_W_NAME, EDIT_H = 250, 290, 132   # components/ui/include/fit_text.h:
                                          # a value's panel, a list of names'
EDIT_PAD = 2 + 16                         # its border, then the theme's padding at 130 dpi
STEPS = (48, 28, 20)                      # fit_text.c: its fonts, largest first


@functools.lru_cache(maxsize=None)
def montserrat(size):
    """LVGL's Montserrat at one size, read from the file the firmware is
    built with (lv_font_montserrat_<size>.c) by mkdisplay's LvglFont -- so
    a width here is the knob's, to the pixel."""
    path = os.path.join(ROOT, "managed_components", "lvgl__lvgl", "src", "font",
                        f"lv_font_montserrat_{size}.c")
    if not os.path.exists(path):
        sys.exit(f"{path} is missing: ESP-IDF's component manager fetches it -- build any "
                 f"firmware once (idf.py build), then run this again")
    return md.LvglFont(path)


def dotted(f, text, room):
    """LV_LABEL_LONG_MODE_DOTS on one line `room` wide, the text centred
    (lv_label.c lv_label_refr_text, lv_label_get_letter_on): the line broken
    at the last letter that fits; on it, the letter under room less three
    dots; there the text ends, in the three dots."""
    n = 0
    while n < len(text) and f.width(text[:n + 1], text[n + 1:]) <= room:
        n += 1
    x = room // 2 - f.width(text[:n], text[n:]) // 2
    at = room - 3 * f.advance(".", ".")
    k = 0
    while k < n - 1:
        gw = f.advance(text[k], text[k + 1:k + 2])
        if at < x + gw:
            break
        x += gw
        k += 1
    k = min(k, len(text) - 3)
    return text[:k] + "..."


def fit(value, size, room):
    """fit_text() (components/ui/fit_text.c): the value in `size` where it is
    at most `room` px wide (EDIT_ROOM: its panel less 5 px each side), or
    else in the first smaller step it fits; too wide even in 20, cut with
    dots there. Returns the size, the text as shown and its width."""
    sizes = (size,) + tuple(s for s in STEPS if s < size)
    for s in sizes:
        w = montserrat(s).width(value)
        if w <= room:
            return s, value, w
    f = montserrat(sizes[-1])
    shown = dotted(f, value, room)
    return sizes[-1], shown, f.width(shown)


# --- the knob ---------------------------------------------------------------



def knob(slug, title, face, notes="", caption=""):
    """The body and the glass, `face` in the screen's own 360 px coordinates,
    `notes` (gesture marks) in the canvas's, and under it all a caption that
    says what the hand does."""
    i = lambda n: f"{slug}-{n}"           # noqa: E731  ids unique per file
    face = face.replace("url(#upd)", f"url(#{i('upd')})")
    notes = notes.replace("url(#arrow)", f"url(#{i('arrow')})")
    h = H
    if caption:                          # just under the body, in its margin
        notes += md.text(OX, OY + md.BODY_R + 30, esc(caption), 16, GESTURE, 700)
    return (
        f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {W:.0f} {h:.0f}" '
        f'width="{W:.0f}" height="{h:.0f}" role="img" aria-label="{esc(title)}">'
        f'<title>{esc(title)}</title><defs>'
        f'<clipPath id="{i("glass")}"><circle cx="180" cy="180" r="180"/></clipPath>'
        f'<linearGradient id="{i("alu")}" x1="0.15" y1="0.05" x2="0.85" y2="0.95">'
        f'<stop offset="0" stop-color="#4F8BF5"/><stop offset="0.45" stop-color="#2A5FDB"/>'
        f'<stop offset="1" stop-color="#16389A"/></linearGradient>'
        f'<linearGradient id="{i("chamfer")}" x1="0.2" y1="0" x2="0.8" y2="1">'
        f'<stop offset="0" stop-color="#9CC0FF"/><stop offset="1" stop-color="#1B3F9E"/>'
        f'</linearGradient>'
        f'<linearGradient id="{i("gloss")}" x1="0" y1="0" x2="0.6" y2="0.7">'
        f'<stop offset="0" stop-color="#FFFFFF" stop-opacity="0.10"/>'
        f'<stop offset="0.5" stop-color="#FFFFFF" stop-opacity="0"/></linearGradient>'
        f'<linearGradient id="{i("upd")}" x1="0" y1="0" x2="0" y2="1">'
        f'<stop offset="0" stop-color="#262015"/><stop offset="1" stop-color="#0C0E13"/>'
        f'</linearGradient>'
        f'<marker id="{i("arrow")}" viewBox="0 0 10 10" refX="7" refY="5" markerWidth="7" '
        f'markerHeight="7" orient="auto-start-reverse"><path d="M0,1 L10,5 L0,9 z" '
        f'fill="{GESTURE}"/></marker>'
        f'</defs>'
        f'<circle cx="{OX:.1f}" cy="{OY:.1f}" r="{md.BODY_R:.1f}" fill="url(#{i("alu")})"/>'
        f'<circle cx="{OX:.1f}" cy="{OY:.1f}" r="{md.BODY_R - 1:.1f}" fill="none" '
        f'stroke="#0E2A74" stroke-width="2"/>'
        f'<circle cx="{OX:.1f}" cy="{OY:.1f}" r="{md.GLASS_R + 2.5:.1f}" fill="none" '
        f'stroke="url(#{i("chamfer")})" stroke-width="3" opacity="0.8"/>'
        f'<circle cx="{OX:.1f}" cy="{OY:.1f}" r="{md.GLASS_R:.1f}" fill="#060709"/>'
        f'<g transform="translate({OX - 180:.1f},{OY - 180:.1f})">'
        f'<g clip-path="url(#{i("glass")})">{face}</g></g>'
        f'<circle cx="{OX:.1f}" cy="{OY:.1f}" r="{md.GLASS_R:.1f}" fill="url(#{i("gloss")})"/>'
        f'{notes}</svg>')


def on_face(x, y):
    """A point of the screen, in the canvas's coordinates."""
    return OX - 180 + x, OY - 180 + y


def turn():
    """A curved arrow along the bezel's right side: the knob turned. The
    caption says what for."""
    r = md.BODY_R + 16
    a0, a1 = -30, 30
    x0, y0 = OX + r * math.cos(math.radians(a0)), OY + r * math.sin(math.radians(a0))
    x1, y1 = OX + r * math.cos(math.radians(a1)), OY + r * math.sin(math.radians(a1))
    return (f'<path d="M{x0:.1f},{y0:.1f} A{r:.1f},{r:.1f} 0 0 1 {x1:.1f},{y1:.1f}" '
            f'fill="none" stroke="{GESTURE}" stroke-width="3.5" stroke-linecap="round" '
            f'marker-start="url(#arrow)" marker-end="url(#arrow)"/>')


def tap(x, y, ring=16):
    """A finger's ring on the screen at (x, y): a tap, or a hold -- the
    caption says which."""
    cx, cy = on_face(x, y)
    return (f'<circle cx="{cx:.1f}" cy="{cy:.1f}" r="{ring}" fill="{GESTURE}" '
            f'fill-opacity="0.22" stroke="{GESTURE}" stroke-width="2.5"/>'
            f'<circle cx="{cx:.1f}" cy="{cy:.1f}" r="4" fill="{GESTURE}"/>')


# --- screens -------------------------------------------------------------------

def setup_screen(title, text, panel=""):
    """ui_setup_show(): over the whole face, the title in the accent at 28 px
    centred 110 px above the middle; the text at 20 px, 280 px wide, centred
    20 px below it -- or 96 px, under a chooser's panel when one is up."""
    return (f'<rect x="0" y="0" width="360" height="360" fill="{md.BG}"/>'
            + lines(180, 70, title, 28, md.ACCENT, 31, 600)
            + lines(180, 180 + (96 if panel else 20), text, 20, md.TEXT, 24)
            + panel)


def edit_panel(title, value, size, hint, colour, wide, beta=False):
    """The editor panel (ui.c, fit_text.h): EDIT_W x EDIT_H for a value, or
    EDIT_W_NAME wide for a list of names (`wide`), centred 6 px above the
    middle; inside its border and the theme's padding, the title at
    20 px in the label colour 2 px from the top, the hint at 14 px 2 px from
    the foot, and between them the value as fit_text() sets it -- in `size`,
    or a smaller step where that is too wide -- centred 4 px below the middle.
    Each line's baseline is where LVGL puts it, and in a browser the value is
    as wide as Montserrat is on the knob, whatever font stands in for it.
    `beta`: the BETA stamp (ui.c s_edit_stamp), 14 px in the danger colour
    in a 2 px box of it, 6 px and 1 px of padding, 10 px in from the content's
    right and 2 px down."""
    pw = EDIT_W_NAME if wide else EDIT_W
    size, shown, w = fit(value, size, pw - 10)
    f, t, k = montserrat(size), montserrat(20), montserrat(14)
    # A label's baseline: its top, plus its line, less the font's base_line.
    top = 174 - EDIT_H // 2 + EDIT_PAD           # the content area: 126, 96 high
    high = EDIT_H - 2 * EDIT_PAD
    t_base = top + 2 + t.line_height - t.base_line                       # TOP_MID 0,2
    v_base = top + high // 2 - f.line_height // 2 + 4 + f.line_height - f.base_line   # CENTER 0,4
    h_base = top + high - 2 - k.base_line                                # BOTTOM_MID 0,-2
    stamp = ""
    if beta:
        sw, sh = k.width("BETA") + 2 * 6 + 2 * 2, k.line_height + 2 * 1 + 2 * 2
        x2 = 180 + pw // 2 - EDIT_PAD - 10                                  # TOP_RIGHT -10,2
        x1, y1 = x2 - sw, top + 2
        stamp = (f'<rect x="{x1 + 1}" y="{y1 + 1}" width="{sw - 2}" height="{sh - 2}" rx="4" '
                 f'fill="none" stroke="{md.DANGER}" stroke-width="2"/>'
                 + md.text((x1 + x2) / 2, y1 + 3 + k.line_height - k.base_line, "BETA", 14, md.DANGER))
    return (f'<rect x="{180 - pw // 2}" y="{174 - EDIT_H // 2}" width="{pw}" '
            f'height="{EDIT_H}" rx="18" fill="{md.BG1}" stroke="{md.ACCENT}" stroke-width="2"/>'
            + md.text(180, t_base, esc(title), 20, md.LABEL)
            + md.text(180, v_base, esc(shown), size, colour, 600,
                      extra=f' textLength="{w}" lengthAdjust="spacingAndGlyphs"')
            + md.text(180, h_base, esc(hint), 14, md.LABEL) + stamp)


def chooser(title, value, hint="turn to choose  -  tap to accept", beta=False):
    """The editor panel as a chooser (ED_CHOICE): the value a name, at 28 px
    where it fits, in the wider panel (edit_panel); a beta's stamped so."""
    return edit_panel(title, value, 28, hint, md.ACCENT_HI, True, beta)


def question(title, hint):
    """ui_ask_turn(): the question panel, 268 x 116, the title at 28 px 16 px
    from its top, the hint at 14 px 14 px from its foot."""
    return (f'<rect x="46" y="116" width="268" height="116" rx="18" fill="{md.BG1}" '
            f'stroke="{md.ACCENT}" stroke-width="2"/>'
            + md.text(180, 156, esc(title), 28, md.ACCENT_HI, 600)
            + lines(180, 200, hint, 14, md.TEXT2, 17))


def address_card(text):
    """The address card: 20 px in the bright accent, 10 px of padding, a
    2 px accent border, centred 6 px above the middle."""
    rows = text.split("\n")
    w = max(len(r) for r in rows) * 11.2 + 20
    h = len(rows) * 24 + 20
    x, y = 180 - w / 2, 174 - h / 2
    return (f'<rect x="{x:.1f}" y="{y:.1f}" width="{w:.1f}" height="{h:.1f}" rx="14" '
            f'fill="{md.BG1}" stroke="{md.ACCENT}" stroke-width="2"/>'
            + lines(180, 174, text, 20, md.ACCENT_HI, 24, 400)
            .replace('font-family="DejaVu Sans', 'xml:space="preserve" font-family="DejaVu Sans'))


def lock_closed(x, y, colour):
    """font_svx_icons_24's lock (U+F023), centred on (x, y): mkrender's
    lock_open, its shackle shut."""
    return (f'<g transform="translate({x - 8:.1f},{y - 11:.1f})">'
            f'<rect x="0" y="9" width="16" height="12" rx="2.5" fill="{colour}"/>'
            f'<path d="M3.5,9.5 V6 a4.5,4.5 0 0 1 9,0 V9.5" fill="none" stroke="{colour}" '
            f'stroke-width="2.6"/></g>')


def lock_pill(locked):
    """The lock's pill (ui.c klock_btn_set), over the S-meter's row with the
    card: 204 x 44 at y 62, its padlock and word centred together -- LOCK in
    the card's colours, UNLOCK in amber with the padlock open."""
    word, col, edge = ("UNLOCK", md.WARN, md.WARN) if locked else ("LOCK", md.ACCENT_HI, md.ACCENT)
    x0 = 180 - (18 + 8 + montserrat(20).width(word)) / 2
    icon = mr.lock_open(x0 + 9, 84, col) if locked else lock_closed(x0 + 8, 84, col)
    return (f'<rect x="79" y="63" width="202" height="42" rx="21" fill="{md.BG1}" '
            f'stroke="{edge}" stroke-width="2"/>'
            + icon + md.text(x0 + 26, 91, word, 20, col, 400, anchor="start"))


def padlock(batt=False):
    """The padlock at the top of the arc while locked (ui.c klock_update), in
    amber, its ink level with the knob's battery's -- left of it when that
    shows."""
    return lock_closed(180 - 27 if batt else 180, 47.5, md.WARN)


def lock_pictures(out, n, slug, pal, name, face_svg, keys=True):
    """The lock, on a firmware's face: the card's LOCK, and the face locked.
    Pictures n and n + 1."""
    md.use_palette(pal)
    card = address_card(f"{name} {VERSION}\n{POWER}\n{ADDRESSES}")
    out[f"{n:02d}-lock"] = knob(f"{slug}-{n:02d}", "The address card's LOCK",
                                face_svg + card + lock_pill(False), tap(252, 84),
                                "hold the S-meter for the card  \u00b7  tap LOCK")
    out[f"{n + 1:02d}-locked"] = knob(f"{slug}-{n + 1:02d}", "Locked: the padlock at the top of the arc",
                                      face_svg + padlock(), "",
                                      "turns, taps, swipes and keying refused  \u00b7  the slab still unkeys"
                                      if keys else "turns, taps and swipes refused")


def update_screen(title, middle, msg, msg_colour="#FF9A3C", percent=None):
    """splash.c's update screen: its own, in the RF.Guru colours -- a glow to
    ink, the ring in gold from the top, the title in gold, the middle in
    white at 48 px, a line under it."""
    s = ['<rect x="0" y="0" width="360" height="360" fill="url(#upd)"/>',
         '<circle cx="180" cy="180" r="145" fill="none" stroke="#1A1D25" stroke-width="10"/>']
    if percent and percent >= 100:
        s.append('<circle cx="180" cy="180" r="145" fill="none" stroke="#E9B61D" stroke-width="10"/>')
    elif percent:
        s.append(md.block(270, 270 + 360 * percent / 100, "#E9B61D", r=145, band=10))
    s += [md.text(180, 125, esc(title), 20, "#ECC34A", 600),
          md.text(180, 191, esc(middle), 48, "#FFFFFF", 600),
          md.text(180, 245, esc(msg), 20, msg_colour, 400)]
    return "".join(s)


def radio_face():
    """A radio firmware's face at rest, as a backdrop: the Icom one, S7 on
    40 m -- the setup firmware is reached from any of them."""
    md.use_palette("icom")
    s = [f'<rect x="0" y="0" width="360" height="360" fill="{md.BG}"/>',
         f'<path d="{md.arc_path(md.ARC_ROT, md.ARC_ROT + md.ARC_SPAN, md.RC)}" fill="none" '
         f'stroke="{md.SUBTLE}" stroke-width="{md.BAND}"/>']
    level = -85
    for lo, hi, col in md.RXZONES:
        if lo >= level:
            break
        s.append(md.block(md.ARC_ROT + md.smeter_frac(lo) * md.ARC_SPAN,
                          md.ARC_ROT + md.smeter_frac(min(hi, level)) * md.ARC_SPAN, col))
    for d in md.RXNOTCH:
        s.append(md.notch(md.ARC_ROT + md.smeter_frac(d) * md.ARC_SPAN))
    s += [md.text(180, 83, "S7", 20, md.TEXT, 700),
          md.text(180, 103, "-85 dBm", 14, md.LABEL),
          md.text(104, 83, "AGC", 14, md.LABEL), md.text(104, 102, "MID", 14, md.TEXT2),
          md.text(256, 83, "P.AMP", 14, md.LABEL), md.text(256, 102, "OFF", 14, md.TEXT2),
          md.text(104, 129, "40m", 20, md.ACCENT), md.text(180, 129, "LSB", 20, md.TEXT),
          md.text(256, 129, "FIL2", 20, md.TEXT2),
          md.readout("  7123" "00", underline=5),
          md.ptt_slab(md.BG1, "PTT", md.TEXT2)]
    md.use_palette("aethersdr")
    return "".join(s)


def dimmed(face):
    """A face behind a panel, as the eye takes it: there, but not the point."""
    return f'<g opacity="0.35">{face}</g>'


# --- a radio firmware's face ---------------------------------------------------

def cover(a0, a1, frac, r=None, band=None):
    """The unreached part of a bar, in the track colour: a still of
    mkdisplay's sweep_cover()."""
    r = r or md.RC
    band = band or md.BAND
    L = md.arc_len(a0, a1, r)
    return (f'<path d="{md.arc_path(a1, a0, r)}" fill="none" stroke="{md.SUBTLE}" '
            f'stroke-width="{band + 0.5}" stroke-dasharray="{L * (1 - frac):.2f} {L + 10:.2f}"/>')


SDR_BLUE = {"icom": "#5A9BFF", "multiflex": "#62BBFF"}   # ui.c SDR_HEX


def face(pal, dbm=-85, band="40m", mode="LSB", filt="FIL2", digits="  7123" "00",
         active=5, step="100 Hz", agc="MID", gain_cap="P.AMP", gain="OFF",
         gain_known=True, sub=None, sdr=None, mem=None, rit="RIT 0", vol="40", mic="100",
         receiver=False, ghz=False, sdr_note=None):
    """A radio firmware's receive face at a reading, in its palette (ui.c;
    mkrender's dial, with the options the guides need): `sub` for what is
    under the S-units, `sdr` a web SDR's level -- its thin line outside the
    radio's, and its reading in blue -- or `sdr_note` the word for why it is
    not heard, in amber where its reading would be, and no line; `mem` =
    (group, name, line) for memory mode. `receiver`: a receiver radio's (the
    IC-R8600), RECEIVER on the slab and no RIT or microphone; `ghz` the
    digits from 1 GHz up."""
    md.use_palette(pal)
    s = [f'<rect x="0" y="0" width="360" height="360" fill="{md.BG}"/>',
         f'<path d="{md.arc_path(md.ARC_ROT, md.ARC_ROT + md.ARC_SPAN, md.RC)}" fill="none" '
         f'stroke="{md.SUBTLE}" stroke-width="{md.BAND}"/>']
    for lo, hi, col in md.RXZONES:
        s.append(md.block(md.ARC_ROT + md.smeter_frac(lo) * md.ARC_SPAN,
                          md.ARC_ROT + md.smeter_frac(hi) * md.ARC_SPAN, col))
    s.append(cover(md.ARC_ROT, md.ARC_ROT + md.ARC_SPAN, md.smeter_frac(dbm)))
    for d in md.RXNOTCH:
        s.append(md.notch(md.ARC_ROT + md.smeter_frac(d) * md.ARC_SPAN))
    for d, ln, _ in md.RXTICKS:
        col = md.TEXT2 if d == -73 else (md.WARN if d > -73 else md.LABEL)
        s.append(md.tick(md.ARC_ROT + md.smeter_frac(d) * md.ARC_SPAN, ln, col, 3 if d == -73 else 2))
    if sdr is not None:
        blue = SDR_BLUE.get(pal, "#4DA6FF")
        s.append(f'<path d="{md.arc_path(md.ARC_ROT, md.ARC_ROT + md.ARC_SPAN, 174)}" fill="none" '
                 f'stroke="{md.SUBTLE}" stroke-width="4"/>')
        s.append(md.block(md.ARC_ROT, md.ARC_ROT + md.smeter_frac(sdr) * md.ARC_SPAN, blue, r=174, band=4))
        sub, sub_colour = md.smeter_text(sdr), blue
    elif sdr_note:
        sub, sub_colour = sdr_note, md.WARN
    else:
        sub, sub_colour = sub or f"{dbm} dBm", md.LABEL
    s += [md.text(180, 83, md.smeter_text(dbm), 20, md.TEXT, 700),
          md.text(180, 103, esc(sub), 14, sub_colour),
          md.aux(agc, gain_cap, gain, gain_known)]
    if mem:
        group, name, line = mem
        s += [md.text(104, 129, group, 20, md.ACCENT), md.text(180, 129, mode, 20, md.TEXT),
              md.text(256, 129, filt, 20, md.TEXT2),
              md.text(180, 170, esc(name), 28, md.TEXT, 600),
              md.text(180, 203, esc(line), 20, md.TEXT2)]
    else:
        s += [md.text(104, 129, band, 20, md.ACCENT), md.text(180, 129, mode, 20, md.TEXT),
              md.text(256, 129, filt, 20, md.TEXT2),
              md.readout(digits, colour=md.TEXT, sep_colour=md.LABEL, underline=active,
                         after_colour=md.TEXT2, active_colour=md.ACCENT_HI, ghz=ghz)]
    if receiver:
        s += [md.text(124, 227, step, 20, md.ACCENT),
              md.icon_readout(236, 227, md.speaker, vol, md.TEXT2),
              md.ptt_slab(md.BG1, "RECEIVER", md.TEXT2)]
        return "".join(s)
    s += [md.text(82, 227, step, 20, md.ACCENT),
          md.text(156, 227, rit, 14, md.WARN if rit != "RIT 0" else md.DISABLED),
          md.icon_readout(222, 227, md.speaker, vol, md.TEXT2),
          md.icon_readout(284, 227, md.microphone, mic, md.TEXT2),
          md.ptt_slab(md.BG1, "PTT", md.TEXT2)]
    return "".join(s)


def tx_face(pal, swr=1.3, watts=50, fs=100, pegs=((10, "10"), (50, "50"), (100, "100")),
            mic_db=-14, band="40m", mode="LSB", filt="FIL2", digits="  7123" "00"):
    """A radio firmware on the air, still: SWR across the left half, forward
    power across the right in its auto-range, the microphone on the thin
    inner ring, the red hairline and the red slab (ui.c; mkdisplay's
    tx_face, frozen)."""
    md.use_palette(pal)
    s = [f'<rect x="0" y="0" width="360" height="360" fill="{md.BG_TX}"/>',
         f'<circle cx="180" cy="180" r="178" fill="none" stroke="{md.TX_RED}" stroke-width="4"/>',
         f'<path d="{md.arc_path(md.SWR_ROT, md.SWR_ROT + md.SWR_SPAN, md.RC)}" fill="none" '
         f'stroke="{md.SUBTLE}" stroke-width="{md.BAND}"/>']
    for lo, hi, col in md.SWRZONES:
        s.append(md.block(md.SWR_ROT + md.swr_frac(lo) * md.SWR_SPAN,
                          md.SWR_ROT + md.swr_frac(hi) * md.SWR_SPAN, col))
    s.append(cover(md.SWR_ROT, md.SWR_ROT + md.SWR_SPAN, md.swr_frac(swr)))
    for v in (1.5, 2.0, 2.5):
        s.append(md.notch(md.SWR_ROT + md.swr_frac(v) * md.SWR_SPAN))
    for v, lab, kind in md.SWRTICKS:
        a = md.SWR_ROT + md.swr_frac(v) * md.SWR_SPAN
        col = md.DANGER if kind == 2 else md.WARN if kind == 1 else md.LABEL
        s.append(md.tick(a, 10 if lab else 6, col, 3 if lab else 2))
        if lab:
            x, y = md.pt(a, 128)
            s.append(md.text(x, y + 5, lab, 14, col))
    s.append(f'<path d="{md.arc_path(md.AUD_ROT, md.AUD_ROT + md.AUD_SPAN, md.RC)}" fill="none" '
             f'stroke="{md.SUBTLE}" stroke-width="{md.BAND}"/>')
    s.append(md.block(md.AUD_ROT, md.AUD_ROT + md.AUD_SPAN, md.PWR))
    s.append(cover(md.AUD_ROT, md.AUD_ROT + md.AUD_SPAN, watts / fs))
    for w, lab in pegs:
        a = md.AUD_ROT + w / fs * md.AUD_SPAN
        if w < fs:
            s.append(md.notch(a))
        s.append(md.tick(a, 9, md.LABEL))
        x, y = md.pt(a, 128)
        s.append(md.text(x, y + 5, lab, 14, md.LABEL))
    rc_mic = md.MIC_R - md.MIC_BAND / 2
    ang = lambda db: md.AUD_ROT + (1 - md.mic_frac(db)) * md.AUD_SPAN   # noqa: E731
    for lo, hi, col in md.MICZONES:
        s.append(md.block(ang(lo), ang(hi), col, r=rc_mic, band=md.MIC_BAND))
    L = md.arc_len(md.AUD_ROT, md.AUD_ROT + md.AUD_SPAN, rc_mic)
    s.append(f'<path d="{md.arc_path(md.AUD_ROT, md.AUD_ROT + md.AUD_SPAN, rc_mic)}" fill="none" '
             f'stroke="{md.SUBTLE}" stroke-width="{md.MIC_BAND + 0.5}" '
             f'stroke-dasharray="{L * (1 - md.mic_frac(mic_db)):.2f} {L + 10:.2f}"/>')
    s += [md.text(180, 83, f"SWR {swr:.1f}", 20,
                  md.DANGER if swr >= 2.5 else md.WARN if swr >= 2.0 else md.TEXT, 700),
          md.text(180, 103, f"PWR {watts}W / {fs}W", 14, md.TX_TEXT),
          md.text(104, 129, band, 20, md.ACCENT), md.text(180, 129, mode, 20, md.TEXT),
          md.text(256, 129, filt, 20, md.TEXT2),
          md.readout(digits, colour=md.TX_TEXT, sep_colour=md.LABEL, underline=5),
          md.ptt_slab(md.TX_RED, "TX", "#FFFFFF")]
    return "".join(s)


def reflector(pal="svxconnect", **reading):
    """The svxconnect firmware's face (mkrender's reflector_dial)."""
    md.use_palette(pal)
    R = dict(dbfs=-14, tg=8, tg_name="70cm Repeaters", server="be.svx.link",
             talker="ON6URE", talking="14s")
    R.update(reading)
    return mr.reflector_dial(R)


# The editors that are lists of names (ui.c edit_render): the wider panel,
# whatever font their value is in -- RX on LOCAL too, and a Kiwi's RIGHT EAR
# on OFF. Told by the title's first word, as the knob tells them by the
# editor -- but not the antennas' (RX ANT, TX ANT), a value's panel.
NAME_LISTS = ("RX", "RADIO", "RECEIVER", "RIGHT", "SPOT", "SSTV", "CALLS")


def editor(title, value, size=48, hint="turn to choose  -  tap to accept", colour=None):
    """The editor panel (ui.c edit_render): the value at 48 px -- 28 for
    names -- where it fits (edit_panel), in the wider panel for a list of
    names (NAME_LISTS); V/M, at 28, keeps a value's."""
    return edit_panel(title, value, size, hint, colour or md.ACCENT_HI,
                      title.split()[0] in NAME_LISTS and not title.endswith(" ANT"))


def hold_slab():
    """A finger held on the slab: a wider ring than a tap's -- the caption
    says how long."""
    return tap(180, 300, ring=24)


def warning(title, fw, net=ADDRESSES, name=None):
    """The warning panel: 268 x 134 in the danger colour, the warning at
    28 px, and under it at 14 px the address card's text -- the firmware and
    its version (`fw`, e.g. "Icom"), the knob's power, then the addresses,
    16 px a line, the last 10 px over the panel's foot as on the knob -- or,
    fw None, a message in its place. `name`, the radio it is about, where
    the firmware names one (ui.c warn_name: the UberSDR firmware's receiver,
    with more than one in its list): a line between them at 20 px in the
    face's text colour, the panel 26 px taller, about the same middle."""
    grow = 26 if name else 0
    top = 107 - grow // 2
    if not fw:
        text = lines(180, 199 + grow // 2, net, 14, md.TEXT2, 17)
    else:
        rows = f"{fw} {VERSION}\n{POWER}\n{net}"
        text = lines(180, 231 + grow // 2 - 8 * len(rows.split("\n")), rows, 14, md.TEXT2, 16)
    if name:
        text = lines(180, top + 52.5, name, 20, md.TEXT, 22) + text
    return (f'<rect x="46" y="{top}" width="268" height="{134 + grow}" rx="18" fill="{md.BG1}" '
            f'stroke="{md.DANGER}" stroke-width="2"/>'
            + md.text(180, top + 38, esc(title), 28, md.DANGER, 600) + text)


def swipe(direction):
    """A finger's swipe across the glass: the way it goes."""
    a, b = {"down": ((180, 70), (180, 190)), "up": ((180, 190), (180, 70)),
            "right": ((60, 180), (300, 180)), "left": ((300, 180), (60, 180))}[direction]
    (x0, y0), (x1, y1) = on_face(*a), on_face(*b)
    return (f'<line x1="{x0:.1f}" y1="{y0:.1f}" x2="{x1:.1f}" y2="{y1:.1f}" stroke="{GESTURE}" '
            f'stroke-width="7" stroke-linecap="round" stroke-opacity="0.85" marker-end="url(#arrow)"/>'
            f'<circle cx="{x0:.1f}" cy="{y0:.1f}" r="11" fill="{GESTURE}" fill-opacity="0.35" '
            f'stroke="{GESTURE}" stroke-width="2"/>')


def callouts(slug, title, face_svg, left, right):
    """The knob with its parts named either side, a leader to each: `left`
    and `right` are (x, y in the screen's coordinates, label)."""
    side = 210
    parts = []

    def column(items, anchor):
        last = -1e9
        for x, y, label in sorted(items, key=lambda t: t[1]):
            px, py = on_face(x, y)
            ty = max(py + 5, last + 24)
            last = ty
            tx = OX - md.BODY_R - 16 if anchor == "end" else OX + md.BODY_R + 16
            lx = tx + (4 if anchor == "end" else -4)
            parts.append(f'<line x1="{lx:.1f}" y1="{ty - 5:.1f}" x2="{px:.1f}" y2="{py:.1f}" '
                         f'stroke="{md.ANNOT}" stroke-width="1.2"/>'
                         f'<circle cx="{px:.1f}" cy="{py:.1f}" r="3.5" fill="{GESTURE}" '
                         f'stroke="#FFFFFF" stroke-width="1"/>'
                         + md.text(tx, ty, esc(label), 14, md.ANNOT, 600, anchor))

    column(left, "end")
    column(right, "start")
    svg = knob(slug, title, face_svg, "".join(parts))
    return svg.replace(f'viewBox="0 0 {W:.0f} {H:.0f}" width="{W:.0f}"',
                       f'viewBox="{-side:.0f} 0 {W + 2 * side:.0f} {H:.0f}" '
                       f'width="{W + 2 * side:.0f}"', 1)


# --- the phone, for the hotspot's page ----------------------------------------

PHONE_W, PHONE_H = 330, 660


def phone(slug, title, content):
    """A phone, the page in it at its CSS sizes: portal.html's colours."""
    return (
        f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {PHONE_W} {PHONE_H}" '
        f'width="{PHONE_W}" height="{PHONE_H}" role="img" aria-label="{esc(title)}">'
        f'<title>{esc(title)}</title>'
        f'<rect x="1" y="1" width="{PHONE_W - 2}" height="{PHONE_H - 2}" rx="42" fill="#1C1F26" '
        f'stroke="#3A3F4B" stroke-width="2"/>'
        f'<rect x="14" y="14" width="{PHONE_W - 28}" height="{PHONE_H - 28}" rx="30" fill="#0b0f14"/>'
        f'<rect x="{PHONE_W / 2 - 42:.0f}" y="24" width="84" height="18" rx="9" fill="#1C1F26"/>'
        f'<g font-family="system-ui,-apple-system,Segoe UI,Roboto,sans-serif">{content}</g>'
        f'</svg>')


def ptext(x, y, s, size, colour, weight=400, anchor="start"):
    return (f'<text x="{x}" y="{y}" font-size="{size}" fill="{colour}" font-weight="{weight}" '
            f'text-anchor="{anchor}">{esc(s)}</text>')


def portal(joined=False):
    """portal.html as a phone shows it: the knob's hotspot page. Its own
    colours (--bg #0b0f14, --card #141b24, --acc #e5a823 ...)."""
    x0, y = 30, 78
    s = [ptext(x0, y, "VFO-Knob", 22, "#e8eef5", 700)]
    y += 26
    for row in ("Choose the WiFi network the knob should", "join. Once it is on it, the knob shows the",
                "firmwares to choose from, on its own dial."):
        s.append(ptext(x0, y, row, 13, "#8a99aa"))
        y += 18
    # The networks it found, one chosen.
    y += 10
    s.append(f'<rect x="24" y="{y}" width="282" height="186" rx="14" fill="#141b24" stroke="#233040"/>')
    nets = (("HomeNetwork", True, 4, True), ("Upstairs", True, 3, False), ("Neighbours", True, 2, False))
    yy = y + 14
    for name, locked, bars, sel in nets:
        s.append(f'<rect x="36" y="{yy}" width="258" height="40" rx="10" fill="#0f151d" '
                 f'stroke="{"#e5a823" if sel else "#233040"}"/>')
        s.append(ptext(48, yy + 25, name + (" \U0001F512" if locked else ""), 14, "#e8eef5"))
        s.append(ptext(282, yy + 25, "●" * bars, 11, "#8a99aa", anchor="end"))
        yy += 48
    s.append(ptext(36, yy + 12, "Look again", 13, "#e5a823"))
    y += 200
    # The network and its password.
    s.append(f'<rect x="24" y="{y}" width="282" height="{250 if joined else 206}" rx="14" '
             f'fill="#141b24" stroke="#233040"/>')
    s.append(ptext(36, y + 26, "Network", 13, "#8a99aa"))
    s.append(f'<rect x="36" y="{y + 34}" width="258" height="40" rx="10" fill="#0f151d" stroke="#233040"/>')
    s.append(ptext(48, y + 59, "HomeNetwork", 14, "#e8eef5"))
    s.append(ptext(36, y + 94, "Password", 13, "#8a99aa"))
    s.append(f'<rect x="36" y="{y + 102}" width="258" height="40" rx="10" fill="#0f151d" stroke="#233040"/>')
    s.append(ptext(48, y + 127, "•" * 10, 14, "#e8eef5"))
    s.append(ptext(282, y + 127, "Show", 13, "#e5a823", 600, "end"))
    s.append(f'<rect x="36" y="{y + 152}" width="258" height="42" rx="10" fill="#e5a823" '
             f'fill-opacity="{0.5 if joined else 1}"/>')
    s.append(ptext(165, y + 179, "Connect", 15, "#111111", 600, "middle"))
    if joined:
        s.append(ptext(36, y + 216, "Connected to HomeNetwork.", 13, "#2ea043", 600))
        s.append(ptext(36, y + 234, "Now choose the firmware on the knob's dial.", 12, "#e8eef5"))
    return "".join(s)


# --- the setup firmware ------------------------------------------------------

def setup_pictures():
    md.use_palette("svxconnect")         # the setup firmware wears SVXConnect's ink and gold
    net = "HomeNetwork"
    out = {}
    out["01-starting"] = knob("setup-01", "The setup firmware starting",
                              setup_screen("VFO-KNOB", "Starting"))
    out["02-hotspot"] = knob("setup-02", "The knob asks to be joined on its hotspot",
                             setup_screen("WIFI SETUP", "Join the WiFi network\nVFOKnob\nwith your phone, then\n"
                                                        "choose your network on\nthe page that opens."))
    out["03-portal"] = phone("setup-03", "The knob's WiFi page on a phone", portal())
    out["04-joining"] = knob("setup-04", "The knob joining the network",
                             setup_screen("WIFI SETUP", f"Joining\n{net}"))
    out["05-connected"] = knob("setup-05", "The knob on the network",
                               setup_screen("WIFI SETUP", f"Connected to\n{net}"))
    out["06-portal-joined"] = phone("setup-06", "The phone's page once the knob has joined", portal(True))
    out["07-firmwares"] = knob(
        "setup-07", "The firmwares published for the knob, one a detent",
        setup_screen("FIRMWARE", "Turn to your radio,\nthen tap to install.",
                     chooser("INSTALL", f"SVXConnect {VERSION}")),
        turn() + tap(282, 150), "turn to choose  \u00b7  tap the panel to install")
    out["08-wifi-again"] = knob(
        "setup-08", "The last choice: another network",
        setup_screen("FIRMWARE", "Turn to your radio,\nthen tap to install.",
                     chooser("WIFI", "Set up again")),
        turn(), "the last choice: another network")
    out["09-installing"] = knob("setup-09", "The firmware installing",
                                update_screen("UPDATING", "64%", "Do not unplug", percent=64))
    out["10-restarting"] = knob("setup-10", "Installed: the knob restarts into it",
                                update_screen("UPDATING", "100%", "Restarting", "#4DD87A", percent=100))
    md.use_palette("aethersdr")          # a radio's firmware, in its own face
    face = radio_face()
    out["11-address-card"] = knob(
        "setup-11", "A radio firmware: the address card, held up on the S-meter",
        dimmed(face) + address_card(f"Icom {VERSION}\n{POWER}\n{ADDRESSES}"),
        tap(180, 40), "hold the S-meter until it clicks")
    out["12-firmware-question"] = knob(
        "setup-12", "Held again three seconds: back to the setup firmware?",
        dimmed(face) + question("FIRMWARE?", "turn the knob for the picker\ntap to cancel; WiFi is kept"),
        turn(), "hold again 3 s until it buzzes, then turn")
    out["13-rebooting"] = knob("setup-13", "Yes: the knob restarts into the setup firmware",
                               update_screen("REBOOTING", "⟳", "into update mode"))
    # A radio's firmware, taken where none of its networks reaches (app_main.c
    # wifi_setup), in that firmware's own colours.
    md.use_palette("icom")
    out["14-away"] = knob("setup-14", "A radio's firmware with none of its networks in reach",
                          setup_screen("WIFI SETUP", "None of its networks\nis in reach. Join\n"
                                                     "VFOKnob with your\nphone to add one,\n"
                                                     "or wait: it keeps looking."))
    md.use_palette("svxconnect")
    # The knob's own battery, while it runs on it (ui.c knob_batt_show): over
    # each screen's title -- the list of firmwares', up for minutes, here.
    out["15-battery"] = knob("setup-15", "On its own battery: the knob's charge over the title",
                             setup_screen("FIRMWARE", "Turn to your radio,\nthen tap to install.",
                                          chooser("INSTALL", f"SVXConnect {VERSION}")) + md.knob_battery(85),
                             "", "unplugged: its charge, by the quarter  \u00b7  none on USB power")
    # The settings' SD card (components/kvstore), on a radio's face: the card
    # that holds them does not answer, or answers wrong (app_main.c ui_task).
    icom = radio_face()
    out["16-no-sd-card"] = knob("setup-16", "NO SD CARD: the card that holds the settings does not answer",
                                icom + warning("NO SD CARD", "Icom"))
    out["17-sd-card-fault"] = knob("setup-17", "SD CARD FAULT: part of the settings could not be read",
                                   icom + warning("SD CARD FAULT", "Icom"))
    # Three starts in a row that never got 20 s in: WiFi and audio off, the
    # card left alone, and no address to give (app_main.c, s_safe_mode).
    out["18-safe-mode"] = knob("setup-18", "SAFE MODE: three failed starts in a row",
                               icom + warning("SAFE MODE", "Icom", "USB   -\nWiFi  -\nsetup  http://-"))
    md.use_palette("svxconnect")
    # The setup firmware's own, once the count has cleared 20 s in
    # (app_main.c setup_safe_task): its setup task does not start in it.
    out["19-setup-safe-mode"] = knob("setup-19", "SAFE MODE on the setup firmware: restart it",
                                     setup_screen("SAFE MODE", "Three starts in a row\nended within 20 s.\n"
                                                  "Restart the knob:\nit starts as usual."))
    # A firmware published as a beta (tools/release.sh BETA), stamped so.
    out["20-beta"] = knob(
        "setup-20", "A firmware still in beta, stamped BETA",
        setup_screen("FIRMWARE", "Turn to your radio,\nthen tap to install.",
                     chooser("INSTALL", f"EpiBeam {VERSION}", beta=True)),
        turn(), "a beta: stamped BETA in the panel's corner")
    return out


# --- the radios' firmwares ------------------------------------------------------

def radio_parts(gain="P.AMP", hold=None):
    """The face's parts, named: the left column and the right one -- and,
    `hold`, what a press held on the slab opens."""
    # Each point at the edge of its part, the dot never on the words.
    left = [(64, 64, "S-meter · hold: the addresses"), (84, 77, "AGC"), (84, 123, "band"),
            (58, 172, "frequency · tap a digit: its step"), (54, 221, "tuning step"),
            (112, 300, "PTT · tap on, tap off")]
    right = [(204, 77, "S-units, dBm under"), (286, 77, gain), (204, 123, "mode"),
             (282, 123, "filter"), (176, 222, "RIT"), (244, 222, "volume"), (306, 222, "mic gain")]
    if hold:
        right.append((248, 300, hold))
    return left, right


def sdr_pictures(pal, slug, reading):
    """A web SDR beside the radio: the dial's RX, the SDR's S-meter, the
    balance. `reading` is the face's."""
    out = {}
    out["rx"] = knob(f"{slug}-rx", "Swipe down: LOCAL or a web SDR",
                     face(pal, **reading) + editor("RX", "Web-888", 28), swipe_at("down"),
                     "swipe down  \u00b7  turn to LOCAL or a receiver  \u00b7  tap the panel")
    out["sdr"] = knob(f"{slug}-sdr", "A web SDR playing: its S-meter and reading in blue",
                      face(pal, sdr=-97, **reading), "",
                      "the SDR: the thin blue line, its S-units in blue")
    out["balance"] = knob(f"{slug}-balance", "BALANCE: the radio left, the SDR right",
                          face(pal, sdr=-97, **reading) + editor("BALANCE", "L | R"),
                          swipe_at("right") + turn(),
                          "swipe from the left  \u00b7  turn: RADIO ... L | R ... SDR")
    return out


def swipe_at(direction, panel=True):
    """A swipe drawn clear of a panel: across the S-meter, or down the edge.
    Down or up beside the editor panel the glass leaves less room: there it
    is shorter and thinner, its head too, between the panel and the glass's
    edge. With no panel (`panel` False) it runs down the edge as it did."""
    width = 6
    if direction in ("right", "left"):
        a, b = ((70, 62), (290, 62)) if direction == "right" else ((290, 62), (70, 62))
    else:
        if panel:
            x, top, foot, width = 180 + EDIT_W_NAME // 2 + 14, 124, 236, 4
        else:
            x, top, foot = 326, 96, 250
        a, b = ((x, top), (x, foot)) if direction == "down" else ((x, foot), (x, top))
    (x0, y0), (x1, y1) = on_face(*a), on_face(*b)
    return (f'<line x1="{x0:.1f}" y1="{y0:.1f}" x2="{x1:.1f}" y2="{y1:.1f}" stroke="{GESTURE}" '
            f'stroke-width="{width}" stroke-linecap="round" stroke-opacity="0.9" marker-end="url(#arrow)"/>'
            f'<circle cx="{x0:.1f}" cy="{y0:.1f}" r="9" fill="{GESTURE}" fill-opacity="0.35" '
            f'stroke="{GESTURE}" stroke-width="2"/>')


def icom_pictures():
    R = dict(dbm=-85, band="40m", mode="LSB", filt="FIL2", digits="  7123" "00", active=5,
             step="1 kHz", agc="MID", gain_cap="P.AMP", gain="OFF")
    f = face("icom", **R)
    left, right = radio_parts("P.AMP · the preamp", "PTT held: IC-7610's antenna")
    out = {}
    out["01-face"] = callouts("icom-01", "The Icom firmware's face", f, left, right)
    out["02-no-link"] = knob("icom-02", "NO LINK: the radio not reached yet",
                             face("icom", dbm=-127, **{k: v for k, v in R.items() if k != "dbm"})
                             + warning("NO LINK", "Icom"))
    out["03-tune"] = knob("icom-03", "Turn to tune; tap a digit for its step", f,
                          turn() + tap(md.dig_places()[0][5], 172),
                          "tap a digit for the step  \u00b7  turn to tune")
    out["04-mode"] = knob("icom-04", "The mode, chosen on the dial",
                          f + editor("MODE", "USB"), turn(),
                          "tap the mode  \u00b7  turn  \u00b7  tap the panel")
    out["05-filter"] = knob("icom-05", "The filter, applied as the knob turns",
                            f + editor("FILTER", "FIL1"), turn(),
                            "applies as you turn  \u00b7  a tap anywhere closes it")
    out["06-rf-gain"] = knob("icom-06", "Swipe from the left: RF GAIN",
                             f + editor("RF GAIN", "80%"), swipe_at("right"),
                             "swipe from the left  \u00b7  tap the panel for POWER")
    out["07-power"] = knob("icom-07", "POWER, in watts", f + editor("POWER", "50 W"), turn(),
                           "applies as you turn  \u00b7  a tap anywhere closes it")
    out["08-tuner"] = knob("icom-08", "Swipe from the right: the IC-7610's tuner",
                           f + editor("TUNER", "ON"), swipe_at("left"),
                           "swipe from the right  \u00b7  in the line, or out")
    out["09-vfo"] = knob("icom-09", "The IC-7610: MAIN or SUB",
                         f + editor("VFO", "SUB"), swipe_at("down"),
                         "swipe down  \u00b7  MAIN or SUB  \u00b7  tap the panel")
    out["10-antenna"] = knob("icom-10", "Then its antenna", f + editor("ANTENNA", "ANT1+RX"), turn(),
                             "then the antenna  \u00b7  tap the panel")
    out["11-vm"] = knob("icom-11", "The IC-705: V/M, for memory mode",
                        f + editor("V/M", "MEMORY", 28), swipe_at("down"),
                        "swipe down  \u00b7  V/M  \u00b7  tap the panel")
    out["12-memory"] = knob("icom-12", "Memory mode: the channel where the frequency was",
                            face("icom", dbm=-127, band="", mode="FM", filt="FIL1", step="",
                                 agc="MID", gain_cap="P.AMP", gain="OFF",
                                 mem=("G02", "ON0ORA", "M40  438.800  -7.6")),
                            turn(), "turn through the group's channels  \u00b7  tap G02 for another")
    out["13-radio"] = knob("icom-13", "Swipe up: another radio",
                           f + editor("RADIO", "IC-705", 28, hint="LAN  -  tap to switch"),
                           swipe_at("up"), "swipe up  \u00b7  turn  \u00b7  tap the panel: it restarts into it")
    for k, v in sdr_pictures("icom", "icom", R).items():
        out[{"rx": "14-rx", "sdr": "15-sdr", "balance": "16-balance"}[k]] = v
    out["17-tx"] = knob("icom-17", "On the air: SWR, power and the microphone",
                        tx_face("icom", swr=1.3, watts=50))
    out["18-headset"] = knob("icom-18", "A Bluetooth headset: its button is the PTT",
                             headset_face(f), "", "the headset's button: key, and key off")
    # The IC-R8600, a receiver: 2 m FM, then the 23 cm calling frequency in the
    # GHz digits, then its antennas.
    rx = dict(agc="MID", gain_cap="P.AMP", gain="ON", receiver=True)
    r = face("icom", dbm=-79, band="2m", mode="FM", filt="FIL1", digits="145500" "00",
             active=3, step="100 kHz", **rx)
    out["19-receiver"] = knob("icom-19", "The IC-R8600: a receiver, nothing to key", r, "",
                              "RECEIVER: the slab keys nothing")
    out["20-ghz"] = knob("icom-20", "From 1 GHz: four MHz digits",
                         face("icom", dbm=-97, band="23cm", mode="USB", filt="FIL2",
                              digits="12962000", active=6, step="1 kHz", ghz=True, **rx),
                         turn(), "1296.200.0 MHz  \u00b7  the step from 100 Hz up")
    out["21-r8600-antenna"] = knob("icom-21", "Swipe down: the IC-R8600's antennas",
                                   r + editor("ANTENNA", "ANT3"), swipe_at("down"),
                                   "after RX  \u00b7  ANT1, ANT2, ANT3  \u00b7  tap the panel")
    out["22-squelch"] = knob("icom-22", "Swipe from the right: the IC-R8600's squelch",
                             r + editor("SQUELCH", "30%"), swipe_at("left"),
                             "in every mode  \u00b7  0% is OPEN  \u00b7  applies as you turn")
    # The antennas, a press held on the slab away: the IC-7610's, here.
    out["23-hold-antenna"] = knob("icom-23", "Hold the slab: the antenna, the quicker way",
                                  f + editor("ANTENNA", "ANT2"), hold_slab(),
                                  "hold PTT until it buzzes  \u00b7  turn  \u00b7  tap the panel")
    # The knob's own battery, while it runs on it (ui.c knob_batt_show): at
    # the top of the arc, over the S-units -- on every firmware's face.
    md.use_palette("icom")
    out["24-battery"] = knob("icom-24", "On its own battery: the knob's charge at the top of the arc",
                             f + md.knob_battery(85), "",
                             "unplugged: its charge, by the quarter  \u00b7  none on USB power")
    # The IC-9700 on 2 m, beyond what any web SDR covers.
    out["25-sdr-out"] = knob("icom-25", "The IC-9700 on 2 m: the web SDR cannot reach it",
                             face("icom", **dict(R, dbm=-91, band="2m", mode="USB", digits="144300" "00"),
                                  sdr_note="can't reach"), "",
                             "the SDR quiet, its line gone  \u00b7  back once the radio is within its range")
    lock_pictures(out, 26, "icom", "icom", "Icom", f)
    return out


def multiflex_pictures():
    R = dict(dbm=-53, band="20m", mode="USB", filt="2700", digits=" 14200" "00", active=6,
             step="100 Hz", agc="MED", gain_cap="RF.G", gain="+8 dB")
    f = face("multiflex", **R)
    left, right = radio_parts("RF.G · the RF gain", "PTT held: the antennas")
    out = {}
    out["01-face"] = callouts("flex-01", "The FlexRadio firmware's face", f, left, right)
    out["02-station"] = knob("flex-02", "At boot, with others on the radio: what to be",
                             f + chooser("DIAL FOR", "thinkstation"), turn(),
                             "turn: STATION OWN, or DIAL FOR a station  \u00b7  tap the panel")
    out["03-menu"] = knob("flex-03", "Swipe from the right: the tuner, then the DSP",
                          f + editor("MENU", "MEM"), swipe_at("left"),
                          "swipe from the right  \u00b7  turn  \u00b7  tap the panel")
    out["04-radio"] = knob("flex-04", "Swipe up: the radios, on the LAN or by SmartLink",
                           f + editor("RADIO", "Lombardsijde", 28, hint="SmartLink  -  tap to switch"),
                           swipe_at("up"), "swipe up  \u00b7  LAN or SmartLink, under the name")
    out["05-rfg"] = knob("flex-05", "RF.G: the panadapter's RF gain",
                         f + editor("RF.G", "+8 dB"), turn(),
                         "tap RF.G  \u00b7  applies as you turn")
    out["06-no-link"] = knob("flex-06", "NO LINK: the radio not reached yet",
                             face("multiflex", dbm=-127, **{k: v for k, v in R.items() if k != "dbm"})
                             + warning("NO LINK", "FlexRadio"))
    for k, v in sdr_pictures("multiflex", "flex", R).items():
        out[{"rx": "07-rx", "sdr": "08-sdr", "balance": "09-balance"}[k]] = v
    out["10-tx"] = knob("flex-10", "On the air: SWR, power and the microphone",
                        tx_face("multiflex", swr=1.2, watts=80, band="20m", mode="USB",
                                filt="2700", digits=" 14200" "00"))
    out["11-headset"] = knob("flex-11", "A Bluetooth headset: its button is the PTT",
                             headset_face(f), "", "the headset's button: key, and key off")
    # The slice's antennas: a press held on the slab, receive then transmit.
    out["12-rx-ant"] = knob("flex-12", "Hold the slab: the receive antenna",
                            f + editor("RX ANT", "RX_A"), hold_slab(),
                            "hold PTT until it buzzes  \u00b7  turn  \u00b7  tap the panel")
    out["13-tx-ant"] = knob("flex-13", "Then the transmit antenna",
                            f + editor("TX ANT", "ANT1"), turn(),
                            "then TX ANT  \u00b7  tap the panel  \u00b7  a tap elsewhere: as it was")
    # The radio's memories: V/M, last on the swipe down, and one on the face.
    out["14-vm"] = knob("flex-14", "Swipe down to V/M, for the radio's memories",
                        f + editor("V/M", "MEMORY", 28), swipe_at("down"),
                        "swipe down  \u00b7  ...  \u00b7  V/M  \u00b7  tap the panel")
    out["15-memory"] = knob("flex-15", "Memory mode: the memory where the frequency was",
                            face("multiflex", dbm=-79, band="10m", mode="FM", filt="16000", step="MEM",
                                 agc="MED", gain_cap="RF.G", gain="+8 dB",
                                 mem=("10m", "ON0TEN", "M02  29.620  -0.1  T79.7")),
                            turn(), "turn: the next memory, in order of frequency")
    # A new knob with no address yet: the radios it hears on the LAN, on the dial.
    out["16-found"] = knob("flex-16", "Swipe up: a radio the knob heard on the LAN",
                           face("multiflex", dbm=-127, **{k: v for k, v in R.items() if k != "dbm"})
                           + editor("RADIO", "Lombardsijde", 28, hint="LAN  -  tap to switch"),
                           swipe_at("up"), "heard on the LAN  \u00b7  tap the panel: it joins the list, in use")
    # The knob's own battery, while it runs on it (ui.c knob_batt_show).
    md.use_palette("multiflex")
    out["17-battery"] = knob("flex-17", "On its own battery: the knob's charge at the top of the arc",
                             f + md.knob_battery(85), "",
                             "unplugged: its charge, by the quarter  \u00b7  none on USB power")
    # 6 m, beyond a KiwiSDR's 30 MHz.
    out["18-sdr-out"] = knob("flex-18", "6 m: the KiwiSDR beside it cannot reach it",
                             face("multiflex", **dict(R, dbm=-91, band="6m", digits=" 50150" "00"),
                                  sdr_note="can't reach"), "",
                             "the SDR quiet, its line gone  \u00b7  back once the radio is within its range")
    lock_pictures(out, 19, "flex", "multiflex", "FlexRadio", f)
    # The DSP, after the tuner's on the menu (ui.c menu_build): lit when on.
    md.use_palette("multiflex")
    out["21-dsp"] = knob("flex-21", "The menu's DSP: NR, on",
                         f + editor("DSP", "NR"), turn(),
                         "turn past MEM  \u00b7  lit when on  \u00b7  a tap switches it")
    return out


def aethersdr_pictures():
    R = dict(dbm=-86, band="40m", mode="LSB", filt="2800", digits="  7161" "73", active=6,
             step="100 Hz", agc="MED", gain_cap="RF.G", gain="", gain_known=False)
    f = face("aethersdr", **R)
    left, right = radio_parts("RF.G · greyed: not over TCI")
    out = {}
    out["01-face"] = callouts("aether-01", "The AetherSDR firmware's face", f, left, right)
    out["02-flip"] = knob("aether-02", "FLIP USB-C: the plug the wrong way round",
                          face("aethersdr", dbm=-127, **{k: v for k, v in R.items() if k != "dbm"})
                          + warning("FLIP USB-C", None, "No computer on this side of\nthe cable. Turn the USB-C\n"
                                                        "plug over, or wait for WiFi."))
    out["03-no-link"] = knob("aether-03", "NO LINK: AetherSDR not reached yet",
                             face("aethersdr", dbm=-127, **{k: v for k, v in R.items() if k != "dbm"})
                             + warning("NO LINK", "AetherSDR", "USB   10.55.42.1\nWiFi  -\nsetup  http://10.55.42.1"))
    out["04-mode"] = knob("aether-04", "The mode, chosen on the dial",
                          f + editor("MODE", "USB"), turn(),
                          "tap the mode  \u00b7  turn  \u00b7  tap the panel")
    out["05-radio"] = knob("aether-05", "Swipe up: AetherSDR on another computer",
                           f + editor("RADIO", "shack-pc", 28, hint="LAN  -  tap to switch"),
                           swipe_at("up"), "swipe up (on WiFi)  \u00b7  tap the panel")
    out["06-tx"] = knob("aether-06", "On the air: SWR, power and the microphone",
                        tx_face("aethersdr", swr=1.3, watts=50, band="40m", mode="LSB",
                                filt="2800", digits="  7161" "73"))
    out["07-headset"] = knob("aether-07", "A Bluetooth headset: its button is the PTT",
                             headset_face(f), "", "the headset's button: key, and key off")
    # The knob's own battery, while it runs on it (ui.c knob_batt_show).
    md.use_palette("aethersdr")
    out["08-battery"] = knob("aether-08", "On its own battery: the knob's charge at the top of the arc",
                             f + md.knob_battery(85), "",
                             "unplugged: its charge, by the quarter  \u00b7  none on USB power")
    lock_pictures(out, 9, "aether", "aethersdr", "AetherSDR", f)
    # The DSP from the right (ui.c menu_build), as AetherSDR offers it: off, dimmed.
    md.use_palette("aethersdr")
    out["11-dsp"] = knob("aether-11", "Swipe from the right: the DSP, NB off",
                         f + editor("DSP", "NB", colour=md.DISABLED), swipe_at("left"),
                         "swipe from the right  \u00b7  turn  \u00b7  dimmed: off  \u00b7  a tap switches it")
    return out


def openx6x00_pictures():
    """The openx6x00 firmware: AetherSDR's face and TCI client, over WiFi to
    an X6100 or X6200 on OpenX6X00, in its RF.Guru colours."""
    R = dict(dbm=-86, band="20m", mode="USB", filt="2800", digits=" 14200" "00", active=6,
             step="100 Hz", agc="AUTO", gain_cap="RF.G", gain="", gain_known=False)
    f = face("openx6x00", **R)
    quiet = face("openx6x00", dbm=-127, **{k: v for k, v in R.items() if k != "dbm"})
    left, right = radio_parts("RF.G · greyed: not over TCI")
    out = {}
    out["01-face"] = callouts("ox-01", "The OpenX6X00 firmware's face", f, left, right)
    out["02-no-link"] = knob("ox-02", "NO LINK: the radio not reached yet",
                             quiet + warning("NO LINK", "OpenX6X00"))
    out["03-mode"] = knob("ox-03", "The mode, chosen on the dial",
                          f + editor("MODE", "USB"), turn(),
                          "tap the mode  \u00b7  turn  \u00b7  tap the panel")
    out["04-radio"] = knob("ox-04", "Swipe up: another radio",
                           f + editor("RADIO", "portable", 28, hint="LAN  -  tap to switch"),
                           swipe_at("up"), "swipe up  \u00b7  tap the panel")
    out["05-tx"] = knob("ox-05", "On the air: SWR, power and the microphone",
                        tx_face("openx6x00", swr=1.2, watts=5, fs=10,
                                pegs=((1, "1"), (5, "5"), (10, "10")), band="20m", mode="USB",
                                filt="2800", digits=" 14200" "00"))
    out["06-headset"] = knob("ox-06", "A Bluetooth headset: its button is the PTT",
                             headset_face(f), "", "the headset's button: key, and key off")
    md.use_palette("openx6x00")
    out["07-battery"] = knob("ox-07", "On its own battery: the knob's charge at the top of the arc",
                             f + md.knob_battery(85), "",
                             "unplugged: its charge, by the quarter  \u00b7  none on USB power")
    # An X6200's two receive bands after 6 m (ui.c BANDS), where OpenX6X00
    # opens them: FM broadcast in wide FM, the airband in AM.
    out["08-x6200"] = knob("ox-08", "An X6200: FM broadcast and the airband, after 6 m",
                           f + editor("BAND", "FM"), turn(),
                           "tap the band  \u00b7  turn  \u00b7  tap the panel")
    lock_pictures(out, 9, "ox", "openx6x00", "OpenX6X00", f)
    return out


def svxconnect_pictures():
    f = reflector()
    left = [(64, 64, "audio, dBFS · hold: addresses"), (88, 122, "lock"),
            (74, 222, "link"), (112, 300, "PTT · tap on, tap off")]
    right = [(226, 77, "who is talking"), (198, 98, "for how long, or where"),
             (214, 123, "talkgroup"), (272, 122, "mute"), (288, 162, "its name"),
             (242, 197, "the reflector"), (244, 222, "volume"), (306, 222, "mic gain")]
    out = {}
    out["01-face"] = callouts("svx-01", "The SVXConnect firmware's face", f, left, right)
    out["02-turn"] = knob("svx-02", "Turn: the next talkgroup",
                          reflector(dbfs=-60, tg=9990, tg_name="Test", talker="--", talking=""),
                          turn(), "turn: the next switchable talkgroup")
    md.use_palette("svxconnect")
    tx = reflector(dbfs=-12, talker="-12 dB", talking="microphone")
    tx = tx.replace(f'fill="{md.BG}"/>', f'fill="{md.BG_TX}"/>', 1)
    tx = tx.replace(md.ptt_slab(md.BG1, "PTT", md.TEXT2), md.ptt_slab(md.TX_RED, "TX", "#FFFFFF"))
    tx += f'<circle cx="180" cy="180" r="178" fill="none" stroke="{md.TX_RED}" stroke-width="4"/>'
    # In transmit both lines are the transmit text's colour (ui.c).
    tx = tx.replace(md.text(180, 83, "-12 dB", 20, md.TEXT, 700),
                    md.text(180, 83, "-12 dB", 20, md.TX_TEXT, 700))
    tx = tx.replace(md.text(180, 103, "microphone", 14, md.GREEN),
                    md.text(180, 103, "microphone", 14, md.TX_TEXT))
    out["03-tx"] = knob("svx-03", "On the air: the microphone on the arc", tx)
    out["04-headset"] = knob("svx-04", "A Bluetooth headset: its button is the PTT",
                             headset_face(reflector()), "", "the headset's button: key, and key off")
    # The knob's own battery, while it runs on it (ui.c knob_batt_show): over
    # who is talking -- and on the air too, the arc being the audio's then.
    out["05-battery"] = knob("svx-05", "On its own battery: the knob's charge over who is talking",
                             reflector() + md.knob_battery(85), "",
                             "its charge, by the quarter  \u00b7  on the air too  \u00b7  none on USB power")
    lock_pictures(out, 6, "svx", "svxconnect", "SVXConnect", reflector())
    return out


# --- the phone firmware -----------------------------------------------------------

def telephone(**reading):
    """The phone firmware's face at a reading (mkrender's phone_dial: idle,
    registered, "Office" on the dial unless told otherwise)."""
    md.use_palette("phone")
    return mr.phone_dial(reading)


def phone_pictures():
    # A call up: Mum, two minutes 47 in. Ofcom's drama numbers throughout.
    call = dict(call=3, secs=167, dbfs=-20, rx_pk=-13, tx_db=-31, tx_pk=-23,
                peer="Mum", peer_num="+447700900123")
    left = [(64, 64, "their audio · hold: addresses"), (98, 122, "the favourite's number"),
            (74, 222, "link: registered"), (112, 300, "CALL the one shown · HANG UP")]
    right = [(300, 70, "your microphone"),
             (194, 77, "-- here: the call's time"), (230, 98, "how many favourites"),
             (286, 122, "mute: your microphone"), (218, 162, "the favourite · turn: the next"),
             (252, 197, "your own number"), (244, 222, "volume"), (306, 222, "mic gain")]
    out = {}
    out["01-face"] = callouts("tel-01", "The Telephone firmware's face", telephone(), left, right)
    out["02-turn"] = knob("tel-02", "Turn: the next favourite",
                          telephone(fav_name="Mum", fav_num="+447700900123"), turn(),
                          "turn: the next favourite  \u00b7  tap CALL")
    out["03-keypad"] = knob("tel-03", "Swipe down: the keypad, to dial a number by hand",
                            telephone(keypad="01632960123", flash="3"), swipe_at("down", panel=False),
                            "swipe down  \u00b7  type  \u00b7  tap CALL  \u00b7  \u00d7 at the top: away")
    out["04-calling"] = knob("tel-04", "Calling: it rings at the other end",
                             telephone(call=1, why="ringing", secs=6, peer="Mum",
                                       peer_num="+447700900123"), "",
                             "HANG UP gives up")
    out["05-incoming"] = knob("tel-05", "A call coming in: DECLINE or ANSWER",
                              telephone(call=2, peer="Office", peer_num="+441632960123"),
                              tap(258, 322), "it rings and buzzes  \u00b7  tap ANSWER, or DECLINE")
    left = [(64, 64, "their audio, its peak"), (124, 129, "their number"),
            (150, 170, "who: their name"), (74, 222, "link: registered"),
            (112, 300, "HANG UP")]
    right = [(300, 70, "your microphone, its peak"),
             (212, 77, "the call's time"), (226, 102, "in a call"),
             (286, 122, "mute: your microphone"), (252, 197, "your own number")]
    out["06-call"] = callouts("tel-06", "A call: its time, who, and both voices",
                              telephone(**call), left, right)
    out["07-dtmf"] = knob("tel-07", "In a call the keypad sends its keys as DTMF",
                          telephone(keypad="1#", flash="#", **call), swipe_at("down", panel=False),
                          "swipe down in a call  \u00b7  the keys go out as tones  \u00b7  \u00d7: away")
    out["08-ended"] = knob("tel-08", "A call ended, and why",
                           telephone(call=4, why="busy", peer="Mum", peer_num="+447700900123"), "",
                           "busy  \u00b7  declined  \u00b7  no answer  \u00b7  not available ...")
    out["09-no-service"] = knob("tel-09", "Not registered: NO SERVICE, and why",
                                telephone(link="disconnected") + warning("NOT REGISTERED", "Telephone"))
    out["10-mute"] = knob("tel-10", "Mute: your microphone off",
                          telephone(muted=True, **call), tap(286, 122),
                          "tap the microphone  \u00b7  they hear nothing, you still hear them")
    out["11-headset"] = knob("tel-11", "A Bluetooth headset: its logo on the slab",
                             telephone(headset=True, **call), "",
                             "its button hangs up  \u00b7  its own mute mutes")
    out["14-incoming-headset"] = knob("tel-14", "A call coming in with a headset: its button answers",
                                      telephone(call=2, headset=True, peer="Office",
                                                peer_num="+441632960123"), "",
                                      "the headset's button answers  \u00b7  the slab declines")
    # A speaker is no headset: the slab splits, the speaker under ANSWER --
    # the finger beside it, clear of it.
    out["15-incoming-speaker"] = knob("tel-15", "A call coming in with a speaker: the slab answers",
                                      telephone(call=2, speaker=True, peer="Office",
                                                peer_num="+441632960123"),
                                      tap(216, 314), "it rings on the jack and the speaker  \u00b7  tap ANSWER")
    md.use_palette("phone")
    out["12-history"] = knob("tel-12", "Swipe from the left: the calls, newest first",
                             telephone() + editor("CALLS 1 / 6", "Mum", 28,
                                                            hint="missed  -  12 min ago",
                                                            colour=md.DANGER),
                             swipe_at("right") + turn(),
                             "swipe from the left  \u00b7  turn  \u00b7  tap the panel: call back")
    out["13-missed"] = knob("tel-13", "A call missed: said under the arc until looked at",
                            telephone(n_missed=2), "",
                            "2 missed  \u00b7  the history clears it")
    # The knob's own battery, while it runs on it (ui.c knob_batt_show):
    # between the meters' tops.
    out["16-battery"] = knob("tel-16", "On its own battery: the knob's charge between the meters",
                             telephone() + md.knob_battery(85), "",
                             "unplugged: its charge, by the quarter  \u00b7  none on USB power")
    lock_pictures(out, 17, "tel", "phone", "Telephone", telephone(), keys=False)
    return out


# --- a Bluetooth headset (docs/headset.md) -------------------------------------

def headset_face(face_svg, **slab):
    """A transmitting firmware's face with a headset connected: the PTT slab
    keeps its caption, the headset's logo at its right end (mkdisplay
    headset_slab), in the face's palette -- the one its drawing last set.
    With speaker=True, a speaker's: a speaker there instead."""
    return face_svg.replace(md.ptt_slab(md.BG1, "PTT", md.TEXT2), md.headset_slab(**slab))


def headset_pictures():
    R = dict(dbm=-85, band="40m", mode="LSB", filt="FIL2", digits="  7123" "00", active=5,
             step="1 kHz", agc="MID", gain_cap="P.AMP", gain="OFF")
    out = {}
    out["01-connected"] = knob("hs-01", "A headset connected: its logo at the slab's end",
                               headset_face(face("icom", **R)), "",
                               "its button is a PTT  \u00b7  the slab keys as always")
    out["02-muted"] = knob("hs-02", "Its microphone muted: the logo in red",
                           headset_face(face("icom", **R), muted=True), "",
                           "muted: the button does not key")
    out["03-raise-boom"] = knob("hs-03", "The boom arm as the PTT, and down: RAISE BOOM",
                                headset_face(face("icom", **R), raise_boom=True), "",
                                "raise the boom once  \u00b7  then down transmits, up stops")
    tx = tx_face("icom", swr=1.3, watts=50)
    out["04-on-the-air"] = knob("hs-04", "On the air through the headset",
                                tx.replace(md.ptt_slab(md.TX_RED, "TX", "#FFFFFF"),
                                           md.headset_slab("TX", tx=True)), "",
                                "the headset's button, or the boom up: back to receive")
    out["05-speaker"] = knob("hs-05", "A speaker connected: a speaker at the slab's end",
                             headset_face(face("icom", **R), speaker=True), "",
                             "the knob's own microphone keys  \u00b7  the speaker plays, a little behind")
    tx = tx_face("icom", swr=1.3, watts=50)
    out["06-speaker-on-the-air"] = knob("hs-06", "On the air with a speaker",
                                        tx.replace(md.ptt_slab(md.TX_RED, "TX", "#FFFFFF"),
                                                   md.headset_slab("TX", tx=True, speaker=True)), "",
                                        "the knob's own microphone  \u00b7  the speaker silent")
    # Its battery beside the logo (ui.c s_hs_batt), in each of its colours.
    for n, pct, colour, caption in ((7, 80, "green", "80 %: green, from half the charge up"),
                                    (8, 40, "yellow", "40 %: yellow, under half"),
                                    (9, 10, "red", "10 %: red, a fifth and under")):
        out[f"{n:02d}-battery-{colour}"] = knob(f"hs-{n:02d}", f"The headset's battery: {pct} %, {colour}",
                                                headset_face(face("icom", **R), batt=pct), "", caption)
    return out


# --- the ubersdr firmware -------------------------------------------------------

UBER_KIWI = "#8B7CF8"                      # ui.c SDR_HEX for ubersdr: UberSDR's violet


def snr_colour(snr):
    """ui.c: the SNR in UberSDR's colours for it, hsv(0..120, 85%, 96%) from
    0 to 15 dB."""
    import colorsys
    f = max(0.0, min(1.0, snr / 15.0))
    r, g, b = colorsys.hsv_to_rgb(f * 120 / 360, 0.85, 0.96)
    return "#%02X%02X%02X" % (round(r * 255), round(g * 255), round(b * 255))


def uber_face(dbm=-91, snr=9, band="20m", mode="USB", filt="2650", digits=" 14215" "00", active=5,
              step="1 kHz", nr="NR4", spot=("LU7YZ", "14.215.0 USB  DX 2m  heard 12 dB", "green", "8 on 20m"),
              kiwi=None, vol="40", headset=False, speaker=False, left=None, kiwi_note=None):
    """The ubersdr firmware's face (ui.c, RX_FACE): the S-meter in UberSDR's
    colours, the SNR where the AGC is, the noise filter where the gain is, no
    RIT and no microphone, and on the slab the spot or voice nearest the dial:
    `spot` is (its call or frequency, where and what it is, "green" heard now
    / "bright" on it / "dim" elsewhere, how many on the band). `kiwi` is a
    KiwiSDR's level beside it -- or `kiwi_note` the word for why it is not
    heard, in amber where its reading would be, and no line; `headset`, a
    Bluetooth headset's logo at the slab's right end (ui.c s_hs_bt), and
    `speaker` a speaker's there. `left` is a guest's time left at the slab's
    left end (ui.c left_slab): what it says, and "dim", "warn" in the last
    five minutes, or "danger" in the last one and an idle limit's."""
    md.use_palette("ubersdr")
    s = [f'<rect x="0" y="0" width="360" height="360" fill="{md.BG}"/>',
         f'<path d="{md.arc_path(md.ARC_ROT, md.ARC_ROT + md.ARC_SPAN, md.RC)}" fill="none" '
         f'stroke="{md.SUBTLE}" stroke-width="{md.BAND}"/>']
    for lo, hi, col in md.RXZONES:
        s.append(md.block(md.ARC_ROT + md.smeter_frac(lo) * md.ARC_SPAN,
                          md.ARC_ROT + md.smeter_frac(hi) * md.ARC_SPAN, col))
    s.append(cover(md.ARC_ROT, md.ARC_ROT + md.ARC_SPAN, md.smeter_frac(dbm)))
    for d in md.RXNOTCH:
        s.append(md.notch(md.ARC_ROT + md.smeter_frac(d) * md.ARC_SPAN))
    for d, ln, _ in md.RXTICKS:
        col = md.TEXT2 if d == -73 else (md.WARN if d > -73 else md.LABEL)
        s.append(md.tick(md.ARC_ROT + md.smeter_frac(d) * md.ARC_SPAN, ln, col, 3 if d == -73 else 2))
    if kiwi is not None:
        s.append(f'<path d="{md.arc_path(md.ARC_ROT, md.ARC_ROT + md.ARC_SPAN, 174)}" fill="none" '
                 f'stroke="{md.SUBTLE}" stroke-width="4"/>')
        s.append(md.block(md.ARC_ROT, md.ARC_ROT + md.smeter_frac(kiwi) * md.ARC_SPAN, UBER_KIWI, r=174,
                           band=4))
        sub, sub_colour = md.smeter_text(kiwi), UBER_KIWI
    elif kiwi_note:
        sub, sub_colour = kiwi_note, md.WARN
    else:
        sub, sub_colour = f"{dbm} dBFS", md.LABEL
    s += [md.text(180, 83, md.smeter_text(dbm), 20, md.TEXT, 700),
          md.text(180, 103, esc(sub), 14, sub_colour),
          md.text(108, 83, "SNR", 14, md.LABEL),
          md.text(108, 102, f"{snr} dB" if snr is not None else "--", 14,
                  snr_colour(snr) if snr is not None else md.DISABLED),
          md.text(252, 83, "FIL", 14, md.LABEL), md.text(252, 102, nr, 14, md.TEXT2),
          md.text(104, 129, band, 20, md.ACCENT), md.text(180, 129, mode, 20, md.TEXT),
          md.text(256, 129, filt, 20, md.TEXT2),
          md.readout(digits, colour=md.TEXT, sep_colour=md.LABEL, underline=active,
                     after_colour=md.TEXT2, active_colour=md.ACCENT_HI),
          md.text(124, 227, step, 20, md.ACCENT),
          md.icon_readout(236, 227, md.speaker, vol, md.TEXT2),
          f'<rect x="0" y="{md.PTT_TOP}" width="360" height="{360 - md.PTT_TOP}" fill="{md.BG1}"/>',
          f'<line x1="0" y1="{md.PTT_TOP + 1}" x2="360" y2="{md.PTT_TOP + 1}" '
          f'stroke="{md.ACCENT}" stroke-width="2"/>']
    if spot:
        l1, l2, look, l3 = spot
        colour = {"green": md.GREEN, "bright": md.TEXT, "dim": md.TEXT2}[look]
        s += [md.text(180, md.PTT_TOP + 8 + 25, esc(l1), 28, colour, 500, extra=' xml:space="preserve"'),
              md.text(180, md.PTT_TOP + 44 + 12, esc(l2), 14, md.TEXT2, extra=' xml:space="preserve"'),
              md.text(180, md.PTT_TOP + 64 + 12, esc(l3), 14, md.LABEL, extra=' xml:space="preserve"')]
    if headset:
        s.append(md.headset_logo(md.ACCENT, md.BG1))
    if speaker:
        s.append(md.speaker_logo(md.ACCENT))
    if left:
        # ui.c LEFT_X, LEFT_Y: Montserrat 14, its baseline 13 px down.
        text, look = left
        colour = {"dim": md.TEXT2, "warn": md.WARN, "danger": md.DANGER}[look]
        s.append(md.text(44, md.PTT_TOP + 15 + 13, esc(text), 14, colour, anchor="start",
                         extra=' xml:space="preserve"'))
    return "".join(s)


def sstv_picture():
    """A test card where the receiver's picture goes: colour bars over a
    grey ramp, 260 x 208 as the viewer fits a 320 x 256 picture."""
    x0, y0, w, h = 50, 76, 260, 208
    bars = ["#C0C0C0", "#C0C000", "#00C0C0", "#00C000", "#C000C0", "#C00000", "#0000C0", "#101010"]
    s = [f'<rect x="{x0 + i * w / 8:.1f}" y="{y0}" width="{w / 8 + 0.5:.1f}" height="{h * 0.62:.1f}" '
         f'fill="{c}"/>' for i, c in enumerate(bars)]
    for i in range(10):
        g = int(20 + i * 23)
        s.append(f'<rect x="{x0 + i * w / 10:.1f}" y="{y0 + h * 0.62:.1f}" width="{w / 10 + 0.5:.1f}" '
                 f'height="{h * 0.38:.1f}" fill="#{g:02X}{g:02X}{g:02X}"/>')
    s.append(md.text(180, y0 + h * 0.86, "CQ SSTV", 28, "#FFFFFF", 700))
    return "".join(s)


def sstv_viewer(title="M2  1 / 15", caption="14.230 USB  19:19Z  5 dB"):
    """ui.c sv_open(): over the whole face, the picture in the middle, what it
    is above it in the bright accent, where and when under it."""
    md.use_palette("ubersdr")
    return (f'<rect x="0" y="0" width="360" height="360" fill="{md.BG}"/>' + sstv_picture()
            + md.text(180, 59, esc(title), 20, md.ACCENT_HI, extra=' xml:space="preserve"')
            + md.text(180, 309, esc(caption), 14, md.TEXT2, extra=' xml:space="preserve"'))


def ubersdr_pictures():
    f = uber_face()
    left = [(64, 64, "S-meter · hold: the addresses"), (84, 77, "SNR"), (84, 123, "band"),
            (58, 172, "frequency · tap a digit: its step"), (96, 221, "tuning step"),
            (40, 270, "a guest's time left"),
            (132, 280, "nearest spot or voice · tap: all"), (112, 300, "where, and what it is")]
    right = [(204, 77, "S-units, dBFS under"), (282, 77, "FIL · the noise filter"), (204, 123, "mode"),
             (282, 123, "filter"), (252, 222, "volume"), (228, 322, "how many on the band")]
    out = {}
    out["01-face"] = callouts("uber-01", "The UberSDR firmware's face", uber_face(left=("52 min", "dim")),
                              left, right)
    md.use_palette("ubersdr")
    out["02-no-link"] = knob("uber-02", "The receiver not reached, and why",
                             uber_face(dbm=-127, snr=None, spot=None) + warning("RECEIVER FULL", "UberSDR"))
    out["03-tune"] = knob("uber-03", "Turn to tune; tap a digit for its step", f,
                          turn() + tap(md.dig_places()[0][5], 172),
                          "tap a digit for the step  \u00b7  turn to tune")
    out["04-filter"] = knob("uber-04", "FIL: the receiver's noise filter",
                            f + editor("NOISE FILTER", "NR4"), turn(),
                            "tap FIL  \u00b7  OFF, NR2, RN2, NR4  \u00b7  applies as you turn")
    out["05-spots"] = knob("uber-05", "The spots and voices on the band, on the dial",
                           f + editor("SPOT 3 / 8", "LU7YZ", 28, hint="14.215.0 USB  DX 2m  heard 12 dB"),
                           turn() + tap(180, 280), "tap the spot  \u00b7  turn  \u00b7  tap the panel: there, in its mode")
    out["06-voice"] = knob("uber-06", "A voice nobody has spotted: by its frequency, in green",
                           uber_face(dbm=-79, snr=22, digits=" 14268" "00",
                                     spot=("14.268.0", "USB  voice 22 dB", "green", "8 on 20m")),
                           "", "a voice heard now: green  \u00b7  a spot: white on it, grey beside it")
    out["07-mode"] = knob("uber-07", "The mode, by UberSDR's names",
                          f + editor("MODE", "CWU"), turn(),
                          "tap the mode  \u00b7  USB LSB CWU CWL AM SAM FM NFM")
    out["08-rx"] = knob("uber-08", "Swipe down: LOCAL, or a KiwiSDR beside it",
                        f + editor("RX", "Web-888", 28), swipe_at("down"),
                        "swipe down  \u00b7  turn to LOCAL or a receiver  \u00b7  tap the panel")
    out["09-kiwi"] = knob("uber-09", "A KiwiSDR playing beside it: its line and reading in violet",
                          uber_face(kiwi=-97), "", "the KiwiSDR: the thin violet line, its S-units in violet")
    out["10-balance"] = knob("uber-10", "BALANCE: the UberSDR left, the KiwiSDR right",
                             uber_face(kiwi=-97) + editor("BALANCE", "L | R"), swipe_at("right") + turn(),
                             "swipe from the left  \u00b7  turn: UBER ... L | R ... KIWI")
    out["11-sstv"] = knob("uber-11", "Swipe from the right: the receiver's SSTV pictures",
                          f + editor("SSTV", "15 PICTURES", 28, hint="tap to look  -  the knob turns them"),
                          swipe_at("left"), "swipe from the right  \u00b7  tap the panel")
    out["12-viewer"] = knob("uber-12", "An SSTV picture: the knob turns to the next",
                            sstv_viewer(), turn(), "turn: newest first  \u00b7  any tap: back to the dial")
    out["13-time-up"] = knob("uber-13", "The receiver ended the session: listen again?",
                             dimmed(uber_face(dbm=-127, snr=None, spot=None))
                             + chooser("TIME UP", "LISTEN AGAIN"),
                             tap(180, 188), "tap the panel: a new session")
    out["14-radio"] = knob("uber-14", "Swipe up: another UberSDR",
                           f + editor("RADIO", "ON6URE-TEL", 28, hint="tap to switch"),
                           swipe_at("up"), "swipe up  \u00b7  turn  \u00b7  tap the panel: it restarts into it")
    # The one in use not reached, the next tried (ui.c warn_name).
    out["21-not-found"] = knob("uber-21", "The receiver in use not found: named under the warning",
                               uber_face(dbm=-127, snr=None, spot=None)
                               + warning("NOT FOUND", "UberSDR", name="ON6URE-TEL-LAN"), "",
                               "the one in use, tried twice  \u00b7  then the next in the list")
    out["22-connecting"] = knob("uber-22", "On to the next receiver in the list",
                                uber_face(dbm=-127, snr=None, spot=None)
                                + warning("CONNECTING", "UberSDR", name="ON6URE-TEL"), "",
                                "the first that answers plays  \u00b7  and is in use from then on")
    out["15-headset"] = knob("uber-15", "A Bluetooth headset connected: its logo beside the spot",
                             uber_face(headset=True), "",
                             "the receiver in the headset  \u00b7  the spots stay")
    out["16-speaker"] = knob("uber-16", "A Bluetooth speaker connected: a speaker beside the spot",
                             uber_face(speaker=True), "",
                             "the receiver in the speaker, a little behind  \u00b7  the spots stay")
    out["17-time-left"] = knob("uber-17", "A guest's time left, at the slab's left end",
                               uber_face(left=("52 min", "dim")), "",
                               "a guest's time on the receiver  \u00b7  none with its password, or on its LAN")
    out["18-last-minutes"] = knob("uber-18", "The last five minutes: the seconds too, in amber",
                                  uber_face(left=("4:59", "warn")), "",
                                  "the last five minutes in amber  \u00b7  the last one in red")
    out["19-idle"] = knob("uber-19", "An idle limit's last minute, in red: use the knob",
                          uber_face(left=("idle 0:42", "danger")), turn(),
                          "a touch or a turn  \u00b7  the receiver hears from the knob, the minute goes")
    # The knob's own battery, while it runs on it (ui.c knob_batt_show).
    out["20-battery"] = knob("uber-20", "On its own battery: the knob's charge at the top of the arc",
                             uber_face() + md.knob_battery(85), "",
                             "unplugged: its charge, by the quarter  \u00b7  none on USB power")
    # 6 m on an UberSDR that covers it, beyond the KiwiSDR's 30 MHz.
    out["23-kiwi-out"] = knob("uber-23", "6 m: the KiwiSDR beside it cannot reach it",
                              uber_face(dbm=-97, snr=12, band="6m", digits=" 50150" "00", kiwi_note="can't reach",
                                        spot=("50.150.0", "USB  voice 12 dB", "green", "2 on 6m")), "",
                              "the KiwiSDR quiet, its line gone  \u00b7  back once the dial is within its range")
    lock_pictures(out, 24, "uber", "ubersdr", "UberSDR", f, keys=False)
    return out


# --- the kiwi firmware (Kiwi888) ---------------------------------------------------

KIWI_PEAK = "#FF0000"                      # ui.c KIWI_PEAK_HEX: the peak held, red as on the page
KIWI_RIGHT = "#99C9FF"                     # ui.c SDR_HEX for kiwi: the right ear, the page's link blue
# The receiver on the slab: its name, then -- the name its antenna already --
# its model and address, and where it is (kiwi_info's lines).
KIWI_RX = ("EchoTracer", "Web-888  81.83.21.23:8077", "Lombardsijde, Belgium")


def kiwi_face(dbm=-85, peak=-79, band="40m", mode="LSB", filt="2400", digits="  7123" "00", active=5,
              step="1 kHz", agc="MED", nr="OFF", ov=False, rx=KIWI_RX, right=None, vol="40", headset=False,
              speaker=False, right_note=None):
    """The kiwi firmware's face (ui.c, KIWI_FACE): the receivers' own page's
    look -- its lime S-meter with the peak held a second in red, the AGC and
    the noise filter (NR) either side, no RIT and no microphone, and on the
    slab the receiver in use: `rx` is its name, its model and address (or
    antenna), and where it is. The readout is the peak's, in dBm under the
    S-units, OV after it in red (`ov`) while the receiver's ADC overloads.
    `right` is the right ear's level -- its thin line outside the S-meter,
    its reading in blue where the dBm is -- or, `right_note`, the word for
    why it is not heard there, in amber, and no line; `headset`, a Bluetooth
    headset's logo at the slab's right end, and `speaker` a speaker's there."""
    md.use_palette("kiwi")
    s = [f'<rect x="0" y="0" width="360" height="360" fill="{md.BG}"/>',
         f'<path d="{md.arc_path(md.ARC_ROT, md.ARC_ROT + md.ARC_SPAN, md.RC)}" fill="none" '
         f'stroke="{md.SUBTLE}" stroke-width="{md.BAND}"/>']
    for lo, hi, col in md.RXZONES:
        s.append(md.block(md.ARC_ROT + md.smeter_frac(lo) * md.ARC_SPAN,
                          md.ARC_ROT + md.smeter_frac(hi) * md.ARC_SPAN, col))
    s.append(cover(md.ARC_ROT, md.ARC_ROT + md.ARC_SPAN, md.smeter_frac(dbm)))
    if peak > -127:
        # vu_band's peak mark (ui.c led_set): three degrees, ending at the peak.
        at = min(max(round(md.smeter_frac(peak) * md.ARC_SPAN) - 3, 0), md.ARC_SPAN - 3)
        s.append(md.block(md.ARC_ROT + at, md.ARC_ROT + at + 3, KIWI_PEAK))
    for d in md.RXNOTCH:
        s.append(md.notch(md.ARC_ROT + md.smeter_frac(d) * md.ARC_SPAN))
    for d, ln, _ in md.RXTICKS:
        col = md.TEXT2 if d == -73 else (md.WARN if d > -73 else md.LABEL)
        s.append(md.tick(md.ARC_ROT + md.smeter_frac(d) * md.ARC_SPAN, ln, col, 3 if d == -73 else 2))
    if right is not None:
        at = min(max(round(md.smeter_frac(right) * md.ARC_SPAN) - 3, 0), md.ARC_SPAN - 3)
        s += [f'<path d="{md.arc_path(md.ARC_ROT, md.ARC_ROT + md.ARC_SPAN, 174)}" fill="none" '
              f'stroke="{md.SUBTLE}" stroke-width="4"/>',
              md.block(md.ARC_ROT, md.ARC_ROT + md.smeter_frac(right) * md.ARC_SPAN, KIWI_RIGHT, r=174, band=4),
              md.block(md.ARC_ROT + at, md.ARC_ROT + at + 3, KIWI_RIGHT, r=174, band=4)]
        sub, sub_colour = md.smeter_text(right), KIWI_RIGHT
    elif right_note:
        sub, sub_colour = right_note, md.WARN
    else:
        sub, sub_colour = f"{peak} dBm" + ("  OV" if ov else ""), md.DANGER if ov else md.LABEL
    name, line2, line3 = rx
    s += [md.text(180, 83, md.smeter_text(peak), 20, md.TEXT, 700),
          md.text(180, 103, esc(sub), 14, sub_colour, extra=' xml:space="preserve"'),
          md.aux(agc, "NR", nr),
          md.text(104, 129, band, 20, md.ACCENT), md.text(180, 129, mode, 20, md.TEXT),
          md.text(256, 129, filt, 20, md.TEXT2),
          md.readout(digits, colour=md.TEXT, sep_colour=md.LABEL, underline=active,
                     after_colour=md.TEXT2, active_colour=md.ACCENT_HI),
          md.text(124, 227, step, 20, md.ACCENT),
          md.icon_readout(236, 227, md.speaker, vol, md.TEXT2),
          f'<rect x="0" y="{md.PTT_TOP}" width="360" height="{360 - md.PTT_TOP}" fill="{md.BG1}"/>',
          f'<line x1="0" y1="{md.PTT_TOP + 1}" x2="360" y2="{md.PTT_TOP + 1}" '
          f'stroke="{md.ACCENT}" stroke-width="2"/>',
          md.text(180, md.PTT_TOP + 8 + 25, esc(name), 28, md.TEXT, 500, extra=' xml:space="preserve"'),
          md.text(180, md.PTT_TOP + 44 + 12, esc(line2), 14, md.TEXT2, extra=' xml:space="preserve"'),
          md.text(180, md.PTT_TOP + 64 + 12, esc(line3), 14, md.LABEL, extra=' xml:space="preserve"')]
    if headset:
        s.append(md.headset_logo(md.ACCENT, md.BG1))
    if speaker:
        s.append(md.speaker_logo(md.ACCENT))
    return "".join(s)


# The configuration page (components/webcfg/www/index.html), its own colours.
PAGE = dict(bg="#14161a", panel="#1c1f26", line="#2b303b", fg="#e6e9ef", dim="#8b93a5",
            accent="#f2a33c", ok="#4caf7d", field="#111317")
PAGE_W, PAGE_H = 760, 742


def page_receivers():
    """The configuration page's Receivers on the kiwi firmware, in a browser:
    two receivers -- the one in use, playing; another in the right ear --
    Add RF.Guru's receivers, and the name their owners see the knob by."""
    P = PAGE
    t = lambda x, y, s, size, col, weight=400, anchor="start", extra="": (   # noqa: E731
        f'<text x="{x}" y="{y}" font-size="{size}" fill="{col}" font-weight="{weight}" '
        f'text-anchor="{anchor}"{extra}>{esc(s)}</text>')

    def field(x, y, w, label, value, pw=False):
        out = t(x, y + 12, label, 12, P["dim"])
        out += (f'<rect x="{x}" y="{y + 18}" width="{w}" height="36" rx="6" fill="{P["field"]}" '
                f'stroke="{P["line"]}"/>')
        if pw:
            out += t(x + 10, y + 41, "•" * 8 if value else "", 14, P["fg"])
            out += t(x + w - 12, y + 40, "Show", 12, P["accent"], anchor="end")
        else:
            out += t(x + 10, y + 41, value, 14, P["fg"] if value else P["dim"])
        return out

    def radio(x, y, on, label, disabled=False):
        col = P["dim"] if disabled else P["fg"]
        out = f'<circle cx="{x + 7}" cy="{y - 5}" r="7" fill="none" stroke="{col}" stroke-width="1.5"/>'
        if on:
            out += f'<circle cx="{x + 7}" cy="{y - 5}" r="3.5" fill="{P["accent"]}"/>'
        return out + t(x + 22, y, label, 14, col)

    def button(x, y, label, w):
        return (f'<rect x="{x}" y="{y}" width="{w}" height="34" rx="6" fill="none" stroke="{P["line"]}"/>'
                + t(x + w / 2, y + 22, label, 14, P["dim"], anchor="middle"))

    x0, w = 60, 640                      # the section's content: 720 less its padding
    s = [f'<rect x="1" y="1" width="{PAGE_W - 2}" height="{PAGE_H - 2}" rx="12" fill="{P["bg"]}" '
         f'stroke="#3A3F4B" stroke-width="2"/>',
         f'<path d="M1,40 V13 a12,12 0 0 1 12,-12 H{PAGE_W - 13} a12,12 0 0 1 12,12 V40 Z" fill="#23262d"/>',
         *[f'<circle cx="{22 + 18 * i}" cy="20" r="6" fill="{c}"/>'
           for i, c in enumerate(("#e5534b", "#e0a23a", "#4caf7d"))],
         f'<rect x="96" y="9" width="{PAGE_W - 192}" height="22" rx="11" fill="#14161a"/>',
         t(PAGE_W / 2, 25, "vfo-knob.local/config#sdr", 12, P["dim"], anchor="middle"),
         f'<rect x="{x0 - 16}" y="58" width="{w + 32}" height="{PAGE_H - 76}" rx="10" fill="{P["panel"]}" '
         f'stroke="{P["line"]}"/>',
         t(x0, 86, "RECEIVERS", 12, P["dim"], 600, extra=' letter-spacing="1.1"')]
    # A Test of the one playing answers from its session, with no login of
    # its own (index.html's LOGIN['in use']), wrapped beside its buttons.
    rows = [(True, False, "EchoTracer", "81.83.21.23:8077", "in use: streaming",
             ("✓ playing now: no second login —", "RF.Guru EchoTracer · Web-888 · 4 of 13 channels in use")),
            (False, True, "TerraBooster", "81.83.21.23:8075", "right ear: streaming", ())]
    y = 100
    for use, right, name, host, state, res in rows:
        s.append(radio(x0, y + 14, use, "In use", disabled=right))
        s.append(field(x0, y + 22, (w - 14) / 2, "Name, on the knob", name))
        s.append(field(x0 + (w + 14) / 2, y + 22, (w - 14) / 2, "Address", host))
        s.append(field(x0, y + 84, (w - 14) / 2, "Password", ""))
        s.append(field(x0 + (w + 14) / 2, y + 84, (w - 14) / 2, "Time-limit password", ""))
        s.append(t(x0, y + 160, state, 12, P["dim"]))
        s.append(button(x0, y + 172, "Test", 64) + button(x0 + 74, y + 172, "Remove", 86))
        for i, line in enumerate(res):
            s.append(t(x0 + 172, y + 186 + 16 * i, line, 12, P["ok"]))
        s.append(f'<line x1="{x0}" y1="{y + 222}" x2="{x0 + w}" y2="{y + 222}" stroke="{P["line"]}"/>')
        y += 230
    # Two receivers: RF.Guru's other two are still to be added.
    s.append(button(x0, y, "Add a receiver", 132) + button(x0 + 142, y, "Add RF.Guru's receivers", 196))
    s.append(field(x0, y + 40, (w - 14) / 2, "Your name, for their owners", "ON6URE"))
    for i, row in enumerate(("A KiwiSDR or a Web-888 lists who listens, for its owner: the knob goes by this",
                             "name there — your callsign, say. Left empty, VFO-Knob. Saved with Save, and",
                             "told at once to a receiver playing.")):
        s.append(t(x0, y + 118 + 18 * i, row, 12, P["dim"]))
    return (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {PAGE_W} {PAGE_H}" width="{PAGE_W}" '
            f'height="{PAGE_H}" role="img" aria-label="The configuration page: Receivers">'
            f'<title>The configuration page: Receivers</title>'
            f'<g font-family="system-ui,-apple-system,Segoe UI,Roboto,sans-serif">{"".join(s)}</g></svg>')


def kiwi_pictures():
    f = kiwi_face()
    # The peak mark's place on the arc, for its callout.
    pa = math.radians(md.ARC_ROT + md.smeter_frac(-79) * md.ARC_SPAN - 1.5)
    peak_at = (round(180 + 164 * math.cos(pa)), round(180 + 164 * math.sin(pa)))
    left = [(64, 64, "S-meter · hold: the addresses"), (84, 77, "AGC · fast, med, slow"), (84, 123, "band"),
            (58, 172, "frequency · tap a digit: its step"), (96, 221, "tuning step"),
            (92, 276, "the receiver · tap: another"), (90, 300, "its model and address")]
    right = [(*peak_at, "the peak, held a second"), (204, 77, "S-units, dBm under, OV in red"),
             (282, 77, "NR · the noise filter"), (204, 123, "mode"), (282, 123, "filter"),
             (252, 222, "volume"), (262, 322, "where it is")]
    out = {}
    out["01-face"] = callouts("kiwi-01", "The Kiwi888 firmware's face", f, left, right)
    down = kiwi_face(dbm=-127, peak=-127, rx=("EchoTracer", "Web-888  81.83.21.23:8077", ""))
    md.use_palette("kiwi")
    out["02-no-link"] = knob("kiwi-02", "The receiver will not have the knob, and why",
                             down + warning("RECEIVER FULL", "Kiwi888"))
    out["03-tune"] = knob("kiwi-03", "Turn to tune; tap a digit for its step", f,
                          turn() + tap(236, 172), "tap a digit for the step  ·  turn to tune")
    out["04-agc"] = knob("kiwi-04", "AGC: the receivers' own three",
                         kiwi_face(agc="SLOW") + editor("AGC", "SLOW"), tap(108, 90) + turn(),
                         "tap AGC  ·  FAST, MED, SLOW  ·  applies as you turn")
    out["05-nr"] = knob("kiwi-05", "NR: the receivers' noise filters",
                        kiwi_face(nr="WDSP") + editor("NOISE FILTER", "WDSP"), tap(252, 90) + turn(),
                        "tap NR  ·  OFF, WDSP, LMS, SPEC  ·  rests half a second, then goes")
    out["06-mode"] = knob("kiwi-06", "The mode, by the receivers' own names",
                          f + editor("MODE", "SAM"), turn(),
                          "tap the mode  ·  USB LSB CW AM SAM SAL SAU NBFM")
    out["07-filter"] = knob("kiwi-07", "The filter, its widths by mode",
                            kiwi_face(filt="2700") + editor("FILTER", "2700 Hz"), turn(),
                            "tap the filter  ·  applies as you turn  ·  a tap anywhere closes it")
    out["08-receiver"] = knob("kiwi-08", "Swipe up, or tap the slab: another receiver, at once",
                              f + editor("RECEIVER", "OctaLoop", 28, hint="tap to switch"),
                              swipe_at("up"), "swipe up  ·  turn  ·  tap the panel: no restart")
    out["09-squelch"] = knob("kiwi-09", "Swipe from the right: SQUELCH",
                             f + editor("SQUELCH", "30%"), swipe_at("left"),
                             "swipe from the right  ·  0% is OPEN  ·  applies as you turn")
    md.use_palette("kiwi")
    out["10-time-up"] = knob("kiwi-10", "The receiver ended the session: listen again?",
                             dimmed(down) + chooser("TIME UP", "LISTEN AGAIN"),
                             tap(180, 188), "tap the panel: a new session")
    md.use_palette("kiwi")
    out["11-day-limit"] = knob("kiwi-11", "DAY LIMIT: the receiver's listening time for today used up",
                               down + warning("DAY LIMIT", "Kiwi888"), "",
                               "left alone until it restarts  ·  chosen again: one more try, two at most")
    out["12-ov"] = knob("kiwi-12", "OV: the receiver's ADC overloaded",
                        kiwi_face(dbm=-20, peak=-13, ov=True, band="20m", mode="USB", filt="2400",
                                  digits=" 14200" "00", active=6, step="100 Hz"), "",
                        "OV after the reading, in red, a second after the last overload")
    out["13-headset"] = knob("kiwi-13", "A Bluetooth headset connected: its logo beside the name",
                             kiwi_face(headset=True), "",
                             "the receiver in the headset  ·  nothing to transmit")
    out["14-right-ear"] = knob("kiwi-14", "Swipe down: a second receiver, in the right ear",
                               f + editor("RIGHT EAR", "TerraBooster", 28), swipe_at("down"),
                               "swipe down  ·  turn to OFF or a receiver  ·  tap the panel")
    out["15-right"] = knob("kiwi-15", "The right ear playing: its line and reading in blue",
                           kiwi_face(right=-97), "", "the right ear: the thin blue line, its S-units in blue")
    md.use_palette("kiwi")
    out["16-right-refused"] = knob("kiwi-16", "The left ear's receiver: never in both ears",
                                   f + editor("RIGHT EAR", "EchoTracer", 28, hint="in the left ear",
                                              colour=md.DISABLED), turn(),
                                   "dimmed: a tap on it is refused, with a triple click")
    out["17-balance"] = knob("kiwi-17", "BALANCE: the left ear's receiver, the right ear's",
                             kiwi_face(right=-97) + editor("BALANCE", "L | R"), swipe_at("right") + turn(),
                             "swipe from the left  ·  turn: LEFT ... L | R ... RIGHT")
    out["18-page"] = page_receivers()
    out["19-speaker"] = knob("kiwi-19", "A Bluetooth speaker connected: a speaker beside the name",
                             kiwi_face(speaker=True), "",
                             "the receivers in the speaker, a little behind  \u00b7  nothing to transmit")
    out["20-right-out"] = knob("kiwi-20", "The right ear's KiwiSDR cannot reach 6 m: quiet, and said so",
                               kiwi_face(dbm=-97, peak=-91, band="6m", mode="USB", digits=" 50150" "00",
                                         right_note="can't reach"), "",
                               "the left ear on 6 m  \u00b7  the right ear quiet until the dial is back under 30 MHz")
    # The knob's own battery, while it runs on it (ui.c knob_batt_show).
    md.use_palette("kiwi")
    out["21-battery"] = knob("kiwi-21", "On its own battery: the knob's charge at the top of the arc",
                             f + md.knob_battery(85), "",
                             "unplugged: its charge, by the quarter  \u00b7  none on USB power")
    lock_pictures(out, 22, "kiwi", "kiwi", "Kiwi888", f, keys=False)
    return out


# --- the EpiBeam firmware ------------------------------------------------------------

BEAM_NAMES = ("N", "NE", "E", "SE", "S", "SW", "W", "NW")
BEAM_GEAR_Y = 180 + 84                    # ui.c BEAM_GEAR_Y
BEAM_BATT_Y = 63                          # ui.c KNOB_BATT_Y on EpiBeam: inside the ring


def centred(cx, cy, s, size, colour, weight=400):
    """An LVGL label of one line aligned CENTER on (cx, cy): its box the
    font's line high, the baseline where Montserrat's base_line puts it."""
    f = montserrat(size)
    top = cy - f.line_height // 2
    return md.text(cx, top + f.line_height - f.base_line, esc(s), size, colour, weight)


def gear(cx, cy, colour):
    """LV_SYMBOL_SETTINGS at 28 px, centred on (cx, cy): eight teeth round a
    hole of the button's colour."""
    pts = []
    for k in range(8):
        for d, r in ((-15, 9.5), (-9, 13.5), (9, 13.5), (15, 9.5)):
            a = math.radians(k * 45 + d)
            pts.append(f"{cx + r * math.sin(a):.1f},{cy - r * math.cos(a):.1f}")
    return (f'<polygon points="{" ".join(pts)}" fill="{colour}"/>'
            f'<circle cx="{cx}" cy="{cy}" r="4.5" fill="{md.BG1}"/>')


def beam_face(n=4, beam=0, linked=False):
    """EpiBeam's compass (ui.c beam_build, beam_draw): the array's directions
    round a ring at the glass's edge, 178 px out and 46 wide, each its
    half-step either side less 2 degrees, the beam's in the accent with its
    name in the ground's colour; in the middle the array, the beam's name at
    48 px and its heading, EpiCenter's link; under them the gear's button."""
    names = BEAM_NAMES if n == 8 else BEAM_NAMES[::2]
    span = 360 // n
    s = [f'<rect x="0" y="0" width="360" height="360" fill="{md.BG}"/>']
    for i, name in enumerate(names):
        deg, on = i * span, i == beam
        s.append(md.block(270 + deg - span / 2 + 2, 270 + deg + span / 2 - 2,
                          md.ACCENT if on else md.SUBTLE, r=178 - 23, band=46))
        a = math.radians(deg)
        s.append(centred(180 + round(155 * math.sin(a)), 180 - round(155 * math.cos(a)), name, 20,
                         md.BG if on else md.LABEL))
    s.append(centred(180, 180 - 76, "8 DIRECTIONS" if n == 8 else "4-SQUARE", 14, md.LABEL))
    s.append(centred(180, 180 - 30, names[beam], 48, md.TEXT, 600))
    s.append(centred(180, 180 + 14, f"{beam * span}\u00b0", 20, md.TEXT2))
    s.append(centred(180, 180 + 44, "EpiCenter" if linked else "EpiCenter: not connected", 14,
                     md.GREEN if linked else md.LABEL))
    s.append(f'<circle cx="180" cy="{BEAM_GEAR_Y}" r="25" fill="{md.BG1}" stroke="{md.SUBTLE}" '
             f'stroke-width="2"/>' + gear(180, BEAM_GEAR_Y, md.TEXT2))
    return "".join(s)


def ring_at(deg, r=155):
    """A point on the ring at `deg` clockwise from north, on the screen."""
    a = math.radians(deg)
    return 180 + r * math.sin(a), 180 - r * math.cos(a)


def epibeam_pictures():
    md.use_palette("svxconnect")         # EpiBeam wears the setup firmware's ink and gold
    f = beam_face()
    left = [(60, 82, "the array's directions \u00b7 tap one"), (140, 104, "4-SQUARE or 8 DIRECTIONS"),
            (80, 224, "EpiCenter's link"), (160, BEAM_GEAR_Y, "the gear")]
    right = [(256, 46, "the beam, lit"), (196, 150, "its direction"), (194, 194, "its heading"),
             (200, BEAM_GEAR_Y, "tap: card \u00b7 hold 3 s: picker")]
    out = {}
    out["01-face"] = callouts("epi-01", "The EpiBeam firmware's face", f, left, right)
    out["02-turn"] = knob("epi-02", "Turn: the next direction",
                          beam_face(beam=1), turn(),
                          "a direction a detent  \u00b7  at rest, E counted out: two clicks")
    out["03-tap"] = knob("epi-03", "Tap the ring: that direction",
                         beam_face(beam=2), tap(*ring_at(180)), "tap the ring: that direction, S")
    out["04-eight"] = knob("epi-04", "An array of 8 directions: the corners between",
                           beam_face(8, 5), turn(), "8 directions: NE two clicks, on round to NW eight")
    card = address_card(f"EpiBeam {VERSION}\n{POWER}\n{ADDRESSES}")
    out["05-card"] = knob("epi-05", "The gear: the address card, for the configuration page",
                          f + card + lock_pill(False), tap(180, BEAM_GEAR_Y),
                          "tap the gear: the address card  \u00b7  again: away")
    out["06-picker"] = knob("epi-06", "The gear held three seconds: back to the setup firmware?",
                            dimmed(f) + question("FIRMWARE?", "turn the knob for the picker\n"
                                                              "tap to cancel; WiFi is kept"),
                            tap(180, BEAM_GEAR_Y, ring=24), "hold the gear 3 s until it buzzes, then turn")
    # The knob's own battery, while it runs on it (ui.c knob_batt_show):
    # inside the ring, over the array -- on the ring it sat on N.
    out["07-battery"] = knob("epi-07", "On its own battery: the knob's charge inside the ring",
                             f + md.battery_glyph(md.KNOB_BATT_X, BEAM_BATT_Y + 18, 85, md.battery_colour(85)),
                             "", "unplugged: its charge, by the quarter  \u00b7  none on USB power")
    out["08-lock"] = knob("epi-08", "The address card's LOCK", f + card + lock_pill(False), tap(252, 84),
                          "tap the gear for the card  \u00b7  tap LOCK")
    # The padlock level with the battery's row (ui.c KLOCK_BADGE_Y), 26 px
    # lower than on the radios' faces.
    out["09-locked"] = knob("epi-09", "Locked: the padlock inside the ring",
                            beam_face(beam=1) + lock_closed(180, 47.5 + 26, md.WARN), "",
                            "turns and taps refused  \u00b7  the gear still brings the card, UNLOCK on it")
    return out


# --- the ADS-B Radar firmware --------------------------------------------------------

AD_R = 172                                # ui.c AD_R: the radar's edge
AD_GEAR_Y = 180 + 146                     # ui.c AD_GEAR_Y
AD_FOOT_Y = 180 + 106                     # ui.c AD_FOOT_Y: the feed's state
AD_BANNER_Y = 62                          # ui.c AD_BANNER_Y
AD_BG, AD_RING, AD_RING_HALF, AD_AXIS = "#030806", "#0E3A1C", "#16582C", "#0A2614"
AD_TICK, AD_COAST, AD_BORDER, AD_APT = "#2E8C4A", "#2A7088", "#1D4A58", "#5FA36A"
AD_TEXT, AD_SWEEP, AD_EMERGENCY, AD_TYPE = "#8CFF9E", "#35DB5F", "#FF3030", "#E5A823"
AD_BAND = ("#9CA3AF", "#FF6EC7", "#FFA22D", "#FFE15C", "#4ADE80", "#38BDF8", "#A78BFA")
AD_RANGES = (5, 10, 20, 30, 50, 75, 100, 150, 200, 250)
AD_MIDDLE = (50.9014, 4.4844)             # EBBR, as the page's airport puts it
# A sky round Brussels: callsign, type, registration, NM and degrees from the
# middle, ft (None on the ground), knots, track, ft/min, squawk.
AD_SKY = (
    ("UAE183", "B77W", "A6-EGO", 8, 150, 3200, 180, 250, -900, ""),
    ("BEL7JK", "A320", "OO-SNA", 12, 300, 12000, 310, 290, 2200, ""),
    ("", "C172", "OO-ABC", 15, 80, 1500, 95, 120, 0, ""),
    ("WZZ1RS", "A21N", "HA-LVA", 20, 255, 35000, 450, 80, 0, ""),
    ("KLM1724", "E190", "PH-EZA", 22, 20, 8500, 280, 200, -1500, ""),
    ("BAF589", "A400", "CT-01", 26, 335, 18000, 330, 150, 0, ""),
    ("EZY82QK", "A320", "G-EZOA", 28, 235, 15000, 290, 60, -2500, "7700"),
    ("RYR4TF", "B738", "EI-DCL", 32, 205, 37000, 455, 30, 0, ""),
    ("SWR781", "BCS3", "HB-JCA", 33, 125, 34000, 440, 310, 0, ""),
    ("TUI3TH", "B38M", "OO-TMA", 36, 62, 33000, 430, 240, 0, ""),
    ("DLH9AB", "A20N", "D-AINA", 41, 98, 39000, 460, 270, 0, ""),
    ("BAW412", "A321", "G-EUXC", 44, 288, 25000, 410, 110, 0, ""),
    ("SAS1594", "A20N", "SE-ROA", 46, 38, 32000, 450, 220, 0, ""),
)


@functools.lru_cache(maxsize=None)
def adsb_tables():
    """components/adsb_client/tables (tools/adsb_tables.py): the map's runs,
    each its kind and points in 1e-4 degree, and the airports."""
    import struct
    d = os.path.join(ROOT, "components", "adsb_client", "tables")
    m = open(os.path.join(d, "map.bin"), "rb").read()
    nruns, _ = struct.unpack_from("<II", m, 4)
    steps_at = 12 + nruns * 24
    runs = []
    for i in range(nruns):
        lat, lon, la0, la1, lo0, lo1, first, n, kind = struct.unpack_from("<iihhhhIHB", m, 12 + i * 24)
        pts = [(lat, lon)]
        for k in range(n):
            dla, dlo = struct.unpack_from("<hh", m, steps_at + (first + k) * 4)
            lat, lon = lat + dla, lon + dlo
            pts.append((lat, lon))
        runs.append((kind, (la0, la1, lo0, lo1), pts))
    a = open(os.path.join(d, "airports.bin"), "rb").read()
    (napt,) = struct.unpack_from("<I", a, 4)
    apts = []
    for i in range(napt):
        lat, lon, code, large = struct.unpack_from("<ii4sB", a, 8 + i * 16)
        apts.append((lat, lon, code.rstrip(b"\0").decode(), bool(large)))
    return runs, apts


def ad_project(lat, lon, nm, mid=AD_MIDDLE):
    """adsb_project(): pixels from the middle, x east, y down."""
    px = AD_R / nm
    dl = (lon - mid[1] + 540.0) % 360.0 - 180.0
    return dl * 60 * math.cos(math.radians(mid[0])) * px, -(lat - mid[0]) * 60 * px


def ad_at(dist, brg, mid=AD_MIDDLE):
    """The place `dist` NM from the middle at `brg` degrees, flat."""
    b = math.radians(brg)
    return (mid[0] + dist * math.cos(b) / 60, mid[1] + dist * math.sin(b) / (60 * math.cos(math.radians(mid[0]))))


def ad_band(alt):
    """adsb_band(): ADSB-Radar's altitude bands."""
    if alt is None:
        return 0
    return 1 + sum(alt >= t for t in (2000, 10000, 20000, 30000, 40000))


def ad_level(alt, vrate):
    """ad_level(): FL above 10000 ft, else feet; an arrow climbing or
    descending more than 300 ft/min."""
    arrow = " \u2191" if vrate > 300 else " \u2193" if vrate < -300 else ""
    if alt is None:
        return "GND"
    return f"FL{(alt + 50) // 100:03d}{arrow}" if alt >= 10000 else f"{alt} ft{arrow}"


def ad_plate(rng):
    """ad_set_plate(): the top the map leaves clear, as wide as the range's
    label or the battery's row with the padlock."""
    half = max(montserrat(20).width(rng) // 2 + 6, 25 // 2 + 30)
    return (180 - half, 0, 180 + half, 37 + 24)


def ad_in_glass(b):
    return all((x - 180) ** 2 + (y - 180) ** 2 <= (AD_R - 2) ** 2 for x in (b[0], b[2]) for y in (b[1], b[3]))


def ad_free(boxes, b):
    return all(b[0] > o[2] or o[0] > b[2] or b[1] > o[3] or o[1] > b[3] for o in boxes)


def radar_face(range_i=4, sky=AD_SKY, show="", types=("A400",), emergency=True, sweep=40, sel=None,
               feed="adsb.fi", ok=True, nopos=False, airports=True, countries=True, banner=True):
    """ADS-B Radar's face (ui.c ADSB_FACE): the map -- the coasts and borders,
    the rings at a quarter, half and three quarters, the compass's ticks,
    the airports -- the aircraft over it as arrows along their track,
    labelled nearest first where there is room; the sweep; the range at the
    top, the feed's state and the gear at the foot; an alert's banner."""
    nm = AD_RANGES[range_i]
    rng = f"{nm} NM" + (f"  \u2022  {show}" if show else "")
    plate = ad_plate(rng)
    s = [f'<rect x="0" y="0" width="360" height="360" fill="{AD_BG}"/>',
         f'<clipPath id="adglass"><circle cx="180" cy="180" r="{AD_R}"/></clipPath>']
    lines_ = []
    if not nopos:
        lines_.append(f'<path d="M180,{180 - AD_R}V{180 + AD_R}M{180 - AD_R},180H{180 + AD_R}" '
                      f'stroke="{AD_AXIS}" stroke-width="1"/>')
        if countries:
            runs, _ = adsb_tables()
            half = nm * 1.05
            dlat, dlon = half / 60, half / (60 * math.cos(math.radians(AD_MIDDLE[0])))
            paths = {0: [], 1: []}
            for kind, (la0, la1, lo0, lo1), pts in runs:
                if la1 / 100 < AD_MIDDLE[0] - dlat or la0 / 100 > AD_MIDDLE[0] + dlat:
                    continue
                if lo1 / 100 < AD_MIDDLE[1] - dlon or lo0 / 100 > AD_MIDDLE[1] + dlon:
                    continue
                xy = [ad_project(la / 1e4, lo / 1e4, nm) for la, lo in pts]
                paths[kind].append("M" + "L".join(f"{180 + x:.1f},{180 + y:.1f}" for x, y in xy))
            for kind, col in ((0, AD_COAST), (1, AD_BORDER)):
                if paths[kind]:
                    lines_.append(f'<path d="{"".join(paths[kind])}" fill="none" stroke="{col}" '
                                  f'stroke-width="1" stroke-linejoin="round"/>')
        for r, col in ((AD_R // 4, AD_RING), (AD_R // 2, AD_RING_HALF), (AD_R * 3 // 4, AD_RING), (AD_R, AD_RING_HALF)):
            lines_.append(f'<circle cx="180" cy="180" r="{r}" fill="none" stroke="{col}" stroke-width="1"/>')
        for deg in range(0, 360, 10):
            ln = 12 if deg % 90 == 0 else 8 if deg % 30 == 0 else 4
            a = math.radians(deg)
            lines_.append(f'<line x1="{180 + math.sin(a) * (AD_R - ln):.1f}" y1="{180 - math.cos(a) * (AD_R - ln):.1f}" '
                          f'x2="{180 + math.sin(a) * AD_R:.1f}" y2="{180 - math.cos(a) * AD_R:.1f}" '
                          f'stroke="{AD_TICK}" stroke-width="1"/>')
        lines_.append(f'<path d="M177,180H184M180,177V184" stroke="{AD_TICK}" stroke-width="1"/>')
    s.append(f'<g clip-path="url(#adglass)">{"".join(lines_)}</g>')
    # The plate: the map's lines stop short of it.
    s.append(f'<rect x="{plate[0]}" y="0" width="{plate[2] - plate[0] + 1}" height="{plate[3] + 1}" fill="{AD_BG}"/>')

    # The airports near, and the aircraft where the filter and the edge leave them.
    apt_lab = []
    if not nopos and airports:
        _, apts = adsb_tables()
        for la, lo, code, large in apts:
            if not large and nm > 100:
                continue
            x, y = ad_project(la / 1e4, lo / 1e4, nm)
            if x * x + y * y > (AD_R - 6) ** 2:
                continue
            ix, iy = 180 + round(x), 180 + round(y)
            if plate[0] - 44 <= ix <= plate[2] and iy <= plate[3] + 8:
                continue
            s.append(f'<rect x="{ix - 1}" y="{iy - 1}" width="3" height="3" fill="{AD_APT}"/>')
            if large or nm <= 50:
                apt_lab.append(((ix - 180) ** 2 + (iy - 180) ** 2, ix, iy, code))
        apt_lab = sorted(apt_lab)[:16]
    planes = []
    for cs, typ, reg, dist, brg, alt, gs, trk, vr, sq in ([] if nopos else sky):
        if show == "MIL" and typ != "A400" and sq != "7700":
            continue
        la, lo = ad_at(dist, brg)
        x, y = ad_project(la, lo, nm)
        if x * x + y * y > (AD_R - 3) ** 2:
            continue
        em = sq == "7700" and emergency
        col = AD_EMERGENCY if em else AD_TYPE if typ in types else AD_BAND[ad_band(alt)]
        planes.append((180 + round(x), 180 + round(y), cs or reg, typ, alt, vr, trk, col, em))

    # The banner: an emergency first, else a type on the list; how many more.
    alerts = [p for p in planes if p[8]] + [p for p in planes if not p[8] and p[3] in types]
    boxes = [plate, (100, AD_FOOT_Y - 10, 260, 359)]
    over = ""
    if banner and alerts and sel is None:
        b = alerts[0]
        what = ("EMERGENCY" if b[8] else b[3]) + "  " + b[2] + (f"  +{len(alerts) - 1}" if len(alerts) > 1 else "")
        w = montserrat(14).width(what) + 20
        bg, fg = (AD_EMERGENCY, md.TEXT) if b[8] else ("#3A2C08", md.ACCENT)
        over += (f'<rect x="{180 - w / 2:.1f}" y="{AD_BANNER_Y}" width="{w}" height="22" rx="10" fill="{bg}"/>'
                 + md.text(180, AD_BANNER_Y + 16, esc(what), 14, fg))
        boxes.append((55, AD_BANNER_Y - 2, 305, AD_BANNER_Y + 26))
    texts = []
    if not nopos:
        half = nm / 2
        t = f"{half:g}" if half != int(half) else f"{int(half)}"
        hx, hy = 180 + int(AD_R / 2 * 0.707) + 3, 180 - int(AD_R / 2 * 0.707) - 14
        texts.append(md.text(hx, hy + 11, t, 12, "#36784C", anchor="start"))
        boxes.append((hx, hy, hx + 30, hy + 14))
        texts.append(md.text(180 + AD_R - 22, 180 + 3, "E", 12, AD_TICK, anchor="start"))
        texts.append(md.text(180 - AD_R + 10, 180 + 3, "W", 12, AD_TICK, anchor="start"))
        boxes += [(180 + AD_R - 24, 171, 180 + AD_R, 188), (180 - AD_R, 171, 180 - AD_R + 26, 188)]
    f12 = montserrat(12)
    labelled = 0
    for x, y, name, typ, alt, vr, trk, col, em in planes:
        a = math.radians(trk)
        pts = " ".join(f"{x + r * math.sin(a + o):.1f},{y - r * math.cos(a + o):.1f}"
                       for r, o in ((8, 0), (6, 2.44), (6, -2.44)))
        s.append(f'<polygon points="{pts}" fill="{col}"/>')
        if name == sel:
            s.append(f'<circle cx="{x}" cy="{y}" r="11" fill="none" stroke="{md.TEXT}" stroke-width="2"/>')
        elif em:
            s.append(f'<circle cx="{x}" cy="{y}" r="11" fill="none" stroke="{AD_EMERGENCY}" stroke-width="2"/>')
        if labelled >= 12:
            continue
        lv = ad_level(alt, vr)
        w = max(f12.width(name), f12.width(lv)) + 2
        box = (x + 8, y - 15, x + 8 + w, y + 14)
        left = False
        if not ad_in_glass(box) or not ad_free(boxes, box):
            box, left = (x - 8 - w, y - 15, x - 8, y + 14), True
            if not ad_in_glass(box) or not ad_free(boxes, box):
                continue
        boxes.append(box)
        labelled += 1
        tx, anchor = (box[2], "end") if left else (box[0], "start")
        texts.append(md.text(tx, box[1] + 12, esc(name), 12, col, anchor=anchor)
                     + md.text(tx, box[1] + 27, esc(lv), 12, col, anchor=anchor))
    for _, ix, iy, code in apt_lab:
        w = f12.width(code)
        box = (ix + 4, iy - 7, ix + 4 + w, iy + 7)
        if not ad_in_glass(box) or not ad_free(boxes, box):
            continue
        if any(box[0] - 6 <= p[0] <= box[2] + 6 and box[1] - 6 <= p[1] <= box[3] + 6 for p in planes):
            continue
        boxes.append(box)
        texts.append(md.text(ix + 4, iy + 5, code, 12, "#305638", anchor="start"))
    s += texts
    if sweep is not None and not nopos and sel is None:
        # ad_sweep_cb: the line and a trail behind it, fainter each step; not
        # over the plate.
        wedges = []
        for i, opa in enumerate((64, 50, 38, 28, 20, 13, 8, 4)):
            a1 = math.radians(sweep - i * 5)
            a0 = math.radians(sweep - i * 5 - 5)
            wedges.append(f'<path d="M180,180L{180 + AD_R * math.sin(a0):.1f},{180 - AD_R * math.cos(a0):.1f}'
                          f'A{AD_R},{AD_R} 0 0 1 {180 + AD_R * math.sin(a1):.1f},{180 - AD_R * math.cos(a1):.1f}Z" '
                          f'fill="{AD_SWEEP}" fill-opacity="{opa / 255:.3f}"/>')
        a = math.radians(sweep)
        wedges.append(f'<line x1="180" y1="180" x2="{180 + AD_R * math.sin(a):.1f}" '
                      f'y2="{180 - AD_R * math.cos(a):.1f}" stroke="{AD_SWEEP}" stroke-opacity="0.59" '
                      f'stroke-width="2"/>')
        s.append(f'<g clip-path="url(#adglass)">{"".join(wedges)}'
                 f'<rect x="{plate[0]}" y="0" width="{plate[2] - plate[0] + 1}" height="{plate[3] + 1}" '
                 f'fill="{AD_BG}"/></g>')
    s.append(md.text(180, 12 + 17, esc(rng), 20, AD_TEXT))
    foot = feed if (nopos or not ok) else f"{feed}  \u2022  {len(planes)}"
    s.append(centred(180, AD_FOOT_Y, foot, 14, md.LABEL if ok and not nopos else md.WARN))
    if nopos:
        s.append(centred(180, 180 - 24, "NO LOCATION", 20, md.WARN))
        s.append(lines(180, 180 + 14, "Set the radar's middle\non the configuration page", 14, md.TEXT2, 17))
    s.append(f'<circle cx="180" cy="{AD_GEAR_Y}" r="20" fill="{md.BG1}" stroke="{md.SUBTLE}" '
             f'stroke-width="2"/>' + gear(180, AD_GEAR_Y, md.TEXT2))
    s.append(over)
    return "".join(s)


def adsb_card(cs, typ, reg, route, level, speed, where, squawk="", em=False):
    """ad_card_fill(): an aircraft's card, 250 x 196 over the radar, its
    callsign at 28 px, its type and registration, the route in gold, the
    level at 20 px, speed and track, where it is, the squawk."""
    s = (f'<rect x="55" y="78" width="250" height="196" rx="18" fill="{md.BG1}" '
         f'stroke="{md.ACCENT}" stroke-width="2"/>'
         + centred(180, 78 + 10 + 16, cs, 28, md.TEXT))
    rows = ((46, f"{typ}  \u2022  {reg}", 14, md.LABEL), (68, route, 14, md.ACCENT), (91, level, 20, md.TEXT),
            (120, speed, 14, md.TEXT2), (142, where, 14, md.TEXT2),
            (164, squawk, 14, md.DANGER if em else md.LABEL))
    for y, t, size, col in rows:
        if t:
            s += centred(180, 78 + y + montserrat(size).line_height // 2, t, size, col)
    return s


def adsb_pictures():
    md.use_palette("svxconnect")         # ADS-B Radar wears the setup firmware's ink and gold
    f = radar_face()
    left = [(144, 22, "the range  \u00b7  turn the dial"), (104, 73, "an alert, called out"),
            (101, 235, "an emergency, ringed"), (116, 286, "the feed  \u00b7  aircraft shown"),
            (160, AD_GEAR_Y, "the gear")]
    right = [(257, 88, "the sweep"), (240, 114, "half the range, on its ring"),
             (231, 171, "an aircraft: callsign, level"), (333, 176, "the compass"),
             (200, AD_GEAR_Y, "tap: card \u00b7 hold 3 s: picker")]
    out = {}
    out["01-face"] = callouts("adsb-01", "The ADS-B Radar firmware's face", f, left, right)
    out["02-turn"] = knob("adsb-02", "Turn: the next range",
                          radar_face(range_i=5), turn(), "a range a detent, 5 to 250 NM  \u00b7  a click each")
    la, lo = ad_at(8, 150)
    x, y = ad_project(la, lo, 50)
    card = adsb_card("UAE183", "B77W", "A6-EGO", "Dubai  \u203a  Brussels", "3200 ft \u2193  -900 ft/min",
                     "180 kt  \u2022  250\u00b0", "8.0 NM at 150\u00b0")
    out["03-card"] = knob("adsb-03", "Tap an aircraft: its card",
                          radar_face(sel="UAE183", sweep=None) + card, tap(180 + x, 180 + y),
                          "tap an aircraft: its card, its route  \u00b7  tap again: away")
    out["04-emergency"] = knob("adsb-04", "An emergency: its card",
                               radar_face(sel="EZY82QK", sweep=None)
                               + adsb_card("EZY82QK", "A320", "G-EZOA", "", "FL150 \u2193  -2500 ft/min",
                                           "290 kt  \u2022  060\u00b0", "28 NM at 235\u00b0", "7700  EMERGENCY", True),
                               "", "a squawk of 7700: called out in red, and the knob buzzes")
    out["05-mil"] = knob("adsb-05", "Military aircraft only",
                         radar_face(show="MIL"), "", "Show: military aircraft  \u2022  an emergency always shows")
    out["06-range"] = knob("adsb-06", "250 NM: the coasts and the large airports",
                           radar_face(range_i=9, sweep=300), turn(), "the widest range: the large airports only")
    acard = address_card(f"ADS-B Radar {VERSION}\n{POWER}\n{ADDRESSES}")
    out["07-card"] = knob("adsb-07", "The gear: the address card, for the configuration page",
                          f + acard + lock_pill(False), tap(180, AD_GEAR_Y),
                          "tap the gear: the address card  \u00b7  again: away")
    out["08-picker"] = knob("adsb-08", "The gear held three seconds: back to the setup firmware?",
                            dimmed(f) + question("FIRMWARE?", "turn the knob for the picker\n"
                                                              "tap to cancel; WiFi is kept"),
                            tap(180, AD_GEAR_Y, ring=24), "hold the gear 3 s until it buzzes, then turn")
    # The knob's own battery and the padlock under the range, on the plate.
    out["09-battery"] = knob("adsb-09", "On its own battery: the knob's charge under the range",
                             f + md.battery_glyph(md.KNOB_BATT_X, 37 + 18, 85, md.battery_colour(85)),
                             "", "unplugged: its charge, by the quarter  \u00b7  none on USB power")
    out["10-lock"] = knob("adsb-10", "The address card's LOCK", f + acard + lock_pill(False), tap(252, 84),
                          "tap the gear for the card  \u00b7  tap LOCK")
    out["11-locked"] = knob("adsb-11", "Locked: the padlock under the range",
                            f + lock_closed(180, 47.5, md.WARN), "",
                            "taps on the aircraft refused  \u00b7  the gear still brings the card, UNLOCK on it")
    # The radar's clip, its id each picture's own: a page may show several.
    return {k: v.replace("adglass", f"adsb-{k[:2]}-glass") for k, v in out.items()}


# --- the Clock firmware --------------------------------------------------------------

# Wednesday 4 November 2026, 14:52:08 in Brussels: 13:52 UTC.
CK_NOW = dict(h=14, m=52, s=8, uh=13, um=52, wday="WED", mday=4, mon="NOV")
CK_LUME, CK_LUME_RIM, CK_STALE = "#E4EDD3", "#8E9686", "#707884"   # clockface.c


def ck_at(deg, r):
    """clockface.c polar(): `deg` clockwise from 12, `r` out from the middle."""
    a = math.radians(deg)
    return 180 + r * math.sin(a), 180 - r * math.cos(a)


def ck_line(x0, y0, x1, y1, w, colour, rnd):
    return (f'<line x1="{x0:.1f}" y1="{y0:.1f}" x2="{x1:.1f}" y2="{y1:.1f}" stroke="{colour}" '
            f'stroke-width="{w}" stroke-linecap="{"round" if rnd else "butt"}"/>')


def ck_ray(deg, r0, r1, w, colour, rnd=False):
    """clockface.c ray(): a line out from the middle at `deg`, r0 to r1."""
    return ck_line(*ck_at(deg, r0), *ck_at(deg, r1), w, colour, rnd)


def ck_disc(x, y, r, colour):
    """A disc LVGL draws from x - r to x + r, both inclusive."""
    return f'<circle cx="{x:.1f}" cy="{y:.1f}" r="{r + 0.5:.1f}" fill="{colour}"/>'


def ck_ring(r, w, colour):
    """lv_draw_arc all round: `r` its outer edge, `w` of it inwards."""
    return (f'<circle cx="180" cy="180" r="{r - w / 2:.1f}" fill="none" stroke="{colour}" '
            f'stroke-width="{w}"/>')


def ck_bell(x, y, h, colour):
    """LV_SYMBOL_BELL, as Montserrat's symbols draw it: a bell `h` high,
    its top at y, centred on x, its clapper under the rim."""
    u = h / 14.0
    def p(a, b):
        return f"{x + (a - 7) * u:.1f},{y + b * u:.1f}"
    return (f'<path d="M{p(7, 0)} C{p(3.6, 0.6)} {p(3, 4)} {p(3, 7)} L{p(3, 9)} L{p(1, 11)} '
            f'L{p(13, 11)} L{p(11, 9)} L{p(11, 7)} C{p(11, 4)} {p(10.4, 0.6)} {p(7, 0)} Z" fill="{colour}"/>'
            f'<circle cx="{x:.1f}" cy="{y + 12.6 * u:.1f}" r="{1.6 * u:.1f}" fill="{colour}"/>')


def ck_mark(cy, mark, size, colour):
    """clockface.c's mark: the next alarm -- LV_SYMBOL_BELL, a space and its
    time -- or the snooze's time, centred on cy as one label."""
    f = montserrat(size)
    if not mark.startswith("BELL "):
        return centred(180, cy, mark, size, colour)
    t = " " + mark[5:]
    bw = round(size * 0.86)                  # the bell's advance, Montserrat's symbols
    w = bw + f.width(t)
    x0 = 180 - w / 2
    top = cy - f.line_height // 2
    return (ck_bell(x0 + bw / 2, top + (f.line_height - size) / 2, size, colour)
            + md.text(x0 + bw, top + f.line_height - f.base_line, esc(t), size, colour, anchor="start",
                      extra=' xml:space="preserve"'))


def ck_dial(face, have=True, t=CK_NOW, mark=""):
    """clockface.c dial_shack, dial_diver, dial_classic: the dial without
    its hands, and NO TIME YET where the date goes until there is a time;
    the Clock's next alarm, `mark`, where each dial has room ("BELL 07:30"
    is the bell and the time)."""
    s = ['<rect x="0" y="0" width="360" height="360" fill="#000000"/>']
    date = f"{t['wday']} {t['mday']}"
    if face == 0:
        s += [ck_disc(180, 180, 179, "#0B1424"), ck_ring(179, 2, "#2A3A55")]
        for i in range(60):
            s.append(ck_ray(i * 6, 161, 171, 2, "#8A9BB5") if i % 5 else ck_ray(i * 6, 150, 171, 4, "#FFFFFF"))
        for h in range(1, 13):
            s.append(centred(*ck_at(h * 30, 131), str(h), 28, "#FFFFFF"))
        s.append(ck_ring(112, 1, "#2E4A73"))
        for h in range(24):
            s.append(ck_ray(h * 15, 107, 113, 2, "#5A9BFF"))
            s.append(centred(*ck_at(h * 15, 97), str(h), 14, "#7FA9E8"))
        s.append(ck_mark(132, mark, 14, "#FFB000") if mark else centred(180, 132, "VFO KNOB", 14, "#8A9BB5"))
        if have:
            s.append(centred(180, 226, f"{date} {t['mon']}", 14, "#FFFFFF"))
            s.append(centred(180, 244, "UTC", 14, "#FF5A36"))
    elif face == 1:
        s += [ck_disc(180, 180, 179, "#060708"), ck_ring(179, 29, "#1B1E23")]
        for i in range(1, 60):
            if i % 10 == 0:
                s.append(centred(*ck_at(i * 6, 165), str(i), 14, "#E8ECF0"))
            elif i % 5 == 0:
                s.append(ck_ray(i * 6, 158, 172, 4, "#E8ECF0"))
            else:
                s.append(ck_ray(i * 6, 163, 171, 2, "#A8ADB4"))
        x, y = ck_at(0, 152)
        s.append(f'<polygon points="{x:.1f},{y:.1f} 170,4 190,4" fill="#E8ECF0"/>')
        s += [ck_disc(180, 12, 3, CK_LUME), ck_ring(150, 2, "#30343A")]
        for h in range(1, 13):
            if h == 3:
                continue
            if h == 12:
                for d in (-3.2, 3.2):
                    s += [ck_ray(d, 112, 142, 9, CK_LUME_RIM), ck_ray(d, 113, 141, 6, CK_LUME)]
            elif h in (6, 9):
                s += [ck_ray(h * 30, 110, 143, 13, CK_LUME_RIM), ck_ray(h * 30, 111, 142, 10, CK_LUME)]
            else:
                x, y = ck_at(h * 30, 128)
                s += [ck_disc(x, y, 10, CK_LUME_RIM), ck_disc(x, y, 8, CK_LUME)]
        # The day-date window at 3: x 262..322, y 167..193, its border inside.
        s.append('<rect x="263" y="168" width="59" height="25" rx="3" fill="#F4F4F0" '
                 'stroke="#8C9096" stroke-width="2"/>')
        if have:
            s.append(centred(292, 180, date, 14, "#101214"))
        s.append(centred(180, 118, "VFO KNOB", 14, "#E8ECF0"))
        s.append(centred(180, 236, "AUTOMATIC", 14, "#FF5A36"))
        if mark:
            s.append(ck_mark(262, mark, 14, "#E8ECF0"))
    else:
        s += [ck_disc(180, 180, 179, "#F2F2EE"), ck_ring(179, 4, "#303030")]
        for i in range(60):
            s.append(ck_ray(i * 6, 161, 171, 3, "#1A1A1A") if i % 5 else ck_ray(i * 6, 128, 171, 10, "#1A1A1A"))
        if have:
            s.append(centred(180, 254, date, 14, "#6A6A6A"))
        if mark:
            s.append(ck_mark(280, mark, 14, "#6A6A6A"))
    if not have:
        s.append(centred(180, 230 if face == 0 else 270, "NO TIME YET", 20,
                         "#6A6A6A" if face == 2 else "#C0C8D4"))
    return "".join(s)


def ck_hand(deg, tail, length, w, colour, rim=None, rnd=True, dot=None):
    """clockface.c hand(), hand_draw(): from `tail` behind the middle to
    `length` before it; a rim 3 px wider under it; the diver's lume dot."""
    (x0, y0), (x1, y1) = ck_at(deg + 180, tail), ck_at(deg, length)
    s = ck_line(x0, y0, x1, y1, w + 3, rim, rnd) if rim else ""
    s += ck_line(x0, y0, x1, y1, w, colour, rnd)
    if dot:
        r, at = dot
        x, y = x0 + (x1 - x0) * at, y0 + (y1 - y0) * at
        s += ck_disc(x, y, r + 2, colour) + ck_disc(x, y, r, CK_LUME)
    return s


def ck_hands(face, t=CK_NOW, seconds=True, fresh=True):
    """clockface.c hands_for() and cap(): grey once the time is a day old."""
    hr = (t["h"] % 12) * 30 + t["m"] * 0.5
    mn = t["m"] * 6 + t["s"] // 10
    sc = t["s"] * 6
    g = lambda c: c if fresh else CK_STALE          # noqa: E731
    if face == 0:
        s = (ck_hand(hr, 14, 80, 8, g("#FFFFFF")) + ck_hand(mn, 18, 138, 5, g("#FFFFFF"))
             + ck_hand((t["uh"] + t["um"] / 60) * 15, 0, 114, 2, g("#FF5A36")))
        if seconds:
            s += ck_hand(sc, 30, 152, 2, g("#FFB000"))
        return s + ck_disc(180, 180, 6, "#FFFFFF") + ck_disc(180, 180, 2, "#0B1424")
    if face == 1:
        s = (ck_hand(hr, 0, 84, 12, g(CK_LUME), "#2A2E33")
             + ck_hand(mn, 0, 128, 8, g(CK_LUME), "#2A2E33"))
        if seconds:
            s += ck_hand(sc, 34, 142, 3, g("#E03A2E"), dot=(5, (34 + 98) / (34 + 142)))
        return s + ck_disc(180, 180, 7, "#C0C4C8")
    s = (ck_hand(hr, 24, 96, 14, g("#1A1A1A"), rnd=False)
         + ck_hand(mn, 30, 150, 10, g("#1A1A1A"), rnd=False))
    if seconds:
        s += ck_hand(sc, 42, 142, 3, g("#D7261E"))
    return s + ck_disc(180, 180, 5, "#D7261E")


@functools.lru_cache(maxsize=None)
def hack96():
    return md.LvglFont(os.path.join(ROOT, "components", "ui", "font_hack_96.c"))


def hack_text(cx, cy, s, colour, f=None):
    """A Hack label centred on (cx, cy), as wide as Hack draws it whatever
    monospace face stands in."""
    f = f or hack96()
    base = cy - f.line_height // 2 + f.line_height - f.base_line
    return md.text(cx, base, esc(s), f.size, colour, family=md.READOUT_FONT,
                   extra=f' xml:space="preserve" textLength="{f.width(s)}" lengthAdjust="spacingAndGlyphs"')


def ck_digital_lines(t=CK_NOW, h24=True, have=True):
    """The digital face's three lines (clockface.c digital, saver.c
    digital_frame): HH:MM, its colon off on odd seconds; the date; UTC."""
    if not have:
        return "--:--", "NO TIME YET", ""
    hh = t["h"] if h24 else t["h"] % 12 or 12
    colon = " " if t["s"] % 2 else ":"
    big = f"{hh:02d}{colon}{t['m']:02d}" if h24 else f"{hh}{colon}{t['m']:02d}"
    ampm = "" if h24 else "  AM" if t["h"] < 12 else "  PM"
    return big, f"{t['wday']} {t['mday']} {t['mon']}{ampm}", f"{t['uh']:02d}:{t['um']:02d} UTC"


def ck_digital(t=CK_NOW, h24=True, have=True, mark=""):
    big, line, utc = ck_digital_lines(t, h24, have)
    s = '<rect x="0" y="0" width="360" height="360" fill="#000000"/>' + hack_text(180, 158, big, "#FFFFFF")
    s += centred(180, 214, line, 20, "#C0C8D4").replace("<text ", '<text xml:space="preserve" ', 1)
    s += centred(180, 238, utc, 20, "#5A9BFF") if utc else ""
    return s + (ck_mark(268, mark, 20, "#707884") if mark else "")


def clock_face(face=0, seconds=True, have=True, fresh=True, mark=""):
    if face == 3:
        return ck_digital(have=have, mark=mark)
    return ck_dial(face, have, mark=mark) + (ck_hands(face, seconds=seconds, fresh=fresh) if have else "")


# ui.c's alarms: the list, one being set, one ringing (AL_*).
AL_ON, AL_SET, AL_RING = "#3FD46C", "#FFB000", "#FF3030"
AL_TIME_Y, AL_DOTS_R = 160, 162


def al_list(n, hh, mm, days, on, mode=None, lit=(), pill=None):
    """ui.c al_show(): alarm `n` (1-10), or the one being set with the part
    the dial sets -- mode "HOURS", "MINUTES", "DAYS" -- in amber; the ten
    dots, those in `lit` on; the pill ON, OFF or STOP."""
    s = [f'<rect x="0" y="0" width="360" height="360" fill="#000000"/>']
    for i in range(10):
        here = i == n - 1
        d = 22 if here else 14
        x = 180 + round(AL_DOTS_R * math.sin(math.radians(i * 36))) - d // 2
        y = 180 - round(AL_DOTS_R * math.cos(math.radians(i * 36))) - d // 2
        fill = AL_ON if (i + 1 in lit or (here and (on or mode))) else md.DISABLED
        s.append(f'<circle cx="{x + d / 2:.1f}" cy="{y + d / 2:.1f}" r="{d / 2 - (1.5 if here else 0):.1f}" '
                 f'fill="{fill}"' + (f' stroke="{md.TEXT}" stroke-width="3"' if here else "") + '/>')
    t20 = montserrat(20)
    s.append(centred(180, 46 + t20.line_height // 2, mode or f"ALARM {n}", 20, AL_SET if mode else md.TEXT2))
    base = md.TEXT if (on or mode) else md.LABEL
    w = hack96().width("0")
    x0 = 180 - w // 2
    s.append(hack_text(x0 - w, AL_TIME_Y, hh, AL_SET if mode == "HOURS" else base))
    s.append(hack_text(x0 + w / 2, AL_TIME_Y, ":", base))
    s.append(hack_text(x0 + 2 * w, AL_TIME_Y, mm, AL_SET if mode == "MINUTES" else base))
    s.append(centred(180, AL_TIME_Y + 56 + t20.line_height // 2, days, 20,
                     AL_SET if mode == "DAYS" else md.TEXT2 if (on or mode) else md.LABEL))
    if not mode:
        word = pill or ("ON" if on else "OFF")
        bg = AL_RING if word == "STOP" else AL_ON if word == "ON" else md.SUBTLE
        s.append(f'<rect x="116" y="262" width="128" height="50" rx="25" fill="{bg}"'
                 + ("" if word in ("ON", "STOP") else f' stroke="{md.DISABLED}" stroke-width="2"') + '/>')
        s.append(centred(180, 287, word, 28, "#000000" if word in ("ON", "STOP") else md.LABEL))
    return "".join(s)


def al_ringing(hh_mm, days):
    """ui.c al_build()'s ringing screen: the red ring round the glass (on, as
    it flashes), the bell and ALARM, its time, its days, the hint."""
    t28, t20, t14 = montserrat(28), montserrat(20), montserrat(14)
    head = "  ALARM"
    bw = round(28 * 0.86)
    w = bw + t28.width(head)
    x0 = 180 - w / 2
    top = 58
    return ('<rect x="0" y="0" width="360" height="360" fill="#000000"/>'
            f'<circle cx="180" cy="180" r="172" fill="none" stroke="{AL_RING}" stroke-width="16"/>'
            + ck_bell(x0 + bw / 2, top + (t28.line_height - 28) / 2, 28, AL_RING)
            + md.text(x0 + bw, top + t28.line_height - t28.base_line, esc(head), 28, AL_RING, anchor="start",
                      extra=' xml:space="preserve"')
            + hack_text(180, AL_TIME_Y, hh_mm, md.TEXT)
            + centred(180, AL_TIME_Y + 56 + t20.line_height // 2, days, 20, md.TEXT2)
            + centred(180, 270 + t14.line_height // 2, "TAP TO STOP", 14, md.LABEL)
            + centred(180, 270 + t14.line_height * 3 // 2, "TURN TO SNOOZE", 14, md.LABEL))


def clock_pictures():
    md.use_palette("aethersdr")          # the card, the question and the lock wear AetherSDR's
    f = clock_face()
    left = [(*ck_at(312, 100), "local time: the hands"), (*ck_at(255, 97), "UTC, 24 hours round"),
            (150, 226, "the date"), (*ck_at(208, 90), "UTC's own hand, red")]
    right = [(*ck_at(48, 120), "the second hand"), (300, 180, "a tap anywhere: the card"),
             (190, 244, "UTC")]
    out = {}
    out["01-face"] = callouts("ck-01", "The Clock firmware's face: the shack clock", f, left, right)
    out["02-diver"] = knob("ck-02", "Turn: the next face, the Diver", clock_face(1), turn(),
                           "a face a detent  ·  the one it rests on is kept")
    out["03-classic"] = knob("ck-03", "The Classic white face", clock_face(2), turn(),
                             "Classic white: a station clock, the red second hand ticking")
    out["04-digital"] = knob("ck-04", "The Digital face", clock_face(3), turn(),
                             "Digital: the time large, the date, UTC")
    out["05-notime"] = knob("ck-05", "Before the time has come from the internet", clock_face(0, have=False),
                            "", "no time yet: no hands, until the WiFi brings it")
    card = address_card(f"Clock {VERSION}\n{POWER}\n{ADDRESSES}")
    out["06-card"] = knob("ck-06", "A tap: the address card, for the configuration page",
                          f + card + lock_pill(False), tap(180, 300),
                          "tap anywhere: the address card  ·  again: away")
    out["07-picker"] = knob("ck-07", "Held three seconds: back to the setup firmware?",
                            dimmed(f) + question("FIRMWARE?", "turn the knob for the picker\n"
                                                              "tap to cancel; WiFi is kept"),
                            tap(180, 300, ring=24), "hold anywhere 3 s until it buzzes, then turn")
    out["08-battery"] = knob("ck-08", "Its own battery low: the charge at the top",
                             f + md.battery_glyph(md.KNOB_BATT_X, md.KNOB_BATT_Y + 18, 15, md.battery_colour(15)),
                             "", "unplugged: the charge only once it is low, a fifth or less")
    out["09-lock"] = knob("ck-09", "The address card's LOCK", f + card + lock_pill(False), tap(252, 84),
                          "tap for the card  ·  tap LOCK")
    out["10-locked"] = knob("ck-10", "Locked: the padlock at the top",
                            f + lock_closed(180, 47.5, md.WARN), "",
                            "the knob turns no face  ·  a tap still brings the card, UNLOCK on it")
    out["11-alarm-mark"] = knob("ck-11", "The next alarm on the dial; a tap on the middle: the alarms",
                                clock_face(mark="BELL 07:30"), tap(180, 180),
                                "the next alarm on the dial  ·  tap the middle: the alarms")
    out["12-alarms"] = knob("ck-12", "The alarms: one at a time, the dial from one to the next",
                            al_list(1, "07", "30", "MON-FRI", True, lit=(1, 4)), turn() + tap(180, 287),
                            "turn: the next alarm  ·  tap ON / OFF  ·  tap the time to set it")
    out["13-alarm-set"] = knob("ck-13", "Setting an alarm: the hours, the minutes, the days",
                               al_list(2, "06", "45", "ONCE", False, mode="HOURS", lit=(1, 4)), turn() + tap(180, 160),
                               "turn: the hours  ·  tap: the minutes, the days, then kept")
    out["14-ringing"] = knob("ck-14", "An alarm ringing", al_ringing("07:30", "MON-FRI"), "",
                             "tap: it stops  ·  turn: it snoozes")
    out["15-snoozed"] = knob("ck-15", "Snoozed: when it rings again, on the dial",
                             clock_face(mark="SNOOZE 07:39"), "",
                             "snoozed: when it rings again  ·  STOP on the list ends it")
    return out


# --- the Weather firmware --------------------------------------------------------

WX_SUN, WX_MOON, WX_CLOUD, WX_CLOUD_DK = "#FFC83C", "#E8E2C0", "#DCE3EC", "#8C96A4"   # ui.c WX_*
WX_RAIN, WX_SNOW, WX_BOLT, WX_FOG, WX_LINE = "#4AA8FF", "#FFFFFF", "#FFD23C", "#A8B0BC", "#FFA040"
WX_PLACE, WX_TIME, WX_ASKED = "Brussels, Belgium", "14:52", "14:55"
# The slab's sky over Brussels (tools/lvhost/slab.c make_sky): the next 24
# hours from 15:00 -- temperature, chance of rain, WMO code -- and 5 days.
WX_HOURS = [(9.6 - 0.4 * (i if i < 15 else 30 - i),
             (0, 5, 0, 10, 20, 35, 60, 80, 75, 40, 10)[i] if i < 11 else 0,
             (3, 0, 0, 2, 3, 61, 63, 65, 80, 51, 3)[i] if i < 11 else 0,
             i < 1 or i > 16) for i in range(24)]
WX_DAYS = [(("TODAY", "THU", "FRI", "SAT", "SUN")[i], (3, 0, 3, 51, 95)[i],
            9.9 + i * 0.3, 4.7 - i, i * 15) for i in range(5)]


def wx_round(v):
    """lroundf: halves away from nought."""
    return int(math.floor(v + 0.5)) if v >= 0 else -int(math.floor(-v + 0.5))


def wx_icon_kind(code):
    """weather.c wx_icon()."""
    for kind, codes in (("clear", (0, 1)), ("partly", (2,)), ("fog", (45, 48)), ("drizzle", (51, 53, 55)),
                        ("sleet", (56, 57, 66, 67)), ("rain", (61, 63, 65)),
                        ("snow", (71, 73, 75, 77, 85, 86)), ("showers", (80, 81, 82)),
                        ("thunder", (95, 96, 99))):
        if code in codes:
            return kind
    return "cloudy"


def wx_disc(x, y, r, c):
    return f'<circle cx="{wx_round(x)}" cy="{wx_round(y)}" r="{r + 0.5:.1f}" fill="{c}"/>'


def wx_line(x1, y1, x2, y2, w, c):
    return ck_line(x1, y1, x2, y2, max(1, wx_round(w)), c, True)


def wx_cloud(x, y, w, c):
    """ui.c wx_cloud(): a flat foot and three domes."""
    x0, x1, y0, y1 = wx_round(x - 0.46 * w), wx_round(x + 0.46 * w), wx_round(y), wx_round(y + 0.24 * w)
    return (wx_disc(x - 0.24 * w, y + 0.04 * w, 0.2 * w, c) + wx_disc(x + 0.02 * w, y - 0.08 * w, 0.27 * w, c)
            + wx_disc(x + 0.27 * w, y + 0.07 * w, 0.17 * w, c)
            + f'<rect x="{x0}" y="{y0}" width="{x1 - x0 + 1}" height="{y1 - y0 + 1}" '
              f'rx="{wx_round(0.12 * w)}" fill="{c}"/>')


def wx_drops(x, y, s, n, length, c):
    return "".join(wx_line(x + (i - (n - 1) / 2) * 0.22 * s, y, x + (i - (n - 1) / 2) * 0.22 * s - 0.35 * length,
                           y + length, 0.07 * s, c) for i in range(n))


def wx_flake(x, y, r):
    out = []
    for i in range(3):
        a = i * 1.0472
        s, c = math.sin(a) * r, math.cos(a) * r
        out.append(wx_line(x - s, y - c, x + s, y + c, r * 0.45, WX_SNOW))
    return "".join(out)


def wx_sun(x, y, r):
    rays = "".join(wx_line(x + 1.4 * r * math.sin(i * 0.785398), y - 1.4 * r * math.cos(i * 0.785398),
                           x + 1.85 * r * math.sin(i * 0.785398), y - 1.85 * r * math.cos(i * 0.785398),
                           r * 0.28, WX_SUN) for i in range(8))
    return rays + wx_disc(x, y, r, WX_SUN)


def wx_moon(x, y, r):
    return wx_disc(x, y, r, WX_MOON) + wx_disc(x + 0.5 * r, y - 0.35 * r, 0.82 * r, "#000000")


def wx_tri(*p, c=WX_BOLT):
    return f'<polygon points="{" ".join(f"{p[i]:.1f},{p[i + 1]:.1f}" for i in (0, 2, 4))}" fill="{c}"/>'


def wx_icon(code, day, x, y, s):
    """ui.c wx_icon_draw(): the icon for a WMO code in a box `s` wide."""
    k, cy = wx_icon_kind(code), y - 0.08 * s
    if k == "clear":
        return wx_sun(x, y, 0.24 * s) if day else wx_moon(x, y, 0.32 * s)
    if k in ("partly", "showers"):
        o = wx_sun(x - 0.16 * s, y - 0.17 * s, 0.15 * s) if day else wx_moon(x - 0.14 * s, y - 0.16 * s, 0.2 * s)
        if k == "partly":
            return o + wx_cloud(x + 0.07 * s, y + 0.06 * s, 0.74 * s, WX_CLOUD)
        return (o + wx_cloud(x + 0.06 * s, cy + 0.02 * s, 0.72 * s, WX_CLOUD)
                + wx_drops(x + 0.08 * s, y + 0.27 * s, s, 3, 0.16 * s, WX_RAIN))
    if k == "fog":
        return wx_cloud(x, y - 0.18 * s, 0.7 * s, WX_CLOUD_DK) + "".join(
            wx_line(x - (0.36 - 0.06 * (i % 2)) * s, y + (0.12 + 0.12 * i) * s,
                    x + (0.36 - 0.06 * ((i + 1) % 2)) * s, y + (0.12 + 0.12 * i) * s, 0.06 * s, WX_FOG)
            for i in range(3))
    if k == "drizzle":
        return wx_cloud(x, cy, 0.8 * s, WX_CLOUD) + wx_drops(x, y + 0.28 * s, s, 3, 0.07 * s, WX_RAIN)
    if k == "rain":
        return wx_cloud(x, cy, 0.8 * s, WX_CLOUD_DK) + wx_drops(x, y + 0.26 * s, s, 3, 0.18 * s, WX_RAIN)
    if k == "sleet":
        return (wx_cloud(x, cy, 0.8 * s, WX_CLOUD_DK) + wx_drops(x - 0.11 * s, y + 0.26 * s, s, 2, 0.16 * s, WX_RAIN)
                + wx_flake(x + 0.24 * s, y + 0.34 * s, 0.07 * s))
    if k == "snow":
        return wx_cloud(x, cy, 0.8 * s, WX_CLOUD) + "".join(
            wx_flake(x + (i - 1) * 0.24 * s, y + (0.38 if i == 1 else 0.3) * s, 0.07 * s) for i in range(3))
    if k == "thunder":
        return (wx_cloud(x, cy, 0.8 * s, WX_CLOUD_DK)
                + wx_tri(x + 0.1 * s, y + 0.08 * s, x - 0.15 * s, y + 0.32 * s, x + 0.05 * s, y + 0.3 * s)
                + wx_tri(x + 0.13 * s, y + 0.24 * s, x - 0.09 * s, y + 0.27 * s, x - 0.06 * s, y + 0.5 * s))
    return (wx_cloud(x + 0.14 * s, y - 0.1 * s, 0.6 * s, WX_CLOUD_DK)
            + wx_cloud(x - 0.04 * s, y + 0.04 * s, 0.78 * s, WX_CLOUD))


def wx_label(y, s, size, colour, dx=0):
    """ui.c wx_label(): one line centred on y (and dx along)."""
    return centred(180 + dx, y, s, size, colour).replace("<text ", '<text xml:space="preserve" ', 1)


def wx_box_text(x1, x2, y, s, size, colour, align):
    """ui.c wx_text_at(): a line in a box x1..x2, its middle at y."""
    f = montserrat(size)
    base = y - f.line_height // 2 + f.line_height - f.base_line
    x, anchor = {"left": (x1, "start"), "right": (x2 + 1, "end"), "centre": ((x1 + x2 + 1) / 2, "middle")}[align]
    return md.text(x, base, esc(s), size, colour, anchor=anchor)


def wx_top(page, foot="Open-Meteo, " + WX_ASKED, foot_colour=None, place=WX_PLACE):
    """What every page has: the time, the caption, the foot."""
    cap = (place, "NEXT 24 HOURS", "5 DAYS")[page]
    return ('<rect x="0" y="0" width="360" height="360" fill="#000000"/>'
            + wx_label(22, WX_TIME, 20, md.TEXT2) + wx_label(74, cap, 14, md.LABEL)
            + wx_label(318, foot, 14, foot_colour or md.LABEL))


def wx_now(foot="Open-Meteo, " + WX_ASKED, foot_colour=None, have=True):
    """The first page (ui.c wx_build, wx_fill): the icon 92 px at 52, 88; the
    temperature at 48 px from 158, 94; then the condition, the wind with its
    arrow, the humidity and pressure, the sun's hours."""
    s = wx_top(0, foot, foot_colour)
    f48, f14 = montserrat(48), montserrat(14)
    if not have:
        return s + md.text(158, 94 + f48.line_height - f48.base_line, "--", 48, md.TEXT, anchor="start")
    s += wx_icon(2, True, 97.5, 133.5, 92)
    s += md.text(158, 94 + f48.line_height - f48.base_line, "10°C", 48, md.TEXT, anchor="start")
    s += md.text(161, 150 + f14.line_height - f14.base_line, "feels like 5°", 14, md.TEXT2, anchor="start")
    s += wx_label(202, "Partly cloudy", 20, md.TEXT)
    wind = "ENE  19 km/h, gusts 39"
    s += wx_label(232, wind, 14, md.TEXT2, dx=12)
    # The arrow, 24 px, left of the words: the wind from 58 degrees, so it points to 238.
    ax, ay = 180 + 12 - f14.width(wind) / 2 - 4 - 12, 232
    r = math.radians(58 + 180)
    sn, cs = math.sin(r), -math.cos(r)
    s += wx_line(ax - 8 * sn, ay - 8 * cs, ax + 3 * sn, ay + 3 * cs, 3, md.TEXT2)
    s += wx_tri(ax + 10 * sn, ay + 10 * cs, ax + 2 * sn + 6 * cs, ay + 2 * cs - 6 * sn,
                ax + 2 * sn - 6 * cs, ay + 2 * cs + 6 * sn, c=md.TEXT2)
    s += wx_label(256, "humidity 63 %    1027 hPa", 14, md.TEXT2)
    return s + wx_label(280, "sunrise 07:39    sunset 17:12", 14, md.TEXT2)


def wx_hours():
    """The second page (ui.c wx_chart_cb): the chance of rain as bars, the
    temperature a line over them with its highest and lowest, an icon every
    four hours, the hour every six."""
    s = wx_top(1)
    a_x1, a_y1, a_x2 = 24, 90, 24 + 312 - 1
    x0, x1 = a_x1 + 14, a_x2 - 14
    dx = (x1 - x0) / 23
    top, bot = a_y1 + 40, a_y1 + 148
    temps = [h[0] for h in WX_HOURS]
    lo, hi = min(temps), max(temps)
    ilo, ihi = temps.index(lo), temps.index(hi)
    mid, span = (lo + hi) / 2, max(4, hi - lo)
    ty0, ty1 = top + 14, bot - 22
    ty = lambda c: ty1 - (c - (mid - span / 2)) / span * (ty1 - ty0)   # noqa: E731
    for i, (_, rain, _, _) in enumerate(WX_HOURS):
        if rain:
            x, h = x0 + dx * i, (bot - top) * rain / 100
            y0 = wx_round(bot - h)
            s += (f'<rect x="{wx_round(x - 4)}" y="{y0}" width="9" height="{bot - y0 + 1}" rx="2" '
                  f'fill="{WX_RAIN}" fill-opacity="0.6"/>')
    s += wx_line(x0, bot + 1, x1, bot + 1, 1, "#30363F")
    pts = " ".join(f"{x0 + dx * i:.1f},{ty(t):.1f}" for i, t in enumerate(temps))
    s += (f'<polyline points="{pts}" fill="none" stroke="{WX_LINE}" stroke-width="3" '
          f'stroke-linecap="round" stroke-linejoin="round"/>')
    xh = x0 + dx * ihi
    s += wx_box_text(int(xh) - 30, int(xh) + 30, int(ty(hi)) - 12, f"{wx_round(hi)}°", 14, WX_LINE, "centre")
    xl = x0 + dx * ilo
    s += wx_box_text(int(xl) - 30, int(xl) + 30, int(ty(lo)) + 13, f"{wx_round(lo)}°", 14, WX_LINE, "centre")
    for i in range(0, 24, 4):
        s += wx_icon(WX_HOURS[i][2], WX_HOURS[i][3], x0 + dx * i, a_y1 + 16, 28)
    for i in range(0, 24, 6):
        x = wx_round(x0 + dx * i)
        hour = f"{(15 + i) % 24:02d}:00"
        s += (wx_box_text(x - 30, x + 30, bot + 16, hour, 14, md.TEXT2, "centre") if i
              else wx_box_text(a_x1, a_x1 + 60, bot + 16, hour, 14, md.TEXT2, "left"))
    return s + wx_label(290, "rain likely at 21:00, 60 %", 14, md.TEXT2)


def wx_days():
    """The third page (ui.c wx_days_cb): a row a day, today first."""
    s = wx_top(2)
    ax1, ay1, ax2 = 40, 88, 40 + 280 - 1
    for i, (name, code, mx, mn, rain) in enumerate(WX_DAYS):
        y = ay1 + 20 + 40 * i
        s += wx_box_text(ax1 + 8, ax1 + 92, y, name, 20, md.ACCENT_HI if not i else md.TEXT, "left")
        s += wx_icon(code, True, ax1 + 114, y, 34)
        s += wx_box_text(ax1 + 134, ax1 + 184, y, f"{wx_round(mx)}°", 20, md.TEXT, "right")
        s += wx_box_text(ax1 + 188, ax1 + 234, y, f"{wx_round(mn)}°", 20, md.LABEL, "right")
        if rain >= 10:
            s += wx_box_text(ax1 + 236, ax2, y, f"{rain}%", 14, WX_RAIN, "right")
    return s


def weather_pictures():
    md.use_palette("aethersdr")          # the weather wears AetherSDR's palette on black
    f = wx_now()
    left = [(98, 134, "now: the sky"), (110, 202, "in words"), (88, 232, "the wind, and its way"),
            (120, 256, "humidity, pressure"), (110, 280, "the sun's hours")]
    right = [(200, 22, "the time"), (230, 74, "the place"), (230, 120, "the temperature"),
             (236, 156, "feels like"), (220, 318, "when Open-Meteo answered")]
    out = {}
    out["01-face"] = callouts("wx-01", "The Weather firmware's first page: now", f, left, right)
    out["02-hours"] = knob("wx-02", "Turn: the next 24 hours", wx_hours(), turn(),
                           "a page a detent  ·  rain as bars, the temperature a line")
    out["03-days"] = knob("wx-03", "Turn again: the next 5 days", wx_days(), turn(),
                          "5 days: highest, lowest, the chance of rain")
    out["04-asking"] = knob("wx-04", "Before the first forecast", wx_now("asking Open-Meteo...", have=False), "",
                            "asking: the place and the time, the forecast on its way")
    out["05-failing"] = knob("wx-05", "Open-Meteo did not answer", wx_now("Open-Meteo: no answer", md.WARN), "",
                             "what went wrong, at the foot  ·  the last forecast kept")
    card = address_card(f"Weather {VERSION}\n{POWER}\n{ADDRESSES}")
    out["06-card"] = knob("wx-06", "A tap: the address card, for the configuration page",
                          f + card + lock_pill(False), tap(180, 300),
                          "tap anywhere: the address card  ·  again: away")
    out["07-picker"] = knob("wx-07", "Held three seconds: back to the setup firmware?",
                            dimmed(f) + question("FIRMWARE?", "turn the knob for the picker\n"
                                                              "tap to cancel; WiFi is kept"),
                            tap(180, 300, ring=24), "hold anywhere 3 s until it buzzes, then turn")
    out["08-battery"] = knob("wx-08", "On its own battery: the knob's charge under the time",
                             f + md.knob_battery(85), "",
                             "unplugged: its charge, by the quarter  ·  none on USB power")
    out["09-lock"] = knob("wx-09", "The address card's LOCK", f + card + lock_pill(False), tap(252, 84),
                          "tap for the card  ·  tap LOCK")
    out["10-locked"] = knob("wx-10", "Locked: the padlock under the time",
                            f + lock_closed(180, 47.5, md.WARN), "",
                            "the knob turns no page  ·  a tap still brings the card, UNLOCK on it")
    return out


# --- the screensavers (every firmware) -----------------------------------------------

MX_CW, MX_CH = 15, 20                     # saver.c MX_CW, MX_CH


def matrix_rain():
    """saver.c's Matrix rain, still: 24 columns of 15 px, each a head of pale
    green and a trail fading to dark green, the half-width katakana, digits
    and signs mirrored, as the film shows them; only what the round glass
    shows of each column. Its own xorshift, so the same rain every run."""
    rng = [0x2545F491]

    def rnd(n):
        v = rng[0]
        v ^= (v << 13) & 0xFFFFFFFF
        v ^= v >> 17
        v ^= (v << 5) & 0xFFFFFFFF
        rng[0] = v
        return v % n if n else 0

    glyphs = [chr(c) for c in range(0xFF66, 0xFF9E)] + list("0123456789") + list(":.=*+-<>|")
    s = ['<rect x="0" y="0" width="360" height="360" fill="#000000"/>']
    rows = 360 // MX_CH
    for col in range(360 // MX_CW):
        dx = col * MX_CW + MX_CW / 2 - 180
        half = math.sqrt(max(0, 180 * 180 - dx * dx))
        shown = [r for r in range(rows) if abs(r * MX_CH + MX_CH / 2 - 180) <= half + MX_CH / 2]
        if not shown:
            continue
        length = 6 + rnd(11)
        head = shown[0] + rnd(len(shown) + length // 2)
        for r in shown:
            d = head - r
            if d < 0 or d > length:
                continue
            if d == 0:
                c = "#C8FFC8"
            else:
                g = 255 - (d - 1) * 215 // length
                c = f"#00{g:02X}{g // 4:02X}"
            x, y = col * MX_CW + MX_CW / 2, r * MX_CH + 16
            s.append(f'<text x="{-x:.1f}" y="{y}" transform="scale(-1,1)" font-size="17" fill="{c}" '
                     f'font-family="Noto Sans CJK JP,Noto Sans Mono CJK JP,MS Gothic,Hiragino Sans,monospace" '
                     f'text-anchor="middle">{esc(glyphs[rnd(len(glyphs))])}</text>')
    return "".join(s)


def drift_box(x, y, big, small, utc="", digits=True):
    """saver.c digital_frame and freq_frame: a box of white, large, and its
    lines under it, wherever the drift has it."""
    if utc:
        f, f20 = hack96(), montserrat(20)
        bw, bh = f.width("00:00"), f.line_height
        s = hack_text(x + 2 + bw / 2, y + 2 + bh / 2, big, "#FFFFFF")
        s += centred(x + 2 + bw / 2, y + 2 + bh + 6 + f20.line_height / 2, small, 20, "#C0C8D4")
        return s + centred(x + 2 + bw / 2, y + 2 + bh + 6 + f20.line_height + 2 + f20.line_height / 2, utc,
                           20, "#5A9BFF")
    f = md.hack() if digits else montserrat(48)
    f20 = montserrat(20)
    w = max(f.width(big), f20.width(small))
    cx = x + 2 + w / 2
    s = hack_text(cx, y + 2 + f.line_height / 2, big, "#FFFFFF", f) if digits else \
        centred(cx, y + 2 + f.line_height / 2, big, 48, "#FFFFFF")
    return s + centred(cx, y + 2 + f.line_height + 4 + f20.line_height / 2, small, 20, "#C0C8D4")


def saver_pictures():
    md.use_palette("aethersdr")
    black = '<rect x="0" y="0" width="360" height="360" fill="#000000"/>'
    big, line, utc = ck_digital_lines()
    out = {}
    out["01-matrix"] = knob("ss-01", "Matrix rain, every firmware's screensaver until set", matrix_rain(), "",
                            "a minute untouched: the rain  ·  a turn or a tap: the dial again")
    out["02-clock"] = knob("ss-02", "The Analog clock screensaver", clock_face(0), "",
                           "Analog clock: the page's clock face, the shack clock until set")
    out["03-digital"] = knob("ss-03", "The Digital clock screensaver, drifting",
                             black + drift_box(46, 86, big, line, utc), "",
                             "Digital clock: the time, the date, UTC, drifting a pixel a second")
    out["04-frequency"] = knob("ss-04", "The Drifting frequency screensaver",
                               black + drift_box(70, 200, "14.074.000", "USB"), "",
                               "Drifting frequency: the dial's frequency and mode, drifting")
    out["05-dark"] = knob("ss-05", "Dark", black, "",
                          "a minute later dimmed, a minute after that dark: all three set on the page")
    return out


FIRMWARES = {"setup": setup_pictures, "icom": icom_pictures, "multiflex": multiflex_pictures,
             "aethersdr": aethersdr_pictures, "openx6x00": openx6x00_pictures,
             "svxconnect": svxconnect_pictures,
             "ubersdr": ubersdr_pictures, "kiwi": kiwi_pictures, "phone": phone_pictures,
             "epibeam": epibeam_pictures, "adsb": adsb_pictures, "clock": clock_pictures,
             "weather": weather_pictures, "screensaver": saver_pictures, "headset": headset_pictures}


def main():
    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    want = sys.argv[1:] or list(FIRMWARES)
    for fw in want:
        out = os.path.join(here, "docs", fw)
        os.makedirs(out, exist_ok=True)
        for name, svg in FIRMWARES[fw]().items():
            with open(os.path.join(out, name + ".svg"), "w") as f:
                f.write(svg)
            print(f"docs/{fw}/{name}.svg: {len(svg)} bytes")


if __name__ == "__main__":
    main()
