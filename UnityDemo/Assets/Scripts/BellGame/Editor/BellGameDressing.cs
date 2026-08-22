// BellGameDressing.cs (Editor 専用)
// CC0 テクスチャ（ambientCG）と HDRI（Poly Haven）から、Unity のマテリアルを組む。
// メニュー: [BellGame > Assets > ...]
//
// ★音響には一切影響しない。
//   ここが触るのは Renderer と Material だけ。音響が見ているのは BoxCollider なので、
//   見た目をいくら差し替えても RT60 も臨界距離も回折も変わらない。
//
// ★ambientCG は Roughness を配るが、Unity の Standard シェーダは Smoothness を使う。
//   そのまま入れると艶が反転するので、ここで
//     RGB = メタリック(0) / A = Smoothness(= 1 − Roughness)
//   の合成テクスチャを作って _MetallicGlossMap に入れる。これが Unity の標準的な持ち方。
using System.Collections.Generic;
using System.IO;
using UnityEditor;
using UnityEngine;

namespace BellGame.EditorTools
{
    public static class BellGameDressing
    {
        public const string TexRoot = "Assets/Textures/CC0";
        public const string MatRoot = "Assets/Materials/BellGame";

        // マテリアル名 → (テクスチャ組, 1タイルの一辺[m], 色の掛け値)
        private struct Def
        {
            public string set;       // CC0 のフォルダ名
            public float tileMeters; // 何メートルで 1 タイルにするか
            public float tint;       // 明るさの掛け値（屋根を暗くする等）
            public Def(string s, float t, float c = 1f) { set = s; tileMeters = t; tint = c; }
        }

        private static readonly Dictionary<string, Def> kMaterials = new()
        {
            // 神殿
            ["Stone_Wall"] = new Def("Travertine009", 2.0f),
            ["Stone_Column"] = new Def("Marble016", 1.5f),
            ["Stone_Roof"] = new Def("Travertine009", 2.5f, 0.62f),
            ["Temple_Ground"] = new Def("Travertine009", 3.0f, 0.80f),
            // 洞窟
            ["Cave_Rock"] = new Def("Rock063", 3.0f),
            ["Cave_Ground"] = new Def("Rock063", 4.0f, 0.75f),
            // 雪山
            ["Snow_Ground"] = new Def("Snow014", 4.0f),
            ["Snow_Ridge"] = new Def("Snow014", 3.0f, 0.92f),
            ["Snow_Rock"] = new Def("Rock063", 2.5f, 0.70f),
            // 草原
            ["Grass_Ground"] = new Def("Grass005", 2.5f),
        };

        // テクスチャを持たないマテリアル（色・金属感・滑らかさだけ）。
        // 鐘の青銅と木部はちょうどよい CC0 テクスチャをまだ落としていないので、
        // まず色で置く。テクスチャが入ったら kMaterials 側へ移すこと。
        private static readonly Dictionary<string, (Color col, float metallic, float smooth, Color emit)> kPlain = new()
        {
            ["Bell_Bronze"] = (new Color(0.55f, 0.41f, 0.19f), 0.90f, 0.62f, Color.black),
            ["Wood_Beam"] = (new Color(0.34f, 0.24f, 0.16f), 0.00f, 0.22f, Color.black),
            ["Wood_Door"] = (new Color(0.42f, 0.29f, 0.17f), 0.00f, 0.20f, Color.black),
            // 火鉢の炭。自己発光。実際の照明は Point Light 側が出す。
            ["Fire_Coals"] = (new Color(0.32f, 0.09f, 0.03f), 0.00f, 0.30f,
                              new Color(3.0f, 1.05f, 0.22f)),
        };

        // 透過付きの葉のアトラス。Color と Opacity が別ファイルなので、
        // Standard(Cutout) に渡すためにアルファへ合成する（Roughness→Smoothness と同じ手口）。
        //   ★両面は**メッシュ側**で出す（Standard は片面しか描かない）。
        //     造形レーンが葉を両面に出しているので、シェーダ側でやることは無い。
        private static readonly Dictionary<string, (string set, float tileMeters)> kFoliage = new()
        {
            ["Grass_Tall"] = ("Foliage001", 1.0f),   // 長い葉。穂・背の高い株
            ["Grass_Short"] = ("Foliage006", 1.0f),  // 短い葉と反った葉。下草
        };

        [MenuItem("BellGame/Assets/1 マテリアルを作る (CC0 テクスチャから)")]
        public static void BuildMaterials()
        {
            Directory.CreateDirectory(MatRoot);
            var built = new HashSet<string>();

            foreach (var kv in kMaterials)
            {
                var d = kv.Value;
                string dir = $"{TexRoot}/{d.set}";
                if (!Directory.Exists(dir))
                {
                    Debug.LogWarning($"[BellGame] テクスチャが無い: {dir} — {kv.Key} は飛ばす");
                    continue;
                }
                if (built.Add(d.set)) FixImporters(d.set);

                var mat = new Material(Shader.Find("Standard"));
                mat.SetTexture("_MainTex", Load<Texture2D>(dir, d.set, "Color"));

                var nrm = Load<Texture2D>(dir, d.set, "NormalGL");
                if (nrm != null) { mat.SetTexture("_BumpMap", nrm); mat.EnableKeyword("_NORMALMAP"); }

                var ms = BuildMetallicSmoothness(d.set);
                if (ms != null)
                {
                    mat.SetTexture("_MetallicGlossMap", ms);
                    mat.EnableKeyword("_METALLICGLOSSMAP");
                    // Smoothness の出どころを「メタリックマップのアルファ」にする（既定はこれ）。
                    mat.SetFloat("_SmoothnessTextureChannel", 0f);
                }

                var ao = Load<Texture2D>(dir, d.set, "AmbientOcclusion");
                if (ao != null) { mat.SetTexture("_OcclusionMap", ao); mat.SetFloat("_OcclusionStrength", 1f); }

                float t = 1f / Mathf.Max(0.01f, d.tileMeters);
                mat.mainTextureScale = new Vector2(t, t);
                mat.SetTextureScale("_BumpMap", new Vector2(t, t));
                mat.SetTextureScale("_MetallicGlossMap", new Vector2(t, t));
                mat.SetTextureScale("_OcclusionMap", new Vector2(t, t));
                mat.color = new Color(d.tint, d.tint, d.tint, 1f);

                string path = $"{MatRoot}/{kv.Key}.mat";
                AssetDatabase.DeleteAsset(path);
                AssetDatabase.CreateAsset(mat, path);
            }

            // 透過付きの葉（Cutout）。
            foreach (var kv in kFoliage)
            {
                string dir = $"{TexRoot}/{kv.Value.set}";
                if (!Directory.Exists(dir))
                {
                    Debug.LogWarning($"[BellGame] 葉のテクスチャが無い: {dir} — {kv.Key} は飛ばす");
                    continue;
                }
                FixFoliageImporters(kv.Value.set);
                var albedo = BuildFoliageAlbedo(kv.Value.set);
                if (albedo == null) continue;

                var mat = new Material(Shader.Find("Standard"));
                // Standard を Cutout に切り替える。_Mode だけでは効かないので、
                // キーワードと RenderType と描画順を全部揃える必要がある。
                mat.SetFloat("_Mode", 1f);
                mat.SetOverrideTag("RenderType", "TransparentCutout");
                mat.EnableKeyword("_ALPHATEST_ON");
                mat.DisableKeyword("_ALPHABLEND_ON");
                mat.DisableKeyword("_ALPHAPREMULTIPLY_ON");
                mat.renderQueue = (int)UnityEngine.Rendering.RenderQueue.AlphaTest;
                mat.SetFloat("_Cutoff", 0.45f);

                mat.SetTexture("_MainTex", albedo);
                var fn = Load1K<Texture2D>(dir, kv.Value.set, "NormalGL");
                if (fn != null) { mat.SetTexture("_BumpMap", fn); mat.EnableKeyword("_NORMALMAP"); }
                mat.SetFloat("_Metallic", 0f);
                mat.SetFloat("_Glossiness", 0.25f);

                float ft = 1f / Mathf.Max(0.01f, kv.Value.tileMeters);
                mat.mainTextureScale = new Vector2(ft, ft);

                string fp = $"{MatRoot}/{kv.Key}.mat";
                AssetDatabase.DeleteAsset(fp);
                AssetDatabase.CreateAsset(mat, fp);
            }

            // テクスチャ無しのもの（鐘・木部）。
            foreach (var kv in kPlain)
            {
                var mat = new Material(Shader.Find("Standard"))
                {
                    color = kv.Value.col,
                };
                mat.SetFloat("_Metallic", kv.Value.metallic);
                mat.SetFloat("_Glossiness", kv.Value.smooth);
                if (kv.Value.emit.maxColorComponent > 0.001f)
                {
                    mat.EnableKeyword("_EMISSION");
                    mat.SetColor("_EmissionColor", kv.Value.emit);
                    mat.globalIlluminationFlags = MaterialGlobalIlluminationFlags.RealtimeEmissive;
                }
                string path = $"{MatRoot}/{kv.Key}.mat";
                AssetDatabase.DeleteAsset(path);
                AssetDatabase.CreateAsset(mat, path);
            }

            AssetDatabase.SaveAssets();
            AssetDatabase.Refresh();
            Debug.Log($"[BellGame] マテリアル {kMaterials.Count + kPlain.Count} 個を {MatRoot} に作成。" +
                      "Roughness は Smoothness へ反転して _MetallicGlossMap のアルファに入れてある。");
        }

        [MenuItem("BellGame/Assets/2 スカイボックスを作る (HDRI から)")]
        public static void BuildSkyboxes()
        {
            Directory.CreateDirectory(MatRoot);
            var shader = Shader.Find("Skybox/Panoramic");
            if (shader == null) { Debug.LogError("[BellGame] Skybox/Panoramic が見つからない"); return; }

            foreach (var (file, name, exposure) in new[]
            {
                ("kloofendal_48d_partly_cloudy_puresky_2k", "Sky_Day", 1.0f),
                ("moonless_golf_2k", "Sky_Night", 1.6f),
            })
            {
                string tp = $"{TexRoot}/HDRI/{file}.hdr";
                var tex = AssetDatabase.LoadAssetAtPath<Texture>(tp);
                if (tex == null) { Debug.LogWarning($"[BellGame] HDRI が無い: {tp}"); continue; }

                var mat = new Material(shader);
                mat.SetTexture("_MainTex", tex);
                mat.SetFloat("_Mapping", 1f);      // Latitude Longitude Layout
                mat.SetFloat("_Exposure", exposure);
                string path = $"{MatRoot}/{name}.mat";
                AssetDatabase.DeleteAsset(path);
                AssetDatabase.CreateAsset(mat, path);
            }
            AssetDatabase.SaveAssets();
            Debug.Log("[BellGame] スカイボックス Sky_Day / Sky_Night を作成。");
        }

        // ── ステージ生成側から使う ──────────────────────────────

        public static Material Load(string name)
            => AssetDatabase.LoadAssetAtPath<Material>($"{MatRoot}/{name}.mat");

        /// マテリアルがあれば貼る。無ければ false（呼び側が Tint に落とす）。
        public static bool Apply(Transform t, string materialName)
        {
            var mat = Load(materialName);
            if (mat == null) return false;
            var r = t.GetComponent<Renderer>();
            if (r == null) return false;
            r.sharedMaterial = mat;
            return true;
        }

        public static void SetSkybox(string name)
        {
            var mat = Load(name);
            if (mat == null) return;
            RenderSettings.skybox = mat;
            RenderSettings.ambientMode = UnityEngine.Rendering.AmbientMode.Skybox;
            DynamicGI.UpdateEnvironment();
        }

        // ── 中身 ──────────────────────────────────────────────

        private static T Load<T>(string dir, string set, string map) where T : Object
            => AssetDatabase.LoadAssetAtPath<T>($"{dir}/{set}_2K-JPG_{map}.jpg");

        private static T Load1K<T>(string dir, string set, string map) where T : Object
            => AssetDatabase.LoadAssetAtPath<T>($"{dir}/{set}_1K-JPG_{map}.jpg");

        // 葉のアトラスの取り込み設定。Color と Opacity は画素を読むので読み取り可にする。
        private static void FixFoliageImporters(string set)
        {
            string dir = $"{TexRoot}/{set}";
            foreach (var (map, isNormal, srgb, readable) in new[]
            {
                ("Color", false, true, true),
                ("Opacity", false, false, true),
                ("NormalGL", true, false, false),
                ("Roughness", false, false, false),
            })
            {
                string p = $"{dir}/{set}_1K-JPG_{map}.jpg";
                var imp = AssetImporter.GetAtPath(p) as TextureImporter;
                if (imp == null) continue;
                bool dirty = false;
                var want = isNormal ? TextureImporterType.NormalMap : TextureImporterType.Default;
                if (imp.textureType != want) { imp.textureType = want; dirty = true; }
                if (imp.sRGBTexture != srgb) { imp.sRGBTexture = srgb; dirty = true; }
                if (readable && !imp.isReadable) { imp.isReadable = true; dirty = true; }
                if (dirty) imp.SaveAndReimport();
            }
        }

        // Color(RGB) + Opacity(R) → RGBA の PNG。Standard(Cutout) はアルファしか見ないので、
        // 別ファイルのままでは葉の形が抜けない（板が四角いまま出る）。
        private static Texture2D BuildFoliageAlbedo(string set)
        {
            string dir = $"{TexRoot}/{set}";
            string outPath = $"{dir}/{set}_AlbedoAlpha.png";
            var existing = AssetDatabase.LoadAssetAtPath<Texture2D>(outPath);
            if (existing != null) return existing;

            var col = Load1K<Texture2D>(dir, set, "Color");
            var opa = Load1K<Texture2D>(dir, set, "Opacity");
            if (col == null || opa == null)
            {
                Debug.LogWarning($"[BellGame] {set}: Color か Opacity が読めない");
                return null;
            }
            if (col.width != opa.width || col.height != opa.height)
            {
                Debug.LogWarning($"[BellGame] {set}: Color と Opacity の寸法が違う"
                                 + $"（{col.width}x{col.height} vs {opa.width}x{opa.height}）");
                return null;
            }

            var c = col.GetPixels32();
            var o = opa.GetPixels32();
            for (int i = 0; i < c.Length; i++) c[i].a = o[i].r;   // Opacity はグレースケール

            var tex = new Texture2D(col.width, col.height, TextureFormat.RGBA32, false);
            tex.SetPixels32(c);
            tex.Apply();
            File.WriteAllBytes(outPath, tex.EncodeToPNG());
            Object.DestroyImmediate(tex);
            AssetDatabase.ImportAsset(outPath);

            var imp = AssetImporter.GetAtPath(outPath) as TextureImporter;
            if (imp != null)
            {
                imp.sRGBTexture = true;
                imp.alphaSource = TextureImporterAlphaSource.FromInput;
                imp.alphaIsTransparency = true;
                imp.SaveAndReimport();
            }
            return AssetDatabase.LoadAssetAtPath<Texture2D>(outPath);
        }

        // 取り込み設定を直す。法線は NormalMap、リニアなデータ（粗さ・AO・変位）は sRGB を切る。
        private static void FixImporters(string set)
        {
            string dir = $"{TexRoot}/{set}";
            foreach (var (map, isNormal, srgb) in new[]
            {
                ("Color", false, true),
                ("NormalGL", true, false),
                ("Roughness", false, false),
                ("AmbientOcclusion", false, false),
                ("Displacement", false, false),
            })
            {
                string p = $"{dir}/{set}_2K-JPG_{map}.jpg";
                var imp = AssetImporter.GetAtPath(p) as TextureImporter;
                if (imp == null) continue;
                bool dirty = false;
                var want = isNormal ? TextureImporterType.NormalMap : TextureImporterType.Default;
                if (imp.textureType != want) { imp.textureType = want; dirty = true; }
                if (imp.sRGBTexture != srgb) { imp.sRGBTexture = srgb; dirty = true; }
                // Roughness は画素を読むので読み取り可にする。
                bool needRead = map == "Roughness";
                if (imp.isReadable != needRead && needRead) { imp.isReadable = true; dirty = true; }
                if (dirty) { imp.SaveAndReimport(); }
            }
        }

        // Roughness → MetallicSmoothness（RGB=0 / A=1−Roughness）。
        // 既に作ってあれば作り直さない。
        private static Texture2D BuildMetallicSmoothness(string set)
        {
            string dir = $"{TexRoot}/{set}";
            string outPath = $"{dir}/{set}_MetallicSmoothness.png";
            var existing = AssetDatabase.LoadAssetAtPath<Texture2D>(outPath);
            if (existing != null) return existing;

            var rough = Load<Texture2D>(dir, set, "Roughness");
            if (rough == null) return null;

            var src = rough.GetPixels32();
            var dst = new Color32[src.Length];
            for (int i = 0; i < src.Length; i++)
            {
                // Roughness はグレースケール。R を使い、反転して Smoothness にする。
                byte s = (byte)(255 - src[i].r);
                dst[i] = new Color32(0, 0, 0, s);   // メタリックは 0（石・雪・草はすべて非金属）
            }
            var tex = new Texture2D(rough.width, rough.height, TextureFormat.RGBA32, false, true);
            tex.SetPixels32(dst);
            tex.Apply();
            File.WriteAllBytes(outPath, tex.EncodeToPNG());
            Object.DestroyImmediate(tex);
            AssetDatabase.ImportAsset(outPath);

            var imp = AssetImporter.GetAtPath(outPath) as TextureImporter;
            if (imp != null)
            {
                imp.sRGBTexture = false;             // リニアなデータ
                imp.alphaIsTransparency = false;
                imp.alphaSource = TextureImporterAlphaSource.FromInput;
                imp.SaveAndReimport();
            }
            return AssetDatabase.LoadAssetAtPath<Texture2D>(outPath);
        }
    }
}
