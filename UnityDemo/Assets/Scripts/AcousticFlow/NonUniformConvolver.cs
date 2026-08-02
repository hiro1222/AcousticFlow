// NonUniformConvolver.cs
// 非一様分割による畳み込み（Gardner 1995 "Efficient Convolution Without Input-Output Delay"）。
//
// なぜ必要か:
//   一様分割 overlap-save は構造上 1ブロック(B)ぶん遅れる。この遅延は「尾の開始時刻
//   （mixing time）」の中に隠す必要があるので、B < mixing time が制約になる。
//   mixing time ≒ √V(ms) なので、6畳(24m³)なら 4.9ms＝約235サンプル。
//   一様分割で B をそこまで小さくすると、1秒の尾に 750 パーティションも要って現実的でない。
//   その妥協として境目に下限(minSplitMs=25ms)を置いていたが、そのせいで
//   「本来は拡散として扱うべき反射」を少数の離散タップで表現することになり、
//   狭い部屋でコムフィルタ（箱っぽく高い音）が立っていた。
//     体積 625m³(約8.5m立方)未満は全部この制約に掛かる＝普通の部屋はほぼ全滅だった。
//
// 解決の要点:
//   各段は IR の一部分だけを担当する。段 k が担当するのは irOffset_k から始まる区間なので、
//   その段の出力はもともと irOffset_k サンプル遅れてよい。
//   一様分割器の持つ遅延 B_k は、この「もともと必要な遅れ」に吸収できる。
//     追加で入れる遅延 = irOffset_k + B_0 - B_k  （倍々＋各段2個なら常に非負）
//   結果、全体の遅延は最小ブロック B_0 だけで決まる（64サンプル = 1.3ms）。
//
//   48kHz / 1秒の尾での比較:
//     一様 B=1024 … 遅延 21.3ms / 47パーティション
//     非一様      … 遅延  1.3ms / 18パーティション
//   パーティションが減るぶんと小サイズFFTが増えるぶんが相殺し、
//   コストはほぼ同じで遅延だけ 1/16 になる。
//
// 実装方針:
//   PartitionedConvolver（一様分割・検証済み）を段ごとに持ち、出力を遅延して足す。
//   ゼロから書き直すより、テスト済みの部品を組み合わせる方が安全で読みやすい。
using System.Collections.Generic;
using UnityEngine;

namespace AcousticFlow
{
    public sealed class NonUniformConvolver
    {
        private sealed class Stage
        {
            public PartitionedConvolver conv;
            public int irOffset;     // 担当する IR の開始位置(サンプル)
            public int length;       // 担当する長さ
            public int extraDelay;   // 出力に足す遅延 = irOffset + B0 - blockSize
            public int blockSize;
            public int numParts;
        }

        private readonly Stage[] _stages;
        private readonly int _channels;
        private readonly int _latency;      // 全体の遅延 = 最小ブロック

        // 段の出力を集める共有リング。読み出したらゼロに戻す。
        private readonly float[][] _accRing;
        private readonly int _ringMask;
        private int _readPos;

        // 段ごとの一時出力（毎フレーム確保しないよう使い回す）。
        private float[][] _scratch;
        private int _scratchFrames;

        private bool _hasIr;

        public int Latency => _latency;
        public int StageCount => _stages.Length;
        public bool HasIr => _hasIr;
        public int TotalPartitions
        {
            get { int n = 0; foreach (var s in _stages) n += s.numParts; return n; }
        }

        /// irLength   : 畳み込む IR の長さ(サンプル)
        /// firstBlock : 最小ブロック。これが全体の遅延になる（小さいほど低遅延・高コスト）
        /// capBlock   : 最大ブロック。これ以上は大きくせず、残りを同サイズで埋める
        public NonUniformConvolver(int irLength, int channels,
                                   int firstBlock = 64, int capBlock = 8192)
        {
            _channels = Mathf.Max(1, channels);
            firstBlock = Mathf.NextPowerOfTwo(Mathf.Max(16, firstBlock));
            capBlock = Mathf.NextPowerOfTwo(Mathf.Max(firstBlock, capBlock));
            irLength = Mathf.Max(1, irLength);
            _latency = firstBlock;

            // ── 分割スケジュールを組む ──
            // ブロックを倍々にしながら各段 2 パーティション。上限に達したら残りを埋める。
            //   各段2個にするのは「追加遅延 irOffset + B0 - B が非負」を満たすため。
            //   1個だと次の段のブロックが開始位置を超えてしまい、遅延を隠せない。
            var defs = new List<Stage>();
            int b = firstBlock;
            int offset = 0;
            while (offset < irLength)
            {
                int remaining = irLength - offset;
                int parts = (b >= capBlock) ? Mathf.CeilToInt(remaining / (float)b) : 2;
                if (b < capBlock && b * parts >= remaining)
                    parts = Mathf.CeilToInt(remaining / (float)b);
                parts = Mathf.Max(1, parts);

                defs.Add(new Stage
                {
                    blockSize = b,
                    numParts = parts,
                    irOffset = offset,
                    length = b * parts,
                    extraDelay = offset + firstBlock - b,
                });
                offset += b * parts;
                if (b < capBlock) b *= 2;
            }
            _stages = defs.ToArray();

            foreach (var s in _stages)
            {
                s.conv = new PartitionedConvolver(s.blockSize, s.numParts, _channels);
                if (s.extraDelay < 0)
                    Debug.LogError($"[NonUniformConvolver] 追加遅延が負: offset={s.irOffset} B={s.blockSize}。"
                                   + "スケジュールが制約を満たしていない。");
            }

            // 集約リングは「最大の追加遅延 + 1回の処理長」を収められる大きさに。
            int maxDelay = 0;
            foreach (var s in _stages) maxDelay = Mathf.Max(maxDelay, s.extraDelay);
            int ringSize = Mathf.NextPowerOfTwo(maxDelay + 4096);
            _accRing = new float[_channels][];
            for (int c = 0; c < _channels; c++) _accRing[c] = new float[ringSize];
            _ringMask = ringSize - 1;
        }

        /// IR を差し替える（main thread）。ir[ch][sample]。段ごとに切り出して渡す。
        public void SetIr(float[][] ir)
        {
            if (ir == null || ir.Length < _channels) return;
            foreach (var s in _stages)
            {
                var slice = new float[_channels][];
                for (int c = 0; c < _channels; c++)
                {
                    slice[c] = new float[s.length];
                    var src = ir[c];
                    if (src == null) continue;
                    int n = Mathf.Min(s.length, Mathf.Max(0, src.Length - s.irOffset));
                    for (int i = 0; i < n; i++) slice[c][i] = src[s.irOffset + i];
                }
                s.conv.SetIr(slice);
            }
            _hasIr = true;
        }

        /// audio thread：モノラル入力を畳み込み、outCh に加算する。
        public void ProcessAdd(float[] input, int inOffset, int n,
                               float[][] outCh, int outOffset, float gain)
        {
            if (input == null || outCh == null || n <= 0) return;
            EnsureScratch(n);

            // 各段を回し、それぞれの追加遅延ぶんずらして集約リングへ足す。
            foreach (var s in _stages)
            {
                for (int c = 0; c < _channels; c++) System.Array.Clear(_scratch[c], 0, n);
                s.conv.ProcessAdd(input, inOffset, n, _scratch, 0, 1f);

                for (int c = 0; c < _channels; c++)
                {
                    var ring = _accRing[c];
                    var sc = _scratch[c];
                    int d = s.extraDelay;
                    for (int i = 0; i < n; i++)
                        ring[(_readPos + i + d) & _ringMask] += sc[i];
                }
            }

            // 集約リングから読み出して出力へ。読んだ場所はゼロに戻す
            // （次にこの位置へ書かれるまでに必ずクリアされている必要がある）。
            for (int c = 0; c < _channels; c++)
            {
                var ring = _accRing[c];
                var dst = outCh[c];
                for (int i = 0; i < n; i++)
                {
                    int p = (_readPos + i) & _ringMask;
                    dst[outOffset + i] += ring[p] * gain;
                    ring[p] = 0f;
                }
            }
            _readPos = (_readPos + n) & _ringMask;
        }

        private void EnsureScratch(int frames)
        {
            if (_scratchFrames >= frames) return;
            _scratch = new float[_channels][];
            for (int c = 0; c < _channels; c++) _scratch[c] = new float[frames];
            _scratchFrames = frames;
        }

        /// 分割の内訳を1行で返す（診断表示用）。
        public string DescribeSchedule()
        {
            var sb = new System.Text.StringBuilder();
            sb.Append($"遅延 {_latency}samp / 段 {_stages.Length} / 計 {TotalPartitions}分割: ");
            foreach (var s in _stages) sb.Append($"[{s.blockSize}x{s.numParts}]");
            return sb.ToString();
        }
    }
}
