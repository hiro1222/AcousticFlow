// AcousticWorld.cs ── 新コア（Flow）のホスト（段 4。段 5 で動く箱）。
//
// ■ 役割
//   場面の箱（BoxCollider）と材質を音響エンジンの「世界」に渡し、毎フレーム リスナーと音源の位置を置いて
//   AF_WorldUpdate を 1 回呼び、配分を各 WorldVoice の Voice に写す。それだけ。
//   音の計算は全部 DLL の中（C# は「音の面の操作」だけ）。
//
// ■ 中の仕組み
//   OnEnable: 世界を作り、BoxCollider を拾って箱を足し（材質は AcousticSurface があればそれ、無ければ既定）、
//             AudioListener の Transform をリスナーに、AudioListener に TailBusRenderer（後期の器と方向バスの持ち主）を付ける。
//             動く箱（SwingDoor の下、非キネマティックの Rigidbody、dynamicColliders に入れた物）は dynamic 印で足す。
//   Update:   動く箱の位置 → リスナー → 音源 → AF_WorldUpdate → 各 Voice へ ApplyVoice。
//             FDN の器: 世界が「古い」と言ったら（部屋グラフの作り直し）、TailBusRenderer.ReplaceFdnMix で新しい器を作り、
//             AF_WorldBindFdn と各 Voice の AF_VoiceSetFdnMix を向け直す。
//   Register/Unregister: WorldVoice が自分を登録する（AF_WorldAddEmitter）。
//
// ■ 繋がり
//   受ける: 場面（BoxCollider、AcousticSurface、SwingDoor、AudioListener、WorldVoice）。
//   渡す:   DLL（NativeWorld）、WorldVoice（Voice の設定）、TailBusRenderer（器）。
//
// ■ 退けた書き方
//   ・AcousticFlowSceneDemo（旧ホスト 3,000 行）を流用: 旧 API と旧の模型の切り替えが焼き込まれている。新コアは 200 行で足りる。
//   ・動く箱を毎フレーム全部送る: 静的な箱を動かすと部屋グラフの作り直し（16 ms）が毎フレーム走る。印の付いた物だけ。
//
// ■ 壊れる所
//   ・AudioListener が無いと世界は作れても音が出ない（リスナー位置が原点のまま）。警告を出す。
//   ・器を差し替えたのに Voice の AF_VoiceSetFdnMix を向け直さないと、Voice は壊された器へ送り続ける。
//   ・扉に dynamic 印が付かないと、閉じた扉が部屋グラフに焼かれて戸口が消える（部屋が 2 つに割れたまま開かない）。
using System;
using System.Collections.Generic;
using UnityEngine;

namespace AcousticFlow
{
    [DefaultExecutionOrder(-50)]
    public sealed class AcousticWorld : MonoBehaviour
    {
        public static AcousticWorld Instance { get; private set; }

        [Header("場面")]
        [Tooltip("ON: 場面の BoxCollider を全部、音がぶつかる箱として世界に渡す（AcousticSurface があれば材質もそこから）")]
        public bool autoCollectBoxColliders = true;
        [Tooltip("AcousticSurface の無い箱の材質")]
        public AcousticMaterialPreset defaultMaterial = AcousticMaterialPreset.Default;
        [Tooltip("リスナー。空なら AudioListener を探す")]
        public Transform listener;
        [Tooltip("動く箱として扱う Collider（SwingDoor の下と非キネマティックの Rigidbody は自動で動く扱い）")]
        public List<Collider> dynamicColliders = new List<Collider>();

        // HRTF（2026-09-12 に戻した欄）。空なら合成（球の頭: 耳の時間差と左右の音量差だけで、前後・上下の手がかりは無い）。
        //   StreamingAssets の .afhr 名（kemar.afhr）を入れると実測を使う。声（直接音）と方向バス（反射・尾・戸口の線音源）の両方に効く。
        //   ★旧コアの VoiceConvolver にあった欄が作り直しで落ちていた。実測を取り込んだのに使われていない状態が前にも一度あった（AudioMonitor.cs）。
        [Tooltip("HRTF の .afhr（StreamingAssets 内の名前）。空なら合成（前後・上下の手がかり無し）。kemar.afhr で実測。再生の前に決める")]
        public string hrtfFile = "";
        // 方向バスの低域と高域の境（2026-09-12）。直接音の HRTF と同じ 700 Hz で分け、低域は素通し・高域だけ HRIR で畳む。
        //   0 で旧（全帯域を畳む）。実測の HRTF は低域が落ちているので、分けないと反射と尾の低域が薄くなる。
        [Tooltip("方向バスの低域と高域の境（Hz、既定 700）。0 で旧（全帯域を畳む）。再生の前に決める")]
        public float busCrossoverHz = 700f;
        /// 実際に読めた HRTF の名前（情報タブ用）。
        public string HrtfName { get; private set; } = "合成（球の頭）";
        private IntPtr _hrtfShared = IntPtr.Zero;   // 声が借りる。Detach で自前の合成へ戻してから壊す

        [Header("レイと予算（段 8）")]
        [Tooltip("予算が無制限（totalRays = 0）のときの 1 音源の本数")]
        public int raysPerEmitter = 256;
        // ★エンジン側の上限（既定 512）。予算が余っていても、これより多くは飛ばさない。
        //   GPU で本数を桁で増やすときは raysPerEmitter と一緒にこれを上げる。上げないと 512 で頭打ちになり、
        //   gpuTrace を入れても聞こえ方が変わらない。
        [Tooltip("1 音源の本数の上限（既定 512）。GPU で本数を増やすときは raysPerEmitter と一緒に上げる")]
        public int maxRaysPerEmitter = 512;

        // 閉じた扉から漏れる回折の扱い（試聴の A/B）。0 は厚さ 6 cm の板を通り抜ける経路が許容 7 cm に
        // 収まって残るので漏れる。1 は貫通を箱自身の薄さと比べる。2 は回折の量に戸口の空き具合を掛ける。
        // 実行中に変えられる。角度に二値は置いていない。
        [Tooltip("閉じた扉から漏れる回折。0 旧 / 1 厚みの割合（既定。10°以降は 0 と同じ）/ 2 口の空き具合（戸口の方向が薄まる）/ 3 両方。実行中に変えられる")]
        [Range(0, 3)] public int leakModel = 1;

        // 隣の部屋にいるときの、向こうの部屋の響きの鳴らし方（段 2-f / 2-g）。同じ部屋の音源はどれでも変わらない（LEV のまま）。
        //   0 旧: 全部を耳の部屋から一様に
        //   1 戸口越しに届く分を、戸口の向きの点から
        //   2 戸口の線音源（既定）: 向こうの部屋の響きを戸口の横幅から HRTF で鳴らし、耳の部屋の響きも戸口の音から鳴り始める
        [Tooltip("隣の部屋の響きの鳴らし方。0 旧（一様）/ 1 戸口の向きの点 / 2 戸口の線音源（既定）。実行中に変えられる")]
        [Range(0, 2)] public int lateThroughMode = 2;
        // 戸口寄せ（2026-09-12）。lateThroughMode 2 のとき、自分の部屋へ流す響きのうちこの割合を戸口の線音源から直接鳴らす。
        //   総量は変えない。0 ＝ レイの割合のまま。壁の陰では戸口から直接が 1% 台で全方向に負けるので、耳で決める。配分タブの割合を見ながら動かす。
        [Tooltip("戸口寄せ（0..1、既定 0）。自分の部屋へ流す響きのうち、戸口から直接鳴らす側へ移す割合。総量は変えない。実行中に動かせる")]
        [Range(0f, 1f)] public float doorPull = 0f;
        // 壁越しの反射（2026-09-12、既定 切）。入れると旧: 壁を横切った反射と残響も透過率で薄めて届ける
        //   （向こうの部屋の反射÷直接の比がそのまま残り、透過の直接音の数 ms 後ろに写しが立って「ダブる」）。
        //   切ると壁を抜けるのは透過の直接音だけ。開いた戸口を通る分はどちらでも同じ。
        [Tooltip("壁越しの反射（既定 切）。入れると旧: 壁を横切った反射と残響も薄めて届ける。切ると壁を抜けるのは透過の直接音だけ。実行中に変えられる")]
        public bool wallReflections = false;
        // 初期反射の出し方（2026-09-13）。0 虚像（鏡に映した音源の点）／1 受取面（既定）。
        //   受取面: 面をレイの受取面にして、面ごと・1 回目の当たりかどうかでタップを立てる。量は面が受けたレイ、耳へは見込みの大きさで配る。
        //   壁に近いほど 1 回目の反射が早く・広く鳴る。実行中に切り替えて聞き比べられる。
        [Tooltip("初期反射の出し方。0 虚像（点）／1 受取面（既定。面ごとのタップ、壁が近いと早く広く）。実行中に変えられる")]
        [Range(0, 1)] public int earlyModel = 1;
        // 隣の部屋の閉じ込め（2026-09-14、既定 1）。音源が別の部屋にいるとき、その残響・反射を今いる部屋で響かせず、戸口から鳴らす。
        //   後期: 今いる部屋の響きへ流していた分を戸口の線音源から直接。初期: 今いる部屋の面の反射を戸口の 1 本へ。総量は変えない。
        //   0 で物理のまま（戸口から入った音が今いる部屋でも響く）。演出の摘み。
        [Tooltip("隣の部屋の閉じ込め（0..1、既定 1）。1 で隣の部屋の音は戸口からだけ鳴る（今いる部屋で響かない）。0 で物理のまま。実行中に動かせる")]
        [Range(0f, 1f)] public float adjacentContain = 1f;
        // 戸口の線音源の低域の相関（2026-09-12）。境より下は 5 点が同じ波形（振幅の和で 1）、上は点ごとに別の波形。0 で旧。
        //   近づいて 5 点が広い角度に散ると、全部が無相関だと両耳の相関が落ちて「前の雲」になる。低域を揃えると戸口の方向が立つ。
        //   IACC（戸口の正面 0.5〜3 m）: 0 で 0.34〜0.54（近いほど雲）、3000 で 0.59〜0.62（距離によらず）、6000 以上で 0.8〜0.9（点に寄って幅が消える）。
        [Tooltip("戸口の線音源の低域の相関の境（Hz、既定 3000）。下は 5 点が同じ波形、上は別の波形。0 で旧（近いと雲）。6000 以上は点に寄る。実行中に動かせる")]
        [Range(0f, 8000f)] public float doorCoherenceHz = 3000f;
        // 先着の重み（2026-09-12）。最初の到達（直接の到達時刻）から遅れる到来ほど耳に出す量を下げる。帳簿（配分タブの内訳）は物理のまま。
        //   直接・透過は変わらず、回折・虚像は少し、部屋の響きは大きく下がる。戸口の線音源は中間。0 dB で今までと同じ。
        //   ゲームなので完全な物理でなく聞こえ方を優先する（発注者の指示）。窓は先行音効果の 40 ms が目安。
        [Tooltip("先着の重み: 十分遅い到来の下げ幅（dB、既定 6）。0 で物理のまま。直接は変わらず、部屋の響きが下がる。実行中に動かせる")]
        [Range(0f, 24f)] public float precedenceDb = 6f;
        [Tooltip("先着の重みの窓（ms、既定 40 ＝ 先行音効果）。この時間で下げ幅の 63% に達する")]
        [Range(5f, 200f)] public float precedenceMs = 40f;

        // 尾のレーンの作り（方向バスへ載せる尾）
        //   0 耳ごとの行（旧）: 点の向きでも左右が別の波形。ITD が効かず、向きが変わると尾の波形が入れ替わる
        //   1 点と拡散を分ける（既定）: 点は 1 本の波形＋点の向きの ITD。自室（一様）は 0 と同じ音
        //   戸口の線音源（lateThroughMode 2）の部屋はレーンを通らない。効くのは戸口で繋がっていない部屋と、lateThroughMode 1 のとき
        [Tooltip("尾のレーンの作り。0 耳ごとの行（旧）/ 1 点と拡散を分ける（既定）。実行中に変えられる")]
        [Range(0, 1)] public int laneModel = 1;

        // レイを GPU で解く（既定 切）。音は作らない ── GPU が出すのは幾何と統計だけで、
        // 音にするのは今までどおりエンジン。ホストのデバイスは借りず、エンジンが自前で持つ。
        // GPU が無い機械や積めない場合は黙って CPU のまま動く（GpuActive で実際の状態が読める）。
        // ★本数が少ないと転送の手間が勝つ。効くのは raysPerEmitter を桁で増やしたとき。
        [Tooltip("レイを GPU で解く（既定 切）。本数を増やすときに効く。実際に使えているかは下の表示で確認")]
        public bool gpuTrace = false;
        public bool GpuActive { get; private set; }
        public int maxBounces = 40;
        [Tooltip("1 フレームの総レイ数。0 で無制限。音源が増えても総量は変わらない（設計文書 Ⅶ）")]
        public int totalRays = 1536;
        [Tooltip("厳密の枠。ここに入った音源だけ 幅・虚像・回折を全部解く")]
        public int fullSlots = 6;
        [Tooltip("簡易の枠。点音源として少ない本数で解く（虚像は作らない）")]
        public int lightSlots = 10;
        [Tooltip("保持のうち毎フレーム解き直す本数（順繰り）。0 で眠らせたまま")]
        public int probesPerFrame = 1;
        [Tooltip("レイをこの数の組に分け、毎フレーム 1 組だけ飛ばす（設計文書 Ⅶ のフレーム分散）。1 で分散なし。\n"
                 + "静止していれば組を回しても揺れない（組ごとの結果は同じ）。動くと この数のフレームで入れ替わる（4 = 43 ms @ 60fps）")]
        [Range(1, 8)] public int rayGroups = 4;
        [Tooltip("音源ごとのループを回すスレッド数。1 で直列（既定）。★Unity は既に全コアを使うので上げるのは測ってから")]
        public int workers = 1;
        public float headCircumferenceCm = 57f;

        [Header("世界の重み（五成分に 1 つずつ。1 = 物理どおり）")]
        [Range(0f, 4f)] public float weightDirect = 1f;
        [Range(0f, 4f)] public float weightEarly = 1f;
        [Range(0f, 4f)] public float weightLate = 1f;
        [Range(0f, 4f)] public float weightDiffract = 1f;
        [Range(0f, 4f)] public float weightTransmit = 1f;

        [Header("速度の表（秒。0 = 毎フレーム）")]
        public float levelSec = 0f;
        public float colourSec = 0.04f;
        public float statSec = 0.05f;
        public float directionSec = 0.06f;

        public IntPtr Handle => _world;
        public int RoomCount => _world != IntPtr.Zero ? NativeWorld.AF_WorldRoomCount(_world) : 0;
        public int ApertureCount => _world != IntPtr.Zero ? NativeWorld.AF_WorldApertureCount(_world) : 0;
        public float ApertureOpenFrac(int i) => _world != IntPtr.Zero ? NativeWorld.AF_WorldApertureOpenFrac(_world, i) : 1f;
        public float UpdateMs { get; private set; }
        public int SpentRays => _world != IntPtr.Zero ? NativeWorld.AF_WorldSpentRays(_world) : 0;
        public int TierOf(WorldVoice v) => (_world != IntPtr.Zero && v != null && v.EmitterId >= 0) ? NativeWorld.AF_WorldEmitterTier(_world, v.EmitterId) : 2;
        public int RaysOf(WorldVoice v) => (_world != IntPtr.Zero && v != null && v.EmitterId >= 0) ? NativeWorld.AF_WorldEmitterRays(_world, v.EmitterId) : 0;
        public int BoxCount => _boxes.Count;
        public int DynamicCount { get; private set; }
        public IReadOnlyList<WorldVoice> Voices => _voices;
        public TailBusRenderer TailHost => _tail;

        private struct Box { public Collider col; public int id; public bool dynamic; public int matId; public AcousticSurface surf; public string key; }
        private int _defaultMatId = -1;                       // defaultMaterial の材質（実行中に書き換える）
        private AcousticMaterialPreset _appliedDefault;
        private IntPtr _world;
        private readonly List<Box> _boxes = new List<Box>();
        private readonly List<WorldVoice> _voices = new List<WorldVoice>();
        private TailBusRenderer _tail;
        private int _sampleRate = 48000;
        private int _maxFrames = 1024;
        private int _appliedWorkers = 1;
        private readonly float[] _w5 = new float[5];

        private void OnEnable()
        {
            Instance = this;
            _sampleRate = AudioSettings.outputSampleRate;
            AudioSettings.GetDSPBufferSize(out int bufLen, out _);
            _maxFrames = Mathf.Max(256, bufLen);

            _world = NativeWorld.AF_WorldCreate();
            if (_world == IntPtr.Zero) { Debug.LogError("[AcousticWorld] 世界を作れませんでした（DLL）"); enabled = false; return; }

            if (listener == null)
            {
                var al = FindFirstObjectByType<AudioListener>();
                if (al != null) listener = al.transform;
                else Debug.LogWarning("[AcousticWorld] AudioListener が見つかりません。リスナーは原点のままです");
            }
            CollectBoxes();
            NativeWorld.AF_WorldSetRays(_world, raysPerEmitter, maxBounces);
            NativeWorld.AF_WorldSetLeakModel(_world, leakModel);
            NativeWorld.AF_WorldSetHeadCm(_world, headCircumferenceCm);
            NativeWorld.AF_WorldSetWorkers(_world, workers);       // 起動時に 1 回（スレッドを毎フレーム作り直さない）
            _appliedWorkers = workers;
            NativeWorld.AF_WorldBuild(_world);

            // HRTF のファイル（空なら合成のまま）。声には Register で貸し、方向バスには TailBusRenderer が同じ名前で自分で読む。
            if (!string.IsNullOrEmpty(hrtfFile))
            {
                string path = System.IO.Path.Combine(Application.streamingAssetsPath, hrtfFile);
                if (System.IO.File.Exists(path))
                {
                    try { _hrtfShared = Native.AF_HrtfLoadFile(path); } catch (EntryPointNotFoundException) { _hrtfShared = IntPtr.Zero; }
                    if (_hrtfShared != IntPtr.Zero) HrtfName = hrtfFile;
                    else Debug.LogWarning("[AcousticWorld] HRTF を読めませんでした（合成のまま）: " + path);
                }
                else Debug.LogWarning("[AcousticWorld] HRTF が見つかりません（合成のまま）: " + path);
            }
            if (listener != null)
            {
                _tail = listener.GetComponent<TailBusRenderer>();
                if (_tail == null) _tail = listener.gameObject.AddComponent<TailBusRenderer>();
                _tail.hrtfFile = (_hrtfShared != IntPtr.Zero) ? hrtfFile : "";   // 方向バスも同じ HRTF（読めた時だけ）
                _tail.busCrossoverHz = busCrossoverHz;
                _tail.enableSharedTail = false;        // 畳み込みの尾は使わない
                RebindFdn();
            }
            foreach (var v in FindObjectsByType<WorldVoice>(FindObjectsSortMode.None)) Register(v);
            Debug.Log($"[AcousticWorld] 世界: 箱 {_boxes.Count}（動く物 {DynamicCount}）/ 部屋 {RoomCount} / 戸口 {ApertureCount} / 音源 {_voices.Count}（レイ {raysPerEmitter} 本）");
        }

        private void OnDisable()
        {
            foreach (var v in _voices) v.Detach();            // 貸した HRTF は Detach で自前の合成へ戻る
            _voices.Clear();
            _boxes.Clear();
            if (_world != IntPtr.Zero) { NativeWorld.AF_WorldDestroy(_world); _world = IntPtr.Zero; }
            // ★声を全部外した後で壊す（オーディオスレッドが貸した HRTF を読んでいる最中に消さない）。
            if (_hrtfShared != IntPtr.Zero) { Native.AF_HrtfDestroy(_hrtfShared); _hrtfShared = IntPtr.Zero; }
            if (Instance == this) Instance = null;
        }

        // ── 実行中の材質の調整（2026-09-12）──
        //   Inspector の defaultMaterial と各 AcousticSurface の変化を拾い、エンジンの材質を書き換える（足さない）。
        //   静的な箱に効く変更はエンジンが次の更新で部屋（RT60・ISM の面）を組み直す（1 回、数百 ms の引っかかり）。
        //   動く箱（扉の板）だけの変更は組み直さず、透過・吸音・散乱はそのフレームから効く。
        //   AcousticSurface を実行中に付ければその面だけ別の材質になり、外せば既定に戻る（箱の登録そのものは再生の前）。
        private void ApplyMaterialChanges()
        {
            if (_defaultMatId >= 0 && defaultMaterial != _appliedDefault)
            {
                try { NativeWorld.AF_WorldUpdateMaterialPreset(_world, _defaultMatId, (int)defaultMaterial); }
                catch (EntryPointNotFoundException) { return; }
                _appliedDefault = defaultMaterial;
                Debug.Log("[AcousticWorld] 既定の材質 → " + defaultMaterial + "（次の更新で部屋を組み直す）");
            }
            for (int i = 0; i < _boxes.Count; i++)
            {
                var b = _boxes[i];
                // 付け外し。付いたらその面に材質を 1 つ足して差し替え、外れたら既定へ戻す。
                var surfNow = (b.col != null) ? b.col.GetComponent<AcousticSurface>() : null;
                if (!ReferenceEquals(surfNow, b.surf))
                {
                    try
                    {
                        if (surfNow != null)
                        {
                            int mat;
                            if (surfNow.mode == AcousticSurfaceMode.Preset) mat = NativeWorld.AF_WorldAddMaterialPreset(_world, (int)surfNow.material);
                            else { var m = surfNow.Resolve(); mat = NativeWorld.AF_WorldAddMaterial(_world, m.transmission, m.absorption, m.scattering); }
                            NativeWorld.AF_WorldSetBoxMaterial(_world, b.id, mat);
                            b.matId = mat; b.surf = surfNow; b.key = SurfaceKey(surfNow);
                            Debug.Log("[AcousticWorld] AcousticSurface が付いた: " + b.col.name + (b.dynamic ? "（動く箱: 組み直しなし）" : "（次の更新で部屋を組み直す）"));
                        }
                        else
                        {
                            NativeWorld.AF_WorldSetBoxMaterial(_world, b.id, _defaultMatId);
                            b.matId = _defaultMatId; b.surf = null; b.key = null;
                            Debug.Log("[AcousticWorld] AcousticSurface が外れた: " + b.col.name + "（既定の材質へ）");
                        }
                    }
                    catch (EntryPointNotFoundException) { return; }
                    _boxes[i] = b;
                    continue;
                }
                if (b.surf == null) continue;
                string key = SurfaceKey(b.surf);
                if (key == b.key) continue;
                try
                {
                    if (b.surf.mode == AcousticSurfaceMode.Preset) NativeWorld.AF_WorldUpdateMaterialPreset(_world, b.matId, (int)b.surf.material);
                    else { var m = b.surf.Resolve(); NativeWorld.AF_WorldUpdateMaterial(_world, b.matId, m.transmission, m.absorption, m.scattering); }
                }
                catch (EntryPointNotFoundException) { return; }
                b.key = key; _boxes[i] = b;
                Debug.Log("[AcousticWorld] 材質を書き換え: " + b.surf.name + (b.dynamic ? "（動く箱: 組み直しなし）" : "（次の更新で部屋を組み直す）"));
            }
        }
        private static string SurfaceKey(AcousticSurface s)
        {
            var sb = new System.Text.StringBuilder(96);
            sb.Append((int)s.mode).Append('|').Append((int)s.material).Append('|')
              .Append(s.absorptionScale.ToString("F3")).Append('|').Append(s.transmissionLossOffsetDb.ToString("F2"));
            AppendBands(sb, s.customAbsorption); AppendBands(sb, s.customTransmissionLossDb); AppendBands(sb, s.customScattering);
            return sb.ToString();
        }
        private static void AppendBands(System.Text.StringBuilder sb, float[] v)
        {
            sb.Append('|');
            if (v == null) return;
            for (int i = 0; i < v.Length; i++) sb.Append(v[i].ToString("F3")).Append(',');
        }

        private bool IsDynamic(Collider col)
        {
            if (dynamicColliders.Contains(col)) return true;
            if (col.GetComponentInParent<SwingDoor>() != null) return true;
            var rb = col.GetComponentInParent<Rigidbody>();
            return rb != null && !rb.isKinematic;
        }

        private static void ObbOf(Collider c, out Vector3 center, out Vector3 half, out Vector3 right, out Vector3 up)
        {
            var box = c as BoxCollider;
            var t = c.transform;
            Vector3 s = t.lossyScale;
            center = t.TransformPoint(box.center);
            half = new Vector3(Mathf.Abs(box.size.x * s.x), Mathf.Abs(box.size.y * s.y), Mathf.Abs(box.size.z * s.z)) * 0.5f;
            right = t.right; up = t.up;
        }

        private void CollectBoxes()
        {
            _boxes.Clear(); DynamicCount = 0;
            if (!autoCollectBoxColliders) return;
            // ★材質は「既定に 1 つ」＋「AcousticSurface の付いた面ごとに 1 つ」で、面どうしで共有しない（2026-09-12）。
            //   実行中に Inspector で 1 面を変えたとき、同じプリセットの他の面を巻き込まずに書き換えられるように。
            _defaultMatId = NativeWorld.AF_WorldAddMaterialPreset(_world, (int)defaultMaterial);
            _appliedDefault = defaultMaterial;
            foreach (var col in FindObjectsByType<BoxCollider>(FindObjectsSortMode.None))
            {
                if (col == null || !col.enabled) continue;
                if (listener != null && col.transform.IsChildOf(listener)) continue;
                if (col.GetComponentInParent<WorldVoice>() != null) continue;      // 音源の見た目の箱は壁にしない
                int mat; string key = null;
                var surf = col.GetComponent<AcousticSurface>();
                if (surf != null)
                {
                    if (surf.mode == AcousticSurfaceMode.Preset) mat = NativeWorld.AF_WorldAddMaterialPreset(_world, (int)surf.material);   // 値はエンジンの表が正
                    else { var m = surf.Resolve(); mat = NativeWorld.AF_WorldAddMaterial(_world, m.transmission, m.absorption, m.scattering); }
                    key = SurfaceKey(surf);
                }
                else mat = _defaultMatId;
                ObbOf(col, out var center, out var half, out var right, out var up);
                bool dyn = IsDynamic(col);
                int id = NativeWorld.AF_WorldAddBox(_world, new AFVector3(center), new AFVector3(half), new AFVector3(right), new AFVector3(up), mat, dyn ? 1 : 0);
                _boxes.Add(new Box { col = col, id = id, dynamic = dyn, matId = mat, surf = surf, key = key });
                if (dyn) DynamicCount++;
            }
        }

        // ── 音源の登録（WorldVoice が呼ぶ）──
        public void Register(WorldVoice v)
        {
            if (v == null || _world == IntPtr.Zero || _voices.Contains(v)) return;
            int id = NativeWorld.AF_WorldAddEmitter(_world, new AFVector3(v.transform.position), v.radius);
            if (id < 0) return;
            _voices.Add(v);
            v.Attach(id, _tail != null ? _tail.FdnMixHandle : IntPtr.Zero, _tail != null ? _tail.GetOrCreateDirectionBus(_maxFrames) : IntPtr.Zero);
            v.SetSharedHrtf(_hrtfShared);                      // 空（Zero）なら声は自前の合成のまま
        }
        public void Unregister(WorldVoice v)
        {
            if (v == null || !_voices.Remove(v)) return;
            if (_world != IntPtr.Zero && v.EmitterId >= 0) NativeWorld.AF_WorldRemoveEmitter(_world, v.EmitterId);
            v.Detach();
        }

        private void RebindFdn()
        {
            if (_tail == null || _world == IntPtr.Zero) return;
            var fdn = _tail.ReplaceFdnMix(_maxFrames);      // 古い器は TailBusRenderer が 1 秒後に壊す
            NativeWorld.AF_WorldBindFdn(_world, fdn);
            foreach (var v in _voices) v.SetFdn(fdn);
        }

        private void Update()
        {
            if (_world == IntPtr.Zero) return;
            long t0 = System.Diagnostics.Stopwatch.GetTimestamp();
            // 動く箱（扉）の位置。静的な箱は触らない（触ると部屋グラフが作り直される）。
            foreach (var b in _boxes)
            {
                if (!b.dynamic || b.col == null) continue;
                ObbOf(b.col, out var center, out _, out var right, out var up);
                NativeWorld.AF_WorldSetBoxTransform(_world, b.id, new AFVector3(center), new AFVector3(right), new AFVector3(up));
            }
            if ((Time.frameCount % 15) == 0) ApplyMaterialChanges();   // 実行中の材質の調整（0.25 s ごとに拾う）
            if (listener != null)
                NativeWorld.AF_WorldSetListener(_world, new AFVector3(listener.position), new AFVector3(listener.forward), new AFVector3(listener.up));
            _w5[0] = weightDirect; _w5[1] = weightEarly; _w5[2] = weightLate; _w5[3] = weightDiffract; _w5[4] = weightTransmit;
            NativeWorld.AF_WorldSetWeights(_world, _w5);
            NativeWorld.AF_WorldSetResponse(_world, levelSec, colourSec, statSec, directionSec);
            NativeWorld.AF_WorldSetRays(_world, raysPerEmitter, maxBounces);
            NativeWorld.AF_WorldSetLeakModel(_world, leakModel);   // 実行中に切り替えられる（試聴の A/B）
            NativeWorld.AF_WorldSetLateThrough(_world, lateThroughMode);   // 同上
            NativeWorld.AF_WorldSetDoorPull(_world, doorPull);             // 同上
            NativeWorld.AF_WorldSetWallReflect(_world, wallReflections ? 1 : 0);   // 同上
            NativeWorld.AF_WorldSetEarlyModel(_world, earlyModel);                  // 同上
            NativeWorld.AF_WorldSetAdjacentContain(_world, adjacentContain);        // 同上
            NativeWorld.AF_WorldSetDoorCoherence(_world, doorCoherenceHz);         // 同上
            NativeWorld.AF_WorldSetPrecedence(_world, precedenceDb, precedenceMs * 0.001f);   // 同上
            NativeWorld.AF_WorldSetLaneModel(_world, laneModel);           // 同上
            NativeWorld.AF_WorldSetGpuTrace(_world, gpuTrace ? 1 : 0);
            GpuActive = NativeWorld.AF_WorldGpuActive(_world) != 0;
            NativeWorld.AF_WorldSetBudget(_world, totalRays, fullSlots, lightSlots, probesPerFrame);
            NativeWorld.AF_WorldSetRayGroups(_world, rayGroups);
            NativeWorld.AF_WorldSetMaxRaysPerEmitter(_world, maxRaysPerEmitter);
            if (workers != _appliedWorkers) { NativeWorld.AF_WorldSetWorkers(_world, workers); _appliedWorkers = workers; }
            foreach (var v in _voices)
                if (v != null && v.EmitterId >= 0)
                    NativeWorld.AF_WorldSetEmitter(_world, v.EmitterId, new AFVector3(v.transform.position), v.radius, v.operated ? 1 : 0, v.loudness);

            NativeWorld.AF_WorldUpdate(_world, Mathf.Max(1e-4f, Time.deltaTime));
            if (NativeWorld.AF_WorldFdnStale(_world) != 0) RebindFdn();

            foreach (var v in _voices)
                if (v != null && v.EmitterId >= 0 && v.VoiceHandle != IntPtr.Zero)
                    NativeWorld.AF_WorldApplyVoice(_world, v.EmitterId, v.VoiceHandle, _sampleRate);
            UpdateMs = (System.Diagnostics.Stopwatch.GetTimestamp() - t0) * 1000f / System.Diagnostics.Stopwatch.Frequency;
        }

        /// 情報タブ用。
        public bool TryGetMixInfo(WorldVoice v, out AFMixInfo info)
        {
            info = default;
            if (_world == IntPtr.Zero || v == null || v.EmitterId < 0) return false;
            return NativeWorld.AF_WorldMixInfo(_world, v.EmitterId, out info) != 0;
        }
        /// 地図用（AF ツールの配分タブ）: エンジンが使っている箱の数と形、戸口の形。DLL が古くて口が無ければ 0 / false。
        public int NativeBoxCount
        {
            get
            {
                if (_world == IntPtr.Zero) return 0;
                try { return NativeWorld.AF_WorldBoxCount(_world); }
                catch (EntryPointNotFoundException) { return 0; }
            }
        }
        public bool TryGetBox(int i, out AFBoxInfo info)
        {
            info = default;
            if (_world == IntPtr.Zero) return false;
            try { return NativeWorld.AF_WorldBoxInfo(_world, i, out info) != 0; }
            catch (EntryPointNotFoundException) { return false; }
        }
        public bool TryGetAperture(int i, out AFApertureInfo info)
        {
            info = default;
            if (_world == IntPtr.Zero) return false;
            try { return NativeWorld.AF_WorldApertureInfo(_world, i, out info) != 0; }
            catch (EntryPointNotFoundException) { return false; }
        }

        /// 聞こえている音の到来（AF ツールの配分タブ用）。書いた数を返す。DLL が古くて口が無ければ 0。
        public int GetArrivals(WorldVoice v, AFArrival[] buf)
        {
            if (_world == IntPtr.Zero || v == null || v.EmitterId < 0 || buf == null) return 0;
            try { return NativeWorld.AF_WorldArrivals(_world, v.EmitterId, buf, buf.Length); }
            catch (EntryPointNotFoundException) { return 0; }
        }
        public int ListenerRoom => (_world != IntPtr.Zero && listener != null) ? NativeWorld.AF_WorldRoomAt(_world, new AFVector3(listener.position)) : -1;
    }
}
