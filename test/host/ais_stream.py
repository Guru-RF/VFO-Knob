#!/usr/bin/env python3
"""AIS Radar's stream (ais_host: components/ais_client/ais_ws.c with
vfo_core ais.c, as the firmware builds them) against tools/mock_aisstream.py:
the upgrade with permessage-deflate or without, the server's context kept
or not, fragments, pings, a subscription changed halfway, an AIS-SART among
the ships -- and a key the stream refuses.

  ais_stream.py --host build/ais_host --mock tools/mock_aisstream.py [-v]

Loopback only: the mock listens on 127.0.0.1."""
import argparse
import os
import re
import subprocess
import sys

ARGS = None
FAILS = []


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what, flush=True)
    if not cond:
        FAILS.append(what)


class Mock:
    def __init__(self, *flags):
        self.p = subprocess.Popen([sys.executable, "-I", ARGS.mock, "--port", "0", *flags],
                                  stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        line = self.p.stdout.readline()
        m = re.search(r"(ws://127\.0\.0\.1:\d+/v0/stream)", line or "")
        if not m:
            raise RuntimeError("the mock did not start: " + (line or "nothing"))
        self.url = m.group(1)

    def stop(self):
        self.p.terminate()
        try:
            out, _ = self.p.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            self.p.kill()
            out, _ = self.p.communicate()
        return out.splitlines()


def host(url, seconds, *flags, key="test-key"):
    env = dict(os.environ, AIS_KEY=key)
    p = subprocess.run([ARGS.host, url, str(seconds), *flags], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                       text=True, timeout=seconds + 60, env=env)
    if ARGS.verbose:
        print("\n".join("    " + l for l in p.stdout.splitlines()))
    return p.returncode, p.stdout


def total(out):
    m = re.search(r"in all: (\d+) messages \((\d+) of ships, (\d+) other, (\d+) not read\), (\d+) ships, "
                  r"(\d+) with a position;.*?(\d+) dropped", out)
    return {k: int(v) for k, v in zip(("msgs", "ship_msgs", "other", "bad", "ships", "pos", "dropped"),
                                       m.groups())} if m else None


def case(name, mock_flags, host_flags=(), seconds=6, key="test-key"):
    print(name, flush=True)
    m = Mock(*mock_flags)
    try:
        rc, out = host(m.url, seconds, *host_flags, key=key)
    finally:
        log = m.stop()
    if ARGS.verbose:
        print("\n".join("    mock: " + l for l in log))
    return rc, out, log


def main():
    global ARGS
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", required=True)
    ap.add_argument("--mock", required=True)
    ap.add_argument("-v", "--verbose", action="store_true")
    ARGS = ap.parse_args()

    rc, out, log = case("deflate, pings, an AIS-SART, subscribed again halfway",
                        ("--ping", "1", "--sart", "--ships", "12"), ("--resub",), seconds=8)
    t = total(out)
    check(rc == 0, f"confirmed, ships with a position (exit {rc})")
    check("deflate" in out.split("\n", 1)[0], "the upgrade took permessage-deflate")
    check("confirmed: compression on" in out, "the confirmation says compression")
    check(bool(t) and t["bad"] == 0 and t["dropped"] == 0, "every message read, none dropped")
    check(bool(t) and t["ships"] >= 12 and t["pos"] >= 12, "every ship known, with its position")
    check(re.search(r"kinds:.* emergency 1", out) is not None, "the AIS-SART an emergency")
    check("subscribed again: sent" in out and any("subscribed again" in l for l in log),
          "the new box reached the stream")
    check(re.search(r"\(([\d.]+)x\)", out) is not None and float(re.search(r"\(([\d.]+)x\)", out).group(1)) > 1.2,
          "deflated on the wire")

    rc, out, _ = case("the stream uncompressed", ("--no-deflate",))
    t = total(out)
    check(rc == 0 and "uncompressed" in out.split("\n", 1)[0], f"no deflate offered back: plain frames (exit {rc})")
    check(bool(t) and t["bad"] == 0 and t["pos"] >= 8, "every ship read")

    rc, out, _ = case("deflate asked for not", (), ("--no-deflate",))
    check(rc == 0 and "uncompressed" in out.split("\n", 1)[0], f"the client's choice kept (exit {rc})")

    rc, out, log = case("the server's context kept, messages in fragments", ("--keep-context", "--fragment"))
    t = total(out)
    check(rc == 0 and "deflate" in out.split("\n", 1)[0], f"inflated across messages (exit {rc})")
    check(bool(t) and t["bad"] == 0 and t["dropped"] == 0 and t["pos"] >= 8, "every fragment joined, every ship read")

    rc, out, log = case("a key the stream refuses", (), seconds=4, key="not-the-key")
    check(rc == 2, f"said as the stream's error (exit {rc})")
    check("error: Api Key Is Not Valid" in out, "its words kept")
    check("not-the-key" not in out, "the key never printed")

    if FAILS:
        print(f"{len(FAILS)} failed")
        sys.exit(1)
    print("all passed")


if __name__ == "__main__":
    main()
