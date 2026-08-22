// LoadTestSilence.cs
// 負荷検証のあいだ、**音は消すが費用は残す。**
//
// ★なぜ「無効にする」ではなく「音量 0」なのか
//   測りたいのは音源が増えたときの費用です。無効にすると
//     ・エンジンが遮蔽・回折・経路を解かなくなる（**主な費用がここ**）
//     ・畳み込み器が止まる（DSP の費用も消える）
//   ので、測りたい物そのものが消えます。音量 0 なら計算は全部走ったまま、
//   耳にだけ届かない。**費用は本物のまま、音だけ消える。**
//
// ★なぜ毎フレーム押さえるのか
//   BeyondAmbience も AmbientSource も、行き先が変わるたびに
//   `_src.volume = volume` を書き戻します。一度 0 にしても戻される。
//   持ち主が複数いるので、**押さえ続ける役**を 1 つ置くのが確実です。
//
// ★F8 で戻せます。費用を測り終わったら音を戻して聴けます。
using UnityEngine;

namespace BellGame
{
    [DefaultExecutionOrder(200)]      // 音量を書く連中より後に走る
    [DisallowMultipleComponent]
    public sealed class LoadTestSilence : MonoBehaviour
    {
        [Tooltip("ON のあいだ、全ての AudioSource の音量を 0 に押さえる。")]
        public bool silent = true;
        public KeyCode toggleKey = KeyCode.F8;

        [Header("診断（読み取り専用）")]
        public int sourcesHeld;

        /// 実質無音（-80dB）。ちょうど 0 は避ける。
        private const float Floor = 0.0001f;

        private AudioSource[] _all;
        private float _next;

        private void LateUpdate()
        {
            if (Input.GetKeyDown(toggleKey))
            {
                silent = !silent;
                _all = null;                       // 戻すときは拾い直す
                Debug.Log($"[BellGame] 負荷検証の消音 = {(silent ? "ON（費用はそのまま）" : "OFF")}", this);
            }
            if (!silent) return;

            // 世界の入れ替えで音源が増減するので、ときどき拾い直す。
            //   毎フレーム FindObjects を叩くと、それ自体が測定を汚す。
            if (_all == null || Time.unscaledTime >= _next)
            {
                _next = Time.unscaledTime + 1f;
                _all = FindObjectsByType<AudioSource>(FindObjectsInactive.Include,
                                                      FindObjectsSortMode.None);
                sourcesHeld = _all.Length;
            }

            // ★0 ではなく 0.0001（-80dB）にする。
            //
            //   狙いは「音は消えるが費用はそのまま」です。ところが**ちょうど 0 にすると
            //   一気に重くなる**という報告が出ました。0 と 0.0001 は耳には同じなので、
            //   もし 0 だけ重いなら、原因は「小さいこと」ではなく「0 であること」です。
            //   ここは費用を保つのが目的なので、**0 を避けます。**
            //   （0 が重い理由そのものは別途エンジン側へ確認中）
            for (int i = 0; i < _all.Length; i++)
                if (_all[i] != null && _all[i].volume != Floor) _all[i].volume = Floor;
        }
    }
}
