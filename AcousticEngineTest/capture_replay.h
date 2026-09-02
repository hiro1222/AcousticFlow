/* capture_replay.h ── 録った**入力**を押し直して、いまのエンジンに解き直させる。
 *
 * ★この道具の前提そのもの:
 *   エンジンは毎フレーム、ホストから
 *     動いた実体の transform → リスナー位置 → 音源位置 → Update
 *   を押されて動く。**その順で押し直せば再現になる**（docs/SOUND_DEBUG_TOOL.md §1 判断1）。
 *   だからイベントログからの再合成も RNG の記録も要らない。
 *
 * ⚠ 形は再現しない。キャプチャに入っているのは**動いた実体だけ**で、静的な形は入っていない
 *   （本番ステージは 8,102 三角形のメッシュで、入れたら容量が破裂する）。
 *   → 呼び手が**同じ形のシーンを作ってから**渡すこと。ヘッダの識別子で照合する。
 */
#ifndef ACOUSTICFLOW_TEST_CAPTURE_REPLAY_H
#define ACOUSTICFLOW_TEST_CAPTURE_REPLAY_H

#include <cmath>
#include <vector>

#include "../AcousticEngine/include/acoustic_scene.h"
#include "../AcousticEngine/src/Debug/capture.h"

namespace af {
namespace replay {

using acoustic::dbg::CaptureFile;

struct FrameDiff {
    int frame = 0;
    float worstRel = 0.0f;       // そのフレームの帯域ゲインの最大相対差
    int exactMismatches = 0;     // ビット一致しなかった要素の数（帯域ゲイン）
    // ★尾は別に見る。帯域ゲインは毎フレーム作り直すので状態を持たないが、
    //   エコグラムは**フレームをまたいで積む**（役割2 を 1/N ずつ撃っている）。
    //   状態を持つのはこちらなので、ここを見ないと「再現できる」の証明にならない。
    float tailRel = 0.0f;
    bool tailExact = true;
};

/* キャプチャの入力を 1 フレームぶん押す。★押す順は録音時と同じでなければならない。 */
inline void pushFrameInputs(AF_SceneHandle s, const CaptureFile& cap, int k) {
    for (const auto& m : cap.moved[static_cast<size_t>(k)]) {
        AF_Vector3 c{m.cx, m.cy, m.cz}, h{m.hx, m.hy, m.hz};
        AF_Vector3 r{m.rx, m.ry, m.rz}, u{m.ux, m.uy, m.uz};
        AF_SceneUpdateInstance(s, m.instance, c, h, r, u);
    }
    const auto& g = cap.global[static_cast<size_t>(k)];
    AF_Vector3 l{g.lx, g.ly, g.lz};
    AF_SceneSetListener(s, l);
    for (const auto& src : cap.sources[static_cast<size_t>(k)]) {
        AF_Vector3 p{src.sx, src.sy, src.sz};
        AF_SceneSetSource(s, src.id, p);
    }
}

/* 押し直して、録れている出力と突き合わせる。
 *   ⚠ 形は呼び手が作っておくこと（この関数は形に触らない）。 */
inline std::vector<FrameDiff> replayAndCompare(AF_SceneHandle s, const CaptureFile& cap,
                                               float dt = 1.0f / 60.0f) {
    std::vector<FrameDiff> out;
    out.reserve(static_cast<size_t>(cap.frames));
    for (int k = 0; k < cap.frames; ++k) {
        pushFrameInputs(s, cap, k);
        // ★窓の先頭で段の位相を戻す。**リスナーを押したあと**に戻すこと
        //   （周回の基準点を今のリスナーへ合わせるため）。
        //   これが無いと尾が永久にずれる（capture.h の phase[] の注記）。
        if (k == 0) {
            int ph[8];
            for (int i = 0; i < 8; ++i) ph[i] = cap.global[0].phase[i];
            AF_SceneDebugSetStagePhase(s, ph);
        }
        AF_SceneUpdate(s, dt);

        FrameDiff d;
        d.frame = static_cast<int>(cap.global[static_cast<size_t>(k)].frame);
        for (const auto& src : cap.sources[static_cast<size_t>(k)]) {
            const int idx = AF_SceneSourceIndex(s, src.id);
            if (idx < 0) { d.exactMismatches += acoustic::dbg::kCapBands; continue; }
            float now[acoustic::dbg::kCapBands] = {};
            AF_SceneGetSourceOcclusion(s, idx, now);
            for (int b = 0; b < acoustic::dbg::kCapBands; ++b) {
                if (now[b] != src.band[b]) ++d.exactMismatches;
                const float den = std::fabs(src.band[b]) > 1e-6f ? std::fabs(src.band[b]) : 1e-6f;
                const float rel = std::fabs(now[b] - src.band[b]) / den;
                if (rel > d.worstRel) d.worstRel = rel;
            }
            // 尾（エコグラムの総和）。状態を持つのはこちら。
            float bins[64 * acoustic::dbg::kCapBands];
            const int nb = AF_SceneGetEchogramBands(s, idx, bins, 64);
            float tot = 0.0f;
            for (int i = 0; i < nb * acoustic::dbg::kCapBands; ++i) tot += bins[i];
            if (tot != src.tailLevel) d.tailExact = false;
            const float tden = std::fabs(src.tailLevel) > 1e-9f ? std::fabs(src.tailLevel) : 1e-9f;
            const float trel = std::fabs(tot - src.tailLevel) / tden;
            if (trel > d.tailRel) d.tailRel = trel;
        }
        out.push_back(d);
    }
    return out;
}

}  // namespace replay
}  // namespace af

#endif  // ACOUSTICFLOW_TEST_CAPTURE_REPLAY_H
