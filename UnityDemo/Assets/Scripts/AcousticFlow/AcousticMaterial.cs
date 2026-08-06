// AcousticMaterial.cs
// 表面の音響特性（透過率・吸収率）を6帯域で持つ C# 側の表現。
// 値は C++ Core (material.cpp) のプリセットと一致させてある。
//   帯域: 125, 250, 500, 1k, 2k, 4k Hz
// AddBoxOriented にそのまま渡すと、transmission/absorption の float[6] が
// C ABI 越しにエンジンへ届く（誰が材質を決めるかは C# 側の責務）。
using System;

namespace AcousticFlow
{
    // プリセット選択肢（AcousticSurface / CollArea のインスペクタで使う）。
    public enum AcousticMaterialPreset
    {
        Default = 0,   // 一般的な内壁（石膏ボード相当）
        Concrete = 1,  // コンクリート（よく遮る）
        Glass = 2,     // ガラス（やや抜ける）
        Opaque = 3,    // 完全不透過（検証用。回り込んだ音だけを残して回折を単体で聴く）
    }

    [Serializable]
    public sealed class AcousticMaterial
    {
        // AddBoxOriented / AddMesh が読む配列（長さ = AcousticEngine.NumBands = 6）。
        public float[] transmission;
        public float[] absorption;
        public float[] scattering;   // 鏡面⇄拡散の混合率（0=鏡面/1=拡散）

        public AcousticMaterial(float[] transmission, float[] absorption, float[] scattering)
        {
            this.transmission = transmission;
            this.absorption = absorption;
            this.scattering = scattering;
        }

        // --- プリセット ---
        // ★ transmission / absorption は「エネルギー比」。透過損失 TL[dB] = -10*log10(transmission)。
        //   質量則で低域ほど漏れる（壁越しにベースが聞こえる）。
        //   以前は同じ値を振幅として使っていたが、refl = 1-α-τ がエネルギーの式なので
        //   単位が壊れていた。エネルギー規約へ統一するにあたり旧値を二乗してある
        //   （0.12 → 0.0144 = TL 18.4dB）。聞こえ方は変わらず、意図した遮音量とも一致する。
        public static AcousticMaterial DefaultWall() => new AcousticMaterial(
            // TL[dB]:      18.4     23.1     28.0     34.0      38.4        42.0
            new[] { 0.0144f, 0.0049f, 0.0016f, 0.0004f, 0.000144f, 0.000064f },  // 石膏ボード相当
            new[] { 0.10f, 0.10f, 0.15f, 0.20f, 0.30f, 0.40f },
            new[] { 0.10f, 0.15f, 0.20f, 0.30f, 0.40f, 0.50f });

        public static AcousticMaterial Concrete() => new AcousticMaterial(
            // TL[dB]:      26.0     30.5      36.5       42.0       48.0        54.0
            new[] { 0.0025f, 0.0009f, 0.000225f, 0.000064f, 0.000016f, 0.000004f },  // ほぼ遮断
            new[] { 0.02f, 0.02f, 0.03f, 0.04f, 0.05f, 0.07f },
            new[] { 0.05f, 0.08f, 0.12f, 0.18f, 0.25f, 0.35f });

        public static AcousticMaterial Glass() => new AcousticMaterial(
            // TL[dB]:     20.0     23.1     26.0      29.1        32.0        34.9
            new[] { 0.01f, 0.0049f, 0.0025f, 0.001225f, 0.000625f, 0.000324f },  // やや抜ける
            new[] { 0.18f, 0.06f, 0.04f, 0.03f, 0.02f, 0.02f },
            new[] { 0.02f, 0.03f, 0.05f, 0.08f, 0.10f, 0.15f });

        // 完全不透過（検証用）。透過を 0 にすると壁を抜けてくる成分が消え、
        // 聞こえるのは回り込んだ音だけになる。現実の材質ではない。
        public static AcousticMaterial Opaque() => new AcousticMaterial(
            new[] { 0f, 0f, 0f, 0f, 0f, 0f },
            new[] { 0.10f, 0.10f, 0.15f, 0.20f, 0.30f, 0.40f },
            new[] { 0.10f, 0.15f, 0.20f, 0.30f, 0.40f, 0.50f });

        public static AcousticMaterial FromPreset(AcousticMaterialPreset preset)
        {
            switch (preset)
            {
                case AcousticMaterialPreset.Concrete: return Concrete();
                case AcousticMaterialPreset.Glass: return Glass();
                case AcousticMaterialPreset.Opaque: return Opaque();
                default: return DefaultWall();
            }
        }
    }
}
