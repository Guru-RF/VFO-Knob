#!/usr/bin/env python3
"""A stand-in for AISStream.io's stream, for the AIS Radar firmware's tests.

    python3 -I tools/mock_aisstream.py [--port 8765] [--key test-key]
        [--no-deflate] [--keep-context] [--fragment] [--ping S] [--ships N]

A WebSocket server in the clear (ws://127.0.0.1:PORT/v0/stream) that does
what the real one does, as its documentation says it: the subscription
expected within 3 s of the upgrade, {"APIKey", "BoundingBoxes", ...}; a key
not the one given answered {"error": ...} and the connection closed; else a
SubscriptionConfirmation, then the made-up ships round the box's middle,
their positions every second and their static data now and then, each a
binary frame of JSON -- compressed (permessage-deflate) when the client
offers it, each message alone unless --keep-context. A subscription sent
again replaces the box. --fragment splits messages into frames;
--ping sends a ping every S seconds. Only the standard library.
"""
import argparse
import base64
import hashlib
import json
import math
import random
import socket
import struct
import sys
import threading
import time
import zlib

sys.dont_write_bytecode = True

GUID = b"258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
NAMES = ["NORDIC STAR", "SEA SPRITE", "NOORDZEE", "WESTERLING", "MORGENSTER", "CAPE ORANGE",
         "BALTIC WAVE", "PRINS FILIP", "ZEEAREND", "LADY GRACE", "OOSTERSCHELDE", "MARE NOSTRUM"]
TYPES = [70, 80, 60, 30, 52, 37, 36, 71, 81, 50, 31, 40]


def recv_exact(c, n):
    b = b""
    while len(b) < n:
        k = c.recv(n - len(b))
        if not k:
            raise ConnectionError("closed")
        b += k
    return b


def read_frame(c):
    h = recv_exact(c, 2)
    op, n = h[0] & 0x0F, h[1] & 0x7F
    if n == 126:
        n = struct.unpack(">H", recv_exact(c, 2))[0]
    elif n == 127:
        n = struct.unpack(">Q", recv_exact(c, 8))[0]
    if not h[1] & 0x80:
        raise ValueError("a client frame not masked")
    m = recv_exact(c, 4)
    p = bytes(b ^ m[i & 3] for i, b in enumerate(recv_exact(c, n)))
    return op, bool(h[0] & 0x80), bool(h[0] & 0x40), p


class Conn:
    def __init__(self, c, args):
        self.c, self.a = c, args
        self.lock = threading.Lock()
        self.box = None
        self.deflate = False
        self.z = None

    def send(self, op, payload, rsv1=False):
        frames = [payload]
        if self.a.fragment and op in (1, 2) and len(payload) > 20:
            cut = len(payload) // 3
            frames = [payload[:cut], payload[cut:2 * cut], payload[2 * cut:]]
        with self.lock:
            for i, p in enumerate(frames):
                b0 = (0x80 if i == len(frames) - 1 else 0) | (op if i == 0 else 0) | (0x40 if rsv1 and i == 0 else 0)
                n = len(p)
                h = bytes([b0, n]) if n < 126 else bytes([b0, 126]) + struct.pack(">H", n) if n < 65536 \
                    else bytes([b0, 127]) + struct.pack(">Q", n)
                self.c.sendall(h + p)

    def message(self, obj):
        data = json.dumps(obj, separators=(",", ":")).encode()
        if self.deflate:
            if not self.a.keep_context or self.z is None:
                self.z = zlib.compressobj(9, zlib.DEFLATED, -15)
            z = self.z.compress(data) + self.z.flush(zlib.Z_SYNC_FLUSH)
            assert z.endswith(b"\x00\x00\xff\xff")
            self.send(2, z[:-4], rsv1=True)
        else:
            self.send(2, data)


def handshake(c, args):
    req = b""
    while b"\r\n\r\n" not in req:
        k = c.recv(4096)
        if not k:
            raise ConnectionError("closed")
        req += k
    lines = req.decode("latin-1").split("\r\n")
    hdr = {l.split(":", 1)[0].strip().lower(): l.split(":", 1)[1].strip() for l in lines[1:] if ":" in l}
    if not lines[0].startswith("GET /v0/stream"):
        c.sendall(b"HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n")
        raise ConnectionError("not the stream")
    acc = base64.b64encode(hashlib.sha1(hdr["sec-websocket-key"].encode() + GUID).digest()).decode()
    ext = hdr.get("sec-websocket-extensions", "")
    deflate = "permessage-deflate" in ext and not args.no_deflate
    resp = ("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
            f"Sec-WebSocket-Accept: {acc}\r\n")
    if deflate:
        resp += "Sec-WebSocket-Extensions: permessage-deflate" + \
                ("" if args.keep_context else "; server_no_context_takeover") + "; client_no_context_takeover\r\n"
    c.sendall((resp + "\r\n").encode())
    print(f"mock: upgraded, {'deflate' if deflate else 'uncompressed'}"
          f"{' (context kept)' if deflate and args.keep_context else ''}", flush=True)
    return deflate


def ship_msgs(i, lat, lon, t):
    mmsi = 205100000 + i * 1111 if i < 6 else 244600000 + i * 777
    name = NAMES[i % len(NAMES)]
    ang = (i * 47 + t * (1 + i % 3)) % 360
    dist = 0.04 + 0.02 * i
    la = lat + dist * math.cos(math.radians(ang))
    lo = lon + dist * math.sin(math.radians(ang)) / math.cos(math.radians(lat))
    meta = {"MMSI": mmsi, "MMSI_String": mmsi, "ShipName": name + " " * (20 - len(name)),
            "latitude": la, "longitude": lo, "time_utc": "2026-11-21 10:00:00.000000000 +0000 UTC"}
    moored = i % 4 == 3
    sog = 0.0 if moored else 4.0 + i
    if i < 6:
        pos = {"MessageType": "PositionReport", "MetaData": meta, "Message": {"PositionReport": {
            "Cog": (ang + 90) % 360, "Latitude": la, "Longitude": lo, "MessageID": 1,
            "NavigationalStatus": 5 if moored else 0, "Sog": sog, "TrueHeading": int((ang + 92) % 360),
            "UserID": mmsi, "Valid": True}}}
    else:
        pos = {"MessageType": "StandardClassBPositionReport", "MetaData": meta, "Message": {
            "StandardClassBPositionReport": {"Cog": (ang + 90) % 360, "Latitude": la, "Longitude": lo,
                                             "Sog": sog, "TrueHeading": 511, "UserID": mmsi, "Valid": True}}}
    static = {"MessageType": "ShipStaticData", "MetaData": meta, "Message": {"ShipStaticData": {
        "CallSign": f"ON{i:03d}", "Destination": "OOSTENDE", "Dimension": {"A": 60 + i * 5, "B": 20, "C": 6, "D": 6},
        "Eta": {"Month": 11, "Day": 22, "Hour": 6, "Minute": 30}, "ImoNumber": 9100000 + i,
        "MaximumStaticDraught": 4.5, "MessageID": 5, "Name": name + "@" * (20 - len(name)),
        "Type": TYPES[i % len(TYPES)], "UserID": mmsi, "Valid": True}}}
    return pos, static


def serve(c, addr, args):
    c.settimeout(None)
    try:
        conn = Conn(c, args)
        conn.deflate = handshake(c, args)
        c.settimeout(3)
        op, fin, rsv1, p = read_frame(c)
        sub = json.loads(p)
        if sub.get("APIKey") != args.key:
            conn.message({"error": "Api Key Is Not Valid"})
            conn.send(8, struct.pack(">H", 1008))
            print("mock: a wrong key, closed", flush=True)
            return
        conn.box = sub["BoundingBoxes"][0]
        print(f"mock: subscribed, box {conn.box}, types {len(sub.get('FilterMessageTypes', []))}", flush=True)
        conn.message({"MessageType": "SubscriptionConfirmation", "Message": {"CompressionEnabled": conn.deflate}})
        c.settimeout(0.2)
        t, last_ping, sent = 0, time.time(), 0
        while True:
            (a1, o1), (a2, o2) = conn.box
            lat, lon = (a1 + a2) / 2, (o1 + o2) / 2
            for i in range(args.ships):
                pos, static = ship_msgs(i, lat, lon, t)
                conn.message(pos)
                sent += 1
                if t % 5 == i % 5:
                    conn.message(static)
                    sent += 1
            if args.sart and t == 2:
                conn.message({"MessageType": "PositionReport", "MetaData": {"MMSI": 970123456, "ShipName": "SART"},
                              "Message": {"PositionReport": {"Latitude": lat + 0.01, "Longitude": lon - 0.02,
                                                             "NavigationalStatus": 14, "Sog": 0, "Cog": 360,
                                                             "TrueHeading": 511, "UserID": 970123456}}})
            if args.ping and time.time() - last_ping > args.ping:
                conn.send(9, b"mock")
                last_ping = time.time()
            end = time.time() + 1
            while time.time() < end:
                try:
                    op, fin, rsv1, p = read_frame(c)
                except socket.timeout:
                    continue
                if op == 8:
                    print(f"mock: the client closed, {sent} messages sent", flush=True)
                    return
                if op == 9:
                    conn.send(10, p)
                elif op == 1:
                    sub = json.loads(p)
                    conn.box = sub["BoundingBoxes"][0]
                    print(f"mock: subscribed again, box {conn.box}", flush=True)
            t += 1
    except (ConnectionError, OSError, ValueError) as e:
        print(f"mock: {addr[0]}: {e}", flush=True)
    finally:
        c.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8765)
    ap.add_argument("--key", default="test-key")
    ap.add_argument("--no-deflate", action="store_true")
    ap.add_argument("--keep-context", action="store_true")
    ap.add_argument("--fragment", action="store_true")
    ap.add_argument("--ping", type=float, default=0)
    ap.add_argument("--ships", type=int, default=8)
    ap.add_argument("--sart", action="store_true")
    args = ap.parse_args()
    s = socket.socket()
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("127.0.0.1", args.port))
    s.listen(4)
    print(f"mock: ws://127.0.0.1:{s.getsockname()[1]}/v0/stream", flush=True)
    while True:
        c, addr = s.accept()
        threading.Thread(target=serve, args=(c, addr, args), daemon=True).start()


if __name__ == "__main__":
    main()
