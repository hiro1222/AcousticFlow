// PartitionedConvolver.cs
// 一様分割 overlap-save による周波数領域畳み込み（Unity C#・audio thread で回る）。
//
// なぜ必要か:
//   後期残響の尾は 1 秒級＝数万サンプルある。時間領域のマルチタップでは畳み込めないので、
//   「密なフルIR」を鳴らすには周波数領域の分割畳み込みが要る。これが無かったために、
//   これまで尾は疎な近似(velvet noise)か合成(FDN)で代用されていた。
//
// なぜ大きいブロックで良いか（重要）:
//   一様分割 overlap-save の固有遅延は 1 ブロック分。通常この遅延が問題になるので
//   ブロックを小さくして数を増やす（＝重い）が、ここで畳み込むのは「後期尾」だけで、
//   尾はそもそも早期部（〜120ms）より後から始まる。つまりブロック遅延を尾の立ち上がり
//   の中に隠せる。ブロック 2048(≒43ms) でも遅れて聞こえない代わりに、パーティション数が
//   1/8 になって劇的に軽くなる。
//
// 入力はモノラル、出力は複数チャンネル（L/R）。入力 FFT は 1 回だけ行い、
// チャンネルごとに異なる IR スペクトルと掛け合わせる（＝ステレオ化がほぼ半額）。
using UnityEngine;

namespace AcousticFlow
{
    // 反復 radix-2 複素 FFT。バタフライ用のテーブルは構築時に作り、実行時は確保しない。
    public sealed class Fft
    {
        private readonly int _n;
        private readonly int[] _rev;
        private readonly float[] _cos, _sin;

        public int Size => _n;

        public Fft(int n)
        {
            if (n < 2 || (n & (n - 1)) != 0) throw new System.ArgumentException("FFT長は2の冪が必要");
            _n = n;
            _rev = new int[n];
            int bits = 0;
            while ((1 << bits) < n) bits++;
            for (int i = 0; i < n; i++)
            {
                int r = 0;
                for (int b = 0; b < bits; b++) if ((i & (1 << b)) != 0) r |= 1 << (bits - 1 - b);
                _rev[i] = r;
            }
            _cos = new float[n / 2];
            _sin = new float[n / 2];
            for (int i = 0; i < n / 2; i++)
            {
                float a = -2f * Mathf.PI * i / n;
                _cos[i] = Mathf.Cos(a);
                _sin[i] = Mathf.Sin(a);
            }
        }

        // in-place 変換。inverse=true で逆変換（1/N 正規化つき）。
        public void Transform(float[] re, float[] im, bool inverse)
        {
            int n = _n;
            for (int i = 0; i < n; i++)
            {
                int j = _rev[i];
                if (j > i)
                {
                    float t = re[i]; re[i] = re[j]; re[j] = t;
                    t = im[i]; im[i] = im[j]; im[j] = t;
                }
            }
            for (int len = 2; len <= n; len <<= 1)
            {
                int half = len >> 1;
                int step = n / len;
                for (int i = 0; i < n; i += len)
                {
                    int k = 0;
                    for (int j = 0; j < half; j++, k += step)
                    {
                        float wr = _cos[k];
                        float wi = inverse ? -_sin[k] : _sin[k];
                        int a = i + j, b = a + half;
                        float xr = re[b] * wr - im[b] * wi;
                        float xi = re[b] * wi + im[b] * wr;
                        re[b] = re[a] - xr; im[b] = im[a] - xi;
                        re[a] += xr; im[a] += xi;
                    }
                }
            }
            if (inverse)
            {
                float s = 1f / n;
                for (int i = 0; i < n; i++) { re[i] *= s; im[i] *= s; }
            }
        }
    }

    // 一様分割 overlap-save 畳み込み器。
    //   blockSize B、FFT長 2B。IR は B サンプルごとのパーティションに切って各々 FFT。
    //   入力側は「直前 B ＋ 今の B」の 2B を FFT し、周波数領域の遅延線(FDL)に積む。
    //   出力 = Σ_p FDL[now-p] * H[p] を逆変換した後半 B サンプル。
    public sealed class PartitionedConvolver
    {
        private readonly int _b;            // ブロック長
        private readonly int _fftLen;       // 2B
        private readonly int _numParts;     // パーティション数
        private readonly int _channels;
        private readonly Fft _fft;

        // 周波数領域遅延線（入力スペクトル履歴）。[part][2B]
        private readonly float[][] _fdlRe, _fdlIm;
        private int _fdlPos;

        // IR スペクトル。[channel][part][2B]。audio thread が読む側。
        private float[][][] _hRe, _hIm;
        // 差し替え待ち（main thread が作り、audio thread が Interlocked で受け取る）。
        private float[][][] _pendingHRe, _pendingHIm;

        // 実際に中身のあるパーティション数。尾が短い部屋では大半が無音なので、
        // そこを掛け算しても 0 が増えるだけ。走査を打ち切ると小部屋が大幅に安くなる。
        private int _activeParts;
        private int _pendingActiveParts;
        public int ActiveParts => _activeParts;

        // 作業領域（audio thread 専用・毎回使い回して確保しない）。
        private readonly float[] _wRe, _wIm, _accRe, _accIm;
        private readonly float[] _inBuf;    // 直前ブロック＋今のブロック（2B）
        private readonly float[][] _outBuf; // チャンネルごとの出力キュー（B）
        // ブロック内の位置。入力の書き込みと出力の読み出しが同じ歩幅で進むので 1 本で足りる。
        private int _pos;

        public int BlockSize => _b;
        public int NumPartitions => _numParts;
        public int TailSamples => _b * _numParts;

        public PartitionedConvolver(int blockSize, int numPartitions, int channels)
        {
            _b = Mathf.NextPowerOfTwo(Mathf.Max(64, blockSize));
            _fftLen = _b * 2;
            _numParts = Mathf.Max(1, numPartitions);
            _channels = Mathf.Max(1, channels);
            _fft = new Fft(_fftLen);

            _fdlRe = new float[_numParts][];
            _fdlIm = new float[_numParts][];
            for (int p = 0; p < _numParts; p++)
            {
                _fdlRe[p] = new float[_fftLen];
                _fdlIm[p] = new float[_fftLen];
            }

            _wRe = new float[_fftLen];
            _wIm = new float[_fftLen];
            _accRe = new float[_fftLen];
            _accIm = new float[_fftLen];
            _inBuf = new float[_fftLen];
            _outBuf = new float[_channels][];
            for (int c = 0; c < _channels; c++) _outBuf[c] = new float[_b];
            // 出力キューは無音で始まる → 最初の 1 ブロックぶんは無音が出る（＝固有遅延 B）。
        }

        // IR を差し替える（main thread から呼ぶ）。ir[channel] は TailSamples 以下の長さ。
        // 実際の切替は次のブロック境界で行われる。
        public void SetIr(float[][] ir)
        {
            if (ir == null || ir.Length < _channels) return;
            var hRe = new float[_channels][][];
            var hIm = new float[_channels][][];
            var re = new float[_fftLen];
            var im = new float[_fftLen];
            int active = 0;
            for (int c = 0; c < _channels; c++)
            {
                hRe[c] = new float[_numParts][];
                hIm[c] = new float[_numParts][];
                for (int p = 0; p < _numParts; p++)
                {
                    System.Array.Clear(re, 0, _fftLen);
                    System.Array.Clear(im, 0, _fftLen);
                    int off = p * _b;
                    int n = Mathf.Min(_b, Mathf.Max(0, (ir[c]?.Length ?? 0) - off));
                    // overlap-save: パーティションは FFT 長の前半に置く。
                    bool any = false;
                    for (int i = 0; i < n; i++)
                    {
                        float v = ir[c][off + i];
                        re[i] = v;
                        if (v != 0f) any = true;
                    }
                    if (any && p + 1 > active) active = p + 1;
                    _fft.Transform(re, im, false);
                    hRe[c][p] = (float[])re.Clone();
                    hIm[c][p] = (float[])im.Clone();
                }
            }
            _pendingActiveParts = Mathf.Max(1, active);
            _pendingHIm = hIm;
            _pendingHRe = hRe;   // これを最後に立てる（audio thread はこれを合図に取り込む）
        }

        // 差し替え待ちも「IRあり」とみなす。_hRe への取り込みは RunBlock の中で起きるが、
        // その RunBlock は ProcessAdd からしか呼ばれない。_hRe だけを見ると
        // 「IRが無いから処理しない → 処理しないから取り込まれない」で永久に始まらない。
        public bool HasIr => _hRe != null || _pendingHRe != null;

        // n サンプル処理する。input[i] が入力、out[c][outOffset + i] に加算していく。
        // 出力は「加算」なので、呼び手は他の成分と混ぜられる。
        public void ProcessAdd(float[] input, int inOffset, int n,
                               float[][] outCh, int outOffset, float gain)
        {
            for (int i = 0; i < n; i++)
            {
                // 出力は「前のブロックで計算済みのぶん」を吐く。
                for (int c = 0; c < _channels; c++)
                    outCh[c][outOffset + i] += _outBuf[c][_pos] * gain;

                // 同じ歩幅で次ブロック用の入力を溜める。
                _inBuf[_b + _pos] = input[inOffset + i];

                if (++_pos >= _b) { RunBlock(); _pos = 0; }
            }
        }

        // 1 ブロック分を周波数領域で畳む。
        private void RunBlock()
        {
            // 保留中の IR があればここで差し替える（ブロック境界＝波形の切れ目が最小）。
            var pRe = System.Threading.Interlocked.Exchange(ref _pendingHRe, null);
            if (pRe != null)
            {
                _hRe = pRe;
                _hIm = System.Threading.Interlocked.Exchange(ref _pendingHIm, null);
                _activeParts = Mathf.Clamp(_pendingActiveParts, 1, _numParts);
            }

            // 入力 2B を FFT して FDL に積む。
            System.Array.Copy(_inBuf, 0, _wRe, 0, _fftLen);
            System.Array.Clear(_wIm, 0, _fftLen);
            _fft.Transform(_wRe, _wIm, false);
            System.Array.Copy(_wRe, _fdlRe[_fdlPos], _fftLen);
            System.Array.Copy(_wIm, _fdlIm[_fdlPos], _fftLen);

            // 次ブロックの「前半」は今回の「後半」。
            System.Array.Copy(_inBuf, _b, _inBuf, 0, _b);

            if (_hRe == null)
            {
                for (int c = 0; c < _channels; c++) System.Array.Clear(_outBuf[c], 0, _b);
                _fdlPos = (_fdlPos + 1) % _numParts;
                return;
            }

            for (int c = 0; c < _channels; c++)
            {
                System.Array.Clear(_accRe, 0, _fftLen);
                System.Array.Clear(_accIm, 0, _fftLen);
                var hReC = _hRe[c];
                var hImC = _hIm[c];
                for (int p = 0; p < _activeParts; p++)
                {
                    int slot = _fdlPos - p;
                    if (slot < 0) slot += _numParts;
                    var xr = _fdlRe[slot]; var xi = _fdlIm[slot];
                    var hr = hReC[p]; var hi = hImC[p];
                    for (int k = 0; k < _fftLen; k++)
                    {
                        _accRe[k] += xr[k] * hr[k] - xi[k] * hi[k];
                        _accIm[k] += xr[k] * hi[k] + xi[k] * hr[k];
                    }
                }
                _fft.Transform(_accRe, _accIm, true);
                // overlap-save: 有効なのは後半 B サンプル（前半は循環畳み込みの巻き込み）。
                System.Array.Copy(_accRe, _b, _outBuf[c], 0, _b);
            }

            _fdlPos = (_fdlPos + 1) % _numParts;
        }
    }
}
