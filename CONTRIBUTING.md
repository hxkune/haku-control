# Contributing

Thanks for helping! The most valuable contributions right now are **device support** and **testing on
hardware the author doesn't have**.

## Reporting a device

Open an issue with:

- the device (exact model), how it connects (USB, ARGB header, SMBus, Wi-Fi)
- for USB devices: the VID/PID (Device Manager → Details → Hardware Ids)
- `%APPDATA%\haku-control\haku-control.log` after a start

## Hardware safety rules

Code that talks to hardware must follow these rules. Pull requests that break them will not be merged.

1. **Identify before writing.** Only write to a device after it has been positively identified
   (USB VID/PID plus a verified protocol, or an SMBus controller's name/version string). Unknown means hands off.
2. **Never persist.** Never write to a device's flash or "save" register, and never remap SMBus addresses.
   Direct mode must be temporary.
3. **Restore on exit.** Snapshot the device's own mode before taking over, and give it back on exit, sleep and shutdown.
4. **Share the bus.** SMBus access goes through the `Global\Access_SMBUS.HTP.Method` mutex, like other RGB tools.
5. **Stay light.** No busy loops. Skip unchanged frames. Polling threads sleep.

## License of contributions

By sending a pull request you agree that your contribution is licensed under the GPL-3.0 (GPL-3.0-only),
like the rest of the project.

## Code style

- C core (`src/`): C11-ish, MSVC, no external dependencies. Keep a comment at the top of each file that
  explains what it talks to and how.
- UI (`ui/`): plain HTML/CSS/JS, no build step. Every visible text goes into `ui/i18n.js` in **both** English and Russian.
- Build with `build.cmd`; it must stay warning-free.
