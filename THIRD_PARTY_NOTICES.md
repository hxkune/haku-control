# Third-party notices

## Bundled

**Microsoft WebView2 SDK** (`third_party/webview2`): the static loader library and headers, from the
`Microsoft.Web.WebView2` NuGet package. BSD-style license, see `third_party/webview2/LICENSE.txt`.
**WebView2 Runtime bootstrapper** (`third_party/webview2/MicrosoftEdgeWebview2Setup.exe`, carried inside
`haku-control-setup.exe`): Microsoft's signed Evergreen bootstrapper 1.3.295.9, unmodified
(SHA-256 `90ae8eb0d9d22d6e51fccde49e4d25d266ff9343ad0cc931faa6f482569cea62`), redistributed as Microsoft allows for
WebView2 apps. haku's setup runs it silently when the runtime is missing; it downloads the runtime from Microsoft.

**PawnIO SMBus module** (`third_party/pawnio/SmbusPIIX4.bin`): from
[namazso/PawnIO.Modules](https://github.com/namazso/PawnIO.Modules) release 0.2.11, unmodified.
GNU LGPL 2.1, see `third_party/pawnio/COPYING`. The source code is available from that repository.
**PawnIO setup** (`third_party/pawnio/PawnIO_setup.exe`, carried inside `haku-control-setup.exe`): the official
signed setup of the PawnIO driver, [namazso/PawnIO.Setup](https://github.com/namazso/PawnIO.Setup) release 2.2.0,
unmodified (SHA-256 `1f519a22e47187f70a1379a48ca604981c4fcf694f4e65b734aaa74a9fba3032`). haku's setup runs it
silently when PawnIO is missing (the PC page can install it too, from that same release). PawnIO is licensed under
the GNU GPL 2.0; its source code is at [namazso/PawnIO](https://github.com/namazso/PawnIO). See https://pawnio.eu.

**Board fan controls** (`src/sio.c`): the Nuvoton NCT67xx Super I/O chip ids, register layout and the way manual
control is taken and given back are ported from [LibreHardwareMonitor](https://github.com/LibreHardwareMonitor/LibreHardwareMonitor)
(`LpcIO.cs`, `Nct677X.cs`). Mozilla Public License 2.0: that file stays under the MPL 2.0, see its header.
**PawnIO LPC module** (`third_party/pawnio/LpcIO.bin`), through which `src/sio.c` reaches the chip: from
[namazso/PawnIO.Modules](https://github.com/namazso/PawnIO.Modules) release 0.2.11, unmodified
(SHA-256 `b3896a1cab0d808fca31fe2ebcae045d59dac690da87b17c858bb8da357eb45e`). GNU LGPL 2.1, see
`third_party/pawnio/COPYING`. The source code is available from that repository.

**CPU temperature** (`src/cputemp.c`): the AMD Zen Tctl and Intel digital thermal sensor registers and formulas are
ported from [LibreHardwareMonitor](https://github.com/LibreHardwareMonitor/LibreHardwareMonitor) (`Amd17Cpu.cs`,
`IntelCpu.cs`). Mozilla Public License 2.0: that file stays under the MPL 2.0, see its header.
**PawnIO CPU modules** (`third_party/pawnio/AMDFamily17.bin`, `third_party/pawnio/IntelMSR.bin`), through which
`src/cputemp.c` reads the CPU: from [namazso/PawnIO.Modules](https://github.com/namazso/PawnIO.Modules) release 0.2.11,
unmodified (SHA-256 `dae74615761b78bdf064dfb3e136252ddcc6fc727d88f14738d0e5800d427a91` and
`d6ed85d65ab17a22f813ef98207d6d537155ee2ded5976a21cb48413c9b92e5f`). GNU LGPL 2.1, see `third_party/pawnio/COPYING`.
The source code is available from that repository.

**Lian Li wireless fans** (`src/lianli.c`): the receiver's discovery request and reply layout (device records, fan
count, rpm and duty fields), and the transmitter's connect, clock broadcast and speed command frames, are ported from
[MarcusSanchez/FanControl.LianLiWireless](https://github.com/MarcusSanchez/FanControl.LianLiWireless),
[Yoinky3000/LL-Connect-Wireless](https://github.com/Yoinky3000/LL-Connect-Wireless) and
[sgtaziz/lian-li-linux](https://github.com/sgtaziz/lian-li-linux), all under the MIT License:

> Copyright (c) 2026 Marcus Sanchez
> Copyright (c) 2026 Yoinky3000
> Copyright (c) 2026 sgtaziz
>
> Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
> documentation files (the "Software"), to deal in the Software without restriction, including without limitation the
> rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit
> persons to whom the Software is furnished to do so, subject to the following conditions:
>
> The above copyright notice and this permission notice shall be included in all copies or substantial portions of the
> Software.
>
> THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
> WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
> COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
> OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

## Downloaded on request

**OpenRGB** 1.0: not bundled. When the PC page finds lighting only OpenRGB can reach (or when asked to there), haku downloads the official Windows build
(`OpenRGB_1.0_Windows_64_81bbe18.zip`, SHA-256 `182a52a3c97c4c4ae52c80286b4260c9666c51c3447dbc416ee8945f66192e90`)
from [OpenRGB's releases](https://codeberg.org/OpenRGB/OpenRGB/releases/tag/release_1.0), unmodified, and runs it as a
separate program, talking to it only through its network SDK protocol. OpenRGB is licensed under the GNU GPL 2.0; its
source code is at [codeberg.org/OpenRGB/OpenRGB](https://codeberg.org/OpenRGB/OpenRGB).

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
