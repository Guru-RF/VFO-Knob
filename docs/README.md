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
| [SVXConnect](svxconnect.md) | `vfo-knob-svxconnect` — a node on an SvxLink reflector, no radio needed |
| [Telephone](phone.md) | `vfo-knob-phone` — a telephone on one SIP account: favourites on the dial, a keypad, a call history, Google Contacts |
| [UberSDR](ubersdr.md) | `vfo-knob-ubersdr` — a dial for an UberSDR web receiver: its spots and voices, SSTV pictures, and a KiwiSDR beside it |
| [A Bluetooth headset or speaker](headset.md) | the companion firmware on the knob's second chip — a headset as the knob's ear, microphone and PTT, or a speaker as its ear, on every firmware but setup |

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
file, so they work as images and inline alike. A value in an editor's panel
is fitted as the knob fits it, with Montserrat's own widths read from LVGL's
font files (under `managed_components/`, fetched by the first
`idf.py build`), and a browser draws it that wide whatever font it has. The
frequency readout is the knob's Hack: each digit in the place `dig_place()`
gives it, named in Hack, then in the faces nearest it (DejaVu Sans Mono,
Menlo, Consolas) and last any monospace one, and drawn as wide as Hack's
advance in `components/ui/font_hack_46.c`.
