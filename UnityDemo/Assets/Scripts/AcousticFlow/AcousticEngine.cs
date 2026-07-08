// AcousticEngine.cs
// 生の P/Invoke(Native) を、Unity から扱いやすい形に包むラッパー。
//   - ハンドル(IntPtr)の生成・破棄を IDisposable で安全に管理
//   - 引数/戻り値を UnityEngine.Vector3 / bool で受け渡し
using System;
using System.Text;
using UnityEngine;

namespace AcousticFlow
{
    public sealed class AcousticEngine : IDisposable
    {
        private IntPtr _handle;

        public bool IsValid => _handle != IntPtr.Zero;

        // DLL のバージョン（境界疎通の確認に使える）。
        public static int NativeVersion => Native.AcousticEngine_GetVersion();

        public AcousticEngine()
        {
            _handle = Native.AcousticEngine_Create();
        }

        // --- ジオメトリ ---
        public void ClearGeometry()
        {
            if (_handle != IntPtr.Zero)
                Native.AcousticEngine_ClearGeometry(_handle);
        }

        public void AddBox(Vector3 center, Vector3 halfExtents)
        {
            if (_handle != IntPtr.Zero)
                Native.AcousticEngine_AddBox(_handle, new AFVector3(center), new AFVector3(halfExtents));
        }

        // 回転対応(OBB)＋帯域別マテリアルで障害物を追加する。
        //   right/up : 箱のローカル軸（transform.right / transform.up をそのまま渡せる）。内部で正規直交化。
        //   material : 透過/吸収を6帯域で持つ。null なら既定マテリアル。
        // CollArea/AcousticSurface で解決したOBB＋材質を、ここから一本でエンジンに渡す。
        public void AddBoxOriented(Vector3 center, Vector3 halfExtents,
                                   Vector3 right, Vector3 up, AcousticMaterial material = null)
        {
            if (_handle == IntPtr.Zero) return;
            float[] trans = material?.transmission;   // null のまま渡すと C 側で既定マテリアル
            float[] absorp = material?.absorption;
            float[] scat = material?.scattering;
            int numBands = (trans != null && absorp != null) ? NumBands : 0;
            Native.AcousticEngine_AddBoxOriented(
                _handle, new AFVector3(center), new AFVector3(halfExtents),
                new AFVector3(right), new AFVector3(up), trans, absorp, scat, numBands);
        }

        // 三角形メッシュ occluder を追加する（CollArea 内など実形状で見せたい領域用）。
        //   worldVerticesXYZ : 頂点を x,y,z 並びで（ワールド座標。呼び出し側で transform 適用済み）。
        //   triangles        : 三角形インデックス（Unity Mesh.triangles をそのまま）。
        //   material         : メッシュ全体の材質。null なら既定。
        // 追加時にネイティブ側で BVH を構築する（重い＝静的前提。毎フレーム呼ばない）。
        // 戻り値: メッシュID（SetMeshActive で有効/無効を切替える用。失敗 -1）。
        public int AddMesh(float[] worldVerticesXYZ, int[] triangles, AcousticMaterial material = null)
        {
            if (_handle == IntPtr.Zero || worldVerticesXYZ == null || triangles == null) return -1;
            float[] trans = material?.transmission;
            float[] absorp = material?.absorption;
            float[] scat = material?.scattering;
            int numBands = (trans != null && absorp != null) ? NumBands : 0;
            return Native.AcousticEngine_AddMesh(_handle, worldVerticesXYZ, worldVerticesXYZ.Length / 3,
                                                 triangles, triangles.Length, trans, absorp, scat, numBands);
        }

        // メッシュ occluder の有効/無効を切替える（エリア入場での音響LOD用）。BVHは保持＝軽い。
        public void SetMeshActive(int meshId, bool active)
        {
            if (_handle == IntPtr.Zero || meshId < 0) return;
            Native.AcousticEngine_SetMeshActive(_handle, meshId, active ? 1 : 0);
        }

        // メッシュ occluder（静的）を全消去。ClearGeometry（箱）とは別。
        public void ClearMeshes()
        {
            if (_handle != IntPtr.Zero)
                Native.AcousticEngine_ClearMeshes(_handle);
        }

        // --- 判定 ---
        public bool IsOccluded(Vector3 from, Vector3 to)
        {
            if (_handle == IntPtr.Zero) return false;
            return Native.AcousticEngine_IsOccluded(_handle, new AFVector3(from), new AFVector3(to)) != 0;
        }

        // 材質ベースの遮蔽量(0..1)。壁の枚数・材質で連続的に変化する。
        // ※ 直線1本のみ＝壁が直接を塞ぐと 1 付近に張り付く（回り込みを無視）。
        public float ComputeOcclusion(Vector3 from, Vector3 to)
        {
            if (_handle == IntPtr.Zero) return 0f;
            return Native.AcousticEngine_ComputeOcclusion(_handle, new AFVector3(from), new AFVector3(to));
        }

        // レイ積分の遮蔽量(0..1)。壁で反射して回り込む成分も数えるので、
        // 直接が塞がれても 1.0 に張り付かず緩む。numRays/maxBounces で精度を調整。
        public float ComputeOcclusionRayIntegrated(Vector3 source, Vector3 listener,
                                                   int numRays, int maxBounces)
        {
            if (_handle == IntPtr.Zero) return 0f;
            return Native.AcousticEngine_ComputeOcclusionRayIntegrated(
                _handle, new AFVector3(source), new AFVector3(listener), numRays, maxBounces);
        }

        // 複数音源の遮蔽を1回のレイ撒きで計算（リスナーベース共有パス）。
        // sources の先頭 count 個を使い、outOcc[j] に音源 j の遮蔽量(0..1)を書く。
        // raycast は音源数に依存しない＝大量音源向けの土台。内部で AFVector3 に詰め替える。
        private AFVector3[] _srcBuf;
        public void ComputeOcclusionMultiSource(Vector3 listener, Vector3[] sources, int count,
                                                float[] outOcc, int numRays, int maxBounces)
        {
            if (_handle == IntPtr.Zero || sources == null || outOcc == null || count <= 0) return;
            if (_srcBuf == null || _srcBuf.Length < count) _srcBuf = new AFVector3[count];
            for (int i = 0; i < count; i++) _srcBuf[i] = new AFVector3(sources[i]);
            Native.AcousticEngine_ComputeOcclusionMultiSource(
                _handle, new AFVector3(listener), _srcBuf, count, outOcc, numRays, maxBounces);
        }

        // エコグラム（到達時間ビン）を outBins に積算（残響可視化用）。広帯域。
        // sources の先頭 count 個を使用。binSeconds=1ビンの秒数、speedOfSound=音速(m/s)。
        // 書き込んだビン数を返す。内部の AFVector3 バッファ(_srcBuf)を使い回す。
        public int ComputeEchogram(Vector3 listener, Vector3[] sources, int count,
                                   int numRays, int maxBounces, float[] outBins, int numBins,
                                   float binSeconds, float speedOfSound)
        {
            if (_handle == IntPtr.Zero || sources == null || outBins == null || count <= 0) return 0;
            if (_srcBuf == null || _srcBuf.Length < count) _srcBuf = new AFVector3[count];
            for (int i = 0; i < count; i++) _srcBuf[i] = new AFVector3(sources[i]);
            return Native.AcousticEngine_ComputeEchogram(
                _handle, new AFVector3(listener), _srcBuf, count, numRays, maxBounces,
                outBins, numBins, binSeconds, speedOfSound);
        }

        // 帯域別の透過ゲイン(0..1)を outGains に書き込む。
        // 帯域: 125,250,500,1k,2k,4kHz の順（計6個）。occlusionに潰す前の生値。
        public const int NumBands = 6;
        public static readonly int[] BandFreqs = { 125, 250, 500, 1000, 2000, 4000 };

        public bool ComputeTransmissionBands(Vector3 from, Vector3 to, float[] outGains)
        {
            if (_handle == IntPtr.Zero || outGains == null) return false;
            int n = Native.AcousticEngine_ComputeTransmissionBands(
                _handle, new AFVector3(from), new AFVector3(to), outGains, outGains.Length);
            return n > 0;
        }

        // origin から dir 方向のレイが最初にぶつかるまでの距離。外れたら -1。
        public float Raycast(Vector3 origin, Vector3 dir, float maxDist)
        {
            if (_handle == IntPtr.Zero) return -1f;
            return Native.AcousticEngine_DebugRaycast(_handle, new AFVector3(origin), new AFVector3(dir), maxDist);
        }

        // 反射経路を outPoints(呼び出し側が確保・再利用)に書き込み、点数を返す。
        // 内部バッファ(_pathBuffer)を使い回して毎フレームの GC を避ける。
        private AFVector3[] _pathBuffer;
        public int TraceReflectionPath(Vector3 origin, Vector3 dir, float maxDist,
                                       int maxBounces, Vector3[] outPoints)
        {
            if (_handle == IntPtr.Zero || outPoints == null || outPoints.Length == 0) return 0;

            int cap = outPoints.Length;
            if (_pathBuffer == null || _pathBuffer.Length < cap)
                _pathBuffer = new AFVector3[cap];

            int n = Native.AcousticEngine_TraceReflectionPath(
                _handle, new AFVector3(origin), new AFVector3(dir), maxDist, maxBounces, _pathBuffer, cap);

            for (int i = 0; i < n; i++)
                outPoints[i] = new Vector3(_pathBuffer[i].x, _pathBuffer[i].y, _pathBuffer[i].z);
            return n;
        }

        // --- 音源面（area source）の経路可視化 ---
        // C 側 AF_SourceShape と値を一致させる。
        public enum SourceShape
        {
            Point = 0,  // 点音源（面の広がり無し）
            Disk = 1,   // 円形の面（halfW=halfH=半径）
            Rect = 2,   // 矩形の面
        }

        // 音源面を cols×rows に切り、各セル → リスナーの直接到達量(0..1, -1=面外)を
        // outGrid(row-major, 呼び出し側が cols*rows 以上確保)に書き込む。書き込んだ数を返す。
        public int ComputeFaceReachability(Vector3 faceCenter, Vector3 faceNormal, Vector3 faceRight,
                                           float halfWidth, float halfHeight, SourceShape shape,
                                           Vector3 listener, float[] outGrid, int cols, int rows)
        {
            if (_handle == IntPtr.Zero || outGrid == null) return 0;
            return Native.AcousticEngine_ComputeFaceReachability(
                _handle, new AFVector3(faceCenter), new AFVector3(faceNormal), new AFVector3(faceRight),
                halfWidth, halfHeight, (int)shape, new AFVector3(listener), outGrid, cols, rows);
        }

        // --- 音源指向性 ---
        // C 側 AF_DirectivityType と値を一致させる（int で境界を越える）。
        public enum DirectivityType
        {
            Omni = 0,           // 無指向
            Cardioid = 1,       // 前方主体
            Supercardioid = 2,  // 前方に鋭い
            Bidirectional = 3,  // 前後に放射
            Beam = 4,           // 強い前方ビーム
        }

        // 音源指向性の広帯域ゲイン(0..1)を返し、こもり量(0..1)を out で返す。
        // ハンドル不要（ジオメトリ非依存の純幾何）。音源ごとに毎フレーム呼ぶ想定。
        public static float ComputeDirectivity(Vector3 sourcePos, Vector3 sourceForward,
                                               Vector3 listenerPos, DirectivityType type,
                                               out float lowpass)
            => Native.AcousticEngine_ComputeDirectivity(
                new AFVector3(sourcePos), new AFVector3(sourceForward), new AFVector3(listenerPos),
                (int)type, out lowpass);

        // --- 音声バックエンド(Wwise) ---
        // Wwise はグローバルなシングルトンなので、再生系は static で扱う。
        public static bool IsWwiseAvailable => Native.AcousticEngine_IsWwiseAvailable() != 0;
        public static bool InitAudio() => Native.AcousticEngine_InitAudio() != 0;
        public static bool IsAudioInitialized => Native.AcousticEngine_IsAudioInitialized() != 0;
        public static void ShutdownAudio() => Native.AcousticEngine_ShutdownAudio();

        // 文字列を UTF-8 + ヌル終端の byte[] にする（C 側の const char* と一致）。
        private static byte[] Utf8(string s) => Encoding.UTF8.GetBytes((s ?? string.Empty) + "\0");

        public static bool SetBankPath(string utf8Path) => Native.AcousticEngine_SetBankPath(Utf8(utf8Path)) != 0;
        public static bool LoadBank(string bankName) => Native.AcousticEngine_LoadBank(Utf8(bankName)) != 0;
        public static void RegisterGameObject(ulong id, string name) => Native.AcousticEngine_RegisterGameObject(id, Utf8(name));
        public static void UnregisterGameObject(ulong id) => Native.AcousticEngine_UnregisterGameObject(id);
        public static void SetDefaultListener(ulong id) => Native.AcousticEngine_SetDefaultListener(id);
        public static uint PostEvent(string eventName, ulong gameObjectId) => Native.AcousticEngine_PostEvent(Utf8(eventName), gameObjectId);
        // イベントに Stop/Pause/Resume（actionType: Stop=0/Pause=1/Resume=2）。Pause/Resume は
        // 音源ボイスを止める/再開、リバーブの尾は残る＝「止めて残響だけ聴く」用。
        public const int ActionPause = 1;
        public const int ActionResume = 2;
        public static void ExecuteActionOnEvent(string eventName, int actionType, ulong gameObjectId)
            => Native.AcousticEngine_ExecuteActionOnEvent(Utf8(eventName), actionType, gameObjectId);
        public static void RenderAudio() => Native.AcousticEngine_RenderAudio();

        // Wwise の State を設定（HRTF↔パンニング切替など）。Wwise 側で State により
        // 出力経路を切り替える前提。グループ/State 名は Wwise の定義と一致させること。
        public static void SetState(string stateGroup, string state)
            => Native.AcousticEngine_SetState(Utf8(stateGroup), Utf8(state));

        // RTPC を設定（残響の wet量・減衰時間などをエンジン計算値で駆動）。
        public static void SetRTPCValue(string name, float value)
            => Native.AcousticEngine_SetRTPCValue(Utf8(name), value);

        // RTPC を音源(ゲームオブジェクト)単位で設定。帯域別EQの Gain を音源ごとに駆動する用。
        public static void SetRTPCValueOnObject(string name, float value, ulong gameObjectId)
            => Native.AcousticEngine_SetRTPCValueOnObject(Utf8(name), value, gameObjectId);

        // 出力(マスターバス)の左右レベル(RMS, 線形)を取得（メーター可視化用）。
        public static void GetOutputLevels(out float left, out float right)
            => Native.AcousticEngine_GetOutputLevels(out left, out right);

        public static void SetGameObjectPosition(ulong id, Vector3 position, Vector3 front, Vector3 top)
            => Native.AcousticEngine_SetGameObjectPosition(id, new AFVector3(position), new AFVector3(front), new AFVector3(top));

        public static void SetObstructionOcclusion(ulong emitterId, ulong listenerId, float obstruction, float occlusion)
            => Native.AcousticEngine_SetObstructionOcclusion(emitterId, listenerId, obstruction, occlusion);

        // 音源→リスナー間の出力バス音量(線形ゲイン)。指向性ゲインの反映に使う。
        public static void SetEmitterListenerVolume(ulong emitterId, ulong listenerId, float volume)
            => Native.AcousticEngine_SetEmitterListenerVolume(emitterId, listenerId, volume);

        // 早期反射(A): AkReflect の aux バスへ像源(位置+線形レベル)を設定。毎フレーム呼ぶ想定。
        // auxBusName 空/null で authoring 既定バス。positions は長さ count*3、levels は長さ count。
        public static void SetEarlyReflections(ulong emitterId, string auxBusName,
                                               float[] positions, float[] levels, int count)
            => Native.AcousticEngine_SetEarlyReflections(
                   emitterId, Utf8(auxBusName), positions, levels, count);

        public void Dispose()
        {
            if (_handle != IntPtr.Zero)
            {
                Native.AcousticEngine_Destroy(_handle);
                _handle = IntPtr.Zero;
            }
        }
    }
}
