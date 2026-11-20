#!/usr/bin/env python3
"""A stand-in TailChaser node, for the tailchaser firmware's tests and for a
knob on the bench: the API of TailChaser's docs/API.md, nothing on the air.

  GET  /api/v1/status   a node's status, the shape src/status.c writes
  GET  /api/v1/block    {"rx":..,"tx":..}
  POST /api/v1/block    rx=1 / tx=0 (a form, JSON or the query); a control
                        key only, 403 for a read key

  GET  /monitor/tx      a websocket: what goes to the transmitter, 16-bit LE
                        mono PCM at 16 kHz, 100 ms a message while keyed (a
                        1 kHz tone at -12 dBFS here), a ping every 5 s
                        otherwise; no key. 404 unless "monitor" is on, 503
                        with four listeners there -- src/monitor.c's ways

The key in X-API-Key or "Authorization: Bearer"; 401 without one, or a wrong
one. One request at a time, Connection: close, Content-Length always, as the
real node answers (a websocket goes on on its own thread).

On its own it makes a control key and a read key, prints them, and plays a
round of what a repeater does -- a talker on the talkgroup, a voice on the
input, a carrier with none, the transmitter keyed -- so a knob's face can be
watched:

  python3 -I tools/mock_tailchaser.py --bind 0.0.0.0 --port 8080 --play --monitor

The tests import it (Node) and steer it themselves.
"""
import argparse
import base64
import copy
import hashlib
import json
import math
import secrets
import select
import socket
import struct
import sys
import threading
import time
import urllib.parse
from http.server import BaseHTTPRequestHandler, HTTPServer

WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
CHUNK = 1600                    # samples a message: 100 ms at 16 kHz
LISTENERS = 4
TONE = struct.pack("<%dh" % CHUNK, *(int(8192 * math.sin(2 * math.pi * 1000 * i / 16000)) for i in range(CHUNK)))


class _Server(HTTPServer):
    """One request at a time, as the node -- but a websocket, once answered,
    is its listener thread's: not shut or closed after the request."""

    def __init__(self, *a, **k):
        self.handed = set()
        super().__init__(*a, **k)

    def shutdown_request(self, request):
        if request in self.handed:
            return
        super().shutdown_request(request)


def base_status(role="transmitter", callsign="ON0TST"):
    """A node's status as TailChaser writes it, the counters shortened."""
    now = int(time.time())
    tx_site = role == "transmitter"
    rx = role in ("simplex", "duplex", "receiver")
    tx = role in ("simplex", "duplex", "transmitter")
    st = {
        "version": "mock", "board": "a PC", "uptime_s": 3600, "now": now,
        "block": {"rx": False, "tx": False},
        "role": role, "title": "repeater " + role,
        "has": {"rx": rx, "tx": tx, "reflector": tx, "uplink": role == "receiver",
                "rxlink": tx_site, "repeater": role in ("duplex", "transmitter")},
        "uplink": {"connected": False, "peer": "", "codec": "", "mute": 0, "connects": 0},
        "receiver": {"connected": tx_site, "peer": "192.0.2.7:5210" if tx_site else "", "codec": "S16",
                     "proto": "2.8", "squelch": False, "siglev": 0.0, "connects": 1},
        "radio": {"freq_mhz": 438.8, "freq_tx_mhz": 438.8, "bandwidth": "narrow", "ctcss_rx": "none",
                  "ctcss_tx": "131.8", "squelch": 1, "firmware": "mock", "type": "sa818"},
        "link": {"connected": True, "audio": True, "waiting_for_cert": False, "host": "reflector.example.org",
                 "port": 5300, "proto": 3, "callsign": callsign, "tg": 8, "home_tg": 8, "talker": "",
                 "connects": 1},
        "tx": {"keyed": False, "why": "", "keyed_ms": 0, "timeout_s": 600, "count": 0},
        "ident": {"callsign": callsign, "interval_min": 15},
        "gate": {"mode": "on", "hold_s": 10, "cos": False, "open": False, "state": "idle", "level_db": -120.0,
                 "rssi": -1, "rssi_lo": 79, "rssi_hi": 100},
        "history": [], "net_history": [], "monitor": False,
    }
    if tx_site:
        rr = base_status("receiver", callsign)
        rr["radio"]["freq_mhz"] = rr["radio"]["freq_tx_mhz"] = 431.2
        rr["radio"]["ctcss_rx"] = "131.8"
        rr["gate"].update(rssi_lo=30, rssi_hi=130)
        st["remote_receiver"] = rr
    return st


class Node:
    """One node. Steer it through .set(), .gate(), .talker(), .keyed();
    .mode makes it misbehave: "ok", "drop" (closes without a word), "slow"
    (answers after .slow_s), "cut" (half its body), "garbage" (no HTTP),
    "500"."""

    def __init__(self, port=0, bind="127.0.0.1", role="transmitter", callsign="ON0TST",
                 control_key=None, read_key=None):
        self.lock = threading.Lock()
        self.st = base_status(role, callsign)
        self.keys = {}
        if control_key:
            self.keys[control_key] = "control"
        if read_key:
            self.keys[read_key] = "read"
        self.requests = []          # (method, path, who, body): who is "control", "read", "none", "wrong"
        self.mode = "ok"
        self.slow_s = 3.0
        self.keyed_at = time.time()     # its keyed_ms counts from here
        self.ws_connects = 0        # websockets answered
        self.ws_live = set()        # their sockets, still open
        self.ws_refused = []        # 404 / 503 / 400 given
        self.ws_pongs = 0
        self.ws_closes = 0          # close frames from a listener
        node = self

        class H(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.0"

            def log_message(self, *a):
                pass

            def _who(self):
                k = self.headers.get("X-API-Key", "")
                a = self.headers.get("Authorization", "")
                if not k and a.lower().startswith("bearer "):
                    k = a[7:].strip()
                if not k:
                    return "none"
                return node.keys.get(k, "wrong")

            def _send(self, code, obj):
                body = (json.dumps(obj, separators=(",", ":")) + "\n").encode()
                mode = node.mode
                if mode == "drop":
                    self.close_connection = True
                    return
                if mode == "slow":
                    time.sleep(node.slow_s)
                if mode == "garbage":
                    self.wfile.write(b"SSH-2.0-OpenSSH_9.6\r\n")
                    return
                if mode == "500":
                    code, body = 500, b'{"ok":false,"error":"internal"}\n'
                self.send_response(code)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.send_header("Connection", "close")
                self.end_headers()
                self.wfile.write(body[: len(body) // 2] if mode == "cut" else body)

            def _plain(self, code, text):
                body = text.encode()
                self.send_response(code)
                self.send_header("Content-Type", "text/plain")
                self.send_header("Content-Length", str(len(body)))
                self.send_header("Connection", "close")
                self.end_headers()
                self.wfile.write(body)

            def _monitor(self, which):
                key = self.headers.get("Sec-WebSocket-Key", "")
                with node.lock:
                    on = node.st.get("monitor") and which == "tx"
                    full = len(node.ws_live) >= LISTENERS
                if not on:
                    node.ws_refused.append(404)
                    return self._plain(404, "listening in is off ([status] monitor)\n")
                if not key:
                    node.ws_refused.append(400)
                    return self._plain(400, "a websocket\n")
                if full:
                    node.ws_refused.append(503)
                    return self._plain(503, "too many listeners\n")
                acc = base64.b64encode(hashlib.sha1((key + WS_GUID).encode()).digest()).decode()
                self.wfile.write(("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                                  "Connection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n" % acc).encode())
                self.wfile.flush()
                sock = self.connection
                self.server.handed.add(sock)
                self.close_connection = True
                with node.lock:
                    node.ws_connects += 1
                    node.ws_live.add(sock)
                threading.Thread(target=node._listener, args=(sock,), daemon=True).start()

            def _handle(self, method):
                u = urllib.parse.urlsplit(self.path)
                if method == "GET" and u.path.startswith("/monitor/"):
                    return self._monitor(u.path[9:])
                n = int(self.headers.get("Content-Length") or 0)
                body = self.rfile.read(n).decode("utf-8", "replace") if n else ""
                who = self._who()
                with node.lock:
                    node.requests.append((method, u.path, who, body))
                if u.path not in ("/api/v1/status", "/api/v1/block"):
                    return self._send(404, {"ok": False, "error": "not found"})
                if who in ("none", "wrong"):
                    return self._send(401, {"ok": False, "error": "unauthorized"})
                if u.path == "/api/v1/status":
                    if method != "GET":
                        return self._send(404, {"ok": False, "error": "not found"})
                    with node.lock:
                        node.st["now"] = int(time.time())
                        if node.st["tx"]["keyed"]:          # counts up, as the node's does
                            node.st["tx"]["keyed_ms"] = int((time.time() - node.keyed_at) * 1000)
                        st = copy.deepcopy(node.st)
                    return self._send(200, st)
                if method == "GET":
                    with node.lock:
                        return self._send(200, dict(node.st["block"]))
                if who != "control":
                    return self._send(403, {"ok": False, "error": "this key may only read"})
                want = {}
                ctype = self.headers.get("Content-Type", "")
                if body and "json" in ctype:
                    try:
                        want = {k: bool(v) for k, v in json.loads(body).items() if k in ("rx", "tx")}
                    except Exception:
                        return self._send(400, {"ok": False, "error": "bad JSON"})
                else:
                    for k, v in urllib.parse.parse_qsl(body or u.query):
                        if k in ("rx", "tx"):
                            want[k] = v in ("1", "true", "yes", "on")
                with node.lock:
                    node.st["block"].update(want)
                    return self._send(200, dict(node.st["block"]))

            def do_GET(self):
                self._handle("GET")

            def do_POST(self):
                self._handle("POST")

        self.httpd = _Server((bind, port), H)           # one request at a time, as the node
        self.port = self.httpd.server_address[1]
        self.thread = threading.Thread(target=self.httpd.serve_forever, daemon=True)
        self.thread.start()

    # ---- the websocket's listener: what the transmitter sends, as src/monitor.c
    def _listener(self, sock):
        def out(op, payload):
            n = len(payload)
            head = struct.pack("!BB", 0x80 | op, n) if n < 126 else struct.pack("!BBH", 0x80 | op, 126, n)
            sock.sendall(head + payload)

        was = False
        idle = 0
        try:
            while True:
                with self.lock:
                    keyed = self.st["tx"]["keyed"]
                    alive = sock in self.ws_live
                if not alive:
                    break
                if keyed:
                    out(2, TONE)
                    idle = 0
                elif was:
                    out(2, TONE[: CHUNK])           # the tail of an over: half a message
                    idle = 0
                else:
                    idle += 1
                    if idle % 50 == 0:
                        out(9, b"")
                was = keyed
                r, _, _ = select.select([sock], [], [], 0.1)
                if r:
                    data = sock.recv(4096)
                    if not data:
                        break
                    # a listener's frames: masked; a pong or a close
                    op = data[0] & 15
                    if op == 10:
                        with self.lock:
                            self.ws_pongs += 1
                    if op == 8:
                        with self.lock:
                            self.ws_closes += 1
                        break
        except OSError:
            pass
        with self.lock:
            self.ws_live.discard(sock)
        self.httpd.handed.discard(sock)
        try:
            sock.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        sock.close()

    def listeners(self):
        with self.lock:
            return len(self.ws_live)

    def cut_listeners(self):
        """The node restarted: every websocket gone."""
        with self.lock:
            socks = list(self.ws_live)
            self.ws_live.clear()
        for s in socks:
            try:
                s.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass

    # ---- steering
    def set(self, path, value):
        """set("link.tg", 206); a dotted path into the status."""
        with self.lock:
            o = self.st
            parts = path.split(".")
            for p in parts[:-1]:
                o = o[p]
            o[parts[-1]] = value

    def get(self, path):
        with self.lock:
            o = self.st
            for p in path.split("."):
                o = o[p]
            return copy.deepcopy(o)

    def gate(self, cos=False, open_=False, rssi=-1, level_db=-120.0):
        """The receiver's squelch: the receive site's at a transmit site."""
        with self.lock:
            g = (self.st.get("remote_receiver") or self.st)["gate"]
            g.update(cos=cos, open=open_, rssi=rssi, level_db=level_db,
                     state="open" if open_ else "listening" if cos else "idle")
            if "remote_receiver" in self.st:
                self.st["receiver"]["squelch"] = cos

    def talker(self, call, tg=None):
        with self.lock:
            if tg is not None:
                self.st["link"]["tg"] = tg
            was = self.st["link"]["talker"]
            self.st["link"]["talker"] = call
            if was and not call:                        # stamped at its end, as the node's
                self.st["net_history"].insert(0, {"t": int(time.time()), "ms": 5000, "call": was,
                                                  "tg": self.st["link"]["tg"], "outcome": "sent"})

    def net_over(self, call, ago_s, ms=1000):
        """An over from the talkgroup that ended ago_s seconds back, only in
        the net history -- one too short for any read to catch."""
        with self.lock:
            self.st["net_history"].insert(0, {"t": int(time.time()) - ago_s, "ms": ms, "call": call,
                                              "tg": self.st["link"]["tg"], "outcome": "transmitted"})

    def keyed(self, on, why="talkgroup audio"):
        with self.lock:
            if on and not self.st["tx"]["keyed"]:
                self.keyed_at = time.time() - 1.2
            self.st["tx"].update(keyed=on, why=why if on else "", keyed_ms=1200 if on else 0)

    def receive_site(self, up):
        """A transmit site's receive site answering, or not."""
        with self.lock:
            self.st["receiver"]["connected"] = up
            if up:
                if self.st.get("remote_receiver") is None:
                    self.st["remote_receiver"] = getattr(self, "_rr", None) or \
                        base_status("transmitter")["remote_receiver"]
            else:
                if self.st.get("remote_receiver") is not None:
                    self._rr = self.st["remote_receiver"]
                self.st["remote_receiver"] = None

    def taken(self, method=None, path=None):
        with self.lock:
            return [r for r in self.requests if (method is None or r[0] == method) and (path is None or r[1] == path)]

    def stop(self):
        self.cut_listeners()
        self.httpd.shutdown()
        self.httpd.server_close()


def play(node):
    """A round of what a repeater does, for a knob's face to be watched."""
    steps = [
        ("quiet", lambda: (node.gate(), node.talker(""), node.keyed(False))),
        ("ON4ABC on TG 206", lambda: (node.talker("ON4ABC", 206), node.keyed(True))),
        ("quiet again", lambda: (node.talker(""), node.keyed(False), node.set("link.tg", 8))),
        ("a voice on the input, S9+10", lambda: (node.gate(True, True, 103, -18.0), node.keyed(True, "repeater"))),
        ("a carrier and no voice, S6", lambda: (node.gate(True, False, 72, -45.0), node.keyed(False))),
        ("quiet", lambda: node.gate()),
    ]
    while True:
        for what, do in steps:
            do()
            print("now:", what, flush=True)
            time.sleep(6)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--bind", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--role", default="transmitter", choices=["simplex", "duplex", "receiver", "transmitter"])
    ap.add_argument("--callsign", default="ON0TST")
    ap.add_argument("--play", action="store_true", help="a round of talkers, voices and carriers")
    ap.add_argument("--monitor", action="store_true", help="listening on: /monitor/tx answers")
    a = ap.parse_args()
    control, read = "tc_" + secrets.token_hex(32), "tc_" + secrets.token_hex(32)
    node = Node(a.port, a.bind, a.role, a.callsign, control, read)
    node.set("monitor", a.monitor)
    print(f"a stand-in TailChaser {a.role} on {a.bind}:{node.port}")
    print(f"  control key: {control}")
    print(f"  read key:    {read}")
    sys.stdout.flush()
    try:
        if a.play:
            play(node)
        else:
            while True:
                time.sleep(3600)
    except KeyboardInterrupt:
        pass
    node.stop()


if __name__ == "__main__":
    main()
