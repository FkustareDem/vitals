# make_icon.ps1 -- 用代码绘制 vitals 图标, 输出多尺寸 vitals.ico
Add-Type -AssemblyName System.Drawing
$outDir = if ($PSScriptRoot) { $PSScriptRoot } else { (Get-Location).Path }
$sizes  = @(16,20,24,32,40,48,64,96,128,256)
$pngs   = @{}

function Draw-Icon([int]$S) {
    $bmp = New-Object System.Drawing.Bitmap($S, $S, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g   = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.Clear([System.Drawing.Color]::Transparent)

    $m = [double]($S * 0.035); $w = $S - 2*$m; $r = [double]($S * 0.24)
    $p = New-Object System.Drawing.Drawing2D.GraphicsPath
    $p.AddArc($m,       $m,       $r, $r, 180, 90)
    $p.AddArc($m+$w-$r, $m,       $r, $r, 270, 90)
    $p.AddArc($m+$w-$r, $m+$w-$r, $r, $r,   0, 90)
    $p.AddArc($m,       $m+$w-$r, $r, $r,  90, 90)
    $p.CloseFigure()

    $bg = New-Object System.Drawing.Drawing2D.LinearGradientBrush(
            (New-Object System.Drawing.PointF(0,0)),
            (New-Object System.Drawing.PointF($S,$S)),
            [System.Drawing.Color]::FromArgb(255, 12, 22, 32),
            [System.Drawing.Color]::FromArgb(255,  3,  7, 11))
    $g.FillPath($bg, $p)
    $penC = New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(255, 0, 229, 255), ([Math]::Max(1.0, $S*0.055)))
    $g.DrawPath($penC, $p)

    $raw = @(
        @(0.06,0.56), @(0.20,0.56), @(0.26,0.50), @(0.32,0.56),
        @(0.38,0.60), @(0.44,0.14), @(0.50,0.94), @(0.56,0.56),
        @(0.64,0.56), @(0.72,0.44), @(0.82,0.56), @(0.94,0.56))
    $pts = New-Object 'System.Drawing.PointF[]' $raw.Count
    for ($i = 0; $i -lt $raw.Count; $i++) {
        $pts[$i] = New-Object System.Drawing.PointF(($raw[$i][0]*$S), ($raw[$i][1]*$S))
    }
    $glow = New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(85, 57, 255, 20), ($S*0.22))
    $glow.LineJoin = [System.Drawing.Drawing2D.LineJoin]::Round
    $g.DrawLines($glow, $pts)

    $penL = New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(255, 74, 255, 60), ([Math]::Max(1.2, $S*0.10)))
    $penL.LineJoin = [System.Drawing.Drawing2D.LineJoin]::Round
    $penL.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
    $penL.EndCap   = [System.Drawing.Drawing2D.LineCap]::Round
    $g.DrawLines($penL, $pts)

    $g.Dispose()
    return $bmp
}

foreach ($S in $sizes) {
    $bmp = Draw-Icon $S
    $ms  = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    $pngs[$S] = $ms.ToArray()
    $bmp.Dispose(); $ms.Dispose()
    Write-Host ("  {0,3} x {0,-3}   {1,6} bytes" -f $S, $pngs[$S].Length)
}

$ms = New-Object System.IO.MemoryStream
$bw = New-Object System.IO.BinaryWriter($ms)
$bw.Write([UInt16]0); $bw.Write([UInt16]1); $bw.Write([UInt16]$sizes.Count)
$offset = 6 + 16*$sizes.Count
foreach ($S in $sizes) {
    $b  = $pngs[$S]
    $dm = if ($S -ge 256) { 0 } else { $S }
    $bw.Write([byte]$dm); $bw.Write([byte]$dm)
    $bw.Write([byte]0);   $bw.Write([byte]0)
    $bw.Write([UInt16]1); $bw.Write([UInt16]32)
    $bw.Write([UInt32]$b.Length); $bw.Write([UInt32]$offset)
    $offset += $b.Length
}
foreach ($S in $sizes) { $bw.Write($pngs[$S]) }
$bw.Flush()
[System.IO.File]::WriteAllBytes((Join-Path $outDir 'vitals.ico'), $ms.ToArray())
$bw.Dispose(); $ms.Dispose()
Write-Host ("[OK] vitals.ico  {0} bytes, {1} sizes" -f (Get-Item (Join-Path $outDir 'vitals.ico')).Length, $sizes.Count)
