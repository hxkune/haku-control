# SPDX-License-Identifier: GPL-3.0-only
# The background of haku's screen on a Divoom Times Frame (800x1280, portrait): a dark gradient with the haku
# mark barely there. The frame fetches it by URL through Divoom's cloud, so it lives in the repository
# (art/frame-bg.jpg) and is used from raw.githubusercontent.com. Run: powershell -File art/gen-frame-bg.ps1
Add-Type -AssemblyName System.Drawing
$w = 800; $h = 1280
$bmp = New-Object System.Drawing.Bitmap $w, $h
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.SmoothingMode = 'AntiAlias'; $g.InterpolationMode = 'HighQualityBicubic'
$rect = New-Object System.Drawing.Rectangle 0, 0, $w, $h
$grad = New-Object System.Drawing.Drawing2D.LinearGradientBrush $rect, ([System.Drawing.Color]::FromArgb(255, 12, 12, 14)), ([System.Drawing.Color]::FromArgb(255, 20, 18, 26)), 90
$g.FillRectangle($grad, $rect)
$mark = Join-Path $PSScriptRoot 'haku-mark-source.png'
if (Test-Path $mark) {
    $img = [System.Drawing.Image]::FromFile($mark)
    $cm = New-Object System.Drawing.Imaging.ColorMatrix
    $cm.Matrix33 = 0.05   # barely there
    $ia = New-Object System.Drawing.Imaging.ImageAttributes
    $ia.SetColorMatrix($cm)
    $mw = 380; $mh = [int]($img.Height * $mw / $img.Width)
    $dest = New-Object System.Drawing.Rectangle ([int](($w - $mw) / 2)), ($h - $mh - 40), $mw, $mh
    $g.DrawImage($img, $dest, 0, 0, $img.Width, $img.Height, 'Pixel', $ia)
    $img.Dispose()
}
$g.Dispose()
$enc = [System.Drawing.Imaging.ImageCodecInfo]::GetImageEncoders() | Where-Object { $_.MimeType -eq 'image/jpeg' }
$p = New-Object System.Drawing.Imaging.EncoderParameters 1
$p.Param[0] = New-Object System.Drawing.Imaging.EncoderParameter ([System.Drawing.Imaging.Encoder]::Quality), 90L
$out = Join-Path $PSScriptRoot 'frame-bg.jpg'
$bmp.Save($out, $enc, $p)
$bmp.Dispose()
"saved $out"
