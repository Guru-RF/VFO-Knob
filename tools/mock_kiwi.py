#!/usr/bin/env python3
"""Mock KiwiSDR / Web-888, for the owners' limits and the knob's session.

The knob's web-SDR code is tried here, never against someone's receiver: a
Kiwi bars an address for good after five logins it refused for its day limit
(and with it every browser behind that address), and polling its /status has
earned addresses a ban before. This mock keeps the same counts, answers as
the servers do, and says what it saw.

One port, as a Kiwi has:
  GET /status                  the receiver's status lines: uptime, date (its
                               clock), sdr_hw with the hourglass when it has
                               time limits, sw_version, freq_offset, name,
                               antenna, loc, and ext_api (KiwiSDR)
  GET /<ts>/SND                the WebSocket on the app path; also /kiwi/<ts>/SND.
                               A Web-888 takes /ws/kiwi/<ts>/SND and drops it
                               at once, as its server does; a KiwiSDR takes it
  GET /mock/stats              what it saw, as JSON: connections, logins, logins
                               refused for the day limit (per address), /status
                               reads, the barred addresses, who the last session
                               said it is (ident, decoded), the most WebSockets
                               it had open at once (max_open: the one a knob
                               leaves may linger here a moment) and how often
                               two stayed open together over half a second
                               (two_at_once), and the rest
  GET /mock/sets               every SET it was sent, keepalives too, with the
                               time (s since the mock started) and connection
  GET /mock/restart            as if it restarted: uptime from 0, its counts and
                               its listening minutes cleared (not its bars)
  GET /mock/advance?h=25       hours pass (its clock and its uptime); a KiwiSDR
                               clears its counts once a day, a Web-888 never
  GET /mock/set?limit=1&...    change a flag while it runs: limit (every address
                               over its minutes), tlimits, ip_limit_after,
                               idle_after, kick_after, silent_door, ext_api, http,
                               badp, too_busy, down, offset, station, dbm, snr,
                               answer_delay, ipl (the time-limit password),
                               no_status (/status answers 404), status_delay,
                               hide_hourglass, antenna, name, loc, ovl,
                               jitter (ms), stall (s: every session's stream held
                               up that long from now, then its backlog at once)
  GET /mock/say?msg=...        a MSG to every session now ("freq_offset=..." ...)
  GET /mock/reset              the stats and the SET record from nothing

The rules, as the servers keep them (KiwiSDR and RaspSDR rx_cmd.cpp,
rx_sound.cpp, stats.cpp):
  - Before its login has taken, a session may send only SET auth, keepalive
    and options: anything else gets it kicked (counted, "kicked_early").
  - More than four keepalives before any of mod, agc or AR OK is a hang, and
    it is kicked ("hangs").
  - The idle timer (--idle-after) runs from the login, and starts over on a
    tune that changes something and on SET inactivity_ack.
  - SET ident_user=<who> is URL-decoded, as the servers do, and listed: its
    owner sees who listens.

Login:
  --badp N              answer every login badp=N: 1 password, 2/4/7 not now,
                        3 not from this address, 5 one per address, 6 updating
  --password PW         the receiver's password: a login without it gets badp=1
  --chan-no-pwd N       its channels needing no password (said before badp)
  --rx-chans N          its channels (13 on a Web-888, 8 on a KiwiSDR)
  --too-busy WHAT       all: no channel free, too_busy=<rx_chans> at once and
                        closed; apps: the Web-888's app check (--app-check S
                        after the login, 10 s) finds its app channels taken,
                        too_busy=<ext_api>; none: it lets no apps in, too_busy=0
  --no-dup-ip           one session per address: a second gets badp=5
Limits:
  --ip-limit-at-login   every address starts over its day's minutes: a login
                        gets MSG ip_limit=<min>,<ip> and no badp, the refusal
                        is counted, and the 5th bars the address
  --ip-limit-after S    a session gets ip_limit S s in, and is closed; not
                        counted, but the address is over its minutes after it
  --idle-after S        MSG inactivity_timeout=<min> once nothing was tuned or
                        acknowledged for S s, then closed
  --kick-after S        MSG kiwi_kick=... S s in, then closed
  --no-tlimits          no hourglass in sdr_hw: no time limits
  --hide-hourglass      time limits, but no hourglass in sdr_hw: a server that
                        does not show them (another fork, an older version)
  --ipl PASS            the time-limit password, which exempts from the limit
Refusals:
  --silent-door         KiwiSDR 1.9's door for apps, shut: on the app path a
                        login gets no answer at all, the socket kept open
  --http CODE           the upgrade answered with this HTTP status, no
                        WebSocket: 403 a refusal, 404 not a Kiwi, 503 full
  --close-early         the WebSocket closed as soon as it is open: no answer
  --down                disabled by its owner: reason_disabled and down, closed
  --no-status           /status answers 404
  --status-delay S      /status answered this long after it is asked -- a slow
                        uplink; the request counted when it comes
  --answer-delay S      the login's answer this long after SET auth -- a slow
                        uplink or a busy receiver; the refusal, if it is one,
                        counted then, the knob still there or not
Stream:
  --rate HZ             its audio rate (12000; also 20250, 24000, 48000)
  --offset KHZ          its frequency offset: it tunes offset..offset+bandwidth
  --bandwidth HZ        what it says it covers (62 MHz Web-888, 30 MHz KiwiSDR);
                        0: never said
  --bw-delay S          ...said this long after the login
  --drift-ppm N         its clock this fast against the PC's: more frames
  --jitter MS           each frame up to MS late on its way (never out of order)
  --bunch N             the frames N at a time, as they come every N frames'
                        time: a way that hands them over in bunches, steadily
  --stall T:D           T s after the login the stream stops for D s, the
                        frames held by the network, then the backlog all at
                        once (repeatable); --stall-every S --stall-for D the
                        same every S s; --stall-drop: the receiver lets the
                        frames go instead -- their sequence numbers missing;
                        --catch-up PCT: the backlog not at once but PCT %
                        faster than the stream, until it has caught up (a
                        busy uplink draining the receiver's queue)
  --hold T              T s after the login one frame is held up a frame's
                        time, then sent with the next: a hiccup of the
                        network, exactly one frame late (repeatable)
  --no-cfg              without the three big settings messages (load_cfg,
                        load_dxcfg, load_dxcomm_cfg, ~20 KB each) a receiver
                        sends at every login
  --cw LO,HI            its owner's CW passband, in load_cfg's passbands --
                        after the rest of it, past the knob's 16 KB: read as
                        the frame goes by (none said: a KiwiSDR's 300,700)
  --dbm DBM             its S-meter (-73)
  --station HZ          a carrier there: heard at its offset from the tuned
                        carrier, inside the passband, silent outside it;
                        without it, a 1 kHz tone wherever it is tuned
  --snr DB              the signal over the noise: a squelch above it closes
                        (flag 0x40, the audio still sent, as the servers do)
  --ovl                 its ADC overloaded: flag 0x02 on every frame
Flavour and clock:
  --kiwisdr             the KiwiSDR flavour (the default is a Web-888)
  --antenna TEXT        its /status antenna= ("MOCK dummy load"; "" says none)
  --name TEXT           its /status name= ("MOCK | the knob's mock Web-888")
  --loc TEXT            its /status loc= ("On the bench")
  --ext-api N           a KiwiSDR's channels for apps, in its /status
  --uptime S            how long it has been up at the start (3 days)
  --restart             it has just started: --uptime 0
  --date-advance H      its clock this many hours ahead
Scripted:
  --say T:MSG           MSG to a session T s after its login (repeatable)
Behind a front -- the kiwisdr.com proxy, Cloudflare -- as such a receiver is
reached: TLS on the port, the receiver's own HTTP behind it, as a front ends
TLS and hands the request on (its client's address with it):
  --tls CERT:KEY        TLS on its port: this certificate chain and its key (PEM);
                        everything above is behind it. /mock/ is on a port of
                        its own, in the clear, never counted: "CONTROL <port>"
                        after LISTENING. /mock/stats counts the handshakes
                        (tls), those that resumed an earlier session
                        (tls_resumed) and those that failed (tls_failed, the
                        client refusing the certificate)
  --redirect CODE       a second port, in the clear, answering everything with
                        CODE (301, 302, 307, 308) to https:// on the host the
                        request named, this port's path kept: "REDIRECTING
                        <port>" after LISTENING; counted (redirects)
  --redirect-to URL     ...to URL and the path instead -- another host, say;
                        {port} in it is the receiver's own port
  --tls-delay S         the front's handshake held up S s: TLS in software, on
                        a slow line
  --http-location URL   --http's answer with this Location: a redirect from
                        the receiver itself ({port} its port)

Audio: IMA-ADPCM (or 16-bit PCM after SET compression=0), 2048 samples a
frame at --rate, with an S-meter of --dbm.

Usage: python3 tools/mock_kiwi.py [--host 0.0.0.0] [--port 8073] [flags...]
The port it listens on is printed first: "LISTENING <port>" (--port 0 picks one).
"""
import argparse
import base64
import hashlib
import itertools
import json
import math
import random
import select
import socket
import ssl
import struct
import sys
import threading
import time
from urllib.parse import parse_qs, quote, unquote, urlsplit

sys.dont_write_bytecode = True       # tools/__pycache__ is in git: leave it as it is
from ws_server import OP_BIN, OP_CLOSE, OP_PING, OP_PONG, OP_TEXT, WSConn, WSError

_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
HOURGLASS = "⏳"
BAR_AT = 5                       # refused logins that bar an address
FRAME = 2048                     # samples per SND frame
PRE_AUTH = ("SET auth", "SET keepalive", "SET options")
HANG_AT = 5                      # keepalives before any tune: a hang
TUNED = ("SET mod=", "SET agc=", "SET AR OK")

ARGS = None
R = None                         # the receiver's state
T0 = time.time()                 # the mock's own start, for the SET record
CONN_IDS = itertools.count(1)


def log(*a):
    t = time.time()
    print(f"[{time.strftime('%H:%M:%S', time.localtime(t))}.{int(t * 1000) % 1000:03d}]", *a, flush=True)


class Receiver:
    def __init__(self, a):
        self.lock = threading.Lock()
        self.kiwisdr = a.kiwisdr
        self.ahead = a.date_advance * 3600.0   # s its clock is ahead of the PC's
        self.boot = self.now() - (0 if a.restart else a.uptime)  # when it started, by its clock
        self.cleared = self.now()        # its clock when it last cleared its counts
        self.limit_all = a.ip_limit_at_login
        self.over = set()                # addresses over their day's minutes
        self.refused = {}                # address -> refusals counted toward a bar
        self.barred = set()
        self.tlimits = not a.no_tlimits
        self.ip_limit_after = a.ip_limit_after
        self.idle_after = a.idle_after
        self.kick_after = a.kick_after
        self.silent_door = a.silent_door
        self.ext_api = a.ext_api
        self.http = a.http
        self.close_early = a.close_early
        self.badp = a.badp
        self.too_busy = a.too_busy
        self.down = a.down
        self.answer_delay = a.answer_delay
        self.no_status = a.no_status
        self.status_delay = a.status_delay
        self.offset = a.offset
        self.station = a.station
        self.dbm = a.dbm
        self.snr = a.snr
        self.jitter = a.jitter
        self.antenna = a.antenna
        self.name = a.name if a.name is not None else \
            f"MOCK | the knob's mock {'KiwiSDR' if a.kiwisdr else 'Web-888'}"
        self.loc = a.loc
        self.ovl = a.ovl
        self.stall_until = 0.0           # time.time() a stall asked for now ends
        self.rx_chans = a.rx_chans or (8 if a.kiwisdr else 13)
        self.bandwidth = a.bandwidth if a.bandwidth >= 0 else (30000000 if a.kiwisdr else 62000000)
        self.sessions = {}               # conn id -> session state, logged in
        self.sets = []                   # every SET: (t, conn, ip, text)
        self.stats = {}
        self.reset_stats()

    def reset_stats(self):
        self.stats = dict(connections=0, status=0, upgrades=0, auths=0, logins=0,
                          refused_at_login=0, limits_mid=0, idles=0, kicks=0,
                          silent=0, barred_attempts=0, dropped_paths=0, sets=0, http_refusals=0,
                          kicked_early=0, hangs=0, acks=0, mods=0, mod_before_bw=0, badp=0,
                          too_busy=0, downs=0, dup_ip=0, frames=0, keepalives=0, app_paths=0,
                          browser_paths=0, stalls=0, held=0, let_go=0, cfgs=0, idents=0, ident=None,
                          max_open=0, two_at_once=0, tls=0, tls_resumed=0, tls_failed=0, redirects=0,
                          hosts=[])
        self.sets = []

    def now(self):
        return time.time() + self.ahead

    def uptime(self):
        return int(self.now() - self.boot)

    def is_over(self, addr):
        return self.limit_all or addr in self.over

    def clear_counts(self, why):
        self.refused.clear()
        self.over.clear()
        self.limit_all = False
        self.cleared = self.now()
        log(f"counts cleared: {why}")

    def status_text(self):
        shown = self.tlimits and not ARGS.hide_hourglass
        hg = f" ⁣ {HOURGLASS} " + ("Limits" if self.kiwisdr else "LIMITS") if shown else ""
        top = self.bandwidth or (30000000 if self.kiwisdr else 62000000)
        if self.kiwisdr:
            hw, sw, umax = "KiwiSDR 2 v1.902 ⁣ \U0001F4E1 GPS" + hg, "KiwiSDR_v1.902", 8
        else:
            hw, sw, umax = "Web-888 v2026.0901 ⁣ \U0001F4E1 GPS" + hg, "Web888_v2026.0901", 13
        lines = [
            "status=active", "offline=no",
            f"name={self.name}",
            f"sdr_hw={hw}", "op_email=", f"bands={int(self.offset * 1000)}-{int(self.offset * 1000) + top}",
            f"freq_offset={self.offset:.3f}",
            f"users={len(self.sessions)}", f"users_max={umax}", f"loc={self.loc}",
            f"sw_version={sw}", f"antenna={self.antenna}",
        ]
        if self.kiwisdr:
            lines.append(f"ext_api={self.ext_api}")
        lines += [f"uptime={self.uptime()}", "date=" + time.asctime(time.gmtime(self.now()))]
        return "\n".join(lines) + "\n"


# ------------------------------------------------------------------ ADPCM

STEP = [7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55,
        60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307,
        337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411,
        1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358,
        5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500,
        20350, 22385, 24623, 27086, 29794, 32767]
IDX = [-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8]


class Adpcm:
    """IMA-ADPCM as the Kiwi sends it: two samples a byte, the low nibble first."""
    def __init__(self):
        self.pred, self.idx = 0, 0

    def code(self, x):
        step = STEP[self.idx]
        diff = x - self.pred
        c = 0
        if diff < 0:
            c, diff = 8, -diff
        d = step >> 3
        if diff >= step:
            c |= 4; diff -= step; d += step
        if diff >= step >> 1:
            c |= 2; diff -= step >> 1; d += step >> 1
        if diff >= step >> 2:
            c |= 1; d += step >> 2
        self.pred = max(-32768, min(32767, self.pred - d if c & 8 else self.pred + d))
        self.idx = max(0, min(88, self.idx + IDX[c]))
        return c

    def encode(self, pcm):
        out = bytearray()
        for i in range(0, len(pcm), 2):
            lo = self.code(pcm[i])
            hi = self.code(pcm[i + 1]) if i + 1 < len(pcm) else 0
            out.append(lo | hi << 4)
        return bytes(out)


# --------------------------------------------------------------- sessions

def path_ok(path):
    """The app path and the browser paths, as each flavour takes them: True
    to serve it, False for a Web-888's drop of the browser path, None for no
    Kiwi path at all."""
    parts = [p for p in urlsplit(path).path.split("/") if p]
    if not parts or parts[-1] != "SND" or len(parts) < 2 or not parts[-2].isdigit():
        return None
    head = parts[:-2]
    if not head:
        return True                                  # /<ts>/SND: the app path
    if R.kiwisdr:
        return head in (["kiwi"], ["ws", "kiwi"], ["no_wf"], ["ws", "no_wf"])
    return True if head in (["kiwi"], ["no_wf"]) else False


def app_path(path):
    parts = [p for p in urlsplit(path).path.split("/") if p]
    return len(parts) == 2


LOWER = ("lsb", "lsn", "sal")


def tone_of(st):
    """The audio's pitch now, Hz, and whether anything is heard: the station's
    offset from the tuned carrier, inside the passband -- or, with no
    station, a 1 kHz tone wherever it is tuned."""
    with R.lock:
        station, offset = R.station, R.offset
    if not station:
        return 1000.0, True
    if st["mod"] is None:
        return 0.0, False
    mode, lo, hi, freq = st["mod"]
    car = (freq + offset) * 1000.0
    rel = station - car
    if not lo <= rel <= hi:
        return 0.0, False
    return abs(rel), True


def session(conn, addr, path):
    cid = next(CONN_IDS)
    st = {"compression": True, "logged_in": False, "t_in": 0.0, "t_use": 0.0, "closing": False,
          "kas": 0, "tuned": False, "mod": None, "bw_said": False, "first_mod": True,
          "squelch": 0, "nbfm": False, "silent": False, "said": set(), "holds": set()}

    def say(msg):
        conn.send(("MSG " + msg).encode(), OP_BIN)      # binary frames, as a Kiwi sends

    def kick(why, stat):
        with R.lock:
            R.stats[stat] += 1
        log(f"{addr}#{cid}: KICKED: {why}")
        st["closing"] = True
        conn.close()

    def stalled(el, now):
        """Whether the stream is held up now: a scripted stall, a periodic
        one, or one asked for with /mock/set?stall=."""
        with R.lock:
            if now < R.stall_until:
                return True
        if any(t <= el < t + d for t, d in ARGS.stall):
            return True
        return bool(ARGS.stall_every and el >= ARGS.stall_every and el % ARGS.stall_every < ARGS.stall_for)

    def streamer():
        enc = Adpcm()
        seq, phase = 0, 0.0
        rate = ARGS.rate
        bias = 127 if R.kiwisdr else 140
        held = []                                    # frames the network holds up in a stall
        bunch = []                                   # ...and those waiting for a bunch to fill
        t_next = time.monotonic()                    # the PC's steady clock: --drift-ppm is all the drift
        while not conn.closed and not st["closing"]:
            now = time.time()
            el = now - st["t_in"]
            with R.lock:
                ipla, idla, kika = R.ip_limit_after, R.idle_after, R.kick_after
                busy, ext, dbm, snr, ovl = R.too_busy, R.ext_api, R.dbm, R.snr, R.ovl
                drift, jitter = ARGS.drift_ppm, R.jitter
            if ipla and el >= ipla:
                with R.lock:
                    R.over.add(addr)
                    R.stats["limits_mid"] += 1
                log(f"{addr}#{cid}: ip_limit mid-session after {el:.0f} s (not counted)")
                say("ip_limit=" + quote(f"60,{addr}"))
                time.sleep(0.3)
                conn.close()
                return
            if idla and now - st["t_use"] >= idla:
                with R.lock:
                    R.stats["idles"] += 1
                log(f"{addr}#{cid}: inactivity_timeout, {now - st['t_use']:.1f} s idle")
                say(f"inactivity_timeout={max(1, int(idla // 60))}")
                time.sleep(0.3)
                conn.close()
                return
            if kika and el >= kika:
                with R.lock:
                    R.stats["kicks"] += 1
                log(f"{addr}#{cid}: kicked")
                say("kiwi_kick=" + ("0," if R.kiwisdr else "") + quote("Kicked by the mock"))
                time.sleep(0.3)
                conn.close()
                return
            # The Web-888's check, 10 s in: its channels for apps.
            if busy in ("apps", "none") and el >= ARGS.app_check:
                n = 0 if busy == "none" else (ext if ext > 0 else 4)
                with R.lock:
                    R.stats["too_busy"] += 1
                log(f"{addr}#{cid}: app check: too_busy={n}")
                say(f"too_busy={n}")
                time.sleep(0.3)
                conn.close()
                return
            for i, (t, msg) in enumerate(ARGS.say):
                if el >= t and i not in st["said"]:
                    st["said"].add(i)
                    log(f"{addr}#{cid}: says {msg}")
                    say(msg)
            if ARGS.bandwidth != 0 and not st["bw_said"] and el >= ARGS.bw_delay:
                with R.lock:
                    off, bw = R.offset, R.bandwidth
                say(f"center_freq={int(off * 1000 + bw // 2)} bandwidth={bw}")
                st["bw_said"] = True
            hz, heard = tone_of(st)
            pcm = []
            w = 2 * math.pi * hz / rate
            for _ in range(FRAME):
                pcm.append(int(3000 * math.sin(phase)) if heard else 0)
                phase = (phase + w) % (2 * math.pi)
            flags = 0x02 if ovl else 0           # its ADC overloaded
            sq = st["squelch"]
            if sq and sq > snr:
                flags |= 0x40                     # closed: the audio still comes
            if st["compression"]:
                flags |= 0x10
                data = enc.encode(pcm)
            else:
                data = struct.pack(f">{FRAME}h", *pcm)
            sm = max(0, min(65535, int(round((dbm + bias) * 10))))
            frame = b"SND" + struct.pack("<BI", flags, seq) + struct.pack(">H", sm) + data
            # A hiccup: this one frame held, and sent with the next.
            hiccup = next((i for i, t in enumerate(ARGS.hold) if el >= t and i not in st["holds"]), None)
            if hiccup is not None:
                st["holds"].add(hiccup)
            if hiccup is not None or stalled(el, now):
                # Held up on its way -- or, --stall-drop, let go by the
                # receiver, its sequence number never seen.
                with R.lock:
                    if not held and not st.get("in_stall"):
                        R.stats["stalls"] += 1
                    R.stats["let_go" if ARGS.stall_drop else "held"] += 1
                st["in_stall"] = True
                if not ARGS.stall_drop:
                    held.append(frame)
            elif ARGS.catch_up and held:
                # Caught up slowly: this frame behind the backlog, and the
                # backlog going a little faster than the stream.
                if st.get("in_stall"):
                    log(f"{addr}#{cid}: stall over, {len(held)} frames to catch up at +{ARGS.catch_up:g} %")
                    st["in_stall"] = False
                held.append(frame)
                st["credit"] = st.get("credit", 0.0) + 1 + ARGS.catch_up / 100.0
                while st["credit"] >= 1 and held:
                    conn.send(held.pop(0), OP_BIN)
                    st["credit"] -= 1
                if not held:
                    st["credit"] = 0.0
                    log(f"{addr}#{cid}: caught up")
            else:
                if st.get("in_stall"):
                    log(f"{addr}#{cid}: stall over, {len(held)} frames at once")
                    st["in_stall"] = False
                for f in held:
                    conn.send(f, OP_BIN)
                held = []
                bunch.append(frame)
                if len(bunch) >= ARGS.bunch:
                    if jitter:
                        time.sleep(random.uniform(0, jitter / 1000.0))
                    for f in bunch:
                        conn.send(f, OP_BIN)
                    bunch = []
            with R.lock:
                R.stats["frames"] += 1
            seq += 1
            t_next += FRAME / (rate * (1 + drift / 1e6))
            time.sleep(max(0.0, t_next - time.monotonic()))

    def login(args):
        with R.lock:
            delay = R.answer_delay
        if delay:
            time.sleep(delay)                        # the answer on its way, slowly
        with R.lock:
            R.stats["auths"] += 1
            exempt = bool(ARGS.ipl) and args.get("ipl") == ARGS.ipl
            over = R.is_over(addr) and not exempt and R.tlimits
            door = R.silent_door and R.kiwisdr and app_path(path)
            badp = R.badp
            dup = ARGS.no_dup_ip and any(s["addr"] == addr for s in R.sessions.values())
        if over:
            with R.lock:
                R.refused[addr] = R.refused.get(addr, 0) + 1
                R.stats["refused_at_login"] += 1
                k = R.refused[addr]
                if k >= BAR_AT:
                    R.barred.add(addr)
            log(f"{addr}#{cid}: login REFUSED for its day limit ({k} of {BAR_AT})"
                + (" -- ADDRESS BARRED" if k >= BAR_AT else ""))
            say("ip_limit=" + quote(f"60,{addr}"))
            time.sleep(0.5)
            conn.close()
            return
        if door:
            with R.lock:
                R.stats["silent"] += 1
            st["silent"] = True
            log(f"{addr}#{cid}: app door shut: no answer")
            return                                   # nothing said, the socket kept
        if badp < 0 and ARGS.password and args.get("p") != ARGS.password:
            badp = 1
        if badp < 0 and dup:
            badp = 5
        say(f"rx_chans={R.rx_chans}")
        say(f"chan_no_pwd={ARGS.chan_no_pwd}")
        say("is_local=0 max_camp=0")
        if badp > 0:
            with R.lock:
                R.stats["badp"] += 1
                if badp == 5:
                    R.stats["dup_ip"] += 1
            log(f"{addr}#{cid}: login refused, badp={badp}")
            say(f"badp={badp}")
            time.sleep(0.3)
            conn.close()
            return
        with R.lock:
            R.stats["logins"] += 1
            off = R.offset
            R.sessions[cid] = {"addr": addr}
        log(f"{addr}#{cid}: logged in")
        say("badp=0")
        if R.kiwisdr:
            say(f"version_maj=1 version_min=902 freq_offset={off:.3f}")
        else:
            say("version_maj=2026 version_min=901")
            say(f"freq_offset={off:.3f}")
        say(f"audio_init=0 audio_rate={int(ARGS.rate)} sample_rate={ARGS.rate:.3f}")
        if not ARGS.no_cfg:
            # Its settings, as a Kiwi sends them at every login: URL-encoded
            # JSON, each well over the knob's 16 KB buffer.
            for key in ("load_cfg", "load_dxcfg", "load_dxcomm_cfg"):
                body = {"ext_api_nchans": R.ext_api, "rx_chans": R.rx_chans, "pad": "x" * 20000} \
                    if key == "load_cfg" else {"dx": ["y" * 64] * 300}
                if key == "load_cfg" and ARGS.cw:
                    body["passbands"] = {"cw": {"lo": ARGS.cw[0], "hi": ARGS.cw[1]}}
                enc_ = quote(json.dumps(body)).replace("%3A", "%3a").replace("%2C", "%2c")
                say(f"{key}={enc_}")
                with R.lock:
                    R.stats["cfgs"] += 1
        st["logged_in"] = True
        st["t_in"] = st["t_use"] = time.time()
        threading.Thread(target=streamer, daemon=True).start()

    def on_set(text):
        """One SET, as the receiver takes it."""
        now = time.time()
        with R.lock:
            R.stats["sets"] += 1
            R.sets.append((round(now - T0, 3), cid, addr, text))
        if not text.startswith("SET keepalive") or ARGS.verbose:
            log(f"{addr}#{cid} -> {text if not text.startswith('SET auth') else 'SET auth ...'}")
        if st["silent"]:
            return                                   # no session behind the door: nothing heard
        if not st["logged_in"]:
            if text.startswith("SET auth"):
                args = dict(kv.split("=", 1) for kv in text.split()[2:] if "=" in kv)
                login(args)
            elif not text.startswith(PRE_AUTH):
                kick(f"\"{text}\" before the login", "kicked_early")
            return
        if text.startswith("SET keepalive"):
            with R.lock:
                R.stats["keepalives"] += 1
            if not st["tuned"]:
                st["kas"] += 1
                if st["kas"] >= HANG_AT:
                    kick(f"{st['kas']} keepalives and no tune: hung", "hangs")
            return
        if text.startswith(TUNED):
            st["tuned"] = True
        if text.startswith("SET mod="):
            kv = dict(x.split("=", 1) for x in text.split()[1:] if "=" in x)
            try:
                mod = (kv["mod"], float(kv["low_cut"]), float(kv["high_cut"]), float(kv["freq"]))
            except (KeyError, ValueError):
                kick(f"\"{text}\": bad params", "kicked_early")
                return
            with R.lock:
                R.stats["mods"] += 1
                if st["first_mod"] and not st["bw_said"] and ARGS.bandwidth != 0:
                    R.stats["mod_before_bw"] += 1
            st["first_mod"] = False
            if mod != st["mod"]:
                st["t_use"] = now                    # a tune: the idle timer starts over
            st["mod"] = mod
            st["nbfm"] = mod[0] in ("nbfm", "nnfm")
        elif text.startswith("SET inactivity_ack"):
            with R.lock:
                R.stats["acks"] += 1
            st["t_use"] = now
        elif text.startswith("SET ident_user="):
            who = unquote(text.split("=", 1)[1])
            with R.lock:
                R.stats["idents"] += 1
                R.stats["ident"] = who
            log(f"{addr}#{cid}: its listener is \"{who}\"")
        elif text.startswith("SET compression="):
            st["compression"] = text.split("=", 1)[1].strip() != "0"
        elif text.startswith("SET squelch="):
            try:
                st["squelch"] = float(text.split()[1].split("=", 1)[1])
            except (IndexError, ValueError):
                pass

    try:
        # On until the knob's end is read: a send that failed (the knob gone
        # mid-login, its close on the way) leaves what it sent before it to
        # be read -- a SET ident_user after the login, say.
        while True:
            op, payload = conn.recv_frame()
            if op == OP_CLOSE:
                break
            if op == OP_PING:
                conn.send(payload, OP_PONG)
                continue
            if op != OP_TEXT:
                continue
            on_set(payload.decode("utf-8", "replace"))
    except (WSError, OSError):
        pass
    finally:
        st["closing"] = True
        conn.closed = True
        with R.lock:
            R.sessions.pop(cid, None)
        try:
            conn.sock.close()
        except OSError:
            pass


# ------------------------------------------------------------------- HTTP

def reply(sock, code, body, ctype="text/plain", location=""):
    if isinstance(body, str):
        body = body.encode()
    reason = {200: "OK", 301: "Moved Permanently", 302: "Found", 307: "Temporary Redirect",
              308: "Permanent Redirect", 400: "Bad Request", 403: "Forbidden", 404: "Not Found",
              429: "Too Many Requests", 503: "Service Unavailable"}.get(code, "")
    where = f"Location: {location}\r\n" if location else ""
    sock.sendall(f"HTTP/1.1 {code} {reason}\r\n{where}Content-Type: {ctype}\r\n"
                 f"Content-Length: {len(body)}\r\nConnection: close\r\n\r\n".encode() + body)


SAY = []                         # (conn, ...) live sessions, for /mock/say


def control(sock, url):
    q = {k: v[-1] for k, v in parse_qs(url.query, keep_blank_values=True).items()}
    if url.path == "/mock/sets":
        with R.lock:
            out = [dict(t=t, c=c, ip=ip, text=text) for t, c, ip, text in R.sets]
        return reply(sock, 200, json.dumps(out), "application/json")
    if url.path == "/mock/say":
        msg = q.get("msg", "")
        with R.lock:
            conns = list(SAY)
        for c in conns:
            if not c.closed:
                c.send(("MSG " + msg).encode(), OP_BIN)
        log(f"says to {len(conns)} session(s): {msg}")
    with R.lock:
        if url.path == "/mock/restart":
            R.boot = R.now()
            R.clear_counts("it restarted")
        elif url.path == "/mock/advance":
            R.ahead += float(q.get("h", "0")) * 3600
            log(f"its clock is now {time.asctime(time.gmtime(R.now()))}")
            if R.kiwisdr and R.now() - R.cleared >= 86400:
                R.clear_counts("a KiwiSDR's daily clear")
        elif url.path == "/mock/set":
            for k, v in q.items():
                if k == "limit":
                    R.limit_all = v not in ("0", "")
                    if not R.limit_all:
                        R.over.clear()
                elif k in ("tlimits", "silent_door", "down"):
                    setattr(R, k, v not in ("0", ""))
                elif k in ("ext_api", "http", "badp"):
                    setattr(R, k, int(v))
                elif k == "too_busy":
                    R.too_busy = v
                elif k in ("ip_limit_after", "idle_after", "kick_after", "offset", "station", "dbm", "snr",
                           "answer_delay", "jitter", "status_delay"):
                    setattr(R, k, float(v))
                elif k == "stall":
                    R.stall_until = time.time() + float(v)
                elif k == "ipl":
                    ARGS.ipl = v
                elif k == "no_status":
                    R.no_status = v not in ("0", "")
                elif k in ("antenna", "name", "loc"):
                    setattr(R, k, v)
                elif k == "ovl":
                    R.ovl = v not in ("0", "")
                elif k == "hide_hourglass":
                    ARGS.hide_hourglass = v not in ("0", "")
            log(f"set {q}")
        elif url.path == "/mock/reset":
            R.reset_stats()
        out = dict(R.stats, refused=R.refused, barred=sorted(R.barred), uptime=R.uptime(),
                   date=time.asctime(time.gmtime(R.now())), limit_all=R.limit_all,
                   over=sorted(R.over), live=len(R.sessions))
    reply(sock, 200, json.dumps(out), "application/json")


def control_client(sock, addr):
    """The --tls mock's /mock/ port: the test's own requests, in the clear
    and counted nowhere."""
    try:
        sock.settimeout(10)
        data = b""
        while b"\r\n\r\n" not in data and len(data) < 16384:
            chunk = sock.recv(4096)
            if not chunk:
                return sock.close()
            data += chunk
        target = (data.split(b"\r\n", 1)[0].decode("latin-1").split() + ["", ""])[1]
        url = urlsplit(target)
        if url.path.startswith("/mock/"):
            control(sock, url)
        else:
            reply(sock, 404, "the receiver is on the other port")
    except OSError:
        pass
    try:
        sock.close()
    except OSError:
        pass


def on_client(sock, addr):
    ip = addr[0]
    with R.lock:
        R.stats["connections"] += 1
        barred = ip in R.barred
        if barred:
            R.stats["barred_attempts"] += 1
    if barred:
        log(f"{ip}: barred, dropped")
        sock.close()
        return
    try:
        sock.settimeout(10)
        data = b""
        while b"\r\n\r\n" not in data:
            chunk = sock.recv(4096)
            if not chunk:
                return sock.close()
            data += chunk
            if len(data) > 16384:
                return sock.close()
        head = data.split(b"\r\n\r\n", 1)[0].decode("latin-1")
        lines = head.split("\r\n")
        method, target = (lines[0].split() + ["", ""])[:2]
        hdr = {}
        for line in lines[1:]:
            if ":" in line:
                k, v = line.split(":", 1)
                hdr[k.strip().lower()] = v.strip()
        url = urlsplit(target)
        with R.lock:
            http = R.http
            if not url.path.startswith("/mock/"):
                R.stats["hosts"] = (R.stats["hosts"] + [hdr.get("host", "")])[-20:]
        if http and url.path not in ("/status",) and not url.path.startswith("/mock/"):
            with R.lock:
                R.stats["http_refusals"] = R.stats.get("http_refusals", 0) + 1
            log(f"{ip}: {url.path}: HTTP {http}")
            reply(sock, http, "no", location=ARGS.http_location.format(port=PORT) if ARGS.http_location else "")
            return sock.close()
        if url.path == "/status":
            with R.lock:
                R.stats["status"] += 1
                text = R.status_text()
            with R.lock:
                no_status, delay = R.no_status, R.status_delay
            log(f"{ip}: /status" + (" -- 404" if no_status else "") + (f", answered in {delay:g} s" if delay else ""))
            if delay:
                time.sleep(delay)
            if no_status:
                reply(sock, 404, "not here")
            else:
                reply(sock, 200, text)
            return sock.close()
        if url.path.startswith("/mock/"):
            control(sock, url)
            return sock.close()
        ok = path_ok(target)
        key = hdr.get("sec-websocket-key")
        if ok is None or not key or hdr.get("upgrade", "").lower() != "websocket":
            reply(sock, 404, "not here")
            return sock.close()
        with R.lock:
            R.stats["app_paths" if app_path(target) else "browser_paths"] += 1
        accept = base64.b64encode(hashlib.sha1((key + _GUID).encode()).digest()).decode()
        sock.sendall(("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                      f"Connection: Upgrade\r\nSec-WebSocket-Accept: {accept}\r\n\r\n").encode())
        with R.lock:
            R.stats["upgrades"] += 1
            early, down, busy = R.close_early, R.down, R.too_busy
        if early:
            log(f"{ip}: closed at once")
            return sock.close()
        if ok is False:
            with R.lock:
                R.stats["dropped_paths"] += 1
            log(f"{ip}: {url.path}: not its path, dropped after the upgrade")
            return sock.close()
        sock.settimeout(None)
        conn = WSConn(sock, addr)
        # Before any login: disabled by its owner, or every channel taken.
        if down or busy == "all":
            if down:
                with R.lock:
                    R.stats["downs"] += 1
                log(f"{ip}: down")
                conn.send(b"MSG reason_disabled=" + quote("down for the mock").encode(), OP_BIN)
                conn.send(b"MSG down", OP_BIN)
            else:
                with R.lock:
                    R.stats["too_busy"] += 1
                log(f"{ip}: no channel free: too_busy={R.rx_chans}")
                conn.send(f"MSG too_busy={R.rx_chans}".encode(), OP_BIN)
            time.sleep(0.3)
            return conn.close()
        with R.lock:
            SAY.append(conn)
            R.stats["max_open"] = max(R.stats["max_open"], len(SAY))
            others = [o for o in SAY if o is not conn]
        if others:
            # Open beside another: a session the knob is leaving closes in a
            # moment -- this thread just has not seen it yet. Still both open
            # half a second on, it is two at once.
            def overlap(new=conn, old=others):
                time.sleep(0.5)
                with R.lock:
                    if new in SAY and any(o in SAY for o in old):
                        R.stats["two_at_once"] += 1
                        log(f"{ip}: two sessions open at once")
            threading.Thread(target=overlap, daemon=True).start()
        try:
            session(conn, ip, target)
        finally:
            with R.lock:
                if conn in SAY:
                    SAY.remove(conn)
    except (OSError, WSError):
        try:
            sock.close()
        except OSError:
            pass


# ------------------------------------------------------------------ fronts

TLS_CTX = None                   # --tls: the front's
PORT = 0                         # the receiver's own port, as it listens


def pump(t, a):
    """Bytes both ways between the client's TLS and the receiver behind it,
    in one thread: an SSL socket takes no reads and writes from two at once.
    What TLS holds decrypted is read before select() is asked. A write to the
    client that fails -- the client gone, its reset on the way -- leaves what
    it sent before it to be read and handed on all the same, as a front
    reading both ways at once does: a SET ident_user right after a Test's
    login, the Test closing as the stream comes."""
    try:
        while True:
            ready = [t] if t.pending() else select.select([t, a], [], [])[0]
            if t in ready:
                data = t.recv(65536)
                if not data:
                    break
                a.sendall(data)
            if a in ready:
                data = a.recv(65536)
                if not data:
                    break
                try:
                    t.sendall(data)
                except (OSError, ssl.SSLError):
                    t.settimeout(1)
                    while True:
                        data = t.recv(65536)
                        if not data:
                            break
                        a.sendall(data)
                    break
    except (OSError, ssl.SSLError):
        pass
    finally:
        for x in (t, a):
            try:
                x.close()
            except OSError:
                pass


def front(raw, addr):
    """A front, as a receiver behind the kiwisdr.com proxy or Cloudflare is
    met: TLS ended here, the receiver's own HTTP behind it -- handed on with
    the client's address, as a front says it."""
    if ARGS.tls_delay:
        time.sleep(ARGS.tls_delay)                   # its ServerHello, slow in coming
    try:
        raw.settimeout(15)
        t = TLS_CTX.wrap_socket(raw, server_side=True)
        t.settimeout(None)
    except (ssl.SSLError, OSError) as e:
        with R.lock:
            R.stats["tls_failed"] += 1
        log(f"{addr[0]}: TLS refused: {e}")
        try:
            raw.close()
        except OSError:
            pass
        return
    with R.lock:
        R.stats["tls"] += 1
        if t.session_reused:
            R.stats["tls_resumed"] += 1
    log(f"{addr[0]}: TLS {t.version()} {t.cipher()[0]}" + (", resumed" if t.session_reused else ""))
    a, b = socket.socketpair()
    threading.Thread(target=on_client, args=(b, addr), daemon=True).start()
    pump(t, a)


def redirector(sock, addr):
    """The second port, in the clear: every request answered with a
    redirect to https:// -- on the host the request named, or --redirect-to."""
    try:
        sock.settimeout(10)
        data = b""
        while b"\r\n\r\n" not in data and len(data) < 16384:
            chunk = sock.recv(4096)
            if not chunk:
                return sock.close()
            data += chunk
        lines = data.split(b"\r\n\r\n", 1)[0].decode("latin-1").split("\r\n")
        target = (lines[0].split() + ["", ""])[1] or "/"
        host = next((l.split(":", 1)[1].strip() for l in lines[1:] if l.lower().startswith("host:")), "127.0.0.1")
        host = host.rsplit(":", 1)[0] if host.count(":") == 1 else host
        base = ARGS.redirect_to.format(port=PORT) if ARGS.redirect_to else f"https://{host}:{PORT}"
        loc = base.rstrip("/") + target
        with R.lock:
            R.stats["redirects"] += 1
        log(f"{addr[0]}: {target} -- {ARGS.redirect} to {loc}")
        reason = {301: "Moved Permanently", 302: "Found", 307: "Temporary Redirect", 308: "Permanent Redirect"}
        body = b"<html>moved</html>"
        sock.sendall(f"HTTP/1.1 {ARGS.redirect} {reason.get(ARGS.redirect, 'Moved')}\r\nLocation: {loc}\r\n"
                     f"Content-Type: text/html\r\nContent-Length: {len(body)}\r\nConnection: close\r\n\r\n"
                     .encode() + body)
    except OSError:
        pass
    try:
        sock.close()
    except OSError:
        pass


def serve(srv, handler):
    while True:
        sock, addr = srv.accept()
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        threading.Thread(target=handler, args=(sock, addr), daemon=True).start()


def said_at(s):
    t, _, msg = s.partition(":")
    return float(t), msg


def said_at_f(s):
    t, _, d = s.partition(":")
    return float(t), float(d)


def main():
    global ARGS, R
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8073)
    ap.add_argument("--kiwisdr", action="store_true")
    ap.add_argument("--ext-api", type=int, default=4)
    ap.add_argument("--badp", type=int, default=-1)
    ap.add_argument("--password", default="")
    ap.add_argument("--chan-no-pwd", type=int, default=0)
    ap.add_argument("--rx-chans", type=int, default=0)
    ap.add_argument("--too-busy", choices=("all", "apps", "none"), default="")
    ap.add_argument("--app-check", type=float, default=10.0)
    ap.add_argument("--no-dup-ip", action="store_true")
    ap.add_argument("--ip-limit-at-login", action="store_true")
    ap.add_argument("--ip-limit-after", type=float, default=0)
    ap.add_argument("--idle-after", type=float, default=0)
    ap.add_argument("--kick-after", type=float, default=0)
    ap.add_argument("--no-tlimits", action="store_true")
    ap.add_argument("--silent-door", action="store_true")
    ap.add_argument("--ipl", default="")
    ap.add_argument("--http", type=int, default=0)
    ap.add_argument("--close-early", action="store_true")
    ap.add_argument("--down", action="store_true")
    ap.add_argument("--no-status", action="store_true")
    ap.add_argument("--status-delay", type=float, default=0.0)
    ap.add_argument("--answer-delay", type=float, default=0.0)
    ap.add_argument("--hide-hourglass", action="store_true")
    ap.add_argument("--rate", type=float, default=12000.0)
    ap.add_argument("--offset", type=float, default=0.0, help="kHz")
    ap.add_argument("--bandwidth", type=int, default=-1, help="Hz; 0: never said")
    ap.add_argument("--bw-delay", type=float, default=0.0)
    ap.add_argument("--drift-ppm", type=float, default=0.0)
    ap.add_argument("--jitter", type=float, default=0.0, help="ms")
    ap.add_argument("--bunch", type=int, default=1)
    ap.add_argument("--stall", type=said_at_f, action="append", default=[], metavar="T:D")
    ap.add_argument("--stall-every", type=float, default=0.0)
    ap.add_argument("--stall-for", type=float, default=0.0)
    ap.add_argument("--stall-drop", action="store_true")
    ap.add_argument("--catch-up", type=float, default=0.0, help="%%")
    ap.add_argument("--hold", type=float, action="append", default=[], metavar="T")
    ap.add_argument("--no-cfg", action="store_true")
    ap.add_argument("--cw", type=lambda v: tuple(int(x) for x in v.split(",")), default=None, metavar="LO,HI")
    ap.add_argument("--dbm", type=float, default=-73.0)
    ap.add_argument("--station", type=float, default=0.0, help="Hz")
    ap.add_argument("--snr", type=float, default=20.0)
    ap.add_argument("--ovl", action="store_true")
    ap.add_argument("--antenna", default="MOCK dummy load")
    ap.add_argument("--name", default=None)
    ap.add_argument("--loc", default="On the bench")
    ap.add_argument("--uptime", type=float, default=3 * 86400, help="s it has been up at the start")
    ap.add_argument("--restart", action="store_true")
    ap.add_argument("--date-advance", type=float, default=0.0, help="hours")
    ap.add_argument("--say", type=said_at, action="append", default=[], metavar="T:MSG")
    ap.add_argument("--tls", default="", metavar="CERT:KEY")
    ap.add_argument("--redirect", type=int, default=0, metavar="CODE")
    ap.add_argument("--redirect-to", default="", metavar="URL")
    ap.add_argument("--tls-delay", type=float, default=0.0, metavar="S")
    ap.add_argument("--http-location", default="", metavar="URL")
    ap.add_argument("-v", "--verbose", action="store_true")
    ARGS = ap.parse_args()
    if not 4000 <= ARGS.rate <= 50000:
        ap.error("--rate: 4000 to 50000 Hz, as a Kiwi has")
    global TLS_CTX, PORT
    if ARGS.tls:
        cert, _, key = ARGS.tls.partition(":")
        TLS_CTX = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        TLS_CTX.load_cert_chain(cert, key or None)
    R = Receiver(ARGS)
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((ARGS.host, ARGS.port))
    srv.listen(16)
    PORT = srv.getsockname()[1]
    print(f"LISTENING {PORT}", flush=True)
    if TLS_CTX:
        ctl = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        ctl.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        ctl.bind((ARGS.host, 0))
        ctl.listen(16)
        print(f"CONTROL {ctl.getsockname()[1]}", flush=True)
        threading.Thread(target=serve, args=(ctl, control_client), daemon=True).start()
    if ARGS.redirect:
        red = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        red.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        red.bind((ARGS.host, 0))
        red.listen(16)
        print(f"REDIRECTING {red.getsockname()[1]}", flush=True)
        threading.Thread(target=serve, args=(red, redirector), daemon=True).start()
    log(f"a mock {'KiwiSDR' if ARGS.kiwisdr else 'Web-888'} on {ARGS.host}:{PORT}"
        + (" behind TLS" if TLS_CTX else "")
        + (", every address over its day limit" if ARGS.ip_limit_at_login else ""))
    try:
        serve(srv, front if TLS_CTX else on_client)
    except KeyboardInterrupt:
        with R.lock:
            log(f"stats: {json.dumps(dict(R.stats, refused=R.refused, barred=sorted(R.barred)))}")


if __name__ == "__main__":
    sys.exit(main())
