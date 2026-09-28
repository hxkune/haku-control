# Removes haku control: stops it, deletes the logon task and C:\Program Files\haku-control.
# Settings and device keys in %APPDATA%\haku-control are kept unless -Purge is given.
param([switch]$Purge)
$ErrorActionPreference = 'Stop'
& (Join-Path $PSScriptRoot 'stop.ps1') | Out-Null
Get-Process haku-control -ErrorAction SilentlyContinue | Wait-Process -Timeout 10 -ErrorAction SilentlyContinue
if (Get-ScheduledTask -TaskName 'haku-control' -ErrorAction SilentlyContinue) {
    Unregister-ScheduledTask -TaskName 'haku-control' -Confirm:$false
}
$dest = Join-Path $env:ProgramFiles 'haku-control'
if (Test-Path $dest) { Remove-Item $dest -Recurse -Force }
if ($Purge) {
    Remove-Item (Join-Path $env:APPDATA 'haku-control') -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item (Join-Path $env:LOCALAPPDATA 'haku-control') -Recurse -Force -ErrorAction SilentlyContinue
}
'haku control removed'
