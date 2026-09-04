# タップの組み立てを DLL へ ── ホストは写すだけ（2026-09-05）

## 何を解くか（不具合 #4 と、レビュー 2.2-3 の二重実装）
畳み込み器へ渡すタップ（直接・反射・回折の遅延と 6 帯域ゲイン、パン、向き、尾の比、mixing time）はホストの `BuildTapsForSource` 170 行が組んでいた。
- 直接タップの透過を**別の関数**（`AF_SceneComputeSoftOcclusion` ＝ 音源まわりの円盤 32 点）で引き直していて、役割1 が出す生存（`computeDirectSoft` ＝ 窓の走査線積分、09-05）と模型が違った。**扉の縁の跳びを直しても、Unity で鳴る直接タップには効いていなかった**。
- 尾の比は音源 0 の距離だけで全音源ぶんを決めていた（#4。2 m と 10 m で 25 倍ずれる）。
- 別のホスト（別のエンジン、ミドルウェアの出荷物）へ持っていくと、この 170 行を書き直すことになる。「載る」条件の 1 つ。

## 形（`AcousticEngine/src/Core/tap_builder.h`、規則はここに 1 つ）
- ホストが渡す物: つまみ `AF_TapParams`（1/r の基準距離、空気吸収の倍率、透過のこもり、回折の音量、平滑の時定数、HRTF のヒステリシス、尾の比の圧縮と天井、混ぜ半径、…。値はホストの既定と同じ）と、リスナーの向き（`AF_SceneSetListenerOrientation`）。
- DLL が返す物: `AF_VoiceProgram`（音源ごと）＝ タップ ≤ 64（遅延 ms・6 帯域・パン・向き・到来点・種類・HRTF 重み）、直接音の向き（平滑後）、HRTF に載せる回折タップ、ITDG、生存の平均、自由音場の直接レベル、尾の代表、**尾の比（音源ごと）**、mixing time、部屋の中である割合、段。
- 組み立ては `updateCompute` の最後（役割1・面の線・二次音源・尾の代表の後）。非同期でも写しに入る（同期とビット一致を回帰で確認）。
- 直接タップの透過 ＝ 役割1 の半影 soft（合成前の担い手）× 傾き × 空気吸収 × 1/r。遮蔽割合 occFrac ＝ 1 − soft[125 Hz]（旧 computeSoftOcclusion の「低域の減衰量」と同じ量）。
- ホスト側: `tapsFromEngine`（既定 ON）で `FillTapsFromEngine` が写す。OFF か古い DLL なら従来の `BuildTapsForSource`（A/B 用。採用が固まったら消す）。畳み込み器（VoiceConvolver）は `SourceTaps.TailRatio / MixingTimeMs` を音源ごとに読む。

```cpp
// 直接タップ: 役割1 の半影（振幅）→ 傾き・音量 → dB で平滑 → 空気吸収 × 1/r
occFrac = clamp01(1.0f - soft[0]);
applyTilt(soft, p.transmissionTilt, p.transmissionHighCutDb);
st.transSm[b] = smoothLog(st.transSm[b], soft[b], a);       // a = 1 − exp(−dt/tapSmoothTime)
// 尾の比（★音源ごと）: rc = 0.057√(V/RT60)、(r/rc)² を指数で圧縮、ソフトニー、部屋の中である割合
out.tailRatio = softCeiling(pow(r*r/rc², exponent), 50, ceiling) * share;
```

## ホストから移した規則（値も式も変えていない）
| 規則 | どこから | 備考 |
|---|---|---|
| 1/r（基準距離、回折は別の基準と指数） | `DistAtten` | |
| 空気吸収（6 帯域の dB/m 表 × 倍率） | `AirAbsorptionBands` | 表は DLL の定数 |
| 透過のこもり（125 Hz 基準の傾き、高域カット、音量） | `ApplyTilt` | |
| 時間方向の平滑（dB 領域） | `SmoothLog` | ★変わった所: ホストは 3 フレームに 1 回 dt を掛けていた。DLL は毎フレーム。時定数の意味が正しくなった（前は実質 3 倍速） |
| R タップの遅延 = (経路長 − 直接距離)/c | `BuildTapsForSource` | |
| F タップ = 配分 × 遮蔽割合 × 回折の音量、distanceOnly、高域カット、末尾 8 本の枠 | 同 | |
| 直接音の向き: 真の向き ↔ 到来方向を遮蔽の深さで slerp → SmoothDamp | `Update` の `_apparentDir` | Unity の SmoothDamp と同じ式を移植 |
| HRTF に載せる回折タップ: 最強、前の開口（1.5 m 以内）を margin 上回るまで乗り換えない | `SelectDiffractionHrtfTap` | |
| 尾の比: rc、指数、ソフトニー（膝 50）、部屋が無ければ外形箱 | `UpdateReverbTargetRatio` | ★音源ごとに（#4）。部屋の中である割合で縮める（#5） |
| mixing time = √V（5〜500 ms） | 同 | |
| パン（到来点とリスナーの右、等パワー） | `ComputePan` | |

## 数字（回帰 `[タップ]`、`AF_ONLY=taps`）
- 直接タップ ＝ 役割1 の半影 × 空気吸収 × 1/r: 最大差 0.0000（125 Hz: soft 0.987 → タップ 0.741 = ×(1.5/2)）。
- 反射タップの遅延の最大差 0.000 ms。影の音源に F 1 本・HRTF 1 本、見通せる音源に F 0 本。
- 尾の比: 2 m と 9.8 m で 1.42 / 34.32 ＝ 比 24.25（期待 (9.8/2)² = 24.25）。**前はどちらも音源 0 の値**。
- 非同期でも同期とビット一致（90 フレーム × 6 本、タップの組み立て込み）。

## 壊れる所
- 直接タップの模型が変わる（円盤 32 点 → 窓の走査線）。閉扉の透過は同じ材質の τ なので同じだが、縁の半影の幅と隙間の立ち上がりが変わる。**試聴で確かめる**（OFF で戻せる）。
- 平滑の時定数が正しくなったぶん、透過の追従が前より遅く聞こえる（前は実質 3 倍速）。`tapSmoothTime` で合わせ直す。
- HUD の `Status.ReverbTargetRatio` は音源 0 の値（表示だけ。畳み込み器は音源ごと）。
- `AF_VoiceProgram` は 3.9 KB。C# のマーシャルは 3 フレームに 1 回 × 音源数。出荷では固定バッファに。
- 非同期の写し（主スレッドの着手）が 12 音源で 2〜7 µs → 119 µs に増えた（program 3.9 KB × 音源数の複製）。同期の 1/50 で上限の内だが、出荷では差分だけ写す。
- ABI 7（構造体が増えた）。古い DLL では C# が従来の組み立てへ落ちる。
- ホストの `BuildTapsForSource` と `UpdateReverbTargetRatio` はまだ残っている（A/B）。採用が固まったら消す。

## 口
- C: `AF_SceneSetListenerOrientation / SetTapParams / GetVoiceProgram`、`AF_TapParams`、`AF_VoiceProgram`。
- ホスト: `AcousticFlowSceneDemo.tapsFromEngine`（既定 ON）、`SourceTaps.TailRatio / MixingTimeMs / RoomShare`。
