# VFO-Knob

A hardware VFO knob and control head for [AetherSDR](https://github.com/aethersdr/AetherSDR),
built on the Waveshare ESP32-S3-Knob-Touch-LCD-1.8. Tune, change step, key the
transmitter, watch the S-meter — over a USB-C cable or over WiFi.

<p align="center">
  <img src="docs/display-rx.svg" width="400" alt="Receiving: the S-meter rises to S9+20 and falls back while its readout follows and the 100 Hz digit ticks; S-units marked around the blue 66 mm body">
  &nbsp;
  <img src="docs/display-tx.svg" width="400" alt="Transmitting: SWR on the left, forward power on the right with the mic level inside it, both readouts following their bars; red PTT slab">
</p>

*Receiving, and transmitting, to scale in the 66 mm body. The readouts follow
the bars. The meter blocks are notched in the background colour, so the
separators appear where the bar has reached and vanish where it has not.
Everything outside the body is annotation: the knob draws the S-meter scale as
bare ticks, so its values are marked around the rim.*

It speaks **TCI v2.0** over a WebSocket, which is AetherSDR's own control
protocol — so the knob is not polling, it is told. Tune at the desktop and the
knob follows; turn the knob and the desktop moves.

---

## What it does

| | |
|---|---|
| **Tune** | Per-digit step selection: tap a digit to set the decade. Acceleration on top, so a flick crosses a band and a slow turn lands on 10 Hz. |
| **PTT** | Toggle — tap to key, tap anywhere along the bottom to unkey. A haptic reminder every 10 s while keyed, and a four-rung teardown that ends in dropping the socket. The transmit time-out is the radio's own. |
| **Meters** | S-meter in receive; SWR, auto-ranging forward power (to 2.5 kW) and mic level in transmit, each holding its peak for a second before it falls, so SSB reads as speech rather than flicker. The mic level uses AetherSDR's own scale: amber from −10 dB, red from 0. SWR above 2.5 runs the haptic motor for as long as it lasts. |
| **Audio** | RX audio out of the 3.5 mm jack, TX audio from the onboard mic, both with adjustable level. |
| **Mode / filter / RIT** | Tap to open, turn to choose, tap anywhere to accept. |
| **Network** | Tap the meter arc to see the knob's addresses. |
| **Branding** | RF.Guru boot splash in the palette of [rfguru.app](https://rfguru.app/), over the site's own backdrop. |

## Two transports

The knob is USB-powered, so it is always plugged into something — and whatever
runs AetherSDR is a computer with a USB port.

- **USB-C (preferred).** The knob enumerates as a **USB network adapter**
  (CDC-NCM), hands your machine an address and talks TCI over the cable. No
  configuration at all, and immune to what a machined metal case does to
  2.4 GHz.
- **WiFi.** Configure an SSID on the configuration page and power the knob from
  any charger.

The cable wins whenever a computer is on the other end of it — the knob then
waits for AetherSDR there rather than switching networks. With no computer on
its side of the cable — a charger, or the plug the wrong way round — it goes to
WiFi straight away. When the cable is chosen, WiFi is shut down — that frees
about 40 kB of internal RAM, which this board genuinely needs.

## First run

1. Plug the knob into the computer running AetherSDR. Windows 10 (version 1903
   or later), Windows 11, macOS and Linux all bring it up as a network adapter
   by themselves — there is nothing to install. **The USB-C socket only works
   one way round:** the other way it reaches the board's second chip, the knob
   finds no computer, and it says **FLIP USB-C**. Turn the plug over.
2. In AetherSDR, enable the TCI server — the `TCI` panel in the button bar —
   and set it to start automatically in Settings, so the knob finds it every
   time. That is the only setup on the computer.
3. Open **`http://10.55.42.1`** — user `admin`, password `admin`.
4. Change the password. The page will nag until you do; it can key a
   transmitter.
5. If you want WiFi, set an SSID and the AetherSDR host. The host is only used
   on WiFi: over the cable the knob always talks to the computer it is plugged
   into.

## If the page does not open

Give the knob about ten seconds after plugging in: for the first six it is a
serial port, which keeps it flashable, and only then a network adapter.

1. **FLIP USB-C on the knob**, or the computer finds a *USB Serial* (CH340)
   port instead — `USB\VID_1A86…` in Windows' Device Manager: the plug is the
   wrong way round. Turn it over. With WiFi set up the knob only says so for a
   few seconds after the splash before carrying on over WiFi, so watch it
   start, or look for that CH340 port.
2. **Windows.** The knob is under *Network adapters* in Device Manager —
   Windows 10 names it after its driver, *UsbNcm Host Device* — with
   *Hardware Ids* (Properties → Details) starting `USB\VID_303A&PID_4000`.
   Windows 10 before version 1903 has no driver for it: update Windows, or set
   WiFi up once over the cable from a computer where it works and run the knob
   from a charger. Firmware 1.3.2 and older is the exception to "nothing to
   install": update it. Until then, on Windows 10 it shows as an unknown device
   (Code 28) and needs its driver chosen by hand — right-click → *Update driver* →
   *Browse my computer for drivers* → *Let me pick from a list of available
   drivers on my computer* → *Network adapters* → **Microsoft**,
   **UsbNcm Host Device** — and on an up-to-date Windows 11 it fails with
   Code 10, which no driver choice fixes.
3. **Check the address.** The computer's side of the link should be
   `10.55.42.2` (`ipconfig` on Windows). A `169.254.x.x` address means it got
   no lease: renew it, or set `10.55.42.2`, mask `255.255.255.0`, no gateway
   and no DNS by hand. `ping 10.55.42.1` should then answer.
4. **Open the page** with the scheme typed out, `http://10.55.42.1/`. If ping
   answers but the browser does not, a VPN or a proxy is taking the request —
   disconnect it, or add `10.55.42.*` to the proxy exceptions.

**WiFi instead of the cable.** With no computer on its side of the cable the
knob goes to WiFi by itself, a few seconds after powering up — so a charger is
all it needs, once an SSID and the AetherSDR host have been set over the cable
from a computer where it works. Plugged into a computer it stays on the cable
even if that computer could not set the adapter up; keep a finger on the screen
from plugging it in until the dial appears to skip USB networking for that
boot.

## If the knob says NO LINK

The page opens but the knob stays on **NO LINK**: either AetherSDR's TCI server
is not running — see *First run* — or the computer's firewall is turning the
knob away. The knob connects *to* AetherSDR, so the computer has to let it in.

Over the cable the link has no internet access, which is correct: nothing lies
behind the knob. With no gateway on it, Windows lists it as *Unidentified
network* and treats it as *Public*, so if AetherSDR's firewall prompt was
answered for private networks only, the knob cannot reach AetherSDR's TCI port
over the cable. Allow it once, from an administrator PowerShell — 50001 is
AetherSDR's default; use the port set on the configuration page:

```powershell
New-NetFirewallRule -DisplayName "VFO-Knob TCI (USB)" -Direction Inbound `
  -Protocol TCP -LocalPort 50001 -RemoteAddress 10.55.42.1 -Action Allow -Profile Any
```

A *Block* rule always beats an *Allow*, and that prompt may have left one for
AetherSDR on public networks. If the rule above changes nothing, open
*Windows Defender Firewall with Advanced Security* (`wf.msc`), find
AetherSDR's *Public* TCP rule under *Inbound Rules*, switch it from Block to
Allow, and on its *Scope* tab set *Remote IP address* to `10.55.42.1`. Left
unscoped, that opens AetherSDR's TCI port — which can key the transmitter —
to everyone on every public network the computer joins.

A Linux firewall that drops inbound connections by default (ufw, or
firewalld's *public* zone) stops the knob the same way — on the cable, and on
WiFi from wherever your network puts it. Let both in once, e.g.
`sudo ufw allow proto tcp from 10.55.42.1 to any port 50001` for the cable and
`sudo ufw allow proto tcp from 192.168.1.0/24 to any port 50001` with your own
LAN's range for WiFi.

## Configuration page

Served on port 80 over whichever interface is up. Status, AetherSDR endpoint,
WiFi credentials, audio levels, access credentials, and firmware updates.

Opened over WiFi, the host field offers a green **my IP** — the address of the
computer you are browsing from, one tap to fill in when that is where
AetherSDR runs. Over the cable it is not offered: there the knob finds
AetherSDR by itself, and the cable's `10.55.42.2` would mean nothing on WiFi,
the only place the host setting is used.

> **It is HTTP Basic over plain HTTP.** A lock on the door, not a safe — treat
> the knob as something that belongs on a network you trust.

## Updates

Images are **RSA-3072 signed** and the signature is checked before an update is
accepted. Nothing is burned into eFuse and Secure Boot is not enabled, so the
board always stays ordinarily flashable — this protects the update path, not
the hardware.

- **On WiFi** the knob checks by itself — at boot, and every 24 h by default —
  and asks on the dial: **UPDATE x.y.z**, *tap here to install*. A tap on the
  question installs it and restarts into it; anything else — ten seconds, a
  turn of the knob, a tap elsewhere — and the question goes away and the dial
  carries on. It never asks while transmitting, and never installs unasked. A
  yes given mid-session restarts the knob first and installs at boot, where
  there is room for it. Set the interval to 0 on the configuration page to
  stop checking.
- **Over USB** the knob has no route to the internet — it is the DHCP *server*
  on that link. The configuration page does the checking and the downloading
  instead, then pushes the image over. Same image, same signature check.

If an update fails to boot, the bootloader rolls back to the previous slot. The
confirmation is tied to the same "this boot looks healthy" timer that clears the
boot-loop guard.

There is one firmware per radio, `vfo-knob-<radio>` — today only
`vfo-knob-aethersdr` — and each has its own update channel, `firmware/<radio>/`,
so a knob is only ever offered its own releases. It also refuses to install
another radio's firmware as an update; switching radios is a deliberate choice
under **Firmware** on the configuration page.

Publishing a release: `tools/release.sh 1.2.3 --push` (another radio's:
`RADIO=<radio> tools/release.sh …`).

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
  periodic haptic reminder and the very loud red screen exist, and why the
  radio's transmit time-out should be set: in AetherSDR, *Radio Setup → TX →
  Timeout*. The radio enforces it itself, so it holds even if the knob, the
  link or the computer does not. The knob no longer keeps a second one.
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

The display initialisation table in `components/panel/sh8601_init_cmds.c` comes
from Waveshare's demo for this board. Waveshare confirmed on enquiry that their
ESP32 demo code is Apache-2.0, so it carries the same terms as the rest and
redistributes cleanly. Full list in
[THIRD_PARTY_LICENSES](THIRD_PARTY_LICENSES).
