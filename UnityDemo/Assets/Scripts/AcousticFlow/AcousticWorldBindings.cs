// AcousticWorldBindings.cs ── 新コア（Flow）の C API の入口（段 4）。
//   acoustic_world.h と 1 対 1。ここには判断も計算も書かない（C# は「音の面の操作」だけ）。
//   ★AFMixInfo は C の struct と並びを揃えること（float×6、float×30、float、float、int×4、float、int×2）。
using System;
using System.Runtime.InteropServices;

namespace AcousticFlow
{
    [StructLayout(LayoutKind.Sequential)]
    public struct AFMixInfo
    {
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 6)]  public float[] energy6;      // 総量（生）
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 30)] public float[] component;    // 五成分 × 6 帯域（生）: 直接・初期・後期・回折・透過
        public float onsetSec;
        public float directSec;
        public int tapCount, sendCount, room, directCrossings;
        public float firstReflectSec;
        public int raysTraced, hits;
        public float visibleFraction;   // 見通しの割合 0..1（段 5）
        public int shadowers;
        public int imageCount;          // 有効な虚像の数（段 7）
        public int imageCandidates;
    }

    public static class NativeWorld
    {
        private const string Dll = "AcousticEngine";
        private const CallingConvention Cc = CallingConvention.Cdecl;

        [DllImport(Dll, CallingConvention = Cc)] public static extern IntPtr AF_WorldCreate();
        [DllImport(Dll, CallingConvention = Cc)] public static extern void AF_WorldDestroy(IntPtr w);

        [DllImport(Dll, CallingConvention = Cc)] public static extern int AF_WorldAddMaterial(IntPtr w, float[] transmission6, float[] absorption6, float[] scattering6);
        [DllImport(Dll, CallingConvention = Cc)] public static extern int AF_WorldAddMaterialPreset(IntPtr w, int preset);

        [DllImport(Dll, CallingConvention = Cc)] public static extern int AF_WorldAddBox(IntPtr w, AFVector3 center, AFVector3 halfExtents, AFVector3 axisX, AFVector3 axisY, int material, int dynamic);
        [DllImport(Dll, CallingConvention = Cc)] public static extern void AF_WorldSetBoxTransform(IntPtr w, int box, AFVector3 center, AFVector3 axisX, AFVector3 axisY);
        [DllImport(Dll, CallingConvention = Cc)] public static extern void AF_WorldSetBoxActive(IntPtr w, int box, int active);

        [DllImport(Dll, CallingConvention = Cc)] public static extern void AF_WorldBuild(IntPtr w);
        [DllImport(Dll, CallingConvention = Cc)] public static extern int AF_WorldRoomCount(IntPtr w);
        [DllImport(Dll, CallingConvention = Cc)] public static extern int AF_WorldBuildCount(IntPtr w);
        [DllImport(Dll, CallingConvention = Cc)] public static extern int AF_WorldRoomAt(IntPtr w, AFVector3 p);
        [DllImport(Dll, CallingConvention = Cc)] public static extern int AF_WorldRoomInfo(IntPtr w, int room, [Out] float[] outRt60_6, out float outVolume, out float outSurface, out float outMeanFreePath);

        [DllImport(Dll, CallingConvention = Cc)] public static extern void AF_WorldSetListener(IntPtr w, AFVector3 pos, AFVector3 forward, AFVector3 up);
        [DllImport(Dll, CallingConvention = Cc)] public static extern int AF_WorldAddEmitter(IntPtr w, AFVector3 pos, float radius);
        [DllImport(Dll, CallingConvention = Cc)] public static extern void AF_WorldRemoveEmitter(IntPtr w, int emitter);
        [DllImport(Dll, CallingConvention = Cc)] public static extern void AF_WorldSetEmitter(IntPtr w, int emitter, AFVector3 pos, float radius, int operated, float loudness);

        [DllImport(Dll, CallingConvention = Cc)] public static extern void AF_WorldSetRays(IntPtr w, int raysPerEmitter, int maxBounces);
        // レイを GPU で解くか（0 切／1 入）。音は作らない。GPU が出すのは幾何と統計だけ。
        [DllImport(Dll, CallingConvention = Cc)] public static extern void AF_WorldSetGpuTrace(IntPtr w, int on);
        [DllImport(Dll, CallingConvention = Cc)] public static extern int  AF_WorldGpuActive(IntPtr w);

        // 閉じた扉から漏れる回折の扱い（試聴の A/B 用）。0 今のまま / 1 厚みの割合 / 2 口の空き具合 / 3 両方
        [DllImport(Dll, CallingConvention = Cc)] public static extern void AF_WorldSetLeakModel(IntPtr w, int model);
        // 隣の部屋の後期の鳴らし方（試聴の A/B 用）。0 旧（一様）/ 1 戸口の向きの点 / 2 戸口の線音源（既定）
        [DllImport(Dll, CallingConvention = Cc)] public static extern void AF_WorldSetLateThrough(IntPtr w, int on);
        // 尾のレーンの作り（試聴の A/B 用）。0 耳ごとの行（旧）/ 1 点と拡散を分ける（既定）
        [DllImport(Dll, CallingConvention = Cc)] public static extern void AF_WorldSetLaneModel(IntPtr w, int model);
        [DllImport(Dll, CallingConvention = Cc)] public static extern void AF_WorldSetWeights(IntPtr w, float[] w5);
        [DllImport(Dll, CallingConvention = Cc)] public static extern void AF_WorldSetResponse(IntPtr w, float levelSec, float colourSec, float statSec, float directionSec);
        [DllImport(Dll, CallingConvention = Cc)] public static extern void AF_WorldSetHeadCm(IntPtr w, float headCircumferenceCm);

        [DllImport(Dll, CallingConvention = Cc)] public static extern void AF_WorldUpdate(IntPtr w, float dt);
        [DllImport(Dll, CallingConvention = Cc)] public static extern void AF_WorldBindFdn(IntPtr w, IntPtr fdn);
        [DllImport(Dll, CallingConvention = Cc)] public static extern int AF_WorldFdnStale(IntPtr w);
        [DllImport(Dll, CallingConvention = Cc)] public static extern void AF_WorldApplyVoice(IntPtr w, int emitter, IntPtr voice, int sampleRate);
        [DllImport(Dll, CallingConvention = Cc)] public static extern int AF_WorldMixInfo(IntPtr w, int emitter, out AFMixInfo info);
        [DllImport(Dll, CallingConvention = Cc)] public static extern void AF_WorldSetBudget(IntPtr w, int totalRays, int fullSlots, int lightSlots, int probesPerFrame);
        [DllImport(Dll, CallingConvention = Cc)] public static extern void AF_WorldSetRayGroups(IntPtr w, int rayGroups);
        // 1 音源の本数の上限（既定 512）。GPU で本数を増やすときはこれも上げる（raysPerEmitter だけでは頭打ち）。
        [DllImport(Dll, CallingConvention = Cc)] public static extern void AF_WorldSetMaxRaysPerEmitter(IntPtr w, int maxRays);
        [DllImport(Dll, CallingConvention = Cc)] public static extern void AF_WorldSetWorkers(IntPtr w, int workers);
        [DllImport(Dll, CallingConvention = Cc)] public static extern int AF_WorldSpentRays(IntPtr w);
        [DllImport(Dll, CallingConvention = Cc)] public static extern int AF_WorldEmitterTier(IntPtr w, int emitter);
        [DllImport(Dll, CallingConvention = Cc)] public static extern int AF_WorldEmitterRays(IntPtr w, int emitter);
        [DllImport(Dll, CallingConvention = Cc)] public static extern int AF_WorldApertureCount(IntPtr w);
        [DllImport(Dll, CallingConvention = Cc)] public static extern float AF_WorldApertureOpenFrac(IntPtr w, int aperture);
    }
}
