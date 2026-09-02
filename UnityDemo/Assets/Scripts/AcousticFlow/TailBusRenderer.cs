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

        // 尾の形の index → バス。形が同じ音源は同じバスへ入る。
        private readonly Dictionary<int, System.IntPtr> _buses = new Dictionary<int, System.IntPtr>();
        private float[] _l, _r;
        private int _sampleRate = 48000;
        private float _tailSeconds = 1f;

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
        }

        private void OnDisable()
        {
            // ★オーディオスレッドが触っている最中に消さない。
            //   OnDisable の時点で Unity はこの GameObject のフィルタを止めているが、
            //   音源側はまだバスを掴んでいる可能性がある。掴んでいる側を先に外させる。
            foreach (var v in FindObjectsByType<VoiceConvolver>(FindObjectsSortMode.None))
                v.DetachTailBus();
            foreach (var kv in _buses)
                if (kv.Value != System.IntPtr.Zero)
                    Native.AF_TailBusDestroy(kv.Value);
            _buses.Clear();
        }

        // ★全音源のミックス後に呼ばれる。ここで各バスを 1 回ずつ畳んで足す。
        //   ⚠ 送りが 1 本も無いブロックでも呼ぶこと。畳み込み器の遅延線を進めないと、
        //     次に送りが来たときに尾が飛ぶ（無音を入れて進めるのが正しい）。
        private void OnAudioFilterRead(float[] data, int channels)
        {
            if (_buses.Count == 0 || channels < 1) return;
            int frames = data.Length / channels;
            if (_l == null || _l.Length < frames) { _l = new float[frames]; _r = new float[frames]; }

            foreach (var kv in _buses)
            {
                if (kv.Value == System.IntPtr.Zero) continue;
                System.Array.Clear(_l, 0, frames);
                System.Array.Clear(_r, 0, frames);
                Native.AF_TailBusRender(kv.Value, frames, _l, _r);
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
