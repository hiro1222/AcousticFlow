// AcousticScene.cs
// 新アーキ(2026-07) Phase1：インスタンス方式 Scene(AF_Scene* C ABI)の C# ラッパー。
// 旧 AcousticEngine(AddBox/ComputeOcclusion 系)の幾何部分を置き換える。
//   幾何 = インスタンス（geomId + OBB transform + materialId）
//   出力 = 6帯域（1スカラに潰さない）
// 音声(Wwise)は従来どおり AcousticEngine の static 関数を使う（別系統）。
using System;
using UnityEngine;

namespace AcousticFlow
{
    public sealed class AcousticScene : IDisposable
    {
        private IntPtr _handle;

        public bool IsValid => _handle != IntPtr.Zero;

        public AcousticScene()
        {
            _handle = Native.AF_SceneCreate();
        }

        // 帯域別材質を登録し materialId を返す（失敗 -1）。null なら既定壁。
        public int AddMaterial(AcousticMaterial mat)
        {
            if (_handle == IntPtr.Zero) return -1;
            float[] t = mat?.transmission;
            float[] a = mat?.absorption;
            float[] s = mat?.scattering;
            int n = (t != null) ? AcousticEngine.NumBands : 0;
            return Native.AF_SceneAddMaterial(_handle, t, a, s, n);
        }

        // ── 調整支援 ──────────────────────────────────────────────────
        /// この直線の透過損失を担っている遮蔽物を、大きい順に返す（件数を返す）。
        ///   ⚠ **透過だけ**を見ます。回り込み（回折）が担っている帯域では、
        ///     ここに出た材質を触っても音は動きません。触る前に内訳を確かめること。
        public int TransmissionCarriers(Vector3 listener, Vector3 source,
                                        int[] instance, int[] material, float[] lossDb)
        {
            if (_handle == IntPtr.Zero || instance == null) return 0;
            try
            {
                return Native.AF_SceneTransmissionCarriers(
                    _handle, new AFVector3(listener), new AFVector3(source),
                    instance, material, lossDb, instance.Length);
            }
            catch (System.Exception) { return 0; }
        }

        /// 材質の現在値を読む（書けた帯域数）。
        public int GetMaterial(int materialId, float[] transmission,
                               float[] absorption = null, float[] scattering = null)
        {
            if (_handle == IntPtr.Zero || transmission == null) return 0;
            try
            {
                return Native.AF_SceneGetMaterial(_handle, materialId, transmission,
                                                  absorption, scattering, transmission.Length);
            }
            catch (System.Exception) { return 0; }
        }

        /// 材質を書き換える。**その材質を使っている実体が一斉に変わります。**
        ///   次のフレームから効きます（BVH は形状だけなので再構築も起きない）。
        public bool SetMaterial(int materialId, float[] transmission,
                                float[] absorption = null, float[] scattering = null)
        {
            if (_handle == IntPtr.Zero || transmission == null) return false;
            try
            {
                return Native.AF_SceneSetMaterial(_handle, materialId, transmission,
                                                  absorption, scattering,
                                                  transmission.Length) != 0;
            }
            catch (System.Exception) { return false; }
        }

        // OBB インスタンスを追加し instanceId を返す（失敗 -1）。
        //   right/up は transform.right / transform.up をそのまま渡せる（内部で正規直交化）。
        public int AddInstanceBox(Vector3 center, Vector3 halfExtents,
                                  Vector3 right, Vector3 up, int materialId)
        {
            if (_handle == IntPtr.Zero) return -1;
            return Native.AF_SceneAddInstanceBox(
                _handle, new AFVector3(center), new AFVector3(halfExtents),
                new AFVector3(right), new AFVector3(up), materialId);
        }

        // ソフト遮蔽。直接経路の透過(振幅・6帯域)と遮蔽割合(0..1)を返す。
        //   単一レイの透過判定と違い、掠める位置でも連続に動く。
        //   これで「見通せているか」の二値分岐なしに直接音と回折を配分できる。
        public bool ComputeSoftOcclusion(Vector3 listener, Vector3 source,
                                         float[] outTrans, out float occFrac)
        {
            occFrac = 0f;
            if (_handle == IntPtr.Zero || outTrans == null) return false;
            return Native.AF_SceneComputeSoftOcclusion(
                _handle, new AFVector3(listener), new AFVector3(source),
                outTrans, outTrans.Length, out occFrac) > 0;
        }

        // 各方向から「どれだけ残響が返るか」を帯域別に得る。
        //   outEnergy[i*6 + b] = 方向 dirs[i] の帯域 b。方向間の相対分布として使う。
        //   何にも当たらない方向は 0（開けている＝残響を返さない）。
        private AFVector3[] _probeDirBuf;   // interop 用の変換バッファ（毎フレーム確保しない）

        public void ProbeDirectionalEnergy(Vector3 origin, Vector3[] dirs, int dirCount,
                                           int maxBounces, float[] outEnergy)
        {
            if (_handle == IntPtr.Zero || dirs == null || outEnergy == null) return;
            if (dirCount > dirs.Length) dirCount = dirs.Length;
            if (dirCount <= 0) return;
            if (_probeDirBuf == null || _probeDirBuf.Length < dirCount)
                _probeDirBuf = new AFVector3[dirCount];
            for (int i = 0; i < dirCount; i++) _probeDirBuf[i] = new AFVector3(dirs[i]);
            Native.AF_SceneProbeDirectionalEnergy(_handle, new AFVector3(origin), _probeDirBuf,
                                                  dirCount, maxBounces, outEnergy);
        }

        // 三角形メッシュを形状(BLAS)として登録し geomId を返す（失敗 -1）。
        //   形状はローカルAABBが単位箱になるよう正規化して保持されるので、
        //   インスタンスの OBB がそのまま local→world の変換になる。
        //   その OBB を組むためのローカルAABBを out で受け取る（MeshInstanceObb を使うと楽）。
        public int AddMesh(Mesh mesh, out Vector3 localCenter, out Vector3 localHalfExtents)
        {
            localCenter = Vector3.zero;
            localHalfExtents = Vector3.one;
            if (_handle == IntPtr.Zero || mesh == null) return -1;

            var verts = mesh.vertices;
            var tris = mesh.triangles;
            if (verts == null || tris == null || verts.Length == 0 || tris.Length < 3) return -1;

            var flat = new float[verts.Length * 3];
            for (int i = 0; i < verts.Length; i++)
            {
                flat[i * 3] = verts[i].x;
                flat[i * 3 + 1] = verts[i].y;
                flat[i * 3 + 2] = verts[i].z;
            }
            int id = Native.AF_SceneAddMesh(_handle, flat, verts.Length, tris, tris.Length,
                                            out var c, out var h);
            if (id < 0) return -1;
            localCenter = c.ToVector3();
            localHalfExtents = h.ToVector3();
            return id;
        }

        public void RemoveMesh(int geomId)
        {
            if (_handle != IntPtr.Zero) Native.AF_SceneRemoveMesh(_handle, geomId);
        }

        // メッシュ形状のインスタンスを追加し instanceId を返す（失敗 -1）。
        public int AddInstanceMesh(int geomId, Vector3 center, Vector3 halfExtents,
                                   Vector3 right, Vector3 up, int materialId)
        {
            if (_handle == IntPtr.Zero) return -1;
            return Native.AF_SceneAddInstanceMesh(
                _handle, geomId, new AFVector3(center), new AFVector3(halfExtents),
                new AFVector3(right), new AFVector3(up), materialId);
        }

        // ローカルAABB(AddMesh の戻り) と Transform から、インスタンスの OBB を組む。
        //   正規化ローカル [-1,1]^3 → ワールド の写像がこれで一意に決まる。
        //   非一様スケールを許す（部屋の内寸変更＝壁を非一様に伸ばす、が要件のため）。
        public static void MeshInstanceObb(Transform t, Vector3 localCenter, Vector3 localHalfExtents,
                                           out Vector3 center, out Vector3 halfExtents,
                                           out Vector3 right, out Vector3 up)
        {
            center = t.TransformPoint(localCenter);
            Vector3 s = t.lossyScale;
            halfExtents = new Vector3(localHalfExtents.x * Mathf.Abs(s.x),
                                      localHalfExtents.y * Mathf.Abs(s.y),
                                      localHalfExtents.z * Mathf.Abs(s.z));
            right = t.right;
            up = t.up;
        }

        // 既存インスタンスの transform を更新（動いた分だけ）。
        public void UpdateInstance(int instanceId, Vector3 center, Vector3 halfExtents,
                                   Vector3 right, Vector3 up)
        {
            if (_handle == IntPtr.Zero) return;
            Native.AF_SceneUpdateInstance(
                _handle, instanceId, new AFVector3(center), new AFVector3(halfExtents),
                new AFVector3(right), new AFVector3(up));
        }

        public void SetInstanceActive(int instanceId, bool active)
        {
            if (_handle == IntPtr.Zero) return;
            Native.AF_SceneSetInstanceActive(_handle, instanceId, active ? 1 : 0);
        }

        public void ClearInstances()
        {
            if (_handle != IntPtr.Zero) Native.AF_SceneClearInstances(_handle);
        }

        // from→to の帯域別透過ゲイン(0..1)を outGains(6要素以上)に書く。true=成功。
        public bool ComputeTransmissionBands(Vector3 from, Vector3 to, float[] outGains)
        {
            if (_handle == IntPtr.Zero || outGains == null) return false;
            return Native.AF_SceneComputeTransmissionBands(
                _handle, new AFVector3(from), new AFVector3(to), outGains, outGains.Length) > 0;
        }

        public bool IsOccluded(Vector3 from, Vector3 to)
        {
            if (_handle == IntPtr.Zero) return false;
            return Native.AF_SceneIsOccluded(_handle, new AFVector3(from), new AFVector3(to)) != 0;
        }

        public float Raycast(Vector3 origin, Vector3 dir, float maxDist)
        {
            if (_handle == IntPtr.Zero) return -1f;
            return Native.AF_SceneRaycast(_handle, new AFVector3(origin), new AFVector3(dir), maxDist);
        }

        // from→to の帯域別回折ゲイン(0..1)を outGains(6要素以上)に書く。true=成功。
        // 遮蔽なし→全1.0 / 稜線を回る迂回路あり→Maekawa(低域ほど大) / 迂回路なし→全0。
        public bool ComputeDiffractionBands(Vector3 from, Vector3 to, float[] outGains)
        {
            if (_handle == IntPtr.Zero || outGains == null) return false;
            return Native.AF_SceneComputeDiffractionBands(
                _handle, new AFVector3(from), new AFVector3(to), outGains, outGains.Length) > 0;
        }

        // 回折の経路：迂回の余剰長δ(m)を返し、迂回点(source→P→listener の P)を out で受ける。
        // 遮蔽なし/迂回路なしは -1（midPoint 不定）。回折経路の描画に使う。
        public float DiffractionPath(Vector3 from, Vector3 to, out Vector3 midPoint)
        {
            midPoint = Vector3.zero;
            if (_handle == IntPtr.Zero) return -1f;
            float d = Native.AF_SceneDiffractionPath(
                _handle, new AFVector3(from), new AFVector3(to), out AFVector3 m);
            if (d >= 0f) midPoint = new Vector3(m.x, m.y, m.z);
            return d;
        }

        // 役割2：反射込み遮蔽量(0..1)を返す。壁で反射して回り込む成分を含むので、壁裏でも
        // 1.0 に張り付かない。outBands6(6要素以上, null可) に 直接⊕回折⊕反射 の帯域別生存を書く。
        public float OcclusionReflected(Vector3 source, Vector3 listener,
                                        float[] outBands6, int numRays, int maxBounces)
        {
            if (_handle == IntPtr.Zero) return 0f;
            return Native.AF_SceneOcclusionReflected(
                _handle, new AFVector3(source), new AFVector3(listener),
                outBands6, numRays, maxBounces);
        }

        // 役割2・複数音源：リスナーレイ1回で全音源へ next-event。outOcc[count] に音源ごとの
        // 遮蔽量、outBands[count*6]（null 可）に帯域別生存を書く。raycast は音源数非依存。
        // outDir[count*3] に「エネルギーが届く支配方向」(単位ベクトル)を書く（null 可）。
        private AFVector3[] _srcBuf;
        public void OcclusionReflectedMulti(Vector3 listener, Vector3[] sources, int count,
                                            float[] outOcc, float[] outBands, float[] outDir,
                                            float directWeight, int numRays, int maxBounces)
        {
            if (_handle == IntPtr.Zero || sources == null || count <= 0) return;
            if (_srcBuf == null || _srcBuf.Length < count) _srcBuf = new AFVector3[count];
            for (int i = 0; i < count; i++) _srcBuf[i] = new AFVector3(sources[i]);
            Native.AF_SceneOcclusionReflectedMulti(
                _handle, new AFVector3(listener), _srcBuf, count, outOcc, outBands, outDir,
                directWeight, numRays, maxBounces);
        }

        // 残響：到達時間ビンのエコグラムを outBins に書く（RT60/wet 算出用）。
        public void ComputeEchogram(Vector3 listener, Vector3[] sources, int count,
                                    float[] outBins, int numBins, float binSeconds,
                                    float speedOfSound, int numRays, int maxBounces)
        {
            if (_handle == IntPtr.Zero || sources == null || count <= 0 || outBins == null) return;
            if (_srcBuf == null || _srcBuf.Length < count) _srcBuf = new AFVector3[count];
            for (int i = 0; i < count; i++) _srcBuf[i] = new AFVector3(sources[i]);
            Native.AF_SceneComputeEchogram(
                _handle, new AFVector3(listener), _srcBuf, count, outBins, numBins,
                binSeconds, speedOfSound, numRays, maxBounces);
        }

        // 残響：帯域別エコグラムを outBins[numBins*6] に書く。高域ほど速く減衰する実測カーブが取れる。
        // 古いDLLではエクスポートが無いので、その場合 false を返す（呼び手は広帯域版へフォールバック）。
        private bool _echogramBandsMissing;
        public bool ComputeEchogramBands(Vector3 listener, Vector3[] sources, int count,
                                         float[] outBins, int numBins, float binSeconds,
                                         float speedOfSound, int numRays, int maxBounces,
                                         float distanceRef)
        {
            if (_handle == IntPtr.Zero || sources == null || count <= 0 || outBins == null) return false;
            if (_echogramBandsMissing) return false;
            if (_srcBuf == null || _srcBuf.Length < count) _srcBuf = new AFVector3[count];
            for (int i = 0; i < count; i++) _srcBuf[i] = new AFVector3(sources[i]);
            try
            {
                Native.AF_SceneComputeEchogramBands(
                    _handle, new AFVector3(listener), _srcBuf, count, outBins, numBins,
                    binSeconds, speedOfSound, numRays, maxBounces, distanceRef);
                return true;
            }
            catch (EntryPointNotFoundException)
            {
                _echogramBandsMissing = true;   // 以後は問い合わせない
                return false;
            }
        }

        // ── リスナー / 音源の保持（API移行 段1）──
        // これまで listener/source はクエリのたびに引数で渡していたが、SPEC §2 では
        // エンジンが保持して内部で音源ループを回す。段1では保持するだけ（挙動は不変）。
        // 古いDLLにはエクスポートが無いので、初回の EntryPointNotFound で以後スキップする。
        private bool _sourceRegistryMissing;

        public bool SourceRegistryAvailable => !_sourceRegistryMissing;

        public void SetListener(Vector3 pos)
        {
            if (_handle == IntPtr.Zero || _sourceRegistryMissing) return;
            try { Native.AF_SceneSetListener(_handle, new AFVector3(pos)); }
            catch (EntryPointNotFoundException) { _sourceRegistryMissing = true; }
        }

        public void SetSource(ulong id, Vector3 pos)
        {
            if (_handle == IntPtr.Zero || _sourceRegistryMissing) return;
            try { Native.AF_SceneSetSource(_handle, id, new AFVector3(pos)); }
            catch (EntryPointNotFoundException) { _sourceRegistryMissing = true; }
        }

        public void RemoveSource(ulong id)
        {
            if (_handle == IntPtr.Zero || _sourceRegistryMissing) return;
            try { Native.AF_SceneRemoveSource(_handle, id); }
            catch (EntryPointNotFoundException) { _sourceRegistryMissing = true; }
        }

        public void ClearSources()
        {
            if (_handle == IntPtr.Zero || _sourceRegistryMissing) return;
            try { Native.AF_SceneClearSources(_handle); }
            catch (EntryPointNotFoundException) { _sourceRegistryMissing = true; }
        }

        public int SourceCount
        {
            get
            {
                if (_handle == IntPtr.Zero || _sourceRegistryMissing) return 0;
                try { return Native.AF_SceneSourceCount(_handle); }
                catch (EntryPointNotFoundException) { _sourceRegistryMissing = true; return 0; }
            }
        }

        // ── バッチ更新（API移行 段2）──
        // 毎フレーム Update を1回呼び、結果を Get* で読む。「どの計算をいつ走らせるか」は
        // エンジンが内部レートで管理するので、ホストはカウンタを持たない。
        // 古いDLL対策は SourceRegistry と同じフラグに相乗り（同じ版で入った API のため）。

        public void SetUpdateConfig(AcousticUpdateConfig cfg)
        {
            if (_handle == IntPtr.Zero || _sourceRegistryMissing) return;
            try { Native.AF_SceneSetUpdateConfig(_handle, ref cfg); }
            catch (EntryPointNotFoundException) { _sourceRegistryMissing = true; }
        }

        public void Update(float dt)
        {
            if (_handle == IntPtr.Zero || _sourceRegistryMissing) return;
            try { Native.AF_SceneUpdate(_handle, dt); }
            catch (EntryPointNotFoundException) { _sourceRegistryMissing = true; }
        }

        public int SourceIndex(ulong id)
        {
            if (_handle == IntPtr.Zero || _sourceRegistryMissing) return -1;
            try { return Native.AF_SceneSourceIndex(_handle, id); }
            catch (EntryPointNotFoundException) { _sourceRegistryMissing = true; return -1; }
        }

        // 帯域別生存（透過⊕回折⊕反射）。out6 は6要素以上。
        public void GetSourceOcclusion(int index, float[] out6)
        {
            if (_handle == IntPtr.Zero || _sourceRegistryMissing || out6 == null) return;
            Native.AF_SceneGetSourceOcclusion(_handle, index, out6);
        }

        public float GetSourceOcclusionScalar(int index)
        {
            if (_handle == IntPtr.Zero || _sourceRegistryMissing) return 0f;
            return Native.AF_SceneGetSourceOcclusionScalar(_handle, index);
        }

        // エネルギーが届く支配方向（単位ベクトル）。
        public Vector3 GetSourceArrivalDir(int index)
        {
            if (_handle == IntPtr.Zero || _sourceRegistryMissing) return Vector3.zero;
            if (_dir3 == null) _dir3 = new float[3];
            Native.AF_SceneGetSourceArrivalDir(_handle, index, _dir3);
            return new Vector3(_dir3[0], _dir3[1], _dir3[2]);
        }
        private float[] _dir3;

        // 早期反射タップ。像源位置と6帯域ゲインを受け、本数を返す。
        public int GetEarlyReflections(int index, Vector3[] outPos, float[] outGain6)
        {
            if (_handle == IntPtr.Zero || _sourceRegistryMissing || outPos == null || outGain6 == null) return 0;
            int cap = outPos.Length;
            if (cap <= 0) return 0;
            if (_erBuf == null || _erBuf.Length < cap) _erBuf = new AFVector3[cap];
            int n = Native.AF_SceneGetEarlyReflections(_handle, index, _erBuf, outGain6, cap);
            for (int i = 0; i < n; i++) outPos[i] = new Vector3(_erBuf[i].x, _erBuf[i].y, _erBuf[i].z);
            return n;
        }

        // 焼く層（第 1 段）: 静的な面のリストとセルごとの見通しを焼く。戻り値は焼いたセル数。古い DLL なら 0。
        public int BakeStaticFaces(float cellSize, int subTaps)
        {
            if (_handle == IntPtr.Zero) return 0;
            try { return Native.AF_SceneBakeStaticFaces(_handle, cellSize, subTaps); }
            catch (EntryPointNotFoundException) { return 0; }
        }
        public void ClearFaceBake()
        {
            if (_handle == IntPtr.Zero) return;
            try { Native.AF_SceneClearFaceBake(_handle); } catch (EntryPointNotFoundException) { }
        }
        public int FaceBakeFaceCount()
        {
            if (_handle == IntPtr.Zero) return 0;
            try { return Native.AF_SceneFaceBakeFaceCount(_handle); } catch (EntryPointNotFoundException) { return 0; }
        }
        // 音響的に動く物（扉など）のタグ。焼く層に入れず、実行時の遮蔽として扱う。古い DLL なら無視。
        public void SetInstanceDynamic(int instanceId, bool dynamic)
        {
            if (_handle == IntPtr.Zero || instanceId < 0) return;
            try { Native.AF_SceneSetInstanceDynamic(_handle, instanceId, dynamic ? 1 : 0); }
            catch (EntryPointNotFoundException) { }
        }

        // 面の焼きをバイト列で出し入れする（ファイル保存／別の作り手の結果）。古い DLL なら null / false。
        public byte[] FaceBakeExport()
        {
            if (_handle == IntPtr.Zero) return null;
            try
            {
                int n = Native.AF_SceneFaceBakeBytes(_handle);
                if (n <= 0) return null;
                var buf = new byte[n];
                return Native.AF_SceneFaceBakeExport(_handle, buf, n) == n ? buf : null;
            }
            catch (EntryPointNotFoundException) { return null; }
        }
        public bool FaceBakeImport(byte[] data)
        {
            if (_handle == IntPtr.Zero || data == null || data.Length == 0) return false;
            try { return Native.AF_SceneFaceBakeImport(_handle, data, data.Length) == 1; }
            catch (EntryPointNotFoundException) { return false; }
        }
        // BVH の書き出し（GPU の走査器へ渡す用）。戻り値は節の数。配列は呼び手が確保する。
        public int BvhNodeCount() { if (_handle == IntPtr.Zero) return 0; try { return Native.AF_SceneBvhNodeCount(_handle); } catch (EntryPointNotFoundException) { return 0; } }
        public int BvhOrderCount() { if (_handle == IntPtr.Zero) return 0; try { return Native.AF_SceneBvhOrderCount(_handle); } catch (EntryPointNotFoundException) { return 0; } }
        public int ExportBvh(AFBvhNode[] nodes, int[] order)
        {
            if (_handle == IntPtr.Zero || nodes == null || order == null) return 0;
            try { return Native.AF_SceneExportBvh(_handle, nodes, nodes.Length, order, order.Length); }
            catch (EntryPointNotFoundException) { return 0; }
        }
        public bool GetInstanceDesc(int instanceId, out AFInstanceDesc desc)
        {
            desc = default;
            if (_handle == IntPtr.Zero) return false;
            try { return Native.AF_SceneGetInstance(_handle, instanceId, out desc) == 1; }
            catch (EntryPointNotFoundException) { return false; }
        }

        // 回折二次音源。位置とゲインを受け、本数を返す。
        public int GetDiffractionSources(int index, Vector3[] outPos, float[] outGain)
        {
            if (_handle == IntPtr.Zero || _sourceRegistryMissing || outPos == null || outGain == null) return 0;
            int cap = Mathf.Min(outPos.Length, outGain.Length);
            if (cap <= 0) return 0;
            if (_diffSrcBuf == null || _diffSrcBuf.Length < cap) _diffSrcBuf = new AFVector3[cap];
            int n = Native.AF_SceneGetDiffractionSources(_handle, index, _diffSrcBuf, outGain, cap);
            for (int i = 0; i < n; i++) outPos[i] = new Vector3(_diffSrcBuf[i].x, _diffSrcBuf[i].y, _diffSrcBuf[i].z);
            return n;
        }

        /// <summary>
        /// 開口率ゲートの設定（比較用。既定は samples=0 で無効）。
        ///
        /// 扉は「ただの壁」として稜線探索に見せ、開き具合は壁と扉の間にできる開口部の
        /// 幾何がそのまま表す、というのが本筋。別指標として割合を持つと二重計上になる。
        /// 開口断面へ点を撒いて「見える割合」を測る方式を比較用に残してある。
        /// </summary>
        public void SetApertureOpen(float refFrac, float power, float radius, int samples)
        {
            if (_handle != IntPtr.Zero)
                Native.AF_SceneSetApertureOpen(_handle, refFrac, power, radius, samples);
        }

        /// <summary>
        /// インスタンスごとに材質を差し替える。
        /// 現実の部屋で音が漏れるのは壁ではなく**扉**なので、
        /// 壁はコンクリ・扉は木、のように分けられることが要る。
        /// </summary>
        public bool SetInstanceMaterial(int instanceId, int materialId)
        {
            if (_handle == IntPtr.Zero) return false;
            return Native.AF_SceneSetInstanceMaterial(_handle, instanceId, materialId) != 0;
        }

        /// <summary>
        /// ポータル（開口の矩形）を登録して id を返す。
        /// **開き具合は渡さない** ── エンジンが毎フレーム実形状から測る。
        /// ホストは「ここが戸口」というトポロジだけ持つ、という役割分担。
        /// </summary>
        public int AddPortal(Vector3 center, Vector3 axisU, Vector3 axisV, float halfU, float halfV)
        {
            if (_handle == IntPtr.Zero) return -1;
            return Native.AF_SceneAddPortal(_handle, new AFVector3(center),
                new AFVector3(axisU), new AFVector3(axisV), halfU, halfV);
        }

        /// <summary>ポータルの配置を更新する（扉ごと動く戸口など）。</summary>
        public void UpdatePortal(int id, Vector3 center, Vector3 axisU, Vector3 axisV,
                                 float halfU, float halfV)
        {
            if (_handle != IntPtr.Zero)
                Native.AF_SceneUpdatePortal(_handle, id, new AFVector3(center),
                    new AFVector3(axisU), new AFVector3(axisV), halfU, halfV);
        }

        /// <summary>
        /// ポータルがどれだけ開いているかを帯域別に測る（診断・可視化用）。
        /// outPoint は開いている部分の重み付き重心＝定位に使う点。
        /// </summary>
        public bool MeasurePortal(int id, Vector3 listener, Vector3 source,
                                  float[] outFrac6, out Vector3 outPoint)
        {
            outPoint = Vector3.zero;
            if (_handle == IntPtr.Zero || outFrac6 == null) return false;
            bool ok = Native.AF_SceneMeasurePortal(_handle, id, new AFVector3(listener),
                new AFVector3(source), outFrac6, out AFVector3 p) != 0;
            outPoint = p.ToVector3();
            return ok;
        }

        /// <summary>
        /// 開口を通る成分を「透過の一部」として扱う（合成透過率 τ=τ壁(1−f)+f の f の項）。
        /// 開口をまっすぐ通る音は曲がりも壁抜けもしないので、前川の δ 減衰を払わせない。
        /// 実測: 音源が戸口に正対する配置で 開−閉 が 3.0dB → 22.3dB。
        /// </summary>
        public void SetApertureIsTransmission(bool on)
        {
            if (_handle != IntPtr.Zero) Native.AF_SceneSetApertureIsTransmission(_handle, on ? 1 : 0);
        }

        /// <summary>
        /// 開口率の**幅**を開く指数（コントラスト）。1.0=素通し。大きいほど開閉の差が開く。
        ///
        /// 開口率の形（1−cosθ のクレッシェンド、高域ほど大きく開く）は物理から出ているが、
        /// 量が足りない（実測で扉の全掃引が 1.3dB。現実の合成透過率は 125Hz で 11dB）。
        /// 形を保ったまま幅だけを開くための演出用。実測は参照であって目標ではない、の適用先。
        /// 開口という一般の量への写像なので、扉を特別視しない。
        /// </summary>
        public void SetApertureContrast(float p)
        {
            if (_handle != IntPtr.Zero) Native.AF_SceneSetApertureContrast(_handle, p);
        }

        // 開口の**音色**の広がり。1=素通し。音量には効かない（帯域平均を保つ）。
        //   どれくらい芝居がかって聞こえるかは作品側の判断なので、外に出しておく。
        public void SetApertureTimbre(float k)
        {
            if (_handle == IntPtr.Zero) return;
            try { Native.AF_SceneSetApertureTimbre(_handle, k); }
            catch (System.Exception) { }   // 古い DLL。ABI 照合の側で警告が出る
        }

        /// <summary>
        /// 回折を BTM（有限楔の稜線積分）で出す。
        /// ※検証途上。BTM 単体は 7 件中 6 件の性質チェックを通っているが、
        ///   シーンへ繋ぐと扉の掃引が反転する（渡している稜線集合の問題）。既定 OFF。
        /// </summary>
        public void SetUseBtm(bool on)
        {
            if (_handle != IntPtr.Zero) Native.AF_SceneSetUseBtm(_handle, on ? 1 : 0);
        }

        /// <summary>開口率(0..1)をそのまま測る（診断用）。</summary>
        public float MeasureApertureOpenness(Vector3 listener, Vector3 source)
        {
            if (_handle == IntPtr.Zero) return 0f;
            return Native.AF_SceneMeasureApertureOpenness(
                _handle, new AFVector3(listener), new AFVector3(source));
        }

        // 帯域別エコグラム（outBins は numBins*6 要素）。書けたビン数を返す。
        //   index : 音源のエンジン内 index（SourceIndex で引いたもの）。
        //           -1 で全音源の和＝部屋全体の響き。
        //   ★音源ごとに持つ。別の部屋にいる音源は届く量も減衰の形も違う
        //     （実測: 量で 10.0dB、形で 500ms 時点 9.0dB）。以前は 1 本に潰していたので、
        //     響く部屋の音源も吸う部屋の音源も同じ尾で鳴っていた。
        public int GetEchogramBands(int index, float[] outBins, int numBins)
        {
            if (_handle == IntPtr.Zero || _sourceRegistryMissing || outBins == null) return 0;
            return Native.AF_SceneGetEchogramBands(_handle, index, outBins, numBins);
        }

        // ===== 部屋と開口（幾何から自動検出）=====
        // 静的な形状をボクセル化して自由空間を塗り分け、狭いくびれ（戸口）で部屋を分ける。
        // 手置きのボリュームは無く、壁を壊せば部屋の繋がりもその場で変わる。
        //
        // ★部屋を音に使うときは必ず RoomVolumeAt / Rt60At / RoomWeights を通すこと。
        //   RoomAt（部屋番号）で残響を切り替えると、プレイヤーが必ず通る戸口のど真ん中に
        //   不連続を置くことになる。このエンジンで潰してきた音の跳ねは、どれも
        //   「二値の判定が音に直結していた」ことが原因だった。
        //   割合なら部屋の真ん中で 100:0、戸口で 50:50 と連続に変わる。

        public int RoomCount
        {
            get { return _handle == IntPtr.Zero ? 0 : Native.AF_SceneRoomCount(_handle); }
        }

        // 点がどの部屋か。-1 は部屋の外／実体の中。★表示・デバッグ用。音の切り替えには使わない。
        public int RoomAt(Vector3 p)
        {
            if (_handle == IntPtr.Zero) return -1;
            return Native.AF_SceneRoomAt(_handle, new AFVector3(p));
        }

        public bool GetRoomInfo(int room, out float volume, out Vector3 centroid,
                                out Vector3 boundsMin, out Vector3 boundsMax)
        {
            volume = 0f; centroid = Vector3.zero; boundsMin = Vector3.zero; boundsMax = Vector3.zero;
            if (_handle == IntPtr.Zero) return false;
            AFVector3 c, lo, hi;
            if (Native.AF_SceneRoomInfo(_handle, room, out volume, out c, out lo, out hi) == 0)
                return false;
            centroid = c.ToVector3(); boundsMin = lo.ToVector3(); boundsMax = hi.ToVector3();
            return true;
        }

        // 半径 radius(m) の球の中で各部屋が占める割合（合計 1、大きい順）。書けた数を返す。
        private int[] _roomIdBuf;
        private float[] _roomWBuf;
        public int GetRoomWeights(Vector3 p, float radius, int[] outRooms, float[] outWeights)
        {
            if (_handle == IntPtr.Zero || outRooms == null || outWeights == null) return 0;
            int cap = Mathf.Min(outRooms.Length, outWeights.Length);
            if (cap <= 0) return 0;
            return Native.AF_SceneRoomWeights(_handle, new AFVector3(p), radius,
                                              outRooms, outWeights, cap);
        }
        /// 空間版。外の世界も分母に入るので合計は 1 以下（残りが屋外）。
        ///   屋外との境目で連続に混ぜたい量はこちら（GetRoomWeights は外へ開く戸口で 0→1 と跳ぶ）。
        /// 点のまわりの空間のうち「どこかの部屋の中」である割合（0..1）。外へ出るときの残響の量はこれで縮める。
        ///   V と RT60 を空間版で混ぜても量は連続にならない（両方が同じ割合で縮み rc が動かない）。量だけをこれで付ける。
        public float RoomShareTotalAt(Vector3 p, float radius)
        {
            if (_handle == IntPtr.Zero) return 0f;
            try { return Native.AF_SceneRoomShareTotalAt(_handle, new AFVector3(p), radius); }
            catch (System.Exception) { return 1f; }   // 古い DLL: 縮めない（従来どおり）
        }

        /// 部屋が 1 つも取れていないのに囲われた形があるとき、一度だけ警告する（種の半径が戸口の半幅以下だと部屋が外へ漏れる）。
        public void CheckRoomsFound(int instanceCountHint, float seedRadius)
        {
            if (_handle == IntPtr.Zero || _roomsWarned) return;
            if (instanceCountHint < 6) return;   // 床・天井・壁 4 枚が無ければ部屋は出ない
            int n;
            try { n = Native.AF_SceneRoomCount(_handle); }
            catch (System.Exception) { return; }
            if (n > 0) return;
            _roomsWarned = true;
            UnityEngine.Debug.LogWarning(
                $"[AcousticScene] 部屋が 1 つも取れていません（実体 {instanceCountHint} 個、種の半径 {seedRadius:0.##} m）。\n"
                + "  種の半径が戸口の半幅以下だと、部屋が戸口から外へ漏れて 0 個になります（0.6 m は戸口 1.0 m まで、0.8 m は 1.4 m まで）。\n"
                + "  roomSeedRadius を「いちばん広い戸口の半幅 ＋ 1 セル」より大きくしてください（部屋の狭い所の半分より小さく）。");
        }
        private bool _roomsWarned;

        public int GetRoomShare(Vector3 p, float radius, int[] outRooms, float[] outWeights)
        {
            if (_handle == IntPtr.Zero || outRooms == null || outWeights == null) return 0;
            int cap = Mathf.Min(outRooms.Length, outWeights.Length);
            if (cap <= 0) return 0;
            try
            {
                return Native.AF_SceneRoomShareAt(_handle, new AFVector3(p), radius,
                                                  outRooms, outWeights, cap);
            }
            catch (System.EntryPointNotFoundException)
            {
                // 古い DLL（Unity が掴んだまま差し替えられなかった時）。部屋どうしの割合で代用する。
                //   屋外との境目で 0→1 と跳ぶが、鳴らないよりはよい。Unity を開き直せば新しい口が使われる。
                return Native.AF_SceneRoomWeights(_handle, new AFVector3(p), radius, outRooms, outWeights, cap);
            }
            catch (System.Exception) { return 0; }
        }

        // 上の割合で混ぜた実効体積(m3)。0 なら部屋の外。
        // 臨界距離 rc = 0.057√(V/RT60) にそのまま入れられる。
        public float RoomVolumeAt(Vector3 p, float radius)
        {
            if (_handle == IntPtr.Zero) return 0f;
            return Native.AF_SceneRoomVolumeAt(_handle, new AFVector3(p), radius);
        }

        // 帯域別の残響時間(s)。部屋ごとの Sabine 値を上の割合で混ぜたもの。
        // outRt60 は 6 要素以上。書けた帯域数を返す。
        public int GetRt60At(Vector3 p, float radius, float[] outRt60)
        {
            if (_handle == IntPtr.Zero || outRt60 == null) return 0;
            return Native.AF_SceneRt60At(_handle, new AFVector3(p), radius,
                                         outRt60, outRt60.Length);
        }

        // 部屋の音響量。境界面積・開口面積・帯域別の平均吸音率と残響時間。
        public bool GetRoomAcoustics(int room, out float surface, out float openArea,
                                     float[] absorb6, float[] rt60_6)
        {
            surface = 0f; openArea = 0f;
            if (_handle == IntPtr.Zero) return false;
            // C 側は 6 帯域ぶん書くので、短い配列を渡させない（渡されたら書かせない）。
            if (absorb6 != null && absorb6.Length < 6) absorb6 = null;
            if (rt60_6 != null && rt60_6.Length < 6) rt60_6 = null;
            return Native.AF_SceneRoomAcoustics(_handle, room, out surface, out openArea,
                                                absorb6, rt60_6) != 0;
        }

        public int ApertureCount
        {
            get { return _handle == IntPtr.Zero ? 0 : Native.AF_SceneApertureCount(_handle); }
        }

        // 開口 index の情報。★ここに出るのは戸口（開口の器）であって、扉の開き具合ではない。
        //   扉は動くものとして静的な塗り分けから外してある。開き具合は回折・透過の経路が
        //   連続量として出しているので、量を決めるときはそちらと組むこと。
        public bool GetApertureInfo(int index, out float area, out Vector3 center,
                                    out Vector3 normal, out int roomA, out int roomB)
        {
            area = 0f; center = Vector3.zero; normal = Vector3.zero; roomA = -1; roomB = -1;
            if (_handle == IntPtr.Zero) return false;
            AFVector3 c, n;
            if (Native.AF_SceneApertureInfo(_handle, index, out area, out c, out n,
                                            out roomA, out roomB) == 0) return false;
            center = c.ToVector3(); normal = n.ToVector3();
            return true;
        }

        // ボクセル一辺(m)。細かいほど狭い戸口を見分けられるがコストが増える。
        public void SetRoomCellSize(float meters)
        {
            if (_handle != IntPtr.Zero) Native.AF_SceneSetRoomCellSize(_handle, meters);
        }

        // 【C1】部屋グラフの開口からポータルを自動生成する。手置きは消えない。
        public void SetAutoPortals(bool on)
        {
            if (_handle == IntPtr.Zero) return;
            try { Native.AF_SceneSetAutoPortals(_handle, on ? 1 : 0); }
            catch (System.Exception) { }   // 古い DLL。ABI 照合の側で警告が出る
        }
        // 音源ごとの段を何コアで回すか。1 以下＝直列（既定）。
        //   中で値の変化を見ているので毎フレーム押してよい（変わったときだけ作り直す）。
        //   ⚠ Unity は既に全コアでジョブを回している。増やしすぎると食い合って
        //     音響は速くなったのに他が遅くなる。上げたら必ず全体のフレーム時間で確かめること。
        public void SetWorkerThreads(int n)
        {
            if (_handle == IntPtr.Zero) return;
            try { Native.AF_SceneSetWorkerThreads(_handle, n); }
            catch (System.Exception) { }   // 古い DLL。ABI 照合の側で警告が出る
        }
        public int GetWorkerThreads()
        {
            if (_handle == IntPtr.Zero) return 0;
            try { return Native.AF_SceneGetWorkerThreads(_handle); }
            catch (System.Exception) { return 0; }
        }

        /// 音源の段。ホストが決める段は**上限**で、予算（SetTierBudget）がそこから下げる。
        ///   Exact   … 回折の合成まで。体験の芯（扉の奥の音・探しているベル）
        ///   Simple  … 遮蔽の音量と帯域カーブだけ。回り込みの方向は出ない
        ///   Virtual … 解かない。可聴限界の外＝素通し。ホストは再生位置だけ進める
        /// GetSourceTierEffective は 3 = 保持（予算から漏れて最後の答えを保っている）も返す。
        ///   聞こえている音源なので、Virtual と違って尾を止めないこと。
        /// ★2D の音（UI・音楽・ナレーション）に段はない。**音源として登録しない**。
        public enum SourceTier { Exact = 0, Simple = 1, Virtual = 2 }
        public const int TierHeld = 3;   // GetSourceTierEffective の値だけに現れる

        public void SetSourceTier(ulong id, SourceTier tier)
        {
            if (_handle == IntPtr.Zero) return;
            try { Native.AF_SceneSetSourceTier(_handle, id, (int)tier); }
            catch (System.Exception) { }   // 古い DLL。ABI 照合の側で警告が出る
        }

        /// 自由音場で聞こえなくなる距離(m)。0 以下＝自動バーチャルを使わない。
        /// ⚠ 響く部屋ではエンジンが自動バーチャルを止める（残響は距離で減らないため）。
        public void SetSourceAudibleRadius(ulong id, float metres)
        {
            if (_handle == IntPtr.Zero) return;
            try { Native.AF_SceneSetSourceAudibleRadius(_handle, id, metres); }
            catch (System.Exception) { }
        }

        /// いま実際に使われている段（自動バーチャルと予算の結果込み。3 = 保持）。見つからなければ -1。
        public int GetSourceTierEffective(int index)
        {
            if (_handle == IntPtr.Zero) return -1;
            try { return Native.AF_SceneGetSourceTierEffective(_handle, index); }
            catch (System.Exception) { return -1; }
        }

        // ── 段の予算と順位（主スレッドの上限）──
        //   厳密・簡易の本数。0 = 無制限（既定。呼ばなければ今までどおり手動の段）。超えたぶんは
        //   可聴性（音量 × 1/r × 生存 × 重要度）の低い順に保持へ落ちる（docs/TIER_BUDGET.md）。
        public void SetTierBudget(int exactMax, int simpleMax)
        {
            if (_handle == IntPtr.Zero) return;
            try { Native.AF_SceneSetTierBudget(_handle, exactMax, simpleMax); }
            catch (System.Exception) { }   // 古い DLL: 予算なし（全音源が手動の段）
        }
        /// 順位の物差しの「音量」（線形）。AudioSource.volume を毎フレーム押してよい。
        public void SetSourceLoudness(ulong id, float gainLinear)
        {
            if (_handle == IntPtr.Zero) return;
            try { Native.AF_SceneSetSourceLoudness(_handle, id, gainLinear); }
            catch (System.Exception) { }
        }
        /// 重要度（倍率）と固定。固定は予算に関わらず段を保つ（枠は消費する）。
        public void SetSourceImportance(ulong id, float importance, bool pinned)
        {
            if (_handle == IntPtr.Zero) return;
            try { Native.AF_SceneSetSourceImportance(_handle, id, importance, pinned ? 1 : 0); }
            catch (System.Exception) { }
        }
        /// 直近の順位の物差し（HUD 用）。範囲外は -1。
        public float GetSourcePriority(int index)
        {
            if (_handle == IntPtr.Zero) return -1f;
            try { return Native.AF_SceneGetSourcePriority(_handle, index); }
            catch (System.Exception) { return -1f; }
        }
        /// このフレームの探り（保持 → 簡易で解き直し）の音源 index。無ければ -1。
        public int GetTierProbeIndex()
        {
            if (_handle == IntPtr.Zero) return -1;
            try { return Native.AF_SceneGetTierProbeIndex(_handle); }
            catch (System.Exception) { return -1; }
        }

        // ── 更新の非同期化（主スレッドの上限）──
        //   ON: Update は解かずに帰り（数 µs）、DLL のワーカースレッドが解く。設定は待ち行列で次の着手に効き、
        //   結果（遮蔽・タップ・尾・段）は 1 フレーム前の入力に対する写しから返る。幾何の問い合わせは解く段と並走する。
        //   ⚠ Dispose（AF_SceneDestroy）は仕事を待ってから畳む。OnDisable で必ず Dispose すること。
        public void SetAsync(bool on)
        {
            if (_handle == IntPtr.Zero) return;
            try { Native.AF_SceneSetAsync(_handle, on ? 1 : 0); }
            catch (System.Exception) { }   // 古い DLL: 同期のまま
        }
        public bool IsAsync
        {
            get
            {
                if (_handle == IntPtr.Zero) return false;
                try { return Native.AF_SceneIsAsync(_handle) != 0; }
                catch (System.Exception) { return false; }
            }
        }
        /// 走っている仕事を待ち、答えを写す（「このフレームの答えが要る」とき。毎フレーム呼ぶと同期と同じ費用になる）。
        public void AsyncWait()
        {
            if (_handle == IntPtr.Zero) return;
            try { Native.AF_SceneAsyncWait(_handle); }
            catch (System.Exception) { }
        }
        /// 統計: ワーカーの計算時間(ms)、写しの古さ(フレーム)、追いつかず投げられなかったフレーム数、待ち行列の長さ。
        public void GetUpdateStats(out float computeMs, out int lagFrames, out int skippedFrames, out int queued)
        {
            computeMs = 0f; lagFrames = 0; skippedFrames = 0; queued = 0;
            if (_handle == IntPtr.Zero) return;
            try { Native.AF_SceneGetUpdateStats(_handle, out computeMs, out lagFrames, out skippedFrames, out queued); }
            catch (System.Exception) { }
        }

        // ── キャプチャ（サウンドデバッグツール）──────────────────────
        //   起動時に CaptureBegin を呼んだら、あとは何もしなくてよい（Update の中で溜まる）。
        //   「いま変だった」と思ったら CaptureMark。前後が揃うと Status が 2 になる。

        /// 録音を始める。0 以下の項目は既定（前 21 秒・後 5 秒・64 音源・48kHz）。
        public void CaptureBegin(int prerollFrames = 0, int postrollFrames = 0,
                                 int maxSources = 0, int sampleRate = 0, bool recordPcm = true)
        {
            if (_handle == IntPtr.Zero) return;
            try { Native.AF_SceneCaptureBegin(_handle, prerollFrames, postrollFrames,
                                              maxSources, sampleRate, recordPcm ? 1 : 0); }
            catch (System.Exception) { }
        }

        /// この音源の尾を担っている代表音源の index。範囲外は -1。
        /// ★規則（同じ部屋のいちばん若い index）はエンジン側に 1 つだけ。
        public int GetTailShapeIndex(int index)
        {
            if (_handle == IntPtr.Zero) return -1;
            try { return Native.AF_SceneGetTailShapeIndex(_handle, index); }
            catch (System.Exception) { return -1; }
        }

        /// いまエッジカタログに載っている稜線の本数（計器表示用）。
        public int EdgeCatalogCount()
        {
            if (_handle == IntPtr.Zero) return 0;
            try { return Native.AF_SceneEdgeCatalogCount(_handle); }
            catch (System.Exception) { return 0; }
        }

        /// 段の間引きの位相（8要素）。入力リプレイで尾を再現するために要る。
        public int[] DebugGetStagePhase()
        {
            var p = new int[8];
            if (_handle == IntPtr.Zero) return p;
            try { Native.AF_SceneDebugGetStagePhase(_handle, p); }
            catch (System.Exception) { }
            return p;
        }

        /// ⚠ リスナー位置を押したあとに呼ぶこと（周回の基準点を合わせるため）。
        public void DebugSetStagePhase(int[] phase8)
        {
            if (_handle == IntPtr.Zero || phase8 == null || phase8.Length < 8) return;
            try { Native.AF_SceneDebugSetStagePhase(_handle, phase8); }
            catch (System.Exception) { }
        }

        public void CaptureEnd()
        {
            if (_handle == IntPtr.Zero) return;
            try { Native.AF_SceneCaptureEnd(_handle); }
            catch (System.Exception) { }
        }

        /// 「いま変だった」を押す。前後が揃うまで 2 回目以降は無視される。
        public void CaptureMark()
        {
            if (_handle == IntPtr.Zero) return;
            try { Native.AF_SceneCaptureMark(_handle); }
            catch (System.Exception) { }
        }

        /// 0=止まっている 1=録っている 2=前後が揃った（保存できる）。
        public int CaptureStatus(out int framesHeld)
        {
            framesHeld = 0;
            if (_handle == IntPtr.Zero) return 0;
            try { return Native.AF_SceneCaptureStatus(_handle, out framesHeld); }
            catch (System.Exception) { return 0; }
        }

        /// ⚠ オーディオスレッドから呼ぶこと（OnAudioFilterRead）。
        public void CapturePushAudio(float[] interleavedStereo, int frames)
        {
            if (_handle == IntPtr.Zero || interleavedStereo == null) return;
            try { Native.AF_SceneCapturePushAudio(_handle, interleavedStereo, frames); }
            catch (System.Exception) { }
        }

        /// .afcap として保存する。成功で true。
        ///   sceneName / dllHash はヘッダに焼く識別子（開いたときに現物と照合するため）。
        public bool CaptureWrite(string path, string sceneName, uint dllHash)
        {
            if (_handle == IntPtr.Zero || string.IsNullOrEmpty(path)) return false;
            try { return Native.AF_SceneCaptureWrite(_handle, path, sceneName, dllHash) != 0; }
            catch (System.Exception) { return false; }
        }

        public void SetAutoPortalMinArea(float m2)
        {
            if (_handle == IntPtr.Zero) return;
            try { Native.AF_SceneSetAutoPortalMinArea(_handle, m2); }
            catch (System.Exception) { }
        }
        public void SetPortalGovernRange(float meters)
        {
            if (_handle == IntPtr.Zero) return;
            try { Native.AF_SceneSetPortalGovernRange(_handle, meters); }
            catch (System.Exception) { }
        }
        public int GetPortalCounts(out int autoCount, out int manualCount)
        {
            autoCount = 0; manualCount = 0;
            if (_handle == IntPtr.Zero) return 0;
            try { return Native.AF_SceneGetPortalCounts(_handle, out autoCount, out manualCount); }
            catch (System.Exception) { return 0; }
        }

        // ポータル 1 枚の矩形を読み出す。自動生成が戸口を正しく見つけたかは
        // **見れば分かる**ようにしておきたい（音で気づくのは難しい）。
        public bool GetPortal(int id, out Vector3 center, out Vector3 axisU, out Vector3 axisV,
                              out float halfU, out float halfV)
        {
            center = Vector3.zero; axisU = Vector3.right; axisV = Vector3.up;
            halfU = 0f; halfV = 0f;
            if (_handle == IntPtr.Zero) return false;
            try
            {
                AFVector3 c, u, v;
                if (Native.AF_SceneGetPortal(_handle, id, out c, out u, out v,
                                             out halfU, out halfV) == 0) return false;
                center = c.ToVector3(); axisU = u.ToVector3(); axisV = v.ToVector3();
                return true;
            }
            catch (System.Exception) { return false; }
        }

        // ── 扉の定点（隣の空間の響きを戸口の位置から鳴らす）──

        /// ポータルが繋いでいる部屋。toOutside なら片側が外の世界（洞窟の口・屋外へ開く戸口、roomA=-1）。
        public bool GetPortalRooms(int id, out int roomA, out int roomB, out bool toOutside)
        {
            roomA = -1; roomB = -1; toOutside = false;
            if (_handle == IntPtr.Zero) return false;
            try
            {
                int o;
                if (Native.AF_SceneGetPortalRooms(_handle, id, out roomA, out roomB, out o) == 0) return false;
                toOutside = (o != 0);
                return true;
            }
            catch (System.Exception) { return false; }
        }

        /// 外の世界へ開く口も自動ポータルにする（既定 OFF）。回折の経路生成は使わず、扉の定点だけが使う。
        /// 中で値の変化を見ているので毎フレーム押してよい。
        private int _outsideApertures = -1;
        public void SetOutsideApertures(bool on)
        {
            if (_handle == IntPtr.Zero) return;
            int v = on ? 1 : 0;
            if (v == _outsideApertures) return;
            _outsideApertures = v;
            try { Native.AF_SceneSetOutsideApertures(_handle, v); } catch (System.Exception) { }
        }

        /// 隣の空間の拡散した響きがこの口を通る割合（帯域別 6 要素、0..1）。音源に依存しない。
        public bool PortalDiffuseCoupling(int id, float[] out6)
        {
            if (_handle == IntPtr.Zero || out6 == null || out6.Length < 6) return false;
            try { return Native.AF_ScenePortalDiffuseCoupling(_handle, id, out6) != 0; }
            catch (System.Exception) { return false; }
        }

        // 部屋が 2 つ以上あるのにポータルが 1 枚も無い状態を **1 度だけ** 警告する。
        //
        //   ★これが C1 のいちばんの動機。ポータルが無いと §4.3 のフレネル帯域積分が
        //     一度も走らず、戸口はただの幾何の隙間として一般の回折で処理される。
        //     「扉がどれだけ開いているかを音色で伝える」というこのエンジンの主題が
        //     丸ごと動いていないのに、**静かに劣化するだけで気づく手段が無い**。
        //     神殿・洞窟を作ったときに実際に起きた。
        private bool _portalWarned;
        public void CheckPortalsPresent()
        {
            if (_handle == IntPtr.Zero || _portalWarned) return;
            int rooms = RoomCount;
            if (rooms < 2) return;                      // 1 部屋なら開口はそもそも無い
            int nAuto, nMan;
            int total = GetPortalCounts(out nAuto, out nMan);
            if (total > 0) return;
            _portalWarned = true;
            UnityEngine.Debug.LogWarning(
                $"[AcousticScene] 部屋が {rooms} 個あるのにポータルが 1 枚もありません。\n"
                + "  戸口はただの幾何の隙間として扱われ、フレネル帯域積分"
                + "（＝扉の開き具合を音色で伝える処理）が走りません。\n"
                + "  autoPortals を ON にするか、AcousticPortal を戸口に置いてください。");
        }

        // 格子が上限に当たって粗くなっていたら **1 度だけ** 警告する。
        //
        //   ★格子は登録された全ボックスの AABB 全体を覆う。総ボクセル数が上限（400万）を
        //     超えると、収まるまでセルが 1.5 倍ずつ粗くなる ── つまり
        //     **広い地面を 1 枚置くだけで SetRoomCellSize が黙って無視される**。
        //     0.25m → 0.375m の降格で 0.9m の戸口が割れなくなり、部屋が繋がったまま
        //     鳴る（＝別の部屋の残響が乗る）。音の結果が変わるのに何も出ないので、
        //     気づけるのは「なんか変」だけになる。ここで数字にして出す。
        //   ★直し方は「音響用の形状を遊ぶ範囲だけに絞る」。見た目の地面は
        //     コライダーを付けなければ格子に入らない。
        private bool _gridWarned;
        public void CheckRoomGridDegraded()
        {
            if (_handle == IntPtr.Zero || _gridWarned) return;
            int bad;
            float req, act; double nv, maxv;
            try { bad = Native.AF_SceneRoomGridDegraded(_handle, out req, out act, out nv, out maxv); }
            catch (System.Exception) { return; }   // 古い DLL。ABI 照合の側で警告が出る
            if (bad == 0) return;
            _gridWarned = true;
            UnityEngine.Debug.LogWarning(
                $"[AcousticScene] 部屋グラフの格子が粗くなりました: 要求 {req:0.###}m → 実際 {act:0.###}m"
                + $"（{nv:N0} / 上限 {maxv:N0} ボクセル）。\n"
                + $"  幅 {act * 2f:0.##}m 未満の戸口では部屋が割れません"
                + "（部屋が繋がったまま鳴る＝別の部屋の残響が乗る）。\n"
                + "  音響用の形状を遊ぶ範囲だけに絞ってください"
                + "（見た目の地面はコライダーを外せば格子に入りません）。");
        }

        // 部屋を戸口で割る半径(m)。幅がこの 2 倍に満たないくびれで部屋が分かれる。
        public void SetRoomSeedRadius(float meters)
        {
            if (_handle != IntPtr.Zero) Native.AF_SceneSetRoomSeedRadius(_handle, meters);
        }

        // 反射経路：origin→dir を鏡面反射で maxBounces 回追い、通過点を outPoints に書き点数を返す。
        // 内部バッファ(_pathBuf)を使い回して毎フレームの GC を避ける。
        private AFVector3[] _pathBuf;
        // 早期反射タップの像源位置バッファ（使い回し）。
        private AFVector3[] _erBuf;
        // 回折候補の迂回点バッファ（可視化用・使い回し）。
        private AFVector3[] _candBuf;
        // 回折二次音源の位置バッファ（使い回し）。
        private AFVector3[] _diffSrcBuf;
        public int TraceReflectionPath(Vector3 origin, Vector3 dir, float maxDist,
                                       int maxBounces, Vector3[] outPoints)
        {
            if (_handle == IntPtr.Zero || outPoints == null || outPoints.Length < 2) return 0;
            int cap = outPoints.Length;
            if (_pathBuf == null || _pathBuf.Length < cap) _pathBuf = new AFVector3[cap];
            int n = Native.AF_SceneTraceReflectionPath(
                _handle, new AFVector3(origin), new AFVector3(dir), maxDist, maxBounces, _pathBuf, cap);
            for (int i = 0; i < n; i++) outPoints[i] = new Vector3(_pathBuf[i].x, _pathBuf[i].y, _pathBuf[i].z);
            return n;
        }

        // A: 早期反射タップ抽出。像源位置を outImagePos、帯域ゲインを outGain(len=maxTaps*6) に書く。
        // 戻り値=書き込んだタップ数。outImagePos.Length を maxTaps とみなす。
        public int ComputeEarlyReflections(Vector3 listener, Vector3 source,
                                           Vector3[] outImagePos, float[] outGain,
                                           int numRays, int maxBounces)
        {
            if (_handle == IntPtr.Zero || outImagePos == null || outGain == null) return 0;
            int maxTaps = outImagePos.Length;
            if (maxTaps <= 0 || outGain.Length < maxTaps * 6) return 0;
            if (_erBuf == null || _erBuf.Length < maxTaps) _erBuf = new AFVector3[maxTaps];
            int n = Native.AF_SceneComputeEarlyReflections(
                _handle, new AFVector3(listener), new AFVector3(source),
                _erBuf, outGain, maxTaps, numRays, maxBounces);
            for (int i = 0; i < n; i++) outImagePos[i] = new Vector3(_erBuf[i].x, _erBuf[i].y, _erBuf[i].z);
            return n;
        }

        // 可視化：遮蔽時の回折候補の迂回点を outPoints、余剰δを outDeltas に書く。戻り値=候補数。
        // outPoints.Length と outDeltas.Length の小さい方を上限とみなす。
        public int DiffractionCandidates(Vector3 from, Vector3 to, Vector3[] outPoints, float[] outDeltas)
        {
            if (_handle == IntPtr.Zero || outPoints == null || outDeltas == null) return 0;
            int cap = Mathf.Min(outPoints.Length, outDeltas.Length);
            if (cap <= 0) return 0;
            if (_candBuf == null || _candBuf.Length < cap) _candBuf = new AFVector3[cap];
            int n = Native.AF_SceneDiffractionCandidates(
                _handle, new AFVector3(from), new AFVector3(to), _candBuf, outDeltas, cap);
            for (int i = 0; i < n; i++) outPoints[i] = new Vector3(_candBuf[i].x, _candBuf[i].y, _candBuf[i].z);
            return n;
        }

        // 回折の二次音源：遮蔽時のエッジをクラスタし、方向つき仮想音源を outPos/outGain に書く。戻り=本数。
        public int ComputeDiffractionSources(Vector3 listener, Vector3 source,
                                             Vector3[] outPos, float[] outGain)
        {
            if (_handle == IntPtr.Zero || outPos == null || outGain == null) return 0;
            int cap = Mathf.Min(outPos.Length, outGain.Length);
            if (cap <= 0) return 0;
            if (_diffSrcBuf == null || _diffSrcBuf.Length < cap) _diffSrcBuf = new AFVector3[cap];
            int n = Native.AF_SceneComputeDiffractionSources(
                _handle, new AFVector3(listener), new AFVector3(source), _diffSrcBuf, outGain, cap);
            for (int i = 0; i < n; i++) outPos[i] = new Vector3(_diffSrcBuf[i].x, _diffSrcBuf[i].y, _diffSrcBuf[i].z);
            return n;
        }


        public int InstanceCount => _handle != IntPtr.Zero ? Native.AF_SceneInstanceCount(_handle) : 0;

        public void Dispose()
        {
            if (_handle != IntPtr.Zero)
            {
                Native.AF_SceneDestroy(_handle);
                _handle = IntPtr.Zero;
            }
        }
    }
}
