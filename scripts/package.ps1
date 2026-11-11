# The release files, made the same way by both workflows (the open build and the Pro release):
#   dist\haku-control-setup.exe.sha256   haku checks updates against it when the GitHub API is out of reach
#   haku-control-<Version>.zip           the portable package
# Run from the repo root after build.cmd and scripts\pack.ps1. Under GitHub Actions the package name goes to PKG.
param([Parameter(Mandatory)][string]$Version)
$ErrorActionPreference = 'Stop'

$h = (Get-FileHash dist\haku-control-setup.exe -Algorithm SHA256).Hash.ToLower()
[IO.File]::WriteAllText("dist\haku-control-setup.exe.sha256", "$h  haku-control-setup.exe`n")

$pkg = "haku-control-$Version"
New-Item -ItemType Directory "$pkg\scripts" | Out-Null
Copy-Item bin "$pkg\bin" -Recurse
Copy-Item scripts\install.ps1, scripts\uninstall.ps1, scripts\stop.ps1, scripts\aidot-setup.ps1 "$pkg\scripts"
Copy-Item README.md, LICENSE, THIRD_PARTY_NOTICES.md $pkg
foreach ($m in 'SmbusPIIX4', 'LpcIO', 'AMDFamily17', 'IntelMSR') { Copy-Item third_party\pawnio\COPYING "$pkg\bin\$m.COPYING.txt" }
Compress-Archive -Path "$pkg\*" -DestinationPath "$pkg.zip"
if ($env:GITHUB_ENV) { "PKG=$pkg" >> $env:GITHUB_ENV }
