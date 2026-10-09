#!/usr/bin/env python3
"""The websdr firmware's receiver (wsdr_host: components/wsdr_client on its
session, as the firmware builds them) against tools/mock_wsdr.py, a boot at a
time: where a site starts it, its bands and band plan, the dial running from
one band into the next, receivers handed over and switched, a busy site, one
that is no WebSDR, the idle timeout, mute, the squelch, a restart, the Test.

  wsdr_client.py --host BIN --mock tools/mock_wsdr.py [--only a,b] [-v]

Loopback only: run it in a network namespace of its own."""
import argparse
import json
import os
import re
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
        self.port = None
        while True:
            line = self.p.stdout.readline()
            if not line:
                raise RuntimeError("the mock did not start")
            w = line.split()
            if w and w[0] == "LISTENING":
                self.port = int(w[1])
                break
        self.url = f"http://127.0.0.1:{self.port}/"

    def get(self, path):
        s = socket.create_connection(("127.0.0.1", self.port), timeout=5)
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
        return [x["text"] for x in self.get("/mock/sent")]

    def stop(self):
        self.p.terminate()
        try:
            self.p.wait(5)
        except subprocess.TimeoutExpired:
            self.p.kill()


def dead_url():
    return "http://127.0.0.1:9/"


def kv(line):
    return {m.group(1): m.group(2) if m.group(2) is not None else m.group(3)
            for m in re.finditer(r'(\w+)=(?:"([^"]*)"|(\S+))', line)}


class Run:
    """One boot of the knob: wsdr_host's lines, parsed."""

    def __init__(self, nvs, *cmds):
        p = subprocess.run([ARGS.host, nvs, *map(str, cmds)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                           text=True, timeout=300)
        self.lines, self.states, self.bands, self.turns, self.rx, self.tests = [], [], [], [], {}, []
        self.pitch, self.activated, self.timeouts, self.band_said = None, [], 0, []
        for line in p.stdout.splitlines():
            self.lines.append(line)
            if ARGS.verbose:
                print("    " + line)
            w = line.split()
            if not w:
                continue
            if w[0] == "@STATE":
                self.states.append(kv(line))
            elif w[0] == "@B":
                m = re.match(r'@B (\d+) "([^"]*)" (-?\d+) (-?\d+) (\d)', line)
                self.bands.append((m.group(2), int(m.group(3)), int(m.group(4)), m.group(5) == "1"))
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
            elif w[0] == "@RX":
                self.rx[int(w[1])] = kv(line)
            elif w[0] == "@TEST":
                self.tests.append(kv(line))
            elif "ERROR: AddressSanitizer" in line or "runtime error:" in line:
                check(False, "sanitizers: " + line.strip())

    def last(self):
        return self.states[-1] if self.states else {}


def nvs():
    return os.path.join(tempfile.mkdtemp(prefix="wsdr-nvs-"), "nvs.txt")


# ---------------------------------------------------------------- the cases

def case_twente():
    print("twente: where it starts, its band plan, a broadcast band in AM, its station")
    m = Mock("--site", "twente")
    try:
        r = Run(nvs(), "ident", "ON6URE", "save", "Twente=" + m.url, "untilrx", 0, 20, "bands", "state",
                "band", 0, "wait", 1, "state", "band", 8, "tune", 14074000, "mode", "usb", "wait", 1, "pitch", 3,
                "state", "off")
        s0 = r.states[0]
        check(r.timeouts == 0 and s0["link"] == "READY" and s0["line3"] == "WebSDR", "it plays")
        check(int(s0["f"]) == 14589800 and s0["mode"] == "usb", f"where its page starts, USB: {s0['f']} {s0['mode']}")
        check(s0["band"] == "hf" and s0["server"] == "Twente" and s0["name"] == "WebSDR", "its band; its names")
        names = [b[0] for b in r.bands]
        check(len(names) == 13 and names[:3] == ["2200 m", "630 m", "160 m"] and "49 m BC" in names and "31 m BC" in names,
              f"its band plan, named, in order: {names}")
        s1 = r.states[2]                    # untilrx's, "state"'s, then band 0's
        check(s1["mode"] == "lsb" and 135700 <= int(s1["f"]) <= 137800 and s1["line2"] == "2200 m  hf",
              f"its first range, an amateur band, in its middle, LSB: {s1['f']} {s1['mode']} \"{s1['line2']}\"")
        bc = names.index("49 m BC")
        check(r.band_said[0] == (0, "ok") and r.band_said[1] == (8, "ok"), "chosen")
        check(r.pitch is not None and abs(r.pitch - 1500) < 15, f"its station on 20 m at 1.5 kHz: {r.pitch}")
        st = m.stats()
        check(st["names"] == ["ON6URE"] and st["agents"][-1] == "VFO-Knob", f"listed as its call: {st['names']}")
        check(st["bandinfo"] == 1 and st["sound_js"] == 1 and st["index"] == 1 and st["upgrades"] == 1,
              "its page read once, one stream")
        check(st["paths"] == ["/~~stream"] and not st["bad_values"] and not st["unknown_keys"] and st["too_fast"] == 0,
              f"all it sent known, none too fast: {st['bad_values']} {st['too_fast']}")
        r = Run(nvs(), "save", m.url, "untilrx", 0, 20, "bands", "band", 3, "wait", 2, "state", "off")
        bc = next(i for i, b in enumerate(r.bands) if b[0] == "49 m BC")
        r = Run(nvs(), "save", m.url, "untilrx", 0, 20, "band", bc, "wait", 2, "state", "off")
        s = r.last()
        check(s["mode"] == "am" and int(s["f"]) == 6050000, f"a broadcast band: its middle, AM ({s['f']} {s['mode']})")
        check(14238 in m.stats()["rates"], "...wide enough that the site doubles its rate")
    finally:
        m.stop()


def case_cross():
    print("maasbree: its bands, the dial over the gaps between them, ?v=11")
    m = Mock("--site", "maasbree", "--strict-path")
    try:
        r = Run(nvs(), "save", m.url, "untilrx", 0, 20, "state", "bands", "tune", 1990000, "turn", 2, 10000,
                "turn", -3, 10000, "turn", -400, 1000, "tune", 21600000, "turn", 5, 10000, "tune", 7077000,
                "wait", 1, "pitch", 3, "state", "off")
        s0 = r.states[0]
        check(int(s0["f"]) == 3630000 and s0["mode"] == "lsb" and s0["band"] == "80m",
              f"where the site starts a listener: {s0['f']} {s0['mode']} {s0['band']}")
        check([b[0] for b in r.bands] == ["160m", "80m", "60m", "40m", "30m", "20m", "17m", "15m"],
              "its eight bands")
        check(r.turns[0] == 3468000, f"off 160 m's top, on at 80 m's bottom: {r.turns[0]}")
        check(r.turns[1] == 1991000, f"back down, at 160 m's top: {r.turns[1]}")
        check(r.turns[2] == 1799000, f"down to 160 m's bottom, the lowest: {r.turns[2]}")
        check(r.turns[3] == 21609000, f"up past 15 m's top: stopped there: {r.turns[3]}")
        check(r.pitch is not None and abs(r.pitch - 1500) < 20, f"40 m's station in LSB: {r.pitch}")
        st = m.stats()
        check(st["paths"] == ["/~~stream?v=11"] and not st["outside"] and not st["foreign_keys"]
              and st["too_fast"] == 0, f"its path; never outside a band: {st['outside']}")
    finally:
        m.stop()


def case_handover():
    print("hand-over: the one in use not reached, the next plays and is in use from then on")
    m = Mock()
    try:
        f = nvs()
        r = Run(f, "save", "Gone=" + dead_url(), "Mock=" + m.url, "state", "untilrx", 1, 30, "quit")
        check(r.states[0]["rx"] == "0", "the one in use first")
        check(r.timeouts == 0 and r.last().get("link") == "READY", "the next plays")
        check(1 in r.activated, "...in use from then on, saved")
        r2 = Run(f, "state", "untilrx", 1, 20, "off")
        check(r2.states[0]["rx"] == "1" and r2.timeouts == 0, "through a restart")
    finally:
        m.stop()


def case_busy():
    print("busy: passed by as a stand-in; the one in use waits five minutes")
    busy = Mock("--max-users", "1", "--others", "1")
    good = Mock()
    try:
        r = Run(nvs(), "save", dead_url(), busy.url, good.url, "untilrx", 2, 40, "wait", 2, "state", "off")
        check(r.timeouts == 0 and r.last()["rx"] == "2", "past the busy stand-in, the next plays")
        check(busy.stats()["refused_busy"] == 1, "the busy one asked once")
        busy.get("/mock/reset")
        r = Run(nvs(), "save", busy.url, "wait", 6, "state", "rxstate", 0, "off")
        s = r.last()
        check(s["why"] == "BUSY" and "busy, again in" in s["state"], f"its word: {s['why']}, \"{s['state']}\"")
        w = int(r.rx[0]["wait"])
        check(r.rx[0]["why"] == "BUSY" and 290 <= w <= 375, f"again in five minutes or so: {w} s")
        check(busy.stats()["refused_busy"] == 1, "not asked again meanwhile")
    finally:
        busy.stop()
        good.stop()


def case_not_wsdr():
    print("no WebSDR there: held until chosen again")
    owrx = os.path.join(os.path.dirname(ARGS.mock), "mock_owrx.py")
    m = Mock(script=owrx)
    try:
        r = Run(nvs(), "save", m.url, "wait", 3, "state", "rxstate", 0, "off")
        check(r.last()["why"] == "NOT A WEBSDR" and r.rx[0]["held"] == "1", f"held: {r.last()['why']}")
    finally:
        m.stop()


def case_switch():
    print("two sites: switched, each its own dial")
    a, b = Mock("--site", "twente"), Mock("--site", "maasbree")
    try:
        r = Run(nvs(), "save", a.url, b.url, "untilrx", 0, 20, "tune", 14080000, "wait", 0.5, "use", 1,
                "untilrx", 1, 20, "tune", 7050000, "wait", 0.5, "state", "use", 0, "untilrx", 0, 20, "off")
        check(r.timeouts == 0 and r.states[1]["rx"] == "1" and r.states[1]["band"] == "80m",
              f"the second plays, on its own start: {r.states[1].get('band')}")
        check(int(r.states[2]["f"]) == 7050000, f"its dial tuned: {r.states[2]['f']}")
        check(int(r.states[3]["f"]) == 14080000, f"back on the first: its own dial again ({r.states[3]['f']})")
        sa, sb = a.stats(), b.stats()
        check(sa["upgrades"] == 2 and sa["max_open"] == 1 and sb["upgrades"] == 1, "one stream at a time")
        check(sa["bandinfo"] == 1 and sb["bandinfo"] == 1, "each page read once a boot")
    finally:
        a.stop()
        b.stop()


def case_restart():
    print("a restart: the dial, the mode, the squelch kept")
    m = Mock()
    try:
        f = nvs()
        Run(f, "save", m.url, "untilrx", 0, 20, "tune", 7123000, "mode", "cw", "squelch", 40, "quit")
        r = Run(f, "untilrx", 0, 20, "state", "off")
        s = r.last()
        check(int(s["f"]) == 7123000 and s["mode"] == "cw" and s["sq"] == "100",
              f"as it was left: {s['f']} {s['mode']} squelch {s['sq']}")
        sent = m.sent()
        cw = [float(x.split("f=", 1)[1].split("&", 1)[0]) * 1000 for x in sent if "lo=-0.95&hi=-0.55" in x]
        check(cw and abs(cw[-1] - 7123750) <= 7,
              f"CW: the carrier 750 Hz above the station (on the site's step), its passband below: {cw[-1:]}")
        check(sent.count("GET /~~param?squelch=1") >= 2, "the squelch on, both boots")
    finally:
        m.stop()


def case_idle():
    print("the site's idle timeout: let go, and taken up at the next touch")
    m = Mock("--site", "maasbree", "--idle-page", "3000")
    try:
        r = Run(nvs(), "save", m.url, "untilrx", 0, 20, "wait", 5, "state", "turn", 1, 1000, "untilrx", 0, 20,
                "off")
        s = r.states[1]
        check(s["why"] == "IDLE" and s["state"] == "idle: turn the dial", f"let go: {s['why']} \"{s['state']}\"")
        check(r.timeouts == 0 and r.last()["link"] == "READY", "a turn of the dial: on again")
        check(m.stats()["upgrades"] == 2, "a stream, then another")
    finally:
        m.stop()


def case_mute():
    print("mute: the site sends silence; unmuted, its audio again")
    m = Mock()
    try:
        r = Run(nvs(), "save", m.url, "untilrx", 0, 20, "tune", 14074000, "mode", "usb", "mute", 1, "wait", 2,
                "state", "mute", 0, "wait", 1, "pitch", 3, "off")
        check(r.states[1]["audible"] == "0", "not audible muted")      # untilrx's, then muted
        check(m.stats()["silent_blocks"] > 50, f"silence sent meanwhile: {m.stats()['silent_blocks']}")
        check(r.pitch is not None and abs(r.pitch - 1500) < 15, f"then the station again: {r.pitch}")
    finally:
        m.stop()


def case_test():
    print("the page's Test: three small reads, never a stream")
    a, b = Mock("--site", "twente"), Mock("--site", "maasbree")
    try:
        r = Run(nvs(), "test", a.url, "test", b.url, "test", dead_url(), "off")
        t = r.tests
        check(t[0]["ok"] == "1" and t[0]["bands"] == "1" and t[0]["plan"] == "13" and t[0]["v11"] == "0"
              and t[0]["name"] == "WebSDR", f"one band and its plan: {t[0]}")
        check(t[1]["ok"] == "1" and t[1]["bands"] == "8" and t[1]["idle"] == "240" and t[1]["v11"] == "1",
              f"eight bands, 4 h idle, ?v=11: {t[1]}")
        check(t[2]["ok"] == "0" and t[2]["error"] == "CAN'T REACH", f"nothing there: {t[2]['error']}")
        check(a.stats()["upgrades"] == 0 and b.stats()["upgrades"] == 0, "no stream opened")
    finally:
        a.stop()
        b.stop()


CASES = ["twente", "cross", "handover", "busy", "not_wsdr", "switch", "restart", "idle", "mute", "test"]


def main():
    global ARGS
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", required=True)
    ap.add_argument("--mock", required=True)
    ap.add_argument("--only", default="")
    ap.add_argument("-v", "--verbose", action="store_true")
    ARGS = ap.parse_args()
    for name in CASES:
        if ARGS.only and name not in ARGS.only.split(","):
            continue
        globals()["case_" + name]()
    print(f"{len(FAILS)} failed" + ("".join("\n  " + x for x in FAILS) if FAILS else ""))
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
