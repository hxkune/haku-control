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

## Not to do as part of this check

- No calibration and no load tests.
- No commands to fans, pumps or screens beyond what the user sets by hand.
- No closing of other programs and no changing of services.
