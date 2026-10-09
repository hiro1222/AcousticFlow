# 08 コネクタ層②：Unreal（UE 5.6 ＋ Wwise 2025.1.8）

Unreal 側の仕事は 3 つだけです：**場面を箱にしてエンジンへ渡す・毎フレームの順に押す・台帳を書く**。音の計算は 1 行もありません。
毎フレームの呼ぶ順は Unity の `AcousticWorld` と同じです（載せ替えで写したのはこの順番）。

```
 地図（壁の箱・扉・音源・司令塔を置く）
   → AAcousticFlowWorld（司令塔）: 箱 → 耳 → 摘み → 音源 → AF_WorldUpdate → Voice へ → 台帳
   → UAcousticFlowEmitter: Voice を持ち、Wwise の Event を鳴らす
   → Wwise のバス AcousticFlow の自前プラグイン（07 章）が台帳から Voice を引いて鳴らす
```

---

## 1. 繋ぎ方

### `AcousticFlowUE.Build.cs`

```csharp
PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine", "InputCore", "AkAudio" });
string RepoRoot = Path.GetFullPath(Path.Combine(ModuleDirectory, "..", "..", "..", ".."));
string EngineInclude = Path.Combine(RepoRoot, "AcousticEngine", "include");   // ヘッダは写さず直に読む（答えを 2 つにしない）
string EngineBin = Path.Combine(RepoRoot, "build", "bin", "Release");
PublicIncludePaths.Add(EngineInclude);
PublicAdditionalLibraries.Add(Path.Combine(EngineBin, "AcousticEngine.lib"));
PublicDelayLoadDLLs.Add("AcousticEngine.dll");
RuntimeDependencies.Add("$(BinaryOutputDir)/AcousticEngine.dll", Path.Combine(EngineBin, "AcousticEngine.dll"));   // ビルドのたびに写す
```

### `AcousticFlowUE.cpp` ── 起動時に DLL を読む

`StartupModule` で `Binaries/Win64/AcousticEngine.dll` を `FPlatformProcess::GetDllHandle` で読みます。
★Wwise のプラグインは `GetModuleHandle` で同じ DLL を探すので、ゲームが先に読んでおかないと、最初に鳴り始めた音が素通しになります。
★DLL は解放しません（Wwise の音のスレッドが終了の直前まで使う）。

### `AcousticFlowSpace.h` ── 座標の橋

```cpp
// Unreal: X 前 / Y 右 / Z 上、cm。エンジン: x 右 / y 上 / z 前、m。どちらも左手系。
inline AF_Vector3 ToEngine(const FVector& U)    { return AF_Vector3{ float(U.Y * 0.01), float(U.Z * 0.01), float(U.X * 0.01) }; }
inline AF_Vector3 DirToEngine(const FVector& U) { return AF_Vector3{ float(U.Y), float(U.Z), float(U.X) }; }
```

並べ替え（X,Y,Z → z,x,y）は巡回の置換なので、向き（右手・左手）は裏返りません。
★並べ替えを忘れると上下と前後が入れ替わって部屋が横倒しになり、1/100 を忘れると部屋が 100 倍になって響きが何十秒も続きます。

### 設定 `Config/DefaultGame.ini`（Wwise の統合）

```ini
[/Script/AkAudio.AkSettings]
WwiseProjectPath=(FilePath="../UnrealDemo_WwiseProject/UnrealDemo_WwiseProject.wproj")
RootOutputPath=(Path="../../UnrealDemo_WwiseProject/GeneratedSoundBanks")   ; 空だと「Could not load project database」
AudioRouting=EnableWwiseOnly
```

Wwise の Unreal 統合（`Plugins/Wwise/`）は Wwise Launcher で入れます（リポジトリには入れない）。
Event のアセットはエディタを開かずに作れます：`UnrealEditor-Cmd.exe <uproject> -run=WwiseReconcile "-modes=create,update"`
→ `/Game/WwiseAudio/Events/Default_Work_Unit/Play_AF_Test`。

---

## 2. 司令塔 `AAcousticFlowWorld`（★`AcousticFlowWorld.h/.cpp`）

地図に 1 つ置きます。

### BeginPlay

```cpp
World = AF_WorldCreate();
CollectBoxes();                  // 印の付いた部品を箱にする（下）
ApplyRoomSettings(World);        // 部屋の割り方（RoomSeedRadiusM・bOutsideMouth）
PushKnobs();                     // 摘み
AF_WorldSetWorkers(World, 1);
AF_WorldBuild(World);            // 部屋グラフ
CreateShared();                  // 方向バス（8＋上下 2、700 Hz、合成 HRTF、57 cm）と FDN の器
AF_HostSetUnitsPerMeter(100.0f); // 台帳は Unreal の cm で書く（Wwise が見ている座標がそれなので）
for (音源の部品) Register(音源);   // AF_WorldAddEmitter と、Voice への FDN・方向バスの差し込み
```

### 箱の拾い方 `CollectBoxesInto`

- 部品のタグで選ぶ：`AF_Wall` ＝ 動かない壁（`AAcousticFlowWallBox` は自分の材質、他はコンクリート）、`AF_Door` ＝ 動く板（木の扉）。
  ★場面の全部の箱を拾わない（床の飾りや見えない当たりも箱なので、何が壁か分からなくなる）。
- 大きさ ＝ メッシュの境界箱 × 拡大率。向きは部品の右（Y）と上（Z）を写す（3 本目の前はエンジンが作る）。
- 材質はプリセットの番号ごとに 1 つ作って共有する（`AF_WorldAddMaterialPreset`）。

### Tick（毎フレーム。この順番が全部）

```
 0) エディタの中（Play していない）なら下見だけ（エリアの表示）して帰る
 1) 動く箱（扉）の位置                           AF_WorldSetBoxTransform
 2) 耳: カメラ（一人称）／キャラクターの頭（三人称、bListenerAtPawn）。向きはどちらもカメラ
                                                AF_WorldSetListener
    Wwise の聞き手をカメラの 1 人に絞る（7 キーで既定の全員と行き来）
 3) 摘み                                         PushKnobs（毎回全部押す。数 µs）
 4) 音源の位置                                   AF_WorldSetEmitter
 5) 録画中なら記録                               CaptureFrame
 6) 1 フレーム解く                               AF_WorldUpdate
 7) 部屋グラフが組み直されたら FDN の器を作り直す  AF_WorldFdnStale → RebindFdn
 8) キー: 8 エリア表示 / 9 響きの重み / 0 隣の部屋の響き / − 影のこもり
 9) 音源ごとに配分を Voice へ                    AF_WorldApplyVoice
10) 台帳                                         PublishToHost（Begin → PutVoice → SetListener → SetShared → Commit）
11) 引退した Voice を壊す                        AF_HostTick
12) 画面の 3 行の状態（0.5 秒ごと）とログ（1 秒ごと）。古い FDN の器を 1 秒後に壊す
```

### 摘み `PushKnobs`（Unity の既定と同じ値）

```cpp
AF_WorldSetWeights(World, W5);                         // 直接 1・初期 1・後期 0.126（−9 dB）・回折 1・透過 1
AF_WorldSetAdjacentLateWeight(World, WeightLateAdjacent);   // 1（エリアは音源に帰属）
AF_WorldSetShadowMuffle(World, ShadowMuffleDb);        // 6 dB（仮）
AF_WorldSetResponse(World, 0.0f, 0.04f, 0.05f, 0.06f);
AF_WorldSetRays(World, RaysPerEmitter, 40);            // 256 本・跳ね返り 40
AF_WorldSetLeakModel(World, 1);  AF_WorldSetLateThrough(World, 2);  AF_WorldSetDoorPull(World, 0.0f);
AF_WorldSetWallReflect(World, 0);  AF_WorldSetEarlyModel(World, EarlyModel);   // 4
AF_WorldSetImageSurface(World, 20.0f, 15.0f, 5.0f, 1.0f, 1.0f);
AF_WorldSetAdjacentContain(World, AdjacentContain);    // 1
AF_WorldSetDoorCoherence(World, 3000.0f);
AF_WorldSetPrecedence(World, PrecedenceDb, 0.04f);     // 6 dB
AF_WorldSetLateDistanceShape(World, LateDistancePow);  // 1.5
AF_WorldSetLaneModel(World, 1);  AF_WorldSetHeadCm(World, 57.0f);
AF_WorldSetBudget(World, 1536, 6, 10, 1);  AF_WorldSetRayGroups(World, 4);  AF_WorldSetMaxRaysPerEmitter(World, 512);
```

### 台帳 `PublishToHost`

```cpp
AF_HostBegin();
AF_HostSetListener(カメラの位置・前・上 ※Unreal の cm・Z 上のまま);
for (音源) AF_HostPutVoice(EmitterId, Voice, 位置 ※cm のまま);
AF_HostSetShared(Fdn, Bus, SampleRate);
AF_HostCommit();
```

★台帳の座標はエンジンへ渡す m・y 上ではなく、**Wwise が見ている Unreal の cm・Z 上のまま**。Unreal 版の Wwise は Unreal の座標を変換せずに Wwise へ渡すので、
台帳も同じにしないと位置で引けません。「同じ音源とみなす距離」の単位は `AF_HostSetUnitsPerMeter(100)` で伝えます。

### EndPlay

音源を外す → 台帳を空にする（プラグインが古い Voice を引かないように）→ 世界を壊す。
器（FDN・方向バス）はプラグインが写しの中で握っているかもしれないので、ここでは壊しません（プロセスの終わりに片付く）。

---

## 3. 音源 `UAcousticFlowEmitter`（★`AcousticFlowEmitter.h/.cpp`）

```cpp
void UAcousticFlowEmitter::BeginPlay() {
    AF_VoiceConfig Cfg{};
    Cfg.sampleRate = SampleRate; Cfg.maxFrames = 4096; Cfg.tailSeconds = 1.0f;   // Wwise の 1 ブロックより十分大きく
    Cfg.tapCrossfadeMs = 30.0f; Cfg.hrtfCrossfadeMs = 12.0f; Cfg.hrtfCrossoverHz = 700.0f;
    Cfg.tailFirstBlock = 64; Cfg.tailCapBlock = 8192;
    Voice = AF_VoiceCreate(&Cfg);
    Hrtf  = AF_HrtfCreateSynthetic(SampleRate);
    AF_VoiceSetHrtf(Voice, Hrtf); AF_VoiceSetHrtfEnabled(Voice, 1);
    AF_VoiceSetOutputGain(Voice, OutputGain);     // 0.6
    World->Register(this);
    // Wwise の自前の遮蔽を切る（既定は 0.2 秒ごとに見通し線を飛ばし、当たれば丸ごと絞る）
    Ak->OcclusionRefreshInterval = 0.0f;
    BindListener(カメラの AkComponent);            // 聞き手を 1 人に絞ってから鳴らす
    UAkGameplayStatics::PostEvent(Ev, GetOwner(), ...);
}
void UAcousticFlowEmitter::EndPlay(...) {
    World->Unregister(this);
    AF_HostRetireVoice(Voice);  AF_HostRetireHrtf(Hrtf);   // ★直に壊さない（音のスレッドがまだ握っている）
}
```

欄：`Event`（既定 `Play_AF_Test`）、`RadiusM`（幅 0.2 m）、`OutputGain`（0.6）、`Loudness`（予算の重要度）、`bOperated`（操作対象）。

★Wwise の遮蔽を残すと、エンジンの影（半影・回折・透過）の上に「当たったら 0」の二値が掛かり、少しでも影に入ると無音になった（2026-09-30 の試聴）。

---

## 4. 地図に置く部品

| クラス | 置き方 | 中身 |
|---|---|---|
| `AAcousticFlowWallBox`（`AcousticFlowPlaceables`） | 壁・床・天井。寸法 `SizeM`（m）、材質 `Material` | 1 m の立方体に `AF_Wall` の印。足元が床に乗る置き方 |
| `AAcousticFlowSoundSource`（同） | 音を出す所（球の中心が音の位置。目の高さなら 160 cm） | 0.4 m の球 ＋ `UAcousticFlowEmitter` |
| `AAcousticFlowDoor` | Actor の位置 ＝ 蝶番（戸口の片端・扉の高さの真ん中・**壁の厚みの真ん中**）。他の向きの壁には水平に回して置く | 角度 → 板の位置・向き。5 キーで開く・6 キーで閉じる。板に `AF_Door` の印 |
| `AAcousticFlowWorld` | 地図に 1 つ | 司令塔 |
| `EAcousticFlowMaterial`（`AcousticFlowMaterial.h`） | — | 材質の番号。エンジンのプリセットと同じ並び（足すときは末尾に、エンジンと同時に） |

```cpp
// 扉: 蝶番の Yaw で回す（AcousticFlowDoor.cpp）
const float Yaw = GetActorRotation().Yaw;
const FVector AlongU = FRotator(0.0f, Yaw, 0.0f).RotateVector(FVector(FMath::Sin(Th), FMath::Cos(Th), 0.0));
const FVector Center = GetActorLocation() + AlongU * (W * 0.5f * 100.0f);
Leaf->SetWorldLocationAndRotation(Center, FRotator(0.0f, Yaw - AngleDeg, 0.0f));
Leaf->SetWorldScale3D(FVector(ThicknessM, W, H));      // 幅 1・高さ 3・厚み 0.06 m、枠との隙間 4 mm
```

★**構造物は地図に置く**（2026-10-04 の決定）。場面をコードで組む物は退役させた（`_attic/`）。

---

## 5. ゲームモードと地図

| 地図 | ゲームモード | プレイヤー |
|---|---|---|
| `/Game/Maps/Flow_SwingDoor` | `AAcousticFlowGameMode`（プロジェクトの既定） | `AAcousticFlowWalker`：WASD で 1.4 m/s、Shift で 3 m/s、右ドラッグで向く、Q/E で上下。耳 ＝ カメラ |
| `/Game/Maps/Cave_ThirdPerson` | `AAcousticFlowThirdPersonGameMode`（World Settings で上書き） | テンプレートの `BP_ThirdPersonCharacter`。耳 ＝ キャラクターの頭（`bListenerAtPawn`） |

★歩く速さを 1.4 m/s に揃えるのは、検査の「歩行の連続性」と同じ速さで聞くため（Unreal の DefaultPawn は 12 m/s で、ITD の追従が間に合わず「飛んだ」）。

---

## 6. その他

| ファイル | 中身 |
|---|---|
| `AcousticFlowWorldAreas.cpp` | エリアの表示。エディタでも下見用の世界を組んで、床の色タイル（部屋ごと）・部屋の札（体積・RT60）・戸口の枠・箱の輪郭を描く。Play 中は 8 キー |
| `AcousticFlowWorldCapture.cpp` | 起動の引数 `-AFCapture` のとき、エンジンへ渡した物を `Saved/Wwise/AFCapture_scene.txt` に記録（`AfReplayWav` が同じ API を呼び直して音を作る） |

画面の 3 行（英数字だけ。既定のフォントに日本語が無い）：
1 行目 ＝ 結び付け（`matched / missed blocks`。**missed が 0 なら全部エンジンを通っている**）、
2 行目 ＝ 部屋・音源の五成分、3 行目 ＝ 摘みの値・プラグインの実体の数（1 であること）・入出力の音量。

### 作る

```bash
"/c/Program Files/Epic Games/UE_5.6/Engine/Build/BatchFiles/Build.bat" AcousticFlowUEEditor Win64 Development -Project="C:/dev/AcousticFlow/Projects/UnrealDemo/AcousticFlowUE.uproject" -WaitMutex
```

★エンジンを作り直したら、この Build でエンジンの DLL が `Binaries/Win64` へ写る。Unreal のエディタは閉じてから。
