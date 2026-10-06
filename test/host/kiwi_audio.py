#!/usr/bin/env python3
"""The kiwi firmware's audio on the PC: components/kiwi_client on the session
in components/kiwi_proto (kiwi_host, as the firmware compiles them), its ring
played by the PC's clock as audio_out.c plays it (test/host/shim), against
tools/mock_kiwi.py -- never against someone's receiver.

    python3 test/host/kiwi_audio.py --host build_host/kiwi_host --mock tools/mock_kiwi.py
    python3 test/host/kiwi_audio.py ... --only drift --minutes 60         # T2's hour
    python3 test/host/kiwi_audio.py ... --only bursts --before OLD_HOST   # the same, then and now

  bursts   the stream held up 0.3, 1, 2 and 3 s, each backlog then at once,
           the frames coming a little unevenly (TerraBooster, 2026-10-03): no
           feed let go, at most one jump to the live point a stall, and that
           in the stall's own silence -- one break a stall. With --before, the
           same against an older kiwi_host, side by side.
  quick    a 3 s stall whose backlog comes 300 % faster than the stream (a
           connection catching up): one jump, in its silence; and 100 %
           faster, right at the knob's limit (twice the pace): measured
  short    a 0.4 s stall, its backlog under the ring's 80 %: left out all the
           same, the ring back at its target at once, not minutes later
  catch_up an 8 s stall whose backlog the network hands over 8 % faster than
           the stream, for 100 s: measured, side by side with --before
  repeated a 0.4 s stall every 8 s, and 0.6 s every 5 s: measured, as one
           break a stall, side by side with --before
  adaptive a 0.4 s stall every 8 s for two minutes: the target grows, kept
           fuller only while the network breaks the stream up, and the breaks
           stop -- counted, side by side with --before
  calm     two and a half minutes of an even stream: the target stays at
           256 ms, not one break
  eases    stalls for a minute, then calm: the target grows, and three
           minutes after the last break comes back down, the ring after it
           through the trim, never a cut -- a hiccup meanwhile ridden out
  ease_stall  a stall the ring cannot ride out while it comes down: one
           jump, in its silence, down to the target -- no second cut
  bunches  frames handed over two and three at a time, steadily: no stall
           seen in them -- three at a time cost one frame, once, as the
           knob learns how they come
  drift    the receiver's clock 200 ppm fast, and 200 slow: the ring stays
           where it is, the trim follows, nothing dropped, no underrun
           (--minutes: 5, or 60 for T2's hour)
  report   the log's line every 30 s, every field in it
  rej      the receiver's three big settings at every login are no lost
           audio: rej= stays 0
  saves    the dial's settings to flash at a quiet moment: not while it plays
           (30 s after the change), at once while nothing does, at a switch
"""
import argparse
import os
import re
import sys
import tempfile
import threading
import time

sys.dont_write_bytecode = True        # nothing written beside the tests
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kiwi_limits as kl              # noqa: E402  the mock, the boots, the checks
import kiwi_client_limits as kcl      # noqa: E402  kiwi_host's @STATE

JUMP = re.compile(r"(\d+) ms left out in one jump")
# The report; before the target could grow (an older kiwi_host, --before) it
# said neither the target nor the breaks.
REPORT = re.compile(r"kiwi: ring (\d+) ms(?: of (\d+))?, trim (\S+), frames (\d+), dropped (\d+), "
                    r"underruns (\d+), (?:breaks (\d+), )?jumps (\d+) \((\d+) ms left out\); gap (\d+) ms, "
                    r"seq lost (\d+), held (\d+)")
AUDIO = re.compile(r"@AUDIO level=(\d+) dropped=(\d+) underruns=(\d+) feeds=(\d+)")
GREW = re.compile(r"kiwi: the stream breaks up again and again: the ring kept at (\d+) ms")
EASED = re.compile(r"kiwi: calm a while: the ring kept at (\d+) ms")
ARGS = None


def reports(b):
    """Each 30 s report: dicts of its fields."""
    out = []
    for line in b.lines:
        m = REPORT.search(line)
        if m:
            out.append(dict(ring=int(m[1]), target=int(m[2] or 256), trim=float(m[3]), frames=int(m[4]),
                            dropped=int(m[5]), underruns=int(m[6]), breaks=int(m[7] or 0), jumps=int(m[8]),
                            left=int(m[9]), gap=int(m[10]), lost=int(m[11]), held=int(m[12])))
    return out


def audios(b):
    return [dict(level=int(m[1]), dropped=int(m[2]), underruns=int(m[3]), feeds=int(m[4]))
            for m in (AUDIO.match(l) for l in b.at) if m]


def knob(c, name="knob", host=None):
    k = kcl.Knob(c, name)
    if host:
        k._args = lambda cmds, h=host, n=k.nvs: [h, n, *map(str, cmds)]
    return k


# ------------------------------------------------------------------ scenarios

STALLS = ((20, 0.3), (45, 1.0), (70, 2.0), (95, 3.0))


def burst_run(c, host, name):
    """One knob through the stalls: what its ring did."""
    flags = [x for t, d in STALLS for x in ("--stall", f"{t}:{d}")]
    m = c.mock("--jitter", "25", *flags)
    k = knob(c, name, host)
    b = k.boot("save", f"127.0.0.1:{m.port}", "until", "streaming", "10", "audio", "wait", "110", "audio",
               "wait", "50", "audio", "state", "off", timeout=240)
    a = audios(b)
    jumps = [line for line in b.lines if JUMP.search(line)]
    st = m.stats()
    return dict(audio=a, jumps=jumps, stalls=st["stalls"], held=st["held"], state=b.state(), reports=reports(b))


def bursts(c):
    """Stalls of 0.3, 1, 2 and 3 s, each backlog at once, the frames up to 25
    ms uneven: no feed let go, one jump a stall at most, and none of the
    ring's sitting full for minutes after."""
    runs = [("now", ARGS.host)] + ([("before", ARGS.before)] if ARGS.before else [])
    res = {}
    threads = [threading.Thread(target=lambda n=n, h=h: res.__setitem__(n, burst_run(c, h, n))) for n, h in runs]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    for n, _ in runs:
        r = res.get(n)
        if not r or len(r["audio"]) < 3:
            c.check(False, f"{n}: no audio counts ({r})")
            continue
        a0, a1, a2 = r["audio"][:3]
        line = (f"{n:6s}: {r['stalls']} stalls, {r['held']} frames held up; feeds let go {a2['dropped'] - a0['dropped']}"
                f" ({a2['dropped'] - a1['dropped']} in the 50 s after the last stall), underruns "
                f"{a2['underruns'] - a0['underruns']}, jumps {len(r['jumps'])}")
        c.note(line)
        for j in r["jumps"]:
            c.note("        " + j.split("kiwi: ", 1)[-1])
        if n == "now":
            c.check(r["stalls"] == len(STALLS), f"the mock held the stream up {len(STALLS)} times ({r['stalls']})")
            c.check(a2["dropped"] == a0["dropped"], f"no feed let go ({a0['dropped']} -> {a2['dropped']})")
            c.check(1 <= len(r["jumps"]) <= len(STALLS), f"one jump a stall at most ({len(r['jumps'])})")
            c.check(a2["underruns"] - a0["underruns"] <= len(STALLS), f"one underrun a stall at most ({a0} {a2})")
            c.check(all("in its silence" in j for j in r["jumps"]),
                    f"each jump where the stall had silenced it already: one break a stall ({r['jumps']})")
            c.check(r["state"].get("state") == "streaming", f"it plays on ({r['state']})")


def catch_up_run(c, host, name):
    """One knob through an 8 s stall whose backlog the network then hands
    over 8 % faster than the stream, for some 100 s."""
    m = c.mock("--stall", "15:8", "--catch-up", "8", "--jitter", "10")
    k = knob(c, name, host)
    b = k.boot("save", f"127.0.0.1:{m.port}", "until", "streaming", "10", "audio", "wait", "140", "audio", "state",
               "off", timeout=200)
    return dict(audio=audios(b), jumps=[line for line in b.lines if JUMP.search(line)],
                state=b.state(), log=[line for line in m.log if "caught up" in line or "catch up" in line])


def catch_up(c):
    """An 8 s stall, its backlog then handed over 8 % faster than the stream
    until it has caught up, ~100 s: the ring cannot have it all at once.
    Measured, not judged: what each knob cuts, and how often."""
    runs = [("now", ARGS.host)] + ([("before", ARGS.before)] if ARGS.before else [])
    res = {}
    threads = [threading.Thread(target=lambda n=n, h=h: res.__setitem__(n, catch_up_run(c, h, n))) for n, h in runs]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    for n, _ in runs:
        r = res.get(n)
        if not r or len(r["audio"]) < 2:
            c.check(False, f"{n}: no audio counts ({r})")
            continue
        a0, a1 = r["audio"][:2]
        left = [int(JUMP.search(j)[1]) for j in r["jumps"]]
        c.note(f"{n:6s}: feeds let go {a1['dropped'] - a0['dropped']}, underruns {a1['underruns'] - a0['underruns']}, "
               f"jumps {len(left)}" + (f" ({min(left)}..{max(left)} ms each, {sum(left)} ms in all)" if left else ""))
        for line in r["log"]:
            c.note("        mock: " + line.split("] ", 1)[-1])
        if n == "now":
            c.check(a1["dropped"] == a0["dropped"], f"no feed let go ({a0} {a1})")
            c.check(r["state"].get("state") == "streaming", f"it plays on ({r['state']})")


def quick(c):
    """A 3 s stall whose backlog comes 300 % faster than the stream -- a
    connection catching up -- left out in one jump, in its silence; at 100 %,
    right at the knob's limit of twice the pace, measured."""
    res = {}

    def one(pct):
        m = c.mock("--stall", "12:3", "--catch-up", str(pct), "--jitter", "10")
        k = knob(c, f"cu{pct}")
        b = k.boot("save", f"127.0.0.1:{m.port}", "until", "streaming", "10", "audio", "wait", "30", "audio",
                   "state", "off", timeout=120)
        res[pct] = (audios(b), [line for line in b.lines if JUMP.search(line)], b.state(), reports(b))

    threads = [threading.Thread(target=one, args=(p,)) for p in (300, 100)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    for pct in (300, 100):
        a, jumps, st, reps = res[pct]
        if len(a) < 2:
            c.check(False, f"+{pct} %: no audio counts ({a})")
            continue
        left = [int(JUMP.search(j)[1]) for j in jumps]
        c.note(f"+{pct:3d} %: feeds let go {a[1]['dropped'] - a[0]['dropped']}, underruns "
               f"{a[1]['underruns'] - a[0]['underruns']}, jumps {len(left)} {left}"
               + (", the first in its silence" if jumps and "in its silence" in jumps[0] else ""))
        c.check(a[1]["dropped"] == a[0]["dropped"], f"+{pct} %: no feed let go ({a})")
        c.check(st.get("state") == "streaming", f"+{pct} %: it plays on ({st})")
        if pct == 300:
            c.check(len(jumps) == 1 and "in its silence" in jumps[0], f"+300 %: one jump, in its silence ({jumps})")
            c.check(a[1]["underruns"] - a[0]["underruns"] <= 1, f"+300 %: the stall's one underrun ({a})")


def short(c):
    """A 0.4 s stall, its backlog well under the ring's 80 %: left out all the
    same, in the stall's silence, and the ring back at its target at once --
    not 150 ms over it for minutes, the trim taking it down."""
    m = c.mock("--stall", "40:0.4", "--jitter", "25")
    k = knob(c)
    b = k.boot("save", f"127.0.0.1:{m.port}", "until", "streaming", "10", "audio", "wait", "92", "audio",
               "state", "off", timeout=150)
    reps = reports(b)
    jumps = [line for line in b.lines if JUMP.search(line)]
    a = audios(b)
    for r in reps:
        c.note(f"ring {r['ring']} ms, trim {r['trim']:.5f}, underruns {r['underruns']}, jumps {r['jumps']} "
               f"({r['left']} ms)")
    c.check(len(jumps) == 1 and "in its silence" in jumps[0], f"one jump, in its silence ({jumps})")
    c.check(len(a) == 2 and a[1]["dropped"] == a[0]["dropped"], f"no feed let go ({a})")
    c.check(len(reps) == 3 and all(r["ring"] <= 300 for r in reps[1:]),
            f"the ring at its target after the stall ({[r['ring'] for r in reps]})")


def repeated_run(c, host, name, every, dur):
    m = c.mock("--jitter", "25", "--stall-every", str(every), "--stall-for", str(dur))
    k = knob(c, name, host)
    b = k.boot("save", f"127.0.0.1:{m.port}", "until", "streaming", "10", "audio", "wait", "120", "audio", "state",
               "off", timeout=200)
    return dict(audio=audios(b), jumps=[line for line in b.lines if JUMP.search(line)], state=b.state(),
                stalls=m.stats()["stalls"], reports=reports(b))


def repeated(c):
    """Short stalls again and again -- 0.4 s every 8 s, 0.6 s every 5 s:
    measured. Each one the ring cannot ride out is one break, the stall's own
    silence, the audio back at the live point; a ring that stayed full (the
    old one) rode more of them out, a second and more behind."""
    runs = [("now", ARGS.host)] + ([("before", ARGS.before)] if ARGS.before else [])
    res = {}
    threads = []
    for every, dur in ((8, 0.4), (5, 0.6)):
        for n, h in runs:
            key = (n, every, dur)
            threads.append(threading.Thread(
                target=lambda key=key, h=h: res.__setitem__(key, repeated_run(c, h, f"{key[0]}{key[1]}", key[1],
                                                                              key[2]))))
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    for every, dur in ((8, 0.4), (5, 0.6)):
        for n, _ in runs:
            r = res.get((n, every, dur))
            if not r or len(r["audio"]) < 2:
                c.check(False, f"{n}: no audio counts ({r})")
                continue
            a0, a1 = r["audio"][:2]
            silent = sum("in its silence" in j for j in r["jumps"])
            rings = [x["ring"] for x in r["reports"]]
            c.note(f"{n:6s} {dur} s every {every} s: {r['stalls']} stalls; underruns {a1['underruns'] - a0['underruns']}, "
                   f"jumps {len(r['jumps'])} ({silent} in a stall's silence), feeds let go "
                   f"{a1['dropped'] - a0['dropped']}; the ring {rings} ms")
            if n == "now":
                c.check(a1["dropped"] == a0["dropped"], f"{dur} s every {every} s: no feed let go ({a0} {a1})")
                c.check(silent == len(r["jumps"]), f"{dur} s every {every} s: each jump in its silence ({r['jumps']})")
                c.check(r["state"].get("state") == "streaming", f"it plays on ({r['state']})")


def breaks_of(r):
    """What was heard as a break: each time the ring ran dry, and each jump
    with sound still in the ring -- a cut."""
    a0, a1 = r["audio"][:2]
    cuts = sum("in its silence" not in j for j in r["jumps"])
    return a1["underruns"] - a0["underruns"] + cuts


def adaptive(c):
    """A 0.4 s stall every 8 s, for two minutes (the stream a little uneven
    too): the target grows -- the ring kept fuller while the network breaks
    the stream up -- and the breaks stop. Counted, and with --before side by
    side with the knob before the target could grow."""
    runs = [("now", ARGS.host)] + ([("before", ARGS.before)] if ARGS.before else [])
    res = {}
    threads = [threading.Thread(target=lambda n=n, h=h: res.__setitem__(n, repeated_run(c, h, f"ad{n}", 8, 0.4)))
               for n, h in runs]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    for n, _ in runs:
        r = res.get(n)
        if not r or len(r["audio"]) < 2:
            c.check(False, f"{n}: no audio counts ({r})")
            continue
        a0, a1 = r["audio"][:2]
        reps = r["reports"]
        c.note(f"{n:6s}: {r['stalls']} stalls in 2 min; breaks {breaks_of(r)} (underruns "
               f"{a1['underruns'] - a0['underruns']}, jumps {len(r['jumps'])}); the ring and its target, each 30 s: "
               f"{[(x['ring'], x['target']) for x in reps]} ms")
        if n == "now":
            c.check(a1["dropped"] == a0["dropped"], f"no feed let go ({a0} {a1})")
            c.check(len(reps) >= 3 and reps[-1]["target"] > 256, f"the target grown ({reps})")
            c.check(breaks_of(r) <= 3, f"three breaks at most, the first stalls' ({breaks_of(r)})")
            c.check(len(reps) >= 3 and all(x["breaks"] == 0 and x["underruns"] == 0 for x in reps[1:]),
                    f"none after the first 30 s ({reps})")
            c.check(r["state"].get("state") == "streaming", f"it plays on ({r['state']})")
    if "before" in res and res["before"].get("audio") and res.get("now", {}).get("audio"):
        c.check(breaks_of(res["now"]) < breaks_of(res["before"]),
                f"fewer breaks than before ({breaks_of(res['now'])} against {breaks_of(res['before'])})")


def calm(c):
    """Two and a half minutes of an even stream, a little jitter: the target
    stays at 256 ms, not one break, nothing said of it."""
    m = c.mock("--jitter", "25")
    k = knob(c)
    b = k.boot("save", f"127.0.0.1:{m.port}", "until", "streaming", "10", "audio", "wait", "152", "audio", "state",
               "off", timeout=220)
    reps, a = reports(b), audios(b)
    c.note(f"the ring and its target, each 30 s: {[(x['ring'], x['target']) for x in reps]} ms")
    c.check(len(reps) >= 5 and all(x["target"] == 256 for x in reps), f"256 ms throughout ({reps})")
    c.check(all(x["breaks"] == 0 and x["underruns"] == 0 and x["jumps"] == 0 for x in reps), f"no break ({reps})")
    c.check(not [line for line in b.lines if GREW.search(line)], "the target never grew")
    c.check(len(a) == 2 and a[1]["underruns"] == a[0]["underruns"] and a[1]["dropped"] == a[0]["dropped"],
            f"no underrun, no feed let go ({a})")


def eases(c):
    """0.4 s stalls every 8 s for a minute, then an even stream: the target
    grows, and three minutes after the last break eases back down, half a
    frame each 30 s, to 256 ms -- the ring brought after it by the trim,
    never a cut: a hiccup while it comes down, a frame held up one frame's
    time (at 290 and 320 s, the ring well over the eased target), is ridden
    out as a ring kept at 256 ms rides it out."""
    stalls = [x for t in range(8, 64, 8) for x in ("--stall", f"{t}:0.4")]
    m = c.mock("--jitter", "25", *stalls, "--hold", "290", "--hold", "320")
    k = knob(c)
    b = k.boot("save", f"127.0.0.1:{m.port}", "until", "streaming", "10", "audio", "wait", "70", "audio",
               "wait", "365", "audio", "state", "off", timeout=520)
    reps, a = reports(b), audios(b)
    grew = [int(x[1]) for x in map(GREW.search, b.lines) if x]
    eased = [int(x[1]) for x in map(EASED.search, b.lines) if x]
    c.note(f"grew to {grew}, eased to {eased}; the ring and its target, each 30 s: "
           f"{[(x['ring'], x['target']) for x in reps]} ms")
    c.note(f"the longest gap between two frames, each 30 s: {[x['gap'] for x in reps]} ms")
    c.check(m.stats()["stalls"] == 9, f"the stream held up 9 times, the last two while it eased "
                                      f"({m.stats()['stalls']})")
    c.check(bool(grew), "the target grew while the stream broke up")
    c.check(bool(eased) and eased[-1] == 256 and eased == sorted(eased, reverse=True),
            f"...and eased back down to 256 ms, a step at a time ({eased})")
    late = reps[3:]
    c.check(late and all(x["jumps"] == 0 and x["underruns"] == 0 and x["breaks"] == 0 for x in late),
            f"never a cut nor a break once calm ({late})")
    c.check(len(a) == 3 and a[2]["underruns"] == a[1]["underruns"] and a[2]["dropped"] == a[0]["dropped"],
            f"no underrun once calm, no feed let go ({a})")
    rings = [x["ring"] for x in reps[6:]]
    c.check(rings and all(b <= a + 15 for a, b in zip(rings, rings[1:])),
            f"the ring coming down after the target, as the trim brings it ({rings})")
    c.check(reps and reps[-1]["target"] == 256 and reps[-1]["ring"] <= 330,
            f"the ring back near 256 ms ({reps[-1:] if reps else reps})")
    c.check(b.state().get("state") == "streaming", f"it plays on ({b.state()})")


AT = re.compile(r"\(\s*(\d+\.\d+)\) kiwi: ")


def ease_stall(c):
    """A stall the ring cannot ride out while it comes down after an eased
    target (1 s at 300 s, the ring ~150 ms over its target): one break, its
    backlog left out in one jump, in its silence, down to the target -- and
    no second cut after the refill, the frames after it judged by the same."""
    stalls = [x for t in list(range(8, 64, 8)) + [300] for x in ("--stall", f"{t}:{1.0 if t == 300 else 0.4}")]
    m = c.mock("--jitter", "25", *stalls)
    k = knob(c, "easestall")
    b = k.boot("save", f"127.0.0.1:{m.port}", "until", "streaming", "10", "wait", "335", "audio", "state", "off",
               timeout=420)
    at = lambda line: float(AT.search(line)[1]) if AT.search(line) else 0.0   # noqa: E731
    eased = [(at(l), int(x[1])) for l, x in ((l, EASED.search(l)) for l in b.lines) if x]
    jumps = [(at(l), l.split("kiwi: ", 1)[-1]) for l in b.lines if JUMP.search(l)]
    late = [j for j in jumps if j[0] > 280]
    c.note(f"eased {eased}; jumps after 280 s: {late}")
    c.check(any(t < 300 and ms > 256 for t, ms in eased), f"easing before the stall ({eased})")
    c.check(len(late) == 1 and "in its silence" in late[0][1], f"one jump for the stall, in its silence ({late})")
    c.check(all("in its silence" in j for _, j in jumps if _ > 60), f"no cut but in a stall's silence ({jumps})")
    c.check(b.state().get("state") == "streaming", f"it plays on ({b.state()})")


def bunches(c):
    """Frames handed over two at a time, and three, steadily, a little
    unevenly: their usual lateness, no stall's. Two at a time: nothing left
    out. Three: the first bunch late enough to look like a stall costs one
    frame, and teaches the knob how its frames come -- none after it."""
    res = {}

    def one(n):
        m = c.mock("--bunch", str(n), "--jitter", "25")
        k = knob(c, f"bunch{n}")
        b = k.boot("save", f"127.0.0.1:{m.port}", "until", "streaming", "10", "audio", "wait", "60", "audio",
                   "state", "off", timeout=150)
        res[n] = (audios(b), [line for line in b.lines if JUMP.search(line)], b.state(), reports(b))

    threads = [threading.Thread(target=one, args=(n,)) for n in (2, 3)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    for n in (2, 3):
        a, jumps, st, reps = res[n]
        if len(a) < 2:
            c.check(False, f"{n} at a time: no audio counts ({a})")
            continue
        c.note(f"{n} at a time: underruns {a[1]['underruns'] - a[0]['underruns']}, feeds let go "
               f"{a[1]['dropped'] - a[0]['dropped']}, jumps {len(jumps)}; the ring {[r['ring'] for r in reps]} ms")
        if n == 2:
            c.check(not jumps, f"two at a time: nothing left out ({jumps})")
            c.check(a[1]["underruns"] == a[0]["underruns"], f"two at a time: no underrun ({a})")
        else:
            left = [int(JUMP.search(j)[1]) for j in jumps]
            c.check(len(left) <= 1 and sum(left) <= 171, f"three at a time: one frame left out at most ({jumps})")
            c.check(a[1]["underruns"] - a[0]["underruns"] <= 1, f"three at a time: an underrun at most ({a})")
        c.check(a[1]["dropped"] == a[0]["dropped"], f"{n} at a time: no feed let go ({a})")


def drift_run(c, ppm, minutes):
    m = c.mock("--drift-ppm", str(ppm))
    k = knob(c, f"ppm{ppm}")
    b = k.boot("save", f"127.0.0.1:{m.port}", "until", "streaming", "10", "audio", "wait", str(minutes * 60),
               "audio", "state", "off", timeout=minutes * 60 + 120)
    return b, reports(b)


def drift(c):
    """The receiver's clock 200 ppm fast, and 200 ppm slow: the trim follows
    it, the ring stays steady, nothing dropped, no underrun, no jump."""
    minutes = ARGS.minutes
    res = {}
    threads = [threading.Thread(target=lambda p=p: res.__setitem__(p, drift_run(c, p, minutes))) for p in (200, -200)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    for ppm in (200, -200):
        b, reps = res[ppm]
        c.check(len(reps) >= minutes * 2 - 1, f"{ppm:+} ppm: a report every 30 s ({len(reps)} in {minutes} min)")
        if not reps:
            continue
        for r in reps:
            c.note(f"{ppm:+4d} ppm: ring {r['ring']} ms, trim {r['trim']:.5f}, frames {r['frames']}, "
                   f"dropped {r['dropped']}, underruns {r['underruns']}, jumps {r['jumps']}")
        bad = [r for r in reps if r["dropped"] or r["underruns"] or r["jumps"]]
        c.check(not bad, f"{ppm:+} ppm: nothing dropped, no underrun, no jump ({bad[:2]})")
        a = audios(b)
        c.check(len(a) == 2 and a[1]["dropped"] == a[0]["dropped"], f"{ppm:+} ppm: no feed let go ({a})")
        # Settled: the second half, or after 15 minutes of an hour.
        settled = reps[min(len(reps) // 2, 30):]
        rings = [r["ring"] for r in settled]
        trims = [r["trim"] for r in settled]
        want = 1 + ppm / 1e6
        if minutes >= 30:
            c.check(max(rings) - min(rings) <= 10, f"{ppm:+} ppm: the ring steady, settled ({min(rings)}..{max(rings)} ms)")
            c.check(all(abs(t - want) <= 0.00005 for t in trims),
                    f"{ppm:+} ppm: the trim on the drift, {want:.5f} ({min(trims):.5f}..{max(trims):.5f})")
        else:
            c.check(all(200 <= r <= 320 for r in rings), f"{ppm:+} ppm: the ring near 256 ms ({rings})")
            toward = (trims[-1] > 1.00002) if ppm > 0 else (trims[-1] < 0.99998)
            c.check(toward and abs(trims[-1] - 1) <= 0.0004,
                    f"{ppm:+} ppm: the trim on its way to {want:.5f} ({trims[-1]:.5f})")


def report(c):
    """Every 30 s, while it plays: the ring, the trim, the frames, what was
    dropped, the underruns, the jumps, the longest gap, the sequence."""
    m = c.mock("--stall", "40:1.5", "--stall-drop")
    k = knob(c)
    b = k.boot("save", f"127.0.0.1:{m.port}", "until", "streaming", "10", "wait", "92", "off", timeout=150)
    reps = reports(b)
    lines = [line for line in b.lines if REPORT.search(line)]
    c.check(len(reps) == 3, f"three reports in 92 s ({len(reps)})")
    for line in lines:
        c.note(line.split(") ", 1)[-1])
    if len(reps) >= 2:
        c.check(150 <= reps[0]["frames"] <= 180 and reps[0]["gap"] < 400, f"the first: 175 frames, no gap ({reps[0]})")
        lost = sum(r["lost"] for r in reps)
        c.check(6 <= lost <= 11, f"the 1.5 s the receiver let go: ~9 frames' numbers missing ({lost})")
        c.check(max(r["gap"] for r in reps) >= 1400, f"...the gap seen ({[r['gap'] for r in reps]})")
        c.check(all(r["held"] == 0 for r in reps), "none held: the frames were lost, not late")
        c.check(all(len(line.split(") ", 1)[-1]) < 256 for line in lines), "each under 256 characters: the log port's")
    m2 = c.mock("--stall", "40:1.5")
    k2 = knob(c, "held")
    b = k2.boot("save", f"127.0.0.1:{m2.port}", "until", "streaming", "10", "wait", "62", "off", timeout=120)
    reps = reports(b)
    held = sum(r["held"] for r in reps)
    c.check(6 <= held <= 11 and sum(r["lost"] for r in reps) == 0,
            f"the network held them: ~9 frames late, none missing ({[(r['held'], r['lost']) for r in reps]})")


def rej(c):
    """A receiver sends its settings at every login, three frames too big for
    the buffer: let go unread, and counted apart -- rej= says lost audio only."""
    runs = [("now", ARGS.host)] + ([("before", ARGS.before)] if ARGS.before else [])
    for n, h in runs:
        m = c.mock()
        k = knob(c, n, h)
        b = k.boot("save", f"127.0.0.1:{m.port}", "until", "streaming", "10", "wait", "8", "state", "off")
        line = next((x for x in reversed(b.at) if x.startswith("@STATE")), "")
        r = re.search(r"rej=(\d+) echo=(\d+)", line)
        got = (int(r[1]), int(r[2])) if r else (-1, -1)
        c.note(f"{n:6s}: rej={got[0]} after {got[1]} frames, {m.stats()['cfgs']} big settings frames at the login")
        if n == "now":
            c.check(m.stats()["cfgs"] == 3, f"the mock sent its three ({m.stats()['cfgs']})")
            c.check(got[0] == 0 and got[1] > 40, f"rej=0 ({line})")


def saves(c):
    """The dial's settings go to flash at a quiet moment: tuned while it
    plays, 30 s on -- not in the first seconds of audio; at once at a switch,
    with nothing playing."""
    a, bm = c.mock(), c.mock()
    k = knob(c)
    b = k.boot("save", f"127.0.0.1:{a.port}", f"127.0.0.1:{bm.port}", "until", "streaming", "10", "wait", "3",
               "nvs", "tune", "7150000", "wait", "25", "nvs", "wait", "8", "nvs",
               "tune", "7160000", "wait", "3", "use", "1", "wait", "1.5", "nvs", "off", timeout=90)
    w = [int(re.search(r"writes=(\d+)", x)[1]) for x in b.at if x.startswith("@NVS")]
    c.check(len(w) == 4, f"four looks ({w})")
    if len(w) == 4:
        c.check(w[1] == w[0], f"tuned while it plays: nothing written in 25 s ({w})")
        c.check(w[2] > w[1], f"...but 30 s on ({w})")
        c.check(w[3] > w[2], f"a switch: written at once, the receiver in use with it ({w})")


SCENARIOS = [bursts, quick, short, catch_up, repeated, adaptive, calm, eases, ease_stall, bunches, drift, report, rej,
             saves]


class Ctx(kl.Ctx):
    def __init__(self, name, tmp):
        super().__init__(name, tmp)
        self.notes = []

    def note(self, s):
        self.notes.append(s)


def main():
    global ARGS
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", required=True, help="the kiwi_host binary")
    ap.add_argument("--mock", required=True, help="tools/mock_kiwi.py")
    ap.add_argument("--before", help="an older kiwi_host, for bursts and rej side by side")
    ap.add_argument("--minutes", type=int, default=5, help="drift: how long (T2: 60)")
    ap.add_argument("--only", help="run the scenarios whose name has this in it")
    ap.add_argument("-v", "--verbose", action="store_true", help="print every log")
    ARGS = ap.parse_args()
    ARGS.host = os.path.abspath(ARGS.host)
    ARGS.mock = os.path.abspath(ARGS.mock)
    if ARGS.before:
        ARGS.before = os.path.abspath(ARGS.before)
    kl.ARGS = ARGS
    chosen = [s for s in SCENARIOS if not ARGS.only or ARGS.only in s.__name__]
    with tempfile.TemporaryDirectory(prefix="kiwi_audio_") as tmp:
        ctxs = [Ctx(s.__name__, tmp) for s in chosen]

        def run(s, c):
            try:
                s(c)
            except Exception as e:                          # a scenario that broke is a failure
                c.check(False, f"{type(e).__name__}: {e}")
            finally:
                for m in c.mocks:
                    m.stop()

        threads = [threading.Thread(target=run, args=(s, c)) for s, c in zip(chosen, ctxs)]
        t0 = time.time()
        for t in threads:
            t.start()
        for t in threads:
            t.join()
        bad = 0
        for s, c in zip(chosen, ctxs):
            print(f"{'FAIL' if c.fails else 'ok  '} {c.name}: {c.passes} checks passed"
                  + (f", {len(c.fails)} failed" if c.fails else ""))
            print("     " + (s.__doc__ or "").strip().splitlines()[0])
            for n in c.notes:
                print("     " + n)
            for f in c.fails:
                print(f"     FAILED: {f}")
            if c.fails:
                bad += 1
            if c.fails or ARGS.verbose:
                print("     --- the knob:")
                for line in c.log:
                    print("     " + line)
                for i, m in enumerate(c.mocks):
                    print(f"     --- mock {i}:")
                    for line in m.log:
                        print("     " + line)
        print(f"{len(chosen) - bad} of {len(chosen)} scenarios passed in {time.time() - t0:.0f} s")
        return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
