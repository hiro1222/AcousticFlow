# 別の PC で動かす手順（NKN と AcousticFlow）

2026-10-10。git で持っていった後、新しい PC で Unreal・Wwise・Unity の試聴台を動かすまで。
同じ物が `C:\dev\NKN\docs\` と `C:\dev\AcousticFlow\docs\` にある。

## 0. git に入っていない物（向こうで入れ直す）

| 物 | 大きさ | 入れ直し方 |
|---|---|---|
| Wwise の Unreal 統合（`Plugins/Wwise*`、SDK の `ThirdParty`） | 約 4 GB | 3. で Audiokinetic Launcher から |
| Wwise の Unity 統合（`Projects/UnityDemo/Assets/Wwise`） | 約 0.8 GB | 7. で Audiokinetic Launcher から |
| Unreal のキャラクター素材（`Content/Characters`） | 126 MB | 4. でエンジンのテンプレートから写す |
| ビルドの生成物（`build/`・`Binaries`・`Intermediate` など） | ― | 組み立て直す |
| 前の試験の音（DemonDanceTokyo_Eve・videoplayback） | ― | 著作物なので持っていかない。試験の音は **FreeTestSound.wav**（フリー音源、git に入っている）に替えた。Wwise の画面で前の素材が「見つからない」と出るのは無視してよい |

## 1. 入れる物（新しい PC）

| 物 | 版 | メモ |
|---|---|---|
| Git for Windows | 2.5x | Git Bash も一緒に入る（`verify.sh` に使う） |
| Visual Studio 2022 Community | 17.10 以降 | 「C++ によるデスクトップ開発」と「C++ によるゲーム開発」。CMake も一緒に入る |
| Epic Games Launcher → Unreal Engine | **5.6** | |
| Audiokinetic Launcher → Wwise | **2025.1.8.9170** | Authoring と SDK（C++、Windows の vc170）。Unreal と Unity の統合もこの Launcher から |
| Python | 3.12 | 「PATH に足す」を入れる |
| Unity Hub → Unity | **6000.0.71f1** | AcousticFlow の Unity の試聴台を使うときだけ |

## 2. 取ってくる（同じ場所に置く）

スクリプトの中に `C:\dev\NKN`・`C:\dev\AcousticFlow` が書いてあるので、**同じ場所**に clone する。

```bash
git clone <AcousticFlow の URL> /c/dev/AcousticFlow
git clone <NKN の URL> /c/dev/NKN
```

AcousticFlow は `git checkout acoustic-scene-core`（いつもの作業の枝）。

## 3. Wwise を Unreal のプロジェクトに入れる（2 つとも）

Audiokinetic Launcher → Unreal Engine → 「Integrate Wwise into Project」で、

- `C:\dev\NKN\Projects\NKNDemo\NKNDemo.uproject`
- `C:\dev\AcousticFlow\Projects\UnrealDemo\AcousticFlowUE.uproject`

を 1 つずつ。版は 2025.1.8.9170、Wwise のプロジェクトは「既にある物を使う」で
`..\NKNDemo_WwiseProject\NKNDemo_WwiseProject.wproj`・`..\UnrealDemo_WwiseProject\UnrealDemo_WwiseProject.wproj`。
★Launcher が `Config/DefaultGame.ini` の Wwise の欄を書き換えたら、`git diff` で見て、`WwiseProjectPath` と `RootOutputPath` が元のままか確かめる。

## 4. キャラクター素材を写す（2 つとも）

`C:\Program Files\Epic Games\UE_5.6\Templates\TemplateResources\High\Characters\Content\` の中身（`Mannequins`）を、
`Projects\NKNDemo\Content\Characters\`・`Projects\UnrealDemo\Content\Characters\` へ写す。

## 5. NKN を組む

1. エンジン（Git Bash で）: `bash /c/dev/NKN/verify.sh` → NKN 99 件・NKN DSP 27 件が合格すればよい。
2. Wwise のプラグイン NKNRender（PowerShell で）:
   ```powershell
   $env:PYTHONUTF8 = "1"
   $env:PATH = "C:\Program Files (x86)\Microsoft Visual Studio\Installer;" + $env:PATH
   $wp = "C:\Audiokinetic\Wwise_2025.1.8.9170\Scripts\Build\Plugins\wp.py"
   cd C:\dev\NKN\WwisePlugin\NKNRender
   python $wp premake Windows_vc170
   python $wp build Windows_vc170 -c Release -x x64
   python $wp build Windows_vc170 -c Profile -x x64
   python $wp premake Authoring
   python $wp build Authoring -c Release -x x64 -t vc170
   ```
   できた `C:\Audiokinetic\Wwise_2025.1.8.9170\SDK\x64_vc170\{Release,Profile}\bin\NKNRender.dll` を
   `C:\dev\NKN\Projects\NKNDemo\Plugins\WwiseSoundEngine\ThirdParty\x64_vc170\{Release,Profile}\bin\` へ写す。
   （画面用の部品で組めない所が出たら、`WwisePlugin/_unused` と同じく Win32 の画面のフォルダを脇へ退ける）
3. Wwise の形とバンク（Wwise の画面は閉じて）: `python C:\dev\NKN\tools\nkn_wwise_setup.py`
4. Unreal（エディタは全部閉じて）:
   ```powershell
   & "C:\Program Files\Epic Games\UE_5.6\Engine\Build\BatchFiles\Build.bat" NKNDemoEditor Win64 Development "-Project=C:\dev\NKN\Projects\NKNDemo\NKNDemo.uproject" -WaitMutex
   ```
5. `NKNDemo.uproject` を開いて、地図 `Flow_SwingDoor`（扉の部屋）か `Cave_ThirdPerson`（洞窟）で Play。
   ログ（Output Log の `LogNKN`）に `reverb render calls … matched 1` と `direct calls … matched 1` が出ていれば、Wwise まで通っている。
   ★この PC では NKN の Wwise の SDK（ThirdParty）が AcousticFlow の物を指していた（ジャンクション）。新しい PC では 3. で NKN にも自分の物が入るので要らない。

## 6. AcousticFlow を組む（Unreal）

1. エンジン（PowerShell、`C:\dev\AcousticFlow` で）:
   ```powershell
   & "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" -S . -B build -G "Visual Studio 17 2022" -A x64
   powershell -ExecutionPolicy Bypass -File tools\dev.ps1 build
   powershell -ExecutionPolicy Bypass -File tools\dev.ps1 test all
   ```
2. Wwise のプラグイン AcousticFlow: `WwisePlugin\AcousticFlow` で 5.-2. と同じ `premake`（Windows_vc170・Authoring）をしてから、
   `powershell -ExecutionPolicy Bypass -File tools\dev.ps1 wwiseplugin`（組んで Unreal と Unity へ配る）と、
   `python $wp build Authoring -c Release -x x64 -t vc170`（Wwise の画面用）。
3. Wwise の形とバンク: `python tools\wwise_setup.py unreal`（Unity 用は `unity`）。
4. Unreal: `Build.bat AcousticFlowUEEditor Win64 Development "-Project=C:\dev\AcousticFlow\Projects\UnrealDemo\AcousticFlowUE.uproject" -WaitMutex`
5. `AcousticFlowUE.uproject` を開いて Play。画面の 2 行目とログ（`[AcousticFlow]`）に `matched 1 / missed 0`。

## 7. AcousticFlow を組む（Unity、使うときだけ）

1. Unity Hub で `C:\dev\AcousticFlow\Projects\UnityDemo` を 6000.0.71f1 で開く（初回は Library を作るので時間がかかる）。
2. Audiokinetic Launcher → Unity → 「Integrate Wwise into Project」で 2025.1.8.9170、Wwise のプロジェクトは `..\UnityDemo_WwiseProject`。
3. `tools\dev.ps1 deploy`（エンジンの DLL を Unity へ）と `tools\dev.ps1 wwiseplugin`（自前のプラグインを Unity の Wwise へ）。
4. 場面 `Flow_SwingDoor`・`Flow_RoomWalk` を開いて Play。音は FreeTestSound.wav。

## 8. Claude Code を向こうでも使うなら

この PC の Claude の記憶は `C:\Users\<名前>\.claude\projects\C--dev-AcousticFlow\memory\` にある。
同じ場所（`C:\dev\AcousticFlow`）で開くなら、このフォルダを向こうの同じ場所へ写せば続きから使える（git には入っていない）。

## うまくいかないとき

| 症状 | 見る所 |
|---|---|
| Build.bat が「Live Coding が動いている」で断る | 開いている Unreal のエディタを全部閉じる |
| Unreal を開くと「AcousticEngine.lib が無い」 | 先にエンジンを組む（5.-1.／6.-1.） |
| 音が鳴らない・`matched 0` | Wwise のバス（NKN_Direct・NKN_Reverb／AcousticFlow）にプラグインが差さっているか、プラグインの DLL が `ThirdParty\x64_vc170\Profile\bin` にあるか |
| Wwise の画面で「プラグインが見つからない」 | Authoring 用の DLL（`Authoring\x64\Release\bin\Plugins\NKNRender.dll`・`AcousticFlow.dll`）を組んだか |
| `.ps1` が文字化けして動かない | UTF-8（BOM 付き）のまま保存されているか |
