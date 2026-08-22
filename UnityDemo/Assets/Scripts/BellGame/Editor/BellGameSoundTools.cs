// BellGameSoundTools.cs
// 目玉の 3 つを、**それぞれ単体で読める窓**にする。
//
//   §4.1 ソフト遮蔽（直接音）
//   §4.2 回折（中核）
//   §4.3 ポータルのフレネル帯域積分 ★潰してはいけない
//
// ★なぜ 1 つのモニターに全部入れないか
//   合計だけ見ていても「どの仕組みが効いているか」は分かりません。
//   3 つは同じ音に対して**別々の答え**を出していて、
//   「開口が広がったから明るくなった」のか「遮蔽が浅くなったから」なのかは、
//   分けて見ないと切り分かない。窓が分かれていれば、片方だけ並べて見比べられます。
//
// ★数字は AcousticFlowSceneDemo.Status（公開）から読むだけ。
//   システム側には何も足していません。
using UnityEditor;
using UnityEngine;
using AcousticFlow;

namespace BellGameEditor
{
    /// 3 つの窓で共通の下ごしらえ。
    internal static class SoundToolShared
    {
        public static AcousticFlowSceneDemo Demo;
        public static GUIStyle Mono;

        public static bool Ready(out AcousticFlowSceneDemo.SourceTaps[] taps)
        {
            taps = null;
            if (!EditorApplication.isPlaying) return false;
            if (Demo == null) Demo = Object.FindFirstObjectByType<AcousticFlowSceneDemo>();
            taps = AcousticFlowSceneDemo.Status.Taps;
            if (Demo != null) _cached = Sources();      // 名前の対応を毎回取り直す
            return Demo != null && taps != null;
        }

        public static void EnsureStyle()
        {
            if (Mono == null) Mono = new GUIStyle(EditorStyles.label) { font = EditorStyles.miniFont };
        }

        // ★音源の名前は**オブジェクトの名前**を使う。
        //
        //   もとは Status.SourceNames を優先していましたが、あれは `sourceEvents`
        //  （Vocal / Guitar / Piano / Bass / Drums / Other）で、音源の並びに
        //   使い回されているだけです。**どれが Falls_Center なのか分からない表**になりました。
        //   数字を読む道具で、行がどれか分からないのは致命的です。
        private static Transform[] _cached = new Transform[0];

        public static string NameOf(int i)
        {
            if (i >= 0 && i < _cached.Length && _cached[i] != null)
            {
                var n = _cached[i].name;
                return n.Length > 13 ? n.Substring(0, 13) : n;
            }
            var names = AcousticFlowSceneDemo.Status.SourceNames;
            if (names != null && i < names.Length && !string.IsNullOrEmpty(names[i]))
                return names[i] + "?";      // ★ 印つき ＝ 実体と対応が取れていない
            return "src" + i;
        }

        /// 6 帯域を並べる。1 が素通り、0 が遮断。
        ///
        /// ★小数 3 桁にしてあります。
        ///   隙間が狭いときの通過ゲインは 0.01〜0.02 で、2 桁だと全部 "0.01" に潰れて
        ///   **低域から順に上がっているかが読めません**（実際に扉 11° でそうなりました）。
        ///   ③ が効いているかを見る窓なので、そこが潰れると窓の意味が無くなります。
        public static string Bands(float[] g, int offset)
        {
            if (g == null || offset < 0 || offset + 6 > g.Length)
                return "    —     —     —     —     —     —";
            var sb = new System.Text.StringBuilder();
            for (int b = 0; b < 6; b++) sb.Append(string.Format("{0,6:F3}", g[offset + b]));
            return sb.ToString();
        }

        public static Transform[] Sources()
        {
            var l = new System.Collections.Generic.List<Transform>();
            if (Demo == null) return l.ToArray();
            if (Demo.source != null) l.Add(Demo.source);
            if (Demo.extraSources != null)
                foreach (var s in Demo.extraSources) if (s != null) l.Add(s);
            return l.ToArray();
        }

        public static void NotPlaying()
            => EditorGUILayout.HelpBox("再生中に数字が出ます。", MessageType.Info);
    }

    // ── §4.1 ソフト遮蔽 ───────────────────────────────────────
    //
    // 見るところ: 遮蔽が**連続に**動くか。物陰へ入る途中で段が出たら、そこが不具合。
    //   帯域は直接音タップの 6 帯域＝透過込みで残っている量。
    public sealed class OcclusionTool : EditorWindow
    {
        [MenuItem("BellGame/音の道具/遮蔽（§4.1）")]
        public static void Open()
            => GetWindow<OcclusionTool>("遮蔽 §4.1").minSize = new Vector2(440f, 200f);

        private Vector2 _s;
        private void Update() { if (EditorApplication.isPlaying) Repaint(); }

        private void OnGUI()
        {
            if (!SoundToolShared.Ready(out var taps)) { SoundToolShared.NotPlaying(); return; }
            SoundToolShared.EnsureStyle();

            EditorGUILayout.LabelField("直接音がどれだけ残っているか（1=素通り / 0=遮断）",
                                       EditorStyles.boldLabel);
            EditorGUILayout.LabelField(" #  名前        距離  遮蔽    125   250   500    1k    2k    4k",
                                       SoundToolShared.Mono);

            var occ = AcousticFlowSceneDemo.Status.Occlusion;
            var ear = SoundToolShared.Demo.listener;
            var srcs = SoundToolShared.Sources();

            _s = EditorGUILayout.BeginScrollView(_s);
            for (int i = 0; i < taps.Length; i++)
            {
                var ts = taps[i];
                if (ts == null || ts.Count <= 0) continue;
                float d = (ear != null && i < srcs.Length && srcs[i] != null)
                        ? Vector3.Distance(ear.position, srcs[i].position) : 0f;
                float o = (occ != null && i < occ.Length) ? occ[i] : 0f;
                EditorGUILayout.LabelField(
                    string.Format("{0,2}  {1,-10} {2,4:F1}m {3,5:F2}  ", i, SoundToolShared.NameOf(i), d, o)
                    + SoundToolShared.Bands(ts.BandGain, 0), SoundToolShared.Mono);
            }
            EditorGUILayout.EndScrollView();

            EditorGUILayout.LabelField("★物陰へ歩いて入る途中で、数字が**跳ばずに**動くか。",
                                       SoundToolShared.Mono);
            EditorGUILayout.LabelField("　段が出たら、そこが連続性の穴です（設計上の第一制約）。",
                                       SoundToolShared.Mono);
        }
    }

    // ── §4.2 回折 ─────────────────────────────────────────────
    //
    // 見るところ: 回り込みが**何本立っていて、どれだけ遠回りしているか**。
    //   遅延が長いほど迂回が大きく、前川の式で落ちる。
    public sealed class DiffractionTool : EditorWindow
    {
        [MenuItem("BellGame/音の道具/回折（§4.2）")]
        public static void Open()
            => GetWindow<DiffractionTool>("回折 §4.2").minSize = new Vector2(460f, 200f);

        private Vector2 _s;
        private void Update() { if (EditorApplication.isPlaying) Repaint(); }

        private void OnGUI()
        {
            if (!SoundToolShared.Ready(out var taps)) { SoundToolShared.NotPlaying(); return; }
            SoundToolShared.EnsureStyle();

            EditorGUILayout.LabelField(
                string.Format("稜線 {0} 本／候補 {1} 本／主音源の δ {2:F2}m",
                    AcousticFlowSceneDemo.Status.EdgeCatalogCount,
                    AcousticFlowSceneDemo.Status.DiffCandCount,
                    AcousticFlowSceneDemo.Status.DiffDelta), EditorStyles.boldLabel);
            EditorGUILayout.LabelField(" #  名前        本数 最短遅延 HRTF    125   250   500    1k    2k    4k",
                                       SoundToolShared.Mono);

            _s = EditorGUILayout.BeginScrollView(_s);
            for (int i = 0; i < taps.Length; i++)
            {
                var ts = taps[i];
                if (ts == null || ts.Count <= 0 || ts.Type == null) continue;

                int nF = 0, best = -1;
                float bestMs = float.MaxValue;
                for (int t = 1; t < ts.Count && t < ts.Type.Length; t++)
                {
                    if (ts.Type[t] != 'F') continue;
                    nF++;
                    if (ts.DelayMs[t] < bestMs) { bestMs = ts.DelayMs[t]; best = t; }
                }
                if (nF == 0 || best < 0) continue;

                string hr = (ts.HrtfTapIndex >= 0) ? string.Format("#{0,-3}", ts.HrtfTapIndex) : "—   ";
                EditorGUILayout.LabelField(
                    string.Format("{0,2}  {1,-10} {2,4} {3,6:F1}ms {4} ",
                                  i, SoundToolShared.NameOf(i), nF, bestMs, hr)
                    + SoundToolShared.Bands(ts.BandGain, best * 6), SoundToolShared.Mono);
            }
            EditorGUILayout.EndScrollView();

            EditorGUILayout.LabelField("★数字はいちばん早い回折タップの 6 帯域。",
                                       SoundToolShared.Mono);
            EditorGUILayout.LabelField("　HRTF 欄＝どれを両耳化しているか。遮蔽中の定位はこれが運びます。",
                                       SoundToolShared.Mono);
        }
    }

    // ── §4.3 ポータルのフレネル帯域積分 ─────────────────────────
    //
    // 見るところ: **隙間が広がるにつれ、低域から順に高域まで通る**か。
    //   ここが作品の芯（扉を開けるほど音が入ってくる）。同時に全部上がるなら潰れています。
    public sealed class ApertureTool : EditorWindow
    {
        [MenuItem("BellGame/音の道具/開口（§4.3）")]
        public static void Open()
            => GetWindow<ApertureTool>("開口 §4.3").minSize = new Vector2(460f, 240f);

        private Vector2 _s;
        private BellGame.PhysicsDoor _door;
        private void Update() { if (EditorApplication.isPlaying) Repaint(); }

        private void OnGUI()
        {
            if (!SoundToolShared.Ready(out var taps)) { SoundToolShared.NotPlaying(); return; }
            SoundToolShared.EnsureStyle();

            if (_door == null) _door = Object.FindFirstObjectByType<BellGame.PhysicsDoor>();
            float ang = (_door != null) ? _door.AngleDeg : 0f;

            EditorGUILayout.LabelField(
                string.Format("扉の開き {0:F0}°　開口コントラスト {1:F1}",
                              ang, SoundToolShared.Demo.apertureContrast), EditorStyles.boldLabel);
            EditorGUILayout.LabelField(" #  名前        経路    125   250   500    1k    2k    4k",
                                       SoundToolShared.Mono);

            _s = EditorGUILayout.BeginScrollView(_s);
            for (int i = 0; i < taps.Length; i++)
            {
                var ts = taps[i];
                if (ts == null || ts.Count <= 0 || ts.Type == null) continue;

                // 開口を抜けてきた経路＝回折タップ。いちばん強いものを見る。
                int best = -1; float bestG = -1f;
                for (int t = 1; t < ts.Count && t < ts.Type.Length; t++)
                {
                    if (ts.Type[t] != 'F') continue;
                    float g = 0f;
                    for (int b = 0; b < 6; b++) g += ts.BandGain[t * 6 + b];
                    if (g > bestG) { bestG = g; best = t; }
                }
                if (best < 0) continue;

                EditorGUILayout.LabelField(
                    string.Format("{0,2}  {1,-10} #{2,-3} ", i, SoundToolShared.NameOf(i), best)
                    + SoundToolShared.Bands(ts.BandGain, best * 6), SoundToolShared.Mono);
            }
            EditorGUILayout.EndScrollView();

            EditorGUILayout.LabelField("★扉を少しずつ開けて、**低域から順に**数字が上がるか見てください。",
                                       SoundToolShared.Mono);
            EditorGUILayout.LabelField("　同時に全部上がる／段で跳ぶなら、フレネル積分が効いていません。",
                                       SoundToolShared.Mono);
            EditorGUILayout.LabelField("　（これが作品の芯です ── 扉を開けるほど音が入ってくる）",
                                       SoundToolShared.Mono);
        }
    }
}
