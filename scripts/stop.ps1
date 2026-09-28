# Stops a running haku control cleanly (works without admin rights).
# Also stops the old "rgbfx" build if it is still running.
Add-Type -Namespace W -Name U -MemberDefinition @"
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern System.IntPtr FindWindow(string c, string n);
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern uint RegisterWindowMessage(string s);
[DllImport("user32.dll")] public static extern bool PostMessage(System.IntPtr h, uint m, System.IntPtr w, System.IntPtr l);
"@
$sent = $false
foreach ($a in @(@('haku-control', 'haku_control_quit'), @('rgbfx', 'rgbfx_quit'))) {
    $h = [W.U]::FindWindow($a[0], $a[0])
    if ($h -ne [IntPtr]::Zero) {
        [void][W.U]::PostMessage($h, [W.U]::RegisterWindowMessage($a[1]), [IntPtr]::Zero, [IntPtr]::Zero)
        $sent = $true
    }
}
if ($sent) { 'stop signal sent' } else { 'haku control is not running' }
