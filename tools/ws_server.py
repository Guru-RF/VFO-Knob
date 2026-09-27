"""Minimal WebSocket server, standard library only.

Enough of RFC 6455 to serve text frames to one or more clients. Written by hand
so the mock AetherSDR has no pip dependencies and runs unchanged in CI.
"""
import base64
import hashlib
import socket
import struct
import threading

_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

OP_CONT, OP_TEXT, OP_BIN, OP_CLOSE, OP_PING, OP_PONG = 0x0, 0x1, 0x2, 0x8, 0x9, 0xA


class WSError(Exception):
    pass


class WSConn:
    def __init__(self, sock, addr):
        self.sock, self.addr = sock, addr
        self._lock = threading.Lock()
        self.closed = False

    # -- handshake ---------------------------------------------------------
    def handshake(self):
        data = b""
        while b"\r\n\r\n" not in data:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise WSError("client closed during handshake")
            data += chunk
            if len(data) > 65536:
                raise WSError("handshake too large")
        head = data.split(b"\r\n\r\n", 1)[0].decode("latin-1")
        key = None
        for line in head.split("\r\n")[1:]:
            if ":" in line:
                k, v = line.split(":", 1)
                if k.strip().lower() == "sec-websocket-key":
                    key = v.strip()
        if not key:
            raise WSError("no Sec-WebSocket-Key")
        accept = base64.b64encode(
            hashlib.sha1((key + _GUID).encode()).digest()
        ).decode()
        self.sock.sendall(
            "HTTP/1.1 101 Switching Protocols\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            f"Sec-WebSocket-Accept: {accept}\r\n\r\n".encode()
        )

    # -- framing -----------------------------------------------------------
    def _recv_exact(self, n):
        buf = b""
        while len(buf) < n:
            chunk = self.sock.recv(n - len(buf))
            if not chunk:
                raise WSError("peer closed")
            buf += chunk
        return buf

    def recv_frame(self):
        """Returns (opcode, payload bytes). Reassembles continuations."""
        frags, first_op = [], None
        while True:
            b0, b1 = self._recv_exact(2)
            fin = b0 & 0x80
            op = b0 & 0x0F
            masked = b1 & 0x80
            ln = b1 & 0x7F
            if ln == 126:
                ln = struct.unpack("!H", self._recv_exact(2))[0]
            elif ln == 127:
                ln = struct.unpack("!Q", self._recv_exact(8))[0]
            mask = self._recv_exact(4) if masked else None
            payload = self._recv_exact(ln) if ln else b""
            if mask:
                payload = bytes(c ^ mask[i % 4] for i, c in enumerate(payload))

            if op in (OP_CLOSE, OP_PING, OP_PONG):
                return op, payload          # control frames are never fragmented
            if first_op is None:
                first_op = op
            frags.append(payload)
            if fin:
                return first_op, b"".join(frags)

    def send(self, payload, op=OP_TEXT):
        if self.closed:
            return
        if isinstance(payload, str):
            payload = payload.encode()
        n = len(payload)
        if n < 126:
            hdr = struct.pack("!BB", 0x80 | op, n)
        elif n < 65536:
            hdr = struct.pack("!BBH", 0x80 | op, 126, n)
        else:
            hdr = struct.pack("!BBQ", 0x80 | op, 127, n)
        with self._lock:
            try:
                self.sock.sendall(hdr + payload)
            except OSError:
                self.closed = True

    def close(self, code=1000, reason=""):
        if self.closed:
            return
        try:
            self.send(struct.pack("!H", code) + reason.encode(), OP_CLOSE)
        except Exception:
            pass
        self.closed = True
        try:
            self.sock.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        self.sock.close()

    def abort(self):
        """Vanish without a close frame -- simulates a yanked power lead."""
        self.closed = True
        try:
            # RST rather than FIN, so the peer sees a hard reset.
            self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER,
                                 struct.pack("ii", 1, 0))
            self.sock.close()
        except OSError:
            pass


def serve(host, port, on_client):
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((host, port))
    srv.listen(8)
    while True:
        sock, addr = srv.accept()
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        threading.Thread(target=on_client, args=(sock, addr), daemon=True).start()
