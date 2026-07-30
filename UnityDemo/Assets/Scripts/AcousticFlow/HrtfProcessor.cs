// HrtfProcessor.cs
// モノラル信号を、指定方向から聞こえるバイノーラル(L/R)に変換する。
//
// 構成:
//   入力 → [ITD 遅延: 耳ごと・小数サンプル精度] → [HRIR 畳み込み: 耳ごと] → L/R
//
// ITD を畳み込みと分けている理由は HrtfSet の冒頭コメントを参照（個人最適化のため）。
//
// 方向が変わったときのクリック対策:
//   HRIR を差し替えると波形が不連続になりプチッと鳴る。新旧2本を同時に畳み込んで
//   クロスフェードする（IrConvolver の IR 切替と同じ方式）。フェード中だけ倍のコスト。
//
// コストの見積り（HRIR 128 タップ / 48kHz）:
//   通常時 128×2ch = 256 MAC/sample、フェード中はその倍。
//   早期反射までHRTF化すると タップ数×これ になるため、まず直接音だけに適用する。
//   反射は先行音効果でほとんど定位に寄与しないので、費用対効果が悪い。
using UnityEngine;

namespace AcousticFlow
{
    public sealed class HrtfProcessor
    {
        private HrtfSet _set;
        private int _sampleRate;

        // 入力履歴。ITD 遅延は「畳み込みの読み出し位置」に折り込むので、
        // 耳ごとの遅延後バッファは持たない（持つとクロスフェード中に
        // 新旧が同じバッファを奪い合って壊れる）。
        //
        // 低域と高域で別のリングを持つ理由（重要）:
        //   実測 HRIR は低域を正しく持っていない。128タップ(2.9ms)では約350Hz以下を
        //   表現できず、測定スピーカーの低域ロールオフも含まれる。実測すると
        //   直流利得は約 0.15(-16dB)で、広帯域RMS(約1.0)に対して大きく欠けている。
        //   そのまま畳み込むと低音が痩せる（「音が軽くなる」）。
        //   物理的にも、低域は波長が頭より遥かに長いので頭を回り込み、減衰しない。
        //   低域の定位手がかりは ITD だけで、耳介も頭部の影も効かない。
        //   → 低域は HRIR を通さず ITD だけ適用し、高域だけ HRTF で畳み込む。
        private float[] _srcRing;     // 高域（HRIR と畳み込む）
        private float[] _lowRing;     // 低域（ITD 遅延だけ適用）
        private int _srcMask;
        private int _srcWritePos;
        private float _lpState;       // 分割用の一次ローパス状態
        private float _lpCoef;        // 分割周波数から決まる係数

        // 現在/次の HRIR とクロスフェード状態。audio thread が読む。
        private float[] _curL, _curR;
        private float[] _nextL, _nextR;
        private volatile bool _hasPending;
        private float[] _pendL, _pendR;
        private float _pendDelayL, _pendDelayR;
        private bool _fading;
        private int _xfadePos, _xfadeLen;

        // ITD 由来の遅延（サンプル。小数可）。耳ごと。
        private float _delayL, _delayR;
        private float _delayLNext, _delayRNext;

        private int _irLen;

        public bool IsReady => _set != null && _set.IsValid && _curL != null;
        public string SetName => _set?.Name ?? "(none)";
        public int CurrentIrLength => _irLen;

        public HrtfProcessor(int sampleRate, float crossfadeMs = 12f, float crossoverHz = 700f)
        {
            _sampleRate = Mathf.Max(8000, sampleRate);
            _xfadeLen = Mathf.Max(1, Mathf.RoundToInt(crossfadeMs * 0.001f * _sampleRate));
            SetCrossover(crossoverHz);
        }

        /// 低域/高域の分割周波数。これより下は HRIR を通さず ITD だけ適用する。
        ///   一次(6dB/oct)の緩い分割にしているのは、低域と高域の位相の食い違いを小さく保つため。
        ///   低域 = LP(x)、高域 = x - LP(x) なので、足すと必ず元に戻る（完全再構成）。
        public void SetCrossover(float hz)
        {
            hz = Mathf.Clamp(hz, 100f, 4000f);
            _lpCoef = Mathf.Clamp01(2f * Mathf.PI * hz / _sampleRate);
        }

        /// データセットを差し替える（main thread）。
        public void SetHrtfSet(HrtfSet set)
        {
            _set = set;
            if (set == null || !set.IsValid) { _curL = null; return; }

            _irLen = set.IrLength;
            // 履歴は「HRIR長 + 最大ITD遅延」ぶん必要。ITD は最大 1ms 程度だが余裕を見て 8ms。
            int srcNeed = Mathf.NextPowerOfTwo(_irLen + Mathf.RoundToInt(0.008f * _sampleRate) + 8);
            if (_srcRing == null || _srcRing.Length < srcNeed)
            {
                _srcRing = new float[srcNeed];
                _lowRing = new float[srcNeed];
                _srcMask = srcNeed - 1;
            }
            // 初期 HRIR（正面）を入れておく。
            int idx = set.NearestIndex(Vector3.forward);
            _curL = set.GetHrir(idx, 0);
            _curR = set.GetHrir(idx, 1);
            _delayL = _delayR = 0f;
            _fading = false;
            _hasPending = false;
        }

        /// 到来方向（リスナー座標系: +x=右, +y=上, +z=前）と頭囲から HRIR/ITD を選ぶ（main thread）。
        /// 実際の切替は次のオーディオブロックでクロスフェードして行われる。
        public void SetDirection(Vector3 dirListenerLocal, float headCircumferenceCm)
        {
            if (_set == null || !_set.IsValid) return;
            int idx = _set.NearestIndex(dirListenerLocal);
            if (idx < 0) return;

            float itd = _set.GetItdSecondsScaled(idx, headCircumferenceCm);
            // 正=右耳が遅い。負の遅延は作れないので、遅い側だけを遅らせる。
            float dl = (itd < 0f) ? -itd * _sampleRate : 0f;
            float dr = (itd > 0f) ? itd * _sampleRate : 0f;

            _pendL = _set.GetHrir(idx, 0);
            _pendR = _set.GetHrir(idx, 1);
            _pendDelayL = dl;
            _pendDelayR = dr;
            _hasPending = true;
        }

        /// audio thread：オーディオブロックの先頭で1回呼ぶ。保留中の HRIR を取り込む。
        /// （サンプル単位で使う場合も、ブロック境界でこれを呼ぶこと）
        public void BeginBlock()
        {
            if (!_hasPending) return;
            _hasPending = false;
            // フェード中に更に新しいものが来たら、進行中の「次」を「現在」に確定させてから差し替える。
            if (_fading) { _curL = _nextL; _curR = _nextR; _delayL = _delayLNext; _delayR = _delayRNext; }
            _nextL = _pendL; _nextR = _pendR;
            _delayLNext = _pendDelayL; _delayRNext = _pendDelayR;
            _xfadePos = 0;
            _fading = true;
        }

        /// audio thread：1サンプルをバイノーラル化する。
        /// IrConvolver のサンプルループ内から直接音タップに適用するため、
        /// ブロック単位ではなくサンプル単位の入口を用意している。
        public void ProcessSample(float x, out float outLs, out float outRs)
        {
            if (!IsReady) { outLs = 0f; outRs = 0f; return; }

            // 低域/高域に分ける。低域は HRIR を通さず ITD だけ（フィールド宣言のコメント参照）。
            _lpState += _lpCoef * (x - _lpState);
            int wp = _srcWritePos & _srcMask;
            _lowRing[wp] = _lpState;
            _srcRing[wp] = x - _lpState;

            if (_fading)
            {
                float t = (float)_xfadePos / _xfadeLen;
                float aL = Convolve(_curL, _delayL), aR = Convolve(_curR, _delayR);
                float bL = Convolve(_nextL, _delayLNext), bR = Convolve(_nextR, _delayRNext);
                outLs = aL * (1f - t) + bL * t;
                outRs = aR * (1f - t) + bR * t;
                // 低域も遅延だけはクロスフェードする（ITD が急に飛ぶとクリックになる）。
                outLs += ReadLow(_delayL) * (1f - t) + ReadLow(_delayLNext) * t;
                outRs += ReadLow(_delayR) * (1f - t) + ReadLow(_delayRNext) * t;
                if (++_xfadePos >= _xfadeLen)
                {
                    _fading = false;
                    _curL = _nextL; _curR = _nextR;
                    _delayL = _delayLNext; _delayR = _delayRNext;
                }
            }
            else
            {
                outLs = Convolve(_curL, _delayL) + ReadLow(_delayL);
                outRs = Convolve(_curR, _delayR) + ReadLow(_delayR);
            }
            _srcWritePos++;
        }

        // 低域を ITD 遅延つきで読む（畳み込みはしない）。
        //   低域は頭を回り込むので両耳とも減衰しない＝利得 1.0 のまま通す。
        private float ReadLow(float delay)
        {
            int d0 = (int)delay;
            float frac = delay - d0;
            int baseIdx = _srcWritePos - d0;
            float s0 = _lowRing[baseIdx & _srcMask];
            if (frac <= 1e-6f) return s0;
            float s1 = _lowRing[(baseIdx - 1) & _srcMask];
            return s0 + frac * (s1 - s0);
        }

        /// audio thread：モノラル入力を畳み込み、outL/outR に加算する（ブロック単位）。
        public void ProcessAdd(float[] input, int inOffset, int n,
                               float[] outL, float[] outR, int outOffset, float gain)
        {
            if (!IsReady || input == null || outL == null || outR == null) return;
            BeginBlock();
            for (int i = 0; i < n; i++)
            {
                ProcessSample(input[inOffset + i], out float l, out float r);
                outL[outOffset + i] += l * gain;
                outR[outOffset + i] += r * gain;
            }
        }

        // 入力履歴を「delay サンプル前」から読みつつ HRIR と畳み込む。
        //   遅延を読み出し位置に折り込むので、耳ごとの中間バッファが要らない。
        //   ＝クロスフェード中に新旧が別の遅延で同時に走っても干渉しない。
        //   遅延は小数サンプル。ITD の弁別限は 10〜20µs で 48kHz の 1 サンプル未満なので、
        //   整数に丸めると定位が粗くなる。よってタップごとに線形補間する。
        private float Convolve(float[] h, float delay)
        {
            if (h == null) return 0f;
            int d0 = (int)delay;
            float frac = delay - d0;
            int baseIdx = _srcWritePos - d0;
            int len = Mathf.Min(_irLen, h.Length);
            float s = 0f;
            if (frac <= 1e-6f)
            {
                for (int k = 0; k < len; k++)
                    s += h[k] * _srcRing[(baseIdx - k) & _srcMask];
            }
            else
            {
                for (int k = 0; k < len; k++)
                {
                    float s0 = _srcRing[(baseIdx - k) & _srcMask];
                    float s1 = _srcRing[(baseIdx - k - 1) & _srcMask];
                    s += h[k] * (s0 + frac * (s1 - s0));
                }
            }
            return s;
        }
    }
}
