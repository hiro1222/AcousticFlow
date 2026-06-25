# build_and_deploy_unity.ps1
# AcousticEngine.dll をビルドして Unity の Plugins フォルダへコピーする。
#
# 使い方:  powershell -ExecutionPolicy Bypass -File build_and_deploy_unity.ps1
#
# 注意（重要なハマりどころ）:
#   Unity エディタが Play 等でネイティブ DLL を読み込むと、その DLL を
#   ロックして上書きできなくなる。コピーが失敗する場合は、いったん
#   Unity エディタを閉じてから再実行すること。
$ErrorActionPreference = "Stop"
$root = $PSScriptRoot

# 1) DLL をビルド（Debug）。
& cmake --build (Join-Path $root "build") --config Debug
if (-not $?) { throw "DLL のビルドに失敗しました。" }

# 2) Unity の Plugins へコピー。
$src = Join-Path $root "build\bin\Debug\AcousticEngine.dll"
$dstDir = Join-Path $root "UnityDemo\Assets\Plugins\x86_64"
if (-not (Test-Path $dstDir)) { New-Item -ItemType Directory -Force -Path $dstDir | Out-Null }
$dst = Join-Path $dstDir "AcousticEngine.dll"

try {
    Copy-Item -Path $src -Destination $dst -Force
    Write-Host "[OK] コピー完了: $dst"
}
catch {
    Write-Host "[FAIL] コピーできませんでした。Unity エディタが DLL をロックしている可能性があります。"
    Write-Host "       Unity を閉じてから再実行してください。詳細: $($_.Exception.Message)"
    exit 1
}

