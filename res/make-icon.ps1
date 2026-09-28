# 런처 아이콘 생성 — res\Tridef3D_Play.ico, res\Tridef3D_Play_SR.ico
#
# 그림 원본은 res\icon-source.png (860x860): 파란 유리 타일에 입체 "3D" 와 "Tridef" 글자.
# 사용자가 고른 시안이다. 이전 아이콘(겹친 두 화면 + 재생 삼각형)은 SRCapture3D 아이콘과 같은
# 계열이라 구분이 안 돼서 바꿨다.
#
#   48px 이상  타일 전체 ("3D" + "Tridef")
#   40px 이하  "Tridef" 글자가 뭉개지므로 "3D" 부분만 확대
#   SR 변형    색조를 보라 쪽으로 돌리고, 40px 이상에는 오른쪽 아래에 "SR" 배지
#
# .ico 는 DIB 프레임으로 직접 조립한다 (아래 Get-DibBytes 설명 참고).
param([ValidateSet('Play', 'SR', 'All')][string]$Variant = 'All')
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$srcPath = Join-Path $PSScriptRoot 'icon-source.png'
$script:src = [Drawing.Bitmap]::FromFile($srcPath)

# icon-source.png 안에서의 영역 (픽셀)
$TileRect  = New-Object Drawing.RectangleF 44, 47, 740, 776     # 타일 전체 (살짝 안쪽으로 잡아 바깥 어두운 테두리를 뺀다)
$GlyphRect = New-Object Drawing.RectangleF 112, 52, 606, 606    # "3D" 글자 주변 정사각형
$CornerRatio = 0.205                                             # 모서리 반지름 / 한 변

function New-RoundRect([single]$x, [single]$y, [single]$w, [single]$h, [single]$r) {
    $p = New-Object Drawing.Drawing2D.GraphicsPath
    $p.AddArc($x, $y, 2*$r, 2*$r, 180, 90)
    $p.AddArc($x + $w - 2*$r, $y, 2*$r, 2*$r, 270, 90)
    $p.AddArc($x + $w - 2*$r, $y + $h - 2*$r, 2*$r, 2*$r, 0, 90)
    $p.AddArc($x, $y + $h - 2*$r, 2*$r, 2*$r, 90, 90)
    $p.CloseFigure()
    return $p
}

# 색조 회전 행렬 (도 단위). 밝기는 유지한다.
function New-HueMatrix([double]$deg) {
    $a = $deg * [Math]::PI / 180; $c = [Math]::Cos($a); $s = [Math]::Sin($a)
    $lr = 0.213; $lg = 0.715; $lb = 0.072
    $m = New-Object Drawing.Imaging.ColorMatrix
    $m.Matrix00 = [single]($lr + $c*(1-$lr) + $s*(-$lr));  $m.Matrix01 = [single]($lr + $c*(-$lr) + $s*0.143);   $m.Matrix02 = [single]($lr + $c*(-$lr) + $s*(-(1-$lr)))
    $m.Matrix10 = [single]($lg + $c*(-$lg) + $s*(-$lg));   $m.Matrix11 = [single]($lg + $c*(1-$lg) + $s*0.140);  $m.Matrix12 = [single]($lg + $c*(-$lg) + $s*$lg)
    $m.Matrix20 = [single]($lb + $c*(-$lb) + $s*(1-$lb));  $m.Matrix21 = [single]($lb + $c*(-$lb) + $s*(-0.283)); $m.Matrix22 = [single]($lb + $c*(1-$lb) + $s*$lb)
    $m.Matrix33 = 1; $m.Matrix44 = 1
    return $m
}

function New-Frame([int]$S, [string]$Kind) {
    $sr = ($Kind -eq 'SR')
    $big = 4 * $S                                  # 4배로 그린 뒤 줄여서 가장자리를 매끈하게
    $work = New-Object Drawing.Bitmap $big, $big, ([Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [Drawing.Graphics]::FromImage($work)
    $g.SmoothingMode = 'AntiAlias'; $g.InterpolationMode = 'HighQualityBicubic'; $g.PixelOffsetMode = 'HighQuality'
    $g.TextRenderingHint = 'AntiAliasGridFit'

    $from = if ($S -le 40) { $GlyphRect } else { $TileRect }
    $clip = New-RoundRect 0 0 $big $big ([single]($CornerRatio * $big))
    $g.SetClip($clip)
    $attr = New-Object Drawing.Imaging.ImageAttributes
    if ($sr) { $attr.SetColorMatrix((New-HueMatrix 62)) }
    $g.DrawImage($script:src, (New-Object Drawing.Rectangle 0, 0, $big, $big),
                 $from.X, $from.Y, $from.Width, $from.Height, [Drawing.GraphicsUnit]::Pixel, $attr)
    if ($S -le 40) {
        # 확대한 조각에는 타일 테두리가 없으니 얇은 하이라이트를 둘러 타일처럼 보이게 한다
        $rim = New-Object Drawing.Pen ([Drawing.Color]::FromArgb(150, 170, 200, 255)), ([single]($big * 0.06))
        $g.DrawPath($rim, $clip); $rim.Dispose()
    }
    $g.ResetClip()

    if ($sr -and $S -ge 40) {
        $u = $big / 256.0
        if ($S -ge 48) {
            # 타일 전체를 쓰는 크기: 아래 "Tridef" 글자 자리를 띠로 덮고 "Tridef SR" 로 다시 쓴다
            $bx = 20*$u; $bw = $big - 40*$u; $bh = 72*$u; $by = $big - $bh - 12*$u
            $label = 'Tridef SR'; $fs = 42*$u
        } else {
            # "3D" 만 확대한 크기: 오른쪽 아래 작은 배지
            $bw = 122*$u; $bh = 76*$u; $bx = $big - $bw - 6*$u; $by = $big - $bh - 6*$u
            $label = 'SR'; $fs = 50*$u
        }
        $badge = New-RoundRect $bx $by $bw $bh (18*$u)
        $bb = New-Object Drawing.SolidBrush ([Drawing.Color]::FromArgb(245, 22, 12, 40))
        $bp = New-Object Drawing.Pen ([Drawing.Color]::FromArgb(255, 255, 200, 70)), ([single](5*$u))
        $g.FillPath($bb, $badge); $g.DrawPath($bp, $badge)
        $font = New-Object Drawing.Font 'Segoe UI', ([single]$fs), ([Drawing.FontStyle]::Bold), ([Drawing.GraphicsUnit]::Pixel)
        $fmt = New-Object Drawing.StringFormat
        $fmt.Alignment = 'Center'; $fmt.LineAlignment = 'Center'
        $g.DrawString($label, $font, [Drawing.Brushes]::White, (New-Object Drawing.RectangleF $bx, $by, $bw, $bh), $fmt)
        foreach ($d in $badge, $bb, $bp, $font, $fmt) { $d.Dispose() }
    }
    foreach ($d in $g, $clip, $attr) { $d.Dispose() }

    $bmp = New-Object Drawing.Bitmap $S, $S, ([Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g2 = [Drawing.Graphics]::FromImage($bmp)
    $g2.InterpolationMode = 'HighQualityBicubic'; $g2.PixelOffsetMode = 'HighQuality'; $g2.CompositingQuality = 'HighQuality'
    $g2.DrawImage($work, 0, 0, $S, $S)
    $g2.Dispose(); $work.Dispose()
    return $bmp
}

# 프레임은 DIB(BITMAPINFOHEADER + 32bpp BGRA + AND 마스크)로 담는다. PNG 프레임도 Windows 는
# 읽지만, PyInstaller 등 .ico 를 직접 파싱하는 도구와 System.Drawing.Icon 이 못 읽는 경우가 있다.
# 주의: 이 파일은 반드시 BOM 있는 UTF-8 로 저장한다. BOM 이 없으면 Windows PowerShell 5.1 이 CP949 로
# 읽어서, 한글 주석 끝 바이트가 줄바꿈을 삼키고 다음 줄이 주석에 붙는다. 실제로 헤더 쓰는 줄 하나가
# 통째로 실행되지 않아 프레임마다 8바이트가 빠진 깨진 .ico 가 만들어졌었다 (PowerShell 7 은 무관).
function Add-U16($ms, [int]$v) { $b = [BitConverter]::GetBytes([uint16]$v); $ms.Write($b, 0, 2) }
function Add-U32($ms, [long]$v) { $b = [BitConverter]::GetBytes([uint32]$v); $ms.Write($b, 0, 4) }

function Get-DibBytes([Drawing.Bitmap]$bmp) {
    $w = $bmp.Width; $h = $bmp.Height
    $ms = New-Object IO.MemoryStream
    $maskStride = [int]([Math]::Floor(($w + 31) / 32)) * 4
    Add-U32 $ms 40; Add-U32 $ms $w; Add-U32 $ms ($h * 2)      # 높이는 색 + 마스크
    Add-U16 $ms 1;  Add-U16 $ms 32; Add-U32 $ms 0
    Add-U32 $ms ($w * $h * 4 + $maskStride * $h)
    Add-U32 $ms 0; Add-U32 $ms 0; Add-U32 $ms 0; Add-U32 $ms 0
    $rect = New-Object Drawing.Rectangle 0, 0, $w, $h
    $data = $bmp.LockBits($rect, [Drawing.Imaging.ImageLockMode]::ReadOnly, [Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $row = New-Object byte[] ($w * 4)
    for ($y = $h - 1; $y -ge 0; $y--) {    # DIB 는 아래에서 위로
        [Runtime.InteropServices.Marshal]::Copy(([IntPtr]($data.Scan0.ToInt64() + $y * $data.Stride)), $row, 0, $row.Length)
        $ms.Write($row, 0, $row.Length)
    }
    $bmp.UnlockBits($data)
    $mask = New-Object byte[] ($maskStride * $h)             # AND 마스크는 전부 0 (알파를 쓴다)
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
    Add-U16 $out 0; Add-U16 $out 1; Add-U16 $out $frames.Count
    $offset = 6 + 16 * $frames.Count
    foreach ($p in $frames) {
        $dim = $p.Size; if ($dim -ge 256) { $dim = 0 }
        $out.WriteByte([byte]$dim); $out.WriteByte([byte]$dim)
        $out.WriteByte(0); $out.WriteByte(0)
        Add-U16 $out 1; Add-U16 $out 32
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
$script:src.Dispose()
