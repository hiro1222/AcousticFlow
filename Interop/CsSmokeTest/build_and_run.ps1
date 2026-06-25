# build_and_run.ps1
# C# 疎通テスト(CsSmokeTest)をビルドして実行する。
# .NET SDK が無い環境でも動くよう、VS 同梱の Roslyn csc を使う。
# 出力先は DLL と同じ build\bin\Debug\ なので、実行時に DLL を自動で見つけられる。
#
# 使い方:  powershell -ExecutionPolicy Bypass -File Interop\CsSmokeTest\build_and_run.ps1

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)  # リポジトリ直下

# Roslyn csc を探す（モダン C# 対応。Framework の csc は C# 5 までなので使わない）。
$csc = Get-ChildItem "C:\Program Files\Microsoft Visual Studio\2022\*\MSBuild\Current\Bin\Roslyn\csc.exe" -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $csc) { throw "Roslyn csc.exe が見つかりません。Visual Studio 2022 を確認してください。" }

$outExe = Join-Path $root "build\bin\Debug\CsSmokeTest.exe"
$src    = Join-Path $PSScriptRoot "Program.cs"

# /platform:x64 で x64 DLL とビット数を揃える（不一致は BadImageFormatException の原因）。
& $csc.FullName /nologo /platform:x64 "/out:$outExe" "$src"
if (-not $?) { throw "コンパイルに失敗しました。" }

Write-Host "[compile OK] -> $outExe"
Write-Host "--- 実行 ---"
& $outExe
Write-Host "exit code: $LASTEXITCODE"

