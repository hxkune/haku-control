# SPDX-License-Identifier: GPL-3.0-only
# Stops a running haku control cleanly (works without admin rights).
# Also stops the old "rgbfx" build if it is still running. -Dev stops only the test build (build.cmd dev).
param([switch]$Dev)
Add-Type -Namespace W -Name U -MemberDefinition @"
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern System.IntPtr FindWindow(string c, string n);
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern uint RegisterWindowMessage(string s);
[DllImport("user32.dll")] public static extern bool PostMessage(System.IntPtr h, uint m, System.IntPtr w, System.IntPtr l);
"@ -ErrorAction SilentlyContinue
$targets = [ordered]@{}   # window class -> quit message
if ($Dev) { $targets['haku-control-dev'] = 'haku_control_quit' }
else { $targets['haku-control'] = 'haku_control_quit'; $targets['rgbfx'] = 'rgbfx_quit' }
$sent = $false
foreach ($cls in $targets.Keys) {
    $h = [W.U]::FindWindow($cls, $cls)
    if ($h -ne [IntPtr]::Zero) {
        [void][W.U]::PostMessage($h, [W.U]::RegisterWindowMessage($targets[$cls]), [IntPtr]::Zero, [IntPtr]::Zero)
        $sent = $true
    }
}
if ($sent) { 'stop signal sent' } else { 'haku control is not running' }
