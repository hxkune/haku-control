# Makes the key pair that signs policy/policy.txt (the list of blocked versions). PowerShell 7:
#   pwsh tools/policy/new-key.ps1
# The private key goes to %USERPROFILE%\.haku-policy\policy-key.pem, never into the repo: keep a copy somewhere safe.
# Without it no new policy can be signed for the versions that carry its public key. Prints the public key
# (X and Y, 64 bytes as hex) for POLICY_KEY in src/update.c. Refuses to overwrite a key that exists.
$dir = Join-Path $env:USERPROFILE '.haku-policy'
$file = Join-Path $dir 'policy-key.pem'
if (Test-Path $file) { throw "$file exists; move it away first if you really want a new key" }
New-Item -ItemType Directory -Force $dir | Out-Null
$k = [System.Security.Cryptography.ECDsa]::Create([System.Security.Cryptography.ECCurve+NamedCurves]::nistP256)
Set-Content -Path $file -Value $k.ExportPkcs8PrivateKeyPem() -NoNewline
$p = $k.ExportParameters($false)
"private key: $file"
"POLICY_KEY: " + (($p.Q.X + $p.Q.Y | ForEach-Object { $_.ToString('x2') }) -join '')
