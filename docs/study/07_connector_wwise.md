# 07 コネクタ層①：Wwise プラグイン

Wwise を「鳴らす器」にするための自前のプラグインです。音の計算はしません。届いた音をエンジンの Voice に通して、左右 2 本にして返すだけです。

```
 Wwise の Event Play_AF_Test → 音 AF_TestTone（ループ、位置あり、減衰なし）
   → バス AcousticFlow（形 = Audio Objects：音が 1 本ずつ位置付きで届く）
     → ★エフェクト AF_Renderer（このプラグイン。会社 64・プラグイン 20311）
        届いた音ごとに: モノラルに畳む → 結び付いた Voice で両耳に → 足す
        部屋の FDN と方向バスを 1 回だけ回して足す
     → ステレオ 1 本 → その先のバス → 出力
```

---

## 1. ファイル

| ファイル | 中身 |
|---|---|
| `SoundEnginePlugin/AcousticFlowFX.h` / `.cpp` | ★本体。`AK::IAkOutOfPlaceObjectPlugin`（場外型・オブジェクト処理） |
| `SoundEnginePlugin/AcousticFlowFXParams.*` | 雛形のまま（パラメータは使っていない） |
| `SoundEnginePlugin/AcousticFlowFXFactory.h` `AcousticFlowFXShared.cpp` | 雛形のまま（Wwise に登録する決まり文句） |
| `WwisePlugin/`（Authoring） | Wwise の画面にプラグインを見せるための部分。雛形のまま（画面の GUI のファイルだけ消した） |
| `AcousticFlowConfig.h` | 会社 ID 64・プラグイン ID 20311（**Wwise プロジェクトの classId と揃える**） |
| `PremakePlugin.lua` | エンジンの `AcousticEngine/include` を読む所を足しただけ |

雛形は Wwise の道具 `wp.py` が作りました（2026-09-29）：

```bash
python "C:/Audiokinetic/Wwise_2025.1.8.9170/Scripts/Build/Plugins/wp.py" new --object_processor -n AcousticFlow -t "AcousticFlow" -a "AcousticFlow" -d "AcousticFlow spatial sound engine renderer (object processor)" --with none --no-prompt
```

---

## 2. 本体の仕組み `AcousticFlowFX.cpp`

### エンジンを名前で引く（リンクしない）

```cpp
void AcousticFlowFX::BindEngine() {
    HMODULE h = ::GetModuleHandleW(L"AcousticEngine.dll");     // ★LoadLibrary しない
    if (!h) { m_engine = Engine{}; return; }                  // 無ければ素通し（時々探し直す）
    m_engine.snapshot    = (decltype(m_engine.snapshot))   ::GetProcAddress(h, "AF_HostSnapshot");
    m_engine.voiceRender = (decltype(m_engine.voiceRender))::GetProcAddress(h, "AF_VoiceRender");
    m_engine.fdnRender   = (decltype(m_engine.fdnRender))  ::GetProcAddress(h, "AF_FdnMixRender");
    m_engine.busRender   = (decltype(m_engine.busRender))  ::GetProcAddress(h, "AF_DirectionBusRender");
    /* report・unitsPerMeter・reportLevels・meterWrite も同じ */
}
```

- ★**ゲームが読んだのと同じ DLL を使う**。台帳はグローバル変数なので、別に `LoadLibrary` すると台帳が別物になって何も引けない。
  だから Unreal は起動時に DLL を読み（08 章）、プラグインは `GetModuleHandle` で探すだけ。
- エンジンの .lib をリンクしない理由：Wwise の画面（Authoring）で試聴するときにエンジンの DLL が無いと、プラグインごと読み込めなくなる。

### Init：出力をステレオに決める

```cpp
io_rFormat.channelConfig.SetStandard(AK_SPEAKER_SETUP_STEREO);   // 出力のオブジェクトは Wwise が 1 本作る
m_mono = AK_PLUGIN_ALLOC(..., sizeof(float) * kMaxFrames);         // 作業用の配列はここで全部取る（Execute では取らない）
```

`GetPluginInfo`：`bIsInPlace = false`（場外型）、`bCanProcessObjects = true`（オブジェクトを受ける）。

### Execute：1 ブロック

```
 0) 音が 5 秒届いていなければ AK_NoMoreData を返して片付けてもらう（前の Play の実体が残らないように）
 1) 係か確かめる（ClaimBlock）。1 ブロックに回すのは実体 1 つだけ（同じ器を 2 回回すと千切れる）
 2) 台帳の写しを取る（AF_HostSnapshot。取れなければ前の写し）
 3) 届いた音ごと:
      モノラルに畳み、Wwise の音量（cumulativeGain）をなめらかに掛ける
      Voice を引く: まず覚えている結び付き（Wwise の音の通し番号 → Voice の鍵）、無ければ位置でいちばん近い Voice（0.5 m 以内）
      見つかれば AF_VoiceRender で左右へ。見つからなければ素通し（左右へ 0.707 ずつ）
 4) AF_FdnMixRender → AF_DirectionBusRender（この順。FDN は方向バスへ送るので先）
 5) 計器（4 段の左右の RMS とピーク）と、結び付けの成否を台帳へ返す
```

★**結び付けは最初の 1 回だけ位置で引く**。位置は聞き手から見た座標で届き、ゲームの台帳と Wwise では数フレームずれます。
毎ブロック位置で引き直していた頃は、頭を回すと 0.5 m を超えて外れ、そのブロックだけ生の音で鳴って「飛んで」いました。

★**係（ClaimBlock）**：Wwise はバスの実体を聞き手の数だけ作ります。どの実体も同じ Voice・FDN・方向バスを回すので、
Wwise のブロック番号（`GetBufferTick`）で見張り、音を受けている実体を優先して 1 つだけが回します。

---

## 3. 作る・配る

```bash
powershell -ExecutionPolicy Bypass -File tools/dev.ps1 wwiseplugin
```

中身：

```
 wp.py build Windows_vc170 -c Release -x x64        → C:\Audiokinetic\…\SDK\x64_vc170\Release\bin\AcousticFlow.dll
 wp.py build Windows_vc170 -c Profile -x x64        → 同 Profile
 写す:
   Profile → Projects/UnityDemo/Assets/Wwise/API/Runtime/Plugins/Windows/x86_64/DSP/
   Profile → Projects/UnrealDemo/Plugins/WwiseSoundEngine/ThirdParty/x64_vc170/Profile/bin/
   Release → Projects/UnrealDemo/Plugins/WwiseSoundEngine/ThirdParty/x64_vc170/Release/bin/
```

- ★PowerShell から `wp.py` を呼ぶときは `PYTHONUTF8=1`（cp932 で読み違えて止まる）、`vswhere.exe` の場所を PATH に足す。
- ★エディタ（Unity・Unreal）が開いていると DLL が掴まれていて写せない。閉じてから。
- Wwise の画面にプラグインを見せる部分は `wp.py premake Authoring` → `wp.py build Authoring -c Release -x x64 -t vc170` で
  Wwise の `Authoring\x64\Release\bin\Plugins\` に入る（最初に 1 回）。

---

## 4. Wwise プロジェクトを組む `tools/wwise_setup.py`

画面を開かずに、WAAPI（`WwiseConsole waapi-server`）で同じ形を組みます。何度回しても同じ形になります。

```bash
python tools/wwise_setup.py unreal
```

| 組む物 | 中身 | 外すと |
|---|---|---|
| バス `AcousticFlow` | Main Audio Bus の子、形 = **Audio Objects**（768） | 音がバスの中で混ぜられてから届き、どの音源か分からない |
| エフェクト `AF_Renderer` | バスの 1 枠目に 1 つだけ（`classId` = 会社 64・プラグイン 20311） | 2 段挿すと、2 段目が 1 段目の音を消す |
| 音 `AF_TestTone` | 出力 = バス AcousticFlow、無限ループ、立体化 = Position、**減衰なし** | 立体化 None だと位置が届かず全部素通し。減衰を残すと 1/r が二重 |
| Event `Play_AF_Test` | AF_TestTone を鳴らす | |
| プロジェクトの設定 | 自動バンク・JSON など 10 個 | Unreal 用はバンクが 1 つも出ない |
| 保存・バンク | Windows のバンクを作る | |

音の素材を替えるときは、`AF_TestTone` に新しい WAV を取り込んで、使う素材をそれにし、バンクを作り直します（`wwise_setup.py` の `WAV` も替える）。

★**Wwise 側の Spatial Audio（Room・Portal・回折・Reflect）、残響の Aux 送り、減衰カーブは使わない**。エンジンの物と二重に数えるため。

---

## 5. 壊れる所（`AcousticFlowFX.h` の頭書きから）

- 同じバスに 2 段（直列）挿すと、2 段目は「このブロックはもう回った」で無音を出し、1 段目の音を消す。ゲームの画面の `plugin instances` が 1 であること。
- 別々の聞き手にだけ届く音源が 2 本あると、回らない方の音源は鳴らない。聞き手はゲームで 1 人に絞る。
- Unity 側の `TailBusRenderer` も回していると二重に鳴る（Unity の出口を Wwise にすると止まる作り）。
- Voice を作った標本化周波数が Wwise と違うと、遅れと ITD がずれる（`AF_HostShared.sampleRate` で確かめる）。
