param(
    [string]$RainmeterIcon = 'C:\Rainmeter4\Application\Rainmeter.ico',
    [string]$Output = (Join-Path $PSScriptRoot '..\src\RainBackup.ico')
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$source = [IO.File]::ReadAllBytes($RainmeterIcon)
$frames = [System.Collections.Generic.List[byte[]]]::new()
$sizes = 16, 20, 24, 32, 48, 64, 128, 256
foreach ($size in $sizes) {
    $index = [Array]::IndexOf($sizes, $size)
    $entry = 6 + 16 * $index
    $length = [BitConverter]::ToUInt32($source, $entry + 8)
    $offset = [BitConverter]::ToUInt32($source, $entry + 12)
    $single = [IO.MemoryStream]::new()
    $writer = [IO.BinaryWriter]::new($single)
    $writer.Write([uint16]0)
    $writer.Write([uint16]1)
    $writer.Write([uint16]1)
    $writer.Write($source, $entry, 12)
    $writer.Write([uint32]22)
    $writer.Write($source, $offset, $length)
    $single.Position = 0
    if ($source[$offset] -eq 137) {
        $frame = [IO.MemoryStream]::new($source, [int]$offset, [int]$length)
        $original = [Drawing.Image]::FromStream($frame)
    } else {
        $frame = $null
        $icon = [Drawing.Icon]::new($single)
        $original = $icon.ToBitmap()
    }
    $image = [Drawing.Bitmap]::new($size, $size, [Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $graphics = [Drawing.Graphics]::FromImage($image)
    $graphics.DrawImage($original, 0, 0, $size, $size)
    $graphics.SmoothingMode = [Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $graphics.ScaleTransform($size / 64.0, $size / 64.0)
    $outline = [Drawing.SolidBrush]::new([Drawing.Color]::FromArgb(245, 249, 255))
    $badge = [Drawing.SolidBrush]::new([Drawing.Color]::FromArgb(15, 65, 116))
    $pen = [Drawing.Pen]::new([Drawing.Color]::White, 3.0)
    $pen.StartCap = $pen.EndCap = [Drawing.Drawing2D.LineCap]::Round
    $pen.LineJoin = [Drawing.Drawing2D.LineJoin]::Round
    $graphics.FillEllipse($outline, 28.0, 27.0, 36.0, 36.0)
    $graphics.FillEllipse($badge, 30.0, 29.0, 32.0, 32.0)
    $graphics.DrawLines($pen, [Drawing.PointF[]]@(
        [Drawing.PointF]::new(37, 47), [Drawing.PointF]::new(37, 53),
        [Drawing.PointF]::new(55, 53), [Drawing.PointF]::new(55, 47)))
    $graphics.DrawLine($pen, 46, 36, 46, 46)
    $graphics.DrawLines($pen, [Drawing.PointF[]]@(
        [Drawing.PointF]::new(41, 42), [Drawing.PointF]::new(46, 47), [Drawing.PointF]::new(51, 42)))
    $png = [IO.MemoryStream]::new()
    $image.Save($png, [Drawing.Imaging.ImageFormat]::Png)
    $frames.Add($png.ToArray())
    foreach ($resource in $png, $pen, $badge, $outline, $graphics, $image, $original, $frame, $icon, $writer, $single) {
        if ($resource) { $resource.Dispose() }
    }
    $icon = $null
}
$writer = [IO.BinaryWriter]::new([IO.File]::Create([IO.Path]::GetFullPath($Output)))
try {
    $writer.Write([uint16]0)
    $writer.Write([uint16]1)
    $writer.Write([uint16]$frames.Count)
    $offset = 6 + 16 * $frames.Count
    for ($i = 0; $i -lt $frames.Count; ++$i) {
        $size = [byte]($sizes[$i] % 256)
        $writer.Write($size)
        $writer.Write($size)
        $writer.Write([uint16]0)
        $writer.Write([uint16]1)
        $writer.Write([uint16]32)
        $writer.Write([uint32]$frames[$i].Length)
        $writer.Write([uint32]$offset)
        $offset += $frames[$i].Length
    }
    foreach ($frame in $frames) { $writer.Write($frame) }
} finally {
    $writer.Dispose()
}
