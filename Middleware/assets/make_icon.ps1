# make_icon.ps1 — icon.svg（または任意の PNG）から icon.ico を作る。
#
#   使い方:
#     powershell -ExecutionPolicy Bypass -File Middleware\assets\make_icon.ps1
#     powershell -ExecutionPolicy Bypass -File Middleware\assets\make_icon.ps1 -Source 好きな絵.png
#
# ★なぜ自前で組み立てるか
#   .NET には「複数寸法の .ico を書く」口がありません（Icon.Save は読み込んだものを
#   そのまま吐くだけ）。ImageMagick も入っていないので、ICO の容器を直に書きます。
#   仕様は単純で、依存を 1 本も増やさずに済みます。
#
# ★寸法ごとの入れ方
#   256 だけ PNG 圧縮、それ以下は 32bit の DIB。
#   PNG 入りは Vista 以降なら全寸法で読めますが、**古い経路（一部のダイアログや
#   リモート デスクトップ）で 16/32 が出ないことがある**ので、小さい側は素の DIB にします。
#
# ★このファイルは UTF-8 BOM 付きで保存すること（PS 5.1 は BOM 無しを ANSI と読む）。

param(
    [string]$Source = "",
    [string]$Out    = ""
)
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing

$here = Split-Path -Parent $MyInvocation.MyCommand.Path
if ($Out -eq "") { $Out = Join-Path $here "icon.ico" }

# ── 1. 元絵を 256x256 の PNG にする
$src256 = ""
if ($Source -ne "") {
    if (-not (Test-Path $Source)) { throw "元絵が見つかりません: $Source" }
    $src256 = (Resolve-Path $Source).Path
} else {
    # SVG を Edge のヘッドレスで 256x256 に焼く。
    # （build.ps1 が PDF を作っているのと同じ手口。この環境には他に手が無い）
    $svg = Join-Path $here "icon.svg"
    if (-not (Test-Path $svg)) { throw "icon.svg がありません" }

    $html = Join-Path $env:TEMP "af_icon.html"
    $uri  = ([System.Uri]$svg).AbsoluteUri
    @"
<!doctype html><meta charset="utf-8">
<style>html,body{margin:0;padding:0;background:transparent;overflow:hidden}
img{display:block;width:256px;height:256px}</style>
<img src="$uri">
"@ | Out-File -FilePath $html -Encoding utf8

    $edge = "C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe"
    if (-not (Test-Path $edge)) { $edge = "C:\Program Files\Google\Chrome\Application\chrome.exe" }
    if (-not (Test-Path $edge)) { throw "Edge も Chrome も見つかりません" }

    $src256 = Join-Path $env:TEMP "af_icon_256.png"
    if (Test-Path $src256) { Remove-Item $src256 }

    # ★stderr をリダイレクトしない（PS 5.1 が無害な警告を失敗に化けさせるため）。
    $prof = Join-Path $env:TEMP "af_icon_profile"
    & $edge --headless --disable-gpu "--user-data-dir=$prof" --window-size=256,256 `
            --default-background-color=00000000 --virtual-time-budget=5000 `
            "--screenshot=$src256" ([System.Uri]$html).AbsoluteUri | Out-Null

    if (-not (Test-Path $src256)) { throw "SVG を PNG に焼けませんでした" }
}

$base = [System.Drawing.Image]::FromFile($src256)

# ── 2. 各寸法へ縮小
function Resize([System.Drawing.Image]$img, [int]$n) {
    $bmp = New-Object System.Drawing.Bitmap $n, $n
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.InterpolationMode  = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $g.PixelOffsetMode    = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $g.SmoothingMode      = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
    $g.CompositingQuality = [System.Drawing.Drawing2D.CompositingQuality]::HighQuality
    $g.DrawImage($img, (New-Object System.Drawing.Rectangle 0, 0, $n, $n))
    $g.Dispose()
    return $bmp
}

# 32bit DIB（BITMAPINFOHEADER + BGRA の XOR + 1bit の AND）を作る。
# ⚠ DIB は**下から上**に並べる。ここを間違えると絵が上下逆さまに出る。
function ToDib([System.Drawing.Bitmap]$bmp) {
    $n = $bmp.Width
    $ms = New-Object System.IO.MemoryStream
    $bw = New-Object System.IO.BinaryWriter $ms

    $bw.Write([uint32]40)          # biSize
    $bw.Write([int32]$n)           # biWidth
    $bw.Write([int32]($n * 2))     # biHeight … XOR と AND を足した高さ（仕様）
    $bw.Write([uint16]1)           # biPlanes
    $bw.Write([uint16]32)          # biBitCount
    $bw.Write([uint32]0)           # biCompression = BI_RGB
    $bw.Write([uint32]0)           # biSizeImage
    0..3 | ForEach-Object { $bw.Write([int32]0) }   # 解像度・色数

    for ($y = $n - 1; $y -ge 0; $y--) {
        for ($x = 0; $x -lt $n; $x++) {
            $c = $bmp.GetPixel($x, $y)
            $bw.Write([byte]$c.B); $bw.Write([byte]$c.G)
            $bw.Write([byte]$c.R); $bw.Write([byte]$c.A)
        }
    }

    # AND マスク。32bit なら中身は使われないが、**行を 4 バイト境界に揃えて置く**必要がある。
    $rowBytes = [math]::Ceiling($n / 32) * 4
    for ($y = 0; $y -lt $n; $y++) { $bw.Write((New-Object byte[] $rowBytes)) }

    $bw.Flush()
    # ★先頭のカンマが要る。PowerShell は配列を return するとばらして返すので、
    #   これが無いと byte[] が object[] に化けて BinaryWriter が 1 バイトも書かない
    #   （実際、最初は 102 バイト＝ヘッダだけの .ico が出来た）。
    return ,$ms.ToArray()
}

function ToPng([System.Drawing.Bitmap]$bmp) {
    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    return ,$ms.ToArray()
}

$sizes   = @(256, 128, 64, 48, 32, 16)
$entries = @()
foreach ($n in $sizes) {
    $bmp = Resize $base $n
    [byte[]]$bytes = if ($n -ge 128) { ToPng $bmp } else { ToDib $bmp }
    $entries += [pscustomobject]@{ Size = $n; Bytes = $bytes }
    $bmp.Dispose()
}
$base.Dispose()

# ── 3. ICO の容器に詰める
$fs = [System.IO.File]::Create($Out)
$bw = New-Object System.IO.BinaryWriter $fs

$bw.Write([uint16]0)                 # 予約
$bw.Write([uint16]1)                 # 種別 1 = アイコン
$bw.Write([uint16]$entries.Count)

$offset = 6 + 16 * $entries.Count
foreach ($e in $entries) {
    # 256 は 0 と書く決まり（1 バイトに収まらないため）
    $bw.Write([byte]($(if ($e.Size -ge 256) { 0 } else { $e.Size })))
    $bw.Write([byte]($(if ($e.Size -ge 256) { 0 } else { $e.Size })))
    $bw.Write([byte]0)               # 色数
    $bw.Write([byte]0)               # 予約
    $bw.Write([uint16]1)             # プレーン
    $bw.Write([uint16]32)            # ビット数
    $bw.Write([uint32]$e.Bytes.Length)
    $bw.Write([uint32]$offset)
    $offset += $e.Bytes.Length
}
foreach ($e in $entries) { $bw.Write($e.Bytes) }

$bw.Flush(); $fs.Close()

Write-Host ""
Write-Host "出力 : $Out"
Write-Host "寸法 : $($sizes -join ', ')"
Write-Host "大きさ: $([math]::Round((Get-Item $Out).Length / 1KB, 1)) KB"

# ── 見張り: 書いたものを読み返して、各寸法が本当に取り出せるか確かめる。
#    （容器の詰め方を間違えても、ファイルは出来てしまう。実際、最初は
#      102 バイトのヘッダだけの .ico が「成功」として出てきた）
#
# ⚠ 256 だけ別のやり方で見る。
#   `System.Drawing.Icon` は **256px の PNG 入りを読めず、次に小さい絵を返す**ので、
#   これで判定すると「NG 256 → 128」と出る。ファイルは正しいのに見張りが嘘をつく。
#   なので 256 は容器の目録を自分で読んで、中身が 256x256 の PNG かを直接確かめる。
$ok = $true
$raw = [System.IO.File]::ReadAllBytes($Out)
$count = [BitConverter]::ToUInt16($raw, 4)

foreach ($n in $sizes) {
    if ($n -lt 256) {
        try {
            $i = New-Object System.Drawing.Icon($Out, $n, $n)
            if ($i.Width -ne $n) {
                Write-Host "  NG $n → $($i.Width)" -ForegroundColor Red; $ok = $false
            }
            $i.Dispose()
        } catch {
            Write-Host "  NG $n ── $($_.Exception.Message)" -ForegroundColor Red; $ok = $false
        }
        continue
    }

    # 目録から「幅 0（＝256 の意）」の項を探して、その中身を絵として開く。
    $found = $false
    for ($k = 0; $k -lt $count; $k++) {
        $e = 6 + 16 * $k
        if ($raw[$e] -ne 0) { continue }
        $len = [BitConverter]::ToUInt32($raw, $e + 8)
        $off = [BitConverter]::ToUInt32($raw, $e + 12)
        $buf = New-Object byte[] $len
        [Array]::Copy($raw, $off, $buf, 0, $len)
        $ms = New-Object System.IO.MemoryStream(, $buf)
        try {
            $img = [System.Drawing.Image]::FromStream($ms)
            if ($img.Width -eq 256 -and $img.Height -eq 256) { $found = $true }
            else { Write-Host "  NG 256 → $($img.Width)x$($img.Height)" -ForegroundColor Red }
            $img.Dispose()
        } catch {
            Write-Host "  NG 256 ── 中身を絵として開けません" -ForegroundColor Red
        }
        $ms.Dispose()
        break
    }
    if (-not $found) { Write-Host "  NG 256 ── 目録に項がありません" -ForegroundColor Red; $ok = $false }
}

if ($ok) { Write-Host "見張り: 全寸法を読み返せました" -ForegroundColor Green }
else     { Write-Host "見張り: ★読み返せない寸法があります" -ForegroundColor Red; exit 1 }
