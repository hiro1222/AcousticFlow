// PerfMeter.cs
// 60fps に張り付かせたうえで、**まだ何 ms 余っているか**を出す。
//
// ★なぜ「フレーム間隔」では駄目か
//   60 に張り付けると、間隔は常に 16.7ms になる。余裕が 8ms でも 0.1ms でも 16.7ms。
//   **間隔は上限に当たっているので、余力の情報を持っていない。**
//   見るべきは「1 フレームで実際に働いた時間」── 待っている時間を含まない値。
//
//   FrameTimingManager が CPU メインスレッドの実働時間を返すので、それを使う。
//   取れない環境では黙って 0 を出さず、**取れないと言う**（数字が嘘になるより良い）。
//
// ★出す数字は 3 つに割る。どこを削ればいいかが分からないと意味が無いため。
//     実働   … CPU メインスレッドが働いた時間
//     うち音 … AcousticFlowSceneDemo が自分で測っている音響の時間
//     余り   … 16.7ms − 実働。**ここが「あと使えるコスト」**
using UnityEngine;
using AcousticFlow;

namespace BellGame
{
    [DisallowMultipleComponent]
    public sealed class PerfMeter : MonoBehaviour
    {
        [Header("張り付かせる先")]
        // ★45fps。60 から下げました（発注者の指定）。
        //   1 フレームの予算が 16.67ms → **22.22ms** になり、余りが 5.55ms 増えます。
        //   ★数字を緩めただけで、音が軽くなったわけではありません。
        //     予算が増えたぶん「入る音源の本数」が変わるので、
        //     費用の話をするときは**必ずこの値もいっしょに言う**こと。
        [Range(30, 240)] public int targetFps = 45;

        // ── 読み取り口（モニターウィンドウが読む）──
        public static float BudgetMs { get; private set; } = 16.67f;
        public static float FrameMs { get; private set; }      // 実測のフレーム間隔
        public static float WorkMs { get; private set; }      // CPU メインの実働
        public static float GpuMs { get; private set; }
        public static float AcousticMs { get; private set; }
        public static float WorstWorkMs { get; private set; }  // 直近 3 秒の最悪値
        public static float Fps { get; private set; }
        public static bool WorkMeasurable { get; private set; }
        public static string Note = "—";

        public static float FreeMs => Mathf.Max(0f, BudgetMs - WorkMs);
        public static float FreePct => (BudgetMs > 0f) ? 100f * FreeMs / BudgetMs : 0f;
        /// 最悪値で見た余り。**平均で足りていても、山で落ちるなら落ちる。**
        public static float WorstFreeMs => BudgetMs - WorstWorkMs;

        private readonly FrameTiming[] _t = new FrameTiming[1];
        private float _worstUntil;
        private float _fpsSm;

        private void OnEnable()
        {
            // ★vSync を切ってから targetFrameRate を効かせる。
            //   vSync が入っていると targetFrameRate は無視される（Unity の仕様）。
            QualitySettings.vSyncCount = 0;
            Application.targetFrameRate = targetFps;
            BudgetMs = 1000f / Mathf.Max(1, targetFps);
            WorstWorkMs = 0f;
            _worstUntil = Time.unscaledTime + 3f;
        }

        private void Update()
        {
            // ★実行中に張り付き先を変えられるようにする。
            //   OnEnable だけで効かせていると、45 と 60 を見比べるのに
            //   毎回入れ直しが要ります。予算がズレたら掛け直すだけ。
            float want = 1000f / Mathf.Max(1, targetFps);
            if (!Mathf.Approximately(BudgetMs, want))
            {
                QualitySettings.vSyncCount = 0;
                Application.targetFrameRate = targetFps;
                BudgetMs = want;
                WorstWorkMs = 0f;                     // 予算が変われば山も測り直し
                _worstUntil = Time.unscaledTime + 3f;
            }

            FrameMs = Time.unscaledDeltaTime * 1000f;
            _fpsSm = Mathf.Lerp(_fpsSm <= 0f ? (1f / Mathf.Max(1e-4f, Time.unscaledDeltaTime)) : _fpsSm,
                                1f / Mathf.Max(1e-4f, Time.unscaledDeltaTime), 0.1f);
            Fps = _fpsSm;

            AcousticMs = AcousticFlowSceneDemo.Status.Valid
                       ? AcousticFlowSceneDemo.Status.AcousticMs : 0f;

            FrameTimingManager.CaptureFrameTimings();
            if (FrameTimingManager.GetLatestTimings(1, _t) > 0)
            {
                WorkMeasurable = true;
                WorkMs = (float)_t[0].cpuMainThreadFrameTime;
                GpuMs = (float)_t[0].gpuFrameTime;
                Note = "FrameTimingManager（待ち時間を含まない実働）";
            }
            else
            {
                // ★取れないときに 0 を出すと「余裕たっぷり」に見えてしまう。嘘をつくより黙る。
                WorkMeasurable = false;
                WorkMs = 0f; GpuMs = 0f;
                Note = "★実働が取れない（Player Settings の Frame Timing Stats を入れてください）"
                     + " ── いまはフレーム間隔しか読めません";
            }

            // 最悪値は 3 秒ごとに床を下げる。下げないと一度の山が居座って現状が読めない。
            if (WorkMs > WorstWorkMs) WorstWorkMs = WorkMs;
            if (Time.unscaledTime >= _worstUntil) { _worstUntil = Time.unscaledTime + 3f; WorstWorkMs = WorkMs; }
        }

        /// 一行の要約。画面にもウィンドウにも同じ文が出るようにしておく。
        public static string OneLine()
        {
            if (!WorkMeasurable)
                return $"{Fps:F0}fps  間隔{FrameMs:F1}ms  音{AcousticMs:F2}ms  余り=測れない";
            return $"{Fps:F0}fps  実働{WorkMs:F1}/{BudgetMs:F1}ms"
                 + $"（うち音{AcousticMs:F2}）  余り {FreeMs:F1}ms ({FreePct:F0}%)"
                 + $"  山の余り {WorstFreeMs:F1}ms";
        }
    }
}
