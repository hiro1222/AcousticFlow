// PhysicsDoor.cs
// 掴んで押し引きする扉。
//
// ★物理（Rigidbody + HingeJoint）はやめた。
//
//   もとは Visage 方式で関節に力を掛けていたが、
//     ・力を掛けても関節が一切動かない（joint/isKinematic/simulationMode は全部正常）
//     ・運動学へ落とすと今度は手を離した瞬間に勝手に閉じる
//   という往復から抜け出せなかった。原因は**角度の答えが 3 箇所にあった**こと ──
//   HingeJoint の解、運動学の積算値、そして診断用の表示値。
//   決めごと #1（同じ問いに 2 つの答えを持たせない）を自分で破っていた。
//
//   いまは **angleDeg ただ 1 つが答え**。キーも掴みもこの値を動かすだけで、
//   LateUpdate がそれを姿勢へ焼く。経路が 1 本なので、動かないなら
//   angleDeg が動いていない、としか起こりようがない。
//
// ★これで失うものは「物理の手触り」だけ。音は何も変わらない。
//   §4.3 が要求するのは「角度が連続で、途中で止まれること」であって、
//   その角度が関節の解であることではない。エンジンは毎フレーム transform を読む。
//
// ★トグルにはしない（ここは変えていない）。
//   0°⇄85° を往復させるとプレイヤーは途中の角度に留まらず、
//   隙間 5cm と 15cm の音色差が一度も体験されない。
//   押している間だけ動く／掴んでいる間だけ動く、を守る。
using UnityEngine;

namespace BellGame
{
    [DisallowMultipleComponent]
    public sealed class PhysicsDoor : MonoBehaviour
    {
        [Header("掴み")]
        [Tooltip("レイの起点。未設定なら Camera.main を使う。")]
        public Transform eye;
        [Tooltip("この距離まで届く。")]
        [Range(1f, 8f)] public float reach = 3.5f;
        [Tooltip("掴むボタン（0=左 / 1=右）。")]
        [Range(0, 1)] public int grabButton = 0;
        [Tooltip("カーソル 1px あたり何度動くか。")]
        [Range(0.02f, 2f)] public float degPerPixel = 0.35f;

        [Header("キー")]
        [Tooltip("押している間、開く。")]
        public KeyCode openKey = KeyCode.Alpha5;
        [Tooltip("押している間、閉じる。")]
        public KeyCode closeKey = KeyCode.Alpha6;
        [Tooltip("キーで動かす速さ(度/秒)。")]
        [Range(10f, 360f)] public float keySpeedDeg = 90f;

        [Header("扉")]
        [Tooltip("開ききる角度(度)。")]
        [Range(30f, 140f)] public float maxAngle = 95f;
        [Tooltip("開く向き。逆に開いてほしいときだけ反転する。")]
        public bool invertSwing;

        [Header("診断（読み取り専用）")]
        [Tooltip("★唯一の答え。0=閉 〜 maxAngle=開。")]
        public float angleDeg;
        public bool grabbed;
        [Tooltip("掴めないときはここを読む。")]
        public string grabStatus = "—";
        [Tooltip("直近 60 フレームの角度の振れ幅(度)。震えの目安。")]
        public float angleJitterDeg;

        private Vector3 _pivotWorld;      // 蝶番の world 位置。回しても動かない
        private Quaternion _closedRot;    // 閉じているときの姿勢
        private Vector3 _closedPos;       // 閉じているときの位置
        private float _applied;           // 姿勢へ焼いた角度
        private float _lastMouseX;
        private float _jMin, _jMax;
        private int _jCount;

        /// 0（閉）〜 maxAngle。WorldDoor と音響側はこれを読む。
        public float AngleDeg => angleDeg;

        private void Awake()
        {
            if (eye == null && Camera.main != null) eye = Camera.main.transform;

            // ★物理の部品が残っていても邪魔をさせない。
            //   古いシーンにも Rigidbody / HingeJoint が付いているので、
            //   ここで無力化する（消すと RequireComponent の名残で警告が出るため）。
            var joint = GetComponent<HingeJoint>();
            if (joint != null) Destroy(joint);
            var rb = GetComponent<Rigidbody>();
            if (rb != null)
            {
                rb.isKinematic = true;      // transform を書くのはこちら
                rb.useGravity = false;
                rb.detectCollisions = true; // 押し出し判定には使う
            }

            // 蝶番は板の左端。BoxCollider の幅から求める。
            var box = GetComponent<BoxCollider>();
            float w = (box != null) ? box.size.x : 1.1f;
            _closedPos = transform.position;
            _closedRot = transform.rotation;
            _pivotWorld = transform.TransformPoint(new Vector3(-w * 0.5f, 0f, 0f));
        }

        private void Update()
        {
            if (eye == null && Camera.main != null) eye = Camera.main.transform;

            if (Input.GetMouseButtonDown(grabButton)) TryGrab();
            if (Input.GetMouseButtonUp(grabButton)) grabbed = false;
            if (grabbed && !Input.GetMouseButton(grabButton)) grabbed = false;

            float delta = 0f;

            // キー。押している間だけ動く。離せばその角度で止まる。
            if (Input.GetKey(openKey) || Input.GetKey(KeyCode.Keypad5))
                delta += keySpeedDeg * Time.deltaTime;
            if (Input.GetKey(closeKey) || Input.GetKey(KeyCode.Keypad6))
                delta -= keySpeedDeg * Time.deltaTime;

            // 掴み。カーソルの横移動をそのまま角度にする。
            if (grabbed)
            {
                float dx = Input.mousePosition.x - _lastMouseX;
                _lastMouseX = Input.mousePosition.x;
                delta += dx * degPerPixel;
            }

            if (Mathf.Abs(delta) > 0f)
                angleDeg = Mathf.Clamp(angleDeg + delta, 0f, maxAngle);

            // 震えの目安。60 フレームぶんの振れ幅。
            if (_jCount == 0) { _jMin = _jMax = angleDeg; }
            _jMin = Mathf.Min(_jMin, angleDeg);
            _jMax = Mathf.Max(_jMax, angleDeg);
            if (++_jCount >= 60) { angleJitterDeg = _jMax - _jMin; _jCount = 0; }
        }

        // ★姿勢を焼くのは LateUpdate。
        //   角度が変わったときだけ、閉じた姿勢からの絶対回転として作り直す。
        //   差分を積むと誤差が溜まって、閉じても隙間が残る（＝音が漏れ続ける）。
        private void LateUpdate()
        {
            if (Mathf.Approximately(_applied, angleDeg)) return;
            _applied = angleDeg;

            float a = invertSwing ? -angleDeg : angleDeg;
            var rot = Quaternion.AngleAxis(a, Vector3.up);
            transform.rotation = rot * _closedRot;
            transform.position = _pivotWorld + rot * (_closedPos - _pivotWorld);
        }

        /// 角度を外から合わせる。**扉は 1 枚しかない**ので、
        /// 世界を移ったときに向こうの扉へ同じ角度を渡すために使う。
        public void ForceAngle(float deg) => angleDeg = Mathf.Clamp(deg, 0f, maxAngle);

        private void TryGrab()
        {
            if (eye == null) { grabStatus = "視点が無い（Camera.main）"; return; }
            // ★自分の音源をすり抜けてレイを飛ばす。
            //   プレイヤーのベル（Bell_Player）はリスナーの 1.5m 前方に付いてくるので、
            //   素の Raycast だと**必ずそれに当たって扉に届かない**。
            //   実際それで掴みが一度も成立していなかった。
            var hits = Physics.RaycastAll(eye.position, eye.forward, reach,
                                          ~0, QueryTriggerInteraction.Ignore);
            System.Array.Sort(hits, (a, b) => a.distance.CompareTo(b.distance));

            string firstBlocker = null;
            foreach (var h in hits)
            {
                if (h.collider == null) continue;
                // 音源（IrConvolver / VoiceConvolver を持つ）とリスナーは無視する。
                if (h.collider.GetComponentInParent<AcousticFlow.IrConvolver>() != null) continue;
                if (h.collider.GetComponentInParent<AcousticFlow.VoiceConvolver>() != null) continue;

                if (h.collider.transform == transform)
                {
                    grabbed = true;
                    grabStatus = "掴んだ";
                    _lastMouseX = Input.mousePosition.x;
                    return;
                }
                if (firstBlocker == null) firstBlocker = h.collider.name;
            }

            grabStatus = firstBlocker != null
                ? $"手前に '{firstBlocker}' がある（扉ではない）"
                : $"{reach}m 先まで何も無い（扉に近づいて正面を向く）";
        }
    }
}
