# 09 コネクタ層③：Unity

Unity 側も Unreal と同じ 3 つの仕事だけです（場面を箱にして渡す・毎フレームの順に押す・台帳を書く）。
違いは「鳴らす器」を 2 つから選べることです。

| `AcousticWorld.output` | 誰が音にするか | 使う所 |
|---|---|---|
| `Unity`（0） | Unity の `OnAudioFilterRead`：音源ごとに `WorldVoice`、耳の側で `TailBusRenderer` | Wwise 無しで試すとき |
| `Wwise`（1、`Flow_*` 場面の設定） | Wwise の自前プラグイン（07 章）。C# は台帳を書くだけ | 本番の形（Unreal と同じ） |

エンジンの計算はどちらも同じで、変わるのは「音にする所を誰が呼ぶか」だけです。

---

## 1. 司令塔 `AcousticWorld.cs`（★）

`[DefaultExecutionOrder(-50)]`（音源より先に立つ）。場面に 1 つ。

### OnEnable（起動）

```
 周波数とブロック: Unity の出力の設定（Wwise なら 48000 と 4096）
 _world = AF_WorldCreate()
 耳 = AudioListener の Transform（無ければ警告。耳が原点のまま）
 CollectBoxes()                        ← 場面の BoxCollider を全部箱にする（下）
 摘み（レイ・漏れの模型・頭囲・ワーカー・部屋の割り方）
 AF_WorldBuild(_world)
 HRTF のファイル（空なら合成のまま）
 耳に TailBusRenderer を付ける（方向バスと FDN の器の持ち主）。Wwise なら renderInUnity = false（二重に鳴らさない）
 RebindFdn()                           ← FDN の器を作って世界と音源に繋ぐ
 場面の WorldVoice を全部 Register    ← AF_WorldAddEmitter、FDN・方向バスを差す
```

### 箱の拾い方 `CollectBoxes`

- 場面の **全部の BoxCollider** を拾う（耳の子と、音源の見た目の箱は除く）。Unreal は印（タグ）で選ぶので、ここだけ違う。
- 材質：`AcousticSurface` が付いていればそれ（プリセットならエンジンの表の番号、そうでなければ 6 帯域を組んで渡す）、無ければ既定（`defaultMaterial`、場面ではコンクリート）。
- 動く印：`SwingDoor` の下・非キネマティックの Rigidbody・`dynamicColliders` に入れた物。
- `ObbOf` で中心・半幅・右・上を出して `AF_WorldAddBox`。

### Update（毎フレーム。Unreal の Tick と同じ順）

```
 1) 動く箱の位置だけ送る                    AF_WorldSetBoxTransform（静的な箱を触ると部屋グラフの作り直しが走る）
 2) 材質の変化を 15 フレームに 1 回拾う     （Inspector で実行中に変えたとき）
 3) 耳                                      AF_WorldSetListener
 4) キー: 9 響きの重み / 0 隣の部屋の響き / − 影のこもり
 5) 摘みを全部押す                          AF_WorldSetWeights ほか（値が変わったかを見ずに毎回。数 µs）
 6) 音源の位置・幅・優先度・音源ごとの上書き AF_WorldSetEmitter ほか
 7) 1 フレーム解く                          AF_WorldUpdate
 8) 器が古ければ繋ぎ直す                    AF_WorldFdnStale → RebindFdn
 9) 音源ごとに配分を Voice へ                AF_WorldApplyVoice
10) Wwise なら台帳                          PublishToHost
11) 引退した Voice を壊す                   AF_HostTick
```

★Unity の台帳は **世界の座標（m）** で書きます（Unity 版の Wwise はその座標で届くため）。Unreal は cm のまま。

### 摘みの既定（Unreal と同じ値）

`weightLate 0.126`（−9 dB）・`weightLateAdjacent 1`・`shadowMuffleDb 6`・`precedenceDb 6`・`lateDistancePow 1.5`・`earlyModel 4`・
`adjacentContain 1`・`lateThroughMode 2`・`totalRays 1536`・`raysPerEmitter 256`・`rayGroups 4`・`laneModel 1`・頭囲 57 cm。

★**Unity は場面に保存された値が、スクリプトの既定より勝ちます**。決めた値は「スクリプトの既定・`Flow_SwingDoor`/`Flow_RoomWalk` の保存値・Unreal の既定」の 3 か所に同時に書く。
（扉の蝶番の位置が場面だけ古いまま残っていて、Unity と Unreal でこもりの出方が違ったことがある。）

---

## 2. 音源 `WorldVoice.cs`（★）

- `Awake`：Voice を作る（周波数とブロックは鳴らす器に合わせる）、合成 HRTF、音量 `outputGain`（0.6）。`AudioSource.spatialBlend = 0`（★Unity の空間化を残すと二重に掛かる）。
- `Start`：`AcousticWorld.Register(this)`。
- 欄：`radius`（幅）・`operated`・`loudness`・`outputGain`・`earlyModelOverride`・`adjacentContainOverride`（−1 で世界の設定）。

Unity で鳴らすとき（`output = Unity`）の音の本体：

```csharp
private void OnAudioFilterRead(float[] data, int channels)        // オーディオスレッド
{
    if (_wwise || !_ready || _voice == IntPtr.Zero) { Array.Clear(data, 0, data.Length); return; }   // Wwise のときはプラグインが鳴らす
    int frames = data.Length / channels;
    for (int f = 0; f < frames; f++) {                            // 入ってきた全チャンネルを足してモノラルに
        float s = 0f; for (int c = 0; c < channels; c++) s += data[f * channels + c];
        _dry[f] = s / channels;
    }
    var m = new Native.AFVoiceMetering();
    Native.AF_VoiceRender(_voice, _dry, frames, _outL, _outR, ref m);   // ★エンジンの Voice で両耳に
    for (int f = 0; f < frames; f++) { data[f * channels] = _outL[f]; data[f * channels + 1] = _outR[f]; }
}
```

★ここで Unity の API（`Debug.Log`・`GetComponent`）を呼ばない。オーディオスレッドから触ると落ちる。

---

## 3. 耳の側の器 `TailBusRenderer.cs`

AudioListener に付きます（`AcousticWorld` が付ける）。

- 方向バス（`AF_DirectionBusCreateVertical(周波数, 8, …, 2)` → クロスオーバー 700 Hz → HRTF → パンの割り方）と、FDN の器（`ReplaceFdnMix`、古い器は 1 秒後に壊す）を持つ。
- Unity で鳴らすとき：**AudioListener のフィルタは全音源のミックスの後に走る** ことを使い、`OnAudioFilterRead` で FDN → 方向バスを 1 回ずつ回して足す（遅れが増えない）。
- Wwise で鳴らすとき：`renderInUnity = false` で何もしない（回すのはプラグイン。二重に回すと 2 倍鳴る）。

---

## 4. 宣言（P/Invoke）

| ファイル | 中身 |
|---|---|
| `AcousticWorldBindings.cs` | `NativeWorld`（`acoustic_world.h`）と `NativeHost`（`acoustic_host.h`）。すべて `CallingConvention.Cdecl`、構造体は `LayoutKind.Sequential` で C と 1 バイトも揃える |
| `AcousticEngineBindings.cs` | `Native`（`acoustic_voice.h`：Voice・HRTF・FDN・方向バス）＋旧コアの宣言 |

新しい関数は `try { … } catch (EntryPointNotFoundException) { }` で包んでいます（古い DLL でも落ちないように）。

---

## 5. 場面に置く物

| 部品 | 中身 |
|---|---|
| `AcousticWorld` | 司令塔（`output = Wwise`） |
| `BoxCollider` ＋ `AcousticSurface` | 壁・床・天井。`AcousticSurface` は材質の印（プリセット／プリセットの形のまま量だけ／6 帯域を直に） |
| `SwingDoor` | 扉。`hinge`（蝶番の Transform）・幅 1・高さ 3・厚み 0.06・隙間 4 mm。5 キーで開く・6 キーで閉じる。蝶番の Y 回転を見る |
| `WorldVoice` ＋ AudioSource | 音源（Unity で鳴らすときは AudioSource の素材、Wwise のときは `AkAmbient` で `Play_AF_Test`） |
| `FlowWalker` | 耳を運ぶ歩き（WASD 1.4 m/s・Shift 3 m/s・右ドラッグで向く・Q/E で上下） |
| `AudioListener`（＋ `AkAudioListener`） | 耳 |
| `AkInitializer` | Wwise を立ち上げる |

Wwise の Unity 統合の設定 `Assets/WwiseSettings.xml`：
`WwiseProjectPath = ../../UnityDemo_WwiseProject/UnityDemo_WwiseProject.wproj`、`RootOutputPath = ../../UnityDemo_WwiseProject/GeneratedSoundBanks/`。
自前のプラグインの DLL は `Assets/Wwise/API/Runtime/Plugins/Windows/x86_64/DSP/` に置く（`tools/dev.ps1 wwiseplugin`）。
エンジンの DLL は `Assets/Plugins/x86_64/AcousticEngine.dll`（`tools/dev.ps1 deploy`）。

### C# の検査

```bash
powershell -ExecutionPolicy Bypass -File tools/check_csharp.ps1
```

Unity を開かずに型検査します（通らない C# を 2 回出したことがあるので、C# を触ったら必ず回す）。
