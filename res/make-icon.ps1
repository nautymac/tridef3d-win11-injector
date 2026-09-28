# 런처 아이콘 생성 - res\Tridef3D_Play.ico, res\Tridef3D_Play_SR.ico
#
# 디자인: 유리 느낌의 둥근 타일 위에 두께가 있는 흰색 "3D" 글자, 그 아래 흰색 "Tridef".
# 사용자가 고른 시안(res\icon-source.png, 파란 유리 타일 + 3D + Tridef)을 본떠 직접 그린다.
# 시안 그림을 그대로 쓰지 않는 이유: 글자를 흰색으로 해 달라는 요청인데, 그림 속 글자는
# 배경과 밝기가 겹쳐 깨끗하게 골라낼 수 없다.
#
#   Play      파란 타일, "3D" + "Tridef"
#   SR        보라 타일, "3D" + "Tridef SR"
#   40px 이하 아래 글자는 읽히지 않으므로 "3D" 만 크게
#
# 주의: 이 파일은 반드시 BOM 있는 UTF-8 로 저장한다. BOM 이 없으면 Windows PowerShell 5.1 이
# CP949 로 읽어서, 한글 주석 끝 바이트가 줄바꿈을 삼키고 다음 줄이 주석에 붙는다. 실제로 헤더를
# 쓰는 줄 하나가 실행되지 않아 프레임마다 8바이트가 빠진 깨진 .ico 가 만들어진 적이 있다.
param([ValidateSet('Play', 'SR', 'All')][string]$Variant = 'All')
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

function New-Color([int]$a, [int]$r, [int]$g, [int]$b) { return [Drawing.Color]::FromArgb($a, $r, $g, $b) }

function New-RoundRect([single]$x, [single]$y, [single]$w, [single]$h, [single]$r) {
    $p = New-Object Drawing.Drawing2D.GraphicsPath
    $p.AddArc($x, $y, 2*$r, 2*$r, 180, 90)
    $p.AddArc($x + $w - 2*$r, $y, 2*$r, 2*$r, 270, 90)
    $p.AddArc($x + $w - 2*$r, $y + $h - 2*$r, 2*$r, 2*$r, 0, 90)
    $p.AddArc($x, $y + $h - 2*$r, 2*$r, 2*$r, 90, 90)
    $p.CloseFigure()
    return $p
}

function Get-FontFamily {
    foreach ($n in 'Segoe UI Black', 'Arial Black', 'Segoe UI') {
        try { $f = New-Object Drawing.FontFamily $n; if ($f.Name -eq $n) { return $f } } catch { }
    }
    return [Drawing.FontFamily]::GenericSansSerif
}

# 글자 외곽선을 만들어 주어진 사각형 안에 비율을 지켜 꽉 채운다
function New-TextPath([string]$text, [Drawing.FontFamily]$family, [Drawing.RectangleF]$box) {
    $p = New-Object Drawing.Drawing2D.GraphicsPath
    $style = [int][Drawing.FontStyle]::Bold
    if (-not $family.IsStyleAvailable([Drawing.FontStyle]::Bold)) { $style = [int][Drawing.FontStyle]::Regular }
    $p.AddString($text, $family, $style, 100, (New-Object Drawing.PointF 0, 0), [Drawing.StringFormat]::GenericTypographic)
    $b = $p.GetBounds()
    $s = [Math]::Min($box.Width / $b.Width, $box.Height / $b.Height)
    $m = New-Object Drawing.Drawing2D.Matrix
    $m.Translate([single]($box.X + ($box.Width - $b.Width * $s) / 2), [single]($box.Y + ($box.Height - $b.Height * $s) / 2))
    $m.Scale([single]$s, [single]$s)
    $m.Translate([single](-$b.X), [single](-$b.Y))
    $p.Transform($m)
    $m.Dispose()
    return $p
}

function Add-Extruded($g, $path, [single]$depth, $sideColor, $shadowColor, [single]$u) {
    # 그림자
    $sb = New-Object Drawing.SolidBrush $shadowColor
    $g.TranslateTransform([single]($depth * 0.9 + 5*$u), [single]($depth * 1.3 + 7*$u))
    $g.FillPath($sb, $path)
    $g.ResetTransform()
    $sb.Dispose()
    # 옆면: 오른쪽 아래로 밀어 가며 겹쳐 그린다
    $steps = [int][Math]::Max(2, [Math]::Ceiling($depth))
    $side = New-Object Drawing.SolidBrush $sideColor
    for ($i = $steps; $i -ge 1; $i--) {
        $t = $depth * $i / $steps
        $g.TranslateTransform([single]($t * 0.55), [single]$t)
        $g.FillPath($side, $path)
        $g.ResetTransform()
    }
    $side.Dispose()
    # 앞면: 흰색 (아래로 갈수록 아주 약간 푸르게)
    $bb = $path.GetBounds()
    $face = New-Object Drawing.Drawing2D.LinearGradientBrush (
        (New-Object Drawing.PointF $bb.X, ($bb.Y - 1)), (New-Object Drawing.PointF $bb.X, ($bb.Bottom + 1)),
        (New-Color 255 255 255 255), (New-Color 255 226 236 250))
    $g.FillPath($face, $path)
    $face.Dispose()
}

function New-Frame([int]$S, [string]$Kind) {
    $sr = ($Kind -eq 'SR')
    $big = 4 * $S
    $u = $big / 256.0
    $work = New-Object Drawing.Bitmap $big, $big, ([Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [Drawing.Graphics]::FromImage($work)
    $g.SmoothingMode = 'AntiAlias'
    $g.PixelOffsetMode = 'HighQuality'

    if ($sr) {
        $cTop = New-Color 255 168 124 214; $cMid = New-Color 255 112 64 168; $cBot = New-Color 255 70 34 120
        $cSide = New-Color 255 196 176 226; $cShadow = New-Color 120 30 8 60
        $cRim = New-Color 230 226 200 250; $cEdge = New-Color 255 52 22 92
    } else {
        $cTop = New-Color 255 122 160 210; $cMid = New-Color 255 60 100 162; $cBot = New-Color 255 34 62 118
        $cSide = New-Color 255 170 192 226; $cShadow = New-Color 120 6 20 56
        $cRim = New-Color 230 190 214 248; $cEdge = New-Color 255 22 44 92
    }

    # 타일
    $inset = 3 * $u
    $tile = New-RoundRect $inset $inset ($big - 2*$inset) ($big - 2*$inset) ([single](52 * $u))
    $bg = New-Object Drawing.Drawing2D.LinearGradientBrush (
        (New-Object Drawing.PointF 0, 0), (New-Object Drawing.PointF 0, $big), $cTop, $cBot)
    $blend = New-Object Drawing.Drawing2D.ColorBlend 3
    $blend.Colors = @($cTop, $cMid, $cBot)
    $blend.Positions = @([single]0, [single]0.45, [single]1)
    $bg.InterpolationColors = $blend
    $g.FillPath($bg, $tile)

    # 유리 광택: 왼쪽 위에서 비스듬히 내려오는 밝은 면
    $g.SetClip($tile)
    $gloss = New-Object Drawing.Drawing2D.GraphicsPath
    $gloss.AddPolygon(@(
        (New-Object Drawing.PointF 0, 0),
        (New-Object Drawing.PointF ([single](150 * $u)), 0),
        (New-Object Drawing.PointF ([single](60 * $u)), ([single](256 * $u))),
        (New-Object Drawing.PointF 0, ([single](256 * $u)))))
    $gb = New-Object Drawing.Drawing2D.LinearGradientBrush (
        (New-Object Drawing.PointF 0, 0), (New-Object Drawing.PointF ([single](150 * $u)), 0),
        (New-Color 70 255 255 255), (New-Color 8 255 255 255))
    $g.FillPath($gb, $gloss)
    $g.ResetClip()

    # 테두리: 바깥 어두운 선 + 안쪽 밝은 선
    $edge = New-Object Drawing.Pen $cEdge, ([single]([Math]::Max(2.0, 5 * $u)))
    $g.DrawPath($edge, $tile)
    $rimPath = New-RoundRect ($inset + 5*$u) ($inset + 5*$u) ($big - 2*$inset - 10*$u) ($big - 2*$inset - 10*$u) ([single](47 * $u))
    $rim = New-Object Drawing.Pen $cRim, ([single]([Math]::Max(2.0, 4 * $u)))
    $g.DrawPath($rim, $rimPath)

    # 글자
    $family = Get-FontFamily
    if ($S -le 40) {
        $box3d = New-Object Drawing.RectangleF ([single](30*$u)), ([single](58*$u)), ([single](190*$u)), ([single](132*$u))
        $depth = [single](10 * $u)
    } else {
        $box3d = New-Object Drawing.RectangleF ([single](34*$u)), ([single](38*$u)), ([single](184*$u)), ([single](118*$u))
        $depth = [single](9 * $u)
    }
    $p3d = New-TextPath '3D' $family $box3d
    Add-Extruded $g $p3d $depth $cSide $cShadow $u
    $p3d.Dispose()

    if ($S -gt 40) {
        $label = if ($sr) { 'Tridef SR' } else { 'Tridef' }
        $boxT = if ($sr) { New-Object Drawing.RectangleF ([single](30*$u)), ([single](180*$u)), ([single](196*$u)), ([single](40*$u)) }
                else     { New-Object Drawing.RectangleF ([single](50*$u)), ([single](178*$u)), ([single](156*$u)), ([single](44*$u)) }
        $pt = New-TextPath $label $family $boxT
        Add-Extruded $g $pt ([single](3 * $u)) $cSide $cShadow $u
        $pt.Dispose()
    }

    foreach ($d in $g, $bg, $gloss, $gb, $edge, $rim, $rimPath, $tile) { $d.Dispose() }

    $bmp = New-Object Drawing.Bitmap $S, $S, ([Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g2 = [Drawing.Graphics]::FromImage($bmp)
    $g2.InterpolationMode = 'HighQualityBicubic'
    $g2.PixelOffsetMode = 'HighQuality'
    $g2.CompositingQuality = 'HighQuality'
    $g2.DrawImage($work, 0, 0, $S, $S)
    $g2.Dispose()
    $work.Dispose()
    return $bmp
}

function Add-U16($ms, [int]$v) { $b = [BitConverter]::GetBytes([uint16]$v); $ms.Write($b, 0, 2) }
function Add-U32($ms, [long]$v) { $b = [BitConverter]::GetBytes([uint32]$v); $ms.Write($b, 0, 4) }

# 프레임은 DIB(BITMAPINFOHEADER + 32bpp BGRA + AND 마스크)로 담는다. PNG 프레임도 Windows 는
# 읽지만, PyInstaller 등 .ico 를 직접 파싱하는 도구와 System.Drawing.Icon 이 못 읽는 경우가 있다.
function Get-DibBytes([Drawing.Bitmap]$bmp) {
    $w = $bmp.Width
    $h = $bmp.Height
    $ms = New-Object IO.MemoryStream
    $maskStride = [int]([Math]::Floor(($w + 31) / 32)) * 4
    Add-U32 $ms 40
    Add-U32 $ms $w
    Add-U32 $ms ($h * 2)
    Add-U16 $ms 1
    Add-U16 $ms 32
    Add-U32 $ms 0
    Add-U32 $ms ($w * $h * 4 + $maskStride * $h)
    Add-U32 $ms 0
    Add-U32 $ms 0
    Add-U32 $ms 0
    Add-U32 $ms 0
    $rect = New-Object Drawing.Rectangle 0, 0, $w, $h
    $data = $bmp.LockBits($rect, [Drawing.Imaging.ImageLockMode]::ReadOnly, [Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $row = New-Object byte[] ($w * 4)
    for ($y = $h - 1; $y -ge 0; $y--) {
        [Runtime.InteropServices.Marshal]::Copy(([IntPtr]($data.Scan0.ToInt64() + $y * $data.Stride)), $row, 0, $row.Length)
        $ms.Write($row, 0, $row.Length)
    }
    $bmp.UnlockBits($data)
    $mask = New-Object byte[] ($maskStride * $h)
    $ms.Write($mask, 0, $mask.Length)
    $bytes = $ms.ToArray()
    $ms.Dispose()
    return ,$bytes
}

function Write-Ico([string]$Kind, [string]$FileName) {
    $sizes = 16, 20, 24, 32, 40, 48, 64, 128, 256
    $frames = @()
    foreach ($s in $sizes) {
        $bmp = New-Frame $s $Kind
        $frames += , @{ Size = $s; Bytes = (Get-DibBytes $bmp) }
        $bmp.Dispose()
    }
    $out = New-Object IO.MemoryStream
    Add-U16 $out 0
    Add-U16 $out 1
    Add-U16 $out $frames.Count
    $offset = 6 + 16 * $frames.Count
    foreach ($p in $frames) {
        $dim = $p.Size
        if ($dim -ge 256) { $dim = 0 }
        $out.WriteByte([byte]$dim)
        $out.WriteByte([byte]$dim)
        $out.WriteByte(0)
        $out.WriteByte(0)
        Add-U16 $out 1
        Add-U16 $out 32
        Add-U32 $out $p.Bytes.Length
        Add-U32 $out $offset
        $offset += $p.Bytes.Length
    }
    foreach ($p in $frames) { $out.Write($p.Bytes, 0, $p.Bytes.Length) }
    $dest = Join-Path $PSScriptRoot $FileName
    [IO.File]::WriteAllBytes($dest, $out.ToArray())
    $out.Dispose()
    Write-Host "wrote $dest ($((Get-Item $dest).Length) bytes, $($frames.Count) frames)"
}

if ($Variant -in 'Play', 'All') { Write-Ico 'Play' 'Tridef3D_Play.ico' }
if ($Variant -in 'SR', 'All')   { Write-Ico 'SR'   'Tridef3D_Play_SR.ico' }
