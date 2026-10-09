#!/usr/bin/env python3
"""Mock OpenWebRX / OpenWebRX+ receiver, for the knob's session.

The knob's OpenWebRX code is tried here, never against someone's receiver: a
band change retunes the SDR for everyone listening, and OpenWebRX+ bans an
address for twelve hours after three quick ones. This mock keeps the
servers' rules as their source has them (OWRX-PROTOCOL.md gives the lines),
answers as they do, and says what it saw.

One port, as a receiver has (under --prefix, as one behind a front is):
  GET <prefix>status.json      what the receiver says of itself: its name,
                               version, and each SDR's profiles with where
                               they are -- read, counted (status)
  GET <prefix>ws/              the WebSocket; <prefix>ws without the slash is
                               404, as the servers have it (ws_no_slash)
  GET <prefix>                 a page
  GET /mock/stats              what it saw, as JSON: connections, sessions,
                               refusals, bans, band changes and the gaps
                               between them, starts, keys it did not know,
                               values of the wrong type, setfrequency and
                               setsdr (never sent by the knob), the most
                               sessions open at once and how often two stayed
                               open together over half a second, waterfall
                               frames sent and whether a session fell 100
                               behind (fft_overflow), audio frames and bytes
  GET /mock/sent               every message the sessions sent, with the time
                               (s since the mock started) and connection
  GET /mock/select?profile=S|P another listener changes band: every session on
                               that SDR gets the new config, its audio stops
                               until it says start again
  GET /mock/set?k=v&...        change a flag while it runs: max_clients, others
                               (listeners besides the sessions), ban=IP,
                               unban=IP, no_sdr, silent, locked (a profile id),
                               audio (adpcm, none), fft (adpcm, none), station
                               (Hz), stall (s: every session's audio held up
                               that long from now, then its backlog at once),
                               close (every session closed now), http, no_status
  GET /mock/reset              the stats and the record from nothing

The rules, as the servers keep them:
  - Nothing is answered before "SERVER DE CLIENT ... type=receiver": not a
    word to a wrong handshake (bad_handshakes).
  - A key the fork's DSP does not know aborts the rest of its message, and
    in connectionproperties it stays and fails again at every SDR restart
    (unknown_keys, poisoned); offset_freq must be an integer (bad_values).
  - No audio and no S-meter until {"type":"dspcontrol","action":"start"},
    and again after any band change, the knob's or another listener's.
  - SAM on upstream stops the audio (demod_errors).
  - The waterfall cannot be declined: --fft-fps frames a second through a
    queue of 100 -- a client that falls behind it is closed (fft_overflow).
  - OpenWebRX+: each band change adds 10 - (s since the last, or since the
    session started) to the session's score, a gap over 10 s clears it, and
    at 30 the address is banned for 12 hours, every session from it closed;
    at connect the score starts at the sum over the address's live sessions
    of 10 - their age, and at 30 the session is left silent after
    receiver_details (its server's own error) -- silent; a banned address
    is answered "Client address banned"; a key_locked profile is refused
    without its key ("This profile is locked, keeping current profile.").

Flags:
  --fork plus|upstream|upstream-1.2.2|upstream-1.0
                        OpenWebRX+ 1.2.126 (default), OpenWebRX 1.3.0-dev,
                        1.2.2, or 1.0.0: as 1.2.2, its audio without SYNCs
                        (csdr's encode_ima_adpcm_i16_u8, 1.0 and 1.1)
  --prefix /OWRX/       behind a front that passes this path on: anything
                        else is 404
  --max-clients N       its max_clients (20): "Too many clients" past it
  --others N            listeners besides the mock's sessions
  --banned              the client's address banned from the start
  --silent              every session left silent after receiver_details
  --no-sdr              no SDR runs: sdr_error at connect
  --locked PID          a key_locked profile (OpenWebRX+)
  --audio adpcm|none    its audio_compression
  --fft adpcm|none      its fft_compression
  --fft-size N          its fft_size (4096); --fft-fps N its waterfall's rate (9)
  --bookmarks-kb K      its bookmarks message this big (3; 300 for EIBI's)
  --photo-kb K          its receiver_details' photo_desc this big
  --profiles N          N more profiles on a third SDR (more than the knob keeps)
  --start SDR|PID       a session starts on this band (its SDR the first)
  --station HZ          a carrier there, besides the mock's own (repeatable)
  --station-cycle ON:OFF  the carriers there ON s, gone OFF s, over and over:
                        a repeater's overs, for a squelch to open and close
  --drift-ppm N         its clock this fast against the PC's: more audio
  --stall T:D           T s after start the audio stops for D s, then its
                        backlog at once (repeatable)
  --close-after S       every session closed S s after it starts playing
  --http CODE           the upgrade answered with this HTTP status
  --no-status           status.json answers 404
  --status-delay S      ...answered this long after it is asked
Behind a front, as receivers on https are:
  --tls CERT:KEY        TLS on its port; /mock/ on a port of its own in the
                        clear: "CONTROL <port>" after LISTENING
  --redirect CODE       a second port in the clear, redirecting everything to
                        https:// on the host asked for: "REDIRECTING <port>"

Usage: python3 tools/mock_owrx.py [--host 127.0.0.1] [--port 8073] [flags...]
The port it listens on is printed first: "LISTENING <port>" (--port 0 picks one).
"""
import argparse
import base64
import collections
import hashlib
import itertools
import json
import math
import queue
import random
import re
import select
import socket
import ssl
import struct
import sys
import threading
import time
from urllib.parse import parse_qs, urlsplit

sys.dont_write_bytecode = True       # tools/__pycache__ is in git: leave it as it is
from owrx_adpcm import Encoder, fft_line
from ws_server import OP_BIN, OP_CLOSE, OP_PING, OP_PONG, OP_TEXT, WSConn, WSError

_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
FORKS = {
    "plus": "v1.2.126",
    "upstream": "v1.3.0-dev",
    "upstream-1.2.2": "v1.2.2",
    "upstream-1.0": "v1.0.0",
}
BAN_MIN = 12 * 60
ARGS = None
R = None
T0 = time.time()
CONN_IDS = itertools.count(1)


def log(*a):
    t = time.time()
    print(f"[{time.strftime('%H:%M:%S', time.localtime(t))}.{int(t * 1000) % 1000:03d}]", *a, flush=True)


def dumps(o):
    """As the servers write every message: json.dumps' own separators,
    non-ASCII as \\u escapes, no NaN."""
    return json.dumps(o, allow_nan=False)


# --------------------------------------------------------------- its SDRs

class Sdr:
    def __init__(self, sid, name, profiles):
        self.id, self.name = sid, name
        self.profiles = collections.OrderedDict()
        for pid, pname, center, rate, start, mod in profiles:
            self.profiles[pid] = dict(name=pname, center_freq=center, samp_rate=rate,
                                      start_freq=start, start_mod=mod)
        self.current = next(iter(self.profiles))
        self.generation = 0              # each band change: every DSP on it detached

    def props(self):
        p = self.profiles[self.current]
        return dict(p, profile_id=self.current)


def make_sdrs(extra):
    sdrs = [
        Sdr("rspdx", "RSPdx", [
            ("40m", "40m", 7100000, 500000, 7074000, "ft8"),
            ("20m", "20m", 14175000, 500000, 14200000, "usb"),
            ("80m", "80m", 3650000, 500000, 3700000, "lsb"),
            ("49m", "49m Broadcast", 6050000, 500000, 6070000, "am"),
        ]),
        Sdr("8c0f6e3a-7c1d-4e2b-9a55-1f2e3d4c5b6a", "RTL-SDR v3", [
            ("1b2c3d4e-5f60-4718-8a9b-0c1d2e3f4a5b", "2m FM Brügge", 145000000, 2400000, 145500000, "nfm"),
            ("2c3d4e5f-6071-4829-9bac-1d2e3f4a5b6c", "FM broadcast", 98000000, 2400000, 98500000, "wfm"),
            ("3d4e5f60-7182-493a-acbd-2e3f4a5b6c7d", "Airband", 126000000, 2400000, 125825000, "am"),
        ]),
    ]
    if extra:
        sdrs.append(Sdr("many", "Airspy HF+", [
            (f"p{i:02d}", f"Band {i:02d}", 1000000 + i * 1000000, 600000, 1000000 + i * 1000000, "usb")
            for i in range(extra)]))
    return sdrs


# What its DSP takes, by fork (owrx/dsp.py's validators): anything else is a
# KeyError, the rest of the message lost.
def _int(v):
    return isinstance(v, int) and not isinstance(v, bool)


def _num(v):
    return isinstance(v, (int, float)) and not isinstance(v, bool)


def _mod(v, plus):
    return isinstance(v, bool) or (isinstance(v, str) and re.fullmatch(r"[a-z0-9-]+" if plus else r"[a-z0-9]+", v))


def validators(fork):
    plus = fork == "plus"
    v = {
        "low_cut": lambda x: x is None or _num(x),
        "high_cut": lambda x: x is None or _num(x),
        "offset_freq": _int,
        "mod": lambda x: _mod(x, plus),
        "squelch_level": _num,
        "secondary_mod": lambda x: x is False or isinstance(x, str),
        "secondary_offset_freq": _int,
        "dmr_filter": _int,
        "output_rate": _int,
        "hd_output_rate": _int,
    }
    if fork == "upstream":
        v["dab_service_id"] = _int
    if plus:
        v.update(audio_service_id=_int, nr_enabled=lambda x: isinstance(x, bool), nr_threshold=_int,
                 rig_transmit=lambda x: isinstance(x, bool))
    return v


def analog_modes(fork):
    plus = fork == "plus"
    ssb = (150, 2750) if plus else (300, 3000)
    m = [("nfm", "FM", -4000, 4000), ("wfm", "WFM", -75000, 75000), ("am", "AM", -4000, 4000),
         ("lsb", "LSB", -ssb[1], -ssb[0]), ("usb", "USB", ssb[0], ssb[1]), ("cw", "CW", 700, 900)]
    if plus:
        m += [("sam", "SAM", -4000, 4000), ("usbd", "DATA", 0, 24000)]
    return m


DIGITAL = ["bpsk31", "bpsk63", "rtty170", "rtty450", "rtty85", "ft8", "ft4", "jt65", "jt9", "wspr",
           "fst4", "fst4w", "q65", "js8", "packet", "pocsag", "hfdl", "vdl2", "adsb", "ism"]


# ------------------------------------------------------------ the receiver

class Receiver:
    def __init__(self, a):
        self.lock = threading.Lock()
        self.fork = a.fork
        self.plus = a.fork == "plus"
        self.version = FORKS[a.fork]
        self.sdrs = make_sdrs(a.profiles)
        self.max_clients = a.max_clients
        self.others = a.others
        self.bans = {}                   # ip -> time.time() it ends
        self.silent = a.silent
        self.no_sdr = a.no_sdr
        self.locked = set(a.locked)
        self.audio = a.audio
        self.fft = a.fft
        self.stations = [7075500.0, 14201000.0, 145500000.0, 6070000.0, 98500000.0] + a.station
        self.stall_until = 0.0
        self.http = a.http
        self.no_status = a.no_status
        self.clients = []                # sessions registered, as its ClientRegistry has them
        self.open = []                   # every WebSocket open
        self.sent = []
        self.reset_stats()

    def reset_stats(self):
        self.stats = dict(connections=0, status=0, upgrades=0, ws_no_slash=0, not_found=0,
                          handshakes=0, bad_handshakes=0, pre_handshake=0, sessions=0, full=0,
                          banned_refused=0, bans=[], silent=0, no_sdr=0, starts=0, params=0,
                          unknown_keys=[], poisoned=0, bad_values=[], demod_errors=0, selects=0,
                          select_gaps=[], select_after_connect=[], locked_refused=0,
                          setfrequency=0, setsdr=0, other_types=[], fft_frames=0, fft_overflow=0,
                          audio_frames=0, audio_bytes=0, smeters=0, max_open=0, two_at_once=0,
                          pings=0, closed_by_mock=0, http_refusals=0, no_type=0, not_json=0,
                          output_rates=[], tls=0, tls_failed=0, redirects=0, hosts=[])
        self.sent = []

    def sdr(self, sid):
        return next((s for s in self.sdrs if s.id == sid), None)

    def banned(self, ip):
        return ip in self.bans and time.time() < self.bans[ip]

    def ban(self, ip):
        self.bans[ip] = time.time() + BAN_MIN * 60
        self.stats["bans"].append(ip)
        log(f"{ip}: BANNED for {BAN_MIN} minutes")
        return [c for c in self.clients if c.ip == ip]

    def status(self):
        return {
            "receiver": {"name": ARGS.name, "admin": "mock@example.org",
                         "gps": {"lat": 51.21, "lon": 3.22}, "asl": 12, "location": "On the bench"},
            "max_clients": self.max_clients,
            "version": self.version,
            "sdrs": [{"name": s.name, "type": "SdrplaySource" if s.id == "rspdx" else "RtlSdrSource",
                      "profiles": [{"name": p["name"], "center_freq": p["center_freq"],
                                    "sample_rate": p["samp_rate"]} for p in s.profiles.values()]}
                     for s in self.sdrs],
        }

    def profiles_msg(self):
        return {"type": "profiles", "value": [{"id": f"{s.id}|{pid}", "name": f"{s.name} {p['name']}"}
                                              for s in self.sdrs for pid, p in s.profiles.items()]}

    def count(self):
        return len(self.clients) + self.others


WATERFALL_COLORS = [int(0x000020 + i * 0x010101 * 0 + (i << 8) + (255 - i)) for i in range(256)]
FFT_LINES = []                       # a few lines, encoded once, sent over and over


def global_config():
    c = {
        "waterfall_scheme": "GoogleTurboWaterfall",
        "waterfall_colors": WATERFALL_COLORS,
        "waterfall_auto_levels": {"min": 3, "max": 10},
        "waterfall_auto_min_range": 50,
        "fft_size": ARGS.fft_size,
        "audio_compression": R.audio,
        "fft_compression": R.fft,
        "max_clients": R.max_clients,
        "tuning_precision": 2,
    }
    if R.plus:
        c.update(allow_center_freq_changes=False, allow_audio_recording=True, allow_chat=False,
                 callsign_url="https://www.qrzcq.com/call/{}", vessel_url="https://www.vesselfinder.com/vessels/details/{}",
                 flight_url="https://flightaware.com/live/modes/{}/redirect", modes_url="https://flightaware.com/live/modes/{}/redirect",
                 receiver_gps={"lat": 51.21, "lon": 3.22}, ui_theme="default")
    elif R.fork == "upstream":
        c["aircraft_tracking_service"] = "flightaware"
    return c


def sdr_config(sdr):
    p = sdr.props()
    c = {"waterfall_levels": {"min": -88, "max": -20}, "samp_rate": p["samp_rate"],
         "start_mod": p["start_mod"], "start_freq": p["start_freq"], "center_freq": p["center_freq"],
         "initial_squelch_level": -150, "sdr_id": sdr.id, "profile_id": p["profile_id"],
         "squelch_auto_margin": 10, "start_offset_freq": p["start_freq"] - p["center_freq"]}
    if R.fork not in ("upstream-1.2.2", "upstream-1.0"):
        c["waterfall_auto_level_default_mode"] = False
    if R.plus:
        c["tuning_step"] = "500" if p["center_freq"] < 30000000 else 12500
        c["initial_nr_level"] = 0
    return c


BOOKMARKS = {}


def bookmarks(kb):
    """kb of them, as one message: made once."""
    if kb not in BOOKMARKS:
        out, size, i = [], 0, 0
        while size < kb * 1024:
            b = {"name": f"Station {i} é", "frequency": 7000000 + i * 5000, "modulation": "usb"}
            if R.plus:
                b.update(underlying="", description="a long description " * 4, scannable=False)
            out.append(b)
            size += len(dumps(b)) + 2
            i += 1
        BOOKMARKS[kb] = out
    return BOOKMARKS[kb]


def modes_msg():
    out = []
    for mod, name, lo, hi in analog_modes(R.fork):
        out.append({"modulation": mod, "name": name, "type": "analog", "requirements": [],
                    "squelch": mod not in ("usbd",), "bandpass": {"low_cut": lo, "high_cut": hi}})
    for d in DIGITAL:
        out.append({"modulation": d, "name": d.upper(), "type": "digimode", "requirements": ["wsjt-x"],
                    "squelch": False, "underlying": ["nfm"] if d in ("packet", "pocsag", "vdl2") else ["usb"],
                    "secondaryFft": True})
    return {"type": "modes", "value": out}


# ------------------------------------------------------------- a session

class Session:
    def __init__(self, conn, ip):
        self.conn, self.ip = conn, ip
        self.cid = next(CONN_IDS)
        self.t_start = time.time()
        self.client = False              # its client object built: past the handshake
        self.sdr = None
        self.dsp = {}                    # the params its DSP holds
        self.props = {}                  # connectionproperties
        self.started = False
        self.gen = -1                    # the SDR's generation its DSP is attached to
        self.demod_ok = True
        self.enc = Encoder(sync=R.fork != "upstream-1.0")
        self.robot = 0.0
        self.last_change = self.t_start          # its bot score's clock (OpenWebRX+)
        self.last_select = self.t_start
        self.hello_seen = False
        self.fftq = queue.Queue(100)
        self.t_play = 0.0
        self.sq_left = 5 * 1024

    def stalled(self):
        with R.lock:
            if time.time() < R.stall_until:
                return True
        el = time.time() - self.t_play if self.t_play else -1
        return any(a <= el < a + b for a, b in ARGS.stall)

    def gate(self):
        """A stall on the way: everything this session sends held up until
        it ends -- then all of it at once, as a network hands over what it
        held."""
        if self.stalled():
            with R.lock:
                R.stats["stalls"] = R.stats.get("stalls", 0) + 1
            while self.stalled() and not self.conn.closed:
                time.sleep(0.01)

    def wire(self, data, op):
        self.gate()
        self.conn.send(data, op)

    def send(self, o):
        if not self.conn.closed:
            self.wire(dumps(o) if isinstance(o, dict) else o, OP_TEXT if isinstance(o, (dict, str)) else OP_BIN)

    def close(self, why):
        log(f"{self.ip}#{self.cid}: closed: {why}")
        shut(self.conn)

    # -- what the knob hears ------------------------------------------------
    def hello(self, text):
        kv = dict(p.split("=", 1) for p in text[17:].split(" ") if "=" in p)
        if kv.get("type") != "receiver":
            with R.lock:
                R.stats["bad_handshakes"] += 1
            log(f"{self.ip}#{self.cid}: invalid handshake: {text!r}")
            return
        with R.lock:
            R.stats["handshakes"] += 1
        self.send(f"CLIENT DE SERVER server=openwebrx version={R.version}")
        self.build()

    def build(self):
        details = {"receiver_name": ARGS.name, "receiver_location": "On the bench", "receiver_asl": 12,
                   "receiver_gps": {"lat": 51.21, "lon": 3.22}, "photo_title": "The bench",
                   "photo_desc": "<p>" + "x" * (ARGS.photo_kb * 1024) + "</p>", "locator": "JO11um"}
        if R.plus:
            details.update(receiver_help="", usage_policy_url="policy", session_timeout=0, keep_files=20)
        self.send({"type": "receiver_details", "value": details})
        with R.lock:
            now = time.time()
            if R.plus:
                score = sum(max(0.0, 10 - (self.t_start - c.t_start)) for c in R.clients if c.ip == self.ip)
                if score >= 30 or R.silent:
                    # Its ban, reached before its stack is there: an error,
                    # swallowed -- the session open, and nothing more.
                    R.stats["silent"] += 1
                    log(f"{self.ip}#{self.cid}: robot score {score:.1f} at connect: left silent")
                    return
                self.robot = score
            elif R.silent:
                R.stats["silent"] += 1
                return
            if R.banned(self.ip):
                R.stats["banned_refused"] += 1
                why = "Client address banned"
            elif R.count() >= R.max_clients:
                R.stats["full"] += 1
                why = "Too many clients"
            else:
                why = None
                R.clients.append(self)
                R.stats["sessions"] += 1
                n = R.count()
                everyone = list(R.clients)
        if why:
            self.send({"type": "backoff", "reason": why})
            return self.close(why)
        self.client = True
        for c in everyone:
            c.send({"type": "clients", "value": n})
        self.send({"type": "config", "value": global_config()})
        self.send({"type": "config", "value": {"waterfall_levels": {"min": -88, "max": -20},
                                               "squelch_auto_margin": 10}})
        with R.lock:
            no_sdr = R.no_sdr
        if no_sdr:
            with R.lock:
                R.stats["no_sdr"] += 1
            self.send({"type": "sdr_error", "value": "No SDR Devices available"})
        else:
            self.set_sdr(R.sdrs[0])
        self.send({"type": "features", "value": {f"feature_{i}": i % 3 != 0 for i in range(64 if R.plus else 39)}})
        self.send(modes_msg())
        with R.lock:
            profiles = R.profiles_msg()
        self.send(profiles)
        threading.Thread(target=self.fft_pump, daemon=True).start()
        threading.Thread(target=self.fft_writer, daemon=True).start()
        threading.Thread(target=self.audio_pump, daemon=True).start()

    def set_sdr(self, sdr):
        """Onto an SDR -- a new DSP, its encoder from nothing -- and what it
        says of it."""
        self.sdr = sdr
        self.enc = Encoder(sync=R.fork != "upstream-1.0")
        self.started = False
        self.send({"type": "secondary_config", "value": {"secondary_fft_size": 2048}})
        self.on_band()

    def on_band(self):
        """The SDR's band, sent: its config, its bookmarks; the DSP detached
        until start comes again."""
        with R.lock:
            cfg = sdr_config(self.sdr)
        self.send({"type": "config", "value": cfg})
        self.send({"type": "dial_frequencies", "value": [{"mode": "ft8", "frequency": cfg["center_freq"] - 26000}]})
        self.send({"type": "bookmarks", "value": bookmarks(ARGS.bookmarks_kb)})
        if R.plus:
            self.send({"type": "bands", "value": [{"name": "40m", "low_bound": 7000000, "high_bound": 7200000,
                                                   "tags": ["hamradio"]}]})

    # -- what the knob says -------------------------------------------------
    def on_text(self, text):
        with R.lock:
            R.sent.append((round(time.time() - T0, 3), self.cid, self.ip, text))
            del R.sent[:-4000]
        if not self.client:
            if text.startswith("SERVER DE CLIENT") and not self.hello_seen and not self.conn.closed:
                self.hello_seen = True
                return self.hello(text)
            with R.lock:
                R.stats["pre_handshake"] += 1
            return
        try:
            m = json.loads(text)
        except ValueError:
            with R.lock:
                R.stats["not_json"] += 1
            return
        if not isinstance(m, dict) or "type" not in m:
            with R.lock:
                R.stats["no_type"] += 1
            return
        t = m["type"]
        if t == "connectionproperties":
            self.on_props(m.get("params", {}))
        elif t == "dspcontrol":
            if m.get("action") == "start":
                self.start()
            if "params" in m:
                self.on_params(m["params"], self.dsp, "dspcontrol")
        elif t == "selectprofile":
            self.on_select(m.get("params", {}))
        elif t == "setfrequency":
            with R.lock:
                R.stats["setfrequency"] += 1
        elif t == "setsdr":
            with R.lock:
                R.stats["setsdr"] += 1
        else:
            with R.lock:
                R.stats["other_types"].append(t)

    def on_params(self, params, into, where):
        table = validators(R.fork)
        for k, v in params.items():                  # in order: the first bad one ends it
            if k not in table:
                with R.lock:
                    R.stats["unknown_keys"].append(f"{where}:{k}")
                log(f"{self.ip}#{self.cid}: {where}: unknown key {k!r}: the rest lost")
                return False
            if not table[k](v):
                with R.lock:
                    R.stats["bad_values"].append(f"{where}:{k}={v!r}")
                log(f"{self.ip}#{self.cid}: {where}: {k}={v!r} refused: the rest lost")
                return False
            into[k] = v
            if k == "mod" and where == "dspcontrol":
                ok = v in [x[0] for x in analog_modes(R.fork)] or v in DIGITAL
                if not ok:
                    with R.lock:
                        R.stats["demod_errors"] += 1
                    log(f"{self.ip}#{self.cid}: no demodulator {v!r}: its audio stops")
                self.demod_ok = ok
        with R.lock:
            R.stats["params"] += where == "dspcontrol"
        return True

    def on_props(self, params):
        if not R.plus:
            self.props = {}                          # upstream replaces, OpenWebRX+ merges
        if not self.on_params(params, self.props, "connectionproperties"):
            with R.lock:
                R.stats["poisoned"] += 1
        with R.lock:
            R.stats["output_rates"].append([self.props.get("output_rate"), self.props.get("hd_output_rate")])

    def start(self):
        with R.lock:
            R.stats["starts"] += 1
            if self.sdr:
                self.gen = self.sdr.generation
        self.started = True

    def on_select(self, params):
        sid, _, pid = str(params.get("profile", "")).partition("|")
        now = time.time()
        with R.lock:
            R.stats["selects"] += 1
            R.stats["select_gaps"].append(round(now - self.last_select, 2))
            R.stats["select_after_connect"].append(round(now - self.t_start, 2))
            sdr = R.sdr(sid)
        self.last_select = now
        if not sdr or pid not in sdr.profiles:
            return
        if sdr is not self.sdr:
            self.set_sdr(sdr)
        if R.plus:
            if pid in R.locked and params.get("key") != "memagic":
                with R.lock:
                    R.stats["locked_refused"] += 1
                self.send({"type": "log_message", "value": "This profile is locked, keeping current profile."})
                self.enc = Encoder(sync=R.fork != "upstream-1.0")  # resetSdr: a new DSP
                self.started = False
                self.send({"type": "secondary_config", "value": {"secondary_fft_size": 2048}})
                return
            score = 10 - (now - self.last_change)
            self.last_change = now
            self.robot = 0.0 if score < 0 else self.robot + score
            if self.robot >= 30:
                with R.lock:
                    gone = R.ban(self.ip)
                for c in gone:
                    c.close("banned")
                return
        log(f"{self.ip}#{self.cid}: selectprofile {sdr.name} {sdr.profiles[pid]['name']}")
        activate(sdr, pid)

    # -- what it sends ------------------------------------------------------
    def playing(self):
        return (self.started and self.demod_ok and self.sdr is not None and self.gen == self.sdr.generation
                and not self.conn.closed)

    def fft_pump(self):
        """Its waterfall, into the queue its sender reads: full, the client
        is 11 s behind, and closed."""
        period = 1.0 / ARGS.fft_fps
        t, k, n = time.monotonic(), 0, 0
        while not self.conn.closed:
            t += period
            time.sleep(max(0.0, t - time.monotonic()))
            if self.sdr is None:
                continue
            try:
                self.fftq.put_nowait(FFT_LINES[k % len(FFT_LINES)])
            except queue.Full:
                with R.lock:
                    R.stats["fft_overflow"] += 1
                return self.close("its waterfall 100 frames behind")
            k += 1
            n += 1
            if n % (3 * ARGS.fft_fps) == 0:
                self.send({"type": "cpuusage", "value": 0.25})
                if R.plus:
                    self.send({"type": "temperature", "value": 51.5})

    def fft_writer(self):
        while not self.conn.closed:
            try:
                frame = self.fftq.get(timeout=1)
            except queue.Empty:
                continue
            self.wire(frame, OP_BIN)
            with R.lock:
                R.stats["fft_frames"] += 1

    def tone(self):
        """What the passband hears: a carrier's pitch, or nothing."""
        d = self.dsp
        if self.sdr is None or not isinstance(d.get("offset_freq"), int):
            return 0.0, False
        demod = self.sdr.props()["center_freq"] + d["offset_freq"]
        lo, hi = d.get("low_cut", -4000), d.get("high_cut", 4000)
        lo = -4000 if lo is None else lo
        hi = 4000 if hi is None else hi
        mod = d.get("mod", "nfm")
        if ARGS.station_cycle:
            on, off = ARGS.station_cycle
            if (time.time() - T0) % (on + off) >= on:
                return 0.0, False                   # between its overs
        with R.lock:
            stations = list(R.stations)
        for s in stations:
            rel = s - demod
            if lo <= rel <= hi:
                if mod in ("am", "sam", "nfm", "wfm"):
                    return 1000.0, True
                return max(100.0, abs(rel)), True
        return 0.0, False

    def audio_pump(self):
        """Its audio, as its DSP's reader hands it on: whatever came since
        the last read, a frame at a time, any size -- a SYNC wherever it
        falls."""
        rnd = random.Random(self.cid)
        rate_was, phase, owed = None, 0.0, 0.0
        tick = 0.04
        t = time.monotonic()
        meter_t = 0.0
        while not self.conn.closed:
            t += tick
            time.sleep(max(0.0, t - time.monotonic()))
            if not self.playing():
                rate_was, owed = None, 0.0
                continue
            if not self.t_play:
                self.t_play = time.time()
            if ARGS.close_after and time.time() - self.t_play >= ARGS.close_after:
                with R.lock:
                    R.stats["closed_by_mock"] += 1
                return self.close("--close-after")
            hd = self.dsp.get("mod") == "wfm"
            rate = (self.props.get("hd_output_rate", 48000) if hd else self.props.get("output_rate", 12000))
            if rate != rate_was:
                owed = 0.0
            rate_was = rate
            owed += rate * tick * (1 + ARGS.drift_ppm / 1e6)
            n = int(owed)
            owed -= n
            pitch, heard = self.tone()
            sq = self.dsp.get("squelch_level", -150)
            power = 1e-6 if heard else 1e-9
            # Its squelch, as csdr's has it: closed, five blocks of zeros
            # (1024 samples each) to flush what follows -- then nothing at
            # all, the S-meter and the waterfall going on.
            if sq > -150 and 10 * math.log10(power) < sq:
                n = min(n, self.sq_left)
                self.sq_left -= n
                pcm = [0] * n
                if not n:
                    with R.lock:
                        R.stats["squelched_ticks"] = R.stats.get("squelched_ticks", 0) + 1
            else:
                self.sq_left = 5 * 1024
                pcm = []
                for _ in range(n):
                    phase += 2 * math.pi * pitch / rate
                    v = (8000 * math.sin(phase) if heard else 0) + rnd.gauss(0, 150)
                    pcm.append(max(-32768, min(32767, int(v))))
                phase %= 2 * math.pi
            if not pcm:
                data = b""
            elif R.audio == "adpcm":
                data, _ = self.enc.feed(pcm)
            else:
                data = struct.pack("<%dh" % len(pcm), *pcm)
            kind = b"\x04" if hd else b"\x02"
            for k, d in [(kind, data)] if data else []:
                # One frame, or two broken anywhere: its reads come as they come.
                cuts = [0, len(d)] if len(d) < 4 or rnd.random() < 0.5 else [0, rnd.randrange(1, len(d)), len(d)]
                for a, b in zip(cuts, cuts[1:]):
                    if a < b:
                        self.wire(k + d[a:b], OP_BIN)
                        with R.lock:
                            R.stats["audio_frames"] += 1
                            R.stats["audio_bytes"] += b - a
            meter_t += tick
            if meter_t >= 0.25:
                meter_t = 0.0
                self.send({"type": "smeter", "value": power * (1 + 0.1 * rnd.random())})
                with R.lock:
                    R.stats["smeters"] += 1


def activate(sdr, pid):
    """A band for the whole SDR: everyone on it told, and every DSP on it
    detached until it says start again."""
    with R.lock:
        if sdr.current == pid:
            return
        sdr.current = pid
        sdr.generation += 1
        on = [c for c in R.clients if c.sdr is sdr]
    for c in on:
        c.on_band()


# --------------------------------------------------------------- the wire

def shut(conn):
    """Closed for good: a close frame where it can still go, and the socket
    shut -- also after a send that timed out, which only marks it closed."""
    if not conn.closed:
        conn.close()
    try:
        conn.sock.shutdown(socket.SHUT_RDWR)
    except OSError:
        pass
    try:
        conn.sock.close()
    except OSError:
        pass


def reply(sock, code, body, ctype="text/plain", location=""):
    if isinstance(body, str):
        body = body.encode()
    reason = {200: "OK", 301: "Moved Permanently", 302: "Found", 307: "Temporary Redirect",
              308: "Permanent Redirect", 400: "Bad Request", 403: "Forbidden", 404: "Not Found",
              429: "Too Many Requests", 502: "Bad Gateway", 503: "Service Unavailable"}.get(code, "")
    where = f"Location: {location}\r\n" if location else ""
    sock.sendall(f"HTTP/1.1 {code} {reason}\r\n{where}Content-Type: {ctype}\r\n"
                 f"Content-Length: {len(body)}\r\nConnection: close\r\n\r\n".encode() + body)


def control(sock, url):
    q = {k: v[-1] for k, v in parse_qs(url.query, keep_blank_values=True).items()}
    if url.path == "/mock/sent":
        with R.lock:
            out = [dict(t=t, c=c, ip=ip, text=text) for t, c, ip, text in R.sent]
        return reply(sock, 200, json.dumps(out), "application/json")
    if url.path == "/mock/select":
        sid, _, pid = q.get("profile", "").partition("|")
        with R.lock:
            sdr = R.sdr(sid)
        if sdr and pid in sdr.profiles:
            log(f"another listener: {sdr.name} {sdr.profiles[pid]['name']}")
            activate(sdr, pid)
    closing = []
    with R.lock:
        if url.path == "/mock/set":
            for k, v in q.items():
                if k in ("max_clients", "others", "http"):
                    setattr(R, k, int(v))
                elif k == "ban":
                    closing += R.ban(v)
                elif k == "unban":
                    R.bans.pop(v, None)
                elif k in ("no_sdr", "silent", "no_status"):
                    setattr(R, k, v not in ("0", ""))
                elif k == "locked":
                    R.locked = set(x for x in v.split(",") if x)
                elif k in ("audio", "fft"):
                    setattr(R, k, v)
                elif k == "station":
                    R.stations.append(float(v))
                elif k == "stall":
                    R.stall_until = time.time() + float(v)
                elif k == "close":
                    closing += list(R.open)
                    R.stats["closed_by_mock"] += len(R.open)
            log(f"set {q}")
        elif url.path == "/mock/reset":
            R.reset_stats()
        out = dict(R.stats, live=len(R.clients), open=len(R.open), bans_now=sorted(ip for ip in R.bans if R.banned(ip)),
                   bands={s.id: s.current for s in R.sdrs}, fork=R.fork)
    for c in closing:
        c.close("the mock's /mock/set")
    reply(sock, 200, json.dumps(out), "application/json")


def read_head(sock):
    data = b""
    while b"\r\n\r\n" not in data:
        chunk = sock.recv(4096)
        if not chunk:
            return None, None, None
        data += chunk
        if len(data) > 16384:
            return None, None, None
    head = data.split(b"\r\n\r\n", 1)[0].decode("latin-1")
    lines = head.split("\r\n")
    method, target = (lines[0].split() + ["", ""])[:2]
    hdr = {}
    for line in lines[1:]:
        if ":" in line:
            k, v = line.split(":", 1)
            hdr[k.strip().lower()] = v.strip()
    return method, target, hdr


def on_client(sock, addr, control_ok=True):
    ip = addr[0]
    with R.lock:
        R.stats["connections"] += 1
    try:
        sock.settimeout(10)
        method, target, hdr = read_head(sock)
        if target is None:
            return sock.close()
        url = urlsplit(target)
        if url.path.startswith("/mock/") and control_ok:
            control(sock, url)
            return sock.close()
        with R.lock:
            R.stats["hosts"] = (R.stats["hosts"] + [hdr.get("host", "")])[-20:]
            http, no_status = R.http, R.no_status
        # A front passing its path on: the receiver sees what is under it.
        path = url.path
        if ARGS.prefix != "/":
            if not path.startswith(ARGS.prefix):
                with R.lock:
                    R.stats["not_found"] += 1
                reply(sock, 404, "not under " + ARGS.prefix)
                return sock.close()
            path = "/" + path[len(ARGS.prefix):]
        if path == "/status.json":
            with R.lock:
                R.stats["status"] += 1
                body = dumps(R.status())
            if ARGS.status_delay:
                time.sleep(ARGS.status_delay)
            if no_status:
                reply(sock, 404, "not here")
            else:
                reply(sock, 200, body, "application/json")
            return sock.close()
        if path == "/":
            reply(sock, 200, "<html><head><title>OpenWebRX</title></head><body>mock</body></html>", "text/html")
            return sock.close()
        if path == "/ws":
            with R.lock:
                R.stats["ws_no_slash"] += 1
            reply(sock, 404, "not here")
            return sock.close()
        if path != "/ws/" or hdr.get("upgrade", "").lower() != "websocket" or not hdr.get("sec-websocket-key"):
            with R.lock:
                R.stats["not_found"] += 1
            reply(sock, 404, "not here")
            return sock.close()
        if http:
            with R.lock:
                R.stats["http_refusals"] += 1
            reply(sock, http, "no")
            return sock.close()
        accept = base64.b64encode(hashlib.sha1((hdr["sec-websocket-key"] + _GUID).encode()).digest()).decode()
        sock.sendall(("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                      f"Connection: Upgrade\r\nSec-WebSocket-Accept: {accept}\r\n\r\n").encode())
        conn = WSConn(sock, addr)
        s = Session(conn, ip)
        with R.lock:
            R.stats["upgrades"] += 1
            R.open.append(s)
            R.stats["max_open"] = max(R.stats["max_open"], len(R.open))
            others = [o for o in R.open if o is not s]
        if others:
            def overlap(new=s, old=others):
                time.sleep(0.5)
                with R.lock:
                    if new in R.open and any(o in R.open for o in old):
                        R.stats["two_at_once"] += 1
                        log(f"{ip}: two sessions open at once")
            threading.Thread(target=overlap, daemon=True).start()
        log(f"{ip}#{s.cid}: WebSocket open")
        try:
            run(s)
        finally:
            with R.lock:
                if s in R.open:
                    R.open.remove(s)
                if s in R.clients:
                    R.clients.remove(s)
                    n, everyone = R.count(), list(R.clients)
                else:
                    everyone = []
            for c in everyone:
                c.send({"type": "clients", "value": n})
            shut(conn)
            log(f"{ip}#{s.cid}: gone")
    except (OSError, WSError):
        try:
            sock.close()
        except OSError:
            pass


def run(s):
    """Its reader: every frame in order; a ping after 30 s of nothing from
    the client, which needs no pong."""
    sock = s.conn.sock
    quiet = time.monotonic()
    while not s.conn.closed:
        try:
            pending = isinstance(sock, ssl.SSLSocket) and sock.pending()
            if not pending and not select.select([sock], [], [], 1.0)[0]:
                if time.monotonic() - quiet >= 30:
                    s.conn.send(b"", OP_PING)
                    with R.lock:
                        R.stats["pings"] += 1
                    quiet = time.monotonic()
                continue
            op, data = s.conn.recv_frame()
        except (OSError, WSError, ValueError):
            return
        quiet = time.monotonic()
        if op == OP_CLOSE:
            return
        if op == OP_PING:
            s.conn.send(b"", OP_PONG)
        elif op == OP_TEXT:
            s.on_text(data.decode("utf-8", "replace"))


# ------------------------------------------------------------------ fronts

TLS_CTX = None
PORT = 0


def pump(t, a):
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
                t.sendall(data)
    except (OSError, ssl.SSLError):
        pass
    finally:
        for x in (t, a):
            try:
                x.close()
            except OSError:
                pass


def front(raw, addr):
    """TLS ended here, the receiver's own HTTP behind it, as a front has it."""
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
    a, b = socket.socketpair()
    threading.Thread(target=on_client, args=(b, addr, False), daemon=True).start()
    pump(t, a)


def control_client(sock, addr):
    try:
        sock.settimeout(10)
        _, target, _ = read_head(sock)
        if target is not None:
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


def redirector(sock, addr):
    try:
        sock.settimeout(10)
        _, target, hdr = read_head(sock)
        if target is not None:
            host = hdr.get("host", "127.0.0.1")
            host = host.rsplit(":", 1)[0] if host.count(":") == 1 else host
            loc = f"https://{host}:{PORT}{target}"
            with R.lock:
                R.stats["redirects"] += 1
            log(f"{addr[0]}: {target} -- {ARGS.redirect} to {loc}")
            reply(sock, ARGS.redirect, "<html>moved</html>", "text/html", location=loc)
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


def listen(port=0):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind((ARGS.host, port))
    s.listen(16)
    return s


def stall_arg(v):
    a, _, b = v.partition(":")
    return float(a), float(b)


def main():
    global ARGS, R, TLS_CTX, PORT
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8073)
    ap.add_argument("--fork", choices=sorted(FORKS), default="plus")
    ap.add_argument("--prefix", default="/")
    ap.add_argument("--name", default="MOCK OpenWebRX Brügge")
    ap.add_argument("--max-clients", type=int, default=20)
    ap.add_argument("--others", type=int, default=0)
    ap.add_argument("--banned", action="store_true")
    ap.add_argument("--silent", action="store_true")
    ap.add_argument("--no-sdr", action="store_true")
    ap.add_argument("--locked", action="append", default=[], metavar="PID")
    ap.add_argument("--audio", choices=("adpcm", "none"), default="adpcm")
    ap.add_argument("--fft", choices=("adpcm", "none"), default="adpcm")
    ap.add_argument("--fft-size", type=int, default=4096)
    ap.add_argument("--fft-fps", type=int, default=9)
    ap.add_argument("--bookmarks-kb", type=int, default=3)
    ap.add_argument("--photo-kb", type=int, default=0)
    ap.add_argument("--profiles", type=int, default=0)
    ap.add_argument("--start", default="", metavar="SDR|PID")
    ap.add_argument("--station", type=float, action="append", default=[], metavar="HZ")
    ap.add_argument("--station-cycle", type=stall_arg, default=None, metavar="ON:OFF")
    ap.add_argument("--drift-ppm", type=float, default=0.0)
    ap.add_argument("--stall", type=stall_arg, action="append", default=[], metavar="T:D")
    ap.add_argument("--close-after", type=float, default=0.0)
    ap.add_argument("--http", type=int, default=0)
    ap.add_argument("--no-status", action="store_true")
    ap.add_argument("--status-delay", type=float, default=0.0)
    ap.add_argument("--tls", default="", metavar="CERT:KEY")
    ap.add_argument("--redirect", type=int, default=0, metavar="CODE")
    ap.add_argument("--seed", type=int, default=1)
    ARGS = ap.parse_args()
    if not ARGS.prefix.startswith("/") or not ARGS.prefix.endswith("/"):
        ap.error("--prefix: a path with a '/' first and last, /OWRX/")
    random.seed(ARGS.seed)
    R = Receiver(ARGS)
    if ARGS.start:
        sid, _, pid = ARGS.start.partition("|")
        sdr = R.sdr(sid)
        if not sdr or pid not in sdr.profiles:
            ap.error("--start: no such band")
        R.sdrs.remove(sdr)
        R.sdrs.insert(0, sdr)
        sdr.current = pid
    if ARGS.banned:
        R.bans["127.0.0.1"] = time.time() + BAN_MIN * 60
    rnd = random.Random(ARGS.seed)
    for k in range(8):
        db = [-100 + 30 * math.exp(-((i - 600 - 300 * k) / 40.0) ** 2) + rnd.gauss(0, 3) for i in range(ARGS.fft_size)]
        if ARGS.fft == "adpcm":
            FFT_LINES.append(b"\x01" + fft_line(db))
        else:
            FFT_LINES.append(b"\x01" + struct.pack("<%df" % len(db), *db))
    if ARGS.tls:
        cert, _, key = ARGS.tls.partition(":")
        TLS_CTX = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        TLS_CTX.load_cert_chain(cert, key or None)
    srv = listen(ARGS.port)
    PORT = srv.getsockname()[1]
    print(f"LISTENING {PORT}", flush=True)
    if TLS_CTX:
        ctl = listen()
        print(f"CONTROL {ctl.getsockname()[1]}", flush=True)
        threading.Thread(target=serve, args=(ctl, control_client), daemon=True).start()
    if ARGS.redirect:
        red = listen()
        print(f"REDIRECTING {red.getsockname()[1]}", flush=True)
        threading.Thread(target=serve, args=(red, redirector), daemon=True).start()
    log(f"a mock {'OpenWebRX+' if R.plus else 'OpenWebRX'} {R.version} on {ARGS.host}:{PORT}{ARGS.prefix}"
        + (" behind TLS" if TLS_CTX else ""))
    try:
        serve(srv, front if TLS_CTX else on_client)
    except KeyboardInterrupt:
        with R.lock:
            log(f"stats: {json.dumps(R.stats)}")


if __name__ == "__main__":
    sys.exit(main())
