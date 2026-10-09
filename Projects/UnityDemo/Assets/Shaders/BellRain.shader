// BellRain.shader
// 雨の筋。**テクスチャを使わない。**
//
// ★粒子ではなく、カメラを包む筒に描く。
//   粒子だと数万個の描画になるうえ、間引くと「雨が薄くなる」のではなく
//   「粒がまばらになる」に見える。筒なら 2 枚で密度が出せて、
//   遠近も筒の半径で作れる（近い筒は速く太く、遠い筒は遅く細く）。
//
// ★崩壊都市で雨が要る理由は演出ではない。
//   **屋根の有無を鳴らし続ける**ため（WORLD_VISUAL §3-2 の芯）。
//   屋根の下では雨音がくぐもり、屋根の落ちた建物では降り込む。
//   だから**屋根の下では筋も消えなければならない** ── そこは `RainVolume` が
//   真上へレイを撃って `_Cover` に入れる。シェーダは受け取るだけ。
Shader "BellGame/Rain"
{
    Properties
    {
        _Color      ("筋の色", Color) = (0.78, 0.84, 0.90, 1)
        _Amount     ("量", Range(0, 1)) = 0.55
        _Cover      ("覆われ具合(1=屋根の下)", Range(0, 1)) = 0
        _Columns    ("横の本数", Range(20, 400)) = 150
        _Speed      ("落ちる速さ", Range(0.2, 8)) = 2.6
        _Length     ("筋の長さ", Range(0.02, 0.6)) = 0.16
        _Sharp      ("筋の締まり", Range(0.01, 0.5)) = 0.09
        _Slant      ("斜めに降る量", Range(-0.5, 0.5)) = 0.12
    }

    SubShader
    {
        Tags { "Queue" = "Transparent" "RenderType" = "Transparent"
               "IgnoreProjector" = "True" }
        // ★ZTest Always。カメラの直前に置く板なので、壁際に立つと
        //   壁に切られて雨が消える。画面に貼る雨なので深度は見ない。
        Cull Off ZWrite Off ZTest Always Blend SrcAlpha OneMinusSrcAlpha

        Pass
        {
            CGPROGRAM
            #pragma vertex vert
            #pragma fragment frag
            #include "UnityCG.cginc"

            half4 _Color;
            float _Amount, _Cover, _Columns, _Speed, _Length, _Sharp, _Slant;

            struct appdata { float4 vertex : POSITION; float2 uv : TEXCOORD0; };
            struct v2f { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

            v2f vert(appdata v)
            {
                v2f o;
                o.pos = UnityObjectToClipPos(v.vertex);
                o.uv = v.uv;
                return o;
            }

            float RainHash(float n) { return frac(sin(n * 78.233) * 43758.5453); }

            half4 frag(v2f i) : SV_Target
            {
                float a = _Amount * (1.0 - saturate(_Cover));
                if (a < 0.002) discard;

                // 横に列を切る。列ごとに落ちる位相と速さを散らす
                //   ── 揃っていると簾（すだれ）に見えて雨にならない。
                float cx = i.uv.x * _Columns;
                float col = floor(cx);
                float within = frac(cx);

                float seed = RainHash(col);
                float seed2 = RainHash(col + 91.7);
                // 列の中でも筋を細くする。太いと水の膜になる。
                float thin = smoothstep(0.5, 0.5 - _Sharp * 2.0, abs(within - 0.5));

                // 斜めに降らせる。真下だと止まって見える。
                float y = i.uv.y + i.uv.x * _Slant;
                float t = y - _Time.y * _Speed * (0.75 + seed2 * 0.6) - seed;
                float f = frac(t);
                // 筋 1 本。頭を締め、尾を引かせる。
                float streak = smoothstep(_Length, _Length - _Sharp, f) * smoothstep(0.0, _Sharp, f);

                // ★上下で消す。筒の縁が見えると「板が回っている」と分かってしまう。
                float fade = smoothstep(0.0, 0.18, i.uv.y) * smoothstep(1.0, 0.82, i.uv.y);
                // 列の 3 割は休ませる。全列に降ると密度が均一で嘘になる。
                float live = step(0.30, seed);

                float alpha = streak * thin * fade * live * a;
                return half4(_Color.rgb, alpha);
            }
            ENDCG
        }
    }
    Fallback Off
}
