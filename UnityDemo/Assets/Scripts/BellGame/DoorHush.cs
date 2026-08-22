// DoorHush.cs
// 扉の周りだけ、**いまいる世界の音を薄くする**。
//
// ★これは物理ではない。演出。
//   この作品は「物理がそのまま体験になる」が主張なので、
//   演出を物理に混ぜると後から見分けが付かなくなる。だから別の部品に切ってある。
//   ここがやるのは**ホスト側の AudioSource.volume を下げること**だけで、
//   エンジンには何も渡さない。遮蔽も開口も嘘をつかない。
//
// ★なぜ要るのか（発注者の判断）
//   扉でやりたいのは「いまいる世界の回折」ではなく
//   **「扉の奥の世界の音が聞こえてくる」**こと。
//   手前の世界が同じ音量で鳴っていると、漏れてくる音がその中に埋もれる。
//   扉に寄るほど手前が薄くなれば、向こうが相対的に立ち上がる。
//
// ★設定とも噛み合う。
//   「扉は世界に属さない。世界と世界の間にあるものが、そこに立っている」。
//   その周りだけ世界の音が薄いのは、設定の側から見ても筋が通る。
//
// ★しきい値は置かない（決めごと #2）。
//   距離に対して滑らかに落とす。ある半径を跨いだ瞬間に音量が変わると、
//   それ自体が「境界がある」という別の情報になってしまう。
//
// ★薄くしないもの:
//   ・扉の奥の音（BeyondAmbience）── これを薄くしたら本末転倒
//   ・プレイヤーのベル ── 空間を測る道具なので、音量を触ると測量が狂う
using System.Collections.Generic;
using UnityEngine;

namespace BellGame
{
    [DisallowMultipleComponent]
    public sealed class DoorHush : MonoBehaviour
    {
        [Header("配線")]
        public Transform listener;
        [Tooltip("この扉のまわりで薄くする。空なら自分の位置。")]
        public Transform door;
        [Tooltip("薄くしない音源（プレイヤーのベルなど）。")]
        public List<AudioSource> keepLoud = new List<AudioSource>();

        [Header("効き")]
        [Tooltip("この距離から効きはじめる。")]
        [Range(1f, 30f)] public float radius = 7f;
        [Tooltip("扉の真下での音量倍率。1 で無効。")]
        [Range(0.05f, 1f)] public float minGain = 0.45f;
        [Tooltip("世界のベルも薄くするか。探索中は残したいことが多い。")]
        public bool duckWorldBell;

        [Header("診断（読み取り専用）")]
        public float distance;
        public float gain = 1f;

        // 元の音量を覚えておく。毎フレーム掛け直すので、
        // 覚えないと下げた値にさらに掛かって落ち続ける。
        private readonly List<AudioSource> _targets = new List<AudioSource>();
        private readonly List<float> _base = new List<float>();

        private void Start()
        {
            if (door == null) door = transform;
            if (listener == null)
            {
                var demo = FindFirstObjectByType<AcousticFlow.AcousticFlowSceneDemo>();
                if (demo != null) listener = demo.listener;
            }

            var bell = FindFirstObjectByType<BellCallResponse>();
            if (bell != null && bell.playerBell != null && !keepLoud.Contains(bell.playerBell))
                keepLoud.Add(bell.playerBell);          // 測る道具は触らない

            foreach (var a in FindObjectsByType<AudioSource>(FindObjectsSortMode.None))
            {
                if (a == null) continue;
                if (keepLoud.Contains(a)) continue;
                if (a.GetComponent<BeyondAmbience>() != null) continue;   // 奥の音は薄くしない
                if (!duckWorldBell && bell != null && a == bell.worldBell) continue;

                // 環境音と世界のベルだけを対象にする（音源として登録されているもの）。
                if (a.GetComponent<AcousticFlow.IrConvolver>() == null) continue;

                _targets.Add(a);
                _base.Add(a.volume);
            }
        }

        private void Update()
        {
            if (listener == null || door == null || _targets.Count == 0) return;

            distance = Vector3.Distance(listener.position, door.position);

            // 0（扉の真下）〜1（radius の外）へ滑らかに。smoothstep なので
            // 端で傾きが 0 になり、効きはじめ／効き終わりに角が立たない。
            float t = Mathf.Clamp01(distance / Mathf.Max(0.01f, radius));
            float s = t * t * (3f - 2f * t);
            gain = Mathf.Lerp(minGain, 1f, s);

            for (int i = 0; i < _targets.Count; i++)
                if (_targets[i] != null) _targets[i].volume = _base[i] * gain;
        }

        private void OnDisable()
        {
            // 眠るときは戻す。下げたまま止まると、起きたときに薄いままになる。
            for (int i = 0; i < _targets.Count; i++)
                if (_targets[i] != null) _targets[i].volume = _base[i];
            gain = 1f;
        }
    }
}
