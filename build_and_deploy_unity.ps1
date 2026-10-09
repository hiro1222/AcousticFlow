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

# 0) cmake を探す。
#    この開発機の cmake は Visual Studio 同梱で、PATH に入っていない
#    （素の PowerShell で叩くと CommandNotFoundException になる）。
#    PATH → VS 同梱 → 単体インストール の順に探して、最初に見つかったものを使う。
$cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
if (-not $cmake) {
    $candidates = @(
        "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
        "C:\Program Files\Microsoft Visual Studio\2022\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
        "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
        "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
        "C:\Program Files\CMake\bin\cmake.exe"
    )
    foreach ($c in $candidates) { if (Test-Path $c) { $cmake = $c; break } }
}
if (-not $cmake) {
    throw "cmake が見つかりません。Visual Studio 2022（C++ ワークロード）か CMake を入れるか、PATH を通してください。"
}
Write-Host "[cmake] $cmake"

# 1) DLL をビルド（Release）。
#    Release CRT は再頒布可能なので、Wwise非依存(既定 AF_USE_WWISE=OFF)の DLL は
#    開発ツールの無い PC でもそのまま動く。Wwise 連携を戻すときは再構成時に
#    cmake -S . -B build -DAF_USE_WWISE=ON を指定してから本スクリプトを実行。
& $cmake --build (Join-Path $root "build") --config Release
if (-not $?) { throw "DLL のビルドに失敗しました。" }

# 2) Unity の Plugins へコピー。
$src = Join-Path $root "build\bin\Release\AcousticEngine.dll"
$dstDir = Join-Path $root "Projects\UnityDemo\Assets\Plugins\x86_64"
if (-not (Test-Path $dstDir)) { New-Item -ItemType Directory -Force -Path $dstDir | Out-Null }
$dst = Join-Path $dstDir "AcousticEngine.dll"

try {
    Copy-Item -Path $src -Destination $dst -Force
    Write-Host "[OK] コピー完了: $dst"
}
catch {
    Write-Host "[FAIL] コピーできませんでした。Unity エディタが DLL をロックしている可能性があります。"
    Write-Host "       Unity を閉じてから再実行してください。詳細: $($_.Exception.Message)"
    Write-Host "       ※ビルド自体は成功しています。新しい DLL はここにあります:"
    Write-Host "         $src"
    exit 1
}

