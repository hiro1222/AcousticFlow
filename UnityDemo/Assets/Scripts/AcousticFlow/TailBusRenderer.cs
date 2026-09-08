// TailBusRenderer.cs — 後期残響の尾を**音源で共有して 1 回だけ畳む**（段階②）。
//
// ★何のためにあるか
//   これまで尾の畳み込みは音源 1 本につき 1 個だった。音源数に比例してオーディオスレッドの
//   費用が増えるのに、**同じ部屋の音源は同じ IR を使う**のでまるごと無駄だった。
//   実測: 音源 8 本で 1 ブロック 2.345 → 0.404 ms（5.81 倍）。出力の差 -123.7dB 下。
//
// ★近似ではない
//   畳み込みは線形なので、同じ IR に対して
//       conv(IR, g1·x1) + conv(IR, g2·x2) = conv(IR, g1·x1 + g2·x2)
//   が厳密に成り立つ。音源ごとのレベル g は足す前に掛けるので、
//   **音源ごとの尾の量は保たれる**。減るのは畳み込みの回数だけ。
//
// ★なぜ AudioListener に付けるのか（ここが要点）
//   音源の OnAudioFilterRead が呼ばれる順は保証されない。だから音源のどれかに
//   「全員が送り終わったら畳む」を書くことはできない。1 ブロック遅らせる手もあるが、
//   尾の立ち上がりが DSP バッファぶん（48kHz/1024 で 21ms）ずれる。
//   この作品は「部屋に入る瞬間」を主題にしているので、そこを鈍らせたくない。
//   → **AudioListener に付けたフィルタは全音源のミックス後に走る。**
//     音源は送るだけ、畳み込みはここで 1 回。**遅延は増えない。**
//
// ★部屋ごとに 1 本
//   IR が違う音源を同じバスへ入れると、片方の部屋の響きがもう片方に付く。
//   尾の形は既に部屋ごとに共有されている（段階①の TailShapeIndex）ので、
//   その index をそのままバスの鍵にする。
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
        private System.IntPtr _dirBus = System.IntPtr.Zero;
        public System.IntPtr DirectionBusHandle => _dirBus;
        public float DirectionBusRms => (_dirBus != System.IntPtr.Zero) ? Native.AF_DirectionBusRms(_dirBus) : 0f;

        public System.IntPtr GetOrCreateDirectionBus(int maxFrames)
        {
            if (!enableDirectionBus) return System.IntPtr.Zero;
            if (_dirBus != System.IntPtr.Zero) return _dirBus;
            try { _dirBus = Native.AF_DirectionBusCreate(_sampleRate, directionLanes, Mathf.Max(maxFrames, 2048)); }
            catch (System.EntryPointNotFoundException) { _dirBus = System.IntPtr.Zero; }
            return _dirBus;
        }

        // 尾の形の index → バス。形が同じ音源は同じバスへ入る。
        private readonly Dictionary<int, System.IntPtr> _buses = new Dictionary<int, System.IntPtr>();
        // 【扉の定点】バスごとの「直接に足す割合」＝リスナーのまわりでその部屋が占める割合 w。
        //   部屋の中では 1（従来どおり）、外では 0（尾は戸口の PortalEmitter が (1−w) で運ぶ）。
        //   主スレッドが書き、オーディオスレッドが読む。float の読み書きは原子的なので配列で持つ
        //   （辞書はオーディオスレッドから触らない）。鍵は尾の形の index（音源の index）。
        private readonly float[] _busWeight = new float[512];
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
            return mix;
        }
        /// 今の器を引退させる（音源を外し、1 秒後に壊す）。
        public void RetireFdnMix()
        {
            if (_fdn == System.IntPtr.Zero) return;
            foreach (var v in FindObjectsByType<VoiceConvolver>(FindObjectsSortMode.None)) v.DetachFdnMix();
            _fdnRetired.Add(new KeyValuePair<float, System.IntPtr>(Time.unscaledTime, _fdn));
            _fdn = System.IntPtr.Zero;
        }
        private void Update()
        {
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

        private void Awake()
        {
            _sampleRate = AudioSettings.outputSampleRate;
            for (int i = 0; i < _busWeight.Length; i++) _busWeight[i] = 1f;   // 既定は従来どおり全部足す
        }

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
        }

        // ★全音源のミックス後に呼ばれる。ここで各バスを 1 回ずつ畳んで足す。
        //   ⚠ 送りが 1 本も無いブロックでも呼ぶこと。畳み込み器の遅延線を進めないと、
        //     次に送りが来たときに尾が飛ぶ（無音を入れて進めるのが正しい）。
        private void OnAudioFilterRead(float[] data, int channels)
        {
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
