// StockUnityCompare.cs — 「Unity 標準だけで作った版」と AcousticFlow を Y キーで聴き比べる。
//
// ★何のためか
//   AcousticFlow が何をしているのかは、**何もしない状態と比べないと分からない**。
//   そこで、ゲーム業界でいちばん普通に使われているやり方を、同じシーンに実装して並べる。
//
// ★標準版の中身（藁人形にしない。実務で使われている作り方をそのまま）
//   1. リスナー→音源へレイを複数本撃ち、通った割合を「遮蔽度」にする（ソフト遮蔽）
//   2. 遮蔽度からローパスの遮断周波数と音量を決める
//   3. 空間化は Unity 内蔵のパンナー（spatialBlend = 1）
//   4. 残響は AudioReverbFilter（プリセット）
//   5. 急変を避けるため時定数で平滑する
//   これは Steam Audio も Wwise も無い環境での定番構成で、多くの製品がこの形。
//
// ★標準版に**原理的にできないこと**（ここが比較の要点）
//   ・音が**開口の方向**から来ない。扉が開いていても音源の方角から鳴る
//   ・**帯域ごとの開き方**が出ない。ローパス 1 段なので「高い音だけ通る」までしか作れない
//   ・遮蔽の境目が**レイの二値**なので、跨いだ瞬間に段差が出る（本数を増やすと緩むが消えない）
//   ・扉の**開き角**そのものは音に出ない。レイが通るか通らないかだけ
//
// ★使い方
//   Y キーで切り替え。画面に今どちらで鳴っているかが出る。
using System.Collections.Generic;
using UnityEngine;

namespace AcousticFlow
{
    [AddComponentMenu("AcousticFlow/Stock Unity Compare")]
    public class StockUnityCompare : MonoBehaviour
    {
        // シーンに手を入れず、実行時に自分で付く。
        //   このシーンは本体プロジェクトの複製なので、1 行も触らないでおきたい。
        [RuntimeInitializeOnLoadMethod(RuntimeInitializeLoadType.AfterSceneLoad)]
        private static void Attach()
        {
            if (FindFirstObjectByType<StockUnityCompare>() != null) return;
            var demo = FindFirstObjectByType<AcousticFlowSceneDemo>();
            if (demo != null) demo.gameObject.AddComponent<StockUnityCompare>();
        }

        public enum Mode { AcousticFlow, StockUnity }

        [Header("切替")]
        public KeyCode toggleKey = KeyCode.Y;
        public Mode mode = Mode.AcousticFlow;

        [Header("標準版の設定")]
        [Tooltip("遮蔽を測るレイの本数。1 本だと二値になるので、実務では数本撃つのが普通。")]
        [Range(1, 16)] public int occlusionRays = 8;
        [Tooltip("レイを散らす半径(m)。音源と耳のまわりにこの範囲で散らす。")]
        [Range(0f, 1.5f)] public float raySpread = 0.4f;
        [Tooltip("完全に遮られたときのローパス遮断周波数(Hz)。")]
        [Range(200f, 5000f)] public float occludedCutoffHz = 900f;
        [Tooltip("完全に遮られたときの音量倍率。")]
        [Range(0f, 1f)] public float occludedVolume = 0.35f;
        [Tooltip("遮蔽度の追従の速さ（秒）。小さいほど機敏だが段差が出やすい。")]
        [Range(0.01f, 0.5f)] public float smoothTime = 0.08f;
        [Tooltip("標準版で使う残響プリセット。部屋の材質に近いものを選ぶ。")]
        public AudioReverbPreset reverbPreset = AudioReverbPreset.StoneCorridor;

        private AcousticFlowSceneDemo _demo;
        private Transform _listener;
        private readonly List<Entry> _entries = new List<Entry>();

        // 音源 1 つぶんの持ち物。
        private class Entry
        {
            public Transform tr;
            public AudioSource src;
            public VoiceConvolver voice;
            public AudioLowPassFilter lpf;
            public AudioReverbFilter rev;
            public float occ;        // 0=素通し / 1=完全に遮蔽
            public float occVel;     // 平滑用
            public int lastBlocked;  // 直近で遮られたレイの本数（表示用）
        }

        private void Start()
        {
            _demo = FindFirstObjectByType<AcousticFlowSceneDemo>();
            if (_demo != null) _listener = _demo.listener;
            if (_listener == null && Camera.main != null) _listener = Camera.main.transform;

            var list = new List<Transform>();
            if (_demo != null)
            {
                if (_demo.source != null) list.Add(_demo.source);
                if (_demo.extraSources != null)
                    foreach (var t in _demo.extraSources) if (t != null) list.Add(t);
            }
            foreach (var t in list)
            {
                var src = t.GetComponent<AudioSource>();
                if (src == null) continue;
                var e = new Entry { tr = t, src = src, voice = t.GetComponent<VoiceConvolver>() };
                // ★フィルタは常に載せておき、mode で有効/無効を切り替える。
                //   実行中に AddComponent すると音が途切れるので、最初に用意する。
                e.lpf = t.GetComponent<AudioLowPassFilter>() ?? t.gameObject.AddComponent<AudioLowPassFilter>();
                e.rev = t.GetComponent<AudioReverbFilter>() ?? t.gameObject.AddComponent<AudioReverbFilter>();
                e.lpf.cutoffFrequency = 22000f;
                e.rev.reverbPreset = reverbPreset;
                _entries.Add(e);
            }
            Apply();   // 起動時の状態を反映
        }

        private void Update()
        {
            if (Input.GetKeyDown(toggleKey))
            {
                mode = (mode == Mode.AcousticFlow) ? Mode.StockUnity : Mode.AcousticFlow;
                Apply();
            }
            if (mode == Mode.StockUnity) UpdateStock();
        }

        // 経路の切替。**どちらか片方だけ**が鳴るようにする。
        private void Apply()
        {
            bool stock = (mode == Mode.StockUnity);
            foreach (var e in _entries)
            {
                if (e.voice != null) e.voice.enabled = !stock;
                // 標準版は Unity 内蔵のパンナーに空間化させる。
                // AcousticFlow 版は畳み込みで両耳化するので、内蔵は切る（二重にしない）。
                e.src.spatialBlend = stock ? 1f : 0f;
                if (e.lpf != null) e.lpf.enabled = stock;
                if (e.rev != null) e.rev.enabled = stock;
                if (!stock) { e.src.volume = 1f; }
            }
            Debug.Log("[Compare] " + (stock
                ? "Unity 標準（レイ遮蔽＋ローパス＋内蔵パンナー＋リバーブ）"
                : "AcousticFlow（幾何音響：回折・開口積分・実測 HRTF・実測エコグラム）"));
        }

        // 標準版の毎フレーム処理：レイで遮蔽度を測り、ローパスと音量へ写す。
        private void UpdateStock()
        {
            if (_listener == null) return;
            foreach (var e in _entries)
            {
                if (e.tr == null || e.src == null) continue;

                // ── 遮蔽度をレイで測る ──
                //   1 本だと通る/通らないの二値になるので、音源と耳のまわりに散らして
                //   「何本通ったか」の割合にする。実務でよく使われる緩和のしかた。
                int blocked = 0;
                for (int i = 0; i < occlusionRays; i++)
                {
                    Vector3 a = _listener.position, b = e.tr.position;
                    if (i > 0 && raySpread > 0f)
                    {
                        // 決め打ちの散らし方（乱数だとフレームごとに値が揺れて雑音になる）。
                        float ang = i * 2.3999632f;      // 黄金角
                        var off = new Vector3(Mathf.Cos(ang), Mathf.Sin(ang) * 0.5f, Mathf.Sin(ang))
                                  * (raySpread * (0.3f + 0.7f * i / occlusionRays));
                        a += off; b -= off;
                    }
                    Vector3 d = b - a;
                    float len = d.magnitude;
                    if (len < 1e-3f) continue;
                    if (Physics.Raycast(a, d / len, len - 0.01f)) blocked++;
                }
                e.lastBlocked = blocked;
                float target = (occlusionRays > 0) ? (float)blocked / occlusionRays : 0f;
                e.occ = Mathf.SmoothDamp(e.occ, target, ref e.occVel, smoothTime);

                // ── 遮蔽度からローパスと音量へ ──
                //   遮断周波数は対数で動かす（線形だと効きが偏る）。
                float f = Mathf.Lerp(Mathf.Log(22000f), Mathf.Log(occludedCutoffHz), e.occ);
                if (e.lpf != null) e.lpf.cutoffFrequency = Mathf.Exp(f);
                e.src.volume = Mathf.Lerp(1f, occludedVolume, e.occ);
            }
        }

        private void OnGUI()
        {
            var st = new GUIStyle(GUI.skin.label) { fontSize = 15, richText = true };
            st.normal.textColor = Color.white;
            GUILayout.BeginArea(new Rect(Screen.width - 430f, 12f, 418f, 150f), GUI.skin.box);
            bool stock = (mode == Mode.StockUnity);
            GUILayout.Label(stock
                ? "<b><color=#ffa0a0>Unity 標準</color></b>  ─ レイ遮蔽＋ローパス＋内蔵パンナー"
                : "<b><color=#a0ffa0>AcousticFlow</color></b>  ─ 幾何音響（回折・開口積分・実測HRTF）", st);
            GUILayout.Label($"<b>{toggleKey}</b> キーで切り替え", st);
            GUILayout.Space(4);
            if (stock)
            {
                var d = FindFirstObjectByType<SwingDoor>();
                if (d != null) GUILayout.Label($"  扉 <b>{d.angleDeg:F0}°</b>", st);
                foreach (var e in _entries)
                    GUILayout.Label($"  {e.tr.name}: 遮蔽 <b>{e.occ:F2}</b> "
                                    + $"({e.lastBlocked}/{occlusionRays} 本) / "
                                    + $"LPF {(e.lpf != null ? e.lpf.cutoffFrequency : 0f):F0} Hz "
                                    + $"/ 音量 {e.src.volume:F2}", st);
                GUILayout.Label("<color=#ffd479>※ 音は開口ではなく音源の方角から鳴ります</color>", st);
            }
            else
            {
                GUILayout.Label("  音は<b>開口の方角</b>から鳴り、開き具合が<b>音色</b>に出ます", st);
            }
            GUILayout.EndArea();
        }
    }
}
