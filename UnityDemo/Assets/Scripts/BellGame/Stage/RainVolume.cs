// RainVolume.cs
// 雨。**カメラの前に板を 2 枚出すだけ。**粒子も、周囲を包む筒も使わない。
//
// ★発注者の判断「周りに降らせなくてもいい。カメラに映るだけで全然いい」。
//   筒をやめた理由はそれだけではない ── 筒は振り向いたときに
//   「筋が付いてくる／止まって見える」の板挟みになる。
//   画面に貼るなら、そもそも向きの問題が起きない。
//
// ★世界観・ステージ構成レーンが置いた。**描画だけで、音響には一切触らない。**
//   コライダーを作らないので `CollectOccluders` の対象外。
//
// ★崩壊都市で雨が要るのは演出のためではない。
//   WORLD_VISUAL §3-2 の芯は「**屋根の有無が読めること**」で、いまそれは
//   ベルを鳴らした瞬間にしか分からない。雨があれば**立っているだけで**分かる ──
//   屋根の下では降り込まず、屋根の落ちた建物では降り込む。
//
// ★だから「屋根の下では止まる」がこの部品の本体で、筋の見た目はその次。
//   真上へレイを 1 本撃って、遮られていれば `_Cover` を上げる。
//   しきい値は置かず、時間で滑らかに戻す（決めごと #2）。
//
// ★音は鳴らさない。雨音は `AmbientVoices` の担当で、
//   「屋内でくぐもる」も音響側が決める。ここで音量をいじると答えが 2 つになる。
using UnityEngine;

namespace BellGame
{
    [ExecuteAlways]
    [DisallowMultipleComponent]
    public sealed class RainVolume : MonoBehaviour
    {
        [Header("量")]
        [Range(0f, 1f)] public float amount = 0.55f;

        [Header("板（2 枚重ねて遠近を作る）")]
        [Tooltip("手前の板までの距離(m)。近いほど筋が大きく速く見える")]
        public float nearDistance = 0.9f;
        [Tooltip("奥の板までの距離(m)")]
        public float farDistance = 2.2f;

        [Header("覆い")]
        [Tooltip("この高さまで真上を見て、遮られていれば雨を止める")]
        public float coverRayLength = 30f;
        [Tooltip("覆いから外れてから戻り切るまでの秒数。急に降り出すと嘘に見える")]
        public float coverFade = 0.6f;
        public LayerMask coverMask = ~0;

        [Header("参照")]
        public Camera follow;             // 未設定ならメインカメラ
        public Material nearMaterial;
        public Material farMaterial;

        private Mesh _quad;
        private float _cover;

        private void OnEnable() { if (_quad == null) _quad = BuildQuad(); }

        private void LateUpdate()
        {
            var cam = follow != null ? follow : Camera.main;
#if UNITY_EDITOR
            if (cam == null && !Application.isPlaying
                && UnityEditor.SceneView.lastActiveSceneView != null)
                cam = UnityEditor.SceneView.lastActiveSceneView.camera;
#endif
            if (cam == null || _quad == null) return;

            // ★真上へ 1 本。屋根・庇・木の下で止まる。
            //   レイ 1 本で足りるのは、雨が「その場に降っているか」だけを知りたいから。
            bool covered = Physics.Raycast(cam.transform.position + Vector3.up * 0.2f,
                                           Vector3.up, coverRayLength, coverMask,
                                           QueryTriggerInteraction.Ignore);
            float dt = Application.isPlaying ? Time.deltaTime : 1f / 60f;
            float step = coverFade > 0.001f ? dt / coverFade : 1f;
            _cover = Mathf.MoveTowards(_cover, covered ? 1f : 0f, step);

            Draw(cam, nearMaterial, nearDistance, 1.0f);
            Draw(cam, farMaterial, farDistance, 0.85f);
        }

        /// カメラの前に板を 1 枚。**視野をちょうど埋める大きさに計算する** ──
        ///   固定寸法にすると、画角や画面比を変えたとき端が空く／はみ出す。
        private void Draw(Camera cam, Material mat, float dist, float scale)
        {
            if (mat == null) return;
            mat.SetFloat("_Cover", _cover);
            mat.SetFloat("_Amount", amount * scale);

            float h = 2f * dist * Mathf.Tan(cam.fieldOfView * 0.5f * Mathf.Deg2Rad);
            float w = h * Mathf.Max(cam.aspect, 0.1f);
            var t = cam.transform;
            // ★少し大きめ（1.06 倍）に。ぴったりだと丸め誤差で縁に線が出る。
            var m = Matrix4x4.TRS(t.position + t.forward * dist, t.rotation,
                                  new Vector3(w * 1.06f, h * 1.06f, 1f));
            // ★この板は**このカメラにだけ**描く。null（全カメラ）にすると、
            //   扉ごしのポータルカメラにも同じ板が本編カメラの位置で出て、
            //   向こうの世界に手前の雨が貼り付く。
            Graphics.DrawMesh(_quad, m, mat, gameObject.layer, cam, 0, null,
                              UnityEngine.Rendering.ShadowCastingMode.Off, false);
        }

        /// 原点中心・1×1 の板。UV は 0..1。
        private static Mesh BuildQuad()
        {
            var mesh = new Mesh { name = "RainQuad" };
            mesh.SetVertices(new System.Collections.Generic.List<Vector3>
            {
                new Vector3(-0.5f, -0.5f, 0f), new Vector3(0.5f, -0.5f, 0f),
                new Vector3( 0.5f,  0.5f, 0f), new Vector3(-0.5f, 0.5f, 0f),
            });
            mesh.SetUVs(0, new System.Collections.Generic.List<Vector2>
            {
                new Vector2(0f, 0f), new Vector2(1f, 0f),
                new Vector2(1f, 1f), new Vector2(0f, 1f),
            });
            mesh.SetTriangles(new[] { 0, 2, 1, 0, 3, 2 }, 0);
            // カメラの直前に置くので、視錐台で切られないよう大きめに宣言する。
            mesh.bounds = new Bounds(Vector3.zero, Vector3.one * 4f);
            return mesh;
        }
    }
}
