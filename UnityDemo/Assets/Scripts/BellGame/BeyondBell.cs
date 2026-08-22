// BeyondBell.cs
// 扉の向こうから聞こえる、行き先の世界のベル。
//
// ★これが扉に音響的な意味を与える。
//   扉の両側がどちらも屋外だと、`2e63c46` のとおりどちらも部屋にならず開口が立たない
//   ＝ 扉を開け閉めしても音がほとんど変わらない。
//   向こう側に**閉じた空間**を置き、そこで音が鳴って初めて
//     ・開口（＝扉の隙間）が部屋グラフに立つ
//     ・§4.3 のフレネル帯域積分が「どれだけ開いているか」を音色で伝える
//     ・向こうの部屋の残響が、隙間ぶんだけ漏れてくる
//   が全部つながる。作品の主張がいちばん素直に出る場面。
//
// ★鳴らす音は**行き先の世界の声**（BellVoices）。
//   扉の行き先を変えると、向こうから聞こえる声も変わる。
//   「どの世界に繋がっているか」を音で確かめられる、というのが狙い。
//
// ★ゲーム側は音量も遮蔽も一切いじらない。
//   AudioSource から素の音を出すだけで、隙間の量も残響の漏れもエンジンが決める。
//   ここで「扉が閉じているから小さくする」を書くと二重計上になる（決めごと #4）。
using UnityEngine;

namespace BellGame
{
    [DisallowMultipleComponent]
    public sealed class BeyondBell : MonoBehaviour
    {
        [Header("配線")]
        public WorldDoor worldDoor;
        public AudioSource source;

        [Header("鳴らし方")]
        [Tooltip("何秒おきに鳴らすか。扉を開け閉めしながら聴き比べるので、待たせすぎない。")]
        [Range(1f, 20f)] public float intervalSeconds = 5.0f;
        [Range(0f, 1f)] public float volume = 0.55f;
        [Tooltip("減衰の長さ(秒)。向こうの部屋の残響を聴かせたいので長めに。")]
        [Range(0.5f, 6f)] public float bellSeconds = 3.5f;

        [Header("診断（読み取り専用）")]
        public string ringingWorld = "—";
        public float secondsToNext;

        private readonly AudioClip[] _clips = new AudioClip[8];
        private AudioClip _silence;
        private float _next;
        private WorldId _current = WorldId.None;

        private void Awake()
        {
            int sr = AudioSettings.outputSampleRate > 0 ? AudioSettings.outputSampleRate : 48000;
            foreach (WorldId id in System.Enum.GetValues(typeof(WorldId)))
            {
                if (id == WorldId.None || BellVoices.Partial(id) <= 0f) continue;
                // 扉の向こうで鳴っているのも断片（倍音 1 本）。
                //   まだ行っていない世界の倍音を、扉ごしに先に聴くことになる。
                _clips[(int)id] = BellCallResponse.MakeFragmentClip(
                    "Beyond_" + id, id, bellSeconds, sr);
            }

            // 無音のループで OnAudioFilterRead を開け続ける（BellCallResponse と同じ理由）。
            if (source != null)
            {
                _silence = AudioClip.Create("Silence", sr / 10, 1, sr, false);
                _silence.SetData(new float[sr / 10], 0);
                source.clip = _silence;
                source.loop = true;
                source.playOnAwake = true;
                source.volume = 1f;
                source.Play();
            }
            _next = Time.time + intervalSeconds;
        }

        private void Update()
        {
            _current = worldDoor != null ? worldDoor.destination : WorldId.None;
            ringingWorld = BellVoices.DisplayName(_current);
            secondsToNext = Mathf.Max(0f, _next - Time.time);

            // 行き先が決まっていなければ鳴らない（扉は何処にも繋がっていない）。
            if (_current == WorldId.None) { _next = Time.time + intervalSeconds; return; }
            if (Time.time < _next) return;

            _next = Time.time + intervalSeconds;
            var clip = _clips[(int)_current];
            if (clip != null && source != null) source.PlayOneShot(clip, volume);
        }
    }
}
