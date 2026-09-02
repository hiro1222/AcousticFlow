// TuningCore.cs — 調整卓の**中身**（計算と書き込み）。見た目は持たない。
//
// ★なぜ切り出したか
//   見せ方が 2 つある:
//     ・ゲーム内パネル（F9・`TuningPanel`）……ビルドでも動く。実機で触りたいとき
//     ・Editor 窓（`AcousticFlow / 調整卓`）……ドッキングできて画面を塞がない
//   ここを片方に書くと、もう片方で写経することになる。写経は必ずずれるので、
//   **計算は 1 箇所**にして、見た目だけ 2 つにする（決めごと #1）。
//
// ★もうひとつの理由（実際に踏んだ）
//   `AcousticScene` を触る処理は**ランタイムアセンブリ**に置く必要がある。
//   Unity は `Editor/` を別アセンブリにするので、そこから internal は見えない
//   （キャプチャの自己診断で CS0122 が 33 件出た）。中身をここへ置けば両方から使える。
//
// 道具そのものの考え方は TuningPanel.cs の冒頭に書いてある（何のための道具か）。
using UnityEngine;

namespace AcousticFlow
{
    public class TuningCore
    {
        public const int NB = 6;
        public static readonly float[] BandHz = { 125f, 250f, 500f, 1000f, 2000f, 4000f };

        public int SourceIndex;
        public int CarrierPick;

        private readonly int[] _cInst = new int[8];
        private readonly int[] _cMat = new int[8];
        private readonly float[] _cLoss = new float[8];
        private int _cCount;

        private readonly float[] _combined = new float[NB];   // 合成後（実際に届く量）
        private readonly float[] _transOnly = new float[NB];  // 透過だけ
        private readonly float[] _matTr = new float[NB];
        private readonly float[] _matAb = new float[NB];

        public string LastResult { get; private set; } = "";

        /// 合成後の帯域ゲイン（読み取り専用のつもりで扱うこと）。
        public float[] Combined { get { return _combined; } }
        /// 透過だけの帯域ゲイン。
        public float[] TransOnly { get { return _transOnly; } }

        public int CarrierCount { get { return _cCount; } }
        public int CarrierInstance(int i) { return (i >= 0 && i < _cCount) ? _cInst[i] : -1; }
        public int CarrierMaterial(int i) { return (i >= 0 && i < _cCount) ? _cMat[i] : -1; }
        public float CarrierLossDb(int i) { return (i >= 0 && i < _cCount) ? _cLoss[i] : 0f; }

        /// 音源の表示名（無ければ番号）。
        public static string[] SourceNames()
        {
            var taps = AcousticFlowSceneDemo.Status.Taps;
            int n = (taps != null) ? taps.Length : 0;
            var names = new string[n];
            var raw = AcousticFlowSceneDemo.Status.SourceNames;
            for (int i = 0; i < n; ++i)
                names[i] = (raw != null && i < raw.Length && !string.IsNullOrEmpty(raw[i]))
                         ? raw[i] : i.ToString();
            return names;
        }

        public static bool Ready
        {
            get
            {
                var scene = AcousticFlowSceneDemo.SharedScene;
                return scene != null && scene.IsValid && AcousticFlowSceneDemo.Status.Taps != null;
            }
        }

        /// いま届いている音を採る。**毎フレーム採り直す**（歩けば変わるので）。
        public bool Sample()
        {
            var demo = Object.FindFirstObjectByType<AcousticFlowSceneDemo>();
            var scene = AcousticFlowSceneDemo.SharedScene;
            if (demo == null || scene == null || !scene.IsValid) return false;
            var taps = AcousticFlowSceneDemo.Status.Taps;
            if (taps == null || SourceIndex < 0 || SourceIndex >= taps.Length
                || taps[SourceIndex] == null) return false;
            int idx = taps[SourceIndex].EngineIndex;
            if (idx < 0) return false;

            scene.GetSourceOcclusion(idx, _combined);
            Vector3 L = demo.listener != null ? demo.listener.position : Vector3.zero;
            Vector3 S = SourcePos(demo, SourceIndex);
            scene.ComputeSoftOcclusion(L, S, _transOnly, out _);
            _cCount = scene.TransmissionCarriers(L, S, _cInst, _cMat, _cLoss);
            if (CarrierPick >= _cCount) CarrierPick = 0;
            if (_cCount > 0) scene.GetMaterial(_cMat[CarrierPick], _matTr, _matAb);
            return true;
        }

        private static Vector3 SourcePos(AcousticFlowSceneDemo demo, int i)
        {
            if (demo == null) return Vector3.zero;
            if (i == 0 && demo.source != null) return demo.source.position;
            int k = i - 1;
            if (demo.extraSources != null && k >= 0 && k < demo.extraSources.Length
                && demo.extraSources[k] != null) return demo.extraSources[k].position;
            return Vector3.zero;
        }

        public static float Db(float lin) { return 20f * Mathf.Log10(Mathf.Max(1e-6f, lin)); }

        /// 低域加重の広帯域レベル。人が「どれだけ通ってきたか」を判断するのは中低域なので。
        public static float BroadBand(float[] g)
        {
            float s = 0f, w = 0f;
            for (int b = 0; b < NB; b++) { float k = 1f / (b + 1f); s += g[b] * k; w += k; }
            return (w > 0f) ? s / w : 0f;
        }

        public float LevelDbNow { get { return Db(BroadBand(_combined)); } }
        public float MuffleNow { get { return _combined[5] > 1e-9f ? _combined[0] / _combined[5] : 0f; } }

        /// ★目標へ寄せる。**透過が担っている取り分でしか動かせない**ので、
        ///   届かないときははっきり言う。効かないつまみを回し続けるのがいちばんの失敗。
        public void ApplyTarget(float targetLevelDb, float targetMuffle)
        {
            var scene = AcousticFlowSceneDemo.SharedScene;
            if (scene == null || !scene.IsValid || _cCount <= 0)
            { LastResult = "担い手がいません"; return; }

            float nowLevel = LevelDbNow;
            float nowMuffle = _combined[5] > 1e-9f ? _combined[0] / _combined[5] : 1f;

            // 回折の取り分。ここは材質を触っても動かない。
            float diffShare;
            {
                float c = BroadBand(_combined), t = BroadBand(_transOnly);
                diffShare = (c > 1e-9f) ? Mathf.Clamp01((c - t) / c) : 0f;
            }

            // 目標との差を、透過へそのまま乗せる。
            //   ★2 つのつまみ（総量・こもり）→ 6 帯域は、**傾き一定**の形を仮定して配る。
            //     実測から採るのは「形」で、絶対値はここで決める（決めごと #3）。
            float dLevel = targetLevelDb - nowLevel;
            float muffleRatio = targetMuffle / Mathf.Max(nowMuffle, 1e-3f);
            var tr = new float[NB];
            for (int b = 0; b < NB; b++)
            {
                float u = b / (float)(NB - 1);                    // 0=125Hz, 1=4kHz
                float tiltDb = -10f * Mathf.Log10(Mathf.Max(muffleRatio, 1e-3f)) * u;
                float gainDb = dLevel + tiltDb;
                // エネルギー比なので dB → 線形は 10^(dB/10)。振幅ではない（material.h の規約）。
                tr[b] = Mathf.Clamp(_matTr[b] * Mathf.Pow(10f, gainDb * 0.1f), 0f, 1f);
            }
            scene.SetMaterial(_cMat[CarrierPick], tr, _matAb);

            // 届いたかは次のフレームでないと分からないので、ここでは見立てを書く。
            if (diffShare > 0.5f)
                LastResult = $"⚠ この経路の {diffShare * 100f:F0}% は回り込みが担っています。"
                           + "材質を触っても大きくは動きません。開口（戸口の幅）のほうが効きます。";
            else if (dLevel > 0f && _matTr[0] >= 0.999f)
                LastResult = "⚠ もう素通しです。これ以上大きくはできません。"
                           + "距離か音源のレベルで調整してください。";
            else
                LastResult = $"材質 {_cMat[CarrierPick]} を書き換えました（総量 {dLevel:+0.0;-0.0} dB）。"
                           + "次のフレームの実測を見てください。";
        }
    }
}
