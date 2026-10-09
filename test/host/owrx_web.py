#!/usr/bin/env python3
"""The openwebrx firmware's configuration and radio pages, in a browser:
owrx_web_host (components/webcfg and components/owrx_client on the PC, the
knob's own pages embedded as the firmware embeds them) and headless Chromium,
driven over its DevTools protocol, against tools/mock_owrx.py -- what a
click on the page does, and what the receiver is sent of it.

  config    the receivers' list: an address pasted whole, cut to what the
            knob keeps; Test reads the mock's status.json -- what it is, its
            SDRs and bands -- and names a receiver that has no name, never
            opening a session; a dead address, and one that is no
            OpenWebRX, said as such; Save keeps the list without moving off
            the one playing; another In use and Save, taken over at once;
            one that bans the knob held, the next playing in its place, as
            each row says
  radio     the radio page on the receiver: its name, software and
            listeners, the meter in its dB on its own scale; its bands, the
            one it is on marked, another asked for and the rest greyed for
            11 s after; a frequency outside the band refused on the page,
            one inside sent; mode, passband and squelch (its dB) sent to the
            receiver; SAM on OpenWebRX+ only; the receivers, another taken
            over at once
  tls       Test on an http:// address that sends the knob on to https://:
            the box says where, and Save keeps it

  owrx_web.py --host BIN --mock tools/mock_owrx.py [--chromium PATH] [--only a,b] [-v]

Loopback only: run it in a network namespace of its own."""
import argparse
import base64
import json
import os
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.parse
import urllib.request

ARGS = None
FAILS = []
AUTH = "Basic " + base64.b64encode(b"admin:admin").decode()


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what, flush=True)
    if not cond:
        FAILS.append(what)


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


# ------------------------------------------------------------------ the mock

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
        threading.Thread(target=lambda: [None for _ in self.p.stdout], daemon=True).start()
        self.control = self.control or self.port
        self.url = f"http://127.0.0.1:{self.port}/"

    def get(self, path):
        with urllib.request.urlopen(f"http://127.0.0.1:{self.control}{path}", timeout=5) as r:
            return json.loads(r.read())

    def stats(self):
        return self.get("/mock/stats")

    def sent(self, kind=None):
        out = []
        for m in self.get("/mock/sent"):
            try:
                j = json.loads(m["text"])
            except ValueError:
                continue
            if not kind or j.get("type") == kind:
                out.append(j)
        return out

    def last_dsp(self):
        """The dspcontrol params as the receiver has them now: every one sent, in turn."""
        p = {}
        for j in self.sent("dspcontrol"):
            p.update(j.get("params") or {})
        return p

    def stop(self):
        self.p.terminate()
        try:
            self.p.wait(5)
        except subprocess.TimeoutExpired:
            self.p.kill()


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


# ------------------------------------------------------------------ the knob

class Knob:
    """One boot of the knob with its web server: owrx_web_host, serving until
    it is stopped."""

    def __init__(self, *saves, env=None):
        self.dir = tempfile.mkdtemp(prefix="owrx-web-")
        self.port = free_port()
        self.lines = []
        args = [ARGS.host, os.path.join(self.dir, "nvs")]
        if saves:
            args += ["save", *saves]
        args += ["web", "serve"]
        self.p = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                  text=True, env=dict(os.environ, SHIM_HTTP_PORT=str(self.port), **(env or {})))
        self.up = threading.Event()
        threading.Thread(target=self._read, daemon=True).start()
        if not self.up.wait(10):
            raise RuntimeError("the knob's web server did not start")
        self.base = f"http://127.0.0.1:{self.port}"

    def _read(self):
        for line in self.p.stdout:
            line = line.rstrip()
            self.lines.append(line)
            if ARGS.verbose:
                print("    | " + line, flush=True)
            if line.startswith("@WEB"):
                self.up.set()

    def api(self, path, data=None):
        req = urllib.request.Request(self.base + path, headers={"Authorization": AUTH},
                                     data=urllib.parse.urlencode(data).encode() if data is not None else None)
        try:
            with urllib.request.urlopen(req, timeout=60) as r:
                body = r.read()
                return r.status, json.loads(body) if body[:1] in (b"{", b"[") else body.decode()
        except urllib.error.HTTPError as e:
            return e.code, e.read().decode()

    def radio(self):
        return self.api("/api/radio")[1]

    def radios(self):
        return self.api("/api/radios")[1]

    def until(self, cond, secs, what):
        end = time.time() + secs
        while time.time() < end:
            try:
                if cond():
                    return True
            except (OSError, KeyError, TypeError, ValueError):
                pass
            time.sleep(0.2)
        print(f"  ...timed out waiting for {what}", flush=True)
        return False

    def stop(self):
        try:
            self.p.stdin.close()
            self.p.wait(10)
        except (OSError, subprocess.TimeoutExpired):
            self.p.kill()
        shutil.rmtree(self.dir, ignore_errors=True)


# ------------------------------------------------------------------ the browser

class CDP:
    """Chromium's DevTools protocol over a WebSocket: a command, its answer."""

    def __init__(self, url):
        u = urllib.parse.urlparse(url)
        self.s = socket.create_connection((u.hostname, u.port), timeout=60)
        key = base64.b64encode(os.urandom(16)).decode()
        self.s.sendall(f"GET {u.path} HTTP/1.1\r\nHost: {u.hostname}:{u.port}\r\nUpgrade: websocket\r\n"
                       f"Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n".encode())
        data = b""
        while b"\r\n\r\n" not in data:
            c = self.s.recv(4096)
            if not c:
                raise ConnectionError("the browser closed during the upgrade")
            data += c
        head, _, self.buf = data.partition(b"\r\n\r\n")
        if b" 101 " not in head.split(b"\r\n")[0]:
            raise ConnectionError(head.split(b"\r\n")[0].decode())
        self.id = 0
        self.events = []

    def _need(self, n):
        while len(self.buf) < n:
            c = self.s.recv(1 << 20)
            if not c:
                raise ConnectionError("the browser closed")
            self.buf += c
        out, self.buf = self.buf[:n], self.buf[n:]
        return out

    def _frame(self):
        b0, b1 = self._need(2)
        n = b1 & 0x7F
        if n == 126:
            n = struct.unpack("!H", self._need(2))[0]
        elif n == 127:
            n = struct.unpack("!Q", self._need(8))[0]
        return b0 & 0x80, b0 & 0x0F, self._need(n)

    def _message(self):
        parts = []
        while True:
            fin, op, p = self._frame()
            if op == 8:
                raise ConnectionError("the browser closed")
            if op in (0, 1, 2):
                parts.append(p)
                if fin:
                    return json.loads(b"".join(parts))

    def send(self, method, **params):
        self.id += 1
        p = json.dumps({"id": self.id, "method": method, "params": params}).encode()
        mask = os.urandom(4)
        n = len(p)
        hdr = bytes([0x81]) + (bytes([0x80 | n]) if n < 126 else bytes([0x80 | 126]) + struct.pack("!H", n)
                               if n < 65536 else bytes([0x80 | 127]) + struct.pack("!Q", n))
        self.s.sendall(hdr + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(p)))
        while True:
            m = self._message()
            if m.get("id") == self.id:
                if "error" in m:
                    raise RuntimeError(f"{method}: {m['error']}")
                return m.get("result", {})
            self.events.append(m)
            del self.events[:-200]


class Browser:
    def __init__(self, knob):
        self.knob = knob
        self.dir = tempfile.mkdtemp(prefix="owrx-web-chromium-")
        self.p = subprocess.Popen([ARGS.chromium, "--headless=new", "--no-sandbox", "--disable-gpu", "--no-first-run",
                                   "--no-default-browser-check", "--disable-extensions", "--remote-debugging-port=0",
                                   f"--user-data-dir={self.dir}", "about:blank"],
                                  stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        f = os.path.join(self.dir, "DevToolsActivePort")
        end = time.time() + 20
        while not os.path.exists(f) or not open(f).read().strip():
            if time.time() > end:
                raise RuntimeError("the browser did not start")
            time.sleep(0.1)
        port = int(open(f).read().split()[0])
        with urllib.request.urlopen(f"http://127.0.0.1:{port}/json/list", timeout=5) as r:
            page = next(t for t in json.loads(r.read()) if t["type"] == "page")
        self.cdp = CDP(page["webSocketDebuggerUrl"])
        self.cdp.send("Network.enable")
        # The page's login, as a browser asked once would send it with every request.
        self.cdp.send("Network.setExtraHTTPHeaders", headers={"Authorization": AUTH})
        self.cdp.send("Page.enable")

    def go(self, path, ready):
        self.cdp.send("Page.navigate", url=self.knob.base + path)
        return self.until(ready, 15, f"{path} to load")

    def js(self, expr):
        r = self.cdp.send("Runtime.evaluate", expression=expr, awaitPromise=True, returnByValue=True)
        if "exceptionDetails" in r:
            d = r["exceptionDetails"]
            raise RuntimeError((d.get("exception") or {}).get("description") or d.get("text"))
        return r["result"].get("value")

    def until(self, expr, secs, what):
        end = time.time() + secs
        last = None
        while time.time() < end:
            try:
                last = self.js(expr)
                if last:
                    return last
            except RuntimeError as e:
                last = str(e)
            time.sleep(0.2)
        print(f"  ...timed out waiting for {what} (last: {last!r})", flush=True)
        return None

    def text(self, sel):
        return self.js(f"(document.querySelector({json.dumps(sel)})||{{}}).textContent||''")

    def stop(self):
        self.p.terminate()
        try:
            self.p.wait(5)
        except subprocess.TimeoutExpired:
            self.p.kill()
        shutil.rmtree(self.dir, ignore_errors=True)


# The configuration page's rows, as the browser sees them.
ROW = "document.querySelectorAll('#rlist .sdr')[{i}]"


def row_set(b, i, key, value):
    """Typed into row i's box, as a person types and leaves it."""
    b.js(f"(()=>{{const e={ROW.format(i=i)}.querySelector('input[data-k={key}]');"
         f"e.value={json.dumps(value)};e.dispatchEvent(new Event('input'));e.dispatchEvent(new Event('change'))}})()")


def row_val(b, i, key):
    return b.js(f"{ROW.format(i=i)}.querySelector('input[data-k={key}]').value")


def row_test(b, i, secs=30):
    """Its Test pressed: what it says when done."""
    b.js(f"{ROW.format(i=i)}.querySelector('[data-act=test]').click()")
    return b.until(f"(()=>{{const t={ROW.format(i=i)}.querySelector('.sdrres').textContent;"
                   f"return /^[\\u2713\\u2717]|^The test|^Not an address|^An address/.test(t)&&t}})()", secs,
                   f"row {i}'s Test")


def row_state(b, i):
    return b.js(f"{ROW.format(i=i)}.querySelector('.rxst').textContent")


def save(b):
    b.js("$('save').click()")
    return b.until("/Saved|not saved|failed/.test($('msg').textContent)&&$('msg').textContent", 20, "the save")


# ------------------------------------------------------------------ the cases

def case_config():
    m1 = Mock("--name", "MOCK One")
    m2 = Mock("--prefix", "/OWRX/", "--fork", "upstream", "--name", "MOCK Two | Somewhere far")
    m3 = Mock("--no-status")
    m4 = Mock("--banned", "--name", "MOCK Banned")
    k = Knob(f"One={m1.url}")
    b = Browser(k)
    try:
        check(k.until(lambda: k.radio()["ready"], 20, "the knob to play"), "the knob plays the first receiver")
        check(b.go("/config", "typeof RIGS!=='undefined'&&RIGS&&RIGS.length===1"), "the configuration page, one receiver")
        check(b.text("#rtitle") == "OpenWebRX receivers", f"its title: {b.text('#rtitle')!r}")
        check(b.js("!$('owrxnote').hidden&&$('rlistnote').hidden&&$('ubernote').hidden"),
              "the receivers' note, not the radios' one")
        check(b.js("!document.querySelector('#rlist input[data-k=port]')&&!document.querySelector('#rlist input[data-k=pass]')"),
              "no port box, no password: the address whole, and no login")
        check(b.text("#radd") == "Add a receiver", "Add a receiver")
        check(b.js("$('microw').hidden"), "no microphone gain on a receiver")
        st = b.until(f"(()=>{{const t={ROW.format(i=0)}.querySelector('.rxst').textContent;return /playing/.test(t)&&t}})()",
                     10, "row 0's state")
        check(st and "in use · playing" in st and "MOCK One" in st and "OpenWebRX+ v1.2.126" in st,
              f"the one in use: playing, what it calls itself: {st!r}")

        # A receiver added: its address pasted whole, Test, named by itself.
        b.js("$('radd').click()")
        row_set(b, 1, "host", f"127.0.0.1:{m2.port}/OWRX/index.html#freq=7100000,mod=usb")
        check(row_val(b, 1, "host") == f"http://127.0.0.1:{m2.port}/OWRX/",
              f"a link pasted whole, cut to what the knob keeps: {row_val(b, 1, 'host')!r}")
        r = row_test(b, 1)
        check(r and r.startswith("✓") and "MOCK Two | Somewhere far" in r and "OpenWebRX v1.3.0-dev" in r
              and "2 SDRs" in r and "7 bands" in r and "20 listeners at most" in r, f"Test: what it is: {r!r}")
        check(row_val(b, 1, "name") == "MOCK Two", f"named as it names itself, for the dial: {row_val(b, 1, 'name')!r}")
        s2 = m2.stats()
        check(s2["status"] == 1 and s2["sessions"] == 0 and s2["upgrades"] == 0,
              f"Test read its status.json once, and opened no session: {s2['status']} {s2['sessions']}")
        b.js("$('radd').click()")
        row_set(b, 2, "host", "http://127.0.0.1:9/")
        r = row_test(b, 2)
        check(r == "✗ no answer there: not reached", f"Test of a dead address: {r!r}")
        row_set(b, 2, "host", m3.url)
        r = row_test(b, 2)
        check(r == "✗ something answers there, but not an OpenWebRX", f"Test of one that is no OpenWebRX: {r!r}")
        row_set(b, 2, "host", "ftp://example/")
        r = row_test(b, 2)
        check(r == "Not an address the knob can use", f"an address it cannot use, said before asking: {r!r}")
        b.js(f"{ROW.format(i=2)}.querySelector('[data-act=rm]').click()")
        check(b.js("RIGS.length") == 2, "the third removed")

        # Saved: the list kept, the knob still on the one it plays.
        s1 = m1.stats()["sessions"]
        msg = save(b)
        check(msg and msg.startswith("Saved"), f"saved: {msg!r}")
        rl = k.radios()
        check([x["host"] for x in rl["list"]] == [m1.url, f"http://127.0.0.1:{m2.port}/OWRX/"] and
              [x["name"] for x in rl["list"]] == ["One", "MOCK Two"] and rl["sel"] == 0,
              f"the list on the knob: {[x['host'] for x in rl['list']]} {[x['name'] for x in rl['list']]} {rl['sel']}")
        time.sleep(2)
        check(m1.stats()["sessions"] == s1 and k.radio()["ready"] and m2.stats()["sessions"] == 0,
              "a list saved alone moves the knob off nothing")

        # Another In use, and Save: taken over at once.
        check(b.until("RIGS&&RIGS.length===2&&!rigsDirty", 10, "the list read again"), "the page reads the list again")
        b.js(f"{ROW.format(i=1)}.querySelector('input[name=ruse]').click()")
        msg = save(b)
        check(msg and msg.startswith("Saved"), f"saved with the other In use: {msg!r}")
        check(k.until(lambda: k.radio()["ready"] and k.radio()["owrx"]["name"].startswith("MOCK Two"), 20,
                      "the second to play"), "the second plays, with no restart")
        check(k.radios()["sel"] == 1 and m2.stats()["sessions"] == 1, "in use on the knob, one session on it")
        st = b.until(f"(()=>{{const t={ROW.format(i=1)}.querySelector('.rxst').textContent;return /playing/.test(t)&&t}})()",
                     10, "row 1's state")
        check(st and st.startswith("in use · playing"), f"its row says so: {st!r}")
        check(not b.js(f"/in use/.test({ROW.format(i=0)}.querySelector('.rxst').textContent)"),
              "the first's row no longer in use")

        # One that bans the knob, chosen: held, and the next plays in its place.
        b.js("$('radd').click()")
        row_set(b, 2, "host", m4.url)
        b.js(f"{ROW.format(i=2)}.querySelector('input[name=ruse]').click()")
        msg = save(b)
        check(msg and msg.startswith("Saved"), f"saved with the banned one In use: {msg!r}")
        check(k.until(lambda: m4.stats()["banned_refused"] >= 1, 20, "the ban"), "the receiver bans the knob")
        st = b.until(f"(()=>{{const t={ROW.format(i=2)}.querySelector('.rxst').textContent;return /held/.test(t)&&t}})()",
                     15, "row 2's state")
        check(st and "held: it bans this address \u2014 not again until you choose it" in st,
              f"its row says why, and until when: {st!r}")
        check(k.until(lambda: k.radio()["ready"], 20, "a stand-in"), "another plays in its place")
        st = b.until(f"(()=>{{const t={ROW.format(i=2)}.querySelector('.rxst').textContent;"
                     f"return t.startsWith('held')&&t}})()", 10, "row 2 no longer in use")
        check(st and st.startswith("held: it bans this address"), f"...and is no longer in use: {st!r}")
        rl = k.radios()
        check(rl["sel"] in (0, 1) and rl["list"][rl["sel"]]["rx"]["playing"] and rl["list"][2]["rx"]["held"],
              f"the one that plays is In use at once, before it is in flash: {rl['sel']}")
        check(b.until(f"{ROW.format(i=2)}.querySelector('input[name=ruse]').checked===false&&"
                      f"[...document.querySelectorAll('#rlist input[name=ruse]')].findIndex(e=>e.checked)+1", 10,
                      "In use on the page") == rl["sel"] + 1, "...and on the page")
        # A list saved now keeps the one playing: never the banned one chosen again.
        row_set(b, rl["sel"], "name", "Stand-in")
        msg = save(b)
        time.sleep(2)
        check(msg and msg.startswith("Saved") and k.radios()["sel"] == rl["sel"] and m4.stats()["banned_refused"] == 1,
              f"a list saved meanwhile keeps the one playing: {k.radios()['sel']}")
        check(m4.stats()["banned_refused"] == 1, "the banned one asked once only")
    finally:
        b.stop()
        k.stop()
        for m in (m1, m2, m3, m4):
            m.stop()


def case_radio():
    m1 = Mock("--name", "MOCK One", "--others", "2")
    m2 = Mock("--fork", "upstream", "--name", "MOCK Two")
    k = Knob(f"One={m1.url}", f"Two={m2.url}")
    b = Browser(k)
    try:
        check(k.until(lambda: k.radio()["ready"], 20, "the knob to play"), "the knob plays")
        check(b.go("/", "typeof R!=='undefined'&&R&&R.owrx&&R.ready"), "the page opens on the radio's controls")
        check(b.text("#model") == "OpenWebRX+" and b.text("#link") == "connected", "OpenWebRX+, connected")
        info = b.until("$('kiwiinfo').textContent.includes('listening')&&$('kiwiinfo').textContent", 10, "the info line")
        check(info and info.startswith("MOCK One · OpenWebRX+ v1.2.126 · 3 of 20 listening"),
              f"what it is, and everyone listening: {info!r}")
        check(b.js("[...$('scale').children].map(e=>e.textContent).join(' ')") == "-108 -86 -65 -43 -22 0",
              "the meter's scale: its waterfall's, 20 dB beyond each end")
        sv = b.until("/^-?\\d+\\.\\d dB$/.test($('sval').textContent)&&$('sval').textContent", 10, "a reading")
        check(sv and float(sv.split()[0]) > -150, f"the reading in its dB, once there is one: {sv!r}")
        check(b.js("getComputedStyle($('mbar')).backgroundColor") == "rgb(34, 255, 47)",
              "the meter green below 70 %, as its page has it")
        check(b.js("$('agcbox').hidden&&$('gainbox').hidden&&$('ritsec').hidden&&$('microw').hidden"),
              "no AGC, gain, RIT or microphone")
        check(b.js("!$('apiband').hidden&&!$('apirecv').hidden&&$('apiradios').hidden"), "the API: band= and receiver=")

        names = b.js("[...$('bands').children].map(e=>e.textContent)")
        check(names == ["40m", "20m", "80m", "49m Broadcast", "2m FM Brügge", "FM broadcast", "Airband"],
              f"its bands, its own names in its own order: {names}")
        check(b.js("$('bands').children[0].classList.contains('on')"), "the one it is on marked")
        check(b.js("$('bands').children[1].title") == "RSPdx · 13.925-14.425 MHz", "each with its SDR and span")
        check(b.text("#bandnote").startswith("40m · RSPdx · 6.850-7.350 MHz"), f"the band: {b.text('#bandnote')!r}")
        check(b.until("![...$('bands').children].some(e=>e.disabled)", 15, "the bands free") is not None,
              "another band once 11 s have passed")
        check("another band moves 2 other listeners with it" in b.text("#bandnote"),
              f"...which moves the others: {b.text('#bandnote')!r}")
        sel0 = m1.stats()["selects"]
        b.js("$('bands').children[1].click()")
        check(b.until("R.owrx.band_sel===1", 10, "20m"), "20m asked for, and on it")
        check(m1.stats()["selects"] == sel0 + 1, "one band change sent")
        check(b.js("[...$('bands').children].filter(e=>e.disabled).length") == 6 and
              "another band in" in b.text("#bandnote"), "the others greyed for now, and the page says how long")
        b.js("$('bands').children[2].disabled=false;$('bands').children[2].click()")
        time.sleep(1)
        check(m1.stats()["selects"] == sel0 + 1 and b.js("R.owrx.band_sel") == 1,
              "one asked for too soon refused by the knob, and nothing sent")
        check(b.until("$('toast').textContent.includes('another band in')", 5, "the toast"), "...and said")
        check(b.until("R.freq>=13925000&&R.freq<=14425000", 10, "the dial on 20m"), "the dial on the new band")

        # A frequency: outside the band, refused on the page; inside, sent.
        f0 = b.js("R.freq")
        b.js("$('fin').value='7.1';$('fform').requestSubmit()")
        check(b.until("$('toast').textContent.startsWith('Outside the band')", 5, "the toast")
              and "13.925-14.425 MHz" in b.text("#toast"), f"outside the band: {b.text('#toast')!r}")
        time.sleep(1)
        check(b.js("R.freq") == f0, "...and nothing tuned")
        b.js("$('fin').value='14.2';$('fform').requestSubmit()")
        check(b.until("R.freq===14200000", 5, "14.200"), "inside it, tuned")
        check(k.until(lambda: m1.last_dsp().get("offset_freq") == 14200000 - 14175000, 5, "the offset"),
              f"...and sent as the receiver's offset: {m1.last_dsp().get('offset_freq')}")

        # Mode, passband, squelch.
        check(b.js("!$('modes').querySelector('[data-v=sam]').hidden"), "SAM on OpenWebRX+")
        b.js("$('modes').querySelector('[data-v=am]').click()")
        check(b.until("R.mode==='am'", 5, "AM"), "AM")
        check(k.until(lambda: m1.last_dsp().get("mod") == "am", 5, "am sent"), "...sent")
        ws = b.js("[...$('filters').children].map(e=>e.textContent)")
        check(ws == ["5.0k", "6.0k", "8.0k", "10.0k", "12.0k"], f"AM's passbands, as the knob's: {ws}")
        b.js("[...$('filters').children].find(e=>e.textContent==='8.0k').click()")
        check(b.until("R.lo===-4000&&R.hi===4000", 5, "8 kHz"), "8 kHz around the dial")
        d = m1.last_dsp()
        check(k.until(lambda: (m1.last_dsp().get("low_cut"), m1.last_dsp().get("high_cut")) == (-4000, 4000), 5,
                      "the passband sent"), f"...sent: {d.get('low_cut')} {d.get('high_cut')}")
        b.js("$('modes').querySelector('[data-v=usb]').click()")
        check(b.until("R.mode==='usb'", 5, "USB"), "USB")
        b.js("[...$('filters').children].find(e=>e.textContent==='2.4k').click()")
        check(b.until("R.lo===150&&R.hi===2550", 5, "2.4 kHz"), "2.4 kHz from 150 Hz off the carrier (OpenWebRX+)")
        check(b.js("!$('sqbox').hidden&&$('sqv').textContent") == "open", "the squelch, open")
        b.js("$('sq').value=50;$('sq').dispatchEvent(new Event('input'));$('sq').dispatchEvent(new Event('change'))")
        check(b.text("#sqv") == "-54 dB", f"...in its dB as it moves: {b.text('#sqv')!r}")
        check(b.until("R.squelch===50", 5, "the squelch"), "the squelch at half the scale")
        time.sleep(2)
        check(b.text("#sqv") == "-54 dB", f"...and as the knob has it: {b.text('#sqv')!r}")
        check(k.until(lambda: m1.last_dsp().get("squelch_level") == -54, 5, "the squelch sent"),
              f"...sent as such: {m1.last_dsp().get('squelch_level')}")

        # The other receiver, at once.
        check(b.js("[...$('radios').children].map(e=>e.textContent).join('|')") == "One|Two" and
              b.text("#radiolbl") == "Receiver", "the receivers, by their names on the dial")
        b.js("$('radios').children[1].click()")
        check(b.until("R.ready&&R.owrx.name==='MOCK Two'", 20, "the second"), "the other receiver, taken over at once")
        check(b.until("$('radios').children[1].classList.contains('on')", 5, "marked"), "...marked")
        check(b.until("$('modes').querySelector('[data-v=sam]').hidden", 5, "no SAM"), "no SAM on OpenWebRX")
        check(b.until("$('model').textContent==='OpenWebRX'&&$('kiwiinfo').textContent.startsWith('MOCK Two · OpenWebRX v1.3.0-dev')",
                      10, "its info"), f"...its own name and software: {b.text('#kiwiinfo')!r}")
        check(m1.stats()["sessions"] == 1 and m2.stats()["sessions"] == 1, "one session each, no more")
    finally:
        b.stop()
        k.stop()
        m1.stop()
        m2.stop()


def case_tls():
    if ARGS.no_tls:
        print("  (no TLS in this build: skipped)")
        return
    d = tempfile.mkdtemp(prefix="owrx-web-tls-")
    certs = make_certs(d)
    if not certs:
        print("  (no openssl: skipped)")
        return
    m0 = Mock("--name", "MOCK Zero")
    m = Mock("--tls", f"{certs[1]}:{certs[2]}", "--prefix", "/OWRX/", "--redirect", "301", "--name", "MOCK Secure")
    k = Knob(f"Zero={m0.url}", env={"KIWI_TEST_CA": certs[0]})
    b = Browser(k)
    try:
        check(b.go("/config", "typeof RIGS!=='undefined'&&RIGS&&RIGS.length===1"), "the configuration page")
        b.js("$('radd').click()")
        row_set(b, 1, "host", f"localhost:{m.redirect}/OWRX/")
        r = row_test(b, 1)
        want = f"https://localhost:{m.port}/OWRX/"
        check(r and r.startswith("✓ MOCK Secure") and f"on {want} from now on" in r,
              f"Test followed it to https://, and says so: {r!r}")
        check(row_val(b, 1, "host") == want, f"...the box says where: {row_val(b, 1, 'host')!r}")
        st = m.stats()
        check(st["redirects"] == 1 and st["status"] == 1 and st["sessions"] == 0, "one redirect, one status.json")
        msg = save(b)
        check(msg and msg.startswith("Saved") and k.radios()["list"][1]["host"] == want, "Save keeps it")
    finally:
        b.stop()
        k.stop()
        m.stop()
        m0.stop()
        shutil.rmtree(d, ignore_errors=True)


CASES = ["config", "radio", "tls"]


def main():
    global ARGS
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", required=True)
    ap.add_argument("--mock", required=True)
    ap.add_argument("--chromium", default=shutil.which("chromium") or shutil.which("chromium-browser")
                    or shutil.which("google-chrome") or "")
    ap.add_argument("--only", default="")
    ap.add_argument("-v", "--verbose", action="store_true")
    ap.add_argument("--no-tls", action="store_true")
    ARGS = ap.parse_args()
    if not ARGS.chromium:
        print("no Chromium to drive the pages: skipped")
        return 77
    only = [c for c in ARGS.only.split(",") if c] or CASES
    for c in only:
        print(f"{c}:", flush=True)
        globals()["case_" + c]()
    print(f"{len(FAILS)} failed" if FAILS else "all passed")
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
