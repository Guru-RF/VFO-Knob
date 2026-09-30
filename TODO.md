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

## Radios

Ticked once the knob has worked with the radio itself, not only built for it.

**Icom**, the icom firmware, over the radio's own LAN:
- [x] IC-705
- [x] IC-7610 — MAIN/SUB and the antennas on the swipe down (after RX),
      RF gain and power from the left, its tuner from the right, and the web
      page's controls
- [ ] IC-7760
- [ ] IC-9700
- [ ] IC-R8600 — a receiver: no PTT
- [ ] IC-7300 MK2

**Yaesu**, through the YAESU SCU-LAN10 interface, which these need:
- [ ] FT-710
- [ ] FTdx10
- [ ] FTdx101 (D or MP)

**Kenwood**
- [ ] TS-890

**Elecraft**
- [ ] K4

**Xiegu**, the xiegu firmware, through the wfview server built into the radio:
- [ ] X6100 — control works (radio on APP 1.2.0, the newest); receive
      audio does not. Its WFSERVER (wfview's server; it says so at login)
      stops sending audio after anything from 0.4 s to 39 s and often never
      recovers, even in a new session. Measured from a PC with a prototype
      client: sooner at 24 kHz, the knob's rate, than at 48 kHz; sooner with
      silence streamed back to it; no help from a longer latency or from
      answering its resend requests as wfview does. Next: a wired link
      instead of its WiFi. If that holds, the firmware should ask for
      48 kHz and halve it for the knob's output.
      - the knob as the radio's network adapter: the xiegu firmware does
        USB networking, like the AetherSDR one, plugged into the radio's USB
        host port (its DHCP server gives the radio 10.55.42.2, where the
        knob finds its WFSERVER), with WiFi beside it for the configuration
        page. Tried: on the X6100's port the knob restarts over and over
        before its start-up finishes -- most likely the port cannot power
        it (unconfirmed: no boot there got as far as logging why). Needs a
        knob on its own battery;
      - or a USB-C Ethernet adapter on the radio (ordered).
- [ ] X6200 — said to behave the same

**FlexRadio**, the multiflex firmware, over its own API:
- [x] FLEX-6600 — on the LAN (a routed hop away) and through SmartLink

**OpenHPSDR**, a firmware of its own: the radios that speak the OpenHPSDR
protocol (1, or 2 for the newer ones) -- UDP on the LAN, discovered by
broadcast, IQ from the radio and demodulated in the knob:
- [ ] Hermes Lite 2
- [ ] Apache Labs ANAN (Hermes, Angelia, Orion)
- [ ] others on the protocol (Red Pitaya's STEMlab SDR transceivers ...)

**Web SDRs, native**: a receiver firmware of its own, with no radio -- the
dial tunes a web SDR itself, its S-meter and audio the knob's:
- [x] UberSDR -- the ubersdr firmware, over UberSDR's own protocol (see
      Firmware); ON6URE-TEL through its tunnel, 2026-10-01
- [ ] Web-888 and KiwiSDR, over the KiwiSDR protocol `components/sdr_rx`
      already speaks beside a radio (its owner's limits kept: a Kiwi that
      lets no apps listen stays out of reach)

wfview's source (`src/radio/`) speaks the Yaesu (SCU-LAN10) and Kenwood
network protocols as well as Icom's: a reference for those clients.

## Firmware

- [x] **PTT keyed on the lift of a tap.** Acting on the touch, as it did, a
      swipe up begun on the slab keyed the radio before it could be seen to
      be a swipe. A tap now keys as the finger lifts without having moved; on
      the air a touch still unkeys at once. Tried with a dry-run build
      (`VFO_PTT_DRY_RUN`): swipes up and sideways from the slab keyed nothing,
      taps all did (2026-09-30).
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
      - [x] **memory mode**, now V/M at the end of the swipe down (a swipe
        down of its own at first, briefly a swipe up): the channel's name, number,
        frequency, shift and tone in place of the frequency, the knob stepping
        through the programmed channels of one group (`1A 00` reads them,
        `08 A0`/`08` selects), and a second swipe back to the VFO, simplex
        (`07`, `0F 10`). The radio cannot be asked which channel or mode it
        is on, so the knob keeps its own and remembers it;
      - [x] a first `icom` release (1.7.0), offered in the radio chooser;
      - [x] **IC-7610**, in the same firmware: a model table keyed by the
        name the radio gives (`icom_client.c`), with its CI-V address from the
        radio, 30 kHz-60 MHz, its S-meter and Po scales, and its modulation
        inputs (`1A 05 00 91/92`, LAN = 5; a radio not in the table gets none
        switched). The swipe down chooses MAIN/SUB (`07 D0/D1`, read back with
        `07 D2`) and then ANT1, ANT2, ANT1+RX or ANT2+RX (`12 <ant> <rx>`),
        never mid-over; PTT is held off on SUB, which only listens. Run
        against the radio on 2026-09-30: `07 D2` and `12` answer, the
        modulation inputs read MIC, audio clean at 24 kHz. It answers only
        about seven CI-V questions sent at once, so the slow poll asks one per
        220 ms -- its power and tuner were never known before;
      - [ ] an over from the knob on each of the IC-705 and the IC-7610 into
        a dummy load: the LAN input switched and put back; PTT refused on SUB;
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
      - [x] TUNE, ATU and the tuner's MEM on the swipe from the right (down
        until the web SDRs took it);
      - [x] a first release (1.9.0), offered in the radio chooser;
      - [ ] as the dial for a station, what the radio does when the knob
        loses power mid-over: the station is still there, so it may stay
        keyed (key from the knob into a dummy load, pull the knob's power);
      - [ ] finding the radio by its discovery broadcast, for a radio on the
        same subnet (it is given by its IP address for now);
      - [x] SmartLink, for a radio away from home (`smartlink.c`, from
        AetherSDR's SmartLinkClient and WanConnection): the account's password
        grant, keeping only the refresh token; the server's register, radio
        list and connect, the server kept open (pinged) while the radio is
        ours; the radio over TLS, its certificate pinned on first use, `wan
        validate` with connect_ready's whole handle -- it holds a '|' of its
        own, and cut there the radio refuses it (500000B1) and hangs up; then
        `client ip` before `client gui`, nothing else before registering;
        UDP by `udp_register` and `ping`. Its radios on the swipe up after the
        configured ones, marked SmartLink. Tried on 2026-09-30 against the
        account and the FLEX-6600 at Lombardsijde (UPnP ports): connected in
        8 s from boot, slice, both Opus streams, no audio lost;
      - [ ] over SmartLink: an over (the user's call, into a dummy load), the
        dial for another station (its list comes only after registering
        there), and hole punching (neither a forwarded port nor UPnP);
      - [ ] the radio's memory channels, and its receive antennas on the
        swipe (`rx_ant_list`).
- [ ] **More than one radio.** Up to four per firmware (not the reflector's):
      a list on the configuration page, each with a name, one in use; a swipe
      up chooses another and the knob restarts into it -- the clients have no
      restart path -- with or without a link to the one it leaves, never on
      the air, the boot confirmed first so the restart cannot roll an update
      back. Not over the USB cable. The IC-705's memory mode moved from the
      swipe up to V/M at the end of the swipe down.
      - [x] the list, the dial's RADIO chooser, `/api/radios` and
        `/api/radios/switch`, the radio page's row; the IC-7610 and the
        IC-705 in the icom firmware's list (2026-09-30);
      - [ ] switched on the dial, both ways, and V/M on the IC-705;
      - [ ] on the multiflex and aethersdr firmwares.
- [ ] **UberSDR, native.** A firmware of its own, `ubersdr`
      (`VFO_RADIO=ubersdr`, `components/uber_client`), receive only
      (`VFO_RX_ONLY`): UberSDR's own protocol, as its v2 page speaks it --
      `/api/description`, `POST /connection` with a UUID of its own,
      `wss://…/ws` with Opus behind UberSDR's version-4 header, the spots on
      `/ws/dxcluster` -- over TLS through the tunnel or in the clear on a LAN.
      - [x] listening: the description, the session, 50 Opus frames a
        second at 24 kHz, the S-meter and the SNR from the frames'
        headers, tuning coalesced to the server's pace, the receiver's
        clock for the spots' ages (2026-10-01, ON6URE-TEL);
      - [x] the noise filter (set_dsp): NR4 asked for and confirmed;
      - [x] spots on the band (cluster and skimmer), aged by the
        receiver's clock, and the voices its detector hears, named or
        not, merged with them (a scanner came and went: the user wanted
        the voices on the slab instead);
      - [x] the SSTV gallery listed; a picture decoded (PNG, the ROM's
        inflate) exactly as a desktop decodes it, on the PC; on the knob
        0.7-2.7 s a picture over one kept-open TLS connection, the next
        ones fetched ahead into a four-picture cache;
      - [x] a KiwiSDR beside it: the Web-888 streaming in the right ear;
      - [x] seen on the glass (2026-10-01): the face, BALANCE (its 0 read
        "0 >" until the chain was fixed), the SSTV viewer (its "fetching"
        unreadable over a picture until it got a pill of its own);
      - [ ] the slab with voices, the spot chooser, TIME UP;
      - [ ] the session's end (an hour without the password), and the
        dial's LISTEN AGAIN;
      - [ ] a receiver on a LAN, in the clear;
      - [ ] the time a session has left, on the face, for a guest;
      - [ ] the dial for UberSDR's own page in a browser, both ways: a
        small browser extension (Chrome and Firefox) on the page's
        documented API (`static/v2/BRIDGE_API.md`, page API 1.8 in
        0.1.66) relaying to the knob over WiFi. The dial and the taps
        tune the tab (`tune`, `mode`, `passband`, `volume`, and `run`
        for any of its ~35 functions -- noise filter, squelch, VFO A/B);
        the tab's `tuning`, `signal` (the S-meter it shows, and the SNR)
        and `spots` topics come back to the knob's face. An extension,
        not a userscript: an https page may not open ws:// to a LAN
        address. Beside it, found in the same code and simpler, but one
        way only (the page sends a controller nothing back): the knob as
        a class-compliant USB MIDI device for the page's SDR Control
        panel, which learns any control onto those functions; and the
        desktop app's TCI server (port 60001) for the aethersdr firmware
        as it is, once the knob halves its 48 kHz audio (it sends no
        S-meter, only -127).
- [ ] **Web SDRs beside the radio.** For the icom, xiegu and multiflex
      firmwares (`VFO_HAS_SDR`, `components/sdr_rx`): a KiwiSDR, a Web-888 or
      an UberSDR's Kiwi input as a second receiver, over KiwiSDR's protocol
      (WebSocket, `SND` frames of IMA-ADPCM at 12 kHz, resampled to 24).
      - [x] up to four on the configuration page, each with a Test (its
        `/status`, a login, and whether its owner lets apps listen); chosen on
        the swipe down (RX: LOCAL, then the receivers), on the radio page and
        in the API (`sdr=`, `balance=`), and kept across a restart;
      - [x] following the radio: frequency, mode and passband;
      - [x] the radio left and the SDR right, each levelled to the same
        loudness, BALANCE first on the swipe from the left (then RF GAIN and
        POWER); the SDR quiet while transmitting;
      - [x] its S-meter as a thin blue line outside the radio's, and its
        reading in blue under the radio's in place of the dBm;
      - [x] both KiwiSDR paths: `/ws/kiwi/<ts>/SND` (KiwiSDR 1.9) and
        `/<ts>/SND` (the Web-888, older Kiwis), the one that answers
        remembered; every address a name stands for, in turn
        (kiwi.on4cdj.be has one that resets);
      - [x] a Kiwi's limits respected, not dodged: one whose owner gives apps
        no channels (`ext_api_nchans` 0, ON4CDJ's) cuts a client without a
        waterfall off after 10 s -- the knob says "no apps allowed" and does
        not come back until chosen again; app channels full, two minutes;
        the day's listening limit per address (`ip_limit`), half an hour;
      - [x] streaming from the Web-888 (81.83.21.23:8077) on the IC-7610
        firmware, 2026-09-30;
      - [ ] listened to on the dial: the RX chooser, the balance, the
        levelling, the mute on transmit;
      - [ ] the UberSDR's Kiwi input (port 8073 on its own address, not the
        https tunnel);
      - [ ] on the multiflex and xiegu firmwares.

## Known hardware quirks

Recorded so they are not rediscovered — see the README for detail.

- SH8601 display, not ST77916. Honours MADCTL `MX` but not `MY`.
- The knob is a bidirectional switch, not a quadrature encoder.
- PDM microphone capture is I2S0-only; the DAC must use I2S1.
- A reset mid-I²C-read leaves a slave holding SDA low; `board_init()` clocks
  the bus free.
