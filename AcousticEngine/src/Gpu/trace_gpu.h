/* Gpu/trace_gpu.h ── TraceScene を GPU へ載せてレイを解く
 *
 * ■ 役割
 *   CPU の EnergyTrace::run と**同じ入力**（TraceScene）を受けて、同じ形の TraceResult を返す。
 *   中で ComputeDevice を回す。呼び出し側から見た形は CPU 版とまったく同じにしてある。
 *   ★そうしておかないと「GPU にしたら音が変わった」のか「呼び方が違う」のか切り分けられない。
 *
 * ■ 中の仕組み
 *   1) 場面を GPU の並びへ詰め直す（Pack*）。C++ の struct と HLSL の struct を**1 バイトも違わず**揃える。
 *      ここがずれると全部が化ける。だから両方を隣に書いて、同じ順で並べてある。
 *   2) レイごとの取り分（RayPartial と同じ中身）を UAV に書かせ、CPU で**本の順に**足す。
 *      GPU の中で足し込まない理由: 足す順が決まらないと答えが揺れる。順は CPU が決める。
 *   3) 直接音は GPU に載せない。決定的に出せる物を GPU へ持っていく理由が無い（CPU で出す）。
 *
 * ■ ビット一致しないこと
 *   丸めも sin/cos も違うので CPU と一致しない。守るのは
 *   「静止していれば毎フレーム同じ」「保存則」「CPU と dB で近い」の 3 つ。
 *
 * ■ 壊れる所
 *   ・HLSL の struct と下の Pack* の並びがずれる（いちばん多い事故）。
 *   ・numthreads(64) と組数の掛け算がレイの本数を下回ると、後ろの本が静かに欠ける。
 *   ・場面を作り直したのに転送し直さないと、古い幾何で解く。毎回 upload する。
 */
#ifndef ACOUSTICFLOW_GPU_TRACE_GPU_H
#define ACOUSTICFLOW_GPU_TRACE_GPU_H

#include <cstdint>
#include <vector>

#include "Flow/energy_trace.h"
#include "Flow/trace_scene.h"
#include "Gpu/compute_d3d11.h"
#include "Gpu/trace_kernel.h"

namespace acoustic {
namespace gpu {

// ── HLSL の struct と 1 対 1（並びを変えないこと）──
struct PackObb  { float c[3]; float he[3]; float ax[3]; float ay[3]; float az[3]; std::int32_t material; std::int32_t active; };
struct PackNode { float bmin[3]; float bmax[3]; std::int32_t left, right, first, count; };
struct PackItem { std::int32_t box, pad0, pad1, pad2; };
struct PackMat  { float reflLo[3]; float reflHi[3]; float tranLo[3]; float tranHi[3];
                  float absbLo[3]; float absbHi[3]; float scat; float pad[3]; };
struct PackOut  { float earlyLo[3]; float earlyHi[3]; float lateLo[3]; float lateHi[3];
                  float emitLo[3];  float emitHi[3];  float absbLo[3]; float absbHi[3];
                  float remLo[3];   float remHi[3];   float escLo[3];  float escHi[3];
                  float firstSec; std::int32_t hits, nee, pad; };
struct PackCb   { float source[3]; float e0; float listener[3]; float mixingSec;
                  std::uint32_t rays, maxBounces, seed, boxCount;
                  std::uint32_t nodeCount, groups, group, itemCount; };

/// GPU でレイを解く器。場面が変わるたびに upload、レイのたびに run。
class GpuTracer {
public:
    bool available() const { return dev_.available() && ready_; }
    const std::string& error() const { return err_.empty() ? dev_.error() : err_; }
    const std::string& adapterName() const { return dev_.adapterName(); }

    /// シェーダを積む（1 回でよい）。
    bool init() {
        if (!dev_.available()) { err_ = dev_.error(); return false; }
        if (!dev_.setShader(kTraceKernelHlsl(), "main")) { err_ = dev_.error(); return false; }
        ready_ = true;
        return true;
    }

    /// 場面を GPU へ送る（1 フレームに 1 回）。
    bool upload(const flow::TraceScene& sc) {
        if (!available()) return false;
        const int n = sc.boxCount();
        obb_.resize(static_cast<std::size_t>(n));
        for (int i = 0; i < n; ++i) {
            const acoustic::Obb& b = sc.obb[static_cast<std::size_t>(i)];
            PackObb& p = obb_[static_cast<std::size_t>(i)];
            p.c[0]  = b.center.x;  p.c[1]  = b.center.y;  p.c[2]  = b.center.z;
            p.he[0] = b.halfExtents.x; p.he[1] = b.halfExtents.y; p.he[2] = b.halfExtents.z;
            p.ax[0] = b.axisX.x; p.ax[1] = b.axisX.y; p.ax[2] = b.axisX.z;
            p.ay[0] = b.axisY.x; p.ay[1] = b.axisY.y; p.ay[2] = b.axisY.z;
            p.az[0] = b.axisZ.x; p.az[1] = b.axisZ.y; p.az[2] = b.axisZ.z;
            p.material = sc.material[static_cast<std::size_t>(i)];
            p.active = sc.active[static_cast<std::size_t>(i)] ? 1 : 0;
        }
        node_.resize(sc.node.size());
        for (std::size_t k = 0; k < sc.node.size(); ++k) {
            const flow::SurfaceBvh::Node& s = sc.node[k];
            PackNode& p = node_[k];
            p.bmin[0] = s.bounds.min.x; p.bmin[1] = s.bounds.min.y; p.bmin[2] = s.bounds.min.z;
            p.bmax[0] = s.bounds.max.x; p.bmax[1] = s.bounds.max.y; p.bmax[2] = s.bounds.max.z;
            p.left = s.left; p.right = s.right; p.first = s.first; p.count = s.count;
        }
        item_.resize(sc.item.size());
        for (std::size_t k = 0; k < sc.item.size(); ++k) { item_[k] = PackItem{sc.item[k], 0, 0, 0}; }
        const int nm = static_cast<int>(sc.scatter1k.size());
        mat_.resize(static_cast<std::size_t>(nm));
        for (int m = 0; m < nm; ++m) {
            PackMat& p = mat_[static_cast<std::size_t>(m)];
            for (int b = 0; b < acoustic::kNumBands; ++b) {
                const flow::SurfaceSplit& s = sc.split[static_cast<std::size_t>(m) * acoustic::kNumBands + b];
                float* rr = (b < 3) ? p.reflLo : p.reflHi;
                float* tt = (b < 3) ? p.tranLo : p.tranHi;
                float* aa = (b < 3) ? p.absbLo : p.absbHi;
                rr[b % 3] = s.reflect; tt[b % 3] = s.transmit; aa[b % 3] = s.absorb;
            }
            p.scat = sc.scatter1k[static_cast<std::size_t>(m)];
            p.pad[0] = p.pad[1] = p.pad[2] = 0.0f;
        }
        if (obb_.empty() || node_.empty() || item_.empty() || mat_.empty()) { err_ = "場面が空"; return false; }
        bool ok = dev_.setInput(0, obb_.data(), obb_.size() * sizeof(PackObb), sizeof(PackObb));
        ok = ok && dev_.setInput(1, node_.data(), node_.size() * sizeof(PackNode), sizeof(PackNode));
        ok = ok && dev_.setInput(2, item_.data(), item_.size() * sizeof(PackItem), sizeof(PackItem));
        ok = ok && dev_.setInput(3, mat_.data(), mat_.size() * sizeof(PackMat), sizeof(PackMat));
        if (!ok) err_ = dev_.error();
        return ok;
    }

    /// 解く。直接音は CPU 側で出すので、ここは反射だけ（TraceResult の反射の欄を埋める）。
    bool run(const flow::TraceScene& sc, const Vec3& source, const Vec3& listener,
             const flow::TraceParams& prm, flow::TraceResult& R) {
        if (!available()) return false;
        const int N = std::max(1, prm.rays);
        PackCb cb{};
        cb.source[0] = source.x; cb.source[1] = source.y; cb.source[2] = source.z;
        cb.listener[0] = listener.x; cb.listener[1] = listener.y; cb.listener[2] = listener.z;
        cb.e0 = 1.0f / static_cast<float>(N);
        cb.mixingSec = prm.mixingSec;
        cb.rays = static_cast<std::uint32_t>(N);
        cb.maxBounces = static_cast<std::uint32_t>(std::max(1, prm.maxBounces));
        cb.seed = prm.seed;
        cb.boxCount = static_cast<std::uint32_t>(sc.boxCount());
        cb.nodeCount = static_cast<std::uint32_t>(sc.node.size());
        cb.groups = static_cast<std::uint32_t>(std::max(1, prm.groups));
        cb.group = static_cast<std::uint32_t>(((prm.group % std::max(1, prm.groups)) + std::max(1, prm.groups)) % std::max(1, prm.groups));
        cb.itemCount = static_cast<std::uint32_t>(sc.item.size());
        if (!dev_.setConstants(&cb, sizeof(cb))) { err_ = dev_.error(); return false; }
        out_.assign(static_cast<std::size_t>(N), PackOut{});
        if (!dev_.setOutput(out_.size() * sizeof(PackOut), sizeof(PackOut))) { err_ = dev_.error(); return false; }
        if (!dev_.dispatch((N + 63) / 64)) { err_ = dev_.error(); return false; }
        if (!dev_.readOutput(out_.data(), out_.size() * sizeof(PackOut))) { err_ = dev_.error(); return false; }
        // ★足すのは CPU。本の順に足すので、答えは実行のたびに変わらない。
        for (int i = 0; i < N; ++i) {
            const PackOut& p = out_[static_cast<std::size_t>(i)];
            if (p.hits == 0 && p.emitLo[0] == 0.0f) continue;      // その組で走らなかった本
            ++R.raysTraced;
            R.hits += p.hits; R.neeVisible += p.nee;
            for (int b = 0; b < acoustic::kNumBands; ++b) {
                const int k = b % 3;
                R.early6[b] += (b < 3) ? p.earlyLo[k] : p.earlyHi[k];
                R.late6[b]  += (b < 3) ? p.lateLo[k]  : p.lateHi[k];
                R.emitted6[b]   += (b < 3) ? p.emitLo[k] : p.emitHi[k];
                R.absorbed6[b]  += (b < 3) ? p.absbLo[k] : p.absbHi[k];
                R.remainder6[b] += (b < 3) ? p.remLo[k]  : p.remHi[k];
                R.escaped6[b]   += (b < 3) ? p.escLo[k]  : p.escHi[k];
            }
            if (p.firstSec > 0.0f && (R.firstReflectSec < 0.0f || p.firstSec < R.firstReflectSec))
                R.firstReflectSec = p.firstSec;
        }
        return true;
    }

private:
    ComputeDevice dev_;
    bool ready_ = false;
    std::string err_;
    std::vector<PackObb>  obb_;
    std::vector<PackNode> node_;
    std::vector<PackItem> item_;
    std::vector<PackMat>  mat_;
    std::vector<PackOut>  out_;
};

}  // namespace gpu
}  // namespace acoustic

#endif  // ACOUSTICFLOW_GPU_TRACE_GPU_H
