# Turns the black-on-white haku logo into a white-on-transparent PNG (alpha = darkness), cropped to the artwork.
# Windows PowerShell 5.1 (WPF): powershell -File logo-alpha.ps1 <in.webp|png> <out.png>
param([string]$In, [string]$Out)
Add-Type -AssemblyName PresentationCore, WindowsBase
$dec = [Windows.Media.Imaging.BitmapDecoder]::Create([Uri]$In, 'None', 'OnLoad')
$src = New-Object Windows.Media.Imaging.FormatConvertedBitmap($dec.Frames[0], [Windows.Media.PixelFormats]::Bgra32, $null, 0)
$w = $src.PixelWidth; $h = $src.PixelHeight; $stride = $w * 4
$px = New-Object byte[] ($stride * $h)
$src.CopyPixels($px, $stride, 0)
$x0 = $w; $y0 = $h; $x1 = 0; $y1 = 0
for ($y = 0; $y -lt $h; $y++) {
    for ($x = 0; $x -lt $w; $x++) {
        $i = $y * $stride + $x * 4
        $lum = (0.114 * $px[$i] + 0.587 * $px[$i + 1] + 0.299 * $px[$i + 2])
        $a = [int][Math]::Round(255 - $lum)
        if ($a -lt 8) { $a = 0 }
        $px[$i] = 255; $px[$i + 1] = 255; $px[$i + 2] = 255; $px[$i + 3] = [byte]$a
        if ($x -lt 12 -or $y -lt 12 -or $x -ge $w - 12 -or $y -ge $h - 12) { $px[$i + 3] = 0; continue }
        if ($a -gt 128) { if ($x -lt $x0) { $x0 = $x }; if ($x -gt $x1) { $x1 = $x }; if ($y -lt $y0) { $y0 = $y }; if ($y -gt $y1) { $y1 = $y } }
    }
}
$full = [Windows.Media.Imaging.BitmapSource]::Create($w, $h, 96, 96, [Windows.Media.PixelFormats]::Bgra32, $null, $px, $stride)
$pad = 8
$rect = New-Object Windows.Int32Rect ([Math]::Max(0, $x0 - $pad)), ([Math]::Max(0, $y0 - $pad)), ([Math]::Min($w - 1, $x1 + $pad) - [Math]::Max(0, $x0 - $pad)), ([Math]::Min($h - 1, $y1 + $pad) - [Math]::Max(0, $y0 - $pad))
$crop = New-Object Windows.Media.Imaging.CroppedBitmap($full, $rect)
$enc = New-Object Windows.Media.Imaging.PngBitmapEncoder
$enc.Frames.Add([Windows.Media.Imaging.BitmapFrame]::Create($crop))
$fs = [IO.File]::Create($Out); $enc.Save($fs); $fs.Close()
"saved $Out  $($rect.Width)x$($rect.Height)"

