#!/bin/bash
# Put the setup firmware on a knob over its USB-C cable: a new board with
# Waveshare's demo on it, or any knob to be made new again. It is what every
# knob ships with -- it asks for the WiFi from a phone, then for the firmware
# of the knob's radio (docs/setup.md). Then its second chip's firmware, the
# Bluetooth headset's, with the plug turned over.
#
#   tools/install-setup.sh                the latest release's setup firmware,
#                                         every firmware onto the SD card, and
#                                         the second chip's firmware
#   tools/install-setup.sh --local        this tree's builds instead (a test):
#                                         build_setup/ and companion/build/
#   tools/install-setup.sh --no-card      the SD card left as it is
#   tools/install-setup.sh --no-second-chip   the second chip left as it is
#   tools/install-setup.sh --second-chip  only the second chip, on a knob in
#                                         use: its headset stays paired
#   tools/install-setup.sh --chip-build DIR   the second chip's from that
#                                         build (companion/build_a, say)
#   tools/install-setup.sh -p PORT        a serial port of your choosing (the
#                                         second chip's, with --second-chip)
#
# Linux, with ESP-IDF 5.5 (for esptool; found through $IDF_PATH or in
# ~/esp/esp-idf) and the repository (the bootloader and partition table are
# built here, in build_setup/, if they are not already: they do not change
# between releases). Your user needs the serial ports' group (uucp, dialout).
#
# The knob's USB-C reaches one of its two chips, by which way round the plug
# is: the ESP32-S3 -- Espressif's USB serial port, 303a:1001 -- or the ESP32
# beside it, the second chip, behind a CH340 (1a86:7523). This writes the S3
# first, then asks for the plug to be turned over and writes the second chip.
# Started the wrong way round, it says so: turn the plug over and run it
# again, or write the second chip alone with --second-chip. A knob running
# the AetherSDR firmware has USB networking instead of a serial port
# (303a:4000): it is asked to restart, and caught in the six seconds its ROM
# serial port is open at boot.
#
# The whole flash is erased -- the demo, its settings, anything the knob
# kept -- before the bootloader, the partition table and the setup firmware
# are written, with a one-time mark in its settings (NVS "vfo"/"sdwipe") that
# has the setup firmware empty the microSD card as it first starts: the demo's
# pictures off it, room for the firmwares it keeps there. Only then -- no
# knob ever empties its card by itself. Then every firmware published goes
# down the cable onto the card (tools/knob-card.py), so a new knob installs
# its radio's in seconds, and needs no network to go back to the setup
# firmware; and the knob restarts, as its owner will first see it. The
# firmwares are fetched once into ~/.cache/vfo-knob and checked against their
# release's sha256: a row of knobs downloads them only once.
#
# The second chip gets what the knob's updates of it never write, once: a
# bootloader that can go back to the firmware before, the partition table,
# an empty otadata -- the bench files published with its first release --
# and the latest release of its firmware, each checked against its sha256
# (tools/knob-card.py chip). On a knob made new its whole flash is erased
# first, Waveshare's demo with it; --second-chip, on a knob in use, keeps its
# settings -- the headset's pairing -- and saves them on this computer first.
# Then its console, as it starts: the firmware's version, and a bootloader
# that can go back. From then on the knob keeps a release on it up to date by
# itself, over the link between the two chips.
set -euo pipefail

cd "$(dirname "$0")/.."
BUILD=build_setup
CACHE="${XDG_CACHE_HOME:-$HOME/.cache}/vfo-knob/firmware"
LOCAL=0
CARD=1
CHIP=1                  # the second chip as well, after the S3
ONLY_CHIP=0             # --second-chip: the second chip alone, its settings kept
CHIP_BUILD=""           # its parts from this build directory, not the release
PORT=""

usage() { sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'; exit "${1:-0}"; }
while [ $# -gt 0 ]; do
    case "$1" in
        --local)          LOCAL=1 ;;
        --no-card)        CARD=0 ;;
        --no-second-chip) CHIP=0 ;;
        --second-chip)    ONLY_CHIP=1 ;;
        --chip-build)     CHIP_BUILD="${2:?a build directory after $1}"; shift ;;
        -p|--port)        PORT="${2:?a port after $1}"; shift ;;
        -h|--help)        usage 0 ;;
        *)                echo "What is $1?" >&2; usage 2 ;;
    esac
    shift
done
[ "$ONLY_CHIP" = 1 ] && [ "$CHIP" = 0 ] && { echo "--second-chip or --no-second-chip: not both" >&2; exit 2; }
if [ "$LOCAL" = 1 ] && [ -z "$CHIP_BUILD" ]; then
    CHIP_BUILD=companion/build
fi

say()  { printf '\n\033[1m%s\033[0m\n' "$*"; }
fail() { printf '\n\033[1;31m%s\033[0m\n' "$*" >&2; exit 1; }

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# --- the knob on the cable ---------------------------------------------------

# A USB device by vendor and product id, present or not.
usb_has() {
    local d
    for d in /sys/bus/usb/devices/*; do
        [ -f "$d/idVendor" ] || continue
        [ "$(cat "$d/idVendor")" = "$1" ] && [ "$(cat "$d/idProduct")" = "$2" ] && return 0
    done
    return 1
}

# The ESP32-S3's serial port, if exactly one is on USB.
find_s3() {
    local ports
    ports=$(ls /dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_* 2>/dev/null || true)
    [ "$(printf '%s' "$ports" | grep -c . || true)" = 1 ] && readlink -f "$ports"
}

# Every CH340's serial port on USB (1a86:7523), one a line: the second
# chip's among them, with the plug that way round.
chip_ports() {
    local t d
    for t in /sys/bus/usb-serial/devices/*; do
        [ -e "$t" ] || continue
        # Up from the port to the USB device it is on: the first with an id.
        d=$(readlink -f "$t")
        while [ -n "$d" ] && [ ! -f "$d/idVendor" ]; do d=${d%/*}; done
        [ "$(cat "$d/idVendor" 2>/dev/null)" = 1a86 ] && [ "$(cat "$d/idProduct" 2>/dev/null)" = 7523 ] &&
            echo "/dev/${t##*/}"
    done
    return 0
}

# --- esptool, from ESP-IDF -----------------------------------------------------

esptool_ready() {
    python -c "import esptool" 2>/dev/null && return 0
    IDF="${IDF_PATH:-$HOME/esp/esp-idf}"
    [ -f "$IDF/export.sh" ] || fail "ESP-IDF not found: set IDF_PATH, or install it in ~/esp/esp-idf."
    # shellcheck disable=SC1091
    . "$IDF/export.sh" >/dev/null 2>&1 || fail "ESP-IDF's export.sh failed: run it by hand to see why."
}

# --- the second chip -----------------------------------------------------------

# What goes on the second chip (tools/knob-card.py chip), each part checked:
# CHIP_BL, CHIP_PT, CHIP_OD and CHIP_APP, the firmware's version and where it
# is from. False when no second-chip firmware is published yet.
chip_parts() {
    local rc=0
    python tools/knob-card.py chip "$CACHE" "$TMP/chip.json" ${CHIP_BUILD:+"$CHIP_BUILD"} || rc=$?
    [ "$rc" = 3 ] && return 1
    [ "$rc" = 0 ] || fail "The second chip's firmware is not to be had: see above."
    { read -r CHIP_BL; read -r CHIP_PT; read -r CHIP_OD; read -r CHIP_APP
      read -r CHIP_VER; read -r CHIP_FROM; read -r CHIP_RELEASE; } < <(python -c 'import json, sys
p = json.load(open(sys.argv[1]))
for k in ("bootloader", "table", "otadata", "image", "version", "source", "release"): print(p[k])' "$TMP/chip.json")
}

# The second chip, through its CH340 with the plug turned over. "new": a knob
# made new, the chip's whole flash erased first, Waveshare's demo with it.
# "keep": a knob in use, its settings -- the headset's pairing among them --
# kept where they are, and saved on this computer first. CHIP_PORT names its
# port, or it is found.
second_chip() {
    local how=$1 cp=$CHIP_PORT before n desc mac rev erase="" bak rc=0
    local -a e
    if [ -z "$cp" ]; then
        before=$(chip_ports)
        if [ "$how" = new ] || [ -z "$before" ]; then
            # The knob's own CH340 comes as the plug is turned over: one that
            # was here before is another device's.
            say "Now the second chip: turn the USB-C plug over ..."
            for _ in $(seq 1 240); do
                cp=$(chip_ports | grep -vxF -f <(printf '%s\n' "$before") || true)
                [ -n "$cp" ] && break
                sleep 0.5
            done
            [ -n "$cp" ] || fail "The second chip did not come in two minutes. Turn the plug over, and
run tools/install-setup.sh --second-chip"
        else
            cp=$before
        fi
        n=$(printf '%s\n' "$cp" | grep -c .)
        [ "$n" = 1 ] || fail "$n CH340 serial ports on USB: unplug the others, or choose the second
chip's with --second-chip -p PORT, from
$cp"
    fi
    [ -e "$cp" ] || fail "No such port: $cp"
    [ -w "$cp" ] || fail "$cp is not yours to write: add yourself to its group
($(stat -c %G "$cp")) and log in again."
    e=(python -m esptool --chip esp32 -p "$cp" -b 921600)

    # Which chip it is. Refused below revision 3, the first to check a
    # firmware's signature in the app, as every update to it needs; and,
    # found here rather than named with -p, unless it is the knob's second
    # chip, an ESP32-U4WDH -- not some other board's ESP32 on a CH340.
    if ! "${e[@]}" read_mac >"$TMP/chip-id.log" 2>&1; then
        tr '\r' '\n' <"$TMP/chip-id.log" | tail -8
        fail "esptool could not reach the second chip on $cp: see above. Is a serial
monitor holding its port? Close it and run this again."
    fi
    desc=$(tr '\r' '\n' <"$TMP/chip-id.log" | sed -n 's/^Chip is //p' | head -1)
    mac=$(tr '\r' '\n' <"$TMP/chip-id.log" | sed -n 's/^MAC: //p' | head -1)
    rev=$(printf '%s' "$desc" | sed -n 's/.*(revision v\([0-9]*\)\.[0-9]*).*/\1/p')
    say "The second chip: $cp, ${desc:-an ESP32 of no description}"
    [ -n "$rev" ] || fail "esptool did not say the chip's revision."
    [ "$rev" -ge 3 ] || fail "Its firmware needs revision 3 or later: the chip checks a firmware's
signature only from there."
    case "$desc" in
        ESP32-U4WDH*) ;;
        *) [ -n "$CHIP_PORT" ] || fail "That is not a knob's second chip, an ESP32-U4WDH. Is it another
device's CH340? Name the knob's with --second-chip -p PORT." ;;
    esac

    if [ "$how" = keep ]; then
        bak="${XDG_DATA_HOME:-$HOME/.local/share}/vfo-knob/second-chip-${mac//:/}-$(date +%Y%m%d-%H%M%S).nvs.bin"
        mkdir -p "${bak%/*}"
        if ! "${e[@]}" read_flash 0x9000 0x5000 "$bak" >"$TMP/chip-nvs.log" 2>&1; then
            tr '\r' '\n' <"$TMP/chip-nvs.log" | grep -v "([0-9]* %)$" | tail -8
            fail "Could not save the second chip's settings: nothing was written."
        fi
        echo "Its settings, the headset's pairing among them, saved in $bak"
    else
        erase=--erase-all
    fi

    say "Writing the second chip: $CHIP_VER, $CHIP_FROM (about half a minute) ..."
    # The otadata emptied with the rest: the firmware just written to ota_0
    # is the one that starts.
    # shellcheck disable=SC2086
    if ! "${e[@]}" --before default_reset --after hard_reset write_flash $erase \
            --flash_mode dio --flash_freq 40m --flash_size keep \
            0x1000 "$CHIP_BL" 0x8000 "$CHIP_PT" 0xe000 "$CHIP_OD" 0x20000 "$CHIP_APP" \
            >"$TMP/chip-flash.log" 2>&1; then
        tr '\r' '\n' <"$TMP/chip-flash.log" | grep -v "([0-9]* %)$" | tail -15
        fail "esptool failed: see above. Run this again: tools/install-setup.sh --second-chip"
    fi
    tr '\r' '\n' <"$TMP/chip-flash.log" | grep -E "^(Wrote|Hash of data)" | uniq

    say "It starts ..."
    python - "$cp" "$CHIP_VER" >"$TMP/chip-console.log" 2>&1 <<'PY' || rc=$?
# The chip's console as it starts: its firmware's banner and its boot story
# (companion/main/upd.c), the second saying which bootloader it has.
import re, sys, time
import serial
port, want = sys.argv[1], sys.argv[2]
s = serial.Serial()
s.port, s.baudrate, s.timeout = port, 115200, 0.2
# Neither line asserted as it opens; then a restart through RTS to EN, as
# esptool's own: the boot from its first line, IO0 high (no loader).
s.dtr = False
s.rts = False
s.open()
s.rts = True
time.sleep(0.1)
s.rts = False
lines, buf, banner, story = [], b"", None, None
end = time.time() + 8
while time.time() < end and not story:
    buf += s.read(4096)
    *done, buf = buf.split(b"\n")
    for raw in done:
        line = raw.decode(errors="replace").strip()
        lines.append(line)
        m = re.search(r"companion: (VFO-Knob companion (\S+))", line)
        banner = banner or m
        m = re.search(r"upd: (boot: .*)", line)
        story = story or m
s.close()
if not banner:
    print("\n".join(lines[-12:]) or "(nothing on its console)")
    sys.exit(2)
print("  " + banner.group(1))
if story:
    print("  " + story.group(1))
if banner.group(2) != want:
    sys.exit(4)
# No story caught is not a story naming an old bootloader: said apart.
if not story:
    print("\n".join(lines[-12:]))
    sys.exit(5)
boot = re.search(r"; bootloader (\d+)", story.group(1))
sys.exit(0 if boot and int(boot.group(1)) >= 2 else 3)
PY
    cat "$TMP/chip-console.log"
    case $rc in
        0) ;;
        2) fail "The second chip did not say it started: its console is above." ;;
        3) fail "It started without a bootloader that can go back: run this again." ;;
        4) fail "It started another firmware than the one written: run this again." ;;
        5) fail "It started $CHIP_VER, but its boot story (which bootloader it has) was not seen:
its console is above. Open its port at 115200 -- that restarts it -- and look
for its 'upd: boot:' line, or run this again." ;;
        *) fail "Could not read the second chip's console: see above." ;;
    esac
}

if [ "$ONLY_CHIP" = 1 ]; then
    CHIP_PORT=$PORT
    esptool_ready
    chip_parts || fail "--chip-build DIR puts a build of this tree's on it instead."
    second_chip keep
    if [ "$CHIP_RELEASE" = True ]; then
        say "Done. The second chip runs $CHIP_VER, and the knob keeps it up to date by itself."
    else
        say "Done. The second chip runs $CHIP_VER, a development build: the knob leaves
it alone until a release is handed to it (POST /api/bt/update?force=1)."
    fi
    exit 0
fi
CHIP_PORT=""

# --- the ESP32-S3 ----------------------------------------------------------------

if [ -z "$PORT" ]; then
    n=$(ls /dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_* 2>/dev/null | wc -l)
    [ "$n" -gt 1 ] && fail "$n knobs on USB: choose one with -p, from
$(ls /dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_*)"
    if ! PORT=$(find_s3); then
        if usb_has 303a 4000; then
            # Our AetherSDR firmware: USB networking, no serial port. Ask it to
            # restart; its ROM serial port is open for six seconds at boot.
            say "The knob runs a firmware with USB networking: asking it to restart ..."
            echo reboot | timeout 3 nc 10.55.42.1 3333 >/dev/null 2>&1 || true
            for _ in $(seq 1 40); do
                PORT=$(find_s3) && break
                sleep 0.25
            done
            [ -n "$PORT" ] || fail "It did not come back as a serial port. Unplug the knob, plug it
in again, and run this within six seconds -- or install the setup
firmware from its configuration page instead (Firmware: Radio, Setup)."
        elif usb_has 1a86 7523; then
            fail "The USB-C plug is the wrong way round for the ESP32-S3: this way it
reaches the board's second chip (its CH340), which comes after it.
Turn the plug over and run this again -- or, for the second chip alone,
run it with --second-chip."
        else
            fail "No knob on USB. Plug it in -- with a data cable, not a charging
one -- and run this again."
        fi
    fi
fi
[ -e "$PORT" ] || fail "No such port: $PORT"
# Its name by its serial number: the one it comes back under after restarts.
BYID=""
for l in /dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_*; do
    [ -e "$l" ] && [ "$(readlink -f "$l")" = "$(readlink -f "$PORT")" ] && BYID="$l"
done
[ -w "$PORT" ] || fail "$PORT is not yours to write: add yourself to its group
($(stat -c %G "$PORT")) and log in again."
say "The knob's ESP32-S3: $PORT"

esptool_ready

# --- what goes on it -----------------------------------------------------------

if [ ! -f "$BUILD/bootloader/bootloader.bin" ] || [ ! -f "$BUILD/partition_table/partition-table.bin" ] ||
   { [ "$LOCAL" = 1 ] && [ ! -f "$BUILD/vfo-knob-setup.bin" ]; }; then
    say "Building the setup firmware's bootloader and partition table ..."
    targets="bootloader partition-table"
    [ "$LOCAL" = 1 ] && targets="build"
    # shellcheck disable=SC2086
    idf.py -B "$BUILD" -D VFO_RADIO=setup -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.setup" \
        $targets >"$TMP/build.log" 2>&1 || { tail -20 "$TMP/build.log"; fail "The build failed."; }
fi

# Every firmware published, the setup firmware with them: into the cache
# when it does not have them yet, each against its manifest's sha256.
say "The firmwares, from the release ..."
python tools/knob-card.py fetch "$CACHE" "$TMP/plan.json" || fail "Could not fetch the firmwares."

if [ "$LOCAL" = 1 ]; then
    APP="$BUILD/vfo-knob-setup.bin"
    WHAT="this tree's build ($(git describe --tags --dirty 2>/dev/null || echo '?'))"
else
    read -r APP VER < <(python -c 'import json,sys
for x in json.load(open(sys.argv[1]))["images"]:
    if x["radio"] == "setup": print(x["image"], x["version"])' "$TMP/plan.json")
    [ -n "${APP:-}" ] || fail "The release has no setup firmware."
    WHAT="setup $VER, the latest release"
fi

# The second chip's, checked now: nothing is erased for a firmware that is
# not to be had.
if [ "$CHIP" = 1 ] && ! chip_parts; then
    CHIP=0
    echo "The second chip is left as it is."
fi

# The mark that has the setup firmware empty the card, this once: a settings
# partition with that one key in it, made by ESP-IDF's own tool.
NVSGEN="${IDF_PATH:-$HOME/esp/esp-idf}/components/nvs_flash/nvs_partition_generator/nvs_partition_gen.py"
printf 'key,type,encoding,value\nvfo,namespace,,\nsdwipe,data,u8,1\n' >"$TMP/mark.csv"
python "$NVSGEN" generate "$TMP/mark.csv" "$TMP/mark.bin" 0x6000 >"$TMP/mark.log" 2>&1 ||
    { cat "$TMP/mark.log"; fail "Could not make the settings partition."; }

# --- on it -----------------------------------------------------------------------

say "Erasing the knob and writing $WHAT (about a minute) ..."
if ! python -m esptool --chip esp32s3 -p "$PORT" -b 921600 --before default_reset --after hard_reset \
        write_flash --erase-all --flash_mode dio --flash_freq 80m --flash_size keep \
        0x0 "$BUILD/bootloader/bootloader.bin" \
        0x8000 "$BUILD/partition_table/partition-table.bin" \
        0x9000 "$TMP/mark.bin" \
        0x20000 "$APP" >"$TMP/flash.log" 2>&1; then
    tr '\r' '\n' <"$TMP/flash.log" | grep -v "([0-9]* %)$" | tail -15
    fail "esptool failed: see above. Unplug the knob, plug it in again, and run this again."
fi
tr '\r' '\n' <"$TMP/flash.log" | grep -E "^(Chip is|MAC|Wrote|Hash of data)" | uniq

# --- the firmwares onto its SD card ---------------------------------------------

if [ "$CARD" = 1 ]; then
    say "The knob starts, and empties its SD card; then the firmwares go onto it ..."
    # The emptying first, undisturbed: opening the port could restart the
    # knob, and the card must not be half-emptied. Then the port, as it comes
    # back after the restart esptool gave it.
    sleep 15
    P="${BYID:-$PORT}"
    for _ in $(seq 1 40); do
        [ -e "$P" ] && break
        sleep 0.5
    done
    python tools/knob-card.py push "$P" "$TMP/plan.json" ||
        fail "The SD card did not get every firmware: run this again, or install them over WiFi from the knob."
fi

# --- its second chip -------------------------------------------------------------

SHOWS="restarts and shows"
if [ "$CHIP" = 1 ]; then
    second_chip new
    SHOWS="shows"
fi

say "Done. The knob $SHOWS WIFI SETUP: join the network VFOKnob
with a phone, and choose your WiFi on the page that opens; then it lists the
firmwares on its dial."
