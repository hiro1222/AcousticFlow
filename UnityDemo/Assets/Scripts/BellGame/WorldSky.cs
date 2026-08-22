// WorldSky.cs
// 世界ごとの「空気」── 空・霧・環境光・太陽。
//
// ★なぜ 1 箇所に集めるか（決めごと #1：同じ問いに 2 つの答えを持たせない）
//   同じ設定を 2 箇所が持つと必ずズレる。ここが唯一の答えで、
//     ・ステージ生成（BellGameStages）が**その世界に居るとき**の見え方を決める
//     ・WorldPortalView が**扉ごしに向こうを描くとき**の見え方を決める
//   の両方が、この 1 つの表を読む。
//   ズレると「扉ごしに見えた世界」と「踏み越えた先の世界」が違って見え、
//   §0 の答え合わせがそのまま壊れる。
//
// ★手触りは「静かだが幻想的、自然を感じる」（発注者決定）。
//   ホラー方向には倒さない。暗さや狭さで不安を作らない。
//   ・空は極端に飽和させない。色は自然界にある範囲で選ぶ
//   ・霧は「怖さ」ではなく**奥行き**のために使う。距離の手がかりになる
//   ・夜の世界も真っ暗にはしない。月と雪の照り返しで形は読める
using UnityEngine;

namespace BellGame
{
    /// 世界 1 つぶんの空気。
    public struct SkySettings
    {
        // ★空の実体は CC0 の HDRI（Poly Haven）。BellGameDressing が Sky_Day / Sky_Night を作る。
        //   2 枚しか無いので、露出と色味で 5 つの世界に振り分ける。
        //   HDRI が増えたらここの skyAsset を差し替えるだけでよい。
        public string skyAsset;          // WorldSkyLibrary が持つ資産の名前
        public Color panoTint;           // Skybox/Panoramic の _Tint（0.5 が素通し）
        public float panoExposure;       // 同 _Exposure

        // HDRI が無いときの手続き生成の空（Skybox/Procedural）。
        public Color skyTint;            // _SkyTint
        public Color groundColor;        // _GroundColor
        public float atmosphere;         // _AtmosphereThickness（大きいほど夕暮れ寄り）
        public float exposure;           // _Exposure

        public Color sunColor;
        public float sunIntensity;
        public Vector3 sunEuler;

        public Color ambientSky;         // 天からの環境光
        public Color ambientEquator;
        public Color ambientGround;

        public bool fog;
        public Color fogColor;
        public float fogDensity;         // Exponential Squared
    }

    /// 空の**資産**をシーンに持たせておく入れ物。
    ///
    /// ★なぜ必要か
    ///   ポータルは実行中に「まだ開いていない世界」の空を描く。
    ///   資産の参照はエディタでしか解決できない（AssetDatabase は実行時に無い）ので、
    ///   ステージ生成のときに全世界ぶんの空をここへ入れておく。
    ///   値（露出・色味・霧・環境光）は WorldSky が持つ。ここが持つのは資産だけ。
    [DisallowMultipleComponent]
    public sealed class WorldSkyLibrary : MonoBehaviour
    {
        [Tooltip("WorldId の番号順。0=None は空のまま。")]
        public Material[] byWorld = new Material[8];

        // ★探して覚える形にしてある。OnEnable で入れるとエディタ実行外で走らず、
        //   ステージ生成のときに空が手続き生成へ落ちてしまう。
        private static WorldSkyLibrary _current;
        public static WorldSkyLibrary Current
        {
            get
            {
                if (_current == null) _current = FindFirstObjectByType<WorldSkyLibrary>();
                return _current;
            }
        }

        private void OnEnable() => _current = this;

        public Material Get(WorldId id)
        {
            int i = (int)id;
            return (byWorld != null && i >= 0 && i < byWorld.Length) ? byWorld[i] : null;
        }
    }

    public static class WorldSky
    {
        public static SkySettings Of(WorldId id)
        {
            switch (id)
            {
                // 草原 ── 昼。基準になる世界なので、いちばん素直な空。
                //   遮蔽も残響も無いところに立たせるので、見た目も開けていてよい。
                case WorldId.Grassland:
                    return new SkySettings
                    {
                        skyAsset = "Sky_Day", panoTint = new Color(0.52f, 0.53f, 0.54f), panoExposure = 1.00f,
                        skyTint = new Color(0.58f, 0.70f, 0.88f),
                        groundColor = new Color(0.42f, 0.45f, 0.35f),
                        atmosphere = 0.85f,
                        exposure = 1.25f,
                        sunColor = new Color(1.00f, 0.96f, 0.88f),
                        sunIntensity = 1.15f,
                        sunEuler = new Vector3(52f, -35f, 0f),
                        ambientSky = new Color(0.62f, 0.72f, 0.86f),
                        ambientEquator = new Color(0.55f, 0.57f, 0.50f),
                        ambientGround = new Color(0.32f, 0.32f, 0.27f),
                        fog = true,
                        fogColor = new Color(0.72f, 0.80f, 0.86f),
                        fogDensity = 0.0035f,
                    };

                // 崩壊都市 ── 曇り。自然に還りつつある廃墟なので、緑を帯びた灰色。
                //   直射を弱めて陰影を潰し、輪郭ではなく**空間の抜け**で読ませる。
                case WorldId.Temple:
                    return new SkySettings
                    {
                        skyAsset = "Sky_Day", panoTint = new Color(0.47f, 0.50f, 0.47f), panoExposure = 0.82f,
                        skyTint = new Color(0.60f, 0.64f, 0.62f),
                        groundColor = new Color(0.34f, 0.36f, 0.31f),
                        atmosphere = 1.35f,
                        exposure = 1.05f,
                        sunColor = new Color(0.92f, 0.94f, 0.90f),
                        sunIntensity = 0.72f,
                        sunEuler = new Vector3(38f, 15f, 0f),
                        ambientSky = new Color(0.56f, 0.60f, 0.58f),
                        ambientEquator = new Color(0.46f, 0.48f, 0.44f),
                        ambientGround = new Color(0.26f, 0.28f, 0.24f),
                        fog = true,
                        fogColor = new Color(0.66f, 0.69f, 0.66f),
                        fogDensity = 0.0090f,
                    };

                // 洞窟 ── 夜。ここだけは霧が濃く、遠くが読めない。
                //   ★ただし真っ暗にはしない。§3 の狙いは「方向が嘘をつく」であって
                //     「見えない」ではない。見えているのに音が合わない、が効く。
                case WorldId.Cave:
                    return new SkySettings
                    {
                        skyAsset = "Sky_Night", panoTint = new Color(0.40f, 0.44f, 0.54f), panoExposure = 1.30f,
                        skyTint = new Color(0.20f, 0.24f, 0.32f),
                        groundColor = new Color(0.10f, 0.10f, 0.12f),
                        atmosphere = 0.55f,
                        exposure = 0.60f,
                        sunColor = new Color(0.62f, 0.72f, 1.00f),
                        sunIntensity = 0.22f,
                        sunEuler = new Vector3(24f, 205f, 0f),
                        ambientSky = new Color(0.16f, 0.19f, 0.26f),
                        ambientEquator = new Color(0.13f, 0.14f, 0.17f),
                        ambientGround = new Color(0.07f, 0.07f, 0.08f),
                        fog = true,
                        fogColor = new Color(0.09f, 0.11f, 0.15f),
                        fogDensity = 0.0300f,
                    };

                // 雪山 ── 夜だが明るい。雪の照り返しで、暗いのに形は読める。
                //   §3 の狙いが「手がかりの喪失」なので、**画は明るく、音だけが痩せる**。
                //   ここを暗くすると「暗いから分からない」になり、音の話でなくなる。
                case WorldId.Snow:
                    return new SkySettings
                    {
                        skyAsset = "Sky_Night", panoTint = new Color(0.52f, 0.56f, 0.66f), panoExposure = 1.95f,
                        skyTint = new Color(0.42f, 0.50f, 0.70f),
                        groundColor = new Color(0.62f, 0.66f, 0.74f),
                        atmosphere = 0.70f,
                        exposure = 0.95f,
                        sunColor = new Color(0.72f, 0.80f, 1.00f),
                        sunIntensity = 0.45f,
                        sunEuler = new Vector3(18f, 200f, 0f),
                        ambientSky = new Color(0.40f, 0.46f, 0.60f),
                        ambientEquator = new Color(0.44f, 0.47f, 0.55f),
                        ambientGround = new Color(0.38f, 0.40f, 0.45f),
                        fog = true,
                        fogColor = new Color(0.56f, 0.60f, 0.70f),
                        fogDensity = 0.0180f,
                    };

                // 海 ── 夜明け。最後の世界。まだ未着手なので暫定。
                case WorldId.Sea:
                    return new SkySettings
                    {
                        skyAsset = "Sky_Day", panoTint = new Color(0.58f, 0.50f, 0.47f), panoExposure = 0.90f,
                        skyTint = new Color(0.72f, 0.62f, 0.60f),
                        groundColor = new Color(0.30f, 0.34f, 0.38f),
                        atmosphere = 1.60f,
                        exposure = 1.10f,
                        sunColor = new Color(1.00f, 0.86f, 0.74f),
                        sunIntensity = 0.85f,
                        sunEuler = new Vector3(6f, 90f, 0f),
                        ambientSky = new Color(0.66f, 0.62f, 0.64f),
                        ambientEquator = new Color(0.52f, 0.52f, 0.54f),
                        ambientGround = new Color(0.28f, 0.30f, 0.32f),
                        fog = true,
                        fogColor = new Color(0.74f, 0.70f, 0.70f),
                        fogDensity = 0.0060f,
                    };

                // 白い部屋 ── プロローグ。**閉じた箱の中なので霧は入れない。**
                //   `BellGameLayout.SetupAirWhite` が置いている値と同じにしてあります
                //  （2 箇所に別々の白を書くと、生成し直すたびに色が変わります）。
                //   ★環境光を白 1 色にしないのも同じ理由 ── 3 色とも白だと壁・床・天井の
                //     境目が消えて、箱の中に居ることが目で分からなくなります。
                case WorldId.White:
                    return new SkySettings
                    {
                        skyAsset = "Sky_White", panoTint = Color.white, panoExposure = 1.00f,
                        skyTint = new Color(0.93f, 0.94f, 0.95f),
                        groundColor = new Color(0.70f, 0.71f, 0.73f),
                        atmosphere = 1.00f,
                        exposure = 1.00f,
                        sunColor = Color.white,
                        sunIntensity = 0.55f,
                        sunEuler = new Vector3(52f, -28f, 0f),
                        ambientSky = new Color(0.93f, 0.94f, 0.95f),
                        ambientEquator = new Color(0.84f, 0.85f, 0.86f),
                        ambientGround = new Color(0.70f, 0.71f, 0.73f),
                        fog = false,
                        fogColor = new Color(0.90f, 0.91f, 0.92f),
                        fogDensity = 0f,
                    };

                default:
                    return Of(WorldId.Grassland);
            }
        }

        /// その世界の空のマテリアルを作る。ポータル用と本編用で同じものを使う。
        ///   ★HDRI があればそれを複製して露出と色味だけ世界ごとに振る。
        ///     無ければ手続き生成の空へ落ちる（資産が無い環境でも動くように）。
        public static Material MakeSkybox(WorldId id, Material hdriSource = null)
        {
            var s = Of(id);

            var src = hdriSource;
            if (src == null && WorldSkyLibrary.Current != null) src = WorldSkyLibrary.Current.Get(id);
            if (src != null)
            {
                var pano = new Material(src) { name = "Sky_" + id };
                if (pano.HasProperty("_Tint")) pano.SetColor("_Tint", s.panoTint);
                if (pano.HasProperty("_Exposure")) pano.SetFloat("_Exposure", s.panoExposure);
                return pano;
            }

            var shader = Shader.Find("Skybox/Procedural");
            if (shader == null) return null;
            var m = new Material(shader) { name = "Sky_" + id };
            m.SetColor("_SkyTint", s.skyTint);
            m.SetColor("_GroundColor", s.groundColor);
            m.SetFloat("_AtmosphereThickness", s.atmosphere);
            m.SetFloat("_Exposure", s.exposure);
            m.SetFloat("_SunSize", 0.035f);
            m.SetFloat("_SunSizeConvergence", 5f);
            return m;
        }

        /// いま居る世界としてグローバルの描画設定を書き換える。
        ///   ★ステージ生成（エディタ）からも、ポータルの一時差し替えからも呼ばれる。
        public static void Apply(WorldId id, Material skybox = null)
        {
            var s = Of(id);
            RenderSettings.skybox = skybox != null ? skybox : MakeSkybox(id);

            // ★環境光は Skybox 由来ではなく Trilight（3 色指定）にする。
            //   Skybox 由来だと HDRI を 2 枚しか持っていない都合で
            //   「洞窟と雪山の環境光が同じ」になってしまう。世界の区別は色で付ける。
            RenderSettings.ambientMode = UnityEngine.Rendering.AmbientMode.Trilight;
            RenderSettings.ambientSkyColor = s.ambientSky;
            RenderSettings.ambientEquatorColor = s.ambientEquator;
            RenderSettings.ambientGroundColor = s.ambientGround;

            RenderSettings.fog = s.fog;
            RenderSettings.fogMode = FogMode.ExponentialSquared;
            RenderSettings.fogColor = s.fogColor;
            RenderSettings.fogDensity = s.fogDensity;
        }

        /// シーン内の平行光を、その世界の太陽に合わせる。
        public static void ApplySun(WorldId id, Light light)
        {
            if (light == null) return;
            var s = Of(id);
            light.type = LightType.Directional;
            light.transform.rotation = Quaternion.Euler(s.sunEuler);
            light.color = s.sunColor;
            light.intensity = s.sunIntensity;
            light.shadows = LightShadows.Soft;
        }
    }
}
