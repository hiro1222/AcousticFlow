// TuningWindow.cs — 調整卓の **Editor 窓**版。
//
//   メニュー: AcousticFlow > 調整卓 (この地点の聞こえ方)
//
// ★ゲーム内パネル（F9）との違いは**見た目だけ**。
//   計算と書き込みは `TuningCore`（ランタイム側）に 1 つだけ置いてある。
//   ここに書き写すと必ずずれるので、写経しないこと。
//
//   | | ゲーム内 F9 | この窓 |
//   |---|---|---|
//   | 動く場所 | ビルドでも | **Editor だけ** |
//   | 画面 | Game ビューを塞ぐ | ドッキングできる。塞がない |
//   | 見え方 | 実機の見た目のまま触れる | 数字が読みやすい |
//
// ⚠ 再生中でないと何も出ない。届いている音を**その場で**採る道具なので。
using UnityEditor;
using UnityEngine;

namespace AcousticFlow.EditorTools
{
    public class TuningWindow : EditorWindow
    {
        private readonly TuningCore _core = new TuningCore();
        private float _targetLevelDb = -30f;
        private float _targetMuffle = 4f;
        private Vector2 _scroll;

        [MenuItem("AcousticFlow/調整卓 (この地点の聞こえ方)")]
        public static void Open()
        {
            var w = GetWindow<TuningWindow>("調整卓");
            w.minSize = new Vector2(420f, 460f);
            w.Show();
        }

        private void OnEnable() { EditorApplication.update += Tick; }
        private void OnDisable() { EditorApplication.update -= Tick; }

        // 歩けば変わるので毎フレーム採り直す。★窓を開いているあいだだけ。
        private void Tick()
        {
            if (!EditorApplication.isPlaying) return;
            if (_core.Sample()) Repaint();
        }

        private void OnGUI()
        {
            if (!EditorApplication.isPlaying)
            {
                EditorGUILayout.HelpBox(
                    "再生中に、いま立っている場所での聞こえ方を決めます。\n"
                    + "同じ道具はゲーム内でも F9 で開けます（ビルドでも動きます）。",
                    MessageType.Info);
                return;
            }
            if (!TuningCore.Ready)
            {
                EditorGUILayout.HelpBox("AcousticFlowSceneDemo がまだ動いていません。", MessageType.Warning);
                return;
            }

            _scroll = EditorGUILayout.BeginScrollView(_scroll);

            EditorGUILayout.LabelField("この地点での聞こえ方を決めます", EditorStyles.boldLabel);

            // ── 音源を選ぶ ──
            string[] names = TuningCore.SourceNames();
            if (names.Length == 0)
            {
                EditorGUILayout.HelpBox("音源がまだありません。", MessageType.Warning);
                EditorGUILayout.EndScrollView();
                return;
            }
            _core.SourceIndex = Mathf.Clamp(_core.SourceIndex, 0, names.Length - 1);
            _core.SourceIndex = EditorGUILayout.Popup("音源", _core.SourceIndex, names);

            // ── いま届いている量 ──
            EditorGUILayout.Space(4);
            EditorGUILayout.LabelField("いま届いている音（合成後 / 透過だけ）", EditorStyles.boldLabel);
            for (int b = 0; b < TuningCore.NB; b++)
            {
                float c = _core.Combined[b], t = _core.TransOnly[b];
                EditorGUILayout.LabelField(
                    string.Format("  {0,6:F0} Hz    合成 {1,7:F1} dB    透過 {2,7:F1} dB    回り込み {3:F4}",
                                  TuningCore.BandHz[b], TuningCore.Db(c), TuningCore.Db(t),
                                  Mathf.Max(0f, c - t)));
            }
            EditorGUILayout.LabelField(string.Format("  総量 {0:F1} dB    こもり倍率 {1:F2}",
                                                     _core.LevelDbNow, _core.MuffleNow),
                                       EditorStyles.boldLabel);

            // ── 担い手 ──
            EditorGUILayout.Space(4);
            EditorGUILayout.LabelField("誰が担っているか（透過損失の大きい順・触る相手を選ぶ）",
                                       EditorStyles.boldLabel);
            if (_core.CarrierCount == 0)
                EditorGUILayout.HelpBox("遮る物がありません（見通せています）。", MessageType.Info);
            for (int i = 0; i < _core.CarrierCount; i++)
            {
                using (new EditorGUILayout.HorizontalScope())
                {
                    bool pick = (_core.CarrierPick == i);
                    if (GUILayout.Toggle(pick, "", EditorStyles.radioButton, GUILayout.Width(18f)) && !pick)
                        _core.CarrierPick = i;
                    EditorGUILayout.LabelField(
                        string.Format("実体 {0} / 材質 {1} / 透過損失 {2:F1} dB",
                                      _core.CarrierInstance(i), _core.CarrierMaterial(i),
                                      _core.CarrierLossDb(i)));
                }
            }
            if (_core.CarrierCount > 0)
                EditorGUILayout.HelpBox("材質は共有です。書き換えると、それを使う実体が全部変わります。",
                                        MessageType.Warning);

            // ── 目標 ──
            EditorGUILayout.Space(6);
            EditorGUILayout.LabelField("こう聞こえてほしい", EditorStyles.boldLabel);
            _targetLevelDb = EditorGUILayout.Slider("総量 (dB)", _targetLevelDb, -80f, 0f);
            _targetMuffle = EditorGUILayout.Slider("こもり (125Hz÷4kHz)", _targetMuffle, 0.5f, 12f);

            using (new EditorGUI.DisabledScope(_core.CarrierCount == 0))
                if (GUILayout.Button("この地点でこの音にする", GUILayout.Height(24f)))
                    _core.ApplyTarget(_targetLevelDb, _targetMuffle);

            if (!string.IsNullOrEmpty(_core.LastResult))
                EditorGUILayout.HelpBox(_core.LastResult, MessageType.None);

            EditorGUILayout.Space(6);
            EditorGUILayout.HelpBox(
                "合うのは「この 1 点」です。他の場所は物理が決めます"
                + "（どこでも目標どおりなら、それはベイクと同じ）。\n"
                + "合わせたあと、その経路を歩いて曲線が壊れていないか必ず確かめてください。",
                MessageType.None);

            EditorGUILayout.EndScrollView();
        }
    }
}
