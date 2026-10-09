# 01 ファイル構成

「今動いている物」と「残っているが使っていない物」を分けて並べます。作り直すときに写すのは前者だけです。

---

## 1. リポジトリの根

```
AcousticFlow/
├─ CMakeLists.txt            根の CMake（C++17・/utf-8・出来上がりを build/bin へ）
├─ AcousticEngine/           ★エンジン（DLL）。この文書の 02〜06 章
│   ├─ CMakeLists.txt
│   ├─ include/              C の窓口（公開ヘッダ）
│   └─ src/
│       ├─ Core/             形・材質・部屋グラフ（＋旧コア）
│       ├─ Flow/             新コア：音の届き方と配分
│       ├─ Dsp/              音を作る部品（オーディオスレッド）
│       ├─ Gpu/              レイを GPU で解く道
│       ├─ Debug/            計測（検査が使う 1 本以外は旧コア用）
│       └─ Export/           C の窓口の実装
├─ AcousticEngineTest/       検査と道具（C++）
├─ WwisePlugin/AcousticFlow/ ★Wwise のプラグイン（07 章）
├─ Projects/
│   ├─ UnrealDemo/           ★Unreal 5.6 のプロジェクト（08 章）
│   ├─ UnrealDemo_WwiseProject/   Unreal 用の Wwise プロジェクト
│   ├─ UnityDemo/            ★Unity のプロジェクト（09 章）
│   └─ UnityDemo_WwiseProject/    Unity 用の Wwise プロジェクト
├─ tools/                    ビルド・配備・Wwise の組み立て・計器・録画
├─ docs/                     設計の記録（この study/ もここ）
└─ build/                    出来上がり（build/bin/Release に DLL と検査の exe）
```

---

## 2. エンジン：今動いている物（50 ファイル・16,374 行）

「段」は [10_rebuild.md](10_rebuild.md) でそのファイルを写す段です。

### include/（公開ヘッダ。段 3）

| ファイル | 行 | 役割 |
|---|---|---|
| `acoustic_world.h` | 282 | 世界（Flow）の C API。箱・材質・耳・音源・摘み・`AF_WorldUpdate` |
| `acoustic_voice.h` | 286 | 音を作る器の C API。Voice・HRTF・FDN の器・方向バス |
| `acoustic_host.h` | 118 | 台帳（ゲームのスレッド → 音のスレッド）と左右の計器 |
| `acoustic_scene.h` | 1,190 | 旧コアの C API。**今は `ACOUSTIC_API` と `AF_Vector3` の定義のためだけに読まれる**（中の関数は使わない） |

### src/Core/（段 2）

| ファイル | 行 | 役割 |
|---|---|---|
| `vec3.h` | 69 | 3 次元ベクトル（エンジン内部用。C API の `AF_Vector3` とは別） |
| `aabb.h` | 241 | 軸に沿った箱・回った箱（OBB）・レイと箱の当たり（スラブ法） |
| `material.h` / `material.cpp` | 53 / 163 | 材質（帯域ごとの吸音・透過・散乱、エネルギー比）と、プリセットの表（**表はここだけ**） |
| `room_graph.h` | 1,484 | 形から「部屋」と「戸口」を見つける（ボクセル・侵食・塗り戻し）、Sabine の RT60 |
| `worker_pool.h` | 134 | 音源ごとのループを複数コアへ割るプール（既定は直列） |
| `maekawa.h` | 97 | 前川の回折減衰（帯域ごと） |

### src/Flow/（新コア。段 2）

| ファイル | 行 | 役割 |
|---|---|---|
| `world_rules.h` | 135 | 世界の決まり：帯域の表・音速・材質の表・世界の重み（影のこもりを含む） |
| `emitter.h` | 106 | 音源（幅を持つ）とリスナー（向き・座標変換） |
| `surfaces.h` | 136 | 音がぶつかる箱の一覧（最近ヒット・横切りの透過） |
| `surface_bvh.h` | 171 | 箱の BVH（ポインタ無しの配列。GPU へ持っていける形） |
| `trace_scene.h` | 185 | レイが見る場面を平らな配列に（CPU と GPU で同じ入力） |
| `probe.h` | 131 | 部屋 1 つの響きの数字（RT60・体積・表面積）。扉の開き具合で RT60 を出し直す |
| `budget.h` | 166 | 予算：総レイ数を固定して音源ごとに配る（厳密・簡易・保持） |
| `energy_trace.h` | 381 | レイトレース。反射の初期・後期を帯域ごとに集計（NEE） |
| `aperture.h` | 329 | 扉の開口の解析：見通しの割合（半影）、戸口の覆い |
| `diffraction.h` | 235 | 回折：稜線を回る最短の経路と前川の減衰 |
| `image_sources.h` | 281 | 虚像法（ISM）：初期反射の向きと重み |
| `image_surface.h` | 161 | 虚像の面音源（既定 `earlyModel 4`）：虚像をつないで面にし、幅と奥行きの重みを出す |
| `image_lattice.h` | 252 | 虚像の網（`earlyModel 3`。既定では使わない） |
| `receiver_layout.h` | 154 | 受取面の小片とレイの受け取り（`earlyModel 1〜3`。既定では使わない） |
| `receiver.h` | 673 | 受取面の耳の側（`earlyModel 1〜3`。既定では使わない） |
| `mix.h` | 129 | 配分の結果の形（タップ・FDN の送り・帳簿） |
| `response.h` | 88 | 速さの表（量は即時、形は 40 ms、統計は 50 ms、向きは 60 ms） |
| `distribute.h` | 456 | ★配分：レイと解析の答えを五成分に配り、タップと送りにする |
| `mix_to_voice.h` | 111 | 配分 → DSP の橋（√ を取るのはここだけ） |
| `world.h` | 1,014 | ★世界：1 フレームを回す。ホストから見える唯一の物 |

### src/Dsp/（段 1）

| ファイル | 行 | 役割 |
|---|---|---|
| `fft.h` | 99 | radix-2 の FFT（実行時に確保しない） |
| `partitioned_convolver.h` | 309 | 一様分割の畳み込み |
| `nonuniform_convolver.h` | 254 | 非一様分割の畳み込み（遅れ 1 ブロック → 64 サンプル） |
| `hrtf_set.h` | 467 | HRTF（実測ファイルの読み込み・合成の球の頭・ITD を抜いて別に持つ） |
| `hrtf_processor.h` | 239 | 1 方向の両耳化（ITD の小数遅延 ＋ HRIR の畳み込み、向きが変わるときのクロスフェード） |
| `early_reflect_conv.h` | 619 | タップの列を鳴らす（6 帯域に割る・鏡面と拡散・タップの差し替えはパラメータの補間） |
| `direction_bus.h` | 295 | 方向バス：水平 8 ＋上下 2 のレーン × 2 耳、固定の HRIR で畳む |
| `fdn_tail.h` | 480 | 帯域ごとの FDN（16 本の遅延線 × 6 帯域、Hadamard の混合） |
| `fdn_room_mix.h` | 996 | 部屋ごとの FDN の配線・レーンへの配り方・戸口の線音源 |
| `voice_renderer.h` | 733 | ★音源 1 本ぶんの信号の流れ（直接は HRTF、反射は方向バス、響きは FDN へ送る） |
| `reverb_tail_ir.h` | 286 | 畳み込みの尾の IR（**旧の道**。FDN を差していると使わないが、Voice が読み込むので写す） |
| `tail_bus.h` | 168 | 畳み込みの尾の共有バス（**旧の道**。同上） |

### src/Gpu/（段 2）

| ファイル | 行 | 役割 |
|---|---|---|
| `compute_d3d11.h` / `.cpp` | 84 / 193 | エンジンが自分で持つ D3D11 の計算デバイス |
| `trace_kernel.h` | 329 | レイ 1 本を追う HLSL（文字列で持つ。CPU 版と同じ式） |
| `trace_gpu.h` | 258 | 場面を GPU へ詰めて流し、CPU 版と同じ形で返す（既定は切） |

### src/Export/（段 3）

| ファイル | 行 | 役割 |
|---|---|---|
| `world_api.cpp` | 493 | `acoustic_world.h` の実装（`Flow/world.h` へ流すだけ） |
| `voice_api.cpp` | 393 | `acoustic_voice.h` の実装（ハンドル ⇄ C++、null の防御） |
| `host_api.cpp` | 268 | `acoustic_host.h` の実装（seqlock の台帳、Voice の引退、計器の共有メモリ） |

---

## 3. エンジン：残っているが使っていない物（写さない）

2026-09-10 の作り直し（「新コア」= Flow）より前の物です。Unity の古い場面（`Test_*`）と古いホスト（`AcousticFlowSceneDemo.cs`）だけが使っていて、
Unreal・Wwise・Unity の `Flow_*` 場面は 1 つも呼びません（呼んでいる 108 個の関数を全部確かめた）。消す予定（段 10）なので学ばなくてよいです。

| ファイル | 行 | 何だったか |
|---|---|---|
| `src/Core/scene.h` | 8,356 | 旧コアの本体（場面・遮蔽・タップの組み立て・尾） |
| `src/Export/scene_api.cpp` | 1,543 | 旧コアの C API（`AF_Scene*`） |
| `src/Core/scene_async.h` | 293 | 旧コアの非同期 |
| `src/Core/tap_builder.h` | 369 | 旧コアのタップの組み立て |
| `src/Core/btm.h` `utd.h` `kirchhoff.h` `aperture_fresnel.h` | 787 | 旧の回折・開口の模型（比べるために残した） |
| `src/Core/bvh.h` `mesh_geom.h` `triangle.h` `ray.h` | 607 | 三角形メッシュの BVH（新コアは箱だけ） |
| `src/Debug/capture*.h` | 920 | 旧コアの録音・走査 |
| `src/Debug/detectors.h` | 207 | 破れの型紙。**DSP の検査だけが使う**ので段 1 で写す |

---

## 4. 検査と道具

| ファイル | 何か | 作り直しで使うか |
|---|---|---|
| `AcousticEngineTest/dsp_regression.cpp` | DSP の検査（160 件。DLL 無しでヘッダを直接読む） | 段 1 |
| `AcousticEngineTest/flow_regression.cpp` ＋ `test_instruments.h` | Flow の検査（196 件。Core・Flow・Gpu・DSP をソースのまま） | 段 2 |
| `AcousticEngineTest/host_regression.cpp` | 台帳の検査（10 件。DLL を通す） | 段 3 |
| `AcousticEngineTest/replay_capture.cpp` | `AfReplayWav`：Unreal の録画の記録から音を作り直す | 任意 |
| `AcousticEngineTest/render_wav_flow.cpp` | `AfFlowWav`：場面を WAV に書き出す | 任意 |
| `AcousticEngineTest/scene_regression.cpp` ほか | 旧コアの検査・道具 | 使わない |
| `tools/dev.ps1` | ビルド・検査・配備の入口（`build` `test` `deploy` `wwiseplugin` `csharp`） | 本家で使う |
| `tools/wwise_setup.py` ＋ `wwise_waapi.py` | Wwise プロジェクトにバス・プラグイン・試験の音・Event を組む（画面を開かずに） | 段 4 |
| `tools/check_csharp.ps1` | Unity を開かずに C# を型検査 | 段 6 |
| `tools/af_meter.py` | 左右の計器の窓（共有メモリを読む） | 任意 |
| `tools/record_unreal_demo.ps1` ＋ `tools/AfCapture/` | 動画を撮る | 任意 |

---

## 5. コネクタ層のファイル

### Wwise プラグイン `WwisePlugin/AcousticFlow/`（07 章）

| ファイル | 何か |
|---|---|
| `SoundEnginePlugin/AcousticFlowFX.h` / `.cpp` | ★プラグインの本体（自分で書いた所。約 500 行） |
| `SoundEnginePlugin/AcousticFlowFXParams.*`・`AcousticFlowFXFactory.h`・`AcousticFlowFXShared.cpp` | Wwise の雛形のまま（パラメータは使っていない） |
| `WwisePlugin/AcousticFlow.xml` ほか | Wwise の画面（Authoring）用の定義（雛形のまま） |
| `AcousticFlowConfig.h` | 会社 ID 64・プラグイン ID 20311 |
| `PremakePlugin.lua` | エンジンの `include/` を読む所を足しただけ |

### Unreal `Projects/UnrealDemo/Source/AcousticFlowUE/`（08 章）

| ファイル | 何か |
|---|---|
| `AcousticFlowUE.Build.cs` | エンジンのヘッダと .lib を繋ぐ。DLL を `Binaries/Win64` へ写す |
| `AcousticFlowUE.cpp` | 起動時にエンジンの DLL を読む |
| `AcousticFlowWorld.h` / `.cpp` | ★司令塔（Unity の AcousticWorld に当たる）。毎フレームの順番はここ |
| `AcousticFlowWorldAreas.cpp` | エリア（部屋・戸口）の表示 |
| `AcousticFlowWorldCapture.cpp` | 録画のための記録 |
| `AcousticFlowEmitter.h` / `.cpp` | ★音源（Voice を持ち、Wwise の Event を鳴らす） |
| `AcousticFlowDoor.h` / `.cpp` | 蝶番で開く扉（5/6 キー） |
| `AcousticFlowPlaceables.h` / `.cpp` | 地図に置く部品：壁の箱・音源 |
| `AcousticFlowMaterial.h` | 材質の番号（エンジンのプリセットと同じ並び） |
| `AcousticFlowSpace.h` | 座標の橋（Unreal の cm・Z 上 → エンジンの m・y 上） |
| `AcousticFlowGameMode.*`・`AcousticFlowWalker.*` | 一人称の試聴（1.4 m/s） |
| `AcousticFlowThirdPersonGameMode.*` | 三人称（テンプレートのキャラクター） |

### Unity `Projects/UnityDemo/Assets/Scripts/AcousticFlow/`（09 章。今動いている 8 本）

| ファイル | 行 | 何か |
|---|---|---|
| `AcousticWorld.cs` | 668 | ★司令塔。毎フレームの順番はここ |
| `WorldVoice.cs` | 188 | ★音源（Voice を持つ。Unity で鳴らすときは OnAudioFilterRead で音にする） |
| `TailBusRenderer.cs` | 342 | 耳の側で 1 回だけ回す器（方向バス・FDN）の持ち主 |
| `AcousticWorldBindings.cs` | 205 | `acoustic_world.h` と `acoustic_host.h` の宣言 |
| `AcousticEngineBindings.cs` | 960 | `acoustic_voice.h` の宣言（＋旧コアの宣言） |
| `SwingDoor.cs` | 209 | 蝶番で開く扉 |
| `AcousticSurface.cs` | 110 | 面の材質の印 |
| `FlowWalker.cs` | 71 | 試聴の歩き（1.4 m/s） |

残りの C# 18 本（`AcousticFlowSceneDemo.cs`・`VoiceConvolver.cs`・`PortalEmitter.cs` など）は旧コアの道で、`Flow_*` 場面では使っていません。
