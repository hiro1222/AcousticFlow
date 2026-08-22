// BeyondAmbience.cs
// 扉の向こうで鳴っている、**次の世界の環境音**。
//
// ★これが §0 の芯そのもの。
//     閉じた扉 → かすかに漏れる音 → 奥を想像する → 開ける → 音が広がる → 答え合わせ
//   これまで「最優先なのに入っていない」と書き続けていた欠けを埋める。
//
// ★「かすかに漏れる」「開けると広がる」を**このスクリプトは一切作らない。**
//   音量も帯域も扉の角度で触らない。ここがやるのは
//   「扉の数 m 奥に音源を 1 本置いて、行き先の音を鳴らし続ける」だけ。
//
//   隙間が狭いときに低域だけが通り、開くほど高域まで通るのは、
//   §4.3 のフレネル帯域積分が経路として計算した結果。
//   ここで演出として真似ると**同じ問いに 2 つの答え**ができる（決めごと #1）し、
//   そもそも「物理がそのまま体験になる」というこの作品の主張が消える。
//
// ★部屋は要らない。
//   一度は扉の向こうに部屋を作ろうとして、絵が合わず撤去した。
//   調べた結果、**手置きポータルがあれば部屋が無くても §4.3 は経路に効く**と分かっている。
//   だから置くのは音源 1 本だけでよい。
//
// ★行き先を変えると漏れてくる音が変わる ── **扉が何処に繋がっているかが音で分かる。**
using UnityEngine;

namespace BellGame
{
    [DisallowMultipleComponent]
    public sealed class BeyondAmbience : MonoBehaviour
    {
        [Header("配線")]
        [Tooltip("この扉の行き先を追う。")]
        public WorldDoor worldDoor;

        [Header("音")]
        [Tooltip("扉の奥の音源の音量。**扉の角度では絶対に触らない**（§4.3 の仕事）。")]
        [Range(0f, 1f)] public float volume = 0.55f;
        [Tooltip("単発物（水滴・鳥）の間隔。")]
        [Range(0.2f, 30f)] public float minInterval = 2.5f;
        [Range(0.3f, 60f)] public float maxInterval = 9.0f;

        // ★聞く目的で信号を使い分ける（F5 で切り替え）。
        //
        //   ・**世界の声**（既定）… 「扉の奥に別の世界がある」と分かるか を聞く場所。
        //     行き先ごとに signature が違うので、繋ぎ先が音で分かる。
        //   ・**持続する広帯域**（テスト信号）… §4.3 が連続かを聞く場所。
        //     合成の環境音は帯域が偏っていて途切れもあるので、隙間が 5cm から 15cm に
        //     なったときの差が「変化」ではなく「別の音になった」に聞こえてしまう。
        //
        //   どちらか一方では判断できない。**目的が違うので両方要る。**
        [Header("聞き比べ")]
        [Tooltip("持続する広帯域に差し替える（§4.3 の連続性を聞く用）。F5 で切り替え。")]
        public bool useTestSignal;
        [Tooltip("そのテスト信号。空なら切り替えても何も起きない。")]
        public AudioClip testSignal;

        [Header("診断（読み取り専用）")]
        public WorldId showing = WorldId.None;
        public AmbientKind kind;
        public float secondsToNext;

        // ★世界ごとの聞こえ方は WorldVoice が唯一の答え（決めごと #1）。
        //   看板の音・残響・明るさをここに書かない。書くと WORLD_SETTING §3 と
        //   二重管理になり、扉ごしに聞いた印象と踏み越えた先が食い違う。
        public static AmbientKind SignatureOf(WorldId id) => WorldVoice.Of(id).signature;

        private AudioSource _src;
        private AudioClip _silence;
        private readonly AudioClip[] _cache = new AudioClip[8];
        private float _next;
        private System.Random _rng;
        private int _sr = 48000;

        private bool IsLoop => kind == AmbientKind.Wind || kind == AmbientKind.Stream;

        private void Awake()
        {
            _src = GetComponent<AudioSource>();
            _sr = AudioSettings.outputSampleRate > 0 ? AudioSettings.outputSampleRate : 48000;
            _rng = new System.Random(7717);

            // 無音のループでフィルタ経路を開けておく（鳴り止むと畳み込みの尾まで切れる）。
            _silence = AudioClip.Create("Silence", _sr / 10, 1, _sr, false);
            _silence.SetData(new float[_sr / 10], 0);
            if (_src != null)
            {
                _src.clip = _silence;
                _src.loop = true;
                _src.playOnAwake = true;
                _src.volume = 1f;
                _src.Play();
            }
        }

        private void Update()
        {
            if (_src == null) return;

            if (Input.GetKeyDown(KeyCode.F5))
            {
                useTestSignal = !useTestSignal;
                Debug.Log($"[BellGame] 扉の奥の信号 → "
                    + (useTestSignal ? "持続する広帯域（§4.3 の連続性を聞く）"
                                     : "世界の声（繋ぎ先が音で分かるかを聞く）"), this);
                _forceRetune = true;
            }

            var dest = (worldDoor != null) ? worldDoor.destination : WorldId.None;
            if (dest != showing || _forceRetune) { _forceRetune = false; Retune(dest); }

            // ★テスト信号のときは単発物を鳴らさない（持続音だけで聞く）。
            if (useTestSignal || showing == WorldId.None || IsLoop) return;

            secondsToNext = Mathf.Max(0f, _next - Time.time);
            if (Time.time < _next) return;
            _next = Time.time + Mathf.Lerp(minInterval, Mathf.Max(minInterval, maxInterval),
                                           (float)_rng.NextDouble());
            var clip = _cache[(int)showing];
            if (clip != null) _src.PlayOneShot(clip, volume);
        }

        private bool _forceRetune;

        private void Retune(WorldId dest)
        {
            showing = dest;

            // ★テスト信号は**繋がっているときだけ**。
            //   繋がっていない扉の奥に音源が無いのは①の約束なので、そこは曲げない。
            if (useTestSignal && dest != WorldId.None && testSignal != null)
            {
                kind = AmbientKind.Wind;
                _src.clip = testSignal;
                _src.loop = true;
                _src.volume = volume;
                _src.Play();
                Debug.Log($"[BellGame] 扉の奥にテスト信号（{testSignal.name}）", this);
                return;
            }

            if (dest == WorldId.None)
            {
                // 行き先が決まっていない扉の奥では何も鳴らない。
                //   ★これも演出ではなく設定の帰結 ── 繋がっていないのだから音源が無い。
                kind = AmbientKind.Wind;
                _src.clip = _silence;
                _src.loop = true;
                _src.volume = 1f;
                _src.Play();
                return;
            }

            kind = SignatureOf(dest);
            if (_cache[(int)dest] == null)
                _cache[(int)dest] = WorldVoice.MakeLeak(dest, _sr);

            if (IsLoop)
            {
                _src.clip = _cache[(int)dest];
                _src.loop = true;
                _src.volume = volume;
                _src.Play();
            }
            else
            {
                _src.clip = _silence;
                _src.loop = true;
                _src.volume = 1f;
                _src.Play();
                _next = Time.time + 0.5f;
            }

            Debug.Log($"[BellGame] 扉の奥から{BellVoices.DisplayName(dest)}の音が漏れてきた（{kind}）");
        }
    }
}
