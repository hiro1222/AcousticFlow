// HrtfSet.cs
// HRTF（頭部伝達関数）データセット。方向 → 左右耳のインパルス応答(HRIR)を返す。
//
// 役割:
//   ・実測データセット（SOFA を事前変換した .afhr バイナリ）の読み込み
//   ・データが無い環境向けの合成HRTF（球体頭モデル）生成
//   ・方向からの HRIR 検索
//   ・個人最適化: ITD（両耳間時間差）を頭のサイズでスケール
//
// ITD を HRIR から分離して持つ理由:
//   実測 HRIR には ITD が波形の遅れとして焼き込まれている。そのまま使うと
//   「そのデータを測った人の頭の大きさ」に固定される。頭囲が違うと音像が
//   頭内に入ったり幅が不自然になるので、読み込み時に
//     ①各耳の立ち上がりを検出して 0 に揃える（ITD を抜く）
//     ②抜いた ITD は別に保持する
//   としておき、再生時に「ITD × 個人スケール」を遅延として掛け直す。
//   これで頭のサイズをスライダ1本で合わせられる（個人最適化で最も効果が大きい部分）。
using System;
using System.IO;
using UnityEngine;

namespace AcousticFlow
{
    public sealed class HrtfSet
    {
        // ファイル形式（.afhr）。SOFA(NetCDF/HDF5)は C# で読むのが重いので、
        // Python で事前にこの平坦な形式へ変換して同梱する（tools/sofa_to_afhr.py）。
        //   magic "AFHR" / version / sampleRate / numDirections / irLength
        //   → 方向配列 (az, el) × numDirections
        //   → HRIR (left[irLength], right[irLength]) × numDirections
        private const uint kMagic = 0x52484641;   // 'A','F','H','R' little-endian
        private const int kVersion = 1;

        public string Name { get; private set; } = "(none)";
        public int SampleRate { get; private set; }
        public int IrLength { get; private set; }
        public int DirectionCount => _az?.Length ?? 0;
        public bool IsValid => DirectionCount > 0 && IrLength > 0;

        // 方向（度）。az: 0=正面, +90=右, -90=左 / el: 0=水平, +90=真上。
        private float[] _az;
        private float[] _el;
        // 立ち上がりを揃えた HRIR。[dir][2][irLength]
        private float[][][] _hrir;
        // 各方向の ITD（秒）。正=右耳が遅い（＝音源が左）。
        private float[] _itdSec;

        // 検索用の単位ベクトル（リスナー座標系: +x=右, +y=上, +z=前）。
        private Vector3[] _dirVec;

        /// このデータセットが測定された頭のサイズ（ITD スケールの基準）。
        /// 実測データの頭囲が不明な場合は成人平均 57cm を仮定する。
        public float ReferenceHeadCircumferenceCm { get; private set; } = 57f;

        // ── 読み込み ──

        public static HrtfSet LoadFromFile(string path)
        {
            try
            {
                using (var fs = new FileStream(path, FileMode.Open, FileAccess.Read))
                using (var br = new BinaryReader(fs))
                {
                    var set = new HrtfSet();
                    if (!set.ReadFrom(br)) return null;
                    set.Name = Path.GetFileNameWithoutExtension(path);
                    set.PrepareLookup();
                    return set;
                }
            }
            catch (Exception e)
            {
                Debug.LogWarning($"[HrtfSet] 読み込み失敗: {path} — {e.Message}");
                return null;
            }
        }

        public static HrtfSet LoadFromBytes(byte[] data, string name)
        {
            if (data == null || data.Length < 20) return null;
            try
            {
                using (var ms = new MemoryStream(data))
                using (var br = new BinaryReader(ms))
                {
                    var set = new HrtfSet();
                    if (!set.ReadFrom(br)) return null;
                    set.Name = name;
                    set.PrepareLookup();
                    return set;
                }
            }
            catch (Exception e)
            {
                Debug.LogWarning($"[HrtfSet] 解析失敗: {name} — {e.Message}");
                return null;
            }
        }

        private bool ReadFrom(BinaryReader br)
        {
            if (br.ReadUInt32() != kMagic) { Debug.LogWarning("[HrtfSet] magic 不一致"); return false; }
            int ver = br.ReadInt32();
            if (ver != kVersion) { Debug.LogWarning($"[HrtfSet] 未対応 version={ver}"); return false; }
            SampleRate = br.ReadInt32();
            int n = br.ReadInt32();
            IrLength = br.ReadInt32();
            if (n <= 0 || IrLength <= 0 || SampleRate <= 0) return false;

            _az = new float[n];
            _el = new float[n];
            for (int i = 0; i < n; i++) { _az[i] = br.ReadSingle(); _el[i] = br.ReadSingle(); }

            _hrir = new float[n][][];
            _itdSec = new float[n];
            for (int i = 0; i < n; i++)
            {
                var l = new float[IrLength];
                var r = new float[IrLength];
                for (int k = 0; k < IrLength; k++) l[k] = br.ReadSingle();
                for (int k = 0; k < IrLength; k++) r[k] = br.ReadSingle();
                // 実測データは ITD が波形に焼き込まれているので、ここで分離する。
                _itdSec[i] = ExtractItdAndAlign(l, r, SampleRate);
                _hrir[i] = new[] { l, r };
            }
            return true;
        }

        // 左右の立ち上がりを検出し、両方を先頭へ揃える。戻り値は抜き取った ITD（秒）。
        //   立ち上がり = 「ピーク振幅の一定割合を最初に超えた位置」。単純だが実用上安定する。
        private static float ExtractItdAndAlign(float[] l, float[] r, int sampleRate)
        {
            int onsetL = FindOnset(l);
            int onsetR = FindOnset(r);
            // 共通ぶんは音源距離由来なので両耳から等しく除く。差だけが ITD。
            int common = Mathf.Min(onsetL, onsetR);
            ShiftLeft(l, common);
            ShiftLeft(r, common);
            return (onsetR - onsetL) / (float)sampleRate;
        }

        private static int FindOnset(float[] h)
        {
            float peak = 0f;
            for (int i = 0; i < h.Length; i++) peak = Mathf.Max(peak, Mathf.Abs(h[i]));
            if (peak <= 1e-9f) return 0;
            float th = peak * 0.15f;
            for (int i = 0; i < h.Length; i++) if (Mathf.Abs(h[i]) >= th) return i;
            return 0;
        }

        private static void ShiftLeft(float[] h, int n)
        {
            if (n <= 0) return;
            if (n >= h.Length) { Array.Clear(h, 0, h.Length); return; }
            Array.Copy(h, n, h, 0, h.Length - n);
            Array.Clear(h, h.Length - n, n);
        }

        private void PrepareLookup()
        {
            int n = DirectionCount;
            _dirVec = new Vector3[n];
            for (int i = 0; i < n; i++) _dirVec[i] = AngleToVector(_az[i], _el[i]);
        }

        // 方位角/仰角(度) → リスナー座標系の単位ベクトル（+x=右, +y=上, +z=前）。
        public static Vector3 AngleToVector(float azDeg, float elDeg)
        {
            float a = azDeg * Mathf.Deg2Rad;
            float e = elDeg * Mathf.Deg2Rad;
            float ce = Mathf.Cos(e);
            return new Vector3(Mathf.Sin(a) * ce, Mathf.Sin(e), Mathf.Cos(a) * ce);
        }

        // ── 合成HRTF（球体頭モデル）──
        // 実測データが無い環境でもパイプライン全体を動かすためのフォールバック。
        //   ITD: Woodworth の球体近似  ITD = (r/c)(θ + sinθ)
        //   ILD: 頭部による影（頭の反対側は高域が減る）を一次ローパスで近似
        // 個人化の器（方向→HRIR、ITD分離）は実測と同じ形で持つので、
        // 実データに差し替えても呼び出し側は変えなくてよい。
        public static HrtfSet CreateSynthetic(int sampleRate, int azStepDeg = 5, int elStepDeg = 10)
        {
            const float headRadius = 0.0875f;   // 頭半径 8.75cm（頭囲 55cm 相当）
            const float c = 343f;

            int azCount = Mathf.Max(1, 360 / Mathf.Max(1, azStepDeg));
            int elMin = -40, elMax = 60;
            int elCount = (elMax - elMin) / Mathf.Max(1, elStepDeg) + 1;
            int n = azCount * elCount;
            int irLen = 128;

            var set = new HrtfSet
            {
                Name = "Synthetic (spherical head)",
                SampleRate = sampleRate,
                IrLength = irLen,
                _az = new float[n],
                _el = new float[n],
                _hrir = new float[n][][],
                _itdSec = new float[n],
                ReferenceHeadCircumferenceCm = 2f * Mathf.PI * headRadius * 100f,
            };

            int idx = 0;
            for (int ei = 0; ei < elCount; ei++)
            {
                float el = elMin + ei * elStepDeg;
                for (int ai = 0; ai < azCount; ai++)
                {
                    float az = ai * azStepDeg;
                    if (az > 180f) az -= 360f;

                    // 耳への入射角（水平面成分で近似）。
                    float azRad = az * Mathf.Deg2Rad;
                    float elRad = el * Mathf.Deg2Rad;
                    float lateral = Mathf.Sin(azRad) * Mathf.Cos(elRad);   // -1(左) .. +1(右)
                    float theta = Mathf.Asin(Mathf.Clamp(lateral, -1f, 1f));

                    // Woodworth: 音源が右(+)なら右耳が早い＝ITDは負（右が先）。
                    float itd = -(headRadius / c) * (theta + Mathf.Sin(theta));

                    var l = new float[irLen];
                    var r = new float[irLen];
                    // 頭部の影: 遠い側の耳ほど高域が落ちる。一次LPの係数で近似。
                    float shadowL = Mathf.Clamp01(0.5f - lateral * 0.5f);   // 右に音源→左耳が影
                    float shadowR = Mathf.Clamp01(0.5f + lateral * 0.5f);
                    BuildShadowedImpulse(l, shadowL);
                    BuildShadowedImpulse(r, shadowR);

                    set._az[idx] = az;
                    set._el[idx] = el;
                    set._hrir[idx] = new[] { l, r };
                    set._itdSec[idx] = itd;
                    idx++;
                }
            }
            set.PrepareLookup();
            return set;
        }

        // 影の強さ shadow(0=完全に影 .. 1=正面) から短いインパルスを作る。
        // 影が強いほど高域が減る＝立ち上がりが鈍る、を一次LPで表現する。
        //   一次LPの直流利得は 1 なので、gain は「低域がどれだけ残るか」を決める。
        //   実際の頭部は低域(〜500Hz)ならほぼ回り込むので、低域はあまり落とさない。
        //   周波数依存の減衰は LP 側（openness）が担う。
        private static void BuildShadowedImpulse(float[] h, float shadow)
        {
            Array.Clear(h, 0, h.Length);
            float openness = Mathf.Lerp(0.12f, 1.0f, Mathf.Clamp01(shadow));  // LP係数（高域の減り方）
            float gain = Mathf.Lerp(0.75f, 1.0f, Mathf.Clamp01(shadow));      // 低域の残り方
            float state = 0f;
            for (int i = 0; i < h.Length; i++)
            {
                float x = (i == 0) ? 1f : 0f;
                state += openness * (x - state);
                h[i] = state * gain;
                if (i > 48 && Mathf.Abs(state) < 1e-5f) break;
            }
        }

        // ── 検索 ──

        /// リスナー座標系の方向ベクトルに最も近い測定点の index を返す。無効なら -1。
        public int NearestIndex(Vector3 dirListenerLocal)
        {
            if (!IsValid) return -1;
            Vector3 d = dirListenerLocal.sqrMagnitude > 1e-12f ? dirListenerLocal.normalized : Vector3.forward;
            int best = 0;
            float bestDot = -2f;
            for (int i = 0; i < _dirVec.Length; i++)
            {
                float dot = Vector3.Dot(_dirVec[i], d);
                if (dot > bestDot) { bestDot = dot; best = i; }
            }
            return best;
        }

        public float[] GetHrir(int index, int ear)
        {
            if (!IsValid || index < 0 || index >= DirectionCount) return null;
            return _hrir[index][ear];
        }

        /// その方向の ITD（秒）。正=右耳が遅い。
        public float GetItdSeconds(int index)
        {
            if (!IsValid || index < 0 || index >= DirectionCount) return 0f;
            return _itdSec[index];
        }

        /// 個人の頭囲に合わせた ITD（秒）。ITD は頭のサイズにほぼ比例する。
        ///   headCircumferenceCm: 実測 or 好みで調整した値。
        public float GetItdSecondsScaled(int index, float headCircumferenceCm)
        {
            float baseItd = GetItdSeconds(index);
            if (ReferenceHeadCircumferenceCm <= 1f) return baseItd;
            return baseItd * (headCircumferenceCm / ReferenceHeadCircumferenceCm);
        }
    }
}
