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

        [DllImport(Dll, CallingConvention = Cc)]
        public static extern void AcousticEngine_ClearGeometry(IntPtr engine);

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

        // 出力(マスターバス)の左右レベル(RMS, 線形)を取得。
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern void AcousticEngine_GetOutputLevels(out float outLeft, out float outRight);

        [DllImport(Dll, CallingConvention = Cc)]
        public static extern void AcousticEngine_RenderAudio();
    }
}
