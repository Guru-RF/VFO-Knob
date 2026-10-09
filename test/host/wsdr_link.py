#!/usr/bin/env python3
"""The WebSDR session (wsdr_link_host: components/wsdr_proto's wsdr_sess.c on
components/websdr_link, as the firmware builds them) against
tools/mock_wsdr.py: what it reads first, how it plays, what it sends -- and
every way a session ends.

  wsdr_link.py --host build/wsdr_link_host --mock tools/mock_wsdr.py
               [--minutes M] [--only NAME] [--no-tls] [-v]

Loopback only: run it in a network namespace of its own to be sure of it
(test/host/CMakeLists.txt says how)."""
import argparse
import json
import os
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
    def __init__(self, *flags, script=None):
        self.p = subprocess.Popen([sys.executable, script or ARGS.mock, "--port", "0", *flags],
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

    def sent(self):
        return self.get("/mock/sent")

    def stop(self):
        self.p.terminate()
        try:
            self.p.wait(5)
        except subprocess.TimeoutExpired:
            self.p.kill()


def host(url, seconds, *what):
    """A session, its lines parsed."""
    p = subprocess.run([ARGS.host, url, str(seconds), *what], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                       text=True, timeout=seconds + 60)
    out = {"reports": [], "lines": p.stdout.splitlines()}
    for line in out["lines"]:
        if ARGS.verbose:
            print("    " + line)
        w = line.split()
        if not w:
            continue
        if w[0] == "@SUMMARY":
            out["sum"] = dict(kv.split("=", 1) for kv in w[1:] if "=" in kv)
        elif w[0] == "@REPORT":
            out["reports"].append(dict(kv.split("=", 1) for kv in w[1:]))
        elif w[0] == "@END":
            out["end"] = " ".join(w[1:])
        elif w[0] in ("@INFO", "@PATH"):
            out[w[0][1:].lower()] = line
        elif w[0] == "@TITLE":
            out["title"] = line.split(" ", 1)[1].strip('"')
        elif w[0] == "@STATE":
            out.setdefault("states", []).append(w[1])
        elif "ERROR: AddressSanitizer" in line or "runtime error:" in line:
            out.setdefault("asan", []).append(line.strip())
    if "asan" in out:
        check(False, "sanitizers: " + out["asan"][0])
    return out


def f(o, k):
    return float(o["sum"][k])


def tunings(m):
    return [x["text"] for x in m.sent() if "f=" in x["text"]]


# ---------------------------------------------------------------- the cases

def case_stream(d):
    secs = ARGS.minutes * 60
    print(f"twente, {secs:.0f} s, its clock 150 ppm fast")
    m = Mock("--site", "twente", "--drift-ppm", "150")
    try:
        o = host(f"http://127.0.0.1:{m.port}/", secs, "dial=14074000", "mode=usb")
        s, st = o.get("sum", {}), m.stats()
        check("bands=1 " in o.get("info", "") and "plan=13" in o.get("info", ""), "bandinfo.js: one band, its plan")
        check("v11=0" in o.get("path", ""), "its page opens /~~stream")
        check(o.get("title") == "WebSDR", f"its title: {o.get('title')}")
        check(o.get("end") == "left", f"ended by the knob: {o.get('end')}")
        check(f(o, "played") > secs - 2.5, f"played {s.get('played')} s of {secs:.0f}")
        check(int(s["underruns"]) == 0 and int(s["jumps"]) == 0 and int(s["breaks"]) == 0,
              f"never dry: underruns {s['underruns']}, jumps {s['jumps']}, breaks {s['breaks']}")
        check(int(s["lost_blocks"]) == 0 and int(s["toolong"]) == 0, "nothing lost, no message too long")
        check(abs(f(o, "pitch") - 1500) < 15, f"the station at 14075.5 heard at 1.5 kHz: {s['pitch']} Hz")
        check(-80 < f(o, "dbm") < -66, f"its S-meter, S9 or so: {s['dbm']} dBm")
        check(int(s["rate"]) == 7119, f"at 7119 Hz: {s['rate']}")
        check(st["paths"] == ["/~~stream"] and st["origins"] == [f"http://127.0.0.1:{m.port}"],
              f"its path and its page's Origin: {st['paths']} {st['origins']}")
        check(st["names"] == ["MOCKTEST"] and st["agents"][-1] == "VFO-Knob", "its name, and the knob's own agent")
        check(not st["unknown_keys"] and not st["bad_values"] and not st["foreign_keys"] and not st["outside"],
              "all it sent known, in range")
        check(st["tunings"] == 1 and st["too_fast"] == 0 and st["max_open"] == 1, "one tuning, one stream")
    finally:
        m.stop()


def case_bands(d):
    print("maasbree: ?v=11, 40 m, then the dial onto 80 m")
    m = Mock("--site", "maasbree", "--strict-path")
    try:
        o = host(f"http://127.0.0.1:{m.port}/", 12, "dial=7074000", "mode=usb", "tune=3629000@5")
        s, st = o.get("sum", {}), m.stats()
        check("bands=8 " in o.get("info", "") and "idle=14400000" in o.get("info", ""), "eight bands, 4 h idle")
        check("v11=1" in o.get("path", ""), "its page opens /~~stream?v=11")
        check(o.get("end") == "left" and int(s["band"]) == 1, f"ended on band 1, 80 m: {s.get('band')}")
        check(abs(f(o, "pitch") - 1500) < 20, f"80 m's station at 3630.5: {s['pitch']} Hz")
        t = tunings(m)
        check(len(t) == 2 and "band=3" in t[0] and "f=7074.000" in t[0] and "band=1" in t[1] and "f=3629.000" in t[1],
              f"40 m, then 80 m: {t}")
        check(st["paths"] == ["/~~stream?v=11"] and not st["outside"] and not st["foreign_keys"],
              "its own path; nothing outside a band; nothing it does not know")
    finally:
        m.stop()


def case_turn(d):
    print("the dial turned fast: a tuning no oftener than every 250 ms, the last always")
    m = Mock("--site", "twente")
    try:
        o = host(f"http://127.0.0.1:{m.port}/", 6, "dial=14074000", "turn=10/20@2")
        st = m.stats()
        t = tunings(m)
        check(st["too_fast"] == 0, f"none within 250 ms of the last: {st['too_fast']}")
        check(2 <= len(t) <= 4, f"ten detents in 200 ms, a few tunings: {len(t)}")
        # Twente's carrier on its 6.952 Hz tuning step: 14074100 goes as 14074.102.
        last = float(t[-1].split("f=", 1)[1].split("&", 1)[0]) * 1000 if t else 0
        check(abs(last - 14074100) <= 7, f"the last where the dial stopped, on the site's step: {last:.0f} Hz")
        check(int(o["sum"]["dial"]) == 14074100, "the dial there")
    finally:
        m.stop()


def case_am(d):
    print("AM: the rate doubles past 3.5 kHz")
    m = Mock("--site", "twente")
    try:
        o = host(f"http://127.0.0.1:{m.port}/", 6, "dial=1000000", "mode=am")
        s, st = o.get("sum", {}), m.stats()
        check(st["rates"][-1] == 14238 and abs(f(o, "pitch") - 1000) < 15,
              f"14238 Hz, its 1 kHz tone: {st['rates']} {s.get('pitch')}")
        check(int(s["underruns"]) == 0, "never dry across the change of rate")
    finally:
        m.stop()


def case_mute(d):
    print("mute and the squelch: silence items, and what was sent")
    m = Mock("--site", "twente")
    try:
        o = host(f"http://127.0.0.1:{m.port}/", 8, "dial=14000000", "mute=1@2", "mute=0@4", "sq=1@5")
        s = o.get("sum", {})
        sent = [x["text"] for x in m.sent()]
        check("GET /~~param?mute=1" in sent and sent.count("GET /~~param?mute=0") == 2
              and "GET /~~param?squelch=1" in sent, f"mute, unmute, squelch sent: {sent[-4:]}")
        check(int(s["silent"]) > 50, f"silence items while muted and squelched: {s['silent']}")
        check(int(s["underruns"]) == 0, "the silence played at its pace")
    finally:
        m.stop()


def case_idle(d):
    print("the site's idle timeout: let go when nothing is touched; kept while something is")
    m = Mock("--site", "maasbree", "--idle-page", "3000")
    try:
        o = host(f"http://127.0.0.1:{m.port}/", 10, "dial=7074000")
        check(o.get("end") == "IDLE" and f(o, "ran") < 5, f"let go after 3 s: {o.get('end')}, {o['sum']['ran']} s")
        o = host(f"http://127.0.0.1:{m.port}/", 8, "dial=7074000", "touch=1")
        check(o.get("end") == "left", f"touched every second: kept to its end ({o.get('end')})")
        sent = [x["text"] for x in m.sent()]
        check(len(sent) == 8, f"no command of the knob's own to keep it: {len(sent)} sent, two sessions' openings")
    finally:
        m.stop()


def case_refusals(d):
    print("refusals: busy, the wrong path, an Origin check, no WebSDR there, nothing listening")
    m = Mock("--max-users", "1", "--others", "1")
    try:
        o = host(f"http://127.0.0.1:{m.port}/", 5)
        check(o.get("end") == "BUSY", f"busy: {o.get('end')}")
    finally:
        m.stop()
    m = Mock("--site", "maasbree", "--strict-path")
    try:
        o = host(f"http://127.0.0.1:{m.port}/", 5, "dial=7074000", "path=plain")
        check(o.get("end") == "NOT A WEBSDR", f"the other path, refused: {o.get('end')}")
    finally:
        m.stop()
    m = Mock("--origin")
    try:
        o = host(f"http://127.0.0.1:{m.port}/", 4)
        check(o.get("end") == "left" and m.stats()["no_origin"] == 0, f"its Origin taken: {o.get('end')}")
    finally:
        m.stop()
    owrx = os.path.join(os.path.dirname(ARGS.mock), "mock_owrx.py")
    m = Mock(script=owrx)
    try:
        o = host(f"http://127.0.0.1:{m.port}/", 4)
        check("end=NOT A WEBSDR" in o.get("info", ""), f"an OpenWebRX there: {o.get('info')}")
    finally:
        m.stop()
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    o = host(f"http://127.0.0.1:{port}/", 4)
    check(o.get("end") == "CAN'T REACH", f"nothing listening: {o.get('end')}")


def case_cut(d):
    print("a file stopped short, as a site's server was seen pausing one: asked again")
    m = Mock("--site", "maasbree", "--strict-path", "--cut", "websdr-sound.js:2000")
    try:
        o = host(f"http://127.0.0.1:{m.port}/", 4, "dial=7074000")
        st = m.stats()
        check("v11=1" in o.get("path", "") and st["cut"] == 1 and st["sound_js"] == 2,
              f"stopped short, asked again, ?v=11 found: {o.get('path')} cut={st['cut']} reads={st['sound_js']}")
        check(o.get("end") == "left", f"...its own path opened: {o.get('end')}")
        check(any("stopped short" in l for l in o["lines"]), "...and the log says so")
    finally:
        m.stop()
    m = Mock("--site", "maasbree", "--cut", "websdr-sound.js:2000:2")
    try:
        o = host(f"http://127.0.0.1:{m.port}/", 3, "dial=7074000")
        check("v11=1" in o.get("path", "") and m.stats()["cut"] == 2,
              f"short twice, no ?v=11 seen: taken for a distributed server's ({o.get('path')})")
    finally:
        m.stop()
    m = Mock("--site", "twente", "--cut", "tmp/bandinfo.js:3000:2")
    try:
        o = host(f"http://127.0.0.1:{m.port}/", 3, "dial=14074000")
        check("bands=1 " in o.get("info", "") and o.get("end") == "left",
              f"bandinfo.js short twice inside its band: the band kept, and it plays ({o.get('info', '')[:40]})")
    finally:
        m.stop()


def case_stall(d):
    print("a stall: 3 s of nothing, then the backlog left out in one jump")
    m = Mock("--site", "twente", "--stall", "5:3")
    try:
        o = host(f"http://127.0.0.1:{m.port}/", 14, "dial=14074000")
        s = o.get("sum", {})
        check(o.get("end") == "left" and int(s["jumps"]) >= 1, f"one jump: {s.get('jumps')}")
        check(abs(f(o, "pitch") - 1500) < 15, f"on its pitch after it: {s.get('pitch')}")
    finally:
        m.stop()


def case_tls(d):
    print("TLS in front, as nginx puts it before some sites")
    if not shutil.which("openssl"):
        print("  skip (no openssl)")
        return
    run = lambda *a: subprocess.run(["openssl", *a], cwd=d, check=True, capture_output=True)
    run("req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", "k.pem", "-out", "c.pem", "-days", "2",
        "-subj", "/CN=localhost", "-addext", "subjectAltName=DNS:localhost")
    m = Mock("--site", "twente", "--tls", f"{d}/c.pem:{d}/k.pem", "--origin")
    try:
        env_ca = os.path.join(d, "c.pem")
        p = subprocess.run([ARGS.host, f"https://localhost:{m.port}/", "5", "dial=14074000"],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=60,
                           env=dict(os.environ, KIWI_TEST_CA=env_ca))
        end = next((l.split(" ", 1)[1] for l in p.stdout.splitlines() if l.startswith("@END")), None)
        info = next((l for l in p.stdout.splitlines() if l.startswith("@INFO")), "")
        st = m.stats()
        check("bands=1" in info and end == "left", f"wss, bandinfo.js over https: {end} {info[:40]}")
        check(st["tls"] >= 4 and st["origins"] and st["origins"][-1].startswith("https://localhost:"),
              f"through the front, an https Origin: {st['origins']}")
    finally:
        m.stop()


CASES = {"stream": case_stream, "bands": case_bands, "turn": case_turn, "am": case_am, "mute": case_mute,
         "idle": case_idle, "refusals": case_refusals, "cut": case_cut, "stall": case_stall, "tls": case_tls}


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
            if name == "tls" and ARGS.no_tls:
                continue
            fn(d)
    print(f"{len(FAILS)} failed" + ("".join("\n  " + x for x in FAILS) if FAILS else ""))
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
