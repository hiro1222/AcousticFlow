using UnityEngine;

namespace AcousticFlow
{
    /// <summary>
    /// 蝶番で開く扉。開き角を連続に変えられる。
    ///
    /// ■ 全体の中の位置
    ///   操作（キー・ドラッグ）→ **ここ**（角度 → 板の位置・向き・寸法）→ Unity の Transform →
    ///   AcousticWorld が「動く箱」として毎フレーム engine へ渡す → engine が影・開口・回折を解く。
    ///   ★このファイルは音に触れない。engine から見れば、扉は「たまたま動く箱」でしかない。
    ///   だから扉のために専用の仕組みを足さなくても、押した箱も同じ式で遮られる。
    ///
    /// ■ 役割
    ///   コンセプト（扉が開くときの音色の変化）を確かめるための、唯一の動く物。
    ///   開閉の 2 状態ではなく、途中のどの角度でも成立していることが要点 ──
    ///     ・隙間が開き始めた側の音源 … 早い段階から直接音が届く
    ///     ・板が覆う側の音源 … 振り切るまで透過のまま（蝶番の非対称）
    ///   事前計算では扱えない（角度ごとに焼き直せない）ので、この作品の主張そのものでもある。
    ///   docs/DIFFRACTION_DESIGN.md §7 の検証シーン。
    ///
    /// ■ 中の仕組み
    ///   角度 angleDeg を 1 つ持ち、Apply() が「蝶番から自由端への向き」を作って Transform に落とす。
    ///   角度の書き手は 1 つだけ（自動往復 > キー・ドラッグ）。複数が同じフレームで書くと往復して震える。
    ///
    /// ■ 繋がり
    ///   受ける: Unity の入力、DoorSweepLab（測定のときに角度を流す）。
    ///   渡す:   Transform（AcousticWorld が dynamic の箱として読む。旧コアでは AcousticFlowSceneDemo）。
    ///
    /// ■ 退けた書き方
    ///   ・Animator で開閉: 角度が曲線に焼かれるので、途中で止めて聞き比べられない。
    ///   ・板を消して「開いた」ことにする: 二値になる。ここが作品の否定したい形そのもの。
    ///
    /// ■ 壊れる所
    ///   ・蝶番の Y が扉の中心の高さでないと板が浮き、下に隙間が残って閉めても漏れる。
    ///   ・蝶番の奥行きは壁の**真ん中**に置く（2026-10-01 の決定。9/24 の「面一」を取り消した）。
    ///     板が枠の厚みの中にある間（0.2 m の壁で約 4°）は、枠の奥行きを通る細いすき間だけになり、こもったまま残る。
    ///     板の端が枠を抜けた所で音色が変わる（実測 φ=0: こもり 4° +17.2 → 5° +10.6 dB）。
    ///     ★大事なのは「開け始めが分かるか」ではなく「すき間が開いたときに変化するか」（発注者）。
    ///     面一（振れる側へ 0.07 m）にすると 1° でこもりがほぼ抜け（+19.2 → +10.9 dB）、扉の影が短くなる。
    ///   ・clearance を 0 にすると閉じた扉の開口が数値として 0 になり、「隙間から高域だけ漏れる」が消える。
    /// </summary>
    [ExecuteAlways]
    public class SwingDoor : MonoBehaviour
    {
        [Header("蝶番")]
        [Tooltip("回転軸の位置（ワールド）。扉の板の端に置く。\n"
                 + "★Y は**扉の中心の高さ**にすること（扉は蝶番を中心に上下へ伸びる）。\n"
                 + "  戸口が床から height まであるなら、蝶番の Y は height/2。\n"
                 + "  部屋の高さの半分を入れると扉が浮き、下に隙間が残って音が漏れる。\n"
                 + "★奥行き（振れる向きの軸）は**壁の厚みの真ん中**に置く（2026-10-01 の決定）。\n"
                 + "  板が枠の厚みを抜けるまでは細いすき間だけで、こもったまま残る。すき間が開いた所で音色が変わる。\n"
                 + "  大事なのは開け始めが分かるかではなく、すき間が開いたときに変化するか。\n"
                 + "  （実測 φ=0: 真ん中は 4° までこもり +17 dB 以上、5° で +10.6 dB。\n"
                 + "   開く側の面と面一（0.07 m 寄せる）だと 1° でこもりがほぼ抜け、扉の影が短くなる）")]
        public Transform hinge;

        [Tooltip("扉の幅(m)。蝶番から自由端まで。戸口の幅と一致させること。")]
        public float width = 1.0f;

        [Tooltip("枠との隙間(m)。実際の扉には 3〜5 mm ある。\n"
                 + "0 だと戸口をぴったり塞ぐので、閉じた状態の開口が数値としてゼロになり\n"
                 + "「隙間から高域だけが漏れる」（作品の前提）が形として存在しなくなる。\n"
                 + "自由端と上下に付ける。蝶番側は密着したまま。\n"
                 + "実測（閉扉の傾き 125Hz-4kHz）: 0mm 12.0 dB / 5mm 11.1 / 10mm 7.3 / 20mm 5.0。")]
        [Range(0f, 0.02f)] public float clearance = 0.004f;

        [Tooltip("扉の高さ(m)。戸口と同じにすること（低いと上に隙間が残る）。")]
        public float height = 3.0f;

        [Tooltip("扉の厚み(m)。")]
        public float thickness = 0.06f;

        [Header("開き")]
        [Range(0f, 120f)]
        [Tooltip("現在の開き角(度)。0=閉／大きいほど開く。Inspector で直接動かせる。")]
        public float angleDeg = 0f;

        [Tooltip("扉が振れる向き。閉じた状態の板の面法線がどちらへ倒れるか。")]
        public bool swingTowardPositiveZ = true;

        [Header("操作")]
        public bool enableKeys = true;
        [Tooltip("押している間だけ開く。離すと止まる（角度は保持）。")]
        public KeyCode openKey = KeyCode.Alpha5;
        [Tooltip("押している間だけ閉じる。")]
        public KeyCode closeKey = KeyCode.Alpha6;
        [Tooltip("開閉の速さ(度/秒)。")]
        public float speedDegPerSec = 45f;
        [Header("ドラッグ操作")]
        [Tooltip("左クリックしながらマウスを上下に動かすと開閉する（上へ動かすと開く）。\n"
                 + "扉を狙っている必要はない。右ドラッグ（視点）とは別のボタンなので同時に使える。\n"
                 + "★DoorSweepLab が流している間は enableKeys ごと止まるので、ここも効かない（角度の書き手は 1 つ）。")]
        public bool enableDrag = true;
        [Tooltip("マウスの上下 1 単位あたりの角度(度)。大きいほど少しの動きで開く。")]
        public float dragDegPerUnit = 15f;
        [Tooltip("自動で往復させる（デモ動画の収録用）。")]
        public bool autoSwing = false;
        [Range(0f, 120f)] public float autoMaxDeg = 90f;

        private float _autoDir = 1f;

        /// 角度を進めて Transform に落とす。角度の書き手はこの 1 か所だけ。
        ///   1) 自動往復（収録用）が入っていればそれが優先。0 と autoMaxDeg で折り返す。
        ///   2) 入っていなければキー（押している間だけ動く）と左ドラッグ（マウスの動きがそのまま速さ）。
        ///   3) 0〜120° に丸めてから Apply()。編集中（再生していないとき）も Apply だけは走るので、
        ///      Inspector で角度を変えれば場面の見た目もその場で動く。
        private void Update()
        {
            if (Application.isPlaying)
            {
                if (autoSwing)
                {
                    angleDeg += _autoDir * speedDegPerSec * Time.deltaTime;
                    if (angleDeg >= autoMaxDeg) { angleDeg = autoMaxDeg; _autoDir = -1f; }
                    else if (angleDeg <= 0f) { angleDeg = 0f; _autoDir = 1f; }
                }
                else if (enableKeys)
                {
                    if (Input.GetKey(openKey)) angleDeg += speedDegPerSec * Time.deltaTime;
                    if (Input.GetKey(closeKey)) angleDeg -= speedDegPerSec * Time.deltaTime;
                    // 左ドラッグの上下で開閉。速さはマウスの動きそのものなので、
                    // ゆっくり開けるのも一気に開けるのも手で決められる（聴き比べ用）。
                    if (enableDrag && Input.GetMouseButton(0))
                        angleDeg += Input.GetAxis("Mouse Y") * dragDegPerUnit;
                    angleDeg = Mathf.Clamp(angleDeg, 0f, 120f);
                }
            }
            Apply();
        }

        /// <summary>角度から板の位置・向き・寸法を作って Transform に入れる（engine が読むのはこの結果だけ）。
        ///   1) 蝶番→自由端の向き along = (cosθ, 0, ±sinθ)。± は振れる向き。
        ///   2) 板の中心は 蝶番 + along × 幅/2。向きは LookRotation(along)（長辺がローカル +Z のため）。
        ///   3) 枠との隙間は幅と高さから引く。★自由端と上下だけで、蝶番側は密着させる
        ///      （現実の扉と同じ。蝶番側に隙間を作ると、閉じていても常に漏れ続ける）。</summary>
        public void Apply()
        {
            if (hinge == null) return;

            // 閉じた状態の板は X 方向に伸びている。蝶番→自由端が +X。
            // 開くと、その向きが Z 側へ倒れていく。
            float th = angleDeg * Mathf.Deg2Rad;
            float s = swingTowardPositiveZ ? 1f : -1f;
            Vector3 along = new Vector3(Mathf.Cos(th), 0f, s * Mathf.Sin(th));  // 蝶番→自由端
            // 蝶番の水平の向き（Y 軸の回転）で回す（2026-10-04、Unreal の AAcousticFlowDoor と同じ）。回していなければ今までと同じ。
            //   ★水平の向きだけ使う（傾けて置いても板は立ったまま）。前は蝶番の向きを見ず、X に沿った壁にしか付けられなかった。
            along = Quaternion.Euler(0f, hinge.eulerAngles.y, 0f) * along;

            // localScale=(厚み, 高さ, 幅) なので、扉の長辺はローカル +Z。
            // したがって LookRotation の forward に along を渡す。
            // 枠との隙間を引く。自由端と上下に付け、蝶番側は密着させる
            //   （実際の扉と同じ。蝶番側に隙間を作ると、閉じていても常に漏れ続ける）。
            float c = Mathf.Max(0f, clearance);
            float w = Mathf.Max(0.01f, width - c);
            float h = Mathf.Max(0.01f, height - 2f * c);
            transform.position = hinge.position + along * (w * 0.5f);
            transform.rotation = Quaternion.LookRotation(along, Vector3.up);
            transform.localScale = new Vector3(thickness, h, w);
        }

        // ★選択していなくても出す。扉がどこにあって何度開いているかは、
        //   音を判断するための前提情報なので、常に見えていないと確認にならない
        //   （選択時だけだと、聞きながら角度を追えない）。
        /// 選択していなくても描く（下の DrawDoorGizmo）。
        private void OnDrawGizmos() { DrawDoorGizmo(false); }
        private void OnDrawGizmosSelected() { DrawDoorGizmo(true); }

        /// 扉の状態を場面に描く: 蝶番の軸（黄）、閉じた位置（白・薄）、今の開き角の扇（橙）、
        /// 自由端の辺（赤。ここが隙間 ＝ 開口を決める辺）。数値でなく形で見えないと、聞きながら追えない。
        private void DrawDoorGizmo(bool selected)
        {
            if (hinge == null) return;
            const float a = 0.9f;
            float s = swingTowardPositiveZ ? 1f : -1f;

            // 蝶番の軸。ここを中心に回る。
            Gizmos.color = new Color(1f, 0.85f, 0.2f, a);
            Gizmos.DrawLine(hinge.position + Vector3.down * height * 0.5f,
                            hinge.position + Vector3.up * height * 0.5f);

            // 閉じた位置（基準）を薄く。今どれだけ開いたかが比較で分かる。
            Gizmos.color = new Color(1f, 1f, 1f, 0.25f);
            Gizmos.DrawLine(hinge.position, hinge.position + new Vector3(width, 0f, 0f));

            // 開き角の扇。今の角度まで塗る。
            Gizmos.color = new Color(1f, 0.55f, 0.15f, selected ? 0.9f : 0.6f);
            Vector3 prev = hinge.position + new Vector3(width, 0f, 0f);
            for (int i = 1; i <= 16; ++i)
            {
                float th = Mathf.Deg2Rad * (angleDeg * i / 16f);
                Vector3 p = hinge.position + new Vector3(Mathf.Cos(th) * width, 0f,
                                                         s * Mathf.Sin(th) * width);
                Gizmos.DrawLine(prev, p);
                Gizmos.DrawLine(hinge.position, p);   // 扇を塗りつぶし気味に
                prev = p;
            }

            // 扉の**自由端**（隙間を作っている辺）を強調する。ここが開口を決める。
            float t = Mathf.Deg2Rad * angleDeg;
            Vector3 edge = hinge.position + new Vector3(Mathf.Cos(t) * width, 0f,
                                                        s * Mathf.Sin(t) * width);
            Gizmos.color = new Color(1f, 0.3f, 0.1f, 1f);
            Gizmos.DrawLine(edge + Vector3.down * height * 0.5f,
                            edge + Vector3.up * height * 0.5f);
            Gizmos.DrawSphere(edge, 0.06f);
        }
    }
}
