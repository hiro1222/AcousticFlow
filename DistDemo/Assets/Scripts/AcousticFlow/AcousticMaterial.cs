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
        Default = 0,   // 部屋の境界としての内壁（両面張りの間仕切り）
        Concrete = 1,  // コンクリート（よく遮る）
        Glass = 2,     // ガラス（やや抜ける）
        Opaque = 3,    // 完全不透過（検証用。回り込んだ音だけを残して回折を単体で聴く）
        WoodDoor = 4,  // 木の扉（壁より弱い＝部屋の弱点。閉めていても向こうが聞こえる）
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
        // 既定は「部屋の境界としての内壁」＝両面張りの間仕切り。
        //   以前は片面の石膏ボード1枚ぶん(TL 18.4dB@125Hz)で、部屋を仕切る壁としては
        //   漏れすぎだった（密閉した部屋でも低域が素通りして聞こえた）。
        public static AcousticMaterial DefaultWall() => new AcousticMaterial(
            // TL[dB]:        25.0    30.0      36.0     40.0        44.0        46.0
            new[] { 0.00316f, 0.001f, 0.000251f, 0.0001f, 0.0000398f, 0.0000251f },
            new[] { 0.10f, 0.10f, 0.15f, 0.20f, 0.30f, 0.40f },
            new[] { 0.10f, 0.15f, 0.20f, 0.30f, 0.40f, 0.50f });

        // 木製の扉。**壁より明確に弱い**のが要点。
        //   現実でも部屋の遮音を決めているのは壁ではなく扉と隙間。
        //   壁 25dB / 扉 15dB という落差が「扉が閉まっている」体験を作る。
        public static AcousticMaterial WoodDoor() => new AcousticMaterial(
            // TL[dB]:      15.0     18.0      21.0      24.0      26.0     27.0
            new[] { 0.0316f, 0.0158f, 0.00794f, 0.00398f, 0.00251f, 0.002f },
            new[] { 0.10f, 0.08f, 0.08f, 0.08f, 0.09f, 0.10f },
            new[] { 0.03f, 0.05f, 0.08f, 0.12f, 0.16f, 0.20f });

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

        // ★プリセットは**エンジンから引く**。下の C# 側の表は、DLL が古い/読めないときの
        //   保険であって正ではない。
        //
        //   ここを自前の表にしていたせいで、C++ と C# で Default と Concrete の透過が
        //   6〜8dB 食い違っていた。C++ 側は「壁が漏らしすぎると扉から漏れる音が壁の漏れに
        //   埋もれてどこから聞こえるか分からなくなる」という理由で意図的に上げてあったが、
        //   C# が置き去りになっていた。Unity で鳴っていたのは C# の値なので、
        //   **回帰テストが守る遮音量と出荷する音が違う**状態だった
        //   （決めごと #1「同じ問いに 2 つの答えを持たせない」そのもの）。
        private static bool _presetWarned;
        public static AcousticMaterial FromPreset(AcousticMaterialPreset preset)
        {
            int n = AcousticEngine.NumBands;
            var tr = new float[n];
            var ab = new float[n];
            var sc = new float[n];
            try
            {
                if (Native.AF_MaterialPresetBands((int)preset, tr, ab, sc) >= n)
                    return new AcousticMaterial(tr, ab, sc);
            }
            catch (System.Exception) { /* 古い DLL＝エントリポイント無し。下へ落ちる */ }

            if (!_presetWarned)
            {
                _presetWarned = true;
                UnityEngine.Debug.LogWarning(
                    "[AcousticMaterial] エンジンからプリセットを引けませんでした。"
                    + "C# 側の保険の表を使います（エンジンとずれている可能性があります）。"
                    + "DLL が古くないか確認してください。");
            }
            return FromPresetFallback(preset);
        }

        // 保険。エンジンが引けないときだけ使う。**ここを正としない。**
        private static AcousticMaterial FromPresetFallback(AcousticMaterialPreset preset)
        {
            switch (preset)
            {
                case AcousticMaterialPreset.Concrete: return Concrete();
                case AcousticMaterialPreset.Glass: return Glass();
                case AcousticMaterialPreset.Opaque: return Opaque();
                case AcousticMaterialPreset.WoodDoor: return WoodDoor();
                default: return DefaultWall();
            }
        }

        // ── 任意の材質を作る ──
        //
        // ★インスペクタに透過の生値（コンクリで 1e-5 級）を並べても人は扱えないので、
        //   外向きの単位は**透過損失 TL[dB]** と**吸音率 α**にする。material.cpp の
        //   コメントと同じ単位なので、文献値をそのまま入れられる。
        //     transmission(エネルギー) = 10^(-TL/10)
        //
        // ★α の上限を 0.99 に切ってある。エンジンは reflection = 1 - α - τ で反射を出すので、
        //   α + τ が 1 を超えると反射が負になる（エンジン側でも clamp しているが、
        //   ここで切っておかないと「入れた値と鳴る値が違う」ことに気づけない）。

        /// 透過損失(dB) → 透過率(エネルギー比)。
        public static float TlDbToEnergy(float tlDb)
        {
            if (tlDb >= 200f) return 0f;                  // 実質の完全不透過
            return UnityEngine.Mathf.Pow(10f, -tlDb / 10f);
        }

        /// 透過率(エネルギー比) → 透過損失(dB)。表示用。
        public static float EnergyToTlDb(float energy)
        {
            if (energy <= 1e-20f) return 200f;
            return -10f * UnityEngine.Mathf.Log10(energy);
        }

        /// 6 帯域を直に指定して作る。alphaBands / tlDbBands / scatterBands は各 6 要素。
        /// null を渡した配列は Default の値を使う。
        public static AcousticMaterial FromBands(float[] alphaBands, float[] tlDbBands,
                                                 float[] scatterBands)
        {
            var baseMat = DefaultWall();
            int n = AcousticEngine.NumBands;
            var tr = new float[n];
            var ab = new float[n];
            var sc = new float[n];
            for (int b = 0; b < n; b++)
            {
                tr[b] = (tlDbBands != null && tlDbBands.Length > b)
                      ? TlDbToEnergy(tlDbBands[b]) : baseMat.transmission[b];
                ab[b] = (alphaBands != null && alphaBands.Length > b)
                      ? UnityEngine.Mathf.Clamp(alphaBands[b], 0f, 0.99f) : baseMat.absorption[b];
                sc[b] = (scatterBands != null && scatterBands.Length > b)
                      ? UnityEngine.Mathf.Clamp01(scatterBands[b]) : baseMat.scattering[b];
            }
            return new AcousticMaterial(tr, ab, sc);
        }

        /// プリセットの**形は残したまま**、量だけ動かす。
        ///   absorptionScale : 吸音率を何倍にするか（1 でそのまま）
        ///   tlOffsetDb      : 透過損失を何 dB 足すか（+ で遮る、− で漏れる）
        /// 決めごと #3「形は物理から採り、絶対値は演出で決める」に沿った入口。
        /// 形そのものを変えたいとき（雪のように高域だけ極端に吸うなど）は FromBands を使う。
        public static AcousticMaterial Adjusted(AcousticMaterialPreset preset,
                                                float absorptionScale, float tlOffsetDb)
        {
            var m = FromPreset(preset);
            int n = AcousticEngine.NumBands;
            var tr = new float[n];
            var ab = new float[n];
            var sc = new float[n];
            for (int b = 0; b < n; b++)
            {
                tr[b] = TlDbToEnergy(EnergyToTlDb(m.transmission[b]) + tlOffsetDb);
                ab[b] = UnityEngine.Mathf.Clamp(m.absorption[b] * absorptionScale, 0f, 0.99f);
                sc[b] = m.scattering[b];
            }
            return new AcousticMaterial(tr, ab, sc);
        }

        /// 中身で決まるキー。materialId のキャッシュに使う。
        ///   ★プリセットの enum をキーにしてはいけない。任意材質を入れると
        ///     「別の材質なのに同じ ID」に潰れる（同じ壁として鳴る）。
        public string ContentKey()
        {
            var sb = new System.Text.StringBuilder(96);
            int n = AcousticEngine.NumBands;
            for (int b = 0; b < n; b++) sb.Append(transmission[b].ToString("G6")).Append(',');
            for (int b = 0; b < n; b++) sb.Append(absorption[b].ToString("G6")).Append(',');
            for (int b = 0; b < n; b++) sb.Append(scattering[b].ToString("G6")).Append(',');
            return sb.ToString();
        }
    }
}
