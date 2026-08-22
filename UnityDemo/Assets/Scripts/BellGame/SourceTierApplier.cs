// SourceTierApplier.cs
// 音源に貼られた段の札を、エンジンへ渡す係。**シーンに 1 つだけ置く。**
//
// ★なぜ 1 つに集めるか
//   段を渡す口は `AcousticScene.SetSourceTier(id, tier)` で、**id が要ります**。
//   id を知っているのは音響ホスト（AcousticFlowSceneDemo）だけなので、
//   音源が個々に自分を登録しに行く形にすると、音源の数だけホストを探すことになります。
//   渡す役は 1 つにして、札を読む側を歩かせるほうが素直です。
//
// ★★id の基底を**推測しません**。
//   システム側は `SourceId(i) => 10 + i` ですが、これは private const です。
//   ここに 10 と書くと、向こうが基底を変えた瞬間に**黙って別の音源へ段が当たります**
//   （エラーは出ません。音が変わるだけで、原因は絶対に分かりません）。
//
//   なので毎回**エンジンに問い合わせて確かめます** ──
//   `SourceIndex(id)` が期待どおりの並び番号を返す基底を探し、
//   見つからなければ**何もせずに黙らず、警告を出して止まります**。
//   システム側を触らずに、システム側の変更に気づける形はこれしかありません。
using System.Collections.Generic;
using AcousticFlow;
using UnityEngine;

namespace BellGame
{
    [DisallowMultipleComponent]
    [DefaultExecutionOrder(200)]        // ホストが音源を組み終わった後
    public sealed class SourceTierApplier : MonoBehaviour
    {
        [Tooltip("空なら実行時に探す。")]
        public AcousticFlowSceneDemo demo;

        [Tooltip("段を当てるか。OFF にすると全部エンジンの既定（＝厳密）に戻ります。")]
        public bool apply = true;

        [Header("診断（読み取り専用）")]
        public string status = "—";
        public int applied;
        public int exact, simple, virtualCount;

        private readonly List<Transform> _srcs = new List<Transform>();
        private ulong _idBase;
        private bool _idBaseKnown;
        private int _lastCount = -1;

        private void OnEnable() { _idBaseKnown = false; _lastCount = -1; }

        private void Update()
        {
            var scene = AcousticFlowSceneDemo.SharedScene;
            if (scene == null || !scene.IsValid) { status = "音響シーンがまだ無い"; return; }

            if (demo == null) demo = FindFirstObjectByType<AcousticFlowSceneDemo>();
            if (demo == null) { status = "音響ホストが見つからない"; return; }

            Gather();
            if (_srcs.Count == 0) { status = "音源が 0 本"; return; }

            // ★本数が変わったときだけ掛け直す。
            //   世界を移ると音源が丸ごと入れ替わるので、そこで拾い直す必要があります。
            //   毎フレーム掛けるのは無駄（段は変わらないので）。
            //   札を触ったときは操作卓が Reapply() を呼びます。
            if (_srcs.Count == _lastCount && _idBaseKnown) { ReadBack(scene); return; }

            if (!ResolveIdBase(scene)) return;
            Push(scene);
            _lastCount = _srcs.Count;
        }

        /// 札を触った後に呼ぶ。掛け直す。
        public void Reapply() { _lastCount = -1; _idBaseKnown = false; }

        private void Gather()
        {
            _srcs.Clear();
            if (demo.source != null) _srcs.Add(demo.source);
            if (demo.extraSources != null)
                foreach (var s in demo.extraSources) if (s != null) _srcs.Add(s);
        }

        // ★基底を**当てにいく**。書き込む前に、読み出しで筋が通ることを確かめる。
        private bool ResolveIdBase(AcousticScene scene)
        {
            // ありうる基底を総当たり。いまは 10 だが、数字そのものには意味を持たせない。
            for (ulong b = 0; b <= 64; b++)
            {
                bool ok = true;
                for (int i = 0; i < _srcs.Count && ok; i++)
                    if (scene.SourceIndex(b + (ulong)i) != i) ok = false;

                if (!ok) continue;
                if (!_idBaseKnown || _idBase != b)
                    Debug.Log($"[BellGame] 音源 id の基底を {b} と確かめました"
                              + $"（{_srcs.Count} 本すべてで並び番号が一致）。"
                              + "★推測ではなくエンジンに聞いています。", this);
                _idBase = b;
                _idBaseKnown = true;
                return true;
            }

            // ★黙って諦めない。ここで黙ると「段を設定したのに効かない」になります。
            _idBaseKnown = false;
            status = "音源 id の基底が分からない（段は当てていません）";
            Debug.LogWarning("[BellGame] 音源 id の基底を確かめられませんでした。"
                + "AcousticFlowSceneDemo.SourceId の付け方が変わった可能性があります。"
                + "★段は 1 つも当てていません（別の音源へ誤爆させないため）。", this);
            return false;
        }

        private void Push(AcousticScene scene)
        {
            applied = exact = simple = virtualCount = 0;

            for (int i = 0; i < _srcs.Count; i++)
            {
                var tag = _srcs[i].GetComponent<SourceTierTag>();

                // 札が無い音源は**厳密のまま**。既定を勝手に落とさない。
                var tier = (tag != null && apply) ? tag.tier : SourceTierTag.Tier.Exact;
                float radius = (tag != null && apply) ? tag.audibleRadius : 0f;

                ulong id = _idBase + (ulong)i;
                scene.SetSourceTier(id, (AcousticScene.SourceTier)(int)tier);
                scene.SetSourceAudibleRadius(id, radius);

                applied++;
                if (tier == SourceTierTag.Tier.Exact) exact++;
                else if (tier == SourceTierTag.Tier.Simple) simple++;
                else virtualCount++;
            }

            status = apply
                ? $"厳密 {exact} / 簡易 {simple} / バーチャル {virtualCount}"
                : $"当てていない（全部エンジンの既定＝厳密）／{applied} 本";
            Debug.Log($"[BellGame] 段を当てました: {status}", this);
        }

        // ★指定した段と、エンジンが**実際に使っている**段は違うことがあります。
        //   バーチャルは半径の外に出たときだけ効くので、近づけば厳密に戻ります。
        //   その食い違いが見えないと「バーチャルにしたのに軽くならない」を追えません。
        private void ReadBack(AcousticScene scene)
        {
            for (int i = 0; i < _srcs.Count; i++)
            {
                var tag = _srcs[i].GetComponent<SourceTierTag>();
                if (tag == null) continue;
                int e = scene.GetSourceTierEffective(i);
                tag.effective = (e == 0) ? "厳密" : (e == 1) ? "簡易" : (e == 2) ? "バーチャル" : "—";
            }
        }
    }
}
