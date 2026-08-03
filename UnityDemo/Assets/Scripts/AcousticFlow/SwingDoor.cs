using UnityEngine;

namespace AcousticFlow
{
    /// <summary>
    /// 蝶番で開く扉。開き角を連続に変えられる。
    ///
    /// 目的は「開いた角度によって直接聞こえる範囲が連続的に変わる」ことの提示。
    /// 開閉の2状態ではなく、途中のどの角度でも成立していることが要点。
    ///   ・隙間が開き始めた方向の音源 … 早い段階からダイレクトに聞こえる
    ///   ・振れた扉の板が覆う側の音源 … 扉が振り切るまで透過のまま
    ///
    /// これは事前計算では扱えない ── 角度ごとに焼き直すことができないため。
    /// docs/DIFFRACTION_DESIGN.md §7 の検証シーンであり、作品の主張そのものでもある。
    ///
    /// 扉は BoxCollider を持つ通常の障害物なので、
    /// AcousticFlowSceneDemo が毎フレーム transform を engine へ流すだけで機能する。
    /// </summary>
    [ExecuteAlways]
    public class SwingDoor : MonoBehaviour
    {
        [Header("蝶番")]
        [Tooltip("回転軸の位置（ワールド）。扉の板の端に置く。")]
        public Transform hinge;

        [Tooltip("扉の幅(m)。蝶番から自由端まで。")]
        public float width = 1.0f;

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
        [Tooltip("自動で往復させる（デモ動画の収録用）。")]
        public bool autoSwing = false;
        [Range(0f, 120f)] public float autoMaxDeg = 90f;

        private float _autoDir = 1f;

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
                    angleDeg = Mathf.Clamp(angleDeg, 0f, 120f);
                }
            }
            Apply();
        }

        /// <summary>蝶番まわりに回した位置・向き・寸法を transform に反映する。</summary>
        public void Apply()
        {
            if (hinge == null) return;

            // 閉じた状態の板は X 方向に伸びている。蝶番→自由端が +X。
            // 開くと、その向きが Z 側へ倒れていく。
            float th = angleDeg * Mathf.Deg2Rad;
            float s = swingTowardPositiveZ ? 1f : -1f;
            Vector3 along = new Vector3(Mathf.Cos(th), 0f, s * Mathf.Sin(th));  // 蝶番→自由端

            // localScale=(厚み, 高さ, 幅) なので、扉の長辺はローカル +Z。
            // したがって LookRotation の forward に along を渡す。
            transform.position = hinge.position + along * (width * 0.5f);
            transform.rotation = Quaternion.LookRotation(along, Vector3.up);
            transform.localScale = new Vector3(thickness, height, width);
        }

        private void OnDrawGizmosSelected()
        {
            if (hinge == null) return;
            Gizmos.color = Color.yellow;
            Gizmos.DrawLine(hinge.position + Vector3.down * height * 0.5f,
                            hinge.position + Vector3.up * height * 0.5f);
            // 開き角の扇を描く（どちらへ振れるかが一目で分かるように）。
            Gizmos.color = new Color(1f, 0.8f, 0.2f, 0.6f);
            float s = swingTowardPositiveZ ? 1f : -1f;
            Vector3 prev = hinge.position + new Vector3(width, 0f, 0f);
            for (int i = 1; i <= 12; ++i)
            {
                float th = Mathf.Deg2Rad * (angleDeg * i / 12f);
                Vector3 p = hinge.position + new Vector3(Mathf.Cos(th) * width, 0f,
                                                         s * Mathf.Sin(th) * width);
                Gizmos.DrawLine(prev, p);
                prev = p;
            }
        }
    }
}
