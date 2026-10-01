#!/bin/bash
# Put the setup firmware on a knob over its USB-C cable: a new board with
# Waveshare's demo on it, or any knob to be made new again. It is what every
# knob ships with -- it asks for the WiFi from a phone, then for the firmware
# of the knob's radio (docs/setup.md).
#
#   tools/install-setup.sh             the latest release's setup firmware
#   tools/install-setup.sh --local     this tree's build_setup/ instead (a test)
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
# are written. Then it restarts into the setup firmware.
set -euo pipefail

cd "$(dirname "$0")/.."
FW_URL="https://raw.githubusercontent.com/Guru-RF/VFO-Knob/firmware/firmware/setup"
BUILD=build_setup
LOCAL=0
PORT=""

usage() { sed -n '2,10p' "$0" | sed 's/^# \{0,1\}//'; exit "${1:-0}"; }
while [ $# -gt 0 ]; do
    case "$1" in
        --local)    LOCAL=1 ;;
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

if [ "$LOCAL" = 1 ]; then
    APP="$BUILD/vfo-knob-setup.bin"
    WHAT="this tree's build ($(git describe --tags --dirty 2>/dev/null || echo '?'))"
else
    say "Fetching the released setup firmware ..."
    curl -fsSL "$FW_URL/manifest.json" -o "$TMP/manifest.json" || fail "Could not fetch $FW_URL/manifest.json"
    read -r FILE SHA VER < <(python -c 'import json,sys; m=json.load(open(sys.argv[1])); print(m["file"], m["sha256"], m["version"])' "$TMP/manifest.json")
    curl -fsSL "$FW_URL/$FILE" -o "$TMP/$FILE" || fail "Could not fetch $FW_URL/$FILE"
    [ "$(sha256sum "$TMP/$FILE" | cut -d' ' -f1)" = "$SHA" ] || fail "$FILE does not match its manifest's sha256."
    APP="$TMP/$FILE"
    WHAT="setup $VER, the latest release"
fi

# --- on it -----------------------------------------------------------------------

say "Erasing the knob and writing $WHAT (about a minute) ..."
if ! python -m esptool --chip esp32s3 -p "$PORT" -b 921600 --before default_reset --after hard_reset \
        write_flash --erase-all --flash_mode dio --flash_freq 80m --flash_size keep \
        0x0 "$BUILD/bootloader/bootloader.bin" \
        0x8000 "$BUILD/partition_table/partition-table.bin" \
        0x20000 "$APP" >"$TMP/flash.log" 2>&1; then
    tr '\r' '\n' <"$TMP/flash.log" | grep -v "([0-9]* %)$" | tail -15
    fail "esptool failed: see above. Unplug the knob, plug it in again, and run this again."
fi
tr '\r' '\n' <"$TMP/flash.log" | grep -E "^(Chip is|MAC|Wrote|Hash of data)" | uniq

say "Done. The knob starts the setup firmware: WIFI SETUP on its screen.
Join the network VFOKnob with a phone, and choose your WiFi on the page
that opens; then the knob lists the firmwares on its dial."
