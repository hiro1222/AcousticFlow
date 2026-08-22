// WorldId.cs
// 世界の識別と、ベルの声。
//
// ★設定: プレイヤーのベルは**欠けている**。各世界に散っているのは、
//   同じベルから分かれた**倍音 1 本**。元が同じだから共鳴して鳴る。
//
//   拾うたびに、その倍音が自分のベルへ戻る。
//   **集めるほど自分のベルの音が豊かになる** ── 「本来の力を取り戻す」が
//   テキストではなく音として聞こえる。
//
// ★芯は失われていない。
//   ハム(0.5倍)と基音(1.0倍)は最初から持っている。ここが無いと「同じベル」に聞こえない。
//
// ★打点の当たり（広帯域の一瞬）は倍音と無関係に常にある。
//   これが無いと、欠けたベルは低域しか持たず HRTF も遮蔽の音色差も効かなくなり、
//   序盤が探索不能になる。実際の鐘も撞いた瞬間は広帯域なので物理的にも正しい。
namespace BellGame
{
    public enum WorldId
    {
        None = 0,
        Grassland = 1,
        Temple = 2,      // 崩壊都市（識別子は歴史的に Temple のまま）
        Cave = 3,
        Snow = 4,
        Sea = 5,

        // ★プロローグの白い部屋（発注者の決定・2026-08-22）。
        //
        //   ベルで行き来する 5 つの世界とは性格が違いますが、**扉でつながる以上
        //   WorldSet に登録できる必要があります**（根っこの名前 `World_<WorldId>` で
        //   世界を見つける作りなので、枠が無いと登録できない）。
        //   枠が無かったあいだ `Temple` を借りていて、白い部屋に崩壊都市の空と声が
        //   当たっていました ── 見た目も音も嘘になるので、専用の枠を切ります。
        //
        // ⚠ **末尾に足すこと。**既存の値を動かすと、保存済みのシーンが黙って別の世界を指します。
        White = 6,
    }

    public static class BellVoices
    {
        /// ベル本体の基音。世界によらず 1 つ。
        public const float BaseHz = 560f;

        /// 欠けても残っている芯。ハムと基音。
        public static readonly float[] CoreRatios = { 0.50f, 1.00f };
        public static readonly float[] CoreAmps = { 0.35f, 1.00f };
        public static readonly float[] CoreDecays = { 0.70f, 1.20f };

        /// その世界に落ちている断片＝倍音の比。
        ///   鐘の非調和倍音の並びから採っている（整数倍でないのが鐘らしさの正体）。
        ///   低い倍音ほど早く手に入るようにしてあるので、音は下から埋まっていく。
        public static float Partial(WorldId id) => id switch
        {
            WorldId.Temple => 1.19f,   // 短三度。最初に戻る倍音
            WorldId.Cave => 1.50f,     // 五度
            WorldId.Snow => 2.00f,     // ノミナル
            WorldId.Sea => 2.55f,      // 上の非調和倍音
            _ => 0f,                   // 草原は出発点なので断片が無い
        };

        public static float PartialAmp(WorldId id) => id switch
        {
            WorldId.Temple => 0.70f,
            WorldId.Cave => 0.50f,
            WorldId.Snow => 0.45f,
            WorldId.Sea => 0.30f,
            _ => 0f,
        };

        /// 高い倍音ほど速く減衰する（実際の鐘と同じ。遠いほど鈍く聞こえる手がかりになる）。
        public static float PartialDecay(WorldId id) => id switch
        {
            WorldId.Temple => 1.80f,
            WorldId.Cave => 2.20f,
            WorldId.Snow => 3.00f,
            WorldId.Sea => 4.00f,
            _ => 2.0f,
        };

        public static string DisplayName(WorldId id) => id switch
        {
            WorldId.Grassland => "草原",
            WorldId.Temple => "崩壊都市",
            WorldId.Cave => "洞窟",
            WorldId.Snow => "雪山",
            WorldId.Sea => "海",
            WorldId.White => "白い部屋",
            _ => "—",
        };
    }
}
