# SPDX-License-Identifier: GPL-3.0-only
# Packs the built program (bin\) into obj\payload.bin for haku-control-setup.exe (see src\setup.c for the format).
param([string]$Bin = (Join-Path (Split-Path $PSScriptRoot) 'bin'), [string]$Out = (Join-Path (Split-Path $PSScriptRoot) 'obj\payload.bin'))
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot
$files = [ordered]@{}
foreach ($f in 'haku-control.exe', 'haku-control-hotspot.exe', 'haku-control-open.exe', 'SmbusPIIX4.bin') { $files[$f] = Join-Path $Bin $f }
Get-ChildItem (Join-Path $Bin 'ui') -Recurse -File | ForEach-Object { $files['ui\' + $_.FullName.Substring((Join-Path $Bin 'ui').Length + 1)] = $_.FullName }
$files['stop.ps1'] = Join-Path $root 'scripts\stop.ps1'
$files['aidot-setup.ps1'] = Join-Path $root 'scripts\aidot-setup.ps1'
$files['LICENSE.txt'] = Join-Path $root 'LICENSE'
$files['THIRD_PARTY_NOTICES.md'] = Join-Path $root 'THIRD_PARTY_NOTICES.md'
$files['SmbusPIIX4.COPYING.txt'] = Join-Path $root 'third_party\pawnio\COPYING'

$ms = New-Object IO.MemoryStream
$w = New-Object IO.BinaryWriter($ms)
$w.Write([Text.Encoding]::ASCII.GetBytes('HAKUPKG1'))
foreach ($name in $files.Keys) {
    $src = $files[$name]
    if (-not (Test-Path $src)) { throw "missing $src" }
    $n = [Text.Encoding]::UTF8.GetBytes($name)
    $data = [IO.File]::ReadAllBytes($src)
    $w.Write([uint16]$n.Length); $w.Write($n); $w.Write([uint32]$data.Length); $w.Write($data)
}
$w.Flush()
New-Item -ItemType Directory -Force (Split-Path $Out) | Out-Null
[IO.File]::WriteAllBytes($Out, $ms.ToArray())
"payload: $($files.Count) files, $([math]::Round($ms.Length / 1KB)) KB"
