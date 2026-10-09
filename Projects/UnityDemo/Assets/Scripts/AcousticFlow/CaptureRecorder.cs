// ★旧コア（Demo / Test_* の場面）。新コア（Flow_* の場面 ＝ AcousticWorld 系）では使っていない。
//   2026-09-24 の棚卸しで印を付けた。試聴で新コアへの乗り換えが決まったら、旧コアの C++ ごと消す（段 10）。
//   それまでは聞き比べの基準として残す。──代わり: 録音（旧コアの場面から）。新コアで要るなら engine 側の口に繋ぎ直す
// CaptureRecorder.cs ── サウンドデバッグツールの録音側（Unity 側の操作）。
//
// 仕様: docs/SOUND_DEBUG_TOOL.md
//
// ★何をする物か（一行）
//   **常時録音**しておいて、「いま変だった」と思ったらキーを 1 つ押す。
//   押した時点の**前 21 秒**と**後 5 秒**が 1 ファイル（.afcap）に落ちる。
//
// ★なぜ「録画開始」を押させないのか
//   変な音は**気づいたときには終わっている**。押してから録るのでは間に合わない。
//   だから常に溜めておいて、押したら**さかのぼって**切り出す。
//
// ★なぜ判定をここでしないのか（実時間で警告を出さない理由）
//   検出器を後で良くしたときに、**昔のキャプチャへ付け直せる**から。
//   実時間で判定すると録った瞬間の判定で固定される。
//   → ここは溜めるだけ。破れの走査は録音後（C++ 側 capture_scan.h）。
//
// ★なぜ AudioListener に付けるのか
//   マスターPCM（実際に何が聞こえたか）が要る。AudioListener に付けたフィルタは
//   **全音源のミックス後**に走るので、ここで拾えば出音そのものが録れる。
//
// ⚠ 診断の列は workerThreads = 1 でしか信用できない（診断カウンタがスレッドごとなので）。
//   きっちり原因を追うときは workerThreads を 1 にして録ること。
//   ただし直列と並列では音響値がビット一致しないので、
//   **「直列で録ったキャプチャ」と「並列で動く本番」は厳密には別物**である、と自覚しておく。
using System.IO;
using UnityEngine;

namespace AcousticFlow
{
    [AddComponentMenu("AcousticFlow/Capture Recorder")]
    [RequireComponent(typeof(AudioListener))]
    public class CaptureRecorder : MonoBehaviour
    {
        [Tooltip("常時録音する。OFF にすると溜めるのを止めます（費用はほぼゼロなので通常 ON）。")]
        public bool recording = true;

        [Tooltip("「いま変だった」を押すキー。押した時点の前後が 1 ファイルに落ちます。")]
        public KeyCode markKey = KeyCode.F10;

        [Tooltip("押した時点より前を何秒残すか。\n\n"
                 + "⚠ 既定が 21 秒なのは、**畳み込み器の遅延線が冷えている**ぶんを捨てるため。"
                 + "T-21s から流して T-20s から見せます。器の内部状態を保存する代わりに"
                 + "1 秒余分に録って暖めます。")]
        [Range(2f, 40f)] public float prerollSeconds = 21f;

        [Tooltip("押してから何秒残すか。")]
        [Range(1f, 15f)] public float postrollSeconds = 5f;

        [Tooltip("マスターPCM（実際に聞こえた音）も録る。OFF だと軽くなりますが、"
                 + "「エンジンを直す前と後」を並べられなくなります。")]
        public bool recordMasterPcm = true;

        [Tooltip("同時に録る音源の上限。")]
        [Range(4, 128)] public int maxSources = 64;

        [Tooltip("画面にキャプチャの状態を出す。")]
        public bool showStatus = true;

        private AcousticScene _scene;
        private float[] _scratch;
        private int _lastStatus = -1;
        private string _lastSaved = "";
        private float _savedFlashUntil;

        [RuntimeInitializeOnLoadMethod(RuntimeInitializeLoadType.AfterSceneLoad)]
        private static void Attach()
        {
            if (FindFirstObjectByType<CaptureRecorder>() != null) return;
            var listener = FindFirstObjectByType<AudioListener>();
            if (listener != null) listener.gameObject.AddComponent<CaptureRecorder>();
        }

        /// 保存先。★決めるのは `CaptureBrowser` の 1 箇所。
        ///   録る側と見る側で別々に決めると、**書いた場所と探す場所がずれる**。
        public static string CaptureDir { get { return CaptureBrowser.CaptureDir; } }

        // ⚠ シーンが取れなくても**無効化しない**。
        //   AcousticFlowSceneDemo.SharedScene はデモの OnEnable で入る。順番は合っているが、
        //   デモが無効だったり初期化に失敗すると null のままになる。そこで enabled=false に
        //   すると**二度と復帰しない**（Unity は自分で有効化し直さない）ので、取れるまで待つ。
        private void OnEnable() { TryBind(); }

        private bool TryBind()
        {
            if (_scene != null) return true;
            _scene = AcousticFlowSceneDemo.SharedScene;
            if (_scene == null) return false;
            CheckFilterOrder();
            BeginRecording();
            return true;
        }

        private void OnDisable()
        {
            if (_scene != null) _scene.CaptureEnd();
        }

        // ★★ 同じ AudioListener に付いた OnAudioFilterRead は**コンポーネントの並び順**に走る ★★
        //   TailBusRenderer は後期残響の尾を**ここで**ミックスへ足している。
        //   もし録音側が先に走ると、**尾の入っていない音**を「実際に聞こえた音」として
        //   保存してしまう ── しかも静かに間違うので、聞いても気づけない。
        //   → 並び順を見て、危なければはっきり警告する（実行時には並べ替えられないため）。
        private void CheckFilterOrder()
        {
            var tail = GetComponent<TailBusRenderer>();
            if (tail == null) return;                    // 尾を共有していないなら関係ない
            var all = GetComponents<MonoBehaviour>();    // ★並び順で返る
            int me = -1, other = -1;
            for (int i = 0; i < all.Length; ++i)
            {
                if (all[i] == this) me = i;
                else if (all[i] == tail) other = i;
            }
            if (me >= 0 && other >= 0 && me < other)
                Debug.LogWarning(
                    "[AcousticFlow] CaptureRecorder が TailBusRenderer より前にあります。"
                    + "このままだとマスターPCM に後期残響の尾が入りません"
                    + "（OnAudioFilterRead はコンポーネントの並び順に走るため）。"
                    + "インスペクタで CaptureRecorder を TailBusRenderer より下へ移動してください。");
        }

        private void BeginRecording()
        {
            if (_scene == null || !recording) return;
            // ★フレーム単位で進める前提なので、秒 → フレームは 60fps 換算で固定する。
            //   実 fps で換算すると、録るたびに窓の長さが変わって比べられなくなる。
            int pre = Mathf.Max(1, Mathf.RoundToInt(prerollSeconds * 60f));
            int post = Mathf.Max(0, Mathf.RoundToInt(postrollSeconds * 60f));
            _scene.CaptureBegin(pre, post, maxSources, AudioSettings.outputSampleRate,
                                recordMasterPcm);
            _lastStatus = -1;
        }

        private void Update()
        {
            if (!TryBind()) return;

            if (Input.GetKeyDown(markKey))
            {
                _scene.CaptureMark();
                _savedFlashUntil = Time.unscaledTime + 1.0f;
            }

            int held;
            int st = _scene.CaptureStatus(out held);
            // 2 = 前後が揃った。★ここで書き出して、すぐ次の録音を始める
            //   （書き出しのあいだ録れていない時間ができるが、フレーム番号は
            //     新しいキャプチャで 0 から振り直すので、時刻の対応は崩れない）。
            if (st == 2 && _lastStatus != 2) SaveAndRestart();
            _lastStatus = st;
        }

        private void SaveAndRestart()
        {
            try { Directory.CreateDirectory(CaptureDir); }
            catch (System.Exception e) { Debug.LogWarning("[AcousticFlow] 保存先を作れません: " + e.Message); return; }

            string sceneName = UnityEngine.SceneManagement.SceneManager.GetActiveScene().name;
            string file = string.Format("{0}_{1:yyyyMMdd_HHmmss}.afcap", sceneName, System.DateTime.Now);
            string path = Path.Combine(CaptureDir, file);

            // ★DLL の識別子をヘッダに焼く。**古い物を掴んだまま気づかない**のがこの
            //   プロジェクトの弱点なので（実際、配り忘れて一日ぶん古いエンジンで音を聞いた）、
            //   開いたときに現物と違えば赤く言えるようにしておく。
            uint dllHash = DllHash();
            bool ok = _scene.CaptureWrite(path, sceneName, dllHash);
            if (ok)
            {
                _lastSaved = path;
                _savedFlashUntil = Time.unscaledTime + 4f;
                Debug.Log("[AcousticFlow] キャプチャを保存: " + path);
            }
            else Debug.LogWarning("[AcousticFlow] キャプチャの保存に失敗: " + path);

            BeginRecording();
        }

        /// 配っている DLL のハッシュ。中身を読まずにサイズと更新時刻から作る簡易版
        /// （目的は「同じ物か」の照合であって、暗号学的な強度ではない）。
        private static uint DllHash()
        {
            try
            {
                string p = Path.Combine(Application.dataPath, "Plugins/x86_64/AcousticEngine.dll");
                if (!File.Exists(p)) return 0u;
                var fi = new FileInfo(p);
                ulong v = (ulong)fi.Length * 2654435761UL
                        ^ (ulong)fi.LastWriteTimeUtc.Ticks;
                return (uint)(v ^ (v >> 32));
            }
            catch (System.Exception) { return 0u; }
        }

        // ⚠ オーディオスレッド。確保もロックもしないこと。
        private void OnAudioFilterRead(float[] data, int channels)
        {
            if (_scene == null || !recording || channels < 1) return;
            int frames = data.Length / channels;
            if (channels == 2)
            {
                _scene.CapturePushAudio(data, frames);
                return;
            }
            // ステレオ以外はステレオへ畳んで渡す（記録の形を 1 つに保つ）。
            //   ★確保はここでは避けたいが、チャンネル数が変わるのは起動時だけなので
            //     初回に 1 回だけ確保して使い回す。
            int need = frames * 2;
            if (_scratch == null || _scratch.Length < need) _scratch = new float[need];
            for (int i = 0; i < frames; ++i)
            {
                float m = data[i * channels];
                _scratch[i * 2] = m;
                _scratch[i * 2 + 1] = channels > 1 ? data[i * channels + 1] : m;
            }
            _scene.CapturePushAudio(_scratch, frames);
        }

        private void OnGUI()
        {
            if (!showStatus) return;
            if (_scene == null)
            {
                // ★黙って何も出さないと「付いていないのか壊れているのか」が分からない。
                GUI.Label(new Rect(8, Screen.height - 46, 520, 20),
                          "録音: 待機（AcousticFlowSceneDemo がまだ動いていません）");
                return;
            }
            int held;
            int st = _scene.CaptureStatus(out held);
            string label;
            if (st == 0) label = "録音: 停止";
            else if (st == 2) label = "録音: 保存中…";
            else label = string.Format("録音中 {0:0.0}s 保持  [{1}] で印", held / 60f, markKey);

            var r = new Rect(8, Screen.height - 46, 420, 20);
            GUI.Label(r, label);
            if (Time.unscaledTime < _savedFlashUntil && !string.IsNullOrEmpty(_lastSaved))
                GUI.Label(new Rect(8, Screen.height - 26, 900, 20),
                          "保存: " + _lastSaved);
        }
    }
}
