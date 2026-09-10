/* Gpu/compute_d3d11.h ── エンジンが自前で持つ計算デバイス（D3D11 コンピュート）
 *
 * ■ 役割
 *   レイを GPU で解くための土台。**ホスト（Unity）のデバイスは借りない。**
 *   エンジンが自分でデバイスを作り、自分でバッファを持ち、自分で流す。
 *   ★そうする理由: 検査と AfFlowWav が Unity 無しで回せる。この作品の作り方の根っこがそこにある。
 *     借りる形にすると、GPU の道だけ「Unity を開かないと確かめられない」になってしまう。
 *
 * ■ 中の仕組み
 *   1) デバイス: D3D11CreateDevice を機能レベル 11_0 で 1 つ。画面へ出さないので
 *      スワップチェーンもウィンドウも要らない。作れなければ available() が false になり、
 *      呼び出し側は CPU のまま進む（GPU が無い機械でも動く）。
 *   2) バッファ: 読み取り専用の入力は構造化バッファ（SRV）、結果は UAV。
 *      読み戻しは staging バッファへ CopyResource してから Map する。
 *      ★GPU のバッファを直接 Map できないので、必ず staging を挟む。ここを忘れると失敗する。
 *   3) シェーダ: HLSL の文字列をその場で D3DCompile する。ファイルを配らなくて済むので、
 *      DLL 1 つで完結する（ミドルウェアとして配る形に合う）。
 *   4) 同期: Dispatch のあと Map(READ) で待つ。**この版は待つ**。
 *      1 フレーム遅らせて待たない形は、CPU 版と答えを突き合わせてからにする。
 *
 * ■ 繋がり
 *   受ける: 平らな配列（TraceScene の中身）と、レイの設定。
 *   渡す:   レイごとの取り分（RayPartial と同じ並び）。
 *
 * ■ 退けた書き方
 *   ・Unity のコンピュートシェーダを C# から投げる: 音の計算がホスト側の言語に出る。
 *     決めごと（音の計算はエンジンだけ）と、検査が Unity 無しで回せなくなる問題の両方に当たる。
 *   ・D3D12 / Vulkan: デバイスを作るまでの手数が桁で多い。画面へ出さない計算だけなら 11 で足りる。
 *   ・HLSL をファイルで配る: DLL の隣にファイルを置く前提が増える。文字列で持てば DLL 1 つ。
 *
 * ■ 壊れる所
 *   ・staging を挟まずに Map すると必ず失敗する（D3D11_USAGE_DEFAULT は Map できない）。
 *   ・構造化バッファは StructureByteStride を要素の大きさに合わせないと、SRV が作れない。
 *   ・Dispatch のスレッド数はシェーダ側の numthreads と掛け算になる。両方を揃えること。
 */
#ifndef ACOUSTICFLOW_GPU_COMPUTE_D3D11_H
#define ACOUSTICFLOW_GPU_COMPUTE_D3D11_H

#include <cstddef>
#include <string>
#include <vector>

namespace acoustic {
namespace gpu {

/// D3D11 のコンピュートを 1 本流すための最小の器。中身（COM）はヘッダに出さない。
class ComputeDevice {
public:
    ComputeDevice();
    ~ComputeDevice();
    ComputeDevice(const ComputeDevice&) = delete;
    ComputeDevice& operator=(const ComputeDevice&) = delete;

    /// デバイスが作れたか。false ならこの機械では GPU の道は使えない（CPU のまま進む）。
    bool available() const;
    /// 作れなかった理由（診断用）。
    const std::string& error() const;
    /// アダプタの名前（診断用）。
    const std::string& adapterName() const;

    /// HLSL を積む。entry は入口の関数名。失敗したら false（error() に理由）。
    bool setShader(const char* hlsl, const char* entry);

    /// 入力の構造化バッファを置く（slot は t0..t7）。stride は要素 1 個の大きさ。
    bool setInput(int slot, const void* data, std::size_t bytes, std::size_t stride);
    /// 定数（16 バイト境界に合わせた塊をそのまま渡す。b0）。
    bool setConstants(const void* data, std::size_t bytes);
    /// 出力の構造化バッファを確保する（u0）。
    bool setOutput(std::size_t bytes, std::size_t stride);

    /// 流す。groups はスレッド組の数（シェーダの numthreads と掛け算になる）。
    bool dispatch(int groupsX);
    /// 結果を読み戻す（staging 経由）。bytes は setOutput と同じ。
    bool readOutput(void* dst, std::size_t bytes);

private:
    struct Impl;
    Impl* d_;
};

}  // namespace gpu
}  // namespace acoustic

#endif  // ACOUSTICFLOW_GPU_COMPUTE_D3D11_H
