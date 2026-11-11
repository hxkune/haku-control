# The UI tests (ui\mock.test.js, ui\mock.tray.test.js) in headless Chrome against the mock cores (ui\mock.js,
# ui\mock.tray.js): no app, no devices.
#   powershell -File tools\sim\ui-tests.ps1 [-Only "TRYX"] [-Minutes 20]   exits with 1 when a test fails or the run never ends
# Real time: the page is asked over the DevTools protocol for window.TEST = { done, failed, log } until done or -Minutes.
# (Virtual time ran on while WebCodecs, the video decoder and image decoding worked on real threads, and dumped the page
# mid-test on a slow PC.)
param([string]$Only = '', [int]$Port = 8766, [int]$Minutes = 20)
$ErrorActionPreference = 'Stop'
$ui = Join-Path $PSScriptRoot '..\..\ui' | Resolve-Path
$chrome = @("$env:ProgramFiles\Google\Chrome\Application\chrome.exe", "${env:ProgramFiles(x86)}\Google\Chrome\Application\chrome.exe",
            "${env:ProgramFiles(x86)}\Microsoft\Edge\Application\msedge.exe") | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $chrome) { Write-Output 'Chrome or Edge not found'; exit 1 }
Write-Output ("browser: " + (Get-Item $chrome).VersionInfo.ProductVersion)
$profileDir = Join-Path ([IO.Path]::GetTempPath()) ('haku-ui-tests-' + [guid]::NewGuid())
$errFile = $profileDir + '.log'
$none = [Threading.CancellationToken]::None

# one DevTools request on the page; $null when the page does not answer in time (busy in a long script, or gone)
function Cdp-Eval($ws, [string]$expr) {
  $script:cdpId++
  $msg = @{ id = $script:cdpId; method = 'Runtime.evaluate'; params = @{ expression = $expr; returnByValue = $true } } | ConvertTo-Json -Compress -Depth 5
  $b = [Text.Encoding]::UTF8.GetBytes($msg)
  if (-not $ws.SendAsync((New-Object 'ArraySegment[byte]' (, $b)), 'Text', $true, $none).Wait(10000)) { return $null }
  # a receive that timed out is still running (a socket takes one at a time): the next call waits on that one, with its
  # buffer and the part of the message read so far, rather than start a second receive (InvalidOperationException)
  while ($true) {
    do {
      if (-not $script:rx) { $script:rx = $ws.ReceiveAsync((New-Object 'ArraySegment[byte]' (, $script:rxBuf)), $none) }
      if (-not $script:rx.Wait(10000)) { return $null }
      $t = $script:rx; $script:rx = $null
      $script:rxMs.Write($script:rxBuf, 0, $t.Result.Count)
    } while (-not $t.Result.EndOfMessage)
    $r = [Text.Encoding]::UTF8.GetString($script:rxMs.ToArray()) | ConvertFrom-Json; $script:rxMs.SetLength(0)
    if ($r.id -eq $script:cdpId) { return $r.result.result.value }   # anything else is an event: skipped
  }
}

$srv = $null; $log = @(); $unfinished = @(); $conns = @()
# CPU time per process at the start: on a failure, who used the machine meanwhile (a slow runner, a scanner)
$cpu0 = @{}; Get-Process | ForEach-Object { $cpu0["$($_.Id)"] = $_.CPU }
$t0 = Get-Date
try {
  $srvLog = $profileDir + '.srv.log'
  $srv = Start-Process python -ArgumentList "`"$(Join-Path $PSScriptRoot 'ui-server.py')`"", $Port, "`"$ui`"" -WorkingDirectory $ui -PassThru -WindowStyle Hidden -RedirectStandardError $srvLog
  $up = $false
  for ($i = 0; $i -lt 100 -and -not $up; $i++) {
    try { $up = (Invoke-WebRequest "http://127.0.0.1:$Port/index.html" -UseBasicParsing -TimeoutSec 2).StatusCode -eq 200 } catch { Start-Sleep -Milliseconds 200 }
  }
  if (-not $up) { Write-Output "the test server did not answer on port $Port"; exit 1 }
  foreach ($page in 'index.html', 'tray.html') {
    $url = "http://127.0.0.1:$Port/$page" + '?mock&test' + $(if ($Only) { '#' + [uri]::EscapeDataString($Only) })
    $portFile = Join-Path $profileDir 'DevToolsActivePort'
    Remove-Item $portFile -Force -ErrorAction SilentlyContinue
    $br = Start-Process $chrome -PassThru -WindowStyle Hidden -RedirectStandardError $errFile -ArgumentList @(
      '--headless=new', '--disable-gpu', '--no-first-run', "--user-data-dir=`"$profileDir`"", '--window-size=1280,900',
      '--remote-debugging-port=0', '--enable-logging=stderr', '--v=0', $url)
    $ws = $null; $state = $null
    try {
      $dev = $null
      for ($i = 0; $i -lt 150 -and -not $dev; $i++) { Start-Sleep -Milliseconds 200; if (Test-Path $portFile) { $dev = (Get-Content $portFile)[0] } }
      if (-not $dev) { throw 'the browser did not open its DevTools port: ' + (@(Get-Content $errFile -ErrorAction SilentlyContinue | Where-Object { $_ -match ':ERROR:' }) -join ' | ') }
      $target = $null
      for ($i = 0; $i -lt 50 -and -not $target; $i++) {
        $list = Invoke-RestMethod "http://127.0.0.1:$dev/json/list" -UseBasicParsing   # Windows PowerShell: the array as one object
        $target = $list | ForEach-Object { $_ } | Where-Object { $_.type -eq 'page' -and $_.url -like "*$Port/$page*" } | Select-Object -First 1
        if (-not $target) { Start-Sleep -Milliseconds 200 }
      }
      if (-not $target) { throw "the browser did not open $page" }
      $ws = New-Object Net.WebSockets.ClientWebSocket
      $ws.ConnectAsync([uri]$target.webSocketDebuggerUrl, $none).Wait(10000) | Out-Null
      $script:cdpId = 0; $script:rx = $null; $script:rxBuf = New-Object byte[] 65536; $script:rxMs = New-Object IO.MemoryStream
      $end = (Get-Date).AddMinutes($Minutes)
      $q = 'JSON.stringify({ done: !!(window.TEST && TEST.done), log: window.TEST ? TEST.log : [], title: document.title, stage: typeof TXB !== "undefined" && TXB && TXB.stage || "" })'
      while ((Get-Date) -lt $end) {
        $v = Cdp-Eval $ws $q
        if ($v) { $state = $v | ConvertFrom-Json; if ($state.done) { break } }
        Start-Sleep -Seconds 1
      }
    } catch { Write-Output "  ${page}: $_" }
    finally {
      if ($ws) { $ws.Dispose() }
      # the connections to the test server open now, the browser still there (out of its 6 per server, or a server not reading)
      $conns += @(netstat -ano -p tcp | Select-String ":$Port\s" | Where-Object { $_.Line -notmatch 'TIME_WAIT' } | ForEach-Object { "${page}: " + ($_.Line -replace '\s+', ' ').Trim() })
      # only the browser started here, with its own child processes
      # a child already on its way out makes taskkill say "could not be terminated": with 'Stop' that ended the run unreported
      if (-not $br.HasExited) { try { & taskkill /T /F /PID $br.Id 2>&1 | Out-Null } catch {} }
      # and gone: on 1 core the killed browser still held its profile's lockfile when the next page's browser started,
      # which then quit ("Lock file can not be created! Error code: 32") and the page never opened
      $br.WaitForExit(30000) | Out-Null
      # and its children too: on 1 core taskkill /T left some (GPU, network, storage), and they held the profile's lockfile.
      # Only those with this run's own profile folder (a new GUID) in their command line
      for ($i = 0; $i -lt 50; $i++) {
        $left = @(Get-CimInstance Win32_Process | Where-Object { $_.CommandLine -and $_.CommandLine.Contains($profileDir) })
        if (-not $left) { break }
        $left | ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
        Start-Sleep -Milliseconds 200
      }
    }
    if ($state) { $log += @($state.log) }
    if (-not ($state -and $state.done)) {
      $unfinished += "$page (" + $(if ($state) { $state.title + $(if ($state.stage) { ', the TRYX making at ' + $state.stage }) } else { 'no answer' }) + ')'
      # what the page said on its console: an error that stopped the tests (not the mock's echo of each command)
      Get-Content $errFile -ErrorAction SilentlyContinue | Where-Object { $_ -match 'CONSOLE|Uncaught' -and $_ -notmatch '"-> ' } | Select-Object -Last 20 | ForEach-Object { Write-Output "  console: $_" }
    }
  }
} finally {
  if ($srv) { Stop-Process -Id $srv.Id -Force -ErrorAction SilentlyContinue }
  Start-Sleep -Milliseconds 300
  # the test server's own trouble (not its line per request answered 200 or 304), said when a test failed
  $srvAll = @(Get-Content $srvLog -ErrorAction SilentlyContinue)
  $srvErr = @($srvAll | Where-Object { $_ -notmatch '" (200|304) ' } | Select-Object -Last 30)
  # the requests since the last frame of the main page was asked for (which of its files reached the server)
  $last = -1; for ($i = 0; $i -lt $srvAll.Count; $i++) { if ($srvAll[$i] -match '"GET /index\.html\?mock HTTP') { $last = $i } }
  $srvTail = if ($last -ge 0) { @($srvAll[$last..($srvAll.Count - 1)] | Where-Object { $_ -match '"GET ' } | Select-Object -First 40) } else { @() }
  # requests begun and never answered, over the whole run
  $open = [ordered]@{}; foreach ($l in $srvAll) { if ($l -match 'begin (GET \S+)') { $open[$Matches[1]] = 1 + [int]$open[$Matches[1]] } elseif ($l -match '"(GET \S+) HTTP[^"]*" \d{3}' -and $open[$Matches[1]]) { $open[$Matches[1]]-- } }
  $hung = @($open.Keys | Where-Object { $open[$_] -gt 0 } | ForEach-Object { "$_ x$($open[$_])" })
  Remove-Item $profileDir -Recurse -Force -ErrorAction SilentlyContinue
  Remove-Item $errFile, $srvLog -Force -ErrorAction SilentlyContinue
}
$log | ForEach-Object { Write-Output $_ }
$bad = @($log | Where-Object { $_ -like 'FAIL *' })
if ($bad.Count -or $unfinished) {
  $srvErr | Where-Object { $_ -notmatch ' begin GET ' } | ForEach-Object { Write-Output "  server: $_" }; $srvTail | ForEach-Object { Write-Output "  asked: $_" }
  Write-Output "  never answered: $(if ($hung) { $hung -join ', ' } else { 'none' })"
  $conns | Select-Object -First 30 | ForEach-Object { Write-Output "  conn: $_" }
  Write-Output ("  cpu: {0} cores, {1:N0} s of the run; the most CPU seconds meanwhile:" -f [Environment]::ProcessorCount, ((Get-Date) - $t0).TotalSeconds)
  Get-Process | ForEach-Object { [pscustomobject]@{ n = $_.ProcessName; s = $_.CPU - [double]$cpu0["$($_.Id)"] } } | Group-Object n |
    ForEach-Object { [pscustomobject]@{ n = $_.Name; s = ($_.Group | Measure-Object s -Sum).Sum } } | Sort-Object s -Descending | Select-Object -First 8 |
    ForEach-Object { Write-Output ("  cpu: {0,8:N1} s  {1}" -f $_.s, $_.n) }
}
if ($unfinished) { Write-Output "the tests never finished in $Minutes min: $($unfinished -join ', ')"; exit 1 }
if (-not $log -and -not $Only) { Write-Output 'no results'; exit 1 }
if ($bad.Count) { Write-Output "$($bad.Count) of $(@($log).Count) UI tests failed"; exit 1 }
Write-Output "all $(@($log).Count) UI tests passed"
