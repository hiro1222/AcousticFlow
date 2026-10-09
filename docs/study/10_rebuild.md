# 10 作り直しの手順

別のフォルダに、同じ物をもう一度作る手順です。**下の層から 1 段ずつ写して、段ごとに検査を通す** 形にしてあります。
どこかで壊れても、壊れた所が 1 段に絞れます。

- **段 0〜3（エンジン）**：この文書を書いたとき（2026-10-07）に、別フォルダでこの通りに回して全部通ることを確かめました。
  DSP 160/160、Flow 196/196、台帳 10/10。出来た DLL は 158 個の関数を出し、Unreal・Wwise・Unity が呼ぶ 108 個が全部入っています
  （本家の DLL は旧コアの分も含めて 318 個）。
- **段 4〜6（コネクタ層）**：本家で実際に使っている手順を並べた物です。作り直し先では回していません（Wwise と Unreal の入れ物が大きいため）。

写す代わりに、**ファイルを自分で書いてもかまいません**。その段の検査が通れば正解です（正解を人に聞かずに判定できる）。
詰まったら、本家のファイルと見比べてください。

```
 段 0  入れ物（CMake）
 段 1  音を作る部品（Dsp/）                 → DspRegressionTest  160 件
 段 2  形・部屋・音の届き方・配分（Core/ Flow/ Gpu/）→ FlowRegressionTest 196 件
 段 3  C の窓口と DLL（include/ Export/）     → HostRegressionTest  10 件
 段 4  Wwise のプラグインとプロジェクト
 段 5  Unreal
 段 6  Unity
```

---

## 段 0：準備

### 要る物

| 物 | 版 | 使う段 |
|---|---|---|
| Visual Studio 2022（C++ によるデスクトップ開発。Unreal ならゲーム開発の C++ も） | 17.x | 全部 |
| CMake | VS に同梱の物で足りる | 1〜3 |
| Git Bash | | 写す（`copy_step.sh`） |
| Python 3 | | 4 |
| Wwise（Launcher で SDK の Windows vc170 と Authoring を入れる） | 2025.1.8.9170 | 4〜6 |
| Unreal Engine | 5.6 | 5 |
| Unity | 6000.0.71f1 | 6 |

### 入れ物を作る

Git Bash で（作り直し先を `/c/dev/AcousticFlowRebuild` とした例）：

```bash
R=/c/dev/AcousticFlowRebuild
S=/c/dev/AcousticFlow
CMAKE="/c/Program Files/Microsoft Visual Studio/2022/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe"
mkdir -p $R/AcousticEngineTest
cp $S/docs/study/rebuild/CMakeLists.txt $R/
cp $S/docs/study/rebuild/AcousticEngineTest/CMakeLists.txt $R/AcousticEngineTest/
```

- 根の CMake：C++17、`/utf-8`（★無いとソースの日本語が cp932 で読まれて壊れる）、出来上がりを `build/bin` に集める。
  `AcousticEngine/CMakeLists.txt` がまだ無ければ DLL を作らない（段 1・2 は検査の exe だけで確かめる）。
- 検査の CMake：写したファイルがある検査だけを作る。段ごとに同じ 1 枚で済む。

---

## 段 1：音を作る部品（DSP）

DSP は Core も Flow も知らない、いちばん下の層です。先に読む：[05_engine_dsp.md](05_engine_dsp.md)

```bash
bash $S/docs/study/rebuild/copy_step.sh 1 $R
"$CMAKE" -S $R -B $R/build -G "Visual Studio 17 2022" -A x64
"$CMAKE" --build $R/build --config Release --target DspRegressionTest
$R/build/bin/Release/DspRegressionTest.exe | tail -1
```

写す物（14 本）：`Dsp/` の 12 本、`Debug/detectors.h`（検査が使う破れの型紙）、`AcousticEngineTest/dsp_regression.cpp`。

**通ったら**：`[OK] 160 件のチェックすべてに合格しました。`

自分で書く順の目安：`fft.h` → `partitioned_convolver.h` → `nonuniform_convolver.h` → `hrtf_set.h` → `hrtf_processor.h`
→ `early_reflect_conv.h` → `direction_bus.h` → `fdn_tail.h` → `fdn_room_mix.h` → `reverb_tail_ir.h` → `tail_bus.h` → `voice_renderer.h`

---

## 段 2：形・部屋・音の届き方・配分

先に読む：[02](02_engine_geometry.md)・[03](03_engine_propagation.md)・[04](04_engine_distribute_frame.md)

```bash
bash $S/docs/study/rebuild/copy_step.sh 2 $R
"$CMAKE" -S $R -B $R/build
"$CMAKE" --build $R/build --config Release --target FlowRegressionTest
$R/build/bin/Release/FlowRegressionTest.exe | tail -1
```

写す物（33 本）：`Core/` の 7 本（`vec3.h` `aabb.h` `material.h` `material.cpp` `worker_pool.h` `room_graph.h` `maekawa.h`）、
`Flow/` の 20 本、`Gpu/` の 4 本、`flow_regression.cpp` と `test_instruments.h`。

- ★`cmake -S -B` を回し直すこと（検査の CMake は、構成するときに `flow_regression.cpp` があるかを見るため）。
- 検査は DLL を使わず、エンジンのソースをそのまま読みます（`material.cpp` と `compute_d3d11.cpp` だけ一緒にコンパイル）。
- 全部回るのに数分かかります。1 つだけ回すときは `AF_ONLY=名前`（例：`AF_ONLY=shadowmuffle`）。

**通ったら**：`[OK] Flow: 196 件のチェックすべてに合格しました。`

自分で書く順の目安：`world_rules.h` → `emitter.h` → `surfaces.h` → `surface_bvh.h` → `trace_scene.h` → `room_graph.h` → `probe.h`
→ `budget.h` → `energy_trace.h` → `aperture.h` → `maekawa.h` → `diffraction.h` → `image_sources.h` → `image_surface.h`
→（`receiver_layout.h` `receiver.h` `image_lattice.h` は写すだけでよい）→ `mix.h` → `response.h` → `distribute.h` → `mix_to_voice.h`
→ `Gpu/` → `world.h`

---

## 段 3：C の窓口と DLL

先に読む：[06_c_api.md](06_c_api.md)

```bash
bash $S/docs/study/rebuild/copy_step.sh 3 $R
mkdir -p $R/AcousticEngine && cp $S/docs/study/rebuild/AcousticEngine/CMakeLists.txt $R/AcousticEngine/
"$CMAKE" -S $R -B $R/build
"$CMAKE" --build $R/build --config Release --target AcousticEngine HostRegressionTest
$R/build/bin/Release/HostRegressionTest.exe | tail -1
```

写す物（8 本）：公開ヘッダ 4 枚（`acoustic_scene.h` は `ACOUSTIC_API` と `AF_Vector3` のためだけ）、`Export/` の 3 本、`host_regression.cpp`。
エンジンの CMake は本家から旧コア（`scene_api.cpp`）を抜いた物です。

```cmake
set(ACOUSTIC_SOURCES
    src/Export/world_api.cpp  src/Export/voice_api.cpp  src/Export/host_api.cpp
    src/Core/material.cpp     src/Gpu/compute_d3d11.cpp)
add_library(AcousticEngine SHARED ${ACOUSTIC_SOURCES} ${ACOUSTIC_HEADERS})
target_include_directories(AcousticEngine PUBLIC include)
target_include_directories(AcousticEngine PRIVATE src)
target_compile_definitions(AcousticEngine PRIVATE ACOUSTICENGINE_EXPORTS)   # dllexport を有効に
target_link_libraries(AcousticEngine PRIVATE d3d11 d3dcompiler dxgi)
```

**通ったら**：`[OK] 台帳: 10 件のチェックすべてに合格しました。` と、`$R/build/bin/Release/AcousticEngine.dll`（と `.lib`）。

出している関数を見るとき（任意）：

```bash
"$(ls -d "/c/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/MSVC/"*/bin/Hostx64/x64/dumpbin.exe | tail -1)" //exports $R/build/bin/Release/AcousticEngine.dll | grep -c " AF_"
```

（158 と出れば本家の作業ツリーと同じ。）

ここまででエンジンは完成です。

---

## 段 4：Wwise のプラグインとプロジェクト

先に読む：[07_connector_wwise.md](07_connector_wwise.md)

### プラグイン

```bash
cp -r $S/WwisePlugin $R/
cd $R/WwisePlugin/AcousticFlow
export PYTHONUTF8=1
WP="/c/Audiokinetic/Wwise_2025.1.8.9170/Scripts/Build/Plugins/wp.py"
python "$WP" premake Windows_vc170
python "$WP" build Windows_vc170 -c Release -x x64
python "$WP" build Windows_vc170 -c Profile -x x64
python "$WP" premake Authoring
python "$WP" build Authoring -c Release -x x64 -t vc170
```

- 出来上がりは Wwise の置き場（`C:\Audiokinetic\Wwise_2025.1.8.9170\SDK\x64_vc170\<構成>\bin\AcousticFlow.dll` と `Authoring\x64\Release\bin\Plugins\`）に入ります。
  ★**本家と同じ名前・同じ ID なので、置き場は本家と共有**です（後から作った方が入る）。本家に戻るときは本家で作り直してください。
- `PremakePlugin.lua` が `../../../AcousticEngine/include` を読むので、`WwisePlugin/AcousticFlow` はリポジトリの根から同じ位置に置く。
- ★`vswhere.exe` が見つからないと止まる。`C:\Program Files (x86)\Microsoft Visual Studio\Installer` を PATH に足す。

### Wwise プロジェクト

本家の `Projects/UnrealDemo_WwiseProject` を写し（`GeneratedSoundBanks/` `.cache/` `*.wsettings` `*.validationcache` は写さない）、
組み立ての道具で同じ形にします。

```bash
mkdir -p $R/tools $R/Projects
cp $S/tools/wwise_setup.py $S/tools/wwise_waapi.py $R/tools/
cp -r $S/Projects/UnrealDemo_WwiseProject $R/Projects/
rm -rf $R/Projects/UnrealDemo_WwiseProject/GeneratedSoundBanks $R/Projects/UnrealDemo_WwiseProject/.cache
mkdir -p $R/Projects/UnityDemo/Assets/Audio && cp <鳴らしたい WAV> $R/Projects/UnityDemo/Assets/Audio/DemonDanceTokyo_Eve.wav
python $R/tools/wwise_setup.py unreal
```

- `wwise_setup.py` はバス `AcousticFlow`（Audio Objects）・エフェクト `AF_Renderer`（1 枠だけ）・音 `AF_TestTone`・Event `Play_AF_Test`・設定 10 個を組み、バンクを作ります。
- 素材の WAV は 16 bit PCM。長さの欄が壊れた WAV（`data` の長さが 0xFFFFFFFF など）は、正しい長さに直してから入れる。
- ★Wwise の画面で同じプロジェクトを開いたまま回さない（保存がぶつかる）。

---

## 段 5：Unreal

先に読む：[08_connector_unreal.md](08_connector_unreal.md)

```bash
mkdir -p $R/Projects/UnrealDemo
cp -r $S/Projects/UnrealDemo/AcousticFlowUE.uproject $S/Projects/UnrealDemo/Config $S/Projects/UnrealDemo/Content $S/Projects/UnrealDemo/Source $R/Projects/UnrealDemo/
```

1. **Wwise の統合を入れる**：Wwise Launcher → Unreal Engine → 作り直し先の `AcousticFlowUE.uproject` に 2025.1.8 を統合する
   （本家の `Projects/UnrealDemo/Plugins/` を写してもよい。Git には入れていない物）。
   `Config/DefaultGame.ini` の `WwiseProjectPath`・`RootOutputPath` は相対パスなので、同じ並びに置けばそのまま使える。
2. **自前のプラグインの DLL を置く**：
   `SDK\x64_vc170\Profile\bin\AcousticFlow.dll` → `Projects/UnrealDemo/Plugins/WwiseSoundEngine/ThirdParty/x64_vc170/Profile/bin/`、
   `Release` も同じく（本家の `tools/dev.ps1 wwiseplugin` がやっている事）。
3. **ビルド**（段 3 の DLL が `$R/build/bin/Release` に要る。`Build.cs` がそこを読み、`Binaries/Win64` へ写す）：

```bash
"/c/Program Files/Epic Games/UE_5.6/Engine/Build/BatchFiles/Build.bat" AcousticFlowUEEditor Win64 Development -Project="C:/dev/AcousticFlowRebuild/Projects/UnrealDemo/AcousticFlowUE.uproject" -WaitMutex
```

4. **Event のアセット**：

```bash
"/c/Program Files/Epic Games/UE_5.6/Engine/Binaries/Win64/UnrealEditor-Cmd.exe" "C:/dev/AcousticFlowRebuild/Projects/UnrealDemo/AcousticFlowUE.uproject" -run=WwiseReconcile "-modes=create,update"
```

5. **確かめる**：エディタで `/Game/Maps/Flow_SwingDoor` を開いて Play。画面 1 行目の `missed blocks` が 0、3 行目の `plugin instances` が 1、
   5 キーで扉を開けると音色が変わる。ログは `Saved/Logs/AcousticFlowUE.log` の `[AcousticFlow]`。

---

## 段 6：Unity

先に読む：[09_connector_unity.md](09_connector_unity.md)

1. 本家の `Projects/UnityDemo` から `Assets/Scripts/AcousticFlow/`・`Assets/Scenes/Flow_SwingDoor.unity`（＋`.meta`）・`Assets/Audio/`・
   `Packages/`・`ProjectSettings/` を写す（今動いている C# は 8 本だが、エディタの道具が他の C# を参照しているので、フォルダごと写すのが早い）。
2. Wwise Launcher → Unity → 作り直し先に 2025.1.8 を統合する。`Assets/WwiseSettings.xml` のパスを、写した Wwise プロジェクトに向ける。
3. DLL を置く：エンジン `AcousticEngine.dll` → `Assets/Plugins/x86_64/`、自前のプラグイン（Profile）→ `Assets/Wwise/API/Runtime/Plugins/Windows/x86_64/DSP/`。
4. Unity 用の Wwise プロジェクトにも `python tools/wwise_setup.py unity`（`Projects/UnityDemo_WwiseProject` を写してから）。
5. C# の型検査（本家の `tools/check_csharp.ps1` を写して回す）→ Unity で `Flow_SwingDoor` を開いて Play。

---

## 段ごとに何を学ぶか（まとめ）

| 段 | 分かるようになること |
|---|---|
| 1 | 音を作る部品：畳み込み・HRTF・方向バス・FDN が、確保もロックも無しにオーディオスレッドで回る作り |
| 2 | 形から部屋と RT60 を出す／レイ（NEE）／扉の半影（解析）／回折（前川）／虚像の面音源／五成分の配分／1 フレームの順 |
| 3 | C++ を C の関数で外に出す作り、スレッドをまたぐ受け渡し（seqlock）、寿命（引退して 1 秒後に壊す） |
| 4 | Wwise のオブジェクト処理プラグイン、名前で DLL を引く、バスの実体と「係」 |
| 5・6 | 器の仕事は「値を渡すだけ」、座標の橋、毎フレームの順番、Unity と Unreal を同じ形に保つ |
