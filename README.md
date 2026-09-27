# VFO-Knob

A tactile hardware VFO knob and control head for [AetherSDR](https://github.com/aethersdr/AetherSDR),
built on the Waveshare ESP32-S3-Knob-Touch-LCD-1.8.

Talks **TCI v2.0 over WiFi** (`ws://<host>:50001`) — the same protocol AetherSDR already serves to
WSJT-X and JTDX. No host-side bridge software is required.

## Status

Under construction. See `docs/` and the bring-up milestones.

## What it does

- Tune the VFO with a real encoder, with acceleration and per-detent haptic detents
- Step size selected by tapping a frequency digit
- PTT (toggle), with a time-out timer and a multi-stage fail-safe teardown
- Mode, filter width, RIT
- Live S-meter, and forward power + SWR while transmitting

## Safety notice

This device keys a transmitter over WiFi.

AetherSDR fails closed when a TCI client disconnects (`abortTciPtt()`), and this firmware adds a
time-out timer, a link-liveness watchdog and a four-stage teardown that ends by deliberately
destroying its own socket. **However:** AetherSDR does not currently ping its TCI clients and has no
idle timeout, so if this device loses power *instantaneously* while transmitting, detection falls to
TCP retransmission — which can leave the radio keyed for several minutes. No firmware on the device
can fix that; it needs an application-layer PTT lease on the AetherSDR side.

**Do not use this as a primary PTT source for unattended operation until that is addressed.**

## Build

Requires ESP-IDF v5.5.5.

```sh
. ~/esp/esp-idf/export.sh
cp sdkconfig.defaults.local.example sdkconfig.defaults.local   # then edit in your WiFi + host
idf.py set-target esp32s3
idf.py build flash monitor
```

### Host tests

The protocol, anti-echo, acceleration and PTT logic live in `components/vfo_core/`, which has **zero
ESP-IDF dependencies** and is tested with plain gcc:

```sh
cmake -B build_host -S test/host && cmake --build build_host && ctest --test-dir build_host --output-on-failure
```

## Licence

GPL-3.0-or-later, matching AetherSDR.
