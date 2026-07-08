# AcousticFlow — コード全体ガイド

リアルタイム音響シミュレーション・エンジン。**自作のC++音響エンジン（DLL）** を **Unity** から呼び出し、計算結果を **Wwise**（オーディオミドルウェア）に流して、壁の遮蔽・残響・音源の指向性などを実際の音に反映する。

> 最終更新: 2026-07-02。今セッションで **OBB（回転壁）/ CollArea エリア法 / メッシュ occluder + BVH / AcousticZone 音響LOD / 時間デシメーション** を追加した状態を反映。

---

## 1. 全体像（アーキテクチャ）

3層 + Unity の構成。**「音の計算」と「音の再生」を完全に分離**しているのが設計の肝。

```
┌─────────────────────────────────────────────────────────┐
│ Unity (C#)   AcousticFlowDemo.cs                         │
│   ・シーンの壁/音源/リスナー/ゾーンを集める              │
│   ・毎フレーム エンジンに計算させ、結果を Wwise へ渡す   │
└───────────────┬─────────────────────────────────────────┘
                │ P/Invoke（C ABI 境界）
┌───────────────▼─────────────────────────────────────────┐
│ AcousticEngine.dll（C++）                                │
│  Export層  acoustic_api.cpp    … C ABI の関数群          │
│  Core層   acoustic_world / vec3 / aabb / ray / material  │
│           triangle / bvh / source_directivity            │
│  Adapter層  wwise_adapter / null_adapter                 │
└───────────────┬─────────────────────────────────────────┘
                │ Wwise SDK
┌───────────────▼─────────────────────────────────────────┐
│ Wwise ランタイム  → 実際に音を鳴らす                    │
└─────────────────────────────────────────────────────────┘
```

**なぜ層を分けるか**
- **Core層** は Wwise も Unity も知らない純計算 → 単体テスト可能・移植可能・GPU化などの将来拡張がしやすい。
- **Adapter層** が「音響ミドルウェアへの依存」を1か所に閉じ込める。`null_adapter.cpp` は無音ダミー実装（テスト/CI用）。
- **Export層** は C++ のクラスを C の関数（ハンドル渡し）に変換する薄い殻。C# は C ABI しか呼べないため。

---

## 2. ディレクトリ構成

```
AcousticEngine/                  ← C++ エンジン（DLL本体）
  include/acoustic_engine.h      公開C APIヘッダ（C#が参照する仕様書）
  src/
    Core/                        純計算層
      vec3.h                     3次元ベクトル + 演算
      aabb.h                     AABB/OBB & 線分/レイ交差（スラブ法＋ローカル空間トリック）
      triangle.h                 三角形 & Möller–Trumbore 交差
      bvh.h                      三角形BVH（構築・raycast/occludes/countCrossings）※ヘッダオンリー
      ray.h                      レイ/ヒット結果の型（materialポインタ付き）
      material.h / .cpp          6帯域マテリアル（透過率・吸収率）
      source_directivity.h/.cpp  音源指向性
      acoustic_world.h / .cpp    ★中核。箱/メッシュ occluder・レイ積分・エコグラム
    Adapter/  wwise_adapter / null_adapter
    Export/   acoustic_api.cpp   C ABI 関数の実装

AcousticEngineTest/main.cpp      C++単体テスト（Wwise無しでCoreを検証）

UnityDemo/Assets/Scripts/AcousticFlow/
  AcousticEngineBindings.cs      DllImport 宣言（生のP/Invoke）
  AcousticEngine.cs              ↑を包む薄いC#ラッパー
  AcousticMaterial.cs            C#側の6帯域マテリアル（C++プリセットと同値）
  AcousticSurface.cs             個別オブジェクトの材質明示（コンポーネント）
  CollArea.cs                    範囲ボックス：忠実度LOD＋素材ゾーン（静的・オーサリング）
  AcousticZone.cs                音響ゾーン：リスナー在否でメッシュBVHを有効/無効（動的LOD）
  AcousticFlowDemo.cs            ★デモ本体。毎フレームの統括
  Editor/
    AcousticFlowDemoSetup.cs         箱部屋デモの自動生成
    AcousticFlowLivehouseSetup.cs    ライブハウスデモの自動生成（LiveStage＋ゾーン）
    BandMonitor / ReverbMonitor / OutputMonitor / SourcePathMonitor  可視化ウィンドウ
```

---

## 3. データフロー（1フレームで何が起きるか）

`AcousticFlowDemo.Update()`：

1. **入力・移動** — WASD等でリスナー移動、矢印で音源移動、各種トグルキー。
2. **ジオメトリ反映** `RebuildGeometry()` — 箱 occluder を毎フレーム登録し直す（軽い）。メッシュは静的（初回1回だけ登録）。
3. **ゾーン切替** `UpdateZones()` — リスナー在否で各 AcousticZone のメッシュBVHを有効/無効（`SetMeshActive`）。軽い（bounds判定のみ）。
4. **重いソルブ（Nフレに1回 = `solveEveryNFrames`）**：
   - `occlusionScalarMultiSource()` … 全音源の遮蔽(0..1)を共有リスナーパスで一括計算。
   - モニター用（帯域透過・指向性・面ヒートマップ）。
   - `computeEchogram()`（さらに間引き）… 到達時間エコグラム → RT60/wet。
5. **Wwiseへ反映（毎フレーム）** — 遮蔽/指向性/残響RTPCを、間引きフレームでは前回値をスムージングして適用。
6. **`RenderAudio()`（毎フレーム必須）**、メーター取得。

> **時間デシメーション**：重いソルブだけ N フレに1回。間は前回の遮蔽値を保持＋MoveTowardsで滑らかに繋ぐ。計算msの表示もソルブフレームの値だけ保持（間の~0msで上書きしない）。

---

## 4. Core層（純計算）の詳細

### vec3.h / ray.h
3次元ベクトルと演算。`RayHit` はヒット距離・点・法線に加え **材質ポインタ**（箱/メッシュ共通に材質を参照）。

### aabb.h — 箱と交差判定（AABB / OBB）
- `Aabb` … 軸並行ボックス。`segmentIntersectsAabb` / `rayIntersectsAabb`（スラブ法）。
- `Obb` … **回転した箱**（中心・halfExtents・正規直交基底）。**ローカル空間トリック**＝レイをOBBローカルへ移すと中心原点のAABBになるので、既存スラブ法をそのまま呼び、法線だけ基底でワールドへ戻す。`segmentIntersectsObb` / `rayIntersectsObb`。AABBは単位回転のOBBとして統一的に扱う。

### triangle.h / bvh.h — メッシュ occluder
- `Triangle` + `rayIntersectsTriangle`（Möller–Trumbore）/ `segmentIntersectsTriangle`。
- `TriangleBvh`（ヘッダオンリー）… 三角形の中央値分割BVH（葉最大4）。`raycast`（最近ヒット）/ `occludes`（any-hit二値）/ `countCrossings`（横切り枚数＝透過の掛け合わせ回数）。構築は一度きり（静的前提）。

### material.h / .cpp — 6帯域マテリアル
```
帯域: 125,250,500,1k,2k,4k Hz （kNumBands=6）
transmission[6] … 透過率(0..1)  absorption[6] … 吸収率(0..1)
```
プリセット：`defaultWall()` / `concrete()` / `glass()`。※係数は暫定（実測差し替え予定）。

### source_directivity.h / .cpp
Omni/Cardioid/Supercardioid/Bidirectional/Beam の広帯域ゲイン＋背面ローパス。ステートレス。

### acoustic_world.cpp — ★中核
占有物を **箱（動的）とメッシュ（静的・BVH保持）** に分けて保持：
```cpp
std::vector<BoxOccluder>  boxes_;   // Obb+材質。clearGeometry() で毎フレーム作り直す
std::vector<MeshOccluder> meshes_;  // BVH+材質+active。clearMeshes() まで保持
```

| メソッド | 役割 |
|---|---|
| `addBox / addBoxOriented` | 箱（軸並行/回転）を材質付きで追加 |
| `addMesh` → ID | メッシュを BVH 構築して追加。**IDを返す** |
| `setMeshActive(id, on)` | メッシュを**有効/無効**（BVH保持のまま走査からスキップ＝音響LOD） |
| `clearGeometry` / `clearMeshes` | 箱のみ / メッシュのみ消去 |
| `isOccluded` | 二値遮蔽（箱=線分、メッシュ=BVH occludes） |
| `raycastClosest` | 最近ヒット（箱＋各メッシュBVH。非アクティブはスキップ） |
| `computeTransmission` | 直線が貫く箱/メッシュの帯域別透過率（メッシュは横切り枚数ぶん） |
| `occlusionScalarMultiSource` | **共有リスナーパス**で多音源の遮蔽を一括 |
| `computeEchogram` | 到達時間エコグラム＝残響の形 |

**レイ積分のキモ**：リスナーから球面（フィボナッチ）にレイを撒き、壁で反射させながら各バウンス点から各音源へ next-event でつなぐ。各レイは帯域別エネルギーを運び、距離減衰・反射率・透過で減衰。到達時間でビン分け＝残響。反射場は立体角の積分なので合算に **2π係数**（単純平均だと反射が約1桁過小評価＝残響が乾く。過去のバグ）。
> ⚠ **エネルギー正規化・反射の重みは現状 placeholder**（聴感チューニングの当て値）。scattering・空気吸収も未実装。ここが「オールスペック化」で確定させる対象（§10）。

---

## 5. Adapter層 — Wwiseへの配線

`wwise_adapter.cpp`。`initAudio`/`loadBank`/`postEvent`/`executeActionOnEvent`（Stop/Pause/Resume）/`setObstructionOcclusion`（遮蔽→LPF/音量）/`setEmitterListenerVolume`（指向性ゲイン）/`setRTPCValue(OnObject)`（残響/EQ）/`setState`（HRTF↔Panning）/バスRMSメータリング。

**Wwiseバス**（プロジェクトは repo 外）：Master ← Music / Panning(Aux) / Reverb(RoomVerb, Aux)。残響は `ReverbDecay`/`ReverbWet` をRTPCで geometry 駆動。
> はまりどころ：RoomVerb等は自前DLLに Factory を**静的リンク**しないと無音。

---

## 6. Export層 — C ABI 境界

`acoustic_api.cpp` / `include/acoustic_engine.h`。C++クラスを不透明ハンドルで渡し、全関数 `extern "C"`、ベクトルは `AF_Vector3`(POD)。主な公開群：

- **生成/破棄**：`Create` / `Destroy`
- **箱ジオメトリ**：`AddBox` / `AddBoxOriented`（回転＋6+6帯域素材配列） / `ClearGeometry`
- **メッシュ**：`AddMesh`（ワールド頂点＋インデックス＋6+6素材、**ID返し**） / `SetMeshActive`(id,on) / `ClearMeshes`
- **遮蔽**：`IsOccluded` / `ComputeOcclusion` / `ComputeOcclusionRayIntegrated` / `ComputeOcclusionMultiSource`
- **可視化**：`ComputeTransmissionBands` / `DebugRaycast` / `TraceReflectionPath` / `ComputeEchogram` / `ComputeFaceReachability`
- **指向性**：`ComputeDirectivity`
- **Wwise**：Init/Load/PostEvent/Action/ObstructionOcclusion/EmitterVolume/State/RTPC/GetOutputLevels/RenderAudio

---

## 7. Unity (C#) 側

### Bindings / AcousticEngine ラッパー
`DllImport` の生宣言と、それを包む使いやすいラッパー（Vector3変換・UTF-8化・`AddMesh`はID返し・`SetMeshActive` 等）。

### ジオメトリ収集パイプライン（AcousticFlowDemo）
「**忠実度と素材は収集時に一度きり静的決定／位置・回転は毎フレームlive**」が基本方針。

- **箱 occluder**：`walls`（Transform配列）or `autoCollectColliders`（BoxCollider自動収集）。各エントリの忠実度は `defaultFidelityObb` と CollArea(provideFidelity) の重なりで OBB/AABB を決める。素材は AcousticSurface＞素材CollArea(中心包含・priority)＞既定。
- **メッシュ occluder**：`meshOccluders`（明示Transform配列。非空ならそれ“だけ”を丸ごとメッシュ化）or `meshOccluderRoots`（子MeshFilterを走査、fidelity CollArea内のみメッシュ化）。**全fidelityメッシュは1本のBVHに統合**（occluder数に依存させない）。未readable/対象外は箱にフォールバック。
- **音響ゾーン**：`RegisterZones()` が各 `AcousticZone` を**別々の統合BVH**で登録（初回）、`UpdateZones()` が毎フレーム、リスナー在否で `SetMeshActive`（出る時だけ hysteresis で緩める）。アクティブ集合は普段1〜3個＝軽い。

### 主なコンポーネント / インスペクタ項目
- **CollArea**（静的オーサリング）：`provideFidelity`（実形状 or OBB化する範囲）/ `provideMaterial`＋`material`＋`priority`（素材ゾーン）。
- **AcousticSurface**：個別オブジェクトの材質を明示（最優先）。
- **AcousticZone**（動的LOD）：`meshes`（所属メッシュ）/ `material` / `hysteresis` / `startActive`。範囲=Collider or position×lossyScale。
- **AcousticFlowDemo**：`meshOccluders` / `meshOccluderRoots` / `defaultFidelityObb` / `autoCollectColliders` / `solveEveryNFrames`（重いソルブの更新頻度）ほか。

### Editor / 監視ウィンドウ
BandMonitor（6帯域透過）/ ReverbMonitor（エコグラム）/ OutputMonitor（L/R RMS波形）/ SourcePathMonitor（音源面ヒートマップ）。

---

## 8. ジオメトリ忠実度 & 音響LOD（今回の追加の核）

| レベル | 表現 | いつ使う |
|---|---|---|
| **AABB** | 軸並行の箱 | 既定・遠景・回転不要な壁 |
| **OBB** | 回転した箱 | 回した壁を正しく遮蔽（`defaultFidelityObb` / fidelity CollArea） |
| **Mesh+BVH** | 三角形の実形状 | 忠実に見せたい面（`meshOccluders` / fidelity CollArea / AcousticZone） |

- **CollArea** = 静的な「どこを高忠実度・どの素材」のオーサリング。判定は Play開始1フレーム目に一度きり（毎フレームBVH再構築を避ける意図）。
- **AcousticZone** = 動的な「入ったら有効化」の音響LODストリーミング。BVHは保持したまま `active` を切替＝再構築なし。均一ボクセルでなく“置きたい所にゾーン”方式。
- **時間デシメーション** `solveEveryNFrames` = 重いソルブを1/Nに。スムージングで音は連続。

---

## 9. 音響の概念と実装段階

| 概念 | 現状 |
|---|---|
| 遮蔽 | レイ積分で反射の回り込みも考慮（箱＋メッシュ） |
| 透過 | 6帯域マテリアル。Wwiseには現状スカラで渡す |
| 残響 | エコグラム→RT60/wet→RoomVerb駆動 |
| 指向性 | 広帯域ゲイン+背面LPF |
| 距離 | 音量1/r²。空気吸収LPFは**未実装** |
| 空間化 | パンニング採用（HRTFはSteam Audio等で後日） |
| 拡散反射(scattering) | **未実装**（鏡面平行反射のみ） |
| 音響LOD | AcousticZone で入場アクティブ化＋Nフレ・スロットル |

---

## 10. 現状の「仮」と次の一手（オールスペック → 凍結 → GPU）

方針（2026-07-02決定）：**CPUで音響モデルをオールスペックまで詰めて凍結してから GPU コンピュートへ移植**（仮モデルをGPU化すると二度手間）。

**GPU移植で凍結が要る＝レイ内側ループ**（先に確定させる）：
1. 反射の重み較正（今は placeholder。Sabine/Eyring RT60 等の解析解を基準にすると客観的に較正できる）
2. scattering（拡散反射）
3. 距離の空気吸収LPF
4. 帯域積算の確定・エコグラムのビン設計

**GPUと直交＝後でよい**：マテリアル実測値／Wwise帯域別受け渡し（6帯域→1スカラ卒業）／HRTF／Sphere・Capsule形状。回折をやるならレイループ側なので凍結前に。

移植先は Unity ComputeShader（BVH/三角形をGPUバッファへ、1スレッド=1レイ、`AsyncGPUReadback`＋`solveEveryNFrames`＋スムージングで遅延吸収）。C++ CPU版はリファレンス/フォールバックとして残す。

---

## 11. ビルド & デプロイ

```bash
cmake --build build --config Debug          # → build/bin/Debug/AcousticEngine.dll
cp build/bin/Debug/AcousticEngine.dll UnityDemo/Assets/Plugins/x86_64/
```
- C#だけの変更なら Unity が自動再コンパイル（DLLビルド不要）。
- **⚠ ネイティブDLLの新API追加時は Unity Editor を再起動**（Unityは native plugin をホットリロードしない＝`EntryPointNotFound` になる）。
- **⚠ メッシュを occluder にする FBX は Read/Write Enabled 必須**（`isReadable:1`）。無効だと `mesh.vertices` が実行時例外（現コードは検知して箱にフォールバック）。
- Wwiseバンク更新時：`GeneratedSoundBanks/Windows/*.bnk` を `StreamingAssets/WwiseBanks/` へ。

---

## 12. 操作キー（デモ）

| キー | 動作 |
|---|---|
| WASD / Space / LeftShift | リスナー移動 |
| 矢印 / PgUp / PgDn | 選択音源の移動 |
| Tab | 音源選択を巡回 |
| H | 空間化 HRTF↔Panning |
| 1 / 2 | 表示モード（届く経路 / 指向性） |
| R / O / T / P | 残響のみ / obstruction強制 / 残響強制 / 全音源一時停止 |

※ `solveEveryNFrames`・`meshOccluders`・忠実度・ゾーンは AcousticFlowDemo/各コンポーネントのインスペクタで設定。

---

## 設計上の意図（見どころ）

- **層分離**で音響計算エンジンをミドルウェアから独立（Coreは単体テスト可能・GPU移植の土台）。
- **レイエネルギー積分**で遮蔽・残響を物理ベースに。
- **共有リスナーパス**で多音源でも raycast コストが音源数に非依存。
- **忠実度LOD（AABB/OBB/Mesh）＋CollArea＋AcousticZone＋時間デシメーション**で、重いメッシュ音響を破綻させず回す設計。
- エンジン内部状態を**可視化ツール**で見せ、ブラックボックスにしない。
