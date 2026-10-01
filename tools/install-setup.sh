#!/bin/bash
# Put the setup firmware on a knob over its USB-C cable: a new board with
# Waveshare's demo on it, or any knob to be made new again. It is what every
# knob ships with -- it asks for the WiFi from a phone, then for the firmware
# of the knob's radio (docs/setup.md).
#
#   tools/install-setup.sh             the latest release's setup firmware, and
#                                      every firmware onto the SD card
#   tools/install-setup.sh --local     this tree's build_setup/ instead (a test)
#   tools/install-setup.sh --no-card   the SD card left as it is
#   tools/install-setup.sh -p PORT     a serial port of your choosing
#
# Linux, with ESP-IDF 5.5 (for esptool; found through $IDF_PATH or in
# ~/esp/esp-idf) and the repository (the bootloader and partition table are
# built here, in build_setup/, if they are not already: they do not change
# between releases). Your user needs the serial ports' group (uucp, dialout).
#
# The knob's USB-C reaches one of its two chips, by which way round the plug
# is: the ESP32-S3 -- Espressif's USB serial port, 303a:1001 -- which is the
# one this writes, or the ESP32 beside it, behind a CH340 (1a86:7523). The
# wrong way round, this says so and stops: turn the plug over and run it
# again. A knob running the AetherSDR firmware has USB networking instead of
# a serial port (303a:4000): it is asked to restart, and caught in the six
# seconds its ROM serial port is open at boot.
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
set -euo pipefail

cd "$(dirname "$0")/.."
BUILD=build_setup
CACHE="${XDG_CACHE_HOME:-$HOME/.cache}/vfo-knob/firmware"
LOCAL=0
CARD=1
PORT=""

usage() { sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//'; exit "${1:-0}"; }
while [ $# -gt 0 ]; do
    case "$1" in
        --local)    LOCAL=1 ;;
        --no-card)  CARD=0 ;;
        -p|--port)  PORT="${2:?a port after $1}"; shift ;;
        -h|--help)  usage 0 ;;
        *)          echo "What is $1?" >&2; usage 2 ;;
    esac
    shift
done

say()  { printf '\n\033[1m%s\033[0m\n' "$*"; }
fail() { printf '\n\033[1;31m%s\033[0m\n' "$*" >&2; exit 1; }

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
            fail "The USB-C plug is the wrong way round: this way it reaches the
board's second chip (its CH340), not the ESP32-S3.
Turn the plug over and run this again."
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

# --- esptool, from ESP-IDF -----------------------------------------------------

if ! python -c "import esptool" 2>/dev/null; then
    IDF="${IDF_PATH:-$HOME/esp/esp-idf}"
    [ -f "$IDF/export.sh" ] || fail "ESP-IDF not found: set IDF_PATH, or install it in ~/esp/esp-idf."
    # shellcheck disable=SC1091
    . "$IDF/export.sh" >/dev/null 2>&1 || fail "ESP-IDF's export.sh failed: run it by hand to see why."
fi

# --- what goes on it -----------------------------------------------------------

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

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

say "Done. The knob restarts and shows WIFI SETUP: join the network VFOKnob
with a phone, and choose your WiFi on the page that opens; then it lists the
firmwares on its dial."
