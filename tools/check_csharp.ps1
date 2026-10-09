# check_csharp.ps1 ── Unity を開かずに C# をコンパイル検査する。
#
# 使い方:
#   powershell -ExecutionPolicy Bypass -File tools/check_csharp.ps1
#   powershell -ExecutionPolicy Bypass -File tools/check_csharp.ps1 -SelfTest
#
# ★なぜ要るか
#   このレーンの C# は Unity を開くまで検査されていなかった。実際に何度も、
#   コンパイルの通らない C# を「出来た」と報告している
#   （Native の書き間違い／Tooltip に生の改行／Editor から internal を参照）。
#
# ★★ この検査自体が 2 回 **偽の合格** を出した。その対策が入っている ★★
#
#   (a) アセンブリを分けていなかった
#       Unity は `Editor/` を**別アセンブリ**にする。1 つにまとめると `internal` が
#       見えてしまい、CS0122 を 33 件見逃した。→ ランタイム → Editor の 2 段にする。
#
#   (b) エラーの拾い方が甘かった
#       `": error "` で拾っていたので、**位置情報の無いエラー**（CS1705 など、
#       ファイル名も行番号も付かない）を取りこぼし、失敗を成功と報告した。
#       → **csc の終了コードを第一の判定にする**。文字列は表示用に使うだけ。
#
#   (c) 検査が効いているかを検査していなかった
#       → `-SelfTest` で**わざと壊した入力**を通し、ちゃんと落ちることを確かめる。
#         （型紙の自己検査と同じ考え方: 黙っているのが健全だからか、
#           壊れて何も見ていないからか、区別できないと意味がない）
#
# ⚠ これは型検査まで。Unity のシリアライズやシーン参照は見ない。
param([switch]$SelfTest)
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent

$verFile = Join-Path $root "Projects\UnityDemo\ProjectSettings\ProjectVersion.txt"
if (-not (Test-Path $verFile)) { throw "ProjectVersion.txt が見つかりません" }
$ver = (Select-String -Path $verFile -Pattern "^m_EditorVersion:\s*(\S+)").Matches[0].Groups[1].Value
$ue = "C:\Program Files\Unity\Hub\Editor\$ver\Editor\Data"
if (-not (Test-Path $ue)) { throw "Unity $ver が見つかりません: $ue" }

$dotnet = Join-Path $ue "NetCoreRuntime\dotnet.exe"
$csc    = Join-Path $ue "DotNetSdkRoslyn\csc.dll"
if (-not (Test-Path $csc)) { throw "Roslyn が見つかりません: $csc" }

# ⚠ BCL は **Unity が実際に使う profile**（unityjit-win32）を参照すること。
#   ここを間違えると本物のコードで偽のエラーが出る:
#     ・4.7.1-api の Facades\netstandard.dll は 2.0。UnityEngine のモジュールは 2.1 を
#       要求するので CS1705（★このエラーは**位置情報が付かない**）。
#     ・4.7.1-api の mscorlib と netstandard 2.1 を混ぜると CS7069（ReadOnlySpan が無い）。
#   → 混ぜない。unityjit-win32 の一式でそろえる。
$mono = Join-Path $ue "MonoBleedingEdge\lib\mono"
$prof = Join-Path $mono "unityjit-win32"
if (-not (Test-Path (Join-Path $prof "mscorlib.dll"))) { $prof = Join-Path $mono "unityaot-win32" }

$baseRefs = @(
    "/r:`"$prof\mscorlib.dll`"",
    "/r:`"$prof\System.dll`"",
    "/r:`"$prof\System.Core.dll`""
)
$nsProf = Join-Path $prof "Facades\netstandard.dll"
if (Test-Path $nsProf) { $baseRefs += "/r:`"$nsProf`"" }
$baseRefs += (Get-ChildItem (Join-Path $ue "Managed\UnityEngine\*.dll") |
              ForEach-Object { "/r:`"$($_.FullName)`"" })
# ⚠ UnityEditor.dll（ファサード）は足さない。Managed\UnityEngine\ に
#   UnityEditor.CoreModule.dll などの**実体**が既に入っていて、両方参照すると
#   EditorWindow が二重定義になる（UnityEngine 側とまったく同じ罠）。
$editorRefs = @()

$common = @("/nologo", "/target:library", "/nostdlib+", "/codepage:65001", "/utf8output",
            "/langversion:9.0", "/nowarn:0169,0414,0649,0067")

$script:Warnings = @()

# csc を回す。★合否は**終了コード**で決める（文字列一致に頼らない）。
function Invoke-Csc([string]$what, [string[]]$argv, [switch]$Quiet) {
    $log = & $dotnet $csc $argv 2>&1
    $code = $LASTEXITCODE
    $errs  = @($log | Where-Object { $_ -match "error CS" })
    $warns = @($log | Where-Object { $_ -match "warning CS" })
    $script:Warnings += $warns
    if ($code -ne 0) {
        if (-not $Quiet) {
            Write-Host "[C# NG] $what : 終了コード $code / エラー $($errs.Count) 件"
            if ($errs) { $errs | ForEach-Object { Write-Host "  $_" } }
            else { $log | Select-Object -First 10 | ForEach-Object { Write-Host "  $_" } }
        }
        return $false
    }
    if (-not $Quiet) { Write-Host "[C# OK] $what : エラーなし（警告 $($warns.Count) 件）" }
    return $true
}

$tmp = Join-Path $env:TEMP ("afcheck_" + [System.Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $tmp | Out-Null
try {
    # ── 0) 検査そのものの検査 ──────────────────────────────────
    if ($SelfTest) {
        Write-Host "[自己検査] わざと壊した入力で、ちゃんと落ちるか"
        $bad = Join-Path $tmp "Bad.cs"
        Set-Content -Path $bad -Encoding utf8 -Value @'
namespace AcousticFlow { internal static class Hidden { public static int X = 1; } }
'@
        $bad2 = Join-Path $tmp "Bad2.cs"
        Set-Content -Path $bad2 -Encoding utf8 -Value @'
public static class Peek { public static int Y = AcousticFlow.Hidden.X; }
'@
        $lib = Join-Path $tmp "hidden.dll"
        $ok1 = Invoke-Csc "自己検査(下ごしらえ)" ($common + @("/out:$lib") + $baseRefs + @("`"$bad`"")) -Quiet
        $ok2 = Invoke-Csc "自己検査(internal を跨ぐ)" `
               ($common + @("/out:$tmp\peek.dll", "/r:`"$lib`"") + $baseRefs + @("`"$bad2`"")) -Quiet
        if ($ok1 -and -not $ok2) { Write-Host "  [OK] 別アセンブリの internal をちゃんと弾いた" }
        else { Write-Host "  [NG] 検査が効いていません（下ごしらえ=$ok1 / 弾き=$ok2）"; exit 2 }

        $syn = Join-Path $tmp "Syn.cs"
        Set-Content -Path $syn -Encoding utf8 -Value @'
public static class Broken { public static string S = "閉じていない
"; }
'@
        $ok3 = Invoke-Csc "自己検査(生の改行)" `
               ($common + @("/out:$tmp\syn.dll") + $baseRefs + @("`"$syn`"")) -Quiet
        if (-not $ok3) { Write-Host "  [OK] 文字列リテラルの生の改行をちゃんと弾いた" }
        else { Write-Host "  [NG] 生の改行を通してしまいました"; exit 2 }
        Write-Host ""
    }

    # ── 1) 本番 ──────────────────────────────────────────────
    $srcDir = Join-Path $root "Projects\UnityDemo\Assets\Scripts\AcousticFlow"
    $all = Get-ChildItem "$srcDir\*.cs" -Recurse
    # Unity の分け方に合わせる: パスに \Editor\ を含むものが Editor アセンブリ。
    $editorSrc  = @($all | Where-Object { $_.FullName -match "\\Editor\\" })
    $runtimeSrc = @($all | Where-Object { $_.FullName -notmatch "\\Editor\\" })
    if ($runtimeSrc.Count -eq 0) { throw "検査する .cs がありません: $srcDir" }
    Write-Host "[C#] Unity $ver / ランタイム $($runtimeSrc.Count) ファイル・Editor $($editorSrc.Count) ファイル"

    $runtimeDll = Join-Path $tmp "AcousticFlow.Runtime.dll"
    $ok = Invoke-Csc "ランタイム" `
          ($common + @("/out:$runtimeDll") + $baseRefs +
           ($runtimeSrc | ForEach-Object { "`"$($_.FullName)`"" }))
    if (-not $ok) { exit 1 }

    if ($editorSrc.Count -gt 0) {
        # ★ランタイムを**参照して**組む。ここで初めて internal が隠れる。
        $ok = Invoke-Csc "Editor" `
              ($common + @("/out:$tmp\AcousticFlow.Editor.dll", "/r:`"$runtimeDll`"") +
               $baseRefs + $editorRefs +
               ($editorSrc | ForEach-Object { "`"$($_.FullName)`"" }))
        if (-not $ok) { exit 1 }
    }

    Write-Host "[C# OK] 全体: エラーなし（警告 $($script:Warnings.Count) 件）"
    $script:Warnings | Select-Object -First 8 | ForEach-Object { Write-Host "  $_" }
    exit 0
}
finally {
    Remove-Item $tmp -Recurse -Force -ErrorAction SilentlyContinue
}
