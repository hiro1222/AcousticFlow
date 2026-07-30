#!/usr/bin/env python3
"""SOFA 形式の HRTF を AcousticFlow の .afhr 形式へ変換する。

なぜ変換するか:
    SOFA は NetCDF4(HDF5) ベースで、C# から読むには重い依存が要る。
    ランタイムは「方向 → HRIR」が引ければ十分なので、平坦なバイナリに落として同梱する。
    変換はオフラインで一度だけ行う。

使い方:
    pip install netCDF4 numpy scipy
    python sofa_to_afhr.py input.sofa output.afhr [--rate 48000]

出力形式 (.afhr, little-endian):
    magic   : uint32  'AFHR'
    version : int32   1
    rate    : int32   サンプリング周波数
    ndir    : int32   測定方向数
    irlen   : int32   HRIR 長(サンプル)
    方向    : float32 × 2 × ndir   (azimuth[deg], elevation[deg])
              az: 0=正面, +90=右, -90=左 / el: 0=水平, +90=真上
    データ  : float32 × irlen × 2 × ndir   (left[irlen], right[irlen]) の順

注意:
    ・データセットのライセンスを必ず確認すること。再配布可否は提供元によって異なる。
      公開リポジトリに同梱する場合は特に。
    ・出力サンプリング周波数はゲームの出力(通常48kHz)に合わせる。
      ずれていると ITD がずれて定位が狂う。
"""
import argparse
import struct
import sys

import numpy as np

MAGIC = 0x52484641  # 'A','F','H','R'
VERSION = 1


def load_sofa(path):
    try:
        from netCDF4 import Dataset
    except ImportError:
        sys.exit("netCDF4 が必要です:  pip install netCDF4")

    ds = Dataset(path, "r")
    conv = getattr(ds, "SOFAConventions", "")
    if "SimpleFreeFieldHRIR" not in conv:
        print(f"[警告] 想定外の SOFAConventions='{conv}'。SimpleFreeFieldHRIR 以外は"
              f"座標系の解釈が違う可能性があります。", file=sys.stderr)

    ir = np.array(ds.variables["Data.IR"][:])          # (M, R, N)
    rate = float(np.array(ds.variables["Data.SamplingRate"][:]).flat[0])
    pos = np.array(ds.variables["SourcePosition"][:])  # (M, C)
    pos_type = getattr(ds.variables["SourcePosition"], "Type", "spherical")
    ds.close()

    if ir.ndim != 3 or ir.shape[1] < 2:
        sys.exit(f"想定外の Data.IR 形状: {ir.shape}（(M,2,N) を期待）")

    if pos_type.lower().startswith("cart"):
        x, y, z = pos[:, 0], pos[:, 1], pos[:, 2]
        az = np.degrees(np.arctan2(y, x))
        el = np.degrees(np.arctan2(z, np.sqrt(x * x + y * y)))
    else:
        az, el = pos[:, 0], pos[:, 1]

    # SOFA は az: 反時計回り(+が左)。AcousticFlow は +が右なので反転する。
    az = -az
    az = (az + 180.0) % 360.0 - 180.0
    return ir.astype(np.float64), rate, az.astype(np.float64), el.astype(np.float64)


def resample(ir, src_rate, dst_rate):
    if abs(src_rate - dst_rate) < 1.0:
        return ir, src_rate
    try:
        from scipy.signal import resample_poly
    except ImportError:
        sys.exit("リサンプリングに scipy が必要です:  pip install scipy")
    from math import gcd
    g = gcd(int(src_rate), int(dst_rate))
    up, down = int(dst_rate) // g, int(src_rate) // g
    out = resample_poly(ir, up, down, axis=-1)
    print(f"  リサンプル {src_rate:.0f} → {dst_rate:.0f} Hz "
          f"(IR長 {ir.shape[-1]} → {out.shape[-1]})")
    return out, float(dst_rate)


def write_afhr(path, ir, rate, az, el):
    m, _, n = ir.shape
    with open(path, "wb") as f:
        f.write(struct.pack("<IiiiI", MAGIC, VERSION, int(rate), m, n))
        for i in range(m):
            f.write(struct.pack("<ff", float(az[i]), float(el[i])))
        for i in range(m):
            f.write(ir[i, 0].astype("<f4").tobytes())
            f.write(ir[i, 1].astype("<f4").tobytes())


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input")
    ap.add_argument("output")
    ap.add_argument("--rate", type=int, default=48000,
                    help="出力サンプリング周波数（既定 48000。ゲームの出力に合わせる）")
    ap.add_argument("--trim", type=int, default=0,
                    help="HRIR をこの長さに切り詰める（0=切らない）。"
                         "長いほど重いので、128〜256 程度に落とすと実用的")
    args = ap.parse_args()

    print(f"読み込み: {args.input}")
    ir, rate, az, el = load_sofa(args.input)
    print(f"  {ir.shape[0]} 方向 / IR長 {ir.shape[2]} / {rate:.0f} Hz")

    ir, rate = resample(ir, rate, args.rate)

    if args.trim > 0 and ir.shape[2] > args.trim:
        # 立ち上がりを保つため、最もエネルギーの早い位置から切り出す。
        onset = int(np.argmax(np.abs(ir).mean(axis=(0, 1)) > 0.05 * np.abs(ir).max()))
        start = max(0, onset - 8)
        ir = ir[:, :, start:start + args.trim]
        print(f"  切り詰め: {args.trim} タップ (開始 {start})")

    # 正規化（全体のピークを 1 に）。データセット間の音量差をならす。
    peak = np.abs(ir).max()
    if peak > 0:
        ir = ir / peak
        print(f"  正規化: ピーク {peak:.4g} → 1.0")

    write_afhr(args.output, ir, rate, az, el)
    print(f"書き出し: {args.output}  "
          f"({ir.shape[0]} 方向 × {ir.shape[2]} タップ × 2ch)")
    print("\nUnity 側の使い方:")
    print("  1) 出力を UnityDemo/Assets/StreamingAssets/ に置く")
    print("  2) IrConvolver の Hrtf File Name にファイル名を入れる")


if __name__ == "__main__":
    main()
