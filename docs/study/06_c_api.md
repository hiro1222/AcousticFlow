# 06 DLL の窓口（C API）と台帳

エンジンの外から触れるのは、`AcousticEngine/include/` の C の関数だけです。C++ のクラスは外に見せません
（C# の P/Invoke でも、Unreal の C++ でも、Wwise のプラグインでも同じ関数で呼べるようにするため）。

---

## 1. 作りの決まり

```c
/* acoustic_scene.h（型の定義のためだけに読む） */
#ifdef ACOUSTICENGINE_EXPORTS            /* DLL を作る側だけ（CMake の target_compile_definitions） */
  #define ACOUSTIC_API __declspec(dllexport)
#else
  #define ACOUSTIC_API __declspec(dllimport)
#endif
typedef struct AF_Vector3 { float x, y, z; } AF_Vector3;   /* m、+x 右・+y 上・+z 前 */
```

- **ハンドルは `void*`**：`AF_WorldHandle`・`AF_VoiceHandle`・`AF_HrtfHandle`・`AF_FdnMixHandle`・`AF_DirectionBusHandle`。中身は C++ の物を指すだけ。
- **実装（`src/Export/*.cpp`）は流すだけ**：ハンドル ⇄ C++ の変換、null の防御、値の範囲の頭打ち。計算は書かない。

```cpp
// world_api.cpp の書き方（どの関数もこの形）
acoustic::flow::World* asWorld(AF_WorldHandle h) { return static_cast<acoustic::flow::World*>(h); }
AF_WorldHandle AF_WorldCreate(void) { return new (std::nothrow) acoustic::flow::World(); }
void AF_WorldSetShadowMuffle(AF_WorldHandle w, float db) {
    if (acoustic::flow::World* W = asWorld(w)) W->rules.weights.shadowMuffleDb = (db > 0.0f) ? (db < 24.0f ? db : 24.0f) : 0.0f;   // NaN も 0
}
```

- **単位**：位置は m、角度は度、材質はエネルギー比、量の重みはエネルギー比（0.5 で −3 dB）。

---

## 2. 3 枚のヘッダ

### `acoustic_world.h` ── 世界（ゲームのスレッドから）

| 群 | 関数 | 何をするか |
|---|---|---|
| 作る・壊す | `AF_WorldCreate` `AF_WorldDestroy` | |
| 材質 | `AF_WorldAddMaterialPreset(番号)` `AF_WorldAddMaterial(τ,α,s)` `AF_WorldSetBoxMaterial` `AF_WorldUpdateMaterial*` | 番号は `material.cpp` のプリセットの並び |
| 箱 | `AF_WorldAddBox(中心, 半幅, 右, 上, 材質, 動く?)` `AF_WorldSetBoxTransform` `AF_WorldSetBoxActive` | 動く箱（扉）は `dynamic = 1` |
| 部屋 | `AF_WorldSetRoomCell` `AF_WorldSetRoomSeedRadius` `AF_WorldSetOutsideMouth` `AF_WorldBuild` `AF_WorldRoomCount` `AF_WorldRoomAt` `AF_WorldRoomInfo` | 部屋の割り方は組む前に |
| 耳・音源 | `AF_WorldSetListener` `AF_WorldAddEmitter` `AF_WorldSetEmitter` `AF_WorldRemoveEmitter` | |
| 1 フレーム | `AF_WorldUpdate(dt)` | ★全部の計算はここ |
| 器を繋ぐ | `AF_WorldBindFdn(FDN の器)` `AF_WorldFdnStale` `AF_WorldApplyVoice(音源, Voice, 周波数)` | |
| 摘み | `AF_WorldSetWeights(w5)` `AF_WorldSetAdjacentLateWeight` `AF_WorldSetShadowMuffle` `AF_WorldSetPrecedence` `AF_WorldSetLateDistanceShape` `AF_WorldSetEarlyModel` `AF_WorldSetImageSurface` `AF_WorldSetAdjacentContain` `AF_WorldSetDoorCoherence` `AF_WorldSetLateThrough` `AF_WorldSetLeakModel` `AF_WorldSetWallReflect` `AF_WorldSetLaneModel` `AF_WorldSetResponse` `AF_WorldSetHeadCm` | 実行中に動かしてよい |
| 予算 | `AF_WorldSetRays` `AF_WorldSetBudget(総数, 厳密, 簡易, 探り)` `AF_WorldSetRayGroups` `AF_WorldSetMaxRaysPerEmitter` `AF_WorldSetWorkers` `AF_WorldSetGpuTrace` | |
| 読むだけ | `AF_WorldMixInfo`（帳簿）`AF_WorldArrivals`（タップの一覧）`AF_WorldApertureInfo` `AF_WorldBoxInfo` `AF_WorldDiffractionInfo` `AF_WorldLateLaneShape` | 画面の表示・計器・検査 |

### `acoustic_voice.h` ── 音を作る器

| 群 | 関数 | どのスレッド |
|---|---|---|
| HRTF | `AF_HrtfCreateSynthetic(周波数)` `AF_HrtfLoadFile` `AF_HrtfDestroy` | 制御 |
| Voice | `AF_VoiceCreate(&AF_VoiceConfig)` `AF_VoiceSetHrtf` `AF_VoiceSetHrtfEnabled` `AF_VoiceSetOutputGain` `AF_VoiceSetTailLevel` `AF_VoiceSetFdnMix` `AF_VoiceSetDirectionBus` | 制御 |
| Voice を鳴らす | `AF_VoiceRender(voice, 入力, frames, 左, 右, 計器)` | ★オーディオ |
| FDN の器 | `AF_FdnMixCreate(周波数, 最大フレーム, 拡散)` `AF_FdnMixSetDirectionBus` `AF_FdnMixDestroy` | 制御 |
| FDN を鳴らす | `AF_FdnMixRender(器, frames, 左, 右)` | ★オーディオ（1 ブロックに 1 回） |
| 方向バス | `AF_DirectionBusCreateVertical(周波数, 8, 最大フレーム, 2)` `AF_DirectionBusSetCrossover` `AF_DirectionBusSetHrtf` `AF_DirectionBusSetPanSplit` | 制御 |
| 方向バスを鳴らす | `AF_DirectionBusRender(バス, frames, 左, 右)` | ★オーディオ（1 ブロックに 1 回） |
| 旧の道 | `AF_TailBus*` `AF_VoiceSetTaps` `AF_VoiceRebuildTail` など | 新コアでは使わない |

```c
typedef struct AF_VoiceConfig {
    int   sampleRate;
    int   maxFrames;        /* 1 回の Render の最大サンプル数（Wwise なら 4096 で作る） */
    float tailSeconds;      /* 旧の尾の長さ（1.0） */
    float tapCrossfadeMs;   /* タップの差し替え（30） */
    float hrtfCrossfadeMs;  /* 向きの変化（12） */
    float hrtfCrossoverHz;  /* これより下は HRIR を通さず ITD だけ（700） */
    int   tailFirstBlock, tailCapBlock;   /* 旧の尾の分割（64 / 8192） */
} AF_VoiceConfig;
```

### `acoustic_host.h` ── 台帳（ゲームのスレッド → 音のスレッド）

| 関数 | 誰が呼ぶ | 何をするか |
|---|---|---|
| `AF_HostBegin` → `AF_HostPutVoice(鍵, Voice, x,y,z)` `AF_HostSetListener` `AF_HostSetShared(FDN, バス, 周波数)` → `AF_HostCommit` | ゲーム（毎フレーム） | 「どの Voice が今どこか」を書いて公開 |
| `AF_HostSetUnitsPerMeter` | ゲーム（起動時） | 台帳の座標の単位（Unreal は 100 ＝ cm） |
| `AF_HostRetireVoice` `AF_HostRetireHrtf` → `AF_HostTick` | ゲーム | Voice を直に壊さず預け、1 秒後に壊す |
| `AF_HostSnapshot(写し先, 上限, 共有)` | 音（1 ブロックに 1 回） | 写しを取る。取れなければ −1（前の写しを使う） |
| `AF_HostReport` `AF_HostReportLevels` | 音 | 結び付けの成否と音量を返す（画面の表示用） |
| `AF_HostMeterWrite` | 音 | 左右の計器（共有メモリ `Local\AcousticFlowMeterV1`、`tools/af_meter.py` が読む） |

---

## 3. 台帳の仕組み（seqlock）`src/Export/host_api.cpp`

音のスレッドは **待てない**（待つとそのブロックの音が途切れる）。そこで、ロックの代わりに番号で確かめます。

```cpp
void AF_HostCommit(void) {                                  // ゲームのスレッド
    const unsigned s = g_seq.load(std::memory_order_relaxed);
    g_seq.store(s + 1, std::memory_order_relaxed);          // 奇数 ＝ 書いている最中
    std::atomic_thread_fence(std::memory_order_release);
    std::memcpy(g_pub.v, g_work.v, sizeof(AF_HostVoice) * g_work.n);
    g_pub.n = g_work.n;  g_pub.shared = g_work.shared;
    std::atomic_thread_fence(std::memory_order_release);
    g_seq.store(s + 2, std::memory_order_release);          // 偶数 ＝ 読んでよい
}
int AF_HostSnapshot(AF_HostVoice* out, int maxOut, AF_HostShared* outShared) {   // 音のスレッド
    thread_local AF_HostVoice tmp[AF_HOST_MAX_VOICES];
    const unsigned s0 = g_seq.load(std::memory_order_acquire);
    if (s0 & 1u) return -1;                                  // 書いている最中 → 前の写しを使ってもらう
    /* tmp へ写す */
    if (g_seq.load(std::memory_order_acquire) != s0) return -1;   // 途中で書き換わった
    std::memcpy(out, tmp, ...);                              // ★確かめてから渡す（取り損ねで前の写しを壊さない）
    return n;
}
```

- ★**Voice を直に壊さない**：音のスレッドが写しの中の古いハンドルで鳴らして落ちる。`AF_HostRetireVoice` で預けて、1 秒後に `AF_HostTick` が壊す。
- ★`AF_FdnMixRender` と `AF_DirectionBusRender` は **1 ブロックに 1 回だけ**（2 か所で回すと 2 倍鳴る）。

---

## 4. 使う順番（どのホストでも同じ）

```
 起動
   world = AF_WorldCreate()
   材質を足す → 箱を足す（動く箱は dynamic=1）→ 部屋の割り方 → 摘み
   AF_WorldBuild(world)
   bus  = AF_DirectionBusCreateVertical(48000, 8, 4096, 2)   → クロスオーバー 700 Hz、合成 HRTF、頭囲 57 cm
   fdn  = AF_FdnMixCreate(48000, 4096, 0.6)                  → AF_FdnMixSetDirectionBus(fdn, bus, 57)
   AF_WorldBindFdn(world, fdn)
   音源ごと: voice = AF_VoiceCreate(&cfg) → HRTF・音量 → id = AF_WorldAddEmitter(...)
            → AF_VoiceSetFdnMix(voice, fdn)・AF_VoiceSetDirectionBus(voice, bus)
 毎フレーム（ゲーム）
   箱 → 耳 → 摘み → 音源 → AF_WorldUpdate → (FdnStale なら器を作り直して繋ぎ直す)
   → 音源ごと AF_WorldApplyVoice → 台帳 Begin/Put/SetShared/Commit → AF_HostTick
 毎ブロック（音）
   AF_HostSnapshot → 音ごと AF_VoiceRender → AF_FdnMixRender → AF_DirectionBusRender → 足して出す
 終わり
   音源を外す → 台帳を空に → AF_WorldDestroy → Voice・HRTF は台帳に預ける（器は音のスレッドが離れてから）
```

★順番の理由：方向バスは HRTF を差してから FDN に繋ぐ（FDN はバスを差した時点の HRTF でレーンの量を揃えるため）。
世界を壊す前に音源を外す。逆順にすると、オーディオスレッドが読んでいる物を消して落ちる。
