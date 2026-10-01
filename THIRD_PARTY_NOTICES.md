# Third-party notices

## Bundled

**Microsoft WebView2 SDK** (`third_party/webview2`): the static loader library and headers, from the
`Microsoft.Web.WebView2` NuGet package. BSD-style license, see `third_party/webview2/LICENSE.txt`.

**PawnIO SMBus module** (`third_party/pawnio/SmbusPIIX4.bin`): from
[namazso/PawnIO.Modules](https://github.com/namazso/PawnIO.Modules) release 0.2.11, unmodified.
GNU LGPL 2.1, see `third_party/pawnio/COPYING`. The source code is available from that repository.
The PawnIO driver itself is not bundled; it is installed separately from https://pawnio.eu.

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
