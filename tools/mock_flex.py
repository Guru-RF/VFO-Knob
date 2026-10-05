#!/usr/bin/env python3
"""Mock FlexRadio: its API on TCP 4992, and its discovery broadcast.

For the multiflex firmware's memories, antennas and discovery, without a
radio: what a FLEX-6600 on SmartSDR 4.2.20 says, as the knob's own probes
recorded it (a slice's ant_list and tx_ant_list) and as FlexLib and
AetherSDR read it (the memories, the discovery packet). It never transmits:
`xmit 1`, `transmit tune 1` and `atu start` are refused, loudly.

    python3 tools/mock_flex.py                     the API on 127.0.0.1:4992
    python3 tools/mock_flex.py --announce          ...and its broadcast, to
                                                   127.0.0.1:4992 once a second
    python3 tools/mock_flex.py --host 0.0.0.0 --announce --to 255.255.255.255
                                                   a mock radio on the LAN, for
                                                   a knob to find and join

Faults, for the paths the real radio does not take every day:
    --refuse-apply      `memory apply` answered with an error: the knob tunes
                        the slice by hand instead
    --air-on-apply      the first `memory apply` finds another station on the
                        air, for a second: the knob tunes nothing meanwhile
    --quiet-apply       `memory apply` brings no slice status: the knob shows
                        the memory from its own list
    --change-after S    S seconds in, a memory renamed, one added, one removed

As a FLEX-6600 does (AetherSDR #1871), `memory apply` sets the slice's
repeater direction, offset and tone but leaves tx_offset_freq -- what moves
the transmitter -- as it was: a client sets that itself. Every change to it
is logged as "transmits on <MHz>", where the slice's next over would go.

The protocol, as flex_client.c speaks it: C<seq>|<command> in, R<seq>|<hex
code>|<text> back, S<handle>|<object> key=value ... for what changed; V and
H to greet. A space in a value travels as 0x7F.
"""
import argparse
import socket
import struct
import sys
import threading
import time

ARGS = None
LOCK = threading.Lock()
CLIENTS = []                       # (sock, handle)
HANDLE0 = 0x2C470BCA

SLICE = {
    "in_use": "1", "sample_rate": "24000", "RF_frequency": "14.200000",
    "index_letter": "A", "rit_on": "0", "rit_freq": "0", "xit_on": "0", "xit_freq": "0",
    "rxant": "ANT1", "mode": "USB", "wide": "0", "filter_lo": "100", "filter_hi": "2800",
    "step": "100", "agc_mode": "med", "pan": "0x40000000", "txant": "ANT1", "lock": "0",
    "tx": "1", "active": "1", "audio_level": "50", "audio_mute": "0",
    "ant_list": "ANT1,ANT2,RX_A,RX_B,XVTA,XVTB",
    "mode_list": "LSB,USB,AM,CW,DIGL,DIGU,SAM,FM,NFM,DFM,RTTY",
    "repeater_offset_dir": "SIMPLEX", "fm_repeater_offset_freq": "0.000000",
    "tx_offset_freq": "0.000000", "fm_tone_mode": "OFF", "fm_tone_value": "67.0",
    "rfgain": "8", "tx_ant_list": "ANT1,ANT2,XVTA,XVTB", "max_internal_pa_power": "100",
}

DEL = "\x7f"                       # the API's space


def mem(idx, freq, name, mode, lo, hi, group="", repeater="SIMPLEX", offset="0.000000",
        tone_mode="OFF", tone="67.0"):
    return {"idx": idx, "owner": "ON6URE", "group": group.replace(" ", DEL), "freq": freq,
            "name": name.replace(" ", DEL), "mode": mode, "step": "100", "repeater": repeater,
            "repeater_offset": offset, "tone_mode": tone_mode, "tone_value": tone, "power": "100",
            "rx_filter_low": str(lo), "rx_filter_high": str(hi), "highlight": "0",
            "highlight_color": "0x00000000", "squelch": "1", "squelch_level": "20",
            "rtty_mark": "2125", "rtty_shift": "170", "digl_offset": "2210", "digu_offset": "1500"}


# Out of frequency order, as a radio numbers them as they were made.
MEMORIES = [
    mem(0, "14.074000", "FT8", "DIGU", 0, 3000, "Digital"),
    mem(1, "7.090000", "Belgian net", "LSB", -2800, -100, "Nets"),
    mem(2, "29.620000", "ON0TEN", "FM", -8000, 8000, "Repeaters", "DOWN", "0.100000",
        "CTCSS_TX", "79.7"),
    mem(4, "3.700000", "", "LSB", -2800, -100),
    mem(7, "50.150000", "6m beacons", "USB", 100, 2800, "Beacons"),
    # 70 cm, 7.6 MHz down, and one shifted up: the offset's sign and its MHz.
    mem(11, "439.150000", "ON0LG", "FM", -8000, 8000, "Repeaters", "DOWN", "7.600000",
        "CTCSS_TX", "74.4"),
    mem(12, "145.000000", "Up test", "FM", -8000, 8000, "Repeaters", "UP", "0.600000"),
]


def log(*a):
    print(f"[{time.strftime('%H:%M:%S')}]", *a, flush=True)


def kvline(d, skip=()):
    return " ".join(f"{k}={v}" for k, v in d.items() if k not in skip)


def send(c, line):
    try:
        c.sendall((line + "\n").encode("latin-1"))
    except OSError:
        pass


def status(c, h, obj):
    send(c, f"S{h:08X}|{obj}")


def to_all(obj, exclude=None):
    with LOCK:
        for c, h in CLIENTS:
            if c is not exclude:
                status(c, h, obj)


def slice_status(keys=None):
    d = SLICE if keys is None else {k: SLICE[k] for k in keys}
    return f"slice 0 client_handle=0x{HANDLE0:08X} " + kvline(d)


def memory_status(m):
    return f"memory {m['idx']} " + kvline(m, ("idx",))


def transmits(why):
    """Where the slice's next over would go: its frequency and its TX offset."""
    f = float(SLICE["RF_frequency"]) + float(SLICE["tx_offset_freq"])
    log(f"transmits on {f:.6f} MHz ({why}: {SLICE['repeater_offset_dir']}, "
        f"tx_offset_freq {SLICE['tx_offset_freq']}, tone {SLICE['fm_tone_mode']} {SLICE['fm_tone_value']})")


def on_the_air(c, h, on):
    """Another station on the air, or not: as the interlock says it."""
    other = HANDLE0 + 0x100
    status(c, h, "interlock state=" + ("TRANSMITTING" if on else "READY") +
           f" tx_allowed=1 reason= source= tx_client_handle=0x{other if on else 0:08X}")
    log("another station " + ("on the air" if on else "back on receive"))


def apply_memory(c, h, seq, idx):
    m = next((m for m in MEMORIES if m["idx"] == idx), None)
    if m is None:
        send(c, f"R{seq}|5000002D|no such memory")
        return
    if ARGS.air_on_apply and not getattr(apply_memory, "aired", False):
        apply_memory.aired = True
        on_the_air(c, h, True)
        threading.Timer(1.0, on_the_air, (c, h, False)).start()
    if ARGS.refuse_apply:
        log(f"memory apply {idx}: refused (--refuse-apply)")
        send(c, f"R{seq}|50000016|mock: memory apply refused")
        return
    SLICE["RF_frequency"] = m["freq"]
    SLICE["mode"] = m["mode"]
    if m["rx_filter_low"] != "0" or m["rx_filter_high"] != "0":
        SLICE["filter_lo"], SLICE["filter_hi"] = m["rx_filter_low"], m["rx_filter_high"]
    SLICE["repeater_offset_dir"] = m["repeater"]
    SLICE["fm_repeater_offset_freq"] = m["repeater_offset"]
    SLICE["fm_tone_mode"] = m["tone_mode"]
    SLICE["fm_tone_value"] = m["tone_value"]
    # ...and tx_offset_freq left as it was, as the radio leaves it.
    log(f"memory apply {idx}: {m['freq']} MHz {m['mode']} \"{m['name'].replace(DEL, ' ')}\"")
    transmits(f"memory {idx} applied")
    send(c, f"R{seq}|0|")
    if not ARGS.quiet_apply:
        to_all(slice_status(["RF_frequency", "mode", "filter_lo", "filter_hi", "repeater_offset_dir",
                             "fm_repeater_offset_freq", "fm_tone_mode", "fm_tone_value"]))


def command(c, h, seq, cmd):
    w = cmd.split()
    ok = f"R{seq}|0|"
    if not w:
        send(c, ok)
    elif cmd.startswith(("xmit 1", "transmit tune 1", "atu start")):
        log(f"!!! '{cmd}': refused -- this mock never transmits")
        send(c, f"R{seq}|50000016|mock: never transmits")
    elif cmd == "ping":
        send(c, ok)
    elif cmd.startswith("client gui"):
        send(c, ok + (w[2] if len(w) > 2 else "00000000-0000-4000-8000-000000000000"))
    elif cmd == "sub client all":
        send(c, ok)
    elif cmd == "sub slice all":
        send(c, ok)
        status(c, h, slice_status())
    elif cmd == "sub pan all":
        send(c, ok)
        status(c, h, f"display pan 0x40000000 client_handle=0x{HANDLE0:08X} x_pixels=50 y_pixels=20 "
                     f"center=14.200000 bandwidth=0.200000 rfgain=8 rxant=ANT1 waterfall=0x42000000 "
                     f"ant_list=ANT1,ANT2,RX_A,RX_B,XVTA,XVTB")
    elif cmd == "sub tx all":
        send(c, ok)
        status(c, h, "interlock state=READY tx_allowed=1 reason= source= tx_client_handle=0x00000000")
    elif cmd == "sub meter all":
        send(c, ok)
        status(c, h, "meter 1.src=SLC#1.num=0#1.nam=LEVEL#1.unit=dBm#1.low=-150.0#1.hi=20.0")
    elif cmd == "sub atu all":
        send(c, ok)
        status(c, h, "atu status=NONE atu_enabled=0 memories_enabled=0 using_mem=0")
    elif cmd == "sub memories all":
        send(c, ok)
        for m in MEMORIES:
            status(c, h, memory_status(m))
        log(f"memories: {len(MEMORIES)} sent")
    elif cmd == "slice list":
        send(c, ok + "0")
    elif cmd.startswith("display pan rfgain_info"):
        send(c, ok + "-8,32,8")
    elif cmd.startswith("stream create type=remote_audio_rx"):
        send(c, ok + "0x04000008")
    elif cmd.startswith("stream create type=remote_audio_tx"):
        send(c, ok + "0x84000000")
    elif w[0] == "memory" and len(w) == 3 and w[1] == "apply":
        apply_memory(c, h, seq, int(w[2]))
    elif w[:2] == ["slice", "set"] and len(w) >= 4:
        send(c, ok)
        shift = False
        for kv in w[3:]:
            k, _, v = kv.partition("=")
            if k in ("rxant", "txant"):
                lst = SLICE["ant_list" if k == "rxant" else "tx_ant_list"].split(",")
                if v not in lst:
                    log(f"!!! slice set {k}={v}: not in the slice's list {lst}")
                log(f"{'receive' if k == 'rxant' else 'transmit'} antenna: {v}")
            elif k == "repeater_offset_dir":
                log(f"repeater offset: {v}")
                if v != v.lower():
                    log(f"!!! repeater_offset_dir={v}: FlexLib and AetherSDR send it in lower case")
            if k in ("repeater_offset_dir", "fm_repeater_offset_freq", "tx_offset_freq",
                     "fm_tone_mode", "fm_tone_value"):
                shift = True
            if k in SLICE:
                SLICE[k] = v.upper() if k in ("repeater_offset_dir", "fm_tone_mode") else v
        if shift:
            transmits("slice set")
        # The radio does not echo a client's own settings to it: to the others.
        to_all(f"slice 0 " + " ".join(w[3:]), exclude=c)
    elif w[:2] == ["slice", "tune"] and len(w) >= 4:
        SLICE["RF_frequency"] = f"{float(w[3]):.6f}"
        log(f"slice tune: {SLICE['RF_frequency']} MHz")
        transmits("slice tune")
        send(c, ok)
    else:
        send(c, ok)
    if w and w[0] not in ("ping",):
        log(f"<- C{seq}|{cmd}")


def client_thread(c, addr):
    global HANDLE0
    with LOCK:
        h = HANDLE0 if not CLIENTS else HANDLE0 + len(CLIENTS)
        CLIENTS.append((c, h))
    log(f"client {addr[0]}:{addr[1]} connected, handle 0x{h:08X}")
    send(c, "V1.4.0.0")
    send(c, f"H{h:08X}")
    buf = b""
    try:
        while True:
            d = c.recv(4096)
            if not d:
                break
            buf += d
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                s = line.decode("latin-1").strip()
                if s.startswith("C") and "|" in s:
                    seq, cmd = s[1:].split("|", 1)
                    command(c, h, seq, cmd)
                elif s == "\x04":
                    log("goodbye byte")
    except OSError:
        pass
    with LOCK:
        CLIENTS[:] = [x for x in CLIENTS if x[0] is not c]
    log(f"client {addr[0]}:{addr[1]} gone")


def discovery_packet(host, port):
    """A VITA-49 extension-data packet with a stream id and a class id --
    FlexRadio's OUI, packet class 0xFFFF -- and the radio's key=value text,
    NUL-padded to whole words, as SmartSDR sends it."""
    text = (f"discovery_protocol_version=3.0.0.2 model=FLEX-6600 serial=1919-1212-6600-0001 "
            f"version=4.2.20.41343 nickname=Mock{DEL}Flex callsign=ON6URE ip={host} port={port} "
            f"status=Available inuse_ip= inuse_host= max_licensed_version=v4 radio_license_id=00 "
            f"fpc_mac= wan_connected=1 licensed_clients=2 available_clients=1 max_panadapters=4 "
            f"available_panadapters=3 max_slices=4 available_slices=3 "
            f"gui_client_ips=172.16.31.9 gui_client_hosts=thinkstation gui_client_programs=SmartSDR-Win "
            f"gui_client_stations=thinkstation gui_client_handles=0x31CE0037 min_software_version=3.0.0.0")
    payload = text.encode("latin-1")
    payload += b"\x00" * (-len(payload) % 4)
    words = 7 + len(payload) // 4
    count = discovery_packet.count = (getattr(discovery_packet, "count", -1) + 1) & 0xF
    # type 3 (extension data, stream id), class id present, TSI 3 (other), TSF 1 (sample count)
    hdr = struct.pack(">IIIIIQ", 0x38000000 | 0x00D00000 | count << 16 | words, 0x00000800,
                      0x00001C2D, 0x534CFFFF, 0, 0)
    return hdr + payload


def announce(to, port, api_host, api_port):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    ip = api_host if api_host not in ("0.0.0.0", "") else ARGS.ip
    log(f"announcing to {to}:{port} once a second, as 'Mock Flex' at {ip}:{api_port}")
    while True:
        s.sendto(discovery_packet(ip, api_port), (to, port))
        time.sleep(1.0)


def changes():
    time.sleep(ARGS.change_after)
    MEMORIES[0]["name"] = "FT8" + DEL + "20m"
    to_all(f"memory 0 name={MEMORIES[0]['name']}")
    log("memory 0 renamed")
    m = mem(9, "21.074000", "FT8 15m", "DIGU", 0, 3000, "Digital")
    MEMORIES.append(m)
    to_all(memory_status(m))
    log("memory 9 added")
    MEMORIES[:] = [m for m in MEMORIES if m["idx"] != 4]
    to_all("memory 4 removed")
    log("memory 4 removed")


def main():
    global ARGS
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--host", default="127.0.0.1", help="the API's address (127.0.0.1)")
    p.add_argument("--port", type=int, default=4992, help="the API's port (4992)")
    p.add_argument("--announce", action="store_true", help="broadcast as a radio does")
    p.add_argument("--to", default="127.0.0.1", help="where to (127.0.0.1; 255.255.255.255 for the LAN)")
    p.add_argument("--to-port", type=int, default=4992, help="the discovery port (4992)")
    p.add_argument("--ip", default="127.0.0.1", help="the address announced when --host is 0.0.0.0")
    p.add_argument("--refuse-apply", action="store_true")
    p.add_argument("--air-on-apply", action="store_true")
    p.add_argument("--quiet-apply", action="store_true")
    p.add_argument("--change-after", type=float, default=0)
    ARGS = p.parse_args()
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((ARGS.host, ARGS.port))
    srv.listen(4)
    port = srv.getsockname()[1]
    log(f"mock FlexRadio API on {ARGS.host}:{port}")
    if ARGS.announce:
        threading.Thread(target=announce, args=(ARGS.to, ARGS.to_port, ARGS.host, port), daemon=True).start()
    if ARGS.change_after:
        threading.Thread(target=changes, daemon=True).start()
    try:
        while True:
            c, addr = srv.accept()
            threading.Thread(target=client_thread, args=(c, addr), daemon=True).start()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    sys.exit(main())
