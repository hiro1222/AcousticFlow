// WorldPortal.shader
// 扉の開口に張る板。RenderTexture を**スクリーン座標で**サンプルする。
//
// ★ここが窓に見えるかテレビに見えるかの分かれ目。
//   板の UV で貼ると、頭を動かしても絵が板に貼り付いたままになり「画面」に見える。
//   スクリーン座標で引くと、ポータルのカメラが描いた画がそのまま窓の向こうに揃う。
Shader "BellGame/WorldPortal"
{
    Properties
    {
        _MainTex ("Portal", 2D) = "black" {}
    }
    SubShader
    {
        Tags { "RenderType" = "Opaque" "Queue" = "Geometry" }
        // ★両面描く。
        //   片面にすると、板の巻き方がひとつ逆なだけで**丸ごと消える**。
        //   しかも「描画中」と報告されたまま消えるので、原因に辿り着けない
        //   （実際にそうなった）。窓に裏表は要らないので、ここは両面でよい。
        Cull Off
        ZWrite On
        Fog { Mode Off }        // 向こうの霧はポータル側で既に焼かれている

        Pass
        {
            CGPROGRAM
            #pragma vertex vert
            #pragma fragment frag
            #include "UnityCG.cginc"

            struct appdata { float4 vertex : POSITION; };
            struct v2f
            {
                float4 pos    : SV_POSITION;
                float4 screen : TEXCOORD0;
            };

            sampler2D _MainTex;

            v2f vert (appdata v)
            {
                v2f o;
                o.pos = UnityObjectToClipPos(v.vertex);
                o.screen = ComputeScreenPos(o.pos);
                return o;
            }

            fixed4 frag (v2f i) : SV_Target
            {
                float2 uv = i.screen.xy / max(i.screen.w, 1e-5);
                return tex2D(_MainTex, uv);
            }
            ENDCG
        }
    }
    FallBack Off
}
