#!/usr/bin/env python3
"""Draw the knob's face as animated SVG, for the README.

The geometry and the palette are copied from components/ui/ui.c rather than
eyeballed, so these stay honest when the firmware changes: 360x360 round glass,
meter arc from 170 to 370 degrees at radius 170 with a 12 px band, and the same
colour blocks and background-coloured notches the device actually draws.
"""
import math, os

CX = CY = 180
ARC_R0, ARC_ROT, ARC_SPAN = 170, 170, 200
BAND = 12
RC = ARC_R0 - BAND / 2                      # centreline of the band
SWR_ROT, SWR_SPAN = ARC_ROT, ARC_SPAN // 2 - 3
AUD_ROT, AUD_SPAN = ARC_ROT + ARC_SPAN // 2 + 3, ARC_SPAN // 2 - 3
PTT_TOP = 248

BG, BG1, BG_TX = "#0F0F1A", "#1A2A3A", "#3A2A0E"
ACCENT, ACCENT_HI = "#00B4D8", "#00C8F0"
TEXT, TEXT2, LABEL, SUBTLE = "#C8D8E8", "#8EA8C0", "#506070", "#1A2330"
WARN, DANGER, TX_RED, TX_TEXT = "#FFB84D", "#FF4D4D", "#E01010", "#F0C890"

RXZONES = [(-127, -121, "#1A6B47"), (-121, -109, "#1F7A52"),
           (-109,  -97, "#2F9E6A"), (-97,   -85, "#4DD87A"),
           (-85,   -73, "#9BD94A"), (-73,   -53, "#FFD24D"),
           (-53,   -33, "#FF9A3C"), (-33,   -13, "#FF4D4D")]
RXNOTCH = [-121, -109, -97, -85, -73, -53, -33]
RXTICKS = [(-121, 6), (-109, 6), (-97, 6), (-85, 6),
           (-73, 11), (-53, 6), (-33, 6), (-13, 9)]
SWRZONES = [(1.0, 2.0, "#4DD87A"), (2.0, 2.5, "#FFB84D"), (2.5, 3.0, "#FF4D4D")]


def smeter_frac(dbm):
    dbm = max(-127.0, min(-13.0, dbm))
    return (0.6 * (dbm + 127) / 54) if dbm <= -73 else (0.6 + 0.4 * (dbm + 73) / 60)


def swr_frac(s):
    return max(0.0, min(1.0, (s - 1.0) / 2.0))


def pt(deg, r):
    a = math.radians(deg)
    return CX + r * math.cos(a), CY + r * math.sin(a)


def arc_path(a0, a1, r):
    x0, y0 = pt(a0, r)
    x1, y1 = pt(a1, r)
    large = 1 if abs(a1 - a0) > 180 else 0
    sweep = 1 if a1 > a0 else 0
    return f"M{x0:.2f},{y0:.2f} A{r:.2f},{r:.2f} 0 {large} {sweep} {x1:.2f},{y1:.2f}"


def arc_len(a0, a1, r):
    return abs(math.radians(a1 - a0)) * r


def block(a0, a1, colour):
    return (f'<path d="{arc_path(a0, a1, RC)}" fill="none" stroke="{colour}" '
            f'stroke-width="{BAND}"/>')


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
            f'stroke="{colour}" stroke-width="{width}"/>')


def sweep_cover(a0, a1, values, dur, colour=SUBTLE):
    """A track-coloured arc drawn backwards over the blocks, its dash length
    animated so the bar appears to fill and fall."""
    L = arc_len(a0, a1, RC)
    lens = " ; ".join(f"{L * (1 - v):.2f} {L + 10:.2f}" for v in values)
    # Open on the first value, so a renderer that ignores SMIL still shows a
    # meter with something in it rather than an empty track.
    first = L * (1 - values[0])
    return (f'<path d="{arc_path(a1, a0, RC)}" fill="none" stroke="{colour}" '
            f'stroke-width="{BAND + 0.5}" stroke-dasharray="{first:.2f} {L + 10:.2f}">'
            f'<animate attributeName="stroke-dasharray" dur="{dur}" '
            f'repeatCount="indefinite" values="{lens}"/></path>')


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
            f'font-family="DejaVu Sans,Verdana,sans-serif" font-weight="{weight}" '
            f'text-anchor="middle" opacity="{1 if i == 0 else 0}">{d}'
            f'<animate attributeName="opacity" dur="{dur}" repeatCount="indefinite" '
            f'calcMode="discrete" values="{";".join(vals)}" '
            f'keyTimes="{";".join(keys)}"/></text>')
    return "".join(out)


def head(title):
    return (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 360 360" '
            f'width="360" height="360" role="img" aria-label="{title}">'
            f'<title>{title}</title>'
            f'<defs><clipPath id="glass"><circle cx="180" cy="180" r="180"/>'
            f'</clipPath></defs><g clip-path="url(#glass)">')


def tail():
    return '</g></svg>'


def rx_face():
    s = [head("VFO-Knob receiving")]
    s.append(f'<circle cx="180" cy="180" r="180" fill="{BG}"/>')
    s.append(f'<path d="{arc_path(ARC_ROT, ARC_ROT + ARC_SPAN, RC)}" fill="none" '
             f'stroke="{SUBTLE}" stroke-width="{BAND}"/>')
    for lo, hi, col in RXZONES:
        s.append(block(ARC_ROT + smeter_frac(lo) * ARC_SPAN,
                       ARC_ROT + smeter_frac(hi) * ARC_SPAN, col))
    # A signal that rises, peaks over S9 and decays, the way a real one does.
    lvl = [-86, -78, -70, -61, -55, -63, -74, -88, -101, -112, -107, -99, -92, -86]
    s.append(sweep_cover(ARC_ROT, ARC_ROT + ARC_SPAN,
                         [smeter_frac(d) for d in lvl], "6s"))
    for d in RXNOTCH:
        s.append(notch(ARC_ROT + smeter_frac(d) * ARC_SPAN))
    for d, ln in RXTICKS:
        col = TEXT2 if d == -73 else (WARN if d > -73 else LABEL)
        s.append(tick(ARC_ROT + smeter_frac(d) * ARC_SPAN, ln, col,
                      3 if d == -73 else 2))
    s.append(f'<text x="180" y="86" font-size="19" fill="{TEXT}" text-anchor="middle" '
             f'font-family="DejaVu Sans,Verdana,sans-serif" font-weight="700">S7</text>')
    s.append(f'<text x="180" y="128" font-size="14" fill="{LABEL}" text-anchor="middle" '
             f'font-family="DejaVu Sans,Verdana,sans-serif">40m &#183; LSB &#183; 2.8k</text>')
    # Fixed pitch, centred, with the 100 Hz digit ticking as if the knob were
    # being turned and the step underline sitting beneath it.
    chars = list("7.161.230")
    pitch, dotw, fs = 26, 12, 40
    total = sum(dotw if c == "." else pitch for c in chars)
    x = CX - total / 2
    tick_x = None
    for i, c in enumerate(chars):
        w = dotw if c == "." else pitch
        cx = x + w / 2
        if i == 6:                      # the 100 Hz digit
            s.append(cycling_digit(cx, 196, fs, ACCENT_HI,
                                   list("2345678901"), "6s"))
            tick_x = cx
        else:
            s.append(f'<text x="{cx:.1f}" y="196" font-size="{fs}" '
                     f'fill="{LABEL if c == "." else TEXT}" '
                     f'font-family="DejaVu Sans,Verdana,sans-serif" '
                     f'font-weight="{400 if c == "." else 700}" '
                     f'text-anchor="middle">{c}</text>')
        x += w
    s.append(f'<rect x="{tick_x - 13:.1f}" y="206" width="26" height="3" '
             f'fill="{ACCENT}"/>')
    s.append(f'<text x="180" y="232" font-size="13" fill="{LABEL}" text-anchor="middle" '
             f'font-family="DejaVu Sans,Verdana,sans-serif">'
             f'100 Hz &#183; RIT 0 &#183; VOL 40</text>')
    s.append(f'<rect x="0" y="{PTT_TOP}" width="360" height="{360 - PTT_TOP}" fill="{BG1}"/>')
    s.append(f'<text x="180" y="{PTT_TOP + 40}" font-size="30" fill="{TEXT2}" '
             f'text-anchor="middle" font-family="DejaVu Sans,Verdana,sans-serif" '
             f'font-weight="700" letter-spacing="4">PTT</text>')
    s.append(tail())
    return "".join(s)


def tx_face():
    s = [head("VFO-Knob transmitting")]
    s.append(f'<circle cx="180" cy="180" r="180" fill="{BG_TX}"/>')
    s.append(f'<circle cx="180" cy="180" r="178" fill="none" stroke="{TX_RED}" '
             f'stroke-width="4"/>')
    # SWR across the left half.
    s.append(f'<path d="{arc_path(SWR_ROT, SWR_ROT + SWR_SPAN, RC)}" fill="none" '
             f'stroke="{SUBTLE}" stroke-width="{BAND}"/>')
    for lo, hi, col in SWRZONES:
        s.append(block(SWR_ROT + swr_frac(lo) * SWR_SPAN,
                       SWR_ROT + swr_frac(hi) * SWR_SPAN, col))
    s.append(sweep_cover(SWR_ROT, SWR_ROT + SWR_SPAN,
                         [swr_frac(v) for v in
                          (1.3, 1.2, 1.4, 1.3, 1.2, 1.5, 1.3, 1.3)], "5s"))
    for v in (1.5, 2.0, 2.5):
        s.append(notch(SWR_ROT + swr_frac(v) * SWR_SPAN))
    # Forward power across the right half, auto-ranged to 100 W.
    s.append(f'<path d="{arc_path(AUD_ROT, AUD_ROT + AUD_SPAN, RC)}" fill="none" '
             f'stroke="{SUBTLE}" stroke-width="{BAND}"/>')
    s.append(block(AUD_ROT, AUD_ROT + AUD_SPAN, WARN))
    s.append(sweep_cover(AUD_ROT, AUD_ROT + AUD_SPAN,
                         [.87, .62, .88, .70, .95, .81, .55, .70, .87], "5s"))
    for f in (0.10, 0.50):
        s.append(notch(AUD_ROT + f * AUD_SPAN))
    s.append(f'<text x="96" y="92" font-size="17" fill="{TX_TEXT}" text-anchor="middle" '
             f'font-family="DejaVu Sans,Verdana,sans-serif" font-weight="700">SWR 1.3</text>')
    s.append(f'<text x="264" y="92" font-size="17" fill="{TX_TEXT}" text-anchor="middle" '
             f'font-family="DejaVu Sans,Verdana,sans-serif" font-weight="700">PWR 87W</text>')
    s.append(f'<text x="180" y="150" font-size="14" fill="{TX_TEXT}" text-anchor="middle" '
             f'font-family="DejaVu Sans,Verdana,sans-serif">40m &#183; LSB &#183; 2.8k</text>')
    s.append(f'<text x="180" y="200" font-size="44" fill="{TX_TEXT}" text-anchor="middle" '
             f'font-family="DejaVu Sans,Verdana,sans-serif" font-weight="700">'
             f'7.161.230</text>')
    s.append(f'<rect x="0" y="{PTT_TOP}" width="360" height="{360 - PTT_TOP}" '
             f'fill="{TX_RED}"><animate attributeName="opacity" dur="2s" '
             f'repeatCount="indefinite" values="1;0.78;1"/></rect>')
    s.append(f'<text x="180" y="{PTT_TOP + 40}" font-size="30" fill="#FFFFFF" '
             f'text-anchor="middle" font-family="DejaVu Sans,Verdana,sans-serif" '
             f'font-weight="700" letter-spacing="4">TX  102s</text>')
    s.append(tail())
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
