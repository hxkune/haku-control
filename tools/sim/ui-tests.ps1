# The UI tests (ui\mock.test.js, ui\mock.tray.test.js) in headless Chrome against the mock cores (ui\mock.js,
# ui\mock.tray.js): no app, no devices.
#   powershell -File tools\sim\ui-tests.ps1 [-Only "TRYX"]      exits with 1 when a test fails or the run never ends
# Virtual time lets the tests' waits pass at once, so the whole run takes seconds.
param([string]$Only = '', [int]$Port = 8766)
$ErrorActionPreference = 'Stop'
$ui = Join-Path $PSScriptRoot '..\..\ui' | Resolve-Path
$chrome = @("$env:ProgramFiles\Google\Chrome\Application\chrome.exe", "${env:ProgramFiles(x86)}\Google\Chrome\Application\chrome.exe",
            "${env:ProgramFiles(x86)}\Microsoft\Edge\Application\msedge.exe") | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $chrome) { Write-Output 'Chrome or Edge not found'; exit 1 }
$profileDir = Join-Path ([IO.Path]::GetTempPath()) ('haku-ui-tests-' + [guid]::NewGuid())
$errFile = $profileDir + '.log'
$srv = Start-Process python -ArgumentList '-m', 'http.server', $Port, '--bind', '127.0.0.1' -WorkingDirectory $ui -PassThru -WindowStyle Hidden
$log = @(); $unfinished = @()
try {
  Start-Sleep -Seconds 2
  $ErrorActionPreference = 'Continue'   # Chrome talks on stderr; Windows PowerShell would stop at the first line
  foreach ($page in 'index.html', 'tray.html') {
    $url = "http://127.0.0.1:$Port/$page" + '?mock&test' + $(if ($Only) { '#' + [uri]::EscapeDataString($Only) })
    $dom = & $chrome --headless=new --disable-gpu --no-first-run --user-data-dir="$profileDir" --window-size=1280,900 `
             --virtual-time-budget=1800000 --enable-logging=stderr --v=0 --dump-dom $url 2>$errFile | Out-String
    # the first result follows <pre ...> on its line, the rest start lines
    $got = @([regex]::Matches($dom, '(?m)(?:^|(?<=<pre[^>]*>))(ok   |FAIL )[^<\r\n]*') | ForEach-Object { [Net.WebUtility]::HtmlDecode($_.Value) })
    if (-not $got -and -not $Only) {   # with -Only a page may simply have no such test
      $unfinished += "$page (" + [Net.WebUtility]::HtmlDecode(([regex]::Match($dom, '<title>([^<]*)</title>')).Groups[1].Value) + ')'
      # what the page said on its console: an error that stopped the tests
      Get-Content $errFile -ErrorAction SilentlyContinue | Where-Object { $_ -match 'CONSOLE|Uncaught' } | Select-Object -Last 20 | ForEach-Object { Write-Output "  console: $_" }
    }
    $log += $got
  }
} finally {
  Stop-Process -Id $srv.Id -Force -ErrorAction SilentlyContinue
  Remove-Item $profileDir -Recurse -Force -ErrorAction SilentlyContinue
  Remove-Item $errFile -Force -ErrorAction SilentlyContinue
}
$log | ForEach-Object { Write-Output $_ }
$bad = @($log | Where-Object { $_ -like 'FAIL *' })
if ($unfinished -or -not $log) { Write-Output "the tests never finished (no result box in $($unfinished -join ', '))"; exit 1 }
if ($bad.Count) { Write-Output "$($bad.Count) of $(@($log).Count) UI tests failed"; exit 1 }
Write-Output "all $(@($log).Count) UI tests passed"
