# build_pptx.ps1 — 紹介資料の HTML から PowerPoint 版（.pptx）を作る。
#
#   使い方:  powershell -ExecutionPolicy Bypass -File portfolio\build_pptx.ps1
#
# ★なぜ HTML から自動で作るのか
#   18 ページぶんの文章を手で写すと、必ずどこかがずれます。**HTML を唯一の原本**にして、
#   PowerPoint 版はそこから機械的に組み立てます。文章を直すときは HTML を直して、
#   これをもう一度回してください。
#
# ★なぜ PowerPoint を COM で動かすのか
#   この開発機には Node も Python も LibreOffice もありません（Python は WindowsApps の
#   スタブ）。一方 PowerPoint 本体は入っています。**本物の PowerPoint に作らせる**のが、
#   手書きの OOXML より確実です（壊れたファイルが出来ない）。
#
# ★このファイルは UTF-8 BOM 付きで保存すること（PS 5.1 は BOM 無しを ANSI と読む）。
#
# ★向き
#   既定は **16:9 横**（画面・プロジェクタ用）。-Portrait を付けると A4 縦（PDF と同じ体裁）。
#
#   ⚠ どちらでも **1 ページ = 1 スライド**を崩しません。本文が「p10 参照」のように
#     ページ番号を参照しているので、分割すると全部ずれます。
#   ⚠ 16:9 では 2 段組みを **3 段**へ詰め直します。面積はほぼ同じ（縦 2×640×247 ≒
#     横 3×370×280）なので文字は小さくなりませんが、**「左が本文・右が補足」という
#     意味のまとまりは崩れます**。そこは目で直す前提です。
#
# ⚠ 実行中は PowerPoint が立ち上がります。作業中の PowerPoint は閉じてから回してください。
param([switch]$Portrait)
$ErrorActionPreference = "Stop"

$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$html = Join-Path $here "acousticflow_portfolio.html"
$out  = Join-Path $here $(if ($Portrait) { "acousticflow_portfolio_a4.pptx" } else { "acousticflow_portfolio.pptx" })
$tmp  = Join-Path $env:TEMP ("af_pptx_" + [System.Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $tmp | Out-Null

# ── 配色（HTML の見た目に合わせた寒色系）────────────────────────────
$PAL = @{
  ink    = "1A2233"   # 本文
  muted  = "6B7688"   # 補足
  deep   = "21295C"   # 見出し
  teal   = "1C7293"   # 強調
  aqua   = "E8F4F7"   # 囲みの地
  warmBg = "FDF1E3"   # 注意の地
  warmLn = "C77C33"
  line   = "D8DEE6"
  white  = "FFFFFF"
}
function RGB([string]$hex) {
  # ⚠ 空や短い値が来たら黙って別の色にせず、**どこが悪いか言って**止める。
  #   静かに色が変わると、出来上がりを見るまで気づけない。
  if (-not $hex -or $hex.Length -lt 6) {
    $cs = (Get-PSCallStack | Select-Object -Skip 1 -First 2 |
           ForEach-Object { "$($_.FunctionName):$($_.ScriptLineNumber)" }) -join " ← "
    throw "RGB に不正な色が渡されました: '[$hex]' 呼び出し元 $cs"
  }
  $r = [Convert]::ToInt32($hex.Substring(0,2),16)
  $g = [Convert]::ToInt32($hex.Substring(2,2),16)
  $b = [Convert]::ToInt32($hex.Substring(4,2),16)
  return $r + ($g * 256) + ($b * 65536)
}

# ── HTML を読む ────────────────────────────────────────────────────
Write-Host "[1/4] HTML を読み込み中..."
$src = [System.IO.File]::ReadAllText($html, [System.Text.Encoding]::UTF8)
$doc = New-Object -ComObject "HTMLFile"
try   { $doc.IHTMLDocument2_write([System.Text.Encoding]::Unicode.GetBytes($src)) }
catch { $doc.write([System.Text.Encoding]::Unicode.GetBytes($src)) }
$pages = @($doc.body.getElementsByClassName("page"))
Write-Host "      $($pages.Count) ページ"

# ページごとの SVG を**生テキスト**から拾っておく（理由は DumpSvg の注記）。
$rawPages = [regex]::Split($src, '(?=<div class="page)')
$svgByPage = @{}
$pi = 0
foreach ($chunk in $rawPages) {
  if ($chunk -notmatch '<div class="page') { continue }
  $svgByPage[$pi] = @([regex]::Matches($chunk, '(?s)<svg\b.*?</svg>') | ForEach-Object { $_.Value })
  $pi++
}
Write-Host ("      図(SVG) " + (($svgByPage.Values | ForEach-Object { $_.Count }) | Measure-Object -Sum).Sum + " 個")

# 要素が持つクラスを調べる小道具。
function HasClass($el, [string]$cls) {
  $c = $el.className
  if (-not $c) { return $false }
  return (" $c " -like "* $cls *")
}

# ── SVG を単体ファイルへ書き出す ──────────────────────────────────
#   ★PowerPoint 2016 以降は SVG をそのまま貼れる（ベクタのまま・図形へ変換も可能）。
#     ラスタ画像にすると編集できなくなるので、SVG のまま渡す。
#
#   ⚠ SVG は **HTMLFile（MSHTML）では読めません。** 古い解析器は SVG を
#     「よその名前空間の要素」として扱えず、outerHTML が**開始タグだけ**（77 文字）に
#     なります。子要素が全部消えるので、そこから作った図は空になります。
#     → **生のテキストから切り出す。** ページごとに分けた原文を正規表現で拾い、
#       出てくる順に使います（HTML は手書きで整形が安定しているので成立します）。
$svgIndex = 0
function DumpSvg([string]$svgText) {
  $script:svgIndex++
  $p = Join-Path $tmp ("fig{0}.svg" -f $script:svgIndex)
  $x = $svgText
  if ($x -notmatch 'xmlns=') { $x = $x -replace '<svg', '<svg xmlns="http://www.w3.org/2000/svg"' }
  # HTML 内では CSS が色を決めていたので、単体で見えるよう最低限の style を足す。
  $style = @'
<style>
.plan .w{fill:#dfe6ee;stroke:#9aa8bb;stroke-width:2}
.plan .dr{fill:#1c7293}
.plan .s{fill:#f0a24a;stroke:#8a5a1e;stroke-width:2}
.plan .sl{fill:#1a2233;font-size:26px;font-family:sans-serif;text-anchor:middle}
.plan .arc{fill:none;stroke:#9aa8bb;stroke-width:2;stroke-dasharray:6 6}
.plan .li{fill:#2f7d32}
.plan .dim{stroke:#6b7688;stroke-width:2}
.plan .dm{fill:#6b7688;font-size:22px;font-family:sans-serif}
text{dominant-baseline:auto}
</style>
'@
  $x = $x -replace '(<svg[^>]*>)', ('$1' + $style)
  [System.IO.File]::WriteAllText($p, $x, (New-Object System.Text.UTF8Encoding($false)))
  return $p
}

# ── PowerPoint を起動 ──────────────────────────────────────────────
Write-Host "[2/4] PowerPoint を起動中..."
$ppt = New-Object -ComObject PowerPoint.Application
$ppt.Visible = -1
$pres = $ppt.Presentations.Add(-1)
$pres.PageSetup.SlideSize = 3            # ppSlideSizeCustom
if ($Portrait) {
  $SW = 595.0; $SH = 842.0; $NCOL0 = 2; $M = 40.0
} else {
  $SW = 960.0; $SH = 540.0; $NCOL0 = 3; $M = 32.0    # 16:9（13.33in × 7.5in）
}
$pres.PageSetup.SlideWidth  = $SW
$pres.PageSetup.SlideHeight = $SH

$GAP  = 20.0
$W    = $SW - ($M * 2)                          # 本文の幅
$COLW = ($W - ($GAP * ($NCOL0 - 1))) / $NCOL0     # 1 段の幅
$BOT  = $SH - $M                                # 段の下端
$FONT = "Yu Gothic UI"

# 入りきらないページだけ、この 2 つを動かして引き直す（ページ組み立ての retry を参照）。
$script:FS   = 1.0        # 文字と余白の倍率
$script:NCOL = $NCOL0      # そのページの段数（詰まったページだけ 4 段にする）
function ColW { ($W - ($GAP * ($script:NCOL - 1))) / $script:NCOL }

# スライド上の図形を名前で覚える／その後に増えたものだけ消す。
#   ⚠ index で消してはいけない。囲みは ZOrder で背面へ回すので**並びが変わり**、
#     別の図形（表など）を消してしまう。実際それで表が 2 つ消えた。
#   ⚠ 返すときは `,` を付ける。付けないと PowerShell が集合を**配列にほどいて**しまい、
#     名前の集合ではなく名前の列が返る（動くが遅い・意図が消える）。
function ShapeNames($sl) {
  $s = New-Object System.Collections.Generic.HashSet[string]
  foreach ($sh in $sl.Shapes) { [void]$s.Add($sh.Name) }
  return ,$s
}
function RemoveNewShapes($sl, $known) {
  $kill = @()
  foreach ($sh in $sl.Shapes) { if (-not $known.Contains($sh.Name)) { $kill += $sh.Name } }
  foreach ($n in $kill) { try { $sl.Shapes.Item($n).Delete() } catch { } }
}

# ── 部品 ────────────────────────────────────────────────────────────
function NewText($sl, [double]$x, [double]$y, [double]$w, [string]$text,
                 [double]$size, [string]$color, [bool]$bold = $false) {
  if (-not $text) { return $y }
  $tb = $sl.Shapes.AddTextbox(1, $x, $y, $w, 20)
  $tf = $tb.TextFrame
  $tf.WordWrap = -1
  $tf.MarginLeft = 0; $tf.MarginRight = 0; $tf.MarginTop = 0; $tf.MarginBottom = 0
  $tr = $tf.TextRange
  $tr.Text = $text
  $tr.Font.Name = $FONT
  $tr.Font.NameFarEast = $FONT
  $tr.Font.Size = $size * $script:FS
  $tr.Font.Bold = $(if ($bold) { -1 } else { 0 })
  $tr.Font.Color.RGB = RGB $color
  $tr.ParagraphFormat.SpaceWithin = 0.95
  $tf.AutoSize = 1                       # 文字に合わせて高さを決める
  return $y + $tb.Height
}

function NewBox($sl, [double]$x, [double]$y, [double]$w, [string]$text,
                [string]$bg, [string]$fg, [double]$size) {
  $pad = 8.0 * $script:FS
  $t = $sl.Shapes.AddTextbox(1, $x + $pad, $y + $pad, $w - ($pad * 2), 20)
  $tf = $t.TextFrame
  $tf.WordWrap = -1
  $tf.MarginLeft = 0; $tf.MarginRight = 0; $tf.MarginTop = 0; $tf.MarginBottom = 0
  $tr = $tf.TextRange
  $tr.Text = $text
  $tr.Font.Name = $FONT; $tr.Font.NameFarEast = $FONT
  $tr.Font.Size = $size * $script:FS
  $tr.Font.Color.RGB = RGB $fg
  $tr.ParagraphFormat.SpaceWithin = 0.95
  $tf.AutoSize = 1
  $h = $t.Height + ($pad * 2)
  $r = $sl.Shapes.AddShape(5, $x, $y, $w, $h)   # 角丸
  $r.Fill.ForeColor.RGB = RGB $bg
  $r.Line.Visible = 0
  $r.Adjustments.Item(1) = 0.06
  $r.ZOrder(1)                                   # 文字の背面へ
  return $y + $h
}

function NewTable($sl, $tblEl, [double]$x, [double]$y, [double]$w) {
  $rows = @($tblEl.getElementsByTagName("tr"))
  if ($rows.Count -eq 0) { return $y }
  $cols = @($rows[0].children).Count
  if ($cols -eq 0) { return $y }
  $sh = $sl.Shapes.AddTable($rows.Count, $cols, $x, $y, $w, 18 * $rows.Count)
  $t = $sh.Table
  for ($r = 0; $r -lt $rows.Count; $r++) {
    $cells = @($rows[$r].children)
    for ($c = 0; $c -lt $cols -and $c -lt $cells.Count; $c++) {
      $cell = $t.Cell($r + 1, $c + 1)
      $tr = $cell.Shape.TextFrame.TextRange
      $tr.Text = ($cells[$c].innerText -replace "\s+", " ").Trim()
      $tr.Font.Name = $FONT; $tr.Font.NameFarEast = $FONT
      $tr.Font.Size = 8.5 * $script:FS
      $isHead = ($cells[$c].tagName -eq "TH")
      $tr.Font.Bold = $(if ($isHead) { -1 } else { 0 })
      $tr.Font.Color.RGB = RGB $(if ($isHead) { $PAL.white } else { $PAL.ink })
      $cell.Shape.Fill.ForeColor.RGB = RGB $(if ($isHead) { $PAL.teal } else { $PAL.white })
      # ★余白も一緒に縮める。文字だけ縮めても、行の高さは余白で頭打ちになる
      #   （18 行の表なら 4pt の差が 72pt になる）。
      $cell.Shape.TextFrame.MarginLeft = 4 * $script:FS
      $cell.Shape.TextFrame.MarginRight = 4 * $script:FS
      $cell.Shape.TextFrame.MarginTop = 2 * $script:FS
      $cell.Shape.TextFrame.MarginBottom = 2 * $script:FS
      $cell.Borders(1).ForeColor.RGB = RGB $PAL.line
      $cell.Borders(2).ForeColor.RGB = RGB $PAL.line
      $cell.Borders(3).ForeColor.RGB = RGB $PAL.line
      $cell.Borders(4).ForeColor.RGB = RGB $PAL.line
    }
  }
  # ★行の高さを一度 1pt へ潰して、PowerPoint に測り直させる。
  #
  #   ⚠ これが無いと表が**桁違いに高くなる**。理由: 新しい表の既定は 18pt で、
  #     こちらは**文字を入れてから**大きさを指定している。行は 18pt の文字に
  #     合わせて伸びたあと、文字を小さくしても**縮まない**（PowerPoint は行を
  #     自動では下げない）。実際 p18 の表が 1429pt になり、段が丸ごと空いていた。
  #   PowerPoint は中身より低くはしないので、1pt を渡せば「必要な高さ」が返る。
  foreach ($r in 1..$rows.Count) { try { $t.Rows.Item($r).Height = 1 } catch { } }
  return $y + $sh.Height
}

# ページの中の 1 ブロックを描く。次の y を返す。
#
# ★ブロック同士の隙間も $script:FS で縮める（$g）。
#   隙間を固定にすると、20 ブロックあるページでは隙間だけで 100pt 以上を占め、
#   文字をいくら縮めても入らなくなる。実際 p18 が 60% でも入らなかった原因。
function DrawBlock($sl, $el, [double]$x, [double]$y, [double]$w) {
  $tag = $el.tagName
  $txt = ($el.innerText -replace "[ \t]+", " ").Trim()
  $g = $script:FS

  if ($tag -eq "TABLE") { return (NewTable $sl $el $x ($y + 4 * $g) $w) + 8 * $g }
  if ($tag -eq "H2")    { return (NewText $sl $x ($y + 8 * $g) $w $txt 12 $PAL.deep $true) + 4 * $g }
  if ((HasClass $el "call") -or (HasClass $el "warnbox")) {
    $bg = $(if (HasClass $el "warnbox") { $PAL.warmBg } else { $PAL.aqua })
    # ⚠ 囲みの中に表が入っていることがある（p6 の鏡像の表）。そのまま文字にすると
    #   **表が文章に潰れて消える**ので、文章と表を分けて描く。
    $inner = @($el.getElementsByTagName("table"))
    if ($inner.Count -gt 0) {
      $rest = $txt
      foreach ($t in $inner) {
        $tt = ($t.innerText -replace "[ \t]+", " ").Trim()
        if ($tt) { $rest = $rest.Replace($tt, "").Trim() }
      }
      $yy = $y + 5 * $g
      if ($rest) { $yy = NewBox $sl $x $yy $w $rest $bg $PAL.ink 9.5 }
      foreach ($t in $inner) { $yy = (NewTable $sl $t $x ($yy + 3 * $g) $w) + 3 * $g }
      return $yy + 5 * $g
    }
    return (NewBox $sl $x ($y + 5 * $g) $w $txt $bg $PAL.ink 9.5) + 5 * $g
  }
  if (HasClass $el "tiny")    { return (NewText $sl $x ($y + 3 * $g) $w $txt 8 $PAL.muted) + 3 * $g }
  if (HasClass $el "ph")      { return (NewBox $sl $x ($y + 5 * $g) $w $txt "F1F3F6" $PAL.muted 9) + 5 * $g }
  # 図。DOM からは中身が取れないので、そのページぶんの生テキストを順に使う。
  $nSvg = @($el.getElementsByTagName("svg")).Count
  if ($nSvg -gt 0) {
    $gap = 6.0
    # ★図も $script:FS で一緒に縮める（縦横比は保つ）。
    #   図だけ大きさを据え置くと、後ろの文章が押し出されて**下から落ちる**。
    #   実際 p8 は文字を 50% にしても、図が 377pt 居座って表が枠外へ出ていた。
    $iw = (($w - ($gap * ($nSvg - 1))) / $nSvg) * $script:FS
    $ih = $iw * 0.7
    # ★縦で頭打ちにする。16:9 は横に広いので、全幅の図は 0.7 倍でも**スライドより高く**なる
    #   （実際 p8 が 896×627pt ＝ 540pt のスライドからはみ出した）。
    #   縦横比は保つので、詰まったら幅の方を縮めて中央へ寄せる。
    # 残りの縦幅を全部は使わせない。図の後ろには必ず説明の文章と表が来るので、
    # 使い切ると**その全部が下から落ちる**（p8 がそれで文字を 50% まで縮めていた）。
    $room = ($BOT - ($y + 5)) * 0.58
    if ($room -gt 40 -and $ih -gt $room) {
      $k = $room / $ih
      $ih = $room
      $iw = $iw * $k
    }
    $span = ($iw * $nSvg) + ($gap * ($nSvg - 1))
    $x0 = $x + (($w - $span) / 2)
    for ($i = 0; $i -lt $nSvg; $i++) {
      if ($script:pageSvgs.Count -le $script:pageSvgCursor) { break }
      $f = DumpSvg $script:pageSvgs[$script:pageSvgCursor]
      $script:pageSvgCursor++
      # ★失敗を握りつぶさない。図が抜けても枚数は変わらないので、黙ると気づけない。
      try { $sl.Shapes.AddPicture($f, 0, -1, $x0 + ($iw + $gap) * $i, $y + 5, $iw, $ih) | Out-Null }
      catch { Write-Host ("      ⚠ 図を貼れませんでした: " + $_.Exception.Message) -ForegroundColor Yellow }
    }
    return $y + $ih + 10
  }
  if ($txt) { return (NewText $sl $x ($y + 3 * $g) $w $txt 9.5 $PAL.ink) + 3 * $g }
  return $y
}

# 段組みの器を受け取り、中身を $NCOL 段へ詰める。次の y を返す。
#
# ★HTML は 2 段だが、16:9 では 3 段に詰め直す。段の数が変わるので、
#   「どのブロックがどの段に来るか」は**引いてみないと分からない**。
#   → **引いてみて、下端を超えたら消して次の段へ引き直す**。
#     高さを事前に測る方法（画面外に描いて測る等）より短くて確実。
#   ⚠ 段が空のときは、超えても受け入れる（1 個で段より高いブロックがあるため。
#     受け入れないと無限に段を送ることになる）。
function RenderCols($sl, $container, [double]$top) {
  $sub = @($container.getElementsByClassName("col"))
  if ($sub.Count -lt 1) { return $top }

  # 読む順にブロックを並べる（左の段 → 右の段）。
  $blocks = @()
  foreach ($c in $sub) { foreach ($cc in @($c.children)) { $blocks += $cc } }
  if ($script:trace) { Write-Host ("      RenderCols top=" + [math]::Round($top) + " blocks=" + $blocks.Count + " ncol=" + $script:NCOL) }

  $cw = ColW
  $ci = 0
  $cy = $top
  $maxY = $top
  foreach ($blk in $blocks) {
    $placed = $false
    while (-not $placed) {
      $cx = $M + (($cw + $GAP) * $ci)
      $before = ShapeNames $sl
      if ($script:trace) { Write-Host ("        blk<" + $blk.tagName + "." + $blk.className + "> ci=" + $ci + " cy=" + [math]::Round($cy)) }
      $ny = DrawBlock $sl $blk $cx $cy $cw
      $empty = ($cy -le $top + 0.5)
      if ($ny -le $BOT -or $empty -or $ci -ge ($script:NCOL - 1)) {
        $cy = $ny
        if ($cy -gt $maxY) { $maxY = $cy }
        $placed = $true
      } else {
        # 入らなかった。引いた図形を取り消して次の段へ。
        RemoveNewShapes $sl $before
        $ci++
        $cy = $top
      }
    }
  }
  return $maxY
}

# ── ページを組む ────────────────────────────────────────────────────
Write-Host "[3/4] スライドを組み立て中..."
$shrunk = @()      # 文字を縮めて入れたページ（最後に報告する）
for ($p = 0; $p -lt $pages.Count; $p++) {
  $page = $pages[$p]
  $sl = $pres.Slides.Add($p + 1, 12)          # ppLayoutBlank
  $sl.FollowMasterBackground = 0
  $sl.Background.Fill.ForeColor.RGB = RGB $PAL.white

  $ttlEl = @($page.getElementsByClassName("tab-ttl"))
  $catEl = @($page.getElementsByClassName("tab-cat"))
  $y = $M
  # このページぶんの図を、出てくる順に使う。
  $script:pageSvgs = @()
  if ($svgByPage.ContainsKey($p)) { $script:pageSvgs = $svgByPage[$p] }
  $script:pageSvgCursor = 0

  if ($p -eq 0) {
    # 表紙は別扱い。3 つのチップは横並びにする（16:9 で縦に積むと間延びする）。
    $topY = $(if ($Portrait) { 150.0 } else { 70.0 })
    $y = NewText $sl $M $topY $W "SOUND ENGINE" 10 $PAL.teal $true
    $y = (NewText $sl $M ($y + 6) $W "AcousticFlow" $(if ($Portrait) { 40 } else { 44 }) $PAL.deep $true)
    $y = (NewText $sl $M ($y + 2) $W "HYBRID GEOMETRIC ACOUSTICS" 11 $PAL.muted) + 14
    $y = (NewText $sl $M $y $W "変わる所だけ実行時に解く、ハイブリッドの幾何音響エンジン" 13 $PAL.ink) + 14
    $lead = @($page.getElementsByClassName("lead"))
    if ($lead.Count) { $y = (NewBox $sl $M $y $W (($lead[0].innerText -replace "\s+"," ").Trim()) $PAL.aqua $PAL.ink 10.5) + 16 }
    $chips = @($page.getElementsByClassName("chip"))
    $cw = ($W - ($GAP * 2)) / 3
    $chipY = $y
    for ($ci = 0; $ci -lt $chips.Count -and $ci -lt 3; $ci++) {
      $v = @($chips[$ci].getElementsByClassName("v")); $k = @($chips[$ci].getElementsByClassName("k"))
      if (-not $v.Count -or -not $k.Count) { continue }
      $cx = $M + (($cw + $GAP) * $ci)
      $yy = NewText $sl $cx $chipY $cw ($v[0].innerText.Trim()) 15 $PAL.teal $true
      $yy = NewText $sl $cx ($yy + 2) $cw ($k[0].innerText.Trim()) 9 $PAL.muted
      if ($yy -gt $y) { $y = $yy }
    }
    $meta = @($page.getElementsByClassName("meta"))
    if ($meta.Count) {
      NewText $sl $M ($SH - $M - 34) $W (($meta[0].innerText -replace "\s*\n\s*", "　／　")).Trim() 9 $PAL.muted | Out-Null
    }
    continue
  }

  if ($catEl.Count) { $y = (NewText $sl $M $y $W ($catEl[0].innerText.Trim()) 8.5 $PAL.teal $true) + 2 }
  if ($ttlEl.Count) { $y = (NewText $sl $M $y $W ($ttlEl[0].innerText.Trim()) 17 $PAL.deep $true) + 8 }

  $brief = @($page.getElementsByClassName("brief"))
  if ($brief.Count) { $y = (NewBox $sl $M $y $W (($brief[0].innerText -replace "[ \t]+"," ").Trim()) $PAL.aqua $PAL.ink 10) + 10 }

  $body = @($page.getElementsByClassName("body"))
  if ($body.Count -eq 0) { continue }

  # ★body の子を順に見る。段組みはその場で 2 段に、それ以外は全幅で描く。
  #
  #   ⚠ ここで 2 回間違えた。どちらも「枚数は変わらないので気づけない」型:
  #     1. 「段組みがあれば段だけ描く」にしていて、段組みの外にある図
  #        （p6 の平面図 5 枚）が丸ごと抜けた。
  #     2. HTML には **<div class="body cols"> のように body 自身が段組み**の
  #        ページがある（p3・p9 …）。それを「段組みでない」と判定して、
  #        段まるごとを 1 つの文章に潰し、**表が 18 個中 15 個消えた**。
  $bodyEl = $body[0]

  # ★入りきらないページは、文字を段階的に縮めて引き直す。
  #   16:9 は A4 縦より**縦に使える長さが短い**ので、3 段にしても入らないページがある。
  #   ページを分割すると本文のページ参照（「p10 参照」）が壊れるので、
  #   **1 ページ = 1 スライドを保ったまま縮める**方を選ぶ。
  #   ⚠ 縮めたページは文字が小さくなる。どのページを縮めたかは最後に報告する。
  $baseNames = ShapeNames $sl
  $script:trace = (($p + 1) -eq [int]($env:AF_TRACE_PAGE))
  $bodyTop = $y
  # 手を打つ順。読みやすさを先に諦めない ──「段を増やす」→「少し縮める」を交互に。
  $ladder = @()
  foreach ($f in 1.00, 0.93, 0.86, 0.79, 0.72, 0.66, 0.60, 0.55, 0.50) {
    foreach ($n in $NCOL0, ($NCOL0 + 1)) { $ladder += ,@($f, $n) }
  }
  foreach ($try in $ladder) {
    $script:FS = $try[0]
    $script:NCOL = $try[1]
    $script:pageSvgCursor = 0
    RemoveNewShapes $sl $baseNames
    $y = $bodyTop
    if (HasClass $bodyEl "cols") {
      $y = RenderCols $sl $bodyEl $y
    } else {
      foreach ($ch in @($bodyEl.children)) {
        if ((HasClass $ch "cols") -and @($ch.getElementsByClassName("col")).Count -ge 2) {
          $y = RenderCols $sl $ch $y
        } else {
          $y = DrawBlock $sl $ch $M $y $W
        }
      }
    }
    if ($y -le $BOT) { break }
  }
  if ($script:FS -lt 0.999 -or $script:NCOL -ne $NCOL0) {
    $shrunk += ("p{0}({1:P0}/{2}段)" -f ($p + 1), $script:FS, $script:NCOL)
  }
  $script:FS = 1.0
  $script:NCOL = $NCOL0
}

# ── 出来上がりを検査する ────────────────────────────────────────────
#
# ★なぜ組み込みにするか
#   ここで壊れる型は全部「**枚数が変わらない**」。表が消えても、文字が枠から出ても、
#   18 枚のままです。目で 18 枚めくる以外に気づく方法が無いので、**毎回数える**。
Write-Host "[4/5] 出来上がりを検査中..."
$nTbl = 0; $nPic = 0
$over = @()
foreach ($sl in $pres.Slides) {
  $bad = $false
  foreach ($sh in $sl.Shapes) {
    if ($sh.HasTable -eq -1) { $nTbl++ }
    # SVG は msoGraphic(28)。msoPicture(13) ではないので 13 だけ数えると 0 になる。
    if ($sh.Type -eq 28 -or $sh.Type -eq 13 -or $sh.Type -eq 11) { $nPic++ }
    $b = $sh.Top + $sh.Height
    $r = $sh.Left + $sh.Width
    if ($b -gt $BOT + 1 -or $r -gt $SW - $M + 1) { $bad = $true }
  }
  if ($bad) { $over += ("p{0}" -f $sl.SlideIndex) }
}
# HTML 側の数（原本）と突き合わせる。
$srcTbl = ([regex]::Matches($src, '<table')).Count
$srcSvg = (($svgByPage.Values | ForEach-Object { $_.Count }) | Measure-Object -Sum).Sum

Write-Host ("      表 : {0} / {1}" -f $nTbl, $srcTbl) -ForegroundColor $(if ($nTbl -ge $srcTbl) { "Green" } else { "Red" })
Write-Host ("      図 : {0} / {1}" -f $nPic, $srcSvg) -ForegroundColor $(if ($nPic -ge $srcSvg) { "Green" } else { "Red" })
if ($over.Count) { Write-Host ("      ★枠外へ出ている: " + ($over -join ", ")) -ForegroundColor Red }
else            { Write-Host "      はみ出し : なし" -ForegroundColor Green }
if ($shrunk.Count) { Write-Host ("      文字を縮めた: " + ($shrunk -join ", ")) -ForegroundColor Yellow }

# ── 保存 ────────────────────────────────────────────────────────────
Write-Host "[5/5] 保存中..."
if (Test-Path $out) { Remove-Item $out -Force }
$pres.SaveAs($out)
$pres.Close()
$ppt.Quit()
[System.Runtime.InteropServices.Marshal]::ReleaseComObject($ppt) | Out-Null
[GC]::Collect()
Remove-Item $tmp -Recurse -Force -ErrorAction SilentlyContinue

Start-Sleep -Milliseconds 400
if (Test-Path $out) {
  Write-Host ""
  Write-Host ("出力   : " + $out) -ForegroundColor Green
  Write-Host ("枚数   : " + $pages.Count + " 枚")
  Write-Host ("サイズ : " + [math]::Round((Get-Item $out).Length / 1KB) + " KB")
  Write-Host ""
  Write-Host "⚠ 図（SVG）は貼り付けていますが、位置と大きさは目で確かめてください。"
  Write-Host "⚠ 文章を直すときは HTML を原本にして、これをもう一度回すのが安全です。"
} else {
  Write-Host "保存に失敗しました" -ForegroundColor Red
}
