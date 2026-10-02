#!/usr/bin/env python3
"""Product renders of the knob, for marketing: the real dial on the real body.

    tools/mkrender.py [--out DIR]   -> DIR/<radio>/*.svg and *.png
                                       (DIR defaults to docs/marketing)
    tools/mkrender.py --glass --radio svxconnect
                                    -> docs/display-svxconnect.svg, the face
                                       alone, for the README

The dial is the device's own face, drawn by tools/mkdisplay.py (the firmware's
geometry and palette), frozen at one reading -- one for each radio's firmware,
in that firmware's colours, so the two sets tell apart at a glance:

  aethersdr   AetherSDR's dark theme: S9+40 on 80 m, 3.630.00 LSB
  icom        the IC-705's screen: S9+20 on 2 m, 145.500.00 FM, P.AMP on
  svxconnect  svxconnect.app's ink and gold: TG 8 on be.svx.link, someone
              talking, the arc at -14 dBFS
  multiflex   the Maestro's colours: S9+20 on 20 m, 14.200.00 USB, RF.G at
              +8 dB -- the FlexRadio's RF gain, which its own API carries
  phone       the Telephone, in SVXConnect's colours: a call up 2:47, the
              caller's name in the middle, both voices on the split arc --
              theirs left, ours right

The body is a 66 mm cylinder, 22 mm deep: a blue anodised ring with diagonal
knurling over a black base, the cover glass, and the 1.8" panel inside it --
the same proportions tools/mkdisplay.py uses. Product shots are close to an
orthographic view, and under that projection a flat circle seen at an angle is
an exact affine image of itself. So the whole face goes onto the glass with a
single SVG matrix, and the sides are faceted strips shaded for a light from
the upper left. No 3D package needed.

Three views, each on a transparent background:

  knob-angled-text-left   standing on its rim and facing left, the body
                          running off to the right: text goes on the left
  knob-angled-text-right  the same facing right. Not a mirrored bitmap: the
                          camera moves, so the dial still reads correctly
  knob-upright       sitting on its base, seen from the front and above
"""
import math, os, subprocess, sys

sys.dont_write_bytecode = True             # no __pycache__ left in tools/
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mkdisplay as D                       # noqa: E402

# --- the readings -------------------------------------------------------------
# Each as that firmware draws it. AetherSDR's TCI carries no RF gain, so its
# RF.G is greyed out, as on the device.
RADIOS = {
    "aethersdr": dict(name="AetherSDR", dbm=-33, band="80m", mode="LSB", filt="2800",
                      digits="  3630" "00", active=6, step="100 Hz",
                      agc="MED", gain_caption="RF.G", gain="", gain_known=False),
    "icom":      dict(name="the IC-705", dbm=-53, band="2m", mode="FM", filt="FIL1",
                      digits="145500" "00", active=4, step="10 kHz",
                      agc="FAST", gain_caption="P.AMP", gain="ON", gain_known=True),
    # A FlexRadio over its own API: RF.G is the panadapter's RF gain, known.
    "multiflex": dict(name="FlexRadio", dbm=-53, band="20m", mode="USB", filt="2700",
                      digits=" 14200" "00", active=6, step="100 Hz",
                      agc="MED", gain_caption="RF.G", gain="+8 dB", gain_known=True),
    # A reflector: the talkgroup where the frequency is, who is talking where
    # the S-units are, and the arc in dBFS.
    "svxconnect": dict(name="SVXConnect", reflector=True, dbfs=-14, tg=8,
                       tg_name="70cm Repeaters", server="be.svx.link",
                       talker="ON6URE", talking="14s"),
    # A telephone on a call. The numbers are Ofcom's, kept for drama: no one
    # answers them.
    "phone": dict(name="Telephone", phone=True, call=3, secs=167, dbfs=-20,
                  rx_pk=-13, tx_db=-31, tx_pk=-23, peer="Mum",
                  peer_num="+447700900123"),
}


def level_frac(db):
    """ui.c smeter_frac() in the svxconnect build: -60 to 0 dBFS, evenly."""
    return max(0.0, min(1.0, (db + 60.0) / 60.0))


# The reflector face's lock and mute (font_svx_icons_24: Font Awesome's
# lock-open and volume-up), centred on (x, y) as LVGL centres the label.
def lock_open(x, y, colour):
    return (f'<g transform="translate({x - 9:.1f},{y - 11:.1f})">'
            f'<rect x="0" y="9" width="16" height="12" rx="2.5" fill="{colour}"/>'
            f'<path d="M3.5,9 V6 a5,5 0 0 1 9.6,-2" fill="none" stroke="{colour}" '
            f'stroke-width="2.6" stroke-linecap="round"/></g>')


def sound_on(x, y, colour):
    return (f'<g transform="translate({x - 11:.1f},{y - 10:.1f})" fill="{colour}">'
            f'<path d="M0,6.5 h4.5 l6,-5.5 v18 l-6,-5.5 h-4.5 z"/>'
            f'<path d="M14,6 a5,5 0 0 1 0,8 M17,3 a9,9 0 0 1 0,14" fill="none" '
            f'stroke="{colour}" stroke-width="2" stroke-linecap="round"/></g>')


def audio_arc(DB):
    """The reflector face's arc: the audio, -60 to 0 dBFS, at DB."""
    s = [f'<circle cx="180" cy="180" r="180" fill="{D.BG}"/>',
         f'<path d="{D.arc_path(D.ARC_ROT, D.ARC_ROT + D.ARC_SPAN, D.RC)}" '
         f'fill="none" stroke="{D.SUBTLE}" stroke-width="{D.BAND}"/>']
    for lo, hi, col in D.RXZONES:
        s.append(D.block(D.ARC_ROT + level_frac(lo) * D.ARC_SPAN,
                         D.ARC_ROT + level_frac(hi) * D.ARC_SPAN, col))
    a0, a1 = D.ARC_ROT, D.ARC_ROT + D.ARC_SPAN
    L = D.arc_len(a0, a1, D.RC)
    s.append(f'<path d="{D.arc_path(a1, a0, D.RC)}" fill="none" stroke="{D.SUBTLE}" '
             f'stroke-width="{D.BAND + 0.5}" '
             f'stroke-dasharray="{L * (1 - level_frac(DB)):.2f} {L + 10:.2f}"/>')
    for d in (-48, -36, -24, -18, -12, -6, -3):
        s.append(D.notch(D.ARC_ROT + level_frac(d) * D.ARC_SPAN))
    # ui.c add_ticks(), REFLECTOR_FACE: -12 dBFS is the landmark, amber above.
    for d, ln, kind in ((-48, 6, 0), (-36, 6, 0), (-24, 6, 0), (-18, 6, 0),
                        (-12, 11, 1), (-6, 6, 2), (-3, 6, 2), (0, 9, 2)):
        col = D.TEXT2 if kind == 1 else (D.WARN if kind == 2 else D.LABEL)
        s.append(D.tick(D.ARC_ROT + level_frac(d) * D.ARC_SPAN, ln, col,
                        3 if kind == 1 else 2))
    return s


def reflector_dial(R):
    """The svxconnect firmware's receive face (ui.c, REFLECTOR_FACE)."""
    s = audio_arc(R["dbfs"])
    # Who is talking, where the S-units are, and for how long.
    s.append(D.text(180, 83, R["talker"], 20, D.TEXT, 700))
    s.append(D.text(180, 103, R["talking"], 14, D.GREEN))
    # The talkgroup's row: the lock left, the mute right.
    s.append(lock_open(D.CX - 76, 122, D.LABEL))
    s.append(D.text(D.CX, 129, f"TG {R['tg']}", 20, D.ACCENT))
    s.append(sound_on(D.CX + 76, 122, D.LABEL))
    # Where the frequency is: the talkgroup's name, and the reflector.
    s.append(D.text(D.CX, 170, R["tg_name"], 28, D.TEXT))
    s.append(D.text(D.CX, 203, R["server"], 20, D.TEXT2))
    s.append(D.text(D.CX - 56, 227, "connected", 20, D.GREEN))
    s.append(D.icon_readout(D.CX + 42, 227, D.speaker, "40", D.TEXT2))
    s.append(D.icon_readout(D.CX + 104, 227, D.microphone, "100", D.TEXT2))
    s.append(D.ptt_slab(D.BG1, "PTT", D.TEXT2))
    return "".join(s)


# --- the telephone's face -------------------------------------------------------
# ui.c's own numbers (PHONE_FACE): the keypad's keys KP_W x KP_H from
# KP_TOP, its number and backspace on KP_ROW_Y.
KP_TOP, KP_W, KP_H, KP_GAP_X, KP_GAP_Y = 66, 70, 42, 6, 4
KP_ROW_Y, KP_NUM_DX, KP_BS_DX = 43, -16, 88
KP_KEYS = "123456789*0#"

# What phone_dial() draws unless told otherwise: idle, registered, a dozen
# favourites with "Office" on the dial. ui_state_t's call: 0 idle, 1 calling
# out, 2 ringing in, 3 talking, 4 ended.
PHONE = dict(call=0, secs=0, why="", peer="", peer_num="", fav_name="Office",
             fav_num="+441632960123", n_fav=12, n_missed=0, number="+447700900461",
             link="connected", muted=False, dbfs=-60, tx_db=-60, rx_pk=None,
             tx_pk=None, keypad=None,
             flash=None, headset=False, vol="40", mic="100")


def split_arc(rx_db, tx_db, rx_pk=None, tx_pk=None):
    """The telephone's arc in two (ui.c vu_build): their audio on the left
    half, filling up from the left end; ours on the right half, filling up
    from the right end. Each with the reflector's zones, notches and ticks on
    its half, and its peak LED, a 3 degree block in its zone's colour."""
    s = [f'<circle cx="180" cy="180" r="180" fill="{D.BG}"/>']
    for side, db, pk in ((0, rx_db, rx_pk), (1, tx_db, tx_pk)):
        rot, span, mirror = (D.SWR_ROT, D.SWR_SPAN, False) if side == 0 else (D.AUD_ROT, D.AUD_SPAN, True)

        def ang(d, rot=rot, span=span, mirror=mirror):
            f = level_frac(d)
            return rot + ((1 - f) if mirror else f) * span
        s.append(f'<path d="{D.arc_path(rot, rot + span, D.RC)}" fill="none" '
                 f'stroke="{D.SUBTLE}" stroke-width="{D.BAND}"/>')
        for lo, hi, col in D.RXZONES:
            if db > lo:
                a0, a1 = ang(lo), ang(min(db, hi))
                s.append(D.block(min(a0, a1), max(a0, a1), col))
        for d in (-48, -36, -24, -18, -12, -6, -3):
            s.append(D.notch(ang(d)))
        for d, ln, kind in ((-24, 6, 0), (-12, 11, 1), (-6, 6, 2), (0, 9, 2)):
            col = D.TEXT2 if kind == 1 else (D.WARN if kind == 2 else D.LABEL)
            s.append(D.tick(ang(d), ln, col, 3 if kind == 1 else 2))
        if pk is not None and pk > -60:
            zc = next((col for lo, hi, col in D.RXZONES if pk < hi), D.RXZONES[-1][2])
            a = ang(pk)
            s.append(D.block(a, a + 3, zc) if mirror else D.block(a - 3, a, zc))
    return s


def sound_muted(x, y, colour):
    """font_svx_icons_24's volume-xmark (SYM_MUTED): sound_on()'s speaker,
    a cross where its waves were."""
    return (f'<g transform="translate({x - 11:.1f},{y - 10:.1f})" fill="{colour}">'
            f'<path d="M0,6.5 h4.5 l6,-5.5 v18 l-6,-5.5 h-4.5 z"/>'
            f'<path d="M14,6.5 l7,7 M21,6.5 l-7,7" fill="none" stroke="{colour}" '
            f'stroke-width="2.2" stroke-linecap="round"/></g>')


def backspace(cx, top, colour, bg):
    """LV_SYMBOL_BACKSPACE, Font Awesome's delete-left, as Montserrat 28 has
    it: 35 x 21 px, centred on cx, its top at `top`."""
    x0, x1, y0, y1 = cx - 17.5, cx + 17.5, top, top + 21
    return (f'<path d="M{x0:.1f},{(y0 + y1) / 2:.1f} L{x0 + 10:.1f},{y0:.1f} H{x1 - 3:.1f} '
            f'a3,3 0 0 1 3,3 V{y1 - 3:.1f} a3,3 0 0 1 -3,3 H{x0 + 10:.1f} Z" fill="{colour}"/>'
            f'<path d="M{cx - 1:.1f},{y0 + 6:.1f} l9,9 M{cx + 8:.1f},{y0 + 6:.1f} l-9,9" '
            f'stroke="{bg}" stroke-width="2.6" stroke-linecap="round"/>')


def esc(s):
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


def phone_dial(R):
    """The telephone's face (ui.c, PHONE_FACE): SVXConnect's, with the
    favourites where the talkgroups were. R as PHONE above; `keypad` the
    digits typed with the keypad up ("" for none yet), `flash` the key just
    tapped."""
    P = dict(PHONE)
    P.update(R)
    call = P["call"]
    # The meters move only in a call (ui.c: both at rest outside one).
    talking = call == 3
    s = split_arc(P["dbfs"] if talking else -60, P["tx_db"] if talking else -60,
                  P["rx_pk"] if talking else None, P["tx_pk"] if talking else None)
    # Under the arc: the call's time, or what the call is doing.
    secs = P["secs"]
    if call == 3:
        a, ca, b, cb = f"{secs // 60:02d}:{secs % 60:02d}", D.TEXT, "in call", D.GREEN
    elif call == 2:
        a, ca, b, cb = "RINGING", D.GREEN, "incoming call", D.GREEN
    elif call == 1:
        a, ca, b, cb = ("RINGING" if P["why"] == "ringing" else "CALLING"), D.TEXT, f"{secs}s", D.WARN
    elif call == 4:
        a, ca, b, cb = "ENDED", D.LABEL, P["why"], D.LABEL
    elif P["n_missed"]:
        a, ca, b, cb = "--", D.LABEL, f"{P['n_missed']} missed", D.DANGER
    else:
        a, ca, b, cb = "--", D.LABEL, f"{P['n_fav']} favourites" if P["n_fav"] else "", D.LABEL
    s.append(D.text(180, 83, esc(a), 20, ca, 700))
    s.append(D.text(180, 103, esc(b), 14, cb))
    # The talkgroup's row: the favourite's number at rest, the other end's in
    # a call. No lock -- nothing to lock -- and the mute out at the right,
    # clear of a long number.
    row = P["fav_num"] if call == 0 else P["peer_num"]
    s.append(D.text(D.CX, 129, esc(row or "--"), 20, D.ACCENT))
    # The mute: the knob's own microphone (font_btmic_28), struck through in
    # red when muted; greyed while a headset is in use.
    s.append(D.microphone28(D.CX + 106, 109, D.DANGER if P["muted"] else
                            D.DISABLED if P["headset"] else D.LABEL, P["muted"], D.BG))
    # The middle: in a call the other end by name, or by number; how a call
    # ended; at rest the favourite -- never a favourite in a call. Our own
    # number under it.
    big = (P["peer"] or P["peer_num"] if call in (1, 2, 3)
           else (P["why"] or "call ended").upper() if call == 4
           else P["fav_name"] if P["n_fav"] else "NO FAVOURITES")
    s.append(D.text(D.CX, 170, esc(big), 28, D.TEXT))
    s.append(D.text(D.CX, 203, esc(P["number"]), 20, D.TEXT2))
    link = P["link"]
    s.append(D.text(D.CX - 56, 227, link, 20, D.GREEN if link == "connected"
                    else D.WARN if link == "connecting" else D.DANGER))
    s.append(D.icon_readout(D.CX + 42, 227, D.speaker, P["vol"], D.TEXT2))
    s.append(D.icon_readout(D.CX + 104, 227, D.microphone, P["mic"], D.TEXT2))
    # The slab: the call's next step.
    kp = P["keypad"]
    if call == 2 and P["headset"]:
        # The headset answers: the slab declines, a line says so (ui.c).
        s.append(D.ptt_slab(D.TX_RED, "DECLINE", "#FFFFFF"))
        s.append(D.text(180, D.PTT_TOP + 52 + 12, "answer on the headset", 14, "#FFFFFF"))
    elif call == 2:
        # No headset: the slab in two, DECLINE left and ANSWER right.
        for x, w, col, lab in ((0, 179, D.TX_RED, "DECLINE"), (181, 179, D.GREEN, "ANSWER")):
            s.append(f'<rect x="{x}" y="{D.PTT_TOP}" width="{w}" height="{360 - D.PTT_TOP}" fill="{col}"/>')
            s.append(f'<line x1="{x}" y1="{D.PTT_TOP + 1}" x2="{x + w}" y2="{D.PTT_TOP + 1}" '
                     f'stroke="{D.ACCENT}" stroke-width="2"/>')
            s.append(D.text(102 if x == 0 else 258, D.PTT_TOP + 14 + 25, lab, 28, "#FFFFFF", 500))
    elif call in (1, 3):
        s.append(D.ptt_slab(D.TX_RED, "HANG UP", "#FFFFFF"))
    elif call == 4:
        s.append(D.ptt_slab(D.BG1, "ENDED", D.TEXT2))
    elif link != "connected":
        s.append(D.ptt_slab(D.BG1, "NO SERVICE", D.TEXT2))
    elif kp:
        s.append(D.ptt_slab(D.BG1, "CALL", D.TEXT))
    else:
        s.append(D.ptt_slab(D.BG1, "CALL" if P["n_fav"] else "----", D.TEXT2))
    if call == 2:                               # ringing in: the rim green (ui.c ring_anim)
        s.append(f'<circle cx="180" cy="180" r="178" fill="none" stroke="{D.GREEN}" '
                 f'stroke-width="4"/>')
    if P["headset"]:                            # only its logo: the slab is the call's
        s.append(D.bluetooth(180 + 126, D.PTT_TOP + 12 + 18, D.ACCENT))
    # The keypad, over everything above the slab: the number -- eight
    # characters in the big type, more in the smaller -- the backspace, keys.
    if kp is not None:
        s.append(f'<rect x="0" y="0" width="360" height="{D.PTT_TOP - 2}" fill="{D.BG}"/>')
        n = len(kp)
        show = kp[-12:] if n else "DTMF" if call == 3 else "number"
        size, base = (20, KP_ROW_Y - 11 + 18) if n > 8 else (28, KP_ROW_Y - 15 + 25)
        s.append(D.text(180 + KP_NUM_DX, base, esc(show), size, D.TEXT if n else D.LABEL))
        s.append(backspace(180 + KP_BS_DX, KP_ROW_Y - 15 + 4, D.LABEL, D.BG))
        # The close, LV_SYMBOL_CLOSE at Montserrat 20, at the top (ui.c s_kp_x).
        s.append(f'<path d="M174,11 l12,12 M186,11 l-12,12" stroke="{D.LABEL}" '
                 f'stroke-width="3" stroke-linecap="round"/>')
        for i, k in enumerate(KP_KEYS):
            x = 180 - (3 * KP_W + 2 * KP_GAP_X) // 2 + (i % 3) * (KP_W + KP_GAP_X)
            y = KP_TOP + (i // 3) * (KP_H + KP_GAP_Y)
            fill = D.ACCENT if k == P["flash"] else D.BG1
            s.append(f'<rect x="{x}" y="{y}" width="{KP_W}" height="{KP_H}" rx="12" fill="{fill}"/>')
            s.append(D.text(x + KP_W / 2, y + 6 + 25, k, 28, D.TEXT))
    return "".join(s)

# --- the body, in face pixels (360 px = the 1.8" panel) ---------------------
R_BODY, R_GLASS = D.BODY_R, D.GLASS_R
DEPTH_BLUE = 13.5 * D.MM                    # knurled ring
DEPTH_ALL = 22.0 * D.MM                     # ring and black base
KNURLS = 28                                 # ridges around the ring
KNURL_TWIST = math.radians(34)              # how far a ridge leans

BLUE = (46, 104, 232)
BLACK = (26, 28, 33)

# The two finishes the knob ships in. The black one is sampled off Waveshare's
# own photograph of it: the knurled ring's faces sit around 25 grey with
# highlights to about 120, the top of the ring about 42, the base about 23 --
# all neutral. Shading brings a side face down to roughly 60% of its albedo,
# so a ring of 40 lands where the photograph does, and the key light's
# highlight supplies the bright diagonal bands.
FINISHES = {
    "": dict(ring=BLUE, base=BLACK, top_gain=0.72, glass_edge="#0B1F5C",
             knurl_spec=0.45, knurl_gloss=18, knurl_dark=0.74, knurl_sheen=0),
    # Black anodising shows its knurl by reflection, not by colour: in the
    # photograph alternate faces carry the studio's softboxes as bright
    # diagonal bands (to ~120) between faces near 20. A point light's highlight
    # cannot do that, so every other face gets that reflected sheen outright,
    # fading towards the silhouette as a reflection does.
    "-black": dict(ring=(40, 40, 41), base=(30, 30, 30), top_gain=0.32,
                   glass_edge="#000000",
                   knurl_spec=0.3, knurl_gloss=8, knurl_dark=0.7, knurl_sheen=110),
}


def norm(v):
    m = math.sqrt(sum(c * c for c in v))
    return tuple(c / m for c in v)


# A small studio, the way the product photographs are lit: a key light from
# the upper left, a broad fill from the front so the side is never black, and
# a light from the right to pick out the knurl on the far side.
LIGHTS = [(norm((-0.45, -0.62, 0.64)), 0.70),
          (norm((0.05, 0.25, 1.00)), 0.45),
          (norm((0.85, -0.20, 0.45)), 0.35)]
KEY = LIGHTS[0][0]
HALF = norm((KEY[0], KEY[1], KEY[2] + 1.0))  # Blinn half-vector, key light


def shade(albedo, n, spec=0.35, gloss=18, ambient=0.22, gain=1.0, lift=0.0):
    """Lambert from each light plus a broad highlight from the key: anodised
    metal, not chrome. gain darkens one face of a knurl ridge; lift adds grey
    reflected off the studio, for finishes that show their shape that way."""
    d = sum(i * max(0.0, sum(a * b for a, b in zip(n, l))) for l, i in LIGHTS)
    s = max(0.0, sum(a * b for a, b in zip(n, HALF))) ** gloss
    return "#%02X%02X%02X" % tuple(
        max(0, min(255, int(c * (ambient + d) * gain + 255 * spec * s + lift)))
        for c in albedo)


def rot_x(a):
    c, s = math.cos(a), math.sin(a)
    return ((1, 0, 0), (0, c, -s), (0, s, c))


def rot_y(a):
    c, s = math.cos(a), math.sin(a)
    return ((c, 0, s), (0, 1, 0), (-s, 0, c))


def mul(m, v):
    return tuple(sum(m[i][k] * v[k] for k in range(3)) for i in range(3))


def mmul(a, b):
    return tuple(tuple(sum(a[i][k] * b[k][j] for k in range(3)) for j in range(3))
                 for i in range(3))


# --- the dial, frozen ---------------------------------------------------------

def dial(radio):
    """A radio's receive face at its reading, in its own 360 px coordinates.
    The palette is the module's: see mkdisplay.use_palette()."""
    R = RADIOS[radio]
    if R.get("reflector"):
        return reflector_dial(R)
    if R.get("phone"):
        return phone_dial(R)
    DBM = R["dbm"]
    s = [f'<circle cx="180" cy="180" r="180" fill="{D.BG}"/>',
         f'<path d="{D.arc_path(D.ARC_ROT, D.ARC_ROT + D.ARC_SPAN, D.RC)}" '
         f'fill="none" stroke="{D.SUBTLE}" stroke-width="{D.BAND}"/>']
    for lo, hi, col in D.RXZONES:
        s.append(D.block(D.ARC_ROT + D.smeter_frac(lo) * D.ARC_SPAN,
                         D.ARC_ROT + D.smeter_frac(hi) * D.ARC_SPAN, col))
    # The unreached part of the scale, covered in the track colour.
    a0, a1 = D.ARC_ROT, D.ARC_ROT + D.ARC_SPAN
    L = D.arc_len(a0, a1, D.RC)
    s.append(f'<path d="{D.arc_path(a1, a0, D.RC)}" fill="none" stroke="{D.SUBTLE}" '
             f'stroke-width="{D.BAND + 0.5}" '
             f'stroke-dasharray="{L * (1 - D.smeter_frac(DBM)):.2f} {L + 10:.2f}"/>')
    for d in D.RXNOTCH:
        s.append(D.notch(D.ARC_ROT + D.smeter_frac(d) * D.ARC_SPAN))
    for d, ln, _ in D.RXTICKS:
        col = D.TEXT2 if d == -73 else (D.WARN if d > -73 else D.LABEL)
        s.append(D.tick(D.ARC_ROT + D.smeter_frac(d) * D.ARC_SPAN, ln, col,
                        3 if d == -73 else 2))
    s.append(D.text(180, 83, D.smeter_text(DBM), 20, D.TEXT, 700))
    s.append(D.text(180, 103, f"{DBM} dBm", 14, D.LABEL))
    s.append(D.aux(R["agc"], R["gain_caption"], R["gain"], R["gain_known"]))
    s.append(D.text(D.CX - 76, 129, R["band"], 20, D.ACCENT))
    s.append(D.text(D.CX, 129, R["mode"], 20, D.TEXT))
    s.append(D.text(D.CX + 76, 129, R["filt"], 20, D.TEXT2))
    # The colours passed, not left to readout()'s defaults: those were bound
    # when mkdisplay was imported, to the AetherSDR palette.
    s.append(D.readout(R["digits"], colour=D.TEXT, sep_colour=D.LABEL,
                       underline=R["active"], after_colour=D.TEXT2,
                       active_colour=D.ACCENT_HI))
    s.append(D.text(D.CX - 98, 227, R["step"], 20, D.ACCENT))
    s.append(D.text(D.CX - 24, 227, "RIT 0", 14, D.DISABLED))
    s.append(D.icon_readout(D.CX + 42, 227, D.speaker, "40", D.TEXT2))
    s.append(D.icon_readout(D.CX + 104, 227, D.microphone, "100", D.TEXT2))
    s.append(D.ptt_slab(D.BG1, "PTT", D.TEXT2))
    return "".join(s)


# --- the body -----------------------------------------------------------------

class View:
    """An orientation of the knob: where its face axes and normal point."""

    def __init__(self, rot):
        self.u = mul(rot, (1, 0, 0))        # face right
        self.v = mul(rot, (0, 1, 0))        # face down
        self.n = mul(rot, (0, 0, 1))        # out of the glass

    def p(self, phi, r, w=0.0):
        """A point on the body: angle phi on the face, radius r, depth w."""
        c, s = math.cos(phi), math.sin(phi)
        return (r * (c * self.u[0] + s * self.v[0]) - w * self.n[0],
                r * (c * self.u[1] + s * self.v[1]) - w * self.n[1])

    def side_normal(self, phi):
        c, s = math.cos(phi), math.sin(phi)
        return tuple(c * a + s * b for a, b in zip(self.u, self.v))


def poly(pts, fill, extra=""):
    d = "M" + " L".join(f"{x:.2f},{y:.2f}" for x, y in pts) + " Z"
    return f'<path d="{d}" fill="{fill}" stroke="{fill}" stroke-width="0.6"{extra}/>'


def band(view, w0, w1, albedo, knurled, finish=None):
    """One ring of the side, as strips between (possibly helical) lines.

    Knurled: KNURLS ridges, each two flat faces leaning either way of the
    radius, following a helix so the ridges run diagonally across the ring.
    Plain: many narrow strips, for a smooth cylinder."""
    out = []
    steps = 8                                   # along the depth
    if knurled:
        faces = 2 * KNURLS
        tilt = math.radians(17)                 # each ridge face off the radius
    else:
        faces, tilt = 144, 0.0
    # A ridge leaning at KNURL_TWIST advances depth * tan(lean) around the
    # rim, i.e. tan(lean) / R radians per pixel of depth.
    twist = math.tan(KNURL_TWIST) / R_BODY if knurled else 0.0
    for f in range(faces):
        p0 = 2 * math.pi * f / faces
        p1 = 2 * math.pi * (f + 1) / faces
        lean = tilt if f % 2 == 0 else -tilt
        for k in range(steps):
            wa = w0 + (w1 - w0) * k / steps
            wb = w0 + (w1 - w0) * (k + 1) / steps
            sa = twist * (wa - w0)
            sb = twist * (wb - w0)
            mid = (p0 + p1) / 2 + (sa + sb) / 2
            n = view.side_normal(mid + lean)
            if view.side_normal(mid)[2] <= 0:
                continue                         # facing away
            pts = [view.p(p0 + sa, R_BODY, wa), view.p(p1 + sa, R_BODY, wa),
                   view.p(p1 + sb, R_BODY, wb), view.p(p0 + sb, R_BODY, wb)]
            if knurled:
                lit = f % 2 == 0
                # Strongest where the ring faces the camera and near its top
                # edge, falling away round the side and down the ring.
                face = max(0.0, view.side_normal(mid)[2]) ** 2.5
                down = ((wa + wb) / 2 - w0) / (w1 - w0)
                sheen = finish["knurl_sheen"] * face * (1 - 0.6 * down) if lit else 0
                col = shade(albedo, n, spec=finish["knurl_spec"],
                            gloss=finish["knurl_gloss"],
                            gain=1.0 if lit else finish["knurl_dark"], lift=sheen)
            else:
                col = shade(albedo, n, spec=0.2)
            out.append(poly(pts, col))
    return "".join(out)


def affine(view, cx, cy):
    """SVG matrix taking the 360 px face (centre 180,180) onto the glass."""
    a, b = view.u[0], view.u[1]
    c, d = view.v[0], view.v[1]
    return (f"matrix({a:.5f},{b:.5f},{c:.5f},{d:.5f},"
            f"{cx - 180 * (a + c):.3f},{cy - 180 * (b + d):.3f})")


def render(view, title, radio, shadow=False, finish=FINISHES[""]):
    # Extent: the front and back rims.
    pts = [view.p(2 * math.pi * i / 180, R_BODY, w)
           for i in range(180) for w in (0, DEPTH_ALL)]
    xs, ys = [p[0] for p in pts], [p[1] for p in pts]
    pad = 40
    x0, y0 = min(xs) - pad, min(ys) - pad
    W, H = max(xs) - min(xs) + 2 * pad, max(ys) - min(ys) + 2 * pad + (60 if shadow else 0)
    cx, cy = -x0, -y0                           # the face centre on the canvas
    ring = finish["ring"]
    # The ring's top faces the camera and would take every light at once;
    # held down to the deeper colour it has in photographs.
    top = shade(ring, view.n, spec=0.15, gain=finish["top_gain"])
    rim = shade(ring, view.n, spec=0.9, gloss=6, ambient=0.6)
    s = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {W:.0f} {H:.0f}" '
         f'width="{W:.0f}" height="{H:.0f}" role="img" aria-label="{title}">'
         f'<title>{title}</title><defs>'
         f'<clipPath id="panel"><circle cx="180" cy="180" r="180"/></clipPath>'
         f'<linearGradient id="sheen" gradientUnits="userSpaceOnUse" '
         f'x1="{-R_BODY:.0f}" y1="{-R_BODY:.0f}" x2="{R_BODY:.0f}" y2="{R_BODY:.0f}">'
         f'<stop offset="0" stop-color="#FFFFFF" stop-opacity="0.22"/>'
         f'<stop offset="0.5" stop-color="#FFFFFF" stop-opacity="0"/>'
         f'<stop offset="1" stop-color="#000000" stop-opacity="0.25"/></linearGradient>'
         f'<linearGradient id="gloss" gradientUnits="userSpaceOnUse" '
         f'x1="{-R_GLASS:.0f}" y1="{-R_GLASS:.0f}" x2="{R_GLASS * 0.4:.0f}" '
         f'y2="{R_GLASS * 0.6:.0f}">'
         f'<stop offset="0" stop-color="#FFFFFF" stop-opacity="0.16"/>'
         f'<stop offset="0.55" stop-color="#FFFFFF" stop-opacity="0.03"/>'
         f'<stop offset="0.56" stop-color="#FFFFFF" stop-opacity="0"/></linearGradient>'
         f'<filter id="soft" x="-50%" y="-50%" width="200%" height="200%">'
         f'<feGaussianBlur stdDeviation="18"/></filter>'
         f'</defs>']
    if shadow:
        # Resting on a surface: a soft contact shadow the shape of the base's
        # footprint, a little larger and nudged towards the viewer.
        bx, by = view.p(0, 0, DEPTH_ALL)
        ry = R_BODY * abs(view.v[1])
        s.append(f'<ellipse cx="{cx + bx:.1f}" cy="{cy + by + 14:.1f}" '
                 f'rx="{R_BODY * 1.04:.1f}" ry="{ry * 1.06:.1f}" fill="#000" '
                 f'opacity="0.45" filter="url(#soft)"/>')
    s.append(f'<g transform="translate({cx:.2f},{cy:.2f})">')
    s.append(band(view, DEPTH_BLUE, DEPTH_ALL, finish["base"], knurled=False))
    s.append(band(view, 0, DEPTH_BLUE, ring, knurled=True, finish=finish))
    s.append('</g>')
    # The front: bezel, glass and panel, all in face coordinates.
    s.append(f'<g transform="translate({cx:.2f},{cy:.2f}) {affine(view, 0, 0)}">'
             f'<g transform="translate(180,180)">'
             f'<circle r="{R_BODY:.1f}" fill="{top}"/>'
             f'<circle r="{R_BODY:.1f}" fill="url(#sheen)"/>'
             f'<circle r="{R_BODY - 1.5:.1f}" fill="none" stroke="{rim}" '
             f'stroke-width="3" opacity="0.7"/>'
             f'<circle r="{R_GLASS + 2.5:.1f}" fill="none" stroke="{finish["glass_edge"]}" '
             f'stroke-width="4" opacity="0.6"/>'
             f'<circle r="{R_GLASS:.1f}" fill="#050608"/></g>'
             f'<g clip-path="url(#panel)">{dial(radio)}</g>'
             f'<g transform="translate(180,180)">'
             f'<circle r="{R_GLASS:.1f}" fill="url(#gloss)"/></g></g>')
    s.append('</svg>')
    return "".join(s)


VIEWS = {
    # Standing on its rim, face turned 32 degrees, camera a little high.
    "knob-angled-text-left": (View(mmul(rot_x(math.radians(-7)), rot_y(math.radians(-32)))),
                         "angled, body to the right", False),
    "knob-angled-text-right": (View(mmul(rot_x(math.radians(-7)), rot_y(math.radians(32)))),
                          "angled, body to the left", False),
    # On its base, seen from 50 degrees above the table.
    "knob-upright": (View(rot_x(math.radians(90 - 50))),
                     "on a desk", True),
}


# --- onto a photograph ----------------------------------------------------------
#
#   tools/mkrender.py --photo product.png [--radio icom] [--seed X,Y] [--scale 2]
#
# Puts the dial on the cover glass of an existing product photograph. The glass
# is found by growing a region from a seed pixel inside it: neighbours join
# while the colour changes only gradually (the glass carries soft studio
# reflections) and stays a dark neutral, so the growth stops at the sharp edge
# where the bezel starts. An ellipse fitted to that region's second moments
# gives the glass outline; the dial goes onto it with one affine distort --
# the same orthographic model as the renders above.

def fit_glass(path, seed, step_tol=8, max_lum=140):
    from collections import deque
    w, h = map(int, subprocess.check_output(
        ["magick", "identify", "-format", "%w %h", path]).split())
    raw = subprocess.check_output(["magick", path, "-depth", "8", "rgb:-"])
    px = lambda x, y: raw[3 * (y * w + x):3 * (y * w + x) + 3]
    seen = bytearray(w * h)
    seen[seed[1] * w + seed[0]] = 1
    q, pts = deque([seed]), []
    while q:
        x, y = q.popleft()
        c = px(x, y)
        pts.append((x, y))
        for nx, ny in ((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)):
            if 0 <= nx < w and 0 <= ny < h and not seen[ny * w + nx]:
                d = px(nx, ny)
                if (max(abs(d[i] - c[i]) for i in range(3)) <= step_tol
                        and max(d) <= max_lum and max(d) - min(d) <= 30):
                    seen[ny * w + nx] = 1
                    q.append((nx, ny))
    n = len(pts)
    mx = sum(p[0] for p in pts) / n
    my = sum(p[1] for p in pts) / n
    sxx = sum((p[0] - mx) ** 2 for p in pts) / n
    syy = sum((p[1] - my) ** 2 for p in pts) / n
    sxy = sum((p[0] - mx) * (p[1] - my) for p in pts) / n
    half, det = (sxx + syy) / 2, sxx * syy - sxy * sxy
    root = math.sqrt(max(0.0, half * half - det))
    # A filled ellipse's semi-axis is twice the square root of its moment.
    return dict(w=w, h=h, cx=mx, cy=my, a=2 * math.sqrt(half + root),
                b=2 * math.sqrt(half - root),
                angle=0.5 * math.atan2(2 * sxy, sxx - syy))


def glass_svg(radio):
    """The cover glass, face-on: black glass, the dial, a touch of gloss."""
    g = R_GLASS
    return (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="{-g:.1f} {-g:.1f} '
            f'{2 * g:.1f} {2 * g:.1f}" width="{2 * g:.0f}" height="{2 * g:.0f}">'
            f'<defs><clipPath id="panel"><circle cx="180" cy="180" r="180"/></clipPath>'
            f'<linearGradient id="gloss" x1="0" y1="0" x2="0" y2="1">'
            f'<stop offset="0" stop-color="#FFFFFF" stop-opacity="0.10"/>'
            f'<stop offset="0.45" stop-color="#FFFFFF" stop-opacity="0.02"/>'
            f'<stop offset="0.46" stop-color="#FFFFFF" stop-opacity="0"/>'
            f'</linearGradient></defs>'
            f'<circle r="{g:.1f}" fill="#07080B"/>'
            f'<g transform="translate(-180,-180)"><g clip-path="url(#panel)">'
            f'{dial(radio)}</g></g>'
            f'<circle r="{g:.1f}" fill="url(#gloss)"/></svg>')


def cutout(photo, out):
    """The photograph with its white studio background made transparent.

    The background is everything light and neutral that connects to the
    border. It becomes black at the opacity its darkness implies, so a soft
    grey shadow under the knob survives as a see-through shadow and the grey
    anti-aliased rim of the black base un-mixes itself. Coloured edge pixels
    -- blue fading into white -- are un-mixed against the colour just inside
    the edge, so no pale fringe is left on a dark page."""
    from collections import deque
    w, h = map(int, subprocess.check_output(
        ["magick", "identify", "-format", "%w %h", photo]).split())
    raw = subprocess.check_output(["magick", photo, "-depth", "8", "rgb:-"])
    N = w * h
    px = [tuple(raw[3 * i:3 * i + 3]) for i in range(N)]
    light = lambda p: min(p) >= 150 and max(p) - min(p) <= 24
    bg = bytearray(N)
    q = deque()
    for x in range(w):
        for y in (0, h - 1):
            if light(px[y * w + x]) and not bg[y * w + x]:
                bg[y * w + x] = 1
                q.append(y * w + x)
    for y in range(h):
        for x in (0, w - 1):
            if light(px[y * w + x]) and not bg[y * w + x]:
                bg[y * w + x] = 1
                q.append(y * w + x)
    while q:
        i = q.popleft()
        x, y = i % w, i // w
        for nx, ny in ((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)):
            j = ny * w + nx
            if 0 <= nx < w and 0 <= ny < h and not bg[j] and light(px[j]):
                bg[j] = 1
                q.append(j)
    # How far each foreground pixel is from the background, up to 3.
    dist = bytearray([0 if bg[i] else 9 for i in range(N)])
    ring = [i for i in range(N) if bg[i]]
    for d in (1, 2, 3):
        nxt = []
        for i in ring:
            x, y = i % w, i // w
            for nx, ny in ((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)):
                j = ny * w + nx
                if 0 <= nx < w and 0 <= ny < h and dist[j] > d:
                    dist[j] = d
                    nxt.append(j)
        ring = nxt
    WHITE = (255, 255, 255)
    rgba = bytearray(4 * N)
    for i in range(N):
        c = px[i]
        if bg[i]:
            a = max(0, min(255, int((252 - sum(c) / 3) / 252 * 255)))
            rgba[4 * i:4 * i + 4] = bytes((0, 0, 0, a if a > 3 else 0))
            continue
        if dist[i] <= 2:
            x, y = i % w, i // w
            ref = [px[j] for j in (yy * w + xx for yy in range(max(0, y - 3), min(h, y + 4))
                                   for xx in range(max(0, x - 3), min(w, x + 4)))
                   if dist[j] >= 3]
            if ref:
                f = tuple(sum(p[k] for p in ref) / len(ref) for k in range(3))
                wf = [WHITE[k] - f[k] for k in range(3)]
                den = sum(v * v for v in wf)
                if den > 900:
                    wc = [WHITE[k] - c[k] for k in range(3)]
                    a = max(0.0, min(1.0, sum(wc[k] * wf[k] for k in range(3)) / den))
                    if a < 0.04:
                        rgba[4 * i:4 * i + 4] = bytes(4)
                        continue
                    col = tuple(max(0, min(255, int((c[k] - (1 - a) * 255) / a)))
                                for k in range(3))
                    rgba[4 * i:4 * i + 4] = bytes(col + (int(a * 255),))
                    continue
        rgba[4 * i:4 * i + 4] = bytes(c + (255,))
    subprocess.run(["magick", "-size", f"{w}x{h}", "-depth", "8", "rgba:-", out],
                   input=bytes(rgba), check=True)


def on_photo(photo, seed, scale, out, radio):
    fit = fit_glass(photo, seed)
    W, H = int(fit["w"] * scale), int(fit["h"] * scale)
    cx, cy = fit["cx"] * scale, fit["cy"] * scale
    a, b, t = fit["a"] * scale, fit["b"] * scale, fit["angle"]
    # Render the glass well above the size it lands at, then distort down.
    S = int(max(2 * a, 1200))
    tmp = out + ".glass.png"
    svg = out + ".glass.svg"
    with open(svg, "w") as f:
        f.write(glass_svg(radio))
    subprocess.run(["rsvg-convert", "-w", str(S), "-h", str(S), svg, "-o", tmp],
                   check=True)
    c, s = math.cos(t), math.sin(t)
    pairs = (f"{S / 2},{S / 2} {cx:.2f},{cy:.2f} "
             f"{S},{S / 2} {cx + a * c:.2f},{cy + a * s:.2f} "
             f"{S / 2},{S} {cx - b * s:.2f},{cy + b * c:.2f}")
    clear = out + ".cutout.png"
    cutout(photo, clear)
    subprocess.run(["magick", clear, "-resize", f"{W}x{H}!", "(", tmp,
                    "-virtual-pixel", "transparent", "-define",
                    f"distort:viewport={W}x{H}+0+0", "-distort", "Affine", pairs,
                    ")", "-composite", out], check=True)
    os.remove(tmp)
    os.remove(svg)
    os.remove(clear)
    print(f"{os.path.basename(out)}: glass at ({fit['cx']:.0f},{fit['cy']:.0f}) "
          f"{2 * fit['a']:.0f}x{2 * fit['b']:.0f} px, x{scale}")


def arg(flag, default=None):
    return sys.argv[sys.argv.index(flag) + 1] if flag in sys.argv else default


def main():
    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    root = arg("--out", os.path.join(here, "docs", "marketing"))
    radios = [arg("--radio")] if "--radio" in sys.argv else list(RADIOS)
    if "--glass" in sys.argv:
        # Just the face on its glass, for the README: docs/display-<radio>.svg.
        for radio in radios:
            D.use_palette(radio)
            path = os.path.join(here, "docs", f"display-{radio}.svg")
            with open(path, "w") as f:
                f.write(glass_svg(radio))
            print(f"{path}: {os.path.getsize(path)} bytes")
        return
    if "--photo" in sys.argv:
        # One radio's dial onto a photograph: --radio names it (aethersdr).
        radio = radios[0] if "--radio" in sys.argv else "aethersdr"
        D.use_palette(radio)
        out = os.path.join(root, radio)
        os.makedirs(out, exist_ok=True)
        photo = sys.argv[sys.argv.index("--photo") + 1]
        w, h = map(int, subprocess.check_output(
            ["magick", "identify", "-format", "%w %h", photo]).split())
        seed = (w // 2, h // 3)
        if "--seed" in sys.argv:
            seed = tuple(int(v) for v in sys.argv[sys.argv.index("--seed") + 1].split(","))
        scale = float(sys.argv[sys.argv.index("--scale") + 1]) if "--scale" in sys.argv else 1
        name = os.path.splitext(os.path.basename(photo))[0]
        on_photo(photo, seed, scale,
                 os.path.join(out, f"{name}-dial{'' if scale == 1 else f'@{scale:g}x'}.png"),
                 radio)
        return
    for radio in radios:
        D.use_palette(radio)
        out = os.path.join(root, radio)
        os.makedirs(out, exist_ok=True)
        for name, (view, what, shadow) in VIEWS.items():
            for suffix, finish in FINISHES.items():
                title = (f"VFO-Knob for {RADIOS[radio]['name']}"
                         f"{', black' if suffix else ''}, {what}")
                svg = render(view, title, radio, shadow, finish)
                path = os.path.join(out, name + suffix + ".svg")
                with open(path, "w") as f:
                    f.write(svg)
                subprocess.run(["rsvg-convert", "-z", "4", path, "-o",
                                os.path.join(out, name + suffix + ".png")], check=True)
                print(f"{radio}/{name}{suffix}: {len(svg)} bytes")


if __name__ == "__main__":
    main()
