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

        public void SetUpdateConfig(Native.AFUpdateConfig cfg)
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

        // 帯域別エコグラム（outBins は numBins*6 要素）。書けたビン数を返す。
        public int GetEchogramBands(float[] outBins, int numBins)
        {
            if (_handle == IntPtr.Zero || _sourceRegistryMissing || outBins == null) return 0;
            return Native.AF_SceneGetEchogramBands(_handle, outBins, numBins);
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
