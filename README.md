# haku control

One lighting scene for your PC **and** your room. haku control drives the RGB inside the case
(motherboard ARGB header, memory) and the lights on the wall (Nanoleaf, Wi-Fi bulbs) from a single
effect engine: colours flow from the RAM sticks through the water block onto the wall, or every
device runs its own effect.

![haku control](docs/screenshot.png)

- **Scene-based effects:** flow, caustics, bubbles, comet, lava, breathe, temperature and static.
  Each device can follow the main effect or run its own, with its own palette, colour or white (2700–6000 K).
- **Profiles for every scenario** (gaming, work, night…): each keeps the effect and colours, which devices are on
  and the widgets on the Effects page. Switch them above the widgets, from the tray, the phone or a hotkey.
- **Light on resources:** a small C core in the tray (~0.05 % CPU, a few MB of RAM). Unchanged frames are
  never sent. The settings window (WebView2) exists only while it is open.
- **Behaves like a light switch:** switching a device off in the app really turns it off. When the PC shuts down or sleeps,
  the room lights go dark (or keep / restore their own state, as you choose).
- **Local only:** no account, no cloud, no telemetry. Devices are controlled over USB, SMBus and your LAN.
- English, Russian and French UI; dark, grey and light themes.

> **Status: early.** haku control grew out of one person's setup, so the list of supported devices is
> short. Adding more is the main goal of the next releases (see the [roadmap](#roadmap)).
> Help with devices you own is very welcome: see [CONTRIBUTING.md](CONTRIBUTING.md).

## Supported devices

**Inside the PC**

| Device | How | Status |
| --- | --- | --- |
| Anything [OpenRGB](https://openrgb.org) supports (graphics cards, boards and RAM of any brand, keyboards, mice, fans…) | OpenRGB SDK server on this PC, listed under *PC → Other PC hardware*; every controller becomes a device, found again by name when OpenRGB renumbers them | protocol tested against a simulator |
| MSI Mystic Light: onboard LED + JRAINBOW1 ARGB header | own driver, USB HID, 185-byte protocol | verified on MPG B650I EDGE WIFI (MS-7D73); other boards are not touched |
| ENE DRAM RGB, controller `AUDA0-E6K5-0101` (e.g. G.Skill Trident Z5 RGB) | own driver, SMBus via [PawnIO](https://pawnio.eu), AMD chipsets | verified; other controllers are detected and left alone |
| NVIDIA GPU temperature (for the temperature effect) | NVML | verified |

**In the room**

| Device | How | Status |
| --- | --- | --- |
| Nanoleaf Blocks / Shapes / Canvas / Lines / Elements, several controllers | official OpenAPI + UDP streaming, each controller its own device | Blocks verified; several controllers tested against a simulator |
| Secretlab MAGRGB (Smart Lighting Edition, by Nanoleaf) | Nanoleaf OpenAPI, streamed | tested against a simulator |
| Nanoleaf Pegboard Desk Dock, PC Screen Mirror Lightstrip (USB) | Nanoleaf's USB HID protocol | **experimental: not tested yet** |
| WLED (ESP8266 / ESP32 LED controllers) | UDP realtime (DNRGB), found via mDNS | tested against a simulator |
| Philips Hue (bridge) | local REST API, link-button pairing | tested against a simulator |
| Govee (lights with *LAN Control*) | official LAN API | tested against a simulator |
| Govee without LAN Control (e.g. AI Sync Box 2) | official cloud API with your API key: on/off and screen sync, or slow colours | tested against a simulator |
| LIFX | official LAN protocol | tested against a simulator |
| Yeelight (with *LAN Control*) | LAN JSON protocol, music mode | tested against a simulator |
| Philips WiZ | local UDP JSON protocol, no keys | tested against a simulator |
| Divoom Times Gate (its RGB lights; the screens stay the Divoom app's) | local HTTP API with the LocalToken from the Divoom app; the light behind the screens follows the effect. Found by a scan through Divoom's own service (it lists the Divoom devices on the same network, as the Divoom app does), or added by IP or Device ID | **experimental: written from community notes, not tested yet** |
| Wooting keyboards (One, Two, 60HE, 80HE, UwU; USB) | Wooting's HID protocol (as in their open RGB SDK), found on USB; effects run across the columns | verified on a 60HE v2; the others follow the SDK |
| Elgato Key Light / Key Light Air / Mini, Ring Light (white), Light Strip (colour) | local HTTP API (port 9123), found by mDNS, no keys; Key Lights start in the White mode | **tested against a simulator only** |
| Tuya / Smart Life colour lights (Lidl, Gosund, Nous, Teckin…) | local protocol 3.3 / 3.4 / 3.5, keys from your own Tuya cloud project | **experimental: not tested yet** |
| AiDot Wi-Fi bulbs (e.g. Linkind / "Matter Smart Light Bulb") | local LAN protocol, keys fetched once | verified with RGBTW bulbs |

"Tested against a simulator" means the protocol was checked with `tools/sim/fake_devices.py`, not yet on the
real device. Reports from owners are very welcome.

The app only writes to hardware it has positively identified. It never writes to the memory controller's
flash or changes SMBus addresses, and it restores each device's own effect when it exits.

## Install

Requirements: Windows 10/11 x64, [WebView2 Runtime](https://developer.microsoft.com/microsoft-edge/webview2/)
(built into Windows 11), and [PawnIO](https://pawnio.eu) for memory lighting.

1. Download `haku-control-setup.exe` from the [latest release](https://github.com/hxkune/haku-control/releases/latest) and run it. Windows asks for administrator rights:
   the app talks to the motherboard and memory, so it starts at sign-in through an elevated task.
2. The app appears in the tray and in the Start menu. On the first start, a short guide shows what was found and
   scans the network for lights.

The setup also updates an existing install (settings are kept) and can be removed from *Settings → Apps*.
`haku-control-setup.exe /extract <folder>` only unpacks the files. Without the installer, `scripts\install.ps1`
does the same from a build folder.

Settings, device keys and the log live in `%APPDATA%\haku-control`; removing the app asks whether to delete
them too. Without the installer, `scripts\uninstall.ps1` removes it (add `-Purge` to delete the settings).

### Room lights

- **WLED, Hue, Govee, LIFX, Yeelight, WiZ, Elgato:** *Devices → Scan network*, then *Add*. Govee and Yeelight need
  *LAN Control* switched on in their apps. Hue asks you to press the bridge's link button after adding.
- **OpenRGB (graphics cards, other boards, RAM, coolers):** install OpenRGB, tick *Start at login*, *Start minimized*
  and *Start server* in its general settings, then *PC → Other PC hardware* lists its controllers (*Add* / *Add all*).
  Hardware haku control drives itself (the MSI board, ENE memory) is not offered there. If OpenRGB still drives
  the same MSI board or memory as haku control's own drivers, set `[devices] msi=0` / `ene=0`.
- **Diagnostics** (*Settings → Files*) list the PC's hardware (board, BIOS, memory sticks, graphics cards and their
  maker, SMBus controller, USB devices of RGB makers, what OpenRGB sees): send it to ask for support of a device.

- **Nanoleaf:** *Devices → Sign-ins and pairing → Connect Nanoleaf*, then hold the controller's power button for
  5–7 s. Every controller becomes its own device (own map, colours and switch); *Add a controller* on the Nanoleaf
  page pairs the next one. **Secretlab MAGRGB:** first turn the strip's API on in Nanoleaf Desktop (select the strip →
  *Enable API*), press *Connect* here, then *Connect to API* in Nanoleaf Desktop within 30 s.
- **Govee without LAN Control (AI Sync Box 2...):** in the Govee Home app, *Profile → Settings → Apply for API Key*
  (it comes by e-mail); paste it under *Devices → Sign-ins and pairing → Govee (cloud)*, then *Scan*. A sync box keeps
  its own screen sync and haku switches it on and off with the PC; other devices can take colours from haku, slowly
  (every command goes through Govee's cloud). The key is saved on this PC and sent only to Govee.
- **Nanoleaf Pegboard / Screen Mirror Lightstrip (USB):** *Devices → Scan* finds them on USB; quit Nanoleaf Desktop
  first, or both apps drive the lights at once.
- **AiDot bulbs:** *Devices → Sign-ins and pairing → Sign in* with your AiDot app account, once. The password goes
  only to AiDot (RSA-encrypted, like their app does it) and is not stored; only each bulb's local key is saved.
  After that, everything runs locally. (`scripts\aidot-setup.ps1` does the same from PowerShell 7.)
- **Lights on the PC's own Wi-Fi (optional):** *Settings → Windows hotspot* keeps the Windows Mobile Hotspot
  running, so lights can join a network that exists whenever the PC is on.

### Phone

*Settings → Phone → Control from your phone* serves the same interface on your home network (port 8723).
Scan the QR code it shows with the phone camera (or open the address and type the PIN), then add the page to
the home screen.
Only private network addresses are served (and Tailscale's, for control from outside through your own tailnet);
Windows asks once whether to allow the app on the network. Siri Shortcuts or scripts can send commands too:

```
POST http://<pc>:8723/api/cmd   header X-Haku-Token: <token from pairing>
{"cmd":"power"}   {"cmd":"effect","id":"lava"}   {"cmd":"brightness","v":40}
```

### Describe a mood

Type something like *"sunset on the beach"* or *"cosy evening with a book"* above the effects, and a language
model picks the colours, the effect and its speed; *Apply* sets them on all lights. It runs on your PC through
[Ollama](https://ollama.com) (free, offline, nothing is sent anywhere). *Settings → Colours from a description →
Download and set up* installs Ollama (its installer from ollama.com, checked for Ollama's signature) and the model
(`gemma3:4b`, 3.3 GB). The model is in memory only while it picks colours; *Ollama only when needed* also keeps
Ollama itself out of Windows startup: haku control starts it for a request and closes it half a minute later.
Any chat model Ollama has works; `[mood] model=` picks one. Works from the phone page too.

## Build

Needs Visual Studio 2019 or newer (or Build Tools) with *Desktop development with C++* and a Windows 10/11 SDK.

```bat
build.cmd
```

Output goes to `bin\`. `build.cmd dev` makes a test build in `bin-dev\`: it keeps its settings in
`%APPDATA%\haku-control-dev`, runs without admin rights and never touches the motherboard or memory, so it
can run next to the installed app. Stop it with `scripts\stop.ps1 -Dev`. `python tools/sim/fake_devices.py` starts
simulated WLED / OpenRGB / Govee / LIFX / Yeelight / Hue / Elgato devices on this PC for it to talk to, and
`python tools/sim/fake_ollama.py` / `fake_nanoleaf.py` / `fake_aidot.py` / `fake_govee_cloud.py` stand-ins for Ollama, a Nanoleaf controller, the AiDot cloud and the Govee cloud.

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
4. ~~Phone control from the local network.~~ ~~Installer.~~ ~~Update check.~~ Signed builds (SignPath, see SIGNING.md).

## Code signing policy

Free code signing provided by [SignPath.io](https://signpath.io), certificate by [SignPath Foundation](https://signpath.org).
Only release builds made by GitHub Actions from this repository are signed, each one approved by hand; see [SIGNING.md](SIGNING.md).

- Committers and reviewers: [hxkune](https://github.com/hxkune)
- Approvers: [hxkune](https://github.com/hxkune)

## Privacy policy

haku control collects no personal data and sends no usage statistics. It connects to other systems only for these
purposes:

- **Updates:** a minute after start and then every 6 hours it asks the GitHub API for the latest release of this
  repository (no data about you or your PC is sent). A newer release's installer is downloaded from GitHub, checked
  (its SHA-256 as GitHub lists it) and installed by itself, after a notice from the tray; not while a full-screen
  game or a presentation runs.
- **Supported versions:** at start and once a day it reads `policy/policy.txt` from this repository (nothing about
  you or your PC is sent). It is a list, signed with the author's key, of versions the author has stopped supporting:
  a version on it lets go of the lights, shows the author's message with a download link and closes. Nothing is
  published there now. An unsigned or altered list is ignored, and without internet access the last list checked
  applies. Versions before 0.3.12 do not read it.
- **Ollama setup:** *Download and set up* downloads Ollama's installer from ollama.com and the model from Ollama's
  library, only when you press it.
- **Your lights:** it talks to the lights you added, on your local network.
- **Sign-ins you start:** AiDot or Tuya receive the sign-in data you enter, once, to hand out your lights' local
  keys. Nothing is sent to them otherwise.
- **Govee (cloud):** only if you enter a Govee API key: the key and the commands for the Govee devices you added
  (on / off, colour, brightness, screen sync) go to Govee's cloud API, as those devices cannot be reached locally.
- **Describe a mood:** the text goes to Ollama on your own PC, if you use it.
- **Phone control:** off by default; when on, it serves the settings page on your local network only.
- **Diagnostics:** *Save* under *Settings → Files* writes a text file to your Downloads folder (log, settings without
  keys or passwords, devices, network adapters). It is not sent anywhere; you decide whether to share it.

## License

Copyright (C) 2026 haku ([hxkune](https://github.com/hxkune)).

haku control is free software: you can redistribute it and/or modify it under the terms of the
[GNU General Public License, version 3](LICENSE) (GPL-3.0-only). In short: you may use, study, change and share it,
but anything you distribute that is based on it must be released under the same license, with its full source code,
and must keep the copyright notices. It comes without any warranty.

Versions up to and including 0.2.0 were published under the MIT license; everything after that is GPL-3.0.

The **haku** name and logo (`art/`, `ui/haku.svg`, `ui/mark.svg`, `ui/icon-256.png`, `res/*.ico`) are not covered by
the license. Forks must use their own name and logo, so nobody mistakes them for the official app.
Official builds come only from [github.com/hxkune/haku-control/releases](https://github.com/hxkune/haku-control/releases).
Third-party components are listed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

Not affiliated with MSI, G.Skill, ENE, Nanoleaf, AiDot or NVIDIA; their names are used only to describe compatibility.
