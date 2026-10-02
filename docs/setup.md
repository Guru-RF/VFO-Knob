# The setup firmware

Every knob ships with the **setup firmware** (`vfo-knob-setup`). It does two
things, with nothing but a phone: it puts the knob on your WiFi, and it
installs the firmware for your radio. Later, any radio's firmware can hand the
knob back to it, to install another.

The pictures below are the knob's own screens, drawn from the firmware's texts
and layout (`tools/mkdocs.py`); the orange marks are what your hand does.

## 1. Power on

The RF.Guru splash, then the setup firmware says it is starting.

![VFO-KNOB, Starting](setup/01-starting.svg)

If the knob already knows a WiFi network, it spends up to 25 seconds joining
it, and goes straight to [the firmware list](#4-choose-your-radios-firmware).
Otherwise it opens a hotspot of its own.

## 2. Join the knob's hotspot with your phone

The knob comes up as an open WiFi network, **VFOKnob**, and says so.

![WIFI SETUP: join the WiFi network VFOKnob with your phone](setup/02-hotspot.svg)

Join **VFOKnob** on your phone. The phone opens the knob's WiFi page by itself,
the way a hotel's sign-in page opens; if it does not, browse to
`http://192.168.4.1`.

## 3. Choose your network

The page lists the networks the knob can hear, strongest first. Tap yours,
enter its password — **Show** shows it as you type, so a phone's keyboard
cannot quietly change it — and tap **Connect**.

![The knob's WiFi page on a phone](setup/03-portal.svg)

The knob tries the network and says how it goes, on its face and on the page:

| On the knob | On the phone |
|---|---|
| ![Joining HomeNetwork](setup/04-joining.svg) ![Connected to HomeNetwork](setup/05-connected.svg) | ![Connected to HomeNetwork: now choose the firmware on the knob's dial](setup/06-portal-joined.svg) |

If it cannot join, both say why — **wrong password?**, **network not found** —
and the page lets you try again. Once it is on, the hotspot goes away after a
few seconds; the phone can be put away.

## 4. Choose your radio's firmware

The knob looks up the firmwares published for it, with their versions, and
shows them on the dial, one a detent. Turn to your radio's and tap the panel.

![FIRMWARE: INSTALL Icom 1.13.0 — turn to choose, tap the panel to install](setup/07-firmwares.svg)

| Firmware | For |
|---|---|
| **AetherSDR** | AetherSDR on a computer, over the USB cable or WiFi (TCI) |
| **Icom** | the IC-705 and the IC-7610, over the radio's own LAN |
| **FlexRadio** | a FLEX-6000/8000 as a MultiFlex station, on the LAN or through SmartLink |
| **SVXConnect** | an SvxLink reflector: the knob is the station |

The list comes from the update server each time, so a knob set up long after
it was made still offers every firmware there is by then. The last entry is
**WIFI — Set up again**: it brings the hotspot back, for another network.

![WIFI: Set up again](setup/08-wifi-again.svg)

## 5. It installs, and restarts into it

The knob downloads the image, checks its signature, installs it and restarts
into it. It keeps the WiFi settings: the new firmware joins the same network.
With an SD card in the knob, a copy of the image is kept on it, and a
firmware already there installs from the card in a few seconds — see
[The SD card](#the-sd-card).

| Installing | Done |
|---|---|
| ![UPDATING 64% — Do not unplug](setup/09-installing.svg) | ![UPDATING 100% — Restarting](setup/10-restarting.svg) |

Then tell the knob where its radio is, on its configuration page: see the
guide for that firmware.

## Back to the setup firmware, from any radio's

To install another radio's firmware, go back to the list from the knob itself.

1. Hold a finger on the S-meter, at the top of the face, until the knob
   clicks: the address card comes up — the firmware and its version, then
   the knob's addresses. Let go.

   ![The address card, held up on the S-meter](setup/11-address-card.svg)

2. Hold the S-meter (or the card) again, for three seconds, until the knob
   buzzes. It asks **FIRMWARE?** — turn the knob for yes. A tap, or ten
   seconds of nothing, says no.

   ![FIRMWARE? turn the knob for the picker](setup/12-firmware-question.svg)

3. The knob restarts, installs the setup firmware — from its SD card, or
   over WiFi — and shows the list again, the WiFi settings kept.

   ![REBOOTING into update mode](setup/13-rebooting.svg)

With something wrong — **NO LINK**, say, on a knob with another radio's
firmware — the warning panel shows the addresses in the card's place: hold
that until the knob buzzes. Without the setup firmware on its SD card it needs
WiFi: on the USB cable the knob then says **Needs WiFi** and carries on as it
was.

Each firmware keeps its own settings, so going back to one later finds its
radio, its login and its choices as they were.

## Another network, later

The knob keeps up to four WiFi networks — home, and a phone's hotspot, say —
and joins whichever is in reach, the one it joined last first. A network
chosen on the phone's page is added to them, not put in their place.

Take the knob where none of them reaches, and whichever firmware it runs puts
up **VFOKnob** again after about 25 seconds, with **WIFI SETUP** on its face:
join it with your phone and choose a network, as above, and the knob goes on
with its radio. It keeps looking for the networks it knows meanwhile, so back
home it simply joins again. Not over the cable: with a computer on it, the
cable is the way.

![None of its networks is in reach: join VFOKnob with your phone to add one](setup/14-away.svg)

The configuration page lists them under **WiFi**: add one there, give one a
new password, or remove one.

## The SD card

The knob keeps a copy of its firmwares on its microSD card. The setup
firmware fills it with every one published, in the background while it shows
the list, and every install puts its image there too; a firmware already on
the card installs from it in a few seconds, its checksum and signature
checked as for a download. So:

- switching firmware is quick, and going back to the setup firmware works
  without any network at all;
- with the update server out of reach, the setup firmware lists what is on
  the card, **From the SD card**, and installs from it.

The card keeps the second chip's firmware too, the Bluetooth headset's: the
setup firmware puts it there with the others, and a radio's firmware hands it
to the chip by itself when it is newer than the one the chip runs — see
[the headset guide](headset.md#the-second-chip).

Everything of the knob's is in one folder on the card, `VFO-KNOB`; nothing
else on it is touched. A knob without a card works as before, downloading
each time.

## If something is not right

| The knob says | What to do |
|---|---|
| **Could not join** *network*: **wrong password?** | Check the password on the phone's page and connect again. |
| **Could not join** *network*: **network not found** | The network is out of reach, or a 5 GHz-only one: the knob uses 2.4 GHz. |
| **None found. Is the network online? Trying again.** | The knob is on the network but cannot reach the update server; it keeps trying. |
| **WiFi would not start.** | Power the knob off and on again. |
| **WIFI SETUP**: **None of its networks is in reach** (a radio's firmware) | Join **VFOKnob** with your phone and add the network where the knob is now, or take it back within reach of one it knows. |
| **Needs WiFi** (from a radio's firmware) | The way back to the list is over WiFi: set up WiFi on that firmware's page, or use **Set up again**. |
