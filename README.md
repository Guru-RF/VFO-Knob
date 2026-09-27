# VFO-Knob

A hardware VFO knob and control head for [AetherSDR](https://github.com/aethersdr/AetherSDR),
built on the Waveshare ESP32-S3-Knob-Touch-LCD-1.8. Tune, change step, key the
transmitter, watch the S-meter — over a USB-C cable or over WiFi.

<p align="center">
  <img src="docs/display-rx.svg" width="300" alt="Receiving: S-meter filling through its colour blocks while the 100 Hz digit ticks">
  &nbsp;&nbsp;
  <img src="docs/display-tx.svg" width="300" alt="Transmitting: SWR on the left, forward power on the right, red PTT slab">
</p>

*Receiving, and transmitting. The meter blocks are notched in the background
colour, so the separators appear where the bar has reached and vanish where it
has not.*

It speaks **TCI v2.0** over a WebSocket, which is AetherSDR's own control
protocol — so the knob is not polling, it is told. Tune at the desktop and the
knob follows; turn the knob and the desktop moves.

---

## What it does

| | |
|---|---|
| **Tune** | Per-digit step selection: tap a digit to set the decade. Acceleration on top, so a flick crosses a band and a slow turn lands on 10 Hz. |
| **PTT** | Toggle — tap to key, tap anywhere along the bottom to unkey. Time-out timer, a haptic reminder every 10 s while keyed, and a four-rung teardown that ends in dropping the socket. |
| **Meters** | S-meter in receive; SWR and auto-ranging forward power (to 2.5 kW) in transmit. |
| **Audio** | RX audio out of the 3.5 mm jack, TX audio from the onboard mic, both with adjustable level. |
| **Mode / filter / RIT** | Tap to open, turn to choose, tap anywhere to accept. |
| **Network** | Tap the meter arc to see the knob's addresses. |

## Two transports

The knob is USB-powered, so it is always plugged into something — and whatever
runs AetherSDR is a computer with a USB port.

- **USB-C (preferred).** The knob enumerates as a **USB network adapter**
  (CDC-NCM), hands your machine an address and talks TCI over the cable. No
  configuration at all, and immune to what a machined metal case does to
  2.4 GHz.
- **WiFi.** Configure an SSID on the configuration page and power the knob from
  any charger.

The cable wins when something answers on it; WiFi is the fallback. When the
cable is chosen, WiFi is shut down — that frees about 40 kB of internal RAM,
which this board genuinely needs.

## First run

1. Plug the knob into the computer running AetherSDR.
2. Open **`http://10.55.42.1`** — user `admin`, password `admin`.
3. Change the password. The page will nag until you do; it can key a
   transmitter.
4. Set the AetherSDR host if it is not on the same machine, and an SSID if you
   want WiFi.

Enable AetherSDR's TCI server first — it is the `TCI` panel in the button bar.

## Configuration page

Served on port 80 over whichever interface is up. Status, AetherSDR endpoint,
WiFi credentials, audio levels, transmit time-out, access credentials, and
firmware updates.

> **It is HTTP Basic over plain HTTP.** A lock on the door, not a safe — treat
> the knob as something that belongs on a network you trust.

## Updates

Images are **RSA-3072 signed** and the signature is checked before an update is
accepted. Nothing is burned into eFuse and Secure Boot is not enabled, so the
board always stays ordinarily flashable — this protects the update path, not
the hardware.

- **On WiFi** the knob checks by itself (every 24 h by default) and installs
  anything newer. It never reboots to apply it; the image waits in the spare
  slot until you restart it.
- **Over USB** the knob has no route to the internet — it is the DHCP *server*
  on that link. The configuration page does the checking and the downloading
  instead, then pushes the image over. Same image, same signature check.

If an update fails to boot, the bootloader rolls back to the previous slot. The
confirmation is tied to the same "this boot looks healthy" timer that clears the
boot-loop guard.

Publishing a release: `tools/release.sh 1.2.3 --push`.

## Building

Needs ESP-IDF 5.5.x.

```sh
idf.py -B build_usbnet \
       -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.usbnet" \
       build flash
```

Leave off `sdkconfig.usbnet` for a WiFi-only build that keeps the serial
console. **No credentials are compiled in** — a unit ships with empty storage
and is configured over the cable.

Host-side tests (no hardware, no ESP-IDF):

```sh
cmake -S test/host -B build_host && cmake --build build_host && (cd build_host && ctest)
```

`tools/mock_aether.py` is a fault-injecting TCI server for exercising the error
paths without a radio.

## Recovering a knob

Once TinyUSB owns the USB pads there is no serial port, and this board makes
that awkward: GPIO0 doubles as the audio mux behind a single button inside the
case, and **the other Type-C plug orientation reaches the board's second chip,
not the ESP32-S3.** So the firmware leaves its own way back in.

- Every boot holds the ROM serial port open for **6 seconds** before starting
  USB networking. `idf.py flash` catches it.
- The PHY mux is put back to serial on *every* startup — it lives in the RTC
  domain and survives a reset, so without that a crash would come back with no
  serial port at all.
- `echo reboot | nc <knob-ip> 3333` restarts it remotely and opens that window.
- Holding a finger on the screen through boot skips USB networking entirely.

Port 3333 is also a log stream — the USB build has no console, so it is the only
way to watch a boot.

## Safety

The knob keys a transmitter. Two things are worth knowing:

- **Toggle PTT** means the radio stays keyed when you let go. That is why the
  time-out timer, the periodic haptic reminder and the very loud red screen all
  exist.
- **A client that loses power while keyed cannot unkey itself.** No firmware on
  this device can fix that; the server has to notice. Filed upstream as
  [aethersdr#5985](https://github.com/aethersdr/AetherSDR/issues/5985).

## Hardware

Waveshare ESP32-S3-Knob-Touch-LCD-1.8: ESP32-S3 with 16 MB flash and 8 MB PSRAM,
360×360 round SH8601 display, CST816 touch, DRV2605L haptics, PCM5100A DAC and a
PDM microphone. Every GPIO number lives in `components/board/board_pins.h`.

## Licence

[Apache-2.0](LICENSE). You may use, modify, sell and ship this firmware,
including on hardware you sell, with no obligation to publish your changes. It
carries an explicit patent grant. Keep the `LICENSE` and `NOTICE` files with
any redistribution, and state what you changed.

**One caveat, and it is a real one.** `components/panel/sh8601_init_cmds.c` is
copied verbatim from a Waveshare demo that ships no licence at all, so it is
not mine to license and the Apache grant above does not reach it. It is
compiled into every binary. Settle it — permission from Waveshare, or re-derive
the table from the SH8601 datasheet — before selling devices with this
firmware on them. See [THIRD_PARTY_LICENSES](THIRD_PARTY_LICENSES).
