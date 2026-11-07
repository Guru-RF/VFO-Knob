#!/usr/bin/env python3
"""Mock Elecraft K4: its remote server on TCP 9205 (the clear port).

For the k4 firmware's client (components/k4_client), without a radio: the
SHA-384 password proof, the K4/0 frame envelope, and the CAT subset the knob
speaks, answered the way the K4 Programmer's Reference (rev. D12) lays its
responses out. It never transmits: a key only moves this mock's own flag, and
each one is logged, loudly.

    python3 tools/mock_k4.py                      127.0.0.1:9205, password "secret"
    python3 tools/mock_k4.py --port 29205 --password hunter2

Faults, for the paths a real radio does not take every day:
    --refuse-tx         `TX;` answered with an error, as a radio that may not
                        transmit (out of band, inhibited) does
    --ignore-tx         `TX;` is not answered and nothing happens: a radio that
                        stays on receive (the capture of 2026-11-07 14:5x)
    --late-tx S         `TX;` takes effect S seconds later, and an `RX;` sent in the
                        meantime does nothing: the worst a slow radio could do
    --change-after S    S seconds in, the radio is retuned from its own front
                        panel: 7.100 MHz, LSB
    --mute-pong         PING is never answered: the knob must call the link dead

What this is NOT: a K4. Its audio is the empty Opus packet -- a TOC byte and
nothing else, which a decoder takes as a lost frame and conceals -- sent every
20 ms once `EM` has been seen: enough for the knob's receive path to run,
nothing to listen to. What it accepts back, it counts and logs (header only).
"""
import argparse
import hashlib
import random
import socket
import struct
import sys
import threading
import time

START = bytes([0xFE, 0xFD, 0xFC, 0xFB])
END = bytes([0xFB, 0xFC, 0xFD, 0xFE])
OPUS_EMPTY_STEREO_20MS = bytes([0xDC])     # CELT SWB, 20 ms, stereo, one frame, no bytes

ARGS = None
LOCK = threading.Lock()
RADIO = {
    "f": 14074000, "mode": "2", "bw": 2400, "is": 1500, "agc": 2, "rg": 0, "rit": 0, "rit_on": 0,
    "pc": 50, "pc_range": "H", "tx": 0, "tune": 0, "nb": 0, "nr": 0, "nr_level": 4, "na": 0,
    "ap": 0, "ap_w": 1, "smeter": -73,
}


def log(msg):
    print(f"{time.strftime('%H:%M:%S')} {msg}", flush=True)


def frame(payload):
    return START + struct.pack(">I", len(payload)) + payload + END


def cat(text):
    return frame(b"\x00\x00\x00" + text.encode())


def if_line():
    r = RADIO
    rit = r["rit"]
    return "IF%011d     %s%04d%d0 00%d%s0000001 ;" % (
        r["f"], "-" if rit < 0 else "+", abs(rit), r["rit_on"], r["tx"], r["mode"])


def answer(cmd, send):
    """One command (no ';') -> the responses it brings."""
    r = RADIO
    if cmd == "PING":
        if not ARGS.mute_pong:
            send(cat("PONG;"))
    elif cmd == "RDY":
        send(cat(if_line()))
    elif cmd in ("K41", "ER1"):
        send(cat(cmd + ";"))
    elif cmd.startswith("EM"):
        log(f"audio mode {cmd}")
        threading.Thread(target=audio_loop, args=(send,), daemon=True).start()
        send(cat(cmd + ";"))
    elif cmd.startswith("SL"):
        send(cat(cmd + ";"))
    elif cmd == "IF":
        send(cat(if_line()))
    elif cmd == "FA":
        send(cat("FA%011d;" % r["f"]))
    elif cmd.startswith("FA") and cmd[2:].isdigit():
        r["f"] = int(cmd[2:])
        log(f"tuned to {r['f']}")
        send(cat("FA%011d;" % r["f"]))
    elif cmd == "MD":
        send(cat("MD%s;" % r["mode"]))
    elif cmd.startswith("MD") and cmd[2:] in list("12345679"):
        r["mode"] = cmd[2:]
        log(f"mode MD{r['mode']}")
        send(cat("MD%s;" % r["mode"]))
        send(cat("BW%04d;" % (r["bw"] // 10)))
    elif cmd == "BW":
        send(cat("BW%04d;" % (r["bw"] // 10)))
    elif cmd.startswith("BW") and cmd[2:].isdigit():
        r["bw"] = int(cmd[2:]) * 10
        log(f"bandwidth {r['bw']} Hz")
        send(cat("BW%04d;" % (r["bw"] // 10)))
    elif cmd == "IS":
        send(cat("IS%04d;" % (r["is"] // 10)))
    elif cmd == "SMH":
        r["smeter"] = max(-120, min(-20, r["smeter"] + random.choice((-2, -1, 0, 1, 2))))
        send(cat("SMH%+04d;" % r["smeter"]))
    elif cmd == "SM":
        send(cat("SM%02d;" % 21))
    elif cmd == "GT":
        send(cat("GT%d;" % r["agc"]))
    elif cmd.startswith("GT") and cmd[2:].isdigit():
        r["agc"] = int(cmd[2:])
        log(f"AGC GT{r['agc']}")
        send(cat("GT%d;" % r["agc"]))
    elif cmd == "RG":
        send(cat("RG-%02d;" % -r["rg"]))
    elif cmd.startswith("RG-"):
        r["rg"] = -int(cmd[3:])
        log(f"RF gain {r['rg']} dB")
        send(cat("RG-%02d;" % -r["rg"]))
    elif cmd == "RO":
        send(cat("RO%s%04d;" % ("-" if r["rit"] < 0 else "+", abs(r["rit"]))))
    elif cmd.startswith("RO") and len(cmd) == 7:
        r["rit"] = int(cmd[3:]) * (-1 if cmd[2] == "-" else 1)
        log(f"RIT offset {r['rit']}")
        send(cat("RO%s%04d;" % ("-" if r["rit"] < 0 else "+", abs(r["rit"]))))
    elif cmd.startswith("RT") and cmd[2:] in ("0", "1"):
        r["rit_on"] = int(cmd[2:])
        log(f"RIT {'on' if r['rit_on'] else 'off'}")
        send(cat(cmd + ";"))
    elif cmd == "PC":
        send(cat("PC%03d%s;" % (r["pc"], r["pc_range"])))
    elif cmd == "NB":
        send(cat("NB05%d0;" % r["nb"]))
    elif cmd.startswith("NB") and len(cmd) == 3:
        r["nb"] = int(cmd[2])
        log(f"NB {'on' if r['nb'] else 'off'}")
        send(cat("NB05%d0;" % r["nb"]))
    elif cmd == "NR":
        send(cat("NR%02d%d;" % (r["nr_level"], r["nr"])))
    elif cmd.startswith("NR") and len(cmd) == 5:
        r["nr_level"], r["nr"] = int(cmd[2:4]), int(cmd[4])
        log(f"NR {'on' if r['nr'] else 'off'} level {r['nr_level']}")
        send(cat("NR%02d%d;" % (r["nr_level"], r["nr"])))
    elif cmd == "NA":
        send(cat("NA%d;" % r["na"]))
    elif cmd.startswith("NA") and len(cmd) == 3:
        r["na"] = int(cmd[2])
        log(f"NA {'on' if r['na'] else 'off'}")
        send(cat("NA%d;" % r["na"]))
    elif cmd == "AP":
        send(cat("AP%d%d;" % (r["ap"], r["ap_w"])))
    elif cmd.startswith("AP") and len(cmd) == 4:
        r["ap"], r["ap_w"] = int(cmd[2]), int(cmd[3])
        log(f"APF {'on' if r['ap'] else 'off'} width {r['ap_w']}")
        send(cat("AP%d%d;" % (r["ap"], r["ap_w"])))
    elif cmd == "ID":
        send(cat("ID017;"))
    elif cmd == "TM1":
        pass
    elif cmd == "TQ":
        if r.get("pending_tx") and time.time() >= r["pending_tx"]:
            r["pending_tx"] = 0
            r["tx"] = 1
            log("*** the radio would TRANSMIT (voice) ***")
        send(cat("TQ%d;" % r["tx"]))
    elif cmd == "TX":
        if ARGS.late_tx:
            r["pending_tx"] = time.time() + ARGS.late_tx
            log("TX will take effect late")
        elif ARGS.ignore_tx:
            log("TX ignored")
        elif ARGS.refuse_tx:
            log("TX refused")
            send(cat("ER14:TX inhibited;"))
        else:
            r["tx"] = 1
            log("*** the radio would TRANSMIT (voice) ***")
            send(cat("TQ1;"))
    elif cmd == "RX":
        if r["tx"]:
            log("the radio is back on receive")
        r["tx"] = 0
        send(cat("RX;"))
        send(cat("TQ0;"))
    elif cmd == "TU1":
        if ARGS.refuse_tx:
            log("TUNE refused")
            send(cat("ER14:TX inhibited;"))
        else:
            r["tx"] = r["tune"] = 1
            log("*** the radio would TRANSMIT (tune carrier) ***")
            send(cat("TQ1;"))
    elif cmd == "TU0":
        r["tune"] = 0
        log("tune carrier off")
    else:
        log(f"unknown command: {cmd}")


def audio_loop(send):
    seq = 0
    try:
        while True:
            time.sleep(0.02)
            payload = bytes([1, 1, seq & 0xFF, 3]) + struct.pack("<H", 240) + b"\x00" + OPUS_EMPTY_STEREO_20MS
            send(frame(payload))
            seq += 1
    except OSError:
        return


def meters_loop(send):
    """While the radio is on the air: the TX meters the K4 pushes on TM1."""
    try:
        while True:
            time.sleep(0.1)
            if RADIO["tx"]:
                send(cat("TM003000%03d012;" % (RADIO["pc"] if RADIO["pc_range"] == "H" else RADIO["pc"])))
    except OSError:
        return


def change_later(send):
    time.sleep(ARGS.change_after)
    RADIO["f"] = 7100000
    RADIO["mode"] = "1"
    log("the front panel retunes the radio: 7.100 MHz LSB")
    try:
        send(cat("FA%011d;MD1;" % RADIO["f"]))
    except OSError:
        pass


def serve(conn, addr):
    wlock = threading.Lock()

    def send(b):
        with wlock:
            conn.sendall(b)

    want = hashlib.sha384(ARGS.password.encode()).hexdigest().encode()
    got = b""
    conn.settimeout(5)
    try:
        while len(got) < len(want):
            d = conn.recv(len(want) - len(got))
            if not d:
                log("closed before the password")
                return
            got += d
    except OSError:
        return
    if got != want:
        log(f"WRONG PASSWORD from {addr[0]} -- closing")
        conn.close()
        return
    log(f"client {addr[0]}:{addr[1]} logged in")
    conn.settimeout(None)
    threading.Thread(target=meters_loop, args=(send,), daemon=True).start()
    if ARGS.change_after:
        threading.Thread(target=change_later, args=(send,), daemon=True).start()
    buf = b""
    tx_frames = 0
    try:
        while True:
            d = conn.recv(4096)
            if not d:
                break
            buf += d
            while True:
                i = buf.find(START)
                if i < 0:
                    buf = buf[-3:]
                    break
                buf = buf[i:]
                if len(buf) < 8:
                    break
                n = struct.unpack(">I", buf[4:8])[0]
                if len(buf) < 8 + n + 4:
                    break
                payload, end = buf[8:8 + n], buf[8 + n:8 + n + 4]
                buf = buf[8 + n + 4:]
                if end != END:
                    log("BAD frame end from the knob")
                    continue
                if payload[:1] == b"\x00":
                    for c in payload[3:].decode(errors="replace").split(";"):
                        if c:
                            log(f"rx: {c}")
                            answer(c, send)
                elif payload[:1] == b"\x01":
                    tx_frames += 1
                    if tx_frames == 1:
                        log("TX audio: mode %d, %d samples, rate code %d, %d bytes of Opus" % (
                            payload[3], struct.unpack("<H", payload[4:6])[0], payload[6], len(payload) - 7))
    except OSError:
        pass
    log(f"client left ({tx_frames} TX audio frames)")
    if RADIO["tx"]:
        RADIO["tx"] = 0
        log("the connection is gone with the radio still keyed (a real K4 may or may not drop the key: not known)")


def main():
    global ARGS
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=9205)
    ap.add_argument("--password", default="secret")
    ap.add_argument("--refuse-tx", action="store_true")
    ap.add_argument("--ignore-tx", action="store_true")
    ap.add_argument("--late-tx", type=float, default=0)
    ap.add_argument("--change-after", type=float, default=0)
    ap.add_argument("--mute-pong", action="store_true")
    ARGS = ap.parse_args()
    s = socket.socket()
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind((ARGS.host, ARGS.port))
    s.listen(4)
    log(f"mock K4 on {ARGS.host}:{ARGS.port}")
    while True:
        c, a = s.accept()
        threading.Thread(target=serve, args=(c, a), daemon=True).start()


if __name__ == "__main__":
    sys.exit(main())
