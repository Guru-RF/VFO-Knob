#!/usr/bin/env python3
"""tools/mock_owrx.py, tried by a client shaped as the receivers' own page:
the handshake, connectionproperties, the DSP's params and start, its audio
decoded (tools/owrx_adpcm.py, the SYNCs found as they come), the S-meter,
the waterfall drained -- and every case the knob must meet: a receiver under
a path, behind TLS and behind a redirect to it, full, banned, silent, without
an SDR, another listener's band change, a locked band, band changes too
quick on OpenWebRX+ and the same on upstream, keys a fork does not know, a
client that stops reading, 300 kB of bookmarks, audio uncompressed, WFM's
48 kHz.

  owrx_mock.py --mock tools/mock_owrx.py [--no-tls] [--only NAME]

Loopback only: run it in a network namespace of its own to be sure of it
(test/host/CMakeLists.txt says how)."""
import argparse
import base64
import json
import math
import os
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
from owrx_adpcm import Decoder  # noqa: E402

ARGS = None
FAILS = []


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what, flush=True)
    if not cond:
        FAILS.append(what)


# ---------------------------------------------------------------- the mock

class Mock:
    def __init__(self, *flags):
        self.p = subprocess.Popen([sys.executable, ARGS.mock, "--port", "0", *flags],
                                  stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        self.port = self.control = self.redirect = None
        while True:
            line = self.p.stdout.readline()
            if not line:
                raise RuntimeError("the mock did not start")
            w = line.split()
            if w[0] == "LISTENING":
                self.port = int(w[1])
            elif w[0] == "CONTROL":
                self.control = int(w[1])
            elif w[0] == "REDIRECTING":
                self.redirect = int(w[1])
            if self.port and (self.control or "--tls" not in flags) and (self.redirect or "--redirect" not in flags):
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


# ------------------------------------------------------- a page's client

def http_get(port, path, tls=None, host="127.0.0.1"):
    s = socket.create_connection(("127.0.0.1", port), timeout=5)
    if tls:
        s = tls.wrap_socket(s, server_hostname="localhost")
    s.sendall(f"GET {path} HTTP/1.1\r\nHost: {host}:{port}\r\nConnection: close\r\n\r\n".encode())
    data = b""
    while True:
        c = s.recv(65536)
        if not c:
            break
        data += c
    s.close()
    head, _, body = data.partition(b"\r\n\r\n")
    status = int(head.split()[1])
    loc = next((l.split(b":", 1)[1].strip().decode() for l in head.split(b"\r\n") if l.lower().startswith(b"location:")), "")
    return status, body, loc


class Client:
    def __init__(self, port, path="/ws/", tls=None, rcvbuf=0):
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        if rcvbuf:
            s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, rcvbuf)   # before the window is offered
        s.settimeout(10)
        s.connect(("127.0.0.1", port))
        if tls:
            s = tls.wrap_socket(s, server_hostname="localhost")
        key = base64.b64encode(os.urandom(16)).decode()
        s.sendall(f"GET {path} HTTP/1.1\r\nHost: localhost:{port}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                  f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n".encode())
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
        self.samples = {2: 0, 4: 0}
        self.pcm = []
        self.fft = 0
        self.texts = []
        self.big = 0
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
        hdr = bytes([0x80 | op]) + (bytes([0x80 | n]) if n < 126 else
                                     bytes([0x80 | 126]) + struct.pack("!H", n) if n < 65536 else
                                     bytes([0x80 | 127]) + struct.pack("!Q", n))
        self.s.sendall(hdr + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(p)))

    def frame(self):
        b0, b1 = self._need(2)
        n = b1 & 0x7F
        if n == 126:
            n = struct.unpack("!H", self._need(2))[0]
        elif n == 127:
            n = struct.unpack("!Q", self._need(8))[0]
        return b0 & 0x0F, self._need(n)

    def pump(self, seconds, raw=False, until=None):
        """Every frame for `seconds`, as the page takes them: audio decoded,
        the waterfall counted and let go, the text kept."""
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
                self.send(b"", 10)
            elif op == 1:
                t = p.decode()
                if len(p) > 65535:
                    self.big += 1
                if t.startswith("{"):
                    m = json.loads(t)
                    self.texts.append(m)
                    if until and until(m):
                        return m
                else:
                    self.texts.append(t)
            elif op == 2 and p:
                if p[0] in (2, 4):
                    if raw:
                        got = list(struct.unpack("<%dh" % ((len(p) - 1) // 2), p[1:1 + (len(p) - 1) // 2 * 2]))
                    else:
                        got = self.dec.feed(p[1:])
                    self.samples[p[0]] += len(got)
                    self.pcm += got
                    del self.pcm[:-48000]
                elif p[0] in (1, 3):
                    self.fft += 1
        return None

    def of(self, kind):
        return [m for m in self.texts if isinstance(m, dict) and m.get("type") == kind]

    def config(self):
        c = {}
        for m in self.of("config"):
            c.update({k: v for k, v in m["value"].items() if v is not None})
        return c

    def hello(self):
        self.send("SERVER DE CLIENT client=owrx_mock.py type=receiver")
        self.send(json.dumps({"type": "connectionproperties", "params": {"output_rate": 12000, "hd_output_rate": 48000}}))

    def tune(self, hz, mod, lo, hi):
        c = self.config()
        self.send(json.dumps({"type": "dspcontrol", "params": {"low_cut": lo, "high_cut": hi,
                  "offset_freq": int(hz - c["center_freq"]), "mod": mod, "squelch_level": -150,
                  "secondary_mod": False}}))
        self.send(json.dumps({"type": "dspcontrol", "action": "start"}))

    def close(self):
        try:
            self.send(struct.pack("!H", 1000), 8)
            self.s.close()
        except OSError:
            pass


def pitch(pcm, rate):
    """A tone's pitch, by its zero crossings."""
    x = pcm[-rate:]
    if len(x) < rate // 2:
        return 0.0
    mean = sum(x) / len(x)
    z = sum(1 for a, b in zip(x, x[1:]) if (a - mean) < 0 <= (b - mean))
    return z * rate / len(x)


def start(port, path="/ws/", tls=None, rcvbuf=0):
    c = Client(port, path, tls, rcvbuf)
    c.hello()
    c.pump(3, until=lambda m: m.get("type") == "config" and "center_freq" in m.get("value", {}))
    return c


# ---------------------------------------------------------------- the cases

def case_play(fork):
    print(f"play, {fork}")
    m = Mock("--fork", fork)
    try:
        c = start(m.port)
        check(isinstance(c.texts[0], str) and c.texts[0].startswith("CLIENT DE SERVER server=openwebrx version=v"),
              "the hello first: " + str(c.texts[0]))
        cfg = c.config()
        check(cfg.get("center_freq") == 7100000 and cfg.get("samp_rate") == 500000, "the first band's span")
        check(("tuning_step" in cfg) == (fork == "plus"), "tuning_step only on OpenWebRX+")
        c.pump(1)
        before = c.samples[2]
        check(before == 0, "no audio before start")
        c.tune(7074000 + 1500 - 1000, "usb", 300, 3000)     # the mock's carrier at 7075500: a 1 kHz tone
        t0 = time.time()
        c.pump(6)
        el = time.time() - t0
        got = c.samples[2]
        check(abs(got - 12000 * el) < 12000 * 0.6, f"12 kHz of audio: {got} samples in {el:.1f} s")
        check(c.dec.lost == 0 and c.dec.syncs >= 5, f"decoded cleanly: {c.dec.syncs} SYNCs, {c.dec.lost} bytes lost")
        p = pitch(c.pcm, 12000)
        check(900 < p < 1100, f"the carrier heard at 1 kHz: {p:.0f} Hz")
        check(len(c.of("smeter")) >= 15, f"the S-meter, 4 a second: {len(c.of('smeter'))}")
        check(c.fft >= 9 * 6, f"the waterfall drained: {c.fft} frames")
        prof = c.of("profiles")
        check(prof and len(prof[-1]["value"]) == 7, "seven bands")
        check(len(c.of("bookmarks")) == 1 and len(c.of("modes")) == 1, "bookmarks, modes")
        # Tuned by offset alone, as a detent sends.
        c.send(json.dumps({"type": "dspcontrol", "params": {"offset_freq": 7075500 - 7100000 - 2000}}))
        c.pump(2)
        p = pitch(c.pcm, 12000)
        check(1900 < p < 2100, f"a detent away: {p:.0f} Hz")
        st = m.stats()
        check(st["starts"] == 1 and not st["unknown_keys"] and not st["bad_values"], "what it sent, all known")
        check(st["output_rates"] == [[12000, 48000]], "connectionproperties as a browser's")
        c.close()
    finally:
        m.stop()


def case_wfm_and_raw():
    print("WFM at 48 kHz; audio uncompressed")
    m = Mock("--audio", "none")
    try:
        c = start(m.port)
        c.tune(7075500, "usb", 300, 3000)
        c.pump(3, raw=True)
        check(c.samples[2] > 12000 * 2 and c.dec.syncs == 0, "int16 at 12 kHz, no ADPCM")
        c.close()
    finally:
        m.stop()
    m = Mock()
    try:
        c = start(m.port)
        c.send(json.dumps({"type": "selectprofile", "params": {"profile": "8c0f6e3a-7c1d-4e2b-9a55-1f2e3d4c5b6a|2c3d4e5f-6071-4829-9bac-1d2e3f4a5b6c"}}))
        c.pump(2, until=lambda x: x.get("type") == "config" and x["value"].get("center_freq") == 98000000)
        c.tune(98500000, "wfm", -75000, 75000)
        c.pump(4)
        check(c.samples[4] > 48000 * 2 and c.samples[2] == 0, f"0x04 frames at 48 kHz: {c.samples[4]}")
        check(c.dec.lost == 0, "one decoder for both kinds")
        c.close()
    finally:
        m.stop()


def case_prefix():
    print("under a path")
    m = Mock("--prefix", "/OWRX/")
    try:
        check(http_get(m.port, "/OWRX/status.json")[0] == 200, "status.json under the path")
        check(http_get(m.port, "/status.json")[0] == 404, "...and not beside it")
        c = Client(m.port, "/ws/")
        check(c.status == 404, "/ws/ outside the path: 404")
        c = Client(m.port, "/OWRX/ws")
        check(c.status == 404, "ws without its slash: 404")
        c = start(m.port, "/OWRX/ws/")
        check(c.status == 101 and c.config().get("center_freq"), "/OWRX/ws/ plays")
        c.close()
    finally:
        m.stop()


def make_certs(d):
    if not shutil.which("openssl"):
        return None
    cfg = os.path.join(d, "san.cnf")
    with open(cfg, "w") as f:
        f.write("[req]\ndistinguished_name=dn\n[dn]\n[ext]\nsubjectAltName=DNS:localhost\n")
    run = lambda *a: subprocess.run(["openssl", *a], cwd=d, check=True, capture_output=True)
    run("req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", "key.pem", "-out", "cert.pem",
        "-days", "2", "-subj", "/CN=localhost", "-config", cfg, "-extensions", "ext")
    return os.path.join(d, "cert.pem"), os.path.join(d, "key.pem")


def case_tls(d):
    print("behind TLS, and a redirect to it")
    certs = make_certs(d)
    if not certs:
        print("  (no openssl: skipped)")
        return
    ctx = ssl.create_default_context(cafile=certs[0])
    m = Mock("--tls", f"{certs[0]}:{certs[1]}", "--prefix", "/OWRX/", "--redirect", "301")
    try:
        status, _, loc = http_get(m.redirect, "/OWRX/status.json", host="localhost")
        check(status == 301 and loc == f"https://localhost:{m.port}/OWRX/status.json", f"the redirect: {loc}")
        check(http_get(m.port, "/OWRX/status.json", ctx)[0] == 200, "status.json over TLS")
        c = start(m.port, "/OWRX/ws/", ctx)
        c.tune(7075500 - 1000, "usb", 300, 3000)
        c.pump(3)
        check(c.samples[2] > 12000 and c.dec.lost == 0, "plays over TLS")
        st = m.stats()
        check(st["tls"] >= 2 and st["redirects"] == 1, "counted")
        c.close()
    finally:
        m.stop()


def case_refusals():
    print("refusals")
    m = Mock("--others", "20")
    try:
        c = start(m.port)
        c.pump(1)
        b = c.of("backoff")
        check(b and b[0]["reason"] == "Too many clients" and c.closed, "full: backoff, closed")
        check(not c.of("config"), "...and nothing else")
    finally:
        m.stop()
    m = Mock("--banned")
    try:
        c = start(m.port)
        c.pump(1)
        b = c.of("backoff")
        check(b and b[0]["reason"] == "Client address banned" and c.closed, "banned: backoff, closed")
    finally:
        m.stop()
    m = Mock("--silent")
    try:
        c = start(m.port)
        c.pump(3)
        kinds = [t if isinstance(t, str) else t["type"] for t in c.texts]
        check(len(kinds) == 2 and kinds[1] == "receiver_details" and not c.closed, f"silent: {kinds}")
    finally:
        m.stop()
    m = Mock("--no-sdr")
    try:
        c = start(m.port)
        c.pump(2)
        e = c.of("sdr_error")
        check(e and e[0]["value"] == "No SDR Devices available" and c.fft == 0, "no SDR: sdr_error, no waterfall")
    finally:
        m.stop()
    m = Mock("--http", "503")
    try:
        check(Client(m.port).status == 503, "a front's 503")
    finally:
        m.stop()
    # Its burst rule: 10 - age, summed over the address's live sessions, at
    # 30: four sessions of one address within a second, and the fifth is
    # left silent.
    m = Mock()
    try:
        cs = [start(m.port) for _ in range(4)]
        c = start(m.port)
        c.pump(2)
        check(not c.config() and m.stats()["silent"] == 1, "the fifth at once: silent")
        for x in cs + [c]:
            x.close()
    finally:
        m.stop()


def case_bands(fork):
    print(f"bands, {fork}")
    m = Mock("--fork", fork, "--locked", "49m")
    try:
        c = start(m.port)
        c.tune(7075500 - 1000, "usb", 300, 3000)
        c.pump(2)
        # Another listener changes band: the new span, and no audio until start.
        m.get("/mock/select?profile=rspdx|20m")
        c.pump(2, until=lambda x: x.get("type") == "config" and x["value"].get("center_freq") == 14175000)
        n = c.samples[2]
        c.pump(2)
        check(c.config()["center_freq"] == 14175000, "another listener's band: told")
        check(c.samples[2] - n < 2000, "...the audio stopped, without start")
        c.tune(14201000 - 1000, "usb", 300, 3000)
        n = c.samples[2]
        c.pump(2)
        check(c.samples[2] - n > 12000, "...and on again after it")
        if fork == "plus":
            c.send(json.dumps({"type": "selectprofile", "params": {"profile": "rspdx|49m"}}))
            c.pump(1)
            lm = c.of("log_message")
            check(lm and lm[-1]["value"].startswith("This profile is locked"), "a locked band: refused")
        # Quick changes: OpenWebRX+ bans (10 - s since the last, summed, at
        # 30: four within a second, this late in a session); upstream does
        # not care.
        for p in ("80m", "40m", "20m", "80m", "40m"):
            c.send(json.dumps({"type": "selectprofile", "params": {"profile": "rspdx|" + p}}))
            c.pump(0.3)
        c.pump(1)
        st = m.stats()
        if fork == "plus":
            check(c.closed and st["bans_now"] == ["127.0.0.1"], "quick changes: banned, closed")
            c2 = start(m.port)
            c2.pump(1)
            check(c2.of("backoff") and c2.of("backoff")[0]["reason"] == "Client address banned", "...and kept out")
        else:
            check(not c.closed and not st["bans_now"], "upstream: no ban")
            c.close()
    finally:
        m.stop()
    if fork == "plus":
        print("bands 11 s apart, OpenWebRX+")
        m = Mock()
        try:
            c = start(m.port)
            for p in ("20m", "80m", "40m"):
                c.pump(11.2)
                c.send(json.dumps({"type": "selectprofile", "params": {"profile": "rspdx|" + p}}))
            c.pump(1)
            st = m.stats()
            check(not c.closed and not st["bans_now"] and min(st["select_gaps"]) >= 11, f"never banned: {st['select_gaps']}")
            c.close()
        finally:
            m.stop()


def case_keys():
    print("keys a fork does not know")
    m = Mock("--fork", "upstream")
    try:
        c = start(m.port)
        c.send(json.dumps({"type": "connectionproperties", "params": {"output_rate": 12000, "nr_enabled": True}}))
        c.send(json.dumps({"type": "dspcontrol", "params": {"offset_freq": 1500.0}}))
        c.send(json.dumps({"type": "dspcontrol", "params": {"mod": "sam"}}))
        c.pump(1)
        st = m.stats()
        check(st["poisoned"] == 1 and "connectionproperties:nr_enabled" in st["unknown_keys"], "nr_enabled to upstream: poisoned")
        check(any(b.startswith("dspcontrol:offset_freq") for b in st["bad_values"]), "a float offset: refused")
        check(st["demod_errors"] == 1, "SAM to upstream: its audio stops")
        c.close()
    finally:
        m.stop()


def case_slow():
    print("a client that stops reading")
    m = Mock("--fft", "none")
    try:
        c = start(m.port, rcvbuf=4096)
        t0 = time.time()
        while m.stats()["open"] and time.time() - t0 < 90:   # not a byte read meanwhile
            time.sleep(1)
        st = m.stats()
        check(st["open"] == 0, f"closed by the receiver after {time.time() - t0:.0f} s "
                               f"(its queue full: {st['fft_overflow']}, else its 10 s to send)")
    finally:
        m.stop()


def case_big():
    print("300 kB of bookmarks, a 64-bit length")
    m = Mock("--bookmarks-kb", "300", "--photo-kb", "40")
    try:
        c = start(m.port)
        c.pump(2)
        b = c.of("bookmarks")
        check(c.big >= 1 and b and len(json.dumps(b[0])) > 300 * 1024, "the bookmarks, whole")
        check(c.of("receiver_details") and len(c.of("receiver_details")[0]["value"]["photo_desc"]) > 40000, "a long photo_desc")
        c.close()
    finally:
        m.stop()


CASES = {
    "play_plus": lambda d: case_play("plus"),
    "play_upstream": lambda d: case_play("upstream"),
    "play_122": lambda d: case_play("upstream-1.2.2"),
    "wfm_raw": lambda d: case_wfm_and_raw(),
    "prefix": lambda d: case_prefix(),
    "tls": case_tls,
    "refusals": lambda d: case_refusals(),
    "bands_plus": lambda d: case_bands("plus"),
    "bands_upstream": lambda d: case_bands("upstream"),
    "keys": lambda d: case_keys(),
    "slow": lambda d: case_slow(),
    "big": lambda d: case_big(),
}


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
    print(f"{len(FAILS)} failed" + ("".join("\n  " + f for f in FAILS) if FAILS else ""))
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
