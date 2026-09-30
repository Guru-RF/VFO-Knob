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

## Upstream: RF gain over TCI, and AGC changes announced

The dial has a place for the RF gain, right of the S-meter's reading, and on
AetherSDR nothing to put there: its TCI has `agc_mode` and `agc_gain` (the AGC
threshold) but no RF gain -- the panadapter's `rfgain`, -8 to +32 dB in 8 dB
steps on a FLEX-6400/6600 -- and neither its CAT nor its rigctl server carries
it either. So the AetherSDR firmware shows **RF.G** greyed out.

A smaller gap alongside it: AetherSDR announces an `agc_mode` change only when
another TCI client made it (`TciServer.cpp` wires `rx_nb_enable`, `rit_enable`
and the rest to the slice's signals, but not `agcModeChanged`). An AGC changed
on the desktop reaches no client, so the knob asks for it every 3 s.

**Action:** ask for an `rf_gain:<trx>,<dB>;` command -- GET, SET, and a
notification whenever it changes -- on the panadapter of that trx's slice,
with its range (AetherSDR already has it, `PanadapterModel::rfGainLow/High/
Step`) so a client need not hard-code one radio's steps; and for
`agcModeChanged` to be broadcast like the other slice flags. The knob's side is
then a few lines in `tci_client.c`: `radio_set_gain()`, and `have_gain` with
the range in `radio_get_status()`.

## Firmware

- [ ] **Endurance soak.** Nothing has run for 24 h. Watch free internal heap,
      task high-water marks, `hap_drops`, WS closes and audio underruns.
- [ ] **Tabular-figure font.** Montserrat is proportional, so digits shift
      width as they change and the readout shimmers slightly while tuning. A
      subset of a monospaced-digit face to `0-9 . M k H z` is about 18 kB.
- [ ] **On-screen provisioning.** WiFi and host currently come from Kconfig via
      NVS seeding. A SoftAP captive portal plus an on-screen host editor would
      remove the reflash-to-change-networks step.
      - [x] the setup firmware (`VFO_RADIO=setup`): the **VFOKnob** hotspot
        with a captive portal for the WiFi, then the firmwares listed from
        `firmware/index.json` on the dial, one installed with the WiFi kept;
        and from any firmware, back to that list from the address card: three
        seconds on the S-meter or the card, a buzz, and a turn of the knob;
      - [x] the hotspot and its sign-in page from an Android phone: it was
        sent to the page, and the knob joined the network given there;
      - [ ] the same from an iPhone, which looks for the portal differently
        (`hotspot-detect.html`);
      - [ ] the radio's address, and an Icom's login, still come from the
        configuration page after that (a phone will do, at the address the
        arc shows): the sign-in page could ask for them too.
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
- [ ] **Icom IC-705 / IC-7300 MK2.** Same repo, the radio chosen at build
      time: everything but the radio's client is shared. Both ways in are
      worth having — over WiFi straight to the radio, which is what makes it
      useful on the go, and through a computer, as with AetherSDR.
      - [x] one firmware per radio, chosen at build time
        (`idf.py -D VFO_RADIO=…`) and named for it: `vfo-knob-aethersdr`,
        `vfo-knob-icom`;
      - [x] an update channel per radio, `firmware/<radio>/`, so a knob is only
        ever offered its own releases (`RADIO=… tools/release.sh`);
      - [x] another radio's image is refused as an update, by upload and by
        OTA alike; switching is deliberate, from the radio chooser under
        **Firmware** on the configuration page;
      - [x] the same signing key and `partitions.csv` for all of them;
      - [x] the radio behind an interface, `components/radio/include/radio.h`,
        with `tci_client` and `icom_client` as its two implementations;
      - [x] `icom_client`: the IC-705's LAN protocol over WiFi — login, CI-V,
        RX and TX audio at 24 kHz, FIL1-3 — and the Icom colour scheme. Other
        LAN Icoms (IC-9700, IC-7610, IC-905) speak the same protocol but need
        their own CI-V address, meter calibration and frequency range;
      - [x] transmit tested into a dummy load: PTT and TX audio, the
        modulation input switched to WLAN for each over and put back after;
      - [ ] Po, SWR and ALC checked against the radio's own meters (SWR read
        5.4 at 70 cm into the dummy load: the load, or the calibration?);
      - [x] **AGC and P.AMP** either side of the S-meter's S-unit readout,
        each with an editor like the mode's: the IC-705's AGC (CI-V `16 12`:
        FAST, MID, SLOW) and preamp (`16 02`: OFF, P.AMP1 and P.AMP2 on HF and
        6 m, a single one on 2 m and 70 cm). On AetherSDR the AGC is TCI's
        `agc_mode` (off, slow, med, fast), and **RF.G** waits on the upstream
        item above;
      - [x] **memory mode** on a swipe down: the channel's name, number,
        frequency, shift and tone in place of the frequency, the knob stepping
        through the programmed channels of one group (`1A 00` reads them,
        `08 A0`/`08` selects), and a second swipe back to the VFO, simplex
        (`07`, `0F 10`). The radio cannot be asked which channel or mode it
        is on, so the knob keeps its own and remembers it;
      - [x] a first `icom` release (1.7.0), offered in the radio chooser;
      - [ ] **IC-7610**, in the same firmware: a model table keyed by the
        name the radio gives (`icom_client.c`), with its CI-V address from the
        radio, 30 kHz-60 MHz, its S-meter and Po scales, and its modulation
        inputs (`1A 05 00 91/92`, LAN = 5; a radio not in the table gets none
        switched). The swipe down chooses MAIN/SUB (`07 D0/D1`, read back with
        `07 D2`) and then ANT1, ANT2, ANT1+RX or ANT2+RX (`12 <ant> <rx>`),
        never mid-over; PTT is held off on SUB, which only listens. Built, not
        yet run against the radio: its first login was refused (user or
        password). To check there: the answers to `07 D2` and `12`, the LAN
        input's value, whether it transmits on SUB, and one over each on the
        IC-705 and the IC-7610;
      - [ ] through a computer: wfview's server speaks the same protocol, so
        the same client should reach a USB-connected radio behind a PC.
- [ ] **FlexRadio direct, as a MultiFlex station.** A firmware of its own,
      `multiflex` (`VFO_RADIO=multiflex`, `components/flex_client`, the
      Maestro's colours), that talks to a FLEX-6000/8000 itself, with no
      AetherSDR or SmartSDR in between: the radio's own API, TCP 4992 on the
      LAN. Tried against a FLEX-6600 on SmartSDR 4.2.20.
      - [x] a station of its own (`client gui`, its id kept so the radio gives
        its slice back), tuning only its own slice; its own transmit settings,
        which the radio keeps per station;
      - [x] or, chosen on the dial at boot, the dial and PTT for a station
        already there (`client bind`): its active slice followed, its PTT and
        its microphone, no audio on the knob;
      - [x] Opus both ways: the radio's 10 ms CELT frames in, the microphone
        out in mono (the uncompressed stream, 1.4 Mbit/s, crackled on WiFi);
      - [x] PTT through the interlock, refused with the reason -- out of band,
        or another station on the air -- and another station keying shown,
        never unkeyed;
      - [x] TUNE, ATU and the tuner's MEM on the swipe;
      - [x] a first release (1.9.0), offered in the radio chooser;
      - [ ] as the dial for a station, what the radio does when the knob
        loses power mid-over: the station is still there, so it may stay
        keyed (key from the knob into a dummy load, pull the knob's power);
      - [ ] finding the radio by its discovery broadcast, for a radio on the
        same subnet (it is given by its IP address for now);
      - [ ] SmartLink, for a radio away from home: its account login and TLS
        relay are a much bigger job than the LAN;
      - [ ] the radio's memory channels, and its receive antennas on the
        swipe (`rx_ant_list`).

## Known hardware quirks

Recorded so they are not rediscovered — see the README for detail.

- SH8601 display, not ST77916. Honours MADCTL `MX` but not `MY`.
- The knob is a bidirectional switch, not a quadrature encoder.
- PDM microphone capture is I2S0-only; the DAC must use I2S1.
- A reset mid-I²C-read leaves a slave holding SDA low; `board_init()` clocks
  the bus free.
