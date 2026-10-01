# VFO-Knob guides

One guide per firmware: what the knob shows and what your hand does, step by
step, with the knob's own screens. Written to be read here and pulled into the
website as they are — plain Markdown, and SVG pictures beside each page.

| Guide | Firmware |
|---|---|
| [Setup](setup.md) | `vfo-knob-setup` — what every knob ships with: WiFi from a phone, then the firmware for your radio |
| [Icom](icom.md) | `vfo-knob-icom` — the IC-705, the IC-7610, the IC-9700 and the IC-R8600 receiver, over the radio's own LAN |
| [FlexRadio](multiflex.md) | `vfo-knob-multiflex` — a MultiFlex station, on the LAN and through SmartLink |
| [AetherSDR](aethersdr.md) | `vfo-knob-aethersdr` — a dial for AetherSDR, over the USB cable or WiFi |
| [SvxLink](svxconnect.md) | `vfo-knob-svxconnect` — a node on an SvxLink reflector, no radio needed |
| [UberSDR](ubersdr.md) | `vfo-knob-ubersdr` — a dial for an UberSDR web receiver: its spots and voices, SSTV pictures, and a KiwiSDR beside it |
| [A Bluetooth headset](headset.md) | the companion firmware on the knob's second chip — a headset as the knob's ear, microphone and PTT, on every firmware but setup |

## The pictures

Each picture in `docs/<firmware>/` is drawn by `tools/mkdocs.py` from the
firmware's own texts, positions and colours — the same helpers that draw the
README's faces (`tools/mkdisplay.py`) — so they show what the knob shows.
When a screen changes in the firmware, change it there and run

```sh
python3 tools/mkdocs.py            # every guide's pictures
python3 tools/mkdocs.py setup      # one guide's
```

The pictures carry no scripts and no web fonts, and their ids are unique per
file, so they work as images and inline alike.
