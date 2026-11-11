# SPDX-License-Identifier: GPL-3.0-only
# The built installer, checked before it is published: it unpacks (setup /extract, no admin rights, nothing installed)
# into a new folder under %TEMP%, every file comes out byte for byte as scripts\pack.ps1 packed it, nothing more or
# less, and the program in it carries res\version.h's version (and -Version's, when given: the release tag).
#   powershell -NoProfile -File tools\sim\setup-check.ps1 [-Version 0.4.63]
# Exits 1 with the reasons when any check fails; the unpacked folder is kept then, for a look.
param([string]$Setup, [string]$Version)
$ErrorActionPreference = 'Stop'
$root = Split-Path (Split-Path $PSScriptRoot)
if (-not $Setup) { $Setup = Join-Path $root 'dist\haku-control-setup.exe' }
$bad = @()
if (-not (Test-Path $Setup)) { "FAIL no installer at $Setup"; exit 1 }

$want = (Select-String -Path (Join-Path $root 'res\version.h') -Pattern '#define HAKU_VER_STR\s+"([^"]+)"').Matches[0].Groups[1].Value
if ($Version -and $Version.TrimStart('v') -ne $want) { $bad += "the tag says $Version, res\version.h says $want" }

$to = Join-Path $env:TEMP ("haku-setup-check-" + [guid]::NewGuid().ToString('N').Substring(0, 8))
$p = Start-Process -FilePath $Setup -ArgumentList "/extract `"$to`"" -Wait -PassThru
if ($p.ExitCode -ne 0) { $bad += "setup /extract exited with $($p.ExitCode)" }
else {
    $files = & (Join-Path $root 'scripts\pack.ps1') -List
    $got = @(Get-ChildItem $to -Recurse -File | ForEach-Object { $_.FullName.Substring($to.Length + 1) })
    foreach ($name in $files.Keys) {
        $f = Join-Path $to $name
        if (-not (Test-Path $f)) { $bad += "missing: $name"; continue }
        if ((Get-FileHash $f).Hash -ne (Get-FileHash $files[$name]).Hash) { $bad += "differs from its source: $name" }
    }
    foreach ($name in $got) { if (-not $files.Contains($name)) { $bad += "not packed by pack.ps1: $name" } }
    $exe = Join-Path $to 'haku-control.exe'
    if (Test-Path $exe) {
        $v = (Get-Item $exe).VersionInfo.ProductVersion
        if ($v -ne $want) { $bad += "haku-control.exe is $v, res\version.h says $want" }
    }
    $sv = (Get-Item $Setup).VersionInfo.ProductVersion
    if ($sv -ne $want) { $bad += "the installer is $sv, res\version.h says $want" }
    if (-not $bad) { "ok   setup /extract: $($files.Count) files, each as packed, version $want" }
}
if ($bad) { $bad | ForEach-Object { "FAIL $_" }; "unpacked in $to"; exit 1 }
Remove-Item $to -Recurse -Force
"setup-check: ok"
