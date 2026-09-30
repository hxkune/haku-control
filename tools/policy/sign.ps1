# Writes policy/policy.txt, the signed list of blocked versions, PowerShell 7. Every version of haku control from
# 0.3.12 on reads it from GitHub (at start and once a day): a version older than -Min, or one named in -Blocked,
# lets go of the lights, shows the message with a download link and quits. Commit and push the file to publish it.
#   pwsh tools/policy/sign.ps1 -Min 0.4.0 -MessageEn "..." -MessageRu "..." [-Blocked 0.4.1,0.4.2] [-Url https://...]
#   pwsh tools/policy/sign.ps1 -Min 0.0.0      (blocks nothing: lifts an earlier block)
# The file: line 1 is the ECDSA P-256 / SHA-256 signature (base64, r||s) of everything after that line.
param(
    [Parameter(Mandatory)][string]$Min,
    [string[]]$Blocked = @(),
    [string]$MessageEn = 'This version of haku control is no longer supported. Please install the new version.',
    [string]$MessageRu = 'Эта версия haku control больше не поддерживается. Установите новую версию.',
    [string]$MessageFr = 'Cette version de haku control n''est plus prise en charge. Installez la nouvelle version.',
    [string]$Url = 'https://github.com/hxkune/haku-control/releases/latest',
    [string]$Key = (Join-Path $env:USERPROFILE '.haku-policy\policy-key.pem')
)
$ver = '^\d+(\.\d+){0,3}$'
if ($Min -notmatch $ver) { throw "-Min is not a version: $Min" }
foreach ($b in $Blocked) { if ($b -notmatch $ver) { throw "-Blocked has something that is not a version: $b" } }
if ($Url -notmatch '^https://') { throw '-Url must be https://' }
$policy = [ordered]@{ min = $Min; blocked = @($Blocked); url = $Url; msg_en = $MessageEn; msg_ru = $MessageRu; msg_fr = $MessageFr; signed = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ') }
$json = $policy | ConvertTo-Json -Compress
$bytes = [Text.Encoding]::UTF8.GetBytes($json)
$k = [System.Security.Cryptography.ECDsa]::Create()
$k.ImportFromPem((Get-Content -Raw $Key))
$sig = $k.SignData($bytes, [System.Security.Cryptography.HashAlgorithmName]::SHA256)   # IEEE P1363: r||s
$out = Join-Path $PSScriptRoot '..\..\policy\policy.txt'
New-Item -ItemType Directory -Force (Split-Path $out) | Out-Null
[IO.File]::WriteAllBytes($out, [Text.Encoding]::UTF8.GetBytes([Convert]::ToBase64String($sig) + "`n") + $bytes)
"wrote $out"
$json
