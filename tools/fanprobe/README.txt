fanprobe: helps haku control learn your motherboard's fan chip

haku control is getting fan speed control. Boards differ, so we need to see what yours reports.
fanprobe only READS by default. It changes nothing and installs nothing.

You need
- Windows 10/11, x64
- PawnIO (free, signed driver). haku's PC tab offers "Install PawnIO", or get it from https://pawnio.eu
- Close other fan / sensor tools first (FanControl, HWiNFO, LibreHardwareMonitor, the board's own app).

Steps
1. Unzip the whole folder somewhere you can write, e.g. the Desktop. Keep LpcIO.bin next to fanprobe.exe.
2. Double-click fanprobe.exe and allow the admin prompt. It runs about 20 seconds.
   Windows may say "Windows protected your PC" (the exe is not code-signed): More info > Run anyway.
3. Send back fanprobe.txt (it appears next to fanprobe.exe), plus:
   - which headers have a fan or pump on them (CPU_FAN, PUMP_FAN, SYS_FAN1 ...), and
   - what speed your fans roughly run at, if you know.

What it records: board and BIOS name, CPU name, Windows build, the fan chip's id and registers,
fan speeds, temperatures, and fan-hub USB devices from cooling brands. Nothing else, and it sends nothing.

Optional, only if asked (MSI boards with the NCT6687D chip)
Open a command prompt as administrator in this folder and run:
    fanprobe set 2 40 10
That sets SYS_FAN1 (control 2) to 40% for 10 seconds, then puts the board's own setting back.
Controls 2..7 are SYS_FAN1..6. The CPU fan and the pump are never touched. Ctrl+C also puts it back.

LpcIO.bin is the unchanged module from PawnIO.Modules 0.2.11 (github.com/namazso/PawnIO.Modules).
fanprobe source: tools/fanprobe in github.com/hxkune/haku-control.
