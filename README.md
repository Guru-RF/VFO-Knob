# VFO-Knob

A tactile hardware VFO knob and control head for
[AetherSDR](https://github.com/aethersdr/AetherSDR), built on the Waveshare
ESP32-S3-Knob-Touch-LCD-1.8.

Talks **TCI v2.0 over WiFi** — the same protocol AetherSDR already serves to
WSJT-X and JTDX — so **no host-side software is required**. Enable the TCI
server in AetherSDR and the knob finds it.

## What it does

- **Tune** with a real detented knob, with velocity acceleration
- **Step** chosen by tapping a frequency digit (10 Hz … 1 MHz)
- **PTT** (toggle), with a time-out timer and a multi-stage fail-safe teardown
- **Mode, filter width, RIT and band** — tap to open an editor, turn to choose,
  tap to accept
- **Live S-meter**, signal in dBm, and forward power while transmitting
- **Receive audio** to the onboard DAC and 3.5 mm jack, with a volume control
- **Transmit audio** from the onboard microphone while keyed

Tuning is deliberately silent. The knob has 30 real mechanical detents, so
synthesising more on top adds nothing; the haptic motor is reserved for what
you cannot otherwise perceive — PTT state, rejected commands, and link loss.

## Setup

1. In AetherSDR, open the **TCI** panel from the button bar and enable the
   server (it listens on port 50001).
2. If the machine runs a firewall, allow TCI from your LAN:
   ```sh
   sudo ufw allow from 192.168.0.0/24 to any port 50001 proto tcp
   ```
3. Copy `sdkconfig.defaults.local.example` to `sdkconfig.defaults.local` and
   fill in your WiFi and the AetherSDR host. That file is gitignored; **never
   commit a PSK**.

## Build

Requires ESP-IDF v5.5.x.

```sh
. ~/esp/esp-idf/export.sh
idf.py set-target esp32s3
idf.py -p /dev/ttyACM0 flash monitor
```

The USB-C port is switchable between the two MCUs on this board. If the serial
port does not appear, unplug, rotate the plug 180°, and reinsert.

### USB-C networking build

The knob can reach AetherSDR over the USB cable instead of WiFi, appearing to
the host as a USB network adapter. Nothing above the IP layer changes, so TCI,
PTT and audio work identically. Useful where 2.4 GHz struggles -- the CNC
aluminium case is not kind to the onboard antenna.

```sh
idf.py -B build_usbnet \
  -D SDKCONFIG="build_usbnet/sdkconfig" \
  -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.local;sdkconfig.usbnet" \
  build flash
```

Each build needs its OWN `SDKCONFIG` path. Without it both variants share the
project's single `sdkconfig` and silently build each other's configuration.

**A USB-networking build has no serial console** -- the ESP32-S3's
USB-Serial-JTAG and USB-OTG peripherals share the same pins. To recover, or to
reflash, **hold a finger on the screen while it boots**: USB networking is
skipped and the console comes back.

### Host tests

The protocol parser, anti-echo classifier, tuning model and PTT state machine
live in `components/vfo_core/`, which has **zero ESP-IDF dependencies** and is
tested with plain gcc — including a byte-stream fuzzer under ASan/UBSan:

```sh
cmake -B build_host -S test/host && cmake --build build_host
ctest --test-dir build_host --output-on-failure
```

### Testing without a radio

`tools/mock_aether.py` is a fault-injecting TCI server (standard library only):

```sh
python3 tools/mock_aether.py --lock --clamp 14000000 14080000 --ptt-refuse
```

It reproduces the behaviours that actually break clients — locked slices that
echo the old frequency, no-op tunes that emit nothing at all, backlog kills,
and `abortTciPtt` when the PTT-owning client disconnects.

## Serial console

- `t` toggle PTT · `k` key · `u` unkey · `s` status
- `p` force a pong-stale abort · `d` force a link-down abort · `o` TOT to 30 s
- `r` cycle screen rotation

## Hardware notes

Findings that contradict commonly published information for this board:

- The display is an **SH8601** over QSPI, not ST77916. Waveshare's own demo
  depends on `esp_lcd_sh8601`. It honours MADCTL `MX` but **not** `MY`, so a
  180° rotation has to be done in LVGL.
- The knob is **not a quadrature encoder**. It is a bidirectional switch knob:
  two independent active-low contacts, one per direction. Decoding it as
  quadrature yields a net count of exactly zero. 30 detents per revolution.
- Contact debounce must filter on **minimum contact duration**, not an edge
  lockout — an edge lockout accepts the first edge of a bounce burst and masks
  the real pulse behind it.
- The haptic driver is a **DRV2605L driving an ERM**, confirmed by a passing
  ERM auto-calibration.
- PDM microphone capture is only available on **I2S0**, so the DAC must use
  I2S1.

## Safety

This device keys a transmitter over WiFi.

AetherSDR fails closed when a TCI client disconnects (`abortTciPtt()`), and this
firmware adds a time-out timer, a pong-based link watchdog, a "still keyed"
reminder every 10 s, and a four-stage teardown that ends by deliberately
destroying its own socket — which is a more reliable unkey than any command,
because the server unkeys on disconnect.

**The gap that remains:** AetherSDR does not ping its TCI clients and has no
idle timeout, so if this device loses power *instantaneously* while
transmitting, detection falls to TCP retransmission — potentially several
minutes with the radio keyed. No firmware on the device can fix that; it needs
an application-layer PTT lease on the AetherSDR side, at the `abortTciPtt()`
hook that already exists.

**Do not use this as a primary PTT source for unattended operation until that
is addressed.**

## Licence

GPL-3.0-or-later, matching AetherSDR.
