// AcousticWorldBindings.cs ── 新コア（Flow）の C API の入口（段 4）。DLL の関数宣言と受け渡しの型だけ。
//
// ■ 全体の中の位置
//   AcousticWorld（何をいつ呼ぶか）→ **ここ**（呼び方）→ AcousticEngine.dll（音の計算）。
//   この 1 枚が Unity と engine の境目で、ここから先に C# は無い。
//   ★Unreal へ載せ替えるときに捨てるのはこのファイルだけ（C++ からは acoustic_world.h を直に include する）。
//
// ■ 役割
//   acoustic_world.h と 1 対 1 の宣言。判断も計算も書かない（決めごと: 音の計算は engine、C# は操作だけ）。
//
// ■ 中の仕組み
//   すべて Cdecl の DllImport。構造体は C の並びと 1 バイトも揃える必要があるので LayoutKind.Sequential、
//   配列は ByValArray で長さを固定する。並びがずれると値が黙って混ざる（落ちない ＝ いちばん見つけにくい）。
//   関数は 7 つの塊に分かれている: 作る／材質／箱と場面／聞き手と音源／レイと予算／模型と演出の摘み／毎フレームと読み出し。
//
// ■ 繋がり
//   受ける: AcousticWorld / AcousticToolsWindow（表示用の読み出し）。渡す: DLL。
//
// ■ 退けた書き方
//   ・C++/CLI のラッパ: Unity では使えない。生の DllImport が最も薄い。
//   ・構造体を byte[] で受けて自前で解く: 並びのずれには強いが、全部の欄で解く手間が毎回かかる。
//
// ■ 壊れる所
//   ・★AFMixInfo は C の struct と並びを揃えること（float×6、float×30、float、float、int×4、float、int×2）。
//   ・DLL を作り直したのに Unity が古い DLL を掴んだままだと、ここは何も言わずに古い挙動で動く。
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

    // 外の器（Wwise のプラグイン）との台帳（acoustic_host.h と 1 対 1、2026-09-29）。
    //   ゲームのスレッドが「どの Voice が今どこか」と共有の器を毎フレーム書いて公開し、Wwise の音のスレッドが写しを取って鳴らす。
    public static class NativeHost
    {
        private const string Dll = "AcousticEngine";
        private const CallingConvention Cc = CallingConvention.Cdecl;
        [DllImport(Dll, CallingConvention = Cc)] public static extern void AF_HostBegin();
        [DllImport(Dll, CallingConvention = Cc)] public static extern void AF_HostPutVoice(int key, IntPtr voice, float x, float y, float z);
        [DllImport(Dll, CallingConvention = Cc)] public static extern void AF_HostSetListener(float px, float py, float pz, float fx, float fy, float fz, float ux, float uy, float uz);
        [DllImport(Dll, CallingConvention = Cc)] public static extern void AF_HostSetShared(IntPtr fdn, IntPtr bus, int sampleRate);
        [DllImport(Dll, CallingConvention = Cc)] public static extern void AF_HostCommit();
        // 壊すのは 1 秒後（Wwise の音のスレッドが写しの中で握っている間は壊せない）。Voice を先に、HRTF は少し後で。
        [DllImport(Dll, CallingConvention = Cc)] public static extern void AF_HostRetireVoice(IntPtr voice);
        [DllImport(Dll, CallingConvention = Cc)] public static extern void AF_HostRetireHrtf(IntPtr hrtf);
        [DllImport(Dll, CallingConvention = Cc)] public static extern void AF_HostTick();
        // Wwise が鳴らしているか（プラグインから 0.5 秒以内に報告が来たか）と、対応付けの結果（表示用）。
        [DllImport(Dll, CallingConvention = Cc)] public static extern int  AF_HostActive();
        [DllImport(Dll, CallingConvention = Cc)] public static extern void AF_HostStats(out int matched, out int missed, out int space, out float meanErr, out int blocks);
    }

    public static class NativeWorld
    {
        private const string Dll = "AcousticEngine";
        private const CallingConvention Cc = CallingConvention.Cdecl;

        // ── 作る・壊す ──
        [DllImport(Dll, CallingConvention = Cc)] public static extern IntPtr AF_WorldCreate();
        [DllImport(Dll, CallingConvention = Cc)] public static extern void AF_WorldDestroy(IntPtr w);

        // ── 材質。add は場面を組むとき、Update は実行中の調整（静的な箱に効くなら次の Update で 1 回だけ組み直す）──
        [DllImport(Dll, CallingConvention = Cc)] public static extern int AF_WorldAddMaterial(IntPtr w, float[] transmission6, float[] absorption6, float[] scattering6);
        [DllImport(Dll, CallingConvention = Cc)] public static extern int AF_WorldAddMaterialPreset(IntPtr w, int preset);
        // 実行中の材質の調整（2026-09-12）。静的な箱に効く変更は次の Update で部屋を組み直す（1 回）。動く箱だけなら組み直さない
        [DllImport(Dll, CallingConvention = Cc)] public static extern void AF_WorldSetBoxMaterial(IntPtr w, int box, int material);
        [DllImport(Dll, CallingConvention = Cc)] public static extern int AF_WorldUpdateMaterial(IntPtr w, int material, [In] float[] transmission6, [In] float[] absorption6, [In] float[] scattering6);
        [DllImport(Dll, CallingConvention = Cc)] public static extern int AF_WorldUpdateMaterialPreset(IntPtr w, int material, int preset);
        [DllImport(Dll, CallingConvention = Cc)] public static extern int AF_WorldMaterialCount(IntPtr w);

        // ── 箱（場面の形）。dynamic=1 の箱（扉）は動かしても部屋グラフを組み直さない ──
        [DllImport(Dll, CallingConvention = Cc)] public static extern int AF_WorldAddBox(IntPtr w, AFVector3 center, AFVector3 halfExtents, AFVector3 axisX, AFVector3 axisY, int material, int dynamic);
        [DllImport(Dll, CallingConvention = Cc)] public static extern void AF_WorldSetBoxTransform(IntPtr w, int box, AFVector3 center, AFVector3 axisX, AFVector3 axisY);
        [DllImport(Dll, CallingConvention = Cc)] public static extern void AF_WorldSetBoxActive(IntPtr w, int box, int active);

        // ── 部屋グラフ（箱から部屋と戸口を作る。数百 ms なので場面を組むときだけ）と、その読み出し ──
        [DllImport(Dll, CallingConvention = Cc)] public static extern void AF_WorldBuild(IntPtr w);
        [DllImport(Dll, CallingConvention = Cc)] public static extern int AF_WorldRoomCount(IntPtr w);
        [DllImport(Dll, CallingConvention = Cc)] public static extern int AF_WorldBuildCount(IntPtr w);
        [DllImport(Dll, CallingConvention = Cc)] public static extern int AF_WorldRoomAt(IntPtr w, AFVector3 p);
        [DllImport(Dll, CallingConvention = Cc)] public static extern int AF_WorldRoomInfo(IntPtr w, int room, [Out] float[] outRt60_6, out float outVolume, out float outSurface, out float outMeanFreePath);

        // ── 聞き手と音源（毎フレーム押す。位置・向き・幅・優先度）──
        [DllImport(Dll, CallingConvention = Cc)] public static extern void AF_WorldSetListener(IntPtr w, AFVector3 pos, AFVector3 forward, AFVector3 up);
        [DllImport(Dll, CallingConvention = Cc)] public static extern int AF_WorldAddEmitter(IntPtr w, AFVector3 pos, float radius);
        [DllImport(Dll, CallingConvention = Cc)] public static extern void AF_WorldRemoveEmitter(IntPtr w, int emitter);
        [DllImport(Dll, CallingConvention = Cc)] public static extern void AF_WorldSetEmitter(IntPtr w, int emitter, AFVector3 pos, float radius, int operated, float loudness);

        // ── レイと計算の資源 ──
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
        // 初期反射の出し方（既定 4）。0 虚像 / 1 壁の受取面 / 2 虚像を面でつなぐ / 3 虚像の網 / 4 虚像の面音源。実行中に切り替えてよい
        [DllImport(Dll, CallingConvention = Cc)] public static extern void AF_WorldSetEarlyModel(IntPtr w, int model);
        // 音源ごとの上書き（−1 / 負で世界の設定）
        [DllImport(Dll, CallingConvention = Cc)] public static extern void AF_WorldSetEmitterEarlyModel(IntPtr w, int emitter, int model);
        [DllImport(Dll, CallingConvention = Cc)] public static extern void AF_WorldSetEmitterAdjacentContain(IntPtr w, int emitter, float amount);
        // 隣の部屋の閉じ込め（0..1）。隣の部屋の残響・反射を耳の部屋で響かせず、戸口から鳴らす割合。実行中に動かせる
        [DllImport(Dll, CallingConvention = Cc)] public static extern void AF_WorldSetAdjacentContain(IntPtr w, float amount);
        // 虚像の面音源（earlyModel 4）の摘み。散乱の幅（度）・つなぐ角度（度）・つなぐ到達（ms）・集まりの重み・近さの重み。実行中に動かせる
        [DllImport(Dll, CallingConvention = Cc)] public static extern void AF_WorldSetImageSurface(IntPtr w, float roughDeg, float connectDeg, float connectMs, float densityPow, float nearPow);
        // 戸口の線音源の低域の相関の境（Hz、既定 3000）。下は 5 点が同じ波形、上は別の波形。0 で旧。実行中に動かせる
        [DllImport(Dll, CallingConvention = Cc)] public static extern void AF_WorldSetDoorCoherence(IntPtr w, float hz);
        // 先着の重み（dB と窓 s）。最初の到達から遅れる到来ほど出口で下げる。0 dB で今までと同じ。実行中に動かせる
        [DllImport(Dll, CallingConvention = Cc)] public static extern void AF_WorldSetPrecedence(IntPtr w, float db, float sec);
        // 自分の部屋の響きを、壁までの距離で方向バスのレーンへ配る強さ（0 で一様）。実行中に動かせる
        [DllImport(Dll, CallingConvention = Cc)] public static extern void AF_WorldSetLateDistanceShape(IntPtr w, float pow);
        // いまの配り方そのものを読む（表示・検査用）。取り分（和 1）・壁までの距離（m）・向き（リスナー座標）を書き、本数を返す。
        //   要らない配列は null で渡してよい（C 側で NULL を見て飛ばす）。
        [DllImport(Dll, CallingConvention = Cc)] public static extern int AF_WorldLateLaneShape(IntPtr w, [Out] float[] share, [Out] float[] distance, [Out] float[] dir3, int maxLanes);
        // 尾のレーンの作り（試聴の A/B 用）。0 耳ごとの行（旧）/ 1 点と拡散を分ける（既定）
        [DllImport(Dll, CallingConvention = Cc)] public static extern void AF_WorldSetLaneModel(IntPtr w, int model);
        [DllImport(Dll, CallingConvention = Cc)] public static extern void AF_WorldSetWeights(IntPtr w, float[] w5);
        // 隣の部屋の響き（2026-10-01）。別の部屋の音源の後期だけに、後期の重みへさらに掛ける（エネルギー比、既定 1）
        [DllImport(Dll, CallingConvention = Cc)] public static extern void AF_WorldSetAdjacentLateWeight(IntPtr w, float weight);
        [DllImport(Dll, CallingConvention = Cc)] public static extern void AF_WorldSetShadowMuffle(IntPtr w, float db);
        [DllImport(Dll, CallingConvention = Cc)] public static extern void AF_WorldSetShadowMuffleFullHz(IntPtr w, float hz);
        // 部屋の割り方（2026-10-04）。部屋を口で割る半径(m、既定 0.6)と、外への口（既定 0）。どちらも AF_WorldBuild の前に
        [DllImport(Dll, CallingConvention = Cc)] public static extern void AF_WorldSetRoomSeedRadius(IntPtr w, float meters);
        [DllImport(Dll, CallingConvention = Cc)] public static extern void AF_WorldSetOutsideMouth(IntPtr w, int on);
        [DllImport(Dll, CallingConvention = Cc)] public static extern void AF_WorldSetResponse(IntPtr w, float levelSec, float colourSec, float statSec, float directionSec);
        [DllImport(Dll, CallingConvention = Cc)] public static extern void AF_WorldSetHeadCm(IntPtr w, float headCircumferenceCm);

        // ── 毎フレームの本体と、鳴らす先・読み出し ──
        //   Update: 幾何 → エネルギー → 配分 までを 1 回。ApplyVoice: その結果を音源の Voice へ置く。
        //   MixInfo / Arrivals / BoxInfo / ApertureInfo は表示と検査のための読み出し（音には影響しない）。
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
