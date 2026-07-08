// AcousticFlowSceneDemo.cs
// 新アーキ(2026-07) Phase1c/5：新コア(AcousticScene)で動く最小デモ（複数音源対応）。
//
//   1. シーンの BoxCollider を occluder としてインスタンス登録（geomId+OBB+matId）。
//   2. 毎フレーム、各インスタンスの transform を更新（動的ジオメトリ）。
//   3. リスナー↔各音源の遮蔽を「反射込み・共有レイ1回」で計算（役割2＝音源数非依存）。
//   4. 6帯域→広帯域(低域加重)の遮蔽量に落として Wwise(既存 static API) の Occlusion を駆動。
//   5. WASD で歩き回れる（一人称）。
using System.Collections.Generic;
using UnityEngine;

namespace AcousticFlow
{
    [DisallowMultipleComponent]
    public sealed class AcousticFlowSceneDemo : MonoBehaviour
    {
        [Header("必須オブジェクト")]
        [Tooltip("リスナー（無指定ならメインカメラを使う）。")]
        public Transform listener;
        [Tooltip("主音源（0番）。回折経路の可視化はこの音源を対象にする。")]
        public Transform source;
        [Tooltip("追加音源（1番以降）。source と合わせて全音源になる。")]
        public Transform[] extraSources;
        [Tooltip("各音源の Wwise イベント名（並びは source, extraSources… の順）。")]
        public string[] sourceEvents = { "Vocal", "Guitar", "Piano", "Bass", "Drums", "Other" };

        [Header("occluder")]
        [Tooltip("ON: シーン内の BoxCollider を自動収集して壁にする（listener/source 配下は除外）。")]
        public bool autoCollectBoxColliders = true;
        [Tooltip("手動指定の occluder（autoCollect と併用可）。")]
        public BoxCollider[] extraOccluders;
        [Tooltip("occluder の材質プリセット（全 occluder 共通・最小デモ用）。")]
        public AcousticMaterialPreset occluderMaterial = AcousticMaterialPreset.Concrete;

        [Header("移動 (WASD)")]
        public bool enableMovement = true;
        public float moveSpeed = 4f;
        public float turnSpeed = 120f;
        public float mouseSensitivity = 2.5f;
        public bool firstPersonCamera = true;
        public float eyeHeight = 0f;

        [Header("反射 (役割2)")]
        [Tooltip("ON: 壁で反射して回り込む成分も含めて遮蔽を計算（壁裏でも聞こえる）。")]
        public bool useReflections = true;
        [Tooltip("反射計算のリスナーレイ本数（共有＝音源数非依存。例 256〜1024）。")]
        public int reflectionRays = 256;
        [Tooltip("反射の最大回数（例 2〜3）。")]
        public int reflectionBounces = 3;

        [Header("残響 (Phase6)")]
        [Tooltip("ON: エコグラムから RT60/wet を算出して Wwise 残響(RoomVerb)を駆動する。")]
        public bool enableReverb = true;
        [Tooltip("エコグラムの時間ビン数。bins×binMs が窓幅（例 100×10ms=1秒）。")]
        public int echogramBins = 100;
        public float echogramBinMs = 10f;
        [Tooltip("残響用レイ本数（共有）と反射回数。尾を追うので反射は多め。")]
        public int echogramRays = 512;
        public int echogramBounces = 8;
        [Tooltip("残響の更新間隔（フレーム）。重いので数フレームに1回で十分（部屋は緩変）。")]
        public int reverbUpdateEveryFrames = 4;

        [Header("遮蔽チューニング")]
        [Tooltip("遮蔽量が変化する速さ（毎秒）。小さいほど滑らか。")]
        public float occlusionSmoothSpeed = 4f;
        [Tooltip("遮蔽量の上限(0..1)。分断壁で塞ぎたいなら高め、裏で消えすぎるなら下げる。")]
        [Range(0f, 1f)] public float maxOcclusion = 0.92f;
        [Tooltip("遮蔽量全体の強さ倍率。小さいほど遮蔽が緩い。")]
        [Range(0f, 2f)] public float occlusionStrength = 1f;

        [Header("音声(Wwise)")]
        [Tooltip("ON: Wwise を初期化して各音源のイベントを再生する（バンクが無ければ幾何計算のみ）。")]
        public bool enableAudio = true;
        public string[] banks = { "Init.bnk", "TokyoGeto.bnk" };

        private const ulong ListenerObjId = 2;
        private const ulong SourceObjIdBase = 10;
        private ulong SourceId(int i) => SourceObjIdBase + (ulong)i;

        private AcousticScene _scene;
        private int _materialId;
        private readonly List<BoxCollider> _occluders = new List<BoxCollider>();
        private readonly List<int> _instanceIds = new List<int>();

        private Transform[] _sources;   // source + extraSources
        private Vector3[] _srcPos;      // 音源位置バッファ
        private float[] _occSmoothed;   // 音源ごとの平滑化遮蔽
        private float[] _bandsPerSource; // 音源ごとの6帯域生存（count*6）
        private Vector3[] _srcHome;     // 音源の元配置（Space で復帰）
        private Vector3 _stackPoint;    // 重ねる座標（元配置の重心）
        private bool _stacked;          // true=全音源を1点に重ねる

        // 主音源(0番)の表示用。
        private readonly float[] _bands = new float[AcousticEngine.NumBands];
        private readonly float[] _diffBands = new float[AcousticEngine.NumBands];
        private float _diffDelta = -1f;
        private Vector3 _diffMid;
        // 遮蔽量の帯域加重（低域=大。低音は回り込んで残るため重い）。
        private static readonly float[] _bandWeights = { 3f, 2.5f, 2f, 1.3f, 1f, 0.8f };

        private float[] _echogram;      // 到達時間ビン
        private float _reverbWet, _reverbDecay;  // エコグラムから算出（RTPCへ）
        private int _echoCountdown = 1;

        // --- モニター窓向け static フィード（Editor の *MonitorWindow が読む） ---
        public const int OutHistLen = 256;
        public static float[] LatestEchogram { get; private set; }
        public static int EchogramBins { get; private set; }
        public static float EchogramBinMs { get; private set; }
        public static float[] LatestBandGains { get; private set; }  // 主音源の帯域別生存
        public static float[] OutHistoryL { get; private set; }
        public static float[] OutHistoryR { get; private set; }
        public static int OutHistoryHead { get; private set; }
        private static float[] _monBandGains;

        private bool _audioReady;
        private string _status = "未初期化";
        private Camera _cam;
        private float _pitch;

        private void OnEnable()
        {
            if (listener == null && Camera.main != null) listener = Camera.main.transform;

            _scene = new AcousticScene();
            if (!_scene.IsValid)
            {
                _status = "Scene 生成失敗（DLL を確認）";
                Debug.LogError("[AcousticFlowScene] AF_SceneCreate が失敗しました。");
                return;
            }

            BuildSources();

            // モニター窓向けの static バッファを用意。
            OutHistoryL = new float[OutHistLen];
            OutHistoryR = new float[OutHistLen];
            OutHistoryHead = 0;
            _monBandGains = new float[AcousticEngine.NumBands];
            LatestBandGains = _monBandGains;

            _materialId = _scene.AddMaterial(AcousticMaterial.FromPreset(occluderMaterial));
            CollectOccluders();
            RegisterInstances();
            SetupCamera();
            if (enableAudio) SetupAudio();

            _status = $"Scene OK / instances={_scene.InstanceCount} / sources={_sources.Length}"
                    + (_audioReady ? " / 再生中" : (enableAudio ? " / 音声なし" : ""));
        }

        // source + extraSources を結合して音源配列とバッファを確定する。
        private void BuildSources()
        {
            var list = new List<Transform>();
            if (source != null) list.Add(source);
            if (extraSources != null)
                foreach (var s in extraSources)
                    if (s != null && s != source && !list.Contains(s)) list.Add(s);
            _sources = list.ToArray();

            int n = Mathf.Max(1, _sources.Length);
            _srcPos = new Vector3[n];
            _occSmoothed = new float[n];
            _bandsPerSource = new float[n * AcousticEngine.NumBands];

            // 元配置を控え、重ね座標＝元配置の重心を求める（Space で切替）。
            _srcHome = new Vector3[_sources.Length];
            Vector3 sum = Vector3.zero;
            for (int i = 0; i < _sources.Length; i++)
            {
                _srcHome[i] = _sources[i] != null ? _sources[i].position : Vector3.zero;
                sum += _srcHome[i];
            }
            _stackPoint = _sources.Length > 0 ? sum / _sources.Length : Vector3.zero;
            _stacked = false;

            _echogram = new float[Mathf.Max(1, echogramBins)];
        }

        // 音源を「元配置」⇄「1点に重ね」で配置し直す。
        private void ApplySourceLayout()
        {
            if (_sources == null || _srcHome == null) return;
            for (int i = 0; i < _sources.Length; i++)
                if (_sources[i] != null)
                    _sources[i].position = _stacked ? _stackPoint : _srcHome[i];
        }

        private void CollectOccluders()
        {
            _occluders.Clear();
            if (autoCollectBoxColliders)
            {
                foreach (var col in FindObjectsOfType<BoxCollider>())
                {
                    if (col == null || !col.enabled) continue;
                    if (IsExcluded(col.transform)) continue;
                    _occluders.Add(col);
                }
            }
            if (extraOccluders != null)
                foreach (var col in extraOccluders)
                    if (col != null && !_occluders.Contains(col)) _occluders.Add(col);
        }

        // listener / 各音源の階層下（本人含む）は occluder から除外する。
        private bool IsExcluded(Transform t)
        {
            for (Transform p = t; p != null; p = p.parent)
            {
                if (p == listener) return true;
                if (_sources != null)
                    foreach (var s in _sources) if (p == s) return true;
            }
            return false;
        }

        private void RegisterInstances()
        {
            _instanceIds.Clear();
            foreach (var col in _occluders)
            {
                GetObb(col, out Vector3 c, out Vector3 half, out Vector3 right, out Vector3 up);
                _instanceIds.Add(_scene.AddInstanceBox(c, half, right, up, _materialId));
            }
        }

        private static void GetObb(BoxCollider col, out Vector3 center, out Vector3 half,
                                   out Vector3 right, out Vector3 up)
        {
            Transform t = col.transform;
            center = t.TransformPoint(col.center);
            Vector3 s = t.lossyScale;
            half = new Vector3(
                Mathf.Abs(col.size.x * 0.5f * s.x),
                Mathf.Abs(col.size.y * 0.5f * s.y),
                Mathf.Abs(col.size.z * 0.5f * s.z));
            right = t.right;
            up = t.up;
        }

        private void SetupCamera()
        {
            if (!firstPersonCamera) return;
            _cam = Camera.main;
            if (listener != null)
            {
                var rend = listener.GetComponentInChildren<Renderer>();
                if (rend != null) rend.enabled = false;
            }
            _pitch = 0f;
        }

        private string EventFor(int i)
        {
            if (sourceEvents != null && i < sourceEvents.Length && !string.IsNullOrEmpty(sourceEvents[i]))
                return sourceEvents[i];
            return "Vocal";
        }

        private void SetupAudio()
        {
            if (!AcousticEngine.IsWwiseAvailable) { _status = "Wwise 非搭載ビルド"; return; }
            if (!AcousticEngine.InitAudio()) { _status = "Wwise 初期化失敗"; return; }

            string bankPath = System.IO.Path.Combine(Application.streamingAssetsPath, "WwiseBanks");
            AcousticEngine.SetBankPath(bankPath);
            bool banksOk = true;
            if (banks != null)
                foreach (var b in banks)
                    if (!string.IsNullOrEmpty(b)) banksOk &= AcousticEngine.LoadBank(b);
            if (!banksOk)
            {
                _status = "バンク未検出（StreamingAssets/WwiseBanks を確認）";
                Debug.LogWarning($"[AcousticFlowScene] バンク読込失敗: {bankPath}");
                return;
            }

            AcousticEngine.RegisterGameObject(ListenerObjId, "Listener");
            AcousticEngine.SetDefaultListener(ListenerObjId);
            for (int i = 0; i < _sources.Length; i++)
                AcousticEngine.RegisterGameObject(SourceId(i), "Source" + i);
            UpdateAudioTransforms();

            int playing = 0;
            for (int i = 0; i < _sources.Length; i++)
                if (AcousticEngine.PostEvent(EventFor(i), SourceId(i)) != 0) playing++;
            _audioReady = playing > 0;
            if (!_audioReady)
                Debug.LogWarning("[AcousticFlowScene] PostEvent 失敗。イベント名（sourceEvents）を確認。");
        }

        private void HandleMovement()
        {
            float dt = Time.deltaTime;
            float yaw = 0f;
            if (Input.GetKey(KeyCode.Q)) yaw -= turnSpeed * dt;
            if (Input.GetKey(KeyCode.E)) yaw += turnSpeed * dt;
            if (Input.GetMouseButton(1))
            {
                yaw += Input.GetAxis("Mouse X") * mouseSensitivity;
                _pitch = Mathf.Clamp(_pitch - Input.GetAxis("Mouse Y") * mouseSensitivity, -80f, 80f);
            }
            if (Mathf.Abs(yaw) > 0f) listener.Rotate(0f, yaw, 0f, Space.World);

            float x = Input.GetAxisRaw("Horizontal");
            float z = Input.GetAxisRaw("Vertical");
            if (x != 0f || z != 0f)
            {
                Vector3 fwd = listener.forward; fwd.y = 0f; fwd.Normalize();
                Vector3 right = listener.right; right.y = 0f; right.Normalize();
                Vector3 move = (fwd * z + right * x).normalized;
                float speed = moveSpeed * (Input.GetKey(KeyCode.LeftShift) ? 2.5f : 1f);
                listener.position += move * speed * dt;
            }
        }

        private void Update()
        {
            if (_scene == null || !_scene.IsValid || listener == null || _sources == null) return;
            if (_sources.Length == 0) return;

            // Space：音源を「元配置」⇄「1点に重ね」でトグル。
            if (Input.GetKeyDown(KeyCode.Space)) { _stacked = !_stacked; ApplySourceLayout(); }

            if (enableMovement) HandleMovement();

            // 1) 動的ジオメトリ更新（動いた分だけ）。
            for (int i = 0; i < _occluders.Count; i++)
            {
                var col = _occluders[i];
                if (col == null || _instanceIds[i] < 0) continue;
                GetObb(col, out Vector3 c, out Vector3 half, out Vector3 right, out Vector3 up);
                _scene.UpdateInstance(_instanceIds[i], c, half, right, up);
            }

            // 2) 音源位置バッファ。
            for (int i = 0; i < _sources.Length; i++)
                _srcPos[i] = _sources[i] != null ? _sources[i].position : listener.position;

            // 3) 帯域別生存を音源ごとに求める。
            if (useReflections)
            {
                // 共有レイ1回で全音源へ（役割2＝音源数非依存）。
                _scene.OcclusionReflectedMulti(listener.position, _srcPos, _sources.Length,
                                               null, _bandsPerSource, reflectionRays, reflectionBounces);
            }
            else
            {
                // 反射なし＝音源ごとに直接（透過⊕回折）。
                for (int i = 0; i < _sources.Length; i++)
                {
                    _scene.ComputeTransmissionBands(_srcPos[i], listener.position, _bands);
                    _scene.ComputeDiffractionBands(_srcPos[i], listener.position, _diffBands);
                    for (int b = 0; b < AcousticEngine.NumBands; b++)
                        _bandsPerSource[i * AcousticEngine.NumBands + b] = Mathf.Max(_bands[b], _diffBands[b]);
                }
            }

            // 4) 音源ごとに 低域加重の遮蔽量 → 平滑化 → Wwise 駆動。
            float dt = Time.deltaTime;
            int nb = AcousticEngine.NumBands;
            for (int i = 0; i < _sources.Length; i++)
            {
                float wsum = 0f, gsum = 0f;
                for (int b = 0; b < nb; b++)
                {
                    float w = _bandWeights[b];
                    gsum += w * _bandsPerSource[i * nb + b];
                    wsum += w;
                }
                float avgGain = (wsum > 0f) ? gsum / wsum : 0f;
                float target = Mathf.Clamp(Mathf.Clamp01(1f - avgGain) * occlusionStrength, 0f, maxOcclusion);
                _occSmoothed[i] = Mathf.MoveTowards(_occSmoothed[i], target, occlusionSmoothSpeed * dt);

                if (_audioReady)
                    AcousticEngine.SetObstructionOcclusion(SourceId(i), ListenerObjId, 0f, _occSmoothed[i]);
            }

            // 5) 主音源(0番)の透過/回折/回折経路を表示用に取得。
            _scene.ComputeTransmissionBands(_srcPos[0], listener.position, _bands);
            _scene.ComputeDiffractionBands(_srcPos[0], listener.position, _diffBands);
            _diffDelta = _scene.DiffractionPath(_srcPos[0], listener.position, out _diffMid);

            // 6) 残響（低レートでエコグラム→RT60/wet→Wwise RoomVerb を RTPC 駆動）。
            if (enableReverb && _audioReady && _echogram != null)
            {
                if (--_echoCountdown <= 0)
                {
                    _echoCountdown = Mathf.Max(1, reverbUpdateEveryFrames);
                    _scene.ComputeEchogram(listener.position, _srcPos, _sources.Length,
                        _echogram, _echogram.Length, echogramBinMs * 0.001f, 343f,
                        echogramRays, echogramBounces);
                    UpdateReverbFromEchogram();
                    // Reverb Monitor 窓へ。
                    LatestEchogram = _echogram;
                    EchogramBins = _echogram.Length;
                    EchogramBinMs = echogramBinMs;
                }
            }

            PublishMonitors();
            if (_audioReady) { UpdateAudioTransforms(); AcousticEngine.RenderAudio(); }
        }

        // モニター窓向け：主音源の帯域別生存＋出力(マスターL/R)波形を static へ流す。
        private void PublishMonitors()
        {
            if (_monBandGains != null && _bandsPerSource != null)
            {
                for (int b = 0; b < AcousticEngine.NumBands; b++) _monBandGains[b] = _bandsPerSource[b];
                LatestBandGains = _monBandGains;
            }
            if (_audioReady && OutHistoryL != null)
            {
                AcousticEngine.GetOutputLevels(out float l, out float r);
                OutHistoryL[OutHistoryHead] = l;
                OutHistoryR[OutHistoryHead] = r;
                OutHistoryHead = (OutHistoryHead + 1) % OutHistLen;
            }
        }

        // エコグラムから wet量（反射割合）と RT60（尾の長さ）を算出し、Wwise 残響へ RTPC 送出。
        //   ReverbWet   : 0..100（反射割合×100） / ReverbDecay : 秒（尾の長さ）
        private void UpdateReverbFromEchogram()
        {
            int bins = _echogram.Length;
            float peak = 0f, total = 0f;
            for (int k = 0; k < bins; k++) { float e = _echogram[k]; if (e > peak) peak = e; total += e; }
            if (peak <= 0f) return;

            float tail = Mathf.Max(0f, total - peak);
            float wet = Mathf.Clamp01(tail / Mathf.Max(total, 1e-6f));
            float floor = peak * 0.001f;   // -60dB
            int tailBin = 0;
            for (int k = 0; k < bins; k++) if (_echogram[k] > floor) tailBin = k;
            float rt60 = (tailBin + 1) * echogramBinMs * 0.001f;

            _reverbWet = Mathf.Lerp(_reverbWet, wet, 0.2f);
            _reverbDecay = Mathf.Lerp(_reverbDecay, rt60, 0.2f);
            AcousticEngine.SetRTPCValue("ReverbWet", _reverbWet * 100f);
            AcousticEngine.SetRTPCValue("ReverbDecay", _reverbDecay);
        }

        private void LateUpdate()
        {
            if (!firstPersonCamera || _cam == null || listener == null) return;
            if (_cam.transform == listener) return;
            _cam.transform.position = listener.position + Vector3.up * eyeHeight;
            _cam.transform.rotation = Quaternion.Euler(_pitch, listener.eulerAngles.y, 0f);
        }

        private void UpdateAudioTransforms()
        {
            AcousticEngine.SetGameObjectPosition(ListenerObjId, listener.position, listener.forward, listener.up);
            if (_sources == null) return;
            for (int i = 0; i < _sources.Length; i++)
                if (_sources[i] != null)
                    AcousticEngine.SetGameObjectPosition(SourceId(i), _sources[i].position, _sources[i].forward, _sources[i].up);
        }

        private void OnDisable()
        {
            if (_audioReady && AcousticEngine.IsAudioInitialized)
            {
                if (_sources != null)
                    for (int i = 0; i < _sources.Length; i++)
                        AcousticEngine.UnregisterGameObject(SourceId(i));
                AcousticEngine.UnregisterGameObject(ListenerObjId);
                AcousticEngine.ShutdownAudio();
            }
            _audioReady = false;
            _scene?.Dispose();
            _scene = null;
        }

        // 可視化：各音源の直接線（緑=素通り/赤=遮蔽）＋主音源の回折経路（青の折れ線）。
        private void OnDrawGizmos()
        {
            if (listener == null) return;
            if (_sources != null && _occSmoothed != null)
            {
                for (int i = 0; i < _sources.Length; i++)
                {
                    if (_sources[i] == null) continue;
                    Gizmos.color = Color.Lerp(Color.green, Color.red, _occSmoothed[i]);
                    Gizmos.DrawLine(_sources[i].position, listener.position);
                }
            }
            else if (source != null)
            {
                Gizmos.color = Color.green;
                Gizmos.DrawLine(source.position, listener.position);
            }

            if (_diffDelta >= 0f)
            {
                Gizmos.color = new Color(0.2f, 0.6f, 1f);
                Vector3 s0 = (_sources != null && _sources.Length > 0 && _sources[0] != null)
                    ? _sources[0].position : (source != null ? source.position : listener.position);
                Gizmos.DrawLine(s0, _diffMid);
                Gizmos.DrawLine(_diffMid, listener.position);
                Gizmos.DrawSphere(_diffMid, 0.15f);
            }
        }

        private void OnGUI()
        {
            var style = new GUIStyle(GUI.skin.label) { fontSize = 13 };
            GUILayout.BeginArea(new Rect(10, 10, 480, 300), GUI.skin.box);
            GUILayout.Label($"[新コア Scene] {_status}", style);
            if (enableMovement)
                GUILayout.Label("操作: WASD移動 / 右ドラッグ見回す / QE旋回 / Shift加速 / Space:音源重ね", style);
            GUILayout.Label($"音源配置: {(_stacked ? "重ね(1点)" : "展開")}", style);
            if (enableReverb)
                GUILayout.Label($"残響(geometry駆動): wet {_reverbWet * 100f:F0}%  RT {_reverbDecay:F2}s", style);

            if (_sources != null && _occSmoothed != null)
            {
                string occ = "遮蔽/音源: ";
                for (int i = 0; i < _sources.Length; i++)
                    occ += $"{EventFor(i)}:{_occSmoothed[i]:F2}  ";
                GUILayout.Label(occ, style);
            }
            GUILayout.Label(_diffDelta >= 0f
                ? $"主音源の回折: 迂回路あり δ={_diffDelta:F2} m（青=経路補完）"
                : "主音源の回折: なし", style);

            if (_scene != null && _scene.IsValid)
            {
                GUILayout.Label(BandLine("主音源 透過", _bands), style);
                GUILayout.Label(BandLine("主音源 回折", _diffBands), style);
            }
            GUILayout.EndArea();
        }

        private static string BandLine(string label, float[] g)
        {
            string s = label + " ";
            for (int b = 0; b < g.Length; b++)
                s += $"{AcousticEngine.BandFreqs[b]}:{g[b]:F2} ";
            return s;
        }
    }
}
