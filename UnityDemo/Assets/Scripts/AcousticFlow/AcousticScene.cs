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

        // B: キューブマップ エッジカタログ構築（リスナー中心・全音源共有）。res=面解像度。
        public void BuildEdgeCatalog(Vector3 listener, int res, float maxDist)
        {
            if (_handle == IntPtr.Zero) return;
            Native.AF_SceneBuildEdgeCatalog(_handle, new AFVector3(listener), res, maxDist);
        }
        public int EdgeCatalogCount => _handle != IntPtr.Zero ? Native.AF_SceneEdgeCatalogCount(_handle) : 0;
        public void ClearEdgeCatalog() { if (_handle != IntPtr.Zero) Native.AF_SceneClearEdgeCatalog(_handle); }

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
