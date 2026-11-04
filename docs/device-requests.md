# Device requests: from a report to a supported device

How the bug reports are used to add hardware haku does not light yet. Nothing in this goes out on its own. The user sends every report and every probe result themselves.

## 1. The request

- Devices → **Lighting in this PC**: a USB device with nothing to light it ("no lighting found on it") has an **Ask for support** button.
- The button opens the usual **Report a problem** form, filled in:
  - the device's name and VID:PID;
  - a line asking what it is and what lights it now;
  - **Attach diagnostics** ticked.
- The last line, `[support usb VVVV:PPPP]`, is the same in every language. The site counts requests by it.
- The person reads the text, can change anything, and sends it, or doesn't.
- Every diagnostics file, sent with a request or not, carries the hardware list from `src/hwinfo.c`. For each USB device of a known maker:
  - `usb: VVVV:PPPP maker - product`, once per device;
  - `usb-if: VVVV:PPPP mi NN col NN usage PPPP:UUUU in N out N feature N`, once per HID interface.
  - The interface details come from the descriptor Windows keeps. Nothing is sent to or read from the device.

## 2. The summary

- **hakune.blog → admin → bug reports → requested devices** (`admin/devices.php` in haku-site) lists every VID:PID found in the reports.
- For each one it shows:
  - **asked**: installations that pressed Ask for support;
  - **installations**: installations whose diagnostics list the device;
  - links to the reports. ★ marks a report that asked.
- The most asked devices come first, then the most common.
- Only makers in `usb_maker()` show up in the hardware list. A device from a maker that isn't in that table is missing until the table gains the maker's VID.
- Devices haku already lights show up too: the list is of what people have, not only of what is missing.

## 3. Picking one

- Prefer a device that:
  - several installations asked for;
  - has a contact or an installation ID, so the reply reaches the person;
  - has a protocol that is already documented, e.g. OpenRGB or another open project's notes, or a hub with an `usb-if` interface of a vendor usage page (FFxx).
- Check that the device is not covered another way first: OpenRGB, the maker's own SDK (Razer, SteelSeries, Logitech), or a driver haku already has with a missing PID.
- A missing PID in an existing driver is the cheap case: add the PID, the simulator case, and go to step 5.

## 4. The reply and the read-only check

- Answer from the report's page (the reply box; the app shows it once, with a tray notice). Write in their language.
- Ask only for what the diagnostics don't already hold:
  - what lights it now (the maker's program and its version);
  - how many fans or strips hang off it;
  - a photo of the label if the name is unclear.
- If a read-only probe is needed, it must:
  - only open the device and read its descriptors, or input reports the device sends on its own;
  - never write an output or feature report;
  - never update firmware or install a driver;
  - be run by the person, with its output pasted into a new report.
- The person answers with a new report. Their installation ID ties it to the first one.

## 5. Driver, simulator, tests

- **Driver:** `src/<maker>.c` with an `ext_driver` (`src/devices.h`), added to the list in `src/devices.c`.
  - It follows the take-over and give-back rules.
  - Add the maker's program to `src/conflicts.c`, so the app says which program is in the way and doesn't fight it.
- **Simulator:** `tools/sim/`, built from what the device was seen to send and accept. It is not built from guesses about bytes nobody has seen.
- **Tests:**
  - the C tests and the UI mock (`ui/mock.js` and `ui/mock.test.js`) for the new device;
  - a lost link, a bad reply, and a restore after sleep.
- **hwRoute** in `ui/app.js`: the device's row now says haku lights it, not "no lighting found on it".

## 6. The beta with that person

- A build goes to the person who asked. They are told what to try and what to watch for, e.g. the effects, sleep and wake, unplugging, and the maker's program running alongside.
- They report back with a report and the diagnostics.
- Fix and repeat until it holds on their hardware.

## 7. Supported

- A device is called supported only after step 6 passes on real hardware, and the person said so.
- Passing in the simulator alone is not support. Until the beta passes, the device is listed as being worked on, never as working.
- Then:
  - reply to the person's reports and mark them done (the app tells them);
  - note the device and the tester in the release notes.
