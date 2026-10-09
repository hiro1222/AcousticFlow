// BellToonSky.shader
// アニメ調の空。**テクスチャを使わない。**
//
// ★写真の HDRI（Poly Haven）から乗り換える理由
//   地上が `BellGame/Toon` の段の付いた陰影なのに、空だけ写真だと質感が食い違う。
//   遠景を霧で空の色へ寄せているので、**空が写真だと地平で継ぎ目が出る**。
//
// ★アニメの空は「滑らかなグラデーション ＋ 縁の立った雲」。
//   だから空そのものには段を付けない（既定 _Steps=0）。セル調は**雲が担う。**
//
// ★地平の色は**霧の色と同じ値にすること。**
//   ずれると、遠景が霧で空へ溶け切る手前に色の段ができる。
//   BellGameLayout / WorldSky が同じ 1 つの値から両方へ入れる。
//
// ★雲は「雲の板」への投影で作る。方向ベクトルをそのまま使うと、
//   天頂も地平も同じ大きさの雲になって**遠近が出ない**。
//   dir.xz / dir.y で高さ一定の板に落とすと、地平へ行くほど詰まって遠く見える。
Shader "BellGame/ToonSky"
{
    Properties
    {
        _ZenithColor  ("天頂の色", Color) = (0.29, 0.52, 0.78, 1)
        _HorizonColor ("地平の色（★霧と同じ値）", Color) = (0.78, 0.86, 0.89, 1)
        _GroundColor  ("地平より下", Color) = (0.42, 0.47, 0.50, 1)
        _HorizonFalloff ("地平のぼかし", Range(0.4, 8)) = 2.2
        _Steps        ("空の段数(0=段なし)", Range(0, 12)) = 0

        _SunDir       ("太陽の向き(xyz)", Vector) = (0.4, 0.55, -0.73, 0)
        _SunColor     ("太陽の色", Color) = (1, 0.97, 0.88, 1)
        _SunSize      ("太陽の大きさ", Range(0.0, 0.06)) = 0.012
        _SunGlow      ("光の広がり", Range(0, 1)) = 0.35
        _SunGlowPower ("広がりの締まり", Range(2, 200)) = 26

        _CloudColor   ("雲の明るい面", Color) = (1, 1, 1, 1)
        _CloudShade   ("雲の影の面", Color) = (0.72, 0.78, 0.85, 1)
        _CloudAmount  ("雲の量(0=晴天)", Range(0, 1)) = 0.55
        _CloudScale   ("雲の細かさ", Range(0.05, 3)) = 0.55
        _CloudSharp   ("雲の縁の立ち方", Range(0.005, 0.4)) = 0.06
        _CloudHeight  ("雲の板の高さ", Range(0.2, 6)) = 1.6
        _CloudSpeed   ("流れる速さ", Range(0, 0.2)) = 0.012
    }

    SubShader
    {
        Tags { "Queue" = "Background" "RenderType" = "Background"
               "PreviewType" = "Skybox" "IgnoreProjector" = "True" }
        Cull Off ZWrite Off

        Pass
        {
            CGPROGRAM
            #pragma vertex vert
            #pragma fragment frag
            #include "UnityCG.cginc"

            half4 _ZenithColor, _HorizonColor, _GroundColor, _SunColor, _CloudColor, _CloudShade;
            float4 _SunDir;
            float _HorizonFalloff, _Steps;
            float _SunSize, _SunGlow, _SunGlowPower;
            float _CloudAmount, _CloudScale, _CloudSharp, _CloudHeight, _CloudSpeed;

            struct appdata { float4 vertex : POSITION; UNITY_VERTEX_INPUT_INSTANCE_ID };
            struct v2f { float4 pos : SV_POSITION; float3 dir : TEXCOORD0; };

            v2f vert(appdata v)
            {
                v2f o;
                UNITY_SETUP_INSTANCE_ID(v);
                o.pos = UnityObjectToClipPos(v.vertex);
                o.dir = v.vertex.xyz;      // スカイボックスの立方体の頂点＝方向
                return o;
            }

            // 値ノイズ。`BellToon` と同じ式にしてある（雲と地面のムラが同じ肌になる）。
            float SkyHash(float2 p)
            {
                p = frac(p * float2(123.34, 456.21));
                p += dot(p, p + 45.32);
                return frac(p.x * p.y);
            }
            float SkyNoise(float2 p)
            {
                float2 i = floor(p), f = frac(p);
                f = f * f * (3.0 - 2.0 * f);
                float a = SkyHash(i), b = SkyHash(i + float2(1, 0));
                float c = SkyHash(i + float2(0, 1)), d = SkyHash(i + float2(1, 1));
                return lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y);
            }
            float SkyFbm(float2 p)
            {
                return SkyNoise(p) * 0.52 + SkyNoise(p * 2.1) * 0.28
                     + SkyNoise(p * 4.3) * 0.14 + SkyNoise(p * 8.7) * 0.06;
            }

            half4 frag(v2f i) : SV_Target
            {
                float3 dir = normalize(i.dir);

                // ── 下地のグラデーション ──────────────────────
                //   ★空には段を付けない（既定）。付けると帯が輪になって出て、
                //     アニメの空ではなく**等高線**に見える。段は雲で出す。
                float up = saturate(dir.y);
                float t = pow(up, 1.0 / max(_HorizonFalloff, 0.01));
                if (_Steps >= 1.0) t = floor(t * _Steps) / _Steps;
                half3 col = lerp(_HorizonColor.rgb, _ZenithColor.rgb, t);
                // 地平より下。地面より遠い所は描かないので、実際はほぼ見えない保険。
                col = lerp(col, _GroundColor.rgb, saturate(-dir.y * 6.0));

                // ── 太陽 ──────────────────────────────────────
                //   ★見た目の太陽をここに置いておくと、後で光芒（god ray）を足すとき
                //     画面上の位置が既に合っている。
                float3 sd = normalize(_SunDir.xyz);
                float d = dot(dir, sd);
                float glow = pow(saturate(d), _SunGlowPower) * _SunGlow;
                float disc = smoothstep(1.0 - _SunSize, 1.0 - _SunSize * 0.35, d);
                col += _SunColor.rgb * (glow + disc * 1.6);

                // ── 雲 ────────────────────────────────────────
                if (_CloudAmount > 0.001 && dir.y > 0.005)
                {
                    // 高さ一定の板へ落とす。地平へ行くほど詰まって遠く見える。
                    float2 p = dir.xz / dir.y * _CloudHeight * _CloudScale;
                    p += _Time.y * _CloudSpeed * float2(1.0, 0.35);

                    float n = SkyFbm(p);
                    // ★縁を立てる。これがアニメの雲。段を細かくすると綿になる。
                    float thr = lerp(0.72, 0.34, _CloudAmount);
                    float a = smoothstep(thr, thr + _CloudSharp, n);

                    // 影の面。少しずらした値との差で、雲の下側だけ暗くする。
                    float n2 = SkyFbm(p + sd.xz * 0.35);
                    half3 cc = lerp(_CloudShade.rgb, _CloudColor.rgb, saturate((n2 - n) * 6.0 + 0.5));

                    // 地平では霧に溶かす。溶かさないと地平線に雲が貼り付く。
                    a *= smoothstep(0.0, 0.14, dir.y);
                    col = lerp(col, cc, a);
                }

                return half4(col, 1);
            }
            ENDCG
        }
    }
    Fallback Off
}
