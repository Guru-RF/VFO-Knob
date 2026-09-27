#!/usr/bin/env python3
"""Tiny stdlib TCI probe: connect, print the greeting, optionally send commands.

Doubles as the M13 harness before the firmware client exists, and as a way to
inspect the REAL AetherSDR greeting at M15:

    python3 tools/ws_probe.py --host 172.16.32.201 --seconds 5
"""
import argparse
import base64
import os
import socket
import struct
import sys
import time


def connect(host, port, timeout=5.0):
    s = socket.create_connection((host, port), timeout=timeout)
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    key = base64.b64encode(os.urandom(16)).decode()
    s.sendall(
        f"GET / HTTP/1.1\r\nHost: {host}:{port}\r\nUpgrade: websocket\r\n"
        f"Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n".encode()
    )
    data = b""
    while b"\r\n\r\n" not in data:
        chunk = s.recv(4096)
        if not chunk:
            raise RuntimeError("closed during handshake")
        data += chunk
    if b"101" not in data.split(b"\r\n", 1)[0]:
        raise RuntimeError("upgrade refused: " + data.split(b"\r\n", 1)[0].decode())
    return s, data.split(b"\r\n\r\n", 1)[1]


def send_text(s, text):
    payload = text.encode()
    mask = os.urandom(4)
    masked = bytes(c ^ mask[i % 4] for i, c in enumerate(payload))
    n = len(payload)
    if n < 126:
        hdr = struct.pack("!BB", 0x81, 0x80 | n)
    else:
        hdr = struct.pack("!BBH", 0x81, 0x80 | 126, n)
    s.sendall(hdr + mask + masked)


def frames(s, leftover=b""):
    buf = leftover
    while True:
        while len(buf) < 2:
            chunk = s.recv(4096)
            if not chunk:
                return
            buf += chunk
        op = buf[0] & 0x0F
        ln = buf[1] & 0x7F
        off = 2
        if ln == 126:
            while len(buf) < 4:
                buf += s.recv(4096)
            ln = struct.unpack("!H", buf[2:4])[0]
            off = 4
        while len(buf) < off + ln:
            chunk = s.recv(4096)
            if not chunk:
                return
            buf += chunk
        payload, buf = buf[off:off + ln], buf[off + ln:]
        yield op, payload


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=50001)
    p.add_argument("--seconds", type=float, default=3.0)
    p.add_argument("--send", action="append", default=[],
                   help="command to send after the greeting (repeatable)")
    a = p.parse_args()

    s, leftover = connect(a.host, a.port)
    s.settimeout(0.3)
    print(f"connected to {a.host}:{a.port}")

    got_ready, sent, t0, n = False, False, time.time(), 0
    gen = frames(s, leftover)
    while time.time() - t0 < a.seconds:
        try:
            item = next(gen, None)
        except socket.timeout:
            if got_ready and not sent:
                for c in a.send:
                    print(f"  -> {c}")
                    send_text(s, c)
                sent = True
            continue
        except OSError as e:
            print(f"socket error: {e}")
            break
        if item is None:
            print("server closed")
            break
        op, payload = item
        if op == 0x8:
            code = struct.unpack("!H", payload[:2])[0] if len(payload) >= 2 else 0
            print(f"CLOSE code={code} reason={payload[2:].decode('utf-8','replace')!r}")
            break
        if op != 0x1:
            continue
        text = payload.decode("utf-8", "replace")
        n += 1
        print(f"  <- {text}")
        if "ready;" in text:
            got_ready = True
            print("  [greeting complete]")
            for c in a.send:
                print(f"  -> {c}")
                send_text(s, c)
            sent = True

    print(f"\n{n} frames, ready={got_ready}")
    return 0 if got_ready or not a.send else 1


if __name__ == "__main__":
    sys.exit(main())
