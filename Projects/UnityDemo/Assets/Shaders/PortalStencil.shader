// PortalStencil.shader
// 扉の開口を**ステンシルで切り抜き**、そこにだけ行き先の絵を貼る。Built-in RP。
//
// ★印の付け方は 1 つの規則だけ。
//
//     ある画素が向こう側か ＝ **目からその画素へ引いた線分が、扉の矩形を横切っているか**
//
//   これは「カメラと扉の面の関係」そのもので、**連続**している。
//     ・扉から離れている  → 開口の形に切り抜かれる
//     ・面の直前で前を向く → ほぼ全画面が向こう
//     ・面の直後で前を向く → 線分が矩形を横切らないので全画面がこちら
//     ・面に重なって真横  → 画面が扉の面で左右に割れる
//   途中に段が無い。場合分けも閾値も無い。
//
// ★ここへ来るまでに 3 つ試して、3 つとも同じ穴で破れた（記録として残す）：
//     ① 開口の矩形を描いて印にする
//        → 目が矩形に入ると near 平面に潰れ、印がほとんど付かない
//     ② 戸口の中では画面全体を印にする（①の穴埋め）
//        → 跨いだ後も全画面が続き、**出てきたばかりの世界が全画面に出る**。
//          範囲を詰めても段は残る（発注者の「連続した切り替えが一番の違和感」）
//     ③ 開口を前後に厚みのある箱にする
//        → 潰れはしないが、箱の中で遮蔽が緩む。真横の割れ方も作れない
//   どれも「**矩形を描いて印にする**」ことが根で、目が矩形に入ると破れる。
//   線分と矩形の交差を直に見れば、その前提ごと消える。
//
// ★扉板の穴あけも要らなくなった。
//   奥へ開いた板の画素は、線分が**面を横切る前に**板に当たるので、
//   交差の条件（0 < t < 画素までの距離）から自動的に外れる。
//   規則が 1 つになると、例外も一緒に消える。
Shader "BellGame/PortalStencil"
{
    Properties
    {
        _PortalTex ("行き先の絵", 2D) = "black" {}
    }

    SubShader
    {
        Tags { "RenderType" = "Opaque" }

        // ── 1. 線分が扉の矩形を横切る画素に印を付ける ──────────
        Pass
        {
            Name "MaskRay"
            Cull Off
            ZWrite Off
            ZTest Always          // 深度は自分で読む。パイプラインの比較には任せない
            ColorMask 0

            Stencil
            {
                Ref 1
                Comp Always
                Pass Replace
            }

            CGPROGRAM
            #pragma vertex vert
            #pragma fragment frag
            #include "UnityCG.cginc"

            sampler2D _CameraDepthTexture;

            // 扉の矩形（世界座標）
            float4 _DoorPos;      // 中心
            float4 _DoorNormal;   // 面の法線（forward）
            float4 _DoorRight;    // 横（正規化）
            float4 _DoorUp;       // 縦（正規化）
            float4 _DoorHalf;     // (半幅, 半高)

            // 視線を組み立てる基底。逆投影行列より取り違えが起きない。
            float4 _CamRight, _CamUp, _CamFwd, _TanHalf;

            struct v2f
            {
                float4 pos    : SV_POSITION;
                float4 screen : TEXCOORD0;   // 深度も視線も**ここから**出す
            };

            // 画面全体を覆う三角形。DrawProcedural(3頂点) で呼ぶこと。
            v2f vert(uint id : SV_VertexID)
            {
                v2f o;
                float2 uv = float2((id << 1) & 2, id & 2);
                float2 ndc = uv * 2.0 - 1.0;
                o.pos = float4(ndc, UNITY_NEAR_CLIP_VALUE, 1.0);

                // ★視線と深度を、**同じ 1 つの値から**出す。
                //
                //   ここが分かれていたのが「印の上下が鏡になる」の正体だった。
                //   視線は頂点のクリップ座標（ndc）から、深度は画面座標から出していて、
                //   上下の向きが逆だった ── 扉が画面の中央にあるときだけ偶然合い、
                //   上下を向くとずれ、扉が中心の反対側へ行くと印が消える。
                //   （発注者の 3 枚のスクリーンショットでちょうど鏡だと確定した。）
                //
                //   ComputeScreenPos は、この上下の食い違いを吸収する Unity の道具。
                //   ★視線も**これ**から出す。頂点の ndc から作ると、
                //     深度だけ吸収されて視線は吸収されないので、片方だけ反転が残る。
                //     実測で「印は扉の、画面中心に対する上下の鏡」と確定した
                //    （下を向いたとき 扉 −217〜−67 に対し 印 +63〜+213）。
                o.screen = ComputeScreenPos(o.pos);
                return o;
            }

            // ★1 回で決着させるための切り分け。
            //
            //   1 にすると**深度を一切読まない**（全部の画素が空＝遠平面にあると見なす）。
            //   すると印は「扉の開口の輪郭」そのものになるはずで、
            //   上下左右どこを向いても開口に貼り付いていなければならない。
            //
            //     ・切り分け中は合う   → 視線の組み立ては正しい。原因は**深度側**
            //     ・切り分け中もずれる → 原因は**視線側**（基底か画角）
            //
            //   反転の有無は 2 通り試して同じ結果だったので、もう推測しない。
            float _IgnoreDepth;

            fixed4 frag(v2f i) : SV_Target
            {
                // その画素に写っている物までの距離（目からの奥行き）。
                float raw = SAMPLE_DEPTH_TEXTURE_PROJ(_CameraDepthTexture,
                                                      UNITY_PROJ_COORD(i.screen));
                if (_IgnoreDepth > 0.5) raw = UNITY_REVERSED_Z ? 0.0 : 1.0;   // 全部を空扱い
                float eyeZ = LinearEyeDepth(raw);
                eyeZ = min(eyeZ, _ProjectionParams.z);      // 空は遠平面まで

                // ★視線は**深度を引いたのと同じ座標**から組み立てる。
                //   ここを頂点の ndc から作っていたのが、上下が鏡になった原因。
                float2 suv = i.screen.xy / i.screen.w;      // 左下 (0,0) / 右上 (1,1)
                float2 ndcRay = suv * 2.0 - 1.0;

                float3 E = _WorldSpaceCameraPos;

                // ★前方成分をちょうど 1 にしておく。
                //   そうすると「視線 × 目からの奥行き」が、そのまま世界座標になる。
                float3 v = _CamFwd.xyz
                         + _CamRight.xyz * ndcRay.x * _TanHalf.x
                         + _CamUp.xyz    * ndcRay.y * _TanHalf.y;
                float3 P = E + v * eyeZ;                     // 画素の世界座標

                float3 dir = normalize(v);
                float dist = length(v) * eyeZ;               // 目から画素までの距離

                // 目 → 画素 の線分が扉の面を横切る位置。
                float denom = dot(dir, _DoorNormal.xyz);
                if (abs(denom) < 1e-5) discard;              // 面と平行。横切らない

                float t = dot(_DoorPos.xyz - E, _DoorNormal.xyz) / denom;

                // ★線分の**内側**で横切ること。
                //   t <= 0   … 面は目より後ろ（振り返っている）
                //   t >= 距離 … 手前に何かが写り込んでいる（扉板・柱・壁）
                //               → 扉板の穴あけが要らないのはここ
                if (t <= 0.0 || t >= dist) discard;

                // 横切った点が開口の矩形の中にあるか。
                float3 X = E + dir * t - _DoorPos.xyz;
                float2 uvDoor = float2(dot(X, _DoorRight.xyz), dot(X, _DoorUp.xyz));
                if (abs(uvDoor.x) > _DoorHalf.x || abs(uvDoor.y) > _DoorHalf.y) discard;

                return 0;    // 生き残った画素にだけ印が付く
            }
            ENDCG
        }

        // ── 2. 扉板のところだけ印を消す ────────────────────────
        //
        // ★一度「線分の規則から自動的に外れる」と考えて消し、**戻した**。逆だった。
        //
        //     板が面より**手前**（こちらへ開く）
        //       → 線分は板に当たってから面へ届く。t ≥ 距離 で弾かれる ✓ 板が見える
        //     板が面より**奥**（奥へ開く）
        //       → 線分は**面を横切った後**に板へ当たる。t < 距離 なので印が付く
        //         ✗ 板が行き先の絵に塗り潰されて消える
        //
        //   奥へ開いた板は規則では守れない。ここで明示的に外す。
        //
        // ★色は触らない。印を 0 に戻すだけ。
        //   本編カメラが既に描いた板がそのまま残るので、板のマテリアルには触らずに済む。
        Pass
        {
            Name "Punch"
            Cull Off
            ZWrite Off
            ZTest Always          // 板が面の手前でも奥でも、同じように印を消す
            ColorMask 0

            Stencil
            {
                Ref 0
                Comp Always
                Pass Replace
            }

            CGPROGRAM
            #pragma vertex vert
            #pragma fragment frag
            #include "UnityCG.cginc"

            float4 vert(float4 v : POSITION) : SV_POSITION { return UnityObjectToClipPos(v); }
            fixed4 frag() : SV_Target { return 0; }
            ENDCG
        }

        // ── 3. 印の中に行き先の絵を貼る ────────────────────────
        Pass
        {
            Name "Composite"
            Cull Off
            ZWrite Off
            ZTest Always
            ColorMask RGB

            Stencil
            {
                Ref 1
                Comp Equal
                Pass Keep
            }

            CGPROGRAM
            #pragma vertex vert
            #pragma fragment frag
            #include "UnityCG.cginc"

            sampler2D _PortalTex;
            float _SolidMask;     // 1 で行き先の絵ではなくマゼンタを貼る（切り分け用）

            struct v2f
            {
                float4 pos    : SV_POSITION;
                float4 screen : TEXCOORD0;
            };

            v2f vert(uint id : SV_VertexID)
            {
                v2f o;
                float2 uv = float2((id << 1) & 2, id & 2);
                o.pos = float4(uv * 2.0 - 1.0, UNITY_NEAR_CLIP_VALUE, 1.0);
                o.screen = ComputeScreenPos(o.pos);
                return o;
            }

            fixed4 frag(v2f i) : SV_Target
            {
                if (_SolidMask > 0.5) return fixed4(1, 0, 1, 1);
                // ★印を付けたときと**同じ引き方**で貼る。
                //   片方だけ別の出どころにすると、必ずそのぶんずれる。
                return tex2Dproj(_PortalTex, UNITY_PROJ_COORD(i.screen));
            }
            ENDCG
        }
    }
    Fallback Off
}
