// ResizableRoom.cs
// 囲まれた部屋（床＋天井＋四方壁）の内寸を実行中に変えられるようにするコンポーネント。
//
// 仕組み:
//   壁は6枚の BoxCollider 付きキューブ。AcousticFlowSceneDemo は毎フレーム
//   occluder の Transform を読んでエンジン側インスタンスを更新している
//   （AcousticFlowSceneDemo.Update の「動的ジオメトリ更新」）ので、
//   ここで壁を動かすだけで 遮蔽/回折/反射/残響 が全部そのまま追従する。
//   → このスクリプトは Unity の Transform を書き換えるだけでよく、
//     エンジン API を直接叩く必要はない。
//
// 注意:
//   壁の生成は Awake で行う。AcousticFlowSceneDemo は OnEnable で
//   BoxCollider を収集するため、そちらより先に壁が存在していないと
//   部屋が遮蔽物として登録されない。DefaultExecutionOrder で先行させている。
using UnityEngine;

namespace AcousticFlow
{
    [DefaultExecutionOrder(-100)]
    public sealed class ResizableRoom : MonoBehaviour
    {
        [Header("寸法")]
        [Tooltip("部屋の内寸(幅x, 高y, 奥z)。実行中に書き換えると即座に壁が動く。")]
        public Vector3 innerSize = new Vector3(8f, 3f, 6f);

        [Tooltip("壁の厚み(m)。透過量に効くので、途中で変えると聞こえ方も変わる。")]
        public float thickness = 0.4f;

        [Tooltip("内寸の下限(m)。これ以下には縮まない。")]
        public Vector3 minSize = new Vector3(2f, 2f, 2f);

        [Tooltip("内寸の上限(m)。")]
        public Vector3 maxSize = new Vector3(60f, 30f, 60f);

        [Header("操作")]
        [Tooltip("ON: [ ] キーで拡縮、1/2/3 でプリセット切替。")]
        public bool enableHotkeys = true;

        [Tooltip("キー押しっぱなしの拡縮速度(m/秒)。")]
        public float resizeSpeed = 4f;

        [Tooltip("ON: 部屋を縮めたときリスナーを内側へ押し戻す（壁にめり込んで無音になるのを防ぐ）。")]
        public bool keepListenerInside = true;

        [Tooltip("押し戻し対象。未設定なら AcousticFlowSceneDemo の listener を自動で使う。")]
        public Transform listener;

        [Tooltip("ON: 画面左下に現在の内寸と操作説明を出す。")]
        public bool showGui = true;

        [Header("構造オプション")]
        [Tooltip("ON: 床を作る。OFF: 作らない（外に大きな地面がある場合、"
                 + "そこと二重になって遮蔽・反射が二重計上されるのを避ける）。")]
        public bool makeFloor = true;
        [Tooltip("ON: 南壁(-Z側)に出入り口を開ける。外から中に入って響きの変化を聴く検証用。")]
        public bool makeDoorway = false;
        [Tooltip("出入り口の幅(m)。")]
        [Range(0.6f, 6f)] public float doorwayWidth = 2f;
        [Tooltip("出入り口の高さ(m)。内寸の高さを超える場合は高さいっぱいになる。")]
        [Range(1.5f, 6f)] public float doorwayHeight = 2.2f;
        [Tooltip("出入り口の中心の X 位置(m, 部屋中心が0)。")]
        public float doorwayCenterX = 0f;

        [Header("プリセット (1/2/3 キー)")]
        public Vector3 presetSmall = new Vector3(4f, 2.5f, 3f);
        public Vector3 presetMedium = new Vector3(10f, 4f, 8f);
        public Vector3 presetLarge = new Vector3(30f, 12f, 24f);

        // 6枚の壁。順に 床/天井/東/西/北/南。
        private Transform[] _walls;
        private static readonly string[] WallNames =
            {
                "Floor", "Ceiling", "Wall_E", "Wall_W", "Wall_N",
                "Wall_S",        // 開口なしのときの南壁（1枚）
                "Wall_S_L", "Wall_S_R", "Wall_S_Top",  // 開口ありのときの南壁（3分割）
            };

        // 直前に反映した値。Inspector を実行中にドラッグした場合の検知に使う。
        private Vector3 _appliedSize;
        private float _appliedThickness;

        private void Awake()
        {
            EnsureWalls();
            ApplyNow();
        }

        // 子として6枚の壁を用意する。既にあれば作り直さず再利用する。
        private void EnsureWalls()
        {
            _walls = new Transform[WallNames.Length];
            for (int i = 0; i < WallNames.Length; i++)
            {
                Transform w = transform.Find(WallNames[i]);
                if (w == null)
                {
                    var go = GameObject.CreatePrimitive(PrimitiveType.Cube);
                    go.name = WallNames[i];
                    go.transform.SetParent(transform, false);
                    w = go.transform;
                }
                _walls[i] = w;
            }
        }

        private void Update()
        {
            if (enableHotkeys) HandleHotkeys();

            // Inspector 直接編集にも追従する（実行中にドラッグして聞き比べる用）。
            if (innerSize != _appliedSize || !Mathf.Approximately(thickness, _appliedThickness))
                ApplyNow();
        }

        private void HandleHotkeys()
        {
            if (Input.GetKeyDown(KeyCode.Alpha1)) { innerSize = presetSmall; return; }
            if (Input.GetKeyDown(KeyCode.Alpha2)) { innerSize = presetMedium; return; }
            if (Input.GetKeyDown(KeyCode.Alpha3)) { innerSize = presetLarge; return; }

            float dir = 0f;
            if (Input.GetKey(KeyCode.RightBracket)) dir += 1f;   // ] 広げる
            if (Input.GetKey(KeyCode.LeftBracket)) dir -= 1f;    // [ 狭める
            if (dir == 0f) return;

            // X/Y/Z を押しながらだとその軸だけ。押していなければ全軸まとめて。
            bool ax = Input.GetKey(KeyCode.X), ay = Input.GetKey(KeyCode.Y), az = Input.GetKey(KeyCode.Z);
            bool anyAxis = ax || ay || az;
            float d = dir * resizeSpeed * Time.deltaTime;

            innerSize += new Vector3(
                (!anyAxis || ax) ? d : 0f,
                (!anyAxis || ay) ? d : 0f,
                (!anyAxis || az) ? d : 0f);
        }

        // 内寸を壁の Transform に反映する。座標は親(この GameObject)基準のローカル。
        // 床の上面が y=0、部屋の中心が原点になるように置く。
        public void ApplyNow()
        {
            if (_walls == null) EnsureWalls();

            innerSize = new Vector3(
                Mathf.Clamp(innerSize.x, minSize.x, maxSize.x),
                Mathf.Clamp(innerSize.y, minSize.y, maxSize.y),
                Mathf.Clamp(innerSize.z, minSize.z, maxSize.z));
            thickness = Mathf.Max(0.05f, thickness);

            float w = innerSize.x, h = innerSize.y, d = innerSize.z, t = thickness;
            float hw = w * 0.5f, hd = d * 0.5f;

            SetActive(0, makeFloor);
            if (makeFloor)
                Set(0, new Vector3(0f, -t * 0.5f, 0f), new Vector3(w + t * 2f, t, d + t * 2f));  // 床
            Set(1, new Vector3(0f, h + t * 0.5f, 0f), new Vector3(w + t * 2f, t, d + t * 2f));   // 天井
            Set(2, new Vector3(hw + t * 0.5f, h * 0.5f, 0f), new Vector3(t, h, d));              // 東
            Set(3, new Vector3(-hw - t * 0.5f, h * 0.5f, 0f), new Vector3(t, h, d));             // 西
            Set(4, new Vector3(0f, h * 0.5f, hd + t * 0.5f), new Vector3(w, h, t));              // 北

            ApplySouthWall(w, h, d, t, hw, hd);

            _appliedSize = innerSize;
            _appliedThickness = thickness;

            if (keepListenerInside && Application.isPlaying) ClampListener();
        }

        // 南壁(-Z側)。開口なしなら1枚、開口ありなら左右＋まぐさの3枚に分ける。
        //   開口の縁が回折エッジになるので、幅・高さは実寸で作る（見た目だけの穴にしない）。
        private void ApplySouthWall(float w, float h, float d, float t, float hw, float hd)
        {
            float z = -hd - t * 0.5f;

            if (!makeDoorway)
            {
                SetActive(5, true);
                Set(5, new Vector3(0f, h * 0.5f, z), new Vector3(w, h, t));
                SetActive(6, false); SetActive(7, false); SetActive(8, false);
                return;
            }

            SetActive(5, false);

            // 開口の範囲を壁の内側に収める。
            float dw = Mathf.Min(doorwayWidth, w);
            float dh = Mathf.Min(doorwayHeight, h);
            float cx = Mathf.Clamp(doorwayCenterX, -hw + dw * 0.5f, hw - dw * 0.5f);
            float left = cx - dw * 0.5f;      // 開口の左端
            float right = cx + dw * 0.5f;     // 開口の右端

            // 左側の壁（-hw 〜 left）。幅が無ければ無効化する。
            float wl = left - (-hw);
            SetActive(6, wl > 1e-3f);
            if (wl > 1e-3f)
                Set(6, new Vector3(-hw + wl * 0.5f, h * 0.5f, z), new Vector3(wl, h, t));

            // 右側の壁（right 〜 hw）。
            float wr = hw - right;
            SetActive(7, wr > 1e-3f);
            if (wr > 1e-3f)
                Set(7, new Vector3(right + wr * 0.5f, h * 0.5f, z), new Vector3(wr, h, t));

            // まぐさ（開口の上）。開口が天井まで届いていれば不要。
            float lintel = h - dh;
            SetActive(8, lintel > 1e-3f);
            if (lintel > 1e-3f)
                Set(8, new Vector3(cx, dh + lintel * 0.5f, z), new Vector3(dw, lintel, t));
        }

        private void Set(int i, Vector3 localPos, Vector3 localScale)
        {
            var w = _walls[i];
            if (w == null) return;
            w.localPosition = localPos;
            w.localRotation = Quaternion.identity;
            w.localScale = localScale;
        }

        private void SetActive(int i, bool on)
        {
            var w = _walls[i];
            if (w != null && w.gameObject.activeSelf != on) w.gameObject.SetActive(on);
        }

        // 縮めたときにリスナーが壁の外へ取り残されないよう内側へ寄せる。
        private void ClampListener()
        {
            if (listener == null)
            {
                var demo = FindObjectOfType<AcousticFlowSceneDemo>();
                if (demo != null) listener = demo.listener;
                if (listener == null) return;
            }

            const float margin = 0.35f;   // 壁にぴったり張り付くと遮蔽計算が不安定なので少し離す
            Vector3 p = transform.InverseTransformPoint(listener.position);
            float hw = innerSize.x * 0.5f - margin;
            float hd = innerSize.z * 0.5f - margin;
            Vector3 c = new Vector3(
                Mathf.Clamp(p.x, -hw, hw),
                Mathf.Clamp(p.y, margin, Mathf.Max(margin, innerSize.y - margin)),
                Mathf.Clamp(p.z, -hd, hd));
            if (c != p) listener.position = transform.TransformPoint(c);
        }

        private void OnValidate()
        {
            // エディタで停止中に値をいじったときも、シーンビューに即反映する。
            // ただし OnValidate 中の GameObject 生成は Unity に警告されるので、
            // 壁が既に揃っているときだけ反映する（生成は Awake / MakeRoom 側の責任）。
            if (Application.isPlaying || !gameObject.scene.IsValid()) return;
            foreach (var n in WallNames)
                if (transform.Find(n) == null) return;
            ApplyNow();
        }

        private void OnGUI()
        {
            if (!showGui) return;
            var style = new GUIStyle(GUI.skin.label) { fontSize = 13 };
            GUILayout.BeginArea(new Rect(10, Screen.height - 92, 560, 82), GUI.skin.box);
            GUILayout.Label($"部屋の内寸: {innerSize.x:F1} × {innerSize.z:F1} m / 高 {innerSize.y:F1} m" +
                            $"   容積 {innerSize.x * innerSize.y * innerSize.z:F0} m³   壁厚 {thickness:F2} m", style);
            if (enableHotkeys)
            {
                GUILayout.Label("[ = 狭める / ] = 広げる（X・Y・Z を押しながらでその軸だけ）", style);
                GUILayout.Label($"プリセット: 1=小({presetSmall.x:F0}×{presetSmall.z:F0})  " +
                                $"2=中({presetMedium.x:F0}×{presetMedium.z:F0})  " +
                                $"3=大({presetLarge.x:F0}×{presetLarge.z:F0})", style);
            }
            GUILayout.EndArea();
        }
    }
}
