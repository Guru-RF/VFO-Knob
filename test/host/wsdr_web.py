#!/usr/bin/env python3
"""The websdr firmware's configuration and radio pages, in a browser:
wsdr_web_host (components/webcfg and components/wsdr_client on the PC, the
knob's own pages embedded as the firmware embeds them) and headless Chromium,
driven over its DevTools protocol, against tools/mock_wsdr.py -- what a click
on the page does, and what the site is sent of it. The browser and the knob
are driven as owrx_web.py drives them (its classes, imported).

  config    the receivers' list: WebSDR's own words, no port box, the
            listener name; an address pasted whole, cut to what the knob
            keeps; Test reads the mock's page -- its bands or its band plan,
            its idle timeout -- names a receiver that has no name, and never
            opens a stream; a dead address said as such; Save keeps the
            list and the name, the site told the name at once; another In
            use and Save, taken over at once
  radio     the radio page on the site: its name, its audio's rate, the
            S-meter in dBm and S-units; a one-band site's band plan as its
            bands, the dial onto one; a frequency sent on the site's step;
            mode, passband and the squelch (on or off) sent; SAM on Twente's
            server only; another site's own bands; the receivers, another
            taken over at once

  wsdr_web.py --host BIN --mock tools/mock_wsdr.py [--chromium PATH] [--only a,b] [-v]

Loopback only: run it in a network namespace of its own."""
import argparse
import json
import os
import shutil
import subprocess
import sys
import threading
import time
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.dont_write_bytecode = True
import owrx_web as ow  # noqa: E402  the browser and the knob, as there

ARGS = None
FAILS = []


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what, flush=True)
    if not cond:
        FAILS.append(what)


class Mock:
    def __init__(self, *flags):
        self.p = subprocess.Popen([sys.executable, ARGS.mock, "--port", "0", *flags],
                                  stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        self.port = None
        while True:
            line = self.p.stdout.readline()
            if not line:
                raise RuntimeError("the mock did not start")
            w = line.split()
            if w and w[0] == "LISTENING":
                self.port = int(w[1])
                break
        threading.Thread(target=lambda: [None for _ in self.p.stdout], daemon=True).start()
        self.url = f"http://127.0.0.1:{self.port}/"

    def get(self, path):
        with urllib.request.urlopen(f"http://127.0.0.1:{self.port}{path}", timeout=5) as r:
            return json.loads(r.read())

    def stats(self):
        return self.get("/mock/stats")

    def sent(self):
        return [x["text"] for x in self.get("/mock/sent")]

    def tunings(self):
        return [t for t in self.sent() if "f=" in t]

    def last(self, key):
        """The last value sent for `key`, from the last frame that had it."""
        for t in reversed(self.sent()):
            q = dict(kv.split("=", 1) for kv in t.split("?", 1)[-1].split("&") if "=" in kv)
            if key in q:
                return q[key]
        return None

    def stop(self):
        self.p.terminate()
        try:
            self.p.wait(5)
        except subprocess.TimeoutExpired:
            self.p.kill()


# ------------------------------------------------------------------ the cases

def case_config():
    tw = Mock("--site", "twente")
    mb = Mock("--site", "maasbree")
    k = ow.Knob(tw.url)
    b = ow.Browser(k)
    try:
        check(b.go("/config", "$('rtitle').textContent==='WebSDR receivers'&&document.querySelectorAll('#rlist .sdr').length"),
              "the page: WebSDR receivers")
        check(b.js("!$('wsdrnote').hidden&&$('owrxnote').hidden&&!$('wsdrwhorow').hidden"),
              "WebSDR's note, and the listener name's box")
        check(b.js("document.querySelectorAll('#rlist input[data-k=port]').length") == 0, "no port box: the address whole")
        up0 = tw.stats()["upgrades"]
        res = ow.row_test(b, 0)
        check(res and res.startswith("✓ WebSDR, 1 band (and a band plan of 13), no idle timeout"),
              f"Test: its bands and band plan: {res!r}")
        check(ow.row_val(b, 0, "name") == "WebSDR", "named from its page's title")
        check(tw.stats()["upgrades"] == up0 and tw.stats()["bandinfo"] >= 2, "Test opened no stream")
        # Another, pasted with its page's name and a fragment.
        b.js("$('radd').click()")
        ow.row_set(b, 1, "host", f"http://127.0.0.1:{mb.port}/index.html#fq=7074")
        check(ow.row_val(b, 1, "host") == mb.url, f"cut to what the knob keeps: {ow.row_val(b, 1, 'host')!r}")
        res = ow.row_test(b, 1)
        check(res and "8 bands" in res and "lets a listener go after 4 hours idle" in res,
              f"Test: its eight bands, its idle timeout: {res!r}")
        b.js("$('radd').click()")
        ow.row_set(b, 2, "host", "http://127.0.0.1:9/")
        res = ow.row_test(b, 2)
        check(res and res.startswith("✗ no answer there"), f"a dead address: {res!r}")
        b.js("document.querySelectorAll('#rlist .sdr')[2].querySelector('[data-act=rm]').click()")
        b.js("$('wsdrwho').value='ON6URE';$('wsdrwho').dispatchEvent(new Event('input'))")
        msg = ow.save(b)
        check(msg and msg.startswith("Saved"), f"saved: {msg!r}")
        r = k.radios()
        check(r["ident"] == "ON6URE" and [x["host"] for x in r["list"]] == [tw.url, mb.url],
              f"the list and the name kept: {r.get('ident')} {[x['host'] for x in r['list']]}")
        check(k.until(lambda: "ON6URE" in tw.stats()["names"], 10, "the name sent"),
              f"the site told the name at once, with a tuning: {tw.stats()['names']}")
        check(k.radio()["ready"] and tw.stats()["upgrades"] == 1, "...the stream not opened again for it")
        # Another In use, and Save: taken over at once.
        b.go("/config", "document.querySelectorAll('#rlist .sdr').length===2")
        b.js("document.querySelectorAll('#rlist .sdr')[1].querySelector('input[name=ruse]').click()")
        msg = ow.save(b)
        check(msg and msg.startswith("Saved"), f"saved: {msg!r}")
        check(k.until(lambda: k.radios()["sel"] == 1 and k.radio()["ready"] and mb.stats()["upgrades"] == 1, 20,
                      "the second"), "the second taken over at once")
        st = b.until("document.querySelectorAll('#rlist .sdr')[1].querySelector('.rxst').textContent.startsWith('in use \u00b7 p')"
                     "&&document.querySelectorAll('#rlist .sdr')[1].querySelector('.rxst').textContent", 10, "its state")
        check(st and st.startswith("in use · playing"), f"...as its row says: {st!r}")
    finally:
        b.stop()
        k.stop()
        tw.stop()
        mb.stop()


def case_radio():
    tw = Mock("--site", "twente")
    mb = Mock("--site", "maasbree")
    k = ow.Knob(f"Twente={tw.url}", f"Maas={mb.url}")
    b = ow.Browser(k)
    try:
        check(k.until(lambda: k.radio()["ready"], 20, "the knob to play"), "the knob plays")
        check(b.go("/", "typeof R!=='undefined'&&R&&R.wsdr&&R.ready"), "the radio page on the site")
        check(b.text("#model") == "WebSDR", f"WebSDR: {b.text('#model')!r}")
        info = b.until("$('kiwiinfo').textContent.includes('kHz audio')&&$('kiwiinfo').textContent", 10, "the info")
        check(info == "WebSDR · 7.119 kHz audio", f"its name, its audio's rate: {info!r}")
        check(b.js("$('agcbox').hidden&&$('gainbox').hidden&&$('ritsec').hidden&&$('microw').hidden"),
              "no AGC, gain, RIT or microphone")
        check(b.js("!$('apiwband').hidden&&!$('apirecv').hidden&&$('apiband').hidden"), "the API: band= and receiver=")
        names = b.js("[...$('bands').children].map(e=>e.textContent)")
        check(len(names) == 13 and names[0] == "2200 m" and "49 m BC" in names and names[-1] == "10 m",
              f"its band plan as its bands: {names}")
        b.js("[...$('bands').children].find(e=>e.textContent==='20 m').click()")
        check(b.until("R.wsdr.band_sel===8&&R.freq===14175000", 10, "20 m"), "20 m: the dial onto its middle")
        check(b.until("$('bandnote').textContent.startsWith('20 m · hf')", 5, "the note"),
              f"the band and the site's own: {b.text('#bandnote')!r}")
        check(k.until(lambda: (tw.last("f") or "").startswith("14175.0"), 5, "the tuning"),
              f"...sent: f={tw.last('f')}")
        b.js("$('fin').value='14.074';$('fform').requestSubmit()")
        check(b.until("R.freq===14074000", 5, "14.074"), "a frequency, tuned")
        check(k.until(lambda: abs(float(tw.last("f") or 0) * 1000 - 14074000) <= 7, 5, "on the site's step"),
              f"...sent on the site's 6.952 Hz step: f={tw.last('f')}")
        sv = b.until("/^S9.*dBm$/.test($('sval').textContent)&&$('sval').textContent", 10, "a reading on the station")
        check(sv and sv.startswith("S9") and "dBm" in sv, f"the S-meter, S-units and dBm, on the station: {sv!r}")

        check(b.js("!$('modes').querySelector('[data-v=sam]').hidden"), "SAM on Twente's server")
        b.js("$('modes').querySelector('[data-v=am]').click()")
        check(b.until("R.mode==='am'", 5, "AM") and k.until(lambda: tw.last("mode") == "1", 5, "mode=1"), "AM, sent")
        ws = b.js("[...$('filters').children].map(e=>e.textContent)")
        check(ws == ["5.0k", "6.0k", "8.0k", "9.0k", "10.0k", "12.0k"], f"AM's passbands, as the knob's: {ws}")
        b.js("[...$('filters').children].find(e=>e.textContent==='6.0k').click()")
        check(b.until("R.lo===-3000&&R.hi===3000", 5, "6 kHz"), "6 kHz around the carrier")
        check(k.until(lambda: (tw.last("lo"), tw.last("hi")) == ("-3", "3"), 5, "sent"), "...sent in kHz")
        b.js("$('modes').querySelector('[data-v=cw]').click()")
        b.until("R.mode==='cw'", 5, "CW")
        b.js("[...$('filters').children].find(e=>e.textContent==='500').click()")
        check(b.until("R.lo===-1000&&R.hi===-500", 5, "CW 500"), "CW: 500 Hz around its 750 Hz tone")
        check(b.js("!$('sqbox').hidden&&$('sqv').textContent") == "open", "the squelch, open")
        b.js("$('sq').value=60;$('sq').dispatchEvent(new Event('input'));$('sq').dispatchEvent(new Event('change'))")
        check(b.text("#sqv") == "on", f"...on as it moves: {b.text('#sqv')!r}")
        check(k.until(lambda: tw.last("squelch") == "1", 5, "squelch=1"), "the site's squelch on")
        st = tw.stats()
        check(not st["unknown_keys"] and not st["bad_values"] and not st["outside"] and st["too_fast"] == 0,
              f"all it was sent known, none too fast: {st['bad_values']} {st['too_fast']}")

        # The other site: its own bands, no SAM.
        check(b.js("[...$('radios').children].map(e=>e.textContent).join('|')") == "Twente|Maas", "the receivers")
        b.js("$('radios').children[1].click()")
        check(b.until("R.ready&&R.wsdr.bands.length===8", 20, "the second"), "the other site, taken over at once")
        check(b.until("$('modes').querySelector('[data-v=sam]').hidden", 5, "no SAM"), "no SAM on a distributed server")
        names = b.js("[...$('bands').children].map(e=>e.textContent)")
        check(names == ["160m", "80m", "60m", "40m", "30m", "20m", "17m", "15m"], f"its own bands: {names}")
        b.js("[...$('bands').children].find(e=>e.textContent==='40m').click()")
        check(k.until(lambda: mb.last("band") == "3", 5, "band=3"), "40m: band 3 sent")
        check(tw.stats()["upgrades"] == 1 and mb.stats()["upgrades"] == 1, "one stream each, no more")
    finally:
        b.stop()
        k.stop()
        tw.stop()
        mb.stop()


CASES = ["config", "radio"]


def main():
    global ARGS
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", required=True)
    ap.add_argument("--mock", required=True)
    ap.add_argument("--chromium", default=shutil.which("chromium") or shutil.which("chromium-browser")
                    or shutil.which("google-chrome") or "")
    ap.add_argument("--only", default="")
    ap.add_argument("-v", "--verbose", action="store_true")
    ARGS = ap.parse_args()
    ow.ARGS = ARGS                      # the knob and the browser read theirs from it
    if not ARGS.chromium:
        print("no Chromium to drive the pages: skipped")
        return 77
    for c in [c for c in ARGS.only.split(",") if c] or CASES:
        print(f"{c}:", flush=True)
        globals()["case_" + c]()
    print(f"{len(FAILS)} failed" if FAILS else "all passed")
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
