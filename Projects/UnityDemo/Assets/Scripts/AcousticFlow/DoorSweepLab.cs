// ★旧コア（Demo / Test_* の場面）。新コア（Flow_* の場面 ＝ AcousticWorld 系）では使っていない。
//   2026-09-24 の棚卸しで印を付けた。試聴で新コアへの乗り換えが決まったら、旧コアの C++ ごと消す（段 10）。
//   それまでは聞き比べの基準として残す。──代わり: 扉を自動で振って測る道具。新コアの場面で要るなら、旧コアへの参照を外して移す
// DoorSweepLab.cs
// 収録用の操作卓。**同じ動きを毎回同じ時間で**再生するためのもの。
//
// ★何のためにあるか
//   扉の開き角は SwingDoor が 5/6 キーの押しっぱなしで動かせるが、
//   手で押すと**テイクごとに速さが変わる**。比較動画では「同じ 3 秒で開いた」ことが
//   前提になるので、そこが揺れると音の違いなのか開け方の違いなのか分からなくなる。
//   ここではボタン 1 つで 開 3 秒 → 停 2 秒 → 閉 3 秒 を固定で流す。
//
//   SwingDoor.autoSwing との違い: あちらは往復し続ける（止まらない・停止区間が無い）。
//   収録は「1 回だけ、途中で止まる」ほうが要るので別に持つ。
//
// ★音源の X は 3 箇所を行き来する
//   Test_SwingDoor は 戸口 x ∈ [-0.5, +0.5] / リスナー z = -3 / 音源 z = +3。
//   直線が z=0 を横切る X は音源の X のちょうど半分なので:
//     x = 0   … 横切る点 0.00 … 戸口の**ど真ん中を通る**（開けば直接音が抜ける）
//     x = ±1  … 横切る点 0.50 … **戸口の縁ちょうど＝影境界の上**
//   ★±1 は「通る／通らない」の境目そのもの。この作品が潰してきたのは
//     「連続量に二値の判定を置くと跨いだ瞬間に音が跳ぶ」型の欠陥なので、
//     跳ねが残っていればここでいちばん出る。逆に言えば**いちばん厳しい置き方**。
//     深い影の側で見たいときは ±1.5（横切る点 0.75 ＝ 縁の 0.25m 外）にする。
//   同じ扉の動きに対して**この 3 つで鳴り方が変わる**ことが見せたいものなので、
//   位置は数値で決め打ちにして、押すたび同じ場所へ戻るようにしてある。
//
// ★角度を書くのはこのスクリプト。SwingDoor のキーは流している間だけ止める
//   同じフレームで 2 か所から angleDeg を書くと、どちらが勝ったのか分からなくなる。
//   流し終わったら元に戻す（勝手に無効のままにしない）。
//
// 操作:
//   T        開閉テイクを開始（実行中は中止）
//   7 / 8 / 9  音源を x = -3 / 0 / +3 へ
//   画面のボタンでも同じことができる（収録中にキーを覚えなくていいように）
using UnityEngine;

namespace AcousticFlow
{
    [DefaultExecutionOrder(-90)]   // SwingDoor(0) より先に角度を書く
    [AddComponentMenu("AcousticFlow/Door Sweep Lab")]
    public sealed class DoorSweepLab : MonoBehaviour
    {
        [Header("対象")]
        [Tooltip("動かす扉。未設定ならシーンから 1 つ目を拾う。")]
        public SwingDoor door;
        [Tooltip("X を動かす音源の番号。0 = AcousticFlowSceneDemo.source、"
                 + "1 以降 = extraSources。")]
        public int sourceIndex = 0;

        [Header("開閉テイク（秒）")]
        [Tooltip("開き切るまでの時間。")]
        public float openSeconds = 3f;
        [Tooltip("開いたまま止めておく時間。")]
        public float holdSeconds = 2f;
        [Tooltip("閉じ切るまでの時間。")]
        public float closeSeconds = 3f;
        [Tooltip("開き切ったときの角度(度)。SwingDoor の上限が 120° なのでそこまで。")]
        [Range(10f, 120f)] public float openAngleDeg = 120f;

        [Header("音源の X 位置")]
        [Tooltip("ボタンと 7/8/9 キーが置く X 座標。Y と Z は動かさない。")]
        public float[] sourceXs = { -1f, 0f, 1f };

        [Header("操作")]
        public bool enableHotkeys = true;
        [Tooltip("テイクの開始／中止。")]
        public KeyCode startKey = KeyCode.T;
        [Tooltip("ON: 画面左下にボタンと状態を出す。")]
        public bool showGui = true;

        private enum Phase { Idle, Opening, Hold, Closing }
        private Phase _phase = Phase.Idle;
        private float _t;
        private float _startAngle;
        private bool _keysWereEnabled;

        private AcousticFlowSceneDemo _demo;
        private Transform[] _srcs;

        private void Update()
        {
            ResolveRefs();
            if (enableHotkeys) HandleHotkeys();
            Advance(Time.deltaTime);
        }

        private void HandleHotkeys()
        {
            if (Input.GetKeyDown(startKey))
            {
                if (_phase == Phase.Idle) StartTake();
                else AbortTake();
            }
            if (Input.GetKeyDown(KeyCode.Alpha7)) MoveSourceTo(0);
            if (Input.GetKeyDown(KeyCode.Alpha8)) MoveSourceTo(1);
            if (Input.GetKeyDown(KeyCode.Alpha9)) MoveSourceTo(2);
        }

        // ── テイク ───────────────────────────────────────────────────────
        /// <summary>開 → 停 → 閉 を 1 回だけ流す。</summary>
        public void StartTake()
        {
            if (door == null) return;
            _startAngle = door.angleDeg;   // ★いまの角度から開く（閉から押せば仕様どおり）
            _phase = Phase.Opening;
            _t = 0f;
            // 流している間だけ SwingDoor 側の入力を止める。往復も止める。
            _keysWereEnabled = door.enableKeys;
            door.enableKeys = false;
            door.autoSwing = false;
        }

        /// <summary>途中で止める。角度はその場に置いたままにする（勝手に閉じない）。</summary>
        public void AbortTake()
        {
            if (_phase == Phase.Idle) return;
            _phase = Phase.Idle;
            _t = 0f;
            if (door != null) door.enableKeys = _keysWereEnabled;
        }

        private void Advance(float dt)
        {
            if (_phase == Phase.Idle || door == null) return;
            _t += dt;

            switch (_phase)
            {
                case Phase.Opening:
                {
                    float dur = Mathf.Max(0.01f, openSeconds);
                    float u = Mathf.Clamp01(_t / dur);
                    door.angleDeg = Mathf.Lerp(_startAngle, openAngleDeg, u);
                    if (u >= 1f) { door.angleDeg = openAngleDeg; _phase = Phase.Hold; _t = 0f; }
                    break;
                }
                case Phase.Hold:
                {
                    door.angleDeg = openAngleDeg;
                    if (_t >= Mathf.Max(0f, holdSeconds)) { _phase = Phase.Closing; _t = 0f; }
                    break;
                }
                case Phase.Closing:
                {
                    float dur = Mathf.Max(0.01f, closeSeconds);
                    float u = Mathf.Clamp01(_t / dur);
                    door.angleDeg = Mathf.Lerp(openAngleDeg, 0f, u);
                    if (u >= 1f)
                    {
                        door.angleDeg = 0f;
                        _phase = Phase.Idle;
                        _t = 0f;
                        door.enableKeys = _keysWereEnabled;   // ★戻す
                    }
                    break;
                }
            }
        }

        private string PhaseText()
        {
            switch (_phase)
            {
                case Phase.Opening: return $"開き中  {_t:F1} / {openSeconds:F1} s";
                case Phase.Hold: return $"停止中  {_t:F1} / {holdSeconds:F1} s";
                case Phase.Closing: return $"閉じ中  {_t:F1} / {closeSeconds:F1} s";
                default: return "待機";
            }
        }

        // ── 音源 ─────────────────────────────────────────────────────────
        /// <summary>sourceXs[i] の X へ置く。Y と Z は触らない。</summary>
        public void MoveSourceTo(int i)
        {
            if (sourceXs == null || i < 0 || i >= sourceXs.Length) return;
            Transform s = CurrentSource();
            if (s == null) return;
            Vector3 p = s.position;
            s.position = new Vector3(sourceXs[i], p.y, p.z);
        }

        private Transform CurrentSource()
        {
            if (_srcs == null || _srcs.Length == 0) return null;
            int i = Mathf.Clamp(sourceIndex, 0, _srcs.Length - 1);
            return _srcs[i];
        }

        private void ResolveRefs()
        {
            if (door == null) door = FindFirstObjectByType<SwingDoor>();
            if (_demo == null) _demo = FindFirstObjectByType<AcousticFlowSceneDemo>();
            if (_demo == null) { _srcs = null; return; }

            int extra = (_demo.extraSources != null) ? _demo.extraSources.Length : 0;
            int n = (_demo.source != null ? 1 : 0) + extra;
            if (_srcs == null || _srcs.Length != n) _srcs = new Transform[n];
            int k = 0;
            if (_demo.source != null) _srcs[k++] = _demo.source;
            for (int i = 0; i < extra && k < n; i++) _srcs[k++] = _demo.extraSources[i];
        }

        // ── HUD ───────────────────────────────────────────────────────────
        private void OnGUI()
        {
            if (!showGui) return;
            ResolveRefs();

            var st = new GUIStyle(GUI.skin.label) { fontSize = 13, richText = true };
            GUILayout.BeginArea(new Rect(12f, Screen.height - 186f, 460f, 174f), GUI.skin.box);

            GUILayout.Label("<b>収録卓</b>   "
                            + $"<color=#ffd479>開 {openSeconds:F0}s → 停 {holdSeconds:F0}s"
                            + $" → 閉 {closeSeconds:F0}s</color>", st);

            float ang = (door != null) ? door.angleDeg : 0f;
            GUILayout.Label($"扉  <b>{ang:F1}°</b>    {PhaseText()}", st);

            GUILayout.BeginHorizontal();
            if (GUILayout.Button(_phase == Phase.Idle ? "開閉テイクを流す (T)" : "中止 (T)",
                                 GUILayout.Height(24f)))
            {
                if (_phase == Phase.Idle) StartTake(); else AbortTake();
            }
            GUILayout.EndHorizontal();

            GUILayout.Space(4);
            Transform cur = CurrentSource();
            GUILayout.Label("<b>音源の X</b>   "
                            + (cur != null ? $"いま <b>{cur.position.x:+0.00;-0.00;0.00}</b>"
                                           + $"（{cur.name}）"
                                           : "<color=#ffa0a0>音源が見つかりません</color>"), st);

            GUILayout.BeginHorizontal();
            int nx = (sourceXs != null) ? sourceXs.Length : 0;
            for (int i = 0; i < nx; i++)
            {
                bool on = cur != null && Mathf.Abs(cur.position.x - sourceXs[i]) < 0.01f;
                string label = $"x = {sourceXs[i]:+0.#;-0.#;0}" + (i < 3 ? $"  ({7 + i})" : "");
                if (on) label = "▶ " + label;
                if (GUILayout.Button(label, GUILayout.Height(24f))) MoveSourceTo(i);
            }
            GUILayout.EndHorizontal();

            if (_srcs != null && _srcs.Length > 1)
            {
                string others = "";
                for (int i = 0; i < _srcs.Length; i++)
                    if (_srcs[i] != null)
                        others += $"{i}:{_srcs[i].name} x={_srcs[i].position.x:F2}   ";
                GUILayout.Label("<color=#a0c0ff>動かすのは 1 本だけ。全体: " + others + "</color>", st);
            }

            GUILayout.EndArea();
        }
    }
}
