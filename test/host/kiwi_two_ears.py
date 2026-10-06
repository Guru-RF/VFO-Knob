#!/usr/bin/env python3
"""The kiwi firmware's two ears, end to end on the PC: its own receiver in the
left ear (components/kiwi_client) and a second one from the same list in the
right (components/sdr_rx, following the left ear's dial) -- kiwi_host, as the
firmware compiles them, against two or three tools/mock_kiwi.py receivers on
their own ports -- or tools/mock_ubersdr.py's Kiwi input (--uber, beside the
other by default). Never against someone's receiver.

    python3 test/host/kiwi_two_ears.py --host build_host/kiwi_host --mock tools/mock_kiwi.py

  both        two sessions at once, one a receiver: the right ear on the
              left ear's dial, mode and passband; each ear's 30 s report
  refused     each ear refuses the other's receiver, and no login comes of
              it; swapped through OFF, they swap
  list_save   a list saved under both: the left ear's own gone, it takes
              one the right ear has not -- and with none other, the right
              ear's, which lets go of it first: never two sessions on one
  at_boot     both ears come back after a restart
  marks       a day-limit mark is the receiver's, whichever ear met it: two
              refused logins at most between the two
  tests       the page's Test of the right ear's receiver, playing or on its
              way: "in use", no second login beside it
  status_once a receiver's /status read once a boot between the two ears
  adaptive    the right ear's ring kept fuller while its network keeps
              breaking the stream up, the left ear's as it was
  uber_input  each ear on an UberSDR's Kiwi input (tools/mock_ubersdr.py):
              UberSDR's own in the left, CW on the carrier; one acting a
              KiwiSDR whose owner set CW at 400..800 in the right -- the app
              path's ten-digit stamp, the tune, the AGC and the squelch
              again once the audio flows, CW where each centres it
  follows_left the right ear on the left ear's AGC, noise filter and
              squelch: each change in both receivers alike, paced alike
  out_of_range a KiwiSDR in the right ear, the dial on 6 m: quiet, "can't
              reach", its session kept -- no login, no tune at every turn,
              no break; back on 20 m, the tune and the audio at once
  quiet_both  a quiet moment for flash only with both ears silent a second:
              both squelches closed, or the right ear out of reach
  tls_quiet_save  a redirect to https:// followed by the right ear while the
              left ear plays: kept at once, in flash only once both are quiet
  tls_switch  the right ear moved on in the middle of a slow TLS handshake:
              let go of at once, the next receiver its own, the left ear's
              audio never touched

With --tls every receiver is behind TLS (as kiwi_limits.py has it), but an
UberSDR's Kiwi input, which has none; the scenarios about TLS itself run in
the clear run.

Each scenario has its own mocks and NVS; they run side by side. Exit 1 on any
failure, with the knob's and the mocks' logs.
"""
import argparse
import json
import os
import re
import socket
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request

sys.dont_write_bytecode = True        # nothing written beside the tests
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kiwi_limits as kl              # noqa: E402  the mock, the boots, the checks
import kiwi_client_limits as kcl      # noqa: E402  kiwi_host's @STATE

RSTATE = re.compile(r'@RSTATE want=(-?\d+) sel=(-?\d+) streaming=(\d) note="([^"]*)" state="([^"]*)" '
                    r'samples=(\d+) dbm=\S+ t=(\S+) range=(\d+)\.\.(\d+)')
REPORT = re.compile(r"(kiwi|sdr): ring (\d+) ms of (\d+), trim (\S+), frames (\d+), dropped (\d+), "
                    r"underruns (\d+), breaks (\d+), jumps (\d+)")


def rstates(b):
    return [dict(want=int(m[1]), sel=int(m[2]), streaming=m[3] == "1", note=m[4], state=m[5],
                 samples=int(m[6]), t=float(m[7]), range=(int(m[8]), int(m[9])))
            for m in (RSTATE.match(l) for l in b.at) if m]


def reports(b, tag):
    return [dict(ring=int(m[2]), target=int(m[3]), frames=int(m[5]), dropped=int(m[6]), underruns=int(m[7]),
                 breaks=int(m[8]), jumps=int(m[9]))
            for m in (REPORT.search(l) for l in b.lines) if m and m[1] == tag]


def said(b, what):
    """Each @USE / @RIGHT answer, as (n, ok)."""
    return [(int(m[1]), m[2] == "ok") for m in (re.match(f"@{what} (-?\\d+) (\\w+)", l) for l in b.at) if m]


def last_mod(m):
    mods = [t for _, t in kcl.sets_of(m) if t.startswith("SET mod=")]
    return mods[-1] if mods else ""


def rx(m):
    """Its address as the knob is given it: https:// behind TLS."""
    return m.rx


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class Uber:
    """tools/mock_ubersdr.py on free ports: an UberSDR on the LAN, its Kiwi
    input the receiver here (`port`). Its log and stop() as a Mock's. Its
    own port it picks itself; the Kiwi input's is found here (0 there says
    none), and found again should another take it before the mock does --
    which the mock says at once, ending before its LISTENING."""
    def __init__(self, *flags):
        for _ in range(5):
            self.port = free_port()
            self.p = subprocess.Popen([sys.executable, "-B", "-u", kl.ARGS.uber, "--port", "0",
                                       "--kiwi-port", str(self.port), *flags],
                                      stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            line = self.p.stdout.readline()
            if line.startswith("LISTENING"):
                break
            line += self.p.communicate()[0]
            if "Address already in use" not in line:
                raise RuntimeError(f"the UberSDR mock did not start: {line}")
        else:
            raise RuntimeError(f"the UberSDR mock found no free port: {line}")
        self.log = []
        threading.Thread(target=self._drain, daemon=True).start()

    def _drain(self):
        for line in self.p.stdout:
            self.log.append(line.rstrip())

    @property
    def rx(self):
        return f"127.0.0.1:{self.port}"             # its Kiwi input has no TLS

    def state(self):
        with urllib.request.urlopen(f"http://127.0.0.1:{self.port}/mock/state", timeout=5) as r:
            return json.loads(r.read())

    def stop(self):
        self.p.terminate()
        try:
            self.p.wait(5)
        except subprocess.TimeoutExpired:
            self.p.kill()


# ------------------------------------------------------------------ scenarios

def both(c):
    """Two sessions at once: the left ear on one receiver, the right on the
    other, following the left ear's dial, mode and passband -- each receiver
    one session, and each ear's report every 30 s."""
    a, b_ = c.mock(), c.mock("--antenna", "MOCK B")
    k = kcl.knob(c)
    b = k.boot("save", f"A={rx(a)}", f"B={rx(b_)}", "until", "streaming", "10", "right", "1",
               "runtil", "streaming", "15", "tune", "14200000", "filter", "300", "2400", "wait", "3",
               "state", "rstate", "wait", "30", "state", "rstate", "raudio", "audio", "off", timeout=90)
    s, r = b.state(), (rstates(b) or [{}])[-1]
    c.check(s.get("state") == "streaming" and s.get("rx") == 0, f"the left ear plays A ({s})")
    c.check(r.get("streaming") and r.get("sel") == 1, f"the right ear plays B ({r})")
    sa, sb = a.stats(), b_.stats()
    c.check(sa["logins"] == 1 and sb["logins"] == 1, f"one login each ({sa['logins']}, {sb['logins']})")
    c.check(sa["two_at_once"] == 0 and sb["two_at_once"] == 0,
            f"one session on each receiver at a time ({sa['two_at_once']}, {sb['two_at_once']})")
    ma, mb = last_mod(a), last_mod(b_)
    c.check(ma and ma == mb and "freq=14200.000" in mb and "mod=usb" in mb and "low_cut=300 high_cut=2400" in mb,
            f"the right ear on the left ear's dial, mode and passband ({ma!r} / {mb!r})")
    left, right = reports(b, "kiwi"), reports(b, "sdr")
    c.check(len(left) >= 1 and len(right) >= 1, f"each ear's 30 s report ({left}, {right})")
    c.check(all(x["target"] == 256 and not x["dropped"] and not x["underruns"] for x in left + right),
            f"both at 256 ms, nothing lost ({left}, {right})")
    ra = [l for l in b.at if l.startswith("@RAUDIO")]
    c.check(bool(ra) and "dropped=0 underruns=0" in ra[-1], f"the right ear's ring: nothing lost ({ra})")


def refused(c):
    """Each ear refuses the other's receiver, and nothing logs in for it;
    the right ear OFF, the left ear takes its receiver -- and the right ear
    then the left ear's old one: swapped, one session on each throughout."""
    a, b_ = c.mock(), c.mock()
    k = kcl.knob(c)
    b = k.boot("save", f"A={rx(a)}", f"B={rx(b_)}", "until", "streaming", "10", "right", "0", "wait", "3",
               "right", "1", "runtil", "streaming", "15", "use", "1", "wait", "3", "state",
               "right", "-1", "wait", "1", "use", "1", "until", "streaming", "15", "right", "0",
               "runtil", "streaming", "15", "wait", "2", "state", "rstate", "off", timeout=90)
    rights, uses = said(b, "RIGHT"), said(b, "USE")
    c.check(rights[:2] == [(0, False), (1, True)], f"the right ear refuses the left ear's ({rights})")
    c.check(uses[:1] == [(1, False)], f"the left ear refuses the right ear's ({uses})")
    c.check(uses[1:2] == [(1, True)] and rights[2:] == [(-1, True), (0, True)],
            f"through OFF they swap ({uses}, {rights})")
    s, r = b.state(), (rstates(b) or [{}])[-1]
    c.check(s.get("rx") == 1 and s.get("state") == "streaming" and r.get("sel") == 0 and r.get("streaming"),
            f"swapped: the left ear on B, the right on A ({s}, {r})")
    sa, sb = a.stats(), b_.stats()
    c.check(sa["two_at_once"] == 0 and sb["two_at_once"] == 0,
            f"never two sessions on one receiver ({sa['two_at_once']}, {sb['two_at_once']})")
    c.check(sa["logins"] == 2 and sb["logins"] == 2,
            f"a login for each ear's own, none for a refusal ({sa['logins']}, {sb['logins']})")


def list_save(c):
    """A list saved under both ears. The left ear's own gone from it, it
    takes the first the right ear has not; with no other left, the right
    ear's -- which the right ear lets go of, and waits out ("left ear")
    before the left ear logs in: never two sessions on one receiver."""
    a, b_, cc = c.mock(), c.mock(), c.mock()
    k = kcl.knob(c)
    b = k.boot("save", f"A={rx(a)}", f"B={rx(b_)}", "until", "streaming", "10", "right", "1",
               "runtil", "streaming", "15", "save", f"B={rx(b_)}", f"C={rx(cc)}", "until", "streaming", "15",
               "runtil", "streaming", "15", "wait", "2", "state", "rstate",
               "save", f"B={rx(b_)}", "wait", "4", "until", "streaming", "15", "runtil", "left ear", "15",
               "wait", "3", "state", "rstate", "off", timeout=120)
    sts, rs = [x for x in b.states() if x["tag"] == "STATE"], rstates(b)
    first = next((x for x in sts if x.get("server") == "C"), {})
    c.check(bool(first) and first.get("state") == "streaming",
            f"A gone: the left ear on C, the first the right ear has not ({sts[:3]})")
    c.check(any(x["streaming"] and x["sel"] == 0 for x in rs), f"the right ear kept on B, now first ({rs})")
    s, r = sts[-1] if sts else {}, rs[-1] if rs else {}
    c.check(s.get("server") == "B" and s.get("state") == "streaming",
            f"only B left: the left ear has it ({s})")
    c.check(r.get("note") == "left ear" and not r.get("streaming"), f"...the right ear waits it out ({r})")
    c.check(b_.stats()["two_at_once"] == 0, f"never two sessions on B ({b_.stats()['two_at_once']})")


def at_boot(c):
    """Both ears' receivers come back after a restart, each to its own."""
    a, b_ = c.mock(), c.mock()
    k = kcl.knob(c)
    k.boot("save", f"A={rx(a)}", f"B={rx(b_)}", "until", "streaming", "10", "right", "1",
           "runtil", "streaming", "15", "wait", "33", "quit", timeout=90)
    b = k.boot("until", "streaming", "15", "runtil", "streaming", "15", "state", "rstate", "off", timeout=60)
    s, r = b.state(), (rstates(b) or [{}])[-1]
    c.check(s.get("rx") == 0 and s.get("state") == "streaming", f"the left ear back on A ({s})")
    c.check(r.get("sel") == 1 and r.get("streaming"), f"the right ear back on B ({r})")
    c.check(a.stats()["two_at_once"] == 0 and b_.stats()["two_at_once"] == 0, "one session on each throughout")


def marks(c):
    """A day-limit mark is the receiver's, whichever ear met it: refused at
    the right ear's login, the left ear's choice of it is the one try left;
    refused again, the right ear's next choice logs in no more -- two
    refused logins in all."""
    a, d = c.mock(), c.mock("--ip-limit-at-login")
    k = kcl.knob(c)
    b = k.boot("save", f"A={rx(a)}", f"D={rx(d)}", "until", "streaming", "10", "right", "1",
               "runtil", "daily limit", "15", "right", "-1", "wait", "1", "use", "1", "until", "day limit", "15",
               "use", "0", "until", "streaming", "15", "right", "1", "wait", "4", "rstate", "marks", "quit",
               timeout=90)
    st = d.stats()
    c.check(st["refused_at_login"] == 2, f"two refused logins in all, one each ear ({st['refused_at_login']})")
    r = (rstates(b) or [{}])[-1]
    c.check(r.get("note") == "day limit" and not r.get("streaming"), f"the right ear holds it, no login ({r})")
    c.check(b.marks()[-1:] == [{1: 2}], f"its mark, two strikes ({b.marks()})")
    c.check(d.stats()["two_at_once"] == 0, "one session on D at a time")


def adaptive(c):
    """The right ear's network breaks the stream up, 0.4 s every 8 s: its
    ring is kept fuller, its breaks stop; the left ear's network is calm,
    its ring stays at 256 ms."""
    a, b_ = c.mock("--jitter", "25"), c.mock("--jitter", "25", "--stall-every", "8", "--stall-for", "0.4")
    k = kcl.knob(c)
    b = k.boot("save", f"A={rx(a)}", f"B={rx(b_)}", "until", "streaming", "10", "right", "1",
               "runtil", "streaming", "15", "raudio", "wait", "95", "raudio", "state", "off", timeout=150)
    left, right = reports(b, "kiwi"), reports(b, "sdr")
    c.note(f"left  {[(x['ring'], x['target'], x['breaks']) for x in left]}")
    c.note(f"right {[(x['ring'], x['target'], x['breaks']) for x in right]}")
    c.check(left and all(x["target"] == 256 and not x["breaks"] for x in left),
            f"the left ear: calm, 256 ms ({left})")
    c.check(right and right[-1]["target"] > 256, f"the right ear kept fuller ({right})")
    c.check(len(right) >= 3 and right[-1]["breaks"] == 0 and right[-1]["underruns"] == 0,
            f"...and its breaks stop ({right})")
    grew = [l for l in b.lines if " sdr: the stream breaks up again and again" in l]
    c.check(bool(grew), "the right ear says why")


def tests(c):
    """The page's Test of the right ear's receiver -- playing, and on its way
    (its login not answered yet): "in use", and no login of the Test's beside
    the right ear's own, as for the left ear's."""
    a, b_, d = c.mock(), c.mock(), c.mock("--answer-delay", "5")
    k = kcl.knob(c)
    b = k.boot("save", f"A={rx(a)}", f"B={rx(b_)}", f"D={rx(d)}", "until", "streaming", "10", "right", "1",
               "runtil", "streaming", "15", "test", "1", "right", "2", "runtil", "connecting", "5", "wait", "1.5",
               "test", "2", "runtil", "streaming", "10", "off", timeout=90)
    t = b.tests()
    c.check(len(t) == 2 and all(x.get("login") == "in use" for x in t),
            f"each Test: in use, from the right ear's session ({[x.get('login') for x in t]})")
    c.check(b_.stats()["upgrades"] == 1, f"B playing: no Test's login beside it ({b_.stats()['upgrades']} sessions)")
    c.check(d.stats()["upgrades"] == 1,
            f"D, its login on its way: no Test's login beside it ({d.stats()['upgrades']} sessions)")
    r = (rstates(b) or [{}])[-1]
    c.check(r.get("sel") == 2 and r.get("streaming"), f"the right ear plays D after all ({r})")


def status_once(c):
    """A receiver's /status is read once a boot between the two ears: the
    right ear reads B's, and the left ear, given B after it, takes that read
    -- its antenna on the slab from it -- with no second read."""
    a, b_ = c.mock(), c.mock("--antenna", "MOCK B")
    k = kcl.knob(c)
    b = k.boot("save", f"A={rx(a)}", f"B={rx(b_)}", "until", "streaming", "10", "right", "1",
               "runtil", "streaming", "15", "right", "-1", "wait", "1", "use", "1", "wait", "4", "state", "off",
               timeout=60)
    s = b.state()
    c.check(s.get("rx") == 1 and s.get("state") == "streaming", f"the left ear on B ({s})")
    c.check(b_.stats()["status"] == 1 and a.stats()["status"] == 1,
            f"one /status read a receiver ({a.stats()['status']}, {b_.stats()['status']})")
    c.check(any("its /status as read" in l for l in b.lines), "the left ear took the right ear's read")
    c.check(s.get("line2") == "MOCK B", f"B's antenna on the slab, from that read ({s.get('line2')})")


def tuned_in_audio(b, tag):
    """The tune again once the audio flowed, from the knob's own log: the
    first SET mod after the ear's "streaming from" the one it sent before."""
    lines = [l for l in b.lines if f" {tag}: " in l]
    at = next((i for i, l in enumerate(lines) if f" {tag}: streaming from" in l), None)
    if at is None:
        return False
    mod = lambda l: l.split(f" {tag}: ", 1)[1] if f" {tag}: SET mod=" in l else None
    before = [mod(l) for l in lines[:at] if mod(l)]
    after = next((mod(l) for l in lines[at:] if mod(l)), None)
    return bool(before) and after == before[-1]


def uber_input(c):
    """Each ear on an UberSDR's Kiwi input: UberSDR's own in the left, which
    makes its channel from the first SET mod with its own passband, letting
    an AGC or a squelch before it go by, and centres CW on the carrier; one
    acting a KiwiSDR whose owner set CW at 400..800 in the right. Both on
    the app path with ten digits in its stamp, which that input takes only
    so; each tunes again once its audio flows, the AGC and the squelch with
    it; and in CW each ear tunes the carrier where its receiver centres CW
    -- the dial on UberSDR's, 600 Hz below it on the other."""
    ua, ub = c.uber("--kiwi-flavour", "ubersdr"), c.uber("--kiwi-flavour", "kiwisdr", "--kiwi-cw", "400,800")
    k = kcl.knob(c)
    b = k.boot("save", f"U={rx(ua)}", f"K={rx(ub)}", "until", "streaming", "10", "right", "1",
               "runtil", "streaming", "15", "wait", "1", "mode", "cw", "filter", "-250", "250",
               "tune", "7030000", "wait", "2", "state", "rstate", "off", timeout=90, env={"SHIM_DEBUG": "1"})
    s, r = b.state(), (rstates(b) or [{}])[-1]
    c.check(s.get("state") == "streaming" and s.get("rx") == 0, f"the left ear plays U ({s})")
    c.check(r.get("streaming") and r.get("sel") == 1, f"the right ear plays K ({r})")
    sa, sb = ua.state(), ub.state()
    paths = sa["paths"] + sb["paths"]
    c.check(paths and all(re.fullmatch(r"/\d{10}/SND", p) for p in paths),
            f"the app path, ten digits in its stamp ({paths})")
    c.check(tuned_in_audio(b, "kiwi"), "the left ear: the tune again once the audio flowed")
    c.check(tuned_in_audio(b, "sdr"), "the right ear: the tune again once the audio flowed")
    c.check(sa["mods"] and not sa["mods"][0]["edges"] and sa["mods"][1:2] and sa["mods"][1]["edges"],
            f"U made the channel with its own passband, and took the knob's from the next ({sa['mods'][:2]})")
    # LSB's AGC (CW's has its own threshold) and the squelch: sent at the
    # login, let go by, and taken only once the audio flowed.
    c.check(sa["squelch"] and sa["agc"] and "thresh=-100" in sa["agc"][0],
            f"U's channel took the squelch and LSB's AGC once it was made ({sa['squelch'][:1]}, {sa['agc'][:1]})")
    ma, mb = (sa["mods"] or [{}])[-1], (sb["mods"] or [{}])[-1]
    c.check(ma.get("mode") == "cwu" and ma.get("freq") == 7030000 and (ma.get("lo"), ma.get("hi")) == (-250, 250)
            and ma.get("edges"), f"U, CW: the carrier on the dial, -250..250 around it ({ma})")
    c.check(mb.get("mode") == "cwu" and mb.get("freq") == 7029400 and (mb.get("lo"), mb.get("hi")) == (350, 850),
            f"K, CW: the carrier 600 Hz below the dial, 350..850 ({mb})")
    c.check(any(" kiwi: " in l and f":{ua.port} centres CW on 0 Hz" in l for l in b.lines),
            "the left ear read U's CW centre, 0 Hz")
    c.check(any(" sdr: " in l and f":{ub.port} centres CW on 600 Hz" in l for l in b.lines),
            "the right ear read K's CW centre, 600 Hz")


def dsp_sets(m):
    """The AGC, noise filter and squelch a receiver was sent, by its clock."""
    return [(t, x) for t, x in kcl.sets_of(m) if x.startswith(("SET agc=", "SET nr ", "SET squelch="))]


def follows_left(c):
    """The right ear plays with the left ear's AGC, noise filter and squelch,
    so one antenna is heard against another fairly: each change reaches the
    right ear's receiver as it reaches the left's, paced alike -- the noise
    filter turned through sent once it has rested half a second, its last
    choice only -- and back again."""
    a, b_ = c.mock(), c.mock("--antenna", "MOCK B")
    k = kcl.knob(c)
    b = k.boot("save", f"A={rx(a)}", f"B={rx(b_)}", "until", "streaming", "10", "right", "1",
               "runtil", "streaming", "15", "wait", "1.5",
               "agc", "fast", "nr", "1", "wait", "0.2", "nr", "2", "wait", "0.2", "nr", "3", "squelch", "30",
               "wait", "2", "agc", "slow", "squelch", "0", "nr", "0", "wait", "2", "state", "rstate", "off",
               timeout=90)
    s, r = b.state(), (rstates(b) or [{}])[-1]
    c.check(s.get("state") == "streaming" and r.get("streaming"), f"both ears play ({s}, {r})")
    da, db = dsp_sets(a), dsp_sets(b_)
    ta, tb = [x for _, x in da], [x for _, x in db]
    c.check(ta == tb, f"the right ear's receiver told what the left ear's was, in order ({ta} / {tb})")
    SPEC, OFF = "SET nr algo=3", "SET nr algo=0"
    for name, texts in (("left", ta), ("right", tb)):
        agc = [x for x in texts if x.startswith("SET agc=")]
        c.check(any("decay=250" in x for x in agc) and bool(agc) and "decay=3000" in agc[-1],
                f"the {name} ear: AGC FAST, then SLOW ({agc})")
        c.check(texts.count(SPEC) == 1 and "SET nr algo=1" not in texts and "SET nr algo=2" not in texts
                and texts[-1:] == [OFF], f"the {name} ear: SPEC once it rested, WDSP and LMS turned past, "
                                         f"then OFF ({texts})")
        sq = [x for x in texts if x.startswith("SET squelch=")]
        c.check(len(set(sq)) == 2 and sq[-1] == sq[0], f"the {name} ear: the squelch at 30 %, then open ({sq})")
    # Paced alike: from AGC FAST to SPEC, the noise filter turned through and
    # its half second at rest, each receiver by its own clock.
    def rest(d):
        t0 = next((t for t, x in d if "decay=250" in x), None)
        t1 = next((t for t, x in d if x == SPEC), None)
        return None if t0 is None or t1 is None else t1 - t0
    ra, rb = rest(da), rest(db)
    c.note(f"AGC FAST to SPEC: left {ra or 0:.3f} s, right {rb or 0:.3f} s")
    c.check(ra is not None and rb is not None and ra >= 0.8 and rb >= 0.8 and abs(ra - rb) < 0.25,
            f"the noise filter after its rest, in both alike ({ra}, {rb})")


def out_of_range(c):
    """A KiwiSDR in the right ear, the left ear's Web-888 on 6 m: the right
    ear goes quiet and says so -- out of range, "can't reach" for the dial,
    the 0-30 MHz it covers -- its session kept: no login again, no tune at
    every turn of the dial out there, its ring fed at the stream's pace, no
    break, its target as it was; back on 20 m, the tune and the audio at
    once. Said in the log once each way."""
    a, z = c.mock(), c.mock("--kiwisdr")
    k = kcl.knob(c)
    b = k.boot("save", f"A={rx(a)}", f"Z={rx(z)}", "until", "streaming", "10", "right", "1",
               "runtil", "streaming", "15", "tune", "14200000", "wait", "2", "rpitch", "1", "rstate",
               "tune", "50150000", "runtil", "out of range", "3", "rpitch", "2", "raudio",
               "tune", "50160000", "wait", "0.3", "tune", "50170000", "wait", "0.3", "mode", "cw", "wait", "0.3",
               "mode", "usb", "filter", "300", "2400", "wait", "0.3", "tune", "50313000",
               "wait", "31", "rstate", "raudio",
               "tune", "14200000", "runtil", "streaming", "2", "rpitch", "1", "wait", "1", "rstate", "raudio",
               "state", "off", timeout=150, env={"SHIM_DEBUG": "1"})
    rs = rstates(b)
    c.check(len(rs) == 6, f"each look at the right ear ({rs})")
    if len(rs) < 6:
        return
    before, out, still, back = rs[1], rs[2], rs[3], rs[4]
    c.check(before["streaming"] and before["state"] == "streaming", f"on 20 m it plays ({before})")
    for x in (out, still):
        c.check(not x["streaming"] and x["state"] == "out of range" and x["note"] == "can't reach",
                f"on 6 m: out of range, \"can't reach\" for the dial, no S-meter ({x})")
    c.check(out["range"] == (0, 30000000), f"...what it covers said: 0-30 MHz ({out['range']})")
    c.check(back["streaming"] and back["state"] == "streaming", f"back on 20 m it plays ({back})")
    c.check(back["t"] - still["t"] < 1.0, f"...within a second of the turn ({still['t']}, {back['t']})")
    p = [(float(m[1]), int(m[2])) for m in (re.match(r"@RPITCH hz=(\S+) samples=(\d+)", l) for l in b.at) if m]
    c.check(len(p) == 3 and abs(p[0][0] - 1000) <= 15, f"on 20 m: its tone ({p})")
    c.check(len(p) == 3 and p[1][0] == 0 and p[1][1] >= 0.9 * 2 * 24000,
            f"on 6 m: silence, fed at the stream's pace ({p[1:2]})")
    c.check(len(p) == 3 and abs(p[2][0] - 1000) <= 60, f"back: its tone again at once ({p[2:]})")
    st = z.stats()
    c.check(st["logins"] == 1 and st["upgrades"] == 1, f"its session kept: one login ({st['logins']})")
    mods = [x for _, x in kcl.sets_of(z) if x.startswith("SET mod=")]
    far = [x for x in mods if float(x.rsplit("freq=", 1)[1]) >= 30000 or "mod=cw" in x]
    c.check(not far, f"never tuned to 6 m, nor to its edge, nor to CW out there ({far})")
    cw_agc = [x for _, x in kcl.sets_of(z) if x.startswith("SET agc=") and "thresh=-130" in x]
    c.check(not cw_agc, f"...nor CW's AGC: the AGC keeps to the mode it has ({cw_agc})")
    c.check(bool(mods) and mods[-1] == "SET mod=usb low_cut=300 high_cut=2400 freq=14200.000",
            f"back: tuned to the dial, its new filter with it ({mods[-1:]})")
    # No tune at every turn: none at all from the knob while it was out.
    lines = b.lines
    outs = [i for i, l in enumerate(lines) if l.startswith("@RSTATE") and "out of range" in l]
    tunes = [l for l in lines[outs[0]:outs[-1]] if " sdr: SET mod=" in l] if outs else ["?"]
    c.check(not tunes, f"no tune while the dial turned out there ({tunes})")
    said_out = [l for l in lines if " sdr: " in l and "out of its range" in l]
    said_back = [l for l in lines if " sdr: " in l and "within its range again" in l]
    c.check(len(said_out) == 1 and "0..30000000 Hz" in said_out[0] and len(said_back) == 1,
            f"the log, once each way ({said_out}, {said_back})")
    right = reports(b, "sdr")
    c.note(f"right {[(x['ring'], x['target'], x['breaks'], x['underruns']) for x in right]}")
    c.check(bool(right) and all(x["breaks"] == 0 and x["underruns"] == 0 and x["target"] == 256 for x in right),
            f"no break, its target as it was ({right})")
    ra = [l for l in b.at if l.startswith("@RAUDIO")]
    c.check(len(ra) == 3 and all("dropped=0 underruns=0" in l for l in ra), f"its ring: nothing lost ({ra})")
    s = b.state()
    c.check(s.get("state") == "streaming" and s.get("f") == 14200000, f"the left ear as it was ({s})")


def quiet_both(c):
    """A quiet moment, for flash, is both ears silent: the left ear's squelch
    closed a second, and the right ear's too -- which it follows -- or its
    receiver out of reach of the dial a second, what it let through before
    played by then. The right ear's squelch open, or its receiver only just
    out of reach, is none."""
    a, z = c.mock("--snr", "10"), c.mock("--kiwisdr", "--snr", "30")
    k = kcl.knob(c)
    b = k.boot("save", f"A={rx(a)}", f"Z={rx(z)}", "until", "streaming", "10", "right", "1",
               "runtil", "streaming", "15", "tune", "14200000", "wait", "1",
               "squelch", "50", "wait", "2", "state",
               "tune", "50150000", "runtil", "out of range", "3", "state", "wait", "1.5", "state",
               "tune", "14200000", "runtil", "streaming", "3", "wait", "0.5", "state",
               "squelch", "100", "wait", "2.5", "state", "squelch", "0", "wait", "1", "state", "off", timeout=90)
    # The first `until` says the state as well: seven looks, six of them ours.
    s = [x for x in b.states() if x["tag"] == "STATE"][1:]
    c.check(len(s) == 6 and all("audible" in x for x in s), f"each look at it ({s})")
    if len(s) < 6 or not all("audible" in x for x in s):
        return
    au = [x["audible"] for x in s]
    c.check(au[0], f"the left ear's squelch closed, the right ear's open on its stronger signal: heard ({au})")
    c.check(au[1], f"the right ear just gone out of reach: its ring still playing, heard ({au})")
    c.check(not au[2], f"...a second on, silent: a quiet moment ({au})")
    c.check(au[3], f"back within its reach, its squelch open: heard ({au})")
    c.check(not au[4], f"both squelches closed a second: a quiet moment ({au})")
    c.check(au[5], f"both open again: heard ({au})")
    sq = [x for _, x in kcl.sets_of(z) if x.startswith("SET squelch=")]
    c.check("SET squelch=40 param=0.50" in sq, f"the right ear's receiver told the left ear's squelch ({sq})")


def tls_quiet_save(c):
    """A redirect to https:// followed by the right ear while the left ear
    plays: kept at once -- the right ear plays its receiver over TLS -- but in
    flash only once both ears are quiet, never while either plays."""
    a, r = c.mock(), c.mock("--redirect", "307", tls=True)
    k = kcl.knob(c)
    cmds = ("save", f"A={rx(a)}", f"R=http://127.0.0.1:{r.redirect_port}", "until", "streaming", "10", "right", "1",
            "runtil", "streaming", "15", "list", "wait", "5", "squelch", "100", "wait", "5", "list", "quit")
    p = k.start(*cmds)
    time.sleep(4)                       # both ears playing: the right ear's redirect followed and kept
    playing = kl.nvs_value(k.nvs, "vfo", "sdrs")
    b = k.finish(p, cmds)
    quiet = kl.nvs_value(k.nvs, "vfo", "sdrs")
    ls = kl.lists(b)
    c.check(ls[:1] == [{0: ("127.0.0.1", a.port, False, 0), 1: ("127.0.0.1", r.port, True, r.redirect_port)}],
            f"the right ear's https:// kept at once ({ls})")
    r_ = (rstates(b) or [{}])
    c.check(any(x.get("streaming") and x.get("sel") == 1 for x in r_), f"the right ear plays it over TLS ({r_})")
    c.check(playing is not None and f"127.0.0.1\t{r.redirect_port}\t".encode() in playing,
            f"while either ear plays, flash keeps the list as it was ({playing})")
    c.check(quiet is not None and f"https://127.0.0.1\t{r.port}/{r.redirect_port}\t".encode() in quiet,
            f"both quiet: in flash ({quiet})")


def tls_switch(c):
    """The right ear moved on in the middle of a slow TLS handshake -- four
    seconds of it, as a front on a slow line: let go of within a slice, the
    next receiver played; the left ear, over TLS too, plays on untouched."""
    a = c.mock(tls=True)
    slow = c.mock("--tls-delay", "4", tls=True)
    nxt = c.mock(tls=True)
    k = kcl.knob(c)
    b = k.boot("save", f"A={a.rx}", f"S={slow.rx}", f"N={nxt.rx}", "until", "streaming", "10", "audio",
               "right", "1", "wait", "1.5", "rstate", "right", "2", "runtil", "streaming", "4", "audio",
               "state", "off", timeout=60)
    r = rstates(b)
    c.check(len(r) >= 2 and r[0]["state"] == "connecting", f"the slow one, on its way ({r[:1]})")
    c.check(len(r) >= 2 and r[-1]["streaming"] and r[-1]["sel"] == 2 and r[-1]["t"] - r[0]["t"] < 2.5,
            f"the next one plays within a second or so of the choice ({r})")
    au = [l for l in b.at if l.startswith("@AUDIO")]
    c.check(len(au) == 2 and au[0].split()[3] == au[1].split()[3],
            f"the left ear's ring never ran dry meanwhile ({au})")
    c.check(slow.stats()["logins"] == 0 and nxt.stats()["logins"] == 1,
            f"no login to the slow one, one to the next ({slow.stats()['logins']}, {nxt.stats()['logins']})")


SCENARIOS = [both, refused, list_save, at_boot, marks, tests, status_once, adaptive, uber_input, follows_left,
             out_of_range, quiet_both, tls_quiet_save, tls_switch]
tls_quiet_save.once = tls_switch.once = True


class Ctx(kl.Ctx):
    def __init__(self, name, tmp):
        super().__init__(name, tmp)
        self.notes = []

    def note(self, s):
        self.notes.append(s)

    def uber(self, *flags):
        m = Uber(*flags)
        self.mocks.append(m)
        return m


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", required=True, help="the kiwi_host binary")
    ap.add_argument("--mock", required=True, help="tools/mock_kiwi.py")
    ap.add_argument("--uber", help="tools/mock_ubersdr.py (beside --mock)")
    ap.add_argument("--only", help="run the scenarios whose name has this in it")
    ap.add_argument("--tls", action="store_true", help="every receiver behind TLS, every address https://")
    ap.add_argument("--no-tls", action="store_true", help="the harness was built without TLS: none tried")
    ap.add_argument("-v", "--verbose", action="store_true", help="print every log")
    args = ap.parse_args()
    args.host = os.path.abspath(args.host)
    args.mock = os.path.abspath(args.mock)
    args.uber = os.path.abspath(args.uber or os.path.join(os.path.dirname(args.mock), "mock_ubersdr.py"))
    kl.ARGS = args
    with tempfile.TemporaryDirectory(prefix="kiwi_ears_") as tmp:
        if not args.no_tls:
            kl.make_certs(tmp)
        chosen = kl.tls_runs(args, [s for s in SCENARIOS if not args.only or args.only in s.__name__])
        if chosen is None:
            return kl.SKIPPED
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
            if c.fails or args.verbose:
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
