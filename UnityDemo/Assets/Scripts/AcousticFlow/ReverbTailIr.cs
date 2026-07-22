// ReverbTailIr.cs
// 実測エコグラム（帯域別）から、後期残響の尾の IR を組み立てる。
//
// 考え方:
//   エコグラムのビンは「その時刻に届くエネルギー」＝尾のエネルギー包絡そのもの。
//   後期残響の波形は知覚的にはノイズなので、
//       尾IR(t) = ノイズ(t) × √(エネルギー包絡(t))
//   で作れる（エネルギー→振幅なので √）。帯域ごとに包絡が違うので、
//   1 本のノイズを 6 帯域に分けてから各々の包絡を掛け、足し戻す。
//   こうすると「高域が先に減る」という実際の部屋の挙動が、ダンピング係数の捏造ではなく
//   実測カーブとして出る。
//
// ノイズ系列は固定:
//   包絡が更新されるたびにノイズを引き直すと、IR 差し替え時に波形が完全に別物になって
//   プチッと鳴る。系列を固定して包絡だけ差し替えれば、前後の IR は強く相関するので
//   ブロック境界での切替がほぼ聞こえない。尾はもともとノイズなので、
//   「どのノイズか」に音質上の意味は無い。
using UnityEngine;

namespace AcousticFlow
{
    public sealed class ReverbTailIr
    {
        private const int kNumBands = 6;
        private static readonly float[] kCrossHz = { 177f, 354f, 707f, 1414f, 2828f };

        private readonly int _sampleRate;
        private readonly int _length;      // 尾IRの長さ(サンプル)
        private readonly int _channels;

        // 固定ノイズを帯域分解したもの。[channel][band][time]
        private readonly float[][][] _bandNoise;
        // 組み立て先。[channel][time]
        private readonly float[][] _ir;

        public float[][] Ir => _ir;
        public int Length => _length;

        public ReverbTailIr(int sampleRate, int length, int channels, int seed = 12345)
        {
            _sampleRate = sampleRate;
            _length = Mathf.Max(1, length);
            _channels = Mathf.Max(1, channels);

            _ir = new float[_channels][];
            _bandNoise = new float[_channels][][];

            uint rng = (uint)seed;
            var noise = new float[_length];
            for (int c = 0; c < _channels; c++)
            {
                _ir[c] = new float[_length];
                // チャンネルごとに独立なノイズ＝L/R が無相関になり、尾が広がる。
                for (int i = 0; i < _length; i++)
                {
                    rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
                    noise[i] = (int)rng * (1f / 2147483648f);
                }
                _bandNoise[c] = SplitBands(noise, _sampleRate);
            }
        }

        // 白色ノイズを 6 帯域に分ける（IrConvolver と同じ直列クロスオーバー）。
        // 全帯域を足すと元のノイズに戻る＝包絡が全帯域同じなら白色のまま。
        private static float[][] SplitBands(float[] src, int sampleRate)
        {
            int n = src.Length;
            var outB = new float[kNumBands][];
            for (int b = 0; b < kNumBands; b++) outB[b] = new float[n];

            var lp = new Biquad[kNumBands - 1];
            for (int i = 0; i < lp.Length; i++) lp[i].SetLowpass(kCrossHz[i], sampleRate);

            for (int i = 0; i < n; i++)
            {
                float rest = src[i];
                for (int b = 0; b < kNumBands - 1; b++)
                {
                    float lo = lp[b].Process(rest);
                    outB[b][i] = lo;
                    rest -= lo;
                }
                outB[kNumBands - 1][i] = rest;
            }
            return outB;
        }

        // 包絡の平滑用スクラッチ（binCount×6）。確保し直さないよう使い回す。
        private float[] _env;
        // 更新をまたいで保持する包絡。毎回の測定をここへ少しずつ寄せる。
        private float[] _envAcc;
        private int _envAccBins;

        // エコグラムの包絡を時間方向に平滑する。
        //
        // なぜ必要か:
        //   レイは有限本数・有限バウンスなので、到達時刻が平均自由行程の倍数付近に固まる。
        //   その塊をそのまま包絡に使うと、ノイズに周期的な振幅変調が掛かって
        //   「なめらかな尾」ではなく「山彦（離散したエコー）」に聞こえる。
        //   実際の部屋では反射密度は t² で増えて滑らかになるので、この塊は
        //   部屋の性質ではなく推定量のばらつき＝평滑して消すのが正しい。
        //   減衰カーブそのもの（数百ms スケール）は平滑しても保たれる。
        // growth: 平滑幅を時刻に比例して広げる割合（0 で固定幅）。
        //   後期ほど推定の分散が大きい。エネルギーが減るうえ、レイのバウンス数上限のせいで
        //   遅い時刻に届く経路の本数自体が激減するため。固定幅だと後半だけガタついたままになり、
        //   それがノイズへの振幅変調＝残響の粒立ちとして聞こえる。
        //   広い部屋ほど顕著（平均自由行程が長く、同じバウンス数で届く時刻が短い）。
        private void SmoothEnvelope(float[] echoBands, int binCount, int halfWin, float growth)
        {
            int need = binCount * kNumBands;
            if (_env == null || _env.Length < need) _env = new float[need];
            if (halfWin <= 0 && growth <= 0f)
            {
                System.Array.Copy(echoBands, _env, need);
                return;
            }
            for (int b = 0; b < kNumBands; b++)
            {
                for (int k = 0; k < binCount; k++)
                {
                    int w = Mathf.Max(halfWin, Mathf.RoundToInt(k * growth));
                    float sum = 0f;
                    int n = 0;
                    int lo = Mathf.Max(0, k - w), hi = Mathf.Min(binCount - 1, k + w);
                    for (int j = lo; j <= hi; j++) { sum += echoBands[j * kNumBands + b]; n++; }
                    _env[k * kNumBands + b] = (n > 0) ? sum / n : 0f;
                }
            }
        }

        // 測定した包絡を、更新をまたいで時間平均する。
        //
        // 二重の効果がある:
        //   ① 分散が下がる。レイの本数を増やさずに推定を滑らかにできるので、
        //      空間方向の平滑（減衰カーブの情報を潰す）に頼らずに粒立ちを減らせる。
        //   ② 更新ごとの IR の差が小さくなる。尾の畳み込みは IR をブロック境界で
        //      ハードスワップするので、差が大きいと切替が音として聞こえる。
        //      特に早期↔後期の境目が動くと包絡の先頭（一番大きい所）が変わるため目立つ。
        //
        // alpha=1 で平均なし（毎回の測定をそのまま使う）。
        private void AccumulateEnvelope(int binCount, float alpha)
        {
            int need = binCount * kNumBands;
            if (_envAcc == null || _envAcc.Length < need || _envAccBins != binCount)
            {
                _envAcc = new float[need];
                _envAccBins = binCount;
                System.Array.Copy(_env, _envAcc, need);   // 初回は測定値で埋める
                return;
            }
            alpha = Mathf.Clamp01(alpha);
            for (int i = 0; i < need; i++)
                _envAcc[i] += (_env[i] - _envAcc[i]) * alpha;
            System.Array.Copy(_envAcc, _env, need);
        }

        // 帯域別エコグラムから尾 IR を組み立てる。
        //   echoBands[k*6 + b] : 時間ビン k・帯域 b のエネルギー
        //   binMs              : 1 ビンの長さ(ms)
        //   startMs            : ここより前は早期反射側が担当するので 0 にする（二重計上を防ぐ）
        //   fadeMs             : startMs 付近の立ち上がりをなめらかにする幅
        //   smoothMs           : 包絡の平滑幅（山彦対策）。0 で無効
        // 戻り値: 尾/直接 のエネルギー比（エコグラム単位）。0 なら尾なし。
        //   IR は「エネルギー1」に正規化して返すので、絶対レベルは呼び手がこの比から決める
        //   （CalibrateGain 参照）。エコグラムは 1/r を含まない“相対の形”だが、
        //   直接ビンと尾ビンは同じ単位なので、両者の比は意味を持つ。
        public float Build(float[] echoBands, int binCount, float binMs, float startMs, float fadeMs,
                           float smoothMs, float smoothGrowth, float envAlpha)
        {
            for (int c = 0; c < _channels; c++) System.Array.Clear(_ir[c], 0, _length);
            if (echoBands == null || binCount <= 0 || binMs <= 0f) return 0f;

            // 平滑 ＋ 距離減衰の適用。以降は _env（加工済み）を使う。
            // ※ 距離減衰はエンジン側（音源→反射点の区間）で掛け済み。ここでは掛けない。
            //    以前はここでビンの到達時刻＝総経路長から掛けていたが、それだと尾に
            //    1/t² の偽の減衰が乗り、広い部屋の中央ほど反響が痩せた。
            SmoothEnvelope(echoBands, binCount, Mathf.RoundToInt(smoothMs / binMs * 0.5f), smoothGrowth);
            AccumulateEnvelope(binCount, envAlpha);
            echoBands = _env;

            // 直接音ビン（= 立ち上がりの最大ビン）と、尾の総エネルギーを測る。
            //   IR のエネルギーは加算的なので、ビンを足したものがそのまま尾のエネルギー。
            //   直接ビンは真の IR ではインパルス1本＝エネルギー g_d² に対応する。
            int startBinIdx = Mathf.Clamp(Mathf.FloorToInt(startMs / binMs), 0, binCount);
            float directEnergy = 0f, tailEnergy = 0f;
            for (int k = 0; k < binCount; k++)
            {
                float e = 0f;
                for (int b = 0; b < kNumBands; b++) e += echoBands[k * kNumBands + b];
                if (k < startBinIdx) { if (e > directEnergy) directEnergy = e; }
                else tailEnergy += e;
            }
            float ratio = (directEnergy > 1e-20f) ? tailEnergy / directEnergy : 0f;

            float samplesPerBin = binMs * 0.001f * _sampleRate;
            if (samplesPerBin < 1f) return 0f;
            int startSample = Mathf.Clamp(Mathf.RoundToInt(startMs * 0.001f * _sampleRate), 0, _length);
            int fadeSamples = Mathf.Max(1, Mathf.RoundToInt(fadeMs * 0.001f * _sampleRate));

            float totalEnergy = 0f;
            for (int c = 0; c < _channels; c++)
            {
                var ir = _ir[c];
                var bn = _bandNoise[c];
                for (int b = 0; b < kNumBands; b++)
                {
                    var nb = bn[b];
                    for (int i = startSample; i < _length; i++)
                    {
                        // ビン境界で段差が出ないよう、包絡はビン間を線形補間する。
                        float x = i / samplesPerBin - 0.5f;
                        int k0 = Mathf.FloorToInt(x);
                        float f = x - k0;
                        float e0 = (k0 >= 0 && k0 < binCount) ? echoBands[k0 * kNumBands + b] : 0f;
                        int k1 = k0 + 1;
                        float e1 = (k1 >= 0 && k1 < binCount) ? echoBands[k1 * kNumBands + b] : 0f;
                        float e = e0 + (e1 - e0) * f;
                        if (e <= 0f) continue;

                        // エネルギー→振幅は √。ビン内に散らばる分の正規化も掛ける。
                        float amp = Mathf.Sqrt(e / samplesPerBin);
                        // 早期部との継ぎ目をなめらかに。
                        int d = i - startSample;
                        if (d < fadeSamples) amp *= (float)d / fadeSamples;
                        ir[i] += nb[i] * amp;
                    }
                }
                for (int i = 0; i < _length; i++) totalEnergy += ir[i] * ir[i];
            }
            if (totalEnergy <= 1e-20f) return 0f;

            // エネルギー1に正規化する。
            //   こうしておくと「この IR で畳み込むと、入力とほぼ同じパワーが出る」状態になり、
            //   絶対レベルは呼び手が D/R 比から一意に決められる（下の CalibrateGain）。
            //   エコグラムは 1/r を含まない“形”なので、そのままでは早期タップと基準が揃わない。
            // 全チャンネル合計でエネルギー1にする。
            //   ここを /_channels にすると「1チャンネルあたり1」＝合計2になり、
            //   ステレオのとき尾が √2 だけ大きく鳴ってしまう。
            float norm = 1f / Mathf.Sqrt(totalEnergy);
            for (int c = 0; c < _channels; c++)
            {
                var ir = _ir[c];
                for (int i = 0; i < _length; i++) ir[i] *= norm;
            }
            return ratio;
        }

        // 尾の絶対ゲインを決める。
        //   directGain : 直接音タップの広帯域ゲイン（距離減衰・透過を含む＝絶対レベルの基準）
        //   target     : 残響/直接エネルギーの目標比 (r/r_c)²（物理式・呼び手が算出）
        // 尾IRはエネルギー1に正規化済みなので、持続入力に対する畳み込み出力パワーは入力と等しい。
        // よって tailGain = directGain × √target で、出力の 尾/直接 パワー比がちょうど target。
        public static float CalibrateGain(float directGain, float target)
        {
            if (target <= 0f || directGain <= 0f) return 0f;
            return directGain * Mathf.Sqrt(target);
        }

        // RBJ バイカッド（IrConvolver のものと同型。オフライン生成用なので別に持つ）。
        private struct Biquad
        {
            private float b0, b1, b2, a1, a2, z1, z2;
            public void SetLowpass(float fc, float fs)
            {
                float w0 = 2f * Mathf.PI * fc / fs;
                float cw = Mathf.Cos(w0), sw = Mathf.Sin(w0);
                float alpha = sw / (2f * 0.70710678f);
                b0 = (1f - cw) * 0.5f; b1 = 1f - cw; b2 = (1f - cw) * 0.5f;
                float a0 = 1f + alpha; a1 = -2f * cw; a2 = 1f - alpha;
                b0 /= a0; b1 /= a0; b2 /= a0; a1 /= a0; a2 /= a0;
                z1 = 0f; z2 = 0f;
            }
            public float Process(float x)
            {
                float y = b0 * x + z1;
                z1 = b1 * x - a1 * y + z2;
                z2 = b2 * x - a2 * y;
                return y;
            }
        }
    }
}
