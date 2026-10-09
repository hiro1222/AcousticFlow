// BellToon.shader
// この作品の**唯一の不透明サーフェス用シェーダ**。Built-in RP。
//
// ★狙い: 同じシーンに 2 つのライティングモデルを置かない。
//   草だけセル調・地面は PBR、という状態を作ってしまったので 1 枚に寄せた。
//   地面も門も木も草も葉も、全部これを使う。違うのは**マテリアルの設定だけ**。
//
//   | 使い方 | _Cull | _WindAmp | _WholeSway | インスタンシング |
//   |---|---|---|---|---|
//   | 地面・地形・石・木の幹 | Back(2) | 0 | 0 | 任意 |
//   | 草（GPU インスタンシング） | Off(0) | 0.10 | 0 | 有効 |
//   | 木の葉（同上） | Off(0) | 0.055 | 1 | 有効 |
//
// ★描画だけ。音響には一切関わらない（コライダーを持たないものに貼る前提）。
//
// セル調の要点:
//   1. 拡散光を _Steps 段に量子化して、境界を硬くする
//   2. 影側を真っ黒にせず _ShadeFloor で持ち上げる（WORLD_SETTING §6-4「夜も真っ暗にしない」）
//   3. リムライトで輪郭を起こす。背景から抜けて、平坦な面でも形が読める
Shader "BellGame/Toon"
{
    Properties
    {
        _MainTex      ("テクスチャ（無くてよい）", 2D) = "white" {}
        _Color        ("色", Color) = (0.8, 0.8, 0.8, 1)
        _TipColor     ("先端の色（草・葉のみ）", Color) = (1, 1, 1, 1)
        _TipBlend     ("先端の色の効き", Range(0, 1)) = 0
        _TipFromUv    ("先端の向きを UV から取る(0=頂点Y 1=UV.v)", Range(0, 1)) = 0

        _ColorB       ("ムラの色", Color) = (0.5, 0.68, 0.32, 1)
        _PatchAmount  ("ムラの強さ", Range(0, 1)) = 0
        _PatchScale   ("ムラの細かさ(1/m)", Float) = 0.045
        _CrackAmount  ("ひび割れの濃さ(0=切)", Range(0, 1)) = 0
        _WetAmount    ("濡れ具合(0=乾いている)", Range(0, 1)) = 0
        _PanelAmount  ("パネル割りの濃さ(0=切)", Range(0, 1)) = 0
        _PanelSize    ("パネル 1 枚の大きさ(m)", Range(0.3, 8)) = 2.4
        _PanelWidth   ("目地の太さ(m)", Range(0.005, 0.2)) = 0.035
        _PanelColor   ("目地の色", Color) = (0.16, 0.19, 0.22, 1)
        _WetColor     ("濡れた色", Color) = (0.10, 0.13, 0.16, 1)
        _RippleScale  ("波紋の細かさ(1/m)", Range(0.1, 4)) = 1.1
        _RippleSpeed  ("波紋の速さ", Range(0.1, 4)) = 1.3
        _RippleStrength ("波紋の明るさ", Range(0, 0.5)) = 0.12
        _CrackScale   ("ひび割れの細かさ(1/m)", Float) = 0.35
        _CrackWidth   ("ひび割れの太さ", Range(0.002, 0.09)) = 0.022
        _CrackColor   ("ひび割れの色", Color) = (0.08, 0.10, 0.12, 1)
        _WindowAmount ("窓の濃さ(0=切)", Range(0, 1)) = 0
        _WindowCell   ("窓の間隔 xy(m) と原点ずらし zw", Vector) = (2.6, 3.2, 0, 0)
        _WindowFill   ("窓が占める割合 xy", Vector) = (0.62, 0.55, 0, 0)
        _WindowColor  ("窓の色", Color) = (0.11, 0.14, 0.17, 1)
        _HeightTint   ("高いところの色", Color) = (0.62, 0.72, 0.66, 1)
        _HeightAmount ("高いところの効き", Range(0, 1)) = 0
        _HeightFrom   ("効き始める高さ(m)", Float) = 0
        _HeightTo     ("効き切る高さ(m)", Float) = 4

        _Steps        ("陰影の段数", Range(2, 5)) = 3
        _ShadeFloor   ("影の明るさ", Range(0, 1)) = 0.45
        _RimColor     ("リムの色", Color) = (1, 1, 1, 1)
        _RimPower     ("リムの締まり", Range(0.5, 8)) = 3.5
        _RimAmount    ("リムの強さ", Range(0, 1)) = 0.18

        _BladeHeight  ("葉の高さ(m)", Float) = 0.35
        _WindAmp      ("風の振れ幅(m)", Float) = 0
        _WindSpeed    ("風の速さ", Float) = 1.6
        _WindFreq     ("風の細かさ", Float) = 0.35
        _WindDir      ("風の向き(XZ)", Vector) = (1, 0, 0.35, 0)
        _WholeSway    ("房ごと揺らす(0=草 1=葉)", Range(0, 1)) = 0

        _WaterAmount  ("水の揺らぎ", Range(0, 1)) = 0
        _WaterSpeed   ("流れの速さ", Float) = 0.30
        _WaterScale   ("さざ波の細かさ", Float) = 4
        _FoamColor    ("白波の色", Color) = (1, 1, 1, 1)

        _FadeStart    ("薄れ始める距離(m)", Float) = 9999
        _FadeEnd      ("消える距離(m)", Float) = 10000

        _OutlineWidth ("輪郭線の太さ(m)。0 で無効", Float) = 0
        _OutlineColor ("輪郭線の色", Color) = (0.21, 0.26, 0.31, 1)
        _OutlineFade  ("輪郭線が消える距離(m)", Float) = 25

        [Enum(UnityEngine.Rendering.CullMode)] _Cull ("面の裏表", Float) = 2
    }

    SubShader
    {
        Tags { "RenderType" = "Opaque" "Queue" = "Geometry" }
        Cull [_Cull]
        LOD 200

        // ── 輪郭線（反転シェル）─────────────────────────────
        // ★既定は無効（_OutlineWidth = 0）。**幻想的な絵から離れやすい**ので、
        //   使う材質だけ個別に太さを入れる。草と葉には入れない
        //   （13 万本に線を引くと潰れるうえ、パスが 2 倍になる）。
        //
        // ★黒くしない。既定色はマスターパレットの Deep Slate #36434F。
        //   ★太さはワールド単位で、遠いほど細くして _OutlineFade で消す。
        //     画面上で一定の太さにすると、遠景で線だけが残って漫画に寄る。
        Pass
        {
            Name "OUTLINE"
            Cull Front
            ZWrite On
            CGPROGRAM
            #pragma vertex vertOutline
            #pragma fragment fragOutline
            #pragma multi_compile_instancing
            #include "UnityCG.cginc"

            float _OutlineWidth, _OutlineFade;
            half4 _OutlineColor;

            struct appdataO { float4 vertex : POSITION; float3 normal : NORMAL; UNITY_VERTEX_INPUT_INSTANCE_ID };
            struct v2fO { float4 pos : SV_POSITION; };

            v2fO vertOutline(appdataO v)
            {
                v2fO o;
                UNITY_SETUP_INSTANCE_ID(v);
                if (_OutlineWidth <= 0.0001)
                {
                    // 無効のときは画面外へ潰す。ラスタライズさせない。
                    o.pos = float4(0, 0, -10, 1);
                    return o;
                }
                float3 wp = mul(unity_ObjectToWorld, v.vertex).xyz;
                float d = distance(wp, _WorldSpaceCameraPos);
                float keep = 1.0 - saturate(d / max(_OutlineFade, 0.001));
                float3 n = normalize(mul((float3x3)unity_ObjectToWorld, v.normal));
                wp += n * (_OutlineWidth * keep);
                o.pos = UnityWorldToClipPos(float4(wp, 1));
                return o;
            }

            half4 fragOutline(v2fO i) : SV_Target { return _OutlineColor; }
            ENDCG
        }

        CGPROGRAM
        #pragma surface surf Toon vertex:vert addshadow fullforwardshadows
        #pragma multi_compile_instancing
        #pragma target 3.0

        sampler2D _MainTex;

        struct Input
        {
            float2 uv_MainTex;
            float height;          // 0=根元 1=先端（風の重み。草・葉のみ意味を持つ）
            float tint;            // 色のグラデーション用（頂点 Y か UV.v）
            float3 worldPos;
            float3 viewDir;
            float facing : VFACE;  // 裏面なら負
        };

        // ── ワールド座標のムラ ─────────────────────────────
        // ★テクスチャを貼らない理由: 地面は 64m と 220m の巨大な箱で、
        //   立方体の UV（面ごとに 0..1）に画像を貼ると引き伸ばされる。
        //   ワールド座標で拾えば、箱でも丘でも継ぎ目なく同じ模様が乗る。
        float BellHash(float2 p)
        {
            p = frac(p * float2(123.34, 456.21));
            p += dot(p, p + 45.32);
            return frac(p.x * p.y);
        }
        float BellNoise(float2 p)
        {
            float2 i = floor(p), f = frac(p);
            f = f * f * (3.0 - 2.0 * f);
            float a = BellHash(i), b = BellHash(i + float2(1, 0));
            float c = BellHash(i + float2(0, 1)), d = BellHash(i + float2(1, 1));
            return lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y);
        }
        float BellFbm(float2 p)
        {
            return BellNoise(p) * 0.55 + BellNoise(p * 2.3) * 0.30 + BellNoise(p * 5.1) * 0.15;
        }

        half4 _Color, _ColorB, _TipColor, _RimColor, _HeightTint, _WindowColor;
        float4 _WindowCell, _WindowFill;
        float _WindowAmount;
        half4 _CrackColor;
        float _CrackAmount, _CrackScale, _CrackWidth;
        half4 _WetColor;
        float _WetAmount, _RippleScale, _RippleSpeed, _RippleStrength;
        half4 _PanelColor;
        float _PanelAmount, _PanelSize, _PanelWidth;
        float _PatchAmount, _PatchScale, _HeightAmount, _HeightFrom, _HeightTo;
        float _TipBlend, _TipFromUv, _Steps, _ShadeFloor, _RimPower, _RimAmount;
        float _BladeHeight, _WindAmp, _WindSpeed, _WindFreq, _WholeSway;
        float4 _WindDir;
        float _FadeStart, _FadeEnd;
        float _WaterAmount, _WaterSpeed, _WaterScale;
        half4 _FoamColor;

        UNITY_INSTANCING_BUFFER_START(Props)
            UNITY_DEFINE_INSTANCED_PROP(half4, _InstColor)
        UNITY_INSTANCING_BUFFER_END(Props)

        // 段の付いた拡散光。アニメ調はここで決まる。
        half4 LightingToon(SurfaceOutput s, half3 lightDir, half3 viewDir, half atten)
        {
            half ndl = saturate(dot(s.Normal, lightDir));
            half stepped = floor(ndl * _Steps) / max(_Steps - 1, 1);
            stepped = lerp(_ShadeFloor, 1.0, saturate(stepped));

            half rim = pow(1.0 - saturate(dot(normalize(viewDir), s.Normal)), _RimPower);

            half4 c;
            c.rgb = s.Albedo * _LightColor0.rgb * stepped * atten
                    + _RimColor.rgb * rim * _RimAmount * ndl;
            c.a = s.Alpha;
            return c;
        }

        void vert(inout appdata_full v, out Input o)
        {
            UNITY_INITIALIZE_OUTPUT(Input, o);

            // 草は「根元は動かない」ので頂点の高さで、葉は房ごと動かすので一律 1。
            float h = lerp(saturate(v.vertex.y / max(_BladeHeight, 0.001)), 1.0, _WholeSway);
            o.height = h;

            // ★色のグラデーションだけは別の量を使える。
            //   頂点 Y は _BladeHeight（草の丈 0.35m）で割っているので、
            //   高さ 2.2m の扉に使うと**下から 0.35m で振り切って**上は一色になる（実際そうなっていた）。
            //   鏡板には v が 0→1 で上がる UV を張ってあるので、そちらを使う。
            o.tint = lerp(h, v.texcoord.y, _TipFromUv);

            if (_WindAmp > 0.0001 || _FadeEnd < 9999)
            {
                float3 wp = mul(unity_ObjectToWorld, float4(v.vertex.xyz, 1)).xyz;

                // 遠いものは縮めて消す。既定（_FadeEnd = 10000）では何も起きない。
                float d = distance(wp, _WorldSpaceCameraPos);
                float keep = 1.0 - saturate((d - _FadeStart) / max(_FadeEnd - _FadeStart, 0.001));
                v.vertex.xyz *= float3(1, keep, 1);

                float phase = _Time.y * _WindSpeed + wp.x * _WindFreq + wp.z * _WindFreq * 0.7;
                float sway = (sin(phase) + 0.35 * sin(phase * 2.3)) * _WindAmp * h * h * keep;
                float3 dirW = normalize(float3(_WindDir.x, 0, _WindDir.z));
                v.vertex.xyz += mul((float3x3)unity_WorldToObject, dirW) * sway;
            }
        }

        void surf(Input IN, inout SurfaceOutput o)
        {
            half4 inst = UNITY_ACCESS_INSTANCED_PROP(Props, _InstColor);
            // インスタンス色が渡っていないとき（a=0）はマテリアルの色に落ちる。
            half4 baseCol = inst.a > 0.001 ? inst : _Color;

            // ムラ。長波長と短波長を重ねた雲状の斑で 2 色を混ぜる。
            //   地面をベタ 1 色で塗ると、広い面がのっぺり見えて奥行きも出ない。
            if (_PatchAmount > 0.001)
            {
                float n = BellFbm(IN.worldPos.xz * _PatchScale);
                baseCol.rgb = lerp(baseCol.rgb, _ColorB.rgb, saturate(n) * _PatchAmount);
            }

            // ★ひび割れ。**イラスト調に寄せるので、写真のような細かい網目にはしない。**
            //   fbm の等高線 0.5 のあたりだけを抜くと、枝分かれした線になる。
            //   太い線と細い線を 2 本重ね、さらに長波長のノイズで
            //   「割れていない区画」を残す ── 全面に走らせると壁紙に見える。
            //   ワールド XZ で拾うので、**水平な面（舗装・地面）専用。**
            if (_CrackAmount > 0.001)
            {
                float2 cp = IN.worldPos.xz * _CrackScale;
                float e = max(_CrackWidth, 0.001);
                float c = 1.0 - smoothstep(0.0, e, abs(BellFbm(cp) - 0.5));
                float c2 = 1.0 - smoothstep(0.0, e * 0.5, abs(BellFbm(cp * 2.7 + 13.0) - 0.5));
                c = max(c, c2 * 0.65);
                c *= saturate(BellFbm(cp * 0.23 + 41.0) * 2.4 - 0.45);
                baseCol.rgb = lerp(baseCol.rgb, _CrackColor.rgb, saturate(c) * _CrackAmount);
            }

            // ★コンクリのパネル割り。**写真の質感は狙わない。**
            //   欲しいのは「打ち放しの板が敷き詰めてある」という**読み**で、
            //   それを作るのは細かい肌ではなく**目地の線と、板ごとの明度差**のほう。
            //   セル調の他の面と揃うのはこちら。
            //
            //   ★上を向いた面にだけ。面の向きは worldPos の画面微分から取る
            //     （窓・濡れと同じ理由で `IN.worldNormal` は使わない）。
            if (_PanelAmount > 0.001)
            {
                float3 pn = normalize(cross(ddx(IN.worldPos), ddy(IN.worldPos)));
                float flatP = saturate(abs(pn.y) * 2.2 - 0.6);
                if (flatP > 0.002)
                {
                    float size = max(_PanelSize, 0.05);
                    float2 g = IN.worldPos.xz / size;
                    float2 fr = frac(g);
                    // 目地までの距離をメートルで測る。板の大きさを変えても線の太さが変わらない。
                    float2 e = min(fr, 1.0 - fr) * size;
                    float j = 1.0 - smoothstep(_PanelWidth * 0.4, _PanelWidth, min(e.x, e.y));

                    // ★板ごとに明度を散らす。**これが無いと方眼紙に見える。**
                    //   打ち放しは 1 枚ずつ打った日が違うので、板単位で色が変わる。
                    float pv = BellHash(floor(g) + 5.1) - 0.5;
                    baseCol.rgb *= 1.0 + pv * 0.16 * _PanelAmount * flatP;
                    baseCol.rgb = lerp(baseCol.rgb, _PanelColor.rgb, j * _PanelAmount * flatP);
                }
            }

            // ★濡れと波紋。**雨は筋だけでは降っているように見えない。**
            //   人が「雨だ」と読むのは、空中の粒よりも**地面の変化**のほう ──
            //   色が濃くなり、水たまりに輪が広がる。筋は添え物。
            //
            //   ★上を向いた面にだけ乗せる（壁に水たまりはできない）。
            //     面の向きは worldPos の画面微分から取る ── 窓と同じ理由で
            //     `IN.worldNormal` は使わない（INTERNAL_DATA が要る）。
            if (_WetAmount > 0.001)
            {
                float3 wn = normalize(cross(ddx(IN.worldPos), ddy(IN.worldPos)));
                float flat_ = saturate(abs(wn.y) * 2.2 - 0.6);   // 水平に近いほど 1
                if (flat_ > 0.002)
                {
                    // 濡れ色。彩度を上げずに暗くする ── 濡れた地面は「濃くなる」であって
                    // 「色が変わる」ではない。
                    baseCol.rgb = lerp(baseCol.rgb, _WetColor.rgb, flat_ * _WetAmount);

                    // 波紋。ワールド XZ を升目に切って、升ごとに 1 つ輪を広げる。
                    //   位相を升ごとに散らさないと、全部が同時に広がって呼吸に見える。
                    float2 rp = IN.worldPos.xz * max(_RippleScale, 0.01);
                    float2 cell = floor(rp);
                    float2 f2 = frac(rp) - 0.5;
                    float h = BellHash(cell + 3.7);
                    float t2 = frac(_Time.y * _RippleSpeed + h);
                    float r2 = length(f2) * 2.0;
                    // 輪 1 本。広がるほど薄くなる。
                    float ring = smoothstep(t2 - 0.10, t2, r2) * smoothstep(t2 + 0.10, t2, r2);
                    ring *= (1.0 - t2) * step(0.35, h);          // 升の 65% だけ使う
                    baseCol.rgb += _RippleStrength * ring * flat_ * _WetAmount;
                }
            }

            // ★窓。**ジオメトリでは作らない。**
            //   崩壊都市の上層は音響の箱より上にあるただの飾りなので、
            //   そこに三角形を使うのは丸損。ワールド座標の格子で暗い矩形を塗るだけにする。
            //   何百枚でも三角形は 0 枚。
            //
            //   ★ワールド座標で拾う理由はムラと同じ。壁は立方体なので面ごとの UV が
            //     引き伸ばされていて、画像も UV 格子も使えない。
            if (_WindowAmount > 0.001)
            {
                // ★面の向きは worldPos の画面微分から取る。**IN.worldNormal を使わないこと。**
                //   Input に worldNormal を入れると INTERNAL_DATA が必要になり
                //   （入れないとコンパイルが通らず全部マゼンタになる）、
                //   接空間の行列が**全頂点に**積まれる。このシェーダは草と葉も描いていて、
                //   葉だけで 12,000 インスタンスあるので、そこに払うのは高すぎる。
                //   微分なら断片段だけの計算で済むうえ、箱の**面の**法線がそのまま出る
                //   （頂点法線と違って平滑化されない）。向きの符号は abs で潰すので問わない。
                float3 wn = normalize(cross(ddx(IN.worldPos), ddy(IN.worldPos)));
                // 水平な面（屋上・床・庇）には窓を出さない。
                float side = 1.0 - saturate(abs(wn.y) * 3.0);
                if (side > 0.001)
                {
                    // 壁の向きで横方向の座標を選ぶ。±Z の壁は X、±X の壁は Z。
                    float u = abs(wn.z) > abs(wn.x) ? IN.worldPos.x : IN.worldPos.z;
                    float2 cell = (float2(u, IN.worldPos.y) + _WindowCell.zw)
                                / max(_WindowCell.xy, 0.01);
                    float2 f = abs(frac(cell) - 0.5) * 2.0;      // 0=窓の中心 1=柱の中心
                    float win = step(f.x, _WindowFill.x) * step(f.y, _WindowFill.y) * side;
                    // 部屋ごとに濃さを散らす。全部同じだと格子が機械に見える。
                    float v = BellHash(floor(cell) + 11.0);
                    baseCol.rgb = lerp(baseCol.rgb, _WindowColor.rgb,
                                       win * _WindowAmount * (0.5 + v * 0.5));
                }
            }

            // 高いところほど別の色へ寄せる（丘の稜線を浮かせる／遠景の色に近づける）。
            if (_HeightAmount > 0.001)
            {
                float t = saturate((IN.worldPos.y - _HeightFrom) / max(_HeightTo - _HeightFrom, 0.001));
                baseCol.rgb = lerp(baseCol.rgb, _HeightTint.rgb, t * _HeightAmount);
            }

            // ★水の流れ。テクスチャを使わず、UV の v を時間で送ってさざ波を作る。
            //   生成器が川筋に沿って v を張ってあるので、**曲がっても流れが川下を向く**
            //   （ワールド座標で流すと、カーブで流れが横に走る）。
            if (_WaterAmount > 0.001)
            {
                float2 p = float2(IN.uv_MainTex.x * 2.0, IN.uv_MainTex.y - _Time.y * _WaterSpeed) * _WaterScale;
                float n1 = BellFbm(p);
                float n2 = BellFbm(p * 1.9 + 37.0 - float2(0, _Time.y * _WaterSpeed * 0.55));
                float w = saturate((n1 + n2) * 0.5);
                baseCol.rgb = lerp(baseCol.rgb, _FoamColor.rgb,
                                   smoothstep(0.60, 0.86, w) * _WaterAmount);
            }

            half3 tex = tex2D(_MainTex, IN.uv_MainTex).rgb;
            o.Albedo = tex * lerp(baseCol.rgb, _TipColor.rgb, saturate(IN.tint) * _TipBlend);

            // 裏面は法線を反転（Cull Off のときに来る）。
            o.Normal = float3(0, 0, IN.facing > 0 ? 1 : -1);
            o.Alpha = 1;
        }
        ENDCG
    }
    FallBack "Diffuse"
}
