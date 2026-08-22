// WorldBounds.cs
// 世界の外へ出さない。ついでに壁をすり抜けないようにする。
//
// ★前提: プレイヤーには当たり判定が無い。
//   AcousticFlowSceneDemo.HandleMovement は `listener.position += move * speed * dt` の
//   直接代入で、CharacterController も Rigidbody も使っていない。
//   つまり**いまは壁も扉も地面もすり抜ける**。
//   デモを触らずに直すため、ここが LateUpdate で位置を押し戻す
//  （PinPosition / FollowTransform と同じやり方。デモが動かした後に上書きする）。
//
// ★境界を「見えないコライダー」で作らない。理由は音にある。
//   デモは BoxCollider と MeshCollider をシーン全体から拾ってオクルーダーにする。
//   見えない壁を箱で置いた瞬間、それは**音響的に本物の壁**になり、
//   何も無いはずの場所から早期反射が返る。
//   「音が世界を伝える」作品で、見えない物が聞こえるのは端的に嘘。
//
//   だから境界は**座標の押し戻し**でやる。音には何も起きない。
//   そのぶん境界そのものは無音になるので、
//   **世界を閉じるのは本来は造形の仕事**（丘・岩・稜線・瓦礫）で、
//   ここがやるのはその保険 ── 隙間をすり抜けた／登ってしまったときの受け止めだけ。
//
// ★押し戻しは連続にする（決めごと #2：連続量にしきい値を置かない）。
//   境界の手前 softMargin から抵抗が滑らかに増えて、境界には決して届かない。
//   カクンと止まる壁だと「見えない壁」の手触りになる。
using UnityEngine;
using AcousticFlow;

namespace BellGame
{
    [DisallowMultipleComponent]
    public sealed class WorldBounds : MonoBehaviour
    {
        [Header("配線")]
        [Tooltip("押し戻す対象。ふつうは Listener。")]
        public Transform listener;

        [Header("遊べる範囲")]
        // ★数字を焼き込まない。実行時に地面のコライダーから導く。
        //   生成時に計算して保存する形にしていたが、それだと
        //   「地面を動かした」「別のステージの値が残った」だけで範囲が世界とズレて、
        //   **出発点で動けなくなる**。実際にそれが起きた。
        //   地面が唯一の答えで、center / extents はその結果でしかない（決めごと #1）。
        [Tooltip("音響用の地面。ここから範囲を導く。空なら下の数字をそのまま使う。")]
        public Collider ground;
        [Tooltip("地面の縁からどれだけ内側を限界にするか。")]
        [Range(0f, 10f)] public float inset = 2f;

        [Tooltip("導かれた範囲（読み取り専用）。ground が空のときだけ手で入れる。")]
        public Vector3 center = Vector3.zero;
        public Vector3 extents = new Vector3(24f, 8f, 29f);
        [Tooltip("この幅だけ手前から抵抗が効きはじめる。0 で硬い壁になる。")]
        [Range(0f, 12f)] public float softMargin = 4f;

        [Header("壁のすり抜けを止める")]
        [Tooltip("切るとゲームの当たり判定が無くなる（デモ本来の挙動に戻る）。")]
        public bool solidWalls = true;
        [Tooltip("プレイヤーの太さ。")]
        [Range(0.1f, 1.2f)] public float bodyRadius = 0.35f;
        [Tooltip("1 フレームに見る障害物の数の上限。")]
        [Range(4, 64)] public int maxContacts = 16;

        [Header("診断（読み取り専用）")]
        [Tooltip("境界（座標クランプ）が効いている。")]
        public bool pushedByBounds;
        [Tooltip("壁の押し出しが効いている数。")]
        public int wallContacts;
        [Tooltip("最後に押し返してきた物の名前。**動けないときはここを見る**。")]
        public string lastPusher = "—";
        [Tooltip("この 1 フレームで押し戻された距離(m)。")]
        public float lastPushDistance;

        private bool _loggedBounds, _loggedWall;

        private Collider[] _hits;
        private int _previewLayer = -1;

        private void Awake()
        {
            _hits = new Collider[Mathf.Max(4, maxContacts)];
            _previewLayer = LayerMask.NameToLayer("WorldPreview");
        }

        // ★Start で組む。Awake ではない ──
        //   扉（WorldDoor）を数え上げるので、全部の Awake が済んでからにする。
        private void Start()
        {
            Derive();
        }

        /// 地面から範囲を導き、出発点と扉が必ず内側に入るまで広げる。
        public void Derive()
        {
            if (ground != null)
            {
                // ★Collider.bounds を使わない。
                //
                //   眠っている世界のコライダーは**無効**なので、bounds は潰れた箱を返す。
                //   すると下の Include(リスナー)/Include(扉) だけが範囲を決めることになり、
                //   **同じ形の世界から違う範囲が出る**（実際 30×30m の正方形から
                //   片方 ±13.0、もう片方 X±7.2 / Z±8.9 が出た）。
                //   起きたときにその狭い箱へ閉じ込められる。
                //
                //   形は transform と size から出せる。有効・無効に依存しない。
                var b = WorldAabb(ground);
                center = new Vector3(b.center.x,
                                     listener != null ? listener.position.y : b.center.y,
                                     b.center.z);
                extents = new Vector3(Mathf.Max(1f, b.extents.x - inset),
                                      20f,                       // 高さは効かせない（重力が無い）
                                      Mathf.Max(1f, b.extents.z - inset));
            }

            // ★出発点は**絶対に**抵抗の効かない側に入れる。
            //   ここを保証しないと、範囲が世界と合っていないときに
            //   「動いても元の場所へ戻される」＝ 詰みになる。境界が人を閉じ込めてはいけない。
            if (listener != null) Include(listener.position, 3f);

            // 扉も同じ。縁の近くに立っている扉が抵抗帯に入ると、
            // 「扉まで行けるのに押し返される」になる（雪山で実際にそうなっていた）。
            //
            // ★ただし**この世界の扉だけ**。
            //   世界が 3 つになって壊れました ── 行き先でない世界は扉から
            //   遠くへ退けるようになったので、その世界の扉まで取り込むと
            //   遊べる箱が 800m 伸びます（実測 X[-7.0, 801.3]）。
            //   世界の外へ抵抗なしで歩けてしまう。
            //
            // ★距離では切りません（決めごと #2）。**属しているかどうか**で切ります。
            //     ・この世界の中にある扉        → 入れる
            //     ・どの世界にも属さない扉      → 入れる（＝装置として外へ出した扉）
            //     ・**別の世界の中にある扉**    → 入れない
            foreach (var d in FindObjectsByType<WorldDoor>(FindObjectsSortMode.None))
            {
                if (d == null) continue;
                if (_previewLayer >= 0 && d.gameObject.layer == _previewLayer) continue;

                bool mine = d.transform.IsChildOf(transform.root);
                bool loose = d.transform.root.GetComponentInChildren<WorldBounds>(true) == null;
                if (!mine && !loose) continue;

                Include(d.transform.position, 3f);
            }

            Debug.Log($"[BellGame] 遊べる範囲: "
                      + $"X[{center.x - extents.x:F1}, {center.x + extents.x:F1}] "
                      + $"Z[{center.z - extents.z:F1}, {center.z + extents.z:F1}]"
                      + $" / 手前 {softMargin}m から抵抗"
                      + (ground != null ? $"（地面 '{ground.name}' から導出）" : "（手入力の値）"));
        }

        /// コライダーの世界 AABB。**有効・無効に依存しない**。
        private static Bounds WorldAabb(Collider col)
        {
            if (col is BoxCollider box)
            {
                var t = box.transform;
                Vector3 c = t.TransformPoint(box.center);
                Vector3 e = box.size * 0.5f;
                // 回転を含めた広がり。軸ごとに、各基底の寄与の絶対値を足す。
                Vector3 ax = t.TransformVector(new Vector3(e.x, 0f, 0f));
                Vector3 ay = t.TransformVector(new Vector3(0f, e.y, 0f));
                Vector3 az = t.TransformVector(new Vector3(0f, 0f, e.z));
                Vector3 ext = new Vector3(
                    Mathf.Abs(ax.x) + Mathf.Abs(ay.x) + Mathf.Abs(az.x),
                    Mathf.Abs(ax.y) + Mathf.Abs(ay.y) + Mathf.Abs(az.y),
                    Mathf.Abs(ax.z) + Mathf.Abs(ay.z) + Mathf.Abs(az.z));
                return new Bounds(c, ext * 2f);
            }

            // Box 以外（Mesh の地形など）は bounds に頼るしかない。
            //   無効だと潰れるので、そのときは分かるようにしておく。
            var b = col.bounds;
            if (b.extents.sqrMagnitude < 1e-4f)
                Debug.LogWarning($"[BellGame] 地面 '{col.name}' の大きさが取れない"
                    + "（コライダーが無効な間の Collider.bounds）。範囲が狭くなる。", col);
            return b;
        }

        private void Include(Vector3 p, float clearance)
        {
            float need = softMargin + clearance;
            var d = p - center;
            extents = new Vector3(Mathf.Max(extents.x, Mathf.Abs(d.x) + need),
                                  extents.y,
                                  Mathf.Max(extents.z, Mathf.Abs(d.z) + need));
        }

        // ★LateUpdate。デモが Update で位置を進めた後に上書きする。
        private void LateUpdate()
        {
            if (listener == null) return;

            if (solidWalls) PushOutOfWalls();

            var p = listener.position;
            var q = new Vector3(
                Soft(p.x, center.x - extents.x, center.x + extents.x, softMargin),
                Soft(p.y, center.y - extents.y, center.y + extents.y, softMargin),
                Soft(p.z, center.z - extents.z, center.z + extents.z, softMargin));

            pushedByBounds = (q - p).sqrMagnitude > 1e-8f;
            if (pushedByBounds)
            {
                lastPushDistance = (q - p).magnitude;
                listener.position = q;
                if (!_loggedBounds)
                {
                    _loggedBounds = true;
                    lastPusher = "境界（座標クランプ）";
                    Debug.LogWarning(
                        $"[BellGame] 境界が効いた。位置 {p} → {q}\n"
                        + $"  範囲: X[{center.x - extents.x:F1}, {center.x + extents.x:F1}] "
                        + $"Y[{center.y - extents.y:F1}, {center.y + extents.y:F1}] "
                        + $"Z[{center.z - extents.z:F1}, {center.z + extents.z:F1}] / 手前 {softMargin}m から抵抗\n"
                        + $"  ★出発点から動けないなら、この範囲が世界と合っていない。", this);
                }
            }
        }

        // 境界に**決して届かない**が、手前では自由に動ける写像。
        //   over（境界 margin 内へ踏み込んだ量）を m*(1 - 1/(1+over/m)) へ潰す。
        //   over→0 で恒等、over→∞ で m に漸近する。連続で単調。
        private static float Soft(float v, float lo, float hi, float m)
        {
            if (hi - lo <= 0f) return Mathf.Clamp(v, lo, hi);
            if (m <= 1e-4f) return Mathf.Clamp(v, lo, hi);
            m = Mathf.Min(m, (hi - lo) * 0.5f);

            float over = v - (hi - m);
            if (over > 0f) return (hi - m) + m * (1f - 1f / (1f + over / m));

            float under = (lo + m) - v;
            if (under > 0f) return (lo + m) - m * (1f - 1f / (1f + under / m));

            return v;
        }

        // ★ぶつかる相手は BoxCollider と MeshCollider だけにする。
        //   これはデモが**オクルーダーとして拾う条件と同じ**（CollectOccluders）。
        //   結果として「聞こえる物にだけぶつかる」が保証される ──
        //   音では素通りなのに体は止まる、という食い違いが原理的に起きない。
        //   音源やリスナーの球（SphereCollider）は自然に外れる。
        private void PushOutOfWalls()
        {
            wallContacts = 0;
            if (_probe == null) return;
            _probe.radius = bodyRadius;
            if (_hits == null || _hits.Length < maxContacts) _hits = new Collider[maxContacts];

            var p = listener.position;
            int n = Physics.OverlapSphereNonAlloc(p, bodyRadius, _hits, ~0, QueryTriggerInteraction.Ignore);

            for (int i = 0; i < n; i++)
            {
                var col = _hits[i];
                if (col == null || !col.enabled) continue;
                // ★ぶつかる相手は「音響が拾う物（Box/Mesh）」＋「敷居」。
                //   敷居だけは**意図的な例外**で、繋がった扉は物理的な壁ではなく
                //   操作で越えるものだから（DoorThreshold）。
                bool isWall = col is BoxCollider || col is MeshCollider;
                bool isBlocker = col.GetComponent<PlayerBlocker>() != null;
                if (!isWall && !isBlocker) continue;
                if (col.transform.IsChildOf(listener)) continue;
                // ★音源にはぶつからない。デモの IsExcluded と同じ扱いにする
                //   （音源の見た目は球なので普通は外れるが、形を差し替えられても壊れないように）。
                if (col.GetComponentInParent<IrConvolver>() != null) continue;
                if (col.GetComponentInParent<VoiceConvolver>() != null) continue;
                // ★音響専用の当たり判定（扉の脇の吸音壁）は体で押さない。
                //   拾うと野原に見えない壁ができて、扉の脇でぶつかる。
                if (col.GetComponent<AcousticOnly>() != null) continue;
                // ★扉の向こうに読み込んだ世界にもぶつからない。
                if (_previewLayer >= 0 && col.gameObject.layer == _previewLayer) continue;

                // 自分を球として、めり込みを解く。
                if (Physics.ComputePenetration(
                        _probe, p, Quaternion.identity,
                        col, col.transform.position, col.transform.rotation,
                        out var dir, out var dist))
                {
                    p += dir * dist;
                    wallContacts++;
                    if (!_loggedWall)
                    {
                        _loggedWall = true;
                        lastPusher = col.name;
                        Debug.LogWarning(
                            $"[BellGame] 壁に押し出された: '{col.name}'（{dist:F2}m / 向き {dir}）\n"
                            + $"  ★出発点から動けないなら、この物の中にプレイヤーが埋まっている。\n"
                            + $"    WorldBounds の solidWalls を切れば素通りに戻る。", col);
                    }
                }
            }

            if (wallContacts > 0)
            {
                lastPushDistance = (p - listener.position).magnitude;
                listener.position = p;
            }
        }

        // ComputePenetration は Collider 同士でしか解けないので、
        // 判定用の球を 1 つだけ持っておく（描画も物理シミュレーションもしない）。
        private SphereCollider _probe;

        private void OnEnable()
        {
            if (_probe != null) return;
            var go = new GameObject("BoundsProbe") { hideFlags = HideFlags.HideAndDontSave };
            go.transform.SetParent(transform, false);
            _probe = go.AddComponent<SphereCollider>();
            _probe.radius = bodyRadius;
            _probe.isTrigger = true;
            _probe.enabled = false;      // 他の判定に混ざらないよう、常時は切っておく
        }

        private void OnDisable()
        {
            if (_probe != null) DestroyImmediate(_probe.gameObject);
            _probe = null;
        }

        private void OnDrawGizmosSelected()
        {
            Gizmos.color = new Color(0.4f, 0.9f, 0.7f, 0.9f);
            Gizmos.DrawWireCube(center, extents * 2f);
            Gizmos.color = new Color(0.9f, 0.8f, 0.3f, 0.5f);
            Gizmos.DrawWireCube(center, (extents - Vector3.one * softMargin) * 2f);
        }
    }
}
