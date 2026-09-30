#!/usr/bin/env python3
"""Screenshot of the face, over the serial console.

    tools/snap.py [port] [out.png]

Sends 'x' to a build with the console (not the USB-networking build: that
has none), collects the frame ui_screenshot() prints, and writes a PNG.
The port is opened without toggling DTR/RTS, so the device is not reset --
the picture is of the face as it is, not of a fresh boot.
"""
import base64
import glob
import struct
import sys
import time
import zlib

import serial


def find_port():
    ports = sorted(glob.glob('/dev/cu.usbmodem*') + glob.glob('/dev/ttyACM*'))
    if not ports:
        sys.exit('no serial port found; pass one')
    return ports[0]


def png(path, w, h, rgb):
    def chunk(tag, data):
        c = struct.pack('>I', len(data)) + tag + data
        return c + struct.pack('>I', zlib.crc32(tag + data) & 0xFFFFFFFF)
    rows = b''.join(b'\0' + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n')
        f.write(chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)))
        f.write(chunk(b'IDAT', zlib.compress(rows, 9)))
        f.write(chunk(b'IEND', b''))


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else find_port()
    out = sys.argv[2] if len(sys.argv) > 2 else 'snap.png'

    s = serial.Serial()
    s.port, s.baudrate, s.timeout = port, 115200, 0.5
    # Opening leaves DTR and RTS asserted together, which the S3's USB-JTAG
    # takes as no reset; setting them one at a time passes through the
    # reset combination on the way.
    s.open()
    # A byte written the instant the port opens is lost: give the USB link a
    # moment before asking.
    time.sleep(1.0)
    s.reset_input_buffer()
    s.write(b'x')

    data, size, lines = b'', None, 0
    deadline = time.time() + 20
    buf = b''
    while time.time() < deadline:
        buf += s.read(65536)
        while b'\n' in buf:
            line, buf = buf.split(b'\n', 1)
            line = line.strip()
            if line.startswith(b'VFO-SNAP-BEGIN'):
                _, w, h, fmt = line.split()
                size = (int(w), int(h))
                data, lines = b'', 0
            elif line.startswith(b'SNAP:') and size:
                data += base64.b64decode(line[5:])
                lines += 1
            elif line == b'VFO-SNAP-END' and size:
                w, h = size
                if len(data) != w * h * 2:
                    sys.exit(f'got {len(data)} bytes, expected {w * h * 2}')
                rgb = bytearray(w * h * 3)
                for i, (v,) in enumerate(struct.iter_unpack('<H', data)):
                    rgb[i * 3] = (v >> 11 & 0x1F) * 255 // 31
                    rgb[i * 3 + 1] = (v >> 5 & 0x3F) * 255 // 63
                    rgb[i * 3 + 2] = (v & 0x1F) * 255 // 31
                png(out, w, h, bytes(rgb))
                print(f'{out}: {w}x{h}')
                return
    sys.exit('no screenshot within 20 s (is this a build with the console?)')


if __name__ == '__main__':
    main()
