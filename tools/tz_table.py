#!/usr/bin/env python3
"""The configuration page's time zones: components/webcfg/www/tz.txt.

One line a zone, "<IANA name> <POSIX rule>", for every zone the system's
tzdata has, links included (a browser may name Europe/Kiev or Asia/Calcutta).
The rule is the footer of the zone's TZif file (version 2 and later): what the
zone does after its last listed change, which is what newlib's tzset() takes.
tzdata is in the public domain.

    python3 -I tools/tz_table.py [/usr/share/zoneinfo]
"""
import sys
import zoneinfo
from pathlib import Path

ROOT = Path(sys.argv[1] if len(sys.argv) > 1 else "/usr/share/zoneinfo")
OUT = Path(__file__).resolve().parent.parent / "components/webcfg/www/tz.txt"


def footer(path: Path) -> str:
    data = path.read_bytes()
    if not data.startswith(b"TZif") or data[4:5] < b"2":
        raise ValueError(f"{path}: no version 2 footer")
    rule = data.rstrip(b"\n").rsplit(b"\n", 1)[1].decode("ascii")
    if not rule or " " in rule:
        raise ValueError(f"{path}: footer {rule!r}")
    return rule


lines = []
for name in sorted(zoneinfo.available_timezones()):
    # The old System V names (EST5EDT) and the bare abbreviations (CET, EST)
    # say less than a city does; UTC stays, for those who keep the shack on it.
    if "/" not in name and name != "UTC":
        continue
    if name.startswith(("Etc/", "SystemV/", "US/", "posix", "right")):
        continue
    lines.append(f"{name} {footer(ROOT / name)}")

OUT.write_text("\n".join(lines) + "\n")
print(f"{OUT}: {len(lines)} zones, {OUT.stat().st_size} bytes")
