// GameProgress.cs
// 世界をまたいで残るもの。
//
// ★持ち越すのは「集めた断片」だけ。
//   位置・扉の角度・探索の途中経過は持ち越さない。
//   世界を移るたびに、その世界は初めての姿で立ち上がってよい ──
//   §0 の「奥を想像する → 答え合わせ」は初回訪問にしか効かないので、
//   状態を積むほど作るものが増えるだけで、体験は増えない。
//
// ★静的な入れ物にしている理由。
//   DontDestroyOnLoad のオブジェクトを持ち回すと、
//   そのオブジェクトが**音源やリスナーを連れてくる事故**が起きやすい
//  （エンジンはシーン全体のコライダーを舐めるので、余計な物が残ると格子に入る）。
//   ただのデータなら何も連れてこない。
//
// ⚠ スクリプトを書き換えると Unity がドメインを再読み込みして、ここは消える。
//   実行中に編集したら最初からやり直しになる。それで正しい（保存機能はまだ無い）。
using System.Collections.Generic;

namespace BellGame
{
    public static class GameProgress
    {
        /// 集めた断片。PlayerBells がこれを読み書きする。
        public static readonly List<WorldId> Held = new List<WorldId>();

        /// 扉をくぐって到着したか。到着なら扉の位置に立たせる。
        public static bool Arriving;

        /// どの世界から来たか（記録用）。
        public static WorldId CameFrom = WorldId.None;

        public static void Reset()
        {
            Held.Clear();
            Arriving = false;
            CameFrom = WorldId.None;
        }
    }
}
