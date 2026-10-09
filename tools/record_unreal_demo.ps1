# record_unreal_demo.ps1 ── Unreal の地図でキャラクターを自動で歩かせて録画し、音つきの MP4 にする（スマホで確かめる用、2026-10-04）。
#
# ■ 流れ
#   1) 録画の道具（build/bin/Release/AfCapture.exe）を先に立ち上げる。ゲームの窓と撮り始めの合図を待つ。
#   2) Unreal を -game で立ち上げる。-AFDemoWalk でキャラクターが自動で歩き（2 秒立つ → 歩く → 止まる → 見回す → 閉じる）、
#      -AFCapture で司令塔がエンジンへ渡した物（箱・摘み・毎フレームの耳と音源）を Saved/Wwise/AFCapture_scene.txt に書き、
#      撮り始めの合図（Saved/Wwise/AFCapture_info.txt）を書く。録画の道具は合図で窓を撮り始め、窓が閉じたら撮り終える。
#   3) ゲームが閉じたら、AfReplayWav が記録を同じエンジンの同じ API で呼び直して音にする（Wwise の自前プラグインと同じ並び）。
#   4) 録画の道具がその音を合図の時刻に合わせて入れ、MP4 を書き終える。出来た場所を最後に出す。
#   -Loopback を付けると、3) をせずにパソコンから実際に出ている音（ループバック）を入れる（出力の機器が動いているときだけ）。
# ■ 使い方
#   powershell -ExecutionPolicy Bypass -File tools/record_unreal_demo.ps1                       … 洞窟（Cave_ThirdPerson）を 25 m 歩く
#   powershell -ExecutionPolicy Bypass -File tools/record_unreal_demo.ps1 -Map /Game/Maps/Flow_SwingDoor -WalkM 5
# ■ 壊れる所
#   ・録画中にゲームの窓の上へ別の最前面の窓が出ると写る。画面がロックされていると黒くなる。
#   ・-AFDemoWalk は三人称のキャラクター（AAcousticFlowThirdPerson）だけ。一人称の地図では歩かない（窓を閉じるまで撮り続ける）。
#   ・作り直した音は Wwise 側の音量（RTPC・減衰）を含まない（試験の音は減衰なし・音量 1）。
param(
    [string]$Map = "/Game/Maps/Cave_ThirdPerson",
    [string]$Out = "",
    [int]$Width = 1280,
    [int]$Height = 720,
    [double]$WalkM = 25,
    [switch]$Loopback
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$proj = Join-Path $root "Projects\UnrealDemo\AcousticFlowUE.uproject"
$ue = "C:\Program Files\Epic Games\UE_5.6\Engine\Binaries\Win64\UnrealEditor.exe"
$cap = Join-Path $root "build\bin\Release\AfCapture.exe"
$replay = Join-Path $root "build\bin\Release\AfReplayWav.exe"
$srcWav = Join-Path $root "Projects\UnrealDemo_WwiseProject\Originals\SFX\videoplayback.wav"   # 試験の音（Play_AF_Test）
$wwiseDir = Join-Path $root "Projects\UnrealDemo\Saved\Wwise"
$info = Join-Path $wwiseDir "AFCapture_info.txt"
$scene = Join-Path $wwiseDir "AFCapture_scene.txt"
$replayWav = Join-Path $wwiseDir "AFCapture_replay.wav"
$movies = Join-Path $root "Projects\UnrealDemo\Saved\Movies"
foreach ($f in @($cap, $replay)) { if (-not (Test-Path $f)) { throw "道具が無い: $f（tools/dev.ps1 build AfCapture / AfReplayWav）" } }
New-Item -ItemType Directory -Force $movies, $wwiseDir | Out-Null
if (-not $Out) { $Out = Join-Path $movies ("{0}_{1}.mp4" -f $Map.Split('/')[-1], (Get-Date -Format "yyyyMMdd_HHmmss")) }
Remove-Item $info, $scene, $replayWav -ErrorAction SilentlyContinue   # 前の記録を消す（残っていると読み込み中から撮り始める）

$capLog = Join-Path $movies "af_capture.log"
$capArgs = @("--out", "`"$Out`"", "--info", "`"$info`"", "--max", "150", "--wait", "180")
if (-not $Loopback) { $capArgs += @("--wav", "`"$replayWav`"") }
$capP = Start-Process -FilePath $cap -ArgumentList $capArgs -PassThru -WindowStyle Hidden -RedirectStandardOutput $capLog -RedirectStandardError "$capLog.err"
$ueArgs = @("`"$proj`"", $Map, "-game", "-windowed", "-ResX=$Width", "-ResY=$Height", "-WinX=60", "-WinY=60",
            "-AFDemoWalk", "-AFDemoWalkM=$WalkM", "-AFCapture", "-nosplash")
$ueP = Start-Process -FilePath $ue -ArgumentList $ueArgs -PassThru
Write-Host "[録画] Unreal と録画の道具を立ち上げた（地図 $Map、$WalkM m 歩く）"
if (-not $ueP.WaitForExit(300000)) { Stop-Process -Id $ueP.Id -Force; Write-Host "[録画] Unreal が 5 分で閉じなかったので止めた" }
if (-not $Loopback) {
    & $replay $scene $srcWav $replayWav | ForEach-Object { "[音] $_" }
}
if (-not $capP.WaitForExit(240000)) { Stop-Process -Id $capP.Id -Force; Write-Host "[録画] 録画の道具が終わらなかったので止めた" }
Get-Content $capLog -Encoding UTF8 -ErrorAction SilentlyContinue
Get-Content "$capLog.err" -Encoding UTF8 -ErrorAction SilentlyContinue
if (Test-Path $Out) { Write-Host ("[録画] 出来た: {0}（{1:N1} MB）" -f $Out, ((Get-Item $Out).Length / 1MB)) } else { Write-Host "[録画] MP4 が出来なかった" }
