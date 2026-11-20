#!/usr/bin/env python3
"""The tailchaser firmware's client on the PC (tailchaser_host) against
stand-in TailChaser nodes (tools/mock_tailchaser.py): what the knob does with
the repeaters the configuration page sets.

  - no repeaters: it says so, and asks nobody
  - the page refuses what is no address, too many repeaters, a bad name
  - two repeaters: the first read, its callsign, output, shift and tone,
    the talkgroup; a talker, a voice on the input with its S-meter, a carrier
    with none, the transmitter keyed
  - the blocks: a tap asks and does nothing, a second within four seconds
    does it with the control key; a second tap too late only asks again; the
    page's buttons; a block set at the repeater itself shown
  - the dial: the second repeater; a read key may not block (403)
  - a key refused (401); a row's key kept when the page sends none, and
    following its row when the rows move
  - a node that closes without a word, one cut short, one answering 500:
    backing off, then back
  - Tailscale: waiting for it (no DNS asked meanwhile), a name on the tailnet
    and a tailnet address through it, a restart asked when its settings
    change, a login address for the page, the knob's identity forgotten
  - the API keys and the auth key never in the log, nor on the page
"""
import argparse
import importlib.util
import json
import os
import secrets
import subprocess
import sys
import tempfile
import threading
import time
import urllib.parse
import urllib.request

FAILS = []
SEEN = []           # everything the page answered, for the secrets' check


def check(ok, what):
    print(("ok   " if ok else "FAIL ") + what, flush=True)
    if not ok:
        FAILS.append(what)
    return ok


def free_port():
    import socket
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


class Host:
    def __init__(self, binary, tmp, name, env=None):
        self.http = free_port()
        e = dict(os.environ, SHIM_HTTP_PORT=str(self.http), SHIM_NVS=os.path.join(tmp, f"nvs-{name}.txt"))
        e.update(env or {})
        self.p = subprocess.Popen([binary], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.STDOUT, text=True, env=e, bufsize=1)
        self.log = []
        self.at = []
        self.cv = threading.Condition()
        threading.Thread(target=self._read, daemon=True).start()
        if not self.wait_at(lambda l: l.startswith("@READY"), 10):
            raise SystemExit("tailchaser_host did not start:\n" + "\n".join(self.log[-30:]))

    def _read(self):
        for line in self.p.stdout:
            line = line.rstrip("\n")
            with self.cv:
                self.log.append(line)
                if line.startswith("@"):
                    self.at.append((time.time(), line))
                self.cv.notify_all()
            if os.environ.get("TC_VERBOSE") and not line.startswith("@STATUS"):
                print("   | " + line, flush=True)

    def wait_at(self, pred, timeout, since_index=0):
        end = time.time() + timeout
        with self.cv:
            while True:
                for i in range(since_index, len(self.at)):
                    if pred(self.at[i][1]):
                        return self.at[i]
                left = end - time.time()
                if left <= 0 or self.p.poll() is not None:
                    return None
                self.cv.wait(left)

    def mark(self):
        with self.cv:
            return len(self.at)

    def ask(self, cmd, answer, timeout=5):
        m = self.mark()
        self.p.stdin.write(cmd + "\n")
        self.p.stdin.flush()
        hit = self.wait_at(lambda l: l.startswith(answer), timeout, m)
        if not hit:
            raise SystemExit(f"no {answer} to {cmd}:\n" + "\n".join(self.log[-30:]))
        return hit[1][len(answer):].strip()

    def do(self, cmd):
        self.ask(cmd, "@OK " + cmd.split()[0])


    def status(self):
        s = self.ask("status", "@STATUS")
        SEEN.append(s)
        return json.loads(s)

    def until(self, pred, timeout, what):
        end = time.time() + timeout
        st = None
        while time.time() < end:
            st = self.status()
            try:
                if pred(st):
                    return check(True, what) and st
            except (KeyError, TypeError):
                pass
            time.sleep(0.1)
        check(False, what + "\n     last: " + json.dumps(st)[:900])
        return None

    def page(self, path, form=None):
        url = f"http://127.0.0.1:{self.http}{path}"
        data = urllib.parse.urlencode(form).encode() if form is not None else None
        with urllib.request.urlopen(urllib.request.Request(url, data=data), timeout=5) as r:
            body = r.read().decode()
        SEEN.append(body)
        return json.loads(body)

    def close(self):
        try:
            if self.p.poll() is None:
                self.p.stdin.write("quit\n")
                self.p.stdin.flush()
                self.p.wait(5)
        except Exception:
            self.p.kill()
        try:
            self.p.stdin.close()        # one that crashed: its pipe is gone
        except Exception:
            pass


def nodes_form(rows, **extra):
    """rows: (address, key or "", was[, name]); was -1 for a new row."""
    f = {"n": str(len(rows))}
    for i, row in enumerate(rows):
        a, k, w = row[:3]
        f[f"a{i}"] = a
        f[f"w{i}"] = str(w)
        if k:
            f[f"k{i}"] = k
        if len(row) > 3:
            f[f"m{i}"] = row[3]
    f.setdefault("ts", "0")
    f.update(extra)
    return f


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", required=True)
    ap.add_argument("--mock", required=True)
    a = ap.parse_args()
    spec = importlib.util.spec_from_file_location("mock_tailchaser", a.mock)
    mock = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mock)

    control = "tc_" + secrets.token_hex(32)
    read = "tc_" + secrets.token_hex(32)
    wrong = "tc_" + secrets.token_hex(32)
    auth = "tskey-auth-" + secrets.token_hex(24)
    A = mock.Node(role="transmitter", callsign="ON0AAA", control_key=control, read_key=read)
    B = mock.Node(role="simplex", callsign="ON0BBB", control_key=control, read_key=read)
    hosts = []
    with tempfile.TemporaryDirectory() as tmp:
        try:
            run(mock, A, B, control, read, wrong, auth, a.host, tmp, hosts)
        finally:
            for h in hosts:
                h.close()
            A.stop()
            B.stop()
        log = "\n".join(l for h in hosts for l in h.log)
        everything = log + "\n".join(SEEN)
        for name, secret in (("the control key", control), ("the read key", read), ("the wrong key", wrong),
                             ("the auth key", auth)):
            check(secret not in everything, f"{name} never in the log nor on the page")
    if FAILS:
        print(f"\n{len(FAILS)} failed")
        sys.exit(1)
    print("\ntailchaser client: all passed")


def run(mock, A, B, control, read, wrong, auth, binary, tmp, hosts):
    print("-- no repeaters")
    h = Host(binary, tmp, "one")
    hosts.append(h)
    st = h.until(lambda s: s["link"] == 0 and s["page"]["n"] == 0, 2, "no repeaters: the link down") or {}
    check(st.get("server") == "no repeaters" and st.get("tg_name") == "", "the face says so")
    check("no repeaters" in st.get("page", {}).get("phase", ""), "the page says so")
    g = h.page("/api/tailchaser")
    check(g["nodes"] == [] and g["ts"] is False and g["tsauth"] is False, "the page: nothing set")
    check(g["tsdefault"] == "vfo-knob-abcd", "the knob's name on a tailnet from its MAC")
    time.sleep(1)
    check(not A.taken() and not B.taken(), "nobody asked")

    print("-- the page refuses")
    r = h.page("/api/tailchaser", nodes_form([('ho"st', "", -1)]))
    check(r["ok"] is False and "no address" in r["why"], "what is no address: " + r.get("why", ""))
    r = h.page("/api/tailchaser", dict(nodes_form([("a", "", -1)] * 9)))
    check(r["ok"] is False and "8" in r["why"], "nine repeaters: " + r.get("why", ""))
    r = h.page("/api/tailchaser", nodes_form([("node", "", -1)], tsname="bad_name"))
    check(r["ok"] is False and "name" in r["why"], "a bad name on the tailnet: " + r.get("why", ""))
    r = h.page("/api/tailchaser", nodes_form([("node:99999", "", -1)]))
    check(r["ok"] is False, "a port that is none")

    print("-- two repeaters")
    aa, ba = f"127.0.0.1:{A.port}", f"127.0.0.1:{B.port}"
    r = h.page("/api/tailchaser", nodes_form([(aa, control, -1), (ba, "  " + read + " ", -1)]))
    check(r == {"ok": True, "restart": False}, "saved, no restart")
    g = h.page("/api/tailchaser")
    check([n["addr"] for n in g["nodes"]] == [aa, ba] and all(n["key"] for n in g["nodes"]),
          "the page: both, each with a key")
    st = h.until(lambda s: s["ready"] and s["tg_name"] == "ON0AAA", 5, "the first read: its callsign")
    if st:
        check(st["server"] == "438.800 -7.6 T131.8", "its output, shift and tone: " + st["server"])
        check(st["tg"] == 8 and st["talker"] == "", "talkgroup 8, nobody talking")
        check(st["reflector"] and not st["tx"] and not st["on_air"], "a reflector's face, nothing keyed")
        check(st["rx_only"], "the knob keys nothing: no TX DISABLED over the face")
        p = st["page"]
        check(p["live"] and p["role"] == "transmitter" and p["rx_site"] is True and p["tailnet"] is False,
              "the page: live, a transmit site, its receive site up")
        check(p["node"] == aa and p["addr"] == "127.0.0.1", "the page: where it is")
    gets = A.taken("GET", "/api/v1/status")
    check(gets and all(w == "control" for _, _, w, _ in gets), "asked with the control key")
    n0 = len(gets)
    time.sleep(3)
    n1 = len(A.taken("GET", "/api/v1/status"))
    check(3 <= n1 - n0 <= 7, f"about once or twice a second ({n1 - n0} in 3 s)")
    nb = len(B.taken("GET", "/api/v1/status"))
    check(nb >= 3, f"the second asked all the same, not shown ({nb})")

    A.talker("ON4ABC", 206)
    st = h.until(lambda s: s["talker"] == "ON4ABC", 3, "a talker on the talkgroup")
    if st:
        check(st["talker_info"] == "TG 206" and st["tg"] == 206, "its talkgroup: " + st["talker_info"])
    A.talker("")
    h.until(lambda s: s["talker"] == "" and s["last_talker"] == "ON4ABC", 3, "gone: the last talker")

    # A short over from the talkgroup: the reflector says it stopped while the
    # repeater, its call sent in Morse first, still has the voice to send and
    # its tail -- still that talker's, until the transmitter drops.
    A.talker("ON4DEF", 206)
    A.keyed(True)
    h.until(lambda s: s["talker"] == "ON4DEF" and s["tx"], 3, "a short over, on the air")
    A.talker("")
    time.sleep(3)
    st = h.status()
    check(st["talker"] == "ON4DEF" and st["tx"],
          f"the reflector done, the repeater still sending it: still its ({st['talker']!r})")
    A.keyed(False)
    h.until(lambda s: s["talker"] == "" and s["last_talker"] == "ON4DEF" and s["talker_ms"] < 3000, 3,
            "the transmitter down: the last talker, just now")
    A.keyed(True, "announcement")
    time.sleep(1.5)
    st = h.status()
    check(st["talker"] == "" and st["talker_info"] == "announcement",
          f"keyed for something else: not the last over's ({st['talker']!r})")
    A.keyed(False)
    # An over no read caught: the last talker from the net history, timed by it.
    A.net_over("ON4GHI", 40)
    st = h.until(lambda s: s["last_talker"] == "ON4GHI", 3, "an over between two reads: the last talker")
    if st:
        check(38000 <= st["talker_ms"] <= 45000, f"...ended when the node says ({st['talker_ms']} ms ago)")
    A.gate(True, True, 103, -18.0)
    st = h.until(lambda s: s["talker"] == "LOCAL", 3, "a voice on the input")
    if st:
        check(st["talker_info"] == "S9+10" and abs(st["smeter_dbm"] - -65.4) < 0.05,
              f"its S-meter, on the node's own scale: {st['talker_info']} {st['smeter_dbm']}")
        check(st["page"]["smeter"] == "S9+10" and st["page"]["open"] and st["page"]["cos"], "the page: the same")
    A.gate(True, False, 72, -45.0)
    st = h.until(lambda s: s["talker"] == "CARRIER", 3, "a carrier with no voice")
    if st:
        check(st["talker_info"] == "S6, no voice", "its S-meter: " + st["talker_info"])
    A.gate()
    h.until(lambda s: s["talker"] == "" and s["rx_db"] <= -119, 3, "quiet again: no level")
    A.keyed(True, "announcement")
    st = h.until(lambda s: s["tx"], 3, "the transmitter keyed")
    if st:
        check(st["talker_info"] == "announcement" and not st["on_air"], "why; the knob itself not on the air")
    A.keyed(False)
    h.until(lambda s: not s["tx"], 3, "the transmitter at rest")

    print("-- blocks (the menu held on the slab)")
    h.do("lock 1")
    st = h.until(lambda s: s["rx_locked"], 3, "RX: the receiver blocked")
    posts = A.taken("POST", "/api/v1/block")
    check(len(posts) == 1 and posts[0][2] == "control" and posts[0][3] == "rx=1", f"with the control key: {posts}")
    check(A.get("block.rx") is True, "the node has it")
    if st:
        check(st["note"] == "RX BLOCKED" and st["haptics"] == 1, "the face says so, a buzz")
        check(st["page"]["block_rx"] is True, "the page: blocked")
    h.do("lock 0")
    h.until(lambda s: not s["rx_locked"] and s["note"] == "RX OPEN", 3, "RX again: open")
    check(A.taken("POST")[-1][3] == "rx=0", "rx=0 asked")
    h.do("mute 1")
    h.until(lambda s: s["tx_blocked"] and s["note"] == "TX BLOCKED", 3, "TX: the transmitter blocked")
    check(len(A.taken("POST")) == 3, "one request a tap")
    r = h.page("/api/tailchaser/block", {"tx": "0"})
    check(r["ok"], "the page's button")
    h.until(lambda s: not s["tx_blocked"], 3, "the page opened it")
    check(A.taken("POST")[-1][3] == "tx=0" and A.get("block.tx") is False, "tx=0 asked")
    A.set("block.rx", True)
    h.until(lambda s: s["rx_locked"], 3, "a block set at the repeater itself shown")
    A.set("block.rx", False)
    h.until(lambda s: not s["rx_locked"], 3, "and its end")

    print("-- the dial")
    t0 = time.time()
    h.do("dial 1")
    st = h.until(lambda s: s["ready"] and s["tg_name"] == "ON0BBB", 5, "one detent: the second repeater")
    check(time.time() - t0 < 0.6, f"at once, read already ({time.time() - t0:.2f} s)")
    if st:
        check(st["server"] == "438.800 T131.8", "a simplex node: no shift: " + st["server"])
        check(st["page"]["sel"] == 1 and st["page"]["rx_site"] is None, "the page: the second, no receive site")
    h.do("lock 1")
    h.until(lambda s: s["note"] == "READ-ONLY KEY", 3, "a read key may not block")
    check(B.get("block.rx") is False and B.taken("POST")[-1][2] == "read", "the node refused it (403)")
    st = h.status()
    check(st["where"] == "simplex 2/2", "which one: its role and its place: " + st["where"])
    hp = st["haptics"]
    h.do("dial 3")
    time.sleep(1.0)
    st = h.status()
    check(st["page"]["sel"] == 1 and st["tg_name"] == "ON0BBB", "past the last: it stays there")
    check(st["haptics"] == hp + 1, "...with a click")
    h.do("dial -5")
    st = h.until(lambda s: s["ready"] and s["tg_name"] == "ON0AAA" and s["page"]["sel"] == 0, 5,
                 "far the other way: the first, not round to the last")
    if st:
        check(st["where"] == "transmitter 1/2", "the first's role and place: " + st["where"])
    h.do("dial 1")
    h.until(lambda s: s["ready"] and s["tg_name"] == "ON0BBB", 5, "and on to the second again")

    print("-- keys")
    r = h.page("/api/tailchaser", nodes_form([(aa, "", 0), (ba, wrong, 1)]))
    check(r["ok"], "the second's key replaced, the first's kept")
    st = h.until(lambda s: s["note"] == "KEY REFUSED" and s["link"] == 0, 5, "a key refused: the link down")
    if st:
        check(st["page"]["refused"] and "admin portal" in st["page"]["phase"], "the page says where to make one")
        check(st["tg_name"] == "127.0.0.1" and st["server"] == "key refused",
              "the face: the node's address and why, nothing of its status: " + st["server"])
    k0 = len(B.taken("GET"))
    time.sleep(3)
    check(len(B.taken("GET")) == k0, "not asked again at once")
    r = h.page("/api/tailchaser", nodes_form([(ba, "", 1), (aa, "", 0)]))
    check(r["ok"], "the rows swapped, no keys sent")
    st = h.until(lambda s: s["ready"] and s["tg_name"] == "ON0AAA", 5, "the first's key followed its row")
    g = h.page("/api/tailchaser")
    check([n["addr"] for n in g["nodes"]] == [ba, aa], "the page: the new order")
    r = h.page("/api/tailchaser", nodes_form([(ba, "", 0), (aa, "", 1), (f"127.0.0.1:{B.port}", "", -1)]))
    h.do("dial 1")
    h.until(lambda s: s["note"] == "NO KEY", 5, "a repeater with no key")
    h.do("dial -1")
    h.until(lambda s: s["ready"] and s["tg_name"] == "ON0AAA", 5, "back on the first")

    print("-- names")
    third = f"127.0.0.1:{B.port}"
    r = h.page("/api/tailchaser", nodes_form([(ba, "", 0), (aa, "", 1, "  ON0AAA-S "), (third, "", 2)]))
    check(r["ok"], "a name for the second")
    h.until(lambda s: s["ready"] and s["tg_name"] == "ON0AAA-S", 5, "the face: the owner's name, not the callsign")
    g = h.page("/api/tailchaser")
    check([n.get("name") for n in g["nodes"]] == ["", "ON0AAA-S", ""], f"the page: the names: {g['nodes']}")
    r = h.page("/api/tailchaser", nodes_form([(ba, "", 0), (aa, "", 1, "x" * 17), (third, "", 2)]))
    check(not r["ok"] and "16" in r.get("why", ""), f"17 characters refused: {r}")
    check(h.page("/api/tailchaser")["nodes"][1]["name"] == "ON0AAA-S", "...and the name kept")

    print("-- the busy one shown")
    r = h.page("/api/tailchaser", nodes_form([(aa, "", 1, "ON0AAA-S"), (ba, read, -1)], hold="2"))
    check(r["ok"], "the hold set to 2 s")
    check(h.page("/api/tailchaser")["hold"] == 2, "the page: the hold")
    h.do("dial -1")
    h.until(lambda s: s["ready"] and s["tg_name"] == "ON0AAA-S", 5, "the first shown")
    t0 = time.time()
    B.talker("ON4XYZ", 1745)
    st = h.until(lambda s: s["tg_name"] == "ON0BBB" and s["talker"] == "ON4XYZ", 6, "a talker on the second: shown")
    if st:
        check(time.time() - t0 >= 1.2, f"...once the dial was left alone for the hold ({time.time() - t0:.1f} s)")
        check(st["page"]["sel"] == 1 and st["where"] == "simplex 2/2", "the second's place: " + st["where"])
    A.talker("ON4ABC", 206)
    time.sleep(3)
    check(h.status()["tg_name"] == "ON0BBB", "the first busy too: the one shown kept while it is busy")
    B.talker("")
    t0 = time.time()
    st = h.until(lambda s: s["tg_name"] == "ON0AAA-S" and s["talker"] == "ON4ABC", 6,
                 "the second quiet for the hold: the first, still busy")
    check(time.time() - t0 >= 1.5, f"...not before the hold ({time.time() - t0:.1f} s)")
    A.talker("")
    B.gate(True, False, 72, -45.0)
    B.keyed(True, "announcement")
    time.sleep(4)
    check(h.status()["tg_name"] == "ON0AAA-S", "a carrier and an announcement on the second: not busy")
    B.gate()
    B.keyed(False)
    B.gate(True, True, 103, -18.0)
    h.until(lambda s: s["tg_name"] == "ON0BBB" and s["talker"] == "LOCAL", 6, "a voice on its input: busy, shown")
    B.gate()
    h.do("dial -1")
    A.talker("ON4ABC", 206)
    h.until(lambda s: s["tg_name"] == "ON0AAA-S", 3, "turned to the first")
    A.talker("")
    B.talker("ON4XYZ", 1745)
    time.sleep(1.0)
    for _ in range(3):
        time.sleep(0.6)
        h.do("dial -1")             # at the first end: a hand on the knob all the same
    check(h.status()["tg_name"] == "ON0AAA-S", "the knob in use: kept, the other busy or not")
    h.until(lambda s: s["tg_name"] == "ON0BBB", 5, "left alone: the busy one")
    B.talker("")
    r = h.page("/api/tailchaser", nodes_form([(aa, "", 0, "ON0AAA-S"), (ba, "", 1)], hold="0"))
    check(r["ok"] and h.page("/api/tailchaser")["hold"] == 0, "the hold 0")
    h.do("dial -1")
    h.until(lambda s: s["ready"] and s["tg_name"] == "ON0AAA-S", 5, "the first")
    B.talker("ON4XYZ", 1745)
    time.sleep(3)
    check(h.status()["tg_name"] == "ON0AAA-S", "0: the dial's alone")
    B.talker("")
    r = h.page("/api/tailchaser", nodes_form([(aa, "", 0), (ba, "", 1)], hold="x"))
    check(not r["ok"] and "3600" in r.get("why", ""), "a hold that is no number refused")
    r = h.page("/api/tailchaser", nodes_form([(aa, "", 0), (ba, "", 1)], hold="3601"))
    check(not r["ok"], "nor over an hour")
    r = h.page("/api/tailchaser", nodes_form([(aa, "", 0, "ON0AAA-S"), (ba, "", 1)]))
    check(r["ok"] and h.page("/api/tailchaser")["hold"] == 0, "no hold in the form: kept")

    print("-- the audio and the S-meter")
    r = h.page("/api/tailchaser", nodes_form([(aa, "", 0, "ON0AAA-S"), (ba, "", 1)], hold="0"))
    check(r["ok"], "the hold 0, for the audio's turns")
    h.do("dial -1")
    st = h.until(lambda s: s["ready"] and s["tg_name"] == "ON0AAA-S"
                 and s["page"]["audio"]["state"] == "off on the repeater", 5,
                 "its listening off: the page says so")
    time.sleep(1)
    check(A.ws_connects == 0 and not A.ws_refused, "...and it is not asked: its status says so")
    check(st and st["smeter_dbm"] == -127.0, "no signal: S0")
    A.gate(True, True, 80, -18.0)               # its receive site: lo 30, hi 130
    st = h.until(lambda s: abs(s["smeter_dbm"] - -85.0) < 0.2, 3, "a signal: 80 of 30..130, S7")
    A.gate()
    h.until(lambda s: s["smeter_dbm"] == -127.0, 3, "gone: S0 again")
    A.set("monitor", True)
    st = h.until(lambda s: s["page"]["audio"]["state"] == "listening", 5, "its listening on: listened to")
    check(A.listeners() == 1 and B.listeners() == 0, "the one shown only")
    s0 = h.status()["samples"]
    time.sleep(1)
    check(h.status()["samples"] == s0, "nothing transmitted: nothing played")
    A.keyed(True)
    st = h.until(lambda s: s["samples"] - s0 >= 16000, 3, "keyed: what it transmits played")
    if st:
        check(-12.5 < st["rx_db"] < -11.5, f"the voice's level: the tone's -12 dBFS ({st['rx_db']})")
        check(st["page"]["audio"]["chunks"] >= 10 and st["page"]["audio"]["muted"] is False, "the page counts it")
    A.keyed(False)
    h.until(lambda s: s["rx_db"] <= -119.0, 2, "unkeyed: the level falls")
    time.sleep(5.5)
    check(A.ws_pongs >= 1, f"its pings answered ({A.ws_pongs})")
    h.do("amute 1")
    st = h.until(lambda s: s["spk_muted"] and s["page"]["audio"]["muted"], 2, "the speaker muted")
    s1 = st["samples"] if st else 0
    A.keyed(True)
    h.until(lambda s: s["rx_db"] > -13.0, 3, "muted: the level still there")
    time.sleep(0.5)
    check(h.status()["samples"] == s1 and A.listeners() == 1, "...nothing played, the stream kept")
    A.keyed(False)
    B.set("monitor", True)
    f0 = h.status()["flushes"]
    h.do("dial 1")
    st = h.until(lambda s: s["tg_name"] == "ON0BBB" and s["page"]["audio"]["state"] == "listening"
                 and B.listeners() == 1, 5, "turned to the second: its audio")
    check(A.listeners() == 0 and A.ws_closes >= 1, "the first let go, with a close")
    check(st and st["spk_muted"] and st["flushes"] > f0, "still muted; the first's audio dropped")
    B.keyed(True)
    time.sleep(0.6)
    check(h.status()["samples"] == s1, "muted for the second too")
    h.do("amute 0")
    st = h.until(lambda s: not s["spk_muted"] and s["samples"] - s1 >= 8000, 3, "the speaker on: the second heard")
    B.gate(True, True, 100, -18.0)              # its own receiver: lo 79, hi 100
    h.until(lambda s: abs(s["smeter_dbm"] - -33.0) < 0.2, 3, "full scale: S9+40")
    B.gate()
    B.cut_listeners()
    t0 = time.time()
    h.until(lambda s: B.listeners() == 1 and s["page"]["audio"]["state"] == "listening", 5, "cut: listened to again")
    check(time.time() - t0 < 3.5, f"...soon ({time.time() - t0:.1f} s)")
    B.keyed(False)
    B.set("monitor", False)
    h.until(lambda s: s["page"]["audio"]["state"] == "off on the repeater" and B.listeners() == 0, 3,
            "its listening turned off: let go")
    h.do("amute 1")
    h.until(lambda s: s["spk_muted"], 2, "muted again")
    h.close()
    h = Host(binary, tmp, "one")
    hosts.append(h)
    h.until(lambda s: s["ready"] and s["spk_muted"], 5, "the mute kept across a restart")
    h.do("amute 0")
    h.until(lambda s: not s["spk_muted"], 2, "on again")
    B.set("monitor", True)
    for _ in range(4):
        B.ws_live.add(object())                 # four listeners there already
    h.do("dial 1")
    h.until(lambda s: s["tg_name"] == "ON0BBB" and s["page"]["audio"]["state"] == "four listeners there", 5,
            "four listeners there: said")
    check(503 in B.ws_refused, "the node's 503")
    B.ws_live.clear()
    B.set("monitor", False)
    A.set("monitor", False)
    h.do("dial -1")

    print("-- a node in trouble")
    for mode, word in (("drop", "no answer"), ("cut", "cut short"), ("500", "answered 500"),
                       ("garbage", "cut short")):
        A.mode = mode
        st = h.until(lambda s: not s["ready"] and word in s["page"]["phase"]
                     and s["server"] in ("no answer", "answered 500"), 8, f"{mode}: {word}")
        A.mode = "ok"
        h.until(lambda s: s["ready"], 12, f"{mode}: back")
    A.mode = "drop"
    t0 = time.time()
    h.until(lambda s: s["link"] == 0 and s["closes"] >= 1, 15, "three failures: down")
    n0 = len(A.taken("GET"))
    time.sleep(6)
    check(len(A.taken("GET")) - n0 <= 2, "backing off")
    A.mode = "ok"
    st = h.until(lambda s: s["ready"], 20, "back up after the backoff")
    if st:
        check(st["connects"] >= 2, f"counted again ({st['connects']})")
    h.close()

    print("-- Tailscale")
    h = Host(binary, tmp, "ts", {"TC_TS_END": "up", "TC_TS_AFTER": "2000", "TC_TS_PEERS": "tcnode=100.100.1.2"})
    hosts.append(h)
    r = h.page("/api/tailchaser", nodes_form([(f"tcnode:{A.port}", control, -1), (f"100.64.1.2:{A.port}", control, -1)],
                                             ts="1", tsauth=auth, tsname="Bench-Knob"))
    check(r == {"ok": True, "restart": False}, "Tailscale on, before it started: no restart")
    h.wait_at(lambda l: l.startswith("@TSSTART"), 3)
    check(any(l == "@TSSTART name=bench-knob auth=set" for l in h.log), "started with its name and the auth key")
    st = h.until(lambda s: "waiting for Tailscale" in s["page"]["phase"] and s["link"] == 1
                 and s["server"] == "waiting for Tailscale", 2, "waiting for Tailscale, connecting, the face says so")
    if st:
        check(st["closes"] == 0, "not counted as a failure")
    m = h.mark()
    st = h.until(lambda s: s["ready"] and s["page"]["tailnet"], 6, "on the tailnet: a name through it")
    check(h.wait_at(lambda l: l == f"@TSCONN 100.100.1.2:{A.port}", 1, m) is not None, "to its tailnet address")
    if st:
        t = st["page"]["ts"]
        check(t["state"] == "connected" and t["ip"] == "100.101.102.103" and t["peers"] == 1, f"the page: {t}")
        check(st["page"]["addr"] == "100.100.1.2", "the page: the address it went to")
    h.do("dial 1")
    m = h.mark()
    st = h.until(lambda s: s["ready"] and s["page"]["sel"] == 1 and s["page"]["tailnet"], 5,
                 "a tailnet address: through it")
    check(h.wait_at(lambda l: l == f"@TSCONN 100.64.1.2:{A.port}", 1, m) is not None, "the tunnel's")
    g = h.page("/api/tailchaser")
    check(g["ts"] is True and g["tsauth"] is True and g["tsname"] == "bench-knob", "the page: on, a key, its name")
    r = h.page("/api/tailchaser", nodes_form([(f"tcnode:{A.port}", "", 0), (f"100.64.1.2:{A.port}", "", 1)],
                                             ts="1", tsname="bench-knob"))
    check(r == {"ok": True, "restart": False}, "nothing of Tailscale changed: no restart")
    r = h.page("/api/tailchaser", nodes_form([(f"tcnode:{A.port}", "", 0)], ts="1", tsname="other"))
    check(r == {"ok": True, "restart": True}, "its name changed: a restart")
    r = h.page("/api/tailchaser", nodes_form([(f"tcnode:{A.port}", "", 0)], ts="1", tsclear="1"))
    check(r["restart"] is True and h.page("/api/tailchaser")["tsauth"] is False, "the auth key cleared")
    r = h.page("/api/tailchaser/forget", {})
    check(r == {"ok": True, "restart": True} and h.wait_at(lambda l: l == "@TSFORGET", 2) is not None,
          "its identity forgotten, a restart")
    h.close()

    h = Host(binary, tmp, "login", {"TC_TS_END": "login", "TC_TS_AFTER": "200"})
    hosts.append(h)
    m = h.mark()
    h.page("/api/tailchaser", nodes_form([(f"tcnode:{A.port}", control, -1)], ts="1"))
    h.until(lambda s: s["page"]["ts"]["login"].startswith("https://login.tailscale.com/")
            and "waiting for Tailscale" in s["page"]["phase"] and s["link"] == 1, 3,
            "no auth key: a login address for the page, waiting, connecting")
    check(any(l == "@TSSTART name=vfo-knob-abcd auth=none" for l in h.log), "started for a login, its own name")
    time.sleep(1.5)
    check(h.wait_at(lambda l: l.startswith("@TSCONN"), 0.1, m) is None, "nothing tried meanwhile")
    h.close()

    print("-- Tailscale that crashes the knob as it starts")
    crash = {"TC_TS_CRASH": "1"}
    h = Host(binary, tmp, "crash", crash)
    hosts.append(h)
    try:
        h.page("/api/tailchaser", nodes_form([(f"127.0.0.1:{A.port}", control, -1)], ts="1"))
    except Exception:
        pass
    h.p.wait(5)
    check(h.p.returncode not in (0, None) and "@CRASH" in h.log, "the first start: down")
    h = Host(binary, tmp, "crash", crash)
    hosts.append(h)
    h.p.wait(5)
    check(h.p.returncode not in (0, None) and "@CRASH" in h.log, "the second start: down again")
    h = Host(binary, tmp, "crash", crash)
    hosts.append(h)
    st = h.until(lambda s: s["page"]["ts"]["held"] and s["ready"], 5,
                 "the third: Tailscale left off, the repeater read as it can be")
    check("@CRASH" not in h.log and not any(l.startswith("@TSSTART") for l in h.log), "...not started")
    if st:
        check(st["page"]["ts"]["state"] == "off", "the page: off, and held")
    h.close()
    h = Host(binary, tmp, "crash", {"TC_BUILD": "hostbld1"})
    hosts.append(h)
    h.wait_at(lambda l: l.startswith("@TSSTART"), 3)
    check(any(l.startswith("@TSSTART") for l in h.log), "a new firmware: tried again without a save")
    h.close()
    # That start counted (it was stopped before a minute): one more that
    # crashes makes two on this firmware, and the next leaves it off.
    h = Host(binary, tmp, "crash", {**crash, "TC_BUILD": "hostbld1"})
    hosts.append(h)
    h.p.wait(5)
    check("@CRASH" in h.log, "a crash on the new firmware")
    h = Host(binary, tmp, "crash", {**crash, "TC_BUILD": "hostbld1"})
    hosts.append(h)
    st = h.until(lambda s: s["page"]["ts"]["held"], 5, "...and held again after two")
    r = h.page("/api/tailchaser", nodes_form([(f"127.0.0.1:{A.port}", "", 0)], ts="1"))
    check(r == {"ok": True, "restart": True}, "saved again: a restart to try it")
    h.close()
    h = Host(binary, tmp, "crash", {"TC_BUILD": "hostbld1"})
    hosts.append(h)
    h.wait_at(lambda l: l.startswith("@TSSTART"), 3)
    check(any(l.startswith("@TSSTART") for l in h.log), "after the save: tried again")
    h.close()

    h = Host(binary, tmp, "refused", {"TC_TS_END": "refused", "TC_TS_AFTER": "200"})
    hosts.append(h)
    h.page("/api/tailchaser", nodes_form([(f"100.64.1.2:{A.port}", control, -1)], ts="1"))
    h.until(lambda s: "Tailscale is refused" in s["page"]["phase"] and s["page"]["ts"]["state"] == "refused", 4,
            "Tailscale refused: a tailnet address not reached")
    h.close()


if __name__ == "__main__":
    main()
