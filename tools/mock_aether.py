#!/usr/bin/env python3
"""Mock AetherSDR TCI server, with fault injection.

Exists so every failure path in the knob's TCI client is exercised before a real
transmitter is ever in the loop. Faults are the point: the happy path is the
easy part, and the protocol has no error frame, so a client that only ever sees
success will look fine right up until it sticks a transmitter on.

Modelled on AetherSDR's src/core/TciProtocol.cpp and TciServer.cpp:
  - one WebSocket TEXT frame per command (sendInitBurst splits on ';')
  - the greeting burst terminates with 'ready;'
  - sets are echoed to ALL clients, including the sender
  - every accepted vfo: also emits a dds:
  - rx_smeter broadcasts on a 200 ms timer, and is SUPPRESSED below -200 dBm
  - there is no error frame: a refusal is "here is the value you already had"

Usage:
    python3 tools/mock_aether.py [--port 50001] [faults...]
"""
import argparse
import random
import struct
import sys
import threading
import time

from ws_server import OP_CLOSE, OP_PING, OP_PONG, OP_TEXT, WSConn, WSError, serve

ARGS = None
LOCK = threading.Lock()
CLIENTS = []

STATE = {
    "freq":   [14074000, 7100000],   # per trx, channel 0
    "tx_freq":[14074000, 7100000],
    "mode":   ["usb", "cw"],
    "filt":   [(-2700, -300), (-250, 250)],
    "rit":    [0, 0],
    "lock":   [False, False],
    "ptt":    False,
    "smeter": -93.0,
}

TRX_COUNT = 2


def log(*a):
    print(f"[{time.strftime('%H:%M:%S')}]", *a, flush=True)


def send(c, cmd):
    if ARGS.latency:
        time.sleep(ARGS.latency / 1000.0)
    c.send(cmd)


def broadcast(cmd, exclude=None):
    with LOCK:
        # Only greeted clients get traffic. A half-open server must be TOTALLY
        # silent, otherwise the client sees frames but never 'ready;' and the
        # test stops exercising the greeting timeout.
        targets = [c for c in CLIENTS
                   if c is not exclude and not c.closed and getattr(c, "greeted", False)]
    for c in targets:
        send(c, cmd)


def greeting(c):
    """The burst, one command per frame, ending in ready;."""
    burst = [
        "protocol:ExpertSDR3,1.5;",
        "device:AetherSDR;",
        "receive_only:false;",
        f"trx_count:{TRX_COUNT};",
        "channels_count:2;",
        # Hardcoded server-side in the real thing; clients must not trust them
        # for band logic.
        "vfo_limits:1000,75000000;",
        "if_limits:-48000,48000;",
        "modulations_list:usb,lsb,cw,cwr,am,sam,fm,nfm,digu,digl,rtty;",
    ]
    for t in range(TRX_COUNT):
        lo, hi = STATE["filt"][t]
        burst += [
            f"vfo:{t},0,{STATE['freq'][t]};",
            f"vfo:{t},1,{STATE['tx_freq'][t]};",
            f"modulation:{t},{STATE['mode'][t]};",
            f"rx_filter_band:{t},{lo},{hi};",
            f"rit_offset:{t},{STATE['rit'][t]};",
            "rit_enable:%d,false;" % t,
            f"xit_offset:{t},0;",
            "xit_enable:%d,false;" % t,
            "split_enable:%d,false;" % t,
            f"lock:{t},{'true' if STATE['lock'][t] else 'false'};",
            f"tx_enable:{t},true;",
        ]
    burst += [
        "drive:0,50;",
        f"trx:0,{'true' if STATE['ptt'] else 'false'};",
        "active_slice:0;",
        "ready;",
    ]

    if ARGS.half_open:
        log("FAULT half-open: accepting TCP, sending nothing at all")
        return                      # client must time out and back off

    for cmd in burst:
        send(c, cmd)
    c.greeted = True
    log(f"greeting sent ({len(burst)} commands)")


def handle_vfo(c, trx, ch, hz):
    if ch != 0:
        return                                   # TX projection; ignore
    if STATE["lock"][trx] or ARGS.lock:
        # A locked slice does NOT produce an error -- it echoes the value the
        # slice already had. This is the case that rubber-bands naive clients.
        log(f"  locked: refusing {hz}, echoing {STATE['freq'][trx]}")
        broadcast(f"vfo:{trx},0,{STATE['freq'][trx]};")
        return

    accepted = hz
    if ARGS.clamp:
        lo, hi = ARGS.clamp
        accepted = max(lo, min(hi, hz))
        if accepted != hz:
            log(f"  clamped {hz} -> {accepted}")

    if accepted == STATE["freq"][trx]:
        # The real server coalesces a no-op tune and emits NOTHING. This is the
        # case that strands an entry in a naive client's echo ring.
        log(f"  no-op {accepted}, emitting nothing")
        return

    STATE["freq"][trx] = accepted
    broadcast(f"vfo:{trx},0,{accepted};")
    broadcast(f"dds:{trx},{accepted};")          # accompanies every accepted tune


def handle_trx(c, trx, on):
    if on:
        if ARGS.ptt_refuse:
            log("  FAULT ptt-refuse: answering trx:false")
            broadcast(f"trx:{trx},false;")
            return
        if ARGS.ptt_never_confirm:
            log("  FAULT ptt-never-confirm: swallowing the request")
            return
        delay = (ARGS.ptt_slow or 0) / 1000.0
        if delay:
            log(f"  FAULT ptt-slow: confirming in {delay:.2f}s")
            time.sleep(delay)
        STATE["ptt"] = True
        broadcast(f"trx:{trx},true;")
        log("  *** TX ON ***")
    else:
        STATE["ptt"] = False
        broadcast(f"trx:{trx},false;")
        log("  *** TX OFF ***")


def dispatch(c, line):
    line = line.strip().rstrip(";")
    if not line:
        return
    name, _, args = line.partition(":")
    a = args.split(",") if args else []
    log(f"RX  {line};")

    try:
        if name == "vfo" and len(a) >= 3:
            handle_vfo(c, int(a[0]), int(a[1]), int(a[2]))
        elif name == "trx" and len(a) >= 2:
            handle_trx(c, int(a[0]), a[1].lower() == "true")
        elif name == "modulation" and len(a) >= 2:
            STATE["mode"][int(a[0])] = a[1]
            broadcast(f"modulation:{a[0]},{a[1]};")
        elif name == "rx_filter_band" and len(a) >= 3:
            STATE["filt"][int(a[0])] = (int(a[1]), int(a[2]))
            broadcast(f"rx_filter_band:{a[0]},{a[1]},{a[2]};")
        elif name == "rit_offset" and len(a) >= 2:
            # Deliberately silent: the real server confirms this on NO path,
            # which is why the client has to GET it back.
            STATE["rit"][int(a[0])] = int(a[1])
            log("  (rit_offset accepted, no confirmation sent -- as upstream)")
        elif name in ("rx_sensors_enable", "tx_sensors_enable"):
            pass
        elif name == "active_slice":
            broadcast("active_slice:0;")
        else:
            log(f"  (ignored {name})")
    except (ValueError, IndexError) as e:
        log(f"  malformed, dropped silently (as upstream): {e}")


def telemetry():
    """200 ms broadcast timer, mirroring TciServer::broadcastStatus."""
    while True:
        time.sleep(0.2)
        if ARGS.silent_smeter:
            # dbm <= -200 is suppressed upstream, so a rig with no radio
            # attached emits NO periodic traffic. A client that infers liveness
            # from traffic will false-unkey here; one that uses PONG will not.
            continue
        STATE["smeter"] += random.uniform(-2, 2)
        STATE["smeter"] = max(-120.0, min(-30.0, STATE["smeter"]))
        broadcast(f"rx_smeter:0,{int(STATE['smeter'])};")
        broadcast(f"rx_channel_sensors:0,0,{STATE['smeter']:.1f};")
        if STATE["ptt"]:
            broadcast("tx_sensors:0,-12.5,48.2,48.2,%.2f,-3.0;" % ARGS.swr)


def band_change_pusher():
    """Unsolicited remote retune, like an operator changing band at the PC."""
    while True:
        time.sleep(ARGS.band_change_push)
        new = random.choice([3573000, 7074000, 14074000, 21074000, 28074000])
        STATE["freq"][0] = new
        log(f"PUSH remote band change -> {new}")
        broadcast(f"vfo:0,0,{new};")


def on_client(sock, addr):
    c = WSConn(sock, addr)
    try:
        c.handshake()
    except (WSError, OSError) as e:
        log(f"handshake failed from {addr}: {e}")
        return

    with LOCK:
        if len(CLIENTS) >= 8:
            log(f"refusing {addr}: at max-clients cap")
            c.close(1013, "server at max-clients cap")
            return
        CLIENTS.append(c)
    log(f"client connected: {addr}  ({len(CLIENTS)} total)")

    greeting(c)

    if ARGS.drop_after:
        def dropper():
            time.sleep(ARGS.drop_after)
            log(f"FAULT drop-after: {'aborting' if ARGS.hard_drop else 'closing'}")
            c.abort() if ARGS.hard_drop else c.close(1001, "going away")
        threading.Thread(target=dropper, daemon=True).start()

    if ARGS.close_1009:
        def killer():
            time.sleep(ARGS.close_1009)
            log("FAULT close-1009: TCI command backlog")
            c.close(1009, "TCI command backlog")
        threading.Thread(target=killer, daemon=True).start()

    buf = ""
    try:
        while not c.closed:
            op, payload = c.recv_frame()
            if op == OP_CLOSE:
                log(f"client sent close: {addr}")
                break
            if op == OP_PING:
                c.send(payload, OP_PONG)
                continue
            if op == OP_PONG:
                continue
            if op != OP_TEXT:
                continue
            buf += payload.decode("utf-8", "replace")
            while ";" in buf:
                line, _, buf = buf.partition(";")
                dispatch(c, line)
    except (WSError, OSError) as e:
        log(f"client gone: {addr} ({e})")
    finally:
        with LOCK:
            if c in CLIENTS:
                CLIENTS.remove(c)
        # Upstream: if the client that owned PTT disconnects, abortTciPtt()
        # unkeys unconditionally. This is the single most important safety
        # behaviour the knob depends on, so the mock must reproduce it.
        if STATE["ptt"]:
            STATE["ptt"] = False
            log("  *** TX OFF (client disconnected -> abortTciPtt) ***")
            broadcast("trx:0,false;")
        c.close()
        log(f"client removed: {addr}  ({len(CLIENTS)} left)")


def main():
    global ARGS
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--host", default="0.0.0.0")
    p.add_argument("--port", type=int, default=50001)
    p.add_argument("--lock", action="store_true",
                   help="slice locked: echo back the old frequency, never move")
    p.add_argument("--clamp", nargs=2, type=int, metavar=("LO", "HI"),
                   help="clamp tunes into [LO,HI]")
    p.add_argument("--latency", type=int, default=0, metavar="MS",
                   help="delay every outbound frame")
    p.add_argument("--drop-after", type=float, metavar="S",
                   help="close the connection S seconds after the greeting")
    p.add_argument("--hard-drop", action="store_true",
                   help="with --drop-after, RST instead of a clean close")
    p.add_argument("--close-1009", type=float, metavar="S",
                   help="close with 1009 'TCI command backlog' after S seconds")
    p.add_argument("--silent-smeter", action="store_true",
                   help="emit no periodic telemetry (the dbm<=-200 case)")
    p.add_argument("--ptt-refuse", action="store_true",
                   help="answer every key request with trx:false")
    p.add_argument("--ptt-never-confirm", action="store_true",
                   help="swallow key requests entirely")
    p.add_argument("--ptt-slow", type=int, metavar="MS",
                   help="confirm PTT after MS milliseconds")
    p.add_argument("--half-open", action="store_true",
                   help="accept TCP but never send the greeting")
    p.add_argument("--band-change-push", type=float, metavar="S",
                   help="push an unsolicited remote retune every S seconds")
    p.add_argument("--swr", type=float, default=1.3,
                   help="SWR reported in tx_sensors (>2.0 should alarm)")
    ARGS = p.parse_args()

    threading.Thread(target=telemetry, daemon=True).start()
    if ARGS.band_change_push:
        threading.Thread(target=band_change_pusher, daemon=True).start()

    faults = [k for k, v in vars(ARGS).items()
              if v and k not in ("host", "port", "swr")]
    log(f"mock AetherSDR TCI on ws://{ARGS.host}:{ARGS.port}")
    log(f"faults active: {', '.join(faults) if faults else 'none (happy path)'}")
    try:
        serve(ARGS.host, ARGS.port, on_client)
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
