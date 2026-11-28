#!/usr/bin/env python3
"""Build the AIS Radar firmware's harbours: ports.bin.

The radar draws the coasts and borders from ADS-B Radar's map.bin
(tools/adsb_tables.py, components/adsb_client/tables/), and the harbours
from this table, kept in its image (components/ais_client/tables/, embedded
by its CMakeLists.txt and read by components/vfo_core ais.c):

  ports.bin   every port, sorted by latitude: its position in 1e-4 degree
              and its name, the local one ("Oostende (Ostend)" is Oostende)

From public-domain data: Natural Earth 1:10m, ne_10m_ports
    https://github.com/nvkelso/natural-earth-vector (geojson/)

    python3 -I tools/ais_tables.py ne_10m_ports.geojson

writes the table, in place. Run it only to take newer data; the table is
committed, so a build never needs the download.
"""
import json
import os
import struct
import sys

sys.dont_write_bytecode = True             # no __pycache__ left in tools/

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "components", "ais_client", "tables")

E4 = 10000            # positions in 1e-4 degree, about 11 m
NAME = 20             # bytes a name, its NUL in them
MAGIC = b"AIP1"


def short(name):
    """The local name, without its English one in brackets, in ASCII the
    face's font has, cut to fit."""
    n = name.split(" (")[0].strip()
    n = n.encode("ascii", "replace").decode("ascii").replace("?", "")
    return n[:NAME - 1]


def main(path):
    with open(path, encoding="utf-8") as f:
        doc = json.load(f)
    rows = []
    for feat in doc["features"]:
        g = feat["geometry"]
        name = (feat["properties"].get("name") or "").strip()
        if not g or g["type"] != "Point" or not name:
            continue
        lon, lat = g["coordinates"][:2]
        n = short(name)
        if n:
            rows.append((round(lat * E4), round(lon * E4), n))
    rows.sort()
    out = bytearray(MAGIC + struct.pack("<I", len(rows)))
    for lat, lon, n in rows:
        out += struct.pack("<ii", lat, lon) + n.encode("ascii").ljust(NAME, b"\0")
    os.makedirs(OUT, exist_ok=True)
    with open(os.path.join(OUT, "ports.bin"), "wb") as f:
        f.write(out)
    print(f"ports.bin: {len(rows)} ports, {len(out)} bytes")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    main(sys.argv[1])
