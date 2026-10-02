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
import math
import os
import sys

sys.dont_write_bytecode = True             # no __pycache__ left in tools/
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mkdisplay as md  # noqa: E402  (the body, the palettes, text())
import mkrender as mr   # noqa: E402  (the reflector's face and its icons)

PAD = 44                                  # room around the body for gestures
W = H = 2 * md.BODY_R + 2 * PAD
OX = OY = W / 2
GESTURE = "#E0902A"                       # mkdisplay's ANNOT_HOT: a hand's doing
VERSION = "1.14.0"                        # what the address card says it runs
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


def chooser(title, value, hint="turn to choose  -  tap to accept"):
    """The editor panel as a chooser (ED_CHOICE): 250 x 132 at 6 px above the
    middle, the title at 20 px in the label colour, the value at 28 px."""
    return (f'<rect x="55" y="108" width="250" height="132" rx="18" fill="{md.BG1}" '
            f'stroke="{md.ACCENT}" stroke-width="2"/>'
            + md.text(180, 128, esc(title), 20, md.LABEL)
            + md.text(180, 188, esc(value), 28, md.ACCENT_HI, 600)
            + md.text(180, 232, esc(hint), 14, md.LABEL))


def question(title, hint):
    """ui_ask_turn(): the question panel, 268 x 116, the title at 28 px 16 px
    from its top, the hint at 14 px 14 px from its foot."""
    return (f'<rect x="46" y="116" width="268" height="116" rx="18" fill="{md.BG1}" '
            f'stroke="{md.ACCENT}" stroke-width="2"/>'
            + md.text(180, 156, esc(title), 28, md.ACCENT_HI, 600)
            + lines(180, 200, hint, 14, md.TEXT2, 17))


def address_card(text):
    """The address card: 20 px in the bright accent, 12 px of padding, a
    2 px accent border, centred 6 px above the middle."""
    rows = text.split("\n")
    w = max(len(r) for r in rows) * 11.2 + 24
    h = len(rows) * 24 + 24
    x, y = 180 - w / 2, 174 - h / 2
    return (f'<rect x="{x:.1f}" y="{y:.1f}" width="{w:.1f}" height="{h:.1f}" rx="14" '
            f'fill="{md.BG1}" stroke="{md.ACCENT}" stroke-width="2"/>'
            + lines(180, 174, text, 20, md.ACCENT_HI, 24, 400)
            .replace('font-family="DejaVu Sans', 'xml:space="preserve" font-family="DejaVu Sans'))


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
         receiver=False, ghz=False):
    """A radio firmware's receive face at a reading, in its palette (ui.c;
    mkrender's dial, with the options the guides need): `sub` for what is
    under the S-units, `sdr` a web SDR's level -- its thin line outside the
    radio's, and its reading in blue -- and `mem` = (group, name, line) for
    memory mode. `receiver`: a receiver radio's (the IC-R8600), RECEIVER on
    the slab and no RIT or microphone; `ghz` the digits from 1 GHz up."""
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


def editor(title, value, size=48, hint="turn to choose  -  tap to accept", colour=None):
    """The editor panel (ui.c edit_render): 250 x 132, 6 px above the middle;
    the title at 20 px, the value at 48 px -- 28 for names -- and the hint."""
    return (f'<rect x="55" y="108" width="250" height="132" rx="18" fill="{md.BG1}" '
            f'stroke="{md.ACCENT}" stroke-width="2"/>'
            + md.text(180, 128, esc(title), 20, md.LABEL)
            + md.text(180, 188 if size == 28 else 196, esc(value), size, colour or md.ACCENT_HI, 600)
            + md.text(180, 232, esc(hint), 14, md.LABEL))


def warning(title, fw, net=ADDRESSES):
    """The warning panel: 268 x 134 in the danger colour, the warning at
    28 px, and under it at 14 px the address card's text -- the firmware and
    its version (`fw`, e.g. "Icom"), then the addresses -- or, fw None, a
    message in its place."""
    text = f"{fw} {VERSION}\n{net}" if fw else net
    return (f'<rect x="46" y="107" width="268" height="134" rx="18" fill="{md.BG1}" '
            f'stroke="{md.DANGER}" stroke-width="2"/>'
            + md.text(180, 145, esc(title), 28, md.DANGER, 600)
            + lines(180, 199, text, 14, md.TEXT2, 17))


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
    md.use_palette("aethersdr")          # the setup firmware wears the default face
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
                     chooser("INSTALL", "Icom 1.13.0")),
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
    face = radio_face()
    out["11-address-card"] = knob(
        "setup-11", "A radio firmware: the address card, held up on the S-meter",
        dimmed(face) + address_card(f"Icom {VERSION}\n{ADDRESSES}"),
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
    md.use_palette("aethersdr")
    return out


# --- the radios' firmwares ------------------------------------------------------

def radio_parts(gain="P.AMP"):
    """The face's parts, named: the left column and the right one."""
    # Each point at the edge of its part, the dot never on the words.
    left = [(64, 64, "S-meter · hold: the addresses"), (84, 77, "AGC"), (84, 123, "band"),
            (58, 172, "frequency · tap a digit: its step"), (54, 221, "tuning step"),
            (112, 300, "PTT · tap on, tap off")]
    right = [(204, 77, "S-units, dBm under"), (286, 77, gain), (204, 123, "mode"),
             (282, 123, "filter"), (176, 222, "RIT"), (244, 222, "volume"), (306, 222, "mic gain")]
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


def swipe_at(direction):
    """A swipe drawn clear of a panel: across the S-meter, or down the edge."""
    a, b = {"down": ((326, 96), (326, 250)), "up": ((326, 250), (326, 96)),
            "right": ((70, 62), (290, 62)), "left": ((290, 62), (70, 62))}[direction]
    (x0, y0), (x1, y1) = on_face(*a), on_face(*b)
    return (f'<line x1="{x0:.1f}" y1="{y0:.1f}" x2="{x1:.1f}" y2="{y1:.1f}" stroke="{GESTURE}" '
            f'stroke-width="6" stroke-linecap="round" stroke-opacity="0.9" marker-end="url(#arrow)"/>'
            f'<circle cx="{x0:.1f}" cy="{y0:.1f}" r="9" fill="{GESTURE}" fill-opacity="0.35" '
            f'stroke="{GESTURE}" stroke-width="2"/>')


def icom_pictures():
    R = dict(dbm=-85, band="40m", mode="LSB", filt="FIL2", digits="  7123" "00", active=5,
             step="1 kHz", agc="MID", gain_cap="P.AMP", gain="OFF")
    f = face("icom", **R)
    left, right = radio_parts("P.AMP · the preamp")
    out = {}
    out["01-face"] = callouts("icom-01", "The Icom firmware's face", f, left, right)
    out["02-no-link"] = knob("icom-02", "NO LINK: the radio not reached yet",
                             face("icom", dbm=-127, **{k: v for k, v in R.items() if k != "dbm"})
                             + warning("NO LINK", "Icom"))
    out["03-tune"] = knob("icom-03", "Turn to tune; tap a digit for its step", f,
                          turn() + tap(236, 172), "tap a digit for the step  \u00b7  turn to tune")
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
    return out


def multiflex_pictures():
    R = dict(dbm=-53, band="20m", mode="USB", filt="2700", digits=" 14200" "00", active=6,
             step="100 Hz", agc="MED", gain_cap="RF.G", gain="+8 dB")
    f = face("multiflex", **R)
    left, right = radio_parts("RF.G · the RF gain")
    out = {}
    out["01-face"] = callouts("flex-01", "The FlexRadio firmware's face", f, left, right)
    out["02-station"] = knob("flex-02", "At boot, with others on the radio: what to be",
                             f + editor("DIAL FOR", "thinkstation", 28), turn(),
                             "turn: STATION OWN, or DIAL FOR a station  \u00b7  tap the panel")
    out["03-menu"] = knob("flex-03", "Swipe from the right: TUNE, ATU, MEM",
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
                            telephone(keypad="01632960123", flash="3"), swipe_at("down"),
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
                          telephone(keypad="1#", flash="#", **call), swipe_at("down"),
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
    return out


# --- a Bluetooth headset (docs/headset.md) -------------------------------------

def headset_face(face_svg, **slab):
    """A transmitting firmware's face with a headset connected: the PTT slab
    keeps its caption, the headset's logo at its right end (mkdisplay
    headset_slab), in the face's palette -- the one its drawing last set."""
    return face_svg.replace(md.ptt_slab(md.BG1, "PTT", md.TEXT2), md.headset_slab(**slab))


def headset_pictures():
    R = dict(dbm=-85, band="40m", mode="LSB", filt="FIL2", digits="  7123" "00", active=5,
             step="1 kHz", agc="MID", gain_cap="P.AMP", gain="OFF")
    out = {}
    out["01-connected"] = knob("hs-01", "A headset connected: its logo at the slab's end",
                               headset_face(face("icom", **R)), "",
                               "its button is the PTT  \u00b7  a tap on the slab only unkeys")
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
    return out


# --- the ubersdr firmware -------------------------------------------------------

KIWI = "#8B7CF8"                           # ui.c SDR_HEX for ubersdr: UberSDR's violet


def snr_colour(snr):
    """ui.c: the SNR in UberSDR's colours for it, hsv(0..120, 85%, 96%) from
    0 to 15 dB."""
    import colorsys
    f = max(0.0, min(1.0, snr / 15.0))
    r, g, b = colorsys.hsv_to_rgb(f * 120 / 360, 0.85, 0.96)
    return "#%02X%02X%02X" % (round(r * 255), round(g * 255), round(b * 255))


def uber_face(dbm=-91, snr=9, band="20m", mode="USB", filt="2650", digits=" 14215" "00", active=5,
              step="1 kHz", nr="NR4", spot=("LU7YZ", "14.215.0 USB  DX 2m  heard 12 dB", "green", "8 on 20m"),
              kiwi=None, vol="40", headset=False):
    """The ubersdr firmware's face (ui.c, RX_FACE): the S-meter in UberSDR's
    colours, the SNR where the AGC is, the noise filter where the gain is, no
    RIT and no microphone, and on the slab the spot or voice nearest the dial:
    `spot` is (its call or frequency, where and what it is, "green" heard now
    / "bright" on it / "dim" elsewhere, how many on the band). `kiwi` is a
    KiwiSDR's level beside it; `headset`, a Bluetooth headset's logo at the
    slab's right end (ui.c s_hs_bt)."""
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
        s.append(md.block(md.ARC_ROT, md.ARC_ROT + md.smeter_frac(kiwi) * md.ARC_SPAN, KIWI, r=174, band=4))
        sub, sub_colour = md.smeter_text(kiwi), KIWI
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
            (84, 276, "nearest spot or voice · tap: all"), (112, 300, "where, and what it is")]
    right = [(204, 77, "S-units, dBFS under"), (282, 77, "FIL · the noise filter"), (204, 123, "mode"),
             (282, 123, "filter"), (252, 222, "volume"), (228, 322, "how many on the band")]
    out = {}
    out["01-face"] = callouts("uber-01", "The UberSDR firmware's face", f, left, right)
    md.use_palette("ubersdr")
    out["02-no-link"] = knob("uber-02", "The receiver not reached, and why",
                             uber_face(dbm=-127, snr=None, spot=None) + warning("RECEIVER FULL", "UberSDR"))
    out["03-tune"] = knob("uber-03", "Turn to tune; tap a digit for its step", f,
                          turn() + tap(236, 172), "tap a digit for the step  \u00b7  turn to tune")
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
    out["15-headset"] = knob("uber-15", "A Bluetooth headset connected: its logo beside the spot",
                             uber_face(headset=True), "",
                             "the receiver in the headset  \u00b7  the spots stay")
    return out


FIRMWARES = {"setup": setup_pictures, "icom": icom_pictures, "multiflex": multiflex_pictures,
             "aethersdr": aethersdr_pictures, "svxconnect": svxconnect_pictures,
             "ubersdr": ubersdr_pictures, "phone": phone_pictures,
             "headset": headset_pictures}


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
