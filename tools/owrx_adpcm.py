#!/usr/bin/env python3
"""OpenWebRX's ADPCM, as its receivers encode it: a port of csdr's.

csdr's src/lib/adpcm.cpp (jketterl/csdr; luarvique's fork has it byte for
byte the same), line for line: AdpcmCodec (Tim Kientzle's IMA ADPCM),
AdpcmEncoder with sync -- "SYNC", the step index and the predictor as int16
little-endian, before its first byte and every 1001 bytes after -- and
FftAdpcmEncoder, the waterfall's, its codec started over each line and ten
samples of padding in front.

OpenWebRX 1.0 and 1.1 encode with csdr's older encode_ima_adpcm_i16_u8
instead: the same codec, from index 0 and predictor 0, with no SYNC at all
-- Encoder(sync=False).

The encoder's own state after each sample is what a decoder must give back
for it, so encode() returns both: the bytes the receiver sends, and the
samples the knob's decoder (components/owrx_proto/owrx_adpcm.c) must make of
them. tools/mock_owrx.py speaks with it; test/host checks the knob's decoder
against it, sample for sample.

  owrx_adpcm.py vectors OUT_STREAM OUT_EXPECT [--seconds S] [--rate R] [--plain]
      a test signal (tones, a sweep, noise, silence, full scale) encoded:
      the stream as the receiver sends it, and the samples, int16 LE;
      --plain as OpenWebRX 1.0 and 1.1 send it, without SYNCs
  owrx_adpcm.py check-csdr CSDR_BIN
      the port against csdr's own encoder, built from its source on the PC:
      CSDR_BIN reads int16 LE on stdin and writes its AdpcmEncoder(sync)'s
      bytes -- the same input must give the same bytes
"""
import math
import random
import struct
import subprocess
import sys

STEP = [
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34,
    37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143,
    157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494,
    544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552,
    1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026,
    4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442,
    11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623,
    27086, 29794, 32767,
]
ADJ = [-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8]

SYNC_EVERY = 1000            # csdr's syncCounter: 1001 bytes from one SYNC to the next
FFT_PAD = 10                 # COMPRESS_FFT_PAD_N


class Codec:
    """AdpcmCodec: its index and previousValue, from 0."""

    def __init__(self):
        self.index = 0
        self.prev = 0

    def reset(self):
        self.index = 0
        self.prev = 0

    def decode(self, code):
        step = STEP[self.index]
        diff = step >> 3
        if code & 1:
            diff += step >> 2
        if code & 2:
            diff += step >> 1
        if code & 4:
            diff += step
        if code & 8:
            diff = -diff
        self.prev += diff
        if self.prev > 32767:
            self.prev = 32767
        elif self.prev < -32768:
            self.prev = -32768
        self.index += ADJ[code]
        if self.index < 0:
            self.index = 0
        elif self.index > 88:
            self.index = 88
        return self.prev

    def encode(self, sample):
        diff = sample - self.prev
        step = STEP[self.index]
        code = 0
        if diff < 0:
            code = 8
            diff = -diff
        if diff >= step:
            code |= 4
            diff -= step
        step >>= 1
        if diff >= step:
            code |= 2
            diff -= step
        step >>= 1
        if diff >= step:
            code |= 1
        self.decode(code)
        return code


class Encoder:
    """AdpcmEncoder(sync=True): one per session's audio, for both its kinds
    of frame -- or, sync=False, OpenWebRX 1.0's encode_ima_adpcm_i16_u8.
    feed() takes int16 samples, a pair a byte; an odd one waits, as the
    server's reader holds a lone short back. Returns (bytes, the decoder's
    samples for them)."""

    def __init__(self, sync=True):
        self.codec = Codec()
        self.counter = 0 if sync else float("inf")
        self.held = []

    def feed(self, samples):
        s = self.held + list(samples)
        pairs = len(s) // 2
        self.held = s[pairs * 2:]
        out = bytearray()
        want = []
        for i in range(pairs):
            if self.counter <= 0:
                out += b"SYNC" + struct.pack("<hh", self.codec.index, self.codec.prev)
                self.counter = SYNC_EVERY
            else:
                self.counter -= 1
            lo = self.codec.encode(s[2 * i])
            want.append(self.codec.prev)
            hi = self.codec.encode(s[2 * i + 1])
            want.append(self.codec.prev)
            out.append(lo | hi << 4)
        return bytes(out), want


class Decoder:
    """The receiving end, as the knob's (owrx_adpcm.c): SYNCs found wherever
    the frames break them, the state taken from each -- or, the session's
    audio not starting with one, plain (OpenWebRX 1.0 and 1.1), a SYNC
    still looked for over the first two blocks' worth. feed() returns the
    samples; `lost` counts bytes passed over looking for a SYNC."""

    WATCH = 2 * (SYNC_EVERY + 1 + 8)

    def __init__(self):
        self.codec = Codec()
        self.st = 0                 # 0 hunting, 1 the header, 2 data, 3 plain
        self.match = 0
        self.hdr = bytearray()
        self.left = 0
        self.syncs = 0
        self.lost = 0
        self.plain = False
        self.watch = 0

    def resync(self):
        """Another SDR's encoder: its SYNC looked for -- plain, on as before."""
        if not self.plain:
            self.st, self.match = 0, 0

    def _take(self):
        idx, pred = struct.unpack("<hh", bytes(self.hdr))
        if not 0 <= idx <= 88:
            return False
        self.codec.index, self.codec.prev = idx, pred
        self.left, self.st = SYNC_EVERY + 1, 2
        self.syncs += 1
        return True

    def _to_plain(self, seen):
        for b in seen:
            self.codec.decode(b & 0x0F)
            self.codec.decode(b >> 4)
        self.plain, self.st, self.watch, self.match = True, 3, self.WATCH, 0

    def _plain(self, b, out):
        out.append(self.codec.decode(b & 0x0F))
        out.append(self.codec.decode(b >> 4))
        if not self.watch:
            return
        self.watch -= 1
        if self.match == 4:
            self.hdr.append(b)
            if len(self.hdr) == 4:
                self.match = 0
                if self._take():
                    self.plain, self.watch = False, 0
        elif b == b"SYNC"[self.match]:
            self.match += 1
            if self.match == 4:
                self.hdr = bytearray()
        else:
            self.match = 1 if b == 0x53 else 0

    def feed(self, data):
        out = []
        for b in data:
            if self.st == 0:
                if b == b"SYNC"[self.match]:
                    self.match += 1
                    if self.match == 4:
                        self.st, self.hdr = 1, bytearray()
                elif not self.syncs and not self.lost:
                    self._to_plain(b"SYNC"[:self.match])
                    self._plain(b, out)
                else:
                    self.lost += self.match + 1 - (b == 0x53)
                    self.match = 1 if b == 0x53 else 0
            elif self.st == 1:
                self.hdr.append(b)
                if len(self.hdr) == 4 and not self._take():
                    if not self.syncs and not self.lost:
                        self._to_plain(b"SYNC" + bytes(self.hdr))
                    else:
                        self.lost += 8
                        self.st, self.match = 0, 0
            elif self.st == 3:
                self._plain(b, out)
            else:
                out.append(self.codec.decode(b & 0x0F))
                out.append(self.codec.decode(b >> 4))
                self.left -= 1
                if not self.left:
                    self.st, self.match = 0, 0
        return out


def fft_line(db):
    """FftAdpcmEncoder: one waterfall line (dB per bin) as the receiver sends
    it, its codec from nothing, (short)(dB * 100) a sample."""
    c = Codec()
    out = bytearray()
    first = int(db[0] * 100)
    first = max(-32768, min(32767, first))
    for _ in range(FFT_PAD // 2):
        out.append(c.encode(first) | c.encode(first) << 4)
    for i in range(0, len(db) - 1, 2):
        a = max(-32768, min(32767, int(db[i] * 100)))
        b = max(-32768, min(32767, int(db[i + 1] * 100)))
        out.append(c.encode(a) | c.encode(b) << 4)
    return bytes(out)


def test_signal(seconds=4.0, rate=12000, seed=1):
    """Tones, a sweep, noise, silence, full scale and its clipping: what
    exercises every step of the codec."""
    rnd = random.Random(seed)
    n = int(seconds * rate)
    out = []
    for i in range(n):
        t = i / rate
        part = (i * 6) // n
        if part == 0:
            v = 9000 * math.sin(2 * math.pi * 800 * t)
        elif part == 1:
            f = 100 + 5000 * (t - seconds / 6) / (seconds / 6)
            v = 12000 * math.sin(2 * math.pi * f * t)
        elif part == 2:
            v = rnd.gauss(0, 4000)
        elif part == 3:
            v = 0
        elif part == 4:
            v = 40000 * math.sin(2 * math.pi * 300 * t)
        else:
            v = rnd.choice((-32768, 32767, 0, 1, -1))
        out.append(max(-32768, min(32767, int(v))))
    return out


def main(argv):
    if len(argv) >= 3 and argv[0] == "vectors":
        seconds, rate = 4.0, 12000
        if "--seconds" in argv:
            seconds = float(argv[argv.index("--seconds") + 1])
        if "--rate" in argv:
            rate = int(argv[argv.index("--rate") + 1])
        enc = Encoder(sync="--plain" not in argv)
        sig = test_signal(seconds, rate)
        stream = bytearray()
        want = []
        # In pieces of every size, as the receiver's reads give them.
        rnd = random.Random(7)
        i = 0
        while i < len(sig):
            k = rnd.choice((1, 2, 3, 17, 160, 999, 2000, 2001, 2002, 4000))
            b, w = enc.feed(sig[i:i + k])
            stream += b
            want += w
            i += k
        with open(argv[1], "wb") as f:
            f.write(stream)
        with open(argv[2], "wb") as f:
            f.write(struct.pack("<%dh" % len(want), *want))
        print("%d samples, %d bytes, %d SYNCs" % (len(want), len(stream), stream.count(b"SYNC")))
        return 0
    if len(argv) == 2 and argv[0] == "check-csdr":
        sig = test_signal(6.0, 12000, seed=3)
        raw = struct.pack("<%dh" % len(sig), *sig)
        got = subprocess.run([argv[1]], input=raw, stdout=subprocess.PIPE, check=True).stdout
        mine, _ = Encoder().feed(sig)
        if got != mine:
            at = next((i for i in range(min(len(got), len(mine))) if got[i] != mine[i]), None)
            print("differs from csdr: %d vs %d bytes, first at %s" % (len(got), len(mine), at))
            return 1
        print("same as csdr's: %d bytes, %d SYNCs" % (len(mine), mine.count(b"SYNC")))
        return 0
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
