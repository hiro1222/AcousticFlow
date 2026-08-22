// AmbientVoices.cs
// 環境音を手続きで合成する。資産を持たない。
//
// ★なぜ合成か
//   ・仕様書 §7.6 のとおり、デモの音源は市販曲で**配布物に入れられない**。
//     同じ轍を踏まないよう、環境音は最初から合成で持つ
//   ・環境音は**遮蔽と回折を確かめるための持続音**でもある。
//     風や川のような広帯域の定常音は、こもり・開口の通り方・残響の量が読みやすい
//     （ベルのような過渡音は定位と遅延には向くが音色差は分かりにくい）
//
// ★ゲーム側で音量も遮蔽もいじらない。
//   AudioSource から素の音を出すだけで、遮蔽・回折・残響はエンジンが決める。
//   「壁の向こうだから小さくする」を書くと二重計上になる（決めごと #4）。
//
// ★コストの注意
//   実機の実測で、フレームコストは音源数ではなく**遮蔽された音源数**で効くと分かっている
//   （神殿 3音源・遮蔽 0.00 → 3.30ms / 洞窟 3音源・遮蔽 0.92 → 14.41ms）。
//   崩壊都市のように建物だらけの世界では環境音が常に何かの陰に入るので、
//   **2〜3 本から始めて実測しながら増やすこと。**
using UnityEngine;

namespace BellGame
{
    public enum AmbientKind
    {
        Wind = 0,     // 風。低めの広帯域ノイズをゆっくり揺らす
        Stream = 1,   // 川。中域寄りの広帯域。定常
        Drip = 2,     // 水滴。単発を不規則な間隔で
        Bird = 3,     // 鳥。単発の囀りを不規則な間隔で
        Leaves = 4,   // 葉擦れ。中高域を突風で切れ切れに
        Falls = 5,    // 滝。**6帯域すべてに芯がある定常音**。扉ごしの音色を読むための音
    }

    /// 環境音を 1 つ鳴らす。ループ物（風・川）と単発物（水滴・鳥）を同じ器で扱う。
    [DisallowMultipleComponent]
    public sealed class AmbientSource : MonoBehaviour
    {
        [Header("何を鳴らすか")]
        public AmbientKind kind = AmbientKind.Wind;
        [Range(0f, 1f)] public float volume = 0.5f;

        [Header("単発物（水滴・鳥）")]
        [Tooltip("次に鳴るまでの最短秒数。")]
        [Range(0.2f, 30f)] public float minInterval = 2.0f;
        [Tooltip("次に鳴るまでの最長秒数。不規則にすると『環境』らしくなる。")]
        [Range(0.3f, 60f)] public float maxInterval = 7.0f;

        [Header("種")]
        [Tooltip("同じ種なら同じ音になる。世界ごとに変えると個性が出る。")]
        public int seed = 1;

        [Header("診断（読み取り専用）")]
        public float secondsToNext;

        private AudioSource _src;
        private AudioClip _clip;
        private AudioClip _silence;
        private float _next;
        private System.Random _rng;

        private bool IsLoop => kind == AmbientKind.Wind || kind == AmbientKind.Stream;

        private void Awake()
        {
            _src = GetComponent<AudioSource>();
            int sr = AudioSettings.outputSampleRate > 0 ? AudioSettings.outputSampleRate : 48000;
            _rng = new System.Random(seed);
            _clip = AmbientVoices.Make(kind, seed, sr);

            if (_src == null) return;
            if (IsLoop)
            {
                _src.clip = _clip;
                _src.loop = true;
                _src.playOnAwake = true;
                _src.volume = volume;
                _src.Play();
            }
            else
            {
                // 単発物は無音ループでフィルタ経路を開けておき、PlayOneShot で重ねる
                //（鳴り止むと OnAudioFilterRead が止まり、畳み込みの尾まで切れるため）。
                _silence = AudioClip.Create("Silence", sr / 10, 1, sr, false);
                _silence.SetData(new float[sr / 10], 0);
                _src.clip = _silence;
                _src.loop = true;
                _src.playOnAwake = true;
                _src.volume = 1f;
                _src.Play();
                _next = Time.time + NextGap();
            }
        }

        private float NextGap()
            => Mathf.Lerp(minInterval, Mathf.Max(minInterval, maxInterval), (float)_rng.NextDouble());

        private void Update()
        {
            if (IsLoop || _src == null || _clip == null) return;
            secondsToNext = Mathf.Max(0f, _next - Time.time);
            if (Time.time < _next) return;
            _next = Time.time + NextGap();
            _src.PlayOneShot(_clip, volume);
        }
    }

    public static class AmbientVoices
    {
        public static AudioClip Make(AmbientKind kind, int seed, int sr) => kind switch
        {
            AmbientKind.Wind => Wind(seed, sr),
            AmbientKind.Stream => Stream(seed, sr),
            AmbientKind.Drip => Drip(seed, sr),
            AmbientKind.Bird => Bird(seed, sr),
            AmbientKind.Leaves => Leaves(seed, sr),
            AmbientKind.Falls => Falls(seed, sr),
            _ => Wind(seed, sr),
        };

        /// 滝。**扉ごしの音色を読むための定常音。**
        ///
        /// ★なぜ足したか ── 既存の環境音は帯域が偏っていて、扉の検証に使えない。
        ///     Stream … 一次ハイパスで低域を捨てている（下の Stream 参照）。高域しか無い。
        ///               そこへ高域を落とすフィルタを掛けると**鈍るのではなく消える**。
        ///               「壁の奥でくぐもった同じ川」ではなく**別の音**に聞こえてしまう。
        ///     Wind  … 逆に 150〜400Hz 以下へ寄っている。こもらせても何も変わらないので、
        ///               扉を開けたことが音色で分からない。
        ///
        ///   扉の検証に要るのは**低・中・高すべてに芯がある定常音**。
        ///   低中が残るから閉じていても「同じ音」と分かり、高域が在るから開けたときに晴れる。
        ///   §4.3 が帯域ごとに違う通し方をしていることが、そのまま音色に出る。
        ///
        /// ★作り方は「違う速さの一次ローパスを重ねる」だけ（ピンク雑音の安い近似）。
        ///   低域は遅い段が、中域は速い段が、高域は素の雑音が受け持つ。
        public static AudioClip Falls(int seed, int sr)
        {
            int n = sr * 8;
            var d = new float[n];
            var r = new Rng(seed + 131);
            float lo = 0f, mid = 0f;
            for (int i = 0; i < n; i++)
            {
                float w = r.Bipolar();
                lo += (w - lo) * 0.010f;      // ≒76Hz まで
                mid += (w - mid) * 0.090f;    // ≒690Hz まで
                // ★揺らぎは浅く（±8%）。滝は鳴り止まないので、音量の波で
                //   「扉のせいか揺らぎのせいか」が分からなくなるのを避ける。
                float t = i / (float)sr;
                float env = 0.92f + 0.08f * Mathf.Sin(2f * Mathf.PI * 0.11f * t);
                d[i] = (lo * 9.0f + mid * 2.2f + w * 0.22f) * 0.30f * env;
            }
            SeamlessLoop(d, sr / 2);
            return Clip("Ambient_Falls", d, sr);
        }

        // ── 決定的な擬似乱数（xorshift32）──
        //   System.Random を使わないのは、同じ種でも実装によって値が変わりうるため。
        //   「昨日と同じ音か」を確かめられないと、音の変化を切り分けられない。
        private struct Rng
        {
            private uint _s;
            public Rng(int seed) { _s = (uint)seed * 2654435761u + 1013904223u; if (_s == 0) _s = 1u; }
            public float Bipolar()
            {
                _s ^= _s << 13; _s ^= _s >> 17; _s ^= _s << 5;
                return (int)_s * (1.0f / 2147483648.0f);   // -1..1
            }
            public float Unit() => (Bipolar() + 1f) * 0.5f;
        }

        // 継ぎ目を消す。末尾を先頭へクロスフェードして畳み込む。
        //   ループ物は繋ぎ目でプツッと鳴ると、それ自体が定位の手がかりになってしまう。
        private static void SeamlessLoop(float[] a, int fade)
        {
            int n = a.Length;
            fade = Mathf.Clamp(fade, 1, n / 3);
            for (int i = 0; i < fade; i++)
            {
                float t = i / (float)fade;
                a[i] = a[i] * t + a[n - fade + i] * (1f - t);
            }
            System.Array.Resize(ref a, n);   // 長さは変えない（末尾は使われる側として残す）
        }

        /// 風。低域寄りの広帯域を、ゆっくりした揺れと突風で動かす。
        public static AudioClip Wind(int seed, int sr)
        {
            int n = sr * 8;
            var d = new float[n];
            var r = new Rng(seed);
            float lp1 = 0f, lp2 = 0f;
            // 一次ローパス 2 段。係数が小さいほど低く落ちる。
            const float k1 = 0.020f, k2 = 0.055f;
            for (int i = 0; i < n; i++)
            {
                float w = r.Bipolar();
                lp1 += (w - lp1) * k1;
                lp2 += (lp1 - lp2) * k2;
                float t = i / (float)sr;
                // ゆっくりした呼吸（0.07Hz）と突風（0.23Hz）を重ねる
                float env = 0.55f
                          + 0.30f * Mathf.Sin(2f * Mathf.PI * 0.07f * t)
                          + 0.15f * Mathf.Sin(2f * Mathf.PI * 0.23f * t + 1.7f);
                d[i] = lp2 * 9.0f * Mathf.Clamp01(env);
            }
            SeamlessLoop(d, sr / 2);
            return Clip("Ambient_Wind", d, sr);
        }

        /// 川。中域寄りの広帯域。風より高く、揺れは小さく定常。
        public static AudioClip Stream(int seed, int sr)
        {
            int n = sr * 6;
            var d = new float[n];
            var r = new Rng(seed + 77);
            float lp = 0f, hp = 0f, prev = 0f;
            for (int i = 0; i < n; i++)
            {
                float w = r.Bipolar();
                lp += (w - lp) * 0.35f;              // 高域を少し落とす
                hp = 0.92f * (hp + lp - prev);       // 低域を切る（一次ハイパス）
                prev = lp;
                float t = i / (float)sr;
                float env = 0.85f + 0.15f * Mathf.Sin(2f * Mathf.PI * 0.9f * t);
                d[i] = hp * 0.55f * env;
            }
            SeamlessLoop(d, sr / 3);
            return Clip("Ambient_Stream", d, sr);
        }

        /// 葉擦れ。中高域の広帯域を、突風で切れ切れにする。
        ///
        /// ★手触りの方針「静かだが自然を感じる」を、いちばん安く担うのがこれ。
        ///   風（低域）だけだと世界が空っぽに聞こえる。葉擦れが乗ると**そこに何かが生えている**
        ///   ことが音だけで分かる。
        ///
        /// ⚠ ただしこの音は 1k〜4kHz に居座るので、**ベルの打点の当たりと帯域が正面衝突する**
        ///   （BellCallResponse.AddStrike。定位の手がかりはそこに乗っている）。
        ///   対策として定常には鳴らさず、**突風で切れ切れにして必ず谷を作る**。
        ///   呼びかけの返しはその谷で聞こえる。音量で殴って解決しないこと ──
        ///   そうすると探索の難易度が「音響の出来」ではなく「ミックス」で決まってしまう。
        public static AudioClip Leaves(int seed, int sr)
        {
            int n = sr * 7;
            var d = new float[n];
            var r = new Rng(seed + 509);
            float lp = 0f, hp = 0f, prev = 0f;
            for (int i = 0; i < n; i++)
            {
                float w = r.Bipolar();
                lp += (w - lp) * 0.55f;             // ざらつきを少しだけ丸める
                hp = 0.86f * (hp + lp - prev);      // 低域を切る（風と役割を分ける）
                prev = lp;

                float t = i / (float)sr;
                // ★3 本の遅い正弦を重ねて、周期の読めない突風にする。
                //   谷（env<=0）では完全に無音になるので、ベルの通り道が必ず空く。
                float gust = 0.55f * Mathf.Sin(2f * Mathf.PI * 0.11f * t)
                           + 0.30f * Mathf.Sin(2f * Mathf.PI * 0.29f * t + 2.1f)
                           + 0.22f * Mathf.Sin(2f * Mathf.PI * 0.53f * t + 0.6f);
                float env = Mathf.Clamp01(gust - 0.18f);
                d[i] = hp * 0.9f * env * env;       // 二乗して谷を深く、山を控えめに
            }
            SeamlessLoop(d, sr / 2);
            return Clip("Ambient_Leaves", d, sr);
        }

        /// 水滴。短い当たりのあと、下がりながら減衰する。
        public static AudioClip Drip(int seed, int sr)
        {
            int n = (int)(sr * 0.5f);
            var d = new float[n];
            var r = new Rng(seed + 131);
            float f0 = 900f + r.Unit() * 700f;     // 個体差
            float phase = 0f;
            for (int i = 0; i < n; i++)
            {
                float t = i / (float)sr;
                // ピッチが落ちる＝水面が広がる感じ。これが「ぽちゃん」の正体。
                float f = f0 * Mathf.Exp(-7.5f * t);
                phase += 2f * Mathf.PI * f / sr;
                float body = Mathf.Sin(phase) * Mathf.Exp(-16f * t);
                // 当たりの一瞬だけノイズを混ぜて輪郭を出す
                float click = r.Bipolar() * Mathf.Exp(-260f * t) * 0.25f;
                d[i] = (body + click) * 0.8f;
            }
            return Clip("Ambient_Drip", d, sr);
        }

        /// 鳥。急速に上下するピッチの囀りを 2〜3 音。
        public static AudioClip Bird(int seed, int sr)
        {
            int n = (int)(sr * 0.9f);
            var d = new float[n];
            var r = new Rng(seed + 313);
            int notes = 2 + (int)(r.Unit() * 2f);
            float at = 0f;
            for (int k = 0; k < notes; k++)
            {
                float dur = 0.07f + r.Unit() * 0.09f;
                float f0 = 2200f + r.Unit() * 1800f;
                float sweep = (r.Bipolar() > 0f) ? 1.9f : 0.55f;   // 上がるか下がるか
                float phase = 0f;
                int start = (int)(at * sr);
                int len = (int)(dur * sr);
                for (int i = 0; i < len && start + i < n; i++)
                {
                    float t = i / (float)sr;
                    float u = t / dur;
                    float f = f0 * Mathf.Pow(sweep, u) * (1f + 0.05f * Mathf.Sin(2f * Mathf.PI * 32f * t));
                    phase += 2f * Mathf.PI * f / sr;
                    // 立ち上がりと減衰を丸める（角があるとクリックになる）
                    float env = Mathf.Sin(Mathf.PI * Mathf.Clamp01(u));
                    d[start + i] += Mathf.Sin(phase) * env * 0.5f;
                }
                at += dur + 0.04f + r.Unit() * 0.10f;
            }
            return Clip("Ambient_Bird", d, sr);
        }

        // ★行き先の部屋の響きを、音そのものへ焼き込む。
        //
        //   扉の奥の音源は「いまいる世界」の音響シーンに立っている。生きている世界は
        //   1 つしか置けないので（AcousticFlowSceneDemo は 1 面まで）、
        //   **向こうの部屋の応答はエンジンからは出てこない**。
        //   洞窟へ通じる扉を開けても、聞こえるのは乾いた水滴で、5.1 秒の尾が付かない。
        //
        //   「向こうの世界の音が聞こえてくる」を物理で言えば、その音は
        //   向こうの部屋の応答を纏っているはず。だから音の側へ焼く。
        //   遠くから穴ごしに届く音なので、これは嘘ではない ──
        //   **手前の空間の響きはエンジンが別に付ける**ので、二重にもならない。
        //
        //   ⚠ ここで作るのは「向こうがどんな空間か」が伝わる程度の粗い尾。
        //     本物の部屋の応答が要るなら、それはエンジンに 2 面目を立てる話になる。
        public static void BakeRoomTail(float[] d, int sr, float rt60, float wet)
        {
            if (rt60 <= 0.05f || wet <= 0f) return;
            int n = d.Length;

            // 素数寄りの遅延を 4 本。整数倍が並ぶと特定の高さだけ鳴く。
            int[] delay = { (int)(0.0297f * sr), (int)(0.0371f * sr),
                            (int)(0.0411f * sr), (int)(0.0437f * sr) };
            var wetBuf = new float[n];

            for (int c = 0; c < delay.Length; c++)
            {
                int m = Mathf.Max(1, delay[c]);
                // 1 周ごとの減衰。RT60 の定義（-60dB までの時間）から素直に出す。
                float g = Mathf.Pow(10f, -3f * (m / (float)sr) / rt60);
                var buf = new float[m];
                int idx = 0;
                for (int i = 0; i < n; i++)
                {
                    float y = buf[idx];
                    buf[idx] = d[i] + y * g;
                    idx++; if (idx >= m) idx = 0;
                    wetBuf[i] += y;
                }
            }

            // 位相をばらす全域通過を 1 段。これが無いと櫛の目が残って金属的になる。
            {
                int m = Mathf.Max(1, (int)(0.0051f * sr));
                const float g = 0.6f;
                var buf = new float[m];
                int idx = 0;
                for (int i = 0; i < n; i++)
                {
                    float x = wetBuf[i] * 0.25f;
                    float y = buf[idx];
                    float o = -g * x + y;
                    buf[idx] = x + g * o;
                    idx++; if (idx >= m) idx = 0;
                    wetBuf[i] = o;
                }
            }

            for (int i = 0; i < n; i++) d[i] = d[i] * (1f - wet) + wetBuf[i] * wet;
        }

        private static AudioClip Clip(string name, float[] data, int sr)
        {
            // 念のため正規化（合成の係数を触ったときに歪ませないため）。
            float peak = 0f;
            for (int i = 0; i < data.Length; i++) peak = Mathf.Max(peak, Mathf.Abs(data[i]));
            if (peak > 1e-6f)
            {
                float g = 0.9f / peak;
                for (int i = 0; i < data.Length; i++) data[i] *= g;
            }
            var c = AudioClip.Create(name, data.Length, 1, sr, false);
            c.SetData(data, 0);
            return c;
        }
    }
}
