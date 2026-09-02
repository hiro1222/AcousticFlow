// TuningPanel.cs — 「この地点でこう聞こえてほしい」に合わせるための調整卓。
//
// ★何のための道具か
//   このエンジンの主題は「周辺環境の変化を音で伝える」こと。だから調整の単位は
//   **立っている場所での聞こえ方**であって、材質の数値ではない。
//   材質の表を睨んで 0.003 を 0.005 にする作業は、耳から遠すぎて追い込めない。
//
//   ここでは:
//     ① いまリスナーに届いている音の内訳を出す（透過ぶん / 回り込みぶん）
//     ② 透過が担っているなら、**誰が担っているか**をエンジンに名指しさせる
//     ③ 「こう聞こえてほしい」を 2 つのつまみで置く（総量 / こもり倍率）
//     ④ 担い手の材質を書き換えて、その場で音が変わる
//     ⑤ 届かない目標は**届かないと言う**（黙って外さない）
//
// ★書き込み先は必ず物理量（材質）にする
//   「この音源だけ EQ を掛ける」形にすると、壁を動かしても補正が付いてこない。
//   実行時に形状が変わる前提の音響エンジン、という位置づけが崩れて、ただのミキサーになる。
//   だから入口は音源でも、**書くのは材質**。
//
// ★合うのは「その 1 点」だけ。それが狙い
//   材質は 1 枚で聞く場所は無限にある。ある点で目標に合わせれば、他の点はそこから
//   物理が決めた値になる。どこでも目標どおりに鳴るなら、それはベイクしたのと同じで、
//   環境は何も語っていない。**代表の 1 点で演出を決め、残りは物理**（決めごと #3）。
//
// ★担い手が複数のときは人が選ぶ
//   取り分が近いと、自動で選ぶ方式は触るたびに相手が入れ替わる。
//   「勝手に何かが変わった」がこの手の道具のいちばんの事故なので、一覧から選ばせる。
using UnityEngine;

namespace AcousticFlow
{
    [AddComponentMenu("AcousticFlow/Tuning Panel")]
    public class TuningPanel : MonoBehaviour
    {
        [Tooltip("調整卓を開くキー。")]
        public KeyCode toggleKey = KeyCode.F9;
        public bool open = false;

        [Header("目標（この地点でこう聞こえてほしい）")]
        [Tooltip("総量。届く音の広帯域レベル（dB）。0 が素通し、負ほど小さい。")]
        [Range(-80f, 0f)] public float targetLevelDb = -30f;
        [Tooltip("こもり倍率。125Hz ÷ 4kHz。1=素直 / 大きいほどこもる。\n"
                 + "ゲームレーンが扉の実測で使っている尺度と同じもの（閉 5.8 → 開 1.2）。")]
        [Range(0.5f, 12f)] public float targetMuffle = 4f;

        // ★計算と書き込みは持たない。`TuningCore` に 1 つだけ置いてある。
        //   同じ道具の Editor 窓版（AcousticFlow / 調整卓）も同じ core を使う。
        //   ここに書き写すと必ずずれる（決めごと #1: 同じ問いに 2 つの答えを持たせない）。
        private readonly TuningCore _core = new TuningCore();
        private const int NB = TuningCore.NB;
        private Vector2 _scroll;

        // ★シーンに手を入れずに実行時へ付く。
        //   シーンは他レーンとも共有していて、`.unity` を触ると差分が大きく出るため。
        //   道具はシーンの一部ではなく「その場で開くもの」なので、これが筋。
        [RuntimeInitializeOnLoadMethod(RuntimeInitializeLoadType.AfterSceneLoad)]
        private static void Attach()
        {
            if (FindFirstObjectByType<TuningPanel>() != null) return;
            var demo = FindFirstObjectByType<AcousticFlowSceneDemo>();
            if (demo != null) demo.gameObject.AddComponent<TuningPanel>();
        }

        private void Update()
        {
            if (Input.GetKeyDown(toggleKey)) open = !open;
            if (open) _core.Sample();
        }

        private void OnGUI()
        {
            if (!open) return;
            var st = new GUIStyle(GUI.skin.label) { fontSize = 13, richText = true };

            // 右下に置く。
            //   ★左上を空けるのは、そこに他の表示が集まっているため
            //     （F11 のキャプチャ一覧が左上、録音の状態が左下）。重ならない場所へ。
            //   ⚠ Game ビューが小さいと 560x520 が入らない。はみ出すと**閉じるキーの
            //     案内ごと画面外**へ行って操作不能になるので、画面に収まるまで縮める。
            const float kPad = 12f, kMaxW = 560f, kMaxH = 520f;
            float w = Mathf.Min(kMaxW, Screen.width - kPad * 2f);
            float h = Mathf.Min(kMaxH, Screen.height - kPad * 2f);
            var area = new Rect(Screen.width - w - kPad, Screen.height - h - kPad, w, h);
            GUILayout.BeginArea(area, GUI.skin.box);
            _scroll = GUILayout.BeginScrollView(_scroll);

            GUILayout.Label($"<b>調整卓</b>（{toggleKey} で閉じる）  "
                            + "<color=#ffd479>この地点での聞こえ方を決めます</color>", st);
            GUILayout.Label("<color=#a0c0ff>同じ道具は Editor の "
                            + "AcousticFlow / 調整卓 でも開けます（画面を塞ぎません）。</color>", st);

            // 音源を選ぶ
            var taps = AcousticFlowSceneDemo.Status.Taps;
            int n = (taps != null) ? taps.Length : 0;
            GUILayout.BeginHorizontal();
            GUILayout.Label("音源", st, GUILayout.Width(40f));
            for (int i = 0; i < n && i < 12; i++)
                if (GUILayout.Toggle(_core.SourceIndex == i, i.ToString(),
                                     EditorLikeButton(), GUILayout.Width(28f)))
                    _core.SourceIndex = i;
            GUILayout.EndHorizontal();

            // いま届いている量
            GUILayout.Space(4);
            GUILayout.Label("<b>いま届いている音</b>（合成後 / 透過だけ）", st);
            for (int b = 0; b < NB; b++)
                GUILayout.Label($"   {TuningCore.BandHz[b],6:F0} Hz"
                                + $"   合成 {TuningCore.Db(_core.Combined[b]),7:F1} dB"
                                + $"   透過 {TuningCore.Db(_core.TransOnly[b]),7:F1} dB"
                                + $"   回り込み {Mathf.Max(0f, _core.Combined[b] - _core.TransOnly[b]):F4}", st);
            GUILayout.Label($"   総量 <b>{_core.LevelDbNow:F1} dB</b>   "
                            + $"こもり倍率 <b>{_core.MuffleNow:F2}</b>", st);

            // 担い手
            GUILayout.Space(4);
            GUILayout.Label("<b>誰が担っているか</b>（透過損失の大きい順・触る相手を選ぶ）", st);
            if (_core.CarrierCount == 0)
                GUILayout.Label("   <color=#ffa0a0>遮る物がありません（見通せています）</color>", st);
            for (int i = 0; i < _core.CarrierCount; i++)
            {
                GUILayout.BeginHorizontal();
                if (GUILayout.Toggle(_core.CarrierPick == i, "", GUILayout.Width(20f)))
                    _core.CarrierPick = i;
                GUILayout.Label($"実体 {_core.CarrierInstance(i)} / 材質 {_core.CarrierMaterial(i)}"
                                + $" / 透過損失 {_core.CarrierLossDb(i):F1} dB", st);
                GUILayout.EndHorizontal();
            }
            GUILayout.Label("   <color=#ffd479>⚠ 材質は共有です。書き換えるとそれを使う実体が"
                            + "全部変わります。</color>", st);

            // 目標
            GUILayout.Space(6);
            GUILayout.Label("<b>こう聞こえてほしい</b>", st);
            GUILayout.BeginHorizontal();
            GUILayout.Label($"総量 {targetLevelDb:F1} dB", st, GUILayout.Width(120f));
            targetLevelDb = GUILayout.HorizontalSlider(targetLevelDb, -80f, 0f);
            GUILayout.EndHorizontal();
            GUILayout.BeginHorizontal();
            GUILayout.Label($"こもり {targetMuffle:F2} 倍", st, GUILayout.Width(120f));
            targetMuffle = GUILayout.HorizontalSlider(targetMuffle, 0.5f, 12f);
            GUILayout.EndHorizontal();

            if (GUILayout.Button("この地点でこの音にする"))
                _core.ApplyTarget(targetLevelDb, targetMuffle);
            if (!string.IsNullOrEmpty(_core.LastResult))
                GUILayout.Label("   " + _core.LastResult, st);

            GUILayout.Space(6);
            GUILayout.Label("<color=#a0c0ff>合うのは<b>この 1 点</b>です。他の場所は物理が決めます"
                            + "（どこでも目標どおりなら、それはベイクと同じ）。</color>", st);
            GUILayout.Label("<color=#a0c0ff>合わせたあと、その経路を歩いて曲線が壊れていないか"
                            + "必ず確かめてください。</color>", st);

            GUILayout.EndScrollView();
            GUILayout.EndArea();
        }

        private static GUIStyle EditorLikeButton()
        {
            var s = new GUIStyle(GUI.skin.button);
            s.padding = new RectOffset(2, 2, 2, 2);
            return s;
        }
    }
}
