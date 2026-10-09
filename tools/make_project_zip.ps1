# make_project_zip.ps1
# 出荷用の「プロジェクト一式」を作る。**開けばそのまま動く**状態で、要らない物だけ抜く。
#
# ★抜く物の考え方
#   ① 作り直せる物   … Library / build / obj / Logs（Unity と CMake が作り直す）
#   ② 渡せない物     … 商用音源の wav、Wwise のバンク（曲そのもの）
#   ③ 作業の残骸     … DistDemo / build2 / _recovered / .git
#
# ★抜いてはいけない物（消えると動きません）
#   ・Projects/UnityDemo/Assets/Plugins/x86_64/AcousticEngine.dll  … 音響エンジン本体
#   ・Projects/UnityDemo/Assets/Textures/CC0/                      … 無いと全部ピンクになる
#   ・Projects/UnityDemo/ProjectSettings/ と Packages/             … 無いとプロジェクトとして開けない
#   ・AcousticEngine/（C++ 一式）                          … これが作品の中身
#
# ★写した後で**在るはずの物を数え直します**。
#   抜きすぎは「開いたら壊れていた」で気づくのが遅く、渡した後だと取り返しがつきません。
#
# 使い方:
#   powershell -ExecutionPolicy Bypass -File tools\make_project_zip.ps1
#   powershell -ExecutionPolicy Bypass -File tools\make_project_zip.ps1 -OutDir D:\ship -NoZip

param(
    [string]$OutDir = "$env:USERPROFILE\Desktop",
    [switch]$NoZip
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$stamp = Get-Date -Format 'yyyyMMdd_HHmm'
$stage = Join-Path $OutDir "AcousticFlow_Project_$stamp"

Write-Host "元: $root"
Write-Host "先: $stage"
Write-Host ""

# ── 写す（robocopy の除外で抜く）──────────────────────────────
#   /MIR は使わない。書き先を消してしまう事故が怖いので、空の場所へ足すだけにする。
$excludeDirs = @(
    'Library', 'Temp', 'obj', 'Logs', 'Build', 'build', 'build2',
    'DistDemo', '_recovered', '.git', '.vs', '.idea',
    'WwiseBanks',            # 曲そのもの。渡せない
    'UserSettings'           # 手元のエディタ設定。相手には不要
)
# ★渡せない音は robocopy に任せません。
#
#   最初は /XF に並べていましたが、**日本語のファイル名を渡した時点で引数が壊れ**、
#   関係ない `Footstep_Asphalt.mp3` まで落ちていました（実測）。
#   robocopy には ASCII のパターンだけ渡し、日本語を含む物は
#   写した後に PowerShell 側で消します。そちらは Unicode で確実に扱えます。
$excludeFiles = @('*.csproj', '*.sln', '*.user')

# 写した後に消す物（名前で完全一致。.meta も一緒に）。
$dropAudio = @(
    'TokyoGeto.wav',
    'videoplayback.wav',
    'ロクデナシ「ブリザード」 Rokudenashi - Blizzard【Official Music Video】 - Rokudenashi (128k).wav'
)

New-Item -ItemType Directory -Force -Path $stage | Out-Null

$rcArgs = @($root, $stage, '/E', '/NFL', '/NDL', '/NJH', '/NJS', '/NP', '/R:1', '/W:1')
$rcArgs += '/XD'; $rcArgs += $excludeDirs
$rcArgs += '/XF'; $rcArgs += $excludeFiles

& robocopy @rcArgs | Out-Null
# robocopy は 0-7 が成功。8 以上が本当の失敗。
if ($LASTEXITCODE -ge 8) { throw "robocopy が失敗しました（コード $LASTEXITCODE）" }

# ── 渡せない音を消す（写した後・Unicode で確実に）──────────────
$dropped = 0
foreach ($n in $dropAudio) {
    foreach ($p in @((Join-Path $stage "Projects\UnityDemo\Assets\Audio\$n"),
                     (Join-Path $stage "Projects\UnityDemo\Assets\Audio\$n.meta"))) {
        if (Test-Path -LiteralPath $p) { Remove-Item -LiteralPath $p -Force; $dropped++ }
    }
}
Write-Host "渡せない音を $dropped 個消しました（商用音源）"

# ── 在るはずの物を数え直す ────────────────────────────────────
$must = @(
    'Projects\UnityDemo\Assets\Plugins\x86_64\AcousticEngine.dll',
    'Projects\UnityDemo\ProjectSettings\ProjectSettings.asset',
    'Projects\UnityDemo\Packages\manifest.json',
    'Projects\UnityDemo\Assets\Scenes\Trial_Stage012_Play.unity',
    'Projects\UnityDemo\Assets\Audio\画面録画-2026-08-24-192052.wav',
    'AcousticEngine\include',
    'docs'
)

$missing = @()
foreach ($m in $must) {
    if (-not (Test-Path (Join-Path $stage $m))) { $missing += $m }
}

$tex = Get-ChildItem (Join-Path $stage 'Projects\UnityDemo\Assets\Textures') -Recurse -File -ErrorAction SilentlyContinue
if (($tex | Measure-Object).Count -lt 10) { $missing += 'Projects\UnityDemo\Assets\Textures\（中身が少なすぎる）' }

# ── 抜けてはいけない物が抜けたら、ここで止める ──
if ($missing.Count -gt 0) {
    Write-Host ""
    Write-Host "!! 抜けてはいけない物がありません:" -ForegroundColor Red
    $missing | ForEach-Object { Write-Host "   $_" -ForegroundColor Red }
    Write-Host ""
    Write-Host "   ZIP は作りません。除外の指定を見直してください。" -ForegroundColor Red
    exit 1
}

# ── 大きさを出す ──────────────────────────────────────────────
$size = (Get-ChildItem $stage -Recurse -File | Measure-Object -Property Length -Sum).Sum
Write-Host ("写し終えました: {0:N0} MB" -f ($size / 1MB))

# 商用音源が紛れていないか、名前で最終確認
$bad = Get-ChildItem $stage -Recurse -File -Include '*.bnk','TokyoGeto.wav','videoplayback.wav','*Rokudenashi*' -ErrorAction SilentlyContinue
if ($bad) {
    Write-Host ""
    Write-Host "!! 渡せない物が残っています:" -ForegroundColor Red
    $bad | ForEach-Object { Write-Host "   $($_.FullName)" -ForegroundColor Red }
    exit 1
}

if ($NoZip) {
    Write-Host "ZIP は作りません（-NoZip）。フォルダのまま: $stage"
    exit 0
}

$zip = "$stage.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip -CompressionLevel Optimal

$zs = (Get-Item $zip).Length
Write-Host ("ZIP: {0}  （{1:N0} MB）" -f $zip, ($zs / 1MB))
Write-Host ""
Write-Host "★渡す前に一度、写したほうを Unity で開いて確かめてください。"
Write-Host "  Library を抜いてあるので、初回は取り込みに数分かかります。"
Write-Host "  開いたら Trial_Stage012_Play を再生し、扉をくぐれることだけ見ればじゅうぶんです。"
