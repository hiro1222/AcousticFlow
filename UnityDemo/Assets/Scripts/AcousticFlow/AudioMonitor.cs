// AudioMonitor.cs ── 音の「操作」と「計器」（2026-09-09 に IrConvolver から移した）。
//
// ★なぜ別ファイルにしたか
//   音の計算は音響エンジン（C++）だけが行い、C# が持つのは**音の面の操作**だけ、と決めた。
//   ところが成分ソロ（1〜4 キー）と段別メーターは IrConvolver の入れ子（IrConvolver.Solo / .Scope）に
//   置かれていて、C++ 経路の VoiceConvolver がそこを 25 箇所も参照していた
//   ── **計算をしない側が、計算する側の型に依存している**という逆向きの繋がりだった。
//   IrConvolver（C# の畳み込み器）を消すにあたって、操作と計器だけをここへ出した。
//
//   Solo  … 何を鳴らすか（音源ごと・タップ種別ごとのゲート）。切り分けの道具であって音は作らない。
//   Scope … 何が鳴っているか（段別 RMS・予算・波形の段差）。読むだけ。
//
// ⚠ どちらも **audio thread から書かれ、main thread から読まれる**。
//   volatile と「1 つの枠には 1 つのスレッドしか書かない」で足りている（ロックは張らない）。
//   ★配列の枠は sourceIndex で分かれているので競合しない。増やすときもその規約を守ること。
using UnityEngine;

namespace AcousticFlow
{
    /// 【道具A】成分と音源のゲート。切り分け用で、音を作る側には関与しない。
    ///   ★窓を閉じたら必ず Reset を呼ぶこと（切ったまま忘れると「音が出ない」で悩む）。
    public static class Solo
    {
        public const int MaxSources = 64;

        public static int Only = -1;                       // -1 = 全部鳴らす。0以上ならその音源だけ
        public static readonly bool[] Mute = new bool[MaxSources];
        public static bool PassDirect = true;              // タップ種別 'D'
        public static bool PassReflect = true;             // 'R'
        public static bool PassDiffract = true;            // 'F'
        public static bool PassTail = true;                // 後期尾（タップではないので別枠）

        public static bool IsDefault
        {
            get
            {
                if (Only >= 0) return false;
                if (!PassDirect || !PassReflect || !PassDiffract || !PassTail) return false;
                for (int i = 0; i < MaxSources; i++) if (Mute[i]) return false;
                return true;
            }
        }

        public static bool AllowsSource(int idx)
        {
            if (idx < 0 || idx >= MaxSources) return true;
            if (Only >= 0 && idx != Only) return false;
            return !Mute[idx];
        }

        public static bool AllowsType(char t)
        {
            if (t == 'R') return PassReflect;
            if (t == 'F') return PassDiffract;
            return PassDirect;      // 'D' と、種別が入っていない古いデータ
        }

        public static void Reset()
        {
            Only = -1;
            PassDirect = PassReflect = PassDiffract = PassTail = true;
            for (int i = 0; i < MaxSources; i++) Mute[i] = false;
        }
    }

    /// 畳み込み後の実出力を覗くための窓（Output Scope が読む）。
    public static class Scope
    {
        public const int BufferLength = 32768;   // 48kHz で約 0.68 秒
        public static readonly float[] L = new float[BufferLength];
        public static readonly float[] R = new float[BufferLength];
        public static volatile int WritePos;
        public static volatile int TriggerPos = -1;   // テストクリックを出した位置（同期表示用）
        public static volatile int SampleRate;
        // 実際に読み込まれた HRTF データ名。「HRTF と表示されているのに中身は合成だった」を
        // 見えるようにするため（実測 kemar を取り込んだのに使われていない状態が続いていた）。
        public static string HrtfName = "";

        // 段ごとの RMS（どこが鳴っていて、どこが鳴っていないかの切り分け用）。
        public static volatile float RmsDirect;    // 直接タップ
        public static volatile float RmsEarly;     // 早期反射の鏡面成分
        public static volatile float RmsScatter;   // 散乱スメアの拡散成分
        public static volatile float RmsTail;      // 後期尾
        public static volatile float RmsOut;       // 最終出力
        public static volatile int TailPartitions; // 実測尾のパーティション数（0=尾IR未設定）
        public static volatile float TailToDirectRatio; // 尾の量に使った目標比（知覚圧縮後）
        public static volatile float PhysicalRatio;     // 圧縮前の物理比 (r/r_c)²（診断用）
        public static volatile float SplitMs;      // 早期↔後期の境目(ms)。部屋の大きさで動く
        public static volatile int ActiveParts;    // 実際に計算しているパーティション数

        // 【道具10 音源予算メータ】audio thread の実測時間(ms/block)。添字は sourceIndex。
        //   各インスタンスが自分の枠だけに書くので競合しない（float の書き込みは原子的）。
        //   これが無いと予算メータが「1音源の実測値 × 本数」の推定になってしまい、
        //   遮蔽の有無で実際には数倍違う（コストは遮蔽された音源数で効く）という実測に合わない。
        //   EMA で均してあるので、表示は「いまの平均的な 1 ブロックの代金」。
        public const int MaxMeteredSources = 64;
        public static readonly float[] BlockMs = new float[MaxMeteredSources];
        public static readonly int[] BlockFrames = new int[MaxMeteredSources];
        public static volatile int MeteredMax = -1;   // 実際に書かれた最大の sourceIndex

        // 【道具A 寄与度順の一覧】音源ごとの出力 RMS。上の RmsOut は全音源まとめた 1 本なので、
        //   「どれが実際に大きいか」がそれでは割れない。添字は sourceIndex。
        //   ★ミュート中も「鳴っていたはずの量」を出す（消した音の大きさが見えないと選べない）。
        public static readonly float[] OutRms = new float[MaxMeteredSources];

        // 【道具 ぷつぷつの切り分け】出力波形の「隣り合うサンプルの最大差 ÷ そのブロックの RMS」。
        //   ★これで「切れている（不連続）」と「音色が変わっただけ」を分けられる。
        //     クリック（波形の飛び）は必ずここに出る。音色の変化なら平常値のまま動かない。
        //   ★ドロップアウト（バッファが間に合わない）は別物で、負荷（BlockMs の合計 ÷
        //     ブロックの実時間）で見る。段差が平常のまま音が途切れるならそちら。
        public static volatile float StepRatio;      // このブロックの値
        public static volatile float StepRatioCalm;  // 平常値（遅い平均。比べる基準）
        public static volatile int   StepSpikes;     // 平常の 4 倍を超えたブロックの累計
        public static volatile float BlockDurMs;     // 1 ブロックの実時間(ms) = frames / sampleRate
    }
}
