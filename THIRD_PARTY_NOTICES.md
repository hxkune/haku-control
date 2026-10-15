# Third-party notices

## Bundled

**Microsoft WebView2 SDK** (`third_party/webview2`): the static loader library and headers, from the
`Microsoft.Web.WebView2` NuGet package. BSD-style license, see `third_party/webview2/LICENSE.txt`.

**PawnIO SMBus module** (`third_party/pawnio/SmbusPIIX4.bin`): from
[namazso/PawnIO.Modules](https://github.com/namazso/PawnIO.Modules) release 0.2.11, unmodified.
GNU LGPL 2.1, see `third_party/pawnio/COPYING`. The source code is available from that repository.
**PawnIO setup** (`third_party/pawnio/PawnIO_setup.exe`, carried inside `haku-control-setup.exe`): the official
signed setup of the PawnIO driver, [namazso/PawnIO.Setup](https://github.com/namazso/PawnIO.Setup) release 2.2.0,
unmodified (SHA-256 `1f519a22e47187f70a1379a48ca604981c4fcf694f4e65b734aaa74a9fba3032`). haku's setup runs it
silently when PawnIO is missing (the PC page can install it too, from that same release). PawnIO is licensed under
the GNU GPL 2.0; its source code is at [namazso/PawnIO](https://github.com/namazso/PawnIO). See https://pawnio.eu.

## Effects

No code from these projects is copied. Ten of the effects (Rainbow, Fire, Ocean, Twinkle, Meteor, Plasma, Aurora,
Ripple, Matrix, Candle) follow well-known LED effects and were written anew for haku control's scene (every LED
takes its colour from its place in the room); they are credited for the ideas.

- [FastLED](https://github.com/FastLED/FastLED) (MIT): Fire2012 (Mark Kriegsman) and Pacifica (Mark Kriegsman and Mary Corey March).
- [WLED](https://github.com/wled/WLED) (EUPL-1.2): its effect collection (Aurora, Ripple, Meteor, Matrix, Twinkle and others).

## Protocol references

No code from these projects is included. They are credited because their public documentation and research
made the device support possible.

- [OpenRGB](https://gitlab.com/CalcProgrammer1/OpenRGB): MSI Mystic Light 185-byte and ENE DRAM protocol documentation.
- [python-aidot](https://pypi.org/project/python-aidot/): AiDot local protocol and app API.
- [Nanoleaf OpenAPI](https://forum.nanoleaf.me/docs): official documentation.
