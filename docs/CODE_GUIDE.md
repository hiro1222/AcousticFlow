# AcousticFlow — コード全体ガイド

リアルタイム音響シミュレーション・エンジン。**自作のC++音響エンジン（DLL）** を **Unity** から呼び出し、計算結果を **Wwise**（オーディオミドルウェア）に流して、壁の遮蔽・残響・音源の指向性などを実際の音に反映する。

---

## 1. 全体像（アーキテクチャ）

3層 + Unity の構成。**「音の計算」と「音の再生」を完全に分離**しているのが設計の肝。

```
┌─────────────────────────────────────────────────────────┐
│ Unity (C#)   AcousticFlowDemo.cs                         │
│   ・シーン上の壁/音源/リスナーの座標を集める             │
│   ・毎フレーム エンジンに計算させ、結果を Wwise へ渡す   │
└───────────────┬─────────────────────────────────────────┘
                │ P/Invoke（C ABI 境界）
┌───────────────▼─────────────────────────────────────────┐
│ AcousticEngine.dll（C++）                                │
│                                                          │
│  Export層  acoustic_api.cpp                              │
│    └ C ABI の関数群（extern "C"）。C# と話す唯一の窓口   │
│                                                          │
│  Core層   acoustic_world / vec3 / aabb / ray / material  │
│           source_directivity                             │
│    └ 純粋な数学・物理計算。Wwiseにも C# にも依存しない   │
│                                                          │
│  Adapter層  wwise_adapter / null_adapter                 │
│    └ Wwiseを叩く（再生・遮蔽・残響RTPC・メーター）       │
└───────────────┬─────────────────────────────────────────┘
                │ Wwise SDK
┌───────────────▼─────────────────────────────────────────┐
│ Wwise ランタイム  → 実際に音を鳴らす                    │
│   バス: Music / Panning / Reverb(RoomVerb) / Master      │
└─────────────────────────────────────────────────────────┘
```

**なぜ層を分けるか**
- **Core層** は Wwise も Unity も知らない純計算 → 単体テスト可能・移植可能・GPU化などの将来拡張がしやすい。
- **Adapter層** が「音響ミドルウェアへの依存」を1か所に閉じ込める。Wwiseを別のものに差し替えても Core は無傷。`null_adapter.cpp` は音を出さないダミー実装（テスト/CI用）。
- **Export層** は C++ のクラスを C の関数（ハンドル渡し）に変換する薄い殻。C# は C ABI しか呼べないため。

---

## 2. ディレクトリ構成

```
AcousticEngine/                  ← C++ エンジン（DLL本体）
  CMakeLists.txt                 ビルド定義
  include/acoustic_engine.h      公開C APIヘッダ（C#が参照する仕様書）
  src/
    Core/                        純計算層
      vec3.h                     3次元ベクトル + 演算
      aabb.h                     軸並行ボックス & 線分/レイ交差（スラブ法）
      ray.h                      レイ/ヒット結果の型
      material.h / .cpp          6帯域マテリアル（透過率・吸収率）
      source_directivity.h/.cpp  音源指向性（前方/背面のゲイン）
      acoustic_world.h / .cpp    ★中核。障害物集合・レイ積分・エコグラム
    Adapter/
      wwise_adapter.h / .cpp     Wwise への実配線
      null_adapter.cpp           無音ダミー実装
    Export/
      acoustic_api.cpp           C ABI 関数の実装

AcousticEngineTest/main.cpp      C++単体テスト（Wwise無しでCoreを検証）

UnityDemo/Assets/Scripts/AcousticFlow/
  AcousticEngineBindings.cs      DllImport 宣言（生のP/Invoke）
  AcousticEngine.cs              ↑を包む薄いC#ラッパー（型を整える）
  AcousticFlowDemo.cs            ★デモ本体。毎フレームの統括
  Editor/
    AcousticFlowDemoSetup.cs     シーン自動生成エディタ拡張
    BandMonitorWindow.cs         6帯域の透過を可視化
    ReverbMonitorWindow.cs       エコグラム（残響）可視化
    OutputMonitorWindow.cs       出力L/Rレベル波形
    SourcePathMonitorWindow.cs   音源面の到達ヒートマップ
```

---

## 3. データフロー（1フレームで何が起きるか）

`AcousticFlowDemo.Update()` が毎フレーム回す処理：

1. **入力処理** — WASDでリスナー移動、矢印で音源移動、各種デバッグキー（後述）。
2. **ジオメトリ反映** — シーンの壁配列 → `ClearGeometry()` + `AddBox()` でエンジンに登録（変更時のみ）。
3. **遮蔽計算** — `occlusionScalarMultiSource()`：リスナーから1024本のレイを撒き、全音源ぶんの遮蔽量(0..1)を一括計算（**共有リスナーパス** = レイ本数は音源数に依存しない）。
4. **残響計算**（4フレームに1回）— `computeEchogram()` で到達時間エコグラムを作り、そこから wet量 / RT60 を算出。
5. **指向性計算** — 音源ごとに `ComputeDirectivity()` で向きによる音量・こもりを得る。
6. **Wwiseへ反映** — 音源ごとに：
   - `SetObstructionOcclusion()` … 遮蔽・こもりをLPF/音量へ
   - `SetEmitterListenerVolume()` … 指向性ゲインを出力バス音量へ
   - `SetRTPCValue("ReverbWet"/"ReverbDecay")` … 残響パラメータ
7. **`RenderAudio()`** — Wwiseのイベント処理を1フレーム進める（必須）。
8. **メーター取得** — `GetOutputLevels()` でマスターバスのL/R RMSを取り、波形履歴に積む。

---

## 4. Core層（純計算）の詳細

### vec3.h
3次元ベクトル `Vec3` と演算（加減・内積・外積・正規化・長さ）。全計算の基礎型。

### aabb.h — 障害物と交差判定
- `Aabb` … 軸並行ボックス（min/max）。壁の最小表現。
- `segmentIntersectsAabb()` … **線分**が箱を貫くか（**スラブ法**）。遮蔽の有無判定に使う。
- `rayIntersectsAabb()` … **レイ**が箱に当たる距離と**面の法線**を返す。反射計算に使う。

> **スラブ法**：箱を「x範囲・y範囲・z範囲」の3枚の板の重なりと見て、レイが各範囲内にいる媒介変数 t の区間を求め、3軸の共通区間が残れば交差。軽量で分岐が少ない王道アルゴリズム。

### ray.h
レイの方向と、ヒット結果（`RayHit`: 当たったか・距離・点・法線・どの箱か）。

### material.h / .cpp — 6帯域マテリアル
```
帯域: 125, 250, 500, 1k, 2k, 4k Hz （kNumBands = 6）
transmission[6] … 透過率(0..1)。1=素通り。低域ほど高い＝壁越しにベースが回る
absorption[6]   … 吸収率(0..1)。反射でどれだけエネルギーを失うか
```
プリセット：`defaultWall()` / `concrete()` / `glass()`。
> 係数は現状「それらしい暫定値」。本番は実測/文献値に差し替え予定。

### source_directivity.h / .cpp — 音源指向性
音源の正面・側面・背面で音量と「こもり」が変わるモデル（Bプラン＝広帯域ゲイン＋背面ローパス）。
タイプ：`Omni / Cardioid / Supercardioid / Bidirectional / Beam`。
ステートレス関数で、音源ごとに毎フレーム呼ぶ。

### acoustic_world.cpp — ★中核
障害物（AABB＋マテリアル）の集合を持ち、以下を計算：

| メソッド | 役割 |
|---|---|
| `isOccluded` | 2点間が壁で遮られてるか（線分判定・二値） |
| `raycastClosest` | 最も近い壁との交差（反射の土台） |
| `traceReflectionPath` | 鏡面反射で経路点を追う（可視化用） |
| `computeTransmission` | 直線が貫く壁の帯域別透過率を掛け合わせ |
| `occlusionScalar` | 透過の広帯域平均 → 遮蔽スカラ（簡易） |
| `occlusionScalarRayIntegrated` | **レイ積分**で反射の回り込みも数える遮蔽 |
| `occlusionScalarMultiSource` | ↑の多音源・共有リスナーパス版 |
| `computeEchogram` | 到達時間ごとのエネルギー＝**残響の形** |
| `computeFaceReachability` | 音源面の「どこが届くか」グリッド |

**レイ積分のキモ**（`computeEchogram` 等）：
リスナーから球面上に均一にレイを撒き、壁で反射させながら各バウンス点から各音源へ「next-event」でつなぐ。各レイは帯域別エネルギーを運び、距離減衰・反射率・透過で減衰する。これを到達時間でビン分けすると「直接音の大ピーク → 初期反射 → 減衰する尾」という残響の形が物理から出てくる。
> 反射場は球面全方向からの到達の**積分**なので、レイ合算には立体角係数（2π）を掛ける。単なる平均(1/N)にすると反射が約1桁過小評価され残響が乾く（実際にあったバグ）。

---

## 5. Adapter層 — Wwiseへの配線

`wwise_adapter.cpp` が Wwise SDK を叩く。主な関数：

| 関数 | Wwise API | 用途 |
|---|---|---|
| `initAudio` / `loadBank` | Init / LoadBank | 起動・バンク読込 |
| `postEvent` | PostEvent | 音源イベント再生 |
| `executeActionOnEvent` | ExecuteActionOnEvent | Stop/Pause/Resume（止めて残響の尾を聴く） |
| `setObstructionOcclusion` | SetObjectObstructionAndOcclusion | 遮蔽→LPF/音量 |
| `setEmitterListenerVolume` | SetGameObjectOutputBusVolume | 指向性ゲイン |
| `setRTPCValue` / `OnObject` | SetRTPCValue | 残響/EQ パラメータ駆動 |
| `setState` | SetState | 空間化(HRTF/Panning)切替 |
| `BusMeteringCallback` | RegisterBusMeteringCallback | マスターバスRMS取得 |

**Wwise側のバス構成**（プロジェクトは repo 外）：
```
Master Audio Bus
├ Music_TokyoGeto        … 音楽のドライ
├ Panning_TokyoGeto(Aux) … パンニング実験用
└ Reverb_TokyoGeto(Aux)  … RoomVerb（残響）。音源から-12dBで送る
```
残響は **`ReverbDecay → RoomVerbのDecay Time`** と **`ReverbWet → Reverbバスの音量`** をRTPCで駆動 → 部屋の形・材質に応じて響きが変わる（geometry駆動）。

> 重要なはまりどころ：RoomVerb等のプラグインは**自前DLLに Factory を静的リンク**しないと、そのバンクの読込/再生が失敗して無音になる。

---

## 6. Export層 — C ABI 境界

`acoustic_api.cpp` / `include/acoustic_engine.h`。
- C++のクラス `AcousticWorld` を **不透明ハンドル**（`AcousticEngineHandle`）として C# に渡す。
- 全関数 `extern "C"`（名前マングリングを無効化）＝ C# の `DllImport` から呼べる形。
- ベクトルは `AF_Vector3`（plain struct）で受け渡し。

これが「C# ↔ C++」の唯一の契約。ヘッダのコメントがそのまま仕様書になっている。

---

## 7. Unity (C#) 側

### AcousticEngineBindings.cs
`[DllImport("AcousticEngine")]` の生宣言。C API と1:1。文字列は UTF-8 の `byte[]` で渡す。

### AcousticEngine.cs
Bindings を包む薄いラッパー。`Vector3`↔`AF_Vector3` 変換、文字列のUTF-8化、`NumBands` 等の定数、`ActionPause/Resume` などを提供。ゲーム側はこのクラスだけ見ればよい。

### AcousticFlowDemo.cs — ★デモ本体
- シーンの壁/音源/リスナーを集めて毎フレーム統括（§3のフロー）。
- HUD（FPS・遮蔽量・残響wet/RT・指向性など）を描画。
- デバッグ用の可視化モード・各種トグルキー。

### Editor/ 監視ウィンドウ群
再生中のエンジン内部状態を別ウィンドウで可視化（エンジンの「中身」を見せる差別化要素）：
- **BandMonitor** … 6帯域の透過ゲイン
- **ReverbMonitor** … エコグラム（到達時間×エネルギー）
- **OutputMonitor** … 出力L/RのRMS波形
- **SourcePathMonitor** … 音源面の到達ヒートマップ

---

## 8. 音響の概念と実装段階

| 概念 | 内容 | 現状 |
|---|---|---|
| **遮蔽 (Occlusion)** | 壁が直接音を遮る | レイ積分で反射の回り込みも考慮 |
| **透過 (Transmission)** | 壁を抜ける（低域ほど） | 6帯域マテリアルで実装。Wwiseには現状スカラで渡す |
| **残響 (Reverb)** | 反射の積み重なり | エコグラム→RT60/wet→RoomVerb駆動 |
| **指向性 (Directivity)** | 音源の向きで音が変わる | 広帯域ゲイン+背面LPF |
| **距離 (Distance)** | 遠いと小さく・こもる | 音量は1/r²。空気吸収LPFは今後 |
| **空間化 (HRTF/Panning)** | 立体定位 | パンニング採用（HRTFは環境都合で見送り） |

---

## 9. ビルド & デプロイ

```bash
# C++ DLL をビルド
cmake --build build --config Debug
#   → build/bin/Debug/AcousticEngine.dll

# Unity へ反映（Unityを閉じてから。開いてるとDLLロックでコピー失敗）
cp build/bin/Debug/AcousticEngine.dll UnityDemo/Assets/Plugins/x86_64/

# Wwise バンク更新時（Wwiseで Generate 後）
#   GeneratedSoundBanks/Windows/{Init,TokyoGeto}.bnk を
#   UnityDemo/Assets/StreamingAssets/WwiseBanks/ へコピー
```
> C#だけの変更なら Unity が自動再コンパイルするのでDLLビルドは不要。

---

## 10. 操作キー一覧（デモ）

| キー | 動作 |
|---|---|
| WASD / 右ドラッグ | リスナー移動・回転 |
| 矢印 / PgUp / PgDn | 選択音源の移動 |
| Tab | 音源選択を巡回 |
| H | 空間化 HRTF↔Panning |
| 1 / 2 | 表示モード（届く経路 / 指向性） |
| R | 残響のみ（ドライをミュート） |
| O | obstruction強制（LPF経路テスト） |
| T | 残響強制ON（残響経路テスト） |
| P | 全音源一時停止（残響の尾を聴く） |

---

## 設計上の意図（就活向けの見どころ）

- **層分離**で「音響計算エンジン」を音響ミドルウェアから独立させた（Coreは単体テスト可能）。
- **レイトレ的なエネルギー積分**で遮蔽・残響を物理ベースに（直線1本の近似から脱却）。
- **共有リスナーパス**で多音源でもraycastコストが音源数に依存しない設計（実ゲームのスケールを意識）。
- **6帯域**の周波数依存（低域は壁を回り込む等）をミドルウェア境界まで持っている。
- エンジン内部状態を**可視化ツール**で見せ、ブラックボックスにしない。
