# 04 エンジン③：配分と 1 フレーム

03 章で出した「物理の量」を、鳴らせる形（タップと送り）に配る所です。**量がどこで決まるか** はほぼ全部この章にあります。

読む順: `mix.h` → `response.h` → `distribute.h` → `mix_to_voice.h` → `world.h`

---

## 1. 配分の結果の形 `Flow/mix.h`

```cpp
struct MixTap {              // 1 本の到来
    TapKind kind;            // Direct / Transmit / Diffract / Early
    int     id;              // 素性（DSP がフレームをまたいで同じタップとして繋ぐ）
    float   delaySec;        // 到達（秒）
    float   e6[6];           // ★帯域ごとのエネルギー（振幅ではない）
    Vec3    dirLocal;        // リスナー座標の向き
    float   spread;          // 広がり（0 点 … 1 一様）
    float   width;           // 面音源の幅（角度）
};
struct FdnSend {             // 部屋の FDN への送り（後期）
    int   room;  float e6[6];
    float dir[3], focus;     // 戸口越しの尾の向きと集まり具合
    float thru6[6];          // e6 のうち、戸口から耳へ直接出す分（残りは耳の部屋の FDN へ流す）
};
struct Mix {
    MixTap  taps[64];  int tapCount;
    FdnSend sends[4];  int sendCount;
    float   onsetSec;                    // 尾の開始（最初の虚像の到達 ＝ ITDG）
    float   energy6[6], component6[5][6];// 帳簿：総量と五成分の内訳（★生の値。平滑も重みも掛けない）
};
```

- **Flow の中は全部エネルギー**。√ を取るのは DSP へ渡す直前の 1 か所だけ（下の `mix_to_voice.h`）。
- 帳簿があるので「五成分の和 ＝ 総量」を毎回確かめられます。以前、扉越しで量を 2 か所が別々に減らして 15.7 dB 小さくなっていたのを、この帳簿で捕まえた。
- 可変長の配列は使いません（毎フレーム作る物なので確保しない）。

---

## 2. 速さの表 `Flow/response.h`

| 欄 | 既定 | 何が追うか |
|---|---|---|
| `levelSec` | 0（即時） | 量（ラウドネス）。★扉を開けた瞬間に音が立ち上がる |
| `colourSec` | 0.04 s | 形（帯域の色）。鮮明さが少し遅れて追いつく |
| `statSec` | 0.05 s | レイの統計（初期・後期の総量）の残りの揺れを均す |
| `directionSec` | 0.06 s | 向き（方向バスのレーン、戸口の線音源の点） |

`Follower6` は帯域のベクトルを「量 L = Σe_b」と「形 s_b = e_b / L」に分け、別々の速さで追います。
1 つの時定数で全部を追うと、扉を開けた瞬間の立ち上がりまで鈍るからです。

---

## 3. 配分 `Flow/distribute.h`（★この章の中心）

### 受け取る物と出す物

```
 受け取る: TraceResult（自由音場の直接・初期・後期・後期の出どころ）、Visibility（見通し・τ）、
           Diffraction（前川の減衰・向き）、ImageSet / ImageSurface（虚像と面音源）、耳と音源の部屋、重み、速さ
 出す    : Mix（タップと送りと帳簿）
```

### 五成分の決め方（帳簿 ＝ 生の値）

```cpp
raw[kDirect][b]   = T.freeDirect6[b] * vis;                               // 見えている分
raw[kTransmit][b] = T.freeDirect6[b] * (1 - vis) * Vis.shadowTau6[b];     // 遮られた分が板を通る
raw[kDiffract][b] = T.freeDirect6[b] * (1 - vis) * diffraction.energy6[b];// 遮られた分が縁を回る
raw[kEarly][b]    = T.early6[b];                                          // レイ
raw[kLate][b]     = T.late6[b];                                           // レイ
```

### 出口（タップ）：平滑 × 重み × 先着の重み × 影のこもり

```cpp
// 影のこもり: 遮られた直接の道（透過・回折）の出口にだけ掛ける高域の傾き。帳簿には掛けない。
float shadow6[kNumBands];
W.shadowMuffle6(shadow6);
auto emitTap = [&](TapKind kind, int comp, float delaySec, const Vec3& dir, float spread, int id) {
    MixTap* t = out.pushTap();
    t->kind = kind; t->id = id; t->delaySec = delaySec; t->dirLocal = dir; t->spread = spread;
    const float pw = pre(delaySec);                                   // 先着の重み
    const bool shadowed = (comp == kTransmit || comp == kDiffract);
    for (int b = 0; b < kNumBands; ++b) {
        out.component6[comp][b] += raw[comp][b];                      // 帳簿は生
        t->e6[b] = sm[comp][b] * W.w[comp] * pw * (shadowed ? shadow6[b] : 1.0f);
    }
};
emitTap(TapKind::Direct,   kDirect,   T.directSec, dirLocal, 0.0f, 1);
emitTap(TapKind::Transmit, kTransmit, T.directSec, dirLocal, 0.5f, 2);
```

- **先着の重み**（`precedenceDb`、ホストは 6 dB）：最初の到達から遅れるほど出口の量を下げる。
  `重み = 10^(−6/10 · (1 − e^(−遅れ/40ms)))`。直接・透過は遅れ 0 で変わらず、部屋の響きは最大 6 dB 下がる。
  ゲームなので、物理より「最初に届く音で場所が分かる」聞こえ方を優先した演出です。
- **影のこもり**（`shadowMuffleDb`、ホストは 6 dB）：透過と回折のタップにだけ、4 kHz で −6 dB の傾き。
  透過・回折の量そのものが (1 − 見通し) に比例するので、影の縁でも連続です。

### 初期反射のタップ

既定（`earlyModel 4`）では、虚像ごとに 1 本のタップ（向き・遅れは虚像、幅は面音源、量は「レイの初期の総量 × 虚像の重み × 面音源の奥行きの重み」）。
方向の分かっていない残りは「方向なし（広がり 1）」の 1 本に回します。

### 後期の送り（響き）

```
 音源と耳が同じ部屋     → 耳の部屋の FDN へ 1 本（一様に聞く）
 音源と耳が別の部屋     → 2 本に割る
   ① 耳の部屋へ: 耳の部屋の面から来た分
   ② 音源の部屋へ: 戸口越しの面から来た分（lateOther6）。戸口の向きと集まり具合を付ける
      さらに thru6（戸口から耳へ直接）と、耳の部屋の FDN へ流す分に分ける
        ・開き具合 doorFeed（＝ 戸口の openFrac）
        ・隣の部屋の閉じ込め adjacentContain（ホストは 1 ＝ 隣の部屋の響きは全部戸口から鳴らす）
```

- **割合は平滑した値どうしの比**で取る（生の比を平滑した総量に掛けると、組の入れ替わりで割合だけ跳ぶ）。
- `lateAdjacent`（隣の部屋の響きの重み）は既定 1。2026-10-04 の原則「**エリア（響き）は音源に帰属する**。耳の部屋で響きの量を変えない。
  戸口をまたいで変わるのは包まれ具合だけ」により、別の部屋の響きを下げる重みは使っていません。
- 外の耳（`outsideListener`、洞窟の地図で使う）：耳に部屋が無いときも、音源の部屋の響きを外への口から鳴らす。

---

## 4. DSP への橋 `Flow/mix_to_voice.h`

```cpp
constexpr float kEnergyToAmp = 4.0f * kPi * 1.0f * 1.0f;          // K = 4π d_ref²（d_ref = 1 m）
inline float ampFromEnergy(float e) { return std::sqrt(std::max(0.0f, e) * kEnergyToAmp); }
// 直接音は E = 1/(4πd²) なので g = 1/d、1 m で 1.0
```

1. **タップ**：直接音を必ず index 0 に置く（DSP の決まり：フル HRTF は index 0 だけ）。
   遅れは **直接音からの相対**（直接は 0）。絶対時刻にすると 3.5 m で 10 ms の待ちが音の出だしに乗る。
   広がり s は 鏡面 √(1−s)・拡散 √s に分ける。
2. **FDN の送り**：部屋ごとに 1 本、振幅 √(帯域平均のエネルギー × K)。帯域の形は部屋ごとの重みで別に置く（`world.h`）。
3. **尾の開始**：(onsetSec − 直接の到達) を **サンプルに丸めて** 渡す（小数だとインパルスが 2 サンプルに割れて最大 −3 dB）。

---

## 5. 世界 `Flow/world.h`（★1 フレームの全部）

### 作る `build()`

```
 動かない箱だけを部屋グラフへ → 部屋ごとにプローブ（RT60）→ 戸口を控える
 → 部屋の格子をレイの場面へ（後期の出どころを引くため）→ ISM の面（動かない箱の 6 面）
 → FDN の器を「古い」にする（ホストが作り直して繋ぎ直す）
```

### 1 フレーム `update(dt)`（実際の順番。長い所はコメントで要約）

```cpp
void update(float dt) {
    if (dirty_) build();                                   // 動かない箱が変わったときだけ
    surfaces.rebuildBvh();                                 // 当たり判定の木（扉が動くので毎フレーム）
    buildTraceScene(surfaces, rules.materials, traceScene_);   // レイが見る平らな配列（1 回だけ）
    listener_.beginFrame(dt);
    updateOpenings();                                      // 戸口ごとの扉の覆い → 開き具合 → RT60
    const int lroom = roomAt(listener_.pos);               // 耳の部屋
    const float mixingSec = mixingSecFor(lroom);           // 3 × 平均自由行程 / c（10〜120 ms）
    // 耳の部屋と繋がる部屋ごとに、使う戸口を 1 つ選ぶ（面積 × 開き具合がいちばん大きい物）
    // 生きている音源を集め、予算で段と本数を決める
    budget.update(live_, listener_.pos, dt, now_, raysPerEmitter, budgetSlots_);
    // レイの注文を全部集める（音源ごとの種は固定、組を 1 つだけ）
    // GPU なら 1 回で全部流す
    // 音源ごと solve: レイ → 見通し（円盤）→ 回折 → 虚像
    // 音源ごと solveMix: 面音源 → 配分（EmitterMixer::run）→ Mix
    updateFdn(dt);                                         // 部屋ごとの FDN の重み・向き・形・戸口の線音源
}
```

★見通しは **段に関わらず円盤のまま** 解きます。以前、簡易・保持の音源を点に落としていて、扉が横切る瞬間に 1 フレームで 11.8〜13.1 dB 跳んだ（音源が 6 本を超えた分から）。
解析で出している物は削らない、削るならレイ（統計）の側、という判断です。

### 戸口の開き具合 `updateOpenings()`

戸口ごとに、動く箱（扉の板）の覆いを足して `openFrac = 1 − 覆い` を出し、板の材質の 吸音＋透過 で部屋の RT60 を出し直します（02 章の式）。

### 部屋ごとの響きを置く `updateFdn()`

```
 1) 部屋ごとに RT60 を FDN へ                         setRoomRt60
 2) 全音源の送りを部屋ごとに足し、帯域の形を重みに      setListenerWeight
 3) 戸口越しの送りから、その部屋の尾の向きと広がり      setListenerDirection
 4) 自分の部屋の響きを、壁までの距離でレーンへ配る      setListenerLaneShape
 5) 戸口の線音源                                      setPortal
```

**4) 壁までの距離で配る**（`lateDistancePow`、ホストは 1.5）：
耳から水平 8 方向と上下の 2 方向へ、細い円錐（9 本のレイ、半角 22.5°）を飛ばし、最初に当たる面までの距離 d で、
レーンの量を (1/d)^1.5 に比例させます（合計は変えない）。近い壁の側ほど響きが濃く、開いた扉の向きは遠くまで抜けるので薄くなります。
0.1 秒かけて追います。1 m と 7 m の壁なら、近い側が約 13 dB 濃くなります。

**5) 戸口の線音源**：隣の部屋の響きを、戸口の横幅に並べた 5 点（幅の −0.8, −0.4, 0, +0.4, +0.8）から鳴らします。
（下は要約。実物は `world.h` の `updateFdn` の後半）

```cpp
for (int k = 0; k < K; ++k) {                                  // K = 5
    const float off = -0.8f + 1.6f * k / (K - 1);
    const Vec3 pt = ap.rectCenter + wAxis * (off * halfW);     // 戸口の横幅の上の点
    const Vec3 u = normalize(pt - listener_.pos);
    dirs[k] = listener_.toLocal(u);                            // 点の向き
    surfaces.transmittance(listener_.pos, pt + u * 1.0f, -1, rules.materials, tau);   // 1 m 奥まで見えるか
    vis[k] = mean(tau);
}
gains[k] = sqrt(vis[k] / Σvis);                                // 見え具合で量を配る（Σg² = 1）
//   ★点の重みは 60 ms で追う（扉の板の端が視線から外れた瞬間に重みが跳び、+2.2 dB → −2.6 dB の「プツッ」が出ていた）
directG = sqrt(thru / E);  feedG = sqrt((E − thru) / E);       // 戸口から直接の分と、耳の部屋へ流す分
fdn_->setPortal(slot, srcRoom, dstRoom, dirs, gains, K, directG, feedG, headCm);
```

### 配分を Voice へ `applyToVoice()`

ホストが毎フレーム `AF_WorldApplyVoice` で呼びます。中身は `applyMixToVoice`（上の橋）だけです。

### 壊れる所（`world.h` の頭書きから）

- `bindFdn` を `build` の前に呼ぶと部屋が無い。順序は build → bindFdn。
- 部屋グラフを作り直したら、FDN の器は作り直す（器から部屋は消せない）。`AF_WorldFdnStale` がその合図。
- 動く箱を部屋グラフに入れると、閉じた扉で戸口が消える。`dynamic` の印で外す。
