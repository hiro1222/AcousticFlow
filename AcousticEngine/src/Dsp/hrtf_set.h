// hrtf_set.h — HRTF（頭部伝達関数）データセット。方向 → 左右耳のインパルス応答(HRIR)。
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
//
// 移行元は UnityDemo/Assets/Scripts/AcousticFlow/HrtfSet.cs。
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace af {
namespace dsp {

class HrtfSet {
public:
    // ファイル形式（.afhr）。SOFA(NetCDF/HDF5) は読むのが重いので、Python で事前に
    // この平坦な形式へ変換して同梱する（tools/sofa_to_afhr.py）。
    //   magic "AFHR" / version / sampleRate / numDirections / irLength
    //   → 方向配列 (az, el) × numDirections
    //   → HRIR (left[irLength], right[irLength]) × numDirections
    static constexpr std::uint32_t kMagic = 0x52484641u;   // 'A','F','H','R' little-endian
    static constexpr int kVersion = 1;

    const std::string& name() const { return name_; }
    int sampleRate() const { return sampleRate_; }
    int irLength() const { return irLength_; }
    int directionCount() const { return static_cast<int>(az_.size()); }
    bool isValid() const { return directionCount() > 0 && irLength_ > 0; }

    /// このデータセットが測定された頭のサイズ（ITD スケールの基準）。
    /// 実測データの頭囲が不明な場合は成人平均 57cm を仮定する。
    float referenceHeadCircumferenceCm() const { return refHeadCm_; }

    // ── 読み込み ──

    static bool loadFromFile(const char* path, HrtfSet& out) {
        if (!path) return false;
        std::FILE* f = std::fopen(path, "rb");
        if (!f) return false;
        std::fseek(f, 0, SEEK_END);
        const long size = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        if (size < 20) { std::fclose(f); return false; }
        std::vector<unsigned char> buf(static_cast<std::size_t>(size));
        const std::size_t got = std::fread(buf.data(), 1, buf.size(), f);
        std::fclose(f);
        if (got != buf.size()) return false;

        // ファイル名（拡張子なし）を名前にする。
        std::string p(path);
        const std::size_t slash = p.find_last_of("/\\");
        std::string base = (slash == std::string::npos) ? p : p.substr(slash + 1);
        const std::size_t dot = base.find_last_of('.');
        if (dot != std::string::npos) base = base.substr(0, dot);
        return loadFromBytes(buf.data(), buf.size(), base.c_str(), out);
    }

    static bool loadFromBytes(const unsigned char* data, std::size_t size,
                              const char* name, HrtfSet& out) {
        if (!data || size < 20) return false;
        Reader r{data, size, 0};

        std::uint32_t magic = 0;
        if (!r.u32(magic) || magic != kMagic) return false;
        std::int32_t ver = 0;
        if (!r.i32(ver) || ver != kVersion) return false;

        HrtfSet set;
        std::int32_t sr = 0, n = 0, irLen = 0;
        if (!r.i32(sr) || !r.i32(n) || !r.i32(irLen)) return false;
        if (n <= 0 || irLen <= 0 || sr <= 0) return false;

        set.sampleRate_ = sr;
        set.irLength_ = irLen;
        set.az_.resize(static_cast<std::size_t>(n));
        set.el_.resize(static_cast<std::size_t>(n));
        for (int i = 0; i < n; ++i)
            if (!r.f32(set.az_[static_cast<std::size_t>(i)]) ||
                !r.f32(set.el_[static_cast<std::size_t>(i)])) return false;

        set.hrir_.assign(static_cast<std::size_t>(n) * 2 * irLen, 0.0f);
        set.itdSec_.assign(static_cast<std::size_t>(n), 0.0f);
        for (int i = 0; i < n; ++i) {
            float* l = set.earPtr(i, 0);
            float* rr = set.earPtr(i, 1);
            for (int k = 0; k < irLen; ++k) if (!r.f32(l[k])) return false;
            for (int k = 0; k < irLen; ++k) if (!r.f32(rr[k])) return false;
            // 実測データは ITD が波形に焼き込まれているので、ここで分離する。
            set.itdSec_[static_cast<std::size_t>(i)] = extractItdAndAlign(l, rr, irLen, sr);
        }

        set.name_ = name ? name : "(none)";
        set.prepareLookup();
        out = std::move(set);
        return true;
    }

    // ── 合成HRTF（球体頭モデル）──
    // 実測データが無い環境でもパイプライン全体を動かすためのフォールバック。
    //   ITD: Woodworth の球体近似  ITD = (r/c)(θ + sinθ)
    //   ILD: 頭部による影（頭の反対側は高域が減る）を一次ローパスで近似
    // 個人化の器（方向→HRIR、ITD分離）は実測と同じ形で持つので、
    // 実データに差し替えても呼び出し側は変えなくてよい。
    static HrtfSet createSynthetic(int sampleRate, int azStepDeg = 5, int elStepDeg = 10) {
        constexpr float headRadius = 0.0875f;   // 頭半径 8.75cm（頭囲 55cm 相当）
        constexpr float c = 343.0f;
        constexpr float kPi = 3.14159265358979323846f;

        const int azStep = std::max(1, azStepDeg);
        const int elStep = std::max(1, elStepDeg);
        const int azCount = std::max(1, 360 / azStep);
        const int elMin = -40, elMax = 60;
        const int elCount = (elMax - elMin) / elStep + 1;
        const int n = azCount * elCount;
        const int irLen = 128;

        HrtfSet set;
        set.name_ = "Synthetic (spherical head)";
        set.sampleRate_ = sampleRate;
        set.irLength_ = irLen;
        set.refHeadCm_ = 2.0f * kPi * headRadius * 100.0f;
        set.az_.resize(static_cast<std::size_t>(n));
        set.el_.resize(static_cast<std::size_t>(n));
        set.itdSec_.assign(static_cast<std::size_t>(n), 0.0f);
        set.hrir_.assign(static_cast<std::size_t>(n) * 2 * irLen, 0.0f);

        int idx = 0;
        for (int ei = 0; ei < elCount; ++ei) {
            const float el = static_cast<float>(elMin + ei * elStep);
            for (int ai = 0; ai < azCount; ++ai) {
                float az = static_cast<float>(ai * azStep);
                if (az > 180.0f) az -= 360.0f;

                // 耳への入射角（水平面成分で近似）。
                const float azRad = az * kPi / 180.0f;
                const float elRad = el * kPi / 180.0f;
                const float lateral = std::sin(azRad) * std::cos(elRad);   // -1(左) .. +1(右)
                const float theta = std::asin(std::min(1.0f, std::max(-1.0f, lateral)));

                // Woodworth: 音源が右(+)なら右耳が早い＝ITDは負（右が先）。
                set.itdSec_[static_cast<std::size_t>(idx)] =
                    -(headRadius / c) * (theta + std::sin(theta));

                // 頭部の影: 遠い側の耳ほど高域が落ちる。
                const float shadowL = clamp01(0.5f - lateral * 0.5f);   // 右に音源→左耳が影
                const float shadowR = clamp01(0.5f + lateral * 0.5f);
                buildShadowedImpulse(set.earPtr(idx, 0), irLen, shadowL);
                buildShadowedImpulse(set.earPtr(idx, 1), irLen, shadowR);

                set.az_[static_cast<std::size_t>(idx)] = az;
                set.el_[static_cast<std::size_t>(idx)] = el;
                ++idx;
            }
        }
        set.prepareLookup();
        return set;
    }

    // ── 検索 ──

    /// 方位角/仰角(度) → リスナー座標系の単位ベクトル（+x=右, +y=上, +z=前）。
    static void angleToVector(float azDeg, float elDeg, float outDir[3]) {
        constexpr float kPi = 3.14159265358979323846f;
        const float a = azDeg * kPi / 180.0f;
        const float e = elDeg * kPi / 180.0f;
        const float ce = std::cos(e);
        outDir[0] = std::sin(a) * ce;
        outDir[1] = std::sin(e);
        outDir[2] = std::cos(a) * ce;
    }

    /// リスナー座標系の方向ベクトルに最も近い測定点の index を返す。無効なら -1。
    int nearestIndex(const float dir[3]) const {
        if (!isValid() || !dir) return -1;
        float d[3] = {dir[0], dir[1], dir[2]};
        const float len2 = d[0]*d[0] + d[1]*d[1] + d[2]*d[2];
        if (len2 > 1e-12f) {
            const float inv = 1.0f / std::sqrt(len2);
            d[0] *= inv; d[1] *= inv; d[2] *= inv;
        } else {
            d[0] = 0.0f; d[1] = 0.0f; d[2] = 1.0f;   // 既定は正面
        }
        int best = 0;
        float bestDot = -2.0f;
        const int n = directionCount();
        for (int i = 0; i < n; ++i) {
            const float* v = &dirVec_[static_cast<std::size_t>(i) * 3];
            const float dot = v[0]*d[0] + v[1]*d[1] + v[2]*d[2];
            if (dot > bestDot) { bestDot = dot; best = i; }
        }
        return best;
    }

    void getAngles(int index, float& azDeg, float& elDeg) const {
        if (!isValid() || index < 0 || index >= directionCount()) { azDeg = 0; elDeg = 0; return; }
        azDeg = az_[static_cast<std::size_t>(index)];
        elDeg = el_[static_cast<std::size_t>(index)];
    }

    const float* hrir(int index, int ear) const {
        if (!isValid() || index < 0 || index >= directionCount() || ear < 0 || ear > 1) return nullptr;
        return hrir_.data() + hrirOffset(index, ear);
    }

    /// その方向の ITD（秒）。正=右耳が遅い。
    float itdSeconds(int index) const {
        if (!isValid() || index < 0 || index >= directionCount()) return 0.0f;
        return itdSec_[static_cast<std::size_t>(index)];
    }

    /// 個人の頭囲に合わせた ITD（秒）。ITD は頭のサイズにほぼ比例する。
    float itdSecondsScaled(int index, float headCircumferenceCm) const {
        const float base = itdSeconds(index);
        if (refHeadCm_ <= 1.0f) return base;
        return base * (headCircumferenceCm / refHeadCm_);
    }

private:
    struct Reader {
        const unsigned char* p;
        std::size_t size;
        std::size_t pos;
        bool raw(void* dst, std::size_t n) {
            if (pos + n > size) return false;
            std::memcpy(dst, p + pos, n);
            pos += n;
            return true;
        }
        bool u32(std::uint32_t& v) { return raw(&v, 4); }
        bool i32(std::int32_t& v) { return raw(&v, 4); }
        bool f32(float& v) { return raw(&v, 4); }
    };

    static float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

    std::size_t hrirOffset(int index, int ear) const {
        return (static_cast<std::size_t>(index) * 2 + ear) * static_cast<std::size_t>(irLength_);
    }
    float* earPtr(int index, int ear) { return hrir_.data() + hrirOffset(index, ear); }

    // 左右の立ち上がりを検出し、両方を先頭へ揃える。戻り値は抜き取った ITD（秒）。
    //   立ち上がり = 「ピーク振幅の一定割合を最初に超えた位置」。単純だが実用上安定する。
    static float extractItdAndAlign(float* l, float* r, int n, int sampleRate) {
        const int onsetL = findOnset(l, n);
        const int onsetR = findOnset(r, n);
        // 共通ぶんは音源距離由来なので両耳から等しく除く。差だけが ITD。
        const int common = std::min(onsetL, onsetR);
        shiftLeft(l, n, common);
        shiftLeft(r, n, common);
        return static_cast<float>(onsetR - onsetL) / static_cast<float>(sampleRate);
    }

    static int findOnset(const float* h, int n) {
        float peak = 0.0f;
        for (int i = 0; i < n; ++i) peak = std::max(peak, std::fabs(h[i]));
        if (peak <= 1e-9f) return 0;
        const float th = peak * 0.15f;
        for (int i = 0; i < n; ++i) if (std::fabs(h[i]) >= th) return i;
        return 0;
    }

    static void shiftLeft(float* h, int n, int k) {
        if (k <= 0) return;
        if (k >= n) { std::fill(h, h + n, 0.0f); return; }
        std::memmove(h, h + k, static_cast<std::size_t>(n - k) * sizeof(float));
        std::fill(h + (n - k), h + n, 0.0f);
    }

    // 影の強さ shadow(0=完全に影 .. 1=正面) から短いインパルスを作る。
    // 影が強いほど高域が減る＝立ち上がりが鈍る、を一次LPで表現する。
    //   一次LPの直流利得は 1 なので、gain は「低域がどれだけ残るか」を決める。
    //   実際の頭部は低域(〜500Hz)ならほぼ回り込むので、低域はあまり落とさない。
    static void buildShadowedImpulse(float* h, int n, float shadow) {
        std::fill(h, h + n, 0.0f);
        const float s = clamp01(shadow);
        const float openness = 0.12f + (1.0f - 0.12f) * s;   // LP係数（高域の減り方）
        const float gain = 0.75f + (1.0f - 0.75f) * s;       // 低域の残り方
        float state = 0.0f;
        for (int i = 0; i < n; ++i) {
            const float x = (i == 0) ? 1.0f : 0.0f;
            state += openness * (x - state);
            h[i] = state * gain;
            if (i > 48 && std::fabs(state) < 1e-5f) break;
        }
    }

    void prepareLookup() {
        const int n = directionCount();
        dirVec_.assign(static_cast<std::size_t>(n) * 3, 0.0f);
        for (int i = 0; i < n; ++i)
            angleToVector(az_[static_cast<std::size_t>(i)], el_[static_cast<std::size_t>(i)],
                          &dirVec_[static_cast<std::size_t>(i) * 3]);
    }

    std::string name_ = "(none)";
    int sampleRate_ = 0;
    int irLength_ = 0;
    float refHeadCm_ = 57.0f;

    // 方向（度）。az: 0=正面, +90=右, -90=左 / el: 0=水平, +90=真上。
    std::vector<float> az_, el_;
    std::vector<float> hrir_;     // 立ち上がりを揃えた HRIR [dir][ear][irLength]
    std::vector<float> itdSec_;   // 各方向の ITD（秒）。正=右耳が遅い（＝音源が左）
    std::vector<float> dirVec_;   // 検索用の単位ベクトル [dir][3]
};

}  // namespace dsp
}  // namespace af
