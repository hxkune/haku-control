# Hardware check: OpenRGB, Home, Help

What the software tests do not prove. Run by hand on a PC with the devices, OpenRGB installed and its SDK server on.
Nothing here is run by haku on its own. Write down the device, the OpenRGB version and what you saw.

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

## Not to do as part of this check

- No calibration and no load tests.
- No commands to fans, pumps or screens beyond what the user sets by hand.
- No closing of other programs and no changing of services.
