# Hardware check: OpenRGB, Home, Help

What the software tests do not prove. Run by hand on a PC with the devices, OpenRGB installed and its SDK server on.
Nothing here is run by haku on its own. Write down the device, the OpenRGB version and what you saw.

**For every check, write down:**
- the device model and its firmware;
- the haku version (Settings → About) and the Windows build;
- what you did, step by step;
- what was expected (the line here) and what actually happened;
- the log lines around it (`haku-control.log`, after a crash also `haku-control.prev.log`).

Do not include device keys, passwords, PINs or licence keys. A check you could not run (no such device, no second monitor) stays **not checked**: say so. It is not "passed".

Status words used in README and in the app:
- **tested on hardware**: this list was run on that device and passed;
- **simulator only**: checked against simulated devices or a fake server, never on the real one;
- **experimental / beta**: may not work; the badge stays until the check here passes on real hardware. Mock or simulator results never remove it.

## OpenRGB: flicker and modes

1. **Direct controller** (most boards, many RAM sticks, keyboards). Add it, run Flow for 5 minutes.
   - Expected: no flashes, no short jumps to another colour or mode. Card: "Effects available".
   - OpenRGB window open alongside: its mode stays Direct.
2. **Static-only per-LED controller.** Card: "Experimental support". Flow runs slowly (a few frames a second), with no flashes between frames.
3. **Steady-colour controller.** Card: "Steady colour only".
   - Static red: one switch, then it holds.
   - Flow: the colour does not change on every frame. It changes only when a colour holds for a while.
4. **A board's two connectors added on their own.** Both light up, neither blinks when the other changes. Remove one: the other keeps going.
5. **Board added whole plus one of its connectors.** The connector card says the board lights it. No flicker from two writers.
6. **Two identical RAM sticks.** Each lights its own colour. Restart haku and OpenRGB: they keep their colours (not swapped).
7. **Mode fight.** With haku running, switch the device's mode in OpenRGB by hand.
   - haku either sets its mode again on reconnect, or the card says the mode did not switch.
   - It never shows "working" while dark.
8. **Lost link.** Stop the OpenRGB SDK server.
   - Cards go to "not reachable · retrying", and Home lists them under "Needs attention".
   - Start the server again: they come back within a minute (or at once with Retry now).
9. **Another lighting program** (Armoury Crate, iCUE, Mystic Light) on together with haku.
   - PC → Programs in the way lists it.
   - haku does not close it.
   - Home shows the held device with the Programs in the way button.

## Home

- No devices at all (a fresh settings folder): "No devices yet" with Find devices. Find devices opens Devices and scans.
- One device off its network: listed under Needs attention. Open goes to its card, Help to the topic in Settings.
- Fans on: the fans card shows the same rpm as the Fans tab, and a click opens Fans.

## Help: diagnostics file

- Settings → Help and diagnostics → Save: a file appears in Downloads, and nothing goes out.
  - Check the network with the firewall log, or Resource Monitor.
- Open to check opens the file in Notepad. Search it for your e-mail, PINs, keys and tokens: none should be there.

## Effects: the new ones, the modes, the ready-made looks

The simulator only proves the colours the engine computes, and that the card preview matches them. It does not prove how a device shows them.
Note each device's LED count, how it is laid out and the haku version.

1. **Long strip** (60+ LEDs), then **short** (8–20). Run Pendulum (Swing, then Bounce), Interference, Steps, Constellation, Meteor shower, and Breathe with a cycle of 8 s.
   - The light moves the same way as on the card. Speed does not depend on the LED count: Pendulum takes as long end to end on both strips.
   - Reverse on the device card turns the direction around.
   - Steps: the segments fill one after another. With Hard edges on, each segment fills at once, with no fade.
2. **Ring or fan** (a closed loop). Pendulum and Meteor shower do not jump where the loop closes. Interference shows two waves running opposite ways.
3. **Panel with a 2D layout.** Interference with Rings or Angle draws circles or a turning pattern.
   - Without a 2D layout, the settings say it needs one, and the strip mode runs instead.
4. **A single lamp** (one LED, a bulb).
   - Pendulum and Meteor shower: the lamp lights as the spot passes it and fades between passes, down to the Background level. With Background 0 it is dark between passes, and that is expected.
   - Constellation always holds a star on a single lamp, and Music shows the level as brightness.
   - A LAN bulb changes no faster than its own limit: no missed commands and no "not reachable" during the effect.
5. **Music.** Switch to Music, Level mode.
   - No playback device (or turned off in Windows): the state line says there is no sound device, and the lights stay dim.
   - Device on, nothing playing: "No sound", dim.
   - Music playing: the level follows it, and the peak falls back slowly. In Beat mode, a flash on each beat, and none between beats closer than the Gap.
   - Check that nothing is recorded or sent: no new files in the settings folder or Downloads, and no traffic in Resource Monitor.
6. **Temperature, sensor lost.** Run Temperature, then close what reads the sensor, or take a GPU that has none.
   - The lights go dim grey and pulse slowly. The state line says the sensor is lost and that this is only a colour.
   - When the reading is back, the colours follow it again.
7. **The eight ready-made looks** on a long and a short strip: Moonlight, Sunset, Deep ocean, Forest, Neon, Embers, Polar aurora and Monochrome.
   - None looks blinding at the brightness you use.
   - Change a setting: the look stops being lit, and the effect name shows.
   - "+" on a look makes a copy under Yours. Your own presets stay as they were.
8. **Settings kept.** Set Pendulum's spot width, switch to Fire and back: the width is still there. Restart haku: it is still there. Reset puts it back to the default.
9. **Several devices, synced.** A strip, a RAM stick and a bulb on Sync devices. They move together, and the bulb only as fast as it can.
   - A device that takes a steady colour only shows one colour, and it does not flicker.
10. **Off.** Lights off during any new effect: everything goes dark at once and stays dark. Lights on brings the effect back with a fade.
11. **Another lighting program** on together with haku while a new effect runs. haku does not take the device from it, and the card says the device is held.

## Fans: what keeps the beta badge on

The self-tests run the QUADRO, the board chip, Lian Li and iCUE LINK against simulated devices. They do not prove how a real device takes a write, or what it does once haku is gone.
Each part loses its beta badge only after this check passes on real hardware. Note the device, its firmware and the haku version.
Send back `haku-control.log` (Settings → Help and diagnostics → the log), plus `haku-control.prev.log` after a crash. The lines that matter start with `fans:`, `sio:`, `lianli:` and `link:`.

### Aqua Computer QUADRO

1. **Draft.** Change a curve point, then do not apply it.
   - The fans keep following the last applied curve, and the page says so.
   - Apply: "Applied · the QUADRO confirmed it". Cancel: the points go back.
2. **Curves.** One fan on a curve by GPU temperature, one by a QUADRO sensor. Load the GPU and warm the sensor: the speed follows each one's own curve, never below 20%.
3. **USB unplugged while the PC runs.**
   - Within about 10 s the page says the link is lost.
   - Plug it back: haku finds it again and takes the channels again.
   - The fans run on the QUADRO's own setting while it is away.
4. **Exit.** Close haku from the tray: every fan goes back to what aquasuite set (curve or speed) within a few seconds.
5. **Sleep.** Put the PC to sleep and wake it: the fans are on the QUADRO's own setting while asleep, and haku takes them again after the wake.
6. **End task.** End haku in Task Manager.
   - The fans stay at haku's last speed, because Windows gives haku no time to act.
   - Start haku, then close it from the tray: the fans go back to what aquasuite set, not to haku's last speed (fans-state.ini kept it).
7. **Crash and watchdog.** Nothing forces either in the release build; check the log whenever haku has crashed or a fan hung.
   - A crash logs `crash: exception …` (in haku-control.prev.log after the restart), and the fans are back on their own setting before the process ends.
   - A stalled control thread logs `fans: fan control of the QUADRO has not run for … s; its channels go back to its own setting` after about 15 s.

### Board fan chip (Nuvoton NCT67xx, through PawnIO)

1. **Readings.** The rpm on the page matches the BIOS and HWiNFO for each header, within a few percent. The log names the chip: `sio: NCT… at …, N fan controls`.
2. **Manual.** Set one header to a fixed speed: it holds for 10 minutes, and HWiNFO shows the same duty.
3. **Exit and sleep.** Close haku, or sleep and wake: the header goes back to its BIOS mode (curve or Smart Fan).
4. **Another monitor** (HWiNFO, FanControl) running alongside: readings go on, and no header jumps between two speeds.
5. A chip the log calls "not a known fan chip" or "not Nuvoton's vendor id" is left alone. Send that line.

### Lian Li UNI FAN wireless

1. **Speeds.** Set each group to 30%, 60% and 100%: the rpm follows, and every fan in the group goes together. Nothing below 30% is offered.
2. **Fans' own.** Back to "Fans' own": the transmitter closes and the fans run on their own setting.
3. **Haku closed or crashed.** Close haku, then end it in Task Manager. Write down what the fans do in each case (their last speed, their own setting, full speed).
4. **Receiver unplugged** while a speed is set: the set groups go to full speed, then run on their own.
5. **Lighting with board ARGB sync on.** Light a group from haku.
   - The board's sync goes off, and haku's colours show.
   - If it does not go off after 10 asks, the group row says so.
   - Remove the light: sync comes back on, or the row says it did not.
6. The first `lianli: asked …` lines in the log are the receiver's raw replies. Send them, as they show how your firmware answers.

### Corsair iCUE LINK

Readings (control off, the default):
- Compare the rpm and the coolant temperature with iCUE, both open in turn.
- With iCUE running, the page says iCUE holds the hub, and haku waits.

Control (Cooling → iCUE LINK → the switch). Close iCUE and OpenLinkHub first; keep HWiNFO or the page open on the coolant temperature.
1. **Taken.** Switch it on: within 10 s the card says "haku drives the hub" and the log has `link: control of the hub taken (software mode)`. Fans go to 50%, the pump to 100%. A chain with an LCD cap or more than 512 LEDs says its lights are not driven.
2. **OpenRGB.** With OpenRGB installed, `%APPDATA%\OpenRGB\OpenRGB.json` has `"Corsair iCUE Link System Hub": false` while this is on, and `true` again once it is off or haku is closed (it goes back to `false` when haku starts with control on).
3. **Speeds.** One fan fixed at 30%, then 80%; another on a GPU curve: the rpm follows. Set the pump to 30%: it stays at 50% (the slider stops there).
4. **Coolant.** Under load, if the coolant passes 57 °C: every fan and pump goes to 100% and the card says so, until every pump reads under 52 °C. Do not heat the loop on purpose for this.
5. **Lights.** Add the hub as a light device and pick an effect: each fan's ring and the pump cap follow it, in channel order.
6. **Given back.** Switch it off, close haku from the tray, and sleep and wake: each time the hub goes back to its own control (iCUE's saved setting) within a few seconds, and the log says `link: the hub has its own control back`.
7. **End task.** End haku in Task Manager: write down what the fans and pump do (their last speed, or the hub's own control after its timeout). Start haku and switch control off: the hub runs on its own.
8. **USB unplugged** while it is held: the card waits, and the hub runs on its own; plugged back, haku takes it again.
Send a diagnostics file (Settings → Help and diagnostics; it has the hub's device list and firmware), the log, and what the hub did at each step.

### Corsair LCD screen (iCUE LINK LCD, ELITE LCD, XC7 ELITE LCD)

Experimental: built from OpenLinkHub's description of the protocol, not yet run on a real screen. Close iCUE, OpenLinkHub and SignalRGB first.
1. **Found.** Find devices lists the screen, and its card has the builder.
2. **Picture.** Build a still with a text over it: the screen shows it within a few seconds, round and not stretched.
3. **Animation.** Build from a GIF or a short video: it moves at about the same speed as in the builder.
4. **Lights off.** Switch the lights off: the screen goes dark (the XC7 ELITE shows black, as it has no backlight). Turn them on again, then change the app's brightness: the screen follows it.
5. **Giving it back.** Close haku, put the PC to sleep and wake it, and end haku in Task Manager. In each case, write down what the screen shows (its own display, black, the last picture).
6. **Another program.** Start iCUE with haku open: the card says iCUE has the screen, and haku sends nothing until iCUE is closed.
7. Send the `corsairlcd:` lines from the log, as they show the screen's reports and how it answered.

## Exit, saving and update

The self-tests check the order and the results in code (`tools/sim/run-tests.cmd`: fans, link, config, setup). They do not show what a real device does when haku closes, or how Windows treats the installer on your PC.

1. **Quit with fans set.** Set a fixed speed on each fan part you have (QUADRO, board header, Lian Li), then Quit from the tray panel's ⋯.
   - Every fan goes back to its own setting within a few seconds.
   - The log has no `not confirmed` line. If it has one, write down which part.
2. **Quit while the settings cannot be written.** Make `%APPDATA%\haku-control\settings.ini` read-only, change the brightness, then Quit.
   - A dialog says the settings could not be saved and names the file. It offers Try again and Exit without saving.
   - Cancel keeps haku running with the change. Try again fails again while the file is read-only.
   - Clear read-only, then Try again: haku quits. Start it again: the new brightness is there.
   - Exit without saving: haku quits, the fans are given back, the old brightness is there on the next start.
3. **Sign out or restart with haku running** (the Windows session ends, which gives haku a few seconds only).
   - After signing in again, the log of the previous run ends with the fans and lights given back. If it has `session end: cooling given back not confirmed`, write down which part.
   - This path is best-effort: Windows may end haku before it is done.
4. **Update notice.** With an older version installed and a newer one out, wait for the daily check (or restart haku).
   - The tray says a new version is out. Nothing installs on its own.
   - Settings → About shows the new version and says haku closes and starts again. Press Update: haku closes, the installer runs, haku starts again with the same settings, profiles and devices.
   - A full-screen game or a presentation running: no tray notice until it ends.
5. **Update while haku cannot give the fans back** (e.g. the QUADRO busy in aquasuite). The installer says the fans may not have been given back and asks before going on. Cancel leaves the installed version as it was.
6. **Update with the folder in use.** Open a command prompt and `cd` into the program folder (it holds the folder), then run the setup.
   - It reports that a file is in use and that the installed version was not changed.
   - haku still starts from the Start menu.
7. **Interrupted update.** Run the setup and end it in Task Manager while it copies. Run the setup again.
   - The second run finishes the update normally (an unfinished copy is thrown away, a half-done swap is put back first).
   - Ended exactly between its two folder renames, haku does not start until the setup is run again: write down if you ever see that.
   - `%APPDATA%\haku-control` is not touched in any case.
8. **Not enough disk space.** On a disk with less than about 70 MB free, the setup says so before touching the installed version.
9. **Blocked version.** Not checked by hand: it needs the update server to mark a version as blocked. In code it is unchanged. It is checked at start and once a day; a blocked version lets the lights go, says why and quits.

## Unplug, replug and conflicts

These repeat the per-device steps above in one pass, for any device you have:
- Unplug the device's USB while an effect runs. Its card says it is lost within about 10 s, and Home lists it under Needs attention. Plug it back: it comes back without restarting haku.
- Start the device maker's own app (iCUE, aquasuite, L-Connect, Armoury Crate) with haku open. haku does not take the device from it, does not close it, and says what holds the device.

## Tray panel

Checked so far only in a browser against a fake core (`tray.html?mock`, `tools/sim/ui-tests.ps1`) and in `tools/sim/tray_test.c`, which covers which click does what. On a real PC:
1. **Open and close.**
   - A left or a right click on the icon opens the panel next to it.
   - A second click, Escape or a click elsewhere closes it. Clicking quickly many times never leaves two panels or a panel that reopens by itself.
   - A double click opens the window, with no panel left open.
   - The old menu shows only when the panel cannot be made (step 7).
2. **Keyboard.** Win+B, arrow to haku's icon, then Enter and also Shift+F10 (or the menu key): the panel opens with the focus in it. Escape closes it.
3. **Where it opens.** Move the taskbar to each side, then turn on auto-hide: the panel sits by the icon, inside the screen, never under the taskbar. Repeat at 100 %, 150 % and 200 % scaling and on a second monitor with another scale (there Windows may blur it slightly). With the icon in the overflow (^) area, the panel opens near where it was clicked, inside the screen.
4. **Controls.** Brightness drag: the lights follow and the panel stays open. Power, a profile, an effect, a preset, a device switch: each changes the lights and the window shows the same. Change the effect in the window: the open panel follows.
5. **Tab and Narrator.** Tab goes through every control with a visible ring; Narrator reads the slider value, the switches' on/off and the profile in use.
6. **Time and memory.** The log has `tray: open in N ms (first)` and `(again)` lines: write both down. Open and close it 30 times, then look at haku's memory and handles in Task Manager: they should not keep growing. After 30 s closed, the panel's WebView2 processes end. Write down how many `msedgewebview2.exe` processes haku has with the window closed, with it open, and with the panel open.
7. **Explorer restart.** With the panel open, end Explorer in Task Manager and start it again: the panel closes, the icon comes back, and both clicks open the panel again.
8. **Fallback.** Rename `ui\tray.html` in the install folder (put it back afterwards): a left or right click shows the old menu instead, and the log says why.
9. **Quit.** Quit from ⋯: the lights are handed back exactly as with Quit in the old menu, and a failed save asks as in "Exit, saving and update" step 2.

## Not to do as part of this check

- No calibration and no load tests.
- No commands to fans, pumps or screens beyond what the user sets by hand.
- No closing of other programs and no changing of services.
