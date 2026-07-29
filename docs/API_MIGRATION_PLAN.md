# API 移行計画 — クエリ型 → バッチ型（af_Update / af_Get*）

作成 2026-07-22。[DYNAMIC_ARCH_SPEC.md](DYNAMIC_ARCH_SPEC.md) §2/§7/§8 を実装に落とすための差分洗い出しと段取り。
**この文書は設計フェーズの成果物**であり、実装はまだ始めていない。

---

## 0. なぜ今これをやるか

残りの構造的な穴（メッシュ対応・ワーカースレッド・カリング/LOD）は**全部この API 形状の上に乗る**。
先に土台を直さないと、メッシュを足した後で境界を作り直すことになる。逆に API を直せば、
残りは SPEC どおりに素直に乗る。

同時に「ホストが薄い殻」（SPEC §8）も達成する必要がある。現状ホストは 1500 行あり、
**このままでは DirectX 等への移植でその 1500 行を書き直すことになる**＝「移植可能」という
コンセプトの中核が成立していない。

---

## 1. 現状と SPEC の差分

### 1-1. 呼び出しモデル

| | 現状 | SPEC |
|---|---|---|
| 形 | **クエリ型**。ホストが用途ごとに個別関数を同期で呼ぶ | **バッチ型**。`af_Update(dt)` 1 発 → `af_Get*` で結果取得 |
| 音源 | 関数ごとに `source` を渡す。音源数ぶんループするのはホスト | エンジンが音源を**登録で保持**し、内部でループ |
| 実行 | 全て呼び出しスレッドで同期実行 | ワーカーで実行、結果はダブルバッファ、1 フレーム遅延 |
| レート制御 | ホストが `〜EveryFrames` カウンタを持ち、呼ぶ／呼ばないを判断 | エンジンが `af_SetRoleRates` で内部管理 |

現状 API は 24 関数。ホストの `Update()` はこれらを**手順として組み立てて**いる（1 ジオメトリ更新 →
2 音源位置 → 3 帯域生存 → 4 遮蔽 → 5 表示用 → 6 残響）。**この手順自体がエンジンの知識**であり、
ホストに置くべきものではない。

### 1-2. ホストが抱えている責務（棚卸し）

`AcousticFlowSceneDemo.cs` 1500 行の中身を、移植時にどうなるかで分類する。

| 分類 | 内容 | 移植時 |
|---|---|---|
| **A: エンジンへ移すべき** | 役割ごとの更新レート管理（`_erCountdown` 等 5 種）、音源ループ、エッジカタログ構築の起動判断、タップ合成（`BuildMainSourceTaps`）、mixing time / 体積推定 / 残響目標比の算出 | 今は**書き直しになる**。移せば不要 |
| **B: ホスト固有（残ってよい）** | Unity の Collider 収集、Transform → OBB 変換、Wwise/AudioSource への出力、入力処理、Gizmo 描画、GUI | 各ホストで書くのが当然の部分 |
| **C: 共通ライブラリにすべき** | 遮蔽/方向/レベルのスムージング、仮想エミッタのプール管理、IR 畳み込み（`IrConvolver` 等） | エンジン本体ではないが**ホストごとに再実装したくない**。C++ 側の「共通ホスト支援層」か、各言語の薄い SDK に置く |

**A が移れば、ホストは 1500 → 500 行程度になる見込み**（B のみ残る）。C は別途置き場所を決める。

### 1-3. 現状 API の分類（24 関数の行き先）

| 現状関数 | 行き先 |
|---|---|
| `AF_SceneCreate/Destroy` | **維持**（名前を `af_CreateScene` 系へ揃える程度） |
| `AF_SceneAddMaterial` | **維持**（SPEC `af_CreateMaterial`） |
| `AF_SceneAddInstanceBox` / `UpdateInstance` / `SetInstanceActive` / `ClearInstances` | **維持**（SPEC `af_CreateObject` / `af_SetObjectTransform` / `af_SetObjectEnabled`）。将来 `geomId` でメッシュへ拡張 |
| `AF_SceneComputeTransmissionBands` | **内部化** → `af_GetSourceOcclusion` の一部 |
| `AF_SceneComputeDiffractionBands` | **内部化** → `af_GetSourceDiffraction` |
| `AF_SceneOcclusionReflected(Multi)` | **内部化** → `af_Update` の役割 2 |
| `AF_SceneComputeEarlyReflections` | **内部化** → `af_GetEarlyReflections` |
| `AF_SceneComputeDiffractionSources` | **内部化** → `af_GetDiffractionSources` |
| `AF_SceneComputeEchogram(Bands)` | **内部化** → `af_GetReverb` |
| `AF_SceneBuildEdgeCatalog` / `EdgeCatalogCount` / `ClearEdgeCatalog` | **内部化**（`af_Update` が管理。Count は診断用に残す） |
| `AF_SceneIsOccluded` / `Raycast` / `DiffractionPath` / `DiffractionCandidates` / `TraceReflectionPath` / `InstanceCount` | **デバッグ API として維持**（`af_Debug*` に改名。可視化・テスト専用と明示） |

要点: **「計算を要求する関数」は全部消えて、「登録」「更新1発」「結果取得」の 3 種類になる**。

---

## 2. 目標 API（SPEC §2 を現実装に合わせて具体化）

```c
/* --- 登録（セットアップ / 変化時のみ）--- */
AF_MatId  af_CreateMaterial(AF_Scene*, const float* t6, const float* a6, const float* s6);
AF_ObjId  af_CreateObject(AF_Scene*, AF_GeomId, AF_MatId, AF_Transform);
void      af_SetObjectTransform(AF_Scene*, AF_ObjId, AF_Transform);
void      af_SetObjectEnabled(AF_Scene*, AF_ObjId, int on);

/* --- リスナー / 音源（エンジンが保持）--- */
void      af_SetListener(AF_Scene*, AF_Transform);
void      af_SetSource(AF_Scene*, AF_SrcId, AF_Vector3 pos /*, 指向性等 */);
void      af_RemoveSource(AF_Scene*, AF_SrcId);

/* --- 毎フレーム 1 発 --- */
void      af_Update(AF_Scene*, float dt);

/* --- 結果取得（前回 Update 完了ぶん）--- */
void  af_GetSourceOcclusion   (AF_Scene*, AF_SrcId, float* out6);
void  af_GetSourceDiffraction (AF_Scene*, AF_SrcId, float* out6);
int   af_GetEarlyReflections  (AF_Scene*, AF_SrcId, AF_Vector3* outPos, float* outGain6, int maxTaps);
int   af_GetDiffractionSources(AF_Scene*, AF_SrcId, AF_Vector3* outPos, float* outGain, int maxSrc);
void  af_GetReverb            (AF_Scene*, float* outEchogramBands, int numBins, float* outRT60_6, float* outWet);

/* --- 設定 --- */
void  af_SetRoleRates(AF_Scene*, int role1EveryN, int role2EveryN);
void  af_SetEchogramConfig(AF_Scene*, int bins, float binSeconds, int rays, int bounces);
void  af_SetDistanceRef(AF_Scene*, float distanceRef);
```

### 未確定（実装前に lock したい）

1. **音源 ID の型** — 現状ホストは配列インデックスで扱う。SPEC は `AF_SrcId`(u64)。
   Wwise の GameObject ID と揃えると配線が楽（現状 `SourceId(i)` で作っている）。→ **u64 で揃える案**。
2. **`af_GetReverb` の粒度** — エコグラム生を返すか、RT60/wet/mixing time まで出すか。
   ホストを薄くするなら後者だが、IR 生成（`ReverbTailIr`）が包絡を必要とするので**両方返す**のが現実的。
3. **知覚圧縮（`reverbRatioExponent`）の置き場所** — 物理量ではないのでエンジンに置くか迷う。
   → **エンジンは物理比を返し、圧縮は共通ホスト支援層（C 分類）**が妥当か。
4. **1 フレーム遅延の見え方** — `af_Get*` が「前回ぶん」を返すことをホストにどう伝えるか。
   ドキュメントだけで足りるか、フレーム番号を返すか。

---

## 3. 段取り（各段で動作を保ったまま進む）

**原則: 各段の終わりに必ず音が鳴る状態にする。** 大きな一括置換をしない。

### 段 1: リスナー / 音源をエンジンが保持する
- `af_SetListener` / `af_SetSource` を追加。エンジン内に `listener` と `sources[]` を持つ。
- 既存関数は**引数で受け取り続ける**（互換維持）。内部で「引数版」と「保持版」を両立。
- ホストは毎フレーム `af_SetSource` を呼ぶだけに変える。
- **確認**: 音に変化がないこと。

### 段 2: `af_Update` + `af_Get*` を追加（既存 API と併存）
- `af_Update` が役割 1（遮蔽・回折・透過）と役割 2（反射・エコグラム）を内部レートで実行し、結果を内部バッファへ。
- `af_Get*` を追加。**まだシングルスレッド**（同期実行）。
- ホストを 1 音源ずつ `af_Get*` を読む形へ書き換え。
- **確認**: 段 1 と同じ音。Output Scope の各段レベルが変わらないこと。

### 段 3: ホストから A 分類を削除
- レート管理カウンタ 5 種、音源ループ、エッジカタログ判断、`BuildMainSourceTaps`、
  体積推定 / mixing time / 残響目標比 を**エンジンへ移す**。
- ホスト側の該当コードを削除。**ここでホストが 1500 → 500 行台になる**。
- **確認**: 同じ音。行数の削減を実測して記録。

### 段 4: 旧クエリ API を撤去 / デバッグ API を分離
- 内部化した 9 関数を公開ヘッダから削除。
- 可視化用（`Raycast` / `DiffractionPath` / `DiffractionCandidates` / `TraceReflectionPath`）を
  `af_Debug*` へ改名し、「デバッグ専用・性能保証なし」と明示。
- **確認**: コンパイルが通り、同じ音。

### 段 5: ワーカースレッド化（SPEC §7）
- `af_Update` が入力をスナップショットしてワーカーへ投げ、即 return。
- 結果はダブルバッファ、`af_Get*` は front を読む。**1 フレーム遅延**。
- ホスト側スムージングで遅延を吸収（既存のスムージングがそのまま効く想定）。
- **確認**: `音響計算 ms/frame` がメインスレッドから消えること。音の遅れが気にならないこと。

### 段 6 以降（別計画）
メッシュ対応（`geomId` / BLAS）、カリング / LOD リング。**段 5 まで終わってから着手する。**

---

## 4. リスクと対処

| リスク | 対処 |
|---|---|
| 一括置換で音が壊れ、原因の切り分けが不能になる | 段ごとに「同じ音」を確認。Output Scope の段別 RMS を**回帰の物差し**に使う |
| 1 フレーム遅延で回折・遮蔽の追従が鈍る | 既存スムージング（`occlusionSmoothSpeed` 等）が吸収する見込み。段 5 で実測して判断 |
| C 分類（スムージング / プール / 畳み込み）の置き場所が決まらない | 段 3 の時点で「ホストに残す」で進め、**移植を実際にやるまで決めない**（早すぎる抽象化を避ける） |
| デバッグ API を消しすぎて可視化が死ぬ | 段 4 で消すのは**内部化した計算系のみ**。可視化系は改名だけで残す |
| C++ 側の回帰を検出できない（テストが 342 行しかない） | 段 2 の前に、既知シーンでの数値回帰テストを最小限足すことを検討（別途判断） |

---

## 5. この計画で達成されること

- **SPEC §7 のワーカースレッドが載る土台ができる**（現状の同期クエリ型では載らない）
- **ホストが薄い殻になり、「移植可能」が実際に成立する**（1500 → 500 行台）
- メッシュ / カリング / LOD が**後から素直に乗る**（作り直しにならない）
- エンジンが「何をいつ計算するか」を持つので、**性能の最適化がエンジン内で完結**する
