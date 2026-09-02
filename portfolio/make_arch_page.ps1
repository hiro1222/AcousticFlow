# make_arch_page.ps1 — 紹介資料に足す「システム構造」のページを画で出す。
#
#   使い方: powershell -ExecutionPolicy Bypass -File portfolio\make_arch_page.ps1
#
# ★なぜ画（PNG）なのか
#   本人の資料は手作りの 10 枚。**そこへ勝手にページを足さない**ため、
#   別のファイルで組んで画にだけ出す。置き場所は本人が決める。
#
# ★なぜ既存の 5 枚目を複製して描くのか
#   題名・下線・上の帯・波形・下の罫といった「枠」を**寸分たがわず**揃えたいから。
#   一から描くと必ずどこかがずれる。5 枚目は中身が無い骨だけなので、複製の元に最適。
#   ⚠ 元のファイルは**読むだけ**。作業用の写しに対して行う。
#
# ★枠の色
#   コンセプトのページ（2〜4 枚目）と同じ **16C1C1（teal）**。
#   帯の左の棒と、題名の下の線（image2.png）の両方を揃える。
#
# ★このファイルは UTF-8 BOM 付きで保存すること（PS 5.1 は BOM 無しを ANSI と読む）。
param(
  [string]$Deck = "$env:USERPROFILE\Downloads\acousticflow_portfolio_original.pptx",
  [string]$Out  = "$env:USERPROFILE\Downloads\acousticflow_system_structure.png"
)
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.IO.Compression.FileSystem

$tmp = Join-Path $env:TEMP ("af_arch_" + [System.Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory $tmp | Out-Null
$work = Join-Path $tmp "work.pptx"
Copy-Item $Deck $work

# 題名の下の線は画像。コンセプトのページが使っている image2.png（teal）を取り出す。
$rule = Join-Path $tmp "rule_teal.png"
$z = [System.IO.Compression.ZipFile]::OpenRead($Deck)
$e = $z.Entries | Where-Object { $_.FullName -eq "ppt/media/image2.png" }
$fs = [System.IO.File]::Create($rule); $e.Open().CopyTo($fs); $fs.Close()
$z.Dispose()

# ── 既存 9 枚から採った作り（1pt 単位。スライドは 960 x 540pt）───────
$PAL = @{
  ink    = "1A1F2B"; body  = "2B3240"; muted = "6B7280"; ghost = "9AA3B2"
  card   = "F7F8FA"; line  = "E5E7EB"; track = "EEF1F5"
  dark   = "151A24"; onDark= "EEF2F8"; onDarkK = "5FE0DC"
  teal   = "16C1C1"; blue  = "3AA8D8"; indigo = "5B5BD6"; amber = "D99022"
}
function RGB([string]$hex) {
  [Convert]::ToInt32($hex.Substring(0,2),16) +
  ([Convert]::ToInt32($hex.Substring(2,2),16) * 256) +
  ([Convert]::ToInt32($hex.Substring(4,2),16) * 65536)
}
$FONT = "Yu Gothic UI"

$ppt = New-Object -ComObject PowerPoint.Application
$ppt.Visible = -1
$pres = $ppt.Presentations.Open($work, $false, $false, $false)

# 5 枚目（骨だけ）を複製して 6 枚目にする。
$sl = $pres.Slides.Item(5).Duplicate().Item(1)

# ── 枠の色をコンセプトのページに合わせる ─────────────────────────
$sl.Shapes.Item("Rectangle 2").Fill.ForeColor.RGB = RGB $PAL.teal   # 帯の左の棒
$old = $sl.Shapes.Item("Picture 61")                                # 題名の下の線
$px = $old.Left; $py = $old.Top; $pw = $old.Width; $ph = $old.Height
$old.Delete()
$sl.Shapes.AddPicture($rule, 0, -1, $px, $py, $pw, $ph) | Out-Null

$sl.Shapes.Item("TextBox 113").TextFrame.TextRange.Text = "システム構造 ── 単独で動くコアと、外へつなぐ層"

# ── 部品 ────────────────────────────────────────────────────────────
# 文字。$runs は @(@(文字, 大きさpt, 太字, 色), ...)。
#   ⚠ 1 個だけの入れ子配列は PowerShell がほどく。必ず戻す。
#     （ほどけたまま流すと文字が 1 文字ずつ属性に散る。実際それで資料が開けなくなった。）
function Text([double]$x, [double]$y, [double]$w, $runs, [double]$lead = 0.95) {
  if ($runs[0] -isnot [System.Array]) { $runs = @(,$runs) }
  $tb = $script:sl.Shapes.AddTextbox(1, $x, $y, $w, 20)
  $tf = $tb.TextFrame
  $tf.WordWrap = -1
  $tf.MarginLeft = 0; $tf.MarginRight = 0; $tf.MarginTop = 0; $tf.MarginBottom = 0
  $tr = $tf.TextRange
  # ⚠ InsertAfter が返す範囲に書式を当てると「指定されたキャストは有効ではありません」で落ちる。
  #   → **先に全文を入れてから Characters(開始, 長さ) で塗り分ける**（1 始まり）。
  $tr.Text = (($runs | ForEach-Object { $_[0] }) -join "")
  $pos = 1
  foreach ($r in $runs) {
    $len = ([string]$r[0]).Length
    $seg = $tr.Characters($pos, $len)
    $seg.Font.Name = $FONT; $seg.Font.NameFarEast = $FONT
    $seg.Font.Size = [single]$r[1]
    $seg.Font.Bold = $(if ($r[2]) { -1 } else { 0 })
    $seg.Font.Color.RGB = RGB $r[3]
    $pos += $len
  }
  $tr.ParagraphFormat.SpaceWithin = $lead
  $tf.AutoSize = 1
  return $tb
}
# 角丸／直角の箱。
function Box([double]$x, [double]$y, [double]$w, [double]$h, [string]$fill,
             [string]$ln = "", [double]$adj = 0.06, [int]$kind = 5) {
  $sh = $script:sl.Shapes.AddShape($kind, $x, $y, $w, $h)
  # ⚠ このファイルのテーマは図形に**青いグラデーションと影**を既定で当ててくる。
  #   色を指定するだけでは消えず、全部の箱が青くなった。**単色と影無しを明に指定する。**
  $sh.Fill.Solid()
  $sh.Fill.Transparency = 0
  $sh.Shadow.Visible = 0
  try { $sh.ThreeD.Visible = 0 } catch { }
  if ($fill) { $sh.Fill.ForeColor.RGB = RGB $fill } else { $sh.Fill.Visible = 0 }
  if ($ln) { $sh.Line.Visible = -1; $sh.Line.ForeColor.RGB = RGB $ln; $sh.Line.Weight = 0.75 } else { $sh.Line.Visible = 0 }
  if ($kind -eq 5) { $sh.Adjustments.Item(1) = $adj }
  return $sh
}

# ── 上の帯 ──────────────────────────────────────────────────────────
Text 68.6 106.9 820 @(
  @("音の計算は Unity にも Wwise にも依存しません。", 11, $true, $PAL.onDarkK),
  @(" ホストに触れるのは外側の 2 層だけです。", 11, $false, $PAL.onDark)) | Out-Null
Text 68.6 126.4 820 @(
  @("載せ替えるときに書き直すのはその 2 層だけで、幾何と DSP は 1 行も変わりません。", 11, $true, "FFFFFF")) | Out-Null

# ── 左: 層の積み ────────────────────────────────────────────────────
#   ★このページの主役。**依存が一方向**であることを、積み方そのもので見せる。
Box 45.75 174.75 495 307 $PAL.card $PAL.line | Out-Null
Text 62 186 460 @(@("依存は上から下へ ── 下の 2 層は上を知らない", 10.5, $true, $PAL.ink)) | Out-Null
Box 62 207 462 0.75 $PAL.line "" 0 1 | Out-Null

# 層 = @(名前, 説明, 地の色, 枠の色, 名前の色, 説明の色)
$layers = @(
  @("ホスト",     "Unity（C#）／ Wwise ／ 検査ハーネス（CLI）",        $PAL.track, "",        $PAL.muted,  $PAL.muted),
  @("Adapter",    "ミドルウェアとの接続。Wwise SDK をここだけに閉じ込める", "FFFFFF", $PAL.line, $PAL.amber,  $PAL.body),
  @("Export",     "C API。extern `"C`" と POD だけ。C++ の型は境界を越えない", "FFFFFF", $PAL.line, $PAL.indigo, $PAL.body),
  @("Core / Dsp", "幾何・回折・部屋／畳み込み・HRTF・尾。外を一切知らない", $PAL.dark, "",       $PAL.teal,   $PAL.onDark)
)
$ly = 214.0
foreach ($L in $layers) {
  Box 62 $ly 462 50 $L[2] $L[3] 0.14 | Out-Null
  Text 76 ($ly + 11) 110 @(@($L[0], 11, $true, $L[4])) | Out-Null
  Text 76 ($ly + 29) 430 @(@($L[1], 9, $false, $L[5])) | Out-Null
  $ly += 60
}
# ★注記は「主張の裏取り」ではなく「この形にすると何ができるか」を書く。
Text 62 452 460 @(@("ミドルウェアを差し替えても、音の計算には触れません。Wwise SDK が無い PC でも DLL は丸ごと建ちます。", 8.5, $false, $PAL.muted)) | Out-Null
Text 62 466 460 @(@("検査もホスト無しで回ります ── 回帰テスト 447 件は Unity も Wwise も使わない CLI 実行です。", 8.5, $false, $PAL.muted)) | Out-Null

# ── 右: デバッグのために両方持っている ──────────────────────────────
Box 563.6 174.75 351 307 $PAL.card $PAL.line | Out-Null
Text 576 186 330 @(
  @("同じ処理を 2 つ持つ", 11, $true, $PAL.teal),
  @(" → ", 11, $false, $PAL.ghost),
  @("音そのもので照合する", 11, $true, $PAL.indigo)) | Out-Null
Box 576 209 326 0.75 $PAL.line "" 0 1 | Out-Null

$items = @(
  @{ y = 220; c = $PAL.teal;   h = "C# と C++、両方に同じ DSP がある";
     b = @("畳み込み・HRTF・後期尾を、Unity の C# 側と",
           "DLL の C++ 側の両方に実装してあります。") },
  @{ y = 280; c = $PAL.blue;   h = "同じシーンに並べて A/B できる";
     b = @("IrConvolver（C#）と VoiceConvolver（DLL）を",
           "置き換えながら、そのまま聞き比べられます。") },
  @{ y = 340; c = $PAL.indigo; h = "差が出れば、その場で分かる";
     b = @("耳で気づけない差は数字で縛っています ── 共有バスと",
           "個別の畳み込みが −123.5 dB で一致、並列と直列が",
           "ビット一致、C++ と C# の値が一致。") }
)
foreach ($it in $items) {
  Box 576 ([double]$it.y + 5) 8 8 $it.c "" 0 1 | Out-Null
  Text 592 $it.y 320 @(@($it.h, 10.5, $true, $PAL.ink)) | Out-Null
  for ($k = 0; $k -lt $it.b.Count; $k++) {
    Text 592 ([double]$it.y + 18 + (14 * $k)) 320 @(@($it.b[$k], 9, $false, $PAL.body)) | Out-Null
  }
}
Box 576 412 326 62 $PAL.dark | Out-Null
Text 588 421 302 @(
  @("境界の型ずれは、音ではなく値を壊します。", 9.5, $true, $PAL.onDarkK),
  @(" C++ の POD と", 9.5, $false, $PAL.onDark)) | Out-Null
Text 588 436 302 @(@("C# の宣言が 1 バイトずれても音は鳴り続けるので、", 9.5, $false, $PAL.onDark)) | Out-Null
Text 588 451 302 @(@("並びを機械で照合する検査を別に持っています。", 9.5, $false, $PAL.onDark)) | Out-Null

# ── 検査してから画に出す ────────────────────────────────────────────
$over = @()
foreach ($sh in $sl.Shapes) {
  if (($sh.Top + $sh.Height) -gt 508 -or ($sh.Left + $sh.Width) -gt 916 -or $sh.Left -lt 0) {
    $over += ("{0}(下{1:N0} 右{2:N0})" -f $sh.Name, ($sh.Top + $sh.Height), ($sh.Left + $sh.Width))
  }
}
if ($over.Count) { Write-Host ("★枠外: " + ($over -join ", ")) -ForegroundColor Red }
else            { Write-Host "はみ出し : なし" -ForegroundColor Green }
Write-Host ("図形 : " + $sl.Shapes.Count + " 個")

if (Test-Path $Out) { Remove-Item $Out -Force }
$sl.Export($Out, "PNG", 1920, 1080)
$pres.Close()
Remove-Item $tmp -Recurse -Force -ErrorAction SilentlyContinue
Write-Host ("出力 : " + $Out) -ForegroundColor Green
