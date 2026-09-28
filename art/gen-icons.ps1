# Renders the haku control mark (same paths as ui/mark.svg) into .ico files with WPF.
# Windows PowerShell 5.1:  powershell -File gen-icons.ps1 <outdir>
#   app.ico        white mark on a black chamfered square (exe)
#   tray-dark.ico  white mark, transparent (tray, title bar, taskbar button on a dark taskbar)
#   tray-light.ico black mark, transparent (tray + taskbar button on a light taskbar)
# The mark: a tribal capital H inside an orbit that opens to the right like a C (haku control).
param([string]$OutDir)
Add-Type -AssemblyName PresentationCore, WindowsBase
$M = [Windows.Media.Geometry]

# the "C": a crescent = outer ellipse minus an inner ellipse shifted right, tilted like an orbit
$rot = New-Object Windows.Media.RotateTransform(-16, 128, 128)
$outer = $M::Parse('M 4,138 A 122,82 0 1 0 248,138 A 122,82 0 1 0 4,138 Z').Clone(); $outer.Transform = $rot
$inner = $M::Parse('M 30,133 A 120,70 0 1 0 270,133 A 120,70 0 1 0 30,133 Z').Clone(); $inner.Transform = $rot
$orbit = [Windows.Media.Geometry]::Combine($outer, $inner, 'Exclude', $null)
# the "H": two blade stems with spikes and a rising crossbar that pierces the C
$glyph = $M::Parse('F1 M 80,6 C 94,40 104,68 106,96 L 106,176 C 104,202 96,226 82,252 C 84,224 82,200 80,176 L 80,102 C 80,72 80,40 80,6 Z ' +
    'M 196,20 C 188,46 182,70 182,94 L 182,168 C 182,194 190,218 204,240 C 180,222 162,200 160,176 L 160,96 C 160,70 174,44 196,20 Z ' +
    'M 104,132 C 128,120 150,112 172,106 L 236,86 C 212,102 190,116 168,128 C 146,140 124,146 104,152 Z')

function MarkGeometry([double]$bold) {
    $cut = New-Object Windows.Media.Pen([Windows.Media.Brushes]::Black, (14 + $bold * 1.5))
    $cut.LineJoin = 'Round'
    $knock = [Windows.Media.Geometry]::Combine($glyph, $glyph.GetWidenedPathGeometry($cut), 'Union', $null)
    $ring = [Windows.Media.Geometry]::Combine($orbit, $knock, 'Exclude', $null)
    $all = [Windows.Media.Geometry]::Combine($ring, $glyph, 'Union', $null)
    if ($bold -gt 0) {
        $p = New-Object Windows.Media.Pen([Windows.Media.Brushes]::Black, $bold); $p.LineJoin = 'Round'
        $all = [Windows.Media.Geometry]::Combine($all, $all.GetWidenedPathGeometry($p), 'Union', $null)
    }
    return $all
}

# PNG bytes of the mark at `size` px; badge = on a black rounded square
function RenderPng([int]$size, [string]$variant) {
    $bold = if ($size -le 16) { 16 } elseif ($size -le 24) { 11 } elseif ($size -le 32) { 8 } elseif ($size -le 48) { 4 } else { 0 }
    $dv = New-Object Windows.Media.DrawingVisual
    $dc = $dv.RenderOpen()
    $k = $size / 256.0
    if ($variant -eq 'badge') {
        # chamfered square, like the UI's cut corners
        $c = $size * 0.16; $s = $size
        $badge = [Windows.Media.Geometry]::Parse("M $c,0 L $s,0 L $s,$($s - $c) L $($s - $c),$s L 0,$s L 0,$c Z")
        $dc.DrawGeometry((New-Object Windows.Media.SolidColorBrush([Windows.Media.Color]::FromRgb(10, 10, 10))), $null, $badge)
        $inner = 0.80; $off = $size * (1 - $inner) / 2
        $dc.PushTransform((New-Object Windows.Media.TranslateTransform($off, $off)))
        $dc.PushTransform((New-Object Windows.Media.ScaleTransform(($k * $inner), ($k * $inner))))
        $brush = [Windows.Media.Brushes]::White
    } else {
        $dc.PushTransform((New-Object Windows.Media.ScaleTransform($k, $k)))
        $dc.PushTransform((New-Object Windows.Media.TranslateTransform(0, 0)))
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
WriteIco (Join-Path $OutDir 'app.ico') 'badge' @(16, 20, 24, 32, 40, 48, 64, 256)
WriteIco (Join-Path $OutDir 'tray-dark.ico') 'dark' @(16, 20, 24, 32, 40, 48, 64, 256)
WriteIco (Join-Path $OutDir 'tray-light.ico') 'light' @(16, 20, 24, 32, 40, 48, 64, 256)
# previews
foreach ($v in 'badge', 'dark') { [IO.File]::WriteAllBytes((Join-Path $OutDir "preview-$v-256.png"), (RenderPng 256 $v)) }
[IO.File]::WriteAllBytes((Join-Path $OutDir 'preview-dark-16.png'), (RenderPng 16 'dark'))
[IO.File]::WriteAllBytes((Join-Path $OutDir 'preview-dark-32.png'), (RenderPng 32 'dark'))
