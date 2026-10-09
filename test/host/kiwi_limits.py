#!/usr/bin/env python3
"""The owners' limits, end to end on the PC: the knob's web-SDR code (sdr_host,
built by this directory's CMakeLists.txt from components/sdr_rx and
components/kiwi_proto as the firmware compiles them) against
tools/mock_kiwi.py -- never against someone's receiver.

    python3 test/host/kiwi_limits.py --host build_host/sdr_host --mock tools/mock_kiwi.py

A run of sdr_host is a boot of the knob; its NVS is a file, so the next run is
a restart, and "off" a power cut. What counts is what the mock saw: logins it
refused for its day limit -- a Kiwi bars an address at the fifth -- and that
the knob never logged in to a marked receiver on its own.

Each scenario has its own mock on a free port and its own NVS; they run side
by side. Exit 1 on any failure, with the knob's and the mock's logs.

With --tls every mock is behind TLS, as a receiver behind the kiwisdr.com
proxy or Cloudflare, and every address the knob is given is https:// -- the
owners' limits kept as in the clear. The scenarios about TLS itself -- a
redirect followed and kept, one elsewhere refused, a certificate refused --
run in the clear run, with their own mocks. The certificates are made for
the run, in its temporary directory: a CA the knob trusts for the run alone
(KIWI_TEST_CA), and never one a knob would.
"""
import argparse
import json
import os
import re
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request

ARGS = None
CERTS = {}                       # the run's: ca, srv (127.0.0.1, localhost), other (elsewhere.example), rogue


def make_certs(d):
    """A CA for this run alone, and what the test fronts show: a certificate it
    signed for 127.0.0.1 and localhost (srv); one it signed for another name
    (other); one signed by nobody the knob knows (rogue). ECDSA P-256, as
    Cloudflare's -- made with the openssl command, gone with the run. False:
    no openssl to make them."""
    try:
        _make_certs(d)
        return True
    except (OSError, subprocess.CalledProcessError) as e:
        print(f"no certificates for the TLS runs ({e}): those left out")
        return False


def tls_runs(args, scenarios):
    """The scenarios to run: --tls, all but those about TLS itself, every
    receiver behind TLS; else all, those about TLS itself too -- unless the
    harness was built without TLS (--no-tls) or no certificate could be made.
    None: nothing to run here (a --tls run that cannot be)."""
    if (args.tls and args.no_tls) or (args.tls and not CERTS):
        print("no TLS here: the --tls run is left out")
        return None
    once = lambda s: getattr(s, "once", False)          # noqa: E731
    if args.tls:
        return [s for s in scenarios if not once(s)]
    return [s for s in scenarios if not once(s) or (CERTS and not args.no_tls)]


SKIPPED = 77                     # ctest's SKIP_RETURN_CODE for a run left out


def _make_certs(d):
    def run(*a):
        subprocess.run(["openssl", *a], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, cwd=d)
    ec = ["-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:P-256", "-nodes"]
    run("req", "-x509", *ec, "-keyout", "ca.key", "-out", "ca.pem", "-days", "2", "-subj", "/CN=VFO-Knob test CA",
        "-addext", "basicConstraints=critical,CA:TRUE", "-addext", "keyUsage=critical,keyCertSign,cRLSign")
    for name, san in (("srv", "IP:127.0.0.1,DNS:localhost"), ("other", "DNS:elsewhere.example")):
        run("req", *ec, "-keyout", f"{name}.key", "-out", f"{name}.csr", "-subj", f"/CN={san.split(':')[1].split(',')[0]}")
        with open(os.path.join(d, f"{name}.ext"), "w") as f:
            f.write(f"subjectAltName={san}\nbasicConstraints=CA:FALSE\nkeyUsage=digitalSignature\n"
                    "extendedKeyUsage=serverAuth\n")
        run("x509", "-req", "-in", f"{name}.csr", "-CA", "ca.pem", "-CAkey", "ca.key", "-CAcreateserial",
            "-out", f"{name}.pem", "-days", "2", "-extfile", f"{name}.ext")
    run("req", "-x509", *ec, "-keyout", "rogue.key", "-out", "rogue.pem", "-days", "2", "-subj", "/CN=127.0.0.1",
        "-addext", "subjectAltName=IP:127.0.0.1,DNS:localhost")
    for k in ("ca", "srv", "other", "rogue"):
        CERTS[k] = os.path.join(d, f"{k}.pem")
        CERTS[k + "_key"] = os.path.join(d, f"{k}.key")


class Mock:
    """tools/mock_kiwi.py on a free port: in the clear, or behind TLS (`tls`:
    the run's own certificate, "other" or "rogue"; None: as the run, --tls).
    `rx`: its address as the knob is given it -- https:// over TLS; and with
    --redirect, `redirect_port` the port that answers with it."""
    def __init__(self, *flags, tls=None):
        tls = getattr(ARGS, "tls", False) if tls is None else tls
        self.tls = bool(tls)
        if self.tls:
            cert = "srv" if tls is True else tls
            flags = ("--tls", f"{CERTS[cert]}:{CERTS[cert + '_key']}", *flags)
        self.p = subprocess.Popen([sys.executable, "-B", "-u", ARGS.mock, "--port", "0", *flags],
                                  stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        line = self.p.stdout.readline()
        if not line.startswith("LISTENING"):
            raise RuntimeError(f"the mock did not start: {line}{self.p.stdout.read()}")
        self.port = int(line.split()[1])
        self.control_port, self.redirect_port = self.port, 0
        if self.tls:
            line = self.p.stdout.readline()
            if not line.startswith("CONTROL"):
                raise RuntimeError(f"the mock's control port did not start: {line}")
            self.control_port = int(line.split()[1])
        if "--redirect" in flags:
            line = self.p.stdout.readline()
            if not line.startswith("REDIRECTING"):
                raise RuntimeError(f"the mock's redirect did not start: {line}")
            self.redirect_port = int(line.split()[1])
        self.log = []
        threading.Thread(target=self._drain, daemon=True).start()

    def addr(self, host="127.0.0.1"):
        return f"{'https://' if self.tls else ''}{host}:{self.port}"

    @property
    def rx(self):
        return self.addr()

    @property
    def hp(self):
        """Its host:port, as the knob names a receiver with no name of its own."""
        return f"127.0.0.1:{self.port}"

    def _drain(self):
        for line in self.p.stdout:
            self.log.append(line.rstrip())

    def get(self, path):
        """/mock/: behind TLS on a port of its own, in the clear, counted nowhere."""
        with urllib.request.urlopen(f"http://127.0.0.1:{self.control_port}{path}", timeout=5) as r:
            return json.loads(r.read())

    def stats(self):
        return self.get("/mock/stats")

    def stop(self):
        self.p.terminate()
        try:
            self.p.wait(5)
        except subprocess.TimeoutExpired:
            self.p.kill()


class Boot:
    """One boot's output: the @ lines, in order."""
    def __init__(self, lines):
        self.lines = lines
        self.at = [l for l in lines if l.startswith("@")]

    def states(self):
        out = []
        for l in self.at:
            m = re.match(r'@(STATE|BOOT) want=(-?\d+) sel=(-?\d+) streaming=(\d) note="([^"]*)" state="([^"]*)"', l)
            if m:
                out.append(dict(tag=m[1], want=int(m[2]), sel=int(m[3]), streaming=m[4] == "1",
                                note=m[5], state=m[6]))
        return out

    def state(self, i=-1):
        s = [x for x in self.states() if x["tag"] == "STATE"]
        try:
            return s[i]
        except IndexError:
            return {}

    def boot(self):
        return next((x for x in self.states() if x["tag"] == "BOOT"), {})

    def marks(self):
        """Each @MARKS group, as {entry: strikes}."""
        groups, cur = [], {}
        for l in self.at:
            m = re.match(r"@MARK (\d+) \S+ strikes=(\d+)", l)
            if m:
                cur[int(m[1])] = int(m[2])
            elif l.startswith("@MARKS"):
                groups.append(cur)
                cur = {}
        return groups

    def tests(self):
        return [json.loads(l[6:]) for l in self.at if l.startswith("@TEST ")]

    def labels(self):
        """Each @LABEL, as (entry, label)."""
        return [(int(m[1]), m[2]) for m in (re.match(r'@LABEL (\d+) "([^"]*)"', l) for l in self.at) if m]


def nvs_value(path, ns, key):
    """A value in a knob's NVS -- the shim's file, "ns key type hex" a line --
    as bytes, or None."""
    try:
        with open(path) as f:
            for line in f:
                p = line.split()
                if len(p) == 4 and p[0] == ns and p[1] == key:
                    return b"" if p[3] == "-" else bytes.fromhex(p[3])
    except OSError:
        pass
    return None


class Knob:
    """A knob: its NVS and its RTC memory, files that outlive a run. `env`
    for a boot: SHIM_RESET (how it began: poweron, panic, brownout ...),
    SHIM_NVS_FULL (a flash with no room)."""
    def __init__(self, ctx, name):
        self.ctx = ctx
        self.nvs = os.path.join(ctx.dir, name + ".nvs")
        self.rtc = os.path.join(ctx.dir, name + ".rtc")

    def _args(self, cmds):
        return [ARGS.host, self.nvs, *map(str, cmds)]

    def _env(self, env):
        e = dict(os.environ, SHIM_RTC=self.rtc)
        e.pop("SHIM_NVS_FULL", None)
        if CERTS:
            e["KIWI_TEST_CA"] = CERTS["ca"]               # the run's CA, trusted for the run alone
        e.update(env or {})
        return e

    def boot(self, *cmds, timeout=180, env=None):
        r = subprocess.run(self._args(cmds), stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                           text=True, timeout=timeout, env=self._env(env))
        return self._done(r.stdout, cmds)

    def start(self, *cmds, env=None):
        return subprocess.Popen(self._args(cmds), stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                                env=self._env(env))

    def finish(self, p, cmds=()):
        out, _ = p.communicate(timeout=180)
        return self._done(out, cmds)

    def _done(self, out, cmds):
        lines = out.splitlines()
        self.ctx.log.append(f"--- boot: {' '.join(map(str, cmds))}")
        self.ctx.log += lines
        if any("runtime error" in l or "AddressSanitizer" in l for l in lines):
            self.ctx.check(False, "sanitizer report in the knob's output")
        return Boot(lines)


class Ctx:
    def __init__(self, name, tmp):
        self.name = name
        self.dir = os.path.join(tmp, re.sub(r"\W+", "_", name))
        os.makedirs(self.dir, exist_ok=True)
        self.log, self.fails, self.passes, self.mocks = [], [], 0, []

    def mock(self, *flags, tls=None):
        m = Mock(*flags, tls=tls)
        self.mocks.append(m)
        return m

    def knob(self, name="knob"):
        return Knob(self, name)

    def check(self, ok, what):
        if ok:
            self.passes += 1
        else:
            self.fails.append(what)
            self.log.append(f"!!! FAILED: {what}")


# ------------------------------------------------------------------ scenarios

def day_limit_across_reboots(c):
    """A Web-888 over its day limit: at most two refused logins over 8 reboots,
    3 list saves, 3 re-choices and 2 Tests -- then its restart lifts the mark at
    the next boot, without spending a try."""
    m = c.mock("--ip-limit-at-login")
    k = c.knob()
    rx = m.rx
    b = k.boot("save", rx, "choose", "0", "wait", "5", "state", "marks", "quit")
    c.check(b.state().get("state") == "daily limit reached" and b.state().get("note") == "day limit",
            f"boot 1: refused at login, held as 'day limit' ({b.state()})")
    c.check(b.marks() == [{0: 1}], f"boot 1: marked with one strike ({b.marks()})")
    c.check(m.stats()["refused_at_login"] == 1, "boot 1: one refused login")
    # The mark's lines go with the second receiver's own, under its tag.
    said = [l for l in b.lines if "day-limit mark" in l]
    c.check(bool(said) and all(" sdr: " in l for l in said), f"the mark's lines under sdr: ({said})")
    for n in range(5):
        b = k.boot("wait", "4", "state", "off")
        c.check(b.boot().get("want") == 0 and b.state().get("state") == "daily limit reached",
                f"reboot {n + 1}: chosen still, and held ({b.state()})")
    c.check(m.stats()["refused_at_login"] == 1, "5 reboots: not one login")
    b = k.boot("save", rx, "wait", "3", "save", rx, "wait", "3", "save", rx, "wait", "3", "state", "quit")
    c.check(b.state().get("state") == "daily limit reached", "3 list saves: held")
    c.check(m.stats()["refused_at_login"] == 1, "3 list saves: not one login")
    b = k.boot("choose", "0", "wait", "6", "state", "choose", "0", "wait", "4", "choose", "0", "wait", "4",
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
        c.check(b.state().get("state") == "daily limit reached", f"reboot {n + 6}: held")
    m.get("/mock/advance?h=2")                     # two hours on: a Web-888 forgets nothing
    b = k.boot("wait", "4", "state", "quit")
    c.check(b.state().get("state") == "daily limit reached", "two hours on, the same boot of it: held")
    st = m.stats()
    c.check(st["refused_at_login"] == 2 and not st["barred"],
            f"at most two refused logins, never barred ({st['refused_at_login']}, {st['barred']})")
    m.get("/mock/restart")                          # then it restarts: its counts start over
    b = k.boot("until", "streaming", "15", "marks", "quit")
    c.check(b.state().get("streaming"), "after its restart, the boot's /status lifts the mark: it plays")
    c.check(b.marks() == [{}], f"no mark ({b.marks()})")
    st = m.stats()
    c.check(st["refused"] == {} and st["logins"] == 1,
            f"no try spent on it: one login, none refused since ({st['logins']}, {st['refused']})")
    c.check(st["status"] <= 20, f"/status read rarely ({st['status']} reads in 16 boots and 2 tests)")


def kiwisdr_daily_clear(c):
    """A KiwiSDR clears its counts once a day: 23 hours on it is still held, 25
    hours on the boot's /status lifts the mark."""
    m = c.mock("--kiwisdr", "--ip-limit-at-login")
    k = c.knob()
    rx = m.rx
    b = k.boot("save", rx, "choose", "0", "wait", "5", "marks", "quit")
    c.check(b.marks() == [{0: 1}] and m.stats()["refused_at_login"] == 1,
            "refused once, on the one path a KiwiSDR takes")
    b = k.boot("wait", "3", "state", "quit")
    c.check(b.state().get("state") == "daily limit reached", "next boot: held (its /status dates the mark)")
    m.get("/mock/advance?h=23")
    b = k.boot("wait", "3", "state", "quit")
    c.check(b.state().get("state") == "daily limit reached", "23 hours on: held")
    m.get("/mock/advance?h=2")
    b = k.boot("until", "streaming", "15", "marks", "quit")
    c.check(b.state().get("streaming") and b.marks() == [{}], "25 hours on: lifted at boot, and it plays")
    st = m.stats()
    c.check(st["refused_at_login"] == 1 and st["logins"] == 1, f"no try spent ({st['logins']} login)")


def test_never_double_strikes(c):
    """Test on a receiver over its day limit: one refused login and the mark,
    on either flavour; a second Test logs in to nothing."""
    for flavour in ([], ["--kiwisdr"]):
        m = c.mock("--ip-limit-at-login", *flavour)
        k = c.knob("knob" + "".join(flavour))
        rx = m.rx
        b = k.boot("save", rx, "test", "0", "test", "0", "test", "0", "marks", "quit")
        t = b.tests()
        name = "KiwiSDR" if flavour else "Web-888"
        c.check(len(t) == 3 and all(x.get("login") == "day limit" for x in t),
                f"{name}: Test says 'day limit' ({[x.get('login') for x in t]})")
        c.check(bool(t) and t[0].get("path") == "app",
                f"{name}: the first Test logged in on the app path ({t[0].get('path') if t else None})")
        st = m.stats()
        c.check(st["refused_at_login"] == 1, f"{name}: three Tests, one refused login ({st['refused_at_login']})")
        c.check(st["browser_paths"] == 0, f"{name}: never a browser's path ({st['browser_paths']})")
        c.check(b.marks() == [{0: 1}], f"{name}: marked by the Test ({b.marks()})")
        said = [line for line in b.lines if "test of" in line]
        c.check(len(said) == 3 and all(" sdr: " in line for line in said),
                f"{name}: the Tests logged as the second receiver's (sdr:), not kiwi: ({said})")


def mid_session_limit(c):
    """ip_limit mid-session counts no refusal: the mark starts at 0, and two
    re-choices spend the two tries; the third gets nothing."""
    m = c.mock("--ip-limit-after", "12")       # past the 10 s in which it counts as the login's
    k = c.knob()
    rx = m.rx
    b = k.boot("save", rx, "choose", "0", "until", "streaming", "10", "wait", "14", "state", "marks",
               "choose", "0", "wait", "6", "state", "marks",
               "choose", "0", "wait", "6", "state", "marks",
               "choose", "0", "wait", "4", "state", "marks", "quit")
    mk = b.marks()
    c.check(mk[:1] == [{0: 0}], f"mid-session: marked, no strike ({mk})")
    c.check(mk[1:] == [{0: 1}, {0: 2}, {0: 2}], f"each re-choice one try, then held ({mk})")
    st = m.stats()
    c.check(st["limits_mid"] == 1 and st["refused_at_login"] == 2,
            f"two refused logins in all ({st['refused_at_login']})")
    c.check(b.state().get("state") == "daily limit reached", "held")


def idle_and_kick(c):
    """Time up and kicked: not again until chosen again; chosen again, it plays."""
    for flag, word in (("--idle-after", "time up"), ("--kick-after", "kicked")):
        m = c.mock(flag, "3")
        k = c.knob(word.replace(" ", "_"))
        rx = m.rx
        b = k.boot("save", rx, "choose", "0", "until", "streaming", "10", "until", word, "10",
                   "wait", "6", "state", "choose", "0", "until", "streaming", "10", "quit")
        s = b.state(-2)
        c.check(s.get("state") == word and s.get("note") == word, f"{word}: said so ({s})")
        c.check(b.state().get("streaming"), f"{word}: chosen again, it plays")
        st = m.stats()
        c.check(st["logins"] == 2, f"{word}: no login of its own in 6 s, one on the choice ({st['logins']} logins)")
        b = k.boot("wait", "3", "marks", "quit")
        c.check(b.marks() == [{}], f"{word}: no day-limit mark")


def selection_by_address(c):
    """The chosen receiver is kept by its address: a list moved under it is
    followed, a receiver gone means none; an old knob's place is trusted once."""
    m = c.mock()
    a, bb = m.rx, m.addr("localhost")
    k = c.knob()
    k.boot("save", a, bb, "choose", "1", "wait", "3", "quit")
    k.boot("rawsave", bb, a, "off")                # moved, and no time to save the place
    b = k.boot("quit")
    c.check(b.boot().get("want") == 0, f"moved: followed to its new place ({b.boot()})")
    k.boot("rawsave", a, "off")
    b = k.boot("quit")
    c.check(b.boot().get("want") == -1, f"gone: none chosen ({b.boot()})")
    # A knob from before: the place kept, its address not.
    old = c.knob("old")
    hexs = f"MOCK\t127.0.0.1\t{m.port}\t\t\nTWO\tlocalhost\t{m.port}\t\t\n".encode().hex()
    with open(old.nvs, "w") as f:
        f.write(f"vfo sdrs s {hexs}\nvfo sdrsel 1 01\n")
    b = old.boot("wait", "3", "quit")
    c.check(b.boot().get("want") == 1, f"an old knob's place, trusted once ({b.boot()})")
    old.boot("rawsave", m.addr("localhost"), m.rx, "off")
    b = old.boot("quit")
    c.check(b.boot().get("want") == 0, f"...and its address kept since ({b.boot()})")


def on_the_air(c):
    """A refusal the receiver counted is kept through a power cut in the
    over. The marks go to the settings (kvstore), here as on the SD card,
    whose write costs the audio nothing: a try chosen in an over is written
    at once and goes ahead -- counted, refused, held. (Without a card,
    kvstore itself holds the NVS write, and so the try, until the over ends.)"""
    m = c.mock("--ip-limit-at-login")
    k = c.knob()
    rx = m.rx
    k.boot("busy", "1", "save", rx, "choose", "0", "wait", "5", "off")
    b = k.boot("wait", "3", "state", "marks", "quit")
    c.check(b.marks() == [{0: 1}] and b.state().get("state") == "daily limit reached",
            f"refused in an over, power cut: the mark kept ({b.marks()})")
    c.check(m.stats()["refused_at_login"] == 1, "one refused login")
    cmds = ("busy", "1", "choose", "0", "wait", "6", "state", "busy", "0", "wait", "6", "state", "marks", "quit")
    p = k.start(*cmds)
    time.sleep(4.5)
    during = m.stats()["refused_at_login"]
    b = k.finish(p, cmds)
    c.check(during == 2, f"chosen again in an over: the try goes ahead ({during})")
    c.check(b.state(0).get("state") == "daily limit reached", f"...refused, and held ({b.state(0)})")
    c.check(m.stats()["refused_at_login"] == 2 and b.marks() == [{0: 2}],
            "the over ended: no more tries -- counted once, held")


def time_limit_password(c):
    """A new time-limit password counts as choosing the receiver again: one
    counted try. Mistyped ones cannot run the count past two; the right one
    plays, and the mark clears once it has streamed."""
    m = c.mock("--ip-limit-at-login", "--ipl", "secret")
    k = c.knob()
    rx = m.rx
    b = k.boot("save", rx, "choose", "0", "wait", "5", "marks",
               "save", rx + "/wrong", "wait", "6", "state", "marks",
               "save", rx + "/wrong2", "wait", "6", "state", "marks", "quit")
    c.check(b.marks() == [{0: 1}, {0: 2}, {0: 2}], f"a wrong password: one try, then held ({b.marks()})")
    c.check(m.stats()["refused_at_login"] == 2, "two refused logins in all")
    m2 = c.mock("--ip-limit-at-login", "--ipl", "secret")
    k2 = c.knob("right")
    rx2 = m2.rx
    b = k2.boot("save", rx2, "choose", "0", "wait", "5", "save", rx2 + "/secret", "until", "streaming", "10",
                "wait", "11", "marks", "quit")
    c.check(b.state().get("streaming"), "the right password: it plays")
    c.check(b.marks() == [{}], f"...and after 10 s the mark is gone ({b.marks()})")
    b = k2.boot("until", "streaming", "10", "quit")
    c.check(b.state().get("streaming"), "next boot: it plays at once (the clear reached flash)")
    st = m2.stats()
    c.check(st["refused_at_login"] == 1 and st["logins"] == 2, f"one refused login ({st['refused_at_login']})")


def slow_answer(c):
    """A day-limited Web-888 whose answer to the login takes 12 s, longer than
    the knob waits: counted in case it was a refusal, and held -- no second
    path, no retry of its own; chosen, one try; two refused logins at most."""
    m = c.mock("--ip-limit-at-login", "--answer-delay", "12")
    k = c.knob()
    rx = m.rx
    b = k.boot("save", rx, "choose", "0", "wait", "40", "state", "marks", "quit", timeout=120)
    st = m.stats()
    c.check(b.marks() == [{0: 1}], f"its login unanswered: one strike, in case ({b.marks()})")
    c.check(b.state().get("note") == "no answer", f"held, and said so ({b.state()})")
    c.check(st["auths"] == 1 and st["refused_at_login"] == 1,
            f"40 s on: no second path, no retry ({st['auths']} logins, {st['refused_at_login']} refused)")
    b = k.boot("choose", "0", "wait", "25", "choose", "0", "wait", "3", "marks", "quit", timeout=120)
    st = m.stats()
    c.check(st["auths"] == 2 and st["refused_at_login"] == 2 and b.marks() == [{0: 2}],
            f"chosen twice: one try, then held ({st['auths']} logins, {b.marks()})")


def list_saved_during_login(c):
    """The list saved while the login's answer is on its way: the answer is
    read first, and the receiver's refusal counted by the knob too."""
    m = c.mock("--ip-limit-at-login", "--answer-delay", "1.0")
    k = c.knob()
    rx = m.rx
    b = k.boot("save", rx, "choose", "0", "wait", "0.6", "save", rx, "wait", "3", "marks",
               "choose", "0", "wait", "3", "choose", "0", "wait", "2", "marks", "quit")
    st = m.stats()
    c.check(b.marks()[:1] == [{0: 1}], f"refused during the save: counted ({b.marks()})")
    c.check(st["refused_at_login"] == 2 and b.marks()[-1:] == [{0: 2}],
            f"the knob's count is the receiver's ({st['refused_at_login']}, {b.marks()})")


def status_read_rarely(c):
    """Two held receivers chosen in turn, again and again: each one's status
    page is read once a minute at most, not at every choice."""
    a, bm = c.mock("--ip-limit-at-login"), c.mock("--ip-limit-at-login")
    k = c.knob()
    ra, rb = a.rx, bm.rx
    b = k.boot("save", ra, rb, "choose", "0", "wait", "3", "choose", "1", "wait", "3", "marks", "quit")
    c.check(b.marks() == [{0: 1, 1: 1}], f"both marked ({b.marks()})")
    sa, sb = a.stats()["status"], bm.stats()["status"]
    turns = []
    for _ in range(8):
        turns += ["choose", "0", "wait", "1", "choose", "1", "wait", "1"]
    k.boot(*turns, "marks", "quit")
    da, db = a.stats()["status"] - sa, bm.stats()["status"] - sb
    c.check(da <= 2 and db <= 2, f"16 choices in 16 s: each read twice at most ({da}, {db})")
    c.check(a.stats()["refused_at_login"] <= 2 and bm.stats()["refused_at_login"] <= 2, "two refusals each at most")


def status_left_unread(c):
    """A /status read let go of for another receiver, chosen while it was on
    its way, is no read: back on the first, its /status is read then."""
    a, bm = c.mock("--status-delay", "2"), c.mock()
    k = c.knob()
    ra, rb = a.rx, bm.rx
    b = k.boot("save", ra, rb, "choose", "0", "wait", "0.5", "choose", "1", "until", "streaming", "8",
               "choose", "0", "wait", "0.3", "until", "streaming", "10", "state", "quit")
    c.check(a.stats()["status"] == 2, f"A's /status asked again, once back on it ({a.stats()['status']})")
    s = b.state()
    c.check(s.get("want") == 0 and s.get("streaming"), f"A plays ({s})")


def full_nvs(c):
    """A knob whose NVS has filled up keeps no mark: on its own it logs in to
    no receiver with time limits, however often it starts."""
    m = c.mock()
    k = c.knob()
    rx = m.rx
    b = k.boot("save", rx, "choose", "0", "until", "streaming", "10", "quit")
    c.check(b.state().get("streaming"), "the list saved while flash had room: it plays")
    m.get("/mock/set?limit=1")
    for n in range(3):
        b = k.boot("wait", "4", "state", "off", env={"SHIM_NVS_FULL": "1"})
        c.check(b.state().get("note") == "memory full", f"start {n + 1}: memory full ({b.state()})")
    st = m.stats()
    c.check(st["auths"] == 1 and st["refused_at_login"] == 0, f"three starts: not one login ({st['auths']})")


def refused_held(c):
    """A receiver that will not talk to this address -- HTTP 403, badp=3 --
    waits to be chosen again: no retry of its own, on either path."""
    for flag, n, wait in ((("--http", "403"), "http_refusals", "25"), (("--badp", "3"), "badp", "65")):
        m = c.mock(*flag)
        k = c.knob("k" + flag[1])
        b = k.boot("save", m.rx, "choose", "0", "wait", wait, "state", "choose", "0", "wait", "3",
                   "state", "quit", timeout=120)
        c.check(b.state(0).get("note") == "refused", f"{flag[1]}: refused ({b.state(0)})")
        st = m.stats()
        c.check(st[n] == 2, f"{flag[1]}: once, and once more when chosen ({st[n]})")


def restart_loop(c):
    """A knob that crashes again and again, each time soon after it started:
    from the second crash in a row its web SDR waits -- no status read, no
    login -- before it contacts the receiver on its own."""
    m = c.mock()
    k = c.knob()
    rx = m.rx
    k.boot("save", rx, "choose", "0", "until", "streaming", "10", "wait", "3", "off", env={"SHIM_RESET": "poweron"})
    b = k.boot("until", "streaming", "10", "off", env={"SHIM_RESET": "panic"})
    c.check(b.state().get("streaming"), f"one crash: back at once ({b.state()})")
    before = m.stats()
    b = k.boot("wait", "6", "state", "off", env={"SHIM_RESET": "brownout"})
    after = m.stats()
    c.check(after["auths"] == before["auths"] and after["status"] == before["status"],
            f"two in a row: not a word to it in 6 s ({before['auths']}/{after['auths']} logins)")
    c.check(b.state().get("note") == "try later", f"try later ({b.state()})")


def held_through_crash(c):
    """Time up, and then the knob crashes: the web SDR waits to be chosen
    again still -- a restart nobody asked for chooses nothing; after a
    power-on it plays at once."""
    m = c.mock("--idle-after", "3")
    k = c.knob()
    rx = m.rx
    k.boot("save", rx, "choose", "0", "until", "streaming", "10", "until", "time up", "10", "wait", "1", "off")
    b = k.boot("wait", "6", "state", "off", env={"SHIM_RESET": "panic"})
    c.check(b.state().get("state") == "time up", f"after the crash: time up still ({b.state()})")
    c.check(m.stats()["logins"] == 1, f"no login of its own ({m.stats()['logins']} in all)")
    b = k.boot("until", "streaming", "10", "off", env={"SHIM_RESET": "poweron"})
    c.check(b.state().get("streaming"), f"a power-on: it plays at once ({b.state()})")


def hourglass_gone_but_refusing(c):
    """A receiver that showed its hourglass when it refused, then shows none
    and refuses still: the mark rests, the next refusal proves its hourglass
    means nothing there, and the mark holds -- two refused logins at most."""
    m = c.mock("--ip-limit-at-login")
    k = c.knob()
    rx = m.rx
    k.boot("save", rx, "choose", "0", "wait", "5", "quit")    # refused; its /status not read yet
    k.boot("wait", "3", "quit")                               # the boot's read: its hourglass seen
    m.get("/mock/set?hide_hourglass=1")
    b = k.boot("wait", "5", "state", "quit")
    c.check(b.state().get("state") == "daily limit reached" and m.stats()["refused_at_login"] == 2,
            f"its hourglass gone: one login of its own, refused, held ({b.state()})")
    for n in range(3):
        k.boot("wait", "3", "choose", "0", "wait", "3", "off")
    st = m.stats()
    c.check(st["refused_at_login"] == 2 and not st["barred"],
            f"three starts and choices more: two refused logins in all ({st['refused_at_login']})")


# --------------------------------------- on the session in components/kiwi_proto
#
# The second receiver moved onto the kiwi firmware's session (KIWI-PLAN.md
# step 3): what it does as before -- the right ear, the chooser, its state
# and notes, the limits above -- and what it now does right on purpose: CW
# heard on the dial, a Web-888's S-meter from its own reference, the app path
# alone, a KiwiSDR's door for apps and the HTTP refusals classed, keepalive
# every 5 s, the cleaner resampler, a backlog left out in one jump.

def sets_of(m, conn=None):
    """Every SET the mock was sent: [(t, text)], its clock -- one
    connection's, or all."""
    return [(x["t"], x["text"]) for x in m.get("/mock/sets") if conn is None or x["c"] == conn]


def pitches(b):
    return [float(re.match(r"@PITCH hz=(\S+)", l)[1]) for l in b.at if l.startswith("@PITCH")]


def dbm_of(b):
    """The SDR's S-meter, as the last @STATE says it."""
    for l in reversed(b.at):
        if l.startswith("@STATE"):
            m = re.search(r" dbm=(\S+)", l)
            return float(m[1]) if m else None
    return None


def cw_on_the_dial(c):
    """The radio in USB 1 kHz below a carrier: a 1 kHz tone; in CW on it: a
    500 Hz tone, the station on the dial -- the receiver told the carrier
    500 Hz below, and the passband round the tone."""
    for flags, what in ((("--station", "14101000"), "Web-888"), (("--kiwisdr", "--station", "14101000"), "KiwiSDR")):
        m = c.mock(*flags)
        k = c.knob(what)
        b = k.boot("save", m.rx, "radio", "14100000", "usb", "300", "2700", "choose", "0",
                   "until", "streaming", "10", "wait", "1", "pitch", "2",
                   "radio", "14101000", "cw", "-250", "250", "wait", "1", "pitch", "2", "quit")
        p = pitches(b)
        c.check(len(p) == 2 and abs(p[0] - 1000) <= 10, f"{what}: USB, 1 kHz below the carrier: 1 kHz ({p})")
        c.check(len(p) == 2 and abs(p[1] - 500) <= 5, f"{what}: CW on the station: a 500 Hz tone ({p})")
        mods = [x for t, x in sets_of(m) if x.startswith("SET mod=")]
        c.check("SET mod=cw low_cut=250 high_cut=750 freq=14100.500" in mods,
                f"{what}: CW's carrier 500 Hz below the dial, its passband round the tone ({mods})")


def out_of_range(c):
    """A KiwiSDR beside a radio gone to 6 m: quiet, and said so -- out of
    range, "can't reach" for the dial, the 0-30 MHz it covers -- its session
    kept: no login again, no tune at every turn out there, its ring fed at the
    stream's pace with no break; back on 20 m, the tune and the audio at once.
    Its AGC, noise filter and squelch the receiver's defaults throughout."""
    m = c.mock("--kiwisdr")
    k = c.knob()
    b = k.boot("save", m.rx, "radio", "14074000", "usb", "300", "2700", "choose", "0",
               "until", "streaming", "10", "wait", "1", "pitch", "1", "state",
               "radio", "50150000", "usb", "300", "2700", "until", "out of range", "3", "pitch", "2", "audio",
               "radio", "50313000", "usb", "300", "2700", "wait", "0.3", "radio", "50313000", "cw", "-250", "250",
               "wait", "0.3", "radio", "51000000", "fm", "-6000", "6000", "wait", "0.3", "state", "audio",
               "radio", "14074000", "usb", "200", "2800", "until", "streaming", "2", "pitch", "1", "audio", "state",
               "quit", timeout=90, env={"SHIM_DEBUG": "1"})
    lines = [l for l in b.at if l.startswith("@STATE")]
    s = [x for x in b.states() if x["tag"] == "STATE"]
    c.check(len(s) == 6 and len(lines) == 6, f"each look at it ({s})")
    if len(s) < 6:
        return
    t = [float(re.search(r" t=(\S+)", l)[1]) for l in lines]
    c.check(s[1]["streaming"], f"on 20 m it plays ({s[1]})")
    for x in s[2:4]:
        c.check(not x["streaming"] and x["state"] == "out of range" and x["note"] == "can't reach",
                f"on 6 m: out of range, \"can't reach\" for the dial ({x})")
    c.check("range=0..30000000" in lines[2], f"...what it covers said: 0-30 MHz ({lines[2]})")
    c.check(s[4]["streaming"] and s[5]["streaming"] and t[4] - t[3] < 1.0,
            f"back on 20 m it plays, within a second ({s[4]}, {t[3]}..{t[4]})")
    p = [(float(x[1]), int(x[2])) for x in (re.match(r"@PITCH hz=(\S+) samples=(\d+)", l) for l in b.at) if x]
    c.check(len(p) == 3 and abs(p[0][0] - 1000) <= 15, f"on 20 m: its tone ({p})")
    c.check(len(p) == 3 and p[1][0] == 0 and p[1][1] >= 0.9 * 2 * 24000,
            f"on 6 m: silence, fed at the stream's pace ({p[1:2]})")
    c.check(len(p) == 3 and abs(p[2][0] - 1000) <= 60, f"back: its tone again at once ({p[2:]})")
    au = [l for l in b.at if l.startswith("@AUDIO")]
    c.check(len(au) == 3 and all("dropped=0 underruns=0" in l for l in au), f"its ring: nothing lost, no break ({au})")
    st = m.stats()
    c.check(st["logins"] == 1 and st["upgrades"] == 1, f"its session kept: one login ({st['logins']})")
    sets = [x for _, x in sets_of(m)]
    mods = [x for x in sets if x.startswith("SET mod=")]
    far = [x for x in mods if float(x.rsplit("freq=", 1)[1]) >= 30000 or "mod=usb" not in x]
    c.check(not far, f"never tuned to 6 m, nor to its edge, nor to CW or FM out there ({far})")
    c.check(mods[-1:] == ["SET mod=usb low_cut=200 high_cut=2800 freq=14074.000"],
            f"back: tuned to the dial, the radio's new filter with it ({mods[-1:]})")
    outs = [i for i, l in enumerate(b.lines) if l.startswith("@STATE") and "out of range" in l]
    tunes = [l for l in b.lines[outs[0]:outs[-1]] if " sdr: SET mod=" in l] if outs else ["?"]
    c.check(not tunes, f"no tune while the radio went on out there ({tunes})")
    said_out = [l for l in b.lines if " sdr: " in l and "out of its range" in l]
    said_back = [l for l in b.lines if " sdr: " in l and "within its range again" in l]
    c.check(len(said_out) == 1 and len(said_back) == 1, f"the log, once each way ({said_out}, {said_back})")
    dsp = [x for x in sets if x.startswith(("SET agc=", "SET squelch=", "SET nr "))]
    c.check(dsp and all("decay=1000" in x and "thresh=-100" in x for x in dsp if x.startswith("SET agc="))
            and all(x.startswith("SET squelch=0 ") for x in dsp if x.startswith("SET squelch="))
            and not any(x.startswith("SET nr ") for x in dsp),
            f"its own defaults, as ever beside a radio: AGC MED, squelch open, no noise filter ({dsp})")


def out_of_range_at_start(c):
    """A KiwiSDR chosen with the radio on 6 m already: one login, its edge
    tuned and quiet, "can't reach"; the radio back on 20 m, it plays -- its
    S-meter from S0 until its first reading, never a reading of nobody's (the
    0 dBm it had at boot: S9+70, held a second on the face)."""
    m = c.mock("--kiwisdr")
    k = c.knob()
    b = k.boot("save", m.rx, "radio", "50150000", "usb", "300", "2700", "choose", "0",
               "until", "out of range", "10", "wait", "1", "state",
               "radio", "14074000", "usb", "300", "2700", "until", "streaming", "3", "state", "wait", "1", "state",
               "quit", timeout=60)
    # Each `until` says the state as well: five looks.
    s = [x for x in b.states() if x["tag"] == "STATE"]
    d = [float(re.search(r" dbm=(\S+)", l)[1]) for l in b.at if l.startswith("@STATE")]
    c.check(len(s) == 5 and len(d) == 5, f"each look at it ({s}, {d})")
    if len(s) < 5:
        return
    c.check(not s[1]["streaming"] and s[1]["state"] == "out of range" and s[1]["note"] == "can't reach",
            f"on 6 m from the start: out of range, \"can't reach\" ({s[1]})")
    c.check(s[3]["streaming"] and (d[3] == -127.0 or abs(d[3] + 73) < 1),
            f"back on 20 m: S0 until its first reading, then its own -- not 0 dBm ({d[3]})")
    c.check(s[4]["streaming"] and abs(d[4] + 73) < 1, f"...a second on, the mock's -73 dBm ({d[4]})")
    mods = [x for _, x in sets_of(m) if x.startswith("SET mod=")]
    c.check(m.stats()["logins"] == 1 and len(mods) >= 2 and mods[-1] == "SET mod=usb low_cut=300 high_cut=2700 "
            "freq=14074.000" and all(x.endswith(" freq=30000.000") for x in mods[:-1]),
            f"one login: its edge, then the dial ({mods})")


def cw_at_the_edge(c):
    """What a receiver says it covers is what it reaches, in CW as in any
    mode: a converter's KiwiSDR, 144-174 MHz, under a CW station 300 Hz over
    its bottom plays it -- its carrier kept at the bottom, the station heard
    at 300 Hz, below its 500 Hz tone, as the kiwi firmware's own dial would
    have it -- and a dial 400 Hz over its top is out of range, its carrier
    inside or not."""
    m = c.mock("--kiwisdr", "--offset", "144000", "--station", "144000300")
    k = c.knob()
    b = k.boot("save", m.rx, "radio", "144000300", "cw", "-250", "250", "choose", "0",
               "until", "streaming", "10", "wait", "1", "pitch", "1", "state",
               "radio", "174000400", "cw", "-250", "250", "until", "out of range", "3", "state",
               "quit", timeout=60)
    # Each `until` says the state as well: four looks.
    lines = [l for l in b.at if l.startswith("@STATE")]
    s = [x for x in b.states() if x["tag"] == "STATE"]
    c.check(len(s) == 4 and len(lines) == 4, f"each look at it ({s})")
    if len(s) < 4:
        return
    c.check(s[1]["streaming"] and s[1]["state"] == "streaming", f"144.000300 MHz in CW: it plays ({s[1]})")
    mods = [x for _, x in sets_of(m) if x.startswith("SET mod=")]
    c.check("SET mod=cw low_cut=250 high_cut=750 freq=0.000" in mods,
            f"...its carrier kept at its bottom, the passband round the tone ({mods})")
    p = pitches(b)
    c.check(len(p) == 1 and abs(p[0] - 300) <= 10, f"...the station heard at 300 Hz, below its tone ({p})")
    said = [l for l in b.lines if " sdr: " in l and ("out of its range" in l or "within its range" in l)]
    c.check(not s[3]["streaming"] and s[3]["state"] == "out of range" and "range=144000000..174000000" in lines[3]
            and len(said) == 1 and "174000400 Hz is out of its range" in said[0],
            f"174.000400 MHz: beyond the 144-174 MHz it covers, out of range ({lines[3]}, {said})")


def s_meter_reference(c):
    """The same signal reads the same on a Web-888 and a KiwiSDR, each from
    its own reference: a Web-888 no longer 13 dB high."""
    for flags, what, want in ((("--dbm", "-73"), "Web-888", -73.0), (("--kiwisdr", "--dbm", "-100"), "KiwiSDR", -100.0)):
        m = c.mock(*flags)
        k = c.knob(what)
        b = k.boot("save", m.rx, "choose", "0", "until", "streaming", "10", "wait", "2", "state",
                   "quit")
        d = dbm_of(b)
        c.check(d is not None and abs(d - want) < 0.6, f"{what}: {want:.0f} dBm ({d})")


def app_path_keepalive(c):
    """The app path alone, as kiwirecorder logs in: the login first, then who
    the owner sees and the rest; keepalive every 5 s, not every second."""
    m = c.mock()
    k = c.knob()
    b = k.boot("save", m.rx, "choose", "0", "until", "streaming", "10", "wait", "22", "state",
               "quit")
    st = m.stats()
    c.check(b.state().get("streaming"), f"it plays ({b.state()})")
    c.check(st["app_paths"] == 1 and st["browser_paths"] == 0,
            f"one login, on the app path ({st['app_paths']} app, {st['browser_paths']} browser)")
    sets = sets_of(m, 1)
    texts = [x for t, x in sets if x != "SET keepalive"]
    c.check(bool(texts) and texts[0].startswith("SET auth") and texts[1:3] == ["SET ident_user=VFO-Knob",
                                                                               "SET compression=1"],
            f"the login, then who the owner sees ({texts[:3]})")
    c.check(not any(x.startswith(("SET gen", "SET genattn")) for x in texts), "no SET gen, which a Web-888 logs as bad")
    kas = [t for t, x in sets if x == "SET keepalive"]
    gaps = [b2 - a2 for a2, b2 in zip(kas, kas[1:])]
    c.check(len(kas) >= 4 and all(4.8 <= g <= 5.7 for g in gaps),
            f"keepalive every 5 s ({len(kas)}, {min(gaps, default=0):.2f}..{max(gaps, default=0):.2f} s)")
    st = m.stats()
    c.check(st["kicked_early"] == 0 and st["hangs"] == 0, "never kicked")


def no_apps_from_status(c):
    """A KiwiSDR whose /status lets no apps in (ON4CDJ's): no apps, said from
    its status page with no socket opened -- not after ten seconds of a
    session -- and not even when chosen again."""
    m = c.mock("--kiwisdr", "--ext-api", "0")
    k = c.knob()
    b = k.boot("save", m.rx, "choose", "0", "until", "no apps", "8", "choose", "0", "wait", "3",
               "state", "quit")
    s = b.state()
    c.check(s.get("state") == "no apps allowed" and s.get("note") == "no apps", f"no apps ({s})")
    st = m.stats()
    c.check(st["upgrades"] == 0 and st["auths"] == 0, f"no WebSocket at all ({st['upgrades']} upgrades)")
    c.check(st["status"] == 1, f"its /status read once ({st['status']})")


def silent_door(c):
    """KiwiSDR 1.9's door for apps, shut, on one with no time limits: busy
    ("app channels in use") after ten seconds of silence, again 120 s on;
    the third time no apps, held."""
    m = c.mock("--kiwisdr", "--silent-door", "--no-tlimits")
    k = c.knob()
    cmds = ("save", m.rx, "choose", "0", "until", "app channels", "16", "wait", "265", "state",
            "quit")
    p = k.start(*cmds)
    t0 = time.time()
    seen = []
    for t in (60, 125, 145, 250, 275):
        time.sleep(max(0.0, t0 + t - time.time()))
        seen.append(m.stats()["silent"])
    b = k.finish(p, cmds)
    first = b.state(0)
    c.check(first.get("state") == "app channels in use" and first.get("note") == "busy", f"busy at ~10 s ({first})")
    c.check(seen[:2] == [1, 1] and seen[2] == 2, f"the second 120 s on, not sooner ({seen})")
    c.check(seen[3] == 2 and seen[4] == 3, f"the third 120 s after that ({seen})")
    c.check(b.state().get("state") == "no apps allowed", f"after the third: no apps, held ({b.state()})")


def http_classed(c):
    """HTTP 404: not a kiwi, held until the list is saved; 503: busy, asked
    again 20 s on."""
    m = c.mock("--http", "404")
    k = c.knob("k404")
    rx = m.rx
    b = k.boot("save", rx, "choose", "0", "until", "not a kiwi", "8", "wait", "25", "state", "save", rx, "wait", "4",
               "state", "quit")
    c.check(b.state(0).get("note") == "not a kiwi", f"404: not a kiwi ({b.state(0)})")
    c.check(m.stats()["http_refusals"] == 2, f"once, and once more when the list was saved ({m.stats()['http_refusals']})")
    m2 = c.mock("--http", "503")
    k2 = c.knob("k503")
    b = k2.boot("save", m2.rx, "choose", "0", "until", "busy", "8", "wait", "16", "state",
                "wait", "8", "state", "quit")
    n = m2.stats()["http_refusals"]
    c.check(b.state(0).get("note") == "busy", f"503: busy ({b.state(0)})")
    c.check(n == 2, f"again 20 s on, not sooner ({n} in ~25 s)")


def pitch_at_every_rate(c):
    """The receiver's audio at 12, 20.25, 24 and 48 kHz: a 1 kHz tone is 1 kHz
    in the right ear at the knob's 24 kHz."""
    for rate in ("12000", "20250", "24000", "48000"):
        m = c.mock("--rate", rate, "--station", "14101000")
        k = c.knob("r" + rate)
        b = k.boot("save", m.rx, "radio", "14100000", "usb", "300", "2700", "choose", "0",
                   "until", "streaming", "10", "wait", "1", "pitch", "3", "quit")
        p = pitches(b)
        c.check(len(p) == 1 and abs(p[0] - 1000) <= 10, f"{rate} Hz: 1 kHz ({p})")


def backlog_one_jump(c):
    """The stream held up 3 s, then its backlog at once, the frames coming a
    little unevenly: the right ear's ring leaves the backlog out in one jump,
    in the stall's own silence, and lets no feed go -- where it used to sit
    full and drop."""
    m = c.mock("--stall", "12:3", "--jitter", "25")
    k = c.knob()
    b = k.boot("save", m.rx, "choose", "0", "until", "streaming", "10", "wait", "40", "audio",
               "state", "quit")
    a = next((l for l in b.at if l.startswith("@AUDIO")), "")
    dropped = int(re.search(r"dropped=(\d+)", a)[1]) if a else -1
    jumps = [l for l in b.lines if "left out in one jump" in l]
    c.check(b.state().get("streaming"), f"it plays on ({b.state()})")
    c.check(dropped == 0, f"no feed let go ({a})")
    c.check(len(jumps) == 1, f"one jump ({jumps})")
    c.check(all("in its silence" in j for j in jumps), f"...where the stall had silenced it: one break ({jumps})")
    c.check(m.stats()["stalls"] == 1, "one stall")


def ident_and_labels(c):
    """Beside a radio as on the kiwi firmware: who the owner sees is the
    page's name for the knob, at the login and at once to the receiver
    playing; on the dial's chooser a receiver with no name goes by its
    antenna once its /status is read -- the one chosen, never another for
    its name alone -- else by its address, whole where it fits; and a Test
    suggests a name."""
    a = c.mock("--antenna", "RF.Guru OctaLoop", "--status-delay", "1")
    bm = c.mock("--antenna", "", "--name", "RF.Guru Lombardsijde | TerraBooster")
    k = c.knob()
    ra, rb = a.rx, bm.rx
    b = k.boot("ident", "ON6URE", "save", ra, rb, "choose", "0", "label", "0", "until", "streaming", "10",
               "label", "0", "label", "1", "ident", "ON6URE/P", "wait", "1", "test", "1", "quit")
    lb = b.labels()
    c.check(lb == [(0, a.hp), (0, "OctaLoop"), (1, bm.hp)],
            f"its address and port, then its antenna; the other by its address ({lb})")
    idents = [x for t, x in sets_of(a) if x.startswith("SET ident_user=")]
    c.check(idents == ["SET ident_user=ON6URE", "SET ident_user=ON6URE%2FP"],
            f"the page's name at the login, a new one at once ({idents})")
    st = a.stats()
    c.check(st["logins"] == 1 and st["ident"] == "ON6URE/P" and st["status"] == 1,
            f"one login, one /status read ({st['logins']}, {st['status']})")
    t = b.tests()
    c.check(len(t) == 1 and t[0].get("fill") == "Lombardsijde" and bm.stats()["ident"] == "ON6URE/P",
            f"a Test: no antenna, so its name's first part, \"RF.Guru \" left off; its login says who ({t})")
    b = k.boot("label", "0", "quit")
    c.check(b.labels() == [(0, a.hp)], f"a new boot: its /status not read yet, its address ({b.labels()})")


def labels_by_host(c):
    """Receivers never read go by their addresses on the chooser, and none is
    contacted for it (nothing is chosen; the names and addresses are
    reserved ones, which reach nothing): whole where it fits; else the start
    of the host, so two on one domain are told apart -- unless another
    receiver has the same host, and then its end with the port, so those
    on one address are told apart."""
    k = c.knob()
    b = k.boot("save", "kiwi1.lan.invalid:8073", "kiwi2.lan.invalid:8073", "192.0.2.123:8074", "192.0.2.123:8075",
               "label", "0", "label", "1", "label", "2", "label", "3", "quit")
    lb = b.labels()
    c.check(lb == [(0, "kiwi1.lan.inval"), (1, "kiwi2.lan.inval"), (2, "..0.2.123:8074"), (3, "..0.2.123:8075")],
            f"one domain: by its start; one address: by its end, the port with it ({lb})")
    c.check(not any("/status" in line or "connect" in line for line in b.lines),
            f"nothing contacted for a name ({[line for line in b.lines if 'sdr:' in line]})")
    b = k.boot("save", "192.0.2.123:8074", "kiwi1.lan.invalid:8073", "label", "0", "quit")
    c.check(b.labels() == [(0, "192.0.2.123")], f"alone on its address: no port needed ({b.labels()})")


def test_beside_choice(c):
    """The page's Test of the receiver just chosen, at once -- before the
    knob has looked at it, or while it reads its /status or waits for its
    login's answer: a Test and a session never log in at once, and the
    choice, made before the Test's refusal, spends no try. One refused
    login, one strike, however the two meet."""
    for i, delay in enumerate([(), (), ("--status-delay", "0.3"), ("--answer-delay", "0.3")]):
        m = c.mock("--ip-limit-at-login", "--kiwisdr", "--ext-api", "4", *delay)
        k = c.knob(f"knob{i}")
        b = k.boot("save", m.rx, "radio", "7100000", "lsb", "-2700", "-300", "choose", "0",
                   "test", "0", "wait", "2", "marks", "quit")
        st, t = m.stats(), [x.get("login") for x in b.tests()]
        c.check(st["refused_at_login"] == 1 and b.marks()[-1:] == [{0: 1}],
                f"choose, Test{' (' + ' '.join(delay) + ')' if delay else ''}: one refused login, one strike "
                f"({st['refused_at_login']}, {b.marks()}, the Test: {t})")


# --------------------------------------------------------- TLS, and redirects

def lists(b):
    """Each @LIST line, as {entry: (host, port, tls, kport)}, grouped as asked."""
    out, cur = [], None
    for l in b.at:
        m = re.match(r"@LIST (\d+) (\S+) (\d+) tls=(\d) kport=(\d+)", l)
        if m:
            if cur is None:
                cur = {}
                out.append(cur)
            cur[int(m[1])] = (m[2], int(m[3]), m[4] == "1", int(m[5]))
        else:
            cur = None
    return out


def tls_redirect_kept(c):
    """An http:// receiver that answers with a redirect to https:// on its own
    host -- the kiwisdr.com proxy's 307, Cloudflare's 301: followed at once,
    in TLS, its session resuming the /status read's TLS; kept -- https:// from
    then on, under the key it had, in flash at a quiet moment; and after a
    restart straight to TLS, never redirected again. Each Host the one a front
    routes on."""
    for code in ("307", "301"):
        m = c.mock("--redirect", code, tls=True)
        k = c.knob(f"knob{code}")
        b = k.boot("save", f"http://127.0.0.1:{m.redirect_port}", "choose", "0", "until", "streaming", "15", "list",
                   "choose", "-1", "wait", "3", "quit")
        st = m.stats()
        c.check(b.state(0).get("streaming"), f"{code}: it plays, over TLS ({b.state(0)})")
        c.check(lists(b)[:1] == [{0: ("127.0.0.1", m.port, True, m.redirect_port)}],
                f"{code}: https:// on its port from then on, known by the port it had ({lists(b)})")
        c.check(st["redirects"] == 1, f"{code}: one redirect, the /status read's: followed ({st['redirects']})")
        c.check(st["tls"] >= 2 and st["tls_resumed"] >= 1,
                f"{code}: the session resumed the /status read's TLS ({st['tls']}, {st['tls_resumed']} resumed)")
        c.check(st["hosts"] and all(h == f"127.0.0.1:{m.port}" for h in st["hosts"]),
                f"{code}: each request names the host and port it went to ({st['hosts']})")
        b = k.boot("choose", "0", "until", "streaming", "15", "list", "quit")
        st = m.stats()
        c.check(b.state().get("streaming") and st["redirects"] == 1,
                f"{code}: after a restart, straight to TLS ({b.state()}, {st['redirects']} redirects)")
        c.check(lists(b)[-1:] == [{0: ("127.0.0.1", m.port, True, m.redirect_port)}],
                f"{code}: kept in flash ({lists(b)})")
tls_redirect_kept.once = True


def tls_redirect_elsewhere(c):
    """A redirect to another host -- or one in the clear, or a second one, from
    the receiver behind the front -- is not followed: "moved to <host>",
    nothing sent there, and held until chosen again; the list as it was."""
    m = c.mock("--redirect", "301", "--redirect-to", "https://localhost:{port}", tls=True)
    k = c.knob()
    b = k.boot("save", f"http://127.0.0.1:{m.redirect_port}", "choose", "0", "until", "moved", "12", "wait", "4",
               "state", "list", "quit")
    s, st = b.state(), m.stats()
    c.check(s.get("state") == "moved to localhost" and s.get("note") == "moved", f"said so, where to ({s})")
    c.check(st["tls"] == 0 and st["connections"] == 0, f"nothing sent there ({st['tls']}, {st['connections']})")
    c.check(st["redirects"] == 2, f"the /status read's and the session's, then held ({st['redirects']})")
    c.check(lists(b) == [{0: ("127.0.0.1", m.redirect_port, False, 0)}], f"the list as it was ({lists(b)})")
    # A second hop: the clear port's redirect followed, the front's own not.
    m2 = c.mock("--redirect", "307", "--http", "301", "--http-location", "https://127.0.0.1:{port}/", tls=True)
    k2 = c.knob("second")
    b = k2.boot("save", f"http://127.0.0.1:{m2.redirect_port}", "choose", "0", "until", "moved", "12", "state",
                "list", "quit")
    s, st = b.state(), m2.stats()
    c.check(s.get("state") == "moved to 127.0.0.1", f"the second redirect not followed ({s})")
    c.check(st["redirects"] == 1 and st["http_refusals"] == 1 and st["upgrades"] == 0,
            f"one hop, never a loop ({st['redirects']} redirects, {st['http_refusals']} answered 301)")
    c.check(lists(b)[-1:] == [{0: ("127.0.0.1", m2.port, True, m2.redirect_port)}],
            f"the first, to https:// on its own host, kept ({lists(b)})")
tls_redirect_elsewhere.once = True


def tls_certificate_refused(c):
    """A certificate that does not verify -- signed by no one the knob trusts,
    or for another name -- is refused: not a word spoken, "certificate not
    valid", held until chosen again; chosen again, one more try."""
    for cert in ("rogue", "other"):
        m = c.mock(tls=cert)
        k = c.knob(cert)
        b = k.boot("save", f"https://127.0.0.1:{m.port}", "choose", "0", "until", "certificate", "12", "wait", "4",
                   "state", "choose", "0", "wait", "3", "state", "quit")
        s, st = b.state(-2), m.stats()
        c.check(s.get("state") == "certificate not valid" and s.get("note") == "certificate",
                f"{cert}: refused, and said so ({s})")
        c.check(st["connections"] == 0 and st["upgrades"] == 0 and st["logins"] == 0,
                f"{cert}: not a word spoken to it ({st['connections']}, {st['logins']})")
        c.check(st["tls_failed"] == 3, f"{cert}: its /status and the session, then one try more when chosen "
                                       f"again ({st['tls_failed']})")
        c.check(b.state().get("state") == "certificate not valid", f"{cert}: held still ({b.state()})")
        said = [l for l in b.lines if "its certificate is not valid" in l]
        why = "signed by no authority in the knob's bundle" if cert == "rogue" else "does not match"
        c.check(bool(said) and all(why in l for l in said), f"{cert}: the log says why, \"{why}\" ({said[:1]})")
tls_certificate_refused.once = True


def tls_redirect_marks(c):
    """The owners' limits across the redirect: an http:// receiver at its day
    limit, reached through its redirect to https://, marks the receiver --
    under the key it keeps -- and after a restart it is held as it was, https://
    now: two refused logins at most, the receiver's own count never reached."""
    m = c.mock("--redirect", "307", "--ip-limit-at-login", tls=True)
    k = c.knob()
    b = k.boot("save", f"http://127.0.0.1:{m.redirect_port}", "choose", "0", "until", "daily limit", "12",
               "wait", "3", "marks", "list", "quit")
    c.check(b.marks() == [{0: 1}], f"marked, one strike ({b.marks()})")
    c.check(lists(b) == [{0: ("127.0.0.1", m.port, True, m.redirect_port)}], f"https:// kept ({lists(b)})")
    for n in range(2):
        b = k.boot("wait", "4", "state", "marks", "off")
        c.check(b.state().get("state") == "daily limit reached" and b.marks() == [{0: 1}],
                f"reboot {n + 1}: held, https:// now, its mark its own ({b.state()}, {b.marks()})")
    b = k.boot("choose", "0", "wait", "5", "choose", "0", "wait", "4", "state", "marks", "quit")
    st = m.stats()
    c.check(st["refused_at_login"] == 2 and b.marks() == [{0: 2}] and not st["barred"],
            f"two refused logins in all ({st['refused_at_login']}, {b.marks()})")
tls_redirect_marks.once = True


def tls_stale_page(c):
    """A page loaded before a redirect moved its receiver still shows the
    address it had: saved from there, no password typed, it is the same
    receiver (sdr_same) -- https:// kept, under the key it had, its password
    and time-limit password with it, in flash too; it logs in at once, and
    its day limit is never spent."""
    m = c.mock("--redirect", "307", "--ip-limit-at-login", "--ipl", "SECRET", "--password", "pw", tls=True)
    k = c.knob()
    rp = m.redirect_port
    b = k.boot("save", f"pw@127.0.0.1:{rp}/SECRET", "choose", "0", "until", "streaming", "15", "choose", "-1",
               "wait", "4", "list", "quit")
    c.check(any(s.get("streaming") for s in b.states()), f"it plays, followed to https:// ({b.states()[-2:]})")
    c.check(lists(b) == [{0: ("127.0.0.1", m.port, True, rp)}], f"https:// kept ({lists(b)})")
    line = f"\thttps://127.0.0.1\t{m.port}/{rp}\tpw\tSECRET\n"
    blob = (nvs_value(k.nvs, "vfo", "sdrs") or b"").decode(errors="replace")
    c.check(line in blob, f"in flash, both passwords with it ({blob!r})")
    # The stale page's Save: the clear address it showed, no password fields.
    b = k.boot("save", f"127.0.0.1:{rp}", "list", "choose", "0", "until", "streaming", "15", "state", "marks",
               "quit")
    st = m.stats()
    blob = (nvs_value(k.nvs, "vfo", "sdrs") or b"").decode(errors="replace")
    c.check(lists(b)[:1] == [{0: ("127.0.0.1", m.port, True, rp)}], f"the same receiver, https:// still ({lists(b)})")
    c.check(line in blob, f"its passwords kept ({blob!r})")
    c.check(b.state().get("streaming") and st["refused_at_login"] == 0 and b.marks() == [{}],
            f"in at once, never refused for its day limit ({b.state()}, {st['refused_at_login']}, {b.marks()})")
    c.check(st["redirects"] == 1, f"no second redirect: straight to TLS ({st['redirects']})")
tls_stale_page.once = True


def tls_test(c):
    """The page's Test of an http:// receiver that redirects: its /status over
    https://, its login there, and the redirect said and kept; of an https://
    one, the same over TLS; of one whose certificate does not verify, said;
    of one that fails after its redirect was followed, the redirect said with
    the failure, for the page to show, and kept."""
    m = c.mock("--redirect", "308", tls=True)
    r = c.mock(tls="rogue")
    n = c.mock("--redirect", "301", "--no-status", tls=True)
    k = c.knob()
    b = k.boot("save", f"http://127.0.0.1:{m.redirect_port}", f"https://127.0.0.1:{r.port}",
               f"http://127.0.0.1:{n.redirect_port}", "test", "0", "list", "test", "1", "test", "2", "list", "quit")
    t = b.tests()
    c.check(len(t) == 3 and t[0].get("login") == "ok" and t[0].get("tls") is True and t[0].get("port") == m.port,
            f"its login over https://, where it was sent ({t[:1]})")
    c.check(lists(b)[:1] == [{0: ("127.0.0.1", m.port, True, m.redirect_port), 1: ("127.0.0.1", r.port, True, 0),
                              2: ("127.0.0.1", n.redirect_port, False, 0)}], f"...and kept ({lists(b)[:1]})")
    c.check(len(t) == 3 and "certificate not valid" in t[1].get("error", ""), f"a certificate refused ({t[1:2]})")
    c.check(r.stats()["connections"] == 0, "not a word to the one whose certificate was refused")
    c.check(len(t) == 3 and "not a kiwi" in t[2].get("error", "") and t[2].get("tls") is True
            and t[2].get("port") == n.port, f"failed after its redirect: said, the redirect with it ({t[2:]})")
    c.check(lists(b)[-1:] and lists(b)[-1].get(2) == ("127.0.0.1", n.port, True, n.redirect_port),
            f"...and kept ({lists(b)[-1:]})")
tls_test.once = True


SCENARIOS = [day_limit_across_reboots, kiwisdr_daily_clear, test_never_double_strikes, mid_session_limit,
             idle_and_kick, selection_by_address, on_the_air, time_limit_password, slow_answer,
             list_saved_during_login, status_read_rarely, status_left_unread, full_nvs, refused_held,
             restart_loop,
             held_through_crash, hourglass_gone_but_refusing,
             cw_on_the_dial, out_of_range, out_of_range_at_start, cw_at_the_edge, s_meter_reference,
             app_path_keepalive, no_apps_from_status, silent_door, http_classed,
             pitch_at_every_rate, backlog_one_jump, ident_and_labels, labels_by_host, test_beside_choice,
             tls_redirect_kept, tls_redirect_elsewhere, tls_certificate_refused, tls_redirect_marks, tls_stale_page,
             tls_test]


def main():
    global ARGS
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", required=True, help="the sdr_host binary")
    ap.add_argument("--mock", required=True, help="tools/mock_kiwi.py")
    ap.add_argument("--only", help="run the scenarios whose name has this in it")
    ap.add_argument("--tls", action="store_true", help="every receiver behind TLS, every address https://")
    ap.add_argument("--no-tls", action="store_true", help="the harness was built without TLS: none tried")
    ap.add_argument("-v", "--verbose", action="store_true", help="print every log")
    ARGS = ap.parse_args()
    ARGS.host = os.path.abspath(ARGS.host)
    ARGS.mock = os.path.abspath(ARGS.mock)
    with tempfile.TemporaryDirectory(prefix="kiwi_limits_") as tmp:
        if not ARGS.no_tls:
            make_certs(tmp)
        chosen = tls_runs(ARGS, [s for s in SCENARIOS if not ARGS.only or ARGS.only in s.__name__])
        if chosen is None:
            return SKIPPED
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
