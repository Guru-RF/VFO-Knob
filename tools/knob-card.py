#!/usr/bin/env python3
"""The firmwares onto a knob's microSD card, through its USB cable -- and
what the bench writes on its second chip.

For tools/install-setup.sh, as it provisions a knob:

    knob-card.py fetch <cache-dir> <plan.json>
        The release's index and every firmware in it -- the setup firmware
        too, and the second chip's -- into the cache, each checked against
        its manifest's sha256, fetched only when the cache does not already
        have it. Writes the plan for `push`.

    knob-card.py push <port> <plan.json>
        Each of them down the cable onto the knob's SD card, in the minutes
        after install-setup.sh wrote the setup firmware: its console takes
        them then (main/app_main.c, card_console_task), and keeps an image
        only with its sha256. Then the index, for the setup firmware's list
        when there is no server.

    knob-card.py chip <cache-dir> <parts.json> [<build-dir>]
        What the bench writes on the second chip, once per knob: a
        bootloader that can go back to the firmware before, the partition
        table and an empty otadata -- published once, with the second chip's
        first release (firmware/companion/bench/) -- and its firmware, the
        latest release's. Into the cache, each checked against its
        manifest's sha256; or, from a build directory (companion/build_a,
        say), that build's. Each is checked for what the chip needs of it,
        and their paths written for install-setup.sh. Exit status 3: no
        second-chip firmware is published yet.

The port is opened without touching DTR and RTS: the ESP32-S3's USB serial
restarts the chip when DTR drops while RTS is up -- which is what pyserial
does on opening -- and the knob may be in the middle of emptying the card.
"""
import hashlib
import json
import os
import select
import struct
import sys
import termios
import time
import urllib.error
import urllib.request

ROOT = "https://raw.githubusercontent.com/Guru-RF/VFO-Knob/firmware/firmware/"
BLOCK = 8192        # main/app_main.c CARD_BLOCK: one block, one answer
NOT_YET = 3         # chip: no second-chip firmware is published yet

# The second chip's flash as the bench leaves it. install-setup.sh writes the
# ESP32's bootloader at 0x1000 and the partition table at 0x8000, empties
# otadata and writes the firmware into ota_0, and leaves the settings (the
# headset's pairing) where they are -- so the table must say just that. It
# is frozen once a knob has it: an update never writes it, and
# tools/release.sh holds every release to the one published.
CHIP_PARTS = {"bootloader.bin": 0x1000, "partition-table.bin": 0x8000, "ota_data_initial.bin": 0xE000}
CHIP_TABLE = {"nvs": (0x9000, 0x5000), "otadata": (0xE000, 0x2000),
              "ota_0": (0x20000, 0x1E0000), "ota_1": (0x200000, 0x1E0000)}


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
    idx = json.loads(index)
    radios = [x["radio"] for x in idx["firmwares"] if x.get("radio")]
    # The second chip's firmware, under a key of its own in the index -- never
    # in "firmwares", the setup firmware's list: onto the card too, where a
    # radio's firmware finds it and hands it to that chip.
    if idx.get("companion") and "companion" not in radios:
        radios.append("companion")
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


def cached(cache, url_path, name, sha):
    """A published file in the cache, as its manifest's sha256 says it is:
    fetched only when the cache does not hold it already."""
    path = os.path.join(cache, name)
    if not (os.path.exists(path) and sha256(open(path, "rb").read()) == sha):
        data = get(ROOT + url_path)
        if sha256(data) != sha:
            sys.exit(f"{url_path} does not match its manifest's sha256")
        with open(path, "wb") as f:
            f.write(data)
    return path


def chip_check(parts):
    """Why these must not go on a second chip, or None; and what they are."""
    b = open(parts["bootloader"], "rb").read()
    # The image's header and its first segment's, then esp_bootloader_desc_t:
    # magic 0x50 at 32, its version (CONFIG_BOOTLOADER_PROJECT_VER) at 36.
    if len(b) < 40 or b[0] != 0xE9 or struct.unpack_from("<H", b, 12)[0] != 0:
        return "its bootloader is not an ESP32's", None
    boot = struct.unpack_from("<I", b, 36)[0] if b[32] == 0x50 else 0
    if boot < 2:
        return f"its bootloader is version {boot}: it cannot go back to a firmware before", None
    t = open(parts["table"], "rb").read()
    table = {}
    for i in range(0, len(t) - 31, 32):
        if t[i:i + 2] != b"\xaa\x50":
            break
        table[t[i + 12:i + 28].split(b"\0")[0].decode(errors="replace")] = struct.unpack_from("<II", t, i + 4)
    for name, (off, size) in CHIP_TABLE.items():
        if table.get(name) != (off, size):
            return f"its partition table does not have {name} at {off:#x}, {size:#x} bytes", None
    o = open(parts["otadata"], "rb").read()
    if len(o) != CHIP_TABLE["otadata"][1] or o.count(0xFF) != len(o):
        return "its otadata is not an empty one", None
    a = open(parts["image"], "rb").read()
    n = len(a)
    # esp_app_desc_t at 32: version at 48, project at 80, app_elf_sha256 at
    # 176. A signed image ends in its 4 kB signature sector, magic 0xE7.
    if (n < 2 * 4096 or n % 4096 or n > CHIP_TABLE["ota_0"][1] or a[0] != 0xE9
            or struct.unpack_from("<H", a, 12)[0] != 0 or a[32:36] != bytes.fromhex("3254cdab")
            or a[80:112].split(b"\0")[0] != b"vfo-knob-companion"):
        return "its firmware is not the second chip's", None
    if a[n - 4096] != 0xE7:
        return "its firmware is not signed", None
    return None, {"bootloader_version": boot, "version": a[48:80].split(b"\0")[0].decode(),
                  "app_sha256": a[176:208].hex()}


def chip(cache, out_path, build=None):
    if build:
        parts = {"bootloader": os.path.join(build, "bootloader", "bootloader.bin"),
                 "table": os.path.join(build, "partition_table", "partition-table.bin"),
                 "otadata": os.path.join(build, "ota_data_initial.bin"),
                 "image": os.path.join(build, "vfo-knob-companion.bin")}
        for p in parts.values():
            if not os.path.exists(p):
                sys.exit(f"{p} is missing: build it first (idf.py -C companion -B {build} build)")
        source = f"this tree's {build}"
        m = None
        # A release (the release overlay's mark) is what the knob keeps up to
        # date by itself; a development build it leaves alone.
        try:
            release = "CONFIG_VFO_COMPANION_RELEASE=y" in open(os.path.join(build, "sdkconfig")).read().split("\n")
        except OSError:
            release = False
    else:
        os.makedirs(cache, exist_ok=True)
        if not json.loads(get(ROOT + "index.json")).get("companion"):
            print("No second-chip firmware is published yet.")
            sys.exit(NOT_YET)
        m = json.loads(get(ROOT + "companion/manifest.json"))
        try:
            bench = json.loads(get(ROOT + "companion/bench/manifest.json"))
        except urllib.error.HTTPError as e:
            sys.exit(f"The second chip's bench files are not published (firmware/companion/bench/): {e}")
        parts = {"image": cached(cache, f"companion/{m['file']}", m["file"], m["sha256"])}
        for key, name in (("bootloader", "bootloader.bin"), ("table", "partition-table.bin"),
                          ("otadata", "ota_data_initial.bin")):
            f = bench["files"][name]
            if int(f["offset"], 16) != CHIP_PARTS[name]:
                sys.exit(f"The bench's {name} goes at {f['offset']}, not at {CHIP_PARTS[name]:#x}: "
                         "this tool predates that change")
            # One set for every release: fixed names in the cache.
            parts[key] = cached(cache, f"companion/bench/{name}", f"companion-bench-{name}", f["sha256"])
        source = f"release {m['version']}"
        release = True
    why, info = chip_check(parts)
    if why:
        sys.exit(f"Not for a second chip: {why} ({source}).")
    # The image must be the one its manifest names: the same version, the
    # same identity, as the knob checks it later.
    if m and (info["version"].lstrip("v") != m["version"].lstrip("v")
              or info["app_sha256"] != m.get("app_sha256", "").lower()):
        sys.exit(f"The second chip's firmware says it is {info['version']} [{info['app_sha256'][:16]}], "
                 f"not what its manifest says ({m['version']})")
    with open(out_path, "w") as f:
        json.dump({**parts, "source": source, "release": release, **info}, f, indent=1)
    print(f"Second chip: {info['version']} [{info['app_sha256'][:16]}], {source}"
          f"{'' if release else ', a development build'}; bootloader version {info['bootloader_version']}")


if __name__ == "__main__":
    if len(sys.argv) == 4 and sys.argv[1] == "fetch":
        fetch(sys.argv[2], sys.argv[3])
    elif len(sys.argv) == 4 and sys.argv[1] == "push":
        push(sys.argv[2], sys.argv[3])
    elif len(sys.argv) in (4, 5) and sys.argv[1] == "chip":
        chip(sys.argv[2], sys.argv[3], sys.argv[4] if len(sys.argv) == 5 else None)
    else:
        sys.exit(__doc__)
