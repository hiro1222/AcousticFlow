# AcousticFlow — 次世代アーキ figspec（実装設計図）

2026-07-02 確定設計（[dynamic-ray-architecture メモ] 準拠）の**実装仕様**。全動的・非ベイク・移植可能（Model 2＝broad-phase/カリング/TLAS/BLASを全部エンジン内に持つ）。ホストは薄いアダプタ。

> ステータス: 設計。実装未着手。⚠️付きは未確定マイクロ決定（要lock）。

---

## 1. データ構造（境界＝blittable POD）

```c
// 3x4 アフィン変換（row-major, [R|t]）。回転+スケール+平行移動。
typedef struct AF_Transform { float m[12]; } AF_Transform;

// 6帯域マテリアル。matId で参照（インスタンスにインライン展開しない）。
//   帯域: 125/250/500/1k/2k/4k Hz
//   transmission[6], absorption[6], scattering[6]

// ハンドル（不透明int。0起点、負=無効）
typedef int AF_MatId;
typedef int AF_GeomId;   // BLAS。予約: 0 = 組み込み単位ボックス
typedef int AF_ObjId;    // シーン内の音響オブジェクト（インスタンス）
typedef unsigned long long AF_SrcId;  // 音源
```

## 2. C ABI（Model 2：register + transform更新、カリング/TLASはエンジン）

```c
/* --- 永続（セットアップ / ジオメトリ変化時のみ）--- */
AF_MatId  af_CreateMaterial(const float* t6, const float* a6, const float* s6);
AF_GeomId af_CreateMeshGeometry(const float* verts, int nVerts,
                                const int* indices, int nIndices);  // BLAS構築1回
void      af_RefitGeometry(AF_GeomId, const float* verts, int nVerts); // 変形物のみ
void      af_DestroyGeometry(AF_GeomId);

/* --- 音響オブジェクト（インスタンス）を永続登録 --- */
AF_ObjId  af_CreateObject(AF_GeomId, AF_MatId, AF_Transform initial);
void      af_SetObjectTransform(AF_ObjId, AF_Transform);   // ★毎フレーム“動いた分だけ”
void      af_SetObjectEnabled(AF_ObjId, int on);           // 破棄せず出し入れ
void      af_DestroyObject(AF_ObjId);
//   箱プロキシは geomId=0（組み込み単位ボックス）で CreateObject。

/* --- リスナー/音源 --- */
void      af_SetListener(AF_Transform);                    // 位置+向き
void      af_SetSource(AF_SrcId, AF_Transform, /*directivity等*/ int type);
void      af_RemoveSource(AF_SrcId);

/* --- 毎フレーム 1発（ワーカースレッドをキック）--- */
void      af_Update(float dt);
//   入力(transform/listener/source)をスナップショットしてワーカーへ投げる。
//   ワーカー内部: dirty反映→BVH-of-OBB(=broad-phase兼TLAS)更新→リングカリング
//        →役割1/2を各レートで実行→LOD遷移クロスフェード→結果を back buffer へ。
//   結果は af_Get*（前回完了ぶん・double buffer の front）から読む。

/* --- 結果取得（Wwise等へ渡す）。★全て6帯域で出す（1スカラに潰さない）--- */
void      af_GetSourceOcclusion(AF_SrcId, float* out6);      // 帯域別・遮蔽(透過⊕回折後の生存)
void      af_GetSourceDiffraction(AF_SrcId, float* out6);    // 帯域別回折ゲイン（内訳・可視化用）
void      af_GetReverb(float* outRT60_6, float* outWet, float* outEarlyTaps /*…*/);
//   ※ 結果は「前回 af_Update(ワーカー)の完了ぶん」。1フレ遅延、ホスト側スムージングで吸収。

/* --- 設定（既定値あり）--- */
void      af_SetRoleRates(int role1EveryN, int role2EveryN);
void      af_SetLODRings(float nearR, float midR);          // 近mesh/中OBB/遠cull
void      af_SetProbeBudget(int maxProbeRays);              // δ探索の1発上限
```

## 3. 空間構造 = BVH-of-OBB（broad-phase と TLAS を兼用）
- **1本の BVH で、葉＝各オブジェクトの OBB**（ローカルAABB×instance transform）。AABB葉より**回転/細長物を密に包む**＝カリングもレイ枝刈りも締まる。OBBテストは `aabb.h` のローカル空間トリックで安い。
- **兼用**: カリング＝この BVH に球クエリ／レイ走査＝この BVH をレイで辿る。broad-phase と TLAS を別々に持たない。
- 葉のOBB内がメッシュなら、そのオブジェクトの **BLAS（三角形BVH）へ降りる**（2レベル＝TLAS→BLAS）。
- dirty時のみ増分更新（refit）。動体多数でも「動いた葉のrefit＋部分再構築」で安い。

## 4. フレーム内パイプライン（ワーカースレッド内）

```
a. dirty transform 反映 → BVH-of-OBB を増分更新（refit）
b. カリング: listener 中心に BVH-of-OBB を球クエリ →
     近(<nearR)=mesh / 中(<midR)=OBBプロキシ / 遠=除外
     ・オブジェクトのリング遷移はヒステリシス付き
c. TLAS＝この BVH-of-OBB そのもの（有効葉のみ有効化。BLAS参照+transform）
d. 役割1（role1EveryN で発火・音源指向）:
     各音源へ直接レイ → 遮蔽? 透過(computeTransmission)?
     遮蔽時のみ δプローブ（§4）→ Maekawa → 帯域別回折ゲイン
     直接項 = 透過 ⊕ 回折（並列2経路）
e. 役割2（role2EveryN で発火・リスナー球面）:
     fibonacci球 → バウンス（scattering）→ next-event（音源ぶん）
     → エコグラム（フルテール・低レート）→ RT60(帯域)/wet/早期タップ
     ＋ λ・ᾱ 測定（Eyring クロスチェック）
f. LOD遷移中のオブジェクト/シェル → 結果パラメータを遷移帯で lerp
g. 結果を内部バッファへ（af_Get* で取得）
```

## 5. δプローブ（役割1の回折・§#2確定）

```
入力: L(listener), S(source)。遮蔽時のみ。
1. L→S 方向の周りに 8方位×コーンで coarse プローブ
2. 各方位で blocked↔clear の境界を検出 → シルエット掠め点 P（最後にhitした点）
3. P→S 見通せれば候補 δ = |LP|+|PS|-|LS|
4. 常時両側: S起点でも同様に探索し、両側で有効な最小 δ
5. 最良方位を二分 coarse-to-fine（本数上限 maxProbeRays / δ収束で終了）
6. δ → Maekawa: N=2δ/λ, att(dB)=10log10(3+20N) → 帯域別ゲイン
単一エッジのみ（多重回折は将来）。既知楔で δ を較正。
```

## 6. 役割別レート / 更新（§#1確定）
- TLAS(BVH-of-OBB): dirty時のみ増分更新（動かなければ据置）。
- 役割1: `role1EveryN`（遮蔽まわり＝中頻度）。
- 役割2: `role2EveryN`（残響＝低頻度・フルテール許容）。
- 各役割は「現在のTLAS」を自分のレートで叩く（TLAS再構築と独立）。
- 出力はスムージング/クロスフェードで繋ぐ（フレーム跨ぎ・LOD遷移）。

## 7. スレッドモデル（af_Update = ワーカー・§確定）
- エンジンの解は**純C++（Unity/Wwise API非依存）→ ワーカースレッドで回せる**。
- `af_Update(dt)`: メインで入力(transform/listener/source)を**スナップショット**→ワーカーへ投げる（即return、メインを塞がない）。
- ワーカー: §4パイプラインを実行 → 結果を **back buffer** へ。完了で front/back を swap。
- `af_Get*`: **front buffer（前回完了ぶん）**を読む。→ **1フレ遅延**、ホスト側スムージングで吸収（#4残響の重さと相性良い）。
- 入力スナップショット＋結果double bufferで**ロックフリー**に近く保つ（1 producer/1 consumer）。

## 8. ホストアダプタ（Unity＝薄い殻の責務）
- 出力が**6帯域**なので、Wwise側に**音源ごと6帯域EQ**を用意し、`af_GetSourceOcclusion`の6値でRTPC駆動（＝周波数形状がそのまま音に出る＝「6帯域→1スカラ」卒業）。
1. 音響対象を集める（Collider/MeshFilter＋AcousticSurface）。※broad-phase/カリングは**持たない**。
2. 起動時: `af_CreateMaterial/Geometry/Object` で登録。
3. 毎フレーム: 動いた物だけ `af_SetObjectTransform`、`af_SetListener/SetSource`、`af_Update(dt)`。
4. `af_Get*` を読んで Wwise（or 各ホストのオーディオ）へ。
→ DirectX等でも 3〜4 の薄いアダプタを書くだけ。エンジン不変。

---

## ✅ 確定済み（2026-07-02）
- **occlusion/回折/透過/RT60 出力＝全て6帯域**（1スカラに潰さない）。Wwiseは音源ごと6帯域EQで駆動。
- **空間構造＝BVH-of-OBB**（葉=オブジェクトOBB。broad-phaseとTLASを兼用。メッシュは葉からBLASへ降りる2レベル）。
- **af_Update＝ワーカースレッド**（入力スナップショット＋結果double buffer、1フレ遅延をスムージング吸収）。

## ⚠️ 残り未確定（lock対象）
1. **箱プロキシ**: geomId=0 の組み込み単位ボックス方式で確定か（analytic高速パスを別に持つか）。
2. **非一様スケール**: 剛体+一様のみ許容（mesh instance）で確定か。
3. **リング遷移**: ヒステリシス幅・near/mid 既定値。
4. **FDN 設置**: (a)ホスト RoomVerb / (b)エンジンC++。残響再生 (i)IR畳み込み / (ii)FDN。← 完成段階。
