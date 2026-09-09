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
        [DllImport(Dll, CallingConvention = Cc)] public static extern void AF_WorldSetWeights(IntPtr w, float[] w5);
        [DllImport(Dll, CallingConvention = Cc)] public static extern void AF_WorldSetResponse(IntPtr w, float levelSec, float colourSec, float statSec, float directionSec);
        [DllImport(Dll, CallingConvention = Cc)] public static extern void AF_WorldSetHeadCm(IntPtr w, float headCircumferenceCm);

        [DllImport(Dll, CallingConvention = Cc)] public static extern void AF_WorldUpdate(IntPtr w, float dt);
        [DllImport(Dll, CallingConvention = Cc)] public static extern void AF_WorldBindFdn(IntPtr w, IntPtr fdn);
        [DllImport(Dll, CallingConvention = Cc)] public static extern int AF_WorldFdnStale(IntPtr w);
        [DllImport(Dll, CallingConvention = Cc)] public static extern void AF_WorldApplyVoice(IntPtr w, int emitter, IntPtr voice, int sampleRate);
        [DllImport(Dll, CallingConvention = Cc)] public static extern int AF_WorldMixInfo(IntPtr w, int emitter, out AFMixInfo info);
        [DllImport(Dll, CallingConvention = Cc)] public static extern int AF_WorldApertureCount(IntPtr w);
        [DllImport(Dll, CallingConvention = Cc)] public static extern float AF_WorldApertureOpenFrac(IntPtr w, int aperture);
    }
}
