#!/usr/bin/env python3
"""tools/mock_wsdr.py, tried by a client shaped as a WebSDR's own page:
bandinfo.js and the sound script fetched, the stream opened with an Origin,
the tuning sent, its audio decoded (tools/wsdr_codec.py), the S-meter -- and
every case the knob must meet: Twente's one wide band and its 64 kB
bandinfo.js, a dist11 site's bands and its ?v=11 path, AM's doubled rate,
mute, a band change, keys and a mode a dist11 server does not know, a
frequency outside the band, a busy server, a strict path, an Origin check,
a server that closes an idle listener, a stall, TLS in front.

  wsdr_mock.py --mock tools/mock_wsdr.py [--no-tls] [--only NAME]

Loopback only: run it in a network namespace of its own to be sure of it
(test/host/CMakeLists.txt says how)."""
import argparse
import base64
import json
import os
import re
import shutil
import socket
import ssl
import struct
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "tools"))
sys.dont_write_bytecode = True
from wsdr_codec import Decoder  # noqa: E402

ARGS = None
FAILS = []


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what, flush=True)
    if not cond:
        FAILS.append(what)


class Mock:
    def __init__(self, *flags):
        self.p = subprocess.Popen([sys.executable, ARGS.mock, "--port", "0", *flags],
                                  stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        self.port = self.control = None
        while True:
            line = self.p.stdout.readline()
            if not line:
                raise RuntimeError("the mock did not start")
            w = line.split()
            if w[0] == "LISTENING":
                self.port = int(w[1])
            elif w[0] == "CONTROL":
                self.control = int(w[1])
            if self.port and (self.control or "--tls" not in flags):
                break
        self.control = self.control or self.port

    def get(self, path):
        s = socket.create_connection(("127.0.0.1", self.control), timeout=5)
        s.sendall(f"GET {path} HTTP/1.1\r\nHost: x\r\n\r\n".encode())
        data = b""
        while True:
            c = s.recv(65536)
            if not c:
                break
            data += c
        s.close()
        return json.loads(data.split(b"\r\n\r\n", 1)[1])

    def stats(self):
        return self.get("/mock/stats")

    def stop(self):
        self.p.terminate()
        try:
            self.p.wait(5)
        except subprocess.TimeoutExpired:
            self.p.kill()


def http_get(port, path, tls=None):
    s = socket.create_connection(("127.0.0.1", port), timeout=5)
    if tls:
        s = tls.wrap_socket(s, server_hostname="localhost")
    s.sendall(f"GET {path} HTTP/1.1\r\nHost: localhost:{port}\r\nUser-Agent: wsdr_mock.py\r\n"
              "Connection: close\r\n\r\n".encode())
    data = b""
    while True:
        c = s.recv(65536)
        if not c:
            break
        data += c
    s.close()
    head, _, body = data.partition(b"\r\n\r\n")
    return int(head.split()[1]), body


class Client:
    """The page's stream: opened with its Origin, frames read, audio
    decoded."""

    def __init__(self, port, path="/~~stream", origin=True, tls=None):
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.settimeout(10)
        s.connect(("127.0.0.1", port))
        if tls:
            s = tls.wrap_socket(s, server_hostname="localhost")
        key = base64.b64encode(os.urandom(16)).decode()
        o = f"Origin: http://localhost:{port}\r\n" if origin is True else f"Origin: {origin}\r\n" if origin else ""
        s.sendall(f"GET {path} HTTP/1.1\r\nHost: localhost:{port}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                  f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n{o}"
                  "User-Agent: wsdr_mock.py\r\n\r\n".encode())
        data = b""
        while b"\r\n\r\n" not in data:
            c = s.recv(4096)
            if not c:
                raise ConnectionError("closed during the upgrade")
            data += c
        head, _, self.buf = data.partition(b"\r\n\r\n")
        self.status = int(head.split()[1])
        self.s = s
        self.dec = Decoder()
        self.pcm = []
        self.samples = 0
        self.rates = []
        self.frames = []                    # arrival times
        self.closed = False

    def _need(self, n):
        while len(self.buf) < n:
            c = self.s.recv(65536)
            if not c:
                raise ConnectionError("closed")
            self.buf += c
        out, self.buf = self.buf[:n], self.buf[n:]
        return out

    def send(self, text, op=1):
        p = text.encode() if isinstance(text, str) else text
        mask = os.urandom(4)
        n = len(p)
        hdr = bytes([0x80 | op]) + (bytes([0x80 | n]) if n < 126 else bytes([0x80 | 126]) + struct.pack("!H", n))
        self.s.sendall(hdr + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(p)))

    def param(self, q):
        self.send("GET /~~param?" + q)

    def frame(self):
        b0, b1 = self._need(2)
        n = b1 & 0x7F
        if n == 126:
            n = struct.unpack("!H", self._need(2))[0]
        elif n == 127:
            n = struct.unpack("!Q", self._need(8))[0]
        return b0 & 0x0F, self._need(n)

    def pump(self, seconds):
        end = time.time() + seconds
        self.s.settimeout(0.5)
        while time.time() < end:
            try:
                op, p = self.frame()
            except socket.timeout:
                continue
            except (ConnectionError, OSError):
                self.closed = True
                return
            if op == 8:
                self.closed = True
                return
            if op == 9:
                self.send(p, 10)
            elif op == 2:
                self.frames.append(time.time())
                got = self.dec.feed(p)
                if self.dec.rate is not None and (not self.rates or self.rates[-1] != self.dec.rate):
                    self.rates.append(self.dec.rate)
                self.samples += len(got)
                self.pcm += got
                del self.pcm[:-30000]

    def close(self):
        try:
            self.send(struct.pack("!H", 1000), 8)
            self.s.close()
        except OSError:
            pass


def pitch(pcm, rate):
    x = pcm[-rate:]
    if len(x) < rate // 2:
        return 0.0
    mean = sum(x) / len(x)
    z = sum(1 for a, b in zip(x, x[1:]) if (a - mean) < 0 <= (b - mean))
    return z * rate / len(x)


# ---------------------------------------------------------------- the cases

def case_twente(d):
    print("twente: one wide band, its 64 kB bandinfo.js, /~~stream; USB, mute, AM's doubled rate")
    m = Mock("--site", "twente")
    try:
        st, bi = http_get(m.port, "/tmp/bandinfo.js")
        bi = bi.decode()
        check(st == 200 and len(bi) > 60000 and "var nbands=1;" in bi and bi.count("freqbands.push(") == 13,
              f"bandinfo.js: {len(bi)} bytes, one band, its band plan")
        st, js = http_get(m.port, "/websdr-sound.js")
        check(st == 200 and b"/~~stream\"" in js and b"v=11" not in js, "its page opens /~~stream")
        c = Client(m.port, "/~~stream")
        check(c.status == 101, f"upgraded: {c.status}")
        c.pump(0.5)
        check(c.rates[:1] == [7119], f"the rate before any tuning: {c.rates}")
        c.param("f=14074.000&band=0&lo=0.3&hi=2.7&mode=0&name=MOCKTEST")
        for k in ("mute=0", "squelch=0", "autonotch=0"):
            c.param(k)
        c.pump(1.0)
        n0, t0 = c.samples, time.time()
        c.pump(5)
        el = time.time() - t0
        got = c.samples - n0
        check(abs(got - 7119 * el) < 7119 * el * 0.15, f"7119 Hz of audio: {got} samples in {el:.1f} s")
        p = pitch(c.pcm, 7119)
        check(1450 < p < 1550, f"the station at 14075.5 heard at 1.5 kHz: {p:.0f} Hz")
        mt = c.dec.meters[-5:]
        check(mt and all(-80 < x < -66 for x in mt), f"S-meter on it about -73 dBm: {[round(x) for x in mt]}")
        sil = c.dec.silent
        c.param("mute=1")
        c.pump(2)
        check(c.dec.silent - sil > 80, f"muted: silence items, {c.dec.silent - sil} in 2 s")
        c.param("mute=0")
        c.param("f=1000.000&band=0&lo=-4.5&hi=4.5&mode=1&name=MOCKTEST")
        c.pump(3)
        check(c.rates[-1] == 14238, f"AM past 3.5 kHz: the rate doubled, {c.rates}")
        p = pitch(c.pcm, 14238)
        check(950 < p < 1050, f"AM's 1 kHz tone: {p:.0f} Hz")
        c.close()
        s = m.stats()
        check(not s["unknown_keys"] and not s["foreign_keys"] and not s["bad_values"] and not s["outside"],
              f"all it was sent, known: {s['unknown_keys']} {s['foreign_keys']} {s['bad_values']}")
        check("MOCKTEST" in s["names"] and s["tunings"] == 2 and s["too_fast"] == 0,
              f"the name, two tunings, none too fast: {s['names']} {s['tunings']} {s['too_fast']}")
        check(s["paths"] == ["/~~stream"] and s["origins"][-1].startswith("http://localhost:"), "path and Origin seen")
    finally:
        m.stop()


def case_maasbree(d):
    print("maasbree: eight bands, ?v=11, a band change; what a dist11 server does not know")
    m = Mock("--site", "maasbree")
    try:
        st, bi = http_get(m.port, "/tmp/bandinfo.js")
        bi = bi.decode()
        check(st == 200 and "var nbands=8;" in bi and "name: '40m'" in bi and "var idletimeout=14400000;" in bi,
              "bandinfo.js: eight bands, a 4 h idle timeout")
        st, js = http_get(m.port, "/websdr-sound.js")
        check(b"/~~stream?v=11\"" in js, "its page opens /~~stream?v=11")
        c = Client(m.port, "/~~stream?v=11")
        c.pump(0.5)
        check(c.rates[:1] == [8000], f"8000 Hz: {c.rates}")
        c.param("f=7074.000&band=3&lo=0.3&hi=2.7&mode=0&name=")
        c.pump(3)
        p = pitch(c.pcm, 8000)
        check(1450 < p < 1550, f"band 3, 40 m: the station at 7075.5 at 1.5 kHz: {p:.0f} Hz")
        c.param("noisered=-999")
        c.param("f=7074.000&band=3&lo=-4.5&hi=4.5&mode=2&name=")
        c.param("f=9000.000&band=3&lo=0.3&hi=2.7&mode=0&name=")
        c.pump(1)
        c.close()
        s = m.stats()
        check(s["foreign_keys"] == ["noisered"], f"Twente's noisered unknown here: {s['foreign_keys']}")
        check(any("mode 2" in x for x in s["bad_values"]), f"AM sync unknown here: {s['bad_values']}")
        check(s["outside"] == 1, "a frequency outside its band seen")
        check(s["too_fast"] >= 1, "two tunings within 250 ms seen")
    finally:
        m.stop()


def case_refusals(d):
    print("refusals: busy, a strict path, an Origin check")
    m = Mock("--max-users", "1", "--others", "1")
    try:
        c = Client(m.port)
        c.pump(2)
        check(c.rates == [0] and c.closed, f"busy: rate 0, then closed ({c.rates}, closed {c.closed})")
        check(m.stats()["refused_busy"] == 1, "counted")
    finally:
        m.stop()
    m = Mock("--site", "maasbree", "--strict-path")
    try:
        c = Client(m.port, "/~~stream")
        check(c.status == 404, f"the other path refused: {c.status}")
        c = Client(m.port, "/~~stream?v=11")
        check(c.status == 101, f"its own taken: {c.status}")
        c.close()
    finally:
        m.stop()
    m = Mock("--origin")
    try:
        check(Client(m.port, origin=None).status == 403, "no Origin: 403")
        check(Client(m.port, origin="http://elsewhere").status == 403, "another Origin: 403")
        c = Client(m.port, origin=True)
        check(c.status == 101, "its own page's Origin: 101")
        c.close()
        check(m.stats()["no_origin"] == 2, "counted")
    finally:
        m.stop()


def case_idle(d):
    print("a server that closes an idle listener")
    m = Mock("--idle-close", "2")
    try:
        a, b = Client(m.port), Client(m.port)
        a.param("f=14074.000&band=0&lo=0.3&hi=2.7&mode=0&name=")
        b.param("f=14074.000&band=0&lo=0.3&hi=2.7&mode=0&name=")
        for _ in range(4):
            b.param("f=14074.100&band=0&lo=0.3&hi=2.7&mode=0&name=")
            a.pump(0.5)
            b.pump(0.5)
        check(a.closed and not b.closed, f"the idle one closed, the tuning one kept: {a.closed} {b.closed}")
        check(m.stats()["idle_closed"] == 1, "counted")
        b.close()
    finally:
        m.stop()


def case_stall(d):
    print("a stall: nothing for 3 s, then what was held")
    m = Mock("--stall", "2:3")
    try:
        c = Client(m.port)
        c.param("f=14074.000&band=0&lo=0.3&hi=2.7&mode=0&name=")
        c.pump(7)
        gaps = [b - a for a, b in zip(c.frames, c.frames[1:])]
        check(gaps and max(gaps) > 2.5, f"the gap: {max(gaps):.1f} s")
        check(c.samples > 7119 * 6 * 0.85, f"the held audio delivered after it: {c.samples} samples in 7 s")
        c.close()
    finally:
        m.stop()


def make_certs(d):
    if not shutil.which("openssl"):
        return None
    run = lambda *a: subprocess.run(["openssl", *a], cwd=d, check=True, capture_output=True)
    run("req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", "k.pem", "-out", "c.pem", "-days", "2",
        "-subj", "/CN=localhost", "-addext", "subjectAltName=DNS:localhost")
    return os.path.join(d, "c.pem"), os.path.join(d, "k.pem")


def case_tls(d):
    print("TLS in front, as nginx puts it before some sites")
    certs = make_certs(d)
    if not certs:
        print("  skip (no openssl)")
        return
    m = Mock("--tls", f"{certs[0]}:{certs[1]}")
    try:
        ctx = ssl.create_default_context(cafile=certs[0])
        st, bi = http_get(m.port, "/tmp/bandinfo.js", tls=ctx)
        check(st == 200 and b"bandinfo" in bi, "bandinfo.js over TLS")
        c = Client(m.port, tls=ctx)
        c.param("f=14074.000&band=0&lo=0.3&hi=2.7&mode=0&name=")
        c.pump(3)
        check(c.status == 101 and 1450 < pitch(c.pcm, 7119) < 1550, "wss: the station heard")
        c.close()
        check(m.stats()["tls"] >= 2, "through the front")
    finally:
        m.stop()


CASES = {"twente": case_twente, "maasbree": case_maasbree, "refusals": case_refusals, "idle": case_idle,
         "stall": case_stall, "tls": case_tls}


def main():
    global ARGS
    ap = argparse.ArgumentParser()
    ap.add_argument("--mock", required=True)
    ap.add_argument("--no-tls", action="store_true")
    ap.add_argument("--only", default="")
    ARGS = ap.parse_args()
    with tempfile.TemporaryDirectory() as d:
        for name, fn in CASES.items():
            if ARGS.only and name not in ARGS.only.split(","):
                continue
            if name == "tls" and ARGS.no_tls:
                continue
            fn(d)
    print(f"{len(FAILS)} failed" + ("".join("\n  " + x for x in FAILS) if FAILS else ""))
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
