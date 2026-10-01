#!/usr/bin/env python3
"""The firmwares onto a knob's microSD card, through its USB cable.

For tools/install-setup.sh, as it provisions a knob:

    knob-card.py fetch <cache-dir> <plan.json>
        The release's index and every firmware in it -- the setup firmware
        too -- into the cache, each checked against its manifest's sha256,
        fetched only when the cache does not already have it. Writes the plan
        for `push`.

    knob-card.py push <port> <plan.json>
        Each of them down the cable onto the knob's SD card, in the minutes
        after install-setup.sh wrote the setup firmware: its console takes
        them then (main/app_main.c, card_console_task), and keeps an image
        only with its sha256. Then the index, for the setup firmware's list
        when there is no server.

The port is opened without touching DTR and RTS: the ESP32-S3's USB serial
restarts the chip when DTR drops while RTS is up -- which is what pyserial
does on opening -- and the knob may be in the middle of emptying the card.
"""
import hashlib
import json
import os
import select
import sys
import termios
import time
import urllib.request

ROOT = "https://raw.githubusercontent.com/Guru-RF/VFO-Knob/firmware/firmware/"
BLOCK = 8192        # main/app_main.c CARD_BLOCK: one block, one answer


def get(url):
    req = urllib.request.Request(url, headers={"User-Agent": "VFO-Knob provisioning"})
    with urllib.request.urlopen(req, timeout=60) as r:
        return r.read()


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def fetch(cache, plan_path):
    os.makedirs(cache, exist_ok=True)
    index = get(ROOT + "index.json")
    with open(os.path.join(cache, "index.json"), "wb") as f:
        f.write(index)
    radios = [x["radio"] for x in json.loads(index)["firmwares"] if x.get("radio")]
    plan = {"index": os.path.join(cache, "index.json"), "images": []}
    fetched = 0
    for radio in radios:
        raw = get(f"{ROOT}{radio}/manifest.json")
        m = json.loads(raw)
        path = os.path.join(cache, m["file"])
        have = os.path.exists(path) and sha256(open(path, "rb").read()) == m["sha256"]
        if not have:
            data = get(f"{ROOT}{radio}/{m['file']}")
            if sha256(data) != m["sha256"]:
                sys.exit(f"{m['file']} does not match its manifest's sha256")
            with open(path, "wb") as f:
                f.write(data)
            fetched += 1
        mpath = os.path.join(cache, f"{radio}.manifest.json")
        with open(mpath, "wb") as f:
            f.write(raw)
        plan["images"].append({"radio": radio, "version": m["version"], "image": path,
                               "manifest": mpath, "size": os.path.getsize(path)})
    with open(plan_path, "w") as f:
        json.dump(plan, f, indent=1)
    total = sum(x["size"] for x in plan["images"]) >> 20
    print(f"{len(plan['images'])} firmwares, {total} MB: {fetched} fetched, "
          f"{len(plan['images']) - fetched} from the cache ({cache})")


class Line:
    """The knob's console: raw bytes, its log ignored, our replies picked out."""

    def __init__(self, port):
        self.fd = os.open(port, os.O_RDWR | os.O_NOCTTY)
        a = termios.tcgetattr(self.fd)
        a[0] = 0                                        # no input processing
        a[1] = 0                                        # no output processing
        a[2] = (a[2] | termios.CS8 | termios.CREAD | termios.CLOCAL) & ~termios.HUPCL
        a[3] = 0                                        # raw: no echo, no lines
        a[6][termios.VMIN] = 0
        a[6][termios.VTIME] = 0
        termios.tcsetattr(self.fd, termios.TCSANOW, a)
        self.buf = b""

    def send(self, data):
        view = memoryview(data)
        while view:
            n = os.write(self.fd, view)
            view = view[n:]

    def reply(self, timeout, want=lambda l: True):
        end = time.time() + timeout
        while time.time() < end:
            while b"\n" in self.buf:
                line, self.buf = self.buf.split(b"\n", 1)
                line = line.strip(b"\r").decode(errors="replace")
                # The first bytes may be the tail of a log line the knob's
                # USB FIFO held while nobody listened: ours is after it.
                at = line.find("@card")
                if at >= 0 and want(line[at:]):
                    return line[at:]
            r, _, _ = select.select([self.fd], [], [], 0.2)
            if r:
                try:
                    self.buf += os.read(self.fd, 4096)
                except OSError:
                    time.sleep(0.2)
        return None


def begin(line):
    """A session: True with a card, False without one."""
    for _ in range(30):
        line.send(b"\n@card begin\n")
        r = line.reply(2, lambda l: l in ("@card ready", "@card none", "@card closed"))
        if r == "@card closed":
            # Only on the start right after install-setup.sh wrote the knob:
            # what the session needs is RAM the knob's WiFi cannot spare.
            sys.exit("The knob takes firmwares onto its SD card only in the minutes after\n"
                     "tools/install-setup.sh wrote it: run that again.")
        if r:
            return r == "@card ready"
    sys.exit("The knob did not answer on its console: is it running the setup firmware?")


def put(line, x):
    """One image onto the card: None when kept, else why not."""
    m = json.dumps(json.load(open(x["manifest"])), separators=(",", ":"))
    data = open(x["image"], "rb").read()
    line.send(f"@card put {x['radio']} {len(data)} {m}\n".encode())
    # A block at a time, each answered before the next: the knob's console
    # drops what its receive buffer cannot hold.
    for off in range(0, len(data), BLOCK):
        line.send(data[off:off + BLOCK])
        r = line.reply(20, lambda l: l.startswith("@card k ") or l.startswith("@card bad"))
        if not r:
            return f"no answer at {off} of {len(data)} bytes"
        if r.startswith("@card bad"):
            return f"at {off} of {len(data)} bytes: " + r.split(" ", 3)[-1]
    r = line.reply(60, lambda l, k=x["radio"]: l.startswith(f"@card ok {k}") or
                   l.startswith(f"@card bad {k}"))
    if not r:
        return "no answer at the end"
    return None if r.startswith("@card ok") else r.split(" ", 3)[-1]


def push(port, plan_path):
    plan = json.load(open(plan_path))
    line = Line(port)
    if not begin(line):
        print("No SD card in the knob: nothing put on it.")
        return
    kept = 0
    for x in plan["images"]:
        why = None
        for attempt in range(3):
            t = time.time()
            why = put(line, x)
            if why is None:
                break
            print(f"  {x['radio']:<11} {x['version']:<8} not kept ({why}), again ...")
            # A fresh session: the old one ends by itself once the line is
            # quiet for long enough.
            time.sleep(22)
            line.buf = b""
            if not begin(line):
                sys.exit("The SD card went away.")
        ok = why is None
        kept += ok
        print(f"  {x['radio']:<11} {x['version']:<8} "
              f"{'on the card' if ok else 'NOT KEPT: ' + why}  "
              f"({x['size'] >> 10} kB in {time.time() - t:.1f} s)")
    index = open(plan["index"], "rb").read()
    line.send(f"@card index {len(index)}\n".encode())
    line.send(index)
    line.reply(20, lambda l: l.startswith("@card ok index") or l.startswith("@card bad index"))
    line.send(b"@card end\n")
    done = line.reply(20, lambda l: l.startswith("@card done"))
    print(f"The SD card holds {kept} of {len(plan['images'])} firmwares." if done
          else "The knob did not say it was done.")
    if kept < len(plan["images"]):
        sys.exit(1)


if __name__ == "__main__":
    if len(sys.argv) == 4 and sys.argv[1] == "fetch":
        fetch(sys.argv[2], sys.argv[3])
    elif len(sys.argv) == 4 and sys.argv[1] == "push":
        push(sys.argv[2], sys.argv[3])
    else:
        sys.exit(__doc__)
