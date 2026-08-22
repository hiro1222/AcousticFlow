// LoadTestRig.cs
// 「重い」を**その場で二分する**ための操作台。
//
// ★なぜ要るか
//   本番 0⇔1 で音響が 172.88ms／山 377.9ms になった。直前に足した物が 2 つある:
//     ・当たり判定（EquipColliders が箱を足した ＝ **そのまま音の遮蔽物**）
//     ・負荷検証の音源 10 本
//   どちらが効いているかは、合計を見ても分からない。片方ずつ外して読む。
//
// ★数を「0 か 10 か」ではなく 1 本ずつ動かせるようにしてある。
//   費用が本数に比例するのか、途中で跳ねるのかで、直し方がまるで変わるため
//  （比例なら 1 本あたりを削る話、跳ねるなら上限や作り直しの話）。
using System.Collections.Generic;
using UnityEngine;
using AcousticFlow;

namespace BellGame
{
    /// EquipColliders が後から足した当たり判定。切り分けのために名札を付けてある。
    public sealed class AddedCollider : MonoBehaviour { }

    [DefaultExecutionOrder(150)]
    [DisallowMultipleComponent]
    public sealed class LoadTestRig : MonoBehaviour
    {
        [Header("配線（空なら自分で探す）")]
        public AcousticFlowSceneDemo demo;

        [Header("操作")]
        [Tooltip("生かす音源の本数を減らす／増やす。")]
        public KeyCode fewerKey = KeyCode.Minus;
        public KeyCode moreKey = KeyCode.Equals;
        [Tooltip("後から足した当たり判定をまとめて入／切。")]
        public KeyCode collidersKey = KeyCode.K;

        [Header("診断（読み取り専用）")]
        public int liveLoadSources;
        public bool addedCollidersOn = true;
        public float acousticMs;

        private readonly List<Transform> _load = new();
        private Transform[] _base;              // 負荷検証以外の音源（元からある物）
        private AddedCollider[] _added;

        private void Start()
        {
            if (demo == null) demo = FindFirstObjectByType<AcousticFlowSceneDemo>();
            _added = FindObjectsByType<AddedCollider>(FindObjectsInactive.Include,
                                                      FindObjectsSortMode.None);
            Gather();
        }

        /// いま装置に繋がっている音源を「元からある物」と「負荷検証の物」に分ける。
        ///   世界を移ると繋ぎ替わるので、押されたときに毎回取り直す。
        private void Gather()
        {
            // ★負荷用は**名前でシーンから拾う**（1 回だけ）。
            //
            //   前は装置に繋がっている音源から拾っていたので、0 本にした瞬間に
            //   **見失って二度と戻せません**でした。装置から外れても実体は在るので、
            //   名前で持っておけば増減が往復できます。
            if (_load.Count == 0)
            {
                foreach (var t in FindObjectsByType<Transform>(FindObjectsInactive.Include,
                                                              FindObjectsSortMode.None))
                    if (t.name.StartsWith("Load_")) _load.Add(t);
                _load.Sort((a, b) => string.CompareOrdinal(a.name, b.name));
            }

            // 元からある音源＝いま繋がっているもののうち負荷用でないもの。
            //   世界を移ると入れ替わるので、こちらは毎回取り直す。
            var keep = new List<Transform>();
            int live = 0;
            if (demo != null && demo.extraSources != null)
                foreach (var s in demo.extraSources)
                {
                    if (s == null) continue;
                    if (s.name.StartsWith("Load_")) live++; else keep.Add(s);
                }
            _base = keep.ToArray();
            liveLoadSources = live;
        }

        private void Update()
        {
            acousticMs = AcousticFlowSceneDemo.Status.Valid
                       ? AcousticFlowSceneDemo.Status.AcousticMs : 0f;

            if (Input.GetKeyDown(fewerKey)) SetCount(liveLoadSources - 1);
            if (Input.GetKeyDown(moreKey)) SetCount(liveLoadSources + 1);
            if (Input.GetKeyDown(collidersKey)) ToggleColliders();

            // ★シーン側の費用を 1 つずつ切る。
            //   音源一覧を見るとタップは 9〜13 本しか無いのに 1 本あたり 12ms 掛かっている。
            //   つまり**費用はタップ（経路）ではなく、経路を探す側＝シーンの解き方**にある。
            //   容疑者は 3 つ。順に切って読むのがいちばん速い。
            if (demo == null) return;
            if (Input.GetKeyDown(KeyCode.J)) Cycle("反響（部屋グラフ）", () => demo.enableReverb = !demo.enableReverb);
            if (Input.GetKeyDown(KeyCode.N)) Cycle("反射", () => demo.useReflections = !demo.useReflections);
            if (Input.GetKeyDown(KeyCode.O)) Cycle("部屋の格子", () =>
                demo.roomCellSize = (demo.roomCellSize < 0.4f) ? 0.6f : 0.25f);
        }

        /// 切り替えて組み直し、直後の音響 ms をログに出す。
        private void Cycle(string what, System.Action change)
        {
            float before = acousticMs;
            change();
            demo.enabled = false; demo.enabled = true;
            Debug.Log($"[BellGame] {what} を切り替えた（反響={demo.enableReverb} 反射={demo.useReflections}"
                      + $" 格子={demo.roomCellSize:F2}m）。切替前の音響 {before:F2}ms。\n"
                      + "  ★数フレーム待ってから、モニターの『うち音響』を読んでください。", this);
        }

        public void SetCount(int n)
        {
            Gather();
            n = Mathf.Clamp(n, 0, _load.Count);

            var list = new List<Transform>(_base);
            for (int i = 0; i < n; i++) list.Add(_load[i]);

            if (demo != null)
            {
                demo.extraSources = list.ToArray();
                demo.enabled = false; demo.enabled = true;   // 組み直さないと届かない
            }
            liveLoadSources = n;
            Debug.Log($"[BellGame] 負荷検証の音源 {n} 本（元からある物 {_base.Length} 本）"
                      + $" → 音響 {acousticMs:F2}ms。数フレーム待ってから読んでください。", this);
        }

        public void ToggleColliders()
        {
            addedCollidersOn = !addedCollidersOn;
            int n = 0;
            foreach (var a in _added)
            {
                if (a == null) continue;
                foreach (var c in a.GetComponents<Collider>()) { c.enabled = addedCollidersOn; n++; }
            }
            if (demo != null) { demo.enabled = false; demo.enabled = true; }
            Debug.Log($"[BellGame] 後から足した当たり判定 = {(addedCollidersOn ? "入" : "切")}"
                      + $"（{n} 個）→ 音響 {acousticMs:F2}ms。\n"
                      + "  ★切って軽くなるなら、犯人は当たり判定（＝遮蔽物が増えたこと）です。", this);
        }

        private void OnGUI()
        {
            GUI.Label(new Rect(12, 34, 1100, 20),
                $"負荷: 音源 {liveLoadSources}本(-/=)  当たり判定 {(addedCollidersOn ? "入" : "切")}(K)  "
                + (demo != null
                   ? $"反響 {(demo.enableReverb ? "入" : "切")}(J)  反射 {(demo.useReflections ? "入" : "切")}(N)  "
                     + $"格子 {demo.roomCellSize:F2}m(O)  " : "")
                + $"音響 {acousticMs:F2}ms");
        }
    }
}
