// AcousticSurface.cs ── その面の材質を指定する印（場面の作り手が触る唯一の音響の欄）。
//
// ■ 全体の中の位置
//   場面（Collider ＋ この印）→ AcousticWorld が箱と材質に直して engine へ → engine が
//   反射・透過・吸収の 3 つの比に使う。ここは「どの材質か」を決めるだけで、音の計算はしない。
//   付いていない Collider は AcousticWorld の既定の材質になる。
//
// ■ 役割
//   材質は見た目から決まらない（同じ灰色でもコンクリートと発泡は別物）ので、要る面にだけ人が付ける。
//   3 つの決め方を持つ: プリセットそのまま／プリセットの形を残して量だけ動かす／6 帯域を直に。
//
// ■ 中の仕組み
//   Resolve() が mode で分岐して AcousticMaterial を組み立てるだけ。値を持つのは AcousticMaterial 側。
//   単位は**人が扱える形**にしてある ── 吸音率 α (0..1) と 透過損失 TL[dB]。
//   透過の生値（コンクリートで 1e-5 級）は書かない（桁を間違えても気付けないため）。
//
// ■ 繋がり
//   受ける: Inspector の入力。渡す: AcousticMaterial → AcousticWorld → AF_WorldAddMaterial。
//
// ■ 退けた書き方
//   ・プリセットの 5 択だけ: 作れない材質がある。雪（α 0.6〜0.9）はいちばん近い Default が 0.208 で桁が違う。
//     廃墟・神殿（α 0.10〜0.15）は WoodDoor 0.088 と Default 0.208 の間が無い。
//   ・吸音と透過をプリセットに束ねたまま: 「よく遮るが吸わない石」「漏れるが吸う布」を作れない
//     （壁の透過を上げたいのに部屋がこもる、という二択で詰まった）。→ Adjusted と Custom を足した。
//
// ■ 壊れる所
//   ・α を 1 に寄せると 反射 = 1 − α − τ が負になる。AcousticMaterial 側で 0.99 に頭打ちしている。
//   ・Custom で 6 個より短い配列を入れると、足りない帯域は Default の値。形が途中で折れる。
//
// ★プリセットの 5 択だけだと作れない材質がある。実例:
//     雪（α 0.6〜0.9）… いちばん近い Default が 0.208 で**桁が違う**
//     廃墟・神殿（α 0.10〜0.15）… WoodDoor 0.088 と Default 0.208 の間が無い
//   さらに透過と吸音がプリセットに束ねられているので「よく遮るが吸わない石」
//   「漏れるが吸う布」を独立に作れない（実際に壁の透過を上げたいのに部屋がこもる、
//   という二択で詰まった）。
//   そこで Adjusted（形は残して量だけ動かす）と Custom（6帯域を直に）を足した。
//
// 単位は**外向きに人が扱える形**にしてある:
//   吸音率 α (0..1) と 透過損失 TL[dB]。透過の生値（コンクリで 1e-5 級）は書かない。
using UnityEngine;

namespace AcousticFlow
{
    // 材質の決め方。
    public enum AcousticSurfaceMode
    {
        // プリセットそのまま（従来の挙動）。
        Preset = 0,
        // プリセットの**形**（帯域ごとの傾き）は残したまま、量だけ動かす。
        //   決めごと #3「形は物理から採り、絶対値は演出で決める」に沿った入口。
        Adjusted = 1,
        // 6 帯域を直に指定する。形そのものを変えたいとき（雪のように高域だけ極端に吸う等）。
        Custom = 2,
    }

    [DisallowMultipleComponent]
    public sealed class AcousticSurface : MonoBehaviour
    {
        [Tooltip("材質の決め方。\n"
                 + "Preset   … プリセットそのまま\n"
                 + "Adjusted … プリセットの形を残して、吸音と透過の量だけ動かす\n"
                 + "Custom   … 6 帯域を直に指定（形そのものを変えたいとき）")]
        public AcousticSurfaceMode mode = AcousticSurfaceMode.Preset;

        [Tooltip("この面の音響マテリアル。エリア素材より優先される。\n"
                 + "Adjusted のときは「形」の出どころになる。")]
        public AcousticMaterialPreset material = AcousticMaterialPreset.Default;

        [Header("Adjusted（形は残して量だけ）")]
        [Tooltip("吸音率を何倍にするか。1 でプリセットのまま。\n"
                 + "例: Default(平均α 0.208) を布敷きにしたいなら 2〜3 倍。\n"
                 + "α は 0.99 で頭打ち（反射 = 1-α-τ が負にならないように）。")]
        [Range(0f, 5f)] public float absorptionScale = 1f;

        [Tooltip("透過損失に足す量(dB)。+ で遮る / − で漏れる。0 でプリセットのまま。\n"
                 + "吸音と独立に動かせるので「よく遮るが吸わない石」も作れる。")]
        [Range(-30f, 30f)] public float transmissionLossOffsetDb = 0f;

        [Header("Custom（6帯域を直に。125 / 250 / 500 / 1k / 2k / 4k Hz）")]
        [Tooltip("吸音率 α (0..1)。空なら Default の値。")]
        public float[] customAbsorption = new float[0];

        [Tooltip("透過損失 TL[dB]。大きいほど遮る（コンクリで 26〜54 相当）。空なら Default の値。\n"
                 + "200 以上で実質の完全不透過。")]
        public float[] customTransmissionLossDb = new float[0];

        [Tooltip("散乱率 (0=鏡面 / 1=拡散)。空なら Default の値。")]
        public float[] customScattering = new float[0];

        /// Inspector の設定を engine に渡せる材質（6 帯域の 透過・吸音・散乱）に直す。
        ///   Adjusted: プリセットの形に 吸音 ×absorptionScale、透過損失 +offset dB を掛ける（形は残る）。
        ///   Custom:   6 帯域を直に。空の欄は Default の値で埋まる。
        ///   Preset:   そのまま。★呼ばれるのは場面を組むときだけ（毎フレームではない）。
        public AcousticMaterial Resolve()
        {
            switch (mode)
            {
                case AcousticSurfaceMode.Adjusted:
                    return AcousticMaterial.Adjusted(material, absorptionScale,
                                                     transmissionLossOffsetDb);
                case AcousticSurfaceMode.Custom:
                    return AcousticMaterial.FromBands(customAbsorption,
                                                      customTransmissionLossDb,
                                                      customScattering);
                default:
                    return AcousticMaterial.FromPreset(material);
            }
        }
    }
}
