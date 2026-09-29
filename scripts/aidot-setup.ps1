# SPDX-License-Identifier: GPL-3.0-only
# Fetches the local-control keys of your AiDot bulbs (one cloud login) into %APPDATA%\haku-control\aidot.json.
# Run in PowerShell 7:  pwsh -File "C:\Program Files\haku-control\aidot-setup.ps1"
# Needed again only after the bulbs were reset / re-added in the AiDot app; haku control picks the new keys up by itself.
# Uses the (unofficial) AiDot app API, like the python-aidot project. Your password is sent only to AiDot, RSA-encrypted,
# and is not stored; only the per-bulb local keys are saved, readable by your Windows user only.
$ErrorActionPreference = 'Stop'
if ($PSVersionTable.PSVersion.Major -lt 7) { Write-Host 'PowerShell 7 is required: run it with pwsh' -ForegroundColor Red; return }
$APP_ID = '1383974540041977857'
$PUBLIC_KEY = @"
-----BEGIN PUBLIC KEY-----
MIGfMA0GCSqGSIb3DQEBAQUAA4GNADCBiQKBgQCtQAnPCi8ksPnS1Du6z96PsKfN
p2Gp/f/bHwlrAdplbX3p7/TnGpnbJGkLq8uRxf6cw+vOthTsZjkPCF7CatRvRnTj
c9fcy7yE0oXa5TloYyXD6GkxgftBbN/movkJJGQCc7gFavuYoAdTRBOyQoXBtm0m
kXMSjXOldI/290b9BQIDAQAB
-----END PUBLIC KEY-----
"@
$COUNTRIES = @{ FR=@('France','eu'); RU=@('Russia','eu'); UA=@('Ukraine','eu'); DE=@('Germany','eu'); IT=@('Italy','eu'); ES=@('Spain','eu'); BE=@('Belgium','eu'); CH=@('Switzerland','eu'); GB=@('United Kingdom','eu'); KZ=@('Kazakhstan','jp'); US=@('United States','us') }
$cc = (Read-Host "Account country (FR, DE, GB, US...)").Trim().ToUpper()
if (-not $COUNTRIES[$cc]) { Write-Host "Unknown country: $cc" -ForegroundColor Red; return }
$country, $region = $COUNTRIES[$cc]
$email = (Read-Host "AiDot email").Trim()
$secure = Read-Host "AiDot password (hidden)" -AsSecureString
$rsa = [Security.Cryptography.RSA]::Create(); $rsa.ImportFromPem($PUBLIC_KEY)
$encPass = [Convert]::ToBase64String($rsa.Encrypt([Text.Encoding]::UTF8.GetBytes([Net.NetworkCredential]::new('', $secure).Password), [Security.Cryptography.RSAEncryptionPadding]::Pkcs1))
$base = "https://prod-$region-api.arnoo.com/v35"
$term = ([guid]::NewGuid().ToString('N'))
$body = @{ countryKey="region:$country"; username=$email; password=$encPass; terminalId=$term; webVersion='0.5.0'; area='Europe/Paris'; UTC='UTC+1' } | ConvertTo-Json -Compress
$login = Invoke-RestMethod -Method Post -Uri "$base/users/loginWithFreeVerification" -Body $body -ContentType 'application/json' -Headers @{ Appid=$APP_ID; Terminal='app' }
$h = @{ Appid=$APP_ID; Terminal='app'; Token=$login.accessToken }
$devices = @(); foreach ($house in (Invoke-RestMethod -Uri "$base/houses" -Headers $h)) { if ($house.isOwner -ne $false) { $devices += Invoke-RestMethod -Uri "$base/devices?houseId=$($house.id)" -Headers $h } }
$lights = foreach ($d in $devices) { if ($d.type -eq 'light' -and $d.aesKey -and $d.aesKey[0]) { [ordered]@{ id="$($d.id)"; name="$($d.name)"; mac="$($d.mac)"; model="$($d.modelId)"; aesKey="$($d.aesKey[0])"; password="$($d.password)"; simpleVersion="$($d.simpleVersion)" } } }
$dir = Join-Path $env:APPDATA 'haku-control'; New-Item -ItemType Directory -Force $dir | Out-Null
$out = Join-Path $dir 'aidot.json'
[ordered]@{ userId="$($login.id)"; lights=@($lights) } | ConvertTo-Json -Depth 4 | Set-Content $out -Encoding UTF8
icacls $out /inheritance:r /grant:r "$($env:USERNAME):(R,W)" | Out-Null
$login = $null; $h = $null; $secure = $null
"Done, bulbs: $(@($lights).Count) -> $out"; $lights | ForEach-Object { "  $($_.name)  $($_.mac)" }
