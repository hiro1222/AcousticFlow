// AcousticEngineBindings.cs
// AcousticEngine.dll の C API を C# から呼ぶための P/Invoke 宣言。
// C 側 (acoustic_engine.h) と 1:1 で対応させる「鏡」。
// ここは生の境界定義に徹し、使いやすい API は AcousticEngine.cs 側で包む。
using System;
using System.Runtime.InteropServices;
using UnityEngine;

namespace AcousticFlow
{
    // C 側 AF_Vector3 (float x,y,z) と同じメモリ配置にする。
    // Sequential = 宣言順にそのまま並べる（C 構造体と一致）。
    [StructLayout(LayoutKind.Sequential)]
    internal struct AFVector3
    {
        public float x;
        public float y;
        public float z;

        public AFVector3(Vector3 v) { x = v.x; y = v.y; z = v.z; }
    }

    internal static class Native
    {
        // 拡張子なしの "AcousticEngine" にしておくと、Windows では
        // AcousticEngine.dll が解決される（Assets/Plugins/x86_64 配下）。
        private const string Dll = "AcousticEngine";
        private const CallingConvention Cc = CallingConvention.Cdecl;

        [DllImport(Dll, CallingConvention = Cc)]
        public static extern int AcousticEngine_GetVersion();

        [DllImport(Dll, CallingConvention = Cc)]
        public static extern IntPtr AcousticEngine_Create();

        [DllImport(Dll, CallingConvention = Cc)]
        public static extern void AcousticEngine_Destroy(IntPtr engine);

        [DllImport(Dll, CallingConvention = Cc)]
        public static extern void AcousticEngine_AddBox(IntPtr engine, AFVector3 center, AFVector3 halfExtents);

        // 回転対応(OBB)＋帯域別マテリアル。transmission/absorption は kNumBands(6) 要素。
        // null を渡すと既定マテリアル（その場合 numBands は無視される）。
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern void AcousticEngine_AddBoxOriented(
            IntPtr engine, AFVector3 center, AFVector3 halfExtents,
            AFVector3 right, AFVector3 up,
            [In] float[] transmission, [In] float[] absorption, [In] float[] scattering, int numBands);

        // 三角形メッシュ occluder（ワールド頂点＋インデックス＋6+6素材）。
        // vertices は x,y,z 並びで vertexCount*3 要素。indices は 3個ずつ。null 素材なら既定。
        // 戻り値: メッシュID（SetMeshActive で有効/無効を切替える用。失敗 -1）。
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern int AcousticEngine_AddMesh(
            IntPtr engine, [In] float[] vertices, int vertexCount,
            [In] int[] indices, int indexCount,
            [In] float[] transmission, [In] float[] absorption, [In] float[] scattering, int numBands);

        // メッシュ occluder の有効/無効（active: 1=有効/0=無効）。BVHは保持＝軽い。音響LOD用。
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern void AcousticEngine_SetMeshActive(IntPtr engine, int meshId, int active);

        // 箱 occluder（動的）を全消去。メッシュは残る。
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern void AcousticEngine_ClearGeometry(IntPtr engine);

        // メッシュ occluder（静的・BVH付き）を全消去。
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern void AcousticEngine_ClearMeshes(IntPtr engine);

        [DllImport(Dll, CallingConvention = Cc)]
        public static extern int AcousticEngine_IsOccluded(IntPtr engine, AFVector3 from, AFVector3 to);

        [DllImport(Dll, CallingConvention = Cc)]
        public static extern float AcousticEngine_ComputeOcclusion(IntPtr engine, AFVector3 from, AFVector3 to);

        // レイ積分の遮蔽。壁で反射して回り込む成分も数えるので、直接が塞がれても緩む。
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern float AcousticEngine_ComputeOcclusionRayIntegrated(
            IntPtr engine, AFVector3 source, AFVector3 listener, int numRays, int maxBounces);

        // 複数音源版の遮蔽（リスナーベース共有パス）。sources/outOcc は count 個以上。
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern void AcousticEngine_ComputeOcclusionMultiSource(
            IntPtr engine, AFVector3 listener, [In] AFVector3[] sources, int count,
            [Out] float[] outOcc, int numRays, int maxBounces);

        // outGains は呼び出し側で確保した配列。書き込んだ帯域数を返す。
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern int AcousticEngine_ComputeTransmissionBands(
            IntPtr engine, AFVector3 from, AFVector3 to, [Out] float[] outGains, int count);

        [DllImport(Dll, CallingConvention = Cc)]
        public static extern float AcousticEngine_DebugRaycast(IntPtr engine, AFVector3 origin, AFVector3 dir, float maxDist);

        // 反射経路トレース。outPoints は呼び出し側が確保した配列に書き込まれる。
        // AFVector3 は blittable なので、配列はピン留めされ追加コピー無しで書き込まれる。
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern int AcousticEngine_TraceReflectionPath(
            IntPtr engine, AFVector3 origin, AFVector3 dir, float maxDist, int maxBounces,
            [In, Out] AFVector3[] outPoints, int maxPoints);

        // エコグラム（到達時間ビン）。outBins は numBins 個以上。書き込んだビン数を返す。
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern int AcousticEngine_ComputeEchogram(
            IntPtr engine, AFVector3 listener, [In] AFVector3[] sources, int count,
            int numRays, int maxBounces, [Out] float[] outBins, int numBins,
            float binSeconds, float speedOfSound);

        // 音源面のグリッド各点 → リスナーへの直接到達量(0..1, -1=面外)を outGrid に書き込む。
        // outGrid は呼び出し側で cols*rows 以上確保。書き込んだセル数を返す。
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern int AcousticEngine_ComputeFaceReachability(
            IntPtr engine, AFVector3 faceCenter, AFVector3 faceNormal, AFVector3 faceRight,
            float halfWidth, float halfHeight, int shape, AFVector3 listener,
            [Out] float[] outGrid, int cols, int rows);

        // 音源指向性の広帯域ゲイン(0..1)を返す。outLowpass にこもり量(0..1)を書き戻す。
        // エンジンハンドル不要（ジオメトリに依存しない純幾何計算）。
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern float AcousticEngine_ComputeDirectivity(
            AFVector3 sourcePos, AFVector3 sourceForward, AFVector3 listenerPos,
            int directivityType, out float outLowpass);

        // ===== 新アーキ Phase1: Scene ベース ABI (acoustic_scene.h) =====
        // インスタンス方式(geomId+OBB transform+matId)の幾何/音響クエリ。
        // 旧 AddBox/IsOccluded/ComputeOcclusion 系を置き換える。ハンドルは別系統(AF_Scene*)。

        [DllImport(Dll, CallingConvention = Cc)]
        public static extern IntPtr AF_SceneCreate();

        [DllImport(Dll, CallingConvention = Cc)]
        public static extern void AF_SceneDestroy(IntPtr scene);

        // 帯域別材質を登録し materialId を返す。各配列は 6 要素 or null（null は既定壁）。
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern int AF_SceneAddMaterial(
            IntPtr scene, [In] float[] transmission, [In] float[] absorption,
            [In] float[] scattering, int numBands);

        // OBB インスタンスを追加し instanceId を返す（失敗 -1）。right/up は transform.right/up。
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern int AF_SceneAddInstanceBox(
            IntPtr scene, AFVector3 center, AFVector3 halfExtents,
            AFVector3 right, AFVector3 up, int materialId);

        // 既存インスタンスの transform を更新（動いた分だけ）。
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern void AF_SceneUpdateInstance(
            IntPtr scene, int instanceId, AFVector3 center, AFVector3 halfExtents,
            AFVector3 right, AFVector3 up);

        [DllImport(Dll, CallingConvention = Cc)]
        public static extern void AF_SceneSetInstanceActive(IntPtr scene, int instanceId, int active);

        [DllImport(Dll, CallingConvention = Cc)]
        public static extern void AF_SceneClearInstances(IntPtr scene);

        // from→to の帯域別透過ゲイン(0..1)を outGains(6要素以上)に書く。書き込んだ帯域数を返す。
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern int AF_SceneComputeTransmissionBands(
            IntPtr scene, AFVector3 from, AFVector3 to, [Out] float[] outGains, int count);

        [DllImport(Dll, CallingConvention = Cc)]
        public static extern int AF_SceneIsOccluded(IntPtr scene, AFVector3 from, AFVector3 to);

        [DllImport(Dll, CallingConvention = Cc)]
        public static extern float AF_SceneRaycast(IntPtr scene, AFVector3 origin, AFVector3 dir, float maxDist);

        // 回折(Phase1.5)：from→to の帯域別回折ゲイン(0..1)。書き込んだ帯域数を返す。
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern int AF_SceneComputeDiffractionBands(
            IntPtr scene, AFVector3 from, AFVector3 to, [Out] float[] outGains, int count);

        // 回折の経路可視化：迂回の余剰長δ(m)を返し、迂回点を out で受ける。遮蔽/迂回無で -1。
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern float AF_SceneDiffractionPath(
            IntPtr scene, AFVector3 from, AFVector3 to, out AFVector3 outMidPoint);

        // 役割2：反射込み遮蔽。直接(透過⊕回折)＋反射で回り込む成分から遮蔽量(0..1)を返す。
        // outBands6 に 直接⊕回折⊕反射 の帯域別生存(0..1) を書く（null 可）。
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern float AF_SceneOcclusionReflected(
            IntPtr scene, AFVector3 source, AFVector3 listener,
            [Out] float[] outBands6, int numRays, int maxBounces);

        // 役割2・複数音源（共有レイ1回で全音源へ）。outOcc[count] に音源ごとの遮蔽量、
        // outBands[count*6] に帯域別生存を書く（どちらも null 可）。raycast は音源数非依存。
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern void AF_SceneOcclusionReflectedMulti(
            IntPtr scene, AFVector3 listener, [In] AFVector3[] sources, int count,
            [Out] float[] outOcc, [Out] float[] outBands, [Out] float[] outDir,
            float directWeight, int numRays, int maxBounces);

        // 残響：到達時間ビンのエコグラムを outBins[numBins] に書く（RT60/wet 算出用）。
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern void AF_SceneComputeEchogram(
            IntPtr scene, AFVector3 listener, [In] AFVector3[] sources, int count,
            [Out] float[] outBins, int numBins, float binSeconds, float speedOfSound,
            int numRays, int maxBounces);

        // 残響：帯域別エコグラムを outBins[numBins*6] に書く（実測された尾のIR生成用）。
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern void AF_SceneComputeEchogramBands(
            IntPtr scene, AFVector3 listener, [In] AFVector3[] sources, int count,
            [Out] float[] outBins, int numBins, float binSeconds, float speedOfSound,
            int numRays, int maxBounces, float distanceRef);

        // 反射経路トレース：origin→dir を鏡面反射で maxBounces 回追い、通過点を outPoints に書く。
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern int AF_SceneTraceReflectionPath(
            IntPtr scene, AFVector3 origin, AFVector3 dir, float maxDist, int maxBounces,
            [In, Out] AFVector3[] outPoints, int maxPoints);

        // B: キューブマップ エッジカタログ構築（リスナー中心・全音源共有）。
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern void AF_SceneBuildEdgeCatalog(IntPtr scene, AFVector3 listener, int res, float maxDist);
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern int AF_SceneEdgeCatalogCount(IntPtr scene);
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern void AF_SceneClearEdgeCatalog(IntPtr scene);

        // 可視化：遮蔽時の回折候補の迂回点 P と余剰δを outPoints/outDeltas に書き、個数を返す。
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern int AF_SceneDiffractionCandidates(
            IntPtr scene, AFVector3 from, AFVector3 to,
            [Out] AFVector3[] outPoints, [Out] float[] outDeltas, int maxCount);

        // 回折を二次音源として：遮蔽時のエッジをクラスタして方向つき仮想音源(位置+ゲイン)を返す。
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern int AF_SceneComputeDiffractionSources(
            IntPtr scene, AFVector3 listener, AFVector3 source,
            [Out] AFVector3[] outPos, [Out] float[] outGain, int maxN);

        // A: 早期反射タップ抽出。像源位置を outImagePos[maxTaps]、帯域ゲインを outGain[maxTaps*6] に書く。
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern int AF_SceneComputeEarlyReflections(
            IntPtr scene, AFVector3 listener, AFVector3 source,
            [Out] AFVector3[] outImagePos, [Out] float[] outGain,
            int maxTaps, int numRays, int maxBounces);

        [DllImport(Dll, CallingConvention = Cc)]
        public static extern int AF_SceneInstanceCount(IntPtr scene);

        [DllImport(Dll, CallingConvention = Cc)]
        public static extern int AcousticEngine_IsWwiseAvailable();

        [DllImport(Dll, CallingConvention = Cc)]
        public static extern int AcousticEngine_InitAudio();

        [DllImport(Dll, CallingConvention = Cc)]
        public static extern int AcousticEngine_IsAudioInitialized();

        [DllImport(Dll, CallingConvention = Cc)]
        public static extern void AcousticEngine_ShutdownAudio();

        // 再生系。文字列は const char*(UTF-8) なので、UTF-8 の byte[] を渡す。
        // （既定の string マーシャリングは ANSI=CP932 で、日本語パスが化けるため）
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern int AcousticEngine_SetBankPath(byte[] utf8Path);

        [DllImport(Dll, CallingConvention = Cc)]
        public static extern int AcousticEngine_LoadBank(byte[] bankName);

        [DllImport(Dll, CallingConvention = Cc)]
        public static extern void AcousticEngine_RegisterGameObject(ulong id, byte[] name);

        [DllImport(Dll, CallingConvention = Cc)]
        public static extern void AcousticEngine_UnregisterGameObject(ulong id);

        [DllImport(Dll, CallingConvention = Cc)]
        public static extern void AcousticEngine_SetGameObjectPosition(ulong id, AFVector3 position, AFVector3 front, AFVector3 top);

        [DllImport(Dll, CallingConvention = Cc)]
        public static extern void AcousticEngine_SetDefaultListener(ulong id);

        [DllImport(Dll, CallingConvention = Cc)]
        public static extern uint AcousticEngine_PostEvent(byte[] eventName, ulong gameObjectId);

        [DllImport(Dll, CallingConvention = Cc)]
        public static extern void AcousticEngine_ExecuteActionOnEvent(byte[] eventName, int actionType, ulong gameObjectId);

        [DllImport(Dll, CallingConvention = Cc)]
        public static extern void AcousticEngine_SetObstructionOcclusion(ulong emitterId, ulong listenerId, float obstruction, float occlusion);

        [DllImport(Dll, CallingConvention = Cc)]
        public static extern void AcousticEngine_SetEmitterListenerVolume(ulong emitterId, ulong listenerId, float volume);

        // State 設定（HRTF↔パンニング切替など）。文字列は UTF-8 byte[]。
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern void AcousticEngine_SetState(byte[] stateGroup, byte[] state);

        // RTPC 設定（残響パラメータ駆動など）。名前は UTF-8 byte[]。
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern void AcousticEngine_SetRTPCValue(byte[] name, float value);

        // RTPC をゲームオブジェクト単位で設定（帯域別EQの音源ごと駆動など）。
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern void AcousticEngine_SetRTPCValueOnObject(byte[] name, float value, ulong gameObjectId);

        // 早期反射(A): AkReflect の aux バスへ像源(位置+線形レベル)を設定。毎フレーム呼ぶ想定。
        // auxBusName は UTF-8 byte[]（null/空で authoring 既定バス）。positions[count*3], levels[count]。
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern void AcousticEngine_SetEarlyReflections(
            ulong emitterId, byte[] auxBusName, [In] float[] positions, [In] float[] levels, int count);

        // 出力(マスターバス)の左右レベル(RMS, 線形)を取得。
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern void AcousticEngine_GetOutputLevels(out float outLeft, out float outRight);

        [DllImport(Dll, CallingConvention = Cc)]
        public static extern void AcousticEngine_RenderAudio();
    }
}
