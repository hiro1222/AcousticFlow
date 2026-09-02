// RoomCompareLab.cs
// 「同じ部屋で聞く」と「隣の部屋で聞く」を、**広さも扉の開き角も変えないまま**
// 切り替えて聴き比べるための検証台。比較動画の素材を撮るのが目的。
//
// ★何を見せる台か
//   この作品が音で伝えようとしているものは 2 つある:
//     ① 同じ部屋にいるときの残響の量と長さ（広さで変わる）
//     ② 隣の部屋にいるときのこもり（壁 1 枚と扉の開き角で変わる）
//   この 2 つは**同じ場面の裏表**なので、別々のシーンで見せると比べられない。
//   ここでは 1 つのシーンに両方を入れて、Enter で立つ側だけを入れ替える。
//
// ★立ち位置は仕切りに対して鏡像にしてある
//   同じ部屋 (+Z 側) と 隣の部屋 (-Z 側) で、戸口からの距離を等しくする。
//   音源からの距離は当然ちがう（壁の向こうなので遠い）ので、
//   **音量ではなく「こもり倍率 125Hz/4kHz」で比べる**。HUD に出しているのはそのため。
//   この尺度はゲームレーンが扉の実測で使っているものと同じ（閉 5.8 → 開 1.2）。
//
// ★形は ResizableRoom に持たせて、仕切りだけここが持つ
//   外殻（床・天井・四方壁）は ResizableRoom がすでに持っている。二重に書かない。
//   ここが足すのは「真ん中の仕切り＋戸口＋扉を、広さを変えても追従させる」ところだけ。
//   Test_SwingDoor が拡縮を諦めていた（`enableHotkeys = false` に
//   「仕切りと扉が連動しないので拡縮させない」と書いてある）のは、この連動が
//   無かったから。ここで埋める。
//
// ★箱は最初に全部作って、あとは動かすだけ
//   AcousticFlowSceneDemo は OnEnable で BoxCollider を集める。実行中に箱を
//   増やしても拾われないので、**個数を変えずに位置と大きさだけ動かす**
//   （ResizableRoom と同じ約束）。まぐさが潰れないよう戸口の高さは天井から
//   0.3m 残す側にクランプしている。
//
// 操作:
//   1 / 2 / 3  広さ（小 / 中 / 大）
//   Enter      居る部屋を切り替え（音源と同じ部屋 ⇄ 隣の部屋）
//   5 / 6      扉を開く / 閉じる（SwingDoor 側の既定キー）
using UnityEngine;

namespace AcousticFlow
{
    [DefaultExecutionOrder(-90)]   // ResizableRoom(-100) の後、SceneDemo(0) の前
    [AddComponentMenu("AcousticFlow/Room Compare Lab")]
    public sealed class RoomCompareLab : MonoBehaviour
    {
        [Header("部品（未設定なら子から探すか作る）")]
        [Tooltip("外殻。床・天井・四方壁を持つ。仕切りはここには含まれない。")]
        public ResizableRoom shell;
        [Tooltip("戸口を塞ぐ扉。開き角は SwingDoor が 5/6 キーで持つ。")]
        public SwingDoor door;
        [Tooltip("リスナー。未設定なら AcousticFlowSceneDemo の listener を使う。")]
        public Transform listener;
        [Tooltip("音源。未設定なら AcousticFlowSceneDemo の source を使う。")]
        public Transform source;

        [Header("広さ (1/2/3 キー) — 外殻の内寸")]
        [Tooltip("片側の部屋 4x2.5x3m ≒ 30m3")]
        public Vector3 presetSmall = new Vector3(4f, 2.5f, 6.2f);
        [Tooltip("片側の部屋 10x4x8m ≒ 320m3")]
        public Vector3 presetMedium = new Vector3(10f, 4f, 16.2f);
        [Tooltip("片側の部屋 24x10x18m ≒ 4320m3")]
        public Vector3 presetLarge = new Vector3(24f, 10f, 36.2f);
        [Tooltip("いま選ばれている広さ。0=小 / 1=中 / 2=大")]
        [Range(0, 2)] public int sizeIndex = 1;

        [Header("仕切りと戸口")]
        [Tooltip("仕切りの厚み(m)。透過量に直接効く。")]
        public float partitionThickness = 0.2f;
        [Tooltip("戸口の幅(m)。扉の幅もこれに合わせる。")]
        public float doorwayWidth = 1.0f;
        [Tooltip("戸口の高さ(m)。天井まで届かせない（まぐさが潰れると箱が退化する）。")]
        public float doorwayHeight = 2.2f;

        [Header("立ち位置")]
        [Tooltip("リスナーを戸口から何 m 離すか。両側で同じ値を使う（鏡像）。")]
        public float listenerFromDoor = 2.0f;
        [Tooltip("音源を奥の壁から何 m 離すか。")]
        public float sourceFromBackWall = 0.8f;
        [Tooltip("リスナーと音源の高さ(m)。")]
        public float standHeight = 1.6f;

        [Header("操作")]
        public bool enableHotkeys = true;
        [Tooltip("居る部屋を切り替えるキー。")]
        public KeyCode switchRoomKey = KeyCode.Return;
        [Tooltip("同上（テンキー側）。")]
        public KeyCode switchRoomKeyAlt = KeyCode.KeypadEnter;
        [Tooltip("ON: 音源と同じ部屋 (+Z 側) にいる。OFF: 隣の部屋 (-Z 側)。")]
        public bool inSourceRoom = false;
        [Tooltip("ON: 画面左上に広さ・居る部屋・扉の角度・実測値を出す。")]
        public bool showGui = true;

        private const int NB = 6;
        private static readonly string[] SizeNames = { "小", "中", "大" };

        private Transform _partL, _partR, _partTop, _hinge;
        private int _appliedSize = -1;
        private bool _appliedSide;

        // HUD 用の読み取りバッファ（毎フレーム確保しない）。
        private readonly float[] _rt60 = new float[NB];
        private readonly float[] _occ = new float[NB];

        private AcousticFlowSceneDemo _demo;

        private void Awake()
        {
            EnsurePieces();
            ApplyNow();
        }

        // ── 部品を用意する。既にあれば作り直さない（ResizableRoom.EnsureWalls と同じ作法）──
        public void EnsurePieces()
        {
            if (shell == null)
            {
                shell = GetComponentInChildren<ResizableRoom>();
                if (shell == null)
                {
                    var go = new GameObject("Shell");
                    go.transform.SetParent(transform, false);
                    shell = go.AddComponent<ResizableRoom>();
                }
            }
            // 広さの入口をここに一本化する。外殻側のキーと GUI は止める
            //   （1/2/3 が二重に効くと、どちらが勝ったのか分からなくなる）。
            shell.enableHotkeys = false;
            shell.showGui = false;
            shell.makeDoorway = false;   // 戸口は南壁ではなく真ん中の仕切りに開ける
            shell.makeFloor = true;

            _partL = EnsureBox("Partition_L");
            _partR = EnsureBox("Partition_R");
            _partTop = EnsureBox("Partition_Top");

            _hinge = transform.Find("Door_Hinge");
            if (_hinge == null)
            {
                var go = new GameObject("Door_Hinge");
                go.transform.SetParent(transform, false);
                _hinge = go.transform;
            }

            if (door == null)
            {
                Transform d = transform.Find("Door");
                if (d != null) door = d.GetComponent<SwingDoor>();
            }
            if (door == null)
            {
                var go = GameObject.CreatePrimitive(PrimitiveType.Cube);
                go.name = "Door";
                go.transform.SetParent(transform, false);
                door = go.AddComponent<SwingDoor>();
                door.thickness = 0.06f;
                door.swingTowardPositiveZ = true;
                door.angleDeg = 0f;
            }
            door.hinge = _hinge;
        }

        private Transform EnsureBox(string name)
        {
            Transform t = transform.Find(name);
            if (t != null) return t;
            var go = GameObject.CreatePrimitive(PrimitiveType.Cube);
            go.name = name;
            go.transform.SetParent(transform, false);
            return go.transform;
        }

        public Vector3 PresetAt(int i)
        {
            if (i <= 0) return presetSmall;
            if (i == 1) return presetMedium;
            return presetLarge;
        }

        /// <summary>いまの sizeIndex を形に反映し、立ち位置も置き直す。</summary>
        public void ApplyNow()
        {
            // 参照のどれか 1 つでも欠けていたら作り直す。Inspector から直接 ApplyNow を
            // 呼ばれても落ちないように（shell だけ見ていると仕切りが null で落ちる）。
            if (shell == null || _partL == null || _partR == null || _partTop == null
                || _hinge == null || door == null) EnsurePieces();
            door.hinge = _hinge;

            sizeIndex = Mathf.Clamp(sizeIndex, 0, 2);
            Vector3 inner = PresetAt(sizeIndex);

            shell.innerSize = inner;
            shell.ApplyNow();

            float w = inner.x, h = inner.y;
            float t = Mathf.Max(0.05f, partitionThickness);
            // まぐさを必ず残す。天井まで開けると箱の高さが 0 になって退化する。
            float dh = Mathf.Clamp(doorwayHeight, 1.0f, h - 0.3f);
            float dw = Mathf.Clamp(doorwayWidth, 0.3f, w - 0.6f);

            float sideW = (w - dw) * 0.5f;
            float sideX = dw * 0.5f + sideW * 0.5f;
            Place(_partL, new Vector3(-sideX, h * 0.5f, 0f), new Vector3(sideW, h, t));
            Place(_partR, new Vector3(sideX, h * 0.5f, 0f), new Vector3(sideW, h, t));
            float topH = h - dh;
            Place(_partTop, new Vector3(0f, dh + topH * 0.5f, 0f), new Vector3(dw, topH, t));

            // 扉。蝶番は戸口の左枠、Y は**扉の中心の高さ**（SwingDoor の約束）。
            _hinge.position = transform.position + new Vector3(-dw * 0.5f, dh * 0.5f, 0f);
            _hinge.rotation = Quaternion.identity;
            door.width = dw;
            door.height = dh;
            door.Apply();

            PlaceSourceAndListener(inner, t);

            _appliedSize = sizeIndex;
            _appliedSide = inSourceRoom;
        }

        private static void Place(Transform t, Vector3 localCenter, Vector3 size)
        {
            if (t == null) return;
            t.localPosition = localCenter;
            t.localRotation = Quaternion.identity;
            t.localScale = size;
        }

        /// <summary>音源は +Z の部屋の奥、リスナーは戸口から等距離の鏡像位置へ。</summary>
        private void PlaceSourceAndListener(Vector3 inner, float t)
        {
            ResolveRefs();

            float halfDepth = (inner.z - t) * 0.5f;              // 片側の部屋の奥行き
            if (halfDepth < 0.5f) halfDepth = 0.5f;

            // ★奥行きに対する割合で頭打ちにする。絶対値でクランプすると、小さい部屋で
            //   リスナーと音源が 0.2m まで近づいて「同室」が near-field になり、
            //   広さの比較にならなかった（小: 奥行き 3m に対し 2.0m + 0.8m）。
            float zl = t * 0.5f + Mathf.Min(Mathf.Max(listenerFromDoor, 0.3f), halfDepth * 0.45f);
            float zs = t * 0.5f + halfDepth
                     - Mathf.Min(Mathf.Max(sourceFromBackWall, 0.3f), halfDepth * 0.3f);

            if (source != null)
                source.position = transform.position + new Vector3(0f, standHeight, zs);
            if (listener != null)
                listener.position = transform.position
                                  + new Vector3(0f, standHeight, inSourceRoom ? zl : -zl);
        }

        private void ResolveRefs()
        {
            if (_demo == null) _demo = FindFirstObjectByType<AcousticFlowSceneDemo>();
            if (_demo == null) return;
            if (listener == null) listener = _demo.listener;
            if (source == null) source = _demo.source;
        }

        private void Update()
        {
            if (enableHotkeys) HandleHotkeys();

            // Inspector を実行中にいじった場合にも追従する。
            if (sizeIndex != _appliedSize) ApplyNow();
            else if (inSourceRoom != _appliedSide)
            {
                PlaceSourceAndListener(PresetAt(sizeIndex), Mathf.Max(0.05f, partitionThickness));
                _appliedSide = inSourceRoom;
            }
        }

        private void HandleHotkeys()
        {
            if (Input.GetKeyDown(KeyCode.Alpha1)) sizeIndex = 0;
            else if (Input.GetKeyDown(KeyCode.Alpha2)) sizeIndex = 1;
            else if (Input.GetKeyDown(KeyCode.Alpha3)) sizeIndex = 2;

            if (Input.GetKeyDown(switchRoomKey) || Input.GetKeyDown(switchRoomKeyAlt))
                inSourceRoom = !inSourceRoom;
        }

        // ── HUD ───────────────────────────────────────────────────────────
        // 出すのは「いまどういう条件か」と「エンジンが実際に何を返しているか」だけ。
        // 数字は全部エンジンからの読み値で、ここでは作らない。
        private void OnGUI()
        {
            if (!showGui) return;
            ResolveRefs();

            var st = new GUIStyle(GUI.skin.label) { fontSize = 13, richText = true };
            // ★AcousticFlowSceneDemo の HUD が Rect(10,10,480,300) を使っているので、
            //   その下に置く。左上に重ねると両方読めなくなる。
            GUILayout.BeginArea(new Rect(12f, 318f, 460f, 250f), GUI.skin.box);

            Vector3 inner = PresetAt(sizeIndex);
            float t = Mathf.Max(0.05f, partitionThickness);
            float halfDepth = Mathf.Max(0.5f, (inner.z - t) * 0.5f);
            float roomVol = inner.x * inner.y * halfDepth;

            GUILayout.Label("<b>部屋の広さ比較台</b>   "
                            + "<color=#ffd479>1/2/3 広さ・Enter 部屋・5/6 扉</color>", st);

            string sizeLine = "";
            for (int i = 0; i < 3; i++)
                sizeLine += (i == sizeIndex ? "<b>[" + (i + 1) + "] " + SizeNames[i] + "</b>"
                                            : " " + (i + 1) + " " + SizeNames[i] + " ") + "  ";
            GUILayout.Label("広さ  " + sizeLine, st);
            GUILayout.Label($"   片側の部屋 {inner.x:F1} x {inner.y:F1} x {halfDepth:F1} m"
                            + $"  = <b>{roomVol:F0} m3</b>", st);

            GUILayout.Label(inSourceRoom
                    ? "居る部屋  <color=#a0ffa0><b>音源と同じ部屋</b></color>（Enter で隣へ）"
                    : "居る部屋  <color=#ffa0a0><b>隣の部屋</b></color>（Enter で音源側へ）", st);

            float ang = (door != null) ? door.angleDeg : 0f;
            GUILayout.Label($"扉  <b>{ang:F1}°</b>   戸口 {doorwayWidth:F2} x {doorwayHeight:F2} m"
                            + $"   仕切り厚 {t:F2} m", st);

            if (listener != null && source != null)
                GUILayout.Label($"音源まで  {Vector3.Distance(listener.position, source.position):F2} m", st);

            GUILayout.Space(4);
            var scene = AcousticFlowSceneDemo.SharedScene;
            if (scene != null && scene.IsValid && listener != null)
            {
                float vol = scene.RoomVolumeAt(listener.position, 1.0f);
                int nb = scene.GetRt60At(listener.position, 1.0f, _rt60);
                if (nb >= NB && vol > 0f)
                {
                    float rtMid = Mathf.Max(0.05f, _rt60[2]);
                    float rc = 0.057f * Mathf.Sqrt(vol / rtMid);
                    float r = (source != null) ? Vector3.Distance(listener.position, source.position) : 0f;
                    GUILayout.Label($"実効体積 <b>{vol:F0} m3</b>   "
                                    + $"RT60(500Hz) <b>{rtMid:F2} s</b>   "
                                    + $"rc <b>{rc:F2} m</b>   r/rc {(r / Mathf.Max(rc, 1e-3f)):F1}", st);
                    GUILayout.Label($"   RT60 帯域別  125 {_rt60[0]:F2} / 500 {_rt60[2]:F2}"
                                    + $" / 4k {_rt60[5]:F2} s", st);
                }
                else
                {
                    GUILayout.Label("<color=#ffa0a0>部屋が検出されていません"
                                    + "（外殻が閉じているか確認）</color>", st);
                }
            }

            var taps = AcousticFlowSceneDemo.Status.Taps;
            if (scene != null && scene.IsValid && taps != null && taps.Length > 0 && taps[0] != null)
            {
                int idx = taps[0].EngineIndex;
                if (idx >= 0)
                {
                    scene.GetSourceOcclusion(idx, _occ);
                    float muffle = (_occ[5] > 1e-9f) ? _occ[0] / _occ[5] : 0f;
                    GUILayout.Label($"こもり倍率 (125Hz/4kHz) <b>{muffle:F2}</b>   "
                                    + $"125 {Db(_occ[0]):F1} dB / 4k {Db(_occ[5]):F1} dB", st);
                    GUILayout.Label("<color=#a0c0ff>音量ではなく<b>こもり倍率</b>で比べること"
                                    + "（部屋を跨ぐと距離も変わるため）。</color>", st);
                }
            }

            GUILayout.EndArea();
        }

        private static float Db(float lin) { return 20f * Mathf.Log10(Mathf.Max(1e-6f, lin)); }
    }
}
