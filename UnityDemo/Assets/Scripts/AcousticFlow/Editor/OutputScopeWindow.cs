/* OutputScopeWindow.cs
 * 畳み込み後の「実際に鳴っている波形」を見るオシロスコープ。
 *
 *   メニュー: AcousticFlow > Output Scope で開く。
 *
 * 既存の Output Monitor との違い:
 *   Output Monitor は AcousticEngine_GetOutputLevels＝Wwise の出力レベルを読む。
 *   IR 畳み込み経路は Wwise を通らない（Unity の OnAudioFilterRead で自前出力する）ので、
 *   あちらには何も映らない。こちらは IrConvolver.Scope＝畳み込み器が実際に書いた波形を読む。
 *
 * 段別 RMS を併記しているのが要点:
 *   直接 / 早期(鏡面) / 散乱(拡散) / 後期尾 のどれが鳴っていて、どれが鳴っていないかが
 *   一目で分かる。「空間感が減った」の切り分けは、まずこの内訳を見るのが速い。
 */
using UnityEditor;
using UnityEngine;
using AcousticFlow;

namespace AcousticFlow.EditorTools
{
    public class OutputScopeWindow : EditorWindow
    {
        [MenuItem("AcousticFlow/Output Scope")]
        public static void Open()
        {
            var w = GetWindow<OutputScopeWindow>("Output Scope");
            w.minSize = new Vector2(420f, 320f);
            w.Show();
        }

        private void OnEnable() { EditorApplication.update += Repaint; }
        private void OnDisable() { EditorApplication.update -= Repaint; }

        private bool _syncToClick = true;
        private float _windowMs = 400f;
        private float _yScale = 1f;
        private bool _logY;

        private void OnGUI()
        {
            if (!EditorApplication.isPlaying)
            {
                EditorGUILayout.HelpBox(
                    "Play 中に、IR畳み込みの実出力波形と段別レベルを表示します。\nシーンを再生してください。",
                    MessageType.Info);
                return;
            }

            int sr = IrConvolver.Scope.SampleRate;
            if (sr <= 0)
            {
                EditorGUILayout.HelpBox(
                    "IrConvolver がまだ音を出していません。\n" +
                    "シーンに IrConvolver 付きの GameObject があるか確認してください。",
                    MessageType.Warning);
                return;
            }

            using (new EditorGUILayout.HorizontalScope())
            {
                _syncToClick = GUILayout.Toggle(_syncToClick, "クリック同期", EditorStyles.miniButton, GUILayout.Width(90));
                _logY = GUILayout.Toggle(_logY, "dB表示", EditorStyles.miniButton, GUILayout.Width(70));
                GUILayout.Label("窓", GUILayout.Width(20));
                _windowMs = GUILayout.HorizontalSlider(_windowMs, 20f, 680f, GUILayout.Width(90));
                GUILayout.Label($"{_windowMs:F0} ms", GUILayout.Width(55));
                GUILayout.Label("拡大", GUILayout.Width(30));
                _yScale = GUILayout.HorizontalSlider(_yScale, 0.2f, 40f, GUILayout.Width(90));
                GUILayout.Label($"×{_yScale:F1}", GUILayout.Width(40));
            }

            DrawWave(sr);
            DrawLevels();
        }

        private void DrawWave(int sr)
        {
            Rect r = GUILayoutUtility.GetRect(10f, 10000f, 120f, 100000f);
            EditorGUI.DrawRect(r, new Color(0.09f, 0.09f, 0.11f));

            int len = IrConvolver.Scope.BufferLength;
            int count = Mathf.Clamp(Mathf.RoundToInt(_windowMs * 0.001f * sr), 16, len - 1);

            // 表示の開始位置。クリック同期ならトリガ位置から、そうでなければ「今」から遡る。
            int start;
            if (_syncToClick && IrConvolver.Scope.TriggerPos >= 0)
                start = IrConvolver.Scope.TriggerPos;
            else
                start = (IrConvolver.Scope.WritePos - count + len) % len;

            Handles.BeginGUI();

            // 中央線とグリッド（10ms 刻み）。
            Handles.color = new Color(1f, 1f, 1f, 0.12f);
            float midY = r.y + r.height * 0.5f;
            Handles.DrawLine(new Vector3(r.x, midY), new Vector3(r.xMax, midY));
            for (float ms = 0f; ms <= _windowMs; ms += 10f)
            {
                float x = r.x + (ms / _windowMs) * r.width;
                Handles.color = new Color(1f, 1f, 1f, (ms % 50f < 0.01f) ? 0.18f : 0.07f);
                Handles.DrawLine(new Vector3(x, r.y), new Vector3(x, r.yMax));
            }

            DrawChannel(IrConvolver.Scope.L, start, count, len, r, new Color(0.35f, 0.75f, 1f, 0.9f), -1);
            DrawChannel(IrConvolver.Scope.R, start, count, len, r, new Color(1f, 0.55f, 0.35f, 0.9f), 1);

            Handles.EndGUI();

            GUI.Label(new Rect(r.x + 6, r.y + 2, 260, 16),
                      _syncToClick ? "青=L / 橙=R（クリック位置から）" : "青=L / 橙=R（現在から遡る）",
                      EditorStyles.miniLabel);
        }

        // 1チャンネル分を min/max エンベロープで描く（1px に複数サンプルが乗るため）。
        private void DrawChannel(float[] buf, int start, int count, int len, Rect r, Color c, int sign)
        {
            Handles.color = c;
            int px = Mathf.Max(1, Mathf.RoundToInt(r.width));
            float midY = r.y + r.height * 0.5f;
            float half = r.height * 0.5f;

            for (int i = 0; i < px; i++)
            {
                int a = start + (int)((long)i * count / px);
                int b = start + (int)((long)(i + 1) * count / px);
                if (b <= a) b = a + 1;
                float mn = float.MaxValue, mx = float.MinValue;
                for (int s = a; s < b; s++)
                {
                    float v = buf[((s % len) + len) % len];
                    if (v < mn) mn = v;
                    if (v > mx) mx = v;
                }
                if (mn > mx) { mn = 0f; mx = 0f; }

                float x = r.x + i;
                float yTop = midY - Map(mx, half);
                float yBot = midY - Map(mn, half);
                Handles.DrawLine(new Vector3(x, yTop), new Vector3(x, yBot));
            }
        }

        // 振幅 → ピクセル。dB表示のときは符号を保ったまま対数に潰す。
        private float Map(float v, float half)
        {
            float a = v * _yScale;
            if (_logY)
            {
                float m = Mathf.Abs(a);
                float db = 20f * Mathf.Log10(Mathf.Max(m, 1e-5f));   // -100dB でフルスケール下端
                float t = Mathf.Clamp01((db + 100f) / 100f);
                a = Mathf.Sign(v) * t;
            }
            return Mathf.Clamp(a, -1f, 1f) * half;
        }

        private void DrawLevels()
        {
            EditorGUILayout.Space(4);
            EditorGUILayout.LabelField("段別レベル（RMS）", EditorStyles.boldLabel);

            Meter("直接音", IrConvolver.Scope.RmsDirect, new Color(0.95f, 0.85f, 0.3f));
            Meter("早期反射(鏡面)", IrConvolver.Scope.RmsEarly, new Color(0.4f, 0.85f, 1f));
            Meter("散乱(拡散)", IrConvolver.Scope.RmsScatter, new Color(0.6f, 1f, 0.6f));
            Meter("後期尾", IrConvolver.Scope.RmsTail, new Color(1f, 0.55f, 0.35f));
            Meter("最終出力", IrConvolver.Scope.RmsOut, new Color(0.9f, 0.9f, 0.9f));

            EditorGUILayout.Space(2);
            int parts = IrConvolver.Scope.TailPartitions;
            EditorGUILayout.LabelField(
                parts > 0
                    ? $"実測尾: 有効（{IrConvolver.Scope.ActiveParts}/{parts} パーティション稼働）　"
                      + $"早期↔後期の境目 {IrConvolver.Scope.SplitMs:F0} ms（mixing time √V）"
                    : "実測尾: IR 未設定（tailMode が Measured でない / 帯域別エコグラムが無い / 尾のエネルギーが0）",
                EditorStyles.miniLabel);

            // ※ ここは必ず毎回同じ数だけ描く。値の有無で描画を分けると、Layout パスと
            //    Repaint パスの間に audio thread が値を書き換えたときコントロール数が食い違い、
            //    "Getting control N's position in a group with only N controls" で落ちる。
            float tail = IrConvolver.Scope.RmsTail;
            float early = IrConvolver.Scope.RmsEarly + IrConvolver.Scope.RmsScatter;
            float dr = IrConvolver.Scope.RmsDirect;

            EditorGUILayout.LabelField(
                early > 1e-6f ? $"尾 / 早期 = {tail / early:F2}（空間感の目安。小さいほど乾く）" : "尾 / 早期 = —",
                EditorStyles.miniLabel);

            // 目標比（知覚圧縮後）と、実際にレンダリングされた尾/直接パワー比を並べる。
            // 較正が正しければ「実測 尾/直接」≒「√目標」になるはず。
            float target = Mathf.Max(0f, IrConvolver.Scope.TailToDirectRatio);
            EditorGUILayout.LabelField(
                dr > 1e-6f
                    ? $"尾 / 直接 = {tail / dr:F2}（目標比 {target:F2} → 目標 尾/直接 {Mathf.Sqrt(target):F2}）"
                    : "尾 / 直接 = —",
                EditorStyles.miniLabel);

            // 圧縮の効きを見る。物理そのままだと部屋間で振れ幅が大きすぎるので指数で寄せている。
            float phys = Mathf.Max(0f, IrConvolver.Scope.PhysicalRatio);
            EditorGUILayout.LabelField(
                phys > 1e-6f
                    ? $"物理比 (r/r_c)² = {phys:F2} → 知覚圧縮後 {target:F2}"
                    : "物理比 (r/r_c)² = —",
                EditorStyles.miniLabel);
        }

        private void Meter(string label, float rms, Color c)
        {
            Rect row = GUILayoutUtility.GetRect(10f, 10000f, 16f, 16f);
            Rect lab = new Rect(row.x, row.y, 110f, row.height);
            Rect bar = new Rect(row.x + 114f, row.y + 3f, Mathf.Max(10f, row.width - 114f - 70f), row.height - 6f);
            Rect num = new Rect(bar.xMax + 4f, row.y, 66f, row.height);

            EditorGUI.LabelField(lab, label, EditorStyles.miniLabel);
            EditorGUI.DrawRect(bar, new Color(0.15f, 0.15f, 0.17f));

            // dB スケール（-60dB 〜 0dB）で描く。線形だと小さい値が全く見えない。
            float db = 20f * Mathf.Log10(Mathf.Max(rms, 1e-6f));
            float t = Mathf.Clamp01((db + 60f) / 60f);
            EditorGUI.DrawRect(new Rect(bar.x, bar.y, bar.width * t, bar.height), c);
            EditorGUI.LabelField(num, rms > 1e-6f ? $"{db:F1} dB" : "—", EditorStyles.miniLabel);
        }
    }
}
