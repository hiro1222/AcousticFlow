// WorldVoice.cs
// 世界ごとの「聞こえ方」── その世界が音をどう鳴らすか。
//
// ★WorldSky と対になる。あちらが見え方を 1 箇所に持ち、こちらが聞こえ方を 1 箇所に持つ。
//   同じ設定を 2 箇所が持つと必ずズレる（決めごと #1）。
//   実際いまズレかけていた ── RT60 が WORLD_SETTING §3 の表と
//   BeyondAmbience の中の表の 2 箇所にあった。
//
// ★考え方
//   扉の奥に音源が置いてある、のではない。
//   **開口そのものが、繋がった世界の聞こえ方を持つ。**
//   だから扉の行き先が変われば、開口の鳴り方がまるごと入れ替わる。
//
// ★物理の担当は 2 つ（発注者の線引き）。
//     ① 扉を開いた際の音の入ってきかた → エンジン（§4.3 フレネル帯域積分）
//     ② 音の反響                        → エンジン（部屋グラフ → Sabine）
//   ただし ② のうち**向こうの世界の反響だけ**はエンジンから出てこない。
//   生きている音響シーンは 1 面しか置けないので、向こうの部屋は存在しないため。
//   その 1 点だけを、ここが音へ焼く。手前の空間の響きはエンジンが別に付けるので
//   二重にはならない。
using UnityEngine;

namespace BellGame
{
    /// 世界 1 つぶんの聞こえ方。
    public struct VoiceSettings
    {
        /// 看板になる音。扉ごしに聞こえた瞬間、どの世界かが分かる 1 つ。
        public AmbientKind signature;

        /// その世界の残響時間(秒)。0 = 屋外（部屋にならない）。
        public float rt60;

        /// 焼き込む残響の量。1 にすると原音が消えるので、あくまで色付け。
        public float wet;

        /// 明るさ。1 = そのまま / 小さいほど高域が落ちる。
        ///   材質の吸音がその世界の音色を決める、という物理をなぞった値。
        public float brightness;
    }

    public static class WorldVoice
    {
        public static VoiceSettings Of(WorldId id)
        {
            switch (id)
            {
                // 草原 ── 部屋 0 個。尾は立たない。基準なので素直な音。
                //   看板は鳥。開けた昼の空気は、鳥だけで伝わる。
                case WorldId.Grassland:
                    return new VoiceSettings
                    {
                        signature = AmbientKind.Bird,
                        rt60 = 0f,
                        wet = 0f,
                        brightness = 1.00f,
                    };

                // 崩壊都市 ── 屋根の残った建物だけが部屋になる（720m³ / α0.15）。
                //   看板は葉擦れ。自然に還りつつある廃墟。
                //   コンクリが風化して少し吸うので、明るさは僅かに落とす。
                case WorldId.Temple:
                    return new VoiceSettings
                    {
                        signature = AmbientKind.Leaves,
                        rt60 = 1.5f,
                        wet = 0.35f,
                        brightness = 0.85f,
                    };

                // 洞窟 ── 裸岩 α0.038。**尾が主役**の世界。
                //   看板は水滴。1 滴に 5 秒の尾が付くので、扉ごしでも「広い」と分かる。
                //   岩は高域も返すが、長い尾に埋もれて鈍く聞こえるので少し暗く。
                case WorldId.Cave:
                    return new VoiceSettings
                    {
                        signature = AmbientKind.Drip,
                        rt60 = 5.1f,
                        wet = 0.55f,
                        brightness = 0.70f,
                    };

                // 雪山 ── 屋外だが、**雪が高域から食う**（α0.30〜0.92）。
                //   尾は立たないのに音が暗い、というのがこの世界の署名。
                //   §3 の主題「手がかりの喪失」は、この暗さがそのまま担う。
                case WorldId.Snow:
                    return new VoiceSettings
                    {
                        signature = AmbientKind.Wind,
                        rt60 = 0f,
                        wet = 0f,
                        brightness = 0.45f,
                    };

                // 海 ── 未着手。暫定で広い屋外。
                case WorldId.Sea:
                    return new VoiceSettings
                    {
                        signature = AmbientKind.Stream,
                        rt60 = 0.6f,
                        wet = 0.15f,
                        brightness = 0.90f,
                    };

                // 白い部屋 ── プロローグ。**閉じた箱**。
                //
                //   ★環境音は**鳴らさない**（signature は形式上 Wind ですが wet で殺します）。
                //     ここは「閉じた場所は響く」を教える場で、草原（無響）との対比が全部です。
                //     環境音を置くと、返ってくる響きがそれに紛れます。
                //
                //   ★rt60 は内寸 8×8×4.2m ＝ 269m³ から Sabine で導いた値
                //    （連絡板 M11）。**焼いた数字ではなく、エンジンが実行時に導きます**が、
                //     扉ごしに漏れる音の作りにはこの値を使います。
                case WorldId.White:
                    return new VoiceSettings
                    {
                        signature = AmbientKind.Wind,
                        rt60 = 1.1f,
                        wet = 0f,       // 環境音そのものは鳴らさない
                        brightness = 0.85f,
                    };

                default:
                    return Of(WorldId.Grassland);
            }
        }

        /// その世界の聞こえ方を持った音を作る。扉の開口がこれを鳴らす。
        public static AudioClip MakeLeak(WorldId id, int sr)
        {
            var v = Of(id);
            var clip = AmbientVoices.Make(v.signature, 900 + (int)id * 17, sr);

            bool needTail = v.rt60 > 0.05f && v.wet > 0f;
            bool needTilt = v.brightness < 0.999f;
            if (!needTail && !needTilt) return clip;

            var data = new float[clip.samples * clip.channels];
            clip.GetData(data, 0);

            // ★順番が大事。**先に響かせてから暗くする。**
            //   逆にすると、暗くした音が響いてまた明るく積み上がる
            //  （尾は同じ音を何度も重ねるので、色付けが打ち消される）。
            if (needTail) AmbientVoices.BakeRoomTail(data, sr, v.rt60, v.wet);
            if (needTilt) Darken(data, sr, v.brightness);

            var made = AudioClip.Create("Leak_" + id, clip.samples, clip.channels, sr, false);
            made.SetData(data, 0);
            return made;
        }

        // 高域を落とす。材質の吸音が音色を決める、という物理をなぞった一極ローパス。
        //   brightness 1 = 素通し / 0 に近いほど暗い。
        private static void Darken(float[] d, int sr, float brightness)
        {
            // 明るさ → 遮断周波数。0.45（雪）で約 1.4kHz、0.85（廃墟）で約 8kHz。
            float fc = Mathf.Lerp(400f, 20000f, Mathf.Pow(Mathf.Clamp01(brightness), 1.6f));
            float k = 1f - Mathf.Exp(-2f * Mathf.PI * fc / sr);
            k = Mathf.Clamp(k, 0.001f, 1f);

            float lp = 0f;
            for (int i = 0; i < d.Length; i++)
            {
                lp += (d[i] - lp) * k;
                d[i] = lp;
            }

            // 落とした分だけ全体が小さくなるので、頂点を戻す。
            //   ★音量で世界を区別させたくない。区別は**音色**が付ける。
            float peak = 0f;
            for (int i = 0; i < d.Length; i++) peak = Mathf.Max(peak, Mathf.Abs(d[i]));
            if (peak > 1e-5f && peak < 0.98f)
            {
                float g = 0.98f / peak;
                for (int i = 0; i < d.Length; i++) d[i] *= g;
            }
        }
    }
}
