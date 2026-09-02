# patch_deck_p5.ps1 — 手作りの紹介資料（10 枚）の 5 枚目「性能 ── フレームの実測」を埋める。
#
#   使い方: powershell -ExecutionPolicy Bypass -File portfolio\patch_deck_p5.ps1 `
#             -Deck "C:\Users\hiro1\Downloads\acousticflow_portfolio.pptx"
#
# ★このスライドは題名と枠だけで中身が空だった。ほかの 9 枚と同じ作りで埋める。
#
# ★数値の出どころは acousticflow_portfolio.html の「性能 ── フレームの実測」ページ。
#   ⚠ 時間の数字は測るたびに 5〜15% 振れるので、**範囲か概数**で書く。
#     3 桁で書くと再現しないので、資料側の書き方と揃える。
#
# ★このファイルは UTF-8 BOM 付きで保存すること（PS 5.1 は BOM 無しを ANSI と読む）。
#
# ⚠ .pptx の中の XML は **必ず [System.IO.File]::ReadAllText(..., UTF8) で読む**。
#   Get-Content は ANSI と読んで日本語を壊す（元に戻せない）。
param(
  [string]$Deck = "$env:USERPROFILE\Downloads\acousticflow_portfolio.pptx",
  [string]$Out  = "",          # 省略すると 〜_p5.pptx
  [switch]$FixCover            # 表紙の背景 PNG から右下のオレンジを消す
  ,[string]$PngDir = ""      # 指定すると 1・5 枚目を PNG に出す
)
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.IO.Compression.FileSystem

# ── 既存 9 枚から採った作り ───────────────────────────────────────
# 1pt = 12700 EMU。スライドは 12192000 x 6858000（16:9）。
$C = @{
  ink    = "1A1F2B"   # 見出し
  body   = "2B3240"   # 本文
  muted  = "6B7280"   # 補足
  ghost  = "9AA3B2"   # 図の説明・矢印
  card   = "F7F8FA"   # カードの地
  line   = "E5E7EB"   # カードの罫
  track  = "EEF1F5"   # 棒の下地
  dark   = "151A24"   # 暗い囲みの地
  onDark = "EEF2F8"   # 暗い囲みの本文
  onDarkK= "5FE0DC"   # 暗い囲みの強調
  teal   = "16C1C1"   # 数字・1 番目
  blue   = "3AA8D8"   # 2 番目
  indigo = "5B5BD6"   # 3 番目（このページの色）
  amber  = "D99022"   # 4 番目・最悪値
}

$script:sid = 300      # 追加する図形の id（既存とぶつからない値から）
function NextId { $script:sid++; return $script:sid }

function Esc([string]$s) { $s.Replace("&","&amp;").Replace("<","&lt;").Replace(">","&gt;") }

# 塗りつぶしの図形（角丸 or 直角）。$ln に色を渡すと枠線が付く。
function Shape([string]$prst, [int64]$x, [int64]$y, [int64]$w, [int64]$h,
               [string]$fill, [string]$ln = "", [int]$adj = 3438) {
  $id = NextId
  $geom = if ($prst -eq "roundRect") {
    "<a:prstGeom prst=`"roundRect`"><a:avLst><a:gd name=`"adj`" fmla=`"val $adj`"/></a:avLst></a:prstGeom>"
  } else {
    "<a:prstGeom prst=`"rect`"><a:avLst/></a:prstGeom>"
  }
  $f = if ($fill) { "<a:solidFill><a:srgbClr val=`"$fill`"/></a:solidFill>" } else { "<a:noFill/>" }
  $l = if ($ln)   { "<a:ln><a:solidFill><a:srgbClr val=`"$ln`"/></a:solidFill></a:ln>" } else { "<a:ln><a:noFill/></a:ln>" }
  return @"
<p:sp><p:nvSpPr><p:cNvPr id="$id" name="Shape $id"/><p:cNvSpPr/><p:nvPr/></p:nvSpPr>
<p:spPr><a:xfrm><a:off x="$x" y="$y"/><a:ext cx="$w" cy="$h"/></a:xfrm>$geom$f$l<a:effectLst/></p:spPr>
<p:txBody><a:bodyPr wrap="none" rtlCol="0" anchor="ctr"/><a:lstStyle/><a:p><a:pPr algn="ctr"/><a:endParaRPr/></a:p></p:txBody></p:sp>
"@
}

# 1 行ぶんの文字。$runs は @(@(文字, 大きさ(1/100pt), 太字 0/1, 色), ...)。
#   ⚠ ほかの 9 枚と同じく **折り返さない**（wrap="none"）。
#     つまり**改行はこちらで決める**。1 行に詰めすぎるとカードから出る。
function Line([int64]$x, [int64]$y, [int]$lnSpc, $runs) {
  $id = NextId
  # ⚠ PowerShell は **1 個だけの入れ子配列をほどく**。@(@("文字",1100,1,"FFFFFF")) は
  #   @("文字",1100,1,"FFFFFF") になり、$r[0] が文字列の 1 文字目を返す。
  #   その結果 sz="コ" のような属性が出来て、**XML としては通るのに PowerPoint が開けなくなる**。
  #   （実際これで 10 枚全部が開けなくなった。XML 検査も zip 検査も素通りする型。）
  if ($runs[0] -isnot [System.Array]) { $runs = @(,$runs) }
  $rs = ""
  foreach ($r in $runs) {
    $t = Esc $r[0]
    $rs += "<a:r><a:rPr lang=`"ja-JP`" altLang=`"en-US`" sz=`"$($r[1])`" b=`"$($r[2])`" dirty=`"0`">" +
           "<a:solidFill><a:srgbClr val=`"$($r[3])`"/></a:solidFill>" +
           "<a:latin typeface=`"Yu Gothic UI`"/><a:ea typeface=`"Yu Gothic UI`"/><a:cs typeface=`"Yu Gothic UI`"/>" +
           "</a:rPr><a:t xml:space=`"preserve`">$t</a:t></a:r>"
  }
  # 幅と高さは PowerPoint が中身に合わせ直す（wrap="none" なので大きめで良い）。
  $h = [int64]([math]::Round($lnSpc * 12700 / 100.0 * 1.05))
  return @"
<p:sp><p:nvSpPr><p:cNvPr id="$id" name="TextBox $id"/><p:cNvSpPr txBox="1"/><p:nvPr/></p:nvSpPr>
<p:spPr><a:xfrm><a:off x="$x" y="$y"/><a:ext cx="9000000" cy="$h"/></a:xfrm><a:prstGeom prst="rect"><a:avLst/></a:prstGeom><a:noFill/></p:spPr>
<p:txBody><a:bodyPr wrap="none" lIns="0" tIns="0" rIns="0" bIns="0" anchor="t"/><a:lstStyle/>
<a:p><a:pPr algn="l"><a:lnSpc><a:spcPts val="$lnSpc"/></a:lnSpc><a:spcBef><a:spcPts val="0"/></a:spcBef><a:spcAft><a:spcPts val="0"/></a:spcAft></a:pPr>$rs</a:p></p:txBody></p:sp>
"@
}

$xml = ""

# ── 上の帯（暗い囲み）── 既にある空の帯に文字を入れる ──────────────
$xml += Line 870793 1357368 1590 @(
  @("60fps の 1 フレーム 16.7 ms のうち、音響が使うのは ", 1100, 0, $C.onDark),
  @("平均 2.0〜2.5 ms（12〜15 %）。", 1100, 1, $C.onDarkK))
$xml += Line 870793 1605018 1590 @(
  @("重い処理は削らずに、共有と並列で減らしました。並列にしても結果は直列とビット一致です。", 1100, 1, "FFFFFF"))

# ── 左上のカード: 1 フレームの予算に対する取り分 ─────────────────
#   ★このページの主役。「16.7 ms のうちどれだけ使うか」を**長さで**見せる。
$xml += Shape "roundRect" 581025 2219325 6286500 1930000 $C.card $C.line
$xml += Line 790575 2390000 1520 @(@("60fps の 1 フレーム（16.7 ms）のうち、音響が使う量", 1050, 1, $C.ink))
$xml += Shape "rect" 790575 2690000 5867400 9525 $C.line

$trX = 1500000; $trW = 4400000; $trH = 230000
# 平均 13 %
$xml += Line 790575 2812000 1370 @(@("平均", 950, 1, $C.body))
$xml += Shape "roundRect" $trX 2790000 $trW $trH $C.track "" 19313
$xml += Shape "roundRect" $trX 2790000 572000 $trH $C.teal "" 19313
$xml += Line 2160000 2822000 1300 @(@("2.0〜2.5 ms", 900, 1, $C.body))
$xml += Line 6060000 2812000 1370 @(@("15 %", 950, 1, $C.teal))
# 最悪 47 %
$xml += Line 790575 3132000 1370 @(@("最悪", 950, 1, $C.body))
$xml += Shape "roundRect" $trX 3110000 $trW $trH $C.track "" 19313
$xml += Shape "roundRect" $trX 3110000 2068000 $trH $C.amber "" 19313
$xml += Line 3660000 3142000 1300 @(@("5.7〜7.8 ms", 900, 1, $C.body))
$xml += Line 6060000 3132000 1370 @(@("47 %", 950, 1, $C.amber))
# 目盛り
$xml += Line 1500000 3400000 1160 @(@("0", 800, 0, $C.muted))
$xml += Line 4300000 3400000 1160 @(@("16.7 ms ＝ 1 フレームの予算", 800, 0, $C.muted))
# 計測条件（言い訳ではなく、読む人が再現できるように条件を置く）
$xml += Line 790575 3690000 1230 @(@("計測条件 ── 4 コア（ホスト既定）・音源 12 本・Unity 実機。1 コアでも平均 4.4〜4.9 ms（26〜29 %）。", 850, 0, $C.muted))
$xml += Line 790575 3880000 1230 @(@("音声スレッドは別勘定で、実機 45 %（改善前は 75 %）。", 850, 0, $C.muted))

# ── 左下の 3 つの数字 ────────────────────────────────────────────
$chipY = 4300000; $chipH = 581025; $chipW = 2050000; $chipGap = 68250
$chips = @(
  @("2.6 ms", "残響・音源 16 本（前は 16.8）"),
  @("0.32 ms", "畳み込み・共有バス（前は 1.7）"),
  @("2.1 倍", "4 コア並列での速度（1 コア比）")
)
for ($i = 0; $i -lt 3; $i++) {
  $cx = 581025 + (($chipW + $chipGap) * $i)
  $xml += Shape "roundRect" $cx $chipY $chipW $chipH $C.card $C.line
  $xml += Line ($cx + 150000) ($chipY + 27623) 2020 @(@($chips[$i][0], 1500, 1, $C.teal))
  $xml += Line ($cx + 150000) ($chipY + 326954) 1160 @(@($chips[$i][1], 800, 0, $C.muted))
}

# ── 左下の暗い囲み: 何を守って速くしたか ─────────────────────────
#   ★読む人が知りたいのは「速い」ではなく「速くするために何を捨てたか」。
#     捨てていない、と言えるのがこのシステムの主張。
$xml += Shape "roundRect" 581025 5252550 6286500 866775 $C.dark
$xml += Line 739677 5377891 1290 @(
  @("速くするために、音の作りは落としていません。", 950, 1, $C.onDarkK),
  @(" 残響も回折も", 950, 0, $C.onDark))
$xml += Line 739677 5577916 1290 @(@("実行時に解いたままで、音源ごとに重複していた計算だけを共有で消しました。", 950, 0, $C.onDark))
$xml += Line 739677 5777941 1290 @(@("予算に収まったのは、焼いたからではなく無駄を削ったからです。", 950, 0, $C.onDark))

# ── 右のパネル: どこが重くて、どう減らしたか ─────────────────────
$pX = 7158037; $pW = 4457701
$xml += Shape "roundRect" $pX 2219325 $pW 3900000 $C.card $C.line
$xml += Line 7359551 2376543 1590 @(
  @("重い順に測る", 1100, 1, $C.teal),
  @(" → ", 1100, 0, $C.ghost),
  @("打つ手を選ぶ", 1100, 1, $C.indigo))
$xml += Shape "rect" 7362825 2714625 4048125 9525 $C.line

$items = @(
  @{ y = 2830000; c = $C.teal;   h = "残響が全体の約半分だった";
     b = @("音源ごとに計算していた尾を、部屋の単位で共有。",
           "音源 16 本で 16.8 → 2.6 ms（約 6 分の 1）。") },
  @{ y = 3480000; c = $C.blue;   h = "音源ごとに複数コアへ割る";
     b = @("1 コア 4.7 ms → 4 コア 2.2 ms（2.1 倍）。") },
  @{ y = 3970000; c = $C.indigo; h = "畳み込みを共有バスへ";
     b = @("音源ごとに畳む 1.7 ms → 0.32 ms。") },
  @{ y = 4460000; c = $C.amber;  h = "解く深さを音源ごとに変える";
     b = @("厳密・簡易・バーチャルを実行時に指定できます。") }
)
foreach ($it in $items) {
  $xml += Shape "rect" 7362825 ([int64]$it.y + 91567) 104775 104775 $it.c
  $xml += Line 7575352 $it.y 1450 @(@($it.h, 1050, 1, $C.ink))
  for ($k = 0; $k -lt $it.b.Count; $k++) {
    $xml += Line 7575352 ([int64]$it.y + 234823 + (190500 * $k)) 1230 @(@($it.b[$k], 900, 0, $C.body))
  }
}

# パネル下の暗い囲み: 速くしても音が変わっていないことの担保
$xml += Shape "roundRect" 7362824 5050000 4048126 866775 $C.dark
$xml += Line 7521476 5175341 1290 @(
  @("速くしても音は変わっていません。", 950, 1, $C.onDarkK),
  @(" 並列と直列、", 950, 0, $C.onDark))
$xml += Line 7521476 5375366 1290 @(@("共有バスと個別の畳み込みが一致することを、", 950, 0, $C.onDark))
$xml += Line 7521476 5575391 1290 @(@("回帰テストで毎回確かめています（−123.5 dB）。", 950, 0, $C.onDark))

# ── 表紙の右下のオレンジを消す ───────────────────────────────────
#
# ★オレンジは図形ではなく、**表紙の背景 PNG（image1.png）に焼き込まれていた**。
#   PowerPoint 上では選べないので、画像そのものを塗り直す。
#   地は滑らかな暗紺のグラデなので、**囲みの左右の外側の色を行ごとに線形でつなぐ**だけで
#   継ぎ目は見えない。
function RemoveOrange([byte[]]$png) {
  Add-Type -AssemblyName System.Drawing
  $ms = New-Object System.IO.MemoryStream(,$png)
  $bmp = New-Object System.Drawing.Bitmap($ms)
  # まずオレンジの範囲を測る（位置を決め打ちにしない）。
  $minX = 99999; $minY = 99999; $maxX = -1; $maxY = -1
  for ($y = 0; $y -lt $bmp.Height; $y++) {
    for ($x = 0; $x -lt $bmp.Width; $x++) {
      $c = $bmp.GetPixel($x, $y)
      if ($c.R -gt 120 -and $c.R -gt ($c.G * 1.5) -and $c.R -gt ($c.B * 1.5)) {
        if ($x -lt $minX) { $minX = $x }; if ($x -gt $maxX) { $maxX = $x }
        if ($y -lt $minY) { $minY = $y }; if ($y -gt $maxY) { $maxY = $y }
      }
    }
  }
  if ($maxX -lt 0) { $bmp.Dispose(); $ms.Dispose(); return $png }   # 既に消えている
  $x0 = [math]::Max(1, $minX - 6); $x1 = [math]::Min($bmp.Width - 2,  $maxX + 6)
  $y0 = [math]::Max(0, $minY - 6); $y1 = [math]::Min($bmp.Height - 1, $maxY + 6)
  $L = $x0 - 1; $R = $x1 + 1
  for ($y = $y0; $y -le $y1; $y++) {
    $cl = $bmp.GetPixel($L, $y); $cr = $bmp.GetPixel($R, $y)
    for ($x = $x0; $x -le $x1; $x++) {
      $t = ($x - $L) / ($R - $L)
      $bmp.SetPixel($x, $y, [System.Drawing.Color]::FromArgb(255,
        [int][math]::Round($cl.R + ($cr.R - $cl.R) * $t),
        [int][math]::Round($cl.G + ($cr.G - $cl.G) * $t),
        [int][math]::Round($cl.B + ($cr.B - $cl.B) * $t)))
    }
  }
  $o = New-Object System.IO.MemoryStream
  $bmp.Save($o, [System.Drawing.Imaging.ImageFormat]::Png)
  $bmp.Dispose(); $ms.Dispose()
  Write-Host ("表紙のオレンジを消しました: x=$minX..$maxX y=$minY..$maxY")
  return $o.ToArray()
}

# ── .pptx を組み直す ─────────────────────────────────────────────
#
# ⚠ 元のファイルを「Update」で開いてはいけない。
#   PowerPoint で開いたままだと**書き込みロックで失敗する**（実際そうなった）。
#   → 元は**読むだけ**にして、**別のファイルへ書き出す**。読むだけなら開いていても通る。
if (-not (Test-Path $Deck)) { throw "資料が見つかりません: $Deck" }
if (-not $Out) {
  $dir  = Split-Path -Parent $Deck
  $base = [System.IO.Path]::GetFileNameWithoutExtension($Deck)
  $Out  = Join-Path $dir ($base + "_p5.pptx")
}
if ((Resolve-Path $Deck).Path -eq $Out) { throw "出力先が元と同じです" }
if (Test-Path $Out) { Remove-Item $Out -Force }

$src = [System.IO.Compression.ZipFile]::OpenRead($Deck)
$dst = [System.IO.Compression.ZipFile]::Open($Out, "Create")
try {
  $done = $false
  foreach ($e in $src.Entries) {
    $ne = $dst.CreateEntry($e.FullName)
    $inS = $e.Open()
    if ($e.FullName -eq "ppt/slides/slide5.xml") {
      $sr = New-Object System.IO.StreamReader($inS, [System.Text.Encoding]::UTF8)
      $cur = $sr.ReadToEnd(); $sr.Close()
      # ⚠ 二度がけを防ぐ。追加した図形には Shape 3xx / TextBox 3xx の名前が付く。
      if ($cur -match 'name="(Shape|TextBox) 3\d\d"') { throw "5 枚目には既に中身が入っています" }
      $new = $cur -replace '</p:spTree>', ($xml + '</p:spTree>')
      if ($new -eq $cur) { throw "差し込み位置が見つかりませんでした" }
      $sw = New-Object System.IO.StreamWriter($ne.Open(), (New-Object System.Text.UTF8Encoding($false)))
      $sw.Write($new); $sw.Flush(); $sw.Close()
      $done = $true
    } elseif ($FixCover -and $e.FullName -eq "ppt/media/image1.png") {
      $buf = New-Object System.IO.MemoryStream
      $inS.CopyTo($buf)
      $fixed = RemoveOrange $buf.ToArray()
      $outS = $ne.Open()
      $outS.Write($fixed, 0, $fixed.Length)
      $outS.Close()
    } else {
      $outS = $ne.Open()
      $inS.CopyTo($outS)
      $outS.Close()
    }
    $inS.Close()
  }
  if (-not $done) { throw "ppt/slides/slide5.xml がありません" }
} finally {
  $dst.Dispose(); $src.Dispose()
}
Write-Host ("差し込んだ図形: " + ([regex]::Matches($xml, '<p:sp>')).Count + " 個")
Write-Host ("出力 : " + $Out)

# ── 開けるか確かめる ─────────────────────────────────────────────
#
# ★ここを省いてはいけない。壊れ方によっては **XML としては正しく、zip も正しいのに
#   PowerPoint だけが開けない**（属性の中身が数値でない等）。本物に開かせるのが唯一の検査。
$ppt = New-Object -ComObject PowerPoint.Application
$ppt.Visible = -1
try {
  $p = $ppt.Presentations.Open($Out, $false, $false, $false)
  $sl = $p.Slides.Item(5)
  $over = 0
  foreach ($sh in $sl.Shapes) {
    if (($sh.Top + $sh.Height) -gt 540 -or $sh.Left -lt 0 -or $sh.Top -lt 0) { $over++ }
  }
  Write-Host ("検査 : 開けました（{0} 枚）／5 枚目の図形 {1} 個／下または左上へはみ出し {2} 個" -f $p.Slides.Count, $sl.Shapes.Count, $over) -ForegroundColor Green
  if ($PngDir) {
    if (-not (Test-Path $PngDir)) { New-Item -ItemType Directory $PngDir | Out-Null }
    foreach ($i in 1, 5) { $p.Slides.Item($i).Export((Join-Path $PngDir ("s{0:D2}.png" -f $i)), "PNG", 1600, 900) }
    Write-Host ("画に出しました: " + $PngDir)
  }
  $p.Close()
} catch {
  Write-Host ("★開けません: " + $_.Exception.Message) -ForegroundColor Red
  throw
}
