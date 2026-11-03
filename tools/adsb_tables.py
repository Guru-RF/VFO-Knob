#!/usr/bin/env python3
"""Build the ADS-B Radar firmware's map: its coasts, borders and airports.

The radar draws the land under the aircraft from two tables kept in its
image (components/adsb_client/tables/, embedded by its CMakeLists.txt and
read by components/vfo_core adsb.c):

  map.bin       the coasts and the borders between countries, as runs of
                points: each run its first point and the steps after it,
                and the box it lies in, so a range draws only what is near
  airports.bin  the large and medium airports with an ICAO code, sorted by
                latitude

Both from public-domain data, never from ADSB-Radar's own data/ (which says
nowhere where it is from):

  Natural Earth 1:50m, ne_50m_coastline and
  ne_50m_admin_0_boundary_lines_land
    https://github.com/nvkelso/natural-earth-vector (geojson/)
  OurAirports, airports.csv
    https://davidmegginson.github.io/ourairports-data/airports.csv

    python3 -I tools/adsb_tables.py COAST.geojson BORDERS.geojson airports.csv

writes both tables, in place. Run it only to take newer data; the tables are
committed, so a build never needs the downloads.
"""
import csv
import json
import math
import os
import struct
import sys

sys.dont_write_bytecode = True             # no __pycache__ left in tools/

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "components", "adsb_client", "tables")

E4 = 10000            # positions in 1e-4 degree, about 11 m
RUN_MAX = 128         # points a run at most: a tight box, so little is drawn unseen
STEP_MAX = 32767      # a step in int16; a longer one (the date line) starts a run

MAP_MAGIC = b"ADM1"
APT_MAGIC = b"ADA1"
COAST, BORDER = 0, 1


def lines(path):
    """Every line of a GeoJSON file of LineStrings and MultiLineStrings."""
    with open(path, encoding="utf-8") as f:
        doc = json.load(f)
    for feat in doc["features"]:
        g = feat["geometry"]
        if g is None:
            continue
        if g["type"] == "LineString":
            yield g["coordinates"]
        elif g["type"] == "MultiLineString":
            yield from g["coordinates"]


def runs(points):
    """One line as runs of at most RUN_MAX points, each step in int16; two
    runs share the point between them, so the line stays whole."""
    pts = []
    for lon, lat in ((p[0], p[1]) for p in points):
        q = (round(lat * E4), round(lon * E4))
        if not pts or q != pts[-1]:
            pts.append(q)
    run = []
    for q in pts:
        if run and (abs(q[0] - run[-1][0]) > STEP_MAX or abs(q[1] - run[-1][1]) > STEP_MAX):
            if len(run) > 1:
                yield run
            run = [q]                    # across the date line: nothing drawn
            continue
        run.append(q)
        if len(run) == RUN_MAX:
            yield run
            run = [q]
    if len(run) > 1:
        yield run


def build_map(coast, borders):
    heads, steps = [], []
    for kind, path in ((COAST, coast), (BORDER, borders)):
        for line in lines(path):
            for run in runs(line):
                lats = [p[0] for p in run]
                lons = [p[1] for p in run]
                # The box in 0.01 degree, rounded outwards.
                box = (math.floor(min(lats) / 100), math.ceil(max(lats) / 100),
                       math.floor(min(lons) / 100), math.ceil(max(lons) / 100))
                heads.append((run[0][0], run[0][1], *box, len(steps), len(run) - 1, kind))
                for a, b in zip(run, run[1:]):
                    steps.append((b[0] - a[0], b[1] - a[1]))
    out = bytearray(MAP_MAGIC)
    out += struct.pack("<II", len(heads), len(steps))
    for h in heads:
        out += struct.pack("<iihhhhIHBx", *h)
    for s in steps:
        out += struct.pack("<hh", *s)
    return bytes(out), len(heads), len(steps)


def icao(row):
    for k in ("icao_code", "gps_code"):
        c = (row.get(k) or "").strip().upper()
        if len(c) == 4 and c.isalnum() and c.isascii():
            return c
    return None


def build_airports(path):
    recs = []
    with open(path, encoding="utf-8", newline="") as f:
        for row in csv.DictReader(f):
            if row["type"] not in ("large_airport", "medium_airport"):
                continue
            code = icao(row)
            if not code:
                continue
            lat = round(float(row["latitude_deg"]) * E4)
            lon = round(float(row["longitude_deg"]) * E4)
            recs.append((lat, lon, code.encode(), 1 if row["type"] == "large_airport" else 0))
    recs.sort()
    out = bytearray(APT_MAGIC)
    out += struct.pack("<I", len(recs))
    for lat, lon, code, large in recs:
        out += struct.pack("<ii4sB3x", lat, lon, code, large)
    return bytes(out), len(recs)


def main(argv):
    if len(argv) != 4:
        sys.exit(__doc__)
    os.makedirs(OUT, exist_ok=True)
    m, nh, ns = build_map(argv[1], argv[2])
    a, na = build_airports(argv[3])
    for name, data in (("map.bin", m), ("airports.bin", a)):
        with open(os.path.join(OUT, name), "wb") as f:
            f.write(data)
    print(f"map.bin: {nh} runs, {ns} steps, {len(m)} bytes; "
          f"airports.bin: {na} airports, {len(a)} bytes")


if __name__ == "__main__":
    main(sys.argv)
