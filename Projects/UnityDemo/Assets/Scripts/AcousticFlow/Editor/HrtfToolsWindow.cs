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
        private string _kemarDir = "";
        private string _report = "";

        private void ImportKemar()
        {
            int sr = AudioSettings.outputSampleRate;
            if (sr <= 0) sr = 48000;

            string dir = Application.streamingAssetsPath;
            if (!Directory.Exists(dir)) Directory.CreateDirectory(dir);
            string path = Path.Combine(dir, "kemar.afhr");

            if (!KemarImporter.ImportToFile(_kemarDir, sr, path, out string importLog))
            {
                _report = "取り込み失敗:\n" + importLog;
                return;
            }
            AssetDatabase.Refresh();

            // 書いたものを読み直して点検まで通す（変換の妥当性をその場で確認する）。
            _inspectPath = path;
            var check = HrtfSet.LoadFromFile(path);
            string verdict = (check != null && check.IsValid)
                ? $"読み直し OK: {check.DirectionCount} 方向 / IR {check.IrLength} タップ / {check.SampleRate}Hz"
                : "⚠ 読み直しに失敗しました。";

            _report = importLog + "\n" + verdict + "\n\n" +
                      "IrConvolver の Hrtf File Name に \"kemar.afhr\" を入れて Play してください。\n" +
                      "下の「読み込んで点検」で ITD 範囲と左右の符号も確認できます。";
        }

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
            EditorGUILayout.LabelField("MIT KEMAR の取り込み", EditorStyles.boldLabel);
            EditorGUILayout.HelpBox(
                "compact.zip を展開したフォルダ（.dat を含む）を指定します。\n" +
                "https://sound.media.mit.edu/resources/KEMAR/compact.zip\n" +
                "※ページ内のリンクは http なのでブラウザにブロックされることがあります。https で直接開いてください。\n\n" +
                "ライセンス: 著者クレジットを明記すれば自由に利用・再配布できます。\n" +
                "  Bill Gardner and Keith Martin, MIT Media Lab (1994)",
                MessageType.Info);
            using (new EditorGUILayout.HorizontalScope())
            {
                _kemarDir = EditorGUILayout.TextField(_kemarDir);
                if (GUILayout.Button("選択", GUILayout.Width(60)))
                {
                    string p = EditorUtility.OpenFolderPanel("KEMAR compact フォルダを選択", "", "");
                    if (!string.IsNullOrEmpty(p)) _kemarDir = p;
                }
            }
            if (GUILayout.Button("変換して StreamingAssets へ書き出す")) ImportKemar();

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

            // 合成HRTFは ITD を別に持つので、遅延として付け直してから書く。
            // そのまま書くと読み直したとき ITD=0 になり定位が消える（AfhrWriter 冒頭参照）。
            if (!AfhrWriter.WriteWithItd(path, set))
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
