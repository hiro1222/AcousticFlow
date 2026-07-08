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
        // 透過率は「質量則」で低域ほど大（壁越しにベースが漏れる）。値は現実的な遮音量に寄せた
        // 目安（コンクリ -30〜40dB級、石膏ボード -18〜28dB級）。旧値は placeholder で高すぎた。
        public static AcousticMaterial DefaultWall() => new AcousticMaterial(
            new[] { 0.12f, 0.07f, 0.04f, 0.02f, 0.012f, 0.008f },  // 石膏ボード相当（低域は少し漏れる）
            new[] { 0.10f, 0.10f, 0.15f, 0.20f, 0.30f, 0.40f },
            new[] { 0.10f, 0.15f, 0.20f, 0.30f, 0.40f, 0.50f });

        public static AcousticMaterial Concrete() => new AcousticMaterial(
            new[] { 0.05f, 0.03f, 0.015f, 0.008f, 0.004f, 0.002f },  // コンクリ相当（ほぼ遮断）
            new[] { 0.02f, 0.02f, 0.03f, 0.04f, 0.05f, 0.07f },
            new[] { 0.05f, 0.08f, 0.12f, 0.18f, 0.25f, 0.35f });

        public static AcousticMaterial Glass() => new AcousticMaterial(
            new[] { 0.10f, 0.07f, 0.05f, 0.035f, 0.025f, 0.018f },  // ガラス（やや抜ける）
            new[] { 0.18f, 0.06f, 0.04f, 0.03f, 0.02f, 0.02f },
            new[] { 0.02f, 0.03f, 0.05f, 0.08f, 0.10f, 0.15f });

        public static AcousticMaterial FromPreset(AcousticMaterialPreset preset)
        {
            switch (preset)
            {
                case AcousticMaterialPreset.Concrete: return Concrete();
                case AcousticMaterialPreset.Glass: return Glass();
                default: return DefaultWall();
            }
        }
    }
}
