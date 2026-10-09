// TailBusRenderer.cs ── 耳の側で 1 回だけ回す DSP（方向バス・部屋ごとの FDN・旧の尾のバス）を持つ器。
//
// ■ 全体の中の位置
//   音源（WorldVoice）→ 直接音と近いタップは音源ごとに畳む／反射と響きは**送るだけ**
//     → **ここ**（AudioListener の上）で全部まとめて 1 回畳む → 出力。
//   AcousticWorld が器の作り直し（部屋グラフが変わったとき）と寿命を見ている。
//   新コアで生きているのは 方向バス と FDN の 2 つ。尾のバス（畳み込みの尾）は旧コアの道で、
//   新コアでは AcousticWorld が enableSharedTail=false にしているので作られない。
//
// ■ 役割
//   「音源の本数に依らない費用」でまとめるための場所。反射のタップも部屋の響きも、
//   最後は方向（レーン）と部屋にまとまるので、音源が何本でも畳む回数は変わらない。
//
// ■ 中の仕組み
//   1) 方向バス（AF_DirectionBus）: 反射・回折のタップを、リスナー座標に固定した 8 レーン（＋上下 2）へ振り、
//      レーンごとに固定の HRIR で畳む。費用が O(タップ) → O(レーン) になる。
//      ★バスにも HRTF を差すこと。差さないと「左の行は左耳、右の行は右耳」と流すだけで、
//        レーンに左右の音量差と方向の音色が付かない（耳の時間差だけになる）。
//   2) FDN（AF_FdnMix）: 部屋ごとの残響器を 1 つの器に持ち、ここで 1 回回す。作り直しは新しい器を作って
//      差し替え、古い器は**1 秒おいてから**壊す（オーディオスレッドが古い器の中にいるのは高々 1 ブロック）。
//   3) 尾のバス（旧）: 同じ部屋の音源は同じ IR なので、レベルを掛けて足してから 1 回畳む。
//      畳み込みは線形なので conv(IR, g1x1)+conv(IR, g2x2) = conv(IR, g1x1+g2x2) が**厳密**に成り立つ
//      （近似ではない。回帰で −123.7 dB 下まで一致）。音源 8 本で 1 ブロック 2.345 → 0.404 ms。
//   ★なぜ AudioListener に付けるのか: 音源の OnAudioFilterRead が呼ばれる順は保証されない。
//     どれかの音源に「全員が送り終わったら畳む」とは書けない。1 ブロック遅らせる手もあるが、尾の立ち上がりが
//     DSP バッファぶん（48 kHz/1024 で 21 ms）ずれる。AudioListener のフィルタは全音源のミックス後に走るので、
//     音源は送るだけ・畳むのはここで 1 回、遅延は増えない。
//
// ■ 繋がり
//   受ける: AcousticWorld（器の作り直し・HRTF の名前・レーンの本数）、engine（送りは DLL の中で溜まる）。
//   渡す:   Unity の出力バッファ（OnAudioFilterRead で足し込む）。
//
// ■ 退けた書き方
//   ・音源ごとに尾を畳む（旧）: 音源数に比例して費用が増えるのに、同じ部屋なら同じ IR で丸ごと無駄。
//   ・器を壊してすぐ作り直す: オーディオスレッドが掴んでいる器を壊すと落ちる。1 秒の墓地に置く。
//
// ■ 壊れる所
//   ・送りが 1 本も無いブロックで Render を呼ばないと、遅延線が進まず次の送りで尾が飛ぶ。無音でも回すこと。
//   ・HRTF を差す順番: クロスオーバー → HRTF → FDN の接続。FDN はバスを差した時点の HRTF でレーンの量を揃える。
//   ・器を壊す順番: 音源を外す → バスと FDN → 最後に HRTF（戸口の線音源が HRTF を借りている）。
using System.Collections.Generic;
using UnityEngine;

namespace AcousticFlow
{
    [AddComponentMenu("AcousticFlow/Tail Bus Renderer")]
    [RequireComponent(typeof(AudioListener))]
    public class TailBusRenderer : MonoBehaviour
    {
        [Tooltip("尾の共有バスを使う。OFF なら音源ごとに畳む（従来どおり）。\n\n"
                 + "★音は変わりません。畳み込みが線形なので、同じ IR なら"
                 + "「レベルを掛けてから足して 1 回畳む」で厳密に等しくなります"
                 + "（回帰テストで -123.7dB 下まで確認）。\n\n"
                 + "実測: 音源 8 本で 1 ブロック 2.345 → 0.404 ms（5.81 倍）。\n"
                 + "音源数が多いほど効きます。1〜2 本なら差はありません。")]
        public bool enableSharedTail = true;

        [Tooltip("尾 IR を差し替えるときのクロスフェード(ms)。0 で即差し替え。\n"
                 + "⚠ 0 にすると、遅延線に溜まった過去の入力が新しい IR で畳み直されて"
                 + "ふくらみます（音源側で +1.97dB が 0.35 秒続くのを実測済み）。")]
        [Range(0f, 200f)] public float crossfadeMs = 60f;

        [Tooltip("方向バス: 反射・回折のタップを 1 本ずつ両耳化せず、リスナー座標で固定した N 本のレーンへ振って"
                 + "レーンごとに固定の HRIR で畳む。費用が O(タップ) → O(レーン) になり、音源数に依らない。\n"
                 + "OFF で従来（タップごとの軽量両耳化）。音は変わるので A/B 用に残す。")]
        public bool enableDirectionBus = true;
        [Tooltip("レーンの本数（水平の環）。8 = 45° 刻みが出発点、12 = 30° が反射の上限。16 以上は聞き分けられない所に払う。")]
        [Range(4, 16)] public int directionLanes = 8;
        [Tooltip("方向バスに真上・真下のレーンを足す（2026-09-19）。部屋の響きを天井・床の近さでも配る。\n"
                 + "上下に聞こえるのは実測の HRTF（hrtfFile）を入れたときだけ（合成の HRTF には上下の手がかりが無い）。器を作るときに決まる")]
        public bool verticalLanes = true;
        // レーンのかぶり（2026-09-25、発注者の指示で既定を「かぶりなし」に）。
        //   入（今まで）: 1 本の反射を隣り合う 2 レーンへ等パワーで分ける。境目をまたいでも跳ばない代わりに、
        //     どの反射も常に 2 方向から鳴るので 45° ぶんにじむ。
        //   切（既定）: いちばん近い 1 レーンだけに入れる。8 本なら 1 レーンが ±22.5° を担当し、方向がはっきり分かれる。
        //     扇の境目をまたぐ瞬間に乗り換えるが、タップの組み直しに 30 ms のクロスフェードが掛かるので段では出ない。
        [Tooltip("レーンのかぶり（既定 切 ＝ かぶりなし）。入で今までどおり隣り合う 2 レーンへ等パワーに分ける。\n"
                 + "切ると 1 レーンの扇（8 本なら ±22.5°）の中だけで鳴り、方向がはっきり分かれる。実行中に切り替えられる")]
        public bool lanePanSplit = false;
        private bool _appliedPanSplit = true;   // 器の今の状態（作った直後はエンジン既定の「入」）
        private System.IntPtr _dirBus = System.IntPtr.Zero;
        private System.IntPtr _busHrtf = System.IntPtr.Zero;     // 方向バスの HRTF（バスと FDN の戸口の線音源が借りる）
        /// 方向バスの HRTF の .afhr 名（StreamingAssets 内）。空なら合成。AcousticWorld がバスを作る前に入れる。
        [System.NonSerialized] public string hrtfFile = "";
        /// 方向バスの低域と高域の境（Hz）。0 で旧（全帯域を畳む）。AcousticWorld がバスを作る前に入れる。
        [System.NonSerialized] public float busCrossoverHz = 700f;
        public string BusHrtfName { get; private set; } = "（無し）";
        public bool BusHasHrtf => _dirBus != System.IntPtr.Zero && Native.AF_DirectionBusHasHrtf(_dirBus) != 0;
        public System.IntPtr DirectionBusHandle => _dirBus;
        public float DirectionBusRms => (_dirBus != System.IntPtr.Zero) ? Native.AF_DirectionBusRms(_dirBus) : 0f;

        /// 方向バスを作る（既にあればそれ）。レーン数と上下の有無は器を作るときに決まる。
        ///   古い DLL には CreateVertical が無いので、見つからなければ水平だけの Create に落ちる。
        ///   順番が大事: クロスオーバー → HRTF（実測 .afhr があればそれ、無ければ合成）→ FDN の接続。
        public System.IntPtr GetOrCreateDirectionBus(int maxFrames)
        {
            if (!enableDirectionBus) return System.IntPtr.Zero;
            if (_dirBus != System.IntPtr.Zero) return _dirBus;
            try { _dirBus = Native.AF_DirectionBusCreateVertical(_sampleRate, directionLanes, Mathf.Max(maxFrames, 2048), verticalLanes ? 2 : 0); }
            catch (System.EntryPointNotFoundException)
            {
                try { _dirBus = Native.AF_DirectionBusCreate(_sampleRate, directionLanes, Mathf.Max(maxFrames, 2048)); }
                catch (System.EntryPointNotFoundException) { _dirBus = System.IntPtr.Zero; }
            }
            if (_dirBus != System.IntPtr.Zero) { try { Native.AF_DirectionBusSetCrossover(_dirBus, busCrossoverHz); } catch (System.EntryPointNotFoundException) { } }   // HRTF より先
            // ★方向バスにも HRTF を差す（2026-09-12）。差さないとバスは「左の行を左耳へ、右の行を右耳へ」流すだけで、
            //   反射と尾のレーンに左右の音量差と方向の音色が付かない（耳の時間差だけ）。検査と試聴の道具は差していたので、
            //   実機だけが違っていた。戸口の線音源（段 2-g）も同じ HRTF を借りる。
            //   FDN を繋ぐより前に差すこと（FDN はバスを差した時点の HRTF でレーンの量を揃える）。
            if (_dirBus != System.IntPtr.Zero && _busHrtf == System.IntPtr.Zero)
            {
                try
                {
                    // 実測の .afhr が指定されていて読めればそれ、なければ合成。器はこのコンポーネントが持つ（バス・FDN と同じ寿命）。
                    if (!string.IsNullOrEmpty(hrtfFile))
                    {
                        string path = System.IO.Path.Combine(Application.streamingAssetsPath, hrtfFile);
                        if (System.IO.File.Exists(path)) _busHrtf = Native.AF_HrtfLoadFile(path);
                    }
                    BusHrtfName = (_busHrtf != System.IntPtr.Zero) ? hrtfFile : "合成（球の頭）";
                    if (_busHrtf == System.IntPtr.Zero) _busHrtf = Native.AF_HrtfCreateSynthetic(_sampleRate);
                    if (_busHrtf != System.IntPtr.Zero) Native.AF_DirectionBusSetHrtf(_dirBus, _busHrtf, 57f);
                }
                catch (System.EntryPointNotFoundException) { _busHrtf = System.IntPtr.Zero; BusHrtfName = "（無し）"; }
            }
            ApplyPanSplit();
            AttachFdnToDirectionBus();
            return _dirBus;
        }

        // 尾の形の index → バス。形が同じ音源は同じバスへ入る。
        private readonly Dictionary<int, System.IntPtr> _buses = new Dictionary<int, System.IntPtr>();
        // 【扉の定点】バスごとの「直接に足す割合」＝リスナーのまわりでその部屋が占める割合 w。
        //   部屋の中では 1（従来どおり）、外では 0（尾は戸口の PortalEmitter が (1−w) で運ぶ）。
        //   主スレッドが書き、オーディオスレッドが読む。float の読み書きは原子的なので配列で持つ
        //   （辞書はオーディオスレッドから触らない）。鍵は尾の形の index（音源の index）。
        private readonly float[] _busWeight = new float[512];
        /// 【扉の定点・旧】そのバスを直接に足す割合。主スレッドが書き、オーディオスレッドが読む。
        ///   float の読み書きは原子的なので配列で持つ（辞書はオーディオスレッドから触らない）。
        public void SetBusWeight(int shapeIndex, float w)
        {
            if ((uint)shapeIndex < (uint)_busWeight.Length) _busWeight[shapeIndex] = Mathf.Clamp01(w);
        }
        /// その形のバスのハンドル（無ければ Zero）。**主スレッドからだけ呼ぶこと**（辞書を引く）。
        public System.IntPtr BusHandle(int shapeIndex)
        {
            return _buses.TryGetValue(shapeIndex, out var b) ? b : System.IntPtr.Zero;
        }
        private float[] _l, _r;
        private int _sampleRate = 48000;
        /// 鳴らす器が Wwise のとき、器（方向バス・FDN・HRTF）を Wwise の標本化周波数で作る。器を作る前に呼ぶこと。
        public void SetSampleRate(int sampleRate) { if (sampleRate > 0) _sampleRate = sampleRate; }
        /// 切ると、この AudioListener では何も鳴らさない（Wwise のプラグインが FDN と方向バスを回す）。
        ///   ★両方で回すと 1 ブロックに 2 回回って 2 倍鳴る。
        [System.NonSerialized] public bool renderInUnity = true;
        private float _tailSeconds = 1f;

        // ── 尾の FDN（tailModel=1、docs/TAIL_FDN_PLAN.md 手順 4）──
        //   部屋ごとの FDN を 1 つの器（AF_FdnMix）に持ち、AudioListener で 1 回回す（尾のバスと同じ場所）。
        //   作り直し（部屋グラフが変わった）は新しい器を作って差し替え、古い器は 1 秒おいてから壊す
        //   （オーディオスレッドが古い器の中にいるのは高々 1 ブロック。1 秒止まっていたら音はもう壊れている）。
        //   部屋を足すのは呼び手（AcousticFlowSceneDemo.UpdateFdnTail: 部屋番号 ＝ 部屋グラフの部屋番号）。
        private System.IntPtr _fdn = System.IntPtr.Zero;
        private readonly List<KeyValuePair<float, System.IntPtr>> _fdnRetired = new List<KeyValuePair<float, System.IntPtr>>();
        public System.IntPtr FdnMixHandle => _fdn;
        public float FdnRms => (_fdn != System.IntPtr.Zero) ? Native.AF_FdnMixRms(_fdn) : 0f;
        public int FdnRoomCount => (_fdn != System.IntPtr.Zero) ? Native.AF_FdnMixRoomCount(_fdn) : 0;

        /// 新しい器を作って差し替える（部屋を足すのは呼び手）。**メインスレッドからだけ。**
        public System.IntPtr ReplaceFdnMix(int maxFrames)
        {
            System.IntPtr mix;
            try { mix = Native.AF_FdnMixCreate(_sampleRate, Mathf.Max(maxFrames, 2048), 0.6f); }
            catch (System.EntryPointNotFoundException) { return System.IntPtr.Zero; }
            RetireFdnMix();
            _fdn = mix;   // 参照の書き換えは原子的。オーディオスレッドは古い器か新しい器のどちらかを見る
            AttachFdnToDirectionBus();
            return mix;
        }
        /// 【手順 6】尾の FDN を方向バスへ（レーンごとに独立した尾。向きは部屋ごと）。両方が揃ったときに差す。
        private void AttachFdnToDirectionBus()
        {
            if (_fdn == System.IntPtr.Zero || _dirBus == System.IntPtr.Zero) return;
            try { Native.AF_FdnMixSetDirectionBus(_fdn, _dirBus, 57f); }
            catch (System.EntryPointNotFoundException) { }
        }
        /// 今の器を引退させる（音源を外し、1 秒後に壊す）。
        public void RetireFdnMix()
        {
            if (_fdn == System.IntPtr.Zero) return;
            foreach (var v in FindObjectsByType<VoiceConvolver>(FindObjectsSortMode.None)) v.DetachFdnMix();
            _fdnRetired.Add(new KeyValuePair<float, System.IntPtr>(Time.unscaledTime, _fdn));
            _fdn = System.IntPtr.Zero;
        }
        /// レーンのかぶりの設定を器へ（変わったときだけ）。次に組み直すタップから効く。
        private void ApplyPanSplit()
        {
            if (_dirBus == System.IntPtr.Zero || _appliedPanSplit == lanePanSplit) return;
            try { Native.AF_DirectionBusSetPanSplit(_dirBus, lanePanSplit ? 1 : 0); _appliedPanSplit = lanePanSplit; }
            catch (System.EntryPointNotFoundException) { }   // 古い DLL では今までどおり（2 レーンへ分ける）
        }

        /// 引退した FDN の器を 1 秒後に壊す（墓地の掃除）と、かぶりの設定の取り込み。
        private void Update()
        {
            ApplyPanSplit();
            for (int i = _fdnRetired.Count - 1; i >= 0; i--)
            {
                if (Time.unscaledTime - _fdnRetired[i].Key < 1f) continue;
                Native.AF_FdnMixDestroy(_fdnRetired[i].Value);
                _fdnRetired.RemoveAt(i);
            }
        }

        // 音源側から引く。無ければ作る。**メインスレッドからだけ呼ぶこと。**
        public System.IntPtr GetOrCreateBus(int shapeIndex, float tailSeconds, int maxFrames)
        {
            if (!enableSharedTail) return System.IntPtr.Zero;
            if (_buses.TryGetValue(shapeIndex, out var got) && got != System.IntPtr.Zero) return got;
            _tailSeconds = tailSeconds;
            var bus = Native.AF_TailBusCreate(
                _sampleRate, tailSeconds, 64, 8192, Mathf.Max(maxFrames, 2048));
            if (bus == System.IntPtr.Zero) return System.IntPtr.Zero;
            Native.AF_TailBusSetCrossfadeMs(bus, crossfadeMs);
            _buses[shapeIndex] = bus;
            return bus;
        }

        /// サンプルレートを取り、バスの重みを 1（全部足す）で埋める。
        private void Awake()
        {
            _sampleRate = AudioSettings.outputSampleRate;
            for (int i = 0; i < _busWeight.Length; i++) _busWeight[i] = 1f;   // 既定は従来どおり全部足す
        }

        /// 器を全部壊す。★掴んでいる側（音源・戸口の定点）を先に外してから壊す。
        ///   OnDisable の時点で自分のフィルタは止まっているが、音源側はまだバスを掴んでいる可能性がある。
        private void OnDisable()
        {
            // ★オーディオスレッドが触っている最中に消さない。
            //   OnDisable の時点で Unity はこの GameObject のフィルタを止めているが、
            //   音源側はまだバスを掴んでいる可能性がある。掴んでいる側を先に外させる。
            foreach (var v in FindObjectsByType<VoiceConvolver>(FindObjectsSortMode.None))
            {
                v.DetachTailBus();
                v.DetachDirectionBus();
            }
            // 扉の定点もバスのモノラルを読んでいるので、先に離す。
            foreach (var e in FindObjectsByType<PortalEmitter>(FindObjectsSortMode.None))
                e.Detach();
            foreach (var kv in _buses)
                if (kv.Value != System.IntPtr.Zero)
                    Native.AF_TailBusDestroy(kv.Value);
            _buses.Clear();
            if (_dirBus != System.IntPtr.Zero) { Native.AF_DirectionBusDestroy(_dirBus); _dirBus = System.IntPtr.Zero; }
            // 尾の FDN。音源は上で外した。引退中の器も一緒に壊す。
            if (_fdn != System.IntPtr.Zero) { Native.AF_FdnMixDestroy(_fdn); _fdn = System.IntPtr.Zero; }
            foreach (var kv in _fdnRetired) Native.AF_FdnMixDestroy(kv.Value);
            _fdnRetired.Clear();
            // バスの HRTF は、バスと FDN（戸口の線音源が借りている）を壊した後で。
            if (_busHrtf != System.IntPtr.Zero) { Native.AF_HrtfDestroy(_busHrtf); _busHrtf = System.IntPtr.Zero; }
        }

        // ★全音源のミックス後に呼ばれる。ここで各バスを 1 回ずつ畳んで足す。
        //   ⚠ 送りが 1 本も無いブロックでも呼ぶこと。畳み込み器の遅延線を進めないと、
        //     次に送りが来たときに尾が飛ぶ（無音を入れて進めるのが正しい）。
        /// 全音源のミックス後に呼ばれる。尾のバス（旧）→ FDN → 方向バス の順に 1 回ずつ畳んで足す。
        ///   足すだけで data は消さない（ここに来る前の直接音を残す）。2ch 未満なら L/R の平均を入れる。
        ///   ⚠ 送りが無いブロックでも Render を呼ぶこと（遅延線を進めないと次の送りで尾が飛ぶ）。
        private void OnAudioFilterRead(float[] data, int channels)
        {
            if (!renderInUnity) return;   // Wwise が鳴らしている
            if ((_buses.Count == 0 && _dirBus == System.IntPtr.Zero && _fdn == System.IntPtr.Zero) || channels < 1) return;
            int frames = data.Length / channels;
            if (_l == null || _l.Length < frames) { _l = new float[frames]; _r = new float[frames]; }

            foreach (var kv in _buses)
            {
                if (kv.Value == System.IntPtr.Zero) continue;
                System.Array.Clear(_l, 0, frames);
                System.Array.Clear(_r, 0, frames);
                Native.AF_TailBusRender(kv.Value, frames, _l, _r);
                // 直接に足す割合 w（既定 1）。バスの中に残るモノラルは薄めない（定点が別に読む）。
                float w = ((uint)kv.Key < (uint)_busWeight.Length) ? _busWeight[kv.Key] : 1f;
                if (w <= 0f) continue;
                if (channels >= 2)
                {
                    for (int i = 0; i < frames; i++)
                    {
                        data[i * channels] += _l[i] * w;
                        data[i * channels + 1] += _r[i] * w;
                    }
                }
                else
                {
                    for (int i = 0; i < frames; i++) data[i * channels] += 0.5f * (_l[i] + _r[i]) * w;
                }
            }

            // 尾の FDN（tailModel=1）。器 1 つで全部屋を回す。送りが無いブロックでも回す（遅延線を進める）。
            var fdn = _fdn;
            if (fdn != System.IntPtr.Zero)
            {
                System.Array.Clear(_l, 0, frames);
                System.Array.Clear(_r, 0, frames);
                Native.AF_FdnMixRender(fdn, frames, _l, _r);
                if (channels >= 2)
                {
                    for (int i = 0; i < frames; i++)
                    {
                        data[i * channels] += _l[i];
                        data[i * channels + 1] += _r[i];
                    }
                }
                else
                {
                    for (int i = 0; i < frames; i++) data[i * channels] += 0.5f * (_l[i] + _r[i]);
                }
            }

            // 方向バス（全音源の反射・回折タップ）。尾のバスの後に 1 回。
            if (_dirBus != System.IntPtr.Zero)
            {
                System.Array.Clear(_l, 0, frames);
                System.Array.Clear(_r, 0, frames);
                Native.AF_DirectionBusRender(_dirBus, frames, _l, _r);
                if (channels >= 2)
                {
                    for (int i = 0; i < frames; i++)
                    {
                        data[i * channels] += _l[i];
                        data[i * channels + 1] += _r[i];
                    }
                }
                else
                {
                    for (int i = 0; i < frames; i++) data[i * channels] += 0.5f * (_l[i] + _r[i]);
                }
            }
        }
    }
}
