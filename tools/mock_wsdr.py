#!/usr/bin/env python3
"""A mock WebSDR (PA3FWM's server software): what the knob's WebSDR firmware
meets on a real one, from the PC, with no receiver contacted.

It serves, as WEBSDR-PROTOCOL.md describes a server:
  GET /tmp/bandinfo.js         the bands, in the server's own JavaScript form
                               -- Twente's 64 kB of it, mostly waterfall scale
                               names, or a smaller site's
  GET /websdr-sound.js         a stand-in of the page's sound script, saying
                               which stream path the site's page opens
  ws  /~~stream[?v=11]         one listener: the knob's tuning up as text
                               ("GET /~~param?f=...&band=..."), its audio
                               down as items -- S-meter, rate, step,
                               conversion, coded blocks (tools/wsdr_codec.py),
                               silence while muted or squelched
and on /mock/... (the same port, or --control's with --tls) what a test asks
of it: /mock/stats (what the knob sent and how), /mock/sent (every
command), /mock/set?k=v (max_users, others, station, stall, close), and
/mock/reset.

A station is a carrier on --station HZ: heard as a tone of its distance from
the carrier the knob is sent, where that lies in its passband -- SSB and CW
-- or as a 1 kHz tone in AM and FM; the S-meter -73 dBm (S9) on it, about
-121 off it.

Sites (--site):
  twente     one band, 0 to 29.16 MHz, 7119 Hz audio (twice that for AM and
             FM past 3.5 kHz); the page opens /~~stream; no idle timeout;
             AM sync and the newer keys known
  maasbree   eight bands, 160 m to 15 m, 8000 Hz audio; the page opens
             /~~stream?v=11; a 4 h idle timeout in bandinfo.js; Twente's
             newer keys (noisered, gain, agchang, squelchlevel) and AM sync
             unknown to it

Flags:
  --strict-path         the other stream path refused (404); otherwise both
                        taken, as nobody knows whether real ones refuse
  --origin              a WebSocket without the Origin its own page would
                        send (http://<Host>, https:// behind --tls) refused
                        (403)
  --max-users N         past N listeners (and --others), refused as the
                        server does: rate 0, then closed
  --idle-close S        the server closes a listener S s after its last
                        command (whether real servers do is not known)
  --stall T:D           at T s into each session, nothing for D s
  --drift-ppm P         its clock P ppm fast
  --close-after S       each session closed after S s of audio
  --tls CERT:KEY        behind a TLS front, as nginx puts one before some
  --bandinfo-kb K       bandinfo.js padded to about K kB with scale names
  --idle-page MS        the idle timeout bandinfo.js gives its page, in place
                        of the site's (a test's few seconds)
  --cut PATH:N[:K]      the first K (1) GETs of PATH ("websdr-sound.js") stop
                        after N bytes of its Content-Length, 7 s silent, then
                        closed -- as a site's server was seen pausing a file
"""
import argparse
import itertools
import json
import math
import os
import random
import select
import socket
import ssl
import sys
import threading
import time
import base64
import hashlib
from urllib.parse import parse_qs, urlsplit

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ws_server import OP_BIN, OP_CLOSE, OP_PING, OP_PONG, OP_TEXT, WSConn, WSError  # noqa: E402
from wsdr_codec import BLOCK, Encoder, meter  # noqa: E402

_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
ARGS = None
R = None
CONN_IDS = itertools.count(1)

SITES = {
    "twente": dict(
        path="/~~stream",
        bands=[("hf", 14579.8, 29159.6, 0.006952, 7.119043, 14589.8)],
        plan=[(5900.0, 6200.0), (9400.0, 9900.0),
              (135.7, 137.8), (472.0, 479.0), (1810.0, 1880.0), (3500.0, 3800.0), (7000.0, 7200.0),
              (10100.0, 10150.0), (14000.0, 14350.0), (18068.0, 18168.0), (21000.0, 21450.0),
              (24890.0, 24990.0), (28000.0, 29700.0)],
        ini_freq=-1.0, ini_mode="", idle_ms=0, newer=True, kb=60),
    "maasbree": dict(
        path="/~~stream?v=11",
        bands=[("160m", 1895.0, 192.0, 0.03125, 8.0, 1905.0), ("80m", 3660.0, 384.0, 0.03125, 8.0, 3670.0),
               ("60m", 5373.0, 192.0, 0.03125, 8.0, 5383.0), ("40m", 7100.0, 384.0, 0.03125, 8.0, 7110.0),
               ("30m", 10150.0, 192.0, 0.03125, 8.0, 10160.0), ("20m", 14175.0, 384.0, 0.03125, 8.0, 14185.0),
               ("17m", 18118.0, 192.0, 0.03125, 8.0, 18128.0), ("15m", 21225.0, 768.0, 0.03125, 8.0, 21235.0)],
        plan=[], ini_freq=3630.0, ini_mode="lsb", idle_ms=14400000, newer=False, kb=14),
}
KEYS = {"f", "band", "lo", "hi", "mode", "name", "mute", "squelch", "autonotch"}
NEWER_KEYS = {"noisered", "squelchlevel", "gain", "maxgain", "agchang"}


def log(*a):
    print(time.strftime("%H:%M:%S"), *a, file=sys.stderr, flush=True)


class Receiver:
    def __init__(self, a):
        self.lock = threading.Lock()
        self.site = SITES[a.site]
        self.stations = list(a.station) or [7075500.0, 14075500.0, 3630500.0, 1000000.0]
        self.max_users = a.max_users
        self.cuts = {}
        for c in a.cut:
            p, n, k = (c.split(":") + ["1"])[:3]
            self.cuts[p] = [int(n), int(k)]
        self.others = a.others
        self.stall_until = 0.0
        self.open = []
        self.sent = []
        self.reset_stats()

    def reset_stats(self):
        self.stats = dict(connections=0, upgrades=0, refused_busy=0, not_found=0, wrong_path=0, no_origin=0,
                          bandinfo=0, sound_js=0, index=0, commands=0, tunings=0, too_fast=0, unknown_keys=[],
                          foreign_keys=[], bad_values=[], outside=0, names=[], agents=[], origins=[], paths=[],
                          max_open=0, two_at_once=0, idle_closed=0, closed_by_mock=0, blocks=0, silent_blocks=0,
                          rates=[], tls=0, tls_failed=0, pings=0, cut=0)
        self.sent = []

    def bandinfo(self):
        """bandinfo.js as the server writes it, its scale image names padded
        out to the site's size (Twente's is about 64 kB)."""
        s = self.site
        want = (ARGS.bandinfo_kb if ARGS.bandinfo_kb >= 0 else s["kb"]) * 1024
        per = max(1, want // len(s["bands"]))
        out = [f"var nbands={len(s['bands'])};", f"var ini_freq={s['ini_freq']:.6f};",
               f"var ini_mode='{s['ini_mode']}';", "var chseq=2;", "var bandinfo= ["]
        for i, (name, c, sr, st, mbw, vfo) in enumerate(s["bands"]):
            rows, size, z = [], 0, 0
            while size < per:
                row = "      [" + ",".join(f'"tmp/1779402868-b{i}z{z}i{k}.png"' for k in range(min(2 ** z, 32))) + "]"
                rows.append(row)
                size += len(row)
                z += 1
            out.append(("," if i else "") + f"  {{ centerfreq: {c:.6f},\n    samplerate: {sr:.6f},\n"
                       f"    tuningstep: {st:.6f},\n    maxlinbw: {mbw:.6f},\n    vfo: {vfo:.6f},\n"
                       f"    maxzoom: {z},\n    name: '{name}',\n    scaleimgs: [\n" + ",\n".join(rows) + "\n    ]\n  }")
        out.append("];")
        if s["plan"]:
            out.append("var freqbands=[];")
            out += [f"freqbands.push( {{ min:{lo:.6f}, max:{hi:.6f} }} )" for lo, hi in s["plan"]]
        idle = ARGS.idle_page if ARGS.idle_page >= 0 else s["idle_ms"]
        out += ["var dxinfoavailable=0;", "var labelsavailable=0;", f"var idletimeout={idle};"]
        return "\n".join(out) + "\n"

    def sound_js(self):
        # Not the page's script: only the line a client reads the path from.
        return ("/* mock_wsdr.py: a stand-in for websdr-sound.js */\n" + "// " + "x" * 76 + "\n") * 60 + \
               (f'function soundinit(){{ ws=new WebSocket("ws://"+window.location.host+"{self.site["path"]}"); '
                'ws.binaryType="arraybuffer"; }\n')


class Session:
    def __init__(self, conn, ip, path, origin):
        self.conn, self.ip = conn, ip
        self.cid = next(CONN_IDS)
        b = R.site["bands"][0]
        self.band = 0
        self.f = b[5]                       # kHz
        self.lo, self.hi, self.mode = (-2.7, -0.3, 0) if R.site["ini_mode"] == "lsb" else (0.3, 2.7, 0)
        if R.site["ini_freq"] > 0:
            self.f = R.site["ini_freq"]
            self.band = next((i for i, x in enumerate(R.site["bands"]) if abs(self.f - x[1]) <= x[2] / 2), 0)
        self.name = ""
        self.mute = self.squelch = False
        self.enc = Encoder(step=40, conv=0x10)
        self.t_start = time.time()
        self.t_cmd = time.time()
        self.t_tune = []
        self.blocks = 0
        self.wlock = threading.Lock()

    def wire(self, data, op=OP_BIN):
        with self.wlock:
            self.conn.send(data, op)

    def close(self, why):
        log(f"{self.ip}#{self.cid}: closed: {why}")
        try:
            self.conn.close()
        except (OSError, WSError):
            pass

    # -- what the knob says ------------------------------------------------
    def on_text(self, text):
        now = time.time()
        with R.lock:
            R.stats["commands"] += 1
            R.sent.append((now, self.cid, self.ip, text))
            del R.sent[:-2000]
        if not text.startswith("GET /~~param?"):
            with R.lock:
                R.stats["bad_values"].append(text[:60])
            return
        q = parse_qs(text[len("GET /~~param?"):], keep_blank_values=True)
        p = {k: v[-1] for k, v in q.items()}
        self.t_cmd = now
        if "f" in p:                        # a tuning frame, its values right or not
            self.t_tune = [t for t in self.t_tune if now - t < 0.25] + [now]
            with R.lock:
                R.stats["tunings"] += 1
                if len(self.t_tune) > 1:
                    R.stats["too_fast"] += 1
        for k in p:
            if k not in KEYS and k not in NEWER_KEYS:
                with R.lock:
                    R.stats["unknown_keys"].append(k)
            elif k in NEWER_KEYS and not R.site["newer"]:
                with R.lock:
                    R.stats["foreign_keys"].append(k)
        try:
            if "band" in p:
                b = int(p["band"])
                if not 0 <= b < len(R.site["bands"]):
                    raise ValueError("band " + p["band"])
                self.band = b
            if "lo" in p:
                self.lo = float(p["lo"])
            if "hi" in p:
                self.hi = float(p["hi"])
            if "mode" in p:
                m = int(p["mode"])
                if m not in (0, 1, 2, 4) or (m == 2 and not R.site["newer"]):
                    raise ValueError("mode " + p["mode"])
                self.mode = m
            if "f" in p:
                f = float(p["f"])
                _, c, sr, _, _, _ = R.site["bands"][self.band]
                if abs(f - c) > sr / 2 + 4:
                    with R.lock:
                        R.stats["outside"] += 1
                    f = max(c - sr / 2, min(c + sr / 2, f))        # as VertexSDR clamps it
                self.f = f
            if "name" in p:
                with R.lock:
                    if p["name"] not in R.stats["names"]:
                        R.stats["names"].append(p["name"])
            if "mute" in p:
                self.mute = p["mute"] == "1"
            if "squelch" in p:
                self.squelch = p["squelch"] == "1"
            if self.lo > self.hi:
                raise ValueError(f"lo {self.lo} > hi {self.hi}")
            lim = 15.0 if self.mode == 4 else R.site["bands"][self.band][4] * 0.95
            if max(abs(self.lo), abs(self.hi)) > lim + 1e-6:
                raise ValueError(f"passband {self.lo}..{self.hi} past {lim:.2f}")
        except ValueError as e:
            with R.lock:
                R.stats["bad_values"].append(str(e))

    # -- what it sends -----------------------------------------------------
    def rate(self):
        r = round(R.site["bands"][self.band][4] * 1000)
        return 2 * r if max(abs(self.lo), abs(self.hi)) > 3.5 else r

    def conv(self):
        one_side = self.lo * self.hi > 0
        return 0x10 if one_side and self.mode not in (1, 2) else 0x00

    def heard(self):
        """(tone Hz, dBm): the station the passband holds, if one."""
        car = self.f * 1000
        for s in R.stations:
            off = s - car
            if self.mode == 0 and self.lo * 1000 <= off <= self.hi * 1000:
                return abs(off), -73.0
            if self.mode != 0 and abs(off) <= max(abs(self.lo), abs(self.hi)) * 1000:
                return 1000.0, -73.0
        return 0.0, -121.0

    def stalled(self):
        el = time.time() - self.t_start
        return time.time() < R.stall_until or any(t <= el < t + d for t, d in ARGS.stall)

    def pump(self):
        rnd = random.Random(self.cid)
        tick, t = 0.04, time.monotonic()
        owed, phase, pcm, held = 0.0, 0.0, [], []
        while not self.conn.closed:
            t += tick
            time.sleep(max(0.0, t - time.monotonic()))
            if ARGS.close_after and time.time() - self.t_start >= ARGS.close_after:
                with R.lock:
                    R.stats["closed_by_mock"] += 1
                return self.close("--close-after")
            if ARGS.idle_close and time.time() - self.t_cmd >= ARGS.idle_close:
                with R.lock:
                    R.stats["idle_closed"] += 1
                return self.close("idle")
            rate = self.rate()
            owed += rate * tick * (1 + ARGS.drift_ppm / 1e6)
            n = int(owed)
            owed -= n
            pitch, dbm = self.heard()
            for _ in range(n):
                phase += 2 * math.pi * pitch / rate
                pcm.append(max(-32768, min(32767, int((6000 * math.sin(phase) if pitch else 0) + rnd.gauss(0, 150)))))
            phase %= 2 * math.pi
            while len(pcm) >= BLOCK:
                x, pcm = pcm[:BLOCK], pcm[BLOCK:]
                m = bytearray(self.enc.header(rate=rate, step=40, conv=self.conv()))
                if self.blocks % 8 == 0:
                    m += meter(dbm + rnd.gauss(0, 1.5))
                if self.mute or (self.squelch and not pitch):
                    m += b"\x84"
                    self.enc.st.forget()
                    with R.lock:
                        R.stats["silent_blocks"] += 1
                else:
                    width = 3 if not pitch else 4 if self.blocks % 50 < 40 else 5
                    b, _ = self.enc.block(x, width, fast=True)
                    m += b
                self.blocks += 1
                with R.lock:
                    R.stats["blocks"] += 1
                    if not R.stats["rates"] or R.stats["rates"][-1] != rate:
                        R.stats["rates"].append(rate)
                held.append(bytes(m))
            if self.stalled():
                continue
            for m in held:
                try:
                    self.wire(m)
                except (OSError, WSError):
                    return
            held = []


# --------------------------------------------------------------- the wire

def reply(sock, code, body, ctype="text/plain", path=""):
    if isinstance(body, str):
        body = body.encode()
    reason = {200: "OK", 403: "Forbidden", 404: "Not Found"}.get(code, "")
    head = (f"HTTP/1.1 {code} {reason}\r\nContent-Type: {ctype}\r\nContent-Length: {len(body)}\r\n"
            "Connection: close\r\n\r\n").encode()
    with R.lock:
        cut = path and path in R.cuts and R.cuts[path][1] > 0
        if cut:
            n = R.cuts[path][0]
            R.cuts[path][1] -= 1
            R.stats["cut"] += 1
    if cut:
        sock.sendall(head + body[:n])
        time.sleep(7)
        return
    sock.sendall(head + body)


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


def control(sock, url):
    q = {k: v[-1] for k, v in parse_qs(url.query, keep_blank_values=True).items()}
    if url.path == "/mock/sent":
        with R.lock:
            out = [dict(t=t, c=c, ip=ip, text=text) for t, c, ip, text in R.sent]
        return reply(sock, 200, json.dumps(out), "application/json")
    closing = []
    with R.lock:
        if url.path == "/mock/set":
            for k, v in q.items():
                if k in ("max_users", "others"):
                    setattr(R, k, int(v))
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
        out = dict(R.stats, live=len(R.open), site=ARGS.site)
    for c in closing:
        c.close("the mock's /mock/set")
    reply(sock, 200, json.dumps(out), "application/json")


def on_client(sock, addr, control_ok=True, tls=False):
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
            ua = hdr.get("user-agent", "")
            if ua not in R.stats["agents"]:
                R.stats["agents"] = (R.stats["agents"] + [ua])[-10:]
        if url.path == "/tmp/bandinfo.js":
            with R.lock:
                R.stats["bandinfo"] += 1
            reply(sock, 200, R.bandinfo(), "application/javascript", "tmp/bandinfo.js")
            return sock.close()
        if url.path == "/websdr-sound.js":
            with R.lock:
                R.stats["sound_js"] += 1
            reply(sock, 200, R.sound_js(), "application/javascript", "websdr-sound.js")
            return sock.close()
        if url.path in ("/", "/index.html"):
            with R.lock:
                R.stats["index"] += 1
            reply(sock, 200, "<html><head><title>WebSDR</title></head><body>mock</body></html>", "text/html")
            return sock.close()
        if url.path != "/~~stream" or hdr.get("upgrade", "").lower() != "websocket" or not hdr.get("sec-websocket-key"):
            with R.lock:
                R.stats["not_found"] += 1
            reply(sock, 404, "not here")
            return sock.close()
        with R.lock:
            R.stats["paths"] = (R.stats["paths"] + [target])[-10:]
            if target != R.site["path"]:
                R.stats["wrong_path"] += 1
        if ARGS.strict_path and target != R.site["path"]:
            reply(sock, 404, "not here")
            return sock.close()
        origin = hdr.get("origin", "")
        with R.lock:
            R.stats["origins"] = (R.stats["origins"] + [origin])[-10:]
        page = ("https://" if tls else "http://") + hdr.get("host", "")   # where its own page is
        if ARGS.origin and origin != page:
            with R.lock:
                R.stats["no_origin"] += 1
            reply(sock, 403, "no")
            return sock.close()
        accept = base64.b64encode(hashlib.sha1((hdr["sec-websocket-key"] + _GUID).encode()).digest()).decode()
        sock.sendall(("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                      f"Connection: Upgrade\r\nSec-WebSocket-Accept: {accept}\r\n\r\n").encode())
        conn = WSConn(sock, addr)
        s = Session(conn, ip, target, origin)
        with R.lock:
            R.stats["upgrades"] += 1
            busy = R.max_users and len(R.open) + R.others >= R.max_users
            if busy:
                R.stats["refused_busy"] += 1
            else:
                R.open.append(s)
                R.stats["max_open"] = max(R.stats["max_open"], len(R.open))
                if len(R.open) > 1:
                    R.stats["two_at_once"] += 1
        if busy:
            log(f"{ip}#{s.cid}: busy: rate 0")
            try:
                conn.send(b"\x81\x00\x00", OP_BIN)
                time.sleep(0.2)
                conn.close()
            except (OSError, WSError):
                pass
            return
        log(f"{ip}#{s.cid}: listening on {target}, Origin {origin or '-'}")
        threading.Thread(target=s.pump, daemon=True).start()
        try:
            run(s)
        finally:
            with R.lock:
                if s in R.open:
                    R.open.remove(s)
            try:
                conn.close()
                conn.sock.close()
            except (OSError, WSError):
                pass
            log(f"{ip}#{s.cid}: gone")
    except (OSError, WSError):
        try:
            sock.close()
        except OSError:
            pass


def run(s):
    sock = s.conn.sock
    while not s.conn.closed:
        try:
            pending = isinstance(sock, ssl.SSLSocket) and sock.pending()
            if not pending and not select.select([sock], [], [], 1.0)[0]:
                continue
            op, data = s.conn.recv_frame()
        except (OSError, WSError, ValueError):
            return
        if op == OP_CLOSE:
            return
        if op == OP_PING:
            s.wire(data, OP_PONG)
        elif op == OP_TEXT:
            s.on_text(data.decode("utf-8", "replace"))


# ------------------------------------------------------------------ fronts

TLS_CTX = None


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
    """TLS ended here, the server's own HTTP behind it, as nginx has it."""
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
    threading.Thread(target=on_client, args=(b, addr, False, True), daemon=True).start()
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
    global ARGS, R, TLS_CTX
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8901)
    ap.add_argument("--site", choices=sorted(SITES), default="twente")
    ap.add_argument("--station", type=float, action="append", default=[], metavar="HZ")
    ap.add_argument("--strict-path", action="store_true")
    ap.add_argument("--origin", action="store_true")
    ap.add_argument("--max-users", type=int, default=0)
    ap.add_argument("--others", type=int, default=0)
    ap.add_argument("--idle-close", type=float, default=0.0, metavar="S")
    ap.add_argument("--stall", type=stall_arg, action="append", default=[], metavar="T:D")
    ap.add_argument("--drift-ppm", type=float, default=0.0)
    ap.add_argument("--close-after", type=float, default=0.0)
    ap.add_argument("--tls", default="", metavar="CERT:KEY")
    ap.add_argument("--bandinfo-kb", type=int, default=-1, metavar="K")
    ap.add_argument("--idle-page", type=int, default=-1, metavar="MS")
    ap.add_argument("--cut", action="append", default=[], metavar="PATH:N[:K]")
    ARGS = ap.parse_args()
    R = Receiver(ARGS)
    if ARGS.tls:
        cert, _, key = ARGS.tls.partition(":")
        TLS_CTX = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        TLS_CTX.load_cert_chain(cert, key or None)
    srv = listen(ARGS.port)
    print(f"LISTENING {srv.getsockname()[1]}", flush=True)
    if TLS_CTX:
        ctl = listen()
        print(f"CONTROL {ctl.getsockname()[1]}", flush=True)
        threading.Thread(target=serve, args=(ctl, control_client), daemon=True).start()
    log(f"a mock WebSDR ({ARGS.site}) on {ARGS.host}:{srv.getsockname()[1]}" + (" behind TLS" if TLS_CTX else ""))
    try:
        serve(srv, front if TLS_CTX else on_client)
    except KeyboardInterrupt:
        with R.lock:
            log(f"stats: {json.dumps(R.stats)}")


if __name__ == "__main__":
    sys.exit(main())
