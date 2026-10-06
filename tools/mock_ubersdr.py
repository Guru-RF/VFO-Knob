#!/usr/bin/env python3
"""Mock UberSDR: a receiver on the LAN, in the clear -- its own protocol on
http://host:8080, and its KiwiSDR input on port 8073.

For the ubersdr firmware's receiver on a LAN, and for the web SDR of the icom,
xiegu and multiflex firmwares on UberSDR's Kiwi input, without a receiver:
what ka9q_ubersdr 0.1.66 serves, as its source has it -- /api/description and
/connection (main.go), /ws and its version-4 Opus frames (websocket.go,
pcm_v4_header.go), /ws/dxcluster (dxcluster_websocket.go), the noise floor's
voices, the SSTV addon's gallery, and the Kiwi input (kiwi_websocket.go). The
audio is real Opus, encoded by libopus through ctypes -- a CW tone sending
VVV DE MOCK -- and on the Kiwi input IMA-ADPCM, so a knob pointed at this plays
something.

    python3 tools/mock_ubersdr.py                 127.0.0.1:8080 and :8073
    python3 tools/mock_ubersdr.py --host 0.0.0.0  on the LAN: on the knob's
                                                  page http://<this PC>:8080,
                                                  or <this PC>:8073 a web SDR

The ports it listens on are printed first: "LISTENING <port> <kiwi-port>"
(--port 0 picks one; --kiwi-port 0 is none, said as 0).

Limits and faults, for the paths a receiver does not take every day:
    --time-limit S     a guest's session ends S s after its first socket (an
                       hour, on many), whatever happened since -- reconnects,
                       tuning: its socket closes and /connection answers 410,
                       as UberSDR answers a session it ended (session.go
                       enforceMaxSessionTime, checked every second)
    --idle-timeout S   ...or once its socket has said nothing for S s, a ping
                       or anything else (session_timeout); without it,
                       /connection says the session's limit in its place, as
                       UberSDR does
    --day-limit S      an address's allowance: S s of open sockets in a day
                       (max_daily_time_per_ip), what is left in /connection's
                       answer, then 429
    --day-check S      ...its sockets closed once it is spent, checked every
                       S s: 30, as UberSDR does (session.go dailyTimeLoop) --
                       until then a session plays on past it
    --guests-limited   private addresses are guests too (UberSDR, as it
                       comes, lets 10/8, 172.16/12 and 192.168/16 past its
                       limits)
    --password PW      the bypass password: with it a session is bypassed,
                       with another one /connection answers 403
    --password-only    no guests: 403 "requires a password"
    --full             no room for a guest: /connection answers 503 "Maximum
                       number of users reached", as a full receiver does --
                       until GET /mock/full?on=0 (?on=1: full again)
    --name NAME        the receiver's name in its description, to tell two
                       mocks apart ("Mock UberSDR on the LAN")
    --kiwi-flavour K   the Kiwi input as 'ubersdr' (CW centred on the carrier,
                       its channel made from the first SET mod and the
                       passband taken from the next, an AGC or a squelch
                       before it let go by) or as 'kiwisdr' (a real KiwiSDR:
                       CW centred on a 500 Hz tone)
    --kiwi-cw LO,HI    a KiwiSDR's CW passband, as its owner may set it

GET /mock/state, on either port: what the knob asked for, as JSON -- with,
for each session a limit applies to, the seconds it has left as the mock
counts them. GET /mock/drop: every audio socket closed, as a network drop
closes them -- the sessions go on. GET /mock/refuse: the same, and every
audio socket refused from then on, answered 500 -- or with ?open=1 opened
and closed at once. GET /mock/restart: the receiver restarted -- every
session forgotten, its time and the day's with it, the sockets closed: a
session's time runs again from its next socket. GET /mock/vanish: the
receiver gone, as a power cut or a network leaves it -- every connection
closed and its ports let go, so that a new one is refused; with ?for=S it is
there again S s on, its sessions remembered. GET /mock/gateway: the receiver
gone from behind its tunnel -- every connection closed, and every request
from then on answered as a tunnel answers for a receiver it cannot reach:
502 Bad Gateway, a page of HTML -- or with ?status=503, 503 Service
Unavailable, as a proxy may answer.

Several at once, each on ports of its own, are a knob's list of receivers.
"""
import argparse
import base64
import ctypes
import ctypes.util
import hashlib
import ipaddress
import json
import math
import re
import socket
import struct
import threading
import time
import urllib.parse
import zlib

ARGS = None
LOCK = threading.Lock()
STATE = {
    "registered": {},          # uuid -> {"at": s, "bypassed": bool, "ua": str}
    "first": {},               # uuid -> its first socket (UberSDR's userSessionFirst)
    "kicked": [], "kicks": [],  # ...and why each was: "time", "idle", "day"
    "tunes": [], "dsp": [], "pings": 0, "audio_sockets": 0, "frames": 0, "pongs": 0,
    "dx_sockets": 0, "subscribed": [], "requests": [], "drops": 0, "restarts": 0,
    "refuse": "",              # audio sockets: "" taken, "500" refused, "open" closed as they open
    "vanished": 0,             # /mock/vanish: gone, refusing connections
    "gateway": 0,              # /mock/gateway: gone from behind its tunnel, this status (502)
    "full": False,             # --full, or /mock/full: no room for a guest
    "kiwi": {"sockets": 0, "auth": [], "mods": [], "frames": 0, "keepalives": 0, "paths": [],
             "agc": [], "squelch": []},                 # the AGC and the squelch its channel took
}
OPEN = {}                      # uuid -> the sessions on its open audio sockets
CONNS = set()                  # every connection open to it: /mock/vanish and /mock/gateway close them
LISTENING = {}                 # port -> its listening socket, while it listens
THERE = threading.Event()      # cleared while it has vanished
THERE.set()
DAY = {}                       # address -> {"used": s of sockets closed, "open": {uuid: [since, sockets]}}

RATE = 12000                   # usb, lsb, cw: radiod's 12 kHz presets
FRAME = RATE // 50             # 20 ms


def log(*a):
    print(f"[{time.strftime('%H:%M:%S')}]", *a, flush=True)


def rfc3339(t):
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(t))


def private(ip):
    try:
        return ipaddress.ip_address(ip).is_private
    except ValueError:
        return False

# ------------------------------------------------------------------ audio


class Opus:
    """libopus's encoder, by ctypes: mono, 20 ms frames."""

    def __init__(self, rate):
        lib = ctypes.CDLL(ctypes.util.find_library("opus") or "libopus.so.0")
        lib.opus_encoder_create.restype = ctypes.c_void_p
        lib.opus_encoder_create.argtypes = [ctypes.c_int32, ctypes.c_int, ctypes.c_int,
                                            ctypes.POINTER(ctypes.c_int)]
        lib.opus_encode.restype = ctypes.c_int32
        lib.opus_encode.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_int16), ctypes.c_int,
                                    ctypes.c_char_p, ctypes.c_int32]
        lib.opus_encoder_destroy.argtypes = [ctypes.c_void_p]
        err = ctypes.c_int()
        self.lib = lib
        self.enc = lib.opus_encoder_create(rate, 1, 2048, ctypes.byref(err))   # OPUS_APPLICATION_VOIP
        if err.value or not self.enc:
            raise RuntimeError(f"opus_encoder_create: {err.value}")
        self.out = ctypes.create_string_buffer(1500)

    def encode(self, pcm):
        n = self.lib.opus_encode(self.enc, (ctypes.c_int16 * len(pcm))(*pcm), len(pcm), self.out, 1500)
        if n < 0:
            raise RuntimeError(f"opus_encode: {n}")
        return self.out.raw[:n]

    def close(self):
        self.lib.opus_encoder_destroy(self.enc)


MORSE = {"V": "...-", "D": "-..", "E": ".", "M": "--", "O": "---", "C": "-.-.", "K": "-.-"}


class Tone:
    """A CW tone keyed with a message, as frames of int16 at RATE."""

    def __init__(self, text="VVV DE MOCK", hz=600.0, wpm=20, level=0.25):
        dit = 1.2 / wpm
        self.seq = []
        for word in text.split():
            for ch in word:
                for el in MORSE[ch]:
                    self.seq += [(True, dit if el == "." else 3 * dit), (False, dit)]
                self.seq[-1] = (False, 3 * dit)
            self.seq[-1] = (False, 7 * dit)
        self.period = sum(d for _, d in self.seq)
        self.hz, self.amp, self.n, self.env = hz, level * 32767, 0, 0.0

    def keyed(self, t):
        t %= self.period
        for on, d in self.seq:
            if t < d:
                return on
            t -= d
        return False

    def frame(self, steady=False):
        out = []
        for _ in range(FRAME):
            t = self.n / RATE
            target = 1.0 if steady or self.keyed(t) else 0.0
            self.env += (target - self.env) * 0.02           # a soft edge, no clicks
            out.append(int(self.amp * self.env * math.sin(2 * math.pi * self.hz * t)))
            self.n += 1
        return out


def uvarint(v):
    b = bytearray()
    while True:
        c = v & 0x7F
        v >>= 7
        if v:
            b.append(c | 0x80)
        else:
            b.append(c)
            return bytes(b)


def zigzag(v):
    return uvarint((v << 1) ^ (v >> 63) if v < 0 else v << 1)


class OpusV4:
    """The version-4 header in front of each Opus frame (AppendOpusHeader): a
    flags byte -- bit 1 metadata, bit 0 signal quality -- then the time stamp,
    whole (u64) with the rate and channels on a resync, else a zigzag delta;
    power and noise in centi-dB when they change."""

    def __init__(self):
        self.last_ts = None
        self.last_q = None
        self.resync = 0

    def header(self, ts, power, noise):
        resync = self.last_ts is None or ts - self.resync >= 5_000_000_000
        q = (int(power * 100), int(noise * 100))
        flags = (2 if resync else 0) | (1 if resync or q != self.last_q else 0)
        h = bytes([flags])
        if resync:
            h += struct.pack("<Q", ts) + uvarint(RATE) + bytes([1])
            self.resync = ts
        else:
            h += zigzag(ts - self.last_ts)
        if flags & 1:
            h += struct.pack("<hh", *q)
        self.last_ts, self.last_q = ts, q
        return h


class Adpcm:
    """IMA ADPCM, as UberSDR's kiwi_adpcm.go encodes for the Kiwi input: two
    samples a byte, the first in the low nibble, state kept across frames."""
    STEP = [7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60,
            66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337,
            371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552,
            1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894,
            6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350,
            22385, 24623, 27086, 29794, 32767]
    IDX = [-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8]

    def __init__(self):
        self.index, self.prev = 0, 0

    def sample(self, s):
        step = self.STEP[self.index]
        diff, code = s - self.prev, 0
        if diff < 0:
            code, diff = 8, -diff
        if diff >= step:
            code |= 4
            diff -= step
        if diff >= step // 2:
            code |= 2
            diff -= step // 2
        if diff >= step // 4:
            code |= 1
        d = step >> 3
        if code & 1:
            d += step >> 2
        if code & 2:
            d += step >> 1
        if code & 4:
            d += step
        self.prev = max(-32768, min(32767, self.prev - d if code & 8 else self.prev + d))
        self.index = max(0, min(88, self.index + self.IDX[code]))
        return code

    def encode(self, pcm):
        return bytes(self.sample(pcm[i]) | self.sample(pcm[i + 1]) << 4 for i in range(0, len(pcm) - 1, 2))


def png(w=320, h=256):
    """An SSTV picture, as the gallery serves them: 8-bit RGB, every row
    unfiltered -- a colour gradient with a white frame."""
    raw = bytearray()
    for y in range(h):
        raw.append(0)
        for x in range(w):
            edge = x < 8 or y < 8 or x >= w - 8 or y >= h - 8
            raw += bytes((255, 255, 255)) if edge else bytes((x * 255 // w, y * 255 // h, 128))

    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(bytes(raw), 6)) + chunk(b"IEND", b""))


PNG = None

# ------------------------------------------------------------- HTTP and WS


def read_request(f):
    """(method, path, query, headers, body), or None once the client is gone."""
    line = f.readline()
    if not line:
        return None
    try:
        method, target, _ = line.decode("latin-1").split(" ", 2)
    except ValueError:
        return None
    headers = {}
    while True:
        h = f.readline()
        if not h or h in (b"\r\n", b"\n"):
            break
        k, _, v = h.decode("latin-1").partition(":")
        headers[k.strip().lower()] = v.strip()
    n = int(headers.get("content-length", "0") or 0)
    body = f.read(n) if n else b""
    u = urllib.parse.urlsplit(target)
    return method, u.path, urllib.parse.parse_qs(u.query), headers, body


REASON = {200: "OK", 400: "Bad Request", 403: "Forbidden", 404: "Not Found", 410: "Gone", 429: "Too Many Requests",
          500: "Internal Server Error", 502: "Bad Gateway", 503: "Service Unavailable"}


def respond(c, status, body, ctype="application/json", keep=True):
    if isinstance(body, (dict, list)):
        body = json.dumps(body).encode()
    elif isinstance(body, str):
        body = body.encode()
    head = (f"HTTP/1.1 {status} {REASON.get(status, 'OK')}\r\nContent-Type: {ctype}\r\n"
            f"Content-Length: {len(body)}\r\nConnection: {'keep-alive' if keep else 'close'}\r\n\r\n")
    c.sendall(head.encode() + body)


class WS:
    """The server's end of a WebSocket: frames out unmasked, in unmasked here;
    pings answered, pongs counted."""

    def __init__(self, c, f, headers):
        key = headers.get("sec-websocket-key", "")
        acc = base64.b64encode(hashlib.sha1((key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode())
                               .digest()).decode()
        c.sendall(("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                   f"Sec-WebSocket-Accept: {acc}\r\n\r\n").encode())
        self.c, self.f, self.wlock, self.open = c, f, threading.Lock(), True

    def send(self, op, data):
        if isinstance(data, str):
            data = data.encode()
        n = len(data)
        hdr = bytes([0x80 | op]) + (bytes([n]) if n < 126 else
                                    bytes([126]) + struct.pack(">H", n) if n < 65536 else
                                    bytes([127]) + struct.pack(">Q", n))
        with self.wlock:
            if not self.open:
                return False
            try:
                self.c.sendall(hdr + data)
                return True
            except OSError:
                self.open = False
                return False

    def text(self, obj):
        return self.send(0x1, obj if isinstance(obj, str) else json.dumps(obj))

    def recv(self):
        """(op, payload) of the next text or binary frame; None once closed."""
        while True:
            h = self.f.read(2)
            if len(h) < 2:
                self.open = False
                return None
            op, n = h[0] & 0x0F, h[1] & 0x7F
            if n == 126:
                n = struct.unpack(">H", self.f.read(2))[0]
            elif n == 127:
                n = struct.unpack(">Q", self.f.read(8))[0]
            mask = self.f.read(4) if h[1] & 0x80 else b"\0\0\0\0"
            data = bytearray(self.f.read(n))
            for i in range(len(data)):
                data[i] ^= mask[i & 3]
            if op == 0x9:
                self.send(0xA, bytes(data))
            elif op == 0xA:
                with LOCK:
                    STATE["pongs"] += 1
            elif op == 0x8:
                self.close()
                return None
            else:
                return op, bytes(data)

    def close(self, code=1000):
        if self.open:
            self.send(0x8, struct.pack(">H", code))
        self.drop()

    def drop(self):
        """Gone, with no word: as a network drop leaves a socket."""
        self.open = False
        try:
            self.c.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass

# ------------------------------------------------------- the receiver's own


BANDS = [("160m", 1810000, 2000000), ("80m", 3500000, 3800000), ("40m", 7000000, 7200000),
         ("30m", 10100000, 10150000), ("20m", 14000000, 14350000), ("17m", 18068000, 18168000),
         ("15m", 21000000, 21450000), ("12m", 24890000, 24990000), ("10m", 28000000, 29700000)]
DX = [("OK1ABC", 14205000, "SSB 59 in Prague"), ("EA8XYZ", 14250000, "up 5"),
      ("JA1QRP", 7012000, "CW"), ("VK2AAA", 7160000, "LSB"), ("ZS6BCD", 14074000, "FT8")]
CW = [("DL1XYZ", 14025000, 18, 24), ("G4ABC", 7023500, 11, 28)]
SSTV = [("14230000_usb_M1_20261004_120517.png", "M1", "usb", "", 14230000, 12.5),
        ("14230000_usb_S2_20261004_113002.png", "S2", "usb", "PA3XYZ", 14230000, 7.0),
        ("7171000_lsb_M2_20261004_101500.png", "M2", "lsb", "", 7171000, 15.1)]


def description(peer):
    with LOCK:
        users = sum(1 for r in STATE["registered"].values() if not r["bypassed"])
    return {
        "receiver": {"name": ARGS.name, "callsign": "MOCK", "location": "127.0.0.1",
                     "antenna": "a dummy load", "public_url": "", "gps": {"lat": 51.2, "lon": 2.8}},
        "version": "0.1.66", "max_clients": 20, "available_clients": max(0, 20 - users),
        "bypassed_users_only": ARGS.password_only,
        "max_session_time": ARGS.time_limit or 3600, "noise_floor": True, "dx_cluster": True,
        "cw_skimmer": True, "tuning_range": {"min_frequency": 10000, "max_frequency": 30000000},
        "default_frequency": 14175000, "default_mode": "usb",
        "dsp": {"enabled": True, "filters": ["nr2", "rn2", "nr4"]}, "addons": ["sstv"],
        "server_time": rfc3339(time.time()),
    }


def connection(c, peer, headers, body):
    try:
        j = json.loads(body or b"{}")
    except ValueError:
        return respond(c, 400, {"allowed": False, "reason": "Invalid request body"})
    uuid, pw = j.get("user_session_id", ""), j.get("password", "")
    base = {"client_ip": peer, "session_timeout": 0, "daily_time_used_secs": 0,
            "daily_time_remaining_secs": -1}
    if ARGS.password and pw and pw != ARGS.password:
        return respond(c, 403, dict(base, allowed=False, reason="Invalid bypass password"))
    bypassed = bool(ARGS.password and pw == ARGS.password) or (private(peer) and not ARGS.guests_limited)
    if ARGS.password_only and not bypassed:
        return respond(c, 403, dict(base, allowed=False, reason="This receiver requires a password to access"))
    with LOCK:
        full = STATE["full"]
    if full and not bypassed:
        log(f"/connection {uuid[:8]} from {peer}: no room -- 503")
        return respond(c, 503, dict(base, allowed=False, reason="Maximum number of users reached. "
                                                                "Please try again later."))
    with LOCK:
        kicked = uuid in STATE["kicked"]
        used = day_used(peer)
    if kicked:
        log(f"/connection {uuid[:8]}: ended before -- 410")
        return respond(c, 410, dict(base, allowed=False,
                                    reason="Your session has been terminated. Please refresh the page."))
    mst = 0 if bypassed else (ARGS.time_limit or 3600)
    if ARGS.day_limit and not bypassed:
        if used >= ARGS.day_limit:
            log(f"/connection {uuid[:8]} from {peer}: the day's {ARGS.day_limit:.0f} s used -- 429")
            return respond(c, 429, dict(base, allowed=False, reason=f"Daily time limit reached "
                                        f"({int(ARGS.day_limit) // 60} minutes per 24 hours). "
                                        "Please try again later."))
        base.update(daily_time_used_secs=int(used), daily_time_remaining_secs=int(ARGS.day_limit - used))
    # Where it keeps no idle limit, UberSDR says the session's limit in its place (main.go).
    if not bypassed:
        base["session_timeout"] = int(ARGS.idle_timeout or mst)
    with LOCK:
        if uuid:
            r = STATE["registered"].setdefault(uuid, {"at": time.time(), "ua": headers.get("user-agent", "")})
            r["bypassed"] = bypassed
    log(f"/connection {uuid[:8]} from {peer}: allowed, {'bypassed' if bypassed else f'a guest, {mst} s'}"
        f"{', with the password' if pw else ''}")
    respond(c, 200, dict(base, allowed=True, max_session_time=mst, bypassed=bypassed,
                         allowed_iq_modes=["iq48", "iq96"] if bypassed else []))


# ------------------------------------------------------------- its limits


def day_used(ip, now=None):
    """An address's seconds in the day: its sockets closed, and those open
    now (ip_daily_time.go GetUsedSeconds). LOCK held."""
    now = now or time.time()
    d = DAY.get(ip)
    return d["used"] + sum(now - since for since, _ in d["open"].values()) if d else 0.0


def day_open(ip, uuid):
    """A socket of the UUID's open from the address: its clock starts with
    the first (RecordSessionStart). LOCK held."""
    o = DAY.setdefault(ip, {"used": 0.0, "open": {}})["open"].setdefault(uuid, [time.time(), 0])
    o[1] += 1


def day_close(ip, uuid):
    """...and with its last closed, the time is the address's
    (RecordSessionEnd). LOCK held."""
    d = DAY.get(ip)
    o = d and d["open"].get(uuid)
    if o:
        o[1] -= 1
        if o[1] <= 0:
            d["used"] += time.time() - o[0]
            del d["open"][uuid]


def kick(uuid, why):
    """KickUserBySessionID: the UUID shut out, its sockets closed with no
    word of why."""
    with LOCK:
        if uuid in STATE["kicked"]:
            return
        STATE["kicked"].append(uuid)
        STATE["kicks"].append({"uuid": uuid, "why": why})
        socks = [s["ws"] for s in OPEN.get(uuid, ())]
    log(f"session {uuid[:8]}: ended ({why}) -- closed")
    for ws in socks:
        ws.close(1000)


def enforce():
    """The receiver's loops, once a second: a session's limit, counted from
    its first socket whether one is open or not; its socket's silence; and
    every --day-check s, the address's day."""
    k = 0
    while True:
        time.sleep(1)
        k += 1
        day = k % max(1, round(ARGS.day_check)) == 0
        now = time.time()
        with LOCK:
            regs = {u: r["bypassed"] for u, r in STATE["registered"].items()}
            first = dict(STATE["first"])
            kicked = set(STATE["kicked"])
            open_ = [(u, s["active"], s["ip"]) for u, ss in OPEN.items() for s in ss]
            spent = {ip for ip in DAY if day and ARGS.day_limit and day_used(ip, now) >= ARGS.day_limit}
        for uuid, t in first.items():
            if ARGS.time_limit and not regs.get(uuid, True) and uuid not in kicked and now - t > ARGS.time_limit:
                kick(uuid, "time")
        for uuid, active, ip in open_:
            if regs.get(uuid, True):
                continue
            if ARGS.idle_timeout and now - active > ARGS.idle_timeout:
                kick(uuid, "idle")
            elif ip in spent:
                kick(uuid, "day")


def status_msg(s):
    return {"type": "status", "frequency": s["freq"], "mode": s["mode"], "bandwidthLow": s["lo"],
            "bandwidthHigh": s["hi"], "sampleRate": RATE, "channels": 1}


def stream_audio(ws, s, stop):
    """An Opus frame every 20 ms, behind its version-4 header."""
    enc, tone, hdr = Opus(RATE), Tone(), OpusV4()
    t0, k = time.monotonic(), 0
    try:
        while not stop.is_set() and ws.open:
            k += 1
            wait = t0 + k * 0.02 - time.monotonic()
            if wait > 0:
                time.sleep(wait)
            power = -60.0 + 3 * math.sin(k / 50.0)                  # dBFS: S-meter and SNR move
            frame = hdr.header(time.time_ns(), round(power, 1), -92.0) + enc.encode(tone.frame())
            if not ws.send(0x2, frame):
                break
            with LOCK:
                STATE["frames"] += 1
    finally:
        enc.close()


def audio_ws(c, f, q, headers, peer):
    uuid = q.get("user_session_id", [""])[0]
    with LOCK:
        reg = STATE["registered"].get(uuid)
        kicked = uuid in STATE["kicked"]
        shut = STATE["refuse"] == "open"
    ws = WS(c, f, headers)
    if shut:
        log(f"audio socket {uuid[:8]}: closed as it opened (/mock/refuse?open=1)")
        return ws.close(1011)
    if kicked:
        ws.text({"type": "error", "error": "Your session has been terminated. Please refresh the page."})
        return ws.close()
    if not reg:
        ws.text({"type": "error", "error": "Invalid session. Please refresh the page and try again."})
        return ws.close()
    if q.get("version", [""])[0] != "4" or q.get("format", [""])[0] != "opus":
        log(f"audio socket {uuid[:8]}: not opus v4 ({q.get('format')}, {q.get('version')})")
    # The session's clock starts with its first socket -- even one then
    # refused, as CreateSession starts it before its checks.
    with LOCK:
        STATE["first"].setdefault(uuid, time.time())
        spent = not reg["bypassed"] and ARGS.day_limit and day_used(peer) >= ARGS.day_limit
    if spent:
        ws.text({"type": "error", "error": f"daily time limit exceeded for your IP address "
                                           f"({int(ARGS.day_limit) // 60} minutes per 24 hours)"})
        return ws.close()
    s = {"uuid": uuid, "freq": int(q.get("frequency", ["14175000"])[0]), "mode": q.get("mode", ["usb"])[0],
         "lo": int(q.get("bandwidthLow", ["50"])[0]), "hi": int(q.get("bandwidthHigh", ["2700"])[0]),
         "ws": ws, "ip": peer, "active": time.time()}
    with LOCK:
        STATE["audio_sockets"] += 1
        OPEN.setdefault(uuid, []).append(s)
        day_open(peer, uuid)
    log(f"audio socket {uuid[:8]}: {s['freq']} Hz {s['mode']} {s['lo']}..{s['hi']}"
        f"{', password in the URL' if 'password' in q else ''}")
    stop = threading.Event()
    threading.Thread(target=stream_audio, args=(ws, s, stop), daemon=True).start()
    try:
        while True:
            m = ws.recv()
            if m is None:
                break
            op, data = m
            if op != 0x1:
                continue
            s["active"] = time.time()                 # any message: TouchSession
            try:
                j = json.loads(data)
            except ValueError:
                ws.text({"type": "error", "error": "Invalid message"})
                break
            t = j.get("type")
            if t == "tune":
                for k, v in (("frequency", "freq"), ("mode", "mode"), ("bandwidthLow", "lo"),
                             ("bandwidthHigh", "hi")):
                    if k in j:
                        if k != "mode" and not isinstance(j[k], int):
                            ws.text({"type": "error", "error": f"invalid {k}"})   # typed fields
                            raise ConnectionError
                        s[v] = j[k]
                with LOCK:
                    STATE["tunes"].append({k: s[k] for k in ("freq", "mode", "lo", "hi")})
                log(f"tune {s['freq']} Hz {s['mode']} {s['lo']}..{s['hi']}")
                ws.text(status_msg(s))
            elif t == "get_status":
                ws.text(status_msg(s))
            elif t == "set_dsp":
                with LOCK:
                    STATE["dsp"].append(j)
                log(f"noise filter {j.get('filter') if j.get('enabled') else 'off'}")
                ws.text({"type": "dsp_status", "info": {"enabled": bool(j.get("enabled")),
                                                        "filter": j.get("filter", "")}})
            elif t == "ping":
                with LOCK:
                    STATE["pings"] += 1
    except ConnectionError:
        ws.close(1003)
    finally:
        stop.set()
        with LOCK:
            OPEN[uuid].remove(s)
            day_close(peer, uuid)
        log(f"audio socket {uuid[:8]}: closed")


def dx_ws(c, f, q, headers):
    uuid = q.get("user_session_id", [""])[0]
    ws = WS(c, f, headers)
    with LOCK:
        known = uuid in STATE["registered"]
        STATE["dx_sockets"] += 1
    if not known:
        ws.text({"type": "error", "error": "Invalid or missing user_session_id. Please refresh the page."})
        return ws.close()
    subs = set()

    def spots():
        k = 0
        while ws.open:
            time.sleep(1)
            k += 1
            if k % 10 == 0:
                ws.send(0x9, b"mock")                    # the server pings, and wants pongs
            now = time.time()
            if "dx" in subs and k % 3 == 1:
                call, hz, comment = DX[(k // 3) % len(DX)]
                ws.text({"type": "dx_spot", "data": {"frequency": float(hz), "dx_call": call, "spotter": "MOCK",
                                                     "comment": comment, "time": rfc3339(now - 60 * (k % 7)),
                                                     "band": "", "country": ""}})
            if "cw" in subs and k % 4 == 2:
                call, hz, snr, wpm = CW[(k // 4) % len(CW)]
                ws.text({"type": "cw_spot", "data": {"frequency": float(hz), "dx_call": call, "snr": snr,
                                                     "wpm": wpm, "time": rfc3339(now - 20)}})
    threading.Thread(target=spots, daemon=True).start()
    while True:
        m = ws.recv()
        if m is None:
            break
        try:
            t = json.loads(m[1]).get("type", "")
        except ValueError:
            continue
        for sub, name in (("subscribe_dx_spots", "dx"), ("subscribe_cw_spots", "cw")):
            if t == sub:
                subs.add(name)
                with LOCK:
                    STATE["subscribed"].append(name)
                ws.text({"type": "subscription_status", "stream": name + "_spots", "enabled": True})
                log(f"spots: subscribed to {name}")


def receiver_conn(c, peer):
    f = c.makefile("rb")
    try:
        while True:
            req = read_request(f)
            if req is None:
                return
            method, path, q, headers, body = req
            keep = headers.get("connection", "").lower() != "close"
            with LOCK:
                STATE["requests"].append(f"{method} {path}")
                gateway = STATE["gateway"]
            if gateway and not path.startswith("/mock/"):
                respond(c, gateway, f"<html><body><h1>{gateway} {REASON[gateway]}</h1></body></html>", "text/html",
                        keep)
                if not keep or headers.get("upgrade", ""):
                    return
                continue
            if headers.get("upgrade", "").lower() == "websocket":
                if path == "/ws":
                    with LOCK:
                        refused = STATE["refuse"] == "500"
                    if refused:
                        log("audio socket: refused (/mock/refuse)")
                        return respond(c, 500, "refused", "text/plain", False)
                    return audio_ws(c, f, q, headers, peer)
                if path == "/ws/dxcluster":
                    return dx_ws(c, f, q, headers)
                return respond(c, 404, "no such socket", "text/plain", False)
            if path == "/api/description":
                respond(c, 200, description(peer), keep=keep)
            elif path == "/connection" and method == "POST":
                connection(c, peer, headers, body)
            elif path == "/api/bands":
                respond(c, 200, [{"label": b, "start": lo, "end": hi, "group": "amateur"} for b, lo, hi in BANDS],
                        keep=keep)
            elif path == "/api/noisefloor/voice-activity":
                band = q.get("band", [""])[0]
                lo, hi = next(((lo, hi) for b, lo, hi in BANDS if b == band), (0, 0))
                acts = [{"estimated_dial_freq": hz, "mode": "USB" if hz > 10e6 else "LSB",
                         "dx_callsign": call if i == 0 else "", "snr": 14.5 - i}
                        for i, (call, hz, comment) in enumerate(d for d in DX if lo <= d[1] <= hi
                                                                and "FT8" not in d[2] and "CW" not in d[2])]
                respond(c, 200, {"band": band, "activities": acts}, keep=keep)
            elif path == "/addon/sstv/api/images":
                respond(c, 200, [{"file": fn, "sstv_mode": m, "audio_mode": a, "callsign": cs,
                                  "rx_end": rfc3339(time.time() - 600 * i), "frequency_hz": hz,
                                  "snr_avg_db": snr} for i, (fn, m, a, cs, hz, snr) in enumerate(SSTV)], keep=keep)
            elif path.startswith("/addon/sstv/images/"):
                name = path.rsplit("/", 1)[1]
                if any(name == s[0] for s in SSTV):
                    respond(c, 200, PNG, "image/png", keep)
                else:
                    respond(c, 404, "not found", "text/plain", keep)
            elif path == "/mock/state":
                with LOCK:
                    st, now = json.loads(json.dumps(STATE)), time.time()
                    guest = {u for u, r in STATE["registered"].items() if not r["bypassed"]}
                    # What each limit leaves, as the mock counts it: the session's,
                    # the address's day, an open socket's silence.
                    st["left"] = {u: ARGS.time_limit - (now - t) for u, t in STATE["first"].items()
                                  if ARGS.time_limit and u in guest}
                    st["day_left"] = {ip: ARGS.day_limit - day_used(ip, now) for ip in DAY} if ARGS.day_limit else {}
                    st["idle_left"] = {u: ARGS.idle_timeout - (now - s["active"]) for u, ss in OPEN.items()
                                       for s in ss if ARGS.idle_timeout and u in guest}
                respond(c, 200, st, keep=keep)
            elif path in ("/mock/drop", "/mock/refuse"):
                with LOCK:
                    socks = [s["ws"] for ss in OPEN.values() for s in ss]
                    STATE["drops"] += 1
                    if path == "/mock/refuse":
                        STATE["refuse"] = "open" if q.get("open", [""])[0] == "1" else "500"
                log(f"dropping {len(socks)} audio socket(s), as the network would"
                    f"{'; refusing them from now on' if path == '/mock/refuse' else ''}")
                for ws in socks:
                    ws.drop()
                respond(c, 200, {"dropped": len(socks)}, keep=keep)
            elif path == "/mock/vanish":
                back = float(q.get("for", ["0"])[0] or 0)
                respond(c, 200, {"vanished": True, "for": back}, keep=False)
                vanish(back)
                return
            elif path == "/mock/gateway":
                status = int(q.get("status", ["502"])[0] or 502)
                if status not in (502, 503):
                    respond(c, 400, {"error": "?status=502 or 503"}, keep=False)
                    return
                with LOCK:
                    STATE["gateway"] = status
                    socks = [s for s in CONNS if s is not c]
                log(f"gone from behind the tunnel: {len(socks)} connection(s) closed, {status} from now on")
                for s in socks:
                    try:
                        s.shutdown(socket.SHUT_RDWR)
                    except OSError:
                        pass
                respond(c, 200, {"gateway": status}, keep=False)
                return
            elif path == "/mock/full":
                with LOCK:
                    STATE["full"] = q.get("on", ["1"])[0] != "0"
                    full = STATE["full"]
                log("full: no room for a guest" if full else "room for a guest again")
                respond(c, 200, {"full": full}, keep=keep)
            elif path == "/mock/restart":
                # Its sessions, their clocks and the day's are in its memory: gone.
                with LOCK:
                    socks = [s["ws"] for ss in OPEN.values() for s in ss]
                    STATE["registered"].clear()
                    STATE["first"].clear()
                    STATE["kicked"].clear()
                    STATE["restarts"] += 1
                    DAY.clear()
                log(f"restarted: every session forgotten, {len(socks)} audio socket(s) closed")
                for ws in socks:
                    ws.drop()
                respond(c, 200, {"dropped": len(socks)}, keep=keep)
            elif path == "/":
                respond(c, 200, "<html><body>mock UberSDR</body></html>", "text/html", keep)
            else:
                respond(c, 404, "not found", "text/plain", keep)
            if not keep:
                return
    except OSError:
        pass
    finally:
        try:
            c.close()
        except OSError:
            pass

# ----------------------------------------------------------- its Kiwi input


def kiwi_status():
    return ("status=active\noffline=no\nname=0-30 MHz SDR, MOCK, 127.0.0.1\n"
            "sdr_hw=KiwiSDR 2 v1.902 ⁣ \U0001F4E1 GPS ⁣ ⏳\U0001F6AB Limits\n"
            "bands=0-30000000\nfreq_offset=0.000\nmode=rx4_wf4\nusers=0\nusers_max=20\next_api=4\n"
            "preempt=0\ngps_good=0\nsw_version=KiwiSDR_v1.902\nant_connected=1\nuptime=100\n")


def kiwi_cfg():
    """Its configuration as load_cfg carries it. UberSDR's: CW on the carrier,
    compact, escaped as Go escapes it (%3A). A KiwiSDR's: CW on --kiwi-cw
    (300..700, its 500 Hz tone), a space after each colon and comma, escaped
    in lower case (%3a), as a real one sends it."""
    if ARGS.kiwi_flavour == "ubersdr":
        j = ('{"passbands":{"am":{"lo":-4900,"hi":4900},"lsb":{"lo":-2400,"hi":-300},"usb":{"lo":300,"hi":2400},'
             '"cw":{"lo":-400,"hi":400},"cwn":{"lo":-250,"hi":250},"nbfm":{"lo":-6000,"hi":6000}},'
             '"rx_grid":"JO11","init":{"freq":7020,"mode":"cw","zoom":0}}')
        return urllib.parse.quote(j, safe="").replace("+", "%20")
    lo, hi = ARGS.kiwi_cw
    j = json.dumps({"passbands": {"am": {"lo": -4900, "hi": 4900}, "cw": {"lo": lo, "hi": hi, "pbw": hi - lo},
                                  "cwn": {"lo": 470, "hi": 530}, "usb": {"lo": 300, "hi": 2700}},
                    "ext_api_nchans": 4, "init": {"freq": 7020, "mode": "cw"}})
    return re.sub(r"%[0-9A-F]{2}", lambda m: m.group(0).lower(), urllib.parse.quote(j, safe=""))


PRESET = {"usb": (50, 2700), "lsb": (-2700, -50), "cwu": (-200, 200), "am": (-5000, 5000),
          "sam": (-5000, 5000), "nfm": (-5000, 5000)}
KIWI_MODES = {"usb": "usb", "lsb": "lsb", "cw": "cwu", "cwn": "cwu", "am": "am", "sam": "sam", "nbfm": "nfm"}


def kiwi_ws(c, f, headers, path, peer):
    ws = WS(c, f, headers)
    with LOCK:
        STATE["kiwi"]["sockets"] += 1
        STATE["kiwi"]["paths"].append(path)
    log(f"kiwi: {path} from {peer}")
    sess, stop = {}, threading.Event()

    def msg(name, value=""):
        ws.send(0x2, b"MSG " + (f"{name}={value}" if value else name).encode())

    def snd():
        enc, tone, seq = Adpcm(), Tone(hz=800.0), 0
        t0, k = time.monotonic(), 0
        while not stop.is_set() and ws.open:
            k += 1
            wait = t0 + k * 0.02 - time.monotonic()
            if wait > 0:
                time.sleep(wait)
            sm = int((-73 + 127) * 10)                            # S9, as (dBm + 127) * 10
            pkt = b"SND" + bytes([0x10]) + struct.pack("<I", seq) + struct.pack(">H", sm)
            if not ws.send(0x2, pkt + enc.encode(tone.frame(steady=True))):
                break
            seq += 1
            with LOCK:
                STATE["kiwi"]["frames"] += 1

    authed = False
    try:
        while True:
            m = ws.recv()
            if m is None:
                break
            cmd = m[1].decode("latin-1", "replace")
            if not cmd.startswith("SET "):
                continue
            p = {}
            for part in cmd[4:].split():
                k, _, v = part.partition("=")
                p[k] = v
            if "auth" in p:
                with LOCK:
                    STATE["kiwi"]["auth"].append(cmd)
                log(f"kiwi: {cmd}")
                if not authed:
                    authed = True
                    for name, v in (("rx_chans", "20"), ("chan_no_pwd", "0"), ("chan_no_pwd_true", "0"),
                                    ("max_camp", "20"), ("badp", "0")):
                        msg(name, v)
                    msg("", "version_maj=1 version_min=840 debian_ver=11 model=2 platform=0 hw=1")
                    msg("load_cfg", kiwi_cfg())
                    msg("center_freq", "15000000")
                    msg("bandwidth", "30000000")
                    msg("sample_rate", f"{RATE:.6f}")
                    msg("client_public_ip", peer)
                    msg("rx_chan", "0")
                    msg("is_local", f"0,{1 if private(peer) else 0},0")
                    msg("cfg_loaded")
                    msg("audio_init", f"0 audio_rate={RATE}")
            elif "mod" in p:
                mode = KIWI_MODES.get(p["mod"].lower())
                if not mode:
                    continue
                hz = int(round(float(p.get("freq", "0")) * 1000))
                lo, hi = int(p.get("low_cut", "0")), int(p.get("high_cut", "0"))
                lo, hi = max(lo, -RATE // 2), min(hi, RATE // 2)
                if not sess:
                    # UberSDR makes the channel with the mode's preset passband,
                    # taking the edges asked for only from the next SET mod.
                    edges = ARGS.kiwi_flavour == "kiwisdr"
                    sess.update(freq=hz, mode=mode, lo=lo if edges else PRESET[mode][0],
                                hi=hi if edges else PRESET[mode][1], edges=edges)
                    threading.Thread(target=snd, daemon=True).start()
                    what = "made"
                else:
                    sess.update(freq=hz, mode=mode)
                    if lo and hi:
                        sess.update(lo=lo, hi=hi, edges=True)
                    what = "retuned"
                with LOCK:
                    STATE["kiwi"]["mods"].append(dict(sess, cmd=cmd))
                log(f"kiwi: {cmd} -> channel {what}: {sess['freq']} Hz {sess['mode']} "
                    f"{sess['lo']}..{sess['hi']}{'' if sess['edges'] else ' (its preset)'}")
            elif "agc" in p or "squelch" in p:
                # The channel's: UberSDR lets them go by while it has none
                # (applyAGC, applySquelch); a KiwiSDR's is there from the login.
                if sess or ARGS.kiwi_flavour == "kiwisdr":
                    with LOCK:
                        STATE["kiwi"]["agc" if "agc" in p else "squelch"].append(cmd)
                    log(f"kiwi: {cmd}")
                else:
                    log(f"kiwi: {cmd} -> let go by: no channel yet")
            elif "keepalive" in cmd:
                with LOCK:
                    STATE["kiwi"]["keepalives"] += 1
    finally:
        stop.set()
        log("kiwi: closed")


def kiwi_conn(c, peer):
    f = c.makefile("rb")
    try:
        req = read_request(f)
        if req is None:
            return
        method, path, q, headers, body = req
        parts = path.strip("/").split("/")
        ws_path = (len(parts) >= 4 and parts[0] == "ws" and parts[1] == "kiwi") or \
                  (len(parts) >= 2 and parts[0].isdigit() and len(parts[0]) >= 10)
        if headers.get("upgrade", "").lower() == "websocket" and ws_path:
            return kiwi_ws(c, f, headers, path, peer)
        if path == "/status":
            return respond(c, 200, kiwi_status(), "text/plain; charset=utf-8", False)
        if path == "/mock/state":
            with LOCK:
                return respond(c, 200, json.loads(json.dumps(STATE["kiwi"])), keep=False)
        respond(c, 404, "404 page not found", "text/plain", False)    # its static files' answer
    except OSError:
        pass
    finally:
        try:
            c.close()
        except OSError:
            pass


def listen(host, port):
    """A port held, from now until it vanishes: --port 0's, the one it picked."""
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind((host, port))
    s.listen(16)
    with LOCK:
        LISTENING[s.getsockname()[1]] = s
    return s


def vanish(back):
    """The receiver gone, as a power cut or a network leaves it: every
    connection closed, its ports let go -- a new one refused -- and with
    `back`, there again so many seconds on, its sessions remembered."""
    THERE.clear()
    with LOCK:
        STATE["vanished"] += 1
        socks = list(LISTENING.values()) + list(CONNS)
    log(f"vanished: {len(socks)} socket(s) closed, connections refused"
        f"{f' for {back:g} s' if back else ''}")
    for s in socks:
        try:
            s.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
    if back:
        def come_back():
            log("there again")
            THERE.set()
        threading.Timer(back, come_back).start()


def serve(s, handler, what):
    """The port s holds, served -- and once the receiver has vanished, held
    again on the same port when it is there again."""
    host, port = s.getsockname()[:2]

    def run(c, peer):
        try:
            handler(c, peer)
        finally:
            with LOCK:
                CONNS.discard(c)
    while True:
        if s is None:
            THERE.wait()
            s = listen(host, port)
        log(f"{what} on {host}:{port}")
        try:
            while True:
                c, a = s.accept()
                c.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                with LOCK:
                    CONNS.add(c)
                threading.Thread(target=run, args=(c, a[0]), daemon=True).start()
        except OSError:
            pass                                         # vanished: its socket shut
        finally:
            with LOCK:
                LISTENING.pop(port, None)
            s.close()
            s = None


def main():
    global ARGS, PNG
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--host", default="127.0.0.1", help="the address to listen on (127.0.0.1)")
    p.add_argument("--port", type=int, default=8080, help="its own protocol's port (8080; 0: any free one)")
    p.add_argument("--kiwi-port", type=int, default=8073, help="its Kiwi input's port (8073; 0: none)")
    p.add_argument("--time-limit", type=float, default=0, help="a guest's session, in seconds (0: none)")
    p.add_argument("--idle-timeout", type=float, default=0, help="a guest's silence, in seconds (0: none)")
    p.add_argument("--day-limit", type=float, default=0, help="an address's seconds a day (0: none)")
    p.add_argument("--day-check", type=float, default=30, help="...enforced every so many seconds (30)")
    p.add_argument("--guests-limited", action="store_true", help="private addresses are guests too")
    p.add_argument("--password", default="", help="the bypass password")
    p.add_argument("--password-only", action="store_true", help="no guests")
    p.add_argument("--full", action="store_true", help="no room for a guest: 503")
    p.add_argument("--name", default="Mock UberSDR on the LAN", help="its name in its description")
    p.add_argument("--kiwi-flavour", choices=("ubersdr", "kiwisdr"), default="ubersdr")
    p.add_argument("--kiwi-cw", type=lambda v: tuple(int(x) for x in v.split(",")), default=(300, 700),
                   help="a KiwiSDR's CW passband, lo,hi (300,700)")
    ARGS = p.parse_args()
    STATE["full"] = ARGS.full
    Opus(RATE).close()                                   # libopus there, before anyone asks
    PNG = png()
    # Both ports held before a word is said, the Kiwi input's first: one
    # already taken ends the mock here, and --port 0 never picks the other.
    kiwi = listen(ARGS.host, ARGS.kiwi_port) if ARGS.kiwi_port else None
    own = listen(ARGS.host, ARGS.port)
    print(f"LISTENING {own.getsockname()[1]} {kiwi.getsockname()[1] if kiwi else 0}", flush=True)
    threading.Thread(target=enforce, daemon=True).start()
    if kiwi:
        threading.Thread(target=serve, args=(kiwi, kiwi_conn, "its Kiwi input"), daemon=True).start()
    serve(own, receiver_conn, "UberSDR, in the clear,")


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
