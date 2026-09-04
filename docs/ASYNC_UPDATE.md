# 更新の非同期化 ── 主スレッドは「入力を渡す・答えを読む」だけ（2026-09-04）

## 何を解くか
AF_SceneUpdate が同期だと、主スレッドが 1 フレームに 5〜10 ms を音響に払う（12 音源・半分が影で 3.9〜8.9 ms）。
ゲームは音響に 1 コアの 5〜10% しか渡せない。段の予算（docs/TIER_BUDGET.md）で解く量に上限を作ったが、それでも解く時間は主スレッドの物だった。
解くのを DLL のワーカースレッドへ移し、主スレッドの費用を音響の計算時間から切り離す。方向バス（音声スレッド）・段の予算（解く量）と合わせて「載る」条件の 3 つ目。

## 形（`AcousticEngine/src/Core/scene_async.h`、規則はここに 1 つ。口の分類は scene_api.cpp）
C の口 148 本を 4 つに分ける。同期モードでは全部が素通しで従来と同じ。

| 分類 | 口 | 非同期のとき |
|---|---|---|
| 設定（post） | SetListener / SetSource / UpdateInstance / UpdatePortal / SetUpdateConfig / SetMaterial / SetTierBudget / SetSourceLoudness … 40 本 | **待ち行列**へ。次の着手（主スレッド、仕事が走っていない瞬間）で順に適用 |
| 結果（snapshot） | SourceIndex / GetSourceOcclusion / GetSourceArrivalDir / GetEarlyReflections / GetDiffractionSources / GetEchogramBands / GetSourceTierEffective / GetTailShapeIndex / GetSourcePriority … 12 本 | **写し**から読む。写しは仕事が終わった次の AF_SceneUpdate で取り替わる ＝ 1 フレーム前の入力に対する答え |
| 幾何の問い合わせ（ReadGuard） | ComputeSoftOcclusion / ComputeTransmissionBands / ComputeDiffractionBands / DiffractionPath / RoomAt / Rt60At / GetPortal* / Measure* … 54 本 | **共有ロック**で走る。解く段と並走し、作り直しの段とだけ待ち合う |
| 同期（SyncGuard） | AddMaterial / AddMesh / AddInstance* / AddPortal / Bake* / FaceBake* / ExportBvh / Capture 操作 / Debug* … 25 本 | 仕事を待ち、待ち行列を反映してから排他で実行（毎フレーム呼ぶ物ではない） |

仕事は 2 段: `updatePrepare()`（部屋グラフ・自動ポータル・BVH・エッジカタログ・面の焼きの作り直し。**排他**）→ `updateCompute(dt)`（解く。幾何は読むだけ、書くのは results_ と自分の作業域。**共有**）。
同期モードは 2 つを続けて呼ぶだけで、従来の `update()` と同じ。

- 待ち行列は `std::function<void(Scene&)>` の列。着手で `applyQueue_()` が順に流す。呼び手の意味（順序）は変わらない。
- 写しは `Scene::Results` の丸ごとの複製（遮蔽・帯域・方向・早期反射・二次音源・エコグラム・段・尾の代表・id・順位）。10 µs の桁。
- `AF_SceneSourceIndex` は写しに無い id（登録直後）だけ同期の口で引き直す。
- `AF_SceneAsyncWait` で「このフレームの答えが要る」ホスト／検査が仕事を待てる。毎フレーム呼ぶと同期と同じ費用。
- `AF_SceneGetUpdateStats`: ワーカーの計算時間、写しの古さ（フレーム）、追いつかず投げられなかったフレーム数、待ち行列の長さ。
- 破棄・`AF_SceneSetAsync(0)` は仕事を待ってから畳む。ホストはシーンを破棄してから終わること（DLL のアンロード中に join しない）。

## 退けた書き方
| 案 | なぜ捨てたか |
|---|---|
| 主スレッドの設定を Scene へ直接書く（ロックで守る） | 仕事が走っている間は主スレッドが待つ。1 フレームに収まらない仕事で主スレッドが止まる ＝ 非同期にならない |
| 幾何の問い合わせも同期の口にする（仕事を待つ） | ホストは Update の直後に問い合わせを叩く（タップの組み立て・HUD）。毎フレーム待つ ＝ 非同期にならない |
| `std::shared_mutex` | 主スレッドが問い合わせを切れ目なく叩くと作り直しの排他が入れず、**120 フレームで仕事が 1 つも終わらなかった**（追いつかず 119/120）。書き手優先の RwLock に替えた（33/120） |
| 自動ポータルの作り直しを dirty で見る | 主スレッドの問い合わせ（roomAt）が先に部屋グラフを作ると dirty が消え、自動ポータルが古いまま残る（同期でも起きうる取りこぼし）。**作り直しの回数**で見る |
| 結果を二重バッファでワーカーが取り替える | 主スレッドの読みが多数の小さなゲッタなので、取り替えの瞬間に混ざる。主スレッドが着手のときに写す（読みは主スレッドだけ） |

## 数字（SceneRegressionTest `[非同期]`、2026-09-04、機械の負荷あり）
12 音源・衝立が毎フレーム回る・リスナーが歩く:

| 物 | 同期 | 非同期 |
|---|---|---|
| 主スレッドの AF_SceneUpdate | 3.9〜5.2 ms | **2.2〜5.5 µs**（着手: 写し＋待ち行列＋投げる）／0.1 µs（仕事中） |
| 答え | ― | 同期と**ビット一致**（90 フレーム × 6 本、遮蔽・段・早期反射・尾、比較 3,240） |
| 問い合わせを叩きながら 120 フレーム（1 フレーム 40 組）の壁時計 | 910〜1,345 ms | 324〜930 ms（追いつかず 33 フレーム、ワーカー 7.7 ms/回） |

追いつかなかったフレームでは答えが古いまま（写しの古さは統計で読める）。ゲームの 16 ms フレームなら 12 音源の 5 ms は収まる。

## 壊れる所
- 答えは 1 フレーム前の入力に対する物。出力の平滑（0.35 s）より十分短いが、瞬間移動の直後の 1 フレームは前の場所の答え。
- 仕事が 1 フレームに収まらないと、答えが 2 フレーム以上古くなる（設定は溜まって次の着手で全部効く）。
- 同期の口（構築・焼き・キャプチャ操作）は仕事を待つ（最大で 1 仕事ぶん）。毎フレーム呼ばないこと。
- `CaptureStatus` と `GetWorkerThreads` は数の読みだけでロック無し（毎フレーム呼ばれるため）。
- 統計の `lag` は「写しの入力のフレーム番号」と「いまの Update の番号」の差。追いついていれば 1。
- 解く段が幾何を書かない前提は、複数コアで音源を割るときの規約（「自分の枠にしか書かない」）と同じ。この規約を破る変更（解く段で遅延構築を足す等）は問い合わせと競合する。作り直しは `updatePrepare()` へ。

## 口
- C: `AF_SceneSetAsync / IsAsync / AsyncWait / GetUpdateStats`。
- ホスト: `AcousticFlowSceneDemo.asyncUpdate`（既定 ON）。HUD「更新: 非同期 ワーカー x ms／答えの古さ n フレーム／追いつかず m／待ち行列 q 主スレッド y ms」。OFF で従来の同期。
- 検査: `AF_ONLY=async SceneRegressionTest.exe` で非同期の節だけ（全体は 8 分）。`AF_ONLY=tier` で段の予算。
