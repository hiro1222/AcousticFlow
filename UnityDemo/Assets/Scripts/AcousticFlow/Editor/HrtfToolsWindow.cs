/* HrtfToolsWindow.cs
 * HRTF データの確認・書き出しツール。
 *
 *   メニュー: AcousticFlow > HRTF Tools
 *
 * 用途:
 *   1) 合成HRTFを .afhr として書き出す
 *      → ファイル読み込み経路(HrtfSet.LoadFromFile)を実データが届く前に検証できる。
 *        実データで初めて読み込みを試すと、パース不具合と変換不具合の切り分けが困難になる。
 *   2) 手元の .afhr を読んで中身を点検する
 *      → 方向数・IR長・SR・ITDの範囲が妥当かを、音を鳴らす前に数字で確認する。
 */
using System.IO;
using UnityEditor;
using UnityEngine;
using AcousticFlow;

namespace AcousticFlow.EditorTools
{
    public class HrtfToolsWindow : EditorWindow
    {
        [MenuItem("AcousticFlow/HRTF Tools")]
        public static void Open()
        {
            var w = GetWindow<HrtfToolsWindow>("HRTF Tools");
            w.minSize = new Vector2(460f, 300f);
            w.Show();
        }

        private string _inspectPath = "";
        private string _report = "";

        private void OnGUI()
        {
            EditorGUILayout.LabelField("合成HRTFの書き出し", EditorStyles.boldLabel);
            EditorGUILayout.HelpBox(
                "球体頭モデルの合成HRTFを .afhr として書き出します。\n" +
                "実データが届く前に「ファイル読み込み経路」を検証しておくと、\n" +
                "実データで問題が出たとき『変換の不具合』と『読み込みの不具合』を切り分けられます。",
                MessageType.Info);

            if (GUILayout.Button("StreamingAssets へ synthetic.afhr を書き出す"))
                ExportSynthetic();

            EditorGUILayout.Space(12);
            EditorGUILayout.LabelField(".afhr の点検", EditorStyles.boldLabel);
            using (new EditorGUILayout.HorizontalScope())
            {
                _inspectPath = EditorGUILayout.TextField(_inspectPath);
                if (GUILayout.Button("選択", GUILayout.Width(60)))
                {
                    string p = EditorUtility.OpenFilePanel("HRTF (.afhr) を選択",
                                                           Application.streamingAssetsPath, "afhr");
                    if (!string.IsNullOrEmpty(p)) _inspectPath = p;
                }
            }
            if (GUILayout.Button("読み込んで点検")) Inspect();

            if (!string.IsNullOrEmpty(_report))
            {
                EditorGUILayout.Space(8);
                EditorGUILayout.TextArea(_report, GUILayout.ExpandHeight(true));
            }
        }

        private void ExportSynthetic()
        {
            int sr = AudioSettings.outputSampleRate;
            if (sr <= 0) sr = 48000;
            var set = HrtfSet.CreateSynthetic(sr);

            string dir = Application.streamingAssetsPath;
            if (!Directory.Exists(dir)) Directory.CreateDirectory(dir);
            string path = Path.Combine(dir, "synthetic.afhr");

            if (!WriteAfhr(path, set))
            {
                _report = "書き出しに失敗しました。";
                return;
            }
            AssetDatabase.Refresh();

            // 往復検証：書いたものを読み直して、方向数・IR長が一致するか確かめる。
            var reloaded = HrtfSet.LoadFromFile(path);
            if (reloaded == null || !reloaded.IsValid)
            {
                _report = $"書き出しはできましたが読み直せませんでした: {path}\n" +
                          "→ HrtfSet の読み込み側に不具合があります。";
                return;
            }
            bool ok = reloaded.DirectionCount == set.DirectionCount &&
                      reloaded.IrLength == set.IrLength &&
                      reloaded.SampleRate == set.SampleRate;
            _report = $"書き出し: {path}\n" +
                      $"往復検証: {(ok ? "一致（読み込み経路は健全）" : "不一致！")}\n" +
                      $"  方向数 {set.DirectionCount} → {reloaded.DirectionCount}\n" +
                      $"  IR長   {set.IrLength} → {reloaded.IrLength}\n" +
                      $"  SR     {set.SampleRate} → {reloaded.SampleRate}\n\n" +
                      "IrConvolver の Hrtf File Name に \"synthetic.afhr\" を入れると、\n" +
                      "内蔵の合成HRTFではなくこのファイル経由で読み込まれます。";
        }

        // HrtfSet と同じ形式で書く。※ITDは書き出し時点で HRIR から分離済みなので、
        // 読み直すと ITD=0 になる（形式は生の HRIR を持つ前提のため）。
        // ここは「読み込み経路の検証」が目的なので、方向数/IR長/SR の一致で判定する。
        private static bool WriteAfhr(string path, HrtfSet set)
        {
            try
            {
                using (var fs = new FileStream(path, FileMode.Create, FileAccess.Write))
                using (var bw = new BinaryWriter(fs))
                {
                    bw.Write(0x52484641u);          // magic 'AFHR'
                    bw.Write(1);                    // version
                    bw.Write(set.SampleRate);
                    bw.Write(set.DirectionCount);
                    bw.Write(set.IrLength);

                    for (int i = 0; i < set.DirectionCount; i++)
                    {
                        Vector3 v = DirOf(set, i);
                        // ベクトル → (az, el) 度。az: 0=正面, +90=右。
                        float az = Mathf.Atan2(v.x, v.z) * Mathf.Rad2Deg;
                        float el = Mathf.Asin(Mathf.Clamp(v.y, -1f, 1f)) * Mathf.Rad2Deg;
                        bw.Write(az);
                        bw.Write(el);
                    }
                    for (int i = 0; i < set.DirectionCount; i++)
                    {
                        var l = set.GetHrir(i, 0);
                        var r = set.GetHrir(i, 1);
                        for (int k = 0; k < set.IrLength; k++) bw.Write(l != null && k < l.Length ? l[k] : 0f);
                        for (int k = 0; k < set.IrLength; k++) bw.Write(r != null && k < r.Length ? r[k] : 0f);
                    }
                }
                return true;
            }
            catch (System.Exception e)
            {
                Debug.LogError($"[HRTF Tools] 書き出し失敗: {e.Message}");
                return false;
            }
        }

        // 方向 index の単位ベクトルを得る（NearestIndex を使った逆引き）。
        private static Vector3 DirOf(HrtfSet set, int index)
        {
            // HrtfSet は方向配列を公開していないので、走査で index に一致する向きを探す。
            // 書き出しは頻繁に行わないので、この程度のコストは許容する。
            const int kAz = 72, kEl = 20;
            for (int e = 0; e <= kEl; e++)
            {
                float el = -40f + e * 5f;
                for (int a = 0; a < kAz; a++)
                {
                    float az = a * 5f; if (az > 180f) az -= 360f;
                    Vector3 v = HrtfSet.AngleToVector(az, el);
                    if (set.NearestIndex(v) == index) return v;
                }
            }
            return Vector3.forward;
        }

        private void Inspect()
        {
            if (string.IsNullOrEmpty(_inspectPath) || !File.Exists(_inspectPath))
            {
                _report = "ファイルが見つかりません。";
                return;
            }
            var set = HrtfSet.LoadFromFile(_inspectPath);
            if (set == null || !set.IsValid)
            {
                _report = "読み込めませんでした（形式が違うか壊れています）。";
                return;
            }

            // ITD の範囲を確認する。人間の頭では最大 ±0.7ms 程度。
            // これを大きく外れていたら座標系（左右）や単位の取り違えが疑われる。
            float minItd = float.MaxValue, maxItd = float.MinValue;
            for (int i = 0; i < set.DirectionCount; i++)
            {
                float t = set.GetItdSeconds(i);
                minItd = Mathf.Min(minItd, t);
                maxItd = Mathf.Max(maxItd, t);
            }

            int sr = AudioSettings.outputSampleRate;
            string srWarn = (sr > 0 && set.SampleRate != sr)
                ? $"\n⚠ 出力SR({sr}Hz)と違います。ITDがずれて定位が狂うので変換し直してください。"
                : "";

            // 右方向の音源で「右耳が早い」か（＝ITDが負）を確認する。符号の取り違え検出。
            int rightIdx = set.NearestIndex(new Vector3(1, 0, 0));
            float rightItd = set.GetItdSeconds(rightIdx);
            string signCheck = rightItd < 0f
                ? "OK（右の音源で右耳が早い）"
                : $"⚠ 符号が逆かもしれません（右の音源なのに ITD={rightItd * 1000f:F3}ms）";

            _report =
                $"名前: {set.Name}\n" +
                $"方向数: {set.DirectionCount}\n" +
                $"IR長: {set.IrLength} タップ ({set.IrLength * 1000f / Mathf.Max(1, set.SampleRate):F1} ms)\n" +
                $"SR: {set.SampleRate} Hz{srWarn}\n" +
                $"ITD 範囲: {minItd * 1000f:F3} 〜 {maxItd * 1000f:F3} ms" +
                (Mathf.Max(Mathf.Abs(minItd), Mathf.Abs(maxItd)) > 0.0012f
                    ? "  ⚠ 人間の頭(±0.7ms程度)より大きい。座標系か単位を確認"
                    : "  （妥当な範囲）") + "\n" +
                $"左右の符号: {signCheck}";
        }
    }
}
