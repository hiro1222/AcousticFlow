# build.ps1 — ポートフォリオ資料の HTML を PDF にする。
#
#   使い方:  powershell -ExecutionPolicy Bypass -File portfolio\build.ps1
#
# なぜ Edge のヘッドレスか:
#   この環境に実行可能な Python が無い（WindowsApps のスタブだけ）ため reportlab が使えない。
#   Edge/Chrome なら日本語フォントの埋め込みも CSS の段組みもそのまま通る。
#   実際、出力 PDF には YuGothicUI Regular / Bold が FontFile2 として埋まっている。
#
# ★このファイルは UTF-8 BOM 付きで保存すること。
#   Windows PowerShell 5.1 は BOM 無しの .ps1 を ANSI として読むので、日本語が化けて構文エラーになる。

$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$html = Join-Path $here "acousticflow_portfolio.html"
$pdf  = Join-Path $here "acousticflow_portfolio.pdf"

$edge = "C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe"
if (-not (Test-Path $edge)) { $edge = "C:\Program Files\Google\Chrome\Application\chrome.exe" }
if (-not (Test-Path $edge)) { throw "Edge も Chrome も見つかりません" }

# プロファイルは使い捨てにしない（毎回作ると起動が遅い）。
$prof = Join-Path $env:TEMP "af_portfolio_profile"

if (Test-Path $pdf) { Remove-Item $pdf }

# ★stderr をリダイレクトしない。
#   PowerShell 5.1 でネイティブ exe の stderr を捕まえると、Edge が必ず出す無害な警告が
#   ErrorRecord に化けて「失敗した」ことにされる。Edge は終了コードで判定する。
$uri = ([System.Uri](Get-Item $html).FullName).AbsoluteUri
& $edge --headless=new --disable-gpu "--user-data-dir=$prof" --virtual-time-budget=10000 "--print-to-pdf=$pdf" $uri

# ヘッドレスは終了後に少し遅れて書き出すことがある。
$deadline = (Get-Date).AddSeconds(20)
while (-not (Test-Path $pdf) -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 300 }
if (-not (Test-Path $pdf)) { throw "PDF が生成されませんでした" }

# 枚数を数える（20 枚以内が提出条件）。
$s = [System.Text.Encoding]::ASCII.GetString([System.IO.File]::ReadAllBytes($pdf))
$counts = [regex]::Matches($s, "/Count\s+(\d+)") | ForEach-Object { [int]$_.Groups[1].Value }
$pages = ($counts | Measure-Object -Maximum).Maximum
$fonts = ([regex]::Matches($s, "/BaseFont\s*/([A-Za-z0-9+\-,#]+)") |
          ForEach-Object { $_.Groups[1].Value } | Sort-Object -Unique) -join ", "
$kb = [math]::Round((Get-Item $pdf).Length / 1KB)

Write-Host ""
Write-Host "出力   : $pdf"
if ($pages -gt 20) {
    Write-Host "枚数   : $pages 枚  ★上限 20 枚を超えています" -ForegroundColor Red
} else {
    Write-Host "枚数   : $pages 枚 / 上限 20" -ForegroundColor Green
}
Write-Host "フォント: $fonts"
Write-Host "サイズ : $kb KB"


# ── 枚数が変わっていないかの見張り ──────────────────────────────
#
# ★なぜ要るか
#   各ページは A4 の**固定の箱**。文章を増やすと、次のページへ送られるか
#   **箱の中で静かに切れる**かのどちらかになる。前者は枚数で気づけるので、
#   まずそこを見張る。
#
# ⚠ **箱の中で切れる場合は枚数が変わりません。**これは自動では見つけられないので、
#   文章を大きく増やしたときは**PDF を目で確かめてください**。
$expected = 19
if ($pages -ne $expected) {
    Write-Host ""
    Write-Host "★枚数が $expected 枚から $pages 枚に変わりました。" -ForegroundColor Red
    Write-Host "  どこかの文章が増えて次のページへ送られた可能性があります。"
    Write-Host "  意図した変更なら、build.ps1 の `$expected を $pages に直してください。"
} else {
    Write-Host "見張り : 枚数は $expected 枚のまま（文章の増減で溢れていない）" -ForegroundColor Green
    Write-Host "         ⚠ 箱の中で切れる場合は枚数が変わりません。大きく直したら目で確認を。"
}
