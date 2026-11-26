# One release: res\version.h, a "Release X.Y.Z" commit, the tag in this repo and in pro\ (the pro tag is what builds and
# publishes the installer), then waits for that run and checks the release has its files.
#   powershell -File tools\release.ps1 [-Version 0.4.87] [-Check]
# The ui\whatsnew.js entry for the version is written by hand first (en, ru, fr); the script only checks it is on top.
# -Check: every check, then stops before anything is changed.
param([string]$Version = '', [switch]$Check)
$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '..')
Set-Location $root
function Fail([string]$m) { Write-Output "release: $m"; exit 1 }
function Invoke-Git { & git @args; if ($LASTEXITCODE) { Fail "git $($args -join ' ') failed" } }

$verFile = 'res\version.h'
$h = [IO.File]::ReadAllText((Join-Path $root $verFile))
if ($h -notmatch 'HAKU_VER_STR\s+"(\d+)\.(\d+)\.(\d+)"') { Fail "no version in $verFile" }
$cur = [version]"$($Matches[1]).$($Matches[2]).$($Matches[3])"
if (-not $Version) { $Version = "$($cur.Major).$($cur.Minor).$($cur.Build + 1)" }
if ($Version -notmatch '^\d+\.\d+\.\d+$') { Fail "version '$Version' is not X.Y.Z" }
$v = [version]$Version
if ($v -le $cur) { Fail "$Version is not after $cur" }
$tag = "v$Version"

# where things stand: the release branch, nothing but the version and whatsnew changed, the tag nowhere yet, pro\ there and clean
if ((git rev-parse --abbrev-ref HEAD) -ne 'main') { Fail 'not on main' }
$dirty = @(git status --porcelain | Where-Object { $_ -notmatch '^.. (res/version\.h|ui/whatsnew\.js)$' })
if ($dirty.Count) { Fail ("other changes first:`n" + ($dirty -join "`n")) }
if (git tag -l $tag) { Fail "$tag is already here" }
if (git ls-remote --tags origin $tag) { Fail "$tag is already on origin" }
if (-not (Test-Path 'pro\.git')) { Fail 'pro\ is missing: the release is built there' }
if (git -C pro status --porcelain) { Fail 'pro\ has changes not committed' }
if (git -C pro tag -l $tag) { Fail "$tag is already in pro\" }
if (-not (Get-Command gh -ErrorAction SilentlyContinue)) { Fail 'gh is not installed' }

# the version's "what's new" is the first entry, in each language
$wn = [IO.File]::ReadAllText((Join-Path $root 'ui\whatsnew.js'))
$i = $wn.IndexOf('const WHATSNEW = [')
if ($i -lt 0) { Fail 'ui\whatsnew.js: no WHATSNEW list' }
$m = [regex]::Match($wn.Substring($i), "\{\s*v:\s*'([^']+)'([\s\S]*?)(?=\{\s*v:\s*'|\];)")
if (-not $m.Success -or $m.Groups[1].Value -ne $Version) { Fail "ui\whatsnew.js: the first entry is not $Version; write it first (en, ru, fr)" }
foreach ($l in 'en', 'ru', 'fr') { if ($m.Groups[2].Value -notmatch "\b${l}:\s*\[\s*'") { Fail "ui\whatsnew.js: $Version has no '$l' text" } }

Write-Output "release: $cur -> $Version"
if ($Check) { Write-Output 'release: checks passed, nothing changed (-Check)'; exit 0 }

$n = "$($v.Major),$($v.Minor),$($v.Build),0"
$h = $h -replace 'HAKU_VER_NUM\s+[\d,]+', "HAKU_VER_NUM   $n" -replace 'HAKU_VER_STR\s+"[^"]*"', "HAKU_VER_STR   `"$Version`"" -replace 'HAKU_VER_WSTR\s+L"[^"]*"', "HAKU_VER_WSTR  L`"$Version`""
[IO.File]::WriteAllText((Join-Path $root $verFile), $h)

Invoke-Git add -- res/version.h ui/whatsnew.js
Invoke-Git -c user.name=haku -c user.email=316277268+hxkune@users.noreply.github.com commit -q -m "Release $Version" -m 'Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>'
Invoke-Git tag $tag
Invoke-Git push -q origin main $tag
Invoke-Git -C pro tag $tag
Invoke-Git -C pro push -q origin $tag
Write-Output "release: $tag pushed here and in pro\; waiting for the build"

# the pro run for the tag: it can take a minute to show, and may sit queued for minutes
$run = $null
for ($t = 0; $t -lt 20 -and -not $run; $t++) {
  Start-Sleep 15
  $run = gh run list -R hxkune/haku-control-pro --workflow release.yml --branch $tag -L 1 --json databaseId -q '.[0].databaseId'
}
if (-not $run) { Fail "no pro run for $tag after 5 min; if it never starts: gh workflow run release.yml -R hxkune/haku-control-pro --ref $tag -f tag=$tag" }
gh run watch $run -R hxkune/haku-control-pro --exit-status --interval 30 | Out-Null
if ($LASTEXITCODE) { Fail "the pro run failed: gh run view $run -R hxkune/haku-control-pro --log-failed" }
$assets = (gh release view $tag -R hxkune/haku-control --json assets -q '.assets[].name') -join ', '
if ($LASTEXITCODE -or $assets -notmatch 'haku-control-setup\.exe') { Fail "$tag built, but the release has no installer: $assets" }
Write-Output "release: $tag published ($assets)"
