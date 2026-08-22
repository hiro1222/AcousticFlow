// InstancedCanopy.cs
// 木の葉を GPU インスタンシングで撒く。幹と枝だけメッシュで作り、樹冠はここが持つ。
//
// ★世界観・ステージ構成レーンが置いた。**描画だけで、音響には一切触らない。**
//   コライダーを作らないので `CollectOccluders` の対象外。
//   幹の BoxCollider（配置スクリプトが付ける）だけが音を遮る ── 葉は元々ほとんど遮らない。
//
// ★塊（Lobe）で樹冠を作るのをやめた理由
//   丸い塊を並べると団子に見えるうえ、**風で揺らせない**。
//   葉を 1 枚ずつインスタンスにすれば、シェーダ側の揺れがそのまま効く。
//
// 撒き方: 房（lobe）をいくつか置き、その楕円体の**外側寄り**に葉を散らす。
//   一様に詰めると中身が見えない球になって、輪郭が硬くなる。
using System.Collections.Generic;
using UnityEngine;

namespace BellGame
{
    [ExecuteAlways]
    [DisallowMultipleComponent]
    public sealed class InstancedCanopy : MonoBehaviour
    {
        [Header("樹冠の形（この Transform のローカル座標）")]
        public Vector3 center = new Vector3(0f, 7.4f, 0f);
        public Vector3 extents = new Vector3(15f, 4.2f, 15f);

        [Tooltip("房の数。1 だと球になって団子に見える")]
        [Range(1, 12)] public int lobes = 6;

        [Tooltip("房ごとの散らし量（extents に対する割合）")]
        [Range(0f, 0.8f)] public float lobeSpread = 0.42f;

        [Tooltip("外側寄りに撒く度合い。0=一様 1=殻だけ")]
        [Range(0f, 1f)] public float shell = 0.55f;

        [Header("葉")]
        [Range(200, 40000)] public int leafCount = 12000;
        public float leafSize = 0.34f;
        [Range(0f, 0.8f)] public float sizeJitter = 0.4f;

        [Header("色")]
        public Color colorA = new Color(0.30f, 0.46f, 0.20f);
        public Color colorB = new Color(0.20f, 0.34f, 0.15f);
        public Color colorLight = new Color(0.46f, 0.62f, 0.26f);

        [Header("描画")]
        public Material material;
        public float drawDistance = 60f;

        [Tooltip("葉に影を落とさせる。切ると幹と枝だけの**骨のような影**になる")]
        public bool castShadows = true;
        public int seed = 4120260;

        [Header("状態（読み取り専用）")]
        public int instanceCount;

        private const int BatchMax = 1023;

        private readonly List<Matrix4x4[]> _batches = new();
        private readonly List<int> _counts = new();
        // ★色は毎フレーム送らない（草と同じ理由）。塊ごとに 1 個持つ。
        private readonly List<MaterialPropertyBlock> _blocks = new();
        private Mesh _leaf;
        private int _builtHash;

        // 組み立て時の姿勢。世界ごと動かされたときに追いかけるための基準。
        private Matrix4x4 _builtTrs = Matrix4x4.identity;
        private bool _trsValid;

        private void OnEnable() => Rebuild();
        private void OnValidate() { if (isActiveAndEnabled) Rebuild(); }

        private int SettingsHash()
        {
            unchecked
            {
                int h = seed;
                h = h * 397 ^ leafCount;
                h = h * 397 ^ lobes;
                h = h * 397 ^ center.GetHashCode();
                h = h * 397 ^ extents.GetHashCode();
                h = h * 397 ^ leafSize.GetHashCode();
                h = h * 397 ^ shell.GetHashCode();
                h = h * 397 ^ lobeSpread.GetHashCode();
                return h;
            }
        }

        public void Rebuild()
        {
            _batches.Clear();
            _counts.Clear();
            _blocks.Clear();
            instanceCount = 0;
            if (leafCount <= 0) return;

            if (_leaf == null) _leaf = BuildLeafMesh();
            _builtHash = SettingsHash();
            _builtTrs = transform.localToWorldMatrix;
            _trsValid = true;

            var rng = new System.Random(seed);
            double R() => rng.NextDouble();

            // 房の中心を先に決める。
            var lobeAt = new Vector3[Mathf.Max(1, lobes)];
            for (int i = 0; i < lobeAt.Length; i++)
            {
                double a = System.Math.PI * 2.0 * i / lobeAt.Length + (R() - 0.5) * 0.6;
                float d = (float)R() * lobeSpread;
                lobeAt[i] = new Vector3((float)System.Math.Cos(a) * extents.x * d,
                                        (float)(R() - 0.35) * extents.y * lobeSpread,
                                        (float)System.Math.Sin(a) * extents.z * d);
            }

            var mats = new List<Matrix4x4>(leafCount);
            var cols = new List<Vector4>(leafCount);
            var world = transform.localToWorldMatrix;

            for (int i = 0; i < leafCount; i++)
            {
                Vector3 lc = lobeAt[(int)(R() * lobeAt.Length) % lobeAt.Length];

                // 単位球上の点。shell で内側をどれだけ空けるか決める。
                Vector3 dir = Random(rng);
                float t = Mathf.Lerp((float)System.Math.Pow(R(), 1.0 / 3.0), 1f, shell);
                Vector3 local = center + lc + Vector3.Scale(dir * t, extents * 0.5f);

                var rot = Quaternion.Euler((float)R() * 360f, (float)R() * 360f, (float)R() * 360f);
                float s = leafSize * (1f + ((float)R() * 2f - 1f) * sizeJitter);

                mats.Add(world * Matrix4x4.TRS(local, rot, Vector3.one * s));

                double pick = R();
                // 上のほうほど明るい葉を混ぜる（陽の当たる面のつもり）。
                float up = Mathf.InverseLerp(-extents.y * 0.5f, extents.y * 0.5f, local.y - center.y);
                Color c = pick < 0.25 + up * 0.35 ? colorLight : (pick < 0.7 ? colorA : colorB);
                cols.Add(new Vector4(c.r, c.g, c.b, 1f));
                instanceCount++;
            }

            for (int off = 0; off < mats.Count; off += BatchMax)
            {
                int n = Mathf.Min(BatchMax, mats.Count - off);
                var mb = new Matrix4x4[n];
                var cb = new Vector4[n];
                for (int k = 0; k < n; k++) { mb[k] = mats[off + k]; cb[k] = cols[off + k]; }
                var block = new MaterialPropertyBlock();
                block.SetVectorArray("_InstColor", cb);
                _batches.Add(mb);
                _counts.Add(n);
                _blocks.Add(block);
            }
        }

        private static Vector3 Random(System.Random rng)
        {
            // 球面一様。z を一様に取ってから角度を振る。
            double z = rng.NextDouble() * 2.0 - 1.0;
            double a = rng.NextDouble() * System.Math.PI * 2.0;
            double r = System.Math.Sqrt(1.0 - z * z);
            return new Vector3((float)(r * System.Math.Cos(a)), (float)z, (float)(r * System.Math.Sin(a)));
        }

        // ★葉の影について。
        //   幹と枝（メッシュ）は既定で影を落とすのに、葉（インスタンシング）を
        //   Off にしていたので、**茂った木の下に枝だけの骨のような影**が出ていた。
        //   葉 12,000 枚 × 2 三角形 = 2.4 万三角形が影のパスに増えるが、
        //   いまの 58.6 万に対して数 % で、絵の破綻に見合う。
        private void LateUpdate()
        {
            if (material == null || _leaf == null || _batches.Count == 0) return;
            if (SettingsHash() != _builtHash) Rebuild();

            // ★世界ごと動かされたぶんを差分で掛ける（InstancedGrassField と同じ理由）。
            //   行列は世界座標で焼いてあるので、WorldSet.PlaceWorld が根っこを動かすと
            //   葉だけ元の場所に取り残される。組み直すと乱数が回り直して形が変わるので、
            //   **差分だけ**掛ける。
            var now = transform.localToWorldMatrix;
            if (!_trsValid) { _builtTrs = now; _trsValid = true; }
            else if (now != _builtTrs)
            {
                var delta = now * _builtTrs.inverse;
                _builtTrs = now;
                for (int b = 0; b < _batches.Count; b++)
                {
                    var arr = _batches[b];
                    int n = _counts[b];
                    for (int k = 0; k < n; k++) arr[k] = delta * arr[k];
                }
            }

            var cam = Camera.main;
#if UNITY_EDITOR
            if (!Application.isPlaying && UnityEditor.SceneView.lastActiveSceneView != null)
                cam = UnityEditor.SceneView.lastActiveSceneView.camera;
#endif
            if (cam == null) return;

            Vector3 c = transform.TransformPoint(center);
            if ((c - cam.transform.position).sqrMagnitude > drawDistance * drawDistance) return;

            // ★描く相手を 1 台に絞らない（camera に null を渡す ＝ 全カメラ）。
            //
            //   Camera.main を渡していたせいで、**扉ごしには葉が一枚も出なかった**。
            //   扉の中は別のカメラ（WorldPortalStencil のポータルカメラ）が描いていて、
            //   そこには渡っていなかった。眠っている世界の物なので、なおさら気づきにくい。
            //
            //   全カメラに渡しても、レイヤーで振り分けられる ──
            //   眠っている世界は専用レイヤーに居て、本編カメラはそれを描かない。
            //   間引き（LOD）の基準は本編カメラのままでよい。
            //   ポータルカメラは本編カメラと同じ位置・同じ画角に置いてあるので、
            //   選び直しても同じ組が選ばれる。
            for (int b = 0; b < _batches.Count; b++)
            {
                Graphics.DrawMeshInstanced(_leaf, 0, material, _batches[b], _counts[b], _blocks[b],
                                           castShadows
                                               ? UnityEngine.Rendering.ShadowCastingMode.On
                                               : UnityEngine.Rendering.ShadowCastingMode.Off,
                                           false,
                                           gameObject.layer, null);
            }
        }

        /// 葉 1 枚。2 三角形。Cull Off で描くので裏面は作らない。
        private Mesh BuildLeafMesh()
        {
            var v = new List<Vector3>
            {
                new Vector3(-0.5f, 0f, -0.35f), new Vector3(0.5f, 0f, -0.35f),
                new Vector3( 0.5f, 0f,  0.35f), new Vector3(-0.5f, 0f,  0.35f),
            };
            // 法線を少し外へ倒して、平らな板がベタ塗りにならないようにする。
            var n = new List<Vector3>
            {
                new Vector3(-0.4f, 1f, -0.3f).normalized, new Vector3(0.4f, 1f, -0.3f).normalized,
                new Vector3( 0.4f, 1f,  0.3f).normalized, new Vector3(-0.4f, 1f, 0.3f).normalized,
            };
            var mesh = new Mesh { name = "CanopyLeaf" };
            mesh.SetVertices(v);
            mesh.SetNormals(n);
            mesh.SetTriangles(new[] { 0, 2, 1, 0, 3, 2 }, 0);
            mesh.RecalculateBounds();
            return mesh;
        }

        private void OnDrawGizmosSelected()
        {
            Gizmos.color = new Color(0.4f, 0.9f, 0.4f, 0.4f);
            Gizmos.matrix = transform.localToWorldMatrix;
            Gizmos.DrawWireCube(center, extents);
        }
    }
}
