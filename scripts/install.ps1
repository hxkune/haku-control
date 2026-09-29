# SPDX-License-Identifier: GPL-3.0-only
# Installs haku control into C:\Program Files\haku-control and registers an elevated logon task.
# Run elevated after build.cmd:  pwsh -File scripts\install.ps1
# Re-running updates the program; settings in %APPDATA%\haku-control are kept.
param([string]$User = "$env:USERDOMAIN\$env:USERNAME")
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot
$bin  = Join-Path $root 'bin'
$dest = Join-Path $env:ProgramFiles 'haku-control'
$data = Join-Path $env:APPDATA 'haku-control'
$task = 'haku-control'

if (-not ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'Run this script as administrator.' }
if (-not (Test-Path "$bin\haku-control.exe")) { throw 'bin\haku-control.exe not found - run build.cmd first.' }

# stop running instances cleanly (the app restores the hardware effects on exit)
& (Join-Path $PSScriptRoot 'stop.ps1') | Out-Null
foreach ($n in 'haku-control', 'rgbfx') {
    $p = Get-Process $n -ErrorAction SilentlyContinue
    if ($p) { $p | Wait-Process -Timeout 10 -ErrorAction SilentlyContinue }
    Get-Process $n -ErrorAction SilentlyContinue | Stop-Process -Force   # last resort
}

# ---- one-time migration from the old "rgbfx" install
New-Item -ItemType Directory -Force $data | Out-Null
$old = Join-Path $env:ProgramFiles 'rgbfx'
$oldData = Join-Path $env:APPDATA 'rgbfx'
if ((Test-Path "$old\rgbfx.ini") -and -not (Test-Path "$data\settings.ini")) {
    $ini = Get-Content "$old\rgbfx.ini" -Raw
    # rgbfx kept the Windows hotspot on by default; haku control does not
    if ($ini -notmatch '(?m)^\[hotspot\]') { $ini = $ini.TrimEnd() + "`r`n`r`n[hotspot]`r`nauto=1`r`n" }
    Set-Content "$data\settings.ini" $ini -NoNewline -Encoding utf8NoBOM
    'migrated rgbfx.ini -> settings.ini'
}
foreach ($f in 'aidot.json', 'nanoleaf.json') {
    if ((Test-Path "$oldData\$f") -and -not (Test-Path "$data\$f")) { Copy-Item "$oldData\$f" $data; "migrated $f" }
}
if ((Test-Path "$old\msi_backup.bin") -and -not (Test-Path "$data\msi_backup.bin")) { Copy-Item "$old\msi_backup.bin" $data }
if (Get-ScheduledTask -TaskName 'rgbfx' -ErrorAction SilentlyContinue) {
    Unregister-ScheduledTask -TaskName 'rgbfx' -Confirm:$false; 'removed old task rgbfx'
}
if (Test-Path "$old\rgbfx.exe") { Remove-Item $old -Recurse -Force; "removed $old" }

# ---- program files
New-Item -ItemType Directory -Force $dest | Out-Null
Copy-Item "$bin\haku-control.exe", "$bin\haku-control-hotspot.exe", "$bin\SmbusPIIX4.bin" $dest -Force
if (Test-Path "$dest\ui") { Remove-Item "$dest\ui" -Recurse -Force }
Copy-Item "$bin\ui" "$dest\ui" -Recurse -Force
Copy-Item (Join-Path $PSScriptRoot 'stop.ps1'), (Join-Path $PSScriptRoot 'aidot-setup.ps1') $dest -Force

# ---- start at logon, elevated (hardware access needs admin rights)
$action    = New-ScheduledTaskAction -Execute "$dest\haku-control.exe" -WorkingDirectory $dest
$trigger   = New-ScheduledTaskTrigger -AtLogOn -User $User
$trigger.Delay = 'PT5S'   # let USB devices settle after logon
$principal = New-ScheduledTaskPrincipal -UserId $User -LogonType Interactive -RunLevel Highest
$settings  = New-ScheduledTaskSettingsSet -ExecutionTimeLimit ([TimeSpan]::Zero) -AllowStartIfOnBatteries `
                -DontStopIfGoingOnBatteries -MultipleInstances IgnoreNew -StartWhenAvailable
Register-ScheduledTask -TaskName $task -Action $action -Trigger $trigger -Principal $principal `
    -Settings $settings -Description 'haku control - PC and room lighting' -Force | Out-Null

# phone page and network scan answers: local network (and Tailscale) only
Remove-NetFirewallRule -DisplayName 'haku control' -ErrorAction SilentlyContinue
New-NetFirewallRule -DisplayName 'haku control' -Direction Inbound -Action Allow -Profile Any `
    -Program "$dest\haku-control.exe" -RemoteAddress LocalSubnet, 100.64.0.0/10 | Out-Null

if (-not (Test-Path "$env:ProgramFiles\PawnIO\PawnIOLib.dll")) {
    Write-Warning 'PawnIO is not installed: memory (SMBus) lighting stays off. Get it from https://pawnio.eu'
}
Start-ScheduledTask -TaskName $task
Start-Sleep -Seconds 4
Get-ChildItem $dest | Select-Object Name, Length | Format-Table -AutoSize
Get-Content "$data\haku-control.log" -ErrorAction SilentlyContinue
