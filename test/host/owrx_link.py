#!/usr/bin/env python3
"""OpenWebRX's session on the PC (owrx_link_host: components/owrx_proto's
owrx_sess.c on components/websdr_link) against tools/mock_owrx.py, its
audio into the shim's ring played by the PC's clock:

  stream     minutes of it, 300 kB of bookmarks passed by on the way: the
             audio whole, at its pitch, the ring never dry, nothing lost
  tls        an http:// address under a path, redirected to https:// and
             followed, its status.json and its session over TLS
  squelch    a repeater's overs, the squelch opening and closing: the
             receiver's audio stops between them, and the ring keeps its time
  listener   another listener changes band: the dial onto the new one, start
             again, and it plays on
  band       the knob's own band change: never within 11 s of the session's
             start, and the dial onto the band
  stall      the network holds everything up 3 s: one jump to the live point
  wfm        WFM's 48 kHz audio, and audio uncompressed
  ends       a full receiver, a banned address, a silent session, no SDR, a
             404, nothing listening, a close while playing: each said so

  owrx_link.py --host BIN --mock tools/mock_owrx.py [--minutes M] [--only a,b]

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


def host(url, seconds, *what, env=None, during=None):
    """A session, its lines parsed: @SUMMARY's fields, @SPANs, @REPORTs,
    @STATUS, @MOVED, @END. `during`: (at, fn) to call while it runs."""
    p = subprocess.Popen([ARGS.host, url, str(seconds), *what], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                         text=True, env=dict(os.environ, **(env or {})))
    out = {"spans": [], "reports": [], "lines": []}
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
        out["lines"].append(line.rstrip())
        if ARGS.verbose:
            print("    " + line.rstrip())
        w = line.split()
        if not w:
            continue
        if w[0] == "@SUMMARY":
            out["sum"] = dict(kv.split("=", 1) for kv in w[1:] if "=" in kv)
        elif w[0] == "@SPAN":
            out["spans"].append((int(w[1]), int(w[2]), int(w[3]), w[4]))
        elif w[0] == "@REPORT":
            out["reports"].append(dict(kv.split("=", 1) for kv in w[1:]))
        elif w[0] == "@END":
            out["end"] = " ".join(w[1:])
        elif w[0] == "@MOVED":
            out["moved"] = int(w[1])
        elif w[0] == "@STATUS":
            out["status"] = line.strip()
        elif "ERROR: AddressSanitizer" in line or "runtime error:" in line:
            out.setdefault("asan", []).append(line.strip())
    p.wait()
    if "asan" in out:
        check(False, "sanitizers: " + out["asan"][0])
    return out


def f(o, k):
    return float(o["sum"][k])


# ---------------------------------------------------------------- the cases

def case_stream(d):
    secs = ARGS.minutes * 60
    print(f"stream, {secs:.0f} s, 300 kB of bookmarks on the way")
    m = Mock("--bookmarks-kb", "300", "--drift-ppm", "150")
    try:
        o = host(f"http://127.0.0.1:{m.port}/", secs, "status")
        st = m.stats()
        s = o.get("sum", {})
        check("ok=1" in o.get("status", "") and "plus=1" in o.get("status", ""), "status.json read: " + o.get("status", "")[:90])
        check(o.get("end") == "left", f"ended by the knob: {o.get('end')}")
        check(f(o, "played") > secs - 2.5, f"played {s.get('played')} s of {secs:.0f}")
        check(int(s["underruns"]) == 0 and int(s["jumps"]) == 0 and int(s["breaks"]) == 0,
              f"never dry: underruns {s['underruns']}, jumps {s['jumps']}, breaks {s['breaks']}")
        check(int(s["lost"]) == 0, "not a byte of audio lost")
        check(abs(f(o, "pitch") - 1000) < 15, f"its carrier at 1 kHz: {s['pitch']} Hz")
        check(int(s["biggest"]) > 300 * 1024, f"the bookmarks passed by: {s['biggest']} bytes")
        check(int(s["fft"]) > 8 * secs, f"the waterfall drained: {s['fft']} frames")
        # The mock's clock 150 ppm fast: its audio a hair shortened, trim
        # 1.00015 -- reached within minutes, from where the ring started.
        trims = [float(r["trim"]) for r in o["reports"]]
        if secs >= 180:
            check(trims and abs(trims[-1] - 1.00015) < 0.00008, f"the receiver's clock followed: trim {trims[-1]}")
        check(not st["unknown_keys"] and not st["bad_values"] and not st["setfrequency"] and not st["setsdr"]
              and not st["selects"], "nothing it should not say")
        check(st["starts"] == 1 and st["two_at_once"] == 0 and st["output_rates"] == [[12000, 48000]],
              "one start, one session, a browser's rates")
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


def case_tls(d):
    print("http:// under a path, redirected to https://")
    if ARGS.no_tls:
        print("  (no TLS in this build: skipped)")
        return
    certs = make_certs(d)
    if not certs:
        print("  (no openssl: skipped)")
        return
    m = Mock("--tls", f"{certs[1]}:{certs[2]}", "--prefix", "/OWRX/", "--redirect", "301")
    try:
        o = host(f"http://localhost:{m.redirect}/OWRX", 12, "status", env={"KIWI_TEST_CA": certs[0]})
        st = m.stats()
        check(f"tls_port={m.port}" in o.get("status", "") and "ok=1" in o.get("status", ""),
              "status.json: the redirect followed, read over TLS")
        check(f(o, "played") > 8 and int(o["sum"]["lost"]) == 0, f"played over TLS: {o['sum']['played']} s")
        check(st["tls"] >= 2 and st["redirects"] == 1 and st["not_found"] == 0, "under /OWRX/, on its TLS port")
        # A certificate no authority it knows signed: not spoken to.
        o = host(f"https://localhost:{m.port}/OWRX/", 5)
        check(o.get("end") == "CERTIFICATE?", f"an unknown authority: {o.get('end')}")
    finally:
        m.stop()


def case_squelch(d):
    print("a repeater's overs through the squelch")
    m = Mock("--station-cycle", "2:2")
    try:
        o = host(f"http://127.0.0.1:{m.port}/", 20, "sq=40")
        st = m.stats()
        s = o["sum"]
        check(st.get("squelched_ticks", 0) > 50, f"the receiver's audio stopped between overs ({st.get('squelched_ticks', 0)} ticks)")
        check(int(s["jumps"]) == 0 and int(s["breaks"]) == 0 and int(s["underruns"]) == 0,
              f"the ring kept its time: jumps {s['jumps']}, breaks {s['breaks']}, underruns {s['underruns']}")
        check(f(o, "played") > 17, f"played on through the pauses: {s['played']} s")
    finally:
        m.stop()


def case_listener(d):
    print("another listener changes band")
    m = Mock()
    try:
        o = host(f"http://127.0.0.1:{m.port}/", 14, "dial=7074500",
                 during=[(5, lambda: m.get("/mock/select?profile=rspdx|20m"))])
        st = m.stats()
        check(len(o["spans"]) == 2 and o["spans"][1][0] == 14175000, f"told: {o['spans']}")
        # 1 kHz before (7075500 on 7074500) and after (14201000 on 14200000).
        check(o["spans"][1][2] == 14200000 and o["spans"][1][3] == "usb", "the dial onto the band's start")
        check(st["starts"] == 2 and st["selects"] == 0, "start again; no band asked for by the knob")
        check(abs(f(o, "pitch") - 1000) < 30 and int(o["sum"]["jumps"]) == 0, f"plays on: {o['sum']['pitch']} Hz")
    finally:
        m.stop()


def case_band(d):
    print("the knob's own band change")
    m = Mock()
    try:
        o = host(f"http://127.0.0.1:{m.port}/", 18, "band=rspdx|80m@1")
        st = m.stats()
        check(st["selects"] == 1 and st["select_after_connect"][0] >= 11, f"asked for {st['select_after_connect']} s in")
        check(len(o["spans"]) == 2 and o["spans"][1][0] == 3650000 and o["spans"][1][2] == 3700000
              and o["spans"][1][3] == "lsb", f"the dial onto 80 m: {o['spans'][-1:]}")
        check(not st["bans_now"] and st["starts"] == 2, "no ban; start again")
    finally:
        m.stop()


def case_old(d):
    print("OpenWebRX 1.0: its audio without SYNCs; then a band changed under it")
    m = Mock("--fork", "upstream-1.0")
    try:
        o = host(f"http://127.0.0.1:{m.port}/", 20, "status")
        s = o.get("sum", {})
        check("ok=1" in o.get("status", "") and "plus=0" in o.get("status", ""), "status.json read: " + o.get("status", "")[:90])
        check(o.get("end") == "left", f"ended by the knob: {o.get('end')}")
        check(f(o, "played") > 17.5, f"played {s.get('played')} s of 20")
        check(int(s["lost"]) == 0, f"not a byte of audio passed over looking for a SYNC: {s['lost']}")
        check(int(s["underruns"]) == 0 and int(s["breaks"]) == 0, f"never dry: underruns {s['underruns']}, breaks {s['breaks']}")
        check(abs(f(o, "pitch") - 1000) < 15, f"its carrier at 1 kHz: {s['pitch']} Hz")
    finally:
        m.stop()
    m = Mock("--fork", "upstream-1.0")
    try:
        o = host(f"http://127.0.0.1:{m.port}/", 20, "band=rspdx|80m@1")
        s = o.get("sum", {})
        st = m.stats()
        check(st["selects"] == 1 and st["starts"] == 2 and len(o["spans"]) == 2, f"onto 80 m: {o['spans'][-1:]}")
        check(f(o, "played") > 17.5 and int(s["lost"]) == 0,
              f"played on through it, plain: {s.get('played')} s of 20, {s.get('lost')} bytes lost")
    finally:
        m.stop()


def case_stall(d):
    print("the network holds it all up 3 s")
    m = Mock("--stall", "6:3")
    try:
        o = host(f"http://127.0.0.1:{m.port}/", 16)
        s = o["sum"]
        check(m.stats().get("stalls", 0) > 0, "held up")
        check(int(s["jumps"]) == 1, f"one jump to the live point: {s['jumps']}")
        check(f(o, "played") > 11 and f(o, "played") < 14.5, f"played {s['played']} s: the stall's time is gone, not heard late")
        check(int(s["lost"]) == 0, "the decoder in step throughout")
    finally:
        m.stop()


def case_wfm(d):
    print("WFM at 48 kHz; audio uncompressed")
    m = Mock("--start", "8c0f6e3a-7c1d-4e2b-9a55-1f2e3d4c5b6a|2c3d4e5f-6071-4829-9bac-1d2e3f4a5b6c")
    try:
        o = host(f"http://127.0.0.1:{m.port}/", 8, "dial=98500000", "mode=wfm", "lo=-75000", "hi=75000")
        s = o["sum"]
        check(s["mode"] == "wfm" and abs(f(o, "pitch") - 1000) < 20 and f(o, "played") > 6,
              f"WFM: {s['pitch']} Hz, {s['played']} s")
    finally:
        m.stop()
    m = Mock("--audio", "none", "--fork", "upstream")
    try:
        o = host(f"http://127.0.0.1:{m.port}/", 8, "mode=sam")
        s = o["sum"]
        check(abs(f(o, "pitch") - 1000) < 20 and f(o, "played") > 6, f"int16: {s['pitch']} Hz, {s['played']} s")
        check(s["mode"] == "am" and m.stats()["demod_errors"] == 0, "SAM upstream: AM instead, never sam")
    finally:
        m.stop()


def case_ends(d):
    print("each end said")
    for flags, want, secs in ((["--others", "20"], "RECEIVER FULL", 5), (["--banned"], "BANNED", 5),
                              (["--silent"], "NO ANSWER", 14), (["--no-sdr"], "NO SDR", 5),
                              (["--http", "404"], "NOT OPENWEBRX", 5), (["--close-after", "4"], "NO ANSWER", 10)):
        m = Mock(*flags)
        try:
            o = host(f"http://127.0.0.1:{m.port}/", secs)
            check(o.get("end") == want, f"{' '.join(flags)}: {o.get('end')}")
        finally:
            m.stop()
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    o = host(f"http://127.0.0.1:{port}/", 5)
    check(o.get("end") == "CAN'T REACH", f"nothing listening: {o.get('end')}")


CASES = {"stream": case_stream, "tls": case_tls, "squelch": case_squelch, "listener": case_listener,
         "band": case_band, "old": case_old, "stall": case_stall, "wfm": case_wfm, "ends": case_ends}


def main():
    global ARGS
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", required=True)
    ap.add_argument("--mock", required=True)
    ap.add_argument("--minutes", type=float, default=1.0)
    ap.add_argument("--only", default="")
    ap.add_argument("--no-tls", action="store_true")
    ap.add_argument("-v", "--verbose", action="store_true")
    ARGS = ap.parse_args()
    with tempfile.TemporaryDirectory() as d:
        for name, fn in CASES.items():
            if ARGS.only and name not in ARGS.only.split(","):
                continue
            fn(d)
    print(f"{len(FAILS)} failed" + ("".join("\n  " + x for x in FAILS) if FAILS else ""))
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
