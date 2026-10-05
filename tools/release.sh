#!/usr/bin/env bash
# Build a signed image and stage it for OTA.
#
# Firmware is published under firmware/ and fetched over
# raw.githubusercontent.com rather than as a GitHub release asset. That is not
# a preference: release assets redirect to release-assets.githubusercontent.com,
# which sends no Access-Control-Allow-Origin, and the configuration page has to
# be able to fetch the image itself. Over the USB cable the knob is the DHCP
# server on a link with no gateway, so it cannot reach GitHub at all and the
# browser must do the downloading. raw.githubusercontent.com sends "*".
#
#   tools/release.sh 1.2.3                  stage locally
#   tools/release.sh 1.2.3 --push           stage and push the firmware branch
#   RADIO=icom tools/release.sh 1.2.3 ...   another radio's firmware
#
#   RADIO=companion tools/release.sh 1.2.3
#                                           the second chip's: staged, and the
#                                           bench to do, as it then says
#   BENCH_LOG=<log> RADIO=companion tools/release.sh 1.2.3 --push
#                                           published, with the bench's log
#
# One firmware per radio, all on the same version, each in its own channel:
# firmware/<radio>/ holds vfo-knob-<radio>-<version>.bin and a manifest naming
# the project, and a knob follows only its own radio's channel. RADIO defaults
# to aethersdr.
#
# The second chip's firmware (companion/, the Bluetooth headset's) has a
# channel too, firmware/companion/, which every radio's firmware reads: the
# knob sends a newer release to its second chip by itself, at a quiet moment.
# A broken one there would leave every knob's second chip waiting for a
# cable, so its release goes in two steps with the bench between them
# (companion_stage, companion_gate), and is released only when it changed,
# on the same vX.Y.Z tags. Its first release also publishes
# firmware/companion/bench/ -- the bootloader, partition table and empty
# otadata a cable writes once per knob (tools/install-setup.sh) -- never to
# change after that.
#
# The firmware branch is kept ORPHAN so 1.7 MB per release never lands in the
# history of main.
set -euo pipefail

VER="${1:-}"
[ -n "$VER" ] || { echo "usage: $0 <version> [--push]" >&2; exit 1; }
[[ "$VER" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || { echo "version must be x.y.z" >&2; exit 1; }
PUSH="${2:-}"
RADIO="${RADIO:-aethersdr}"
[[ "$RADIO" =~ ^[a-z0-9]+$ ]] || { echo "RADIO must be [a-z0-9]" >&2; exit 1; }
# build_usbnet has always been the AetherSDR firmware's; any other radio's
# gets a directory of its own, or its configuration would be the last one's,
# and its own overlay: sdkconfig.usbnet is the USB build, which a radio reached
# over WiFi has no use for. The second chip's is its own project, built
# afresh in a directory of its own each release.
BUILD="build_usbnet"
OVERLAY="sdkconfig.usbnet"
if [ "$RADIO" = companion ]; then
    BUILD="companion/build_release"
    OVERLAY=""
elif [ "$RADIO" != aethersdr ]; then
    BUILD="build_$RADIO"
    OVERLAY="sdkconfig.$RADIO"
fi
[ -z "$OVERLAY" ] || [ -f "$OVERLAY" ] || { echo "$OVERLAY is missing" >&2; exit 1; }

# The setup firmware's list -- index.json's "firmwares", rebuilt with every
# release below -- in this order, with these names, and no others: a channel
# not named here is left out, so nothing reaches the dial by accident, the
# second chip's above all, which no knob may install as its own. So a radio
# is released only once it is named here: published unnamed, its knobs would
# update, and no new knob could ever choose it.
NAMES='{"aethersdr": "AetherSDR", "icom": "Icom", "multiflex": "FlexRadio", "ubersdr": "UberSDR",
        "svxconnect": "SVXConnect", "phone": "Telephone", "setup": "Setup"}'
if [ "$RADIO" != companion ] &&
   ! python3 -c 'import json, sys; sys.exit(sys.argv[2] not in json.loads(sys.argv[1]))' "$NAMES" "$RADIO"; then
    echo "$RADIO has no name in NAMES (tools/release.sh): the setup firmware would never offer it" >&2
    echo "-- name it there first" >&2
    exit 1
fi

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

[ -f ota_signing_key.pem ] || {
    echo "ota_signing_key.pem is missing -- without it nothing already in the" >&2
    echo "field can ever be updated again. Restore it from your backup." >&2
    exit 1; }

# The image takes its version from `git describe`, so the release commit must
# be tagged and the tree clean. An image that calls itself anything else is
# installed, still reads as older than the manifest, and is offered again on
# every check, forever. (The second chip's takes it from the release overlay,
# but is built from the tagged commit all the same.)
DESC=$(git describe --tags --dirty --always)
[ "$DESC" = "v$VER" ] || {
    echo "HEAD describes as $DESC, not a clean v$VER -- tag it first" >&2
    exit 1; }
# --dirty sees only what git tracks. A source, a test or a guide's picture
# never added would be built into these images from the tree, yet be missing
# from the tag that is to rebuild them -- and from the guides the website
# pulls. Only where the firmware and its guides live: notes of one's own at
# the top are not looked at.
LEFT_OUT=$(git ls-files --others --exclude-standard -- components main companion test docs tools \
           CMakeLists.txt partitions.csv 'sdkconfig.*' 'dependencies*.lock')
[ -z "$LEFT_OUT" ] || {
    echo "untracked -- so not in v$VER -- yet part of this release:" >&2
    sed 's/^/    /' <<<"$LEFT_OUT" >&2
    echo "-- commit them with the release (or ignore them) and tag again" >&2
    exit 1; }

verify_signature() {
    echo "==> verifying the signature before publishing it"
    espsecure.py verify_signature --version 2 --keyfile ota_signing_key.pem "$1" \
        | grep -q "verification successful" \
        || { echo "image is not signed with this key -- refusing to publish" >&2; exit 1; }
}

# The second chip's image as the knob (ota_companion_image_ok) and the chip
# take it: an ESP32's, the second chip's project, this release's version,
# for a chip of revision 3 or later (the first to check a signature in the
# app), whole 4 kB sectors that fit its slot, ending in its signature
# sector. Prints its identity: the first 16 hex of its app_elf_sha256, as
# the knob's log names it.
companion_image() {
    python3 - "$1" "v$VER" <<'PY'
import struct, sys
b = open(sys.argv[1], "rb").read()
n, why = len(b), None
if n < 2 * 4096 or b[0] != 0xE9:
    why = "not an ESP image"
elif struct.unpack_from("<H", b, 12)[0] != 0:
    why = "not an ESP32's (chip id %d)" % struct.unpack_from("<H", b, 12)[0]
elif struct.unpack_from("<H", b, 15)[0] < 300:
    why = "it would start on a chip below revision 3"
elif b[32:36] != bytes.fromhex("3254cdab"):
    why = "it has no app description"
elif b[80:112].split(b"\0")[0] != b"vfo-knob-companion":
    why = "it is another project's"
elif b[48:80].split(b"\0")[0].decode(errors="replace") != sys.argv[2]:
    why = "it says it is " + b[48:80].split(b"\0")[0].decode(errors="replace")
elif n % 4096 or n > 0x1E0000:
    why = "%d bytes: not whole 4 kB sectors within its 0x1E0000 slot" % n
elif b[n - 4096] != 0xE7:
    why = "it has no signature sector"
if why:
    sys.exit("the image is not a release of the second chip's firmware: " + why)
print(b[176:184].hex())
PY
}

# The second chip's firmware, step 1: built afresh with the release overlay
# -- its version and the release mark set here, never by git describe -- and
# checked as the knob, the chip and the bench will check it. Staged, with its
# bench files and what the bench must show; nothing is published.
companion_stage() {
    rm -rf "$BUILD"
    mkdir -p "$BUILD"
    cat > "$BUILD/release.defaults" <<CFG
# tools/release.sh: a release, which the knob keeps up to date by itself.
CONFIG_VFO_COMPANION_RELEASE=y
CONFIG_APP_PROJECT_VER_FROM_CONFIG=y
CONFIG_APP_PROJECT_VER="v$VER"
CFG
    echo "==> building the second chip's firmware $VER"
    idf.py -C companion -B "$BUILD" -D COMPANION_DEFAULTS="$ROOT/$BUILD/release.defaults" build >/dev/null

    # What lets a chip go back to the firmware before, and check what it is
    # sent: written by a cable once per knob, never by an update, so a
    # release without it is refused (as companion/CMakeLists.txt refuses it).
    # And the bootloader's RTC watchdog kept on into the app, which a
    # firmware on trial feeds: without it, one hung on trial never goes back.
    local c
    for c in BOOTLOADER_APP_ROLLBACK_ENABLE=y BOOTLOADER_PROJECT_VER=2 SECURE_SIGNED_APPS_RSA_SCHEME=y \
             SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT=y BOOTLOADER_WDT_DISABLE_IN_USER_CODE=y ESP32_REV_MIN_3=y \
             VFO_COMPANION_RELEASE=y BT_A2DP_ENABLE=y; do
        grep -qx "CONFIG_$c" "$BUILD/sdkconfig" || {
            echo "its sdkconfig has no CONFIG_$c -- refusing to stage it" >&2; exit 1; }
    done
    local bin="$BUILD/vfo-knob-companion.bin" app16
    app16=$(companion_image "$bin")
    # The bench files, as tools/install-setup.sh will take them: its own
    # check of the bootloader, the partition table and the empty otadata.
    python3 tools/knob-card.py chip "$BUILD" "$BUILD/bench-parts.json" "$BUILD"
    verify_signature "$bin"

    # The partition table is frozen once a knob has it: an update never
    # writes it, so every release must fit the one the bench wrote. The
    # published one decides -- here, and again as it is published.
    local note="its first release: these bench files go with it, never to change"
    git fetch origin firmware:firmware >/dev/null 2>&1 || true
    if git cat-file -e firmware:firmware/companion/bench/partition-table.bin 2>/dev/null; then
        git show firmware:firmware/companion/bench/partition-table.bin >"$BUILD/published-table.bin"
        cmp -s "$BUILD/published-table.bin" "$BUILD/partition_table/partition-table.bin" || {
            echo "its partition table is not the one every second chip has" >&2
            echo "(firmware/companion/bench/ on the firmware branch) -- refusing to stage it" >&2
            exit 1; }
        note="the bench files published with its first release stand; its partition table is theirs"
    fi

    mkdir -p "$DIR/bench"
    cp "$bin" "$DIR/$OUT"
    local sha size app
    sha=$(sha256sum "$DIR/$OUT" | cut -d' ' -f1)
    size=$(stat -c%s "$DIR/$OUT")
    app=$(python3 -c 'import sys; print(open(sys.argv[1], "rb").read()[176:208].hex())' "$DIR/$OUT")
    # app_sha256 is its identity, bytes 176-207: the knob knows by it, before
    # any download, what its second chip runs, and what it will not take again.
    cat > "$DIR/manifest.json" <<JSON
{
  "project": "vfo-knob-companion",
  "version": "$VER",
  "file": "$OUT",
  "sha256": "$sha",
  "app_sha256": "$app",
  "size": $size
}
JSON
    # Staged with every release, so that its publishing can hold it to the
    # published ones; published only with the first.
    cp "$BUILD/bootloader/bootloader.bin" "$BUILD/partition_table/partition-table.bin" \
       "$BUILD/ota_data_initial.bin" "$DIR/bench/"
    python3 - "$DIR/bench" "$VER" <<'PY'
import hashlib, json, os, sys
d, ver = sys.argv[1], sys.argv[2]
at = {"bootloader.bin": "0x1000", "partition-table.bin": "0x8000", "ota_data_initial.bin": "0xe000"}
files = {}
for name, off in at.items():
    data = open(os.path.join(d, name), "rb").read()
    files[name] = {"offset": off, "sha256": hashlib.sha256(data).hexdigest(), "size": len(data)}
boot = int.from_bytes(open(os.path.join(d, "bootloader.bin"), "rb").read()[36:40], "little")
with open(os.path.join(d, "manifest.json"), "w") as f:
    json.dump({"project": "vfo-knob-companion", "release": ver, "bootloader_version": boot,
               "files": files}, f, indent=2)
    f.write("\n")
PY

    echo "==> staged $DIR/$OUT ($((size/1024)) KB): v$VER [$app16]"
    cat "$DIR/manifest.json"
    echo "==> $note"
    cat <<EOF
==> not published. First the bench, on a knob whose second chip already takes
    updates (docs/headset.md, For developers), its log kept from before step 1:
      nc <knob> 3333 | tee bench.log
    1. Hand it over (with ?force=1 if the chip runs a development build):
         curl -u admin:<password> -H "Expect:" --data-binary @$DIR/$OUT \\
              http://<knob>/api/bt/update
       and wait for: second chip: update from ... to v$VER [$app16]: kept
    2. From it, any other signed second-chip build -- a test release below
       this one goes back to this one by itself, once it is out:
         curl -u admin:<password> -H "Expect:" --data-binary @<that image> \\
              'http://<knob>/api/bt/update?force=1'
       and wait for: second chip: update from v$VER [$app16] to ...: kept
    3. Publish it:
         BENCH_LOG=bench.log RADIO=companion tools/release.sh $VER --push
EOF
}

# The second chip's firmware, step 2: nothing is built again. The very image
# staged, once the knob's log (bt_link.c, kept()) shows that it went onto a
# second chip over the link and was kept, and that it then took an update
# itself and kept that one: both by its identity, so no other build of the
# same version passes for it.
companion_gate() {
    [ -f "$DIR/$OUT" ] && [ -f "$DIR/manifest.json" ] && [ -f "$DIR/bench/manifest.json" ] || {
        echo "$DIR/$OUT is not staged: RADIO=companion $0 $VER first" >&2; exit 1; }
    local sha app16
    sha=$(sha256sum "$DIR/$OUT" | cut -d' ' -f1)
    python3 - "$DIR" "$VER" "$OUT" "$sha" <<'PY'
import hashlib, json, os, sys
d, ver, out, sha = sys.argv[1:]
m = json.load(open(os.path.join(d, "manifest.json")))
if (m.get("version"), m.get("file"), m.get("sha256")) != (ver, out, sha):
    sys.exit("the staged manifest is not this image's -- stage it again")
b = json.load(open(os.path.join(d, "bench", "manifest.json")))
for name, f in b["files"].items():
    if hashlib.sha256(open(os.path.join(d, "bench", name), "rb").read()).hexdigest() != f["sha256"]:
        sys.exit(f"the staged bench/{name} is not its manifest's -- stage it again")
PY
    app16=$(companion_image "$DIR/$OUT")
    verify_signature "$DIR/$OUT"
    [ -n "${BENCH_LOG:-}" ] && [ -r "$BENCH_LOG" ] || {
        echo "the second chip's firmware is published only after the bench -- with its log:" >&2
        echo "  BENCH_LOG=<the knob's log> RADIO=companion $0 $VER --push" >&2
        exit 1; }
    python3 - "$BENCH_LOG" "v$VER" "$app16" <<'PY'
import re, sys
log = open(sys.argv[1], errors="replace").read()
ver, app = re.escape(sys.argv[2]), re.escape(sys.argv[3])
other = r"\S* \[[0-9a-f]{16}\]"                 # any firmware, by its version and identity
on = re.search(r"second chip: update from " + other + " to " + ver + r" \[" + app + r"\]: kept", log)
took = re.search(r"second chip: update from " + ver + r" \[" + app + r"\] to " + other + ": kept", log)
if not on:
    sys.exit(f"the bench log never has it kept: \"to {sys.argv[2]} [{sys.argv[3]}]: kept\"")
if not took:
    sys.exit(f"the bench log never has it take an update itself: "
             f"\"update from {sys.argv[2]} [{sys.argv[3]}] to ...: kept\"")
print("==> the bench: " + on.group(0))
print("==> the bench: " + took.group(0))
PY
}

if [ "$RADIO" = companion ]; then
    DIR="firmware/companion"
    OUT="vfo-knob-companion-$VER.bin"
    [ "$PUSH" = "--push" ] || { companion_stage; exit 0; }
    companion_gate
else
    # sdkconfig.defaults.local is optional, as in CMakeLists.txt. IDF refuses a
    # defaults file that does not exist, and a machine that only cuts releases
    # has no business holding anyone's WiFi password.
    DEFAULTS="sdkconfig.defaults"
    [ -f sdkconfig.defaults.local ] && DEFAULTS="$DEFAULTS;sdkconfig.defaults.local"

    echo "==> building the $RADIO firmware $VER"
    # reconfigure: CMake reads `git describe` only when it configures, and a new
    # tag on an already-built commit does not make it configure again.
    idf.py -B "$BUILD" -D VFO_RADIO="$RADIO" \
           -D SDKCONFIG_DEFAULTS="$DEFAULTS;$OVERLAY" \
           reconfigure build >/dev/null

    BIN="$BUILD/vfo-knob-$RADIO.bin"
    # esp_app_desc_t leads the first segment: magic at byte 32, version at 48.
    IMGVER=$(python3 -c 'import sys; b = open(sys.argv[1], "rb").read()
print(b[48:80].split(b"\0")[0].decode() if b[32:36] == bytes.fromhex("3254cdab") else "?")' "$BIN")
    [ "$IMGVER" = "v$VER" ] || {
        echo "the image says it is $IMGVER, not v$VER -- refusing to publish" >&2
        exit 1; }

    verify_signature "$BIN"

    DIR="firmware/$RADIO"
    mkdir -p "$DIR"
    OUT="vfo-knob-$RADIO-$VER.bin"
    cp "$BIN" "$DIR/$OUT"
    SHA=$(sha256sum "$DIR/$OUT" | cut -d' ' -f1)
    SIZE=$(stat -c%s "$DIR/$OUT")

    cat > "$DIR/manifest.json" <<JSON
{
  "project": "vfo-knob-$RADIO",
  "version": "$VER",
  "file": "$OUT",
  "sha256": "$SHA",
  "size": $SIZE
}
JSON

    echo "==> staged $DIR/$OUT ($((SIZE/1024)) KB)"
    cat "$DIR/manifest.json"

    [ "$PUSH" = "--push" ] || { echo "==> not pushed (pass --push)"; exit 0; }
fi

echo "==> publishing to the firmware branch"
TMP=$(mktemp -d)
# Always hand the branch back: a worktree left holding it made the next
# radio's release fail ("'firmware' is already used by worktree").
trap 'git worktree remove --force "$TMP/wt" >/dev/null 2>&1 || true; rm -rf "$TMP"' EXIT
cp "$DIR/manifest.json" "$DIR/$OUT" "$TMP/"
git fetch origin firmware:firmware 2>/dev/null || true
if git rev-parse --verify firmware >/dev/null 2>&1; then
    git worktree add "$TMP/wt" firmware >/dev/null
else
    git worktree add --detach "$TMP/wt" >/dev/null
    git -C "$TMP/wt" checkout --orphan firmware >/dev/null
    git -C "$TMP/wt" rm -rf . >/dev/null 2>&1 || true
fi
mkdir -p "$TMP/wt/$DIR"
cp "$TMP/manifest.json" "$TMP/$OUT" "$TMP/wt/$DIR/"

# The second chip's bench files go up with its first release only, and never
# change after it: every second chip bench-flashed since has that partition
# table, which no update can write.
if [ "$RADIO" = companion ]; then
    if [ -f "$TMP/wt/$DIR/bench/partition-table.bin" ]; then
        cmp -s "$TMP/wt/$DIR/bench/partition-table.bin" "$DIR/bench/partition-table.bin" || {
            echo "its partition table is not the one every second chip has ($DIR/bench/) -- refusing to publish" >&2
            exit 1; }
    else
        mkdir -p "$TMP/wt/$DIR/bench"
        cp "$DIR/bench/bootloader.bin" "$DIR/bench/partition-table.bin" "$DIR/bench/ota_data_initial.bin" \
           "$DIR/bench/manifest.json" "$TMP/wt/$DIR/bench/"
        echo "==> the second chip's first release: its bench files go with it, never to change"
    fi
fi

# Every firmware published, with its name and version: what the setup
# firmware offers. Read from the server, not built in, so a setup firmware
# made before a radio's first release still offers it. Rebuilt from the
# manifests on the branch; a new radio gets its name in NAMES, at the top.
python3 - "$TMP/wt/firmware" "$NAMES" <<'PY'
import glob, json, os, sys
root = sys.argv[1]
# NAMES above: "firmwares" holds these and no others, in its order.
NAMES = json.loads(sys.argv[2])
order = list(NAMES)
index = {"firmwares": []}
for m in sorted(glob.glob(os.path.join(root, "*", "manifest.json"))):
    d = os.path.basename(os.path.dirname(m))
    man = json.load(open(m))
    if d == "companion":
        # The second chip's firmware, under a key of its own: the setup
        # firmware puts it on the SD card, and tools/knob-card.py too.
        index["companion"] = {k: man[k] for k in ("version", "file", "sha256", "size", "app_sha256")}
    elif d in NAMES:
        index["firmwares"].append({"radio": d, "name": NAMES[d], "version": man["version"]})
    else:
        print(f"WARNING: firmware/{d}/ has no name in NAMES (tools/release.sh): left out of the "
              f"setup firmware's list", file=sys.stderr)
index["firmwares"].sort(key=lambda f: order.index(f["radio"]))
with open(os.path.join(root, "index.json"), "w") as f:
    json.dump(index, f, indent=2)
    f.write("\n")
PY
cat "$TMP/wt/firmware/index.json"

git -C "$TMP/wt" add firmware
git -C "$TMP/wt" commit -q -m "firmware $VER for $RADIO"
# GitHub's SSH port times out from here now and then: try again rather than
# leave the release half made.
for try in 1 2 3 4 5; do
    git -C "$TMP/wt" push -u origin firmware && break
    [ "$try" = 5 ] && { echo "push failed; the commit waits on the local firmware branch" >&2; exit 1; }
    sleep 20
done
if [ "$RADIO" = companion ]; then
    echo "==> published; every knob updates its second chip by itself, at a quiet moment"
else
    echo "==> published; $RADIO knobs will see $VER within their check interval"
fi
