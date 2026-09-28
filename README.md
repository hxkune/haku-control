# haku control

One lighting scene for your PC **and** your room. haku control drives the RGB inside the case
(motherboard ARGB header, memory) and the lights on the wall (Nanoleaf, Wi-Fi bulbs) from a single
effect engine: colours flow from the RAM sticks through the water block onto the wall, or every
device runs its own effect.

![haku control](docs/screenshot.png)

- **Scene-based effects:** flow, caustics, bubbles, comet, lava, breathe, temperature and static.
  Each device can follow the main effect or run its own, with its own palette, colour or white (2700–6000 K).
- **Light on resources:** a small C core in the tray (~0.05 % CPU, a few MB of RAM). Unchanged frames are
  never sent. The settings window (WebView2) exists only while it is open.
- **Behaves like a light switch:** switching a device off in the app really turns it off. When the PC shuts down or sleeps,
  the room lights go dark (or keep / restore their own state, as you choose).
- **Local only:** no account, no cloud, no telemetry. Devices are controlled over USB, SMBus and your LAN.
- English and Russian UI.

> **Status: early.** haku control grew out of one person's setup, so the list of supported devices is
> short. Adding more is the main goal of the next releases (see the [roadmap](#roadmap)).
> Help with devices you own is very welcome: see [CONTRIBUTING.md](CONTRIBUTING.md).

## Supported devices

**Inside the PC**

| Device | How | Status |
| --- | --- | --- |
| Anything [OpenRGB](https://openrgb.org) supports (hundreds of boards, RAM, GPUs, keyboards, mice, fans…) | OpenRGB SDK server, every controller becomes a device | protocol tested against a simulator |
| MSI Mystic Light: onboard LED + JRAINBOW1 ARGB header | own driver, USB HID, 185-byte protocol | verified on MPG B650I EDGE WIFI (MS-7D73); other boards are not touched |
| ENE DRAM RGB, controller `AUDA0-E6K5-0101` (e.g. G.Skill Trident Z5 RGB) | own driver, SMBus via [PawnIO](https://pawnio.eu), AMD chipsets | verified; other controllers are detected and left alone |
| NVIDIA GPU temperature (for the temperature effect) | NVML | verified |

**In the room**

| Device | How | Status |
| --- | --- | --- |
| Nanoleaf Blocks / Shapes / Canvas / Lines | official OpenAPI + UDP streaming | Blocks verified |
| WLED (ESP8266 / ESP32 LED controllers) | UDP realtime (DNRGB), found via mDNS | tested against a simulator |
| Philips Hue (bridge) | local REST API, link-button pairing | tested against a simulator |
| Govee (lights with *LAN Control*) | official LAN API | tested against a simulator |
| LIFX | official LAN protocol | tested against a simulator |
| Yeelight (with *LAN Control*) | LAN JSON protocol, music mode | tested against a simulator |
| AiDot Wi-Fi bulbs (e.g. Linkind / "Matter Smart Light Bulb") | local LAN protocol, keys fetched once | verified with RGBTW bulbs |

"Tested against a simulator" means the protocol was checked with `tools/sim/fake_devices.py`, not yet on the
real device. Reports from owners are very welcome.

The app only writes to hardware it has positively identified. It never writes to the memory controller's
flash or changes SMBus addresses, and it restores each device's own effect when it exits.

## Install

Requirements: Windows 10/11 x64, [WebView2 Runtime](https://developer.microsoft.com/microsoft-edge/webview2/)
(built into Windows 11), and [PawnIO](https://pawnio.eu) for memory lighting.

1. Download `haku-control-setup.exe` from the latest release and run it. Windows asks for administrator rights:
   the app talks to the motherboard and memory, so it starts at sign-in through an elevated task.
2. The app appears in the tray and in the Start menu. On the first start, a short guide shows what was found and
   scans the network for lights.

The setup also updates an existing install (settings are kept) and can be removed from *Settings → Apps*.
`haku-control-setup.exe /extract <folder>` only unpacks the files. Without the installer, `scripts\install.ps1`
does the same from a build folder.

Settings, device keys and the log live in `%APPDATA%\haku-control`. To remove the app, run
`scripts\uninstall.ps1` (add `-Purge` to delete the settings too).

### Room lights

- **WLED, Hue, Govee, LIFX, Yeelight:** *Devices → Scan network*, then *Add*. Govee and Yeelight need
  *LAN Control* switched on in their apps. Hue asks you to press the bridge's link button after adding.
- **OpenRGB:** start OpenRGB with *SDK Server* on, then *Devices → Scan network* lists its controllers. If
  OpenRGB drives the same MSI board or memory as haku control's own drivers, set `[devices] msi=0` / `ene=0`.

- **Nanoleaf:** open the Nanoleaf tab, press *Pair*, then hold the controller's power button for 5–7 s.
- **AiDot bulbs:** run `pwsh -File "C:\Program Files\haku-control\aidot-setup.ps1"` once. It logs in to your
  AiDot account (the password goes only to AiDot and is not stored) and saves each bulb's local key. After that,
  everything runs locally.
- **Lights on the PC's own Wi-Fi (optional):** *Settings → Windows hotspot* keeps the Windows Mobile Hotspot
  running, so lights can join a network that exists whenever the PC is on.

### Phone

*Settings → Phone → Control from your phone* serves the same interface on your home network (port 8723).
Open the address it shows on the phone, enter the PIN from the PC once, and add the page to the home screen.
Only private network addresses are served (and Tailscale's, for control from outside through your own tailnet);
Windows asks once whether to allow the app on the network. Siri Shortcuts or scripts can send commands too:

```
POST http://<pc>:8723/api/cmd   header X-Haku-Token: <token from pairing>
{"cmd":"power"}   {"cmd":"effect","id":"lava"}   {"cmd":"brightness","v":40}
```

## Build

Needs Visual Studio 2019 or newer (or Build Tools) with *Desktop development with C++* and a Windows 10/11 SDK.

```bat
build.cmd
```

Output goes to `bin\`. `build.cmd dev` makes a test build in `bin-dev\`: it keeps its settings in
`%APPDATA%\haku-control-dev`, runs without admin rights and never touches the motherboard or memory, so it
can run next to the installed app. Stop it with `scripts\stop.ps1 -Dev`. `python tools/sim/fake_devices.py` starts
simulated WLED / OpenRGB / Govee / LIFX / Yeelight / Hue devices on this PC for it to talk to.

For UI work without the app, serve `ui\` with any static server and open
`index.html?mock`, for example `python -m http.server -d ui 8766`. The mock fakes the core with sample data.

### Layout

```
src/        C core: main loop, effects, PC drivers (dev_*.c), LAN drivers (drv_*.c, devices.c), window (ui_web.cpp)
ui/         settings page (HTML/CSS/JS, no framework), i18n.js holds all texts
res/        icons and version info
art/        logo sources and the icon generator
scripts/    install / uninstall / stop, AiDot key setup
tools/sim/  simulated LAN lights for testing
third_party WebView2 SDK loader, PawnIO SMBus module
```

## Roadmap

1. ~~Open source, CI builds, settings in `%APPDATA%`.~~
2. ~~First-start guide; network scan.~~
3. ~~LAN / bridge lights: OpenRGB bridge, WLED, Philips Hue, Govee, LIFX, Yeelight.~~ Next: confirm them on real
   hardware, more Mystic Light boards and ENE versions in the own drivers.
4. ~~Phone control from the local network.~~ Installer, signed builds, auto-update.

## License

Code: [MIT](LICENSE). The **haku** name and logo (`art/`, `ui/haku.png`, `ui/mark.svg`, `ui/icon-256.png`, `res/*.ico`) are not
covered by the MIT license. Please don't use them for other projects or forks you distribute.
Third-party components are listed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

Not affiliated with MSI, G.Skill, ENE, Nanoleaf, AiDot or NVIDIA; their names are used only to describe compatibility.
