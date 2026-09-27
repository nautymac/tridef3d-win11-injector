# Tridef3D_Play 아이콘 생성 — res\Tridef3D_Play.ico
#
# 그림: 어두운 바탕에 좌/우 눈을 뜻하는 두 화면(시안·마젠타)이 엇갈려 겹쳐 있고, 그 앞에 흰색
# 재생 삼각형. "3D 로 게임을 띄우는 런처"라는 뜻이다. 16px 에서도 삼각형과 겹친 사각형으로 읽힌다.
# SRCapture3D 아이콘(겹친 두 화면 + 렌티큘러 세로줄)과 같은 계열이되 삼각형으로 구분된다.
#
# .ico 는 PNG 프레임을 담는 Vista 형식으로 직접 조립한다.
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

function New-Frame([int]$S) {
    $bmp = New-Object Drawing.Bitmap $S, $S, ([Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = 'AntiAlias'
    $u = $S / 256.0

    # 배경 라운드 사각형
    $r = 44 * $u
    $path = New-Object Drawing.Drawing2D.GraphicsPath
    $path.AddArc(0, 0, 2*$r, 2*$r, 180, 90)
    $path.AddArc($S - 2*$r, 0, 2*$r, 2*$r, 270, 90)
    $path.AddArc($S - 2*$r, $S - 2*$r, 2*$r, 2*$r, 0, 90)
    $path.AddArc(0, $S - 2*$r, 2*$r, 2*$r, 90, 90)
    $path.CloseFigure()
    $bg = New-Object Drawing.Drawing2D.LinearGradientBrush (
        (New-Object Drawing.Point 0, 0), (New-Object Drawing.Point 0, $S),
        [Drawing.Color]::FromArgb(255, 34, 22, 48), [Drawing.Color]::FromArgb(255, 14, 9, 22))
    $g.FillPath($bg, $path)

    # 두 장의 화면 (오른눈 마젠타가 뒤, 왼눈 시안이 앞)
    $w = 132 * $u; $h = 108 * $u; $shift = 24 * $u
    $cx = $S / 2.0; $cy = $S / 2.0
    $rr = 12 * $u
    $rects = @(
        @((New-Object Drawing.RectangleF ($cx - $w/2 + $shift), ($cy - $h/2 - 12*$u), $w, $h),
          (New-Object Drawing.SolidBrush ([Drawing.Color]::FromArgb(225, 232, 62, 140)))),
        @((New-Object Drawing.RectangleF ($cx - $w/2 - $shift), ($cy - $h/2 + 12*$u), $w, $h),
          (New-Object Drawing.SolidBrush ([Drawing.Color]::FromArgb(225, 40, 205, 226))))
    )
    foreach ($pair in $rects) {
        $rc = $pair[0]; $br = $pair[1]
        $p2 = New-Object Drawing.Drawing2D.GraphicsPath
        $p2.AddArc($rc.X, $rc.Y, 2*$rr, 2*$rr, 180, 90)
        $p2.AddArc($rc.Right - 2*$rr, $rc.Y, 2*$rr, 2*$rr, 270, 90)
        $p2.AddArc($rc.Right - 2*$rr, $rc.Bottom - 2*$rr, 2*$rr, 2*$rr, 0, 90)
        $p2.AddArc($rc.X, $rc.Bottom - 2*$rr, 2*$rr, 2*$rr, 90, 90)
        $p2.CloseFigure()
        $g.FillPath($br, $p2)
        $p2.Dispose(); $br.Dispose()
    }

    # 재생 삼각형 — 가운데, 약간의 그림자로 화면에서 떠 보이게
    $tw = 62 * $u; $th = 74 * $u
    $tx = $cx - $tw * 0.35; $ty = $cy
    $tri = New-Object Drawing.Drawing2D.GraphicsPath
    $tri.AddPolygon(@(
        (New-Object Drawing.PointF ($tx - $tw*0.30), ($ty - $th/2)),
        (New-Object Drawing.PointF ($tx - $tw*0.30), ($ty + $th/2)),
        (New-Object Drawing.PointF ($tx + $tw*0.70), $ty)
    ))
    $shadow = New-Object Drawing.SolidBrush ([Drawing.Color]::FromArgb(110, 0, 0, 0))
    $g.TranslateTransform((3*$u), (3*$u)); $g.FillPath($shadow, $tri); $g.ResetTransform()
    $g.FillPath([Drawing.Brushes]::White, $tri)

    # 테두리 하이라이트
    $edge = New-Object Drawing.Pen ([Drawing.Color]::FromArgb(70, 200, 200, 255)), ([single]([Math]::Max(1.0, 3 * $u)))
    $g.DrawPath($edge, $path)

    foreach ($d in $g, $bg, $shadow, $edge, $path, $tri) { $d.Dispose() }
    return $bmp
}

# 프레임은 DIB(BITMAPINFOHEADER + 32bpp BGRA + AND 마스크)로 담는다. PNG 프레임도 Windows 는
# 읽지만, PyInstaller 등 .ico 를 직접 파싱하는 도구와 System.Drawing.Icon 이 못 읽는 경우가 있다.
function Get-DibBytes([Drawing.Bitmap]$bmp) {
    $w = $bmp.Width; $h = $bmp.Height
    $ms = New-Object IO.MemoryStream
    $bw = New-Object IO.BinaryWriter $ms
    $maskStride = [int]([Math]::Floor(($w + 31) / 32)) * 4
    $bw.Write([uint32]40); $bw.Write([int32]$w); $bw.Write([int32]($h * 2))   # 높이는 색 + 마스크
    $bw.Write([uint16]1); $bw.Write([uint16]32); $bw.Write([uint32]0)
    $bw.Write([uint32]($w * $h * 4 + $maskStride * $h))
    $bw.Write([uint32]0); $bw.Write([uint32]0); $bw.Write([uint32]0); $bw.Write([uint32]0)
    $rect = New-Object Drawing.Rectangle 0, 0, $w, $h
    $data = $bmp.LockBits($rect, [Drawing.Imaging.ImageLockMode]::ReadOnly, [Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $row = New-Object byte[] ($w * 4)
    for ($y = $h - 1; $y -ge 0; $y--) {    # DIB 는 아래에서 위로
        [Runtime.InteropServices.Marshal]::Copy(([IntPtr]($data.Scan0.ToInt64() + $y * $data.Stride)), $row, 0, $row.Length)
        $bw.Write($row)
    }
    $bmp.UnlockBits($data)
    $bw.Write((New-Object byte[] ($maskStride * $h)))   # AND 마스크는 전부 0 (알파를 쓴다)
    $bw.Flush()
    $bytes = $ms.ToArray()
    $bw.Dispose(); $ms.Dispose()
    return ,$bytes
}

$sizes = 16, 20, 24, 32, 40, 48, 64, 128, 256
$pngs = @()
foreach ($s in $sizes) {
    $bmp = New-Frame $s
    $pngs += , @{ Size = $s; Bytes = (Get-DibBytes $bmp) }
    $bmp.Dispose()
}

$out = New-Object IO.MemoryStream
$bw = New-Object IO.BinaryWriter $out
$bw.Write([uint16]0); $bw.Write([uint16]1); $bw.Write([uint16]$pngs.Count)
$offset = 6 + 16 * $pngs.Count
foreach ($p in $pngs) {
    $dim = $p.Size; if ($dim -ge 256) { $dim = 0 }
    $bw.Write([byte]$dim); $bw.Write([byte]$dim)
    $bw.Write([byte]0); $bw.Write([byte]0)
    $bw.Write([uint16]1); $bw.Write([uint16]32)
    $bw.Write([uint32]$p.Bytes.Length)
    $bw.Write([uint32]$offset)
    $offset += $p.Bytes.Length
}
foreach ($p in $pngs) { $bw.Write($p.Bytes) }
$bw.Flush()
$dest = Join-Path $PSScriptRoot 'Tridef3D_Play.ico'
[IO.File]::WriteAllBytes($dest, $out.ToArray())
$bw.Dispose(); $out.Dispose()
Write-Host "wrote $dest ($((Get-Item $dest).Length) bytes, $($pngs.Count) frames)"
