// LayoutWalker.cs
// 配置シーンを歩いて確かめるためだけの操作。**本編のプレイヤーではない。**
//
// ★世界観・ステージ構成レーンが置いた。形と絵を目で確かめるための道具で、
//   本編の移動・当たり・境界はゲームシステム側（AcousticFlowSceneDemo / WorldBounds）が持つ。
//   ここが無くても本編は動くし、本編に持ち込む前提でもない。
//
// ★音響には一切触らない。コライダーは CharacterController が 1 つ持つが、
//   これは Capsule なので `CollectOccluders`（BoxCollider と MeshCollider だけ拾う）
//   の対象外 ── 部屋グラフにも格子にも入らない。
//
// 操作は既存のデモに合わせてある（覚え直さずに済むように）:
//   WASD 移動 / 右ドラッグ 視線 / Shift で 2.5 倍速
//
// ★境界は円。WorldBounds は箱しか持たないので、ここでは半径で押し戻している。
//   手前 4m から効き始めて、境界には届かない（カクンと止めない）。
using UnityEngine;

namespace BellGame
{
    [RequireComponent(typeof(CharacterController))]
    [DisallowMultipleComponent]
    public sealed class LayoutWalker : MonoBehaviour
    {
        [Header("移動")]
        public float moveSpeed = 4f;
        public float runMultiplier = 2.5f;
        public float eyeHeight = 1.6f;
        public float gravity = 18f;

        [Header("視線")]
        public float mouseSensitivity = 2.5f;
        [Tooltip("右ドラッグ中だけ視線を回す。false なら常時")]
        public bool lookNeedsRightDrag = true;

        [Header("境界")]
        public Vector3 center = Vector3.zero;
        [Tooltip("円の境界。0 以下なら halfExtents の矩形を使う")]
        public float radius = 30f;
        [Tooltip("矩形の境界（radius <= 0 のときだけ効く）。崩壊都市のように四角い世界用")]
        public Vector2 halfExtents = Vector2.zero;
        [Tooltip("境界の手前どれだけから抵抗を効かせるか")]
        public float softMargin = 4f;

        [Header("床")]
        [Tooltip("草原の丘の式で床を受け止める。草原以外では false")]
        public bool terrainFloor = true;
        [Tooltip("terrainFloor が false のときの床の高さ")]
        public float floorY = 0f;

        [Header("参照")]
        public Transform eye;          // カメラ。未設定なら子から探す

        private CharacterController _cc;
        private float _yaw, _pitch, _fall;

        private void Awake()
        {
            _cc = GetComponent<CharacterController>();
            if (eye == null && transform.childCount > 0) eye = transform.GetChild(0);
            _yaw = transform.eulerAngles.y;
        }

        private void Update()
        {
            float dt = Time.deltaTime;

            // ── 視線 ────────────────────────────────────
            if (!lookNeedsRightDrag || Input.GetMouseButton(1))
            {
                _yaw += Input.GetAxis("Mouse X") * mouseSensitivity;
                _pitch = Mathf.Clamp(_pitch - Input.GetAxis("Mouse Y") * mouseSensitivity, -85f, 85f);
            }
            transform.rotation = Quaternion.Euler(0f, _yaw, 0f);
            if (eye != null) eye.localRotation = Quaternion.Euler(_pitch, 0f, 0f);

            // ── 移動 ────────────────────────────────────
            float x = Input.GetAxisRaw("Horizontal");
            float z = Input.GetAxisRaw("Vertical");
            Vector3 wish = transform.right * x + transform.forward * z;
            if (wish.sqrMagnitude > 1f) wish.Normalize();

            float speed = moveSpeed * (Input.GetKey(KeyCode.LeftShift) ? runMultiplier : 1f);
            Vector3 step = wish * speed;

            // ★境界の抵抗。外へ向かう成分だけを、境界に近づくほど削る。
            //   横に滑る成分は残るので、壁沿いに歩ける（真っ直ぐ止められると壁に見える）。
            Vector3 flat = transform.position - center;
            flat.y = 0f;
            if (radius > 0f)
            {
                float d = flat.magnitude;
                if (d > radius - softMargin && d > 0.001f)
                {
                    Vector3 outward = flat / d;
                    float outSpeed = Vector3.Dot(step, outward);
                    if (outSpeed > 0f)
                    {
                        float t = Mathf.InverseLerp(radius - softMargin, radius, d);
                        step -= outward * (outSpeed * t);
                    }
                }
            }
            else
            {
                // 矩形。X と Z を別々に削る。角でも斜めに滑れる。
                step.x = Edge(step.x, flat.x, halfExtents.x);
                step.z = Edge(step.z, flat.z, halfExtents.y);
            }

            // ── 重力 ────────────────────────────────────
            if (_cc.isGrounded && _fall < 0f) _fall = -2f;
            _fall -= gravity * dt;
            step.y = _fall;

            _cc.Move(step * dt);

            // ★コライダーの無い地形（縁の丘）へ落ちないよう、式の高さで受け止める。
            //   縁の丘はわざとコライダーを付けていない（格子が降格するため）。
            //   これは**草原の式**なので、他のステージでは切ること（切らないと丘の高さに浮く）。
            float floor = terrainFloor
                ? GrasslandTerrain.Height(transform.position.x, transform.position.z)
                : floorY;
            if (transform.position.y < floor)
            {
                var p = transform.position;
                p.y = floor;
                transform.position = p;
                _fall = 0f;
            }
        }

        /// 境界の手前で、外へ向かう成分だけ削る。円と同じ考え方を 1 軸ぶんだけ。
        private float Edge(float v, float pos, float half)
        {
            if (half <= 0f) return v;
            float d = Mathf.Abs(pos);
            if (d < half - softMargin) return v;
            float outward = Mathf.Sign(pos);
            if (v * outward <= 0f) return v;
            return v * (1f - Mathf.InverseLerp(half - softMargin, half, d));
        }

        private void OnDrawGizmosSelected()
        {
            Gizmos.color = new Color(0.3f, 0.8f, 1f, 0.6f);
            if (radius <= 0f)
            {
                Gizmos.DrawWireCube(center, new Vector3(halfExtents.x * 2f, 0.2f, halfExtents.y * 2f));
                return;
            }
            Gizmos.DrawWireSphere(center, radius);
            Gizmos.color = new Color(0.3f, 0.8f, 1f, 0.25f);
            Gizmos.DrawWireSphere(center, radius - softMargin);
        }
    }
}
