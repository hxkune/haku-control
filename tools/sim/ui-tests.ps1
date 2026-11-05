# The UI tests (ui\mock.test.js) in headless Chrome against the mock core (ui\mock.js): no app, no devices.
#   powershell -File tools\sim\ui-tests.ps1 [-Only "TRYX"]      exits with 1 when a test fails or the run never ends
# Virtual time lets the tests' waits pass at once, so the whole run takes seconds.
param([string]$Only = '', [int]$Port = 8766)
$ErrorActionPreference = 'Stop'
$ui = Join-Path $PSScriptRoot '..\..\ui' | Resolve-Path
$chrome = @("$env:ProgramFiles\Google\Chrome\Application\chrome.exe", "${env:ProgramFiles(x86)}\Google\Chrome\Application\chrome.exe",
            "${env:ProgramFiles(x86)}\Microsoft\Edge\Application\msedge.exe") | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $chrome) { Write-Output 'Chrome or Edge not found'; exit 1 }
$profileDir = Join-Path ([IO.Path]::GetTempPath()) ('haku-ui-tests-' + [guid]::NewGuid())
$srv = Start-Process python -ArgumentList '-m', 'http.server', $Port, '--bind', '127.0.0.1' -WorkingDirectory $ui -PassThru -WindowStyle Hidden
try {
  Start-Sleep -Seconds 2
  $ErrorActionPreference = 'Continue'   # Chrome talks on stderr; Windows PowerShell would stop at the first line
  $url = "http://127.0.0.1:$Port/index.html?mock&test" + $(if ($Only) { '#' + [uri]::EscapeDataString($Only) })
  $dom = & $chrome --headless=new --disable-gpu --no-first-run --user-data-dir="$profileDir" --window-size=1280,900 `
           --virtual-time-budget=1800000 --dump-dom $url 2>$null | Out-String
} finally {
  Stop-Process -Id $srv.Id -Force -ErrorAction SilentlyContinue
  Remove-Item $profileDir -Recurse -Force -ErrorAction SilentlyContinue
}
# the first result follows <pre ...> on its line, the rest start lines
$log = [regex]::Matches($dom, '(?m)(?:^|(?<=<pre[^>]*>))(ok   |FAIL )[^<\r\n]*') | ForEach-Object { [Net.WebUtility]::HtmlDecode($_.Value) }
$log | ForEach-Object { Write-Output $_ }
$bad = @($log | Where-Object { $_ -like 'FAIL *' })
if (-not $log) { Write-Output 'the tests never finished (no result box in the page)'; exit 1 }
if ($bad.Count) { Write-Output "$($bad.Count) of $(@($log).Count) UI tests failed"; exit 1 }
Write-Output "all $(@($log).Count) UI tests passed"
