# TODO

## Upstream: propose a PTT lease for AetherSDR

**This is the highest-value outstanding item, and it cannot be fixed here.**

AetherSDR fails closed when a TCI client disconnects — `abortTciPtt()` at
`src/core/TciServer.cpp:975` unkeys the radio unconditionally. That covers every
failure this firmware can detect, and the teardown ladder deliberately destroys
its own socket to trigger it.

What it does not cover is a client that dies *instantaneously* — power pulled,
brownout, a crash with no chance to close the socket. AetherSDR never pings its
TCI clients and has no idle timeout, so detection falls to TCP retransmission.
On Linux, `tcp_retries2` defaults to 15, which is roughly **13–15 minutes with
the radio keyed**.

No firmware on the device can close this. It needs an application-layer lease:

- While a client holds PTT, require some liveness signal within N seconds
  (a WebSocket pong is sufficient and costs the client nothing).
- On expiry, call the `abortTciPtt()` path that already exists.
- Roughly 50 lines and a timer, at a hook that is already there and already
  does the right thing.

**Action:** file an issue against `aethersdr/AetherSDR` describing the failure
mode and proposing the lease. Measure the real duration first (unplug the
device while keyed into a dummy load and time how long the rig stays keyed)
so the report carries a number rather than a theory.

Until this lands, the README says — and should keep saying — that this device
is not a primary PTT source for unattended operation.

## Upstream: expose compressor gain reduction over TCI

Smaller than the PTT lease, and worth bundling into the same conversation.

`tx_sensors` carries exactly five fields — `mic_dbm`, `fwd_watts`,
`peak_watts`, `swr`, `alc_dbfs` — and there is no compressor reading anywhere
in AetherSDR's TCI surface. ALC is available and useful, but it is **not**
compression: ALC is the radio limiting drive to protect itself, whereas a
speech compressor deliberately reduces dynamic range to raise average power.
An operator setting compression needs the gain-reduction figure, and AetherSDR
already computes one for its own meter (`meter.gainReduction` exists in the
theme).

Note that `peak_watts` is currently the same cached value as `fwd_watts`
("peak ≈ avg for now"), so a sixth field is not the only thing worth revisiting
in that payload.

**Action:** ask whether a compressor gain-reduction field can be appended to
`tx_sensors`. Index-based parsers ignore trailing fields, which is exactly how
`alc_dbfs` was added, so it is backward compatible by construction.

## Upstream: compressor gain reduction, and a real peak-power figure

Smaller than the PTT lease, and worth raising in the same conversation.

`tx_sensors` carries exactly five fields — `mic_dbm`, `fwd_watts`,
`peak_watts`, `swr`, `alc_dbfs` — and there is **no compressor reading anywhere
in AetherSDR's TCI surface**. ALC is available and useful, but it is not
compression: ALC is the radio limiting drive to protect itself, whereas a
speech compressor deliberately reduces dynamic range to raise average power.
An operator setting compression needs the gain-reduction figure, and AetherSDR
already computes one for its own meter (`meter.gainReduction` exists in the
theme).

Separately, `peak_watts` is currently the same cached value as `fwd_watts`
(`"peak ~ avg for now"`, TciServer.cpp), so the two are indistinguishable on
the wire. This firmware reads the peak field regardless, so it becomes correct
the moment upstream fills it in.

**Action:** ask whether a compressor gain-reduction field can be appended to
`tx_sensors`, and whether `peak_watts` can carry a real peak. Index-based
parsers ignore trailing fields — which is exactly how `alc_dbfs` was added —
so an extra field is backward compatible by construction.

## Firmware

- [ ] **Endurance soak.** Nothing has run for 24 h. Watch free internal heap,
      task high-water marks, `hap_drops`, WS closes and audio underruns.
- [ ] **Tabular-figure font.** Montserrat is proportional, so digits shift
      width as they change and the readout shimmers slightly while tuning. A
      subset of a monospaced-digit face to `0-9 . M k H z` is about 18 kB.
- [ ] **On-screen provisioning.** WiFi and host currently come from Kconfig via
      NVS seeding. A SoftAP captive portal plus an on-screen host editor would
      remove the reflash-to-change-networks step.
- [x] **Persist settings to NVS** — volume, mic gain, the dim and dark
      timings, the update interval, WiFi, host and page credentials. Not
      rotation, which only the serial console can change, and the USB build
      has no console.
- [ ] **Slice following.** The knob can only *follow* focus: AetherSDR ignores
      `active_slice` SETs and `set_in_focus` is a stub, so a slice *selector*
      is not implementable against today's server.
- [x] **OTA.** Signed images from the `firmware` branch: on WiFi the knob
      checks and asks on the dial; over USB the configuration page downloads
      and pushes the image.
- [ ] **PTT slab as an antenna selector.** A setting on the configuration
      page for what the bottom slab is: *PTT* (as now), *RX antenna* or
      *TX antenna*. As an antenna selector it works like the mode and filter
      editors — tap it, turn the knob to scroll through the radio's antennas,
      tap to accept — and the slab shows the selected antenna in place of the
      "PTT" text. First check what AetherSDR's TCI exposes for RX/TX antenna
      selection; if nothing, it is an upstream request like the ones above.
      With the slab repurposed the knob has no PTT at all, so the red TX
      screen must still follow the radio when it is keyed from elsewhere.
- [ ] **Icom IC-705 / IC-7300 MK2.** When an IC-705 is at hand. Same repo,
      the radio chosen at build time: everything but the TCI client is
      shared. Both ways in are worth having — over WiFi straight to the radio,
      which is what makes it useful on the go, and through a computer, as with
      AetherSDR. The ground is laid:
      - [x] one firmware per radio, chosen at build time
        (`idf.py -D VFO_RADIO=…`) and named for it: `vfo-knob-aethersdr`;
      - [x] an update channel per radio, `firmware/<radio>/`, so a knob is only
        ever offered its own releases (`RADIO=… tools/release.sh`);
      - [x] another radio's image is refused as an update, by upload and by
        OTA alike; switching is deliberate, from the radio chooser under
        **Firmware** on the configuration page (Icom listed, not yet offered);
      - [x] the same signing key and `partitions.csv` for all of them.
      Still to do: put the radio behind an interface — `main/app_main.c` and
      `components/webcfg` still call the TCI client directly — then write the
      Icom client as the second implementation, add `icom` to `_vfo_radios`
      in `CMakeLists.txt`, and offer it in the chooser.

## Known hardware quirks

Recorded so they are not rediscovered — see the README for detail.

- SH8601 display, not ST77916. Honours MADCTL `MX` but not `MY`.
- The knob is a bidirectional switch, not a quadrature encoder.
- PDM microphone capture is I2S0-only; the DAC must use I2S1.
- A reset mid-I²C-read leaves a slave holding SDA low; `board_init()` clocks
  the bus free.
