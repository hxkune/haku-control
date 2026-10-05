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
- **Local only:** no account, no cloud, no telemetry. Devices are controlled over USB, SMBus and your LAN (a Pro
  subscription key is checked with the store, see the [privacy policy](#privacy-policy)).
- English, Russian and French UI; dark, grey and light themes.

## Free and Pro

haku control is free for the PC: the PC's own lighting, USB devices, Razer / SteelSeries / Logitech through their
apps, and every effect. This repository is that part, open source under the GPL-3.0.

**haku Pro** adds the lights on Wi-Fi and the network (Nanoleaf, Hue, WLED, Govee, LIFX, Yeelight, WiZ, Elgato,
Divoom, Tuya, AiDot) and phone control: $2 a month or $10 a year, with 14 days to try it in the app (no card, no
account). The installer from the [releases](https://github.com/hxkune/haku-control/releases/latest) is the full app:
without a key it runs as the free one once the trial is over, and the room lights simply pause. See
[hakune.blog/haku-control.html#pro](https://hakune.blog/haku-control.html#pro). The Pro code lives in a private
repository and is not under the GPL; versions up to 0.3.30 had all of it here, and they stay GPL-3.0.

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
| Razer keyboards, mice, mousepads, headsets, keypads, Chroma Link | through Razer Synapse: its Chroma SDK REST API on this PC | **tested against a simulator only** |
| SteelSeries keyboards (per key), mice, headsets, mousepads (by their number of zones) | through SteelSeries GG: its GameSense API on this PC | **tested against a simulator only** |
| Logitech keyboards (per key), mice, headsets, speakers (one colour) | through Logitech G HUB: the LED library G HUB installs (Logitech's LED SDK), checked for Logitech's signature | **tested against a stand-in library only** |

**In the room** (haku Pro)

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
| Divoom Times Gate (its RGB lights, back and sides; the screens stay the Divoom app's), Times Frame (its side light, through Channel/SetAmbientLight, effects picked by number; its screen: a Divoom dial per profile, or haku's own screen with the time, CPU, memory, GPU temperature, effect and profile, and off with the lights; the background image comes from this repository through Divoom's cloud) | local HTTP API with the LocalToken from the Divoom app; the light behind the screens follows the effect. Found by a scan through Divoom's own service (it lists the Divoom devices on the same network, as the Divoom app does), or added by IP or Device ID | **experimental: written from community notes, not tested yet** |
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
- **Razer, SteelSeries, Logitech:** with the maker's own app running (Razer Synapse, SteelSeries GG, Logitech G
  HUB), *Devices → Scan network* offers its devices: *Add* the ones you have. Their apps do not say which devices
  are plugged in, so each kind is offered (Razer: keyboard, mouse, mousepad, headset, keypad, Chroma Link;
  SteelSeries and Logitech: the keyboard and the other devices). The maker's app keeps running and gets the lighting
  back when haku control lets go. For Logitech, G HUB's *Allow games & applications to control my lighting* has to
  be on.
- **Lighting in this PC** (*PC*, at the top) lists what haku control finds in the PC (the board, the memory, the
  graphics cards, USB devices of lighting makers) and says for each which way it is lit: by haku control itself,
  through OpenRGB (with *Add* when OpenRGB found it but it is not added yet), OpenRGB still to set up, PawnIO
  missing for the memory, or no lighting known. Nothing is written to any device to find this out.
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

(haku Pro.) *Settings → Phone → Control from your phone* serves the same interface on your home network (port 8723).
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

### Backup and sharing

*Settings → Files → Backup* saves every setting and profile to one `.hakubackup` file, to keep or to move to another
PC; *Restore* puts them back. Device keys and tokens, passwords, e-mail addresses, PINs and the licence are left out
(the same rule as the diagnostics) and stay on each PC: restoring keeps this PC's own, and a device whose key is not
here is paired once again. A preset's *Share* gives a short code (`haku:…`) with its look only (effect, colours,
speed, brightness); *Import* on the presets tab adds it as a preset. The *Community* tab (next to *My presets*) lists presets
people published (popular or new, search, by effect): *Add* takes one, ♥ likes it, and *Publish* in a preset's
*Share* window sends one for review. The site's side is described in `docs/community-api.md`.

### Home Assistant (haku Pro)

*Settings → Home Assistant*: haku connects to the MQTT broker Home Assistant uses (usually its *Mosquitto broker*
add-on; a Home Assistant user's name and password) and appears by itself through MQTT discovery as a light (on / off,
brightness, the effects by name, an RGB colour as the Static effect) and a select with the profiles, for automations,
dashboards and voice. The state goes back to Home Assistant as it changes, in haku or there. Topics: `haku/<pc>/…`.
`python tools/sim/fake_mqtt.py --script` stands in for a broker with Home Assistant behind it.

### Screen (ambilight)

The *Screen* effect lights the room with what is on the screen. Strips run around its edges (from the bottom left,
clockwise, as a backlight strip is laid), flat devices such as a Nanoleaf wall or a keyboard show it as a mosaic, and
single lights take its average colour; each device's card can pick another part (the whole screen, the edges, left,
right, top, bottom). The effect's speed sets how fast the lights follow. The screen is copied with DXGI Desktop
Duplication and shrunk by the graphics card, so it costs next to nothing (about 0.1 % of an 8-core processor at
2560x1440); it runs only while the effect is on. With more than one screen, the effect's panel picks which. Video
protected against copying comes out dark, as in any screen capture.

## Build

Needs Visual Studio 2019 or newer (or Build Tools) with *Desktop development with C++* and a Windows 10/11 SDK.

```bat
build.cmd
```

Output goes to `bin\`. From this repository alone that is the free app (the PC's lighting, USB devices, the makers'
apps, every effect); with the private Pro repository checked out as `pro\` next to `src\`, the full one
(`set HAKU_FREE=1` builds the free one anyway). `build.cmd dev` makes a test build in `bin-dev\`: it keeps its settings in
`%APPDATA%\haku-control-dev`, runs without admin rights and never touches the motherboard or memory, so it
can run next to the installed app. Stop it with `scripts\stop.ps1 -Dev`. `python tools/sim/fake_devices.py` starts
simulated WLED / OpenRGB / Govee / LIFX / Yeelight / Hue / Elgato / Divoom devices, Razer Synapse and SteelSeries GG
on this PC for it to talk to (`tools/sim/fake_logiled.c` stands in for G HUB's LED library), and
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
- **haku Pro:** a subscription key is activated with Lemon Squeezy (the store that sells it) and checked there at
  each start and every 12 hours: the key, this installation's id and the name "haku control" are sent, nothing else.
  The trial start and the latest date seen are kept on the PC (settings and registry) and sent nowhere. Gift keys
  are checked on the PC.
- **Razer, SteelSeries, Logitech:** it talks to Razer Synapse, SteelSeries GG or Logitech G HUB on your PC, only
  for the devices you added; it sends them colours.
- **Sign-ins you start:** AiDot or Tuya receive the sign-in data you enter, once, to hand out your lights' local
  keys. Nothing is sent to them otherwise.
- **Govee (cloud):** only if you enter a Govee API key: the key and the commands for the Govee devices you added
  (on / off, colour, brightness, screen sync) go to Govee's cloud API, as those devices cannot be reached locally.
- **Describe a mood:** the text goes to Ollama on your own PC, if you use it.
- **Screen effect:** the screen is read on your PC to colour the lights; no picture is kept or sent anywhere.
- **Divoom:** a device scan, and a Divoom device added by its Device ID, ask Divoom's service which Divoom devices are
  on your network (it sees your internet address, as the Divoom app does); a Times Frame's list of dials comes from
  Divoom's cloud. The Times Frame's own screen background is fetched by the frame from this repository.
- **Home Assistant:** off by default; when on, the light's state (on / off, brightness, effect, colour) and the profile names go to the MQTT broker you entered, on your network, with the user name and password you gave.
- **Community presets:** the *Community* tab reads the list of published presets from hakune.blog. Adding, liking and
  publishing one send a random id made on first use (so each counts once per install; nothing about you or your PC)
  and, when publishing, the preset's look and the author name you typed. The site keeps hashes of these ids and IPs only.
- **Phone control:** off by default; when on, it serves the settings page on your local network only.
- **Diagnostics:** *Save* under *Settings → Files* writes a text file to your Downloads folder (log, settings without
  keys or passwords, devices, network adapters). It is not sent anywhere; you decide whether to share it.

## License

Copyright (C) 2026 haku ([hxkune](https://github.com/hxkune)).

The code in this repository is free software: you can redistribute it and/or modify it under the terms of the
[GNU General Public License, version 3](LICENSE) (GPL-3.0-only). In short: you may use, study, change and share it,
but anything you distribute that is based on it must be released under the same license, with its full source code,
and must keep the copyright notices. It comes without any warranty.

Versions up to and including 0.2.0 were published under the MIT license; everything after that is GPL-3.0.

The **haku** name and logo (`art/`, `ui/haku.svg`, `ui/mark.svg`, `ui/icon-256.png`, `res/*.ico`) are not covered by
the license. Forks must use their own name and logo, so nobody mistakes them for the official app.
Official builds come only from [github.com/hxkune/haku-control/releases](https://github.com/hxkune/haku-control/releases).
Third-party components are listed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

Not affiliated with MSI, G.Skill, ENE, Nanoleaf, AiDot or NVIDIA; their names are used only to describe compatibility.
