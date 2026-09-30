# Renders the haku control mark (ui/mark.svg, the one source of the shape) into the .ico files and the 256 px PNG
# with WPF.
# Windows PowerShell 5.1:  powershell -File art\gen-icons.ps1 <outdir>
#   app.ico        white mark on a black chamfered square (exe, installer)
#   tray-dark.ico  white mark, transparent (tray, title bar, taskbar button on a dark taskbar)
#   tray-light.ico black mark, transparent (tray + taskbar button on a light taskbar)
#   icon-256.png   the app.ico picture (phone home screen, ui/manifest.webmanifest)
# Small sizes are drawn bolder (the outline widened), so the thin spikes and the orbit still show at 16-32 px.
param([string]$OutDir)
Add-Type -AssemblyName PresentationCore, WindowsBase
$M = [Windows.Media.Geometry]
$svg = Get-Content -Raw (Join-Path $PSScriptRoot '..\ui\mark.svg')
if ($svg -notmatch '<path[^>]*\sd="([^"]+)"') { throw 'no path in ui/mark.svg' }
$Mark = $M::Parse('F1 ' + $Matches[1])   # viewBox 0 0 256 256

function MarkGeometry([double]$bold) {
    if ($bold -le 0) { return $Mark }
    $p = New-Object Windows.Media.Pen([Windows.Media.Brushes]::Black, $bold); $p.LineJoin = 'Round'
    return $M::Combine($Mark, $Mark.GetWidenedPathGeometry($p), 'Union', $null)
}

# PNG bytes of the mark at `size` px; badge = on a black chamfered square
function RenderPng([int]$size, [string]$variant) {
    $bold = if ($size -le 16) { 9 } elseif ($size -le 20) { 7 } elseif ($size -le 24) { 6 } elseif ($size -le 32) { 4 } elseif ($size -le 48) { 2 } else { 0 }
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
    $dc.DrawGeometry($brush, $null, (MarkGeometry $bold))
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
