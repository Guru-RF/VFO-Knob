#!/usr/bin/env python3
"""A WebSDR's audio items, both ways: an encoder written from the format as
WEBSDR-PROTOCOL.md §3 describes it, and a decoder.

The format: a message is a run of items -- the S-meter (0xF_), the sample
rate (0x81), the step (0x82), the conversion byte (0x83), silence (0x84),
A-law (0x80) and coded blocks of 128 samples (0x90-0xDF with a new coder
width I, 0x00-0x7F with the same). A coded sample is a residual: a quotient
in unary (past 15 - I zeros an escape to eight bits), a mantissa of I - 1
bits less a shrink, a sign. residual x step + step/2 corrects a prediction
from the last 20 outputs, whose taps adapt each sample and leak towards
zero; AM and FM (conversion without 0x10) add an integrator.

The encoder keeps the decoder's state as its own, so it knows each sample
the decoder will make: encode() returns the bytes and those samples.
tools/mock_wsdr.py streams with it; test/host checks the knob's decoder
(components/wsdr_proto/wsdr_codec.c) against it, sample for sample.

  wsdr_codec.py vectors OUT_STREAM OUT_EXPECT [--seconds S] [--rate R]
      a test signal through every coder width, with and without the
      integrator, at even and odd steps, with A-law, silence and S-meter
      items between: the messages (each a u32 LE length, then its bytes) and
      the samples the decoder must give, int16 LE
"""
import math
import random
import struct
import sys

SHRINK = [999, 999, 8, 4, 2, 1, 99, 99]
BLOCK = 128


def i32(v):
    v &= 0xFFFFFFFF
    return v - 0x100000000 if v & 0x80000000 else v


def i16(v):
    v &= 0xFFFF
    return v - 0x10000 if v & 0x8000 else v


def tdiv2(v):
    """v / 2 toward zero, as JavaScript's ToInt32 of a half does."""
    return -((-v) // 2) if v < 0 else v // 2


# ITU-T G.711 A-law.
def alaw_dec(c):
    a = c ^ 0x55
    seg = (a >> 4) & 7
    t = (a & 0x0F) << 4
    t = t + 8 if seg == 0 else (t + 0x108) << (seg - 1)
    return t if a & 0x80 else -t


def alaw_enc(x):
    """The code whose decoding is nearest x."""
    best, bc = None, 0
    for c in range(256):
        e = abs(alaw_dec(c) - x)
        if best is None or e < best:
            best, bc = e, c
    return bc


class State:
    """The predictor and integrator, as the decoder keeps them: the history
    doubled, so an odd step's half stays whole."""

    def __init__(self):
        self.forget()
        self.step = 0
        self.conv = 0

    def forget(self):
        self.tap = [0] * 20
        self.hist2 = [0] * 20
        self.integ = 0

    def predict(self):
        p = i32(tdiv2(sum(t * h for t, h in zip(self.tap, self.hist2))))
        return (p if p >= 0 else p + 4095) >> 12

    def j4(self):
        return i32(self.integ) >> 4

    def out_for(self, pred, res):
        """The sample a residual would give, the state untouched."""
        corr2 = 2 * res * self.step + self.step
        return i16(tdiv2(2 * pred + corr2 + 2 * self.j4()))

    def apply(self, pred, res):
        corr2 = 2 * res * self.step + self.step
        adapt = i32(tdiv2(corr2)) >> 4
        sh = 12 if self.conv & 0x10 else 14
        for i in range(19, -1, -1):
            self.tap[i] += -(self.tap[i] >> 7) + (i32(tdiv2(self.hist2[i] * adapt)) >> sh)
            if i:
                self.hist2[i] = self.hist2[i - 1]
        self.hist2[0] = 2 * pred + corr2
        out = i16(tdiv2(self.hist2[0] + 2 * self.j4()))
        if self.conv & 0x10:
            self.integ = 0
        else:
            self.integ += i32(i32(tdiv2(self.hist2[0])) << 4) >> 3
        return out


def code(res, width):
    """The bits for a residual at a coder width, and the residual the
    decoder will make of them -- a large quotient shrinks the mantissa, its
    low bits lost; None where it cannot be coded (its quotient past 255)."""
    neg = res < 0
    mag = ~res if neg else res
    q = mag >> (width - 1)
    if q > 255:
        return None
    sh = min((q >= SHRINK[width]) + (q >= SHRINK[width - 1]), width - 1)
    mant = (mag & ((1 << (width - 1)) - 1)) >> sh
    lim = 15 - width
    bits = [0] * q + [1] if q < lim else [0] * lim + [(q >> k) & 1 for k in range(7, -1, -1)]
    nm = width - 1 - sh
    bits += [(mant >> k) & 1 for k in range(nm - 1, -1, -1)]
    bits.append(1 if neg else 0)
    got = (q << (width - 1)) + (mant << sh)
    return bits, (~(got | ((1 << sh) - 1)) if neg else got)


class Encoder:
    """One stream's coder: header items when they change, blocks of 128."""

    def __init__(self, step=40, conv=0x10):
        self.st = State()
        self.st.step = step
        self.st.conv = conv
        self.width = None
        self.told = {}

    def header(self, rate=None, step=None, conv=None):
        out = bytearray()
        if rate is not None and self.told.get("rate") != rate:
            out += bytes([0x81, rate >> 8, rate & 255])
            self.told["rate"] = rate
        if step is not None:
            self.st.step = step
        if self.told.get("step") != self.st.step:
            out += bytes([0x82, self.st.step >> 8, self.st.step & 255])
            self.told["step"] = self.st.step
        if conv is not None:
            self.st.conv = conv
        if self.told.get("conv") != self.st.conv:
            out += bytes([0x83, self.st.conv])
            self.told["conv"] = self.st.conv
        return bytes(out)

    def block(self, x, width, fast=False):
        """128 samples at a width: (bytes, the decoder's samples). `fast`:
        the nearest residual alone, not the best of three -- a live
        stream's, cheaper."""
        assert len(x) == BLOCK and 1 <= width <= 5
        bits, want = [], []
        for v in x:
            pred = self.st.predict()
            target = v - self.st.j4() - pred
            r = math.floor((target - self.st.step / 2) / max(1, self.st.step) + 0.5)
            best = None
            for want_res in ((r,) if fast else (r - 1, r, r + 1)):
                c = code(want_res, width)
                if c is None:
                    continue
                b, res = c
                e = abs(self.st.out_for(pred, res) - v)
                if best is None or e < best[0]:
                    best = (e, res, b)
            if best is None:                 # past what the width codes: the largest it can
                big = (255 << (width - 1)) | ((1 << (width - 1)) - 1)
                b, res = code(~big if r < 0 else big, width)
                best = (0, res, b)
            _, res, b = best
            bits += b
            want.append(self.st.apply(pred, res))
        if width != self.width:
            head = [(((6 - width) << 4) ^ 0x80) >> k & 1 for k in range(7, 3, -1)]
            self.width = width
        else:
            head = [0]
        allbits = head + bits
        allbits += [0] * (-len(allbits) % 8)
        out = bytes(int("".join(map(str, allbits[i:i + 8])), 2) for i in range(0, len(allbits), 8))
        return out, want

    def silence(self):
        self.st.forget()
        return b"\x84", [0] * BLOCK

    def alaw(self, x):
        self.st.forget()
        c = [alaw_enc(v) for v in x]
        return b"\x80" + bytes(c), [alaw_dec(k) for k in c]


def meter(dbm):
    v = max(0, min(4095, int(round((dbm + 127) * 10))))
    return bytes([0xF0 | v >> 8, v & 255])


class Decoder:
    """The receiving end, for the mock's own self-test: whole messages in,
    samples out; rate, meter and silence seen along the way."""

    def __init__(self):
        self.st = State()
        self.width = 1
        self.rate = None
        self.meters = []
        self.silent = 0
        self.blocks = 0

    def feed(self, m):
        out = []
        n = len(m)
        at = lambda k: m[k] if k < n else 0
        i = 0
        while i < n:
            b = m[i]
            if b >= 0xF0:
                self.meters.append(((b & 15) << 8 | at(i + 1)) / 10 - 127)
                i += 2
                continue
            if b == 0x80:
                out += [alaw_dec(at(i + 1 + k)) for k in range(BLOCK)]
                self.st.forget()
                self.blocks += 1
                i += 1 + BLOCK
                continue
            if 0x90 <= b <= 0xDF or b < 0x80:
                if b >= 0x90:
                    self.width = 14 - (b >> 4)
                    pos, bit = i, 4
                else:
                    pos, bit = i, 1
                for _ in range(BLOCK):
                    win = 0
                    for k in range(4):
                        win = win << 8 | at(pos + k)
                    win = (win << bit) & 0xFFFFFFFF
                    w, lim, zeros = self.width, 15 - self.width, 0
                    if win:
                        while not win & 0x80000000 and zeros < lim:
                            win = (win << 1) & 0xFFFFFFFF
                            zeros += 1
                    if zeros < lim:
                        q, used = zeros, zeros + 1
                        win = (win << 1) & 0xFFFFFFFF
                    else:
                        q, used = win >> 24, zeros + 8
                        win = (win << 8) & 0xFFFFFFFF
                    sh = min((q >= SHRINK[w]) + (q >= SHRINK[w - 1]), w - 1)
                    res = ((win >> (33 - w)) if w > 1 else 0) & ~((1 << sh) - 1)
                    res += q << (w - 1)
                    if win & (1 << (32 - w + sh)):
                        res = ~(res | ((1 << sh) - 1))
                    bit += used + w - sh
                    pos += bit >> 3
                    bit &= 7
                    out.append(self.st.apply(self.st.predict(), res))
                self.blocks += 1
                i = pos + 1 if bit else pos
                continue
            if b == 0x81:
                self.rate = at(i + 1) << 8 | at(i + 2)
                i += 3
            elif b == 0x82:
                self.st.step = at(i + 1) << 8 | at(i + 2)
                i += 3
            elif b == 0x83:
                self.st.conv = at(i + 1)
                i += 2
            elif b == 0x84:
                out += [0] * BLOCK
                self.st.forget()
                self.silent += 1
                self.blocks += 1
                i += 1
            elif b in (0x85, 0x87):
                i += 7
            else:
                i += 1
        return out


def test_signal(n, rate, seed=1):
    """Tones, a sweep, noise, silence, near full scale: every width's use."""
    rnd = random.Random(seed)
    out = []
    for k in range(n):
        t = k / rate
        part = (k * 6) // n
        if part == 0:
            v = 6000 * math.sin(2 * math.pi * 700 * t) + 3000 * math.sin(2 * math.pi * 1900 * t)
        elif part == 1:
            v = 9000 * math.sin(2 * math.pi * (100 + 3000 * k / n) * t)
        elif part == 2:
            v = rnd.gauss(0, 3000)
        elif part == 3:
            v = rnd.gauss(0, 20)
        elif part == 4:
            v = 30000 * math.sin(2 * math.pi * 300 * t)
        else:
            v = rnd.gauss(0, 200) + 2000 * math.sin(2 * math.pi * 1000 * t)
        out.append(max(-32768, min(32767, int(v))))
    return out


def vectors(seconds, rate):
    """Messages and the samples they must decode to: every width, both
    conversions, steps even and odd, the other items between, and now and
    then two blocks in one message, as some servers send them."""
    sig = test_signal(int(seconds * rate) // BLOCK * BLOCK, rate)
    rnd = random.Random(5)
    enc = Encoder()
    msgs, want = [], []
    plan = [(40, 0x10), (40, 0x00), (128, 0x11), (41, 0x10), (7, 0x02), (512, 0x10), (40, 0x10)]
    k = 0
    while k < len(sig):
        nb = k // BLOCK
        step, conv = plan[(nb // 23) % len(plan)]
        m = bytearray(enc.header(rate=rate, step=step, conv=conv))
        if nb % 8 == 0:
            m += meter(-127 + nb % 140)
        for _ in range(2 if rnd.random() < 0.2 and k + BLOCK < len(sig) else 1):
            x = sig[k:k + BLOCK]
            r = rnd.random()
            if r < 0.03:
                b, w = enc.silence()
            elif r < 0.06:
                b, w = enc.alaw(x)
            else:
                b, w = enc.block(x, 1 + (k // BLOCK // 5) % 5)
            m += b
            want += w
            k += BLOCK
        if rnd.random() < 0.05:
            m += bytes([0x86])
        msgs.append(bytes(m))
    return msgs, want


def main(argv):
    if len(argv) >= 3 and argv[0] == "vectors":
        seconds, rate = 6.0, 7119
        if "--seconds" in argv:
            seconds = float(argv[argv.index("--seconds") + 1])
        if "--rate" in argv:
            rate = int(argv[argv.index("--rate") + 1])
        msgs, want = vectors(seconds, rate)
        # The same through this decoder first: encoder and decoder agree.
        d = Decoder()
        got = []
        for m in msgs:
            got += d.feed(m)
        if got != want:
            at = next((i for i in range(min(len(got), len(want))) if got[i] != want[i]), None)
            print("the Python decoder differs from the encoder's mirror: %d vs %d, first at %s"
                  % (len(got), len(want), at))
            return 1
        with open(argv[1], "wb") as f:
            for m in msgs:
                f.write(struct.pack("<I", len(m)) + m)
        with open(argv[2], "wb") as f:
            f.write(struct.pack("<%dh" % len(want), *want))
        nbytes = sum(len(m) for m in msgs)
        print("%d messages, %d bytes, %d samples (%.1f bit/sample)" % (len(msgs), nbytes, len(want),
                                                                      8 * nbytes / max(1, len(want))))
        return 0
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
