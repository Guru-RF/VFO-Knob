#!/usr/bin/env python3
"""The owners' limits on the kiwi firmware's own receiver, end to end on the PC:
components/kiwi_client and the session in components/kiwi_proto (kiwi_host,
built by this directory's CMakeLists.txt as the firmware compiles them) against
tools/mock_kiwi.py -- never against someone's receiver.

    python3 test/host/kiwi_client_limits.py --host build_host/kiwi_host --mock tools/mock_kiwi.py

As kiwi_limits.py does for the web SDR beside a radio: a run of kiwi_host is a
boot of the knob, its NVS a file, "off" a power cut, and what counts is what
the mock saw. The day limit, the silent door, the HTTP refusals and every
answer to a login, time up and kicked, the courtesy cap, switching receivers
live; and the session itself as the mock recorded it -- the keepalive, the
inactivity_ack only after a touch, the first tune only once the receiver has
said where it tunes, CW and an offset, CW where its owner centres it (its
load_cfg, read as it goes by), the S-meter's reference, the pitch at
every audio rate; the receivers' names on the dial and the slab from their
/status, who their owners see, the noise filter and the squelch, OV.

Each scenario has its own mock on a free port and its own NVS; they run side
by side, the longest some four minutes. Exit 1 on any failure, with the
knob's and the mock's logs. --tls: every receiver behind TLS, as
kiwi_limits.py has it; the scenarios about TLS itself run in the clear run.
"""
import argparse
import os
import re
import socket
import sys
import tempfile
import threading
import time

sys.dont_write_bytecode = True        # nothing written beside the tests
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kiwi_limits as kl              # noqa: E402  the mock, the boots, the checks

STATE = re.compile(r'@(STATE|BOOT) link=(\w+) rx=(-?\d+) why="([^"]*)" state="([^"]*)" server="([^"]*)" '
                   r'line2="([^"]*)" line3="([^"]*)" choice=(\d+) f=(-?\d+) mode=(\w*) lo=(-?\d+) hi=(-?\d+) '
                   r'fmax=(-?\d+) dbm=(\S+) model="([^"]*)" samples=(\d+) t=(\S+)')


MORE = re.compile(r'nr=(-?\d+)/(\S*) sq=(\d+)/(\d) ovl=(\d) audible=(\d)')


class Boot(kl.Boot):
    """One boot of kiwi_host: its @ lines."""
    def states(self):
        out = []
        for line in self.at:
            m = STATE.match(line)
            if m:
                s = dict(tag=m[1], link=m[2], rx=int(m[3]), why=m[4], state=m[5], server=m[6],
                         line2=m[7], line3=m[8], choice=int(m[9]), f=int(m[10]), mode=m[11],
                         lo=int(m[12]), hi=int(m[13]), fmax=int(m[14]), dbm=float(m[15]),
                         model=m[16], samples=int(m[17]), t=float(m[18]))
                x = MORE.search(line)
                if x:
                    s.update(nr=int(x[1]), nr_name=x[2], sq=int(x[3]), has_sq=x[4] == "1", ovl=x[5] == "1",
                             audible=x[6] == "1")
                out.append(s)
        return out

    def pitches(self):
        """Each @PITCH, Hz."""
        return [float(re.match(r"@PITCH hz=(\S+)", l)[1]) for l in self.at if l.startswith("@PITCH")]

    def touches(self):
        """When each touch was, by the knob's clock."""
        return [float(l.split()[1]) for l in self.at if l.startswith("@TOUCH")]


class Knob(kl.Knob):
    def _done(self, out, cmds):
        b = super()._done(out, cmds)
        return Boot(b.lines)


MARK = re.compile(r"@MARK (\d+) \S+ strikes=(\d+) kind=(\d+) boot=(\d+) utc=(\d+)(?: unsure=(\d) hg=(\d))?")
REST = re.compile(r"@REST (\d+) \S+ strikes=(\d+)")


def marks_full(b):
    """Each @MARKS group, as {entry: {strikes, utc, unsure, rest}}: the marks
    that hold a receiver back, and those at rest."""
    groups, cur = [], {}
    for line in b.at:
        m = MARK.match(line)
        if m:
            cur[int(m[1])] = dict(strikes=int(m[2]), utc=int(m[5]), unsure=m[6] == "1", rest=False)
            continue
        m = REST.match(line)
        if m:
            cur[int(m[1])] = dict(strikes=int(m[2]), rest=True)
        elif line.startswith("@MARKS"):
            groups.append(cur)
            cur = {}
    return groups


def knob(c, name="knob"):
    return Knob(c, name)


def finish_after(c, k, p, cmds, marks):
    """The boot running in the background, and the mock's stats taken at the
    given seconds: [(t, stats)]."""
    t0 = time.time()
    seen = []
    m = c.mocks[-1]
    for t in marks:
        time.sleep(max(0.0, t0 + t - time.time()))
        seen.append((t, m.stats()))
    return k.finish(p, cmds), seen


# ------------------------------------------------------------------ scenarios

def day_limit_across_reboots(c):
    """A Web-888 over its day limit: at most two refused logins over 9 reboots,
    3 list saves, 3 re-choices and 2 Tests -- then its restart lifts the mark at
    the next boot, without spending a try."""
    m = c.mock("--ip-limit-at-login")
    k = knob(c)
    rx = m.rx
    b = k.boot("save", rx, "until", "day limit", "15", "marks", "quit")
    s = b.state()
    c.check(s.get("state") == "day limit" and s.get("why") == "DAY LIMIT",
            f"boot 1: refused at login, held as DAY LIMIT ({s})")
    c.check(b.marks() == [{0: 1}], f"boot 1: marked with one strike ({b.marks()})")
    c.check(m.stats()["refused_at_login"] == 1, "boot 1: one refused login")
    for n in range(5):
        b = k.boot("wait", "4", "state", "off")
        s = b.state()
        c.check(s.get("rx") == 0 and s.get("state") == "day limit", f"reboot {n + 1}: in use still, and held ({s})")
    c.check(m.stats()["refused_at_login"] == 1, "5 reboots: not one login")
    b = k.boot("save", rx, "wait", "3", "save", rx, "wait", "3", "save", rx, "wait", "3", "state", "quit")
    c.check(b.state().get("state") == "day limit", "3 list saves: held")
    c.check(m.stats()["refused_at_login"] == 1, "3 list saves: not one login")
    b = k.boot("use", "0", "wait", "6", "state", "use", "0", "wait", "4", "use", "0", "wait", "4",
               "test", "0", "test", "0", "state", "marks", "quit")
    st = m.stats()
    c.check(st["refused_at_login"] == 2,
            f"3 re-choices, 2 tests: one try, refused -- two refused logins in all ({st['refused_at_login']})")
    c.check(b.marks() == [{0: 2}], f"held with two strikes ({b.marks()})")
    t = b.tests()
    c.check(len(t) == 2 and all(x.get("login") == "day limit" and x.get("mark", {}).get("held") for x in t),
            f"Test says 'day limit', held, and logs in to nothing ({t})")
    for n in range(3):
        b = k.boot("wait", "4", "state", "off")
        c.check(b.state().get("state") == "day limit", f"reboot {n + 6}: held")
    m.get("/mock/advance?h=2")                     # two hours on: a Web-888 forgets nothing
    b = k.boot("wait", "4", "state", "quit")
    c.check(b.state().get("state") == "day limit", "two hours on, the same boot of it: held")
    st = m.stats()
    c.check(st["refused_at_login"] == 2 and not st["barred"],
            f"at most two refused logins, never barred ({st['refused_at_login']}, {st['barred']})")
    m.get("/mock/restart")                          # then it restarts: its counts start over
    b = k.boot("until", "streaming", "15", "marks", "quit")
    c.check(b.state().get("state") == "streaming", "after its restart, the boot's /status lifts the mark: it plays")
    c.check(b.marks() == [{}], f"no mark ({b.marks()})")
    st = m.stats()
    c.check(st["refused"] == {} and st["logins"] == 1,
            f"no try spent on it: one login, none refused since ({st['logins']}, {st['refused']})")
    c.check(st["status"] <= 20, f"/status read rarely ({st['status']} reads in 12 boots and 2 tests)")


def kiwisdr_daily_clear(c):
    """A KiwiSDR clears its counts once a day: 23 hours on it is still held, 25
    hours on the boot's /status lifts the mark."""
    m = c.mock("--kiwisdr", "--ip-limit-at-login")
    k = knob(c)
    rx = m.rx
    b = k.boot("save", rx, "until", "day limit", "15", "marks", "quit")
    c.check(b.marks() == [{0: 1}] and m.stats()["refused_at_login"] == 1, "refused once, marked")
    b = k.boot("wait", "3", "state", "quit")
    c.check(b.state().get("state") == "day limit", "next boot: held (its /status dates the mark)")
    m.get("/mock/advance?h=23")
    b = k.boot("wait", "3", "state", "quit")
    c.check(b.state().get("state") == "day limit", "23 hours on: held")
    m.get("/mock/advance?h=2")
    b = k.boot("until", "streaming", "15", "marks", "quit")
    c.check(b.state().get("state") == "streaming" and b.marks() == [{}], "25 hours on: lifted at boot, and it plays")
    st = m.stats()
    c.check(st["refused_at_login"] == 1 and st["logins"] == 1, f"no try spent ({st['logins']} login)")
    c.check(b.state().get("model") == "KiwiSDR", f"it says what it is ({b.state().get('model')})")


def silent_door(c):
    """KiwiSDR 1.9's door for apps, shut, on one with no time limits: no word,
    the socket kept open. APPS FULL after 10 s, again 120 s on; the third time
    NO APPS, held."""
    m = c.mock("--kiwisdr", "--silent-door", "--no-tlimits")
    k = knob(c)
    rx = m.rx
    cmds = ("save", rx, "until", "apps full", "16", "wait", "265", "state", "wait", "20", "state", "quit")
    p = k.start(*cmds)
    b, seen = finish_after(c, k, p, cmds, [60, 125, 145, 250, 290])
    first = b.state(0)
    c.check(first.get("why") == "APPS FULL", f"the first: APPS FULL at about 10 s ({first})")
    silent = [s["silent"] for _, s in seen]
    c.check(silent[:2] == [1, 1] and silent[2] == 2, f"the second 120 s on, not sooner ({silent})")
    c.check(silent[3] == 2 and silent[4] == 3, f"the third 120 s after that ({silent})")
    last = b.state()
    c.check(last.get("state") == "no apps" and last.get("why") == "NO APPS",
            f"after the third: NO APPS, held ({last})")
    st = m.stats()
    c.check(st["silent"] == 3 and st["logins"] == 0, f"three logins in all, all silent ({st['silent']})")


def no_apps_from_status(c):
    """A KiwiSDR whose /status says it lets no apps in: NO APPS, and not one
    WebSocket opened -- not even when chosen again."""
    m = c.mock("--kiwisdr", "--ext-api", "0")
    k = knob(c)
    rx = m.rx
    b = k.boot("save", rx, "until", "no apps", "10", "use", "0", "wait", "4", "state", "quit")
    s = b.state()
    c.check(s.get("state") == "no apps" and s.get("why") == "NO APPS", f"NO APPS ({s})")
    st = m.stats()
    c.check(st["upgrades"] == 0 and st["auths"] == 0, f"no WebSocket at all ({st['upgrades']} upgrades)")
    c.check(st["status"] == 1, f"its /status read once ({st['status']})")


def http_refused(c):
    """HTTP 403: REFUSED, once -- not again until chosen again."""
    m = c.mock("--http", "403")
    k = knob(c)
    rx = m.rx
    b = k.boot("save", rx, "until", "refused", "10", "wait", "15", "state", "quit")
    s = b.state()
    c.check(s.get("why") == "REFUSED", f"REFUSED ({s})")
    c.check(m.stats()["http_refusals"] == 1, f"once in 15 s ({m.stats()['http_refusals']})")
    b = k.boot("wait", "5", "state", "quit")
    c.check(m.stats()["http_refusals"] == 2, "a boot is a new try: the hold is this boot's")


def http_not_kiwi(c):
    """HTTP 404: NOT A KIWI, once -- not again until the list is saved or it
    is chosen again; then once more."""
    m = c.mock("--http", "404")
    k = knob(c)
    rx = m.rx
    b = k.boot("save", rx, "until", "not a kiwi", "10", "wait", "15", "state",
               "save", rx, "wait", "4", "state", "quit")
    s = b.state(0)
    c.check(s.get("why") == "NOT A KIWI", f"NOT A KIWI ({s})")
    c.check(m.stats()["http_refusals"] == 2, f"once, and once after the list saved ({m.stats()['http_refusals']})")


def http_full(c):
    """HTTP 503: RECEIVER FULL, again 30 s on, then every 60 s."""
    m = c.mock("--http", "503")
    k = knob(c)
    rx = m.rx
    cmds = ("save", rx, "until", "full", "10", "wait", "100", "state", "quit")
    p = k.start(*cmds)
    b, seen = finish_after(c, k, p, cmds, [20, 40, 80, 100])
    n = [s["http_refusals"] for _, s in seen]
    c.check(n == [1, 2, 2, 3], f"at 0, 30 and 90 s ({n})")
    c.check(b.state().get("why") == "RECEIVER FULL", f"RECEIVER FULL ({b.state()})")


def idle_and_kick(c):
    """Time up and kicked: LISTEN AGAIN is asked, and nothing logs in until it
    is answered; answered, it plays."""
    for flag, word, why in (("--idle-after", "time up", "TIME UP"), ("--kick-after", "kicked", "KICKED")):
        m = c.mock(flag, "3")
        k = knob(c, word.replace(" ", "_"))
        rx = m.rx
        b = k.boot("save", rx, "until", "streaming", "10", "until", word, "10", "wait", "6", "state",
                   "relisten", "until", "streaming", "10", "quit")
        s = b.state(-2)
        c.check(s.get("why") == why and s.get("choice") == 1, f"{word}: said, and LISTEN AGAIN asked ({s})")
        c.check(b.state().get("state") == "streaming", f"{word}: answered, it plays")
        st = m.stats()
        c.check(st["logins"] == 2, f"{word}: no login of its own in 6 s, one on the answer ({st['logins']} logins)")


def courtesy_cap(c):
    """A receiver that never answers: the knob's own attempts at 0, 2, 7, 17,
    47 and 107 s -- and no seventh within ten minutes: TRY LATER."""
    m = c.mock("--close-early")
    k = knob(c)
    rx = m.rx
    cmds = ("save", rx, "wait", "190", "state", "use", "0", "wait", "3", "state", "quit")
    p = k.start(*cmds)
    b, seen = finish_after(c, k, p, cmds, [30, 100, 115, 185, 193])
    n = [s["upgrades"] for _, s in seen]
    c.check(n[:4] == [4, 5, 6, 6], f"six of its own in 2 minutes, then none ({n})")
    s = b.state(0)
    c.check(s.get("why") == "TRY LATER", f"held: TRY LATER ({s})")
    c.check(n[4] == 7, f"the operator's choice goes at once ({n})")


def live_switch(c):
    """Two receivers: the other is taken over at once, and back, with no
    restart -- the first one's channel let go."""
    a, bm = c.mock(), c.mock()
    k = knob(c)
    ra, rb = a.rx, bm.rx
    b = k.boot("save", ra, rb, "until", "streaming", "10", "use", "1", "wait", "0.3", "until", "streaming", "5",
               "use", "0", "wait", "0.3", "until", "streaming", "5", "state", "quit")
    st = b.states()
    s1 = [s for s in st if s["tag"] == "STATE"]
    c.check(len(s1) >= 3 and s1[1].get("rx") == 1 and s1[1].get("state") == "streaming",
            f"the second, at once ({s1[1] if len(s1) > 1 else s1})")
    c.check(s1[-1].get("rx") == 0 and s1[-1].get("state") == "streaming", f"and back ({s1[-1]})")
    sa, sb = a.stats(), bm.stats()
    c.check(sa["logins"] == 2 and sb["logins"] == 1, f"one login each time ({sa['logins']}, {sb['logins']})")
    b = k.boot("until", "streaming", "10", "state", "quit")
    c.check(b.state().get("rx") == 0, f"the one in use, kept through a restart ({b.state()})")
    c.check(b.state().get("server") == "MOCK dummy load", f"named for its antenna ({b.state().get('server')})")


# ---------------------------------------------------- the session, as recorded

def sets_of(m, conn=None):
    """Every SET the mock was sent, keepalives too: [(t, text)], its clock --
    one connection's, or all."""
    return [(x["t"], x["text"]) for x in m.get("/mock/sets") if conn is None or x["c"] == conn]


def login_at(sets):
    """When the login took, by the mock's clock: the first SET after it."""
    return next((t for t, x in sets if x.startswith("SET ident_user")), None)


def activity_ack(c):
    """SET inactivity_ack only once the glass is touched, a minute apart at
    most -- and it keeps a receiver with an idle timer playing, while one
    nobody touches runs out. Keepalive every 5 s; nothing before the login
    took but the login, and the settings in their order after it."""
    m = c.mock("--idle-after", "70")
    k = knob(c)
    rx = m.rx
    b = k.boot("save", rx, "until", "streaming", "10", "wait", "14", "touches", "12", "5", "wait", "3", "state",
               "until", "time up", "100", "state", "off", timeout=260)
    mid, end = b.state(1), b.state()
    c.check(mid.get("state") == "streaming", f"touched from 14 s to 69 s: at 78 s it plays ({mid})")
    c.check(end.get("why") == "TIME UP", f"untouched since: it ran out, TIME UP ({end})")
    c.check(135 <= end.get("t", 0) <= 152, f"...70 s after the last word of a listener, at ~145 s ({end.get('t')})")
    sets = sets_of(m, 1)
    t_in = login_at(sets)
    acks = [t - t_in for t, x in sets if x == "SET inactivity_ack"]
    first = b.touches()[0] - b.state(0)["t"] if b.touches() and b.state(0) else 14
    c.check(len(acks) == 2, f"two acks: the first touch's, and a minute later the rest's ({acks})")
    if len(acks) == 2:
        c.check(acks[0] >= first - 0.5 and acks[0] <= first + 1.5,
                f"none until the glass was touched, {first:.1f} s in; then at once ({acks[0]:.2f} s)")
        c.check(60.0 <= acks[1] - acks[0] <= 61.5, f"the next a minute on, not sooner ({acks[1] - acks[0]:.2f} s)")
    kas = [t for t, x in sets if x == "SET keepalive"]
    gaps = [b2 - a2 for a2, b2 in zip(kas, kas[1:])]
    c.check(len(kas) >= 25 and all(4.8 <= g <= 5.7 for g in gaps),
            f"keepalive every 5 s ({len(kas)}, {min(gaps, default=0):.2f}..{max(gaps, default=0):.2f} s)")
    texts = [x for t, x in sets if x != "SET keepalive"]
    c.check(texts[:1] and texts[0].startswith("SET auth"), f"the login first ({texts[:1]})")
    want = ("SET ident_user=VFO-Knob", "SET compression=1", "SET squelch=", "SET agc=")
    c.check(len(texts) > 6 and all(texts[1 + i].startswith(w) for i, w in enumerate(want)),
            f"then who the owner sees, compression, the squelch, the AGC ({texts[1:5]})")
    c.check(any(x.startswith("SET AR OK in=12000 out=24000") for x in texts[5:7]) and
            any(x.startswith("SET mod=") for x in texts[5:7]), f"then its rate answered and the tune ({texts[5:7]})")
    st = m.stats()
    c.check(st["kicked_early"] == 0 and st["hangs"] == 0, f"never kicked ({st['kicked_early']}, {st['hangs']})")


def first_tune_after_bandwidth(c):
    """The first SET mod waits until the receiver has said where it tunes, and
    keeps the dial inside: a Web-888's 6 m comes down to a KiwiSDR's 30 MHz on
    a switch; a receiver that never says is taken for 30 MHz after 2 s."""
    a = c.mock("--bw-delay", "1.5")
    kz = c.mock("--kiwisdr", "--bw-delay", "1")
    k = knob(c)
    ra, rz = a.rx, kz.rx
    b = k.boot("save", ra, rz, "until", "streaming", "10", "tune", "50100000", "wait", "1", "state",
               "use", "1", "wait", "0.3", "until", "streaming", "10", "wait", "1", "state", "off")
    s6, s30 = b.state(1), b.state()
    c.check(s6.get("f") == 50100000 and s6.get("fmax") == 62000000 and s6.get("mode") == "usb",
            f"a Web-888's 6 m, on its upper sideband ({s6})")
    sa = sets_of(a, 1)
    mods = [(t, x) for t, x in sa if x.startswith("SET mod=")]
    c.check(bool(mods) and mods[-1][1].endswith("freq=50100.000"), f"...and tuned there ({mods[-1:]})")
    c.check(bool(mods) and mods[0][0] - login_at(sa) >= 1.4,
            f"its first tune once it said its bandwidth, 1.5 s on ({mods[0][0] - login_at(sa) if mods else None})")
    c.check(s30.get("rx") == 1 and s30.get("f") == 30000000 and s30.get("fmax") == 30000000,
            f"on the KiwiSDR the dial comes down to its 30 MHz ({s30})")
    zm = [(t, x) for t, x in sets_of(kz) if x.startswith("SET mod=")]
    c.check(bool(zm) and zm[0][1].endswith("freq=30000.000"), f"its first tune inside it ({zm[:1]})")
    c.check(a.stats()["mod_before_bw"] == 0 and kz.stats()["mod_before_bw"] == 0, "no tune before the bandwidth")

    n = c.mock("--bandwidth", "0")
    k2 = knob(c, "never")
    b = k2.boot("save", n.rx, "until", "streaming", "10", "wait", "1", "state", "off")
    s = b.state()
    sn = sets_of(n, 1)
    nm = [t for t, x in sn if x.startswith("SET mod=")]
    c.check(bool(nm) and 1.9 <= nm[0] - login_at(sn) <= 2.6,
            f"never said: the first tune 2 s on ({nm[0] - login_at(sn) if nm else None})")
    c.check(s.get("fmax") == 30000000, f"...taken for a KiwiSDR's 30 MHz ({s})")


def cw_and_offset(c):
    """A KiwiSDR behind a converter, 100 MHz up: the dial tunes 100-130 MHz and
    the receiver is told the baseband; in CW the carrier goes 500 Hz below the
    dial, and the station on the dial is heard at a 500 Hz tone."""
    m = c.mock("--kiwisdr", "--offset", "100000", "--station", "100101000")
    k = knob(c)
    b = k.boot("save", m.rx, "until", "streaming", "10", "mode", "usb", "tune", "100100000",
               "wait", "1", "pitch", "2", "state", "mode", "cw", "tune", "100101000", "wait", "1", "pitch", "2",
               "state", "off")
    s = b.state(1)
    c.check(s.get("fmax") == 130000000 and s.get("f") == 100100000, f"it tunes 100-130 MHz ({s})")
    p = b.pitches()
    c.check(len(p) == 2 and abs(p[0] - 1000) <= 10, f"USB, 1 kHz below the station: a 1 kHz tone ({p})")
    c.check(len(p) == 2 and abs(p[1] - 500) <= 5, f"CW, the dial on the station: a 500 Hz tone ({p})")
    mods = [x for t, x in sets_of(m) if x.startswith("SET mod=")]
    c.check("SET mod=usb low_cut=300 high_cut=2700 freq=100.000" in mods, f"USB's baseband ({mods})")
    c.check("SET mod=cw low_cut=300 high_cut=700 freq=100.500" in mods, f"CW's carrier, 500 Hz below ({mods})")


def cw_centre_read(c):
    """A KiwiSDR whose owner set its CW at 400..800, said in its load_cfg past
    the knob's 16 KB: read as that frame goes by, the carrier 600 Hz below the
    dial and the passband around that tone -- the station on the dial heard
    at 600 Hz."""
    m = c.mock("--kiwisdr", "--cw", "400,800", "--station", "14101000")
    k = knob(c)
    b = k.boot("save", m.rx, "until", "streaming", "10", "mode", "cw", "tune", "14101000",
               "wait", "1", "pitch", "2", "state", "off")
    c.check(any("centres CW on 600 Hz" in l for l in b.lines), "its CW centre read as the frame went by: 600 Hz")
    p = b.pitches()
    c.check(len(p) == 1 and abs(p[0] - 600) <= 6, f"CW, the dial on the station: a 600 Hz tone ({p})")
    mods = [x for t, x in sets_of(m) if x.startswith("SET mod=")]
    c.check("SET mod=cw low_cut=400 high_cut=800 freq=14100.400" in mods, f"CW's carrier, 600 Hz below ({mods})")


def offset_said_again(c):
    """Where it tunes, said again mid-session (a scripted MSG): the dial kept
    inside the new range, and the receiver told at once."""
    m = c.mock("--kiwisdr", "--say", "3:freq_offset=100000.000")
    k = knob(c)
    b = k.boot("save", m.rx, "until", "streaming", "10", "wait", "5", "state", "off")
    s = b.state()
    c.check(s.get("fmax") == 130000000 and s.get("f") == 100000000, f"100-130 MHz now, the dial at its foot ({s})")
    mods = [x for t, x in sets_of(m) if x.startswith("SET mod=")]
    c.check(len(mods) >= 2 and mods[-1].endswith("freq=0.000"), f"told the new baseband ({mods})")


def s_meter_bias(c):
    """The same signal reads the same on a Web-888 and a KiwiSDR, each counted
    from its own reference -- a Web-888 whose /status cannot be read known by
    its version."""
    for flags, what in ((("--dbm", "-73"), "Web-888"), (("--kiwisdr", "--dbm", "-100"), "KiwiSDR"),
                        (("--no-status", "--dbm", "-73"), "Web-888 without /status")):
        m = c.mock(*flags)
        k = knob(c, re.sub(r"\W+", "_", what))
        b = k.boot("save", m.rx, "until", "streaming", "10", "wait", "2", "state", "off")
        s = b.state()
        want = float(flags[-1])
        model = "KiwiSDR" if "--kiwisdr" in flags else "Web-888"
        c.check(abs(s.get("dbm", 0) - want) < 0.6 and s.get("model") == model,
                f"{what}: {want:.0f} dBm, as itself ({s.get('dbm')}, {s.get('model')})")


def pitch_at(rate):
    def scenario(c):
        m = c.mock("--rate", str(rate), "--station", "14101000")
        k = knob(c)
        b = k.boot("save", m.rx, "until", "streaming", "10", "mode", "usb", "tune", "14100000",
                   "wait", "1", "pitch", "3", "mode", "cw", "tune", "14101000", "wait", "1", "pitch", "3", "off")
        p = b.pitches()
        c.check(len(p) == 2 and abs(p[0] - 1000) <= 10 and abs(p[1] - 500) <= 5,
                f"audio at {rate} Hz: 1 kHz in USB, 500 Hz in CW, played at 24 kHz ({p})")
        ar = [x for t, x in sets_of(m) if x.startswith("SET AR OK")]
        c.check(ar == [f"SET AR OK in={rate} out=24000"], f"its rate answered ({ar})")
    scenario.__name__ = f"pitch_at_{rate}"
    scenario.__doc__ = f"The pitch is right with the receiver's audio at {rate} Hz."
    return scenario


# ------------------------------------------------------- every answer to a login

def answer(flags, why, state, retry, then=None, more=(), name=None, doc=None, ends_at=0.0):
    """A scenario: a receiver that answers the login so. The face says `why`;
    the knob asks again on its own `retry` s after the answer -- not sooner --
    or, with None, not at all until chosen again; then the commands in `more`
    run ("RX" is the mock's address) and `then` checks what they did. The
    answer comes `ends_at` s after the login (a Web-888's app check later)."""
    def scenario(c):
        m = c.mock(*flags)
        k = knob(c)
        hold = retry or 30
        cmds = ("save", m.rx, "until", state, "12", "wait", str(hold + 6), "state",
                *[x.replace("RX", m.rx) for x in more], "off")
        p = k.start(*cmds)
        marks = [ends_at + hold - 4] + ([ends_at + hold + 4] if retry else [])
        b, seen = finish_after(c, k, p, cmds, marks)
        s = b.state(0)
        c.check(s.get("why") == why and s.get("state") == state, f"{why} ({s})")
        n = [st["upgrades"] for _, st in seen]
        if retry:
            c.check(n == [1, 2], f"asked again on its own {retry} s on, not sooner ({n} connections)")
            c.check(b.state(1).get("why") == why, f"...and {why} again ({b.state(1)})")
        else:
            c.check(n == [1], f"not asked again on its own in {hold - 4} s: held ({n} connections)")
        if then:
            then(c, b, m)
    scenario.__name__ = name
    scenario.__doc__ = doc
    return scenario


def password_then_saved(c, b, m):
    s = b.state()
    st = m.stats()
    c.check(s.get("state") == "streaming" and st["logins"] == 1 and st["auths"] == 2,
            f"the list saved with its password: in at once ({s.get('state')}, {st['auths']} logins asked)")


def refused_chosen(c, b, m):
    st = m.stats()
    c.check(st["upgrades"] == 2, f"chosen again: once more, and held again ({st['upgrades']})")
    c.check(b.state().get("why") == "REFUSED", f"REFUSED again ({b.state()})")


def no_apps_held(c, b, m):
    st = m.stats()
    c.check(st["logins"] == 1 and st["too_busy"] == 1, f"its app check said none: held ({st['logins']} logins)")


LOGIN_ANSWERS = [
    answer(("--badp", "3"), "REFUSED", "refused", None, refused_chosen, ("use", "0", "wait", "3", "state"),
           "badp_3_refused", "badp=3: REFUSED, held until chosen again; chosen, one more attempt."),
    answer(("--password", "secret"), "PASSWORD?", "password?", None, password_then_saved,
           ("save", "secret@RX", "until", "streaming", "8", "state"),
           "password", "A wrong password: PASSWORD?, held; the list saved with the right one, in at once."),
    answer(("--badp", "1", "--chan-no-pwd", "3"), "RECEIVER FULL", "full", 30, name="badp_1_free_channels_taken",
           doc="No password, its password-free channels taken: RECEIVER FULL, again 30 s on."),
    answer(("--badp", "5"), "ONE PER IP", "one per ip", 60, name="badp_5_one_per_ip",
           doc="badp=5, one connection per address: ONE PER IP, again a minute on."),
    answer(("--badp", "6"), "UPDATING", "updating", 60, name="badp_6_updating",
           doc="badp=6, its database updating: UPDATING, again a minute on."),
    answer(("--badp", "2"), "TRY LATER", "try later", 60, name="badp_2_try_later",
           doc="badp=2, its own address not known yet: TRY LATER, again a minute on."),
    answer(("--too-busy", "all"), "RECEIVER FULL", "full", 30, name="too_busy_all",
           doc="too_busy before the login, every channel taken: RECEIVER FULL, again 30 s on."),
    answer(("--too-busy", "apps", "--app-check", "2"), "APPS FULL", "apps full", 120, name="too_busy_apps",
           doc="A Web-888's app check finds its app channels taken: APPS FULL, again two minutes on.",
           ends_at=2.0),
    answer(("--too-busy", "none", "--app-check", "2"), "NO APPS", "no apps", None, no_apps_held,
           name="too_busy_none", doc="A Web-888's app check: no apps at all -- NO APPS, held.", ends_at=2.0),
    answer(("--down",), "DOWN", "down", 60, name="down", doc="Disabled by its owner: DOWN, again a minute on."),
]


def one_per_address(c):
    """An owner's one session per address: a second knob behind the same
    address is told so -- ONE PER IP -- while the first plays on."""
    m = c.mock("--no-dup-ip")
    k1, k2 = knob(c, "first"), knob(c, "second")
    rx = m.rx
    cmds = ("save", rx, "until", "streaming", "10", "wait", "12", "state", "off")
    p = k1.start(*cmds)
    time.sleep(3)
    b2 = k2.boot("save", rx, "until", "one per ip", "10", "wait", "3", "state", "off")
    b1 = k1.finish(p, cmds)
    c.check(b2.state().get("why") == "ONE PER IP", f"the second: ONE PER IP ({b2.state()})")
    c.check(b1.state().get("state") == "streaming", f"the first plays on ({b1.state()})")
    st = m.stats()
    c.check(st["dup_ip"] == 1 and st["logins"] == 1, f"one refused, one in ({st['dup_ip']}, {st['logins']})")


# ----------------------------------------------- the limits, where it is hard

def slow_answer(c):
    """A day-limited Web-888 whose answer to the login takes 12 s, longer than
    the knob waits: the login counted in case it was a refusal, and held from
    then on, through restarts; tried again only when chosen -- two refused
    logins at most, never barred."""
    m = c.mock("--ip-limit-at-login", "--answer-delay", "12")
    k = knob(c)
    rx = m.rx
    b = k.boot("save", rx, "wait", "45", "state", "marks", "quit", timeout=120)
    s, mk = b.state(), marks_full(b)
    c.check(mk[-1].get(0, {}).get("strikes") == 1 and mk[-1][0].get("unsure"),
            f"its login unanswered: one strike, in case ({mk})")
    c.check(s.get("why") == "NO ANSWER" and s.get("state") == "no answer", f"NO ANSWER, held ({s})")
    st = m.stats()
    c.check(st["auths"] == 1 and st["refused_at_login"] == 1,
            f"45 s on, no second login of its own; the mock counted the one ({st['auths']}, {st['refused_at_login']})")
    k.boot("wait", "15", "state", "quit")
    c.check(m.stats()["auths"] == 1, "a restart: no login of its own")
    b = k.boot("use", "0", "wait", "25", "use", "0", "wait", "5", "state", "marks", "quit", timeout=120)
    st = m.stats()
    c.check(st["auths"] == 2 and st["refused_at_login"] == 2,
            f"chosen twice: one try, unanswered again, then held ({st['auths']} logins, {st['refused_at_login']} refused)")
    c.check(marks_full(b)[-1].get(0, {}).get("strikes") == 2, f"two strikes, no more ({marks_full(b)})")
    c.check(not st["barred"], "never barred")


def slow_answer_plays(c):
    """A receiver whose answer to the login takes 6 s: the knob waits for it,
    and plays -- nothing counted."""
    m = c.mock("--answer-delay", "6")
    k = knob(c)
    b = k.boot("save", m.rx, "until", "streaming", "15", "marks", "quit")
    c.check(b.state().get("state") == "streaming", f"it plays ({b.state()})")
    c.check(marks_full(b) == [{}], f"no mark ({marks_full(b)})")
    c.check(m.stats()["auths"] == 1, f"one login ({m.stats()['auths']})")


def switch_during_login(c):
    """Another receiver chosen while the login's answer is on its way: the
    answer is read first -- a day limit's refusal, counted as the receiver
    counted it -- then the other plays."""
    a, bm = c.mock("--ip-limit-at-login", "--answer-delay", "1.0"), c.mock()
    k = knob(c)
    ra, rb = a.rx, bm.rx
    b = k.boot("save", ra, rb, "wait", "0.6", "use", "1", "until", "streaming", "8", "marks",
               "use", "0", "wait", "4", "marks", "use", "0", "wait", "3", "marks", "quit")
    mk = marks_full(b)
    c.check(mk[0].get(0, {}).get("strikes") == 1, f"A's refusal, read and counted ({mk})")
    c.check(b.state(0).get("rx") == 1 and b.state(0).get("state") == "streaming", f"then B plays ({b.state(0)})")
    st = a.stats()
    c.check(st["refused_at_login"] == 2 and mk[-1].get(0, {}).get("strikes") == 2,
            f"A chosen twice more: one try, then held -- the knob's count is the receiver's "
            f"({st['refused_at_login']}, {mk})")


def status_left_unread(c):
    """A /status read let go of for another receiver, chosen while it was on
    its way, is no read: back on the first, its /status is read then, and its
    session knows the receiver -- where it is, on the slab."""
    a, bm = c.mock("--status-delay", "2"), c.mock()
    k = knob(c)
    ra, rb = a.rx, bm.rx
    b = k.boot("save", ra, rb, "wait", "0.5", "use", "1", "until", "streaming", "8", "use", "0", "wait", "0.3",
               "until", "streaming", "10", "state", "quit")
    c.check(a.stats()["status"] == 2, f"A's /status asked again, once back on it ({a.stats()['status']})")
    s = b.state()
    c.check(s.get("rx") == 0 and s.get("state") == "streaming", f"A plays ({s})")
    c.check(s.get("line3") == "On the bench", f"...and its /status is known: where it is ({s.get('line3')})")


def test_during_login(c):
    """The page's Test of the receiver in use while its session logs in: no
    second login beside it -- the knob's count of refusals is the
    receiver's."""
    m = c.mock("--ip-limit-at-login", "--answer-delay", "0.5")
    k = knob(c)
    b = k.boot("save", m.rx, "wait", "0.35", "test", "0", "wait", "2", "marks",
               "use", "0", "wait", "3", "marks", "use", "0", "wait", "2", "marks", "quit")
    t = b.tests()
    c.check(bool(t) and t[0].get("login") == "in use", f"the Test: in use, no login ({[x.get('login') for x in t]})")
    st, mk = m.stats(), marks_full(b)
    c.check(st["refused_at_login"] == mk[-1].get(0, {}).get("strikes") == 2,
            f"the knob counts what the receiver counted, two at most ({st['refused_at_login']}, {mk})")


def test_beside_first_login(c):
    """The page's Test of the receiver the knob is about to log in to -- the
    list just saved, or the receiver just chosen -- at once: before the knob
    has looked at it, while it reads its /status, or while the answers are
    on their way. A Test and a session never log in at once, and a choice
    made before the Test's refusal spends no try: one refused login, one
    strike, however the two meet."""
    for i, (act, flags) in enumerate([("save", ()), ("save", ("--status-delay", "0.3")),
                                      ("save", ("--answer-delay", "0.3")), ("use", ()), ("use", ()),
                                      ("use", ("--answer-delay", "0.3"))]):
        m = c.mock("--ip-limit-at-login", "--kiwisdr", "--ext-api", "4", *flags)
        k = knob(c, f"knob{i}")
        b = k.boot("save", m.rx, *(("use", "0") if act == "use" else ()), "test", "0",
                   "wait", "2", "marks", "quit")
        st, mk, t = m.stats(), marks_full(b), [x.get("login") for x in b.tests()]
        c.check(st["refused_at_login"] == 1 and mk[-1:] and mk[-1].get(0, {}).get("strikes") == 1,
                f"{act}, Test{' (' + ' '.join(flags) + ')' if flags else ''}: one refused login, one strike "
                f"({st['refused_at_login']}, {mk}, the Test: {t})")


def full_nvs(c):
    """A knob whose NVS has filled up keeps no mark: on its own it logs in to
    no receiver with time limits -- MEMORY FULL -- however often it starts;
    chosen, it does."""
    m = c.mock()
    k = knob(c)
    rx = m.rx
    b = k.boot("save", rx, "until", "streaming", "10", "quit")
    c.check(b.state().get("state") == "streaming", "the list saved while flash had room: it plays")
    m.get("/mock/set?limit=1")
    full = {"SHIM_NVS_FULL": "1"}
    for n in range(4):
        b = k.boot("wait", "4", "state", "off", env=full)
        s = b.state()
        c.check(s.get("why") == "MEMORY FULL", f"start {n + 1}: MEMORY FULL ({s})")
    st = m.stats()
    c.check(st["auths"] == 1 and st["refused_at_login"] == 0, f"four starts: not one login ({st['auths']} in all)")
    b = k.boot("use", "0", "wait", "3", "use", "0", "wait", "3", "use", "0", "wait", "2", "state", "quit", env=full)
    st = m.stats()
    c.check(st["refused_at_login"] == 2, f"chosen: refused, one try, then held ({st['refused_at_login']})")
    c.check(b.state().get("why") == "DAY LIMIT", f"held ({b.state()})")
    b = k.boot("wait", "4", "state", "off", env=full)
    c.check(b.state().get("why") == "MEMORY FULL" and m.stats()["refused_at_login"] == 2,
            f"its mark gone with the restart: still no login of its own ({b.state()})")


def hidden_hourglass(c):
    """A receiver that refuses for its day limit while its /status shows no
    hourglass: no hourglass proves nothing then, and the mark holds."""
    m = c.mock("--ip-limit-at-login", "--hide-hourglass")
    k = knob(c)
    k.boot("save", m.rx, "until", "day limit", "15", "quit")
    for n in range(4):
        b = k.boot("wait", "3", "state", "off")
        c.check(b.state().get("state") == "day limit", f"start {n + 2}: held ({b.state()})")
    st = m.stats()
    c.check(st["refused_at_login"] == 1, f"one refused login in five starts ({st['refused_at_login']})")


def strikes_kept(c):
    """A time-limit password lets the knob in, and the mark rests -- its
    strikes kept, as a Web-888 keeps its count: when the password is taken
    away, the knob has spent what it may."""
    m = c.mock("--ip-limit-at-login", "--ipl", "OLD")
    k = knob(c)
    rx = m.rx
    b = k.boot("save", rx, "until", "day limit", "15", "save", rx + "/OLD", "until", "streaming", "10",
               "wait", "12", "marks", "quit")
    mk = marks_full(b)
    c.check(mk[-1].get(0, {}).get("rest") and mk[-1][0].get("strikes") == 2,
            f"it plays: the mark rests, its two strikes kept ({mk})")
    m.get("/mock/set?ipl=NEW")
    b = k.boot("wait", "5", "use", "0", "wait", "3", "state", "quit")
    st = m.stats()
    c.check(st["refused_at_login"] == 2, f"its password gone: two refused logins in all ({st['refused_at_login']})")
    c.check(b.state().get("why") == "DAY LIMIT", f"held ({b.state()})")


def mark_time_without_status(c):
    """A KiwiSDR refuses again 20 h later, its /status not to be read then:
    the mark does not keep the first refusal's time -- its day is counted
    from the last."""
    m = c.mock("--kiwisdr", "--ip-limit-at-login")
    k = knob(c)
    rx = m.rx
    b = k.boot("save", rx, "until", "day limit", "15", "marks", "quit")
    first = marks_full(b)[-1].get(0, {}).get("utc", 0)
    m.get("/mock/advance?h=20")
    m.get("/mock/set?no_status=1")
    b = k.boot("wait", "3", "use", "0", "wait", "3", "marks", "quit")
    mk = marks_full(b)[-1].get(0, {})
    c.check(first > 0 and m.stats()["refused_at_login"] == 2, f"refused again 20 h on ({first})")
    c.check(mk.get("strikes") == 2 and (mk.get("utc") == 0 or mk.get("utc", 0) >= first + 20 * 3600),
            f"its time: not the first refusal's, but unknown until read ({first} -> {mk})")


def cap_counts_only_reached(c):
    """Attempts that never reached the receiver -- nothing listening there,
    as with the WiFi down -- are not held against it: once it is there, the
    knob plays at its next try, not ten minutes on."""
    sk = socket.socket()                # the port held, not listening: refused, as with nothing
    sk.bind(("127.0.0.1", 0))           # there -- and no other test's socket takes it meanwhile
    port = sk.getsockname()[1]
    k = knob(c)
    cmds = ("save", f"{'https://' if kl.ARGS.tls else ''}127.0.0.1:{port}", "until", "streaming", "300", "state",
            "quit")
    p = k.start(*cmds)
    time.sleep(150)                     # its attempts at 0, 2, 7, 17, 47 and 107 s: none reached it
    sk.close()
    c.mock("--port", str(port))
    b = k.finish(p, cmds)
    s = b.state()
    c.check(s.get("state") == "streaming" and s.get("t", 999) <= 230,
            f"it plays at its next try, at ~170 s ({s.get('state')}, {s.get('t')})")


def restart_loop(c):
    """A knob that crashes again and again, each time soon after it started:
    from the second crash in a row it waits before it contacts the receiver
    on its own -- no status read, no login -- the operator's choice going
    at once; a power-on is a fresh start."""
    m = c.mock()
    k = knob(c)
    rx = m.rx
    k.boot("save", rx, "until", "streaming", "10", "wait", "3", "off", env={"SHIM_RESET": "poweron"})
    b = k.boot("until", "streaming", "10", "off", env={"SHIM_RESET": "panic"})
    c.check(b.state().get("state") == "streaming", f"one crash: back at once ({b.state()})")
    before = m.stats()
    cmds = ("wait", "8", "state", "use", "0", "until", "streaming", "10", "off")
    p = k.start(*cmds, env={"SHIM_RESET": "panic"})
    b, seen = finish_after(c, k, p, cmds, [6])
    mid = seen[0][1]
    c.check(mid["status"] == before["status"] and mid["auths"] == before["auths"],
            f"two in a row: 6 s in, not a word to it ({before['status']}/{mid['status']} reads, "
            f"{before['auths']}/{mid['auths']} logins)")
    c.check(b.state(0).get("why") == "TRY LATER", f"TRY LATER ({b.state(0)})")
    c.check(b.state().get("state") == "streaming", f"chosen: at once ({b.state()})")
    b = k.boot("until", "streaming", "10", "off", env={"SHIM_RESET": "poweron"})
    c.check(b.state().get("state") == "streaming", f"a power-on: at once ({b.state()})")


def silent_door_time_limits(c):
    """A KiwiSDR with time limits whose door for apps is shut: its silence
    could be a refusal whose answer was lost -- counted as one, held, and
    chosen, one try more."""
    m = c.mock("--kiwisdr", "--silent-door")
    k = knob(c)
    b = k.boot("save", m.rx, "until", "apps full", "16", "wait", "140", "state", "marks",
               "use", "0", "wait", "14", "use", "0", "wait", "3", "state", "marks", "quit", timeout=240)
    c.check(b.state(0).get("why") == "APPS FULL", f"APPS FULL ({b.state(0)})")
    mk = marks_full(b)
    c.check(mk[0].get(0, {}).get("strikes") == 1 and mk[0][0].get("unsure"), f"one strike, in case ({mk})")
    c.check(b.state(1).get("why") == "APPS FULL", f"140 s on: still held, nothing of its own ({b.state(1)})")
    st = m.stats()
    c.check(st["silent"] == 2 and mk[-1].get(0, {}).get("strikes") == 2,
            f"chosen twice: one try, then held ({st['silent']} logins, {mk})")


def held_through_crash(c):
    """Kicked, and then the knob crashes: the receiver is held still -- a
    restart nobody asked for chooses nothing -- and LISTEN AGAIN is asked;
    after a power-on it plays at once."""
    m = c.mock("--kick-after", "3")
    k = knob(c)
    rx = m.rx
    k.boot("save", rx, "until", "streaming", "10", "until", "kicked", "10", "wait", "1", "off")
    b = k.boot("wait", "6", "state", "relisten", "until", "streaming", "10", "until", "kicked", "10", "wait", "1",
               "off", env={"SHIM_RESET": "panic"})
    s = b.state(0)
    c.check(s.get("why") == "KICKED" and s.get("choice") == 1,
            f"after the crash: KICKED still, and LISTEN AGAIN asked ({s})")
    c.check(b.state(1).get("state") == "streaming", f"answered: it plays ({b.state(1)})")
    c.check(m.stats()["logins"] == 2, f"no login of its own after the crash ({m.stats()['logins']} in all)")
    b = k.boot("until", "streaming", "10", "off", env={"SHIM_RESET": "poweron"})
    c.check(b.state().get("state") == "streaming", f"a power-on: it plays at once ({b.state()})")
    c.check(m.stats()["logins"] == 3, f"one login more ({m.stats()['logins']})")


def time_limits_off_and_on(c):
    """Its owner takes the time limits away, then puts them back without a
    restart -- a Web-888 still counting: the mark rested meanwhile, its strike
    kept, so the knob has no more tries than it had."""
    m = c.mock("--ip-limit-at-login")
    k = knob(c)
    rx = m.rx
    k.boot("save", rx, "until", "day limit", "15", "quit")
    m.get("/mock/set?tlimits=0")
    b = k.boot("until", "streaming", "15", "marks", "quit")
    mk = marks_full(b)[-1].get(0, {})
    c.check(b.state().get("state") == "streaming" and mk.get("rest") and mk.get("strikes") == 1,
            f"no hourglass now: it plays, the mark at rest with its strike ({b.state().get('state')}, {mk})")
    m.get("/mock/set?tlimits=1")
    b = k.boot("wait", "5", "use", "0", "wait", "3", "state", "marks", "quit")
    st = m.stats()
    c.check(st["refused_at_login"] == 2, f"the limits back: two refused logins in all ({st['refused_at_login']})")
    c.check(b.state().get("why") == "DAY LIMIT", f"held, no try left ({b.state()})")


def hourglass_gone_but_refusing(c):
    """A receiver that showed its hourglass when it refused, then shows none
    and refuses still: the mark rests, the next refusal proves its hourglass
    means nothing there, and the mark holds from then on -- two refused
    logins at most, however often it starts or is chosen."""
    m = c.mock("--ip-limit-at-login")
    k = knob(c)
    rx = m.rx
    k.boot("save", rx, "until", "day limit", "15", "quit")
    m.get("/mock/set?hide_hourglass=1")
    b = k.boot("wait", "5", "state", "quit")
    c.check(b.state().get("why") == "DAY LIMIT" and m.stats()["refused_at_login"] == 2,
            f"its hourglass gone: one login of its own, refused, held ({b.state()})")
    for n in range(3):
        k.boot("wait", "3", "use", "0", "wait", "3", "off")
    st = m.stats()
    c.check(st["refused_at_login"] == 2 and not st["barred"],
            f"three starts and choices more: two refused logins in all ({st['refused_at_login']})")


# ------------------------------------------------- the receivers, on the dial

def labels_and_slab(c):
    """Receivers on the dial: one with no name goes by its antenna as its
    /status says it ("RF.Guru " left off) once that is read -- before its
    first session, never for a label alone -- else by its address, whole
    where it fits. Under it, its model and address (its model alone under
    its address), or under a name of the page's its antenna; then where it
    is. The page's Test says what it is and the name it suggests: its
    antenna, else its own name's first part."""
    a = c.mock("--antenna", "RF.Guru EchoTracer", "--loc", "Lombardsijde, Belgium",
               "--name", "RF.Guru Lombardsijde | EchoTracer", "--status-delay", "1")
    bm = c.mock("--kiwisdr", "--ext-api", "4", "--antenna", "", "--name", "ON4CDJ KiwiSDR | JO11")
    k = knob(c)
    ra, rb = a.rx, bm.rx
    b = k.boot("save", ra, rb, "label", "0", "label", "1", "until", "streaming", "10", "label", "0", "label", "1",
               "state", "test", "1", "test", "0", "quit")
    lb = b.labels()
    c.check(lb[:2] == [(0, a.hp), (1, bm.hp)], f"before its /status: by address and port ({lb[:2]})")
    c.check(lb[2:4] == [(0, "EchoTracer"), (1, bm.hp)],
            f"read: by its antenna, \"RF.Guru \" left off; the other, never read, by its address ({lb[2:4]})")
    s = b.state()
    c.check(s.get("server") == "EchoTracer" and s.get("line2") == f"Web-888  {a.hp}"
            and s.get("line3") == "Lombardsijde, Belgium",
            f"the slab: its name, its model and address, where it is ({s.get('server')} / {s.get('line2')} / "
            f"{s.get('line3')})")
    t = b.tests()
    c.check(len(t) == 2, f"two Tests ({t})")
    if len(t) == 2:
        c.check(t[0].get("login") == "ok" and t[0].get("model") == "KiwiSDR" and t[0].get("ext_api") == 4
                and t[0].get("antenna") == "" and t[0].get("loc") == "On the bench"
                and t[0].get("fill") == "ON4CDJ KiwiSDR",
                f"Test: what it is, its apps, where -- and no antenna, so its name's first part ({t[0]})")
        c.check(t[1].get("login") == "in use" and t[1].get("antenna") == "RF.Guru EchoTracer"
                and t[1].get("fill") == "EchoTracer" and t[1].get("model") == "Web-888",
                f"the one in use: no second login, its antenna, the name it suggests ({t[1]})")
    sa, sb = a.stats(), bm.stats()
    c.check(sa["status"] == 2 and sb["status"] == 1,
            f"/status: once for the session, once a Test; never for a label ({sa['status']}, {sb['status']})")
    c.check(sa["logins"] == 1 and sb["logins"] == 1, f"one login each: the session's, the Test's "
            f"({sa['logins']}, {sb['logins']})")
    # A name of the page's: on the dial, its antenna under it.
    b = k.boot("save", "Tower=" + ra, rb, "until", "streaming", "10", "label", "0", "state", "quit")
    s = b.state()
    c.check(b.labels() == [(0, "Tower")] and s.get("server") == "Tower" and s.get("line2") == "RF.Guru EchoTracer",
            f"named on the page: that name, its antenna under it ({b.labels()}, {s.get('line2')})")
    # Named as its antenna, the way the page's RF.Guru button names the four:
    # the antenna is not said twice -- its model and address under it.
    b = k.boot("save", "EchoTracer=" + ra, rb, "until", "streaming", "10", "state", "quit")
    s = b.state()
    c.check(s.get("server") == "EchoTracer" and s.get("line2") == f"Web-888  {a.hp}",
            f"named as its antenna: its model and address under it ({s.get('server')} / {s.get('line2')})")
    # No name, no antenna: its address on the first line, whole -- not said
    # again under it, only its model.
    b = knob(c, "knob2").boot("save", rb, "until", "streaming", "10", "state", "quit")
    s = b.state()
    c.check(s.get("server") == bm.hp and s.get("line2") == "KiwiSDR",
            f"by its address: its model alone under it ({s.get('server')} / {s.get('line2')})")


def ident_user(c):
    """Who the owner sees: VFO-Knob, until the page gives the knob a name --
    then that, URL-encoded as the receivers' page sends it, at once to the
    receiver playing with no new login; at every login after, through a
    restart; in a Test's login too. Cleared, VFO-Knob again."""
    m, o = c.mock(), c.mock()
    k = knob(c)
    rx, ro = m.rx, o.rx
    b = k.boot("save", rx, ro, "until", "streaming", "10", "ident", " ON6URE / Jo\u00ebl ", "wait", "1", "state",
               "test", "1", "quit")
    idents = [x for t, x in sets_of(m) if x.startswith("SET ident_user=")]
    c.check(idents == ["SET ident_user=VFO-Knob", "SET ident_user=ON6URE%20%2F%20Jo%C3%ABl"],
            f"VFO-Knob at the login, the page's name at once after ({idents})")
    st = m.stats()
    c.check(st["ident"] == "ON6URE / Jo\u00ebl" and st["logins"] == 1 and b.state().get("state") == "streaming",
            f"the receiver lists it, decoded; no new login for it ({st['ident']}, {st['logins']} login)")
    c.check(any(l == '@IDENT saved "ON6URE / Jo\u00ebl"' for l in b.at), f"kept as given, its spaces trimmed "
            f"({[l for l in b.at if l.startswith('@IDENT')]})")
    c.check(o.stats()["ident"] == "ON6URE / Jo\u00ebl", f"a Test's login says it too ({o.stats()['ident']})")
    b = k.boot("until", "streaming", "10", "ident", "", "wait", "1", "quit")
    idents = [x for t, x in sets_of(m) if x.startswith("SET ident_user=")]
    c.check(idents[2:] == ["SET ident_user=ON6URE%20%2F%20Jo%C3%ABl", "SET ident_user=VFO-Knob"],
            f"after a restart, at the login; cleared, VFO-Knob ({idents[2:]})")


def nr_and_squelch(c):
    """The noise filter and the squelch, as the face's editors and the API
    set them: the filter once the choice has rested half a second, in the
    receivers' page's own commands -- one turned past never sent; the squelch
    in dB over the noise, in NBFM on the receiver's own scale. Over the
    signal it closes: silence, its frames still coming. Both kept through a
    restart, and sent at the login -- the squelch again once the audio flows,
    as an UberSDR's Kiwi input lets one before its channel go by."""
    m = c.mock("--snr", "10")
    k = knob(c)
    rx = m.rx
    b = k.boot("save", rx, "until", "streaming", "10", "nr", "1", "nr", "2", "wait", "0.2", "nr", "1", "wait", "1.5",
               "state", "squelch", "50", "wait", "1.5", "pitch", "2", "state", "squelch", "0", "wait", "1",
               "pitch", "2", "mode", "nbfm", "squelch", "30", "wait", "1", "state", "wait", "2", "quit")
    sets = [x for t, x in sets_of(m) if x.startswith(("SET nr", "SET squelch"))]
    WDSP = ["SET nr algo=1", "SET nr type=0 param=0 pval=64", "SET nr type=0 param=1 pval=16",
            "SET nr type=0 param=2 pval=0.000080", "SET nr type=0 param=3 pval=0.125",
            "SET nr type=1 param=0 pval=64", "SET nr type=1 param=1 pval=16",
            "SET nr type=1 param=2 pval=0.000080", "SET nr type=1 param=3 pval=0.125",
            "SET nr type=0 en=1", "SET nr type=1 en=0"]
    nr = [x for x in sets if x.startswith("SET nr")]
    c.check(nr == WDSP, f"WDSP, once it rested -- LMS, turned past, never ({nr})")
    sq = [x for x in sets if x.startswith("SET squelch")]
    c.check(sq[:4] == ["SET squelch=0 param=0.50"] * 2 + ["SET squelch=20 param=0.50", "SET squelch=0 param=0.50"]
            and sq[-1:] == ["SET squelch=30 param=0.00"] and set(sq[4:-1]) <= {"SET squelch=0 param=0.00"},
            f"open at the login and once the audio flowed, 50 % (20 dB), open, then NBFM's 30 ({sq})")
    st = [x for x in b.states() if x["tag"] == "STATE"]
    s1, s2, s3 = (st[i] if len(st) > i else {} for i in (1, 2, 3))
    c.check(s1.get("nr") == 1 and s1.get("nr_name") == "WDSP" and s1.get("has_sq"), f"the face: NR WDSP ({s1})")
    p = b.pitches()
    c.check(len(p) == 2 and p[0] < 50 and abs(p[1] - 1000) <= 10,
            f"closed over a 10 dB signal: silence; open: its 1 kHz ({p})")
    c.check(s2.get("sq") == 50 and not s2.get("audible") and s2.get("state") == "streaming",
            f"closed, it still streams, and nothing is heard: a quiet moment ({s2})")
    c.check(s3.get("sq") == 30 and s3.get("mode") == "nbfm", f"NBFM, 30 % ({s3})")
    # Closed over the signal in NBFM too: a quiet moment, so the settings
    # reach flash before the restart below.
    n0 = len(sets_of(m))
    b = k.boot("until", "streaming", "10", "wait", "1", "state", "quit")
    after = [x for t, x in sets_of(m)[n0:] if x.startswith(("SET nr", "SET squelch"))]
    c.check(after[:1] == ["SET squelch=30 param=0.00"] and after[1:12] == WDSP,
            f"after a restart: at the login, as they were ({after})")
    s = b.state()
    c.check(s.get("nr") == 1 and s.get("sq") == 30, f"...and on the face ({s})")


def adc_overload(c):
    """The receiver's ADC overloaded (its frames' flag 0x02): OV beside the
    reading, for a second after the last such frame -- in one session still
    there just after the overload stops, gone a little later -- and none at
    all on a receiver that does not overload."""
    m = c.mock("--ovl")
    k = knob(c)
    cmds = ["save", m.rx] + ["state", "wait", "0.2"] * 30 + ["quit"]
    t0 = time.time()
    p = k.start(*cmds)
    # A second of its audio, overloaded; then none.
    while m.stats()["frames"] < 8 and time.time() - t0 < 20:
        time.sleep(0.05)
    time.sleep(0.5)
    m.get("/mock/set?ovl=0")
    t_off = time.time() - t0                    # the knob's clock, near enough: it started with t0
    b = k.finish(p, cmds)
    st = [s for s in b.states() if s["tag"] == "STATE"]
    before = [s["ovl"] for s in st if s["t"] < t_off - 0.3]
    after = [s["ovl"] for s in st if t_off + 0.1 < s["t"] < t_off + 0.7]
    late = [s["ovl"] for s in st if s["t"] > t_off + 2.0]
    c.check(any(before), f"OV while it overloads ({before})")
    c.check(any(after), f"...still, just after the overload stops: a second after its last frame ({after})")
    c.check(late and not any(late), f"...and gone a little later, in the same session ({late})")
    b = k.boot("until", "streaming", "10", "wait", "2.5", "state", "quit")
    c.check(b.state().get("ovl") is False, f"none on a receiver that does not overload ({b.state()})")


# --------------------------------------------------------- TLS, and redirects

def tls_redirect_kept(c):
    """A receiver at an http:// address that answers with a redirect to
    https:// on its own host -- the kiwisdr.com proxy's 307: followed at once,
    in TLS, the session resuming the /status read's; https:// kept from then
    on, known by the port it had; the slab names where it plays; after a
    restart straight to TLS, never redirected again."""
    m = c.mock("--redirect", "307", tls=True)
    k = knob(c)
    b = k.boot("save", f"http://127.0.0.1:{m.redirect_port}", "until", "streaming", "15", "list", "wait", "1",
               "state", "quit")
    st, s = m.stats(), b.state()
    c.check(s.get("state") == "streaming" and s.get("line2") == f"Web-888  127.0.0.1:{m.port}",
            f"it plays, the slab naming where ({s.get('state')}, {s.get('line2')})")
    c.check(kl.lists(b)[:1] == [{0: ("127.0.0.1", m.port, True, m.redirect_port)}],
            f"https:// from now on, known by the port it had ({kl.lists(b)})")
    c.check(st["redirects"] == 1 and st["tls"] >= 2 and st["tls_resumed"] >= 1,
            f"one redirect followed; the session resumed the read's TLS ({st['redirects']}, {st['tls']}, "
            f"{st['tls_resumed']})")
    b = k.boot("until", "streaming", "15", "list", "quit")
    st = m.stats()
    c.check(b.state().get("state") == "streaming" and st["redirects"] == 1,
            f"after a restart straight to TLS ({b.state().get('state')}, {st['redirects']} redirects)")
    c.check(kl.lists(b)[-1:] == [{0: ("127.0.0.1", m.port, True, m.redirect_port)}], f"kept in flash ({kl.lists(b)})")
tls_redirect_kept.once = True


def tls_moved(c):
    """A redirect to another host is not followed: MOVED on the face, "moved
    to <host>" on the pages, nothing sent there, and held until chosen again
    -- chosen, once more."""
    m = c.mock("--redirect", "301", "--redirect-to", "https://localhost:{port}", tls=True)
    k = knob(c)
    cmds = ("save", f"http://127.0.0.1:{m.redirect_port}", "until", "moved", "12", "wait", "6", "state",
            "use", "0", "wait", "3", "state", "quit")
    b, seen = finish_after(c, k, k.start(*cmds), cmds, [5])
    s, st = b.state(-2), seen[0][1]
    c.check(s.get("why") == "MOVED" and s.get("state") == "moved to localhost", f"said so, where to ({s})")
    c.check(st["tls"] == 0 and st["connections"] == 0 and st["redirects"] == 2,
            f"nothing sent there; the read's redirect and the session's, then held ({st['tls']}, "
            f"{st['redirects']})")
    c.check(b.state().get("why") == "MOVED" and m.stats()["redirects"] == 3,
            f"chosen again: once more, and held again ({b.state()}, {m.stats()['redirects']})")
tls_moved.once = True


def tls_certificate(c):
    """A certificate that does not verify -- signed by no one the knob trusts,
    or for another name: not a word spoken, CERTIFICATE? on the face, held
    until chosen again."""
    for cert in ("rogue", "other"):
        m = c.mock(tls=cert)
        k = knob(c, cert)
        b = k.boot("save", f"https://127.0.0.1:{m.port}", "until", "certificate", "12", "wait", "4", "state",
                   "quit")
        s, st = b.state(), m.stats()
        c.check(s.get("why") == "CERTIFICATE?" and s.get("state") == "certificate", f"{cert}: said so ({s})")
        c.check(st["connections"] == 0 and st["tls_failed"] == 2,
                f"{cert}: not a word spoken; its /status and the session refused it, then held "
                f"({st['connections']}, {st['tls_failed']})")
tls_certificate.once = True


def tls_marks(c):
    """The owners' limits across a redirect: refused for its day limit through
    the redirect, the receiver is marked under the key it keeps; a restart
    holds it as it was, https:// now -- and the operator's choices are its two
    tries, no more."""
    m = c.mock("--redirect", "307", "--ip-limit-at-login", tls=True)
    k = knob(c)
    b = k.boot("save", f"http://127.0.0.1:{m.redirect_port}", "until", "day limit", "12", "list", "marks", "quit")
    c.check(b.marks() == [{0: 1}] and kl.lists(b)[:1] == [{0: ("127.0.0.1", m.port, True, m.redirect_port)}],
            f"marked, https:// kept ({b.marks()}, {kl.lists(b)})")
    b = k.boot("wait", "4", "state", "marks", "off")
    c.check(b.state().get("why") == "DAY LIMIT" and b.marks() == [{0: 1}], f"after a restart held ({b.state()})")
    b = k.boot("use", "0", "wait", "5", "use", "0", "wait", "4", "state", "marks", "quit")
    st = m.stats()
    c.check(st["refused_at_login"] == 2 and b.marks()[-1:] == [{0: 2}] and not st["barred"],
            f"two refused logins in all ({st['refused_at_login']}, {b.marks()})")
tls_marks.once = True


SCENARIOS = [day_limit_across_reboots, kiwisdr_daily_clear, silent_door, no_apps_from_status, http_refused,
             http_not_kiwi, http_full, idle_and_kick, courtesy_cap, live_switch, activity_ack,
             first_tune_after_bandwidth, cw_and_offset, cw_centre_read, offset_said_again, s_meter_bias,
             *[pitch_at(r) for r in (12000, 20250, 24000, 48000)], *LOGIN_ANSWERS, one_per_address,
             slow_answer, slow_answer_plays, switch_during_login, status_left_unread, test_during_login,
             test_beside_first_login, full_nvs,
             hidden_hourglass,
             strikes_kept, mark_time_without_status, cap_counts_only_reached, restart_loop,
             silent_door_time_limits, held_through_crash, time_limits_off_and_on, hourglass_gone_but_refusing,
             labels_and_slab, ident_user, nr_and_squelch, adc_overload,
             tls_redirect_kept, tls_moved, tls_certificate, tls_marks]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", required=True, help="the kiwi_host binary")
    ap.add_argument("--mock", required=True, help="tools/mock_kiwi.py")
    ap.add_argument("--only", help="run the scenarios whose name has this in it")
    ap.add_argument("--tls", action="store_true", help="every receiver behind TLS, every address https://")
    ap.add_argument("--no-tls", action="store_true", help="the harness was built without TLS: none tried")
    ap.add_argument("-v", "--verbose", action="store_true", help="print every log")
    args = ap.parse_args()
    args.host = os.path.abspath(args.host)
    args.mock = os.path.abspath(args.mock)
    kl.ARGS = args
    with tempfile.TemporaryDirectory(prefix="kiwi_client_") as tmp:
        if not args.no_tls:
            kl.make_certs(tmp)
        chosen = kl.tls_runs(args, [s for s in SCENARIOS if not args.only or args.only in s.__name__])
        if chosen is None:
            return kl.SKIPPED
        ctxs = [kl.Ctx(s.__name__, tmp) for s in chosen]

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
