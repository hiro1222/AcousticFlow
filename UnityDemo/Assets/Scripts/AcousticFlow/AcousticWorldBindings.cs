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

    // 聞こえている音の到来（AF ツールの配分タブ用）。acoustic_world.h の AF_Arrival と並びを揃える（int×2、float×6）。
    [StructLayout(LayoutKind.Sequential)]
    public struct AFArrival
    {
        // 0 直接 / 1 初期（虚像）/ 2 初期（方向なし）/ 3 回折 / 4 透過 /
        // 5 後期・耳の部屋の響き / 6 後期・戸口から直接 / 7 後期・戸口から流した響き / 8 後期・戸口の向きの点（案1）
        public int kind;
        public int emitter;
        public float dirX, dirY, dirZ;   // リスナー座標（+x 右 / +y 上 / +z 前）。全方向なら 0
        public float spread;             // 0 点 … 1 一様
        public float energy;             // 耳に届く量（帯域の平均エネルギー）
        public float delaySec;
        public float originX, originY, originZ;   // 出どころ（world、地図用）
        public int hasOrigin;                     // 出どころが意味を持つか（全方向の分は 0）
        public int box;                           // 初期（虚像）が耳の側で返った箱（無ければ -1）
    }

    // 地図用の箱（AF ツールの配分タブ）。acoustic_world.h の AF_BoxInfo と並びを揃える（float×15、int×2）。
    [StructLayout(LayoutKind.Sequential)]
    public struct AFBoxInfo
    {
        public float cx, cy, cz, hx, hy, hz;
        public float xx, xy, xz, yx, yy, yz, zx, zy, zz;   // 軸 X / Y / Z（world）
        public int dynamic, active;
    }

    // 地図用の戸口。AF_ApertureInfo と並びを揃える（float×12、int×2）。
    [StructLayout(LayoutKind.Sequential)]
    public struct AFApertureInfo
    {
        public float cx, cy, cz, ux, uy, uz, vx, vy, vz;
        public float halfU, halfV, openFrac;
        public int roomA, roomB;
    }

    public static class NativeWorld
    {
        private const string Dll = "AcousticEngine";
        private const CallingConvention Cc = CallingConvention.Cdecl;

        [DllImport(Dll, CallingConvention = Cc)] public static extern IntPtr AF_WorldCreate();
        [DllImport(Dll, CallingConvention = Cc)] public static extern void AF_WorldDestroy(IntPtr w);

        [DllImport(Dll, CallingConvention = Cc)] public static extern int AF_WorldAddMaterial(IntPtr w, float[] transmission6, float[] absorption6, float[] scattering6);
        [DllImport(Dll, CallingConvention = Cc)] public static extern int AF_WorldAddMaterialPreset(IntPtr w, int preset);
        // 実行中の材質の調整（2026-09-12）。静的な箱に効く変更は次の Update で部屋を組み直す（1 回）。動く箱だけなら組み直さない
        [DllImport(Dll, CallingConvention = Cc)] public static extern void AF_WorldSetBoxMaterial(IntPtr w, int box, int material);
        [DllImport(Dll, CallingConvention = Cc)] public static extern int AF_WorldUpdateMaterial(IntPtr w, int material, [In] float[] transmission6, [In] float[] absorption6, [In] float[] scattering6);
        [DllImport(Dll, CallingConvention = Cc)] public static extern int AF_WorldUpdateMaterialPreset(IntPtr w, int material, int preset);
        [DllImport(Dll, CallingConvention = Cc)] public static extern int AF_WorldMaterialCount(IntPtr w);

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
        // 戸口寄せ（0..1）。耳の部屋へ流す分のうちこの割合を戸口の線音源から直接鳴らす。総量は変えない。実行中に動かせる
        [DllImport(Dll, CallingConvention = Cc)] public static extern void AF_WorldSetDoorPull(IntPtr w, float pull);
        // 壁越しの反射（既定 0 ＝ 通さない）。1 で旧: 壁を横切った反射と残響も透過率で薄めて届ける。実行中に切り替えてよい
        [DllImport(Dll, CallingConvention = Cc)] public static extern void AF_WorldSetWallReflect(IntPtr w, int on);
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
        [DllImport(Dll, CallingConvention = Cc)] public static extern int AF_WorldArrivals(IntPtr w, int emitter, [Out] AFArrival[] buf, int maxOut);
        [DllImport(Dll, CallingConvention = Cc)] public static extern int AF_WorldBoxCount(IntPtr w);
        [DllImport(Dll, CallingConvention = Cc)] public static extern int AF_WorldBoxInfo(IntPtr w, int box, out AFBoxInfo info);
        [DllImport(Dll, CallingConvention = Cc)] public static extern int AF_WorldApertureInfo(IntPtr w, int aperture, out AFApertureInfo info);
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
