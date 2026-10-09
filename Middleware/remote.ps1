# remote.ps1 — 別の機械（ノートなど）から、この機械の AfHost を触るための入口。
#
# ★狙い: **実行環境をこの機械に固定したまま**、手元はノートで済ませる。
#   音響の数字（1 回あたりのサンプル数・負荷率）は、この機械の
#   本物のオーディオ機器で測ったものでないと意味がありません。
#   だから「動かすのはここ、見るのはノート」に分けます。
#
#   使い方（ノートから SSH で入って叩く）:
#     powershell -ExecutionPolicy Bypass -File Middleware\remote.ps1 -Check
#     powershell -ExecutionPolicy Bypass -File Middleware\remote.ps1 -Shot プロパティ
#     powershell -ExecutionPolicy Bypass -File Middleware\remote.ps1 -Check -Shot イベント
#     powershell -ExecutionPolicy Bypass -File Middleware\remote.ps1 -Health
#
# ⚠ 音は届きません。SSH は文字だけです。
#   耳で確かめたいときは画面転送（Steam Remote Play）を使ってください。
#   ただし転送した音で**遅延や負荷を判断しないこと** ── 流し物になるので、
#   ステータスバーの数字は転送用の仮想デバイスのものに変わります
#   （どのデバイスを開いたかは画面に出るので、そこで見分けられます）。
#
# ★このファイルは UTF-8 BOM 付きで保存すること（PS 5.1 は BOM 無しを ANSI と読む）。

param(
    [switch]$Check,                 # ビルドして自己検査を回す
    [string]$Shot = "",             # 画面を PNG に撮る（値は前に出すパネル名）
    [string]$Project = "",          # 開くプロジェクト（省略可）
    [string]$OutDir = "",           # PNG の置き場所（省略時はリポジトリの下）
    [switch]$NoBuild,               # ビルドを飛ばす
    [switch]$Health                 # 遠隔で入り直すための足回りが生きているか見る
)
$ErrorActionPreference = "Stop"

$root  = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$mw    = Join-Path $root "Middleware"
$bin   = Join-Path $mw "build\bin\Release"
$cmake = "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"

if ($OutDir -eq "") { $OutDir = Join-Path $mw "shots" }
if (-not (Test-Path $OutDir)) { New-Item -ItemType Directory -Force $OutDir | Out-Null }
# ── -Health ────────────────────────────────────────────────────────
# ★ここだけはビルドもせず、走っているものも止めない。
#   「締め出されていないか」を見るための窓なので、副作用があってはいけません。
#
#   入り直す道具は Chrome リモートデスクトップ（サービス名 chromoting）。
#   RustDesk を見送ったのは、ウイルスバスターに PUA として隔離され得るからです
#   ── 留守中に隔離されると、入り直す手段そのものが消えます。
#   SSH はさらにその下。Microsoft 純正なので検知されず、ロック画面でも再起動後でも
#   生きているので、上が倒れてもここから入って直せます。
if ($Health) {
    function Line($label, $ok, $detail) {
        $mark = if ($ok -eq $true) { "OK  " } elseif ($ok -eq $false) { "NG  " } else { "--  " }
        $col  = if ($ok -eq $true) { "Green" } elseif ($ok -eq $false) { "Red" } else { "Gray" }
        Write-Host ("  {0}{1,-24} {2}" -f $mark, $label, $detail) -ForegroundColor $col
    }
    Write-Host ""
    Write-Host "  遠隔で入り直すための足回り"
    Write-Host "  ────────────────────────────────────────"

    $svc = Get-Service chromoting -ErrorAction SilentlyContinue
    if ($svc) {
        Line "Chrome リモートデスクトップ" ($svc.Status -eq "Running") "$($svc.Status)"
    } else {
        Line "Chrome リモートデスクトップ" $false "サービス chromoting が居ません"
    }

    # ⚠ 前提。Chrome が消えると host の更新が止まります。
    $chrome = @("C:\Program Files\Google\Chrome\Application\chrome.exe",
                "C:\Program Files (x86)\Google\Chrome\Application\chrome.exe") |
              Where-Object { Test-Path $_ } | Select-Object -First 1
    Line "  └ Chrome" ([bool]$chrome) $(if ($chrome) { (Get-Item $chrome).VersionInfo.ProductVersion } else { "見当たりません" })

    $ssh = Get-Service sshd -ErrorAction SilentlyContinue
    Line "SSH（最後の砦）" ($null -ne $ssh -and $ssh.Status -eq "Running") $(if ($ssh) { $ssh.Status } else { "未導入" })

    Line "Steam" ([bool](Get-Process steam -ErrorAction SilentlyContinue)) "画面配信の側"
    $af = [bool](Get-Process AfHost -ErrorAction SilentlyContinue)
    Line "AfHost" $null $(if ($af) { "動いている" } else { "動いていない（異常ではない）" })

    $boot = (Get-CimInstance Win32_OperatingSystem).LastBootUpTime
    Line "最後の起動" $null ("{0}（{1:N1} 日前）" -f $boot, ((Get-Date) - $boot).TotalDays)

    $pending = Test-Path "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\WindowsUpdate\Auto Update\RebootRequired"
    Line "再起動待ち" (-not $pending) $(if ($pending) { "あり ── 次の再起動でサインイン画面に戻ります" } else { "なし" })

    Write-Host ""
    if (-not $svc -or $svc.Status -ne "Running") {
        if ($ssh -and $ssh.Status -eq "Running") {
            Write-Host "  ⚠ 入り直す道具が動いていません。SSH は生きているので、ここから直せます:" -ForegroundColor Yellow
        } else {
            Write-Host "  ⚠ 入り直す道具も SSH も居ません。いま締め出されたら戻れません:" -ForegroundColor Red
        }
        Write-Host "      Start-Service chromoting              （止まっているだけなら）"
        Write-Host "      直らなければ remotedesktop.google.com/access で設定し直す"
        exit 1
    }
    exit 0
}

# ⚠ 走っている AfHost が居ると exe を掴んでリンクが失敗する。
#   （原本を掴まないのは読み込む DLL の話で、exe 自身は掴まれます）
$running = Get-Process AfHost -ErrorAction SilentlyContinue
if ($running) {
    Write-Host "走っている AfHost を止めます（exe を掴んでいてビルドできないため）"
    $running | Stop-Process -Force
    Start-Sleep -Milliseconds 400
}

if (-not $NoBuild) {
    if (-not (Test-Path $cmake)) { throw "cmake が見つかりません: $cmake" }
    Write-Host "ビルド中…"
    $out = & $cmake --build (Join-Path $mw "build") --config Release 2>&1
    $errs = $out | Select-String -Pattern "error C|error LNK"
    if ($errs) {
        $errs | ForEach-Object { Write-Host $_ -ForegroundColor Red }
        exit 1
    }
    Write-Host "ビルド: 通りました"
}

$failed = 0

if ($Check) {
    Write-Host ""
    Push-Location $root
    & (Join-Path $bin "AfHostCheck.exe") --selftest
    if ($LASTEXITCODE -ne 0) { $failed = 1 }
    Pop-Location
}

if ($Shot -ne "") {
    $stamp = Get-Date -Format "MMdd_HHmmss"
    $png = Join-Path $OutDir "afhost_$stamp.png"

    # ⚠ $args は PowerShell の予約変数なので使わないこと
    $appArgs = @()
    if ($Project -ne "") { $appArgs += $Project }
    $appArgs += @("--panel", $Shot, "--shot", $png, "--frames", "90")

    Push-Location $bin
    Start-Process (Join-Path $bin "AfHost.exe") -ArgumentList $appArgs -Wait
    Pop-Location
    Get-Process AfHost -ErrorAction SilentlyContinue | Stop-Process -Force

    if (Test-Path $png) {
        Write-Host ""
        Write-Host "画面: $png"
        Write-Host "  ノートへ持って行くには（ノート側で）:"
        Write-Host "    scp <ユーザ>@<この機械>:'$png' ."
    } else {
        Write-Host "画面を撮れませんでした" -ForegroundColor Red
        $failed = 1
    }
}

if (-not $Check -and $Shot -eq "" -and -not $Health) {
    Write-Host ""
    Write-Host "  -Check              ビルドして自己検査を回す"
    Write-Host "  -Shot <パネル名>    画面を PNG に撮る（SoundLibrary / イベント / プロパティ …）"
    Write-Host "  -Project <パス>     開くプロジェクト"
    Write-Host "  -NoBuild            ビルドを飛ばす"
    Write-Host "  -Health             遠隔で入り直すための足回りが生きているか見る"
}

exit $failed
