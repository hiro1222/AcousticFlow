/* Flow/mix.h ── 配分の結果の形式（段 1）
 *
 * ■ 役割
 *   distribute が出し、DSP（voice_renderer）が受ける「1 音源ぶんの配分」。
 *   タップの並び（直接・初期反射・回折・透過）、部屋ごとの FDN への送り、尾の開始、
 *   そして帳簿（総量と五成分の内訳）。
 *
 * ■ 中の仕組み
 *   ★単位は Flow の中では**全部エネルギー**（振幅の二乗）。DSP へ渡す直前（段 4 の橋）で 1 回だけ √ を取る。
 *     旧 material.h の警告「エネルギーと振幅が混ざると壁越しの音が二乗ぶん小さくなる」を、規約で潰す。
 *     欄の名前を e6 にして、読む側が「エネルギーだ」と分かるようにした。
 *   1) MixTap: 種別 kind、到達 delaySec、帯域別エネルギー e6、リスナー座標の到来方向 dirLocal、
 *      広がり spread（0 点 … 1 一様。面の可視率で広げた虚像や、拡散成分に使う）。
 *      遅延は秒で持つ。サンプル数はサンプルレートを知っている DSP 側が出す（配分はレートを知らなくてよい）。
 *   2) FdnSend: 部屋番号と帯域別エネルギー。後期の残響はタップでなく FDN に注ぐ。
 *   3) onsetSec: 尾の開始 ＝ 最初の虚像の到達（地図 7 章「尾の開始 ＝ ITDG」）。
 *   4) 帳簿: energy6（総量）と component6[5][6]（五成分の内訳）。
 *      conserves() で「五成分の和 ＝ 総量」を検査できる。地図 6 章の決めごと 3。
 *      ★旧実装の 15.7 dB の穴（扉越しで配線と生存が別々に量を減らした）は、この帳簿が無かったから
 *        見つかるまで 1 日かかった。出口で毎回足し算するだけで捕まる。
 *
 * ■ 繋がり
 *   受ける: distribute が書く。
 *   渡す:   段 4 の橋が MixTap → EarlyReflectConv::Tap（√ を取り、秒 → サンプル）、FdnSend → setFdnSends、
 *           onsetSec → setFdnOnsetMs に写す。帳簿は検査と AF ツールの情報タブが読む。
 *
 * ■ 退けた書き方
 *   ・タップに振幅を持たせる（DSP の形式をそのまま使う）: 配分の途中で √ が混ざる。旧実装がそれで壊れた。
 *   ・可変長 std::vector: 毎フレーム作る物なので確保しない。固定配列 ＋ 本数。
 *   ・帳簿を持たない（タップから逆算する）: タップは方向で割れているので足し戻すのが面倒で、誰もやらない。
 *     配分の側で書けば 6 回の足し算。
 *
 * ■ 壊れる所
 *   ・kMaxTaps を超えると後ろが落ちる（黙って）。distribute は虚像の数を上限で切ってから書くこと。
 *   ・conserves を「タップの和」で書き換えると、FDN の送り（後期）が入らず常に落ちる。内訳は component6。
 *   ・e6 に振幅を書くと、二乗ぶん小さい音になり、conserves は通ってしまう（内訳も振幅なら辻褄が合う）。
 *     検査は「energy_trace の集計（エネルギー）と一致するか」で見るので、そこで捕まる。
 */
#ifndef ACOUSTICFLOW_FLOW_MIX_H
#define ACOUSTICFLOW_FLOW_MIX_H

#include <cmath>
#include <cstdint>
#include "Core/material.h"
#include "Core/vec3.h"
#include "Flow/world_rules.h"

namespace acoustic {
namespace flow {

enum class TapKind : std::uint8_t { Direct = 0, Early = 1, Diffract = 2, Transmit = 3 };

struct MixTap {
    TapKind kind = TapKind::Direct;
    // ★素性（identity）。DSP がフレームをまたいでタップを繋ぐときの手がかり。
    //   直接・透過・回折・初期はほぼ同じ遅延に並ぶので、遅延の近さで繋ぐと入れ替わる
    //   （EarlyReflectConv の許容は 5.3 ms ＝ その 4 本が丸ごと入る）。入れ替わると
    //   大きいタップが小さいタップのゲインへ向かって補間し、乗り換えの 30 ms だけ音が凹む。
    //   固定の番号: 1 直接 / 2 透過 / 3 回折 / 4 初期（虚像なしの 1 本）
    //   虚像は面の組で決める（16 + 面0×256 + 面1+1）── 並び順が変わっても同じ虚像は同じ番号。
    int     id = -1;
    float   delaySec = 0.0f;                 // 到達（秒）
    float   e6[kNumBands] = {};              // エネルギー（帯域別）
    Vec3    dirLocal{0, 0, 0};               // リスナー座標の到来方向（単位）。ゼロ = 方向なし
    float   spread = 0.0f;                   // 0 点 … 1 一様
};

struct FdnSend {
    int   room = -1;
    float e6[kNumBands] = {};                // エネルギー（帯域別）
    // 段 2-f: この送りの尾が来る向き（ワールドの単位ベクトル）と集まり具合（0 全方向〜1 一点）。
    //   耳の部屋へ行く分は 0（一様）。戸口越しの分はレイが測った向きと R を持つ。World が部屋ごとに束ねて FDN に置く。
    float dir[3] = {0.0f, 0.0f, 0.0f};
    float focus = 0.0f;
    // 段 2-g（戸口の線音源）: e6 のうち、戸口からリスナーへ直接出す分。残り（e6 − thru6）は戸口の線音源が
    //   耳の部屋の FDN へ流す分。戸口の線音源が無い送りでは 0 のまま（使わない）。
    float thru6[kNumBands] = {};
};

struct Mix {
    static constexpr int kMaxTaps = 64;
    static constexpr int kMaxSends = 4;

    MixTap  taps[kMaxTaps];
    int     tapCount = 0;
    FdnSend sends[kMaxSends];
    int     sendCount = 0;
    float   onsetSec = 0.0f;                 // 尾の開始（最初の虚像の到達）

    // ── 帳簿（エネルギー）──
    float energy6[kNumBands] = {};                       // 総量
    float component6[kNumComponents][kNumBands] = {};    // 五成分の内訳（kDirect … kTransmit）

    void clear() {
        tapCount = 0; sendCount = 0; onsetSec = 0.0f;
        for (int b = 0; b < kNumBands; ++b) {
            energy6[b] = 0.0f;
            for (int c = 0; c < kNumComponents; ++c) component6[c][b] = 0.0f;
        }
    }
    MixTap* pushTap() { return (tapCount < kMaxTaps) ? &taps[tapCount++] : nullptr; }
    FdnSend* pushSend() { return (sendCount < kMaxSends) ? &sends[sendCount++] : nullptr; }

    float sumComponents(int b) const {
        float s = 0.0f;
        for (int c = 0; c < kNumComponents; ++c) s += component6[c][b];
        return s;
    }
    /// 五成分の和が総量に tolDb 以内で一致するか（全帯域）。どちらも 0 の帯域は一致とみなす。
    bool conserves(float tolDb = 0.1f) const {
        for (int b = 0; b < kNumBands; ++b) {
            const float s = sumComponents(b), t = energy6[b];
            if (s <= 1e-12f && t <= 1e-12f) continue;
            if (s <= 1e-12f || t <= 1e-12f) return false;
            if (std::fabs(10.0f * std::log10(s / t)) > tolDb) return false;
        }
        return true;
    }
};

}  // namespace flow
}  // namespace acoustic

#endif  // ACOUSTICFLOW_FLOW_MIX_H
