# dev.ps1 — ビルド・検査・計測の入口を 1 本にまとめる。
#
#   使い方（プロジェクト直下で）:
#     powershell -ExecutionPolicy Bypass -File tools/dev.ps1 build [ターゲット...]   既定: AcousticEngine SceneRegressionTest AfPatternTable
#     powershell -ExecutionPolicy Bypass -File tools/dev.ps1 test  [scene|dsp|btm|all]  既定: scene
#     powershell -ExecutionPolicy Bypass -File tools/dev.ps1 table [csv] [AF_GAPW=1.40 AF_LDIST=0.5 AF_SWEEP=1 ...]
#     powershell -ExecutionPolicy Bypass -File tools/dev.ps1 pdf                      資料の PDF を作る
#     powershell -ExecutionPolicy Bypass -File tools/dev.ps1 pptx                     資料の PPTX を作る（PowerPoint が要る）
#     powershell -ExecutionPolicy Bypass -File tools/dev.ps1 csharp                   Unity を開かずに C# を型検査
#     powershell -ExecutionPolicy Bypass -File tools/dev.ps1 deploy                   DLL を UnityDemo へ配備
#
# ★なぜ 1 本にまとめるか
#   Claude Code の許可規則は**コマンドの先頭一致**で効く。cmake の絶対パスや環境変数の
#   代入で始まる複数行のスクリプトは毎回別物と見なされ、そのたびに確認が出ていた
#   （settings.local.json が 434 KB に膨らんでいたのがその痕跡）。
#   入口をこのファイルに固定すれば `tools/dev.ps1 *` の 1 規則で全部通る。
#
# ★このファイルは UTF-8 BOM 付きで保存すること（PS 5.1 は BOM 無しを ANSI と読む）。
param(
  [Parameter(Position = 0)] [string]$Cmd = "help",
  [Parameter(Position = 1, ValueFromRemainingArguments = $true)] [string[]]$Rest = @()
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
Set-Location $root

# cmake は VS 同梱のものを使う（PATH には無い）。
$cmake = [System.IO.Path]::Combine(${env:ProgramFiles}, "Microsoft Visual Studio", "2022", "Community",
           "Common7", "IDE", "CommonExtensions", "Microsoft", "CMake", "CMake", "bin", "cmake.exe")
if (-not (Test-Path $cmake)) { $cmake = "cmake" }
$bin = Join-Path $root "build\bin\Release"

function Build([string[]]$targets) {
  if (-not $targets -or $targets.Count -eq 0) { $targets = @("AcousticEngine", "SceneRegressionTest", "AfPatternTable") }
  $args = @("--build", "build", "--config", "Release")
  foreach ($t in $targets) { $args += @("--target", $t) }
  & $cmake @args 2>&1 | Where-Object { $_ -match 'error|warning C4|->' }
  if ($LASTEXITCODE -ne 0) { throw "ビルド失敗 (exit $LASTEXITCODE)" }
}

# ⚠ exe の標準出力をパイプに乗せたまま return すると、呼び出し側の `| Out-Null` で
#   **出力ごと捨てられる**（table の結果が空になった）。出力は素通しにして、終了コードだけ見る。
function RunExe([string]$name, [string[]]$exeArgs) {
  $exe = Join-Path $bin $name
  if (-not (Test-Path $exe)) { throw "見つかりません: $exe（先に build）" }
  & $exe @exeArgs
  if ($LASTEXITCODE -ne 0) { Write-Warning "$name exit $LASTEXITCODE" }
}

switch ($Cmd) {
  "build" { Build $Rest }

  "test" {
    $which = if ($Rest.Count -gt 0) { $Rest[0] } else { "scene" }
    $map = @{ scene = "SceneRegressionTest.exe"; dsp = "DspRegressionTest.exe"; btm = "BtmRegressionTest.exe" }
    $list = if ($which -eq "all") { @("scene", "dsp", "btm") } else { @($which) }
    $fail = 0
    foreach ($k in $list) {
      if (-not $map.ContainsKey($k)) { throw "test の引数は scene|dsp|btm|all" }
      $out = & (Join-Path $bin $map[$k]) 2>&1 | ForEach-Object { "$_" }
      $code = $LASTEXITCODE
      # 落ちた行と集計行だけを出す（全文は長い）。
      $out | Where-Object { $_ -match '\[FAIL\]|件のチェック' }
      if ($code -ne 0) { $fail++ }
    }
    if ($fail -gt 0) { exit 1 }
  }

  "table" {
    # 引数のうち NAME=VALUE は環境変数（AF_*）に、それ以外は exe の引数（csv 等）に渡す。
    $exeArgs = @()
    foreach ($a in $Rest) {
      if ($a -match '^([A-Za-z_][A-Za-z0-9_]*)=(.*)$') { Set-Item -Path ("Env:" + $Matches[1]) -Value $Matches[2] }
      else { $exeArgs += $a }
    }
    RunExe "AfPatternTable.exe" $exeArgs
  }

  "pdf"    { & powershell -ExecutionPolicy Bypass -File (Join-Path $root "portfolio\build.ps1") }
  "pptx"   { & powershell -ExecutionPolicy Bypass -File (Join-Path $root "portfolio\build_pptx.ps1") @Rest }
  "csharp" { & powershell -ExecutionPolicy Bypass -File (Join-Path $root "tools\check_csharp.ps1") @Rest }

  "deploy" {
    $src = Join-Path $bin "AcousticEngine.dll"
    $dst = Join-Path $root "UnityDemo\Assets\Plugins\x86_64\AcousticEngine.dll"
    Copy-Item $src $dst -Force
    "配備: $dst"
  }

  default {
    "dev.ps1 build|test|table|pdf|pptx|csharp|deploy  （先頭のコメントを参照）"
  }
}
