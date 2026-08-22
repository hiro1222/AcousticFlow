// InstancedGrassField.cs
// 草を GPU インスタンシングで撒く。実体メッシュ（OBJ）の代わり。
//
// ★世界観・ステージ構成レーンが置いた。**描画だけで、音響には一切触らない。**
//   コライダーを 1 つも作らないので `CollectOccluders` の対象外 ──
//   部屋グラフにも格子にも 1 ボクセルも入らない。
//
// ★なぜ OBJ をやめるか
//   実体メッシュ版は 11.7 万三角形・10.4MB の OBJ になっていた。公開リポジトリに
//   毎回この重さが乗るうえ、密度を上げるとほぼ線形に増える。
//   インスタンシングなら**リポジトリのファイルは 0 バイト**で、密度は数字ひとつで変わる。
//
// ★Unity Terrain の草機能は使えない。
//   Terrain が持つのは TerrainCollider で、音響ホストが拾うのは
//   BoxCollider と MeshCollider だけ。地面が音響から消えて早期反射が全部無くなる。
//
// 仕組み:
//   1. 起動時に決定的な擬似乱数で位置を作り、タイル（既定 8m 角）へ仕分ける
//   2. タイルごとに Matrix4x4 を 1023 本ずつの塊にしておく
//   3. 毎フレーム、カメラから近いタイルだけ Graphics.DrawMeshInstanced へ渡す
//      → 距離での間引きが CPU 側で効く。遠くの草は submit すらしない
//   4. 葉の揺れ・段の付いた陰影・裏面描画はシェーダ（BellGame/InstancedGrass）側
using System.Collections.Generic;
using UnityEngine;

namespace BellGame
{
    [ExecuteAlways]
    [DisallowMultipleComponent]
    public sealed class InstancedGrassField : MonoBehaviour
    {
        [Header("撒く範囲（この Transform の位置が中心）")]
        [Tooltip("半径 m。草原のボーダーと同じ 30m を想定")]
        public float radius = 29.6f;

        [Tooltip("1 m² あたりの本数")]
        [Range(1f, 400f)] public float density = 110f;

        [Tooltip("この半径の内側には撒かない（扉の丘の平場など）。0 で無効")]
        public float innerHole = 0f;

        [Tooltip("草原の地形（扉の丘・縁の丘）に沿わせる。切ると全部 y=0 に並ぶ")]
        public bool followTerrain = true;

        [Tooltip("川の中心線からこの距離までは撒かない。0 で無効")]
        public float riverClear = 0f;

        [Tooltip("画面に入らないタイルを描かない。切ると全周を描く（切り分け用）")]
        public bool frustumCull = true;

        // 石段や床など、草が生えては困る物の footprint を矩形で渡す。
        [Tooltip("撒かない矩形。(中心X, 中心Z, 半幅X, 半幅Z)")]
        public Vector4[] excludeRects;

        [Tooltip("ボーダー(30m)の外は密度をこの倍率まで落とす。丘の上の草を薄くする")]
        [Range(0f, 1f)] public float outerDensity = 0.45f;

        [Header("ボーダーの外の色（暗い地面に合わせる）")]
        public Color outerColorA = new Color(0.30f, 0.42f, 0.24f);
        public Color outerColorB = new Color(0.22f, 0.32f, 0.19f);

        [Header("葉")]
        [Tooltip("穂を付ける。背の高い層に使うと、空に対する細い縦線が出る")]
        public bool seedHead = false;

        public float bladeHeight = 0.70f;
        public float bladeWidth = 0.017f;
        [Tooltip("高さのばらつき（±割合）")]
        [Range(0f, 0.8f)] public float heightJitter = 0.45f;

        [Header("色（インスタンスごとに 3 色から選ぶ）")]
        public Color colorA = new Color(0.42f, 0.56f, 0.24f);
        public Color colorB = new Color(0.27f, 0.40f, 0.18f);
        public Color colorDry = new Color(0.63f, 0.56f, 0.31f);
        [Range(0f, 1f)] public float dryRatio = 0.12f;

        [Header("描画")]
        public Material material;
        [Tooltip("この距離より遠いタイルは submit しない")]
        public float drawDistance = 30f;

        [Tooltip("ここまでは全部の草を描く。これより遠いタイルは間引いた組だけ描く")]
        public float nearDistance = 15f;

        [Tooltip("遠くでも残す割合。残す草はそのぶん大きくして、density が保たれて見えるようにする")]
        [Range(0.05f, 1f)] public float farKeep = 0.16f;

        [Tooltip("遠くに残した草の拡大率。間引いた隙間を埋める")]
        public float farScale = 1.95f;
        public float tileSize = 6f;
        public int seed = 20260820;

        [Header("状態（読み取り専用）")]
        public int instanceCount;
        public int tilesDrawnLastFrame;
        public int tilesTotal;
        public int instancesDrawnLastFrame;

        private const int BatchMax = 1023;

        // ★遠近で 2 組に分ける。far は間引いた組で、近くでも遠くでも描く。
        //   near は近いタイルでだけ描き足す。こうすると「遠くまで密度を保つ」を
        //   描画量を倍にせずに出せる（そのまま距離だけ伸ばすと 2 倍以上になる）。
        private sealed class Tile
        {
            public Vector3 center;
            // ★視錐台で捨てるための境界箱。組み立て時に 1 回だけ作る。
            //   毎フレーム作り直すと、間引きで浮かせたぶんをそこで使ってしまう。
            public Bounds bounds;
            public readonly List<Matrix4x4[]> batches = new();
            public readonly List<int> counts = new();
            public readonly List<bool> isFar = new();
            // ★色は毎フレーム送らない。塊ごとに MaterialPropertyBlock を 1 個作って持つ。
            //   SetVectorArray を毎フレーム呼ぶと、密度を上げた分だけ CPU がそこで詰まる
            //   （15 万本なら毎フレーム 15 万個の Vector4 を積み直すことになる）。
            public readonly List<MaterialPropertyBlock> blocks = new();
        }

        private readonly List<Tile> _tiles = new();
        private Mesh _blade;
        private int _builtHash;

        // ★組み立て時の姿勢を控える。**世界ごと動かされたときに追いかけるため。**
        //
        //   行列は世界座標で焼いてある。WorldSet は扉に合わせて世界の根っこを動かすので
        //  （PlaceWorld）、動かされた瞬間に草だけ元の場所に取り残される。
        //   視錐台で捨てる作りなので、結果は「草が全部消える」になる。
        //
        //   ★組み直しではなく**差分を掛ける**。組み直すと地形のレイを撃ち直し、
        //     乱数も回り直すので、草の生え方そのものが変わってしまう
        //    （場所を移しただけなのに景色が別物になる）。差分なら同じ草がそのまま動く。
        private Matrix4x4 _builtTrs = Matrix4x4.identity;
        private bool _trsValid;

        private void OnEnable() => Rebuild();
        private void OnValidate() { if (isActiveAndEnabled) Rebuild(); }

        private int SettingsHash()
        {
            unchecked
            {
                int h = seed;
                h = h * 397 ^ radius.GetHashCode();
                h = h * 397 ^ density.GetHashCode();
                h = h * 397 ^ innerHole.GetHashCode();
                h = h * 397 ^ bladeHeight.GetHashCode();
                h = h * 397 ^ bladeWidth.GetHashCode();
                h = h * 397 ^ heightJitter.GetHashCode();
                h = h * 397 ^ tileSize.GetHashCode();
                h = h * 397 ^ dryRatio.GetHashCode();
                h = h * 397 ^ followTerrain.GetHashCode();
                h = h * 397 ^ outerDensity.GetHashCode();
                h = h * 397 ^ farKeep.GetHashCode();
                h = h * 397 ^ farScale.GetHashCode();
                h = h * 397 ^ seedHead.GetHashCode();
                h = h * 397 ^ riverClear.GetHashCode();
                if (excludeRects != null) foreach (var e in excludeRects) h = h * 397 ^ e.GetHashCode();
                return h;
            }
        }

        public void Rebuild()
        {
            _tiles.Clear();
            instanceCount = 0;
            if (radius <= 0f || density <= 0f) return;
            // ★材質が入るまで組まない。**AddComponent 直後の 1 回目を止める**ため。
            //   配置スクリプトは AddComponent → 各種設定 → material の順に入れるので、
            //   ここで止めないと 448,000 本を**二度**組む（起動が数秒延びる）。
            if (material == null) return;

            if (_blade == null) _blade = BuildBladeMesh();

            _builtHash = SettingsHash();
            _builtTrs = transform.localToWorldMatrix;   // 追いかける基準を組み立て時に取り直す
            _trsValid = true;

            int total = Mathf.RoundToInt(Mathf.PI * radius * radius * density);
            var rng = new System.Random(seed);
            Vector3 origin = transform.position;

            // タイルの格子。半径を覆う正方の並びを作り、円の外は空のまま捨てる。
            int side = Mathf.Max(1, Mathf.CeilToInt(radius * 2f / Mathf.Max(tileSize, 1f)));
            // タイルごとに「遠くでも残す組」と「近くだけの組」を別々に貯める。
            var farGrid = new Dictionary<int, List<(Matrix4x4 m, Vector4 c)>>();
            var nearGrid = new Dictionary<int, List<(Matrix4x4 m, Vector4 c)>>();

            for (int i = 0; i < total; i++)
            {
                // 円内一様。sqrt を掛けないと中心に寄る。
                double a = rng.NextDouble() * System.Math.PI * 2.0;
                double rr = System.Math.Sqrt(rng.NextDouble()) * radius;
                float x = (float)(System.Math.Cos(a) * rr);
                float z = (float)(System.Math.Sin(a) * rr);
                if (innerHole > 0f && (x * x + z * z) < innerHole * innerHole) continue;

                // ★安い判定から順に置く。下の川の距離が**このループで一番高い**ので、
                //   先に捨てられる本数を増やしておく。
                //   ボーダーの外は薄く ── 丘まで同じ密度で敷くと、行けない場所に一番草が生える。
                bool outside = (x * x + z * z) > GrasslandTerrain.Border * GrasslandTerrain.Border;
                if (outside && rng.NextDouble() > outerDensity) continue;

                // ★川の水面と土手には生やさない。OBJ の草には入れていた除外を、
                //   インスタンシングへ移したとき引き継いでいなかった（草が水面から生えていた）。
                if (riverClear > 0f && GrasslandTerrain.RiverDistance(x, z) < riverClear) continue;

                // ★石の上に草を生やさない。インスタンシングは置いてある物を知らないので、
                //   footprint を矩形で渡してもらう（門の石段に生えていた）。
                if (excludeRects != null)
                {
                    bool blocked = false;
                    foreach (var e in excludeRects)
                        if (Mathf.Abs(x - e.x) < e.z && Mathf.Abs(z - e.y) < e.w) { blocked = true; break; }
                    if (blocked) continue;
                }

                float yaw = (float)(rng.NextDouble() * 360.0);
                float s = 1f + ((float)rng.NextDouble() * 2f - 1f) * heightJitter;

                // ★地形に沿わせる。y=0 に並べると、扉の丘（0.7m）の草が丘の中に埋まる。
                float gy = followTerrain ? GrasslandTerrain.Height(x, z) : 0f;
                var pos = origin + new Vector3(x, gy, z);

                // 斜面では少し寝かせる。垂直のままだと丘の上だけ生え方が不自然になる。
                Quaternion rot = Quaternion.Euler(0f, yaw, 0f);
                if (followTerrain)
                {
                    var n = GrasslandTerrain.Normal(x, z);
                    rot = Quaternion.Slerp(Quaternion.identity, Quaternion.FromToRotation(Vector3.up, n), 0.7f) * rot;
                }
                bool far = rng.NextDouble() < farKeep;
                float w = far ? farScale : 1f;      // 残す草は太らせて隙間を埋める
                var m = Matrix4x4.TRS(pos, rot, new Vector3(w, s * (far ? Mathf.Lerp(1f, farScale, 0.5f) : 1f), w));

                double pick = rng.NextDouble();
                Color ca = outside ? outerColorA : colorA;
                Color cb = outside ? outerColorB : colorB;
                Color col = pick < dryRatio ? colorDry : (pick < 0.5 + dryRatio * 0.5 ? ca : cb);

                int gx = Mathf.FloorToInt((x + radius) / tileSize);
                int gz = Mathf.FloorToInt((z + radius) / tileSize);
                int key = gz * (side + 2) + gx;
                var g = far ? farGrid : nearGrid;
                if (!g.TryGetValue(key, out var list)) g[key] = list = new List<(Matrix4x4, Vector4)>();
                list.Add((m, new Vector4(col.r, col.g, col.b, 1f)));
                instanceCount++;
            }

            // タイルは far / near の和集合で作る（どちらか片方しか無いタイルもある）。
            var keys = new HashSet<int>(farGrid.Keys);
            keys.UnionWith(nearGrid.Keys);

            foreach (int key in keys)
            {
                var tile = new Tile();
                Vector3 sum = Vector3.zero;
                int total2 = 0;

                Fill(farGrid, key, tile, isFar: true, ref sum, ref total2);
                Fill(nearGrid, key, tile, isFar: false, ref sum, ref total2);
                if (total2 == 0) continue;

                tile.center = sum / total2;
                // 境界箱。位置は行列の平行移動成分から取る。
                //   ★上へ葉の高さぶん、横へ半分ぶん広げる ── 原点しか包まないと
                //     画面の縁で葉がちらつく（原点は外だが葉先は中、という状態）。
                var bmin = new Vector3(float.MaxValue, float.MaxValue, float.MaxValue);
                var bmax = new Vector3(float.MinValue, float.MinValue, float.MinValue);
                for (int b = 0; b < tile.batches.Count; b++)
                    for (int k = 0; k < tile.counts[b]; k++)
                    {
                        var e = tile.batches[b][k];
                        var p = new Vector3(e.m03, e.m13, e.m23);
                        bmin = Vector3.Min(bmin, p);
                        bmax = Vector3.Max(bmax, p);
                    }
                float pad = bladeHeight * (1f + heightJitter) * farScale;
                bmin -= new Vector3(pad, 0f, pad);
                bmax += new Vector3(pad, pad, pad);
                tile.bounds = new Bounds((bmin + bmax) * 0.5f, bmax - bmin);
                _tiles.Add(tile);
            }
            tilesTotal = _tiles.Count;

            static void Fill(Dictionary<int, List<(Matrix4x4 m, Vector4 c)>> grid, int key,
                             Tile tile, bool isFar, ref Vector3 sum, ref int total)
            {
                if (!grid.TryGetValue(key, out var src)) return;
                foreach (var e in src) sum += new Vector3(e.m.m03, e.m.m13, e.m.m23);
                total += src.Count;

                for (int off = 0; off < src.Count; off += BatchMax)
                {
                    int n = Mathf.Min(BatchMax, src.Count - off);
                    var mats = new Matrix4x4[n];
                    var cols = new Vector4[n];
                    for (int k = 0; k < n; k++) { mats[k] = src[off + k].m; cols[k] = src[off + k].c; }
                    var block = new MaterialPropertyBlock();
                    block.SetVectorArray("_InstColor", cols);
                    tile.batches.Add(mats);
                    tile.counts.Add(n);
                    tile.blocks.Add(block);
                    tile.isFar.Add(isFar);
                }
            }
        }

        /// 世界ごと動かされたぶんを、焼いた行列に**差分で**掛ける。
        ///   組み直さないので、生え方は 1 本も変わらない。
        private void FollowTransform()
        {
            var now = transform.localToWorldMatrix;
            if (!_trsValid) { _builtTrs = now; _trsValid = true; return; }
            if (now == _builtTrs) return;

            var delta = now * _builtTrs.inverse;
            _builtTrs = now;

            foreach (var tile in _tiles)
            {
                for (int b = 0; b < tile.batches.Count; b++)
                {
                    var arr = tile.batches[b];
                    int n = tile.counts[b];
                    for (int k = 0; k < n; k++) arr[k] = delta * arr[k];
                }
                tile.center = delta.MultiplyPoint3x4(tile.center);
                // 境界箱も一緒に運ぶ。置いていくと視錐台の判定だけ元の場所に残る。
                tile.bounds = new Bounds(delta.MultiplyPoint3x4(tile.bounds.center), tile.bounds.size);
            }
        }

        private void LateUpdate()
        {
            // ★組み直しの判定を**描く判定より先に**置くこと。
            //   逆にすると、材質待ちで空のまま組まれた回（_tiles.Count==0）に引っ掛かって
            //   二度と組み直されない。
            if (material == null) return;
            if (SettingsHash() != _builtHash) Rebuild();
            FollowTransform();
            if (_blade == null || _tiles.Count == 0) return;

            var cam = Camera.main;
#if UNITY_EDITOR
            if (!Application.isPlaying && UnityEditor.SceneView.lastActiveSceneView != null)
                cam = UnityEditor.SceneView.lastActiveSceneView.camera;
#endif
            if (cam == null) return;

            Vector3 eye = cam.transform.position;
            float cutFar = drawDistance + tileSize;
            // ★視錐台で捨てる。**距離の間引きだけでは 4 倍損している。**
            //   草は円盤状に撒いてあるが、16:9 の水平画角は約 91 度 ── 見ているのは 4 分の 1。
            //   残りはラスタライズもされずに捨てられるが、submit と頂点処理は掛かる。
            //
            //   ★基準は**本編カメラ**でよい。ポータルカメラは本編と同じ位置・同じ画角に
            //     置いてあるので、同じ組が選ばれる（葉と同じ考え方）。
            //   ★影は関係しない。草は ShadowCastingMode.Off で描いているので、
            //     画面外を捨てても影が欠けることはない。
            var planes = frustumCull ? GeometryUtility.CalculateFrustumPlanes(cam) : null;
            float cutNear = nearDistance + tileSize;
            tilesDrawnLastFrame = 0;
            instancesDrawnLastFrame = 0;

            foreach (var tile in _tiles)
            {
                Vector3 d = tile.center - eye;
                d.y = 0f;
                float dist2 = d.sqrMagnitude;
                if (dist2 > cutFar * cutFar) continue;
                if (planes != null && !GeometryUtility.TestPlanesAABB(planes, tile.bounds)) continue;
                bool near = dist2 <= cutNear * cutNear;

                for (int b = 0; b < tile.batches.Count; b++)
                {
                    // 遠いタイルでは「間引いた組」だけ描く。
                    if (!near && !tile.isFar[b]) continue;
                    instancesDrawnLastFrame += tile.counts[b];
                    // ★描く相手を 1 台に絞らない（camera に null を渡す ＝ 全カメラ）。
                    //
                    //   Camera.main を渡していたせいで、**扉ごしには草が一本も出なかった**。
                    //   扉の中は別のカメラ（WorldPortalStencil のポータルカメラ）が描いていて、
                    //   そこには渡っていなかった。眠っている世界の物なので気づきにくい。
                    //
                    //   全カメラに渡しても、レイヤーで振り分けられる ──
                    //   眠っている世界は専用レイヤーに居て、本編カメラはそれを描かない。
                    //   間引き（上の isFar）の基準は本編カメラのままでよい。
                    //   ポータルカメラは本編カメラと同じ位置・同じ画角に置いてある。
                    Graphics.DrawMeshInstanced(_blade, 0, material, tile.batches[b],
                                               tile.counts[b], tile.blocks[b],
                                               UnityEngine.Rendering.ShadowCastingMode.Off, false,
                                               gameObject.layer, null);
                }
                tilesDrawnLastFrame++;
            }
        }

        /// 葉 1 枚。3 節＋先端で 7 三角形。Cull Off で描くので裏面は作らない。
        ///
        ///   ★法線を面から計算しない。平らな板の法線は全部同じ向きになり、
        ///     セル調の段が葉の面ごとにベタっと乗って「ポリゴンが見える」原因になる。
        ///     幅方向へ扇状に倒した法線を手で入れて、丸い葉のように陰影を回す。
        private Mesh BuildBladeMesh()
        {
            float h = bladeHeight, w = bladeWidth;
            const float Curve = 1.15f;   // 大きいほど断面が丸く見える

            var v = new List<Vector3>();
            var nrm = new List<Vector3>();
            // t = 0, 0.42, 0.74, 0.92 の 4 段＋先端。先へ行くほど細く、前へ反る。
            float[] ts = { 0f, 0.42f, 0.74f, 0.92f };
            float[] ws = { 1f, 0.72f, 0.44f, 0.20f };
            for (int i = 0; i < ts.Length; i++)
            {
                float y = h * ts[i];
                float z = h * ts[i] * ts[i] * 0.34f;
                float ww = w * ws[i];
                v.Add(new Vector3(-ww, y, z));
                v.Add(new Vector3( ww, y, z));
                nrm.Add(new Vector3(-Curve, 0f, 1f).normalized);
                nrm.Add(new Vector3( Curve, 0f, 1f).normalized);
            }
            v.Add(new Vector3(0f, h, h * 0.34f));
            nrm.Add(new Vector3(0f, 0.2f, 1f).normalized);

            var tris = new List<int>();
            for (int i = 0; i < ts.Length - 1; i++)
            {
                int a = i * 2;
                tris.AddRange(new[] { a, a + 2, a + 3, a, a + 3, a + 1 });
            }
            int last = (ts.Length - 1) * 2;
            int tipIdx = v.Count - 1;
            tris.AddRange(new[] { last, tipIdx, last + 1 });

            // ★穂。細い茎の先に紡錘を十字で 2 枚。遠景の「草原」はこの縦線が作る。
            //   実体メッシュで出していた穂を消したとき、空に対する縦線が丸ごと無くなった。
            if (seedHead)
            {
                Vector3 top = v[tipIdx];
                float len = h * 0.22f, hw = len * 0.20f;
                for (int q = 0; q < 2; q++)
                {
                    Vector3 sd = q == 0 ? Vector3.right : Vector3.forward;
                    int b0 = v.Count;
                    v.Add(top);
                    v.Add(top + sd * hw + Vector3.up * (len * 0.45f));
                    v.Add(top + Vector3.up * len);
                    v.Add(top - sd * hw + Vector3.up * (len * 0.45f));
                    for (int k = 0; k < 4; k++) nrm.Add(sd == Vector3.right ? Vector3.forward : Vector3.right);
                    tris.AddRange(new[] { b0, b0 + 1, b0 + 2, b0, b0 + 2, b0 + 3 });
                }
            }

            var mesh = new Mesh { name = "GrassBlade" };
            mesh.SetVertices(v);
            mesh.SetNormals(nrm);
            mesh.SetTriangles(tris, 0);
            mesh.RecalculateBounds();
            return mesh;
        }

        private void OnDrawGizmosSelected()
        {
            Gizmos.color = new Color(0.3f, 0.9f, 0.4f, 0.5f);
            Gizmos.DrawWireSphere(transform.position, radius);
            if (innerHole > 0f) Gizmos.DrawWireSphere(transform.position, innerHole);
        }
    }
}
