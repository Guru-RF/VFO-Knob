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

**Filed** upstream as [aethersdr#5985](https://github.com/aethersdr/AetherSDR/issues/5985),
describing the failure mode and proposing the lease. A measured duration
(unplug the device while keyed into a dummy load and time how long the rig
stays keyed) would give it a number rather than a theory.

Until this lands, the README says — and should keep saying — that this device
is not a primary PTT source for unattended operation.

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
- [x] IC-9700 — 2 m, 70 cm and 23 cm, each band's power in its own watts
- [x] IC-R8600 — a receiver: no PTT; its three antennas on the swipe
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
- [x] Web-888 -- the kiwi firmware, Kiwi888, over KiwiSDR's protocol (see
      Firmware), its owner's limits kept; TerraBooster, one of the four
      Lombardsijde Web-888s, streaming on the knob, 2026-10-03
- [x] KiwiSDR -- the same firmware; ON3RVH's (KiwiSDR v1.902, De Haan)
      behind Cloudflare, https:// only: its 301 followed, TLS in 1.3 s,
      logged in and streaming on the knob, 2026-10-06. Through the
      kiwisdr.com proxy not yet: N0BQV's answered port 80 with a 307 on
      2026-10-05 and nothing on 80 or 443 the day after

wfview's source (`src/radio/`) speaks the Yaesu (SCU-LAN10) and Kenwood
network protocols as well as Icom's: a reference for those clients.

## Firmware

- [ ] **Every firmware's settings on the SD card** (components/kvstore,
      SETTINGS-ON-CARD-PLAN.md). Built and on the bench knob (2026-10-09):
      every setting but the WiFi, the Bluetooth records and the boot counter
      is held in RAM and written to `VFO-CFG` on the card -- three copies,
      read back, one-cluster files written in place -- or to NVS without a
      card. Its first start moved vfo, svx, svxpki, sl, flex, phone and icom
      onto the card (0.6 s); later starts read them in about 55 ms; a save
      takes 3-20 ms; the WebSDR and OpenWebRX firmwares, switched back and
      forth, each found their receivers. The NVS copies stay where they are,
      frozen, so a released firmware still finds its settings, and what it
      changes there is merged back at the next start.
      - [x] power cuts (2026-10-09, the bench knob's built-in "APPSD" 480 MB
            card, a test build writing a count and 4 kB beside it): with one
            save every 5 s, as in use, 10 cuts, every save kept, cuts within a
            second or two of a save among them. With 25 saves a second
            nonstop, 7 cuts: 5 kept everything, 2 lost the last 2.4 s and 18 s
            of saves the card had already taken and read back -- the card's
            own controller going back, not the store. Every start, in both,
            loaded a whole save, never a torn one.
      - [x] a failing card, made up in a test build (`-D VFO_KV_TEST=1`,
            /api/kv/test): crashes at random and halfway through a write (the
            next start leaves the card alone, the one after rejects the torn
            copy), a crash while reading, a damaged copy, unreadable copies
            (that namespace off the card, SD CARD FAULT), no card (NO SD CARD,
            a change made meanwhile merged on when it is back), "Carry on
            without the SD card" and "Use the SD card again" (a restart that
            merges; it first overwrote the card with NVS's older copy -- fixed)
      - [x] released in every firmware, v1.20.0 (2026-10-09), the NVS copies
            still frozen
      - [ ] a later release round, after the user's days of testing: then the
            NVS copies emptied after a confirmed start, the floor version
            (older firmware refused), deletions made while the card was away
            (`kvs/fbdel`), the page's login without the card (`kvs/wver`),
            the boot counter's RTC record
- [ ] **Guides for OpenWebRX and WebSDR**, released in v1.20.0 without
      them: docs/owrx.md and docs/websdr.md with their tools/mkdocs.py
      screens, as for the other firmwares (the website pulls them).
- [x] **PTT keyed on the lift of a tap.** Acting on the touch, as it did, a
      swipe up begun on the slab keyed the radio before it could be seen to
      be a swipe. A tap now keys as the finger lifts without having moved; on
      the air a touch still unkeys at once. Tried with a dry-run build
      (`VFO_PTT_DRY_RUN`): swipes up and sideways from the slab keyed nothing,
      taps all did (2026-09-30).
- [x] **A Bluetooth headset, through the board's second chip** (2026-10-01).
      The companion firmware (`companion/`) makes the ESP32 beside the S3 a
      hands-free audio gateway; `components/bt_link` is the knob's end of the
      UART between them. The headset is the knob's ear, its microphone and,
      with its call button or optionally its boom arm, the PTT; the slab shows
      it, mute in red. Tried with a Jabra Evolve2 65: listening and keying on
      the IC-705, overs through the SvxLink parrot (TG 9990).
      - [x] the companion's updates through the knob (v1.18.0): signed
        images over the link into the chip's other slot, kept only once the
        new one has started and talked to the knob
        (COMPANION-UPDATE-PLAN.md). Each knob's second chip needs the bench
        step once, by cable, before it takes them.
      - [x] a Bluetooth speaker (A2DP), tried with a JLab speaker
        (2026-10-04): told from a headset by its class, its services and how
        it drops a call's audio, or by hand on the page (**Use as**); the
        knob's own microphone again while a speaker plays, and the speaker
        silent on the air;
      - [x] the speaker's volume from the knob's (AVRCP absolute volume), and
        any speaker sent a quarter of the level (12 dB less): tried with the
        JLab, 2026-10-05 -- VOLUME 2 sets it to 3 of 127, "ideal";
      - [x] the headset's or speaker's battery, beside the Bluetooth logo on
        every face -- green from half its charge up, yellow under half, red
        at a fifth and below; white on the red slab -- and its charge on the
        configuration page (decided 2026-10-04): built; the JLab speaker's
        battery seen on the knob (2026-10-05).
        The second chip answers Apple's `AT+XAPL` as an iPhone does, wanting
        the battery alone, and takes `AT+IPHONEACCEV` (in tenths) -- the
        Jabra offers the one, the JLab speaker sends both on its hands-free
        link. HFP's own indicator, `AT+BIEV`, only comes unasked: ESP-IDF
        5.5's Bluedroid offers a headset no HF indicators (its `+BRSF` masks
        the bit out, and it has no `AT+BIND`). Unknown AT commands, answered
        with nothing until now (`esp_hf_ag_unknown_at_send` refuses a NULL),
        get ERROR. The charge goes with the hands-free link it came over: a
        speaker that closes that link and plays on shows none, not a stale
        one.
      - [x] a level for each headset or speaker (the user, 2026-10-06: the
        Sony SRS-XB100 -- AVRCP, its own volume the knob's -- far quieter than
        the JLab at the same 12 dB down): **Level** beside the device on the
        configuration page, -24 to +12 dB in 3 dB steps (each the square root
        of two: -12 is the old quarter exactly), heard at once; a speaker
        -12 dB until set, a headset 0 dB. The JLab (its own volume the knob's)
        gets the very samples it did once its swell is done, the swell block
        for block as long and each sample within a least bit, and the Jabra
        each within a least bit; a speaker that keeps its own volume the true
        quarter where the old sums cut it short -- 1.9 dB more at VOLUME 1, a
        few tenths at the other odd ones below 10. On top of the VOLUME, and
        of the full-level path, in place of the fixed "/4". Above 0 dB, where
        a loud passage can pass full scale, the audio is held back a block
        (10 ms, 15 at 16 kHz; the speaker's delay counts it) and the gain
        turned down smoothly ahead of it and back up a 32nd a block, never
        clipped: each block brought down whole by its own peak, as first
        built, clicked at the block edges on CW at +9 and +12 dB (the review,
        2026-10-06). At 0 dB and below nothing is held. Kept on the S3 by
        address for the last eight set (NVS btlink/levels, written by the
        supervisor 2 s after the last step, never during an over or a call,
        and on Kiwi888 at a quiet moment or 30 s on, as its VOLUME -- merged
        for 1.19.0; it moves to the card with the rest of btlink), forgotten
        with Forget it;
        `GET /api/bt` `level`/`level_default`, `POST /api/bt do=level` (the
        whole field a step, else 400). The arithmetic and the records in
        `components/bt_link/bt_level.c`, tested in test/host (`test_bt_level`:
        the old tap's sums matched; keyed CW and speech at +12 dB, no gain
        step past a 500th of full scale where the first build's reached a
        half); the page's control tried with node. On the knob, 2026-10-06:
        the Sony raised to 0 dB, and at 0 again after a restart; the row's
        note before its buttons, so that neither jumps as it comes and goes
        (the user). By ear still: the JLab at -12, the Jabra at 0, a level
        above 0 dB on loud audio.
- [ ] **The knob's own battery** (asked 2026-10-05): its charge at the top of
      the arc, over the S-meter's reading, as a phone's status bar has it, on
      every face, the setup firmware's too, while the knob runs on it -- a
      headset's battery's glyph and colours; none on USB power, where the rail
      is the cable's and the cell cannot be read, nor on a radio's face on the
      air, where the transmit scale's numbers are. The 5 V rail through
      BATT_ADC (GPIO1, halved by R62/R63): the battery under 4.20 V, which no
      cell exceeds, USB from 4.30, and between them as it was; two readings
      in a row for a plug or an unplug (measured 4.46-4.56 V on USB,
      4.08-4.10 just off a full charge). A USB port at its spec's lowest,
      4.75 V, gives about 4.30 past the diode, a weak one less: taken for
      the battery. The charge from a 4.2 V Li-ion curve less the knob's load,
      4.05 V full to 3.40 where the 3V3 regulator gives out, shown in fives:
      settling its first half minute on the battery, then smoothed over some
      30 s, and only down (`components/board/knob_batt.c`, its test in
      `test/host`). No reading at all without the chip's ADC calibration. The
      address card says **battery 85 %** or **on USB power**, the page and
      `/api/status` the same with the rail's volts; the AetherSDR firmware's
      FLIP USB-C only with power on the cable. `[PWR]` in the log once a
      minute and on a plug or an unplug. Built, every face checked on the PC
      (`tools/lvhost` slab-check); on the knob since 2026-10-05, its run-down
      under way:
      - [ ] unplugged just off a full charge: the battery within two
        seconds, full and green, 100 % on the card; plugged in again: gone
        within two, **on USB power**;
      - [ ] plugged in with the battery yellow or red, on an ordinary cable:
        the page's **Power** says **on USB power**, its volts well over
        4.30 -- a rail near or under it would be taken for the battery;
      - [ ] a long run-down, the `[PWR]` lines a minute apart: the curve
        against the cell's own, and the rail where the knob stops -- the
        table moves to fit.
- [ ] **Endurance soak.** Nothing has run for 24 h. Watch free internal heap,
      task high-water marks, `hap_drops`, WS closes and audio underruns.
- [x] **Tabular-figure font.** Montserrat is proportional, so digits shifted
      width as they changed and the readout shimmered while tuning. The
      readout is now Hack, fixed width, at 46 px: eleven glyphs, 4 kB of flash
      (`components/ui/font_hack_46.c`, its licence in THIRD_PARTY_LICENSES);
      seen on the knob 2026-10-04.
- [ ] **On-screen provisioning.** No reflash to change networks: the setup
      firmware's hotspot and sign-in page set the WiFi. Left: the radio's
      address, the last item below.
      - [x] the setup firmware (`VFO_RADIO=setup`): the **VFOKnob** hotspot
        with a captive portal for the WiFi, then the firmwares listed from
        `firmware/index.json` on the dial, one installed with the WiFi kept;
        and from any firmware, back to that list from the address card: three
        seconds on the S-meter or the card, a buzz, and a turn of the knob;
      - [x] the hotspot and its sign-in page from an Android phone: it was
        sent to the page, and the knob joined the network given there;
      - [x] the same from an iPhone, which looks for the portal differently
        (`hotspot-detect.html`): tried 2026-10-04, it works;
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
- [ ] **PTT slab as an antenna selector.** Decided 2026-10-04: no setting, the
      slab stays the PTT. A press held on it half a second, until the knob
      buzzes, opens the antennas -- RX ANT then TX ANT on the FlexRadio, the
      antenna on the IC-7610 and the IC-R8600 -- and there a tap keys about
      0.15 s after the finger lifts (the R8600 has nothing to key), so that a
      hold can be told from a tap. Built and tried on the PC (`tools/lvhost` slab-check,
      `tools/flexhost`); the knob's test left. AetherSDR's TCI not looked
      at for antennas yet.
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
        same subnet: built -- the knob listens on UDP 4992, and the
        configuration page's **On this network** lists each radio heard (its
        name, model, address and who is on it) with **Add**; tried on the PC
        against `tools/mock_flex.py`; the knob's test left (the FLEX-6600 at
        Lombardsijde, on the knob's own LAN);
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
        swipe (`rx_ant_list`): built -- RX ANT and TX ANT on the swipe down
        (and held on the slab), the memories on V/M as an Icom's channels,
        each by `memory apply` with its shift and tone then set by the knob
        (the radio leaves the last memory's offset, AetherSDR #1871); tried
        on the PC against `tools/mock_flex.py`; the knob's test left.
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
      - [x] the slab with voices, the spot chooser (live as it turns, since
        v1.18.4), TIME UP;
      - [x] the session's end (an hour without the password), and the
        dial's LISTEN AGAIN;
      - [x] a receiver on a LAN, in the clear: `http://<address>:8080`
        on a socket of its own (no esp-tls, 2 kB of internal RAM a connection
        spared), `https://` over TLS as before, the scheme kept with the
        address; tried on the PC against `tools/mock_ubersdr.py`
        (`make -C tools/uberhost test`: registered and bypassed, Opus
        streaming, tuned, NR2, spots and voices, an SSTV picture, TIME UP for
        a guest, the password's answers), and on the knob: no underruns;
      - [ ] the time a session has left, on the face, for a guest, at the
        left end of the slab (decided 2026-10-04): built, the knob's test
        left. Counted as ka9q_ubersdr counts it -- the session's limit from
        its first socket, on through a reconnect under the same UUID and the
        dial; a day's allowance where the receiver keeps one (/connection
        says what is left of it); an idle limit's last minute, which the
        knob's use gives back. "52 min", the seconds too in the last five
        minutes, in amber, red in the last; on the radio page and in
        `GET /api/radio` (`time_left_s`, `time_left_by`). A spot's call too
        long to stay centred beside it moves aside, whole; only one too long
        for the room left is cut with dots. Closed on 0:00, the session asks
        /connection again at once -- once: a receiver that plays on (its
        clock restarted) while its socket fails gets the backoff. Tried on
        the PC: `make -C tools/uberhost test` against
        `tools/mock_ubersdr.py` (`--time-limit`, `--idle-timeout`,
        `--day-limit`, `--day-check`, `/mock/restart`, `/mock/refuse`), the
        face with `make -C tools/lvhost slab-check`;
      - [x] the next receiver in the list when the one in use cannot be
        reached (the user, 2026-10-06: back home, the remote station's
        `http://ubersdr-tel.local:8080` sat on NOT FOUND). On the knob the
        same day: that name not found twice, 4 s apart, then ON6URE-TEL
        through its tunnel, playing 25 s after the restart and saved as In
        use (the timer's stack: 2,020 bytes never used). Its description not
        read -- the name not found,
        no answer, refused, TLS failed, a tunnel's or a proxy's page in
        its place (502, a 503 without "allowed", or HTML: no answer of an
        UberSDR's, which always says "allowed", a full one's 503 too) --
        the one in use tried again 4 s on, then the next in turn, once
        each, wrapping round; a round of them all, then the backoff, 2 s
        growing to 60. The first that lets the knob in plays and is saved
        as In use (by a timer, just before its session plays; the list
        keeps its order; not where the dial or the page chose another
        meanwhile; one that answers only to refuse is not). In the client,
        with no restart: the spots' task drops the last one's connection,
        bands, spots, voices and gallery; the noise filter goes with the
        knob, the last boot's too; the SSTV viewer closes. RECEIVER FULL,
        BUSY, DAY LIMIT, a password refused, TIME UP keep the one in use's
        turn; a stand-in that answers so is passed by for the next (the
        user, 2026-10-06); no hand-over while the knob's own WiFi is down;
        a single receiver as ever. The face names the receiver under the
        warning (NOT FOUND, then CONNECTING with the next one's name),
        `/api/radio` has `why`
        and `uber.rx`/`rx_name`, the radio page says it. Tried on the PC:
        `make -C tools/uberhost test` (handover, fullnext, standin,
        pwnext, chosen, allgone, fullstays, timeupstays, wrongpwstays,
        gone, tunnelgone, proxygone, blip, single; two mocks, `/mock/vanish`,
        `/mock/gateway[?status=503]`, `--full` and `/mock/full`, names
        never looked up), the face with `make -C tools/lvhost slab-check`;
      - [ ] (later, the user's call, 2026-10-04) the dial for UberSDR's
        own page in a browser, both ways: a
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
- [ ] **Kiwi888, native.** A firmware of its own, `kiwi`
      (`VFO_RADIO=kiwi`, `components/kiwi_client` on the session in
      `components/kiwi_proto`), receive only: a KiwiSDR or a Web-888 over
      KiwiSDR's protocol on the app path, the web SDRs' list its receivers,
      one in use and another taken over live. KIWI-PLAN.md, its steps:
      - [x] step 1, the smallest useful firmware: modes, filters, AGC
        presets, live switching (swipe up, a tap on the slab, the page's
        In use, `receiver=`), every refusal classed and kept to (day-limit
        marks, holds, the silent door, the HTTP statuses, the cap on its
        own attempts), the receivers' page's look; built, its limits passed
        against `tools/mock_kiwi.py` (`kiwi_client_limits`), 2026-10-03;
      - [x] step 2, the limits complete: `SET inactivity_ack` only after a
        touch or a turn of the knob (`ui_user_seq`, `radio_user_activity`),
        a minute apart at most, the UberSDR's ping on the same rule; the
        full mock (every login answer, rates, offset, a carrier, the SET
        record), `test_kiwi_proto` (ADPCM against kiwiclient's decoder,
        squelched frames) and `fuzz_kiwi`, 2026-10-03;
      - [x] the review's limits, here and in the second receiver: a login's
        answer always read (10 s, never cut short by a switch or a save); a
        login left unanswered by a receiver with time limits counted, in case
        it was a refusal; two logins at once each counted; a mark at rest
        after a stream keeps its strikes; "no hourglass" lifts only one that
        showed it; a full NVS (MEMORY FULL) and a run of crashes keep the knob
        from logging in on its own; the cap counts only tries that reached
        the receiver; REFUSED held; the branch on eaed41f, 2026-10-03; then
        a lift short of a restart (the time limits taken away) keeps the
        strikes, a refusal beside "no hourglass" stops that lift, and time
        up, kicked and refused hold through a crash, 2026-10-03;
      - [x] EchoTracer playing and tuning, on the knob; switching live --
        from ON3RVH's KiwiSDR to EchoTracer in 1.5 s, the one session closed
        before the next logged in; "sounds perfect" (the user, 2026-10-06);
      - [x] step 3, the second receiver on the session (`components/sdr_rx`
        on `kiwi_sess_run`, its own session and test code gone): the app
        path, the silent door and the HTTP refusals classed, CW on the dial,
        a Web-888's S-meter from its own reference, keepalive every 5 s, the
        courtesy cap; its limits and the fixes passed against the mock
        (`kiwi_limits`), 2026-10-04;
      - [x] step 5, the audio: half-band stages to 32 kHz or more and a cubic
        interpolator (flat within 0.07 dB to 5 kHz at 12 kHz, the rest at
        least 42 dB down), the drift trim on the ring's level, a stall's
        backlog left out in one jump, whole frames, in the stall's own
        silence when it comes at once or at twice the pace or more
        (TerraBooster's stall, 2026-10-03), the 30 s report in the log, rej=
        lost audio only, saves at quiet moments (`kiwi_audio`), 2026-10-04;
      - [x] decided (as recommended, unanswered): a backlog the network
        hands over slowly is played as it comes and cut back every few
        seconds, as before -- no new login at the live point; short stalls
        again and again grow the ring's target (`kiwi_link_t.target_max`):
        two breaks within two minutes, and it holds a stall as long as the
        last, to 0.7 s, the ring's room growing with it (the pre-roll too,
        `audio_out_set_preroll`), eased back half a frame each 30 s after
        three calm minutes, the trim draining the ring, never a cut; the
        target and the breaks in the 30 s report. Against the mock, 0.4 s
        every 8 s for two minutes: 2 breaks, 14 before; 0.6 s every 5 s: 2,
        23 before; a calm stream at 256 ms throughout; the right ear the
        same (`kiwi_audio`, `kiwi_two_ears`), 2026-10-04;
      - [x] step 4, the receivers on the dial: an unnamed one by the antenna
        its `/status` names ("RF.Guru " left off), once read -- before its
        first session, never for a label alone -- else by its address, the
        port kept where another receiver shares the host and only there
        (`kiwi_label`), so neither four on one address nor receivers on one
        domain look alike; the slab's second and third lines from it;
        the page's Test says antenna, whereabouts, model and `ext_api`, and
        names an unnamed receiver as it names itself (`kiwi_status_name`),
        which Save keeps;
        decision 4(b): "Add RF.Guru's receivers" fills in the four
        Lombardsijde Web-888s by address and port, logging in to none;
        decision 10: "Your name, for their owners" (`SET ident_user`,
        URL-encoded, VFO-Knob when empty), kept with the list (NVS `sdrid`)
        for every session -- the second receiver's too -- and told at once
        to the one playing; passed against the mock, 2026-10-04; and after
        review, a Test and a session never log in to one receiver at once,
        nor does a choice made before a Test's refusal spend a try
        (`kiwi_test_busy`), 2026-10-04;
      - [x] step 6, the rest of the face and pages: the noise filter on the
        right of the S-meter (OFF WDSP LMS SPEC, sent once the choice has
        rested), the squelch on the swipe from the right (`kwn`, `kwq`), OV
        in red after the reading, the S-meter in one `vu_band` object with
        its peak in red (`vu_band_led_color`: phase 3 of VU-PLAN starts
        here; `tools/lvhost/kiwi_face.c` against the arc stack), the mic rows
        hidden on a receive-only firmware, a squelch slider on the radio
        page (the IC-R8600's too), the day-limit lines under the caller's
        tag, 2026-10-04;
      - [ ] on the knob: NR, the squelch and OV by ear and eye on EchoTracer
        (the noise filter and the squelch heard working, 2026-10-06; an OV
        still to see: no signal strong enough came by);
        the owner's list showing the knob's name;
      - [x] step 8, a second Kiwi in the right ear (decision 11): kiwi joins
        `VFO_HAS_SDR`, the right ear `components/sdr_rx` on the left ear's
        dial, mode and passband (`sdr_rx_tune`, also with the left ear
        down), RIGHT EAR with OFF on the swipe down, BALANCE LEFT ... RIGHT,
        its line and reading in the page's link blue; a session of its own,
        each ear its receiver's owner's limits, the marks shared; never both
        ears on one receiver -- each ear refuses the other's (dimmed on the
        dial, a triple click; 409 on the pages and the API, `sdr=`), and one
        the other ear only leaves, or gets from a list saved under both, is
        waited out (`sdr_rx_primary_cb`, `sdr_rx_in_session`); its own 30 s
        report; the page's rows say "right ear", the radio page has its
        buttons and the balance; the mixer flushes the left ear's ring while
        the right plays; flash at quiet moments of both ears
        (`sdr_rx_quiet_cb`); the right ear's choice and balance under keys of
        its own (`kwr`, `kwrh`, `kwbal`), so a web SDR chosen beside a radio
        on another firmware starts no second session here. Against two and
        three mocks (`kiwi_two_ears`), and `tools/lvhost` (`make
        kiwi-check`), 2026-10-04;
      - [x] step 7, the guide's pictures: `kiwi_face()` and `kiwi_pictures()`
        in tools/mkdocs.py, 18 of them (the face named, RECEIVER FULL, tune,
        AGC, NR, mode, filter, the receiver, squelch, TIME UP, DAY LIMIT, OV,
        the headset, the right ear, its line, the left ear's refused,
        BALANCE, and the page's Receivers with Add RF.Guru's receivers and
        the owners' name), the kiwi palette in tools/mkdisplay.py, docs/kiwi.md
        and README.md with them, 2026-10-04;
      - [x] after review: a ring coming down after an eased target rides out
        a hiccup it holds, as at 256 ms -- never a cut (`flow_top`), and a
        stall that runs it dry meanwhile leaves its backlog out down to the
        eased target, no second cut (the mock's `--hold`, `kiwi_audio`'s
        eases and ease_stall); the right ear's receiver set with the list it
        is saved with (`sdr_save`'s `sel`), so the left ear never takes it in
        between; the left ear says its receiver before it looks at the right
        ear's; a receiver's `/status` read once a boot between the two ears
        (`kiwi_status_keep`, `kiwi_two_ears`' status_once); a mark at rest
        flushed only with both ears quiet; the right ear's next hold keeps
        what holds the left ear through a crash; `receiver=`, `sdr=` and
        `sel=` out of the list 400, nothing done; a Test of the right ear's
        receiver tested (`kiwi_two_ears`' tests); the host shim mixes as
        `mix_block` does (a ring short of a 10 ms block, an underrun),
        2026-10-05;
      - [x] merged with main's work after v1.18.4, 2026-10-05: the session
        reads where a receiver centres CW (`load_cfg`, whole or as it goes
        by), puts ten digits in the app path's stamp and tunes again once
        the audio flows -- the AGC and the squelch with it, which that input
        lets go by before it has a channel -- so either ear plays an
        UberSDR's Kiwi input (`kiwi_two_ears`' uber_input,
        `kiwi_client_limits`' cw_centre_read); a Bluetooth speaker beside the
        receiver's name, the name cut short of it, both ears in it (the
        guide's 19th picture); the readout in Hack;
      - [x] on the knob: a Bluetooth speaker, both ears playing -- the
        Marshall Stockwell III, the two ears mixed into it and BALANCE
        moving it; 0 dry, 0 skips (2026-10-06);
      - [x] decided (as recommended): a Bluetooth speaker's own volume
        buttons do not count as someone listening, for a receiver's idle
        limit -- only the dial and the glass do, as the guides say -- and
        the right ear logs in again at a list save, as the web SDR beside a
        radio does on every firmware: both kept as they are, 2026-10-05;
      - [x] decided and done (as recommended): the right ear on the left
        ear's AGC, noise filter and squelch, asked at every pass and sent
        at that ear's pace, the noise filter once rested
        (`sdr_rx_settings_cb`): two antennas compared fairly -- and both
        squelches closed a second a quiet moment for flash
        (`sdr_rx_audible`); and a web SDR that cannot reach the dial -- the
        right ear, and beside a radio on every firmware -- quiet and saying
        so, rather than playing its edge (kiwi_sess.h's `reach`,
        `kiwi_tune_reaches`: the dial within what the receiver says it
        covers): no login again, the receiver kept where it was tuned, no
        tune at every turn, silence fed at the stream's pace (no break, no
        growth of the target), its thin line gone, *can't reach* in amber
        where its reading was, S0 back within its range until its first
        reading, "out of range: it covers 0-30 MHz" on the pages, a log
        line each way; back within its range, the tune and the audio at
        once (`kiwi_two_ears`' follows_left, out_of_range and quiet_both,
        `kiwi_limits`' out_of_range, out_of_range_at_start and
        cw_at_the_edge, `tools/lvhost` kiwi-check and slab-check on every
        face with a web SDR, a picture in the kiwi, icom, multiflex and
        ubersdr guides), 2026-10-05. The left ear needs none: its dial is
        held within its receiver's range (`cb_range`), so the dial shows
        where it plays -- but in CW within the tone of a converter's bottom
        edge (on a 144 MHz offset, 144.0000 to 144.0005), where both ears
        keep the carrier at the edge and the station there is heard below
        its tone, or not at all: left as it is;
      - [ ] to decide: a session that starts with the dial already out of
        its receiver's reach (the radio on 6 m at boot, a KiwiSDR chosen)
        logs in, tunes its edge once and sits silent on one of its channels;
        and like one gone out of reach it sends nothing meanwhile, so an
        owner's idle limit may end it (*time up*, then held until chosen
        again). Wait for the dial instead, with no login, where an earlier
        session this boot said its range?
      - [x] the mix a Bluetooth speaker takes, before the volume since the
        AVRCP change: a block past full scale -- the leveller's 8x for a
        weak source still held as a strong one starts -- brought down whole,
        by 32767 over its peak, not clipped (`components/audio/mix_tap.h`,
        `test_mix_tap`); the jack's path as it was, 2026-10-05;
      - [ ] on the knob: the right ear's AGC, NR and squelch with the left
        ear's -- its noise filter heard following, 2026-10-06; a KiwiSDR in
        the right ear beyond its range done the same day (ON3RVH's, to 30
        MHz, with the left on 30.5 MHz: quiet, its line gone, "out of range",
        and playing again within a second back on 20 m); a speaker at a
        strong station's start, no crackle;
      - [x] on the knob: both ears on two of the Lombardsijde receivers --
        EchoTracer left, TerraBooster right (in 1.6 s), BALANCE from either
        alone to both, the right ear's thin blue line; their 30 s reports
        clean, 89 kB internal free with the speaker on (2026-10-06); the
        refusals by hand still to try;
      - [x] rebased onto v1.18.5, 2026-10-05: the knob's own battery at the
        top of Kiwi888's face too, clear of the arc and its red peak mark,
        the reading with OV after it and the right ear's line; a headset's
        or speaker's battery beside its logo, the receiver's name kept clear
        of them as the UberSDR's spot is -- centred where it fits so, else
        moved aside, whole, and only one too long for its room cut with dots
        (`call_place`); the address card's power line; a speaker sent a
        quarter of the level, rising into it, after the tap's headroom
        (`mix_tap.h`). Every placement checked on the PC (`tools/lvhost`
        slab-check's `slab_kiwi`), the guide's 21st picture. The other
        guides' *can't reach* pictures numbered after v1.18.5's: icom's 25,
        multiflex's 18, ubersdr's 23 (after the hand-over's 21 and 22, merged
        for 1.19.0). The setup guide's list with Kiwi888 in
        it; the UberSDR mock says the ports it holds (`LISTENING`), so
        `kiwi_two_ears` no longer loses one to a test running beside it;
      - [ ] on the knob: its battery over Kiwi888's face; a headset's or a
        speaker's battery beside a long receiver's name;
      - [x] receivers reached only over https, 2026-10-06 -- the kiwisdr.com
        proxy (n0bqv.proxy.kiwisdr.com: port 80 answered 307) and a KiwiSDR
        behind Cloudflare (kiwisdr.on3rvh.be: 80 answered 301, 8073 silent),
        the user's two: `https://` on the page and the API, kept with each
        receiver ("https://" before its host in NVS, the port a redirect
        moved it from after its own, "443/80": an old list reads as it did);
        /status over https and the session over wss, the Host the front
        routes on, the certificate checked against the bundle for that name
        (SNI) -- mbedTLS on the knob's own socket, its contexts in PSRAM, not
        esp-tls's 2 kB of internal RAM (`components/kiwi_proto/kiwi_tls.c`);
        the handshake a step at a time, `go_on` asked between them, and one
        connection's step at a time of all of them, the idle task's turn
        after a long one: the receivers' handshakes keep core 0 from its idle
        task for one step at most (its certificates, half a handshake's
        computing on the PC, up to a second or so on the S3), never near the
        task watchdog's 5 s; the newest four
        receivers' TLS sessions kept, so the session after the /status read,
        a Test after its read and a receiver chosen again resume with no
        handshake's worth of software ECC. An http:// receiver's 301, 302,
        307 or 308 to https on its own host followed at once, once, and kept
        (`sdr_moved`: the list in RAM at once, in flash at a quiet moment,
        never during audio), under the key it had (`kport`), its marks and
        holds its own still; any other redirect said, MOVED / "moved to
        <host>", not "not a kiwi"; a certificate that does not verify --
        signed by no authority in the bundle (the bundle's callback makes it
        mbedTLS's fatal error, its flags all set) or for another name --
        CERTIFICATE?, not a word spoken; both held until chosen again or the
        list saved. A page loaded before a redirect saves the receiver at
        its old address as it is now, https:// and passwords kept
        (`sdr_same`); the list's two flash writers one at a time. The page's
        Test on a task of its own, its stack in PSRAM (the web server's has
        no room for a handshake), its way to the login 25 s at most. Stacks
        for a session 16 kB, in PSRAM: a handshake took ~9 kB on the PC. A
        full socket over TLS waited for on the write alone (it spun at 100 %
        with audio still coming in). Tried on the PC:
        `tools/mock_kiwi.py --tls` (a front ending TLS before the mock, the
        run's own CA from `make_certs`), `--redirect`, `--tls-delay`;
        `kiwi_limits`, `kiwi_client_limits` and `kiwi_two_ears` with `--tls`
        (every scenario over TLS), and their tls_* scenarios -- redirects
        kept and refused, one hop, certificates refused as the bundle
        refuses them (`shim_tls.c` attaches as ESP-IDF does), marks across a
        redirect, a stale page's save, a quiet moment for the save, a switch
        in a slow handshake;
      - [ ] on the knob: kiwisdr.on3rvh.be done, 2026-10-06 -- its http://
        link's 301 followed, TLS in 1.29 s (1.15 s of it computing), a
        resumed one in 0.05 s, saved as https://, streaming, 91 kB internal
        free with one ear; n0bqv.proxy.kiwisdr.com answered nothing that day
        on 80 or 443 (its KiwiSDR off, most likely). Left: the proxy once it
        is up (and their http:// links, redirected),
        the handshake's time ("TLS in N s, M s of it computing": a resumed
        one's next to nothing) and the internal RAM free with both ears over
        TLS, the [STK] line for the kiwi, sdr and sdrtest tasks; both ears
        on https receivers at boot and a Test pressed meanwhile: core 0's
        idle and no task watchdog;
      - [ ] the website's list (KIWI-PLAN section 17), in that repo's session,
        after the user's go;
      - [x] its release, 1.19.0, merged with main's work since 1.18.5 (the
        UberSDR's hand-over, a level for each Bluetooth device): `soon:true`
        gone from the page's Firmware list, `dependencies.kiwi.lock` and
        `sdkconfig.kiwi` committed; `tools/mkdocs.py`'s VERSION 1.19.0 and
        every guide's pictures drawn again; a kiwi set in `tools/mkrender.py`,
        in its own palette (S9+10 on 20 m, EchoTracer on the slab);
        `tools/release.sh`'s NAMES and the setup firmware's list offer it,
        and the setup guide, docs/README.md and README.md name it with the
        others, 2026-10-06.
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
        (kiwi.on4cdj.be has one that resets) -- the app path alone since
        1.19.0, `/ws/kiwi/` dropped with step 3;
      - [x] a Kiwi's limits respected, not dodged: one whose owner gives apps
        no channels (`ext_api_nchans` 0, ON4CDJ's) cuts a client without a
        waterfall off after 10 s -- the knob says "no apps allowed" and does
        not come back until chosen again; app channels full, two minutes;
        the day's listening limit per address (`ip_limit`): marked through
        restarts, tried again only when chosen, twice at most (KIWI-PLAN.md
        step 0, released in 1.19.0);
      - [x] step 0 released together with step 3, in 1.19.0 (2026-10-06):
        step 3 built on the kiwi branch, 2026-10-04 -- the second receiver
        on the app path alone, with the silent door (120 s, then NO APPS),
        so a KiwiSDR 1.9's owner's limit on apps holds;
      - [x] on the same branch: an unnamed receiver on the RX chooser by its
        antenna once read, else by its address, with the port where another
        shares the host (four on one address told apart), Test filling an
        empty name, and who the owners see (`SET ident_user`) from the page,
        2026-10-04;
      - [x] on the same branch, with the Kiwi888 right ear: the SDR's ring
        kept fuller while short stalls come again and again (up to 0.7 s,
        its room in PSRAM grown by 21 KB), eased back once calm; the
        receiver's clock followed (the drift trim); a 30 s report in the log
        (`sdr: ring ... ms of ...`); its underruns counted
        (`audio_out_sdr_stats`); the mixer now flushes and trims the radio's
        ring while the SDR plays, 2026-10-04 -- released with step 3;
      - [x] streaming from the Web-888 (81.83.21.23:8077) on the IC-7610
        firmware, 2026-09-30;
      - [x] a receiver reached only over https -- behind the kiwisdr.com
        proxy or Cloudflare -- by its `https://` link, and an http:// one's
        redirect to https on its own host followed and kept: the same as on
        Kiwi888 (its entry above), built and tried on the PC, 2026-10-06;
        its list to flash at a quiet moment for the web SDR, the radio's own
        receive audio playing on, as for the other settings there;
      - [ ] on the knob: an https web SDR beside the IC-705 or the IC-7610
        (n0bqv.proxy.kiwisdr.com, kiwisdr.on3rvh.be), Test and listen: the
        sdr: log's TLS lines, the redirect kept;
      - [x] listened to on the dial: the RX chooser, the balance, the
        levelling, the mute on transmit (the Icom firmware);
      - [ ] the UberSDR's Kiwi input (port 8073 on its own address, not the
        https tunnel): read in its source (kiwi_websocket.go) -- the knob's
        path, login, ADPCM and S-meter fit it as they are; it centres CW on
        the carrier (its load_cfg's -400..400) where a KiwiSDR centres it on
        500 Hz, and makes its channel from the first SET mod, the passband
        only from the next. The knob now reads where the receiver centres CW
        and tunes the carrier that far below, as the Kiwi's own page does --
        on a KiwiSDR its CW was 500 Hz off before -- and tunes again once the
        audio flows; tried on the PC against the mock, both flavours; the
        knob's test left (its `enable_kiwisdr` on). Merged with Kiwi888 into
        the session (`components/kiwi_proto`), so its two ears have it too:
        `load_cfg` read whole or as it passes, ten digits in the app path's
        stamp (the Kiwi input takes it only so), the tune again once the
        audio flows, and the AGC and the squelch with it, which the input
        lets go by before it has a channel (applyAGC, applySquelch)
        (`test_kiwi_proto`, `fuzz_kiwi`, `make -C tools/uberhost test`),
        2026-10-05;
      - [ ] on the multiflex and xiegu firmwares.
      - [x] on the kiwi branch, with the right ear's: a web SDR that cannot
        reach the radio's frequency -- a KiwiSDR beside the IC-7610 on 6 m
        -- goes quiet and says so (*can't reach* in amber, "out of range: it
        covers 0-30 MHz" on the pages), its session kept, until the radio is
        back within its range, rather than playing its top edge; tried on
        the PC (`kiwi_limits`' out_of_range), 2026-10-05; the knob's test
        left.

## Known hardware quirks

Recorded so they are not rediscovered — see the README for detail.

- SH8601 display, not ST77916. Honours MADCTL `MX` but not `MY`.
- The knob is a bidirectional switch, not a quadrature encoder.
- PDM microphone capture is I2S0-only; the DAC must use I2S1.
- A reset mid-I²C-read leaves a slave holding SDA low; `board_init()` clocks
  the bus free.
