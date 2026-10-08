#!/usr/bin/env python3
"""The openwebrx firmware's receiver on the PC (owrx_host: components/
owrx_client on components/owrx_proto's session and the web SDRs' link)
against tools/mock_owrx.py -- one run of owrx_host a boot of the knob, its
NVS a file that outlives it:

  stream     a receiver plays: its band, its name and software on the slab,
             its bands with where each is, the dial on the band's start in
             its mode, the station heard at its pitch; one status.json read
  edge       the dial stops at the edges of the band the receiver is on
  band       another band: refused within 11 s of the session's start, then
             asked once, the dial onto it; the one in use, and one too soon
             after, refused
  listener   another listener's band change followed, the dial onto it
  handover   the one in use not reached: after a fair chance the next plays,
             and is the one in use from then on -- through a restart too
  standin    a stand-in that answers full is passed by for the next
  full       the one in use full: its own word, and not asked again for 32 s
  banned     a banned address: not again until chosen again, and then once
  switch     another receiver chosen: the session on it, the first closed;
             each keeps its own dial
  saved      the list saved while one plays, without it: on to the one left
  restart    the dial, the mode and the squelch through a restart
  squelch    the squelch as the receiver's dB, sent as such
  upstream   OpenWebRX: no SAM (AM in its place), no OpenWebRX+ on the slab
  tls        an http:// one under a path that redirects to https://: followed
             by its status.json read, and its sessions straight to https://
             from then on, a reconnect too

  owrx_client.py --host BIN --mock tools/mock_owrx.py [--only a,b] [-v]

Loopback only: run it in a network namespace of its own."""
import argparse
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time

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
        self.port = self.control = self.redirect = None
        while True:
            line = self.p.stdout.readline()
            if not line:
                raise RuntimeError("the mock did not start")
            w = line.split()
            if w and w[0] == "LISTENING":
                self.port = int(w[1])
            elif w and w[0] == "CONTROL":
                self.control = int(w[1])
            elif w and w[0] == "REDIRECTING":
                self.redirect = int(w[1])
            if self.port and (self.control or "--tls" not in flags) and (self.redirect or "--redirect" not in flags):
                break
        self.control = self.control or self.port
        self.url = f"http://127.0.0.1:{self.port}/"

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

    def sent(self):
        return self.get("/mock/sent")

    def stop(self):
        self.p.terminate()
        try:
            self.p.wait(5)
        except subprocess.TimeoutExpired:
            self.p.kill()


def dead_url():
    """An address nothing listens on: refused at once. Port 9, never one from
    the ephemeral range -- a connection to one of those on loopback can come
    back to itself, the knob's own request read as the receiver's answer."""
    return "http://127.0.0.1:9/"


def kv(line):
    """@STATE's fields: k=v and k="v w"."""
    return {m.group(1): m.group(2) if m.group(2) is not None else m.group(3)
            for m in re.finditer(r'(\w+)=(?:"([^"]*)"|(\S+))', line)}


class Run:
    """One boot of the knob: owrx_host's lines, parsed."""

    def __init__(self, nvs, *cmds, during=None, env=None):
        p = subprocess.Popen([ARGS.host, nvs, *map(str, cmds)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                             text=True, env=dict(os.environ, **(env or {})))
        self.lines, self.states, self.bands, self.uses, self.band_said, self.turns = [], [], [], [], [], []
        self.pitch = None
        self.activated = []
        self.timeouts = 0
        t0 = time.time()
        pending = sorted(during or [], key=lambda x: x[0])
        import selectors
        sel = selectors.DefaultSelector()
        sel.register(p.stdout, selectors.EVENT_READ)
        while True:
            while pending and time.time() - t0 >= pending[0][0]:
                pending.pop(0)[1]()
            if not sel.select(0.1):
                if p.poll() is not None:
                    break
                continue
            line = p.stdout.readline()
            if not line:
                break
            line = line.rstrip()
            self.lines.append(line)
            if ARGS.verbose:
                print("    " + line)
            w = line.split()
            if not w:
                continue
            if w[0] == "@STATE":
                self.states.append(kv(line))
            elif w[0] == "@B":
                m = re.match(r'@B (\d+) "([^"]*)" "([^"]*)" (-?\d+) (-?\d+)', line)
                self.bands.append((int(m.group(1)), m.group(2), m.group(3), int(m.group(4)), int(m.group(5))))
            elif w[0] == "@USE":
                self.uses.append((int(w[1]), w[2]))
            elif w[0] == "@BAND":
                self.band_said.append((int(w[1]), w[2]))
            elif w[0] == "@TURN":
                self.turns.append(int(w[1].split("=")[1]))
            elif w[0] == "@PITCH":
                self.pitch = float(w[1].split("=")[1])
            elif w[0] == "@ACTIVATED":
                self.activated.append(int(w[1]))
            elif w[0] == "@TIMEOUT":
                self.timeouts += 1
            elif "ERROR: AddressSanitizer" in line or "runtime error:" in line:
                check(False, "sanitizers: " + line.strip())
        p.wait()

    def last(self):
        return self.states[-1] if self.states else {}


def nvs():
    d = tempfile.mkdtemp(prefix="owrx-nvs-")
    return os.path.join(d, "nvs.txt")


# ---------------------------------------------------------------- the cases

def case_stream():
    m = Mock()
    try:
        r = Run(nvs(), "save", "Mock=" + m.url, "untilrx", 0, 20, "bands", "pitch", 3, "state", "off")
        s = r.last()
        check(r.timeouts == 0 and s.get("link") == "READY" and s.get("rx") == "0", "it plays")
        check(s.get("band") == "40m" and s.get("line2") == "40m  RSPdx", f"its band on the slab: {s.get('line2')}")
        check(s.get("line3") == "OpenWebRX+ 1.2.126" and s.get("plus") == "1", f"its software: {s.get('line3')}")
        check(s.get("server") == "Mock", "the page's name on the slab")
        check(int(s["f"]) == 7074000 and s["mode"] == "usb",
              f"the dial on the band's start, FT8's sideband: {s['f']} {s['mode']}")
        check(int(s["fmin"]) == 6850000 and int(s["fmax"]) == 7350000, "the dial's limits: the band's span")
        check(int(s["users"]) >= 1, f"its listeners said: {s['users']}")
        check(float(s["mlo"]) == -108.0 and float(s["mhi"]) == 0.0, "the meter's scale: its page's")
        names = [b[1] for b in r.bands]
        check(names[:4] == ["40m", "20m", "80m", "49m Broadcast"] and len(names) == 7, f"its bands: {names}")
        b40 = next((b for b in r.bands if b[1] == "40m"), None)
        check(b40 and b40[2] == "RSPdx" and b40[3] == 6850000 and b40[4] == 7350000,
              f"each with its SDR and where it is (status.json): {b40}")
        check(r.pitch is not None and abs(r.pitch - 1500) < 15, f"the station at 1500 Hz: {r.pitch}")
        st = m.stats()
        check(st["status"] == 1, f"one status.json read: {st['status']}")
        check(st["sessions"] == 1 and not st["unknown_keys"] and not st["bad_values"] and not st["setfrequency"],
              "one session, nothing it does not know")
    finally:
        m.stop()


def case_edge():
    m = Mock()
    try:
        r = Run(nvs(), "save", m.url, "untilrx", 0, 20, "turn", 400, 1000, "wait", 0.5, "turn", -1000, 1000,
                "wait", 0.5, "state", "off")
        check(r.turns[:2] == [7350000, 6850000], f"the dial stops at the band's edges: {r.turns}")
        offs = [json.loads(x["text"]).get("params", {}).get("offset_freq") for x in m.sent()
                if '"offset_freq"' in x.get("text", "")]
        offs = [o for o in offs if o is not None]
        check(offs and max(abs(o) for o in offs) <= 250000, f"never off the span: {min(offs)}..{max(offs)}")
    finally:
        m.stop()


def case_band():
    m = Mock()
    try:
        r = Run(nvs(), "save", m.url, "untilrx", 0, 20, "band", 2, "state", "wait", 12, "state", "band", 2,
                "wait", 3, "state", "band", 2, "band", 1, "state", "off")
        check(r.band_said[0] == (2, "refused"), "refused within 11 s of the session's start")
        s0 = r.states[1]
        check(0 < int(s0["wait"]) <= 11, f"...how long said: {s0['wait']} s")
        check(int(r.states[2]["wait"]) == 0, "nothing to wait for 12 s on")
        check(r.band_said[1] == (2, "ok"), "then asked")
        s = r.states[3]
        check(s["band"] == "80m" and int(s["f"]) == 3700000 and s["mode"] == "lsb",
              f"the dial onto it, its own mode: {s['band']} {s['f']} {s['mode']}")
        check(r.band_said[2] == (2, "refused"), "the one in use: refused")
        check(r.band_said[3] == (1, "refused") and int(r.states[4]["wait"]) > 0, "one too soon after: refused")
        st = m.stats()
        check(st["selects"] == 1 and st["select_after_connect"][0] >= 11,
              f"one select, 11 s in at the soonest: {st['select_after_connect']}")
        check(not st["bans"], "no ban")
    finally:
        m.stop()


def case_listener():
    m = Mock()
    try:
        r = Run(nvs(), "save", m.url, "untilrx", 0, 20, "wait", 6, "state", "pitch", 2, "off",
                during=[(3, lambda: m.get("/mock/select?profile=rspdx|20m"))])
        s = r.last()
        check(s["band"] == "20m" and int(s["f"]) == 14200000 and s["mode"] == "usb",
              f"another listener's band followed: {s['band']} {s['f']} {s['mode']}")
        check(r.pitch is not None and abs(r.pitch - 1000) < 15, f"and it plays there: {r.pitch} Hz")
        check(m.stats()["selects"] == 0, "the knob asked for none")
    finally:
        m.stop()


def case_handover():
    m = Mock()
    try:
        f = nvs()
        r = Run(f, "save", "Gone=" + dead_url(), "Mock=" + m.url, "state", "untilrx", 1, 30, "quit")
        check(r.states[0]["rx"] == "0", "the one in use first")
        check(r.timeouts == 0 and r.last().get("link") == "READY", "the next plays")
        check(1 in r.activated, "...and is in use from then on, saved")
        r2 = Run(f, "state", "untilrx", 1, 20, "off")
        check(r2.states[0]["rx"] == "1", "through a restart")
        check(r2.timeouts == 0, "...and it plays again")
    finally:
        m.stop()


def case_standin():
    full = Mock("--max-clients", "1", "--others", "1")
    good = Mock()
    try:
        r = Run(nvs(), "save", dead_url(), full.url, good.url, "untilrx", 2, 40, "wait", 3, "state", "off")
        check(r.timeouts == 0 and r.last()["rx"] == "2", "past the full stand-in, the next plays")
        check(full.stats()["full"] == 1, f"the full one asked once: {full.stats()['full']}")
    finally:
        full.stop()
        good.stop()


def case_full():
    m = Mock("--max-clients", "1", "--others", "1")
    try:
        r = Run(nvs(), "save", m.url, "wait", 6, "state", "off")
        s = r.last()
        check(s["why"] == "RECEIVER FULL" and "again in" in s["state"], f"its word: {s['why']}, {s['state']}")
        check(m.stats()["full"] == 1, f"not asked again within 32 s: {m.stats()['full']}")
    finally:
        m.stop()


def case_banned():
    m = Mock("--banned")
    try:
        r = Run(nvs(), "save", m.url, "wait", 4, "state", "use", 0, "wait", 3, "state", "off")
        check(r.states[0]["why"] == "BANNED", f"its word: {r.states[0]['why']}")
        st = m.stats()
        check(st["banned_refused"] == 2, f"once by itself, once chosen again: {st['banned_refused']}")
    finally:
        m.stop()


def case_switch():
    a, b = Mock(), Mock()
    try:
        r = Run(nvs(), "save", a.url, b.url, "untilrx", 0, 20, "tune", 7080000, "wait", 0.5, "use", 1,
                "untilrx", 1, 20, "tune", 7050000, "wait", 0.5, "state", "use", 0, "untilrx", 0, 20, "off")
        check(r.uses == [(1, "ok"), (0, "ok")], "chosen")
        check(r.timeouts == 0 and r.states[1]["rx"] == "1" and r.states[1]["link"] == "READY", "the second plays")
        check(int(r.states[2]["f"]) == 7050000, f"its dial tuned: {r.states[2]['f']}")
        check(int(r.states[3]["f"]) == 7080000, f"back on the first: its own dial again ({r.states[3]['f']})")
        sa, sb = a.stats(), b.stats()
        check(sa["sessions"] == 2 and sa["max_open"] == 1, "the first's session ended before its next")
        check(sb["sessions"] == 1 and sa["status"] == 1 and sb["status"] == 1,
              "one session on the second; each status.json read once")
    finally:
        a.stop()
        b.stop()


def case_saved():
    a, b = Mock(), Mock()
    try:
        f = nvs()
        r = Run(f, "save", a.url, b.url, "untilrx", 0, 20, "save", b.url, "untilrx", 0, 20, "list", "off")
        check(r.timeouts == 0 and r.last()["link"] == "READY", "on to the one left")
        check(b.stats()["sessions"] == 1, "...a session on it")
    finally:
        a.stop()
        b.stop()


def case_restart():
    m = Mock()
    try:
        f = nvs()
        r = Run(f, "save", m.url, "untilrx", 0, 20, "tune", 7123000, "mode", "cw", "squelch", 40, "quit")
        r2 = Run(f, "state", "off")
        s = r2.states[0]
        check(int(s["f"]) == 7123000 and s["mode"] == "cw" and s["sq"] == "40",
              f"the dial, the mode, the squelch kept: {s['f']} {s['mode']} {s['sq']}")
    finally:
        m.stop()


def case_squelch():
    m = Mock()
    try:
        r = Run(nvs(), "save", m.url, "untilrx", 0, 20, "squelch", 60, "wait", 1, "state", "off")
        check(r.last()["sqdb"] == "-43", f"60 % of -108..0: {r.last()['sqdb']} dB")
        lv = [json.loads(x["text"]).get("params", {}).get("squelch_level") for x in m.sent()
              if '"squelch_level"' in x.get("text", "")]
        lv = [v for v in lv if v is not None]
        check(lv and lv[-1] == -43, f"sent as such: {lv[-3:]}")
    finally:
        m.stop()


def case_upstream():
    m = Mock("--fork", "upstream")
    try:
        r = Run(nvs(), "save", m.url, "untilrx", 0, 20, "mode", "sam", "wait", 0.5, "state", "off")
        s = r.last()
        check(s["mode"] == "am" and s["plus"] == "0", f"no SAM upstream, AM in its place: {s['mode']}")
        check(s["line3"].startswith("OpenWebRX ") and s["model"] == "OpenWebRX", f"its software: {s['line3']}")
        st = m.stats()
        check(st["demod_errors"] == 0 and not st["unknown_keys"], "nothing it cannot play")
    finally:
        m.stop()


def make_certs(d):
    if not shutil.which("openssl"):
        return None
    cfg = os.path.join(d, "san.cnf")
    with open(cfg, "w") as fh:
        fh.write("[req]\ndistinguished_name=dn\n[dn]\n[ext]\nsubjectAltName=DNS:localhost\n"
                 "basicConstraints=critical,CA:FALSE\n[ca]\nbasicConstraints=critical,CA:TRUE\n")
    run = lambda *a: subprocess.run(["openssl", *a], cwd=d, check=True, capture_output=True)
    run("req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", "ca.key", "-out", "ca.pem", "-days", "2",
        "-subj", "/CN=Mock CA", "-config", cfg, "-extensions", "ca")
    run("req", "-newkey", "rsa:2048", "-nodes", "-keyout", "srv.key", "-out", "srv.csr", "-subj", "/CN=localhost",
        "-config", cfg)
    run("x509", "-req", "-in", "srv.csr", "-CA", "ca.pem", "-CAkey", "ca.key", "-CAcreateserial", "-out", "srv.pem",
        "-days", "2", "-extfile", cfg, "-extensions", "ext")
    return os.path.join(d, "ca.pem"), os.path.join(d, "srv.pem"), os.path.join(d, "srv.key")


def case_tls():
    if ARGS.no_tls:
        print("  (no TLS in this build: skipped)")
        return
    d = tempfile.mkdtemp(prefix="owrx-tls-")
    certs = make_certs(d)
    if not certs:
        print("  (no openssl: skipped)")
        return
    m = Mock("--tls", f"{certs[1]}:{certs[2]}", "--prefix", "/OWRX/", "--redirect", "301")
    try:
        r = Run(nvs(), "save", f"http://localhost:{m.redirect}/OWRX/", "untilrx", 0, 30, "wait", 9,
                "untilrx", 0, 20, "off", env={"KIWI_TEST_CA": certs[0]},
                during=[(6, lambda: m.get("/mock/set?close=1"))])
        check(r.timeouts == 0 and r.states[0]["link"] == "READY", "it plays over TLS, under its path")
        check(r.states[-1]["link"] == "READY" and int(r.states[-1]["closes"]) == 1,
              "closed by the receiver, and playing again")
        st = m.stats()
        check(st["redirects"] == 1, f"the redirect followed once, a reconnect straight to https://: {st['redirects']}")
        check(st["sessions"] == 2 and st["status"] == 1 and st["not_found"] == 0,
              f"two sessions, one status.json, nothing off its path: {st['sessions']} {st['status']}")
    finally:
        m.stop()


CASES = ["stream", "edge", "band", "listener", "handover", "standin", "full", "banned", "switch", "saved",
         "restart", "squelch", "upstream", "tls"]


def main():
    global ARGS
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", required=True)
    ap.add_argument("--mock", required=True)
    ap.add_argument("--only", default="")
    ap.add_argument("-v", "--verbose", action="store_true")
    ap.add_argument("--no-tls", action="store_true")
    ARGS = ap.parse_args()
    only = [c for c in ARGS.only.split(",") if c] or CASES
    for c in only:
        print(f"{c}:", flush=True)
        t = time.time()
        globals()["case_" + c]()
        print(f"  ({time.time() - t:.0f} s)", flush=True)
    print(f"{len(FAILS)} failed" if FAILS else "all good")
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
