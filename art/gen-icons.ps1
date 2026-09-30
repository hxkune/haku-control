# Renders the haku control mark (art/mark.json, made by art/mark.py; the same shapes as ui/mark.svg) into the
# .ico files and the 256 px PNG with WPF.
# Windows PowerShell 5.1:  powershell -File art\gen-icons.ps1 <outdir>
#   app.ico        white mark on a black chamfered square (exe, installer)
#   tray-dark.ico  white mark, transparent (tray, title bar, taskbar button on a dark taskbar)
#   tray-light.ico black mark, transparent (tray + taskbar button on a light taskbar)
#   icon-256.png   the app.ico picture (phone home screen, ui/manifest.webmanifest)
# The orbit passes in front of the h below its long axis and behind it above; where one crosses the other, the one
# behind gets a gap. Up to 32 px the mark leaves out the inner curl and is drawn bolder.
param([string]$OutDir)
Add-Type -AssemblyName PresentationCore, WindowsBase
$M = [Windows.Media.Geometry]
$J = Get-Content -Raw (Join-Path $PSScriptRoot 'mark.json') | ConvertFrom-Json

function Round-Pen([double]$w) { $p = New-Object Windows.Media.Pen([Windows.Media.Brushes]::Black, $w); $p.LineJoin = 'Round'; $p.StartLineCap = 'Round'; $p.EndLineCap = 'Round'; return $p }
function Union($a, $b) { return $M::Combine($a, $b, 'Union', $null) }

# the two halves of the plane on either side of the orbit's long axis
function Half([bool]$front) {
    $y = if ($front) { $J.cy - 1 } else { $J.cy - 500 }
    $r = New-Object Windows.Media.RectangleGeometry((New-Object Windows.Rect(-300, $y, 900, 500)))
    $r.Transform = New-Object Windows.Media.RotateTransform($J.tilt, $J.cx, $J.cy)
    return $r
}

function MarkGeometry([double]$bold, [bool]$small) {
    $h = $M::Parse('F1 ' + $J.h)
    $o = $M::Parse('F1 ' + $(if ($small) { $J.orbit_small } else { $J.orbit }))
    $gap = $J.gap * (1 + $bold / 12)
    $front = $M::Combine($o, (Half $true), 'Intersect', $null)
    $back = $M::Combine($o, (Half $false), 'Intersect', $null)
    # the letter gets a gap where the front of the orbit crosses it, the back of the orbit where the letter does
    $letter = $M::Combine($h, (Union $front $front.GetWidenedPathGeometry((Round-Pen (2 * $gap)))), 'Exclude', $null)
    $behind = $M::Combine($back, (Union $h $h.GetWidenedPathGeometry((Round-Pen (2 * $gap)))), 'Exclude', $null)
    $all = Union (Union $behind $letter) $front
    if ($bold -gt 0) { $all = Union $all $all.GetWidenedPathGeometry((Round-Pen $bold)) }
    return $all
}

# PNG bytes of the mark at `size` px; badge = on a black chamfered square
function RenderPng([int]$size, [string]$variant) {
    $small = $size -le 32
    $bold = if ($size -le 16) { 14 } elseif ($size -le 20) { 11 } elseif ($size -le 24) { 9 } elseif ($size -le 32) { 6 } elseif ($size -le 48) { 3 } else { 0 }
    $dv = New-Object Windows.Media.DrawingVisual
    $dc = $dv.RenderOpen()
    $k = $size / 256.0
    if ($variant -eq 'badge') {
        # chamfered square, like the UI's cut corners
        $c = $size * 0.16; $s = $size
        $badge = $M::Parse("M $c,0 L $s,0 L $s,$($s - $c) L $($s - $c),$s L 0,$s L 0,$c Z")
        $dc.DrawGeometry((New-Object Windows.Media.SolidColorBrush([Windows.Media.Color]::FromRgb(10, 10, 10))), $null, $badge)
        $inner = 0.86; $off = $size * (1 - $inner) / 2
        $dc.PushTransform((New-Object Windows.Media.TranslateTransform($off, $off)))
        $dc.PushTransform((New-Object Windows.Media.ScaleTransform(($k * $inner), ($k * $inner))))
        $brush = [Windows.Media.Brushes]::White
    } else {
        $dc.PushTransform((New-Object Windows.Media.TranslateTransform(0, 0)))
        $dc.PushTransform((New-Object Windows.Media.ScaleTransform($k, $k)))
        $brush = if ($variant -eq 'light') { [Windows.Media.Brushes]::Black } else { [Windows.Media.Brushes]::White }
    }
    $dc.DrawGeometry($brush, $null, (MarkGeometry $bold $small))
    $dc.Pop(); $dc.Pop()
    $dc.Close()
    $bmp = New-Object Windows.Media.Imaging.RenderTargetBitmap($size, $size, 96, 96, [Windows.Media.PixelFormats]::Pbgra32)
    $bmp.Render($dv)
    $enc = New-Object Windows.Media.Imaging.PngBitmapEncoder
    $enc.Frames.Add([Windows.Media.Imaging.BitmapFrame]::Create($bmp))
    $ms = New-Object IO.MemoryStream; $enc.Save($ms)
    return , $ms.ToArray()
}

# ICO with PNG-compressed entries (supported since Windows Vista)
function WriteIco([string]$path, [string]$variant, [int[]]$sizes) {
    $imgs = foreach ($s in $sizes) { , (RenderPng $s $variant) }
    $fs = [IO.File]::Create($path); $w = New-Object IO.BinaryWriter($fs)
    $w.Write([uint16]0); $w.Write([uint16]1); $w.Write([uint16]$sizes.Count)
    $offset = 6 + 16 * $sizes.Count
    for ($i = 0; $i -lt $sizes.Count; $i++) {
        $s = $sizes[$i]; $b = if ($s -ge 256) { 0 } else { $s }
        $w.Write([byte]$b); $w.Write([byte]$b); $w.Write([byte]0); $w.Write([byte]0)
        $w.Write([uint16]1); $w.Write([uint16]32); $w.Write([uint32]$imgs[$i].Length); $w.Write([uint32]$offset)
        $offset += $imgs[$i].Length
    }
    foreach ($im in $imgs) { $w.Write($im) }
    $w.Close()
    "wrote $path"
}

New-Item -ItemType Directory -Force $OutDir | Out-Null
$sizes = @(16, 20, 24, 32, 40, 48, 64, 256)
WriteIco (Join-Path $OutDir 'app.ico') 'badge' $sizes
WriteIco (Join-Path $OutDir 'tray-dark.ico') 'dark' $sizes
WriteIco (Join-Path $OutDir 'tray-light.ico') 'light' $sizes
[IO.File]::WriteAllBytes((Join-Path $OutDir 'icon-256.png'), (RenderPng 256 'badge'))
# previews
foreach ($s in 16, 24, 32, 48) { [IO.File]::WriteAllBytes((Join-Path $OutDir "preview-dark-$s.png"), (RenderPng $s 'dark')) }
[IO.File]::WriteAllBytes((Join-Path $OutDir 'preview-badge-48.png'), (RenderPng 48 'badge'))
