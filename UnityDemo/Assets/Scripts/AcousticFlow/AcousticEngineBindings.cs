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
    // BVH の節（C 側 AF_BvhNode と同じ並び）。GPU の compute shader へ渡すための物。
    [StructLayout(LayoutKind.Sequential)]
    public struct AFBvhNode { public float minX, minY, minZ, maxX, maxY, maxZ; public int leftFirst, count; }
    // インスタンスの記述（C 側 AF_InstanceDesc と同じ並び）。
    [StructLayout(LayoutKind.Sequential)]
    public struct AFInstanceDesc
    {
        public AFVector3 center, halfExtents, axisX, axisY, axisZ;
        public int materialId, geomId, active, moved, dynamicTag;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct AFVector3
    {
        public float x;
        public float y;
        public float z;

        public AFVector3(Vector3 v) { x = v.x; y = v.y; z = v.z; }
        public Vector3 ToVector3() { return new Vector3(x, y, z); }
    }

    // バッチ更新（AF_SceneUpdate）の設定。C 側 AF_UpdateConfig と同じ並び・型であること。
    //
    // 他の interop 型（AFVector3 / Native）は internal で隠しているが、これは
    // 「ホストがエンジンに何をどのレートで計算させるか」を指定する公開 API の一部なので public。
    // ホストが持つべき設定であり、interop の実装詳細ではない。
    [StructLayout(LayoutKind.Sequential)]
    public struct AcousticUpdateConfig
    {
        // 役割ごとの更新間隔（フレーム）。1=毎フレーム。0以下はエンジン側で1に丸める。
        public int role1EveryN, role2EveryN, earlyEveryN, diffSrcEveryN, catalogEveryN;

        // 役割1: 反射込み遮蔽
        public int reflectionRays, reflectionBounces;
        public float directWeight;
        public int useReflections;          // 0/1

        // エッジカタログ（回折が使う）
        public int useEdgeCatalog, edgeCatalogRes;
        public float edgeCatalogMaxDist;

        // 役割2: 残響（エコグラム）
        public int enableReverb, echogramBins;
        public float echogramBinSeconds;
        public int echogramRays, echogramBounces;
        public float speedOfSound, distanceRef;

        // 役割2: 早期反射 / 回折二次音源
        public int enableEarlyReflections, earlyTaps, earlyRays, earlyBounces;
        public int enableDiffractionSources, diffSources;

        // 2026-09-03 早期反射の模型。0 = 像源をレイで拾う（旧）／1 = 面ごとの線音源（既定）。
        //   earlyFaceSubTaps = 面 1 枚あたりの下位タップ数。echogramSkipFirstOrder = 尾から 1 次反射を外す（二重防止）。
        public int earlyModel, earlyFaceSubTaps, echogramSkipFirstOrder;
    }

    internal static class Native
    {
        // 拡張子なしの "AcousticEngine" にしておくと、Windows では
        // AcousticEngine.dll が解決される（Assets/Plugins/x86_64 配下）。
        private const string Dll = "AcousticEngine";
        private const CallingConvention Cc = CallingConvention.Cdecl;

        // ===== 新アーキ Phase1: Scene ベース ABI (acoustic_scene.h) =====
        // インスタンス方式(geomId+OBB transform+matId)の幾何/音響クエリ。
        // 旧 AddBox/IsOccluded/ComputeOcclusion 系を置き換える。ハンドルは別系統(AF_Scene*)。

        // 材質プリセットの唯一の出どころ。ホストが自前で値を持つと必ずずれる
        // （実際 Default と Concrete の透過が C++ と 6〜8dB 食い違っていた）。
        //   preset: 0=Default / 1=Concrete / 2=Glass / 3=Opaque / 4=WoodDoor
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern int AF_MaterialPresetBands(int preset,
            [Out] float[] outTransmission, [Out] float[] outAbsorption,
            [Out] float[] outScattering);

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

        // ソフト遮蔽。直接経路の透過(振幅・6帯域)と遮蔽割合(0..1)を別々に返す。
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern int AF_SceneComputeSoftOcclusion(
            IntPtr scene, AFVector3 listener, AFVector3 source,
            float[] outTrans, int count, out float outOccFrac);

        // 各方向から「どれだけ残響が返るか」を帯域別に得る。outEnergy は dirCount*6 要素。
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern void AF_SceneProbeDirectionalEnergy(
            IntPtr scene, AFVector3 origin, AFVector3[] dirs, int dirCount,
            int maxBounces, float[] outEnergy);

        // 三角形メッシュを形状(BLAS)として登録し geomId を返す（失敗 -1）。
        //   outLocalCenter / outLocalHalfExtents に、正規化に使ったローカルAABBが返る。
        //   ホストはこれを使ってインスタンスの OBB（＝変換）を組む。
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern int AF_SceneAddMesh(
            IntPtr scene, float[] verticesXYZ, int vertexCount, int[] indices, int indexCount,
            out AFVector3 outLocalCenter, out AFVector3 outLocalHalfExtents);

        // 形状を解放。参照していたインスタンスは箱（境界ボックス）扱いに落ちる。
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern void AF_SceneRemoveMesh(IntPtr scene, int geomId);

        // メッシュ形状のインスタンスを追加し instanceId を返す（失敗 -1）。
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern int AF_SceneAddInstanceMesh(
            IntPtr scene, int geomId, AFVector3 center, AFVector3 halfExtents,
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

        // リスナー/音源の保持（段1）。エンジンが内部で音源ループを回せるようにする。
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern void AF_SceneSetListener(IntPtr scene, AFVector3 pos);

        [DllImport(Dll, CallingConvention = Cc)]
        public static extern void AF_SceneSetSource(IntPtr scene, ulong id, AFVector3 pos);

        [DllImport(Dll, CallingConvention = Cc)]
        public static extern void AF_SceneRemoveSource(IntPtr scene, ulong id);

        [DllImport(Dll, CallingConvention = Cc)]
        public static extern void AF_SceneClearSources(IntPtr scene);

        [DllImport(Dll, CallingConvention = Cc)]
        public static extern int AF_SceneSourceCount(IntPtr scene);

        [DllImport(Dll, CallingConvention = Cc)]
        public static extern void AF_SceneSetUpdateConfig(IntPtr scene, ref AcousticUpdateConfig cfg);

        [DllImport(Dll, CallingConvention = Cc)]
        public static extern void AF_SceneUpdate(IntPtr scene, float dt);

        [DllImport(Dll, CallingConvention = Cc)]
        public static extern int AF_SceneSourceIndex(IntPtr scene, ulong id);

        [DllImport(Dll, CallingConvention = Cc)]
        public static extern void AF_SceneGetSourceOcclusion(IntPtr scene, int index, [Out] float[] out6);

        [DllImport(Dll, CallingConvention = Cc)]
        public static extern float AF_SceneGetSourceOcclusionScalar(IntPtr scene, int index);

        [DllImport(Dll, CallingConvention = Cc)]
        public static extern void AF_SceneGetSourceArrivalDir(IntPtr scene, int index, [Out] float[] out3);

        [DllImport(Dll, CallingConvention = Cc)]
        public static extern int AF_SceneGetEarlyReflections(
            IntPtr scene, int index, [In, Out] AFVector3[] outPos, [Out] float[] outGain6, int maxTaps);

        // 焼く層: 静的な面のリストとセルごとの見通し。戻り値は焼いたセル数。
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern int AF_SceneBakeStaticFaces(IntPtr scene, float cellSize, int subTaps);

        [DllImport(Dll, CallingConvention = Cc)]
        public static extern void AF_SceneClearFaceBake(IntPtr scene);

        [DllImport(Dll, CallingConvention = Cc)]
        public static extern int AF_SceneFaceBakeFaceCount(IntPtr scene);

        // 音響的に動く物のタグ（焼く層に入れない）。
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern void AF_SceneSetInstanceDynamic(IntPtr scene, int instanceId, int dynamic);

        // 外の作り手のための口（GPU 化の下ごしらえ）: BVH の書き出しと、面の焼きの出し入れ。
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern int AF_SceneBvhNodeCount(IntPtr scene);
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern int AF_SceneBvhOrderCount(IntPtr scene);
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern int AF_SceneExportBvh(IntPtr scene, [Out] AFBvhNode[] outNodes, int maxNodes, [Out] int[] outOrder, int maxOrder);
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern int AF_SceneGetInstance(IntPtr scene, int instanceId, out AFInstanceDesc outDesc);
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern int AF_SceneFaceBakeBytes(IntPtr scene);
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern int AF_SceneFaceBakeExport(IntPtr scene, [Out] byte[] outBytes, int cap);
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern int AF_SceneFaceBakeImport(IntPtr scene, [In] byte[] data, int size);

        [DllImport(Dll, CallingConvention = Cc)]
        public static extern int AF_SceneGetDiffractionSources(
            IntPtr scene, int index, [In, Out] AFVector3[] outPos, [Out] float[] outGain, int maxSrc);

        [DllImport(Dll, CallingConvention = Cc)]
        // 音源 index のエコグラム。index=-1 で全音源の和（部屋全体の響き）。
        public static extern int AF_SceneGetEchogramBands(IntPtr scene, int index,
                                                          [Out] float[] outBins, int numBins);

        // ===== 部屋と開口（幾何から自動検出）=====
        // 静的な形状をボクセル化して自由空間を塗り分け、狭いくびれ（戸口）で部屋を分ける。
        // 手置きのボリュームは無い。壁を壊せば部屋の繋がりもその場で変わる。
        //
        // ★部屋を音に使うときは必ず RoomWeights / RoomVolumeAt / Rt60At を通すこと。
        //   部屋番号(RoomAt)で残響を切り替えると、プレイヤーが必ず通る戸口のど真ん中に
        //   不連続を置くことになる。割合なら部屋の真ん中で 100:0、戸口で 50:50 と連続に変わる。

        [DllImport(Dll, CallingConvention = Cc)]
        public static extern int AF_SceneRoomCount(IntPtr scene);

        // 点がどの部屋か。-1 は部屋の外／実体の中。★表示・デバッグ用。音の切り替えには使わない。
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern int AF_SceneRoomAt(IntPtr scene, AFVector3 p);

        [DllImport(Dll, CallingConvention = Cc)]
        public static extern int AF_SceneRoomInfo(IntPtr scene, int room, out float outVolume,
                                                  out AFVector3 outCentroid,
                                                  out AFVector3 outMin, out AFVector3 outMax);

        // 半径 radius(m) の球の中で各部屋が占める割合（合計 1、大きい順）。書けた数を返す。
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern int AF_SceneRoomWeights(IntPtr scene, AFVector3 p, float radius,
                                                     [Out] int[] outRooms, [Out] float[] outWeights,
                                                     int maxOut);

        // 上の割合で混ぜた実効体積(m3)。0 なら部屋の外。臨界距離 rc=0.057√(V/RT60) に入れる。
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern float AF_SceneRoomVolumeAt(IntPtr scene, AFVector3 p, float radius);

        // 帯域別の残響時間(s)。部屋ごとの Sabine 値を上の割合で混ぜたもの。out は 6 要素以上。
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern int AF_SceneRt60At(IntPtr scene, AFVector3 p, float radius,
                                                [Out] float[] outRt60, int count);

        // 部屋の音響量。境界面積・開口面積・帯域別の平均吸音率と残響時間。
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern int AF_SceneRoomAcoustics(IntPtr scene, int room,
                                                       out float outSurface, out float outOpenArea,
                                                       [Out] float[] outAbsorb6,
                                                       [Out] float[] outRt60_6);

        // 部屋どうしを繋ぐ開口（戸口・窓・壊れた壁の穴）。面積の大きい順。
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern int AF_SceneApertureCount(IntPtr scene);

        [DllImport(Dll, CallingConvention = Cc)]
        public static extern int AF_SceneApertureInfo(IntPtr scene, int index, out float outArea,
                                                      out AFVector3 outCenter, out AFVector3 outNormal,
                                                      out int outRoomA, out int outRoomB);

        // ボクセル一辺(m)。既定 0.25。細かいほど狭い戸口を見分けられるがコストが増える。
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern void AF_SceneSetRoomCellSize(IntPtr scene, float meters);
        // 【C1】部屋グラフの開口からポータルを自動生成する。手置きは消えない。
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern void AF_SceneSetAutoPortals(IntPtr scene, int enable);
        // 音源ごとの段（遮蔽・回折／早期反射／回折二次音源）を何コアで回すか。1 以下＝直列。
        //   並列にしても**結果はビット一致**（回帰テストで 616 個の値を厳密比較して確認）。
        //   ⚠ スレッドはシーンが持ち AF_SceneDestroy で join される。
        //      OnDisable → Dispose の経路を切らないこと。切るとドメインリロードで固まる。
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern void AF_SceneSetWorkerThreads(IntPtr scene, int threads);
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern int AF_SceneGetWorkerThreads(IntPtr scene);

        // 音源の段。0=厳密 / 1=簡易 / 2=バーチャル。**音源ごとに固定**する。
        //   ⚠ 距離で自動に切り替えないこと（歩くだけで段が変わり、切り替わりが聞こえる）。
        //   ★2D の音（UI・音楽・ナレーション）に段はない。音源として登録しないこと。
        // 【調整支援】この直線の透過損失を誰が担っているか（大きい順）。
        //   ⚠ 透過だけ。回り込み（回折）が担っている帯域では材質を触っても動かない。
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern int AF_SceneTransmissionCarriers(
            IntPtr scene, AFVector3 listener, AFVector3 source,
            int[] outInstance, int[] outMaterial, float[] outLossDb, int maxCount);
        // 材質の現在値を読む（道具が「いま幾つか」を出すのに要る）。
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern int AF_SceneGetMaterial(IntPtr scene, int materialId,
                                                     float[] outTransmission, float[] outAbsorption,
                                                     float[] outScattering, int count);
        // 材質を書き換える。**その材質を使っている実体が一斉に変わる。**
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern int AF_SceneSetMaterial(IntPtr scene, int materialId,
                                                     float[] transmission, float[] absorption,
                                                     float[] scattering, int numBands);

        [DllImport(Dll, CallingConvention = Cc)]
        public static extern void AF_SceneSetSourceTier(IntPtr scene, ulong id, int tier);
        // 自由音場で聞こえなくなる距離(m)。0 以下＝自動バーチャルを使わない。
        //   ⚠ 響く部屋（臨界距離 <= この半径）ではエンジン側が自動バーチャルを止める。
        //     残響は距離でほとんど減らないので、距離だけで切ると聞こえている音を黙らせる。
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern void AF_SceneSetSourceAudibleRadius(IntPtr scene, ulong id, float metres);
        // いま実際に使われている段: 0 厳密／1 簡易／2 バーチャル（素通し）／3 保持（予算から漏れた。止めないこと）。
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern int AF_SceneGetSourceTierEffective(IntPtr scene, int index);
        // ── 段の予算と順位（主スレッドの上限）── 厳密・簡易の本数。0 = 無制限。
        //   超えたぶんは可聴性（音量 × 1/r × 生存 × 重要度）の低い順に「保持」へ落ちる（docs/TIER_BUDGET.md）。
        // ── タップの組み立て（2026-09-05）── ホストの BuildTapsForSource を DLL へ。★C の構造体と並びを合わせること（ABI 7）。
        [StructLayout(LayoutKind.Sequential)]
        public struct AFTapParams
        {
            public float distanceRef, diffractionDistanceRef, diffractionDistancePower, airAbsorptionScale;
            public float transmissionTilt, transmissionHighCutDb, transmissionGainDb, diffractionGainDb;
            public int   diffractionDistanceOnly;
            public float diffractionHighCutDb, tapSmoothTime, directionSmoothTime, steerThreshold;
            public int   diffractionHrtf;
            public float diffractionHrtfMarginDb, reverbRatioExponent, reverbRatioCeiling, roomBlendRadius;
            public int   reverbShareFade;
            public float fallbackRt60;
            public int   maxTaps, diffractionTapReserve;
        }
        [StructLayout(LayoutKind.Sequential)]
        public struct AFProgramTap
        {
            public float delayMs;
            public float g0, g1, g2, g3, g4, g5;
            public float panL, panR;
            public float dirX, dirY, dirZ;
            public float arrX, arrY, arrZ;
            public int   type;          // 0 直接／1 反射／2 回折
            public float hrtfWeight;
        }
        public const int ProgramMaxTaps = 64;
        [StructLayout(LayoutKind.Sequential)]
        public struct AFVoiceProgram
        {
            public int count;
            [MarshalAs(UnmanagedType.ByValArray, SizeConst = ProgramMaxTaps)] public AFProgramTap[] taps;
            public float directDirX, directDirY, directDirZ;
            public int   hrtfTapIndex;
            public float hrtfDirX, hrtfDirY, hrtfDirZ;
            public float itdgMs, sourceLevel, freeFieldDirect;
            public int   tailShapeIndex;
            public float tailRatio, tailRatioPhysical, mixingTimeMs, roomShare;
            public int   tier;
        }
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern void AF_SceneSetListenerOrientation(IntPtr scene, AFVector3 forward, AFVector3 up);
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern void AF_SceneSetTapParams(IntPtr scene, ref AFTapParams p);
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern int AF_SceneGetVoiceProgram(IntPtr scene, int index, out AFVoiceProgram outProgram);

        [DllImport(Dll, CallingConvention = Cc)]
        public static extern void AF_SceneSetTierBudget(IntPtr scene, int exactMax, int simpleMax);
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern void AF_SceneSetSourceLoudness(IntPtr scene, ulong id, float gainLinear);
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern void AF_SceneSetSourceImportance(IntPtr scene, ulong id, float importance, int pinned);
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern float AF_SceneGetSourcePriority(IntPtr scene, int index);
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern int AF_SceneGetTierProbeIndex(IntPtr scene);
        // ── 更新の非同期化 ── AF_SceneUpdate は解かずに帰り、DLL のワーカーが解く（docs/ASYNC_UPDATE.md）。
        //   設定は待ち行列で次の着手に効き、結果は 1 フレーム前の入力に対する写しから返る。
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern void AF_SceneSetAsync(IntPtr scene, int enable);
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern int AF_SceneIsAsync(IntPtr scene);
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern void AF_SceneAsyncWait(IntPtr scene);
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern void AF_SceneGetUpdateStats(IntPtr scene, out float outComputeMs, out int outLagFrames,
                                                         out int outSkippedFrames, out int outQueued);
        // この音源の尾を担っている代表音源の index（同じ部屋のいちばん若いもの）。
        //   ★規則はエンジン側に 1 つだけ。ホストで同じ規則を持たないこと。
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern int AF_SceneGetTailShapeIndex(IntPtr scene, int index);

        // ── キャプチャ（サウンドデバッグツール。docs/SOUND_DEBUG_TOOL.md）──
        //   常時録っておいて「いま変だった」を押すと、押した時点の前 preroll と
        //   後 postroll が 1 ファイル（.afcap）に落ちる。破れの判定は録音後（走査）で行う。
        //   ★録るのは結果ではなく**入力**なので、押し直せばそれが再現になる。
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern void AF_SceneCaptureBegin(IntPtr scene, int prerollFrames,
                                                       int postrollFrames, int maxSources,
                                                       int sampleRate, int recordPcm);
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern void AF_SceneCaptureEnd(IntPtr scene);
        // いまカタログに載っている稜線の本数（計器表示用）。
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern int AF_SceneEdgeCatalogCount(IntPtr scene);

        // ── 録った .afcap を開いて調べる ──
        //   ★走査（型紙を当てる）は**DLL の中**で回す。回帰テストが呼ぶのと同じ関数で
        //     なければ「Unity では印が出るのに検査は通る」が起きる。C# へ写経すると必ずずれる。
        [StructLayout(LayoutKind.Sequential)]
        public struct AF_CaptureInfo
        {
            public int frames;
            public int sampleRate;
            public int maxSources;
            public int preroll;
            public int postroll;
            public int workerThreads;
            public int markedFrame;
            public uint dllHash;
            public int boxCount;
            public int meshCount;      // >0 なら C++ の検査には吐けない場面
            public int materialCount;
            public int pcmFrames;
            public int sourceCount;
        }

        // ⚠ 文字列は byte[] で受けて **UTF-8 として自分で読む**こと。
        //   ByValTStr にすると既定の Ansi マーシャリング（このマシンでは CP932）で
        //   復号され、C++ が UTF-8 で書いた「型紙1 跳ばない」が化ける。
        //   → 化けても例外は出ないので、**画面が読めなくなるまで気づけない**類の壊れ方。
        //   読み出しは Native.MarkText() を使う。
        [StructLayout(LayoutKind.Sequential)]
        public struct AF_CaptureMark
        {
            public ulong sourceId;
            public int frame;
            public float seconds;
            public float amount;
            public int reserved;
            [MarshalAs(UnmanagedType.ByValArray, SizeConst = 40)] public byte[] templateName;
            [MarshalAs(UnmanagedType.ByValArray, SizeConst = 24)] public byte[] what;
        }

        /// 固定長 byte 配列から UTF-8 文字列を取り出す（NUL で切る）。
        public static string Utf8(byte[] raw)
        {
            if (raw == null) return "";
            int n = 0;
            while (n < raw.Length && raw[n] != 0) ++n;
            return n > 0 ? System.Text.Encoding.UTF8.GetString(raw, 0, n) : "";
        }

        [DllImport(Dll, CallingConvention = Cc, CharSet = CharSet.Ansi)]
        public static extern IntPtr AF_CaptureOpen(string path);
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern void AF_CaptureClose(IntPtr cap);
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern int AF_CaptureGetInfo(IntPtr cap, out AF_CaptureInfo info);
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern int AF_CaptureGetSceneName(IntPtr cap, byte[] buf, int bufSize);
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern int AF_CaptureGetSourceIds(IntPtr cap, ulong[] outIds, int maxOut);
        // ⚠★ [In, Out] が要る。
        //   AF_CaptureMark は byte[] を持つので **blittable ではない**。blittable でない
        //   構造体の配列は既定で **In だけ**マーシャリングされ、ネイティブ側の書き込みが
        //   managed へ戻ってこない（例外も出ず、全フィールドが 0／文字列が空になる）。
        //   実際にこれで自己診断が「印 1 件・型紙名『』」で落ちた。
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern int AF_CaptureScan(IntPtr cap,
                                                [In, Out] AF_CaptureMark[] outMarks, int maxOut);

        // 連続した同じ印をまとめたもの。★まとめ方は AfCapScan（CLI）と同じ関数。
        //   実機のキャプチャは印 382 件 → まとめると 5 か所。生のままでは読めない。
        [StructLayout(LayoutKind.Sequential)]
        public struct AF_CaptureRun
        {
            public ulong sourceId;
            public int firstFrame;
            public int lastFrame;
            public int count;          // 続いたフレーム数
            public int reserved;
            public float firstSeconds;
            public float lastSeconds;
            public float worst;        // 続いたあいだの最大の破れ量
            public float reserved2;
            [MarshalAs(UnmanagedType.ByValArray, SizeConst = 40)] public byte[] templateName;
            [MarshalAs(UnmanagedType.ByValArray, SizeConst = 24)] public byte[] what;
        }

        [DllImport(Dll, CallingConvention = Cc)]
        public static extern int AF_CaptureScanRuns(IntPtr cap,
                                                    [In, Out] AF_CaptureRun[] outRuns, int maxOut);
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern int AF_CaptureGetPcm(IntPtr cap, float[] outInterleaved, int maxFrames);
        // ⚠ 必ず 0 から順に呼ぶこと。飛ばすと尾が再現しない（段の位相を 0 で戻すため）。
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern int AF_CaptureApplyFrame(IntPtr scene, IntPtr cap, int frameIndex);
        [DllImport(Dll, CallingConvention = Cc, CharSet = CharSet.Ansi)]
        public static extern int AF_CaptureEmitCase(IntPtr cap, int markIndex, string testName,
                                                    byte[] buf, int bufSize);
        // 段の間引きの位相（8要素）。★入力リプレイで**尾を再現する**ために要る。
        //   戻さないとエコグラムのレイを撃つフレームが録音時とずれ、扉が動き続けるかぎり
        //   永久に一致しない（実測 91 フレーム全部不一致 → 位相を戻すと 6 フレームで収束）。
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern void AF_SceneDebugGetStagePhase(IntPtr scene, int[] out8);
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern void AF_SceneDebugSetStagePhase(IntPtr scene, int[] in8);
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern void AF_SceneCaptureMark(IntPtr scene);
        // 0=止まっている 1=録っている 2=前後が揃った（保存できる）
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern int AF_SceneCaptureStatus(IntPtr scene, out int framesHeld);
        // ⚠ オーディオスレッドから呼ぶこと（OnAudioFilterRead）。内部でロックも確保もしない。
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern void AF_SceneCapturePushAudio(IntPtr scene, float[] interleavedStereo,
                                                           int frames);
        [DllImport(Dll, CallingConvention = Cc, CharSet = CharSet.Ansi)]
        public static extern int AF_SceneCaptureWrite(IntPtr scene, string path,
                                                      string sceneName, uint dllHash);

        // ── 尾の共有バス（段階②）──
        //   後期残響の畳み込みを音源で共有する。畳み込みは線形なので、同じ IR なら
        //   「レベルを掛けてから足して 1 回畳む」で厳密に等しい。
        //   実測: 音源 8 本で 1 ブロック 2.345 → 0.404 ms（5.81 倍）。出力の差 -123.7dB 下。
        //   ⚠ 同じ IR の音源だけを同じバスへ（＝部屋ごとに 1 本）。
        //   ⚠ 代表を 1 本差すこと。0 本だと IR が入らず尾が丸ごと鳴らない。
        //   ⚠ Render は AudioListener 側で 1 回（全音源のミックス後に走るので順序が保証される）。
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern IntPtr AF_TailBusCreate(int sampleRate, float tailSeconds,
                                                     int firstBlock, int capBlock, int maxFrames);
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern void AF_TailBusDestroy(IntPtr bus);
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern void AF_TailBusSetCrossfadeMs(IntPtr bus, float ms);
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern void AF_TailBusRender(IntPtr bus, int frames,
                                                   float[] outL, float[] outR);
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern float AF_TailBusRms(IntPtr bus);
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern int AF_TailBusHasIr(IntPtr bus);

        // 点のまわりの空間のうち「どこかの部屋の中」である割合（0..1）。外へ出るときの残響の量はこれで縮める（#5）。
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern float AF_SceneRoomShareTotalAt(IntPtr scene, AFVector3 p, float radius);
        // 点のまわりの部屋の占め方・空間版（外の世界も分母。合計 ≤ 1、残りが屋外）。扉の定点の (1−w) はこちら。
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern int AF_SceneRoomShareAt(IntPtr scene, AFVector3 p, float radius,
                                                     [Out] int[] outRooms, [Out] float[] outWeights, int maxOut);

        // ── 扉の定点（隣の空間の響きを戸口の位置から鳴らす）──
        // 直近に Render したブロックの尾のモノラル（左右の平均）。Render の後、同じオーディオスレッドで読む。
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern int AF_TailBusLastMono(IntPtr bus, [Out] float[] outMono, int frames);
        // ポータルが繋いでいる部屋。toOutside=1 なら片側が外の世界（洞窟の口・屋外へ開く戸口）。
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern int AF_SceneGetPortalRooms(IntPtr scene, int id,
                                                        out int roomA, out int roomB, out int toOutside);
        // 外の世界へ開く口も自動ポータルにする（既定 0。回折の経路生成は使わず、扉の定点だけが使う）。
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern void AF_SceneSetOutsideApertures(IntPtr scene, int on);
        // 隣の空間の拡散した響きがこの口を通る割合（帯域別 6 要素、0..1）。音源に依存しない。
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern int AF_ScenePortalDiffuseCoupling(IntPtr scene, int id, [Out] float[] out6);
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern void AF_VoiceSetTailBus(IntPtr voice, IntPtr bus, int isOwner);

        // ── 方向バス（2026-09-04）: 反射・回折タップをリスナー座標で固定したレーンへ振り、レーンごとに固定の HRIR で畳む ──
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern IntPtr AF_DirectionBusCreate(int sampleRate, int lanes, int maxFrames);
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern void AF_DirectionBusDestroy(IntPtr bus);
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern void AF_DirectionBusSetHrtf(IntPtr bus, IntPtr hrtf, float headCircumferenceCm);
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern int AF_DirectionBusHasHrtf(IntPtr bus);
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern int AF_DirectionBusLanes(IntPtr bus);
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern void AF_DirectionBusRender(IntPtr bus, int frames, [In, Out] float[] outL, [In, Out] float[] outR);
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern float AF_DirectionBusRms(IntPtr bus);
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern void AF_VoiceSetDirectionBus(IntPtr voice, IntPtr bus);
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern void AF_SceneSetAutoPortalMinArea(IntPtr scene, float m2);
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern int AF_SceneGetPortalCounts(IntPtr scene, out int auto_, out int manual);
        // 自動生成された矩形を読み出す（ギズモで確かめる用）。並びは [手置き..., 自動...]。
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern int AF_SceneGetPortal(IntPtr scene, int id, out AFVector3 center,
                                                    out AFVector3 axisU, out AFVector3 axisV,
                                                    out float halfU, out float halfV);
        // ポータルの支配が及ぶ距離(m)。これを超えたら回折は一般の稜線探索へ完全に戻る。
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern void AF_SceneSetPortalGovernRange(IntPtr scene, float meters);

        // 格子が上限に当たって粗くなっていないか。戻り値 1 で降格あり。
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern int AF_SceneRoomGridDegraded(IntPtr scene, out float requested,
                                                          out float actual, out double voxels,
                                                          out double maxVoxels);

        // 部屋を戸口で割る半径(m)。既定 0.6。幅がこの 2 倍に満たないくびれで部屋が分かれる。
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern void AF_SceneSetRoomSeedRadius(IntPtr scene, float meters);

        // ===== 音源レンダリング（段4: DSP の C++ 移行）=====
        // これを使うと IR 畳み込み・HRTF・後期尾が全部エンジン側で回る。
        // 既存の IrConvolver(C#) と並べて A/B できるよう、別系統として足してある。

        [StructLayout(LayoutKind.Sequential)]
        public struct AFVoiceConfig
        {
            public int sampleRate;
            public int maxFrames;
            public float tailSeconds;
            public float tapCrossfadeMs;
            public float hrtfCrossfadeMs;
            public float hrtfCrossoverHz;
            public int tailFirstBlock;
            public int tailCapBlock;
        }

        // index 0 は必ず直接音にすること（AF_VoiceSetDirection の方向で HRTF に通る）。
        // それ以外のタップは hrtfWeight=1 にすると HRTF バスへ載る
        // （方向は AF_VoiceSetDiffractionDirection で別に渡す）。
        // ★並びは C 側の AF_VoiceTap と一致させること。
        [StructLayout(LayoutKind.Sequential)]
        public struct AFVoiceTap
        {
            public int delaySamples;
            public float g0, g1, g2, g3, g4, g5;
            public float panL, panR;
            public float gSpec, gDiff;
            public float hrtfWeight;
            // 到来方向（リスナー座標系）。反射タップの軽量な両耳化（ITD＋帯域別 ILD）に使う。
            //   acoustic_voice.h の AF_VoiceTap と**並びを合わせること**（ヘッダの指示）。
            //   ABI 5 で追加。VoiceConvolver.cs が既に書き込んでいる。
            public float dirX, dirY, dirZ;
        }

        [StructLayout(LayoutKind.Sequential)]
        public struct AFVoiceMetering
        {
            public float rmsDirect, rmsEarly, rmsScatter, rmsTail, rmsOut;
        }

        // C ABI の版。**構造体を変えたら C 側の AF_ABI_VERSION と一緒に上げること。**
        //   DLL だけ古いまま C# を更新すると、AF_VoiceTap の長さが食い違って
        //   マーシャラが別の刻み幅で書き込む（44→48 バイトになった）。例外も出ずに
        //   タップの中身が化けるので、原因に辿り着けない。ここで止める。
        public const int ExpectedAbiVersion = 7;   // 2026-09-05: AF_TapParams / AF_VoiceProgram（タップの組み立てを DLL へ）
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern int AF_AbiVersion();

        [DllImport(Dll, CallingConvention = Cc)]
        public static extern IntPtr AF_HrtfLoadFile(string path);
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern IntPtr AF_HrtfCreateSynthetic(int sampleRate);
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern void AF_HrtfDestroy(IntPtr hrtf);
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern int AF_HrtfDirectionCount(IntPtr hrtf);

        [DllImport(Dll, CallingConvention = Cc)]
        public static extern IntPtr AF_VoiceCreate(ref AFVoiceConfig cfg);
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern void AF_VoiceDestroy(IntPtr voice);
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern void AF_VoiceSetTaps(IntPtr voice, [In] AFVoiceTap[] taps, int count);
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern void AF_VoiceScatterSplit(float delayMs, float mixingTimeMs,
            float scatterAmount, float timeGrowth, out float outSpec, out float outDiff);
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern void AF_VoiceSetHrtf(IntPtr voice, IntPtr hrtf);
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern void AF_VoiceSetHrtfEnabled(IntPtr voice, int enabled);
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern void AF_VoiceSetDirection(IntPtr voice, AFVector3 dir, float headCm);
        // 回折バス（hrtfWeight>0 のタップ）の到来方向。直接音とは別方向を持つ。
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern void AF_VoiceSetDiffractionDirection(IntPtr voice, AFVector3 dir,
                                                                  float headCm);
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern float AF_VoiceRebuildTail(IntPtr voice,
            [In] float[] echoBands, int binCount, float binMs, float startMs, float fadeMs,
            float smoothMs, float smoothGrowth, float envAlpha,
            float directGain, float targetRatio, [In] float[] earBandGain, int earBandGainLen);
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern void AF_VoiceSetOutputGain(IntPtr voice, float gain);
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern void AF_VoiceSetTailLevel(IntPtr voice, float level);
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern void AF_VoiceSetScatterDiffusion(IntPtr voice, float g);
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern void AF_VoiceSetTailEnvelope(IntPtr voice, float wet, float srcLevel);
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern int AF_VoiceTailPartitions(IntPtr voice);
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern int AF_VoiceTailLatency(IntPtr voice);
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern void AF_VoiceRender(IntPtr voice, [In] float[] input, int frames,
            [Out] float[] outL, [Out] float[] outR, ref AFVoiceMetering outMetering);

        // 開口率（比較用。既定は samples=0 で無効）。扉は「ただの壁」として稜線探索に見せ、
        // 開き具合は壁と扉の間にできる開口部の幾何がそのまま表す、というのが本筋。
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern void AF_SceneSetApertureOpen(
            IntPtr scene, float refFrac, float power, float radius, int samples);
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern float AF_SceneMeasureApertureOpenness(
            IntPtr scene, AFVector3 listener, AFVector3 source);

        // 開口率の**幅**を開く指数。形（1−cosθ のクレッシェンド）は物理から出ているが、
        // 量が足りない（実測で扉の全掃引が 1.3dB）。形を保ったまま幅だけを開く演出用。
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern void AF_SceneSetApertureContrast(IntPtr scene, float p);
        // 開口の音色の広がり。1=素通し。音量には効かない（帯域平均を保つ）。
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern void AF_SceneSetApertureTimbre(IntPtr scene, float k);

        // 開口を通る成分を「透過の一部」として扱う。開口を素通りする音は
        // 曲がりも壁抜けもしないので、前川の δ 減衰を払わせない。
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern void AF_SceneSetApertureIsTransmission(IntPtr scene, int on);

        // インスタンスごとに材質を差し替える。壁はコンクリ、扉だけ木、のような使い方。
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern int AF_SceneSetInstanceMaterial(IntPtr scene, int instanceId,
                                                             int materialId);

        // ── ポータル（開口の矩形）──
        // ホストは「ここが戸口」という矩形だけ置く。**開き具合は渡さない**。
        // エンジンが毎フレーム実際の形状から測るので、扉が板で部分的に覆っていることも、
        // 音源が戸口の正面にあるかどうかも、そのまま結果に出る。
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern int AF_SceneAddPortal(IntPtr scene, AFVector3 center,
            AFVector3 axisU, AFVector3 axisV, float halfU, float halfV);
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern void AF_SceneUpdatePortal(IntPtr scene, int id, AFVector3 center,
            AFVector3 axisU, AFVector3 axisV, float halfU, float halfV);
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern int AF_SceneMeasurePortal(IntPtr scene, int id,
            AFVector3 listener, AFVector3 source, [Out] float[] outFrac6, out AFVector3 outPoint);

        // 回折を BTM(有限楔の稜線積分)で出す。0=従来(前川＋開口積分) / 1=BTM。
        // ※BTM はまだ検証途上（扉の掃引が反転する）。既定 OFF のまま使うこと。
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern void AF_SceneSetUseBtm(IntPtr scene, int on);

        // 直接経路の半影を帯域ごとのフレネル半径で作る。1=新（既定）/ 0=従来（0.4 m・8 点・帯域共通）。
        // 新旧を同じビルドで聞き比べるための切り替え。採用が固まったら 0 側ごと消す予定。
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern void AF_SceneSetDirectPenumbra(IntPtr scene, int on);

        // 反射経路トレース：origin→dir を鏡面反射で maxBounces 回追い、通過点を outPoints に書く。
        [DllImport(Dll, CallingConvention = Cc)]
        public static extern int AF_SceneTraceReflectionPath(
            IntPtr scene, AFVector3 origin, AFVector3 dir, float maxDist, int maxBounces,
            [In, Out] AFVector3[] outPoints, int maxPoints);


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

        // ── ABI の照合 ──
        //
        //   配置されている DLL が C# と同じ版か確かめる。違えば false を返し、
        //   呼び出し側は音を鳴らさない。**黙って化けるより止めたほうがよい**。
        //   （A1 で「人手で同期する約束は守られない」を学んだので、機械に照合させる）
        private static int _abiState;   // 0=未確認 / 1=一致 / -1=不一致
        public static bool CheckAbi()
        {
            if (_abiState != 0) return _abiState > 0;
            int got = -1;
            try { got = AF_AbiVersion(); }
            catch (System.Exception)
            {
                // エントリポイントが無い＝AF_AbiVersion より前の DLL。
                _abiState = -1;
                UnityEngine.Debug.LogError(
                    "[AcousticFlow] DLL が古すぎます（AF_AbiVersion がありません）。"
                    + "Unity を閉じて AcousticEngine.dll を差し替えてください。"
                    + "このまま鳴らすと AF_VoiceTap の長さが食い違ってタップが化けます。");
                return false;
            }
            if (got != ExpectedAbiVersion)
            {
                _abiState = -1;
                UnityEngine.Debug.LogError(
                    $"[AcousticFlow] DLL の ABI 版が違います（DLL {got} / C# {ExpectedAbiVersion}）。"
                    + "Unity を閉じて AcousticEngine.dll を差し替えてください。");
                return false;
            }
            _abiState = 1;
            return true;
        }
    }
}
