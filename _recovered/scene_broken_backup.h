/* Core/scene.h  笏笏 譁ｰ繧｢繝ｼ繧ｭ(2026-07) Phase 0・壹う繝ｳ繧ｹ繧ｿ繝ｳ繧ｹ譁ｹ蠑上・髻ｳ髻ｿ繧ｷ繝ｼ繝ｳ
 *
 * 譌ｧ AcousticWorld 縺ｮ boxes_/meshes_ 蛻・屬繧貞ｻ・＠縲∵ｬ｡荳紋ｻ｣險ｭ險医・縲後う繝ｳ繧ｹ繧ｿ繝ｳ繧ｹ縲阪↓邨ｱ荳縺吶ｋ縲・ *   繧､繝ｳ繧ｹ繧ｿ繝ｳ繧ｹ = geomId(蠖｢迥ｶ) + transform(OBB) + materialId(譚占ｳｪ)
 * 縺薙ｌ縺ｯ TLAS/BLAS 縺ｮ蝨溷床縲１hase 0 縺ｧ縺ｯ蜈ｨ繧､繝ｳ繧ｹ繧ｿ繝ｳ繧ｹ繧・OBB 繝励Μ繝溘ユ繧｣繝悶→縺励※邱壼ｽ｢襍ｰ譟ｻ縺励・ * BVH-of-OBB(TLAS) 縺ｯ Phase 3 縺ｧ蟾ｮ縺苓ｾｼ繧・郁ｵｰ譟ｻ繝ｫ繝ｼ繝励□縺大ｷｮ縺玲崛縺医ｌ縺ｰ貂医・繧医≧ API 繧貞・髮｢・峨・ *
 * 險ｭ險域婿驥晢ｼ・[dynamic-ray-architecture]] 縺ｫ貅匁侠・・
 *   - 髱咏噪髻ｳ髻ｿ遨ｺ髢薙ｒ菴懊ｉ縺ｪ縺・ゅず繧ｪ繝｡繝医Μ縺ｯ豈弱ヵ繝ｬ繝ｼ繝譖ｴ譁ｰ蜿ｯ閭ｽ縺ｪ迚ｩ諤ｧ縺ｮ縺ｿ縲・ *   - 譚占ｳｪ縺ｯ materials_ 繝・・繝悶Ν縺ｫ荳蜈・喧縺励√う繝ｳ繧ｹ繧ｿ繝ｳ繧ｹ縺ｯ matId 縺ｧ蜿ら・・育┥縺九↑縺・ｼ峨・ *   - Wwise 縺ｫ繧・C API 縺ｫ繧ゆｾ晏ｭ倥＠縺ｪ縺・ｴ皮ｲ玖ｨ育ｮ怜ｱ､・・TL 閾ｪ逕ｱ・峨・ *
 * Phase 0 縺梧署萓帙☆繧九け繧ｨ繝ｪ:
 *   - raycastClosest : 譛霑大ｍ繝偵ャ繝茨ｼ医Ξ繧､繧ｭ繝｣繧ｹ繝医・蝨溷床・・ *   - isOccluded     : 2轤ｹ髢薙・隕矩壹＠・井ｺ悟､・・ *   - computeTransmission : 逶ｴ邱壻ｸ翫・螢√・蟶ｯ蝓溷挨騾城℃繧ｲ繧､繝ｳ(6蟶ｯ蝓・ 竊・蠖ｹ蜑ｲ1縺ｮ邏
 */
#ifndef ACOUSTICFLOW_CORE_SCENE_H
#define ACOUSTICFLOW_CORE_SCENE_H

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "Core/aabb.h"
#include "Core/aperture_fresnel.h"
#include "Core/btm.h"
#include "Core/kirchhoff.h"
#include "Core/maekawa.h"
#include "Core/material.h"
#include "Core/mesh_geom.h"
#include "Core/utd.h"
#include "Core/vec3.h"

namespace acoustic {

// 笏笏 繝ｬ繧､繧ｵ繝ｳ繝励Μ繝ｳ繧ｰ陬懷勧・亥ｽｹ蜑ｲ2縺ｮ蜿榊ｰ・ヨ繝ｬ繝ｼ繧ｹ逕ｨ・峨よ立 acoustic_world.cpp 縺九ｉ遘ｻ讀阪・namespace scene_detail {

constexpr float kPiF = 3.14159265358979f;

inline float clamp01(float x) { return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); }

// 迺ｰ蠅・､画焚 AF_DIFF_DEBUG 縺ｧ 2 谺｡蝗樊釜縺ｮ蜀・ｨｳ繧・stderr 縺ｫ蜃ｺ縺呻ｼ郁ｪｿ譟ｻ逕ｨ縲よ里螳壹・辟｡蜉ｹ・峨・inline bool diffDebug() {
    static const bool on = (std::getenv("AF_DIFF_DEBUG") != nullptr);
    return on;
}

// 繝輔ぅ繝懊リ繝・メ逅・ｼ壻ｸ讒倥↓霑代＞譁ｹ蜷大・謨｣縲・inline Vec3 fibonacciSphereDir(int i, int n) {
    const float k = static_cast<float>(i) + 0.5f;
    const float phi = std::acos(1.0f - 2.0f * k / static_cast<float>(n));
    const float theta = kPiF * (1.0f + std::sqrt(5.0f)) * k;
    const float s = std::sin(phi);
    return Vec3(s * std::cos(theta), std::cos(phi), s * std::sin(theta));
}

// 霆ｽ驥上ワ繝・す繝･荵ｱ謨ｰ・・CG邉ｻ・峨・inline float hashRand01(uint32_t& state) {
    state = state * 747796405u + 2891336453u;
    uint32_t w = ((state >> ((state >> 28) + 4u)) ^ state) * 277803737u;
    w = (w >> 22) ^ w;
    return static_cast<float>(w) * (1.0f / 4294967296.0f);
}

// 豕慕ｷ・n 縺ｾ繧上ｊ縺ｮ菴吝ｼｦ驥阪∩蜊顔帥繧ｵ繝ｳ繝励Ν・医Λ繝ｳ繝舌・繝域僑謨｣・峨・inline Vec3 cosineHemisphere(const Vec3& n, uint32_t& rng) {
    const float u1 = hashRand01(rng);
    const float u2 = hashRand01(rng);
    const float r = std::sqrt(u1);
    const float th = 2.0f * kPiF * u2;
    const float x = r * std::cos(th);
    const float y = r * std::sin(th);
    const float z = std::sqrt(std::max(0.0f, 1.0f - u1));
    const Vec3 t = (std::fabs(n.x) > 0.9f) ? Vec3(0.0f, 1.0f, 0.0f) : Vec3(1.0f, 0.0f, 0.0f);
    const Vec3 b1 = normalized(cross(n, t));
    const Vec3 b2 = cross(n, b1);
    return normalized(b1 * x + b2 * y + n * z);
}

// scattering(0..1) 縺ｧ蜿榊ｰ・婿蜷代ｒ髀｡髱｢竍・僑謨｣縺ｫ遒ｺ邇・噪縺ｫ謖ｯ繧雁・縺代ｋ縲・inline Vec3 scatteredDir(const Vec3& d, const Vec3& n, float s, uint32_t& rng) {
    if (s > 0.0f && hashRand01(rng) < s) return cosineHemisphere(n, rng);  // 諡｡謨｣
    return reflect(d, n);                                                  // 髀｡髱｢
}

inline float scatteringMean(const AcousticMaterial& m) {
    float s = 0.0f;
    for (int b = 0; b < kNumBands; ++b) s += m.scattering[b];
    return s / static_cast<float>(kNumBands);
}

// 縲仙屓謚・Phase 4)縲題ｿょ屓菴吝臆髟ｷ ﾎｴ(m) 縺九ｉ蟶ｯ蝓溷挨蝗樊釜繧ｲ繧､繝ｳ(0..1)繧貞・縺吶・//   ITU-R P.526 繝翫う繝輔お繝・ず蝗樊釜謳・J(ﾎｽ)・夷ｽ=Fresnel-Kirchhoff 繝代Λ繝｡繝ｼ繧ｿ・峨・//   Maekawa 繧医ｊ騾｣邯壹〒讓呎ｺ也噪縲ょｽｱ蠅・阜(ﾎｴ=0)縺ｧ ~6dB=0.5縲∽ｽ主沺縺ｻ縺ｩ蝗槭ｊ霎ｼ繧・夷ｽ 蟆擾ｼ晄錐蟆擾ｼ峨・inline void knifeEdgeGain(float delta, float outGain[kNumBands]) {
    const float kSpeed = 343.0f;
    const float bandFreq[kNumBands] = {125.0f, 250.0f, 500.0f, 1000.0f, 2000.0f, 4000.0f};
    const float dd = delta > 0.0f ? delta : 0.0f;
    for (int b = 0; b < kNumBands; ++b) {
        const float lambda = kSpeed / bandFreq[b];
        const float nu = 2.0f * std::sqrt(dd / lambda);  // ﾎｽ = 2竏・ﾎｴ/ﾎｻ)・亥ｽｱ蛛ｴ ﾎｴ竕･0・・        const float t = nu - 0.1f;
        float J = 6.9f + 20.0f * std::log10(std::sqrt(t * t + 1.0f) + t);  // dB 謳・        if (J < 0.0f) J = 0.0f;
        outGain[b] = std::pow(10.0f, -J / 20.0f);
    }
}

}  // namespace scene_detail

// 繧ｷ繝ｼ繝ｳ蜀・・1縺､縺ｮ蜊譛臥黄縲ょｽ｢迥ｶ縺ｯ geomId 縺ｧ驕ｸ縺ｶ・・1=邂ｱ / >=0=繝｡繝・す繝･・峨・struct Instance {
    Obb obb;              // 繝ｯ繝ｼ繝ｫ繝臥ｩｺ髢薙・譛牙髄蠅・阜繝懊ャ繧ｯ繧ｹ・・ransform 逶ｸ蠖難ｼ・    int materialId = 0;   // materials_ 繝・・繝悶Ν縺ｸ縺ｮ蜿ら・
    // 蠖｢迥ｶ(BLAS)縺ｮ謖・ｮ壹・1 = 邂ｱ・・bb 縺昴・繧ゅ・・会ｼ・=0 = meshes_ 縺ｮ豺ｻ蟄励・    //   繝｡繝・す繝･縺ｮ蝣ｴ蜷医｛bb 縺ｯ縲悟､画鋤陦悟・縲阪→縲後Ρ繝ｼ繝ｫ繝牙｢・阜繝懊ャ繧ｯ繧ｹ縲阪ｒ蜈ｼ縺ｭ繧九・    //   繝｡繝・す繝･縺ｯ豁｣隕丞喧繝ｭ繝ｼ繧ｫ繝ｫ遨ｺ髢難ｼ・ABB=[-1,1]^3・峨〒謖√▽縺ｮ縺ｧ縲｛bb 縺後◎縺ｮ縺ｾ縺ｾ
    //   local竊蜘orld 縺ｮ蜀吝ワ縺ｫ縺ｪ繧具ｼ・esh_geom.h 蜿ら・・峨・    int geomId = -1;
    bool active = true;   // false 縺ｮ縺ｨ縺榊・襍ｰ譟ｻ縺九ｉ繧ｹ繧ｭ繝・・・・OD 繧ｹ繝医Μ繝ｼ繝溘Φ繧ｰ逕ｨ・・};

// 繝ｬ繧､襍ｰ譟ｻ縺ｮ邨先棡縲・struct SceneHit {
    bool  hit = false;
    float t = 0.0f;                    // origin 縺九ｉ繝偵ャ繝医∪縺ｧ縺ｮ霍晞屬・・ir 豁｣隕丞喧蜑肴署・・    Vec3  point{0.0f, 0.0f, 0.0f};     // 繝偵ャ繝井ｽ咲ｽｮ・・rigin + dir*t・・    Vec3  normal{0.0f, 0.0f, 0.0f};    // 繝ｯ繝ｼ繝ｫ繝画ｳ慕ｷ・    int   instanceId = -1;             // 繝偵ャ繝医＠縺溘う繝ｳ繧ｹ繧ｿ繝ｳ繧ｹ
    int   materialId = -1;             // 縺昴・譚占ｳｪ
};

class Scene {
public:
    // --- 讒狗ｯ会ｼ育匳骭ｲ・・---

    // 譚占ｳｪ繧偵ユ繝ｼ繝悶Ν縺ｫ霑ｽ蜉縺励√◎縺ｮ materialId 繧定ｿ斐☆縲・    int addMaterial(const AcousticMaterial& m) {
        materials_.push_back(m);
        return static_cast<int>(materials_.size()) - 1;
    }

    // 譌｢蟄倥・譚占ｳｪ縺ｮ荳ｭ霄ｫ繧呈嶌縺肴鋤縺医ｋ縲ゅ◎縺ｮ譚占ｳｪ繧剃ｽｿ縺｣縺ｦ縺・ｋ繧､繝ｳ繧ｹ繧ｿ繝ｳ繧ｹ縺御ｸ譁峨↓螟峨ｏ繧九・    //   譚占ｳｪ縺ｯ materials_ 繧呈ｯ主屓蠑輔″逶ｴ縺励※菴ｿ縺・ｼ医く繝｣繝・す繝･繧ょ燕險育ｮ励ｂ辟｡縺・ｼ峨・縺ｧ縲・    //   縺薙％繧呈嶌縺肴鋤縺医ｌ縺ｰ谺｡縺ｮ繧ｯ繧ｨ繝ｪ縺九ｉ蜉ｹ縺上・VH 縺ｯ蠖｢迥ｶ縺縺代↑縺ｮ縺ｧ蜀肴ｧ狗ｯ峨ｂ荳崎ｦ√・    bool setMaterial(int id, const AcousticMaterial& m) {
        if (id < 0 || id >= materialCount()) return false;
        materials_[static_cast<std::size_t>(id)] = m;
        return true;
    }

    // 繧､繝ｳ繧ｹ繧ｿ繝ｳ繧ｹ縺ｮ譚占ｳｪ繧剃ｻ倥￠譖ｿ縺医ｋ縲ゅ後％縺ｮ謇峨□縺第惠縲阪・繧医≧縺ｪ菴ｿ縺・婿縲・    bool setInstanceMaterial(int instanceId, int materialId) {
        if (!validInstance(instanceId)) return false;
        if (materialId < 0 || materialId >= materialCount()) return false;
        instances_[static_cast<std::size_t>(instanceId)].materialId = materialId;
        return true;
    }

    int instanceMaterial(int instanceId) const {
        return validInstance(instanceId)
             ? instances_[static_cast<std::size_t>(instanceId)].materialId : -1;
    }

    // 繧､繝ｳ繧ｹ繧ｿ繝ｳ繧ｹ繧定ｿｽ蜉縺・instanceId 繧定ｿ斐☆縲ＮaterialId 縺ｯ addMaterial 縺ｮ謌ｻ繧雁､縲・    // 遽・峇螟・materialId 縺ｯ 0 縺ｫ荳ｸ繧√ｋ・域攝雉ｪ譛ｪ逋ｻ骭ｲ縺ｪ繧画里螳壼｣√ｒ1縺､蜈･繧後※縺翫￥縺薙→・峨・    // geomId: -1 = 邂ｱ・・bb 縺昴・繧ゅ・・会ｼ・=0 = addMesh 縺ｮ謌ｻ繧雁､縲・    int addInstance(const Obb& obb, int materialId, int geomId = -1) {
        Instance inst;
        inst.obb = obb;
        inst.materialId = clampMaterialId(materialId);
        inst.geomId = validMesh(geomId) ? geomId : -1;
        instances_.push_back(inst);
        bvhDirty_ = true;
        return static_cast<int>(instances_.size()) - 1;
    }

    // 笏笏 蠖｢迥ｶ(BLAS) 笏笏笏笏笏笏笏笏笏笏笏笏笏笏笏笏笏笏笏笏笏笏笏笏笏笏笏笏笏笏笏笏笏笏笏笏笏笏笏笏笏笏笏笏笏笏笏笏
    // 荳芽ｧ貞ｽ｢繝｡繝・す繝･繧堤匳骭ｲ縺・geomId 繧定ｿ斐☆縲ょ､ｱ謨励・ -1縲・    //   螳溯｡梧凾縺ｫ蜻ｼ縺ｹ繧九らｴ螢翫ｄ繝励Ο繧ｷ繝ｼ繧ｸ繝｣繝ｫ逕滓・縺ｧ蠖｢迥ｶ縺悟｢励∴繧九・縺ｯ驟咲ｽｮ縺ｮ謫堺ｽ懊〒縺ゅｊ縲・    //   讒狗ｯ峨′隕√ｋ縺ｮ縺ｯ縺薙・ 1 蛟九・繧薙□縺托ｼ医Ξ繝吶Ν蜈ｨ菴薙・蜀崎ｨ育ｮ励・豎ｺ縺励※襍ｷ縺阪↑縺・ｼ峨・    //   outLocalCenter / outLocalHalfExtents 縺ｫ縺ｯ縲∵ｭ｣隕丞喧縺ｫ菴ｿ縺｣縺溘Ο繝ｼ繧ｫ繝ｫ AABB 繧定ｿ斐☆縲・    //   繝帙せ繝医・縺薙ｌ繧剃ｽｿ縺｣縺ｦ繧､繝ｳ繧ｹ繧ｿ繝ｳ繧ｹ縺ｮ OBB・茨ｼ晏､画鋤・峨ｒ菴懊ｋ縺薙→縲・    int addMesh(const float* verticesXYZ, int vertexCount, const int* indices, int indexCount,
                Vec3* outLocalCenter = nullptr, Vec3* outLocalHalfExtents = nullptr) {
        // 遨ｺ縺阪せ繝ｭ繝・ヨ繧貞・蛻ｩ逕ｨ縺吶ｋ縲りｩｰ繧∫峩縺吶→譌｢蟄倥・ geomId 縺悟｣翫ｌ繧九・縺ｧ邨ｶ蟇ｾ縺ｫ縺励↑縺・・        int slot = -1;
        for (size_t i = 0; i < meshes_.size(); ++i)
            if (!meshes_[i].used) { slot = static_cast<int>(i); break; }
        if (slot < 0) { meshes_.emplace_back(); slot = static_cast<int>(meshes_.size()) - 1; }

        if (!meshes_[static_cast<size_t>(slot)].build(verticesXYZ, vertexCount, indices, indexCount)) {
            meshes_[static_cast<size_t>(slot)].clear();
            return -1;
        }
        if (outLocalCenter) *outLocalCenter = meshes_[static_cast<size_t>(slot)].localCenter;
        if (outLocalHalfExtents) *outLocalHalfExtents = meshes_[static_cast<size_t>(slot)].localHalfExtents;
        return slot;
    }

    // 蠖｢迥ｶ繧定ｧ｣謾ｾ縺吶ｋ縲ょ盾辣ｧ縺励※縺・◆繧､繝ｳ繧ｹ繧ｿ繝ｳ繧ｹ縺ｯ邂ｱ謇ｱ縺・↓關ｽ縺｡繧具ｼ亥｢・阜繝懊ャ繧ｯ繧ｹ縺ｨ縺励※谿九ｋ・峨・    void removeMesh(int geomId) {
        if (!validMesh(geomId)) return;
        meshes_[static_cast<size_t>(geomId)].clear();
        for (Instance& inst : instances_)
            if (inst.geomId == geomId) inst.geomId = -1;
        bvhDirty_ = true;
    }

    // 謚ｽ蜃ｺ縺輔ｌ縺溷屓謚倡ｨ懃ｷ壹・譛ｬ謨ｰ・郁ｨｺ譁ｭ逕ｨ・峨ゅユ繝・そ繝ｬ繝ｼ繧ｷ繝ｧ繝ｳ縺ｫ萓晏ｭ倥＠縺ｪ縺・％縺ｨ縺ｮ遒ｺ隱阪↓菴ｿ縺・・    int meshEdgeCount(int geomId) const {
        if (!validMesh(geomId)) return -1;
        return static_cast<int>(meshes_[static_cast<size_t>(geomId)].edges.size());
    }

    int meshCount() const { return static_cast<int>(meshes_.size()); }
    bool validMesh(int geomId) const {
        return geomId >= 0 && geomId < static_cast<int>(meshes_.size())
            && meshes_[static_cast<size_t>(geomId)].used;
    }

    // 譌｢蟄倥う繝ｳ繧ｹ繧ｿ繝ｳ繧ｹ縺ｮ transform 繧呈峩譁ｰ縺吶ｋ・亥虚縺・◆蛻・□縺托ｼ抂(moved) 縺ｮ蝨溷床・峨・    // 遽・峇螟・id 縺ｯ辟｡隕悶・    void updateInstanceTransform(int instanceId, const Obb& obb) {
        if (!validInstance(instanceId)) return;
        instances_[instanceId].obb = obb;
        bvhDirty_ = true;
    }

    // 繧､繝ｳ繧ｹ繧ｿ繝ｳ繧ｹ縺ｮ譛牙柑/辟｡蜉ｹ繧貞・繧頑崛縺医ｋ縲・    void setInstanceActive(int instanceId, bool active) {
        if (!validInstance(instanceId)) return;
        instances_[instanceId].active = active;
        bvhDirty_ = true;
    }

    // 蜈ｨ繧､繝ｳ繧ｹ繧ｿ繝ｳ繧ｹ繧呈ｶ医☆・域攝雉ｪ繝・・繝悶Ν縺ｯ菫晄戟・峨よｯ弱ヵ繝ｬ繝ｼ繝菴懊ｊ逶ｴ縺咏畑騾斐・    void clearInstances() { instances_.clear(); bvhDirty_ = true; }

    // 譚占ｳｪ繧ゅう繝ｳ繧ｹ繧ｿ繝ｳ繧ｹ繧ょｽ｢迥ｶ繧ょ・豸医＠縲・    void clearAll() {
        instances_.clear();
        materials_.clear();
        meshes_.clear();
        bvhDirty_ = true;
    }

    // --- 繧ｯ繧ｨ繝ｪ ---

    // origin 縺九ｉ dir 譁ｹ蜷代∈譛霑大ｍ繝偵ャ繝医ｒ霑斐☆縲Ｅir 縺ｯ蜀・Κ縺ｧ豁｣隕丞喧縺吶ｋ縲・VH-of-OBB 縺ｧ蜉騾溘・    SceneHit raycastClosest(const Vec3& origin, const Vec3& dir, float maxDist) const {
        ensureBvh();
        SceneHit best;
        const Vec3 d = normalized(dir);
        float closest = maxDist;
        if (bvhNodes_.empty()) return best;
        int stack[64];
        int sp = 0;
        stack[sp++] = 0;
        while (sp > 0) {
            const BvhNode& node = bvhNodes_[stack[--sp]];
            float tn;
            Vec3 nn;
            if (!rayIntersectsAabb(origin, d, node.bounds, closest, tn, nn)) continue;
            if (node.count > 0) {  // 闡・                for (int k = 0; k < node.count; ++k) {
                    const int i = bvhOrder_[node.leftFirst + k];
                    const Instance& inst = instances_[i];
                    float t;
                    Vec3 n;
                    if (instanceRaycast(inst, origin, d, closest, t, n) && t < closest) {
                        closest = t;
                        best.hit = true;
                        best.t = t;
                        best.point = origin + d * t;
                        best.normal = n;
                        best.instanceId = i;
                        best.materialId = inst.materialId;
                    }
                }
            } else if (sp + 2 <= 64) {
                stack[sp++] = node.leftFirst;
                stack[sp++] = node.leftFirst + 1;
            }
        }
        return best;
    }

    // 笏笏 繧､繝ｳ繧ｹ繧ｿ繝ｳ繧ｹ蜊倅ｽ阪・蟷ｾ菴募愛螳夲ｼ育ｮｱ・上Γ繝・す繝･繧貞精蜿弱☆繧具ｼ俄楳笏笏笏笏笏笏笏笏笏笏笏笏笏笏
    //   TLAS 縺ｮ闡峨〒縺ｯ縺薙・2縺､縺縺代ｒ蜻ｼ縺ｶ縲ょｽ｢迥ｶ縺ｮ遞ｮ鬘槭・縺薙％縺ｫ髢峨§霎ｼ繧√ｋ縲・
    // 繝ｬ繧､譛霑代ヲ繝・ヨ縲ＮaxT/outT 縺ｯ繝ｯ繝ｼ繝ｫ繝芽ｷ晞屬縲・    //   繝｡繝・す繝･縺ｯ繝ｭ繝ｼ繧ｫ繝ｫ縺ｸ遘ｻ縺励※縺九ｉ BVH 縺ｫ蝠上≧縲・*譁ｹ蜷代ｒ豁｣隕丞喧縺励↑縺・*縺ｮ縺ｧ
    //   繝ｭ繝ｼ繧ｫ繝ｫ蛛ｴ縺ｮ t 縺後Ρ繝ｼ繝ｫ繝芽ｷ晞屬縺ｨ荳閾ｴ縺励∽ｻ悶う繝ｳ繧ｹ繧ｿ繝ｳ繧ｹ縺ｨ縺ｮ豈碑ｼ・′縺昴・縺ｾ縺ｾ騾壹ｋ縲・    bool instanceRaycast(const Instance& inst, const Vec3& origin, const Vec3& dir,
                         float maxT, float& outT, Vec3& outNormal) const {
        if (inst.geomId < 0) return rayIntersectsObb(origin, dir, inst.obb, maxT, outT, outNormal);
        if (inst.geomId >= static_cast<int>(meshes_.size())) return false;
        const MeshGeometry& g = meshes_[static_cast<size_t>(inst.geomId)];
        if (!g.used) return false;
        // 蜈医↓繝ｯ繝ｼ繝ｫ繝牙｢・阜繝懊ャ繧ｯ繧ｹ縺ｧ蠑ｾ縺擾ｼ・bb 縺ｯ繝｡繝・す繝･縺ｮ蠅・阜繧ょ・縺ｭ縺ｦ縺・ｋ・峨・        float bt; Vec3 bn;
        if (!rayIntersectsObb(origin, dir, inst.obb, maxT, bt, bn)) return false;
        const Vec3 lo = meshWorldToLocalPoint(origin, inst.obb);
        const Vec3 ld = meshWorldToLocalDir(dir, inst.obb);
        Vec3 ln;
        if (!g.bvh.raycast(lo, ld, maxT, outT, ln)) return false;
        outNormal = meshLocalNormalToWorld(ln, inst.obb);
        return true;
    }

    // 邱壼・縺後％縺ｮ繧､繝ｳ繧ｹ繧ｿ繝ｳ繧ｹ縺ｫ驕ｮ繧峨ｌ繧九°縲・    bool instanceOccludes(const Instance& inst, const Vec3& from, const Vec3& to) const {
        if (inst.geomId < 0) return segmentIntersectsObb(from, to, inst.obb);
        if (inst.geomId >= static_cast<int>(meshes_.size())) return false;
        const MeshGeometry& g = meshes_[static_cast<size_t>(inst.geomId)];
        if (!g.used) return false;
        if (!segmentIntersectsObb(from, to, inst.obb)) return false;   // 蠅・阜繝懊ャ繧ｯ繧ｹ縺ｧ蜈医↓譽・唆
        return g.bvh.occludes(meshWorldToLocalPoint(from, inst.obb),
                              meshWorldToLocalPoint(to, inst.obb));
    }

    // 2轤ｹ髢薙′菴輔°縺ｫ驕ｮ繧峨ｌ縺ｦ縺・ｋ縺具ｼ井ｺ悟､・峨・譛ｬ縺ｧ繧ょｽ薙◆繧後・驕ｮ阡ｽ縲・VH 縺ｧ蜉騾溘・    bool isOccluded(const Vec3& from, const Vec3& to) const {
        ensureBvh();
        if (bvhNodes_.empty()) return false;
        int stack[64];
        int sp = 0;
        stack[sp++] = 0;
        while (sp > 0) {
            const BvhNode& node = bvhNodes_[stack[--sp]];
            if (!segmentIntersectsAabb(from, to, node.bounds)) continue;
            if (node.count > 0) {
                for (int k = 0; k < node.count; ++k) {
                    const int i = bvhOrder_[node.leftFirst + k];
                    if (instanceOccludes(instances_[i], from, to)) return true;
                }
            } else if (sp + 2 <= 64) {
                stack[sp++] = node.leftFirst;
                stack[sp++] = node.leftFirst + 1;
            }
        }
        return false;
    }

    // isOccluded 縺ｨ蜷後§縺縺後∵欠螳壹う繝ｳ繧ｹ繧ｿ繝ｳ繧ｹ繧堤┌隕悶☆繧九ょ屓謚倅ｸｭ縺ｮ縲悟ｽ薙・邂ｱ縲阪ｒ髯､螟悶＠縺ｦ
    // 縲御ｻ悶・髫懷ｮｳ迚ｩ縺縺代阪〒 P 蛹ｺ髢薙・隕矩壹＠繧定ｦ九ｋ縺ｮ縺ｫ菴ｿ縺・ｼ域滋繧√ｋ轤ｹ竊帝浹貅舌′蠖薙・邂ｱ縺ｮ陬上↓
    // 蜀咲ｪ∝・縺励※閾ｪ蛻・〒閾ｪ蛻・ｒ驕ｮ繧九√ｒ髦ｲ縺撰ｼ峨Ｆxcept<0 縺ｯ isOccluded 縺ｨ蜷後§縲・    bool isOccludedExcept(const Vec3& from, const Vec3& to, int except) const {
        ensureBvh();
        if (bvhNodes_.empty()) return false;
        int stack[64];
        int sp = 0;
        stack[sp++] = 0;
        while (sp > 0) {
            const BvhNode& node = bvhNodes_[stack[--sp]];
            if (!segmentIntersectsAabb(from, to, node.bounds)) continue;
            if (node.count > 0) {
                for (int k = 0; k < node.count; ++k) {
                    const int i = bvhOrder_[node.leftFirst + k];
                    if (i == except) continue;
                    if (instanceOccludes(instances_[i], from, to)) return true;
                }
            } else if (sp + 2 <= 64) {
                stack[sp++] = node.leftFirst;
                stack[sp++] = node.leftFirst + 1;
            }
        }
        return false;
    }

    // 逶ｴ邱・from->to 縺碁壹ｋ螢√・蟶ｯ蝓溷挨騾城℃繧ｲ繧､繝ｳ(0..1)繧・outGain 縺ｫ譖ｸ縺上・VH 縺ｧ蜉騾溘・    //   螢√↑縺・竊・蜈ｨ蟶ｯ蝓・1.0 / 螢√ｒ騾壹ｋ縺ｻ縺ｩ・域攝雉ｪ谺｡隨ｬ縺ｧ鬮伜沺縺鯉ｼ牙ｰ上＆縺上↑繧九・    void computeTransmission(const Vec3& from, const Vec3& to,
                             float outGain[kNumBands]) const {
        for (int b = 0; b < kNumBands; ++b) outGain[b] = 1.0f;
        ensureBvh();
        if (bvhNodes_.empty()) return;
        int stack[64];
        int sp = 0;
        stack[sp++] = 0;
        while (sp > 0) {
            const BvhNode& node = bvhNodes_[stack[--sp]];
            if (!segmentIntersectsAabb(from, to, node.bounds)) continue;
            if (node.count > 0) {
                for (int k = 0; k < node.count; ++k) {
                    const int i = bvhOrder_[node.leftFirst + k];
                    const Instance& inst = instances_[i];
                    if (!instanceOccludes(inst, from, to)) continue;
                    const AcousticMaterial& m = materialOf(inst.materialId);
                    for (int b = 0; b < kNumBands; ++b) outGain[b] *= m.transmission[b];
                }
            } else if (sp + 2 <= 64) {
                stack[sp++] = node.leftFirst;
                stack[sp++] = node.leftFirst + 1;
            }
        }
    }

    // 笏笏 B: 繧ｭ繝･繝ｼ繝悶・繝・・ 繧ｨ繝・ず繧ｫ繧ｿ繝ｭ繧ｰ・・hase 4-B, [[dynamic-ray-architecture]] 鬆・・・笏笏
    // 蝗樊釜縺ｫ蜉ｹ縺上す繝ｫ繧ｨ繝・ヨ遞懃ｷ壹６TD 縺ｮ繧ｦ繧ｧ繝・ず蟷ｾ菴戊ｾｼ縺ｿ縲・    struct DiffEdge {
        Vec3 p0, p1;      // 遞懃ｷ壹・遶ｯ轤ｹ・医Ρ繝ｼ繝ｫ繝会ｼ・        Vec3 edgeDir;     // 蜊倅ｽ阪お繝・ず譁ｹ蜷・        Vec3 refTangent;  // 0髱｢謗･邱夲ｼ遺冠edge, 螟門髄縺阪６TD逕ｨ・・        float n;          // 繧ｦ繧ｧ繝・ず謖・焚・育ｮｱ=1.5・・        int instance;     // 縺薙・遞懃ｷ壹′螻槭☆繧九う繝ｳ繧ｹ繧ｿ繝ｳ繧ｹ・磯・阡ｽ蛻､螳壹・閾ｪ蟾ｱ髯､螟也畑縲・1=荳肴・・・    };

    // 繝ｪ繧ｹ繝翫・荳ｭ蠢・↓繧ｭ繝･繝ｼ繝悶・繝・・(6髱｢ﾃ羊esﾂｲ)縺ｧ繝ｬ繧､繧呈鋳縺阪・團謗･繧ｻ繝ｫ縺ｮ豺ｱ蠎ｦ荳埼｣邯壹ｒ
    // 繧ｷ繝ｫ繧ｨ繝・ヨ遞懃ｷ壹→縺励※諡ｾ縺｣縺ｦ繧ｫ繧ｿ繝ｭ繧ｰ蛹悶☆繧九・蝗樊鋳縺代・蜈ｨ髻ｳ貅舌〒蜈ｱ譛峨〒縺阪ｋ・医Μ繧ｹ繝翫・菫ら蕗・峨・    // res=髱｢隗｣蜒丞ｺｦ・井ｾ・6縲・2・峨［axDist=繝ｬ繧､蛻ｰ驕碑ｷ晞屬縲よｯ弱ヵ繝ｬ繝ｼ繝 or 菴弱Ξ繝ｼ繝医〒蜻ｼ縺ｶ縲・    void buildEdgeCatalog(const Vec3& listener, int res, float maxDist) const {
        edgeCatalog_.clear();
        if (instanceCount() == 0 || res < 2) return;
        const int F = 6, R = res;
        std::vector<float> depth(static_cast<size_t>(F) * R * R, 1e30f);
        std::vector<int> inst(static_cast<size_t>(F) * R * R, -1);
        std::vector<Vec3> hp(static_cast<size_t>(F) * R * R);
        for (int f = 0; f < F; ++f)
            for (int j = 0; j < R; ++j)
                for (int i = 0; i < R; ++i) {
                    const float u = (i + 0.5f) / R * 2.0f - 1.0f;
                    const float v = (j + 0.5f) / R * 2.0f - 1.0f;
                    const SceneHit h = raycastClosest(listener, cubeTexelDir(f, u, v), maxDist);
                    const int idx = (f * R + j) * R + i;
                    if (h.hit) { depth[idx] = h.t; inst[idx] = h.instanceId; hp[idx] = h.point; }
                }
        // 髫｣謗･繧ｻ繝ｫ・亥承繝ｻ荳九∝酔荳髱｢蜀・ｼ峨〒豺ｱ蠎ｦ縺碁｣帙・・昴す繝ｫ繧ｨ繝・ヨ縲りｿ代＞蛛ｴ縺ｮ繝偵ャ繝医ｒ遞懃ｷ夂せ縺ｫ縲・        auto consider = [&](int a, int b) {
            int nearIdx = -1;
            if (inst[a] >= 0 && inst[b] < 0) nearIdx = a;
            else if (inst[a] < 0 && inst[b] >= 0) nearIdx = b;
            else if (inst[a] >= 0 && inst[b] >= 0) {
                const float dmin = std::min(depth[a], depth[b]);
                if (std::fabs(depth[a] - depth[b]) > 0.15f * dmin)  // 逶ｸ蟇ｾ15%莉･荳翫・鬟帙・
                    nearIdx = (depth[a] < depth[b]) ? a : b;
            }
            if (nearIdx >= 0) addCatalogEdge(inst[nearIdx], hp[nearIdx]);
        };
        for (int f = 0; f < F; ++f)
            for (int j = 0; j < R; ++j)
                for (int i = 0; i < R; ++i) {
                    const int idx = (f * R + j) * R + i;
                    if (i + 1 < R) consider(idx, (f * R + j) * R + (i + 1));
                    if (j + 1 < R) consider(idx, (f * R + (j + 1)) * R + i);
                }
    }

    int edgeCatalogCount() const { return static_cast<int>(edgeCatalog_.size()); }
    void clearEdgeCatalog() const { edgeCatalog_.clear(); }

    // 縲仙屓謚倥・蛟呵｣懷・謖呻ｼ亥・譛会ｼ峨鷹・阡ｽ譎ゅ∝屓繧願ｾｼ縺ｿ蛟呵｣懊お繝・ず繧貞・謖吶＠縲∝推繧ｨ繝・ず縺ｧ縲梧滋繧√ｋ轤ｹ P
    // ・・rom竊単竊稚o 縺梧怙遏ｭ縺ｫ縺ｪ繧狗せ・昜ｸ牙・謗｢邏｢・峨阪→菴吝臆邨瑚ｷｯ ﾎｴ 繧偵∽ｸ｡蛹ｺ髢楢ｦ矩壹○繧九ｂ縺ｮ縺縺・    //   fn(P, delta, edgeDir, refT) 縺ｧ貂｡縺吶ゅお繝・ず繧ｫ繧ｿ繝ｭ繧ｰ縺後≠繧後・縺昴ｌ繧偵∫┌縺代ｌ縺ｰ邂ｱ遞懃ｷ壹ｒ菴ｿ縺・    //   ・医き繧ｿ繝ｭ繧ｰ縺ｧ譛牙柑蛟呵｣懊′ 0 縺ｪ繧臥ｮｱ縺ｸ繝輔か繝ｼ繝ｫ繝舌ャ繧ｯ・峨ょ腰荳譛遏ｭ(diffractionDetour)繧・    //   螟夐㍾蜷域・(diffractionComposite)繧ゅ％繧後ｒ蝨溷床縺ｫ縺吶ｋ縲・    template <class Fn>
    // blockerMargin: 繝悶Ο繝・き繝ｼ蛻､螳壹ｒ閹ｨ繧峨∪縺帙ｋ驥・m)縲・ 縺ｪ繧峨悟ｮ滄圀縺ｫ逶ｴ謗･邱壹ｒ蝪槭＄邂ｱ縲阪□縺代・    //   蠖ｱ蠅・阜縺ｮ螟門・・育・繧峨＆繧後◆鬆伜沺・峨〒繧ょ屓謚伜ｴ繧呈ｱゅａ縺溘＞蝣ｴ蜷医↓ > 0 繧呈ｸ｡縺吶・    //   縺昴％縺ｧ縺ｯ逶ｴ謗･邱壹′邂ｱ縺ｮ閼・ｒ縺九☆繧√※騾壹ｋ縺縺代↑縺ｮ縺ｧ縲∬・繧峨∪縺帙↑縺・→蛟呵｣懊′0譛ｬ縺ｫ縺ｪ繧九・    //   驕縺・お繝・ず繧呈鏡縺｣縺ｦ繧・UTD 縺ｮ邱丞粋繧ｲ繧､繝ｳ縺ｯ 1.0 縺ｫ蜿取據縺吶ｋ縺ｮ縺ｧ縲∝､壹ａ縺ｧ繧ょｮｳ縺ｯ縺ｪ縺・・    // requireBothEnds: false 縺ｪ繧峨掲rom 縺九ｉ隕矩壹○繧九阪□縺代ｒ譚｡莉ｶ縺ｫ縺吶ｋ・・o 蛛ｴ縺ｯ蝠上ｏ縺ｪ縺・ｼ峨・    //   2谺｡蝗樊釜縺ｮ 1 谿ｵ逶ｮ縺ｧ菴ｿ縺・ら悄縺ｮ2谺｡蝗樊釜縺ｧ縺ｯ縲√←縺ｮ遞懃ｷ壹ｂ from 縺ｨ to 縺ｮ**荳｡譁ｹ**縺九ｉ縺ｯ
    //   隕矩壹○縺ｪ縺・笏笏 縺昴ｌ縺後∪縺輔↓縲・谺｡縺ｧ縺励°螻翫°縺ｪ縺・阪→縺・≧縺薙→縺ｪ縺ｮ縺ｧ縲∽ｸ｡譁ｹ繧定ｦ∵ｱゅ☆繧九→
    //   蛟呵｣懊′ 0 譛ｬ縺ｫ縺ｪ繧翫∝・蟶ｰ縺ｫ蜈･繧句燕縺ｫ繝ｫ繝ｼ繝励′遨ｺ縺ｫ縺ｪ繧九・    void forEachDiffractionCandidate(const Vec3& from, const Vec3& to, Fn&& fn,
                                     float blockerMargin = 0.0f,
                                     bool requireBothEnds = true) const {
        const float direct = std::max(length(to - from), 1e-4f);
        // 笘・ｨ懃ｷ壹・螳溷ｽ｢迥ｶ縺ｮ荳翫↓鄂ｮ縺上ゆｻ･蜑阪・ 0.15m 閹ｨ繧峨∪縺帙※縺・◆縺後√◎繧後□縺ｨ UTD 縺瑚ｦ九ｋ蠖ｱ蠅・阜縺・        //   螳滄圀縺ｮ蠖ｱ蠅・阜縺九ｉ縺壹ｌ繧具ｼ医％縺ｮ霍晞屬縺縺大屓謚倡せ縺悟虚縺上・縺ｧ・峨６TD 縺ｯ縲檎峩謗･髻ｳ縺梧ｶ医∴繧狗せ縲阪→
        //   縲悟屓謚伜ｴ縺檎ｬｦ蜿ｷ蜿崎ｻ｢縺吶ｋ轤ｹ縲阪′荳閾ｴ縺吶ｋ縺薙→縺ｧ騾｣邯壹↓縺ｪ繧倶ｻ慕ｵ・∩縺ｪ縺ｮ縺ｧ縲∽ｸ｡閠・′縺壹ｌ繧九→
        //   縺昴・蛹ｺ髢薙□縺大､縺悟｣翫ｌ繧具ｼ亥ｮ滓ｸｬ縺ｧ 3.8dB 縺ｮ谿ｵ蟾ｮ縺悟・縺ｦ縺・◆蛹ｺ髢薙→荳閾ｴ・峨・        //   荳譁ｹ縺ｧ 0 縺ｫ繧ゅ〒縺阪↑縺・ょ屓謚倡せ繧帝壹ｋ邨瑚ｷｯ縺ｯ邂ｱ縺ｮ髱｢繧偵梧滋繧√ｋ縲阪・縺ｧ縲・擇縺ｫ豐ｿ縺｣縺ｦ襍ｰ繧九・        //   遞懃ｷ壹′陦ｨ髱｢縺ｴ縺｣縺溘ｊ縺縺ｨ縲√◎縺ｮ邨瑚ｷｯ縺瑚・蛻・・邂ｱ縺ｨ謗･隗ｦ縺励◆縺ｨ蛻､螳壹＆繧後※縺励∪縺・        //   ・域磁隗ｦ縺ｯ蝗樊釜轤ｹ縺ｮ霑大ｍ縺ｧ縺ｯ縺ｪ縺冗ｵ瑚ｷｯ蜈ｨ菴薙↓蛻・ｸ・☆繧九・縺ｧ縲∫ｫｯ轤ｹ繧貞ｼ輔￥縺縺代〒縺ｯ驕ｿ縺代ｉ繧後↑縺・ｼ峨・        //   竊・髱｢縺九ｉ遒ｺ螳溘↓髮｢繧後ｋ譛蟆城剞縺縺大､悶∈蜃ｺ縺吶ょ燕蟾昴・蠑上・ ﾎｴ 縺ｮ縺ｿ縺ｫ萓晏ｭ倥☆繧九・縺ｧ縲・        //     縺薙・遞句ｺｦ縺ｮ縺壹ｌ縺ｧ縺ｯ騾｣邯壽ｧ縺ｯ螢翫ｌ縺ｪ縺・ｼ・TD 縺ｯ蠖ｱ蠅・阜縺ｮ菴咲ｽｮ縺後★繧後※螢翫ｌ縺ｦ縺・◆・峨・        const float margin = 0.02f;   // 2cm

        // 隱ｿ譟ｻ逕ｨ縺ｮ譽・唆繧ｫ繧ｦ繝ｳ繧ｿ・・F_DIFF_DEBUG 縺ｮ縺ｨ縺阪□縺大・縺呻ｼ峨ゅ←縺ｮ蛻､螳壹′蛟呵｣懊ｒ豸医＠縺ｦ縺・ｋ縺九・        int dbgRej[7] = {0, 0, 0, 0, 0, 0, 0};
        int dbgBlockers = 0, dbgEdges = 0, dbgEmit = 0;

        // 遞懃ｷ・A-B 荳翫〒 from竊単竊稚o 縺梧怙遏ｭ縺ｫ縺ｪ繧狗せ・茨ｼ晉峩邱壹′謗繧√ｋ隗抵ｼ会ｼ狗ｫｯ轤ｹ縺九ｉ縲∽ｸ｡蛹ｺ髢楢ｦ矩壹○繧・        // 譛遏ｭ繧・縺､驕ｸ繧薙〒 fn 縺ｫ貂｡縺吶Ｈ(t)=|from-P|+|P-to| 縺ｯ蜊伜ｳｰ縺ｪ縺ｮ縺ｧ荳牙・謗｢邏｢縺ｧ譛蟆冗せ繧貞ｾ励ｋ縲・        // exceptInst = 縺薙・遞懃ｷ壹′螻槭☆繧狗ｮｱ・・蛹ｺ髢薙・驕ｮ阡ｽ蛻､螳壹°繧芽・蟾ｱ髯､螟悶☆繧九よ滋繧√ｋ轤ｹ竊帝浹貅舌′
        // 蠖薙・邂ｱ縺ｮ陬上↓蜀咲ｪ∝・縺励※閾ｪ蛻・〒閾ｪ蛻・ｒ驕ｮ繧具ｼ晏呵｣懈ｶ域ｻ・・荳ｭ螟ｮ豁ｻ隗偵√ｒ髦ｲ縺撰ｼ峨・        auto tryEdge = [&](const Vec3& A, const Vec3& B, const Vec3& edgeDir, const Vec3& refT,
                           int exceptInst) -> bool {
            auto Pf = [&](float t) { return A + (B - A) * t; };
            auto g  = [&](float t) { const Vec3 p = Pf(t); return length(p - from) + length(to - p); };
            float lo = 0.0f, hi = 1.0f;
            for (int it = 0; it < 20; ++it) {
                const float m1 = lo + (hi - lo) * (1.0f / 3.0f);
                const float m2 = hi - (hi - lo) * (1.0f / 3.0f);
                if (g(m1) < g(m2)) hi = m2; else lo = m1;
            }
            // 蝗樊釜轤ｹ P 縺九ｉ from / to 縺ｮ荳｡譁ｹ繧定ｦ矩壹○繧九°縲・            //
            //   笘・ｽ薙・邂ｱ(exceptInst)縺ｯ縲碁勁螟悶阪〒縺ｯ縺ｪ縺上瑚ｲｫ騾夐聞縺ｧ蛻､螳壹阪☆繧九・            //     莉･蜑阪・蠖薙・邂ｱ繧貞愛螳壹°繧我ｸｸ縺斐→螟悶＠縺ｦ縺・◆縲ょ字縺ｿ縺ｮ縺ゅｋ螢√・遞懃ｷ壹ｒ蝗槭ｋ邨瑚ｷｯ縺ｯ縲・            //     蟷ｾ菴慕噪縺ｫ蠢・★縺昴・螢√・蜴壹∩縺ｶ繧薙ｒ雋ｫ縺上・縺ｧ縲∝腰邏斐↓縲御ｺ､蟾ｮ・晉┌蜉ｹ縲阪↓縺吶ｋ縺ｨ
            //     豁｣縺励＞蝗樊釜邨瑚ｷｯ縺ｾ縺ｧ豸医∴縺ｦ縺励∪縺・°繧峨〒縺ゅｋ縲・            //     縺励°縺嶺ｸｸ縺斐→髯､螟悶・荵ｱ證ｴ縺吶℃縺溘ょ､ｧ縺阪↑螢√〒縺ｯ縲後◎縺ｮ螢√ｒ菴輔Γ繝ｼ繝医Ν繧らｪ√″謚懊￠縺ｦ
            //     驕縺・ｨ懃ｷ壹↓驕斐☆繧狗ｵ瑚ｷｯ縲阪∪縺ｧ隕矩壹○繧九→蛻､螳壹＆繧後・浹縺碁壹ｉ縺ｪ縺・ｴ謇縺碁幕蜿｣縺ｨ縺励※
            //     隱崎ｭ倥＆繧後ｋ・亥ｮ滓ｩ溘〒縲∝｣√・陬上・隕句ｽ馴＆縺・↑譁ｹ蜷代↓蝗樊釜邨瑚ｷｯ縺檎函縺医※縺・◆・峨・            //     竊・荳｡閠・ｒ蛻・￠繧九・縺ｯ縲碁壹ｋ縺句凄縺九阪〒縺ｯ縺ｪ縺上後←繧後□縺鷹壹ｋ縺九阪・            //       邂ｱ縺ｮ縺・■縺ｰ繧楢埋縺・婿蜷代ｒ騾壹ｊ謚懊￠繧九・繧薙∪縺ｧ縺ｯ縲悟屓繧願ｾｼ縺ｿ縲阪→縺励※險ｱ縺励・            //       縺昴ｌ繧定ｶ・∴繧玖ｲｫ騾壹・縲悟｣√ｒ遯√▲蛻・▲縺ｦ縺・ｋ縲阪→縺励※譽・唆縺吶ｋ縲・            //
            //   笘・・縺代ｋ縺ｮ縺ｯ雋ｫ騾壹・縲碁㍼縲阪〒縺ｯ縺ｪ縺上悟ｴ謇縲阪・            //     驥上〒隕九ｋ縺ｨ隧ｰ繧: 險ｱ螳ｹ繧貞字縺ｿ縺ｶ繧・thinnest+0.05)縺ｫ縺吶ｋ縺ｨ**阮・＞譚ｿ縺碁乗・**縺ｫ縺ｪ繧翫・            //     蜴壹∩ 6cm 縺ｮ謇峨ｒ遯√″謚懊￠繧狗ｵ瑚ｷｯ縺悟粋豕輔↓縺ｪ縺｣縺ｦ謇峨′髻ｳ髻ｿ逧・↓蟄伜惠縺励↑縺上↑繧九・            //     縺九→縺・▲縺ｦ蝗ｺ螳壹・蟆上＆縺・､縺ｫ縺吶ｋ縺ｨ縲∝字縺・｡晉ｫ九・隗偵ｒ蝗槭ｋ豁｣蠖薙↑邨瑚ｷｯ縺梧ｶ医∴繧・            //     ・亥ｮ滓ｸｬ: 6cm 蝗ｺ螳壹〒陦晉ｫ九・蝗樊釜邨瑚ｷｯ縺ｨ髢句哨蛛ｴ縺ｮ螳壻ｽ阪′螢翫ｌ縺滂ｼ峨・            //     荳｡閠・・蜷後§雋ｫ騾夐聞縺ｧ繧・*雋ｫ騾壹☆繧句ｴ謇**縺碁＆縺・
            //       隗偵ｒ謗繧√ｋ豁｣蠖薙↑邨瑚ｷｯ 窶ｦ 遞懃ｷ壹・縺吶＄霑代￥縺縺代ｒ雋ｫ縺擾ｼ域滋繧√ｋ縺ｮ縺ｧ蠖鍋┯・・            //       譚ｿ繧堤ｪ√″謚懊￠繧句⊃縺ｮ邨瑚ｷｯ 窶ｦ 髱｢縺ｮ逵溘ｓ荳ｭ繧偵∵攸縺ｮ蟷・＞縺｣縺ｱ縺・↓雋ｫ縺・            //     謗繧√′莨ｸ縺ｳ繧玖ｷ晞屬縺ｯ邂ｱ縺ｮ蜴壹∩縺ｮ遞句ｺｦ縺ｪ縺ｮ縺ｧ縲√◎縺薙ｒ蝓ｺ貅悶↓霑大ｍ繧呈ｱｺ繧√ｋ縲・            const Obb& selfObb = instances_[exceptInst].obb;
            const float thinnest = 2.0f * std::min(selfObb.halfExtents.x,
                                        std::min(selfObb.halfExtents.y, selfObb.halfExtents.z));
            const float edgeNear = thinnest * 2.0f + 0.05f;
            const Vec3 dirTravel = normalized(to - from);
            // 笘・梧滋繧√ｋ縲阪→縲碁壹ｊ謚懊￠繧九阪ｒ**蟷ｾ菴輔〒**蛻・￠繧九る明蛟､繧堤ｽｮ縺九↑縺・・            //     謗繧√ｋ    窶ｦ 迚ｩ菴薙・螟悶ｒ蝗槭ｋ縲ゆｸｭ蠢・擇繧呈ｨｪ蛻・ｉ縺ｪ縺・°縲∵ｨｪ蛻・▲縺ｦ繧よ妙髱｢縺ｮ螟・            //     騾壹ｊ謚懊￠繧銀ｦ 譁ｭ髱｢縺ｮ**荳ｭ**縺ｧ荳ｭ蠢・擇繧呈ｨｪ蛻・ｋ
            //   菴咲ｽｮ繧・聞縺輔・髢ｾ蛟､縺ｧ縺ｯ蛹ｺ蛻･縺ｧ縺阪↑縺・ｼ磯哩縺倥◆謇峨・蟷ｻ縺ｯ螢√・隗偵ｒ 4.5cm 謗繧√・            //   陦晉ｫ九・豁｣蠖薙↑邨瑚ｷｯ縺ｯ 14cm 謗繧√ｋ縲る聞縺輔〒蛻・ｋ縺ｨ蠕瑚・′豁ｻ縺ｬ 笏笏 螳滄圀縺ｫ豁ｻ繧薙□・峨・            //   荳ｭ蠢・擇縺ｯ譚ｿ縺ｮ**譛繧り埋縺・ｻｸ**縺ｫ蝙ら峩縺ｪ髱｢縲よ攸繧定ｲｫ縺上→縺ｯ縺昴％繧呈妙髱｢蜀・〒雜翫∴繧九％縺ｨ縲・            const Vec3 selfAxes[3] = {selfObb.axisX, selfObb.axisY, selfObb.axisZ};
            const float selfHalf[3] = {selfObb.halfExtents.x, selfObb.halfExtents.y,
                                       selfObb.halfExtents.z};
            int thinAxis = 0;
            for (int k = 1; k < 3; ++k) if (selfHalf[k] < selfHalf[thinAxis]) thinAxis = k;
            // 笘・Γ繝・す繝･縺ｫ縺ｯ菴ｿ縺医↑縺・０BB 縺ｯ蠅・阜邂ｱ縺ｧ縺励°縺ｪ縺上√◎縺ｮ縲御ｸｭ蠢・擇縲阪・
            //   螳溷ｽ｢迥ｶ縺ｨ蟇ｾ蠢懊＠縺ｪ縺・ｼ域虻蜿｣縺ｮ遨ｺ縺・◆繝｡繝・す繝･螢√↑繧峨∫ｩｴ縺ｮ逵溘ｓ荳ｭ繧帝壹ｋ
            //   豁｣蠖薙↑邨瑚ｷｯ縺ｾ縺ｧ縲梧攸繧定ｲｫ縺・◆縲阪→蛻､螳壹＆繧後※蝗樊釜縺悟・驛ｨ豸医∴繧具ｼ峨・            //   繝｡繝・す繝･縺ｯ螳溷ｽ｢迥ｶ縺ｧ驕ｮ阡ｽ蛻､螳壹＆繧後ｋ縺ｮ縺ｧ縲√％縺ｮ陬懷勧蛻､螳壹・隕√ｉ縺ｪ縺・・            const bool selfIsMesh = instances_[exceptInst].geomId >= 0;
            // 笘・・ 蟷ｻ縺ｮ邨瑚ｷｯ縺ｯ縲・轤ｹ縺ｧ蝗樊釜縺吶ｋ縲肴ｨ｡蝙九〒縺ｯ蜴溽炊逧・↓蛻・屬縺ｧ縺阪↑縺・笘・・
            //   髢画演縺ｧ繝ｪ繧ｹ繝翫・繧呈万繧√↓鄂ｮ縺上→縲∵虻蜿｣縺ｮ**螂･蛛ｴ**縺ｮ邵√∈螢√ｒ雋ｫ騾壹＠縺ｦ螻翫￥
            //   蛛ｽ縺ｮ邨瑚ｷｯ縺悟呵｣懊↓縺ｪ繧具ｼ・(-0.58,1.60,0.17)縲∝｣√・荳ｭ繧・27.7cm 騾ｲ繧・峨・            //   縺薙ｌ繧呈ｶ医◎縺・→縺励※4騾壹ｊ隧ｦ縺励∝・驛ｨ**豁｣蠖薙↑邨瑚ｷｯ縺ｾ縺ｧ谿ｺ縺励◆**:
            //     繝ｻ雋ｫ騾壹・髟ｷ縺輔〒蛻・ｋ        8cm 縺ｧ陦晉ｫ九・蝗槭ｊ霎ｼ縺ｿ(14cm)縺梧ｭｻ縺ｬ
            //     繝ｻ譁ｭ髱｢縺ｮ蜀・・縺ｧ荳ｭ蠢・擇繧呈ｨｪ蛻・▲縺溘ｉ譽・唆  3cm 縺ｧ4莉ｶ豁ｻ縺ｫ縲・cm 縺ｧ蟷ｻ縺悟ｾｩ豢ｻ
            //     繝ｻ閾ｪ蛻・・螳滉ｽ薙・髯､螟悶ｒ遞懃ｷ夊ｿ大ｍ縺縺代↓    6cm 縺ｧ謗繧√′豁ｻ縺ｫ縲・5cm 縺ｧ縺ｯ
            //                                          螢√・謇句燕縺ｧ蛻・ｌ縺ｦ讀懈渊縺ｫ縺ｪ繧峨↑縺・            //     繝ｻmargin 縺縺醍ｸｮ繧√◆縲瑚官縲阪ｒ讓ｪ蛻・▲縺溘ｉ譽・唆  5莉ｶ豁ｻ縺ｬ
            //
            //   譛蠕後・螳滓ｸｬ縺ｧ逅・罰縺悟愛譏弱＠縺溘ょ字縺・｡晉ｫ・0.4m)繧剃ｸ翫°繧牙屓繧願ｾｼ繧**豁｣蠖薙↑**邨瑚ｷｯ縺ｯ縲・            //   荳企擇縺ｫ豐ｿ縺｣縺ｦ謗繧√ｋ縺ｮ縺ｧ蠢・★螳滉ｽ薙・荳ｭ繧帝壹ｋ:
            //     蝗樊釜轤ｹ P(0, 4.02, -0.22) 竊・髻ｳ貅・0,1.6,4) 縺ｮ邱壹・荳ｭ蠢・擇 z=0 縺ｧ y=3.894縲・            //     陦晉ｫ九・荳顔ｫｯ縺ｯ y=4.0 縺ｪ縺ｮ縺ｧ縲・*荳ｭ繧帝壹▲縺ｦ縺・ｋ**縲・            //   蟷ｻ繧ょｮ滉ｽ薙・荳ｭ繧帝壹ｋ縲・*蜷後§諤ｧ雉ｪ縺ｪ縺ｮ縺ｧ蟷ｾ菴暮㍼縺ｧ縺ｯ蛻・￠繧峨ｌ縺ｪ縺・・*
            //
            //   蛻・屬縺ｫ縺ｯ邨瑚ｷｯ縺・**2蝗樊峇縺後ｋ** 縺薙→縺瑚ｦ√ｋ・郁ｿ代＞邵・竊・驕縺・ｸ・ｼ峨・            //   蜴壹＞髫懷ｮｳ迚ｩ縺ｮ蝗槭ｊ霎ｼ縺ｿ縺ｯ譛ｬ譚･縺昴≧縺・≧蠖｢縺ｧ縲・轤ｹ霑台ｼｼ縺碁剞逡後ｒ菴懊▲縺ｦ縺・ｋ縲・            //
            //   竊・縺昴％縺ｧ 1 谺｡縺ｫ縺ｯ縲・*闃ｯ繧呈ｨｪ蛻・▲縺溘ｉ譽・唆**縲阪ｒ隱ｲ縺吶・            //     闃ｯ = 螳滉ｽ薙ｒ margin 縺縺醍ｸｮ繧√◆繧ゅ・縲Ｎargin 縺ｮ谿ｻ縺ｯ謗繧√・險ｱ螳ｹ縺ｪ縺ｮ縺ｧ縲・            //     縺昴％繧帝壹ｋ縺ｮ縺ｯ豁｣蠖薙∬官繧呈ｨｪ蛻・ｋ縺ｮ縺ｯ騾壹ｊ謚懊￠縲・            //     縺薙ｌ縺ｧ蟷ｻ繧ょ字縺・｡晉ｫ九・1轤ｹ霑台ｼｼ繧ょ酔譎ゅ↓豸医∴繧九・            //     蜴壹＞陦晉ｫ九・豁｣蠖薙↑蝗槭ｊ霎ｼ縺ｿ縺ｯ **2 谺｡**・郁ｿ代＞邵≫・驕縺・ｸ・ｼ峨′閧ｩ莉｣繧上ｊ縺吶ｋ縲・            //   谿ｻ縺ｮ蜴壹＆縺ｯ **margin 蝗ｺ螳壹〒縺ｯ雜ｳ繧翫↑縺・*縲よ滋繧√′螳滉ｽ薙↓鬟溘＞霎ｼ繧豺ｱ縺輔・
            //   髫懷ｮｳ迚ｩ縺ｮ蜴壹∩縺ｫ豈比ｾ九＠縺ｦ莨ｸ縺ｳ繧九・縺ｧ・亥字 200mm 繧定ｶ・∴繧九→ 2cm 縺ｮ谿ｻ繧・            //   遯√″謚懊￠縺ｦ蛟呵｣懊′ 0 縺ｫ縺ｪ縺｣縺溘ょｮ滓ｸｬ・峨∝字縺ｿ縺ｮ蜑ｲ蜷医〒繧ら｢ｺ菫昴☆繧九・            //   笘・ｩｦ縺励※鬧・岼縺縺｣縺溘ｂ縺ｮ・・ 蝗樒岼・・ 谿ｻ繧・*霆ｸ縺斐→縺ｫ螟峨∴繧・*縲・            //     蟷ｻ縺後←縺薙ｒ騾壹▲縺ｦ縺・ｋ縺九・迚ｹ螳壹〒縺阪◆ 笏笏 3 霆ｸ繧貞酔縺倥□縺醍ｸｮ繧√ｋ縺帙＞縺ｧ縲・            //     遞懃ｷ壹↓豐ｿ縺｣縺ｦ蟷・thinnest/2 縺ｮ蟶ｯ縺後瑚ｲｫ騾壹＠謾ｾ鬘後阪↓縺ｪ縺｣縺ｦ縺・ｋ:
            //       陲門｣・x竏・-6,-0.6]縲∝字縺ｿ 0.3 竊・闃ｯ縺ｮ蜀・ｸ√・ x=-0.75・亥・邵√°繧・15cm・・            //       蟷ｻ縺ｯ z=0 繧・x=-0.638 縺ｧ讓ｪ蛻・ｋ・晏ｸｯ縺ｮ荳ｭ縺ｪ縺ｮ縺ｧ闃ｯ蛻､螳壹↓謗帙°繧峨★縲・            //       蜴壹∩ 30cm 繧定ｲｫ縺・※螂･蛛ｴ縺ｮ邵・P(-0.58,1.60,0.17) 縺ｫ驕斐＠縺ｦ縺・◆縲・            //     縺昴％縺ｧ縲瑚埋縺・ｻｸ縺縺大字縺ｿ豈比ｾ九・擇蜀・・蟆上＆縺上阪ｒ隧ｦ縺励・擇蜀・・蛟､繧定ｵｰ譟ｻ縺励◆:
            //       髱｢蜀・0.02(margin) 窶ｦ 蟷ｻ縺ｯ**豸医∴繧・* 縺梧ｭ｣蠖薙↑邨瑚ｷｯ縺・4 莉ｶ豁ｻ縺ｬ
            //       髱｢蜀・0.05        窶ｦ 蟷ｻ縺悟ｾｩ豢ｻ縲√°縺､ 4 莉ｶ豁ｻ繧薙□縺ｾ縺ｾ
            //       髱｢蜀・0.08        窶ｦ 蟷ｻ縺ゅｊ縲・ 莉ｶ豁ｻ縺ｬ
            //       髱｢蜀・0.12        窶ｦ 蟷ｻ縺ゅｊ縲・ 莉ｶ豁ｻ縺ｬ
            //       髱｢蜀・thinnest/2  窶ｦ 蟷ｻ縺ゅｊ縲∽ｻ悶・蜈ｨ驛ｨ逕溘″繧具ｼ茨ｼ晉樟迥ｶ・・            //     **荳｡遶九☆繧狗ｪ薙′辟｡縺・*縲ょｹｻ繧呈ｮｺ縺帙ｋ蛟､縺ｧ縺ｯ蠢・★豁｣蠖薙↑邨瑚ｷｯ縺梧ｭｻ縺ｬ縲・            //     蟷ｾ菴暮㍼縺ｧ縺ｯ蛻・￠繧峨ｌ縺ｪ縺・√→縺・≧譌｢蟄倥・邨占ｫ悶′蛻･譁ｹ蜷代°繧峨ｂ陬丈ｻ倥￠繧峨ｌ縺溘・            //     谿九ｋ遲九・縲悟･･蛛ｴ縺ｮ邵√ｒ 1 谺｡縺ｧ逶ｴ謗･菴ｿ繧上○縺ｪ縺・阪→縺・≧繝医・繝ｭ繧ｸ縺ｮ蛻､螳・            //     ・井ｸ九・譛ｪ隗｣豎ｺ繝｡繝｢繧貞盾辣ｧ・峨・            const float shell = std::max(margin, thinnest * 0.5f);
            // 笘・擇蜀・・谿ｻ繧・margin(2cm) 縺ｾ縺ｧ邱繧√ｋ縺ｨ蟷ｻ縺ｯ豸医∴繧九′縲・*蜴壹＞陦晉ｫ九ｒ荳翫°繧・            //   蝗槭ｊ霎ｼ繧豁｣蠖薙↑邨瑚ｷｯ**縺梧ｭｻ縺ｬ・亥屓謚倡せ (0,4.02,ﾂｱ0.22) 竊・髻ｳ貅舌・邱壹′
            //   荳ｭ蠢・擇 z=0 繧・y=3.89 ・・邵√°繧・0.11m 蜀・・縺ｧ讓ｪ蛻・ｋ縺溘ａ・峨・            //   蟷ｻ縺ｮ讓ｪ蛻・ｊ縺ｯ邵√°繧・0.038m 縺ｧ縲・*蟷ｻ縺ｮ譁ｹ縺檎ｸ√↓霑代＞**縲りｷ晞屬縺ｧ縺ｯ蛻・屬荳崎・縲・            //   竊・蛻・屬縺ｯ縲檎ｸ√・蜷代％縺・′髢九＞縺ｦ縺・ｋ縺九阪・蛻､螳夲ｼ井ｸ九・霍ｨ縺守ｷ壼・・峨′諡・≧縲・            const float shellPlane = shell;
            const Obb coreObb = [&] {
                Obb o = selfObb;
                float hs[3] = {o.halfExtents.x, o.halfExtents.y, o.halfExtents.z};
                for (int k = 0; k < 3; ++k)
                    hs[k] = std::max(hs[k] - (k == thinAxis ? shell : shellPlane), 1e-3f);
                o.halfExtents = Vec3(hs[0], hs[1], hs[2]);
                return o;
            }();
            auto crossesCore = [&](const Vec3& a, const Vec3& c) {
                if (selfIsMesh) return false;   // 蠅・阜邂ｱ縺ｮ闃ｯ縺ｯ螳溷ｽ｢迥ｶ縺ｨ蟇ｾ蠢懊＠縺ｪ縺・                float t0, t1;
                return segmentObbPenetrationSpan(a, c, coreObb, t0, t1);
            };
            // 雋ｫ騾壼玄髢薙′蝗樊釜轤ｹ p 縺ｮ霑大ｍ縺ｫ蜿弱∪縺｣縺ｦ縺・ｋ縺具ｼ区攸繧定ｲｫ縺・※縺・↑縺・°縲・            //
            // 笘・ｨｱ螳ｹ繧・*莠悟､縺ｫ縺励↑縺・*縲りｿ斐☆縺ｮ縺ｯ 0縲・ 縺ｮ驥阪∩縲・            //   謗繧√・豺ｱ縺包ｼ磯｣邯夐㍼・峨↓髢ｾ蛟､ edgeNear 繧堤ｽｮ縺・※蜿ｯ蜷ｦ繧呈ｱｺ繧√※縺・◆縺ｮ縺ｧ縲・            //   繝ｪ繧ｹ繝翫・縺悟ｰ代＠蜍輔＞縺ｦ髢ｾ蛟､繧定ｷｨ縺・□迸ｬ髢薙↓蛟呵｣懊′逕溘∪繧後◆繧頑ｶ医∴縺溘ｊ縺励※縺・◆縲・            //   螳滓ｸｬ・亥屓謚倥□縺代す繝ｼ繝ｳ縲・幕蜿｣縺ｮ謇句燕蛛ｴ縺ｮ邵ｦ遞懃ｷ・x=2.02・・
            //     繝ｪ繧ｹ繝翫・ x=-7.0  謗繧・0.645m 竕･ 0.65 竊・蜈ｨ蝓滉ｸ榊庄隕厄ｼ亥呵｣懊ぞ繝ｭ・・            //     繝ｪ繧ｹ繝翫・ x=-6.0  謗繧・0.594m < 0.65 竊・謗｡逕ｨ・域ｺ鬘搾ｼ・            //   1m 蜍輔＞縺溘□縺代〒謾ｯ驟咲ｵ瑚ｷｯ縺悟・繧梧崛繧上ｊ縲∫ｵ瑚ｷｯ髟ｷ縺・2.65m 霍ｳ繧薙〒縺・◆縲・            //   縺薙ｌ縺ｯ螳滓ｩ溘〒譛ｬ莠ｺ縺瑚◇縺・◆縲梧ｭｩ縺上→蝗樊釜縺梧ｶ医∴繧具ｼ城浹縺梧ｰ玲戟縺｡謔ｪ縺・阪→蜷後§莉慕ｵ・∩縲・            //   豺ｱ縺・⊇縺ｩ 0 縺ｸ貊代ｉ縺九↓關ｽ縺ｨ縺帙・縲∫函縺ｾ繧後ｋ迸ｬ髢薙・驥阪∩縺・0 縺ｪ縺ｮ縺ｧ霍ｳ縺ｰ縺ｪ縺・・            //   險育ｮ鈴㍼縺ｯ蜑阪→蜷後§・亥酔縺倩ｷ晞屬繧呈ｸｬ縺｣縺ｦ豈碑ｼ・・莉｣繧上ｊ縺ｫ蜀吝ワ縺吶ｋ縺縺托ｼ峨・            auto penNearWeight = [&](const Vec3& a, const Vec3& c, const Vec3& p) -> float {
                if (crossesCore(a, c)) return 0.0f;   // 闃ｯ繧呈ｨｪ蛻・▲縺滂ｼ晄滋繧√〒縺ｯ縺ｪ縺・                float t0, t1;
                if (!segmentObbPenetrationSpan(a, c, selfObb, t0, t1)) return 1.0f;  // 雋ｫ騾壹↑縺・                const Vec3 d = c - a;
                const float far = std::max(length(a + d * t0 - p), length(a + d * t1 - p));
                // 笘・ｩｦ縺励◆縺・*1轤ｹ繧ょ､峨ｏ繧峨↑縺九▲縺・*繧ゅ・: 險ｱ螳ｹ繧貞・蟆・ｧ偵〒莨ｸ縺ｰ縺・                //   ・亥字縺ｿ t 繧定ｧ貞ｺｦ ﾎｸ 縺ｧ讓ｪ蛻・ｌ縺ｰ髱｢縺ｫ豐ｿ縺・ｵｰ繧翫・ t/sinﾎｸ縲√→縺・≧逅・ｱ医〒
                //    edgeNear 繧・thinnest*2/sinﾎｸ+0.05 縺ｫ縺吶ｋ・峨・                //   縲碁屬繧後ｋ縺ｨ遞懃ｷ壹′謗｢邏｢縺ｧ縺阪↑縺上↑繧九・縺ｧ縺ｯ縲阪ｒ逍代▲縺ｦ蜈･繧後◆縺後∝ｮ滓ｸｬ縺ｧ縺ｯ
                //   蝗ｺ螳壼､縺ｨ螳悟・縺ｫ蜷御ｸ縺ｮ邨先棡縺縺｣縺・
                //     讓ｪ 18m・亥・蟆・9ﾂｰ・峨∪縺ｧ蛻ｰ驕斐・Κ螻九・譬ｼ蟄舌ｂ 10/585 縺ｧ蜷後§縲∝屓蟶ｰ繧ょ酔縺倥・                //   髮｢繧後※豁ｻ繧薙〒縺・ｋ縺ｮ縺ｧ縺ｯ縺ｪ縺・・縺ｧ縲√％縺ｮ譁ｹ蜷代・謇句ｽ薙※縺ｯ隕√ｉ縺ｪ縺・・                //   谿九ｋ閼ｱ關ｽ縺ｯ莉募・繧翫°繧・0.35縲・.6m 縺ｮ蟶ｯ縺縺代〒縲∝次蝗縺ｯ crossesCore 縺ｮ譁ｹ
                //   ・亥屓謚倡せ縺檎ｮｱ縺ｮ隗・z=ﾂｱ0.17 縺ｫ縺励°鄂ｮ縺代↑縺・◆繧√√◎縺薙°繧蛾浹貅舌∈蠑輔＞縺溽ｷ壹′
                //    譚ｿ縺ｮ荳ｭ蠢・擇繧呈妙髱｢縺ｮ蜀・・縺ｧ讓ｪ蛻・ｋ・峨ょ字縺・｣√ｒ1轤ｹ縺ｧ蝗樊釜縺輔○繧区ｨ｡蝙九・髯千阜縲・                if (far >= edgeNear) return 0.0f;
                const float x = 1.0f - far / edgeNear;
                return x * x * (3.0f - 2.0f * x);     // smoothstep・育ｫｯ縺ｧ蛯ｾ縺阪ｂ 0・・            };
            auto vis = [&](float t) {
                const Vec3 p = Pf(t);
                // 笘・・繧峨∪縺帙◆轤ｹ縺・*蛻･縺ｮ螳滉ｽ薙・荳ｭ**縺ｪ繧峨√◎縺薙・髢句哨縺ｧ縺ｯ縺ｪ縺・・                //   遞懃ｷ壹・髱｢縺九ｉ 2cm 螟悶∈蜃ｺ縺励※縺ゅｋ・磯擇縺吶ｌ縺吶ｌ縺ｮ邨瑚ｷｯ繧呈鏡縺・◆繧√↓蠢・ｦ・ｼ峨・                //   縺昴・縺溘ａ縲∵演縺ｮ閾ｪ逕ｱ遶ｯ縺梧虻蜿｣縺ｮ譫縺ｫ蟇・捩縺励※縺・※繧・2cm 縺ｶ繧薙・髫咎俣縺・                //   髢九＞縺ｦ隕九∴縲・哩縺倥◆謇峨↓蟷ｻ縺ｮ邨瑚ｷｯ縺檎函縺医※縺・◆
                //   ・亥ｮ滓ｸｬ: 髢画演縺ｧ ﾎｴ 縺・0.05m ・・蠕蠕ｩ 2cm 縺ｶ繧薙＠縺狗┌縺九▲縺滂ｼ峨・                //   陦晉ｫ九・蠖ｱ蠅・阜縺ｧ縺ｯ閹ｨ繧峨∪縺帙◆轤ｹ縺ｯ遨ｺ荳ｭ縺ｪ縺ｮ縺ｧ縲√％縺ｮ蛻､螳壹↓謗帙°繧峨↑縺・                //   ・磯｣邯壽ｧ 0.033 繧貞｣翫＆縺ｪ縺・ｼ峨・                if (pointInsideOther(p, exceptInst, margin)) return false;
                // 笘・ｩｦ縺励※鬧・岼縺縺｣縺溘ｂ縺ｮ: 縲梧焔蜑榊・縺ｮ蟇ｾ縺ｫ縺ｪ繧狗ｸ√′蝪槭′繧後※縺・ｋ縺ｪ繧牙･･縺ｸ縺ｯ
                //   螻翫＞縺ｦ縺・↑縺・搾ｼ井ｸｭ蠢・擇縺ｧ髀｡譏縺励◆轤ｹ縺ｮ蜿ｯ隕匁ｧ縺ｧ蛻､螳夲ｼ峨・                //   螳滓ｸｬ縺ｧ縺ｯ蟷ｻ縺梧ｶ医∴縺壹∵演 10ﾂｰ 縺ｮ髢九″蟋九ａ縺・0 縺ｫ縺ｪ繧句憶菴懃畑縺縺大・縺溘・                // 笘・◎縺ｮ隗偵・閼・ｒ**縺ｾ縺｣縺吶＄騾壹ｊ謚懊￠繧峨ｌ繧九°**繧・譛ｬ縺ｮ繝ｬ繧､縺ｧ閨槭￥縲・                //   髢峨§縺滓演縺ｨ縲∬｡晉ｫ九・豁｣蠖薙↑蝗槭ｊ霎ｼ縺ｿ縺ｯ縲∬ｲｫ騾壹・驥上〒繧ゆｽ咲ｽｮ縺ｧ繧ょ玄蛻･縺ｧ縺阪↑縺・                //   ・亥ｮ滓ｸｬ: 髢画演縺ｮ蟷ｻ縺ｯ螢√・隗偵ｒ 4.5cm 謗繧√∬｡晉ｫ九・豁｣蠖薙↑邨瑚ｷｯ縺ｯ 14cm 謗繧√ｋ縲・                //    謗繧√・險ｱ螳ｹ縺ｯ蜴壹＞螢√・縺溘ａ縺ｫ蠢・ｦ√↑縺ｮ縺ｧ縲・明蛟､縺ｧ縺ｯ縺ｩ縺｡繧峨°縺悟ｿ・★螢翫ｌ繧具ｼ峨・                //   驕輔≧縺ｮ縺ｯ**隗偵・蜷代％縺・↓謚懊￠繧峨ｌ繧九°縺ｩ縺・°**縺縺代よ演縺悟｡槭＞縺ｧ縺・ｌ縺ｰ謚懊￠繧峨ｌ縺ｪ縺・・                //   笘・譛ｬ縺ｧ縺ｯ縺ｪ縺剰､・焚譛ｬ縺ｫ謨｣繧峨＠縺ｦ縲・*1譛ｬ縺ｧ繧る壹ｌ縺ｰ蛟呵｣・*縺ｨ縺吶ｋ縲・                //     1譛ｬ縺縺ｨ縲碁壹ｋ・城壹ｉ縺ｪ縺・阪・莠悟､縺ｪ縺ｮ縺ｧ縲∵演縺碁幕縺榊ｧ九ａ縺溽椪髢薙↓
                //     繧ｲ繧､繝ｳ縺・0 縺九ｉ縺・″縺ｪ繧顔ｫ九■荳翫′繧具ｼ亥ｮ滓ｸｬ: 5ﾂｰ縺ｧ0 竊・10ﾂｰ縺ｧ0.192・峨・                //     閠ｳ縺ｫ縺ｯ縲後＄繧上▲縺ｨ螟ｧ縺阪￥縺ｪ繧九阪〒縺ｯ縺ｪ縺上後き繝√ャ縺ｨ蛻・ｊ譖ｿ繧上ｋ縲阪→閨槭％縺医ｋ縲・                //     騾壹▲縺滓悽謨ｰ縺ｮ蜑ｲ蜷医・髢九″蜈ｷ蜷医→縺ｨ繧ゅ↓騾｣邯壹↓蠅励∴繧九・縺ｧ縲√◎繧後ｒ繧ｲ繧､繝ｳ縺ｫ菴ｿ縺・                //     ・・assThroughFraction縲ゅた繝輔ヨ驕ｮ阡ｽ縺ｨ蜷後§閠・∴譁ｹ・峨・                //
                // 笘・・笘・縺薙％縺後悟ｹｻ縺ｮ邨瑚ｷｯ縲榊撫鬘後・遲斐∴・磯聞縺九▲縺溽ｵ檎ｷｯ繧呈ｮ九☆・俄・笘・・
                //
                //   縲仙ｹｻ縺後←縺・・遶九＠縺ｦ縺・◆縺九鷹哩謇峨・繝ｪ繧ｹ繝翫・繧呈ｨｪ縺ｸ謖ｯ縺｣縺滄・鄂ｮ縺ｧ蠎ｧ讓吶ｒ霑ｽ縺｣縺・
                //     竭 margin 縺ｮ謚ｼ縺怜・縺励・**隗偵〒縺ｯ 2 譁ｹ蜷代∈蜷梧凾縺ｫ蜉ｹ縺・*縲らｸｦ遞懃ｷ壹・轤ｹ縺ｯ
                //        x 縺ｫ繧・z 縺ｫ繧・2cm 謚ｼ縺輔ｌ繧九・縺ｧ縲￣(-0.58,1.60,+0.17) 縺ｯ
                //        縲梧虻蜿｣縺ｮ荳ｭ縺ｸ 2cm繝ｻ螢√・螂･髱｢縺九ｉ 2cm縲阪・蟇ｾ隗偵↓遶九▽縲・                //        ・・繝ｪ繧ｹ繝翫・縺九ｉ隕九※**螢√・蜷代％縺・・**縺ｫ蝗樊釜轤ｹ縺後〒縺阪ｋ縲・                //     竭｡ 縺昴％縺ｸ螻翫￥邱壹・蠢・┯逧・↓螢√・荳ｭ繧帝壹ｋ・・=-0.15 繧・x=-0.69 縺ｧ讓ｪ蛻・ｋ・峨・                //     竭｢ 謇峨・ z 蟶ｯ縺ｧ縺ｯ邱壹・ x=-0.648縲・0.638 縺ｫ縺・ｋ縲よ演縺ｯ x>=-0.6 縺ｪ縺ｮ縺ｧ
                //        **謇峨・閼・・螢√・荳ｭ**繧呈栢縺代※縺・※縲∵演縺ｫ縺ｯ蠖薙◆繧峨↑縺・・                //     竭｣ 雋ｫ縺九ｌ縺ｦ縺・ｋ陲門｣√・ isOccludedExcept 縺ｮ髯､螟門ｯｾ雎｡・育ｨ懃ｷ壹′螻槭☆繧狗ｮｱ・・                //        縺ｪ縺ｮ縺ｧ縲√◎縺ｮ蛻､螳壹↓縺ｯ**隕九∴縺ｪ縺・*縲・                //     竭､ crossesCore 縺ｯ邂ｱ繧・0.15m 邵ｮ繧√◆闃ｯ縺ｧ隕九ｋ縲り官縺ｮ蜀・ｸ・x=-0.75 縺ｫ蟇ｾ縺・                //        讓ｪ蛻・ｊ縺ｯ x=-0.638 ・・蟶ｯ縺ｮ荳ｭ縺ｪ縺ｮ縺ｧ縲√☆繧頑栢縺代ｋ縲・                //
                //   縲舌↑縺懆ｷ晞屬縺ｧ縺ｯ蛻・￠繧峨ｌ縺ｪ縺九▲縺溘°縲題官繧堤ｷ繧√※豁ｻ繧薙□邨瑚ｷｯ繧堤音螳壹＠縺・
                //     蜴壹＞陦晉ｫ・0.4m)繧・*荳翫°繧牙屓繧願ｾｼ繧豁｣蠖薙↑**邨瑚ｷｯ縺ｯ縲∝屓謚倡せ (0,4.02,ﾂｱ0.22)
                //     縺九ｉ髻ｳ貅舌∈縺ｮ邱壹′荳ｭ蠢・擇 z=0 繧・y=3.89・育ｸ√°繧・0.11m 蜀・・・峨〒讓ｪ蛻・ｋ縲・                //     蜴壹∩繧呈ｸ｡繧九・縺ｧ蠢・★縺昴≧縺ｪ繧九・                //     蟇ｾ縺励※蟷ｻ縺ｮ讓ｪ蛻・ｊ縺ｯ邵√°繧・**0.038m**縲・*蟷ｻ縺ｮ譁ｹ縺檎ｸ√↓霑代＞**縲・                //     髢ｾ蛟､縺ｧ蛻・ｋ縺ｪ繧峨瑚ｿ代＞譁ｹ繧定誠縺ｨ縺吶榊ｿ・ｦ√′縺ゅｊ縲∝次逅・噪縺ｫ荳榊庄閭ｽ縺縺｣縺溘・                //     8 蝗槭・螟ｱ謨励・縺薙ｌ縺檎炊逕ｱ縲ゅ碁㍼縺ｧ繧ゆｽ咲ｽｮ縺ｧ繧ょ・縺代ｉ繧後↑縺・阪・豁｣縺励°縺｣縺溘・                //
                //   縲先悽蠖薙・驕輔＞縲・*邵√・蜷代％縺・′螳滄圀縺ｫ髢九＞縺ｦ縺・ｋ縺・*縺縺代□縺｣縺溘・                //     豁｣蠖難ｼ郁｡晉ｫ九・荳奇ｼ俄ｦ 邵√・蜷代％縺・・遨ｺ
                //     蟷ｻ・磯哩謇会ｼ・     窶ｦ 邵√・蜷代％縺・・謌ｸ蜿｣縺縺後∵演縺悟｡槭＞縺ｧ縺・ｋ
                //   縺薙ｌ繧堤峩謗･閨槭￥縲１ 繧呈検繧薙〒邂ｱ縺ｮ蜴壹∩縺ｶ繧楢ｷｨ縺千ｷ壼・縺悟｡槭′縺｣縺ｦ縺・ｌ縺ｰ縲・                //   縺昴％縺ｫ騾壹ｊ驕薙・辟｡縺・ゆｸ翫・萓九↑繧・(-0.58,1.60,0.00) 縺梧演縺ｮ蜀・Κ縺ｪ縺ｮ縺ｧ關ｽ縺｡繧九・                //
                //   霍ｨ縺占ｻｸ縺ｯ**邨瑚ｷｯ縺ｮ蜷代″縺ｫ譛繧よｲｿ縺・ｮｱ縺ｮ霆ｸ**繧呈治繧九・                //     繝ｻ邂ｱ縺ｮ縲梧怙繧り埋縺・ｻｸ縲咲沿縺ｯ遶区婿菴薙〒霆ｸ縺梧▲諢冗噪縺ｫ豎ｺ縺ｾ繧具ｼ・ 霆ｸ蜷悟､・峨・                //     繝ｻ遞懃ｷ壹・ 2 髱｢縺九ｉ螟門髄縺阪↓蜃ｺ縺咏沿縺ｯ蟷ｻ繧定誠縺ｨ縺帙↑縺九▲縺滂ｼ・.088竊・.183 縺ｨ謔ｪ蛹厄ｼ峨・                //     繝ｻ邨瑚ｷｯ蝓ｺ貅悶↑繧画攸繝ｻ遶区婿菴薙・譟ｱ繧・*蛻・｡槭○縺壹↓**貂医∩縲∵э蜻ｳ繧らｴ逶ｴ:
                //       縲檎ｵ瑚ｷｯ縺瑚ｶ翫∴繧医≧縺ｨ縺励※縺・ｋ髱｢繧偵√◎縺ｮ蝣ｴ謇縺ｧ雜翫∴繧峨ｌ繧九°縲阪・                //     蠖｢繧貞・鬘槭☆繧矩明蛟､繧呈戟縺｡霎ｼ縺ｾ縺ｪ縺・・縺ｧ縲∝・鬘槭・蠅・岼縺ｧ鬟帙・縺薙→繧ゅ↑縺・・                //
                //   縲仙ｾ捺擂縺ｮ 3 譛ｬ・磯ｲ陦梧婿蜷代・蜈･繧翫・蜃ｺ繧奇ｼ峨ｒ鄂ｮ縺肴鋤縺医◆逅・罰縲・                //     霍晞屬縺・**蜴壹∩縺ｫ豈比ｾ・*・亥字縺ｿ+marginﾃ・・峨＠縺ｦ縺・※縲・囮髢薙・蟷・ｒ遏･繧峨↑縺・・                //     髫咎俣縺悟｣√・蜴壹∩繧医ｊ迢ｭ縺・→蠢・★蜿榊ｯｾ蛛ｴ縺ｮ譫縺ｫ蠖薙◆繧九・縺ｧ縲・                //     蜴壹＞螢√・迢ｭ縺・囮髢薙ｒ縲瑚ｲｫ騾壹阪→蜷御ｸ隕悶＠縺ｦ縺・◆縲ょｮ滓ｸｬ:
                //       螢√・蜊雁字 0.05m 竊・0.06m 縺ｮ髫咎俣縺九ｉ魑ｴ繧・                //       螢√・蜊雁字 0.15m 竊・0.35m 縺九ｉ・・.25m 莉･荳九・蛟呵｣・0 譛ｬ・・                //       螢√・蜊雁字 0.30m 竊・1.00m 縺九ｉ・・m 縺ｮ謌ｸ蜿｣縺吶ｉ蝪槭′縺｣縺ｦ縺・ｋ蛻､螳夲ｼ・                //     蜴壹＞螢√⊇縺ｩ蠎・＞髫咎俣繧定ｦ∵ｱゅ☆繧九√→縺・≧迴ｾ螳溘→騾・・謖吝虚縺縺｣縺溘・                //
                //   繝｡繝・す繝･縺ｫ縺ｯ菴ｿ繧上↑縺・ょ｢・阜邂ｱ縺ｮ霆ｸ縺ｯ螳溷ｽ｢迥ｶ縺ｨ蟇ｾ蠢懊＠縺ｪ縺・・縺ｧ縲・                //   霍ｨ縺守ｷ壼・縺悟ｽ｢迥ｶ縺ｨ辟｡髢｢菫ゅ↑謇繧帝壹ｋ・亥ｮ滓ｸｬ縺ｧ蝗樊釜縺悟・驛ｨ豸医∴縺滂ｼ峨・                if (!selfIsMesh) {
                    int ax = 0; float bestA = -1.0f;
                    for (int k = 0; k < 3; ++k) {
                        const float a = std::fabs(dot(selfAxes[k], dirTravel));
                        if (a > bestA) { bestA = a; ax = k; }
                    }
                    const float half = selfHalf[ax] + 2.0f * margin;
                    const Vec3 nAx = selfAxes[ax];
                    if (isOccluded(p - nAx * half, p + nAx * half)) { ++dbgRej[0]; return false; }
                } else {
                    if (isOccluded(p, p + dirTravel * (thinnest + 2.0f * margin))) {
                        ++dbgRej[0]; return false;
                    }
                    const float reach = thinnest + 2.0f * margin;
                    const Vec3 din = normalized(p - from);
                    if (isOccluded(p, p + din * reach)) { ++dbgRej[1]; return false; }
                    if (requireBothEnds) {
                        const Vec3 dout = normalized(to - p);
                        if (isOccluded(p, p + dout * reach)) { ++dbgRej[2]; return false; }
                    }
                    // 笘・Γ繝・す繝･蛛ｴ縺ｫ縺ｯ縺ｾ縺蟷ｻ縺梧ｮ九ｊ縺・ｋ・井ｸ翫・霍ｨ縺主愛螳壹′菴ｿ縺医↑縺・◆繧・ｼ峨・                    //   邂ｱ蛛ｴ縺ｧ蜉ｹ縺・◆縲檎ｸ√・蜷代％縺・′髢九＞縺ｦ縺・ｋ縺九阪↓逶ｸ蠖薙☆繧九ｂ縺ｮ繧・                    //   螳溷ｽ｢迥ｶ縺ｧ菴懊ｋ縺ｮ縺檎ｭ九よ悴逹謇九・                }
                if (isOccludedExcept(from, p, exceptInst)) { ++dbgRej[3]; return false; }   // 莉悶・髫懷ｮｳ迚ｩ
                // 蜿ｯ隕門愛螳壹◎縺ｮ繧ゅ・縺ｯ縲碁㍾縺ｿ縺・0 繧医ｊ螟ｧ縺阪＞縺九阪る㍾縺ｿ縺ｮ蛟､縺ｯ荳九〒謗｡繧九・                if (penNearWeight(from, p, p) <= 0.0f) { ++dbgRej[4]; return false; }
                if (!requireBothEnds) return true;
                if (isOccludedExcept(p, to, exceptInst)) { ++dbgRej[5]; return false; }
                if (penNearWeight(p, to, p) <= 0.0f) { ++dbgRej[6]; return false; }
                return true;
            };

            // 笘・屓謚倡せ縺ｯ縲悟庄隕也ｯ・峇縺ｫ蛻ｶ邏・＠縺・g 縺ｮ譛蟆冗せ縲阪・            //   莉･蜑阪・ {譛驕ｩ轤ｹ, 遶ｯ轤ｹA, 遶ｯ轤ｹB} 縺ｮ3轤ｹ縺励°隧ｦ縺輔★縲∵怙驕ｩ轤ｹ縺瑚ｦ矩壹○縺ｪ縺・→ P 縺檎ｫｯ轤ｹ縺ｸ
            //   繧ｹ繝翫ャ繝励＠縺ｦ縺・◆縲らｫｯ轤ｹ縺ｯ蜍輔°縺ｪ縺・・縺ｧ ﾎｴ 縺悟､牙喧縺帙★雕翫ｊ蝣ｴ縺ｫ縺ｪ繧翫∵怙驕ｩ轤ｹ縺瑚ｦ矩壹○縺・            //   迸ｬ髢薙↓ P 縺檎ｫｯ縺九ｉ荳ｭ螟ｮ縺ｸ繝ｯ繝ｼ繝励＠縺ｦ谿ｵ蟾ｮ縺悟・繧具ｼ亥ｮ滓ｸｬ縺ｧ 3.8dB 縺ｮ霍ｳ縺ｳ・峨・            //   蜿ｯ隕門｢・阜縺ｯ蟷ｾ菴輔→縺ｨ繧ゅ↓騾｣邯壹↓蜍輔￥縺ｮ縺ｧ縲√◎縺薙ｒ莠悟・縺ｧ豎ゅａ繧後・ P 繧る｣邯壹↓蜍輔￥縲・            const float tStar = 0.5f * (lo + hi);
            float bestT = -1.0f;
            if (vis(tStar)) {
                bestT = tStar;
            } else {
                // 譛驕ｩ轤ｹ縺九ｉ蟾ｦ蜿ｳ縺ｸ邊励￥繧ｹ繧ｭ繝｣繝ｳ縺励∵怙蛻昴↓蜿ｯ隕悶∈螟峨ｏ繧句玄髢薙ｒ莠悟・縺ｧ隧ｰ繧√ｋ縲・                // g 縺ｯ蜊伜ｳｰ縺ｪ縺ｮ縺ｧ縲∝庄隕夜伜沺蜀・・譛蟆上・譛驕ｩ轤ｹ縺ｫ譛繧りｿ代＞蜿ｯ隕門｢・阜縺ｫ縺ゅｋ縲・                constexpr int kScan = 16, kBisect = 12;
                for (int side = 0; side < 2; ++side) {
                    const float span = side ? (1.0f - tStar) : tStar;
                    if (span <= 1e-5f) continue;
                    float prev = tStar;
                    for (int k = 1; k <= kScan; ++k) {
                        const float t = side ? (tStar + span * (k / float(kScan)))
                                             : (tStar - span * (k / float(kScan)));
                        if (!vis(t)) { prev = t; continue; }
                        float bad = prev, good = t;            // bad:荳榊庄隕・ good:蜿ｯ隕・                        for (int it = 0; it < kBisect; ++it) {
                            const float m = 0.5f * (bad + good);
                            if (vis(m)) good = m; else bad = m;
                        }
                        if (bestT < 0.0f || g(good) < g(bestT)) bestT = good;
                        break;
                    }
                }
            }
            if (scene_detail::diffDebug())
                std::fprintf(stderr, "        [遞懃ｷ咯 A(%.2f,%.2f,%.2f)-B(%.2f,%.2f,%.2f) %s\n",
                             A.x, A.y, A.z, B.x, B.y, B.z,
                             bestT < 0.0f ? "蜈ｨ蝓滉ｸ榊庄隕・ : "謗｡逕ｨ");
            if (bestT < 0.0f) return false;                    // 遞懃ｷ壼・菴薙′隕矩壹○縺ｪ縺・            ++dbgEmit;

            const Vec3 bp = Pf(bestT);
            // 謗繧√・豺ｱ縺輔°繧画擂繧矩㍾縺ｿ・・縲・・峨ら函縺ｾ繧後ｋ迸ｬ髢薙・ 0 縺ｪ縺ｮ縺ｧ蛟呵｣懊・蜃ｺ蜈･繧翫〒霍ｳ縺ｰ縺ｪ縺・・            //   笘・ｩｦ縺励※**蜑ｲ縺ｫ蜷医ｏ縺ｪ縺九▲縺・*蛻･譯・ 遞懃ｷ壹・蜿ｯ隕門牡蜷医ｒ 8 轤ｹ繧ｵ繝ｳ繝励Ν縺励※驥阪∩縺ｫ縺吶ｋ縲・            //     螳滓ｸｬ縺ｧ 邨瑚ｷｯ髟ｷ縺ｮ譛螟ｧ髫｣謗･蟾ｮ 2.65m 竊・2.44m 縺励°謾ｹ蝟・○縺壹・            //     1繝輔Ξ繝ｼ繝 7.0ms 竊・8.1ms・・15%・峨ょ庄隕門愛螳壹・繝ｬ繧､繧剃ｽ墓悽繧よ茶縺､縺ｮ縺ｧ鬮倥＞縲・            //     荳ｻ蝗縺ｯ縲瑚ｦ九∴蟋九ａ縲阪〒縺ｯ縺ｪ縺乗滋繧∝愛螳壹・髢ｾ蛟､縺昴・繧ゅ・縺縺｣縺溘・            const float edgeW = requireBothEnds
                ? std::min(penNearWeight(from, bp, bp), penNearWeight(bp, to, bp))
                : penNearWeight(from, bp, bp);
            if (edgeW <= 0.0f) return false;
            float bd = length(bp - from) + length(to - bp) - direct;
            if (bd < 0.0f) bd = 0.0f;
            // 笘・ｫｯ轤ｹ A,B 繧よｸ｡縺吶・TM 縺ｯ**譛蛾剞縺ｮ遞懃ｷ・*縺ｧ縺ゅｋ縺薙→繧剃ｽｿ縺・・縺ｧ縲・            //   譁ｹ蜷代□縺代〒縺ｯ雜ｳ繧翫↑縺・ｼ医◎縺薙′蜑榊ｷ昴→縺ｮ譛ｬ雉ｪ逧・↑驕輔＞・峨・            fn(bp, bd, edgeDir, refT, A, B, edgeW, exceptInst);
            return true;
        };

        // 笘・屓謚倥・縲檎峩謗･邨瑚ｷｯ繧貞ｮ滄圀縺ｫ蝪槭＞縺ｧ縺・ｋ邂ｱ・医ヶ繝ｭ繝・き繝ｼ・峨阪・遞懃ｷ壹□縺代ｒ蝗槭ｋ縲るΚ螻九・蠎・螟ｩ莠・螢√↑縺ｩ
        //   逶ｴ謗･邱壹→莠､蟾ｮ縺励↑縺・ｮｱ縺ｮ遞懃ｷ壹∪縺ｧ蛟呵｣懊↓縺吶ｋ縺ｨ縲∫┌髢｢菫ゅ↑驕縺・婿蜷代∈蛟呵｣懊′鬟帙ｓ縺ｧ蜷域・繧呈ｿ√☆縲・        //   逶ｴ謗･ from竊稚o 縺ｮ邱壼・縺・OBB 縺ｨ莠､蟾ｮ縺吶ｋ邂ｱ・昴ヶ繝ｭ繝・き繝ｼ縲√□縺代ｒ蟇ｾ雎｡縺ｫ縺吶ｋ縲・        auto isBlocker = [&](int inst) {
            if (inst < 0 || inst >= instanceCount() || !instances_[inst].active) return false;
            if (blockerMargin <= 0.0f) return segmentIntersectsObb(from, to, instances_[inst].obb);
            // 閹ｨ繧峨∪縺帙◆ OBB 縺ｧ蛻､螳壹☆繧具ｼ亥濠蠕・婿蜷代↓ margin 縺縺第僑螟ｧ・峨・            Obb fat = instances_[inst].obb;
            fat.halfExtents = fat.halfExtents + Vec3(blockerMargin, blockerMargin, blockerMargin);
            return segmentIntersectsObb(from, to, fat);
        };

        // 笘・幕蜿｣縺ｮ邵√・縲∫峩謗･邱壹ｒ蝪槭＞縺ｧ縺・ｋ邂ｱ縺ｫ螻槭☆繧九→縺ｯ髯舌ｉ縺ｪ縺・・        //
        //   莉募・繧翫′縲悟ｷｦ縺ｮ螢・ｼ句承縺ｮ螢√阪・ 2 譫壹〒縺ｧ縺阪※縺・ｋ縺ｨ・・nity 縺ｮ Test_DiffractionGap 縺・        //   縺ｾ縺輔↓縺昴ｌ・峨∫峩謗･邱壹ｒ蝪槭＄縺ｮ縺ｯ迚・婿縺縺代〒縲・幕蜿｣縺ｮ**蜿榊ｯｾ蛛ｴ縺ｮ邵・*繧呈戟縺､譁ｹ縺ｯ
        //   繝悶Ο繝・き繝ｼ縺ｫ縺ｪ繧峨↑縺・ゅ▽縺ｾ繧企幕蜿｣縺ｮ邵√′迚・婿縺励°蛟呵｣懊↓蜈･繧峨↑縺・・        //   谿九▲縺溽援譁ｹ繧ゆｻ募・繧翫・隗偵ｒ謗繧√ｋ縺ｮ縺ｧ雋ｫ騾壼愛螳壹〒關ｽ縺｡繧九％縺ｨ縺後≠繧翫√◎縺・↑繧九→
        //   **蛟呵｣懊′蜈ｨ貊・＠縺ｦ辟｡髻ｳ縺ｫ縺ｪ繧・*・亥ｮ滓ｸｬ: Test_DiffractionGap 縺ｨ蜷悟ｽ｢迥ｶ縺ｧ
        //   繝ｪ繧ｹ繝翫・ x竏・-1.5,0.5] 縺ｮ 4 轤ｹ縺悟呵｣・0 譛ｬ縲よｭｩ縺上→蝗樊釜髻ｳ縺御ｸｸ縺斐→蛻・ｌ繧具ｼ峨・        //   縺昴・縺ｨ縺阪√Μ繧ｹ繝翫・縺九ｉ蜿榊ｯｾ蛛ｴ縺ｮ邵√∈縺ｮ邱壹・髢句哨縺ｮ荳ｭ繧堤ｴ騾壹ｊ縺励※縺・◆
        //   笏笏 蛟呵｣懊↓蜈･縺｣縺ｦ縺輔∴縺・ｌ縺ｰ騾壹▲縺ｦ縺・◆縲√→縺・≧縺縺代・隧ｱ縲・        //
        //   繝悶Ο繝・き繝ｼ縺ｨ**蜷後§蟷ｳ髱｢縺ｫ縺ゅｋ邂ｱ**縺ｯ蜷後§莉募・繧翫・荳驛ｨ縺ｪ縺ｮ縺ｧ縲∝酔縺倬幕蜿｣繧貞峇縺｣縺ｦ縺・ｋ縲・        //   謇峨・謌ｸ蜿｣繝ｻ髢句哨繧貞錐謖・＠縺励↑縺・ｸ闊ｬ縺ｮ隕丞援縺ｪ縺ｮ縺ｧ縲∥uthoring 縺ｯ隕√ｉ縺ｪ縺・＠
        //   謇狗ｶ壹″逕滓・縺ｧ螢√′蛻・牡縺輔ｌ縺ｦ繧ょ柑縺上・        //   蛟呵｣懊′蠅励∴縺ｦ繧ゅ∽ｸ九・蜿ｯ隕門愛螳壹ｒ騾壹ｉ縺ｪ縺代ｌ縺ｰ邨瑚ｷｯ縺ｫ縺ｯ縺ｪ繧峨↑縺・・        constexpr int kMaxBlockerPlanes = 8;
        Vec3  bpN[kMaxBlockerPlanes]; Vec3 bpC[kMaxBlockerPlanes]; int nbp = 0;
        auto thinNormalOf = [&](const Obb& b) {
            const float hx = b.halfExtents.x, hy = b.halfExtents.y, hz = b.halfExtents.z;
            Vec3 n = b.axisZ;
            if (hx <= hy && hx <= hz) n = b.axisX;
            else if (hy <= hx && hy <= hz) n = b.axisY;
            return (length(n) > 1e-4f) ? normalized(n) : Vec3(0, 0, 1);
        };
        for (int i = 0; i < instanceCount(); ++i) {
            if (nbp >= kMaxBlockerPlanes) break;
            if (!instances_[i].active || instances_[i].geomId >= 0) continue;
            if (!isBlocker(i)) continue;
            const Obb& b = instances_[i].obb;
            const Vec3 n = thinNormalOf(b);
            bool dup = false;
            for (int k = 0; k < nbp; ++k)
                if (std::fabs(dot(n, bpN[k])) > 0.999f
                    && std::fabs(dot(n, b.center - bpC[k])) < 0.05f) { dup = true; break; }
            if (!dup) { bpN[nbp] = n; bpC[nbp] = b.center; ++nbp; }
        }
        // 繝悶Ο繝・き繝ｼ縲√∪縺溘・繝悶Ο繝・き繝ｼ縺ｨ蜷御ｸ蟷ｳ髱｢縺ｮ邂ｱ縲・        //   繝｡繝・す繝･縺ｯ螳溷ｽ｢迥ｶ縺ｧ驕ｮ阡ｽ蛻､螳壹＆繧後ｋ縺ｮ縺ｧ蠎・￡縺ｪ縺・ｼ亥｢・阜邂ｱ縺ｮ蟷ｳ髱｢縺ｯ蠖｢迥ｶ縺ｨ蟇ｾ蠢懊＠縺ｪ縺・ｼ峨・        auto isEdgeSource = [&](int inst) {
            if (isBlocker(inst)) return true;
            if (inst < 0 || inst >= instanceCount()) return false;
            const Instance& in2 = instances_[inst];
            if (!in2.active || in2.geomId >= 0) return false;
            const Vec3 n = thinNormalOf(in2.obb);
            for (int k = 0; k < nbp; ++k)
                if (std::fabs(dot(n, bpN[k])) > 0.999f
                    && std::fabs(dot(n, in2.obb.center - bpC[k])) < 0.05f) return true;
            return false;
        };

        // 繧ｨ繝・ず繧ｫ繧ｿ繝ｭ繧ｰ・・: 繧ｭ繝･繝ｼ繝悶・繝・・逕ｱ譚･縺ｮ繧ｷ繝ｫ繧ｨ繝・ヨ遞懃ｷ夲ｼ峨→邂ｱ縺ｮ螳溽ｨ懃ｷ壹・荳｡譁ｹ繧貞呵｣懊↓縺吶ｋ・亥柱髮・粋・峨・        // 繧ｫ繧ｿ繝ｭ繧ｰ縺縺代□縺ｨ迚・・縺励°諡ｾ縺医↑縺・％縺ｨ縺後≠繧具ｼ遺・荳｡蛛ｴ縺九ｉ魑ｴ繧峨↑縺・・繧ｨ繝・ず蜈･譖ｿ縺ｧ譁ｹ蜷代′鬟帙・・峨・縺ｧ縲・        // 邂ｱ縺ｮ 12 遞懃ｷ壹ｂ蠢・★蜉縺医※荳｡蛛ｴ繧堤｢ｺ螳溘↓蜿悶ｋ縲ゅ◆縺縺怜曙譁ｹ縺ｨ繧ゅヶ繝ｭ繝・き繝ｼ縺ｮ遞懃ｷ壹↓髯舌ｋ縲・        if (!edgeCatalog_.empty())
            for (const DiffEdge& e : edgeCatalog_)
                if (isEdgeSource(e.instance))
                    tryEdge(e.p0, e.p1, e.edgeDir, e.refTangent, e.instance);

        // 邂ｱ(OBB)縺ｮ 12 遞懃ｷ壹◎繧後◇繧後〒謗繧√ｋ隗偵ｒ謗｢邏｢・郁ｧ偵・縺ｿ縺縺ｨ阮・＞螢√〒螟ｱ謨励☆繧九・縺ｧ遞懃ｷ壼・菴難ｼ峨・        //   繝｡繝・す繝･繧､繝ｳ繧ｹ繧ｿ繝ｳ繧ｹ縺ｯ邂ｱ縺ｮ 12 遞懃ｷ壹〒縺ｯ縺ｪ縺上・*蠖｢迥ｶ縺九ｉ謚ｽ蜃ｺ縺励◆蝗樊釜遞懃ｷ・*繧剃ｽｿ縺・・        //   OBB 縺ｮ遞懃ｷ壹・縲後Γ繝・す繝･縺ｮ遞懃ｷ壹阪〒縺ｯ縺ｪ縺・・縺ｧ縲√◎縺ｮ縺ｾ縺ｾ蝗槭☆縺ｨ驕ｮ阡ｽ蛻､螳夲ｼ亥ｮ溷ｽ｢迥ｶ・峨→
        //   蝗樊釜蛻､螳夲ｼ亥｢・阜繝懊ャ繧ｯ繧ｹ・峨′遏帷崟縺励◆蟷ｾ菴輔ｒ隕九ｋ縺薙→縺ｫ縺ｪ繧・笏笏 螢√→謌ｸ蜿｣縺・1 繝｡繝・す繝･縺ｪ繧峨・        //   驕ｮ阡ｽ縺ｯ縲梧虻蜿｣繧帝壹ｋ縲阪∝屓謚倥・縲悟｣√・螟門捉繧貞屓繧後阪→險縺・ｼ郁ｨｭ險・ﾂｧ5-5・峨・        const int fixed2[4][2] = {{-1, -1}, {-1, 1}, {1, -1}, {1, 1}};
        for (int i = 0; i < instanceCount(); ++i) {
            const Instance& inst = instances_[i];
            if (!inst.active) continue;
            const Obb& b = inst.obb;
            // 繝悶Ο繝・き繝ｼ・句酔荳蟷ｳ髱｢縺ｮ邂ｱ縺悟屓謚伜ｯｾ雎｡・・argin>0 縺ｪ繧芽ｿ大ｍ繧ょ性繧・・            if (!isEdgeSource(i)) continue;
            ++dbgBlockers;

            if (inst.geomId >= 0 && inst.geomId < static_cast<int>(meshes_.size())) {
                const MeshGeometry& g = meshes_[static_cast<size_t>(inst.geomId)];
                if (!g.used) continue;
                for (const DiffractionEdgeLocal& e : g.edges) {
                    const Vec3 A = meshLocalToWorldPoint(e.a, b);
                    const Vec3 B = meshLocalToWorldPoint(e.b, b);
                    const Vec3 dir = B - A;
                    if (length(dir) < 1e-5f) continue;
                    const Vec3 ed = normalized(dir);
                    // refT 縺ｯ UTD 逕ｨ・域ｯ碑ｼ・ｮ溯｣・・縺ｿ縺御ｽｿ縺・ｼ峨らｨ懃ｷ壹↓逶ｴ莠､縺吶ｋ驕ｩ蠖薙↑謗･邱壹〒繧医＞縲・                    Vec3 t0 = cross(ed, Vec3(0, 1, 0));
                    if (length(t0) < 1e-3f) t0 = cross(ed, Vec3(1, 0, 0));
                    tryEdge(A, B, ed, normalized(t0), i);
                }
                continue;
            }

            const Vec3 ax[3] = {b.axisX, b.axisY, b.axisZ};
            const float h[3] = {b.halfExtents.x + margin, b.halfExtents.y + margin,
                                b.halfExtents.z + margin};
            auto pointAt = [&](const float a[3]) {
                return b.center + ax[0] * (h[0] * a[0]) + ax[1] * (h[1] * a[1]) + ax[2] * (h[2] * a[2]);
            };
            for (int freeAxis = 0; freeAxis < 3; ++freeAxis) {
                const int o1 = (freeAxis + 1) % 3;
                const int o2 = (freeAxis + 2) % 3;
                for (const auto& sg : fixed2) {
                    // 遞懃ｷ壽婿蜷托ｼ拉reeAxis 霆ｸ縲・髱｢(o1髱｢)謗･邱夲ｼ昴お繝・ず縺九ｉ螟門・縺ｸ・遺・o2ﾂｷsg1・峨・                    const Vec3 edgeDir = ax[freeAxis];
                    const Vec3 refT = ax[o2] * (-static_cast<float>(sg[1]));
                    float aA[3], aB[3];
                    aA[freeAxis] = -1.0f; aA[o1] = static_cast<float>(sg[0]); aA[o2] = static_cast<float>(sg[1]);
                    aB[freeAxis] = +1.0f; aB[o1] = static_cast<float>(sg[0]); aB[o2] = static_cast<float>(sg[1]);
                    ++dbgEdges;
                    tryEdge(pointAt(aA), pointAt(aB), edgeDir, refT, i);
                }
            }
        }
        if (scene_detail::diffDebug())
            std::fprintf(stderr,
                "  [蛟呵｣彎 from(%.2f,%.2f,%.2f) 繝悶Ο繝・き繝ｼ%d 遞懃ｷ・d 騾夐℃%d ・懈｣・唆: "
                "騾ｲ陦・d 蜈･%d 蜃ｺ%d 謇句燕驕ｮ%d 謇句燕雋ｫ%d 螂･驕ｮ%d 螂･雋ｫ%d\n",
                from.x, from.y, from.z, dbgBlockers, dbgEdges, dbgEmit,
                dbgRej[0], dbgRej[1], dbgRej[2], dbgRej[3], dbgRej[4], dbgRej[5], dbgRej[6]);
    }

    // 縲仙屓謚・Phase 1.5 證ｫ螳壹・蜿ｯ隕門喧逕ｨ)縲醍峩謗･ from->to 縺碁・阡ｽ縺輔ｌ縺ｦ縺・ｋ縺ｨ縺阪∫ｨ懃ｷ壹ｒ蝗槭ｋ縲梧怙遏ｭ霑ょ屓縲阪・
    // 菴吝臆邨瑚ｷｯ髟ｷ ﾎｴ(= 霑ょ屓髟ｷ 竏・逶ｴ邱夐聞, m)繧定ｿ斐＠縲∵怙濶ｯ縺ｮ霑ょ屓轤ｹ繧・outPoint 縺ｫ譖ｸ縺上る・阡ｽ縺ｪ縺・or 霑ょ屓霍ｯ縺ｪ縺励・
    // -1・・utPoint 荳榊ｮ夲ｼ峨ＰutEdgeDir/outRefTangent : null 縺ｧ縺ｪ縺代ｌ縺ｰ譛濶ｯ遞懃ｷ壹・縲後お繝・ず譁ｹ蜷代阪・髱｢謗･邱壹阪ｒ譖ｸ縺上・    //   blockerMargin > 0 縺ｪ繧峨・・阡ｽ縺輔ｌ縺ｦ縺・↑縺上※繧りｿ大ｍ縺ｮ繧ｨ繝・ず繧呈爾縺呻ｼ亥ｽｱ蠅・阜縺ｮ螟門・縺ｧ
    //   蝗樊釜蝣ｴ繧呈ｱゅａ繧九◆繧・ｼ峨よ里螳・0 縺ｧ縺ｯ蠕捺擂縺ｩ縺翫ｊ縲碁・阡ｽ譎ゅ・縺ｿ縲阪・    float diffractionDetour(const Vec3& from, const Vec3& to, Vec3& outPoint,
                            Vec3* outEdgeDir = nullptr, Vec3* outRefTangent = nullptr,
                            float blockerMargin = 0.0f) const {
        if (blockerMargin <= 0.0f && !isOccluded(from, to)) return -1.0f;
        float best = -1.0f;
        Vec3 bestP{0, 0, 0}, bestEdge{1, 0, 0}, bestRefT{0, 1, 0};
        forEachDiffractionCandidate(from, to, [&](const Vec3& P, float d, const Vec3& e, const Vec3& r, const Vec3&, const Vec3&, float, int) {
            if (best < 0.0f || d < best) { best = d; bestP = P; bestEdge = e; bestRefT = r; }
        }, blockerMargin);
        if (best >= 0.0f) {
            outPoint = bestP;
            if (outEdgeDir) *outEdgeDir = bestEdge;
            if (outRefTangent) *outRefTangent = bestRefT;
        }
        return best;
    }

    // 縲仙屓謚倥・螟夐㍾繧ｨ繝・ず蜷域・縲鷹・阡ｽ譎ゅ∬､・焚縺ｮ蝗槭ｊ霎ｼ縺ｿ繧ｨ繝・ず繧貞粋謌舌＠縺溘悟ｱ翫￥譁ｹ蜷代阪ｒ outDir 縺ｫ譖ｸ縺上・    // 蜊倅ｸ譛遏ｭ繧ｨ繝・ず縺縺代□縺ｨ (1)繧ｨ繝・ず縺悟・繧梧崛繧上▲縺溽椪髢薙↓譁ｹ蜷代′鬟帙・ (2)荳｡蛛ｴ遨ｺ縺・※縺ｦ繧ら援蛛ｴ縺九ｉ縺励°
    // 魑ｴ繧峨↑縺・ょ推繧ｨ繝・ず縺ｮ謗繧√ｋ轤ｹ縺ｮ譁ｹ蜷代ｒ縲∬ｿょ屓縺ｮ遏ｭ縺・w=exp(-(ﾎｴ-ﾎｴmin)/scale) 縺ｧ蜉驥榊粋謌撰ｼ晏芦譚･譁ｹ蜷代・
    // 繧､繝ｳ繝・Φ繧ｷ繝・ぅ驥榊ｿ・や・ 貊代ｉ縺九↓蛻・崛繧上ｊ縲∬､・焚縺ｮ髢句哨縺後≠繧後・荳｡譁ｹ縺九ｉ螻翫￥縲・    // 謌ｻ繧雁､ = 譛遏ｭﾎｴ・郁ｷ晞屬貂幄｡ｰ/驥阪∩逕ｨ縲・1=霑ょ屓霍ｯ縺ｪ縺暦ｼ峨ＰutDir 縺ｯ霑ょ屓霍ｯ縺ｪ縺励・縺ｨ縺肴悴譖ｸ謠帙・    float diffractionComposite(const Vec3& from, const Vec3& to, Vec3& outDir) const {
        if (!isOccluded(from, to)) return -1.0f;
        constexpr int kMaxCand = 64;  // 蜷梧凾縺ｫ蜷域・縺吶ｋ蝗槭ｊ霎ｼ縺ｿ邨瑚ｷｯ縺ｮ荳企剞・域焚蜊∵悽縺ｾ縺ｧ・・        Vec3 dirs[kMaxCand];
        float deltas[kMaxCand];
        int nc = 0;
        float dmin = -1.0f;
        forEachDiffractionCandidate(from, to, [&](const Vec3& P, float d, const Vec3&, const Vec3&, const Vec3&, const Vec3&, float, int) {
            if (nc < kMaxCand) { dirs[nc] = normalized(P - from); deltas[nc] = d; ++nc; }
            if (dmin < 0.0f || d < dmin) dmin = d;
        });
        if (nc == 0) return -1.0f;
        // 蜈ｨ蛟呵｣懊ｒ蜷檎ｭ峨・驥阪∩縺ｧ蜷域・・域怙遏ｭ蛛城㍾繧偵ｄ繧√ｋ・峨ゅ←縺ｮ繧ｨ繝・ず繧ょｮ壻ｽ阪↓遲峨＠縺丞柑縺上・縺ｧ縲・        // 繧ｨ繝・ず縺・譛ｬ蜈･繧梧崛繧上▲縺ｦ繧よ婿蜷代・螟牙喧縺梧ｦゅ・ 1/譛ｬ謨ｰ 縺ｫ蜿弱∪繧翫∝・譖ｿ縺ｮ繧ｬ繧ｯ縺､縺阪′貂帙ｋ縲・        Vec3 acc{0, 0, 0};
        for (int i = 0; i < nc; ++i) acc = acc + dirs[i];
        if (length(acc) < 1e-6f) acc = to - from;
        outDir = normalized(acc);
        return dmin;
    }

    // 縲仙庄隕門喧縲鷹・阡ｽ譎ゅ・蝗樊釜蛟呵｣懶ｼ域滋繧√ｋ轤ｹ P 縺ｨ菴吝臆ﾎｴ・峨ｒ譛螟ｧ maxCount 蛟・outP/outDelta 縺ｫ譖ｸ縺阪・    // 譖ｸ縺・◆蛟区焚繧定ｿ斐☆縲ょ・蛟呵｣懊ｒ謠冗判縺励∵怙遏ｭﾎｴ縺ｮ繧ゅ・繧貞他縺ｳ蜃ｺ縺怜・縺ｧ min 繧貞叙縺｣縺ｦ濶ｲ蛻・￠縺吶ｋ繝・ヰ繝・げ逕ｨ縲・    int diffractionCandidates(const Vec3& from, const Vec3& to,
                              Vec3* outP, float* outDelta, int maxCount) const {
        if (!outP || !outDelta || maxCount <= 0) return 0;
        if (!isOccluded(from, to)) return 0;
        int n = 0;
        forEachDiffractionCandidate(from, to, [&](const Vec3& P, float d, const Vec3&, const Vec3&, const Vec3&, const Vec3&, float, int) {
            if (n < maxCount) { outP[n] = P; outDelta[n] = d; ++n; }
        });
        return n;
    }

    // 縲宣囮髢薙・蟷・大屓謚倡せ P 縺ｫ縺翫￠繧九檎ｨ懃ｷ壹ｒ讓ｪ蛻・ｋ蜷代″縺ｮ閾ｪ逕ｱ縺ｪ蟾ｮ縺玲ｸ｡縺励・m)縲・    //
    //   笘・ｸｬ繧玖ｻｸ繧・cross(遞懃ｷ壹・蜷代″, 騾ｲ陦梧婿蜷・ 縺ｫ蜿悶ｋ縺ｮ縺瑚ｦ∫せ縲・    //     驕主悉縺ｫ3蝗槭∝・逶､繧・檎ｵ瑚ｷｯ縺ｮ霆ｸ縺ｫ蝙ら峩縺ｪ4譁ｹ蜷代阪〒貂ｬ縺｣縺ｦ螟ｱ謨励＠縺ｦ縺・ｋ縲・    //     縺昴ｌ繧峨・**遞懃ｷ壹↓豐ｿ縺｣縺滓婿蜷・*縺ｾ縺ｧ蜷ｫ繧薙〒縺励∪縺・・縺ｧ縲∵演縺ｮ髫咎俣縺ｧ縺ｯ縺ｪ縺・    //     驛ｨ螻九・蟾ｮ縺玲ｸ｡縺暦ｼ亥ｮ滓ｸｬ 3.0m・峨ｒ貂ｬ縺｣縺ｦ縺励∪縺・・幕縺榊・蜷医→辟｡髢｢菫ゅ↑蛟､縺ｫ縺ｪ繧九・    //     遞懃ｷ壹↓蝙ら峩繝ｻ縺九▽騾ｲ陦梧婿蜷代↓蝙ら峩縺ｪ霆ｸ縺ｪ繧峨・*髫咎俣繧呈ｨｪ蛻・ｋ蜷代″縺縺・*繧定ｦ九ｉ繧後ｋ縲・    //
    //   縺薙・霆ｸ縺ｪ繧峨碁幕蜿｣縲阪→縲瑚・逕ｱ縺ｪ邵√阪ｂ蛻・°繧後ｋ:
    //     謇峨・髫咎俣 窶ｦ 荳｡蛛ｴ縺ｨ繧ょ｡槭′縺｣縺ｦ縺・ｋ 竊・蟾ｮ縺玲ｸ｡縺励′蟆上＆縺・ｼ茨ｼ晞壹ｊ縺ｫ縺上＞・・    //     陦晉ｫ九・邵・窶ｦ 迚・・縺檎┌髯舌↓髢九＞縺ｦ縺・ｋ 竊・蟾ｮ縺玲ｸ｡縺励′螟ｧ縺阪＞・茨ｼ晏宛髯舌＠縺ｪ縺・ｼ・    //   蜀・乢縺ｧ貂ｬ繧九→陦晉ｫ九・邵√〒繧ょ濠蛻・′蠖ｱ縺ｫ蜈･縺｣縺ｦ隱､縺｣縺ｦ邨槭ｉ繧後ｋ・亥ｮ滓ｸｬ縺ｧ蠖ｱ蠅・阜縺悟｣翫ｌ縺滂ｼ峨・    //
    //   蜑榊ｷ昴・蠑上・ ﾎｴ 縺励°隕九↑縺・・縺ｧ縲・.8cm 縺ｮ髫咎俣縺ｧ繧・1.8m 縺ｮ髢句哨縺ｧ繧ゅ∬ｿょ屓驥上′蜷後§縺ｪ繧・    //   蜷後§蛟､繧定ｿ斐☆・郁ｨｭ險・ﾂｧ6-2・峨ゅ◎縺ｮ谺縺代ｒ縺薙％縺ｧ蝓九ａ繧九・    //   笘・ｻｸ縺ｯ2譛ｬ隕√ｋ縲・*遞懃ｷ壹↓蝙ら峩縺ｪ蜷代″**縺ｨ**遞懃ｷ壹↓豐ｿ縺｣縺溷髄縺・*縺ｮ荳｡譁ｹ繧呈ｸｬ縺｣縺ｦ譛蟆上ｒ謗｡繧九・    //     縺ｩ縺｡繧峨′髫咎俣繧呈ｨｪ蛻・ｋ縺九・縲∝呵｣懊′縺ｩ縺ｮ遞懃ｷ壹↓荵励▲縺溘°縺ｧ螟峨ｏ繧九◆繧・
    //       謇峨・閾ｪ逕ｱ遶ｯ・亥桙逶ｴ縺ｪ遞懃ｷ夲ｼ俄ｦ 蝙ら峩縺ｪ霆ｸ・晄ｨｪ譁ｹ蜷・縺碁囮髢薙ｒ讓ｪ蛻・ｋ
    //       縺ｾ縺舌＆・域ｰｴ蟷ｳ縺ｪ遞懃ｷ夲ｼ・   窶ｦ 豐ｿ縺｣縺溯ｻｸ・晄ｨｪ譁ｹ蜷・縺碁囮髢薙ｒ讓ｪ蛻・ｋ
    //     迚・婿縺縺代□縺ｨ縲√∪縺舌＆縺ｫ荵励▲縺溷呵｣懊′縲梧虻蜿｣縺ｮ鬮倥＆ 2.4m縲阪ｒ貂ｬ縺｣縺ｦ縺励∪縺・・    //     1.8cm 縺ｮ髫咎俣縺ｪ縺ｮ縺ｫ邏騾壹ｊ縺ｨ蛻､螳壹＆繧後ｋ・亥ｮ滓ｸｬ縺ｧ縺昴≧縺ｪ縺｣縺滂ｼ峨・    //     陦晉ｫ九・閾ｪ逕ｱ縺ｪ邵√・縺ｩ縺｡繧峨・霆ｸ繧ら援蛛ｴ縺碁幕縺・※縺・ｋ縺ｮ縺ｧ縲［in 繧呈治縺｣縺ｦ繧ょ､ｧ縺阪＞縺ｾ縺ｾ縲・    float slitWidthAt(const Vec3& P, const Vec3& edgeDir, const Vec3& travelDir) const {
        constexpr float kMaxSpan = 4.0f;   // 縺薙ｌ莉･荳雁ｺ・￠繧後・髢句哨縺ｨ縺励※蠕矩溘＠縺ｪ縺・        auto freeDist = [&](const Vec3& d) {
            const SceneHit hit = raycastClosest(P, d, kMaxSpan);
            return hit.hit ? hit.t : kMaxSpan;
        };
        auto span = [&](const Vec3& d) {
            return freeDist(d) + freeDist(Vec3(-d.x, -d.y, -d.z));
        };
        float narrowest = kMaxSpan * 2.0f;
        Vec3 w = cross(edgeDir, travelDir);
        if (length(w) > 1e-4f) narrowest = std::min(narrowest, span(normalized(w)));
        if (length(edgeDir) > 1e-4f) narrowest = std::min(narrowest, span(normalized(edgeDir)));
        return narrowest;
    }

    // 髫咎俣縺ｮ蟷・竊・蠎・ｸｯ蝓溘ご繧､繝ｳ(0..1)縲ら強縺・⊇縺ｩ騾壹ｉ縺ｪ縺・・    //   蝓ｺ貅門ｹ・・縲後％繧後ｈ繧雁ｺ・￠繧後・邏騾壹ｊ縲阪・逶ｮ螳峨ら黄逅・噪縺ｫ縺ｯ豕｢髟ｷ縺ｮ蜊雁・縺ゅ◆繧翫′蠅・岼
    //   ・・00Hz 縺ｧ 0.34m・峨ゅせ繧ｫ繝ｩ縺ｧ霑斐☆縺ｮ縺ｯ縲∝屓謚倥′驕九・諠・ｱ繧貞ｮ壻ｽ阪↓邨槭ｋ譁ｹ驥昴・縺溘ａ縲・    float slitWidthGain(float width) const {
        if (slitWidthRef_ <= 1e-4f) return 1.0f;         // 0 縺ｧ辟｡蜉ｹ蛹・        const float r = width / slitWidthRef_;
        if (r >= 1.0f) return 1.0f;
        if (r <= 0.0f) return 0.0f;
        if (slitWidthPower_ == 1.0f) return r;
        return std::pow(r, slitWidthPower_);
    }

    // 縲宣幕蜿｣縺ｮ螳溷柑蟷・鷹・阡ｽ迚ｩ縺ｮ髱｢縺ｫ譬ｼ蟄舌ｒ蠑ｵ繧翫√碁浹縺梧栢縺代ｉ繧後ｋ繧ｻ繝ｫ縲阪ｒ謨ｰ縺医※蟷・m)繧貞・縺吶・    //
    //   笘・％繧後・蝗樊釜轤ｹ縺ｫ**荳蛻・ｾ晏ｭ倥＠縺ｪ縺・*縲る℃蜴ｻ6蝗槭・螟ｱ謨励・蜈ｨ驛ｨ縲悟屓謚倡せ縺ｾ繧上ｊ繧呈ｸｬ繧九・    //     縺ｨ縺・≧蜑肴署繧貞・譛峨＠縺ｦ縺・※縲∝屓謚倡せ縺ｨ迢ｭ遯・Κ縺悟挨縺ｮ蝣ｴ謇縺ｫ縺ゅｋ縺帙＞縺ｧ遐ｴ邯ｻ縺励※縺・◆
    //     ・域演縺梧万繧√↓髢九￥縺ｨ縲∵演縺ｮ邵√・螢√・髱｢繧医ｊ 21cm 謇句燕縺ｸ鬟帙・蜃ｺ縺呻ｼ峨・    //     螢√◎縺ｮ繧ゅ・繧呈ｸｬ繧後・縲∫強遯・Κ縺後←縺薙↓縺ゅ▲縺ｦ繧る未菫ゅ↑縺・・    //
    //   髱｢縺ｮ蜑榊ｾ後↓蜴壹∩繧呈戟縺溘○縺ｦ蛻､螳壹☆繧九・縺瑚ｦ∫せ縲る擇荳翫□縺代〒隕九ｋ縺ｨ縲∝｣√・髱｢繧医ｊ
    //   10cm 螂･縺ｫ遶九▲縺ｦ縺・ｋ謇峨ｒ蜿悶ｊ縺薙⊂縺呻ｼ・蝗樒岼縺後％繧後〒螟悶＠縺滂ｼ峨・    //
    //   髢句哨縺ｨ閾ｪ逕ｱ縺ｪ邵√・蛹ｺ蛻･繧り・辟ｶ縺ｫ縺､縺・
    //     謌ｸ蜿｣   窶ｦ 譬ｼ蟄舌・螟ｧ蜊翫′螢√〒縲・幕縺・◆繧ｻ繝ｫ縺縺代′髫咎俣 竊・螳溷柑蟷・′蟆上＆縺・    //     陦晉ｫ九・邵・窶ｦ 陦晉ｫ九・螟門・縺ｮ繧ｻ繝ｫ縺ｯ蜈ｨ驛ｨ謚懊￠繧峨ｌ繧・竊・螳溷柑蟷・′螟ｧ縺阪＞・亥ｾ矩溘＠縺ｪ縺・ｼ・    //
    //   螳溷柑蟷・・蜃ｺ縺玲婿: 髢九＞縺滄擇遨・ﾃｷ 髢九＞縺滄伜沺縺ｮ髟ｷ縺・婿縺ｮ蟾ｮ縺玲ｸ｡縺励・    //     鬮倥＆ H繝ｻ蟷・w 縺ｮ邵ｦ繧ｹ繝ｪ繝・ヨ縺ｪ繧・(wH)/H = w 縺悟・繧九ょ､ｧ縺阪↑髢句哨縺ｧ繧ら洒縺・婿縺ｮ霎ｺ縺悟・繧九・    //   seed 縺ｯ縲碁幕縺・※縺・ｋ蝣ｴ謇縺ｮ蠖薙◆繧翫阪ｒ荳弱∴繧九ヲ繝ｳ繝茨ｼ亥屓謚倡せ繧呈ｸ｡縺呎Φ螳夲ｼ峨・    //     荳讒倩ｵｰ譟ｻ縺縺代〒縺ｯ cm 蜊倅ｽ阪・髫咎俣繧定ｦ九▽縺代ｉ繧後↑縺・ｼ亥濠蠕・.5m繧・蛻・牡縺励※繧ゅそ繝ｫ縺ｯ
    //     21cm縲・0ﾂｰ縺ｮ謇峨・髫咎俣縺ｯ7cm・峨・*貂ｬ繧句ｴ謇縺ｯ髱｢縺ｮ縺ｾ縺ｾ**縺ｧ縲∵爾縺怜ｧ九ａ繧句ｴ謇縺縺・    //     謨吶∴繧九る℃蜴ｻ縺ｮ螟ｱ謨励・縲悟屓謚倡せ縺ｧ貂ｬ縺｣縺ｦ縺・◆縲阪％縺ｨ縺ｧ縲∝・逋ｺ轤ｹ縺ｫ縺吶ｋ縺ｮ縺ｯ蛻･縺ｮ隧ｱ縲・    // 縲宣幕蜿｣縺ｮ繧ｨ繝阪Ν繧ｮ繝ｼ繝ｻ蟶ｯ蝓溷挨縲代ヵ繝ｬ繝阪Ν繧ｾ繝ｼ繝ｳ縺ｮ縺・■縺ｩ繧後□縺鷹幕縺・※縺・ｋ縺九ｒ蟶ｯ蝓溘＃縺ｨ縺ｫ霑斐☆縲・    //   隧ｳ縺励＞逅・ｱ医→縲√↑縺懊％繧後〒騾｣邯壽ｧ縺悟叙繧後ｋ縺九・ Core/aperture_fresnel.h 縺ｮ蜀帝ｭ繧貞盾辣ｧ縲・    //   謌ｻ繧雁､: 驕ｮ繧九ｂ縺ｮ縺檎┌縺代ｌ縺ｰ false・茨ｼ晏宛髯舌＠縺ｪ縺・ｼ峨・    // ======================================================== 繝昴・繧ｿ繝ｫ・磯幕蜿｣縺ｮ遏ｩ蠖｢・・    //
    // 縺ｪ縺懷ｰ主・縺吶ｋ縺・
    //   莉頑律縺薙％縺ｾ縺ｧ縲・幕蜿｣縺ｮ貂ｬ繧頑婿繧・騾壹ｊ隧ｦ縺励※蜈ｨ驛ｨ螟ｱ謨励＠縺溘ょ次蝗縺ｯ豈主屓蜷後§縺ｧ
    //   **縲碁擇縺ｮ縺・■縺ｩ縺薙∪縺ｧ繧堤ｩ榊・縺吶ｋ縺九阪↓豁｣隗｣縺檎┌縺・*縺薙→縺縺｣縺溘・    //     逶ｴ邱壹→蟷ｳ髱｢縺ｮ莠､轤ｹ縺ｫ遯・竊・髻ｳ貅舌′豁｣髱｢縺ｫ辟｡縺・→遯薙′螢√・荳ｭ縺ｫ豐医・
    //     遞懃ｷ壹・髢句哨轤ｹ縺ｫ遯・    竊・轤ｹ縺瑚ｷｳ縺ｶ・亥ｮ滓ｸｬ 3m・・    //     髢九＞縺ｦ縺・ｋ鬆伜沺縺ｮ驥榊ｿ・竊・驛ｨ螻九・螟悶∪縺ｧ縲碁幕縺・※縺・ｋ縲阪→謨ｰ縺医ｋ・磯哩謇峨〒 0.517・・    //     隍・焚縺ｮ髱｢縺ｧ譛蟆・      竊・譁懊ａ縺ｮ阮・＞譚ｿ縺梧妙髱｢縺ｫ迴ｾ繧後↑縺・    //   繝昴・繧ｿ繝ｫ縺ｯ縺薙・蝠上＞縺ｮ遲斐∴縺昴・繧ゅ・縲・*遨榊・遽・峇・昴・繝ｼ繧ｿ繝ｫ縺ｮ遏ｩ蠖｢**縲・    //   繝帙せ繝医′縲後％縺薙′謌ｸ蜿｣縲阪→鄂ｮ縺・◆譎らせ縺ｧ蠅・阜縺梧ｱｺ縺ｾ繧九・    //   ・医お繝ｪ繧｢縺ｮ繝医・繝ｭ繧ｸ縺ｯ繝帙せ繝医・浹髻ｿ逧・↑迥ｶ諷九・繧ｨ繝ｳ繧ｸ繝ｳ縺梧ｯ弱ヵ繝ｬ繝ｼ繝蟷ｾ菴輔°繧画ｸｬ繧九・    //     縺ｨ縺・≧蜑阪↓豎ｺ繧√◆蠅・阜縺ｫ豐ｿ縺・る幕縺榊・蜷医ｒ authoring 縺励↑縺・・縺瑚ｦ∫せ縲ゑｼ・    //
    // 菴輔′螟峨ｏ繧九°・亥ｮ滓ｸｬ縺ｨ縺ｮ蟇ｾ豈費ｼ・
    //   蠕捺擂縺ｯ髢画演縺ｧ繧る幕蜿｣邇・0.328 縺ゅ▲縺滂ｼ育ｪ薙′驛ｨ螻九・螟悶∪縺ｧ蠎・′縺｣縺ｦ縺・◆縺溘ａ・峨・    //   繝昴・繧ｿ繝ｫ縺ｪ繧画演縺檎洸蠖｢繧定ｦ・＞縺阪ｌ縺ｰ 0.0縲∽ｽ輔ｂ蝪槭′縺ｪ縺代ｌ縺ｰ 1.0 笏笏 繝輔Ν繝ｬ繝ｳ繧ｸ縺ｫ縺ｪ繧九・    //   邨瑚ｷｯ縺後檎┌縺九ｉ譛蛾剞縺ｮ繧ｲ繧､繝ｳ縺ｧ逕溘∪繧後ｋ縲阪％縺ｨ縺梧ｧ矩逧・↓襍ｷ縺阪↑縺・・    struct Portal {
        Vec3 center{0, 0, 0};
        Vec3 axisU{1, 0, 0};   // 遏ｩ蠖｢縺ｮ讓ｪ譁ｹ蜷托ｼ亥腰菴搾ｼ・        Vec3 axisV{0, 1, 0};   // 遏ｩ蠖｢縺ｮ邵ｦ譁ｹ蜷托ｼ亥腰菴搾ｼ・        float halfU = 0.6f;
        float halfV = 1.2f;
        bool active = true;
    };

    int addPortal(const Vec3& center, const Vec3& axisU, const Vec3& axisV,
                  float halfU, float halfV) {
        Portal p;
        p.center = center;
        p.axisU = normalized(axisU);
        p.axisV = normalized(axisV);
        p.halfU = std::max(halfU, 1e-3f);
        p.halfV = std::max(halfV, 1e-3f);
        portals_.push_back(p);
        return static_cast<int>(portals_.size()) - 1;
    }
    void updatePortal(int id, const Vec3& center, const Vec3& axisU, const Vec3& axisV,
                      float halfU, float halfV) {
        if (id < 0 || id >= static_cast<int>(portals_.size())) return;
        Portal& p = portals_[static_cast<std::size_t>(id)];
        p.center = center; p.axisU = normalized(axisU); p.axisV = normalized(axisV);
        p.halfU = std::max(halfU, 1e-3f); p.halfV = std::max(halfV, 1e-3f);
    }
    int portalCount() const { return static_cast<int>(portals_.size()); }
    void clearPortals() { portals_.clear(); }

    // 繝昴・繧ｿ繝ｫ縺後←繧後□縺鷹幕縺・※縺・ｋ縺九ｒ蟶ｯ蝓溷挨縺ｫ貂ｬ繧九・    //   outFrac6 : 蟶ｯ蝓溘＃縺ｨ縺ｮ縲碁壹ｋ蜑ｲ蜷医・0..1)縲ら洸蠖｢縺悟ｮ悟・縺ｫ蝪槭′繧後ｌ縺ｰ 0縲∫ｴ騾壹＠縺ｪ繧・1縲・    //   outPoint : 髢九＞縺ｦ縺・ｋ驛ｨ蛻・・驥阪∩莉倥″驥榊ｿ・ｼ医Ρ繝ｼ繝ｫ繝会ｼ峨・*螳壻ｽ阪↓菴ｿ縺・*縲・    //              繧ｨ繝阪Ν繧ｮ繝ｼ縺ｨ譁ｹ蜷代′蜷後§遨榊・縺九ｉ蜃ｺ繧九・縺ｧ縲∽ｸ｡閠・′縺壹ｌ縺ｦ鬟帙・縺薙→縺後↑縺・・    //
    //   蟶ｯ蝓溷ｷｮ縺ｯ繝輔Ξ繝阪Ν繧ｾ繝ｼ繝ｳ縺ｮ螟ｧ縺阪＆縺九ｉ蜃ｺ繧九るｫ伜沺縺ｯ繧ｾ繝ｼ繝ｳ縺悟ｰ上＆縺・・縺ｧ髫咎俣縺ｫ蜿弱∪縺｣縺ｦ
    //   繧医￥騾壹ｊ縲∽ｽ主沺縺ｯ繧ｾ繝ｼ繝ｳ縺悟､ｧ縺阪＞縺ｮ縺ｧ蝪槭′繧後◆驛ｨ蛻・↓謗帙°繧・・昴碁幕縺上→譏弱ｋ縺上↑繧九阪・    //   繧ｾ繝ｼ繝ｳ縺ｮ荳ｭ蠢・・逶ｴ邱壹→遏ｩ蠖｢髱｢縺ｮ莠､轤ｹ・磯｣邯壹↓蜍輔￥・峨・*遏ｩ蠖｢縺ｧ蛻・ｉ繧後ｋ縺ｮ縺ｧ證ｴ襍ｰ縺励↑縺・* 笏笏
    //   縺薙％縺悟ｾ捺擂縺ｨ縺ｮ豎ｺ螳夂噪縺ｪ驕輔＞縺ｧ縲∽ｸｭ蠢・・鄂ｮ縺肴婿縺ｫ遲斐∴縺檎┌縺・撫鬘後′豸医∴繧九・    bool portalOpenBands(const Portal& pt, const Vec3& listener, const Vec3& source,
                         float* outFrac6, Vec3* outPoint) const {
        for (int b = 0; b < kNumBands; ++b) outFrac6[b] = 1.0f;
        if (!pt.active) return false;

        Vec3 n = normalized(cross(pt.axisU, pt.axisV));
        // 豕慕ｷ壹・繝ｪ繧ｹ繝翫・蛛ｴ縺ｸ蜷代￠繧九ゅ悟･･縺ｫ縺ゅｋ迚ｩ菴薙阪ｒ蛻､螳壹☆繧九・縺ｫ隕√ｋ縲・        if (dot(n, listener - pt.center) < 0.0f) n = Vec3(-n.x, -n.y, -n.z);
        const Vec3 u = pt.axisU, v = pt.axisV;

        // 繧ｾ繝ｼ繝ｳ縺ｮ荳ｭ蠢・ｼ晉峩邱壹→遏ｩ蠖｢髱｢縺ｮ莠､轤ｹ・育洸蠖｢縺ｮ螟悶↓蜃ｺ縺溘ｉ遶ｯ縺ｸ蟇・○繧具ｼ峨・        const Vec3 ls = source - listener;
        const float den = dot(n, ls);
        Vec3 hitP = pt.center;
        if (std::fabs(den) > 1e-6f) {
            const float t = dot(n, pt.center - listener) / den;
            hitP = listener + ls * t;
        }
        float cu = dot(hitP - pt.center, u);
        float cv = dot(hitP - pt.center, v);
        cu = std::min(std::max(cu, -pt.halfU), pt.halfU);
        cv = std::min(std::max(cv, -pt.halfV), pt.halfV);

        const Vec3 zoneCenter = pt.center + u * cu + v * cv;
        const float d1 = std::max(length(zoneCenter - listener), 1e-3f);
        const float d2 = std::max(length(source - zoneCenter), 1e-3f);

        // 驕ｮ繧九ｂ縺ｮ繧・*繝ｪ繧ｹ繝翫・縺九ｉ隕九◆蠖ｱ**縺ｨ縺励※遏ｩ蠖｢髱｢縺ｸ關ｽ縺ｨ縺呻ｼ亥・螟夊ｧ貞ｽ｢縺ｮ髮・∪繧奇ｼ峨・        //   譁ｭ髱｢縺ｧ縺ｯ縺ｪ縺丞ｽｱ縺ｪ縺ｮ縺ｯ縲∵万繧√↓遶九▲縺溯埋縺・攸縺梧妙髱｢縺ｫ縺ｻ縺ｼ迴ｾ繧後↑縺・◆繧・ｼ亥ｮ滓ｸｬ縺ｧ遒ｺ隱搾ｼ峨・        struct Poly { float u[8], v[8]; int n; };
        static thread_local std::vector<Poly> polys;
        polys.clear();
        const float planeD = dot(n, pt.center - listener);
        auto project = [&](const Vec3& q, float& ou, float& ov) -> bool {
            const Vec3 dq = q - listener;
            const float dd = dot(n, dq);
            if (std::fabs(dd) < 1e-6f) return false;
            const float t = planeD / dd;
            if (t <= 0.0f) return false;
            const Vec3 p = listener + dq * t;
            ou = dot(p - pt.center, u);
            ov = dot(p - pt.center, v);
            return true;
        };
        auto hullInto = [&](const float* px, const float* py, int m, Poly& out) {
            int idx[8];
            for (int i = 0; i < m; ++i) idx[i] = i;
            std::sort(idx, idx + m, [&](int a, int b2) {
                return (px[a] != px[b2]) ? (px[a] < px[b2]) : (py[a] < py[b2]);
            });
            auto cr2 = [&](int o, int a, int b2) {
                return (px[a] - px[o]) * (py[b2] - py[o]) - (py[a] - py[o]) * (px[b2] - px[o]);
            };
            int st[18]; int k = 0;
            for (int i = 0; i < m; ++i) {
                while (k >= 2 && cr2(st[k - 2], st[k - 1], idx[i]) <= 0.0f) --k;
                st[k++] = idx[i];
            }
            const int lower = k + 1;
            for (int i = m - 2; i >= 0; --i) {
                while (k >= lower && cr2(st[k - 2], st[k - 1], idx[i]) <= 0.0f) --k;
                st[k++] = idx[i];
            }
            out.n = std::min(k - 1, 8);
            for (int i = 0; i < out.n; ++i) { out.u[i] = px[st[i]]; out.v[i] = py[st[i]]; }
        };
        // 笘・・繝ｼ繧ｿ繝ｫ髱｢繧医ｊ**螂･**縺ｫ縺ゅｋ迚ｩ菴薙・縲√・繝ｼ繧ｿ繝ｫ繧貞｡槭＞縺ｧ縺・↑縺・・        //   繝ｪ繧ｹ繝翫・縺九ｉ謌ｸ蜿｣雜翫＠縺ｫ隕九∴繧句･･縺ｮ驛ｨ螻九・蠎翫ｄ螟ｩ莠輔・縲√Ξ繧､縺後・繝ｼ繧ｿ繝ｫ繧・        //   騾壹ｊ謚懊￠縺・*蜈・*縺ｧ蠖薙◆繧九ｂ縺ｮ縲ゅ％繧後ｒ蝪槭＞縺ｧ縺・ｋ縺ｨ謨ｰ縺医ｋ縺ｨ縲∝・髢九〒繧・        //   髢句哨邇・′荳翫′繧峨↑縺・ｼ亥ｮ滓ｸｬ縺ｧ 90ﾂｰ 縺ｧ繧・0.343 豁｢縺ｾ繧翫□縺｣縺溷次蝗縺後％繧鯉ｼ峨・        //   蜈ｨ髢九・謇峨ｂ縲∵虻蜿｣縺ｮ螟悶∈謖ｯ繧悟・繧後・縲悟･･縺ｮ驛ｨ螻九・迚ｩ菴薙阪↓縺ｪ繧・笏笏 縺昴ｌ縺ｧ豁｣縺励＞縲・        //   髢句哨縺碁幕縺・※縺・ｋ縺九←縺・°縺ｨ縲√◎縺ｮ蜈医↓菴輔′縺ゅｋ縺九・蛻･縺ｮ隧ｱ縲・        auto beyondPortal = [&](const Obb& ob) {
            const float d = dot(ob.center - pt.center, n);
            const float ext = std::fabs(dot(ob.axisX, n)) * ob.halfExtents.x
                            + std::fabs(dot(ob.axisY, n)) * ob.halfExtents.y
                            + std::fabs(dot(ob.axisZ, n)) * ob.halfExtents.z;
            return (d + ext) < -0.01f;      // 螳悟・縺ｫ螂･蛛ｴ
        };
        for (const Instance& inst : instances_) {
            if (!inst.active || beyondPortal(inst.obb)) continue;
            if (inst.geomId >= 0 && inst.geomId < static_cast<int>(meshes_.size())
                && meshes_[static_cast<std::size_t>(inst.geomId)].used) {
                const MeshGeometry& g = meshes_[static_cast<std::size_t>(inst.geomId)];
                const int nt = g.bvh.triangleCount();
                for (int ti = 0; ti < nt && polys.size() < 2048; ++ti) {
                    const Triangle& lt = g.bvh.triangle(ti);
                    const Vec3 tv[3] = {meshLocalToWorldPoint(lt.v0, inst.obb),
                                        meshLocalToWorldPoint(lt.v1, inst.obb),
                                        meshLocalToWorldPoint(lt.v2, inst.obb)};
                    Poly pg; bool ok = true;
                    for (int i = 0; i < 3 && ok; ++i) ok = project(tv[i], pg.u[i], pg.v[i]);
                    if (!ok) continue;
                    pg.n = 3;
                    polys.push_back(pg);
                }
            } else {
                const Obb& ob = inst.obb;
                float px[8], py[8]; bool ok = true;
                for (int i = 0; i < 8 && ok; ++i) {
                    const float sx = (i & 1) ? 1.0f : -1.0f;
                    const float sy = (i & 2) ? 1.0f : -1.0f;
                    const float sz = (i & 4) ? 1.0f : -1.0f;
                    ok = project(ob.center + ob.axisX * (ob.halfExtents.x * sx)
                                           + ob.axisY * (ob.halfExtents.y * sy)
                                           + ob.axisZ * (ob.halfExtents.z * sz), px[i], py[i]);
                }
                if (!ok) continue;
                Poly pg; hullInto(px, py, 8, pg);
                if (pg.n >= 3) polys.push_back(pg);
            }
        }

        // 陦後＃縺ｨ縺ｫ縲悟ｽｱ縺ｮ蜥碁寔蜷医阪・陬憺寔蜷茨ｼ晞幕縺・※縺・ｋ蛹ｺ髢薙ｒ蜃ｺ縺励∝ｸｯ蝓溘＃縺ｨ縺ｮ譬ｸ縺ｧ遨阪・縲・        constexpr float kBandHz[kNumBands] = {125, 250, 500, 1000, 2000, 4000};
        constexpr int kRows = 32, kCols = 12;
        struct Span { float lo, hi; };
        static thread_local std::vector<Span> spans, openRow;
        static thread_local std::vector<int> begin_;
        openRow.clear();
        begin_.assign(kRows + 1, 0);
        const float rowH = 2.0f * pt.halfV / kRows;
        double area = 0.0, ccu = 0.0, ccv = 0.0;
        for (int j = 0; j < kRows; ++j) {
            begin_[j] = static_cast<int>(openRow.size());
            const float y = -pt.halfV + rowH * (j + 0.5f);
            spans.clear();
            for (const Poly& pg : polys) {
                float lo, hi;
                if (fresnel::polygonSpanAtY(pg.u, pg.v, pg.n, y, lo, hi))
                    spans.push_back(Span{lo, hi});
            }
            std::sort(spans.begin(), spans.end(),
                      [](const Span& a, const Span& b) { return a.lo < b.lo; });
            float cursor = -pt.halfU;
            auto emit = [&](float lo, float hi) {
                if (hi <= lo) return;
                openRow.push_back(Span{lo, hi});
                const double w = static_cast<double>(hi - lo) * rowH;
                area += w; ccu += w * 0.5 * (lo + hi); ccv += w * y;
            };
            for (const Span& sp : spans) {
                const float lo = std::min(std::max(sp.lo, -pt.halfU), pt.halfU);
                const float hi = std::min(std::max(sp.hi, -pt.halfU), pt.halfU);
                if (lo > cursor) emit(cursor, lo);
                cursor = std::max(cursor, hi);
                if (cursor >= pt.halfU) break;
            }
            if (cursor < pt.halfU) emit(cursor, pt.halfU);
        }
        begin_[kRows] = static_cast<int>(openRow.size());

        if (area <= 1e-9) {                       // 螳悟・縺ｫ蝪槭′繧後※縺・ｋ
            for (int b = 0; b < kNumBands; ++b) outFrac6[b] = 0.0f;
            if (outPoint) *outPoint = pt.center;
            return true;
        }
        if (outPoint)
            *outPoint = pt.center + u * static_cast<float>(ccu / area)
                                  + v * static_cast<float>(ccv / area);

        for (int b = 0; b < kNumBands; ++b) {
            const float lambda = 343.0f / kBandHz[b];
            const float r1 = std::max(std::sqrt(lambda * d1 * d2 / (d1 + d2)), 1e-3f);
            const float invR2 = 1.0f / (r1 * r1);
            auto kern = [&](float pu, float pv) {
                const float du = pu - cu, dv = pv - cv;
                return 1.0 / (1.0 + static_cast<double>(du * du + dv * dv) * invR2);
            };
            auto integ = [&](float lo, float hi, float pv) {
                if (hi <= lo) return 0.0;
                const float st = (hi - lo) / kCols;
                double acc = 0.0;
                for (int c = 0; c < kCols; ++c) acc += kern(lo + st * (c + 0.5f), pv);
                return acc * st;
            };
            double numer = 0.0, denom = 0.0;
            for (int j = 0; j < kRows; ++j) {
                const float y = -pt.halfV + rowH * (j + 0.5f);
                denom += integ(-pt.halfU, pt.halfU, y);      // 遏ｩ蠖｢蜈ｨ菴難ｼ晉ｴ騾壹＠縺ｮ縺ｨ縺・                for (int i = begin_[j]; i < begin_[j + 1]; ++i)
                    numer += integ(openRow[static_cast<std::size_t>(i)].lo,
                                   openRow[static_cast<std::size_t>(i)].hi, y);
            }
            outFrac6[b] = (denom > 1e-12)
                        ? static_cast<float>(std::min(1.0, numer / denom)) : 1.0f;
        }
        return true;
    }

    const Portal& portal(int i) const { return portals_[static_cast<std::size_t>(i)]; }

    // 繝昴・繧ｿ繝ｫ**邨檎罰縺ｧ螳滄圀縺ｫ螻翫￥驥・*縲る幕蜿｣邇・ｼ医←繧後□縺鷹幕縺・※縺・ｋ縺具ｼ峨↓縲・    // 菴咲ｽｮ髢｢菫ゅ〒豎ｺ縺ｾ繧・縺､縺ｮ菫よ焚繧呈寺縺代ｋ縲る幕蜿｣邇・◎縺ｮ繧ゅ・縺ｯ蟷ｾ菴輔・驥上↑縺ｮ縺ｧ豎壹＆縺ｪ縺・・    //
    //   竭 髢句哨縺ｮ謖・髄諤ｧ 窶ｦ 謌ｸ蜿｣縺ｯ豁｣髱｢縺ｸ蠑ｷ縺乗叛蟆・＠縲∵万繧√〒縺ｯ蠑ｱ縺・ｼ・os 蜑・ｼ峨・    //      荳｡蛛ｴ・医Μ繧ｹ繝翫・蛛ｴ繝ｻ髻ｳ貅仙・・峨↓謗帙°繧九・    //   竭｡ 驕蝗槭ｊ縺ｮ蠎・′繧頑錐螟ｱ 窶ｦ 繝ｪ繧ｹ繝翫・竊帝幕蜿｣竊帝浹貅舌・逶ｴ邱壹ｈ繧企聞縺・ゅ◎縺ｮ豈斐〒貂帙ｋ縲・    //
    // 縺ｪ縺懆ｦ√ｋ縺具ｼ亥ｮ滓ｸｬ・・
    //   縲檎峩邱壹′遏ｩ蠖｢繧呈ｨｪ蛻・ｋ縺九阪・莠悟､蛻､螳壹ｒ螟悶＠縺ｦ蟠厄ｼ・.8dB・峨・豸医∴縺溘′縲・    //   莉｣繧上ｊ縺ｫ**菴咲ｽｮ縺ｫ繧医ｋ螟牙喧縺ｾ縺ｧ蟷ｳ繧峨↓縺ｪ縺｣縺・*・磯浹貅舌ｒ謌ｸ蜿｣縺九ｉ 1.6m 螟悶＠縺ｦ繧・    //   0.978 縺ｮ縺ｾ縺ｾ・峨る幕蜿｣邇・・繧ｾ繝ｼ繝ｳ縺ｮ荳ｭ蠢・ｒ遏ｩ蠖｢蜀・∈荳ｸ繧√ｋ縺ｮ縺ｧ縲・    //   邱壹′縺ｩ繧後□縺大､悶ｌ縺ｦ繧ゅ檎洸蠖｢縺ｮ遶ｯ縺九ｉ隕九◆髢九″蜈ｷ蜷医阪＠縺玖ｦ九※縺・↑縺・・    //   螟悶ｌ縺溯ｷ晞屬繧貞柑縺九○繧九・縺後％縺ｮ2菫よ焚縲ゅ←縺｡繧峨ｂ髢ｾ蛟､繧呈戟縺溘↑縺・・縺ｧ蟠悶・謌ｻ繧峨↑縺・・    bool portalCoupling(const Portal& pt, const Vec3& listener, const Vec3& source,
                        float* outBands6, Vec3* outPoint) const {
        Vec3 cp(0, 0, 0);
        if (!portalOpenBands(pt, listener, source, outBands6, &cp)) return false;
        if (outPoint) *outPoint = cp;

        Vec3 n = normalized(cross(pt.axisU, pt.axisV));
        const Vec3 toL = listener - cp;
        const Vec3 toS = source - cp;
        const float dl = std::max(length(toL), 1e-3f);
        const float ds = std::max(length(toS), 1e-3f);
        // 髢句哨縺ｮ謖・髄諤ｧ縲よｳ慕ｷ壹°繧蛾屬繧後ｋ縺ｻ縺ｩ蠑ｱ縺・り｣丞・縺ｸ縺ｯ蜃ｺ縺輔↑縺・ｼ・ 縺ｧ蛻・ｋ・峨・        const float cl2 = std::fabs(dot(toL, n)) / dl;
        const float cs2 = std::fabs(dot(toS, n)) / ds;
        // 驕蝗槭ｊ縺ｮ蠎・′繧頑錐螟ｱ・域険蟷・ｯ費ｼ峨ゅ∪縺｣縺吶＄騾壹ｌ縺ｰ 1縲・        const float direct = std::max(length(source - listener), 1e-3f);
        const float detour = std::min(direct / (dl + ds), 1.0f);

        const float k = scene_detail::clamp01(cl2) * scene_detail::clamp01(cs2) * detour;
        for (int b = 0; b < kNumBands; ++b) outBands6[b] *= k;
        return true;
    }

    // 縺薙・邨瑚ｷｯ繧呈髪驟阪☆繧九・繝ｼ繧ｿ繝ｫ縲らｷ壼・縺檎洸蠖｢繧帝壹ｋ繧ゅ・繧定ｿ斐☆・育┌縺代ｌ縺ｰ nullptr・峨・    //   笘・後・繝ｼ繧ｿ繝ｫ縺後≠繧九↑繧峨・繝ｼ繧ｿ繝ｫ縺悟髪荳縺ｮ遲斐∴縲阪ｒ1邂・園縺ｧ蛻､螳壹☆繧九◆繧√・蜈･蜿｣縲・    //     蜷後§蝠上＞縺ｫ譌ｧ邨瑚ｷｯ・亥燕蟾晢ｼ矩幕蜿｣遨榊・・峨→繝昴・繧ｿ繝ｫ縺ｮ2縺､縺檎ｭ斐∴繧呈戟縺､縺ｨ縲・    //     迚・婿縺悟商縺上↑縺｣縺ｦ遏帷崟縺吶ｋ・亥ｮ滓ｸｬ: 髢峨§縺滄Κ螻九↑縺ｮ縺ｫ蝗樊釜縺・-39dB 縺ｮ蠎翫ｒ菴懊ｊ縲・    //     螢√・陬上□縺第攝雉ｪ縺ｫ髢｢菫ゅ↑縺丞ｹｳ蝮ｦ縺ｫ縺ｪ縺｣縺ｦ縺・◆・峨・    const Portal* governingPortal(const Vec3& listener, const Vec3& source) const {
        if (portals_.empty()) return nullptr;
        const Vec3 d = source - listener;
        const float dist = length(d);
        if (dist < 1e-4f) return nullptr;
        const Vec3 dir = d * (1.0f / dist);
        for (const Portal& pt : portals_) {
            if (!pt.active) continue;
            const Vec3 pn = normalized(cross(pt.axisU, pt.axisV));
            const float dn = dot(pn, dir);
            if (std::fabs(dn) < 1e-4f) continue;
            const float t = dot(pn, pt.center - listener) / dn;
            if (t <= 0.0f || t >= dist) continue;
            const Vec3 hit = listener + dir * t;
            if (std::fabs(dot(hit - pt.center, pt.axisU)) > pt.halfU) continue;
            if (std::fabs(dot(hit - pt.center, pt.axisV)) > pt.halfV) continue;
            return &pt;
        }
        return nullptr;
    }

    // atPoint 繧呈ｸ｡縺吶→縲√ヵ繝ｬ繝阪Ν繧ｾ繝ｼ繝ｳ縺ｮ荳ｭ蠢・ｒ**縺昴・轤ｹ**縺ｫ鄂ｮ縺上・    //
    // 笘・↑縺懆ｦ√ｋ縺具ｼ亥ｮ滓ｸｬ縺ｧ貎ｰ縺励◆螟ｱ謨暦ｼ・
    //   荳ｭ蠢・ｒ縲後Μ繧ｹ繝翫・縺ｨ髻ｳ貅舌ｒ邨舌・逶ｴ邱壹→蟷ｳ髱｢縺ｮ莠､轤ｹ縲阪↓鄂ｮ縺上→縲・浹貅舌′謌ｸ蜿｣縺ｮ豁｣髱｢縺ｫ
    //   辟｡縺・→縺阪◎縺ｮ逶ｴ邱壹・**螢√・荳ｭ**繧帝壹ｋ縲ゅ☆繧九→繧ｾ繝ｼ繝ｳ縺ｯ謌ｸ蜿｣縺ｧ縺ｯ縺ｪ縺丞｣√・荳翫↓荵励ｊ縲・    //   貂ｬ縺｣縺ｦ縺・ｋ縺ｮ縺後梧演縺ｮ髢九″蜈ｷ蜷医阪〒縺ｯ縺ｪ縺上梧虻蜿｣縺ｨ繧ｾ繝ｼ繝ｳ縺ｮ髱｢遨肴ｯ斐阪→縺・≧
    //   蝗ｺ螳壼､縺ｫ縺ｪ繧九ょｮ滓ｸｬ・域演 0ﾂｰ竊・0ﾂｰ・・
    //       125Hz 0.041 竊・0.183(10ﾂｰ) 竊・0.203(90ﾂｰ)   ・・10ﾂｰ縺ｧ鬆ｭ謇薙■
    //       4kHz  0.000 竊・0.000      竊・0.000        ・・荳蠎ｦ繧る幕縺九↑縺・    //   蟷ｾ菴輔〒縺ｯ髫咎俣縺ｯ 10ﾂｰ縺ｧ1.8cm縲・0ﾂｰ縺ｧ1.2m ・・67蛟埼幕縺上・縺ｫ縲・浹縺ｧ縺ｯ2.3蛟阪＠縺句虚縺九↑縺・・    //   ・昴後←縺ｮ縺上ｉ縺・幕縺・◆縺九阪ｒ莨昴∴繧九→縺・≧繧ｳ繝ｳ繧ｻ繝励ヨ縺昴・繧ゅ・繧貞､悶＠縺ｦ縺・◆縲・    //
    //   蝗樊釜縺ｧ縺ｯ髻ｳ縺悟ｮ滄圀縺ｫ騾壹ｋ蝣ｴ謇縺ｯ髢句哨縺ｪ縺ｮ縺ｧ縲√◎縺薙ｒ荳ｭ蠢・↓縺吶ｋ縲・    //   縺吶ｋ縺ｨ鬮伜沺・医だ繝ｼ繝ｳ縺悟ｰ上＆縺・ｼ峨・髢句哨縺ｫ蜿弱∪縺｣縺ｦ 1.0 縺ｾ縺ｧ荳翫′繧翫・    //   縲碁幕縺上→譏弱ｋ縺上↑繧九阪′讒区・縺九ｉ蜃ｺ繧九・    //
    //   nullptr 縺ｮ縺ｨ縺阪・蠕捺擂縺ｩ縺翫ｊ逶ｴ邱壹→縺ｮ莠､轤ｹ縲ら峩謗･髻ｳ縺ｮ蛻､螳壹・縺薙■繧峨・縺ｾ縺ｾ
    //   ・亥ｽｱ蠅・阜縺ｮ蟠悶ｒ豸医＠縺滉ｿｮ豁｣縺後◎縺薙↓荵励▲縺ｦ縺・ｋ縺ｮ縺ｧ螢翫＆縺ｪ縺・ｼ峨・    bool apertureFresnelBands(const Vec3& listener, const Vec3& source,
                              float outFrac[kNumBands],
                              const Vec3* atPoint = nullptr) const {
        for (int b = 0; b < kNumBands; ++b) outFrac[b] = 1.0f;

        Vec3 axis = source - listener;
        const float dist = length(axis);
        if (dist < 1e-4f) return false;
        axis = axis * (1.0f / dist);
        // 笘・ｸｬ繧矩擇縺ｯ**1譫壹〒縺ｯ雜ｳ繧翫↑縺・*縲らｵ瑚ｷｯ縺碁壹ｊ謚懊￠繧句｣√ｒ鬆・↓諡ｾ縺・√◎繧後◇繧後・髱｢縺ｧ
        //   貂ｬ縺｣縺ｦ**譛繧ら強縺・園・域怙蟆擾ｼ・*繧呈治繧九・        //   1譫壹□縺代□縺ｨ縲梧怙蛻昴↓繝ｬ繧､縺悟ｽ薙◆縺｣縺溷｣√阪〒髱｢縺梧ｱｺ縺ｾ繧九・縺ｧ縲√Μ繧ｹ繝翫・縺悟虚縺・※
        //   蠖薙◆繧雁・縺悟・繧梧崛繧上▲縺溽椪髢薙↓蛟､縺碁｣帙・縲ょ鴻魑･驟咲ｽｮ縺ｮ謌ｸ蜿｣縺ｧ螳滓ｸｬ:
        //     x=-1.15  蠖薙◆繧・z=4縺ｮ螢・ 髢句哨邇・0.275
        //     x=-1.10  蠖薙◆繧・z=0縺ｮ螢・ 髢句哨邇・0.543   竊・5cm 縺ｧ 2蛟・        //   邨瑚ｷｯ縺ｯ**荳｡譁ｹ縺ｮ謌ｸ蜿｣繧帝壹▲縺ｦ縺・ｋ**縺ｮ縺ｫ迚・婿縺励°貂ｬ縺｣縺ｦ縺・↑縺九▲縺溘√→縺・≧縺縺代・隧ｱ縲・        //   譛蟆上ｒ謗｡繧後・縲・擇縺悟｢励∴縺ｦ繧ゅ◎縺ｮ蛟､縺悟､ｧ縺阪￠繧後・邨先棡縺ｯ螟峨ｏ繧峨↑縺・・縺ｧ霍ｳ縺ｰ縺ｪ縺・・        //   繝繧ｯ繝医ｒ騾壹ｋ髻ｳ縺ｯ譛繧ら強縺・妙髱｢縺ｧ豎ｺ縺ｾ繧九√→縺・≧迚ｩ逅・→繧ょ粋縺・・        //   笘・ｳｨ諢・ 莉･蜑阪％縺薙〒隧ｦ縺輔ｌ縺溘・*蜷後§髱｢**繧堤ｵ瑚ｷｯ譁ｹ蜷代∈蜑榊ｾ後↓縺壹ｉ縺励※隍・焚譫壹阪・
        //     1繝薙ャ繝医ｂ螟峨ｏ繧峨↑縺九▲縺滂ｼ亥ｽ鍋┯縺ｧ縲√★繧峨＠縺ｦ繧りｦ九※縺・ｋ縺ｮ縺ｯ蜷後§螢・ｼ峨・        //     隕√ｋ縺ｮ縺ｯ髱｢縺ｮ繧ｪ繝輔そ繝・ヨ縺ｧ縺ｯ縺ｪ縺・*蛻･縺ｮ螢・*縺縺｣縺溘・        //   笘・園螻槭・蛻､螳壹・縲檎ｷ壼・縺・*螳滉ｽ薙↓蠖薙◆繧・*縺九阪↓縺励※縺ｯ縺・￠縺ｪ縺・ゅ◎繧後□縺ｨ謌ｸ蜿｣繧・        //     縺吶ｊ謚懊￠縺溽椪髢薙↓縺昴・螢√′蛟呵｣懊°繧芽誠縺｡繧九′縲∬誠縺｡繧狗峩蜑阪・蛟､縺ｯ 1 縺ｧ縺ｯ縺ｪ縺・        //     ・亥ｮ滓ｸｬ: x=0.90 縺ｧ 0.159 縺縺｣縺滄擇縺・x=0.95 縺ｧ豸医∴縲∝・菴薙′ 0.026 竊・0.101 縺ｫ霍ｳ繧薙□・峨・        //     蛻､螳壹・縲檎ｵ瑚ｷｯ縺後◎縺ｮ**髱｢繧呈ｨｪ蛻・ｋ**縺九阪↓縺吶ｋ縲よ虻蜿｣繧帝壹ｊ謚懊￠縺ｦ繧る擇縺ｯ讓ｪ蛻・▲縺ｦ
        //     縺・ｋ縺ｮ縺ｧ關ｽ縺｡縺ｪ縺・る・↓蠎翫・螟ｩ莠輔・豌ｴ蟷ｳ縺ｪ邨瑚ｷｯ縺ｨ莠､繧上ｉ縺ｪ縺・・縺ｧ蜈･繧峨↑縺・        //     ・亥・繧後※縺励∪縺・→驛ｨ螻九・蠎翫〒蜈ｨ驛ｨ縺梧ｽｰ繧後ｋ・峨・        //   笘・Ξ繧､繧帝ｲ繧√↑縺後ｉ謦・■逶ｴ縺呎婿蠑上ｂ荳榊庄縲ょ｣√・蜀・Κ縺九ｉ謦・▽縺ｨ霍晞屬縺ｻ縺ｼ 0 縺ｮ蠖薙◆繧翫′
        //     霑斐ｊ縲・cm 縺壹▽縺励°騾ｲ繧√★縺ｫ蜴壹＆ 30cm 縺ｮ螢√ｒ謚懊￠繧峨ｌ縺ｪ縺・ｼ亥ｮ滓ｸｬ縺ｧ 12 豁ｩ縺ｨ繧ょ酔縺伜｣・ｼ峨・        //   荳九・蠖ｱ縺ｮ謚募ｽｱ縺後←縺ｮ縺ｿ縺｡蜈ｨ繧､繝ｳ繧ｹ繧ｿ繝ｳ繧ｹ繧貞屓繧九・縺ｧ縲√％縺薙・ O(n) 縺ｯ隱､蟾ｮ縲・        constexpr int kMaxPlanes = 4;
        int   planeInst[kMaxPlanes] = {};
        float planeRank[kMaxPlanes] = {};        // 莠､轤ｹ縺悟ｮ滉ｽ薙↓縺ｩ繧後□縺題ｿ代＞縺・m)縲ょｰ上＆縺・⊇縺ｩ蜉ｹ縺・        Vec3  planeNrm[kMaxPlanes];
        int nPlanes = 0;
        {
            const int ni = static_cast<int>(instances_.size());
            for (int i = 0; i < ni; ++i) {
                const Instance& inst = instances_[static_cast<std::size_t>(i)];
                if (!inst.active) continue;
                const Obb& ob = inst.obb;
                // 譚ｿ縺ｮ豕慕ｷ夲ｼ昴＞縺｡縺ｰ繧楢埋縺・ｻｸ縲・                const float hx = ob.halfExtents.x, hy2 = ob.halfExtents.y, hz = ob.halfExtents.z;
                Vec3 nn = ob.axisZ;
                if (hx <= hy2 && hx <= hz) nn = ob.axisX;
                else if (hy2 <= hx && hy2 <= hz) nn = ob.axisY;
                if (length(nn) < 1e-4f) continue;
                nn = normalized(nn);
                const float den = dot(nn, source - listener);
                if (std::fabs(den) < 1e-3f * dist) continue;        // 髱｢縺ｫ蟷ｳ陦鯉ｼ晄ｨｪ蛻・ｉ縺ｪ縺・                const float tt = dot(nn, ob.center - listener) / den;
                if (tt < 0.02f || tt > 0.98f) continue;             // 莠､轤ｹ縺檎ｷ壼・縺ｮ螟・                // 莠､轤ｹ縺悟ｮ滉ｽ薙°繧峨←繧後□縺鷹屬繧後※縺・ｋ縺具ｼ・ 縺ｪ繧牙｣√・荳ｭ・晏ｮ悟・縺ｫ蝪槭＞縺ｧ縺・ｋ・峨・                const Vec3 xp = listener + (source - listener) * tt;
                const Vec3 lp = obbToLocalPoint(xp, ob);
                const float ex = std::max(std::fabs(lp.x) - hx, 0.0f);
                const float ey = std::max(std::fabs(lp.y) - hy2, 0.0f);
                const float ez = std::max(std::fabs(lp.z) - hz, 0.0f);
                const float rank = std::sqrt(ex * ex + ey * ey + ez * ez);

                // 蜷後§蟷ｳ髱｢縺ｮ繧､繝ｳ繧ｹ繧ｿ繝ｳ繧ｹ縺ｯ縺ｾ縺ｨ繧√ｋ・域虻蜿｣縺ｮ蟾ｦ蜿ｳ縺ｮ螢√↑縺ｩ・峨ょｽｱ縺ｮ謚募ｽｱ縺ｯ
                //   縺ｩ縺ｮ縺ｿ縺｡蜈ｨ繧､繝ｳ繧ｹ繧ｿ繝ｳ繧ｹ繧定ｦ九ｋ縺ｮ縺ｧ縲∝酔縺伜ｹｳ髱｢繧剃ｺ悟ｺｦ貂ｬ繧区э蜻ｳ縺後↑縺・・                int same = -1;
                for (int k = 0; k < nPlanes; ++k) {
                    if (std::fabs(dot(planeNrm[k], nn)) < 0.999f) continue;
                    const Vec3 oc = instances_[static_cast<std::size_t>(planeInst[k])].obb.center;
                    if (std::fabs(dot(nn, ob.center - oc)) < 1e-3f) { same = k; break; }
                }
                if (same >= 0) {
                    if (rank < planeRank[same]) { planeInst[same] = i; planeRank[same] = rank; }
                    continue;
                }
                int slot = nPlanes;
                if (nPlanes >= kMaxPlanes) {
                    // 縺・▲縺ｱ縺・↑繧峨√＞縺｡縺ｰ繧灘柑縺・※縺・↑縺・ｼ井ｺ､轤ｹ縺悟ｮ滉ｽ薙°繧蛾□縺・ｼ峨ｂ縺ｮ縺ｨ蜈･繧梧崛縺医ｋ縲・                    int worst = 0;
                    for (int k = 1; k < kMaxPlanes; ++k)
                        if (planeRank[k] > planeRank[worst]) worst = k;
                    if (planeRank[worst] <= rank) continue;
                    slot = worst;
                } else ++nPlanes;
                planeInst[slot] = i;
                planeRank[slot] = rank;
                planeNrm[slot] = nn;
            }
        }
        if (nPlanes == 0) {
            // 笘・ｽ薙◆繧峨↑縺上※繧ゅご繝ｼ繝医ｒ蛻・▲縺ｦ縺ｯ縺・￠縺ｪ縺・ょ・繧九→蠖ｱ蠅・阜縺ｮ螟門・縺ｧ髻ｳ驥上′霍ｳ縺ｶ
            //   ・亥ｮ滓ｸｬ: 0.285 竊・0.563・峨ゅ☆縺占х縺ｫ螢√′縺ゅｋ縺ｪ繧峨√ヵ繝ｬ繝阪Ν繧ｾ繝ｼ繝ｳ縺ｯ縺ｾ縺
            //   縺昴・螢√↓蜊雁・髫縺輔ｌ縺ｦ縺・ｋ 笏笏 邵√ｒ騾壹ｊ驕弱℃繧九↓縺､繧後※髢句哨邇・′ 1 縺ｸ**騾｣邯壹↓**
            //   霑代▼縺上・縺梧ｭ｣縺励＞縲ゅ□縺九ｉ縲檎ｵ瑚ｷｯ縺ｫ譛繧りｿ代＞驕ｮ阡ｽ迚ｩ縲阪ｒ驕ｸ繧薙〒貂ｬ繧顔ｶ壹￠繧九・            //   邨瑚ｷｯ縺梧悽蠖薙↓髢九￠縺ｦ縺・ｌ縺ｰ縲√だ繝ｼ繝ｳ縺ｯ縺ｻ縺ｼ髢九＞縺ｦ縺・※閾ｪ蜍慕噪縺ｫ 1 縺ｫ縺ｪ繧九・            float best = 1e30f;
            int bestId = -1;
            const int ni = static_cast<int>(instances_.size());
            for (int i = 0; i < ni; ++i) {
                const Instance& inst = instances_[static_cast<std::size_t>(i)];
                if (!inst.active) continue;
                // 邱壼・縺ｨ OBB 荳ｭ蠢・・霍晞屬・郁ｿ代＆縺ｮ逶ｮ螳峨ょ宍蟇・〒縺ｪ縺上※繧医＞・峨・                const Vec3 d0 = inst.obb.center - listener;
                const float t2 = std::min(std::max(dot(d0, axis), 0.0f), dist);
                const float dd = length(d0 - axis * t2);
                if (dd < best) { best = dd; bestId = i; }
            }
            if (bestId < 0) return false;
            planeInst[nPlanes++] = bestId;
        }

        const float directLen = std::max(length(source - listener), 1e-4f);

        for (int b = 0; b < kNumBands; ++b) outFrac[b] = 1.0f;

        // 笘・酔縺倬擇繧堤ｵ瑚ｷｯ譁ｹ蜷代∈縺壹ｉ縺励※隍・焚譫壹√→縺・≧縺ｮ縺ｯ**1繝薙ャ繝医ｂ螟峨ｏ繧峨↑縺九▲縺・*
        //   ・按ｱ1m 繧・譫壹〒繧ょ酔縺伜､・峨ょｹｳ髱｢縺ｮ譁ｭ髱｢縺ｯ縲梧万繧√↓蠑ｵ繧雁・縺励◆阮・＞髫懷ｮｳ迚ｩ縲阪ｒ
        //   隕九ｉ繧後↑縺・笏笏 45ﾂｰ縺ｮ謇峨・縺ｩ縺ｮ髱｢縺ｧ蛻・▲縺ｦ繧ょ字縺ｿ3cm縺ｮ遲九↓縺励°縺ｪ繧峨↑縺・・縺ｧ縲・        //   蜷後§螢√ｒ蜑榊ｾ後↓縺壹ｉ縺励※繧らｵ先棡縺ｯ螟峨ｏ繧峨↑縺・ゅ□縺九ｉ髱｢縺ｮ繧ｪ繝輔そ繝・ヨ縺ｯ蟒・＠縺溘・        //   莉｣繧上ｊ縺ｫ荳翫〒**蛻･縺ｮ螢・*繧呈鏡縺｣縺ｦ縺ゅｋ縲ゅ％縺薙・縺昴ｌ繧帝・↓蝗槭ｋ縲・        for (int pl = 0; pl < nPlanes; ++pl) {
        const Obb& planeObb = instances_[static_cast<std::size_t>(planeInst[pl])].obb;

        // 髱｢縺ｮ蜷代″縺ｯ**螢∬・霄ｫ縺ｮ蠖｢**縺九ｉ豎ｺ繧√ｋ縲０BB 縺ｮ縺・■縺ｰ繧楢埋縺・ｻｸ・晄攸縺ｮ豕慕ｷ壹・        //   繝ｬ繧､縺悟ｽ薙◆縺｣縺滄擇縺ｮ豕慕ｷ壹ｒ菴ｿ縺｣縺ｦ縺ｯ縺・￠縺ｪ縺・ゅ☆繧後☆繧後・蜈･蟆・□縺ｨ繝ｬ繧､縺ｯ豁｣髱｢縺ｧ縺ｯ縺ｪ縺・        //   縲悟字縺ｿ蛛ｴ縺ｮ髱｢縲阪↓蠖薙◆繧翫√◎縺ｮ豕慕ｷ壹・譚ｿ縺ｫ**蝙ら峩**縺ｪ縺ｮ縺ｧ縲∝｣√ｒ邵ｦ縺ｫ蛻・▲縺・        //   縺ｻ縺ｼ遨ｺ縺｣縺ｽ縺ｮ譁ｭ髱｢繧定ｦ九※縺励∪縺・ｼ亥ｮ滓ｸｬ: 蠖ｱ蠅・阜縺ｮ謇句燕縺ｧ髢句哨邇・′ 0.485 竊・0.947 縺ｫ霍ｳ繧薙□・峨・        Vec3 nrm;
        {
            const float hx = planeObb.halfExtents.x, hy2 = planeObb.halfExtents.y,
                        hz = planeObb.halfExtents.z;
            Vec3 thin = planeObb.axisZ;
            if (hx <= hy2 && hx <= hz) thin = planeObb.axisX;
            else if (hy2 <= hx && hy2 <= hz) thin = planeObb.axisY;
            // 繝ｪ繧ｹ繝翫・蛛ｴ繧貞髄縺代ｋ・育ｬｦ蜿ｷ縺ｯ蝓ｺ蠎輔・菴懊ｊ譁ｹ縺ｫ蠖ｱ髻ｿ縺吶ｋ縺縺代□縺後∵純縺医※縺翫￥・峨・            if (dot(thin, axis) > 0.0f) thin = Vec3(-thin.x, -thin.y, -thin.z);
            nrm = thin;
        }
        if (length(nrm) < 1e-4f) nrm = axis;
        nrm = normalized(nrm);
        Vec3 u = cross(nrm, Vec3(0, 1, 0));
        if (length(u) < 1e-3f) u = cross(nrm, Vec3(1, 0, 0));
        u = normalized(u);
        const Vec3 v = normalized(cross(nrm, u));

        // 髱｢蜀・・菴咲ｽｮ縺ｯ縲檎峩邱壹′蠖薙◆縺｣縺溽せ縲阪・*豺ｱ縺輔・驕ｮ阡ｽ迚ｩ縺ｮ荳ｭ蠢・*縺ｫ鄂ｮ縺上・        //   陦ｨ髱｢縺｡繧・≧縺ｩ縺ｫ蟷ｳ髱｢繧堤ｽｮ縺上→譁ｭ髱｢縺碁蛹悶☆繧・笏笏 髱｢荳翫・遞懃ｷ壹・荳芽ｧ貞ｽ｢縺ｯ荳｡遶ｯ縺ｨ繧・        //   蟷ｳ髱｢荳翫↓縺ゅｋ縺ｮ縺ｧ隨ｦ蜿ｷ縺悟､峨ｏ繧峨★縲∽ｺ､轤ｹ縺・縺､繧よ鏡縺医↑縺・ｼ亥ｮ滓ｸｬ縺ｧ繝｡繝・す繝･縺ｮ譁ｭ髱｢縺・        //   遨ｺ縺ｫ縺ｪ繧翫∝屓謚倥′ 1/20 縺ｫ貎ｰ繧後◆縺ｾ縺ｾ縺縺｣縺滂ｼ峨ょ字縺ｿ縺ｮ逵溘ｓ荳ｭ縺ｪ繧臥｢ｺ螳溘↓蛻・ｌ繧九・        //   笘・ｹｳ髱｢縺ｯ**辟｡髯舌↓蟒ｶ髟ｷ**縺励※縲∫峩邱壹→縺ｮ莠､轤ｹ繧剃ｸｭ蠢・↓縺吶ｋ縲・        //     繝ｬ繧､縺梧滋繧√◆轤ｹ繧偵◎縺ｮ縺ｾ縺ｾ菴ｿ縺・→縲√☆繧後☆繧後・蜈･蟆・〒莠､轤ｹ縺梧攸縺ｮ邵√↓蠑ｵ繧贋ｻ倥″縲・        //     繝ｪ繧ｹ繝翫・縺悟虚縺・※繧ゆｸｭ蠢・′蜍輔°縺ｪ縺・ｼ晞幕蜿｣邇・′鬆ｭ謇薙■縺ｫ縺ｪ繧・        //     ・亥ｮ滓ｸｬ: 蠖ｱ蠅・阜縺ｮ謇句燕縺ｧ 0.522 縺ｮ縺ｾ縺ｾ蟷ｳ繧峨↓縺ｪ繧翫√◎縺ｮ蜈医〒 1.0 縺ｸ鬟帙ｓ縺・峨・        //     蟷ｳ髱｢縺ｨ縺ｮ莠､轤ｹ縺ｪ繧峨∫ｸ√ｒ雜翫∴縺溷ｾ後ｂ騾｣邯壹↓螟悶∈蜍輔＞縺ｦ縺・￥縲・        const Vec3 ic = planeObb.center;
        Vec3 centerP;
        if (atPoint) {
            // 髢句哨縺ｮ轤ｹ繧帝・阡ｽ迚ｩ縺ｮ**蜴壹∩縺ｮ荳ｭ螟ｮ**縺ｸ關ｽ縺ｨ縺吶□縺托ｼ磯擇蜀・・菴咲ｽｮ縺ｯ蜍輔°縺輔↑縺・ｼ峨・            centerP = *atPoint - nrm * dot(*atPoint - ic, nrm);
        } else {
            const float den = dot(nrm, source - listener);
            centerP = (std::fabs(den) > 1e-6f)
                    ? listener + (source - listener) * (dot(nrm, ic - listener) / den)
                    : ic;
        }
        if (scene_detail::diffDebug())
            std::fprintf(stderr, "      [fres髱｢] L(%.2f) 髱｢%d/%d inst=%d 荳ｭ蠢・%.2f,%.2f,%.2f)\n",
                         listener.x, pl + 1, nPlanes, planeInst[pl],
                         centerP.x, centerP.y, centerP.z);
        const float d1 = std::max(length(centerP - listener), 1e-3f);
        const float d2 = std::max(length(source - centerP), 1e-3f);

        // 笘・・阡ｽ迚ｩ繧偵碁擇縺ｧ蛻・▲縺滓妙髱｢縲阪〒縺ｯ縺ｪ縺上・*繝ｪ繧ｹ繝翫・縺九ｉ隕九◆蠖ｱ**縲阪→縺励※髱｢縺ｫ關ｽ縺ｨ縺吶・        //
        //   譁ｭ髱｢縺ｧ縺ｯ譁懊ａ縺ｫ遶九▲縺溯埋縺・黄菴薙ｒ隕九ｉ繧後↑縺・・5ﾂｰ縺ｮ謇峨・縺ｩ縺ｮ髱｢縺ｧ蛻・▲縺ｦ繧・        //   蜴壹∩3cm縺ｮ遲九↓縺励°縺ｪ繧峨★縲・擇遨阪ｒ縺ｻ縺ｨ繧薙←蝪槭′縺ｪ縺・ｼ亥ｮ滓ｸｬ: 髱｢繧・譫壹↓蠅励ｄ縺励※繧・        //   蛟､縺・繝薙ャ繝医ｂ螟峨ｏ繧峨↑縺九▲縺滂ｼ峨・        //   縺ｨ縺薙ｍ縺悟ｮ滄圀縺ｫ髻ｳ繧呈ｭ｢繧√※縺・ｋ縺ｮ縺ｯ譁ｭ髱｢遨阪〒縺ｯ縺ｪ縺・*驕ｮ縺｣縺ｦ縺・ｋ隕九°縺代・蠎・＆**縺ｧ縲・        //   45ﾂｰ縺ｮ謇峨・謚募ｽｱ縺吶ｋ縺ｨ 1.2ﾂｷcos45ﾂｰ = 85cm 繧貞｡槭＄縲・        //   蟒ｺ遽蛾浹髻ｿ縺ｮ蜷域・騾城℃邇・′菴ｿ縺・幕蜿｣髱｢遨阪ｂ縺薙・謚募ｽｱ縺ｧ縲∵演縺ｮ髫咎俣縺・1竏団os ﾎｸ 縺ｧ
        //   蠅励∴繧具ｼ亥燕蜊翫ｆ縺｣縺上ｊ繝ｻ蠕悟濠縺ｧ諤･縺ｫ髢九￥・昴け繝ｬ繝・す繧ｧ繝ｳ繝会ｼ峨・縺ｯ縺薙％縺九ｉ蜃ｺ繧九・        //
        //   謚募ｽｱ縺ｯ繝ｪ繧ｹ繝翫・縺九ｉ縺ｮ**騾剰ｦ匁兜蠖ｱ**縲ゅ檎ｪ薙・縺・■縺薙・迚ｩ菴薙′縺ｩ繧後□縺大｡槭＞縺ｧ縺・ｋ縺九・        //   縺昴・繧ゅ・縺ｪ縺ｮ縺ｧ縲√Ξ繧､繧呈鋳縺・※謨ｰ縺医ｋ縺ｮ縺ｨ蜷後§縺薙→繧定ｧ｣譫千噪縺ｫ繧・▲縺ｦ縺・ｋ縺薙→縺ｫ縺ｪ繧九・        //   謇峨・謌ｸ蜿｣繝ｻ髢九″隗偵・蜿ら・縺励↑縺・らｮｱ縺ｧ繧ゆｸ芽ｧ貞ｽ｢縺ｧ繧ょ酔縺伜ｼ上〒蠖ｱ縺ｫ縺ｪ繧九・        //
        //   蠖ｱ縺ｯ**蜃ｸ螟夊ｧ貞ｽ｢**縺ｨ縺励※謖√▽縲ょ・縺ｪ繧芽｡後＃縺ｨ縺ｮ蛹ｺ髢薙′蠢・★1譛ｬ縺ｫ豎ｺ縺ｾ繧九・縺ｧ縲・        //   迚ｩ菴薙ｒ縺ｾ縺溘＞縺ｧ蛛ｶ螂・ｦ丞援繧剃ｽｿ縺・ｿ・ｦ√′豸医∴繧具ｼ亥柱髮・粋繧堤峩謗･蜿悶ｌ繧具ｼ峨・        //   莉･蜑阪・蛛ｶ螂・ｒ蜈ｨ迚ｩ菴薙∪縺ｨ繧√※謗帙￠縺ｦ縺・※縲∵妙髱｢縺梧磁縺吶ｋ謇縺ｧ蜀・､悶′蜿崎ｻ｢縺励※縺・◆縲・        struct Poly { float u[8], v[8]; int n; };
        constexpr int kMaxPoly = 2048;
        static thread_local std::vector<Poly> polys;
        polys.clear();

        // 轤ｹ繧偵Μ繧ｹ繝翫・縺九ｉ髱｢縺ｸ騾剰ｦ匁兜蠖ｱ縺吶ｋ縲る擇繧医ｊ謇句燕/蠕後ｍ縺ｫ蝗槭ｊ霎ｼ繧轤ｹ縺ｯ謐ｨ縺ｦ繧九・        const float planeD = dot(nrm, centerP - listener);
        auto project = [&](const Vec3& q, float& ou, float& ov) -> bool {
            const Vec3 dq = q - listener;
            const float den = dot(nrm, dq);
            if (std::fabs(den) < 1e-6f) return false;
            const float t = planeD / den;
            if (t <= 0.0f) return false;
            const Vec3 p = listener + dq * t;
            ou = dot(p - centerP, u);
            ov = dot(p - centerP, v);
            return true;
        };
        // 謚募ｽｱ縺励◆轤ｹ鄒､縺ｮ蜃ｸ蛹・ｼ亥腰隱ｿ繝√ぉ繧､繝ｳ・峨ょｽｱ縺ｮ霈ｪ驛ｭ縲・        auto hullInto = [&](const float* px, const float* py, int m, Poly& out) {
            int idx[8];
            for (int i = 0; i < m; ++i) idx[i] = i;
            std::sort(idx, idx + m, [&](int a, int b2) {
                return (px[a] != px[b2]) ? (px[a] < px[b2]) : (py[a] < py[b2]);
            });
            auto cross2 = [&](int o, int a, int b2) {
                return (px[a] - px[o]) * (py[b2] - py[o]) - (py[a] - py[o]) * (px[b2] - px[o]);
            };
            int st[18]; int k = 0;
            for (int i = 0; i < m; ++i) {
                while (k >= 2 && cross2(st[k - 2], st[k - 1], idx[i]) <= 0.0f) --k;
                st[k++] = idx[i];
            }
            const int lower = k + 1;
            for (int i = m - 2; i >= 0; --i) {
                while (k >= lower && cross2(st[k - 2], st[k - 1], idx[i]) <= 0.0f) --k;
                st[k++] = idx[i];
            }
            out.n = std::min(k - 1, 8);
            for (int i = 0; i < out.n; ++i) { out.u[i] = px[st[i]]; out.v[i] = py[st[i]]; }
        };

        for (const Instance& inst : instances_) {
            if (!inst.active || static_cast<int>(polys.size()) >= kMaxPoly) continue;
            if (inst.geomId >= 0 && inst.geomId < static_cast<int>(meshes_.size())
                && meshes_[static_cast<std::size_t>(inst.geomId)].used) {
                // 繝｡繝・す繝･・壻ｸ芽ｧ貞ｽ｢縺斐→縺ｫ蠖ｱ繧定誠縺ｨ縺吶ゆｸ芽ｧ貞ｽ｢縺ｯ蜃ｸ縺ｪ縺ｮ縺ｧ縺昴・縺ｾ縺ｾ菴ｿ縺医ｋ縲・                //   笘・｢・阜邂ｱ縺ｧ莉｣逕ｨ縺励※縺ｯ縺・￠縺ｪ縺・よ虻蜿｣縺ｮ遨ｺ縺・◆螢√ｒ荳譫壽攸縺ｨ縺励※謇ｱ縺｣縺ｦ縺励∪縺・・                //     蝗樊釜縺・1/20 縺ｫ貎ｰ繧後ｋ・亥ｮ滓ｸｬ・峨・                const MeshGeometry& g = meshes_[static_cast<std::size_t>(inst.geomId)];
                const int nt = g.bvh.triangleCount();
                for (int ti = 0; ti < nt && static_cast<int>(polys.size()) < kMaxPoly; ++ti) {
                    const Triangle& lt = g.bvh.triangle(ti);
                    const Vec3 tv[3] = {meshLocalToWorldPoint(lt.v0, inst.obb),
                                        meshLocalToWorldPoint(lt.v1, inst.obb),
                                        meshLocalToWorldPoint(lt.v2, inst.obb)};
                    Poly pg; pg.n = 0;
                    bool ok = true;
                    for (int i = 0; i < 3 && ok; ++i)
                        ok = project(tv[i], pg.u[i], pg.v[i]);
                    if (!ok) continue;
                    pg.n = 3;
                    polys.push_back(pg);
                }
            } else {
                // 邂ｱ・・鬆らせ繧呈兜蠖ｱ縺励※蜃ｸ蛹・ｒ蜿悶ｋ・茨ｼ晉ｮｱ縺ｮ蠖ｱ縺ｮ霈ｪ驛ｭ・峨・                const Obb& ob = inst.obb;
                float px[8], py[8];
                bool ok = true;
                for (int i = 0; i < 8 && ok; ++i) {
                    const float sx = (i & 1) ? 1.0f : -1.0f;
                    const float sy = (i & 2) ? 1.0f : -1.0f;
                    const float sz = (i & 4) ? 1.0f : -1.0f;
                    const Vec3 q = ob.center + ob.axisX * (ob.halfExtents.x * sx)
                                             + ob.axisY * (ob.halfExtents.y * sy)
                                             + ob.axisZ * (ob.halfExtents.z * sz);
                    ok = project(q, px[i], py[i]);
                }
                if (!ok) continue;
                Poly pg; pg.n = 0;
                hullInto(px, py, 8, pg);
                if (pg.n >= 3) polys.push_back(pg);
            }
        }
        if (polys.empty()) continue;      // 縺薙・髱｢縺ｫ縺ｯ蠖ｱ縺檎┌縺・ｼ昴％縺ｮ髱｢縺ｯ邨槭ｉ縺ｪ縺・
        // ================= 髢九＞縺ｦ縺・ｋ謇繧剃ｸ蠎ｦ縺縺題ｧ｣縺阪√◎縺ｮ荳翫〒蟶ｯ蝓溘ｒ蝗槭☆ =================
        //
        // 笘・碁幕蜿｣繧定ｦ九▽縺代※貂ｬ繧九阪％縺ｨ繧・*縺励※縺・↑縺・*縲る幕蜿｣縺ｨ縺・≧蟇ｾ雎｡縺ｯ縺薙％縺ｫ蟄伜惠縺励↑縺・・        //   髱｢縺ｮ荳翫ｒ荳讒倥↓闊舌ａ縺ｦ縲∫せ縺斐→縺ｫ縲悟ｽｱ縺ｫ蜈･縺｣縺ｦ縺・ｋ縺九阪□縺代ｒ隕九ｋ縲・        //   謇峨・謌ｸ蜿｣繝ｻ陜ｶ逡ｪ繝ｻ髢九″隗偵・荳蠎ｦ繧ょ盾辣ｧ縺励↑縺・よ演縺ｯ蠖ｱ繧定誠縺ｨ縺咏黄菴薙・荳縺､縺ｧ縲・        //   譛ｨ邂ｱ縺ｧ繧ら逃遉ｫ縺ｧ繧ょ酔縺伜ｼ上′蜷後§繧医≧縺ｫ襍ｰ繧九・        //
        // 蟶ｯ蝓溘＃縺ｨ縺ｮ驕輔＞繧偵←縺薙°繧牙・縺吶°・医％縺薙′菴募ｺｦ繧る俣驕輔∴縺滓園・・
        //   豕｢蜍輔→縺励※豁｣縺励＞縺ｮ縺ｯ縲・*髢句哨縺梧ｳ｢髟ｷ縺ｫ蟇ｾ縺励※螟ｧ縺阪＞縺ｻ縺ｩ繧医￥騾壹ｋ**縲阪・        //   縺縺九ｉ髢九￥縺ｨ鬮伜沺縺九ｉ蜈医↓邏騾壹ｊ縺ｫ縺ｪ繧・・昴碁幕縺上→譏弱ｋ縺上↑繧九阪・        //   縺ｨ縺薙ｍ縺檎ｪ薙ｒ**逶ｴ邱壹→蟷ｳ髱｢縺ｮ莠､轤ｹ**縺ｫ鄂ｮ縺上→縲・ｫ伜沺縺ｻ縺ｩ遯薙′蟆上＆縺上↑繧翫・        //   蟆上＆縺上↑繧九⊇縺ｩ謌ｸ蜿｣縺九ｉ髮｢繧後※髢句哨繧定ｦ句､ｱ縺・らｬｦ蜿ｷ縺碁・ｻ｢縺励※縲・        //   髢九＞縺ｦ繧るｫ伜沺縺御ｼｸ縺ｳ縺ｪ縺上↑繧具ｼ亥ｮ滓ｸｬ: 蜈ｨ髢九〒繧・4kHz 0.007・峨・        //
        //   縺九→縺・▲縺ｦ遯薙ｒ遞懃ｷ夂罰譚･縺ｮ髢句哨轤ｹ縺ｫ鄂ｮ縺上→霍ｳ縺ｶ・亥ｮ滓ｸｬ 3m・峨・        //   縺昴％縺ｧ**髢九＞縺ｦ縺・ｋ鬆伜沺縺昴・繧ゅ・縺ｮ驥榊ｿ・*縺ｫ鄂ｮ縺上よ演縺悟屓繧後・髢九＞縺ｦ縺・ｋ謇縺ｯ
        //   騾｣邯壹↓蜍輔￥縺ｮ縺ｧ縲√◎縺ｮ驥榊ｿ・ｂ騾｣邯壹↓蜍輔￥縲る屬謨｣逧・↑謗｢邏｢繧偵＠縺ｦ縺・↑縺・・縺ｧ鬟帙・縺ｪ縺・・        //
        // 謇矩・
        //   A) 螟ｧ縺阪＞蝗ｺ螳壹・遯薙〒縲∬｡後＃縺ｨ縺ｮ縲碁幕縺・※縺・ｋ蛹ｺ髢薙阪ｒ荳蠎ｦ縺縺大・縺呻ｼ亥ｹｾ菴輔・1蝗橸ｼ・        //   B) 縺昴・髱｢遨埼㍾蠢・ｒ蜃ｺ縺呻ｼ磯｣邯壹↓蜍輔￥轤ｹ・・        //   C) 蟶ｯ蝓溘＃縺ｨ縺ｫ縲・㍾蠢・ｒ荳ｭ蠢・→縺吶ｋ豕｢髟ｷ繧ｵ繧､繧ｺ縺ｮ譬ｸ縺ｧ髢九＞縺ｦ縺・ｋ謇繧堤ｩ阪・
        //      譬ｸ k(p) = 1/(1 + (|p竏帝㍾蠢ポ / r1_b)ﾂｲ)縲〉1_b 縺ｯ隨ｬ1繝輔Ξ繝阪Ν蜊雁ｾ・        //      髢句哨縺・r1_b 繧医ｊ螟ｧ縺阪￠繧後・譬ｸ縺ｯ縺ｻ縺ｼ髢句哨縺ｫ蜿弱∪繧・竊・1 縺ｫ霑代▼縺擾ｼ域・繧九＞・・        constexpr float kBandHz[kNumBands] = {125.0f, 250.0f, 500.0f, 1000.0f, 2000.0f, 4000.0f};
        constexpr int kRows = 32;      // v 譁ｹ蜷代・陦梧焚
        constexpr int kCols = 12;      // 蜷・玄髢薙・荳ｭ縺ｧ譬ｸ繧堤ｩ阪・轤ｹ謨ｰ・亥玄髢薙・遶ｯ縺ｯ蜴ｳ蟇・ｼ・        constexpr float kWindow = 3.0f;
        struct Span { float lo, hi; };
        static thread_local std::vector<Span> spans;    // 縺昴・陦後・蠖ｱ・井ｽ懈･ｭ逕ｨ・・        static thread_local std::vector<Span> openRow;  // 陦後＃縺ｨ縺ｮ髢九＞縺ｦ縺・ｋ蛹ｺ髢難ｼ磯｣邨舌＠縺ｦ菫晄戟・・        static thread_local std::vector<int> openBegin; // 陦・j 縺ｮ髢句ｧ倶ｽ咲ｽｮ

        // 驥榊ｿ・↓譬ｸ繧堤ｽｮ縺乗婿蠑上ｒ隧ｦ縺吶◆繧√・繧ｹ繧､繝・メ縲よ里螳壹・ OFF・井ｸ九・險倬鹸繧貞盾辣ｧ・峨・        const bool useCentroidKernel = false;

        // 笘・・ 螳滓ｸｬ縺ｧ蛻・°縺｣縺溘％縺ｨ・医％縺ｮ譁ｹ蠑上・譛ｪ螳梧・縲よ里螳壹〒縺ｯ菴ｿ縺｣縺ｦ縺・↑縺・ｼ俄・笘・        //   遯薙ｒ菴主沺縺ｮ螟ｧ縺阪＆縺ｫ蝗ｺ螳壹＠縺ｦ驥榊ｿ・∈譬ｸ繧堤ｽｮ縺上→縲・*驛ｨ螻九・螟門・縺後碁幕縺・※縺・ｋ縲阪→
        //   謨ｰ縺医ｉ繧後ｋ**縲ゆｻ募・繧翫・蟷ｳ髱｢縺ｯ驛ｨ螻九・螟悶∪縺ｧ莨ｸ縺ｳ縺ｦ縺・※縲√◎縺薙↓縺ｯ菴輔ｂ辟｡縺・・縺ｧ
        //   驕ｮ繧峨ｌ縺ｦ縺・↑縺・桶縺・↓縺ｪ繧翫・㍾蠢・′縺昴■繧峨∈蠑輔▲蠑ｵ繧峨ｌ繧九・        //   邨先棡縲・哩謇峨〒繧・0.517 縺悟・縺ｦ縲∵演縺ｮ蜈ｨ謗・ｼ輔〒 4% 縺励°蜍輔°縺ｪ縺九▲縺溘・        //   ・・莉･蜑阪・ ﾎｴ 驥阪∩縺ｯ縲梧ｸ幄｡ｰ縲阪・鬘斐ｒ縺励※**鬆伜沺繧堤ｵ槭ｋ蠖ｹ逶ｮ繧よ球縺｣縺ｦ縺・◆**縲・        //   逶ｴ縺吶↓縺ｯ ﾎｴ 縺ｨ縺ｯ蛻･縺ｫ縲・擇縺ｮ縺・■諢丞袖縺ｮ縺ゅｋ遽・峇繧呈ｱｺ繧√ｋ莉慕ｵ・∩縺瑚ｦ√ｋ縲・        //   ・磯Κ螻九→縺・≧讎ょｿｵ縺ｯ繧ｳ繧｢縺ｫ謖√◆縺ｪ縺・婿驥昴↑縺ｮ縺ｧ縲√◎縺薙・險ｭ險医°繧会ｼ・        const float lamLo = 343.0f / kBandHz[0];
        const float r1lo = std::sqrt(lamLo * d1 * d2 / (d1 + d2));
        if (r1lo < 1e-4f) continue;
        const float R = r1lo * kWindow;
        const float rowH = 2.0f * R / kRows;

        // 笏笏 A) 髢九＞縺ｦ縺・ｋ蛹ｺ髢薙ｒ陦後＃縺ｨ縺ｫ蜃ｺ縺呻ｼ亥ｹｾ菴輔・蟶ｯ蝓溘↓萓昴ｉ縺ｪ縺・・縺ｧ1蝗槭□縺托ｼ俄楳笏
        openRow.clear();
        openBegin.assign(kRows + 1, 0);
        double area = 0.0, cu = 0.0, cv = 0.0;
        for (int j = 0; j < kRows; ++j) {
            openBegin[j] = static_cast<int>(openRow.size());
            const float y = -R + rowH * (j + 0.5f);

            // 蠖ｱ・亥・螟夊ｧ貞ｽ｢・峨＃縺ｨ縺ｫ縲√％縺ｮ陦後ｒ讓ｪ蛻・ｋ蛹ｺ髢薙ｒ1譛ｬ縺壹▽蜿悶ｋ縲・            //   蜃ｸ縺ｪ縺ｮ縺ｧ蛹ｺ髢薙・蠢・★1譛ｬ・晉ｫｯ縺ｯ隗｣譫千噪縺ｫ豎ｺ縺ｾ繧九Ｖ 譁ｹ蜷代・謗｢邏｢縺励※縺・↑縺・・縺ｧ
            //   縺ｩ繧薙↑縺ｫ邏ｰ縺・囮髢薙ｂ蜿悶ｊ縺薙⊂縺輔↑縺・ょｽｱ縺ｯ驥阪↑縺｣縺ｦ繧医＞・亥柱髮・粋繧貞叙繧具ｼ峨・            spans.clear();
            for (const Poly& pg : polys) {
                float lo, hi;
                if (fresnel::polygonSpanAtY(pg.u, pg.v, pg.n, y, lo, hi))
                    spans.push_back(Span{lo, hi});
            }
            std::sort(spans.begin(), spans.end(),
                      [](const Span& a, const Span& b) { return a.lo < b.lo; });

            // 蠖ｱ縺ｮ**陬憺寔蜷・*・晞幕縺・※縺・ｋ蛹ｺ髢薙・            float cursor = -R;
            auto emit = [&](float lo, float hi) {
                if (hi <= lo) return;
                openRow.push_back(Span{lo, hi});
                const double w = static_cast<double>(hi - lo) * rowH;
                area += w; cu += w * 0.5 * (lo + hi); cv += w * y;
            };
            for (const Span& sp : spans) {
                const float lo = std::min(std::max(sp.lo, -R), R);
                const float hi = std::min(std::max(sp.hi, -R), R);
                if (lo > cursor) emit(cursor, lo);
                cursor = std::max(cursor, hi);
                if (cursor >= R) break;
            }
            if (cursor < R) emit(cursor, R);
        }
        openBegin[kRows] = static_cast<int>(openRow.size());
        if (area <= 1e-9) {                 // 縺ｩ縺薙ｂ髢九＞縺ｦ縺・↑縺・            for (int b = 0; b < kNumBands; ++b) outFrac[b] = 0.0f;
            continue;
        }

        // 笏笏 B) 髢九＞縺ｦ縺・ｋ鬆伜沺縺ｮ髱｢遨埼㍾蠢・る｣邯壹↓蜍輔￥轤ｹ縲や楳笏
        const float ku = static_cast<float>(cu / area);
        const float kv = static_cast<float>(cv / area);

        // 笏笏 C) 蟶ｯ蝓溘＃縺ｨ縺ｫ縲・㍾蠢・∪繧上ｊ縺ｮ豕｢髟ｷ繧ｵ繧､繧ｺ縺ｮ譬ｸ縺ｧ遨阪・ 笏笏
        for (int b = 0; b < kNumBands; ++b) {
            const float lambda = 343.0f / kBandHz[b];
            const float r1 = std::sqrt(lambda * d1 * d2 / (d1 + d2));
            if (r1 < 1e-4f) continue;
            const float invR2 = 1.0f / (r1 * r1);
            const float invLam2 = 2.0f / lambda;

            // 譌｢螳壹・ ﾎｴ・磯□蝗槭ｊ・峨↓繧医ｋ驥阪∩縲ゅ％繧後・貂幄｡ｰ縺ｧ縺ゅｋ縺ｨ蜷梧凾縺ｫ縲・            //   **髱｢縺ｮ縺・■諢丞袖縺ｮ縺ゅｋ遽・峇繧堤ｵ槭ｋ蠖ｹ逶ｮ**繧よ球縺｣縺ｦ縺・ｋ・亥､悶☆縺ｨ驛ｨ螻九・螟悶∪縺ｧ
            //   縲碁幕縺・※縺・ｋ縲阪→謨ｰ縺医※縺励∪縺・ｼ峨ょｽｹ蜑ｲ縺御ｺ後▽荵励▲縺ｦ縺・ｋ縺ｮ縺御ｻ翫・蠑ｱ轤ｹ縲・            auto kern = [&](float pu, float pv) -> double {
                if (useCentroidKernel) {
                    const float du = pu - ku, dv = pv - kv;
                    return 1.0 / (1.0 + static_cast<double>(du * du + dv * dv) * invR2);
                }
                const Vec3 p = centerP + u * pu + v * pv;
                const float dl = length(p - listener) + length(source - p) - directLen;
                const float nz = std::max(dl, 0.0f) * invLam2 * deltaWeight_;
                return 1.0 / (1.0 + static_cast<double>(nz) * nz);
            };
            auto integrate = [&](float lo, float hi, float pv) -> double {
                if (hi <= lo) return 0.0;
                const float step = (hi - lo) / kCols;
                double acc = 0.0;
                for (int c = 0; c < kCols; ++c) acc += kern(lo + step * (c + 0.5f), pv);
                return acc * step;
            };

            double numer = 0.0, denom = 0.0;
            for (int j = 0; j < kRows; ++j) {
                const float y = -R + rowH * (j + 0.5f);
                denom += integrate(-R, R, y);
                for (int i = openBegin[j]; i < openBegin[j + 1]; ++i)
                    numer += integrate(openRow[static_cast<std::size_t>(i)].lo,
                                       openRow[static_cast<std::size_t>(i)].hi, y);
            }
            const float here = (denom > 1e-12)
                             ? static_cast<float>(std::min(1.0, numer / denom))
                             : 1.0f;
            outFrac[b] = std::min(outFrac[b], here);
        }
        }   // 髱｢縺ｮ繝ｫ繝ｼ繝・        return true;
    }

    // 縲宣幕蜿｣縺ｮ繧ｨ繝阪Ν繧ｮ繝ｼ縲鷹・阡ｽ迚ｩ縺ｮ髱｢縺ｫ髢九＞縺ｦ縺・ｋ**髱｢遨・*(mﾂｲ)縺ｨ縲√◎縺ｮ螳溷柑蟷・m)繧定ｿ斐☆縲・    //
    //   笘・％繧後・蟷ｾ菴輔〒縺ｯ縺ｪ縺・*豕｢蜍募・縺ｮ驥・*縲ゅく繝ｫ繝偵・繝・ヵ縺ｮ髢句哨遨榊・縺ｮ髮｢謨｣霑台ｼｼ縺ｫ縺ゅ◆繧九・    //     蟷ｾ菴暮浹髻ｿ縺ｯ縲後Ξ繧､縲阪→縲檎ｨ懃ｷ壹阪＠縺狗衍繧峨★縲・*髢句哨髱｢遨阪→縺・≧讎ょｿｵ繧呈戟縺溘↑縺・*縲・    //     蜑榊ｷ昴・蠑上・蜊顔┌髯舌せ繧ｯ繝ｪ繝ｼ繝ｳ縺ｮ螳滄ｨ灘ｼ上〒縲√お繝阪Ν繧ｮ繝ｼ菫晏ｭ倥°繧牙ｰ弱°繧後◆繧ゅ・縺ｧ縺ｯ縺ｪ縺・・    //     縺縺九ｉ縲碁囮髢薙ｒ縺ｩ繧後□縺代・繧ｨ繝阪Ν繧ｮ繝ｼ縺碁壹ｋ縺九阪ｒ遞懃ｷ壽爾邏｢縺九ｉ邨槭ｊ蜃ｺ縺昴≧縺ｨ縺吶ｋ縺ｮ縺ｯ
    //     繝｢繝・Ν縺ｮ驕ｩ逕ｨ蝓溘・螟悶〒縲∝ｮ滄圀6蝗槫､ｱ謨励＠縺溘ょｽｹ蜑ｲ繧貞・縺代ｋ縺ｮ縺梧ｭ｣縺励＞:
    //       譁ｹ蜷托ｼ亥ｮ壻ｽ搾ｼ俄ｦ 蟷ｾ菴包ｼ育ｨ懃ｷ壽爾邏｢・峨ゅ％縺薙・蟷ｾ菴輔・譛ｬ鬆・    //       繧ｨ繝阪Ν繧ｮ繝ｼ   窶ｦ 髢句哨髱｢遨阪ゅ％縺薙・豕｢蜍募・
    //
    //   笘・｣邯壽ｧ縺ｮ縺溘ａ縺ｫ縲碁屬謨｣逧・↑謗｢邏｢縺ｮ荳翫↓貂ｬ螳壹ｒ荵励○縺ｪ縺・阪・    //     莉･蜑阪・縲碁幕縺・◆轤ｹ繧・縺､隕九▽縺代※縲√◎縺薙°繧峨・蟾ｮ縺玲ｸ｡縺励阪ｒ貂ｬ縺｣縺ｦ縺・◆縲・    //     隕九▽縺九ｋ轤ｹ縺悟､峨ｏ繧後・蛟､縺碁｣帙・縺ｮ縺ｧ縲√∪縺輔↓豸医＠縺溘°縺｣縺滓ｮｵ蟾ｮ繧定・蛻・〒菴懊▲縺ｦ縺・◆
    //     ・亥ｮ滓ｸｬ: 10ﾂｰ縺ｧ0.012 竊・15ﾂｰ縺ｧ4.000 竊・25ﾂｰ縺ｧ0.377・峨・    //     陦後＃縺ｨ縺ｫ縲梧栢縺代ｉ繧後ｋ蛹ｺ髢薙・髟ｷ縺輔阪ｒ莠悟・謗｢邏｢縺ｧ豎ゅａ縺ｦ雜ｳ縺嶺ｸ翫￡繧後・縲・    //     謇峨′蜍輔￥縺ｨ**蛹ｺ髢薙・遶ｯ縺碁｣邯壹↓蜍輔￥** so 髱｢遨阪ｂ騾｣邯壹↓螟峨ｏ繧九・    //
    //   貂ｬ繧矩擇縺ｨ雋ｫ騾壽婿蜷代・**螢√・豕慕ｷ・*縺ｧ豎ｺ繧√ｋ縲らｵ瑚ｷｯ縺ｮ蜷代″縺ｧ豎ｺ繧√ｋ縺ｨ螢√ｒ譁懊ａ縺ｫ蛻・▲縺滄擇縺ｫ
    //   縺ｪ繧翫∬ｵｰ譟ｻ轤ｹ繧りｲｫ騾壹Ξ繧､繧ょ｣√・螟悶∈縺壹ｌ繧具ｼ亥ｮ滓ｸｬ縺ｧ 3m 邏壹・隱､縺｣縺溷､縺悟・縺滂ｼ峨・    //   荳ｭ蠢・・縲檎峩邱壹′髱｢縺ｨ莠､繧上ｋ轤ｹ縲阪ょ酔荳蟷ｳ髱｢縺ｮ螢√↑繧峨∝ｽ薙◆繧九う繝ｳ繧ｹ繧ｿ繝ｳ繧ｹ縺・    //   蟾ｦ蜿ｳ縺ｮ螢√・髢薙〒蜈･繧梧崛繧上▲縺ｦ繧ょｹｳ髱｢縺ｯ螟峨ｏ繧峨↑縺・・縺ｧ騾｣邯壹・    float apertureOpenArea(const Vec3& listener, const Vec3& source, float* outWidth) const {
        if (outWidth) *outWidth = 0.0f;
        const int rows = gridSamples_;
        if (rows <= 1) return 0.0f;

        Vec3 axis = source - listener;
        const float dist = length(axis);
        if (dist < 1e-4f) return 0.0f;
        axis = axis * (1.0f / dist);
        const SceneHit hit = raycastClosest(listener, axis, dist);
        if (!hit.hit) return -1.0f;              // 驕ｮ繧九ｂ縺ｮ縺檎┌縺・ｼ晏宛髯舌＠縺ｪ縺・
        Vec3 nrm = hit.normal;
        if (length(nrm) < 1e-4f) nrm = axis;
        nrm = normalized(nrm);
        Vec3 u = cross(nrm, Vec3(0, 1, 0));
        if (length(u) < 1e-3f) u = cross(nrm, Vec3(1, 0, 0));
        u = normalized(u);
        const Vec3 v = normalized(cross(nrm, u));

        const float R = gridRadius_;
        const float depth = gridDepth_;
        const Vec3 origin = hit.point;
        auto passes = [&](float x, float y) {
            const Vec3 p = origin + u * x + v * y;
            return !isOccluded(p - nrm * depth, p + nrm * depth);
        };

        // 陦後＃縺ｨ縺ｫ縲梧栢縺代ｉ繧後ｋ蛹ｺ髢薙・髟ｷ縺輔阪ｒ遨阪・縲らｫｯ縺ｯ莠悟・謗｢邏｢縺ｧ隧ｰ繧√ｋ縺ｮ縺ｧ縲・        // 謇峨′蜍輔￥縺ｨ髟ｷ縺輔′騾｣邯壹↓螟峨ｏ繧具ｼ医そ繝ｫ縺・縺､縺壹▽蜈･繧梧崛繧上ｋ縺ｮ縺ｧ縺ｯ縺ｪ縺・ｼ峨・        constexpr int kCoarse = 16;      // 1陦後≠縺溘ｊ縺ｮ邊励＞蛻ｻ縺ｿ
        constexpr int kBisect = 6;
        const float step = 2.0f * R / kCoarse;
        const float rowH = 2.0f * R / static_cast<float>(rows - 1);

        double area = 0.0;
        float openMinV = 1e30f, openMaxV = -1e30f;
        // 1陦後・繧薙・縲梧栢縺代ｉ繧後ｋ髟ｷ縺輔阪Ｔteps 繧貞｢励ｄ縺吶→邏ｰ縺・囮髢薙∪縺ｧ諡ｾ縺医ｋ縲・        auto scanRow = [&](float y, int steps) {
            const float st = 2.0f * R / static_cast<float>(steps);
            double rowLen = 0.0;
            bool prevOpen = passes(-R, y);
            float runStart = -R;
            for (int i = 1; i <= steps; ++i) {
                const float x = -R + st * i;
                const bool nowOpen = passes(x, y);
                if (nowOpen == prevOpen) continue;
                // 蠅・阜繧剃ｺ悟・縺ｧ隧ｰ繧√ｋ縲ゅ％縺薙′騾｣邯壽ｧ縺ｮ隕・笏笏 遶ｯ縺碁｣邯壹↓蜍輔￥縺ｮ縺ｧ
                // 謇峨′蜍輔￥縺ｨ髟ｷ縺輔ｂ騾｣邯壹↓螟峨ｏ繧具ｼ医そ繝ｫ縺・縺､縺壹▽蜈･繧梧崛繧上ｋ縺ｮ縺ｧ縺ｯ縺ｪ縺・ｼ峨・                float lo = x - st, hi = x;
                for (int it = 0; it < kBisect; ++it) {
                    const float mid = 0.5f * (lo + hi);
                    if (passes(mid, y) == prevOpen) lo = mid; else hi = mid;
                }
                const float edge = 0.5f * (lo + hi);
                if (prevOpen) rowLen += edge - runStart; else runStart = edge;
                prevOpen = nowOpen;
            }
            if (prevOpen) rowLen += R - runStart;
            return rowLen;
        };

        for (int j = 0; j < rows; ++j) {
            const float y = -R + rowH * j;
            double rowLen = scanRow(y, kCoarse);
            // 笘・ｩｺ謖ｯ繧翫＠縺溯｡後□縺醍ｴｰ縺九￥蠑輔″逶ｴ縺吶・            //   邊励＞蛻ｻ縺ｿ(18cm)繧医ｊ邏ｰ縺・囮髢薙・蜿悶ｊ縺薙⊂縺吶ょ叙繧翫％縺ｼ縺吶→縲碁幕蜿｣縺ｪ縺暦ｼ晏宛髯舌＠縺ｪ縺・・            //   縺ｫ關ｽ縺｡縺ｦ髻ｳ驥上′霍ｳ縺ｭ荳翫′繧九・縺ｧ縲∵演縺碁幕縺榊ｧ九ａ繧玖ｧ貞ｺｦ縺ｧ蟠悶↓縺ｪ繧・            //   ・亥ｮ滓ｸｬ: 20ﾂｰ縺ｧ髫｣謗･蟾ｮ 0.21・峨らｩｺ謖ｯ繧頑凾縺縺第鴛縺医・繧ｳ繧ｹ繝医・譎ｮ谿ｵ縺九°繧峨↑縺・・            if (rowLen <= 1e-6) rowLen = scanRow(y, kCoarse * 4);
            if (rowLen > 1e-4) { openMinV = std::min(openMinV, y); openMaxV = std::max(openMaxV, y); }
            area += rowLen * rowH;
        }
        // 笘・ｦ九▽縺九ｉ縺ｪ縺上※繧・0 繧定ｿ斐＠縺ｦ縺ｯ縺・￠縺ｪ縺・・*縺薙・貂ｬ螳壹・迢ｭ繧√ｋ縺縺代・蠖ｹ逶ｮ**縺ｧ縲・        //   邨瑚ｷｯ縺悟惠繧九°縺ｩ縺・°縺ｯ蛟呵｣懈爾邏｢縺梧里縺ｫ豎ｺ繧√※縺・ｋ縲よｨ呎悽縺励◆遽・峇縺ｫ髢句哨縺檎┌縺・・縺ｯ
        //   縲悟挨縺ｮ蝗槭ｊ譁ｹ繧偵＠縺ｦ縺・ｋ縲搾ｼ郁｡晉ｫ九・邵√ｒ蝗槭ｋ縺ｪ縺ｩ縲・幕蜿｣縺悟濠蠕・・螟悶↓縺ゅｋ・峨・諢丞袖縲・        //   0 繧定ｿ斐☆縺ｨ陦晉ｫ九・蝗樊釜繧呈ｮｺ縺呻ｼ亥ｮ滓ｸｬ縺ｧ2莉ｶ縺ｮ繝・せ繝医′關ｽ縺｡縺滂ｼ峨・        if (area <= 1e-6) return -1.0f;          // 蛻ｶ髯舌＠縺ｪ縺・
        // 螳溷柑蟷・= 髱｢遨・ﾃｷ 邵ｦ縺ｮ蠎・′繧翫るｫ倥＆H繝ｻ蟷・縺ｮ邵ｦ繧ｹ繝ｪ繝・ヨ縺ｪ繧・(wH)/H = w縲・        if (outWidth) {
            const float spanV = std::max(openMaxV - openMinV + rowH, 1e-3f);
            *outWidth = static_cast<float>(area) / spanV;
        }
        return static_cast<float>(area);
    }

    float apertureWidthByGrid(const Vec3& listener, const Vec3& source,
                              const Vec3* seed = nullptr) const {
        constexpr float kMaxWidth = 4.0f;      // 縺薙ｌ莉･荳翫・縲碁幕蜿｣縺ｨ縺励※蠕矩溘＠縺ｪ縺・・        const int n = gridSamples_;
        if (n <= 1) return kMaxWidth;

        Vec3 axis = source - listener;
        const float dist = length(axis);
        if (dist < 1e-4f) return kMaxWidth;
        axis = axis * (1.0f / dist);

        const SceneHit hit = raycastClosest(listener, axis, dist);
        if (!hit.hit) return kMaxWidth;        // 驕ｮ繧九ｂ縺ｮ縺檎┌縺・
        // 笘・擇縺ｨ雋ｫ騾壽婿蜷代・**螢√・豕慕ｷ・*縺ｧ豎ｺ繧√ｋ縲らｵ瑚ｷｯ縺ｮ蜷代″縺ｧ豎ｺ繧√※縺ｯ縺・￠縺ｪ縺・・        //   邨瑚ｷｯ縺ｯ螢√↓蟇ｾ縺励※譁懊ａ縺ｪ縺ｮ縺ｧ縲√◎縺ｮ蝙ら峩髱｢縺ｯ螢√ｒ譁懊ａ縺ｫ蛻・▲縺滄擇縺ｫ縺ｪ繧翫・        //   襍ｰ譟ｻ轤ｹ繧りｲｫ騾壹Ξ繧､繧ょ｣√・螟悶∈縺壹ｌ縺ｦ縺・￥・亥ｮ滓ｸｬ縺ｧ 3m 邏壹・蛟､縺悟・縺ｦ縺・◆・峨・        //   貂ｬ繧翫◆縺・・縺ｯ縲悟｣√↓髢九＞縺溽ｩｴ縲阪↑縺ｮ縺ｧ縲∝｣√・髱｢蜀・〒隕九※縲∝｣√・蜴壹∩繧定ｲｫ縺上・        Vec3 nrm = hit.normal;
        if (length(nrm) < 1e-4f) nrm = axis;
        nrm = normalized(nrm);
        Vec3 u = cross(nrm, Vec3(0, 1, 0));
        if (length(u) < 1e-3f) u = cross(nrm, Vec3(1, 0, 0));
        u = normalized(u);
        const Vec3 v = normalized(cross(nrm, u));

        const float R = gridRadius_;
        const float depth = gridDepth_;
        // 髱｢縺ｮ謇句燕縺九ｉ螂･縺ｾ縺ｧ縲√∪縺｣縺吶＄謚懊￠繧峨ｌ繧九°縲ょ字縺ｿ繧呈戟縺溘○繧九・縺瑚ｦ∫せ縺ｧ縲・        // 髱｢荳翫□縺題ｦ九ｋ縺ｨ螢√ｈ繧雁･･縺ｫ遶九▲縺ｦ縺・ｋ謇峨ｒ蜿悶ｊ縺薙⊂縺吶・        // 貂ｬ繧矩擇縺ｯ驕ｮ阡ｽ迚ｩ縺ｮ髱｢縲ょ次轤ｹ縺ｯ繝偵Φ繝医ｒ**縺昴・髱｢縺ｸ關ｽ縺ｨ縺励◆轤ｹ**縺ｫ鄂ｮ縺上・        //   逶ｴ邱壹′蠖薙◆縺｣縺溽せ繧貞次轤ｹ縺ｫ縺吶ｋ縺ｨ縲・幕蜿｣縺後◎縺薙°繧・1.7m 髮｢繧後※縺・※
        //   貂ｬ螳壼濠蠕・1.5m)縺ｮ螟悶∈蜃ｺ縺ｦ縺励∪縺・ｼ亥ｮ滓ｸｬ縺ｧ邏ｰ縺・囮髢薙ｒ蜈ｨ驛ｨ蜿悶ｊ縺薙⊂縺励※縺・◆・峨・        //   髱｢縺ｮ菴咲ｽｮ・郁ｻｸ譁ｹ蜷托ｼ峨・驕ｮ阡ｽ迚ｩ縺ｮ縺ｾ縺ｾ縺ｧ縲・擇蜀・・荳ｭ蠢・□縺鷹幕蜿｣縺ｸ蟇・○繧九・        Vec3 origin = hit.point;
        if (seed) origin = *seed - nrm * dot(*seed - hit.point, nrm);

        auto passes = [&](float x, float y) {
            const Vec3 p = origin + u * x + v * y;
            return !isOccluded(p - nrm * depth, p + nrm * depth);
        };

        // 竭 縲梧栢縺代ｉ繧後ｋ轤ｹ縲阪ｒ1縺､隕九▽縺代ｋ縲ゅ∪縺壹ヲ繝ｳ繝医・霑大ｍ繧堤ｴｰ縺九￥縲∫┌縺代ｌ縺ｰ蜈ｨ菴薙ｒ邊励￥縲・        float ox = 0.0f, oy = 0.0f;
        bool found = false;
        if (seed) {
            const float sx = 0.0f, sy = 0.0f;   // 蜴溽せ繧偵ヲ繝ｳ繝医↓鄂ｮ縺・※縺ゅｋ縺ｮ縺ｧ荳ｭ蠢・°繧・            // 繝偵Φ繝医ｒ騾壹ｋ**蜊∝ｭ・*繧堤ｴｰ縺九￥襍ｰ繧九る擇蜈ｨ菴薙ｒ邏ｰ縺九￥蠑ｵ繧九→鬮倥￥縺､縺上′縲・            // 髫咎俣縺ｯ蠢・★遞懃ｷ壹↓謗･縺励※縺・ｋ縺ｮ縺ｧ縲∫ｨ懃ｷ壹・蠖薙◆繧翫ｒ騾壹ｋ邱壻ｸ翫ｒ隕九ｌ縺ｰ謗帙°繧九・            //   ﾂｱ25cm 繧・1.5cm 蛻ｻ縺ｿ縲・0ﾂｰ縺ｮ謇峨・髫咎俣(7cm)繧ゅ・0ﾂｰ(1.8cm)繧よ鏡縺医ｋ縲・            constexpr int kFine = 33;
            constexpr float kFineR = 0.25f;
            for (int i = 0; i < kFine && !found; ++i) {
                const float t2 = -kFineR + 2.0f * kFineR * i / (kFine - 1);
                if (std::fabs(sx + t2) <= R && std::fabs(sy) <= R && passes(sx + t2, sy)) {
                    ox = sx + t2; oy = sy; found = true;
                }
            }
            for (int i = 0; i < kFine && !found; ++i) {
                const float t2 = -kFineR + 2.0f * kFineR * i / (kFine - 1);
                if (std::fabs(sx) <= R && std::fabs(sy + t2) <= R && passes(sx, sy + t2)) {
                    ox = sx; oy = sy + t2; found = true;
                }
            }
        }
        for (int j = 0; j < n && !found; ++j) {
            const float y = -R + 2.0f * R * j / static_cast<float>(n - 1);
            for (int i = 0; i < n && !found; ++i) {
                const float x = -R + 2.0f * R * i / static_cast<float>(n - 1);
                if (passes(x, y)) { ox = x; oy = y; found = true; }
            }
        }
        // 笘・ｦ九▽縺九ｉ縺ｪ縺代ｌ縺ｰ縲悟宛髯舌＠縺ｪ縺・阪・ 繧定ｿ斐＠縺ｦ縺ｯ縺・￠縺ｪ縺・・        //   邨瑚ｷｯ縺悟惠繧九°縺ｩ縺・°縺ｯ蛟呵｣懈爾邏｢縺梧里縺ｫ豎ｺ繧√※縺・ｋ縲ゅ％縺薙・**迢ｭ繧√ｋ縺縺・*縺ｮ蠖ｹ逶ｮ縺ｧ縲・        //   霑代￥縺ｫ髢句哨縺瑚ｦ九▽縺九ｉ縺ｪ縺・・縺ｯ縲悟挨縺ｮ蝗槭ｊ譁ｹ繧偵＠縺ｦ縺・ｋ縲搾ｼ郁｡晉ｫ九・邵√↑縺ｩ・峨・諢丞袖縲・        //   縺薙％縺ｧ 0 繧定ｿ斐☆縺ｨ陦晉ｫ九・蝗樊釜繧呈ｮｺ縺呻ｼ亥ｮ滓ｸｬ縺ｧ縺昴≧縺ｪ縺｣縺滂ｼ峨・        if (!found) return kMaxWidth;

        // 竭｡ 縺昴％縺九ｉ莠悟・謗｢邏｢縺ｧ蠅・阜繧定ｩｰ繧√ｋ縲よｼ蟄舌・蛻ｻ縺ｿ縺ｧ縺ｯ cm 蜊倅ｽ阪・髫咎俣繧定ｧ｣蜒上〒縺阪↑縺・        //    ・亥濠蠕・.5m 繧・蛻・牡縺吶ｋ縺ｨ繧ｻ繝ｫ縺・0cm縲・0ﾂｰ縺ｮ謇峨・髫咎俣縺ｯ7cm 縺励°縺ｪ縺・ｼ峨・        auto edgeDist = [&](float dx, float dy) {
            float lo = 0.0f, hi = 0.0f;
            const float step = 2.0f * R / static_cast<float>(n - 1);
            // 邊励￥螟悶∈騾ｲ繧薙〒縲∝｡槭′繧区園繧定ｦ九▽縺代ｋ縲・            for (hi = step; hi <= 2.0f * R; hi += step) {
                if (!passes(ox + dx * hi, oy + dy * hi)) break;
                lo = hi;
            }
            if (hi > 2.0f * R) return 2.0f * R;     // 遶ｯ縺ｾ縺ｧ髢九＞縺ｦ縺・ｋ
            for (int it = 0; it < 8; ++it) {        // 莠悟・縺ｧ蠅・阜縺ｸ蟇・○繧・                const float mid = 0.5f * (lo + hi);
                if (passes(ox + dx * mid, oy + dy * mid)) lo = mid; else hi = mid;
            }
            return lo;
        };
        const float spanU = edgeDist(1.0f, 0.0f) + edgeDist(-1.0f, 0.0f);
        const float spanV = edgeDist(0.0f, 1.0f) + edgeDist(0.0f, -1.0f);
        return std::min(std::min(spanU, spanV), kMaxWidth);
    }

    void setApertureGrid(int samples, float radius, float depth) {
        gridSamples_ = std::min(std::max(samples, 0), 15);
        gridRadius_ = radius;
        gridDepth_ = depth;
    }
    int apertureGridSamples() const { return gridSamples_; }

    // 髫咎俣縺ｮ蟷・竊・**蟶ｯ蝓溘＃縺ｨ**縺ｮ繧ｲ繧､繝ｳ縲ゅ碁幕縺上→髻ｳ濶ｲ縺碁幕縺上阪ｒ菴懊ｋ譛ｬ菴薙・    //
    //   螳滓ｸｬ・育樟螳溘・謇峨・髢矩哩繧貞庶骭ｲ繝ｻ隗｣譫撰ｼ・
    //     髢銀・髢峨・蟾ｮ縺ｯ 125Hz +2.6dB / 500Hz +7.9dB / 1kHz +10.3dB / 4kHz +8.8dB
    //   菴主沺縺ｯ縺ｻ縺ｨ繧薙←螟峨ｏ繧峨★縲∽ｸｭ鬮伜沺縺縺代′螟ｧ縺阪￥蠅励∴繧九ら炊逕ｱ縺ｯ蠖ｹ蜑ｲ蛻・球縺ｧ隱ｬ譏弱〒縺阪ｋ:
    //     菴主沺縺ｯ髢峨§縺ｦ縺・※繧・*螢√ｒ謚懊￠縺ｦ縺上ｋ**・郁ｳｪ驥丞援・峨・縺ｧ縲・幕縺代※繧ょ｢励∴繧倶ｽ吝慍縺悟ｰ代↑縺・    //     荳ｭ鬮伜沺縺ｯ螢√〒豁｢縺ｾ縺｣縺ｦ縺・ｋ縺ｮ縺ｧ縲・幕縺・◆縺ｶ繧薙□縺台ｸｸ縺斐→蠅励∴繧・    //   縺､縺ｾ繧頑演繧帝幕縺代◆縺ｨ縺阪・縲後＄繧上▲縺ｨ蠎・′繧九阪・髻ｳ驥上〒縺ｯ縺ｪ縺・*髻ｳ濶ｲ縺碁幕縺上％縺ｨ**縺縺｣縺溘・    //
    //   繝｢繝・Ν縺ｯ縲碁囮髢薙・繝上う繝代せ縲阪る嚆縺ｮ蜻ｨ豕｢謨ｰ縺ｯ蟷・〒豎ｺ縺ｾ繧・ fc 竕・c / (2w)縲・    //     w=1.8cm(10ﾂｰ) 竊・fc 9.5kHz 窶ｦ 鬮伜沺縺縺代′蟆代＠貍上ｌ繧・    //     w=1.2m(蜈ｨ髢・ 竊・fc 143Hz  窶ｦ 縺ｻ縺ｼ邏騾壹＠
    //   蟷・′蠎・′繧九↓縺､繧碁嚆縺御ｸ九′繧翫∽ｽ弱＞蟶ｯ蝓溘′鬆・↓蜈･縺｣縺ｦ縺上ｋ縲・    //
    //   笘・％繧後・莉･蜑肴昏縺ｦ縺溘悟屓謚倥・ LPF縲阪→縺ｯ**騾・髄縺・*縺ｪ縺ｮ縺ｧ豺ｷ蜷後＠縺ｪ縺・％縺ｨ縲・    //     謐ｨ縺ｦ縺溘・縺ｯ蜑榊ｷ昴・蠑上・蟶ｯ蝓滉ｾ晏ｭ假ｼ昴悟屓繧願ｾｼ繧縺ｨ鬮伜沺縺梧ｸ帙ｋ縲阪〒縲∝ｮ壻ｽ阪・謇九′縺九ｊ繧・    //     閾ｪ繧牙炎繧玖・蟾ｱ遏帷崟縺縺｣縺溘ゅ％縺｡繧峨・縲碁幕縺上→鬮伜沺縺悟・繧九阪〒縲・幕縺榊・蜷医ｒ髻ｳ濶ｲ縺ｧ
    //     莨昴∴繧・笏笏 繧ｳ繝ｳ繧ｻ繝励ヨ縺ｮ荳ｭ蠢・◎縺ｮ繧ゅ・縲・    void slitWidthBandGain(float width, float outGain[kNumBands]) const {
        if (slitWidthRef_ <= 1e-4f || width <= 0.0f) {
            for (int b = 0; b < kNumBands; ++b) outGain[b] = (width > 0.0f) ? 1.0f : 0.0f;
            return;
        }
        constexpr float kBandHz[kNumBands] = {125.0f, 250.0f, 500.0f, 1000.0f, 2000.0f, 4000.0f};
        const float fc = 343.0f / (2.0f * std::max(width, 1e-4f));
        for (int b = 0; b < kNumBands; ++b) {
            // 髫・ｈ繧贋ｸ翫・邏騾壹＠縲∽ｸ九・ 6dB/oct・・litBandSlope_ 縺ｧ隱ｿ謨ｴ蜿ｯ・峨〒關ｽ縺｡繧九・            float g = kBandHz[b] / fc;
            if (g > 1.0f) g = 1.0f;
            outGain[b] = (slitBandSlope_ == 1.0f) ? g : std::pow(g, slitBandSlope_);
        }
    }

    /// 髢句哨繧偵碁城℃縺ｮ荳驛ｨ縲阪→縺励※謇ｱ縺・ｼ亥粋謌宣城℃邇・ﾏЮeff = ﾏЮ螢・1竏断) + f 縺ｮ f 縺ｮ鬆・ｼ峨・    ///   ON: 髢句哨繧ｿ繝・・縺ｯ蜑榊ｷ昴・ ﾎｴ 貂幄｡ｰ繧呈鴛繧上★縲’ 繧偵◎縺ｮ縺ｾ縺ｾ謖√▽縲ょ｣√・騾城℃縺ｯ (1竏断) 縺ｧ貂帙ｋ縲・    ///   OFF: 蠕捺擂・亥燕蟾・ﾃ・髢句哨邇・ｼ峨・    void setApertureIsTransmission(int on) { apertureIsTransmission_ = (on != 0); }
    // 遞懃ｷ壽爾邏｢縺ｧ隕九▽縺代◆髢句哨繧偵碁幕蜿｣邇・怙螟ｧ縺ｮ繝昴・繧ｿ繝ｫ縲阪→縺励※隧穂ｾ｡縺吶ｋ・域里螳・OFF・峨・    void setApertureAsPortal(int on) { apertureAsPortal_ = (on != 0); }
    int  apertureAsPortal() const { return apertureAsPortal_ ? 1 : 0; }
    int apertureIsTransmission() const { return apertureIsTransmission_ ? 1 : 0; }

    /// 髢句哨邇・・**蟷・*繧帝幕縺乗欠謨ｰ縲ょｽ｢縺ｯ螟峨∴縺壹さ繝ｳ繝医Λ繧ｹ繝医□縺代ｒ荳翫￡繧九・.0=邏騾壹＠縲・    ///   迚ｩ逅・°繧牙・繧九・縺ｯ蠖｢縺ｧ縲・㍼縺ｯ貍泌・縺ｧ豎ｺ繧√ｋ・亥ｮ滓ｸｬ縺ｯ蜿ら・縺ｧ縺ゅ▲縺ｦ逶ｮ讓吶〒縺ｯ縺ｪ縺・ｼ峨・    void setApertureContrast(float p) { apertureContrast_ = (p > 0.05f) ? p : 0.05f; }
    float apertureContrast() const { return apertureContrast_; }

    /// BTM・域怏髯先･斐・遞懃ｷ夂ｩ榊・・峨〒蝗樊釜縺ｮ蟶ｯ蝓溘ご繧､繝ｳ繧貞・縺吶よ里螳・OFF・亥燕蟾晢ｼ矩幕蜿｣遨榊・・峨・    ///   ON 縺ｫ縺吶ｋ縺ｨ蜑榊ｷ昴・ ﾎｴ 貂幄｡ｰ繧る幕蜿｣邇・ｂ菴ｿ繧上★縲。TM 縺ｮ蛟､縺後◎縺ｮ縺ｾ縺ｾ蟶ｯ蝓溘ご繧､繝ｳ縺ｫ縺ｪ繧九・    void setUseBtm(int on) { useBtm_ = (on != 0); }
    int useBtm() const { return useBtm_ ? 1 : 0; }
    /// 讌斐・髢九″隗・rad)縲よ里螳・1.5ﾏ・晉ｮｱ縺ｮ蜃ｸ遞懃ｷ壹り埋縺・｡晉ｫ九↑繧・2ﾏ縲・    void setBtmWedgeAngle(float rad) { btmWedgeAngle_ = (rad > 0.1f) ? rad : 4.712389f; }

    /// 髢句哨縺ｮ遨榊・縺ｧ ﾎｴ 繧偵←繧後□縺大柑縺九○繧九°・・=縺昴・縺ｾ縺ｾ / 0=蜉ｹ縺九○縺ｪ縺・ｼ峨ＥeltaWeight_ 蜿ら・縲・    void setApertureDeltaWeight(float w) { deltaWeight_ = std::max(w, 0.0f); }
    float apertureDeltaWeight() const { return deltaWeight_; }

    void setSlitWidth(float ref, float power) { slitWidthRef_ = ref; slitWidthPower_ = power; }
    void setSlitBandSlope(float s) { slitBandSlope_ = s; }
    float slitBandSlope() const { return slitBandSlope_; }
    float slitWidthRef() const { return slitWidthRef_; }
    float slitWidthPower() const { return slitWidthPower_; }

    // 轤ｹ縺・self 莉･螟悶・邂ｱ縺ｮ蜀・Κ縺ｫ縺ゅｋ縺九り・繧峨∪縺帙◆遞懃ｷ壼呵｣懊′螢√↓蝓九∪縺｣縺ｦ縺・↑縺・°縺ｮ蛻､螳壹・    //   繝｡繝・す繝･縺ｯ隕九↑縺・ゅΓ繝・す繝･縺ｮ obb 縺ｯ蠅・阜邂ｱ縺ｪ縺ｮ縺ｧ縲∽ｸｭ遨ｺ縺ｮ驛ｨ螻九〒繧ょ・驛ｨ繧貞沂繧√※縺励∪縺・・    //   tol 縺ｯ遞懃ｷ壹ｒ閹ｨ繧峨∪縺帙◆驥上り・繧峨∪縺帙◆縺ｶ繧薙□縺醍ｮｱ繧ょ､ｪ繧峨○縺ｦ蛻､螳壹＠縺ｪ縺・→縲・    //   謇峨・閾ｪ逕ｱ遶ｯ縺ｨ謌ｸ蜿｣縺ｮ譫縺後・縺｣縺溘ｊ謗･縺励※縺・ｋ迥ｶ諷九〒縲∝呵｣懃せ縺梧演縺ｮ**謨ｰ mm 螟・*縺ｫ
    //   關ｽ縺｡縺ｦ蛻､螳壹ｒ縺吶ｊ謚懊￠繧具ｼ亥ｮ滓ｸｬ: 髢画演縺ｧ 6mm 螟悶↓關ｽ縺｡縺ｦ蟷ｻ縺ｮ邨瑚ｷｯ縺梧ｮ九▲縺滂ｼ峨・    bool pointInsideOther(const Vec3& P, int selfInst, float tol) const {
        const int n = static_cast<int>(instances_.size());
        for (int i = 0; i < n; ++i) {
            if (i == selfInst) continue;
            const Instance& inst = instances_[i];
            if (!inst.active || inst.geomId >= 0) continue;
            const Vec3 l = obbToLocalPoint(P, inst.obb);
            if (std::fabs(l.x) <= inst.obb.halfExtents.x + tol &&
                std::fabs(l.y) <= inst.obb.halfExtents.y + tol &&
                std::fabs(l.z) <= inst.obb.halfExtents.z + tol) return true;
        }
        return false;
    }

    // 縲宣幕蜿｣縺ｮ髢九″蜈ｷ蜷医鷹幕蜿｣縺ｾ繧上ｊ縺ｮ譁ｭ髱｢縺ｮ縺・■縲・浹縺悟ｮ滄圀縺ｫ騾壹ｌ繧句牡蜷・0..1)繧呈焚縺医ｋ縲・    //
    //   笘・％繧後′蝗樊釜縺ｮ**髻ｳ驥・*繧呈ｱｺ繧√ｋ驥上らｨ懃ｷ壽爾邏｢縺ｯ**螳壻ｽ・*縺縺代ｒ諡・ｽ薙☆繧九・    //
    //   蜑榊ｷ昴・蠑上・ ﾎｴ・郁ｿょ屓縺ｮ菴吝臆髟ｷ・峨□縺代・髢｢謨ｰ縺ｧ縲・幕蜿｣縺後←繧後□縺鷹幕縺・※縺・ｋ縺九ｒ隕九※縺・↑縺・・    //   謇峨′蝗槭▲縺ｦ繧ょ屓謚倡ｵ瑚ｷｯ縺悟屓繧区虻蜿｣縺ｮ譫縺ｯ蜍輔°縺ｪ縺・・縺ｧ ﾎｴ 縺悟､峨ｏ繧峨★縲・    //   縲梧演縺後←縺ｮ縺上ｉ縺・幕縺・◆縺九阪′髻ｳ縺ｫ蜃ｺ縺ｪ縺九▲縺滂ｼ亥ｮ滓ｸｬ: 0ﾂｰ竊・0ﾂｰ 縺ｧ邱城㍼ 1.000 縺ｮ縺ｾ縺ｾ・峨・    //
    //   莉･蜑阪・縲碁幕蜿｣轤ｹ縺ｾ繧上ｊ縺ｮ閾ｪ逕ｱ縺ｪ蟾ｮ縺玲ｸ｡縺・m)縲阪ｒ貂ｬ縺｣縺ｦ縺・◆縺後√％繧後・陦後″豁｢縺ｾ繧翫□縺｣縺溘・    //   髢句哨轤ｹ縺ｯ謌ｸ蜿｣縺ｮ譫縺ｮ荳翫↓縺ゅｊ縲∵棧縺ｯ謇峨′蝗槭▲縺ｦ繧ょ虚縺九↑縺・ゅ◎縺薙・閾ｪ逕ｱ遨ｺ髢薙・
    //   縲梧虻蜿｣縺ｮ蟷・阪°縲碁Κ螻九・蟾ｮ縺玲ｸ｡縺励阪〒縺ゅ▲縺ｦ縲∵演縺ｮ髫咎俣縺ｧ縺ｯ縺ｪ縺・    //   ・亥ｮ滓ｸｬ: 髢九″隗偵→辟｡髢｢菫ゅ↓ 2.2縲・.0m 縺ｨ縺ｰ繧峨▽縺上□縺代□縺｣縺滂ｼ峨・    //
    //   蜍輔￥縺ｮ縺ｯ譚ｿ縺ｮ譁ｹ縺ｪ縺ｮ縺ｧ縲・*譁ｭ髱｢縺ｮ縺ｩ繧後□縺代′蝪槭′縺｣縺ｦ縺・↑縺・°**繧堤峩謗･謨ｰ縺医ｋ縲・    //
    //   譁ｭ髱｢縺ｯ**驕ｮ阡ｽ迚ｩ繧呈栢縺代◆蜈・*縺ｫ鄂ｮ縺上る・阡ｽ迚ｩ縺ｮ謇句燕縺ｫ鄂ｮ縺上→縲∵攸繧医ｊ蜑阪〒蛻､螳壹＠縺ｦ
    //     縺励∪縺・演縺ｮ髢矩哩縺御ｸ蛻・・縺ｪ縺・ｼ亥ｮ滓ｸｬ: 髢画演縺ｧ繧・0.583 縺ｨ蜃ｺ縺滂ｼ峨・    //
    //   荳ｭ蠢・→蜊雁ｾ・・縲悟｡槭′繧後◆逶ｴ邱壹阪→縲碁幕蜿｣轤ｹ縲阪・荳｡譁ｹ繧貞性繧繧医≧縺ｫ蜿悶ｋ縲・    //     髢句哨轤ｹ縺縺代ｒ荳ｭ蠢・↓縺吶ｋ縺ｨ蜀・乢縺ｮ蜊雁・縺悟｣√・荳ｭ縺ｫ蜈･繧具ｼ亥ｮ滓ｸｬ: 0/12 縺・1/12・峨・    //     逶ｴ邱壹・蠖薙◆繧頑園縺縺代ｒ荳ｭ蠢・↓縺吶ｋ縺ｨ縲∬｡晉ｫ九・繧医≧縺ｫ**邵√′驕縺・*髫懷ｮｳ迚ｩ縺ｧ
    //     蝗槭ｊ霎ｼ繧蜈医′蜀・乢縺ｮ螟悶∈蜃ｺ縺ｦ縺励∪縺・∵ｭ｣蠖薙↑蝗樊釜縺ｾ縺ｧ 0 縺ｫ縺ｪ繧九・    //     荳｡遶ｯ繧貞性繧蜀・乢縺ｪ繧峨∵虻蜿｣縺ｧ繧り｡晉ｫ九〒繧ゅ碁浹縺・squeeze 縺吶ｋ鬆伜沺縲阪ｒ隕・∴繧九・    //
    //   蛻､螳壹・縲・*繝ｪ繧ｹ繝翫・縺九ｉ縺昴・轤ｹ縺瑚ｦ九∴繧九°**縲阪□縺代る浹貅舌∪縺ｧ隕矩壹○繧九°縺ｯ蝠上ｏ縺ｪ縺・・    //     蝠上≧縺ｦ縺励∪縺・→縲・ 谿ｵ逶ｮ縺ｮ謌ｸ蜿｣縺悟挨縺ｫ蝨ｨ繧矩・鄂ｮ・亥鴻魑･・峨〒蟶ｸ縺ｫ 0 縺ｫ縺ｪ繧翫・    //     豁｣蠖薙↑ 2 谺｡蝗樊釜縺悟・驛ｨ鮟吶ｋ縲ゅ％縺薙〒貂ｬ繧翫◆縺・・縺ｯ縲梧焔蜑阪・髢句哨縺後←繧後□縺鷹幕縺・※縺・ｋ縺九阪・    //
    //   蜷・し繝ｳ繝励Ν縺ｯ迢ｬ遶九↑縺ｮ縺ｧ縲∵演縺悟屓繧後・縲瑚ｦ九∴繧九し繝ｳ繝励Ν謨ｰ縲阪′貊代ｉ縺九↓螟峨ｏ繧・    //   ・晞｣邯壽ｧ縺梧ｧ矩逧・↓菫晁ｨｼ縺輔ｌ繧九・    float apertureOpenness(const Vec3& listener, const Vec3& source,
                           const Vec3& aperture) const {
        const int n = apertureOpenSamples_;
        if (n <= 0) return 1.0f;                       // 0 縺ｧ辟｡蜉ｹ蛹厄ｼ亥ｸｸ縺ｫ邏騾壹＠・・        Vec3 axis = source - listener;
        const float dist = length(axis);
        if (dist < 1e-4f) return 1.0f;
        axis = axis * (1.0f / dist);

        const SceneHit hit = raycastClosest(listener, axis, dist);
        if (!hit.hit) return 1.0f;                     // 驕ｮ繧九ｂ縺ｮ縺檎┌縺・
        Vec3 u = cross(axis, Vec3(0, 1, 0));
        if (length(u) < 1e-3f) u = cross(axis, Vec3(1, 0, 0));
        u = normalized(u);
        const Vec3 v = normalized(cross(axis, u));

        // 譁ｭ髱｢荳翫〒縺ｮ縲悟｡槭′繧後◆轤ｹ縲阪→縲碁幕蜿｣轤ｹ縲阪・菴咲ｽｮ・郁ｻｸ譁ｹ蜷代・謌仙・縺ｯ謐ｨ縺ｦ繧具ｼ峨・        const Vec3 dh = hit.point - listener;
        const Vec3 da = aperture - listener;
        const float hu = dot(dh, u), hv = dot(dh, v);
        const float au = dot(da, u), av = dot(da, v);
        const float cu = 0.5f * (hu + au), cv = 0.5f * (hv + av);
        // 荳｡遶ｯ繧貞性繧蜊雁ｾ・る幕蜿｣縺碁□縺・⊇縺ｩ蠎・￥隕九ｋ・郁｡晉ｫ九・繧医≧縺ｫ邵√′驕縺・ｴ蜷茨ｼ峨・        const float half = 0.5f * std::sqrt((au - hu) * (au - hu) + (av - hv) * (av - hv));
        const float radius = std::max(apertureOpenRadius_, half + apertureOpenRadius_ * 0.5f);

        // 驕ｮ阡ｽ迚ｩ繧呈栢縺代◆蜈医∈騾√ｋ縲よ焔蜑阪□縺ｨ譚ｿ縺ｮ蜑阪〒蛻､螳壹＠縺ｦ縺励∪縺・・        const Vec3 base = hit.point + axis * kOpenClearance
                        + u * (cu - dot(dh, u)) + v * (cv - dot(dh, v));

        // 繝輔ぅ繝懊リ繝・メ蜀・乢・域ｱｺ螳夂噪・峨ゆｹｱ謨ｰ縺縺ｨ繝輔Ξ繝ｼ繝縺斐→縺ｫ繝代ち繝ｼ繝ｳ縺悟､峨ｏ縺｣縺ｦ
        // 蜑ｲ蜷医′縺｡繧峨▽縺・笏笏 computeSoftOcclusion 縺ｨ蜷後§逅・罰縺ｧ蝗ｺ螳夐・鄂ｮ縺ｫ縺吶ｋ縲・        constexpr float kGolden = 2.39996323f;
        int open = 0;
        for (int i = 0; i < n; ++i) {
            const float rr = radius * std::sqrt((i + 0.5f) / n);
            const float aa = kGolden * i;
            const Vec3 p = base + u * (rr * std::cos(aa)) + v * (rr * std::sin(aa));
            if (!isOccluded(listener, p)) ++open;
        }
        return static_cast<float>(open) / static_cast<float>(n);
    }
    // 譁ｭ髱｢繧帝・阡ｽ迚ｩ縺ｮ縺ｩ繧後□縺大・縺ｫ鄂ｮ縺上°(m)縲よ攸縺ｮ蜴壹∩繧堤｢ｺ螳溘↓雜翫∴繧玖ｷ晞屬縲・    static constexpr float kOpenClearance = 0.5f;

    // 髢句哨邇・竊・蠎・ｸｯ蝓溘ご繧､繝ｳ(0..1)縲・    //   apertureOpenRef_ 縺ｯ縲後％繧後□縺鷹幕縺・※縺・ｌ縺ｰ邏騾壹＠縺ｨ縺ｿ縺ｪ縺吶榊牡蜷医・    //     蜀・乢縺ｯ謌ｸ蜿｣縺ｮ蜻ｨ繧翫・螢√ｂ蜷ｫ繧縺ｮ縺ｧ縲∝・髢九・謌ｸ蜿｣縺ｧ繧ょ牡蜷医・ 1 縺ｫ縺ｪ繧峨↑縺・・    //     謌ｸ蜿｣ 1.2m 繧貞濠蠕・1m 縺ｮ蜀・乢縺ｧ隕九ｋ縺ｨ 3 蜑ｲ蜑榊ｾ後よ里螳壹・縺昴・隕句ｽ薙・    //   apertureOpenPower_ 縺ｯ繧ｫ繝ｼ繝悶・.0 = 蜑ｲ蜷医↓豈比ｾ具ｼ磯幕縺榊ｧ九ａ縺九ｉ邏逶ｴ縺ｫ蠅励∴繧具ｼ峨・    //   繧ｹ繧ｫ繝ｩ縺ｧ霑斐☆縺ｮ縺ｯ縲∝屓謚倥′驕九・諠・ｱ繧貞ｮ壻ｽ阪↓邨槭ｋ譁ｹ驥昴・縺溘ａ・・PF 縺ｯ騾城℃縺梧球蠖難ｼ峨・    float apertureOpenGain(float frac) const {
        if (apertureOpenRef_ <= 1e-4f) return 1.0f;    // 0 縺ｧ辟｡蜉ｹ蛹・        const float r = frac / apertureOpenRef_;
        if (r >= 1.0f) return 1.0f;
        if (r <= 0.0f) return 0.0f;
        if (apertureOpenPower_ == 1.0f) return r;
        if (apertureOpenPower_ <= 0.0f) return 1.0f;
        return std::pow(r, apertureOpenPower_);
    }

    void setApertureOpenRef(float f) { apertureOpenRef_ = f; }
    float apertureOpenRef() const { return apertureOpenRef_; }
    void setApertureOpenPower(float p) { apertureOpenPower_ = p; }
    float apertureOpenPower() const { return apertureOpenPower_; }
    void setApertureOpenRadius(float m) { apertureOpenRadius_ = m; }
    float apertureOpenRadius() const { return apertureOpenRadius_; }
    void setApertureOpenSamples(int n) { apertureOpenSamples_ = n; }
    int apertureOpenSamples() const { return apertureOpenSamples_; }

    // ================================================================ 蝗樊釜・医く繝ｫ繝偵・繝・ヵ・・    // 髢句哨縺ｮ縲悟､ｧ縺阪＆縲阪ｒ貂ｬ繧翫√ヵ繝ｬ繝阪Ν繝ｻ繧ｭ繝ｫ繝偵・繝・ヵ縺ｮ隗｣譫占ｧ｣縺ｫ貂｡縺吶・    //
    //   蜑榊ｷ昴・蠑上・ ﾎｴ・郁ｿょ屓縺ｮ菴吝臆髟ｷ・峨□縺代・髢｢謨ｰ縺ｪ縺ｮ縺ｧ縲・*髢句哨縺ｮ蟷・↓蜿榊ｿ懊〒縺阪↑縺・*縲・    //   謇峨′蝗槭▲縺ｦ繧ょ屓謚倡ｵ瑚ｷｯ縺悟屓繧区虻蜿｣縺ｮ譫縺ｯ蜍輔°縺ｪ縺・◆繧・ﾎｴ 縺悟､峨ｏ繧峨★縲・    //   縲梧演縺後←縺ｮ縺上ｉ縺・幕縺・◆縺九阪′ piecewise constant 縺ｫ縺ｪ縺｣縺ｦ縺・◆・亥ｮ滓ｸｬ: 0縲・ﾂｰ縺ｧ螳悟・縺ｫ蟷ｳ蝮ｦ縲・    //   6.5ﾂｰ縺ｧ 0.234 霍ｳ縺ｶ・峨ゆｽ懷刀縺ｮ繧ｳ繝ｳ繧ｻ繝励ヨ縺ｮ荳ｭ譬ｸ縺後Δ繝・Ν縺ｮ讒矩縺ｧ陦ｨ迴ｾ縺ｧ縺阪※縺・↑縺・憾諷九□縺｣縺溘・    //
    //   縺薙％縺ｧ縺ｯ蜈・・豕募援縺ｫ謌ｻ繧九る浹貅舌→蜿鈴浹轤ｹ縺ｮ髢薙↓髱｢繧堤ｽｮ縺阪・*縺昴・髱｢縺ｮ縺・■蝪槭′繧後※縺・↑縺・ｯ・峇**繧・    //   螳滓ｸｬ縺励※縲∫洸蠖｢髢句哨縺ｮ隗｣譫占ｧ｣・・irchhoff.h・峨↓貂｡縺吶る幕蜿｣縺ｮ蟷・・謇峨・髢九″蜈ｷ蜷医・蜻ｨ豕｢謨ｰ萓晏ｭ倥′
    //   縺吶∋縺ｦ蜷後§蠑上°繧牙・繧九る明蛟､繧ょｴ蜷亥・縺代ｂ隕√ｉ縺ｪ縺・・    //
    //   謌ｻ繧雁､ = 髢句哨縺瑚ｦ九▽縺九ｌ縺ｰ true縲ＰutAperture 縺ｫ髢句哨荳ｭ蠢・ｼ亥ｮ壻ｽ阪↓菴ｿ縺・ｼ峨・    bool diffractionKirchhoff(const Vec3& listener, const Vec3& source,
                              float outGain[kNumBands], Vec3& outAperture,
                              float& outPathLength) const {
        const Vec3 axis = source - listener;
        const float dist = length(axis);
        if (dist < 1e-4f) return false;
        const Vec3 n = axis * (1.0f / dist);

        // 笏笏 髱｢縺ｮ菴咲ｽｮ 笏笏
        //   驕ｮ阡ｽ縺輔ｌ縺ｦ縺・ｋ縺ｪ繧画怙蛻昴↓蠖薙◆縺｣縺滓園・医◎縺薙′髻ｳ繧呈ｭ｢繧√※縺・ｋ髱｢・峨・        //   驕ｮ阡ｽ縺檎┌縺・↑繧我ｸｭ轤ｹ・育ｬｬ1繝輔Ξ繝阪Ν繧ｾ繝ｼ繝ｳ縺梧怙繧ょｺ・￥縲・・阡ｽ縺ｮ蠖ｱ髻ｿ縺梧怙繧ょ､ｧ縺阪＞菴咲ｽｮ・峨・        float d1 = dist * 0.5f;
        const SceneHit hit = raycastClosest(listener, n, dist);
        if (hit.hit) d1 = hit.t;
        d1 = std::max(std::min(d1, dist - 0.05f), 0.05f);
        const float d2 = dist - d1;
        const Vec3 planeOrigin = listener + n * d1;

        // 髱｢荳翫・逶ｴ莠､蝓ｺ蠎輔・        Vec3 u = cross(n, Vec3(0, 1, 0));
        if (length(u) < 1e-3f) u = cross(n, Vec3(1, 0, 0));
        u = normalized(u);
        const Vec3 v = normalized(cross(n, u));

        // 髱｢荳翫・轤ｹ縺後碁幕縺・※縺・ｋ縲阪°・晞浹貅舌→蜿鈴浹轤ｹ縺ｮ蜿梧婿縺九ｉ隕矩壹○繧九°縲・        auto openAt = [&](float su, float sv) {
            const Vec3 P = planeOrigin + u * su + v * sv;
            return !isOccluded(listener, P) && !isOccluded(P, source);
        };

        // 謗｢邏｢遽・峇縺ｯ隨ｬ1繝輔Ξ繝阪Ν繧ｾ繝ｼ繝ｳ縺ｮ謨ｰ蛟搾ｼ域怙菴主沺蝓ｺ貅厄ｼ峨ゅ◎縺薙°繧牙､悶・蟇・ｸ弱′謇薙■豸医＠蜷医≧縲・        const float D = (d1 * d2) / std::max(d1 + d2, 1e-6f);
        const float lambdaLow = kirchhoff::kSpeed / kirchhoff::kBandFreq[0];
        const float searchR = 3.0f * std::sqrt(lambdaLow * D);

        // 笏笏 髢句哨縺ｮ遞ｮ繧帝寔繧√ｋ 笏笏
        //   笘・ｭ蛾俣髫斐↓謗｢縺吶→蟆上＆縺ｪ髫咎俣繧定ｦ矩・☆縲よ演縺・6ﾂｰ 髢九＞縺溘→縺阪・髫咎俣縺ｯ 6mm 縺励°縺ｪ縺上・        //     繝輔Ξ繝阪Ν繧ｾ繝ｼ繝ｳ謨ｰ繝｡繝ｼ繝医Ν繧・6mm 蛻ｻ縺ｿ縺ｧ謗｢縺吶・縺ｯ髱樒樟螳溽噪・・荳・せ隕乗ｨ｡・峨・        //     縺昴％縺ｧ**遞懃ｷ壽爾邏｢繧堤ｨｮ縺ｫ菴ｿ縺・* 笏笏 遞懃ｷ壹・髢句哨縺ｮ邵√◎縺ｮ繧ゅ・縺ｪ縺ｮ縺ｧ縲√◎縺ｮ霑大ｍ繧定ｦ九ｌ縺ｰ
        //     縺ｩ繧薙↑縺ｫ邏ｰ縺・囮髢薙〒繧よ拷縺医ｉ繧後ｋ縲ゅ檎ｨ懃ｷ壹′縺ｩ縺薙°繧呈蕗縺医∫ｩ榊・縺後←繧後□縺代°繧呈ｱｺ繧√ｋ縲阪・        constexpr int kMaxSeed = 24;
        float seedU[kMaxSeed], seedV[kMaxSeed];
        int nseed = 0;
        auto pushSeed = [&](float a, float b) {
            if (nseed < kMaxSeed) { seedU[nseed] = a; seedV[nseed] = b; ++nseed; }
        };
        pushSeed(0.0f, 0.0f);                       // 霆ｸ荳奇ｼ郁ｦ矩壹○縺ｦ縺・ｋ縺ｪ繧峨％縺薙′髢句哨・・        forEachDiffractionCandidate(listener, source,
            [&](const Vec3& P, float, const Vec3&, const Vec3&, const Vec3&, const Vec3&, float, int) {
                const Vec3 rel = P - planeOrigin;
                pushSeed(dot(rel, u), dot(rel, v));  // 遞懃ｷ壻ｸ翫・轤ｹ繧帝擇縺ｸ蟆・ｽｱ
            }, 4.0f);

        // 霑代☆縺弱ｋ遞ｮ縺ｯ縺ｾ縺ｨ繧√ｋ・亥酔縺倬幕蜿｣繧剃ｽ募ｺｦ繧よｸｬ繧峨↑縺・ｼ峨・        for (int i = 0; i < nseed; ++i)
            for (int j = i + 1; j < nseed; ) {
                const float du2 = seedU[i] - seedU[j], dv2 = seedV[i] - seedV[j];
                if (du2 * du2 + dv2 * dv2 < 0.04f) {   // 20cm 莉･蜀・・蜷後§髢句哨縺ｨ縺ｿ縺ｪ縺・                    seedU[j] = seedU[nseed - 1]; seedV[j] = seedV[nseed - 1]; --nseed;
                } else ++j;
            }

        // 遞懃ｷ壻ｸ翫・轤ｹ縺ｯ蠅・阜縺昴・繧ゅ・縺ｪ縺ｮ縺ｧ縲∝ｰ代＠縺壹ｉ縺励※髢九＞縺ｦ縺・ｋ蛛ｴ繧呈爾縺吶・        //   笘・*譛蛻昴↓髢九＞縺溽ｨｮ縺ｧ豎ｺ繧√※縺ｯ縺・￠縺ｪ縺・*縲る・ｺ丈ｾ晏ｭ倥↓縺ｪ繧翫∵演縺ｮ髫咎俣縺ｧ縺ｯ縺ｪ縺・        //     螢√・螟門・繧・ｸ翫ｒ諡ｾ縺｣縺ｦ縺励∪縺・ｼ亥ｮ滄圀縺ｫ縺昴≧縺ｪ縺｣縺滂ｼ峨ょ・驛ｨ縺ｮ遞ｮ縺ｧ髢句哨繧呈ｸｬ繧翫・        //     縺・■縺ｰ繧薙ｈ縺城壹ｋ髢句哨繧呈治繧九・        auto seedOpen = [&](float bu, float bv, float& ou, float& ov) {
            const float nudge[3] = {0.0f, 0.02f, 0.10f};
            const float dirU[5] = {0.0f, 1.0f, -1.0f, 0.0f, 0.0f};
            const float dirV[5] = {0.0f, 0.0f, 0.0f, 1.0f, -1.0f};
            for (int e = 0; e < 3; ++e)
                for (int d = 0; d < 5; ++d) {
                    const float cu = bu + dirU[d] * nudge[e];
                    const float cv = bv + dirV[d] * nudge[e];
                    if (openAt(cu, cv)) { ou = cu; ov = cv; return true; }
                }
            return false;
        };

        // 笏笏 髢句哨縺ｮ蠎・′繧翫ｒ貂ｬ繧・笏笏
        //   遞ｮ縺九ｉ4譁ｹ蜷代∈騾ｲ縺ｿ縲∝｡槭′繧後ｋ菴咲ｽｮ繧剃ｺ悟・縺ｧ隧ｰ繧√ｋ縲る幕蜿｣縺ｮ邵√′騾｣邯壹↓蜍輔￥縺ｮ縺ｧ縲・        //   謇峨′蝗槭ｌ縺ｰ遽・峇繧る｣邯壹↓蜍輔￥ 笏笏 縺薙％縺後後←縺ｮ縺上ｉ縺・幕縺・◆縺九阪・螳滉ｽ薙・        //   笘・綾繧薙〒騾ｲ縺ｿ縲・*譛蛻昴↓蝪槭′繧後◆謇**縺ｧ豁｢繧√ｋ縲・        //     縲檎ｫｯ縺碁幕縺・※縺・ｌ縺ｰ蜈ｨ髢九阪→譌ｩ譛溷愛螳壹＠縺ｦ縺ｯ縺・￠縺ｪ縺・笏笏 髢九＞縺滄伜沺縺ｯ髱槫・縺ｧ縲・        //     螢√・荳翫°繧我ｸ九∈騾ｲ繧縺ｨ螢√ｒ雋ｫ騾壹＠縺ｦ蜿榊ｯｾ蛛ｴ縺ｮ髢九＞縺滄伜沺縺ｫ蜃ｺ縺ｦ縺励∪縺・・        //     縺昴ｌ繧貞・髢九→隱､蛻､螳壹☆繧九→縲・哩縺倥◆謇峨〒繧ゅご繧､繝ｳ縺・1.0 縺ｫ縺ｪ繧具ｼ亥ｮ滄圀縺ｫ縺昴≧縺ｪ縺｣縺滂ｼ峨・        auto extent = [&](float su, float sv, float du, float dv) {
            constexpr int kStep = 16;
            float lastOpen = 0.0f;
            for (int i = 1; i <= kStep; ++i) {
                const float r = searchR * (i / float(kStep));
                if (!openAt(su + du * r, sv + dv * r)) {
                    // lastOpen(髢・ 縺ｨ r(髢・ 縺ｮ髢薙↓邵√′縺ゅｋ縲ゆｺ悟・縺ｧ隧ｰ繧√ｋ縲・                    float lo = lastOpen, hi = r;
                    for (int j = 0; j < 10; ++j) {
                        const float m = 0.5f * (lo + hi);
                        if (openAt(su + du * m, sv + dv * m)) lo = m; else hi = m;
                    }
                    return lo;
                }
                lastOpen = r;
            }
            return searchR;   // 謗｢邏｢遽・峇縺ｮ遶ｯ縺ｾ縺ｧ髢九＞縺ｦ縺・ｋ
        };

        // 遞ｮ縺斐→縺ｫ髢句哨繧呈ｸｬ繧翫√＞縺｡縺ｰ繧薙ｈ縺城壹ｋ繧ゅ・繧呈治繧九・        //   窶ｻ隍・焚髢句哨縺ｮ隍・ｴ蜷域・縺ｯ蛻晉沿縺ｧ縺ｯ陦後ｏ縺ｪ縺・ｼ域髪驟埼幕蜿｣縺ｮ霑台ｼｼ・峨・        bool any = false;
        float bestScore = -1.0f;
        for (int s = 0; s < nseed; ++s) {
            float su, sv;
            if (!seedOpen(seedU[s], seedV[s], su, sv)) continue;
            const float u2 = su + extent(su, sv, 1, 0), u1 = su - extent(su, sv, -1, 0);
            const float v2 = sv + extent(su, sv, 0, 1), v1 = sv - extent(su, sv, 0, -1);
            float g[kNumBands];
            kirchhoff::apertureGain(u1, u2, v1, v2, d1, d2, g);
            // 菴主沺蜉驥阪〒縲後←繧後□縺鷹壹ｋ縺九阪ｒ1繧ｹ繧ｫ繝ｩ縺ｫ・亥屓謚倥・菴主沺縺悟屓繧願ｾｼ繧・峨・            const float wgt[kNumBands] = {3.0f, 2.5f, 2.0f, 1.3f, 1.0f, 0.8f};
            float gs = 0.0f, ws = 0.0f;
            for (int b = 0; b < kNumBands; ++b) { gs += wgt[b] * g[b]; ws += wgt[b]; }
            const float score = gs / ws;
            if (score > bestScore) {
                bestScore = score;
                for (int b = 0; b < kNumBands; ++b) outGain[b] = g[b];
                const float cu = 0.5f * (u1 + u2), cv = 0.5f * (v1 + v2);
                outAperture = planeOrigin + u * cu + v * cv;
                outPathLength = length(outAperture - listener) + length(source - outAperture);
                any = true;
            }
        }
        return any;
    }

    // ================================================================ 蝗樊釜・井ｸ譛ｬ蛹厄ｼ・    // docs/DIFFRACTION_DESIGN.md ﾂｧ2縲・*蝗樊釜縺ｮ謗｢邏｢縺ｯ縺薙％ 1 邂・園縺縺・*縲・    //
    //   莉･蜑阪・縲後ご繧､繝ｳ逕ｨ縲阪→縲梧婿蜷醍畑縲阪′蛻･縲・↓遞懃ｷ壹ｒ豁ｩ縺阪∝挨縲・・驥阪∩繝ｻ蛻･縲・・蛟呵｣懈擅莉ｶ繧・    //   謖√▲縺ｦ縺・◆縲ゅ◎縺ｮ縺帙＞縺ｧ縲後ご繧､繝ｳ縺ｯ蝗樊釜繧定ｦ九※縺・ｋ縺ｮ縺ｫ髢句哨縺ｯ 0 譛ｬ縲阪→縺・≧荳肴紛蜷医′襍ｷ縺阪・    //   隕矩壹＠縺碁幕騾壹＠縺溽椪髢薙↓螳壻ｽ阪′豸医∴縺ｦ縺・◆・郁ｨｭ險・ﾂｧ7-1 縺ｮ螳滓ｸｬ・峨・    //
    //   驕ｮ阡ｽ譎・  : 髢句哨縺斐→縺ｮ邨瑚ｷｯ縲ょ屓繧願ｾｼ繧薙〒螻翫￥謌仙・繧偵・幕蜿｣縺ｮ譁ｹ蜷代°繧蛾ｳｴ繧峨☆縲・    //   隕矩壹＠譎・: 1 譛ｬ縺縺題ｿ斐☆縲Ｂperture = 髻ｳ貅蝉ｽ咲ｽｮ縲“ain = 髢句哨縺ｫ繧医ｋ繝輔Ξ繝阪Ν陬懈ｭ｣縲・    //              逶ｴ謗･髻ｳ縺ｯ髻ｳ貅先婿蜷代°繧画擂繧九・縺ｧ縲∵婿蜷代・髻ｳ貅舌◎縺ｮ繧ゅ・縲り｣懈ｭ｣縺縺代′諢丞袖繧呈戟縺､縲・    struct DiffractionPath {
        Vec3  aperture{0, 0, 0};    // 髻ｳ縺梧栢縺代※縺上ｋ轤ｹ・亥ｮ壻ｽ阪↓菴ｿ縺・ｼ・        float pathLength = 0.0f;    // listener 竊・aperture 竊・source 縺ｮ螳滄聞・磯≦蟒ｶ繝ｻ霍晞屬貂幄｡ｰ・・        // 隨ｦ蜿ｷ莉倥″縺ｮ霑ょ屓菴吝臆髟ｷ・亥ｽｱ縺ｧ豁｣繝ｻ蠅・阜縺ｧ 0繝ｻ辣ｧ蟆・・縺ｧ雋・峨・*繧ｲ繧､繝ｳ縺ｯ縺薙ｌ縺九ｉ蜃ｺ縺吶・*
        //   aperture 縺ｯ繧ｯ繝ｩ繧ｹ繧ｿ縺ｮ驥榊ｿ・↑縺ｮ縺ｧ縲∫ｵ瑚ｷｯ髟ｷ縺九ｉ ﾎｴ 繧帝・ｮ励＠縺ｦ縺ｯ縺・￠縺ｪ縺・        //   笏笏 驥榊ｿ・・繧ｯ繝ｩ繧ｹ繧ｿ縺ｮ讒区・縺悟､峨ｏ繧九→霍ｳ縺ｶ縺ｮ縺ｧ縲√ご繧､繝ｳ縺御ｸ埼｣邯壹↓縺ｪ繧具ｼ亥ｮ滓ｸｬ縺ｧ
        //   謇峨・謗・ｼ輔′ 4kHz 縺ｧ 0.474竊・.132竊・.495 縺ｨ繧ｸ繧ｰ繧ｶ繧ｰ縺励◆・峨・        //   ﾎｴ 閾ｪ菴薙・蛟呵｣懊・ min 縺ｪ縺ｮ縺ｧ騾｣邯壹・        float delta = 0.0f;
        // 髢句哨縺ｮ髢九″蜈ｷ蜷医↓繧医ｋ蠎・ｸｯ蝓溘・謚大宛(0..1)縲る哩縺ｾ縺｣縺ｦ縺・ｋ縺ｻ縺ｩ蟆上＆縺・・        //   gain 縺ｫ縺ｯ譌｢縺ｫ謗帙￠縺ｦ縺ゅｋ縲りｷ晞屬貂幄｡ｰ縺縺代〒魑ｴ繧峨☆蛛ｴ・亥屓謚倥・蜻ｨ豕｢謨ｰ萓晏ｭ倥ｒ謐ｨ縺ｦ繧玖ｨｭ螳夲ｼ峨・
        //   縺薙ｌ繧貞挨騾疲寺縺代ｋ蠢・ｦ√′縺ゅｋ縺ｮ縺ｧ縲∝腰菴薙〒繧ょ叙繧後ｋ繧医≧縺ｫ縺励※縺ゅｋ縲・        float openGain = 1.0f;
        float slitWidth = 0.0f;   // 螳滓ｸｬ縺ｮ髫咎俣蟷・m)縲りｨｺ譁ｭ逕ｨ・医ご繧､繝ｳ縺ｯ openGain 縺ｫ蜈･縺｣縺ｦ縺・ｋ・・        // 髫咎俣縺ｮ蟷・↓繧医ｋ蟶ｯ蝓溘＃縺ｨ縺ｮ騾壹ｊ繧・☆縺輔る幕縺上⊇縺ｩ菴弱＞蟶ｯ蝓溘′蜈･縺｣縺ｦ縺上ｋ縲・        float openBand[kNumBands] = {1, 1, 1, 1, 1, 1};
        // 髢句哨縺ｮ**蠎・′繧・*繧定｡ｨ縺吝ｮ溷惠縺ｮ轤ｹ・医・繧､繝倥Φ繧ｹ・峨る幕蜿｣繧堤せ1縺､縺ｧ魑ｴ繧峨☆縺ｨ謌ｸ蜿｣縺・        // 繝斐Φ繝昴う繝ｳ繝医↓閨槭％縺医ｋ 笏笏 迴ｾ螳溘・髢句哨縺ｯ髱｢蜈ｨ菴薙′莠梧ｬ｡髻ｳ貅舌→縺励※蜈峨ｋ縲・        // spread[0] 縺ｯ aperture 縺ｨ蜷後§縲ゆｻ･髯阪・髢句哨縺ｮ遶ｯ縺ｮ譁ｹ縺ｸ謨｣繧峨＠縺溽せ縲・        static constexpr int kMaxSpread = 3;
        Vec3 spread[kMaxSpread];
        int nSpread = 1;
        float gain[kNumBands] = {0, 0, 0, 0, 0, 0};
    };

    //   order: 謗｢邏｢縺吶ｋ蝗樊釜縺ｮ谺｡謨ｰ縲・=1谺｡縺ｮ縺ｿ / 2=1谺｡縺ｧ螻翫°縺ｪ縺代ｌ縺ｰ髢句哨繧呈眠縺励＞蟋狗せ縺ｫ蜀榊ｸｰ縲・    //     L 蟄励・蟒贋ｸ九ｄ縲∝｣√′豸医∴縺ｦ縺ｧ縺阪◆譁ｰ縺励＞騾夊ｷｯ縺・2 谺｡縺ｫ蠖薙◆繧九・    //     1 谺｡縺ｧ螻翫￠縺ｰ 2 谺｡縺ｯ謗｢邏｢縺励↑縺・・縺ｧ縲・幕縺代◆蝣ｴ謇縺ｧ縺ｯ繧ｳ繧ｹ繝医′蠅励∴縺ｪ縺・・    int findDiffractionPaths(const Vec3& listener, const Vec3& source,
                             DiffractionPath* out, int maxPaths, int order = 2) const {
        if (!out || maxPaths <= 0) return 0;
        const bool occ = isOccluded(listener, source);
        const float directDist = std::max(length(source - listener), 1e-4f);

        // 笘・ヶ繝ｭ繝・き繝ｼ縺ｯ縲檎峩謗･邱壹′螳滄圀縺ｫ莠､蟾ｮ縺吶ｋ邂ｱ縲阪□縺代↓縺吶ｋ・郁・蠑ｵ縺ｪ縺暦ｼ峨・        //
        //   荳譎・margin=4m 繧貞ｸｸ逕ｨ縺励◆縺後・*髢峨§縺滄Κ螻九〒縺ｯ蠎翫・螟ｩ莠輔・蛛ｴ螢√∪縺ｧ
        //   繝悶Ο繝・き繝ｼ縺ｫ蜈･縺｣縺ｦ縺励∪縺・*縲・幕蜿｣縺・縺､縺励°縺ｪ縺・・縺ｫ蛟呵｣懊′3譛ｬ遶九▲縺溘・        //   蛛ｽ縺ｮ蛟呵｣懊←縺・＠縺ｧ謾ｯ驟阪け繝ｩ繧ｹ繧ｿ縺悟・繧梧崛繧上ｊ縲∫ｵ瑚ｷｯ髟ｷ縺・13.8m 竍・19.8m 縺ｨ
        //   6m 繧る｣帙ｓ縺ｧ縺・◆・郁ｷ晞屬貂幄｡ｰ縺縺代〒魑ｴ繧峨☆縺ｨ縲√◎縺ｮ縺ｾ縺ｾ髻ｳ驥上→驕・ｻｶ縺ｮ鬟帙・縺ｫ縺ｪ繧具ｼ峨・        //   繧ｳ繧ｹ繝医ｂ 1.4ms 竊・10.6ms/frame 縺ｫ謔ｪ蛹悶＠縺溘・        //
        //   荳譁ｹ margin=0 縺ｫ縺吶ｋ縺ｨ縲∬ｦ矩壹＠蛛ｴ縺ｧ蛟呵｣懊′豸医∴縺ｦ繝輔Ξ繝阪Ν陬懈ｭ｣縺悟柑縺九↑縺上↑繧翫・        //   蠖ｱ蠅・阜縺ｮ繧ｲ繧､繝ｳ縺・0.033 竊・0.438 縺ｫ謔ｪ蛹悶＠縺溘・        //   陬懈ｭ｣縺悟柑縺冗ｯ・峇縺ｯ ﾎｴ < 0.1ﾎｻ 遞句ｺｦ・・25Hz 縺ｧ 0.28m・峨↑縺ｮ縺ｧ縲・*1m 縺ゅｌ縺ｰ雜ｳ繧翫ｋ**縲・        //   髢峨§縺滄Κ螻九〒繧・1m 縺ｮ閹ｨ蠑ｵ縺ｪ繧牙ｺ翫・螟ｩ莠輔・蛛ｴ螢√・逶ｴ謗･邱壹→莠､蟾ｮ縺励↑縺・・        constexpr float margin = 1.0f;

        // 髢句哨・晄婿蜷代け繝ｩ繧ｹ繧ｿ縲よ滋繧√ｋ轤ｹ縺ｯ繧ｯ繝ｩ繧ｹ繧ｿ蜀・・驥阪∩莉倥″驥榊ｿ・↓縺励※騾｣邯壹↓繧ｹ繝ｩ繧､繝峨＆縺帙ｋ
        // ・域焔蜑咲ｨ懃ｷ壺・螂･遞懃ｷ壹・荵励ｊ謠帙∴縺ｧ驥榊ｿ・′貊代ｉ縺九↓遘ｻ繧具ｼ晞｣帙・縺ｪ縺・ｼ峨・        //
        //   驥阪∩縺ｯ**荳企剞縺ｧ鬆ｭ謇薙■縺ｫ縺励↑縺・*蜑榊ｷ晏､・・aekawa::apertureWeight・峨・        //   蜃ｺ蜉帙ご繧､繝ｳ縺ｫ縺ｯ 24dB 縺ｮ荳企剞縺後≠繧九′縲・㍾縺ｿ縺ｫ荳企剞繧偵°縺代ｋ縺ｨ驕縺・幕蜿｣縺悟・驛ｨ蜷檎せ縺ｫ縺ｪ繧翫・        //   謾ｯ驟埼幕蜿｣縺瑚埋縺ｾ縺｣縺ｦ**螳壻ｽ阪′縺ｼ繧・￠繧・*縲よ悽菴懊〒蜆ｪ蜈医☆繧九・縺ｯ驕ｮ阡ｽ驥上・邊ｾ蠎ｦ縺ｧ縺ｯ縺ｪ縺・        //   蝗樊釜轤ｹ縺ｸ縺ｮ螳壻ｽ阪↑縺ｮ縺ｧ縲∝━蜉｣縺後・縺｣縺阪ｊ莉倥￥譁ｹ繧呈治繧九・        constexpr int kMaxCl = 24;
        // 髢句哨轤ｹ縺ｯ縲碁㍾蠢・↓譛繧りｿ代＞**螳溷惠縺ｮ蛟呵｣懃せ**縲阪↓縺吶ｋ・磯㍾蠢・◎縺ｮ繧ゅ・縺ｯ菴ｿ繧上↑縺・ｼ峨・        //
        //   驥榊ｿ・pAcc/w 縺ｯ縺ｩ縺ｮ遞懃ｷ壹・荳翫↓繧ゅ√←縺ｮ髫咎俣縺ｮ荳ｭ縺ｫ繧ら┌縺・楔遨ｺ縺ｮ轤ｹ縺ｧ縲∵演縺ｮ蟾ｦ蜿ｳ縺ｮ
        //   遞懃ｷ壹′ 1 繧ｯ繝ｩ繧ｹ繧ｿ縺ｫ縺ｪ繧九→譚ｿ縺ｮ荳ｭ繧・攸繧帝壹ｊ驕弱℃縺溷・縺ｮ遨ｺ髢薙∈蜃ｺ縺ｦ縺励∪縺・・        //   縺吶ｋ縺ｨ髢句哨蟷・′謇峨・髫咎俣縺ｧ縺ｯ縺ｪ縺城Κ螻九・蟾ｮ縺玲ｸ｡縺励ｒ貂ｬ繧奇ｼ亥ｮ滓ｸｬ: 髢画演縺ｧ 3.03m・峨・        //   騾・↓譚ｿ縺ｮ荳ｭ縺ｫ關ｽ縺｡繧後・蟷・0 縺ｨ縺励※髢九＞縺ｦ縺・ｋ謇峨∪縺ｧ鮟吶ｉ縺帙ｋ・亥ｮ滓ｸｬ: 15縲・5ﾂｰ 縺・0・峨・        //   縺九→縺・▲縺ｦ ﾎｴ 譛蟆上・蛟呵｣懊ｒ謗｡繧九→螟悶ｌ蛟､繧呈鏡縺・ｼ亥ｮ滓ｸｬ: 髢句哨縺・y=42 繧・x=-20 繧呈欠縺励◆・峨・        //   竊・驥榊ｿ・・螳牙ｮ壽ｧ縺ｯ縺昴・縺ｾ縺ｾ縺ｫ縲∫せ縺縺大ｮ溷惠縺ｮ繧ゅ・縺ｸ蟇・○繧九・        //   蛟呵｣懊・謨ｰ蛟九≠繧後・雜ｳ繧翫ｋ・亥酔縺倥け繝ｩ繧ｹ繧ｿ蜀・・譁ｹ蜷・25ﾂｰ 莉･蜀・↑縺ｮ縺ｧ蟇・ｼ峨・        constexpr int kClPts = 8;
        struct Cl {
            Vec3 pAcc; Vec3 dir; float w; float minDelta;
            Vec3 pts[kClPts]; int npts;
            // 蜷・呵｣懃せ縺御ｹ励▲縺ｦ縺・◆遞懃ｷ壹・蜷代″縲る幕蜿｣轤ｹ繧帝∈繧薙□蠕後・*縺昴・轤ｹ縺ｮ遞懃ｷ・*縺ｧ
            // 髫咎俣蟷・ｒ貂ｬ繧九◆繧√↓隕√ｋ縲ゅけ繝ｩ繧ｹ繧ｿ蜀・〒 max/min 繧呈治繧九→縲∝挨縺ｮ遞懃ｷ壹′
            // 貂ｬ縺｣縺滄Κ螻九・蟾ｮ縺玲ｸ｡縺励↓荳頑嶌縺阪＆繧後※髫咎俣縺瑚ｦ九∴縺ｪ縺上↑繧具ｼ亥ｮ滓ｸｬ縺ｧ縺昴≧縺ｪ縺｣縺滂ｼ峨・            Vec3 edges[kClPts];
            // BTM 逕ｨ縲らｨ懃ｷ壹・**遶ｯ轤ｹ**縺ｨ 0 髱｢謗･邱壹・TM 縺ｯ譛蛾剞縺ｧ縺ゅｋ縺薙→繧剃ｽｿ縺・・縺ｧ
            // 譁ｹ蜷代□縺代〒縺ｯ雜ｳ繧翫↑縺・ｼ亥燕蟾昴→縺ｮ譛ｬ雉ｪ逧・↑驕輔＞縺後％縺難ｼ峨・            Vec3 eA[kClPts], eB[kClPts], eRefT[kClPts];
            int inst;   // 遞懃ｷ壹′螻槭☆繧狗ｮｱ縲ゅ悟酔縺倬幕蜿｣縺九阪ｒ蛻､螳壹☆繧九・縺ｫ菴ｿ縺・            // 髢句哨縺ｮ**蟷ｾ菴慕噪縺ｪ荳ｭ蠢・*逕ｨ縺ｮ闢・ｩ阪る㍾縺ｿ縺ｯ謗繧√・豺ｱ縺・edgeW)縺縺代↓縺吶ｋ縲・            //   pAcc 蛛ｴ縺ｯ apertureWeight(ﾎｴ) 驥阪∩縺ｪ縺ｮ縺ｧ繝ｪ繧ｹ繝翫・蛛ｴ縺ｮ邵√∈蠑ｷ縺丞ｯ・ｋ縺後・            //   縺薙■繧峨・縺ｻ縺ｼ蟷ｳ繧会ｼ晞幕蜿｣縺ｮ逵溘ｓ荳ｭ繧呈欠縺吶・            //   笘・腰邏泌ｹｳ蝮・↓縺励※縺ｯ縺・￠縺ｪ縺・よ眠縺励＞轤ｹ縺梧ｺ鬘阪〒蜈･縺｣縺ｦ荳ｭ蠢・′鬟帙・
            //     ・亥ｮ滓ｸｬ: 邨瑚ｷｯ髟ｷ縺ｮ髫｣謗･蟾ｮ縺・1.38m 竊・1.76m 縺ｫ謔ｪ蛹悶＠縺滂ｼ峨・            //     edgeW 縺ｯ逕溘∪繧後ｋ迸ｬ髢・0 縺ｪ縺ｮ縺ｧ縲・㍾縺ｿ縺ｫ菴ｿ縺医・騾｣邯壹↓蜈･繧九・            Vec3 gAcc; float gw;
        };
        Cl cl[kMaxCl];
        int ncl = 0;
        const float cosThresh = 0.90f;   // ~25ﾂｰ莉･蜀・・蜷後§髢句哨縺ｨ縺ｿ縺ｪ縺呻ｼ郁ｨｭ險・ﾂｧ6-1・・        float globalMinDelta = -1.0f;

        // 2 縺､縺ｮ繧ｯ繝ｩ繧ｹ繧ｿ縺・*蜷後§髢句哨**繧貞峇縺｣縺ｦ縺・ｋ縺九よ演繧・虻蜿｣繧貞錐謖・＠縺励↑縺・ｸ闊ｬ縺ｮ蛻､螳壹・        //   竭 遞懃ｷ壹・荵励ｋ邂ｱ縺悟酔荳蟷ｳ髱｢・亥酔縺倅ｻ募・繧翫・荳驛ｨ縲よ攸縺ｮ譛繧り埋縺・ｻｸ繧呈ｳ慕ｷ壹→縺吶ｋ・・        //   竭｡ 莉｣陦ｨ轤ｹ縺ｩ縺・＠縺ｮ髢薙↓驕ｮ繧九ｂ縺ｮ縺檎┌縺・ｼ茨ｼ昴◎縺ｮ髢薙′髢句哨縺昴・繧ゅ・・・        //   髢峨§縺滓演縺後≠繧後・ 竭｡ 縺梧・遶九＠縺ｪ縺・・縺ｧ縲√∪縺ｨ繧√ｉ繧後↑縺・笏笏 縺薙ｌ縺ｯ豁｣縺励＞縲・        //   縺昴・蝣ｴ蜷医◎繧ゅ◎繧る壹ｊ驕薙′辟｡縺・・縺ｧ縲√ち繝・・繧らｫ九◆縺ｪ縺・・        auto sameApertureCluster = [&](const Cl& a, const Cl& b) {
            if (a.inst < 0 || b.inst < 0
                || a.inst >= instanceCount() || b.inst >= instanceCount()) return false;
            const Obb& oa = instances_[static_cast<std::size_t>(a.inst)].obb;
            const Obb& ob = instances_[static_cast<std::size_t>(b.inst)].obb;
            auto thinN = [](const Obb& o) {
                const float hx = o.halfExtents.x, hy = o.halfExtents.y, hz = o.halfExtents.z;
                Vec3 nn = o.axisZ;
                if (hx <= hy && hx <= hz) nn = o.axisX;
                else if (hy <= hx && hy <= hz) nn = o.axisY;
                return (length(nn) > 1e-4f) ? normalized(nn) : Vec3(0, 0, 1);
            };
            const Vec3 na = thinN(oa), nb = thinN(ob);
            if (std::fabs(dot(na, nb)) < 0.999f) return false;                 // 竭 蜷代″縺碁＆縺・            if (std::fabs(dot(na, oa.center - ob.center)) > 0.05f) return false; // 竭 蛻･縺ｮ蟷ｳ髱｢
            if (a.npts <= 0 || b.npts <= 0) return false;
            return !isOccluded(a.pts[0], b.pts[0]);                            // 竭｡ 髢薙′髢九＞縺ｦ縺・ｋ
        };

        auto gather = [&](bool bothEnds) {
            ncl = 0;
            globalMinDelta = -1.0f;
            const Vec3 travelDir = normalized(source - listener);
            forEachDiffractionCandidate(listener, source,
                [&](const Vec3& P, float d, const Vec3& edgeDir, const Vec3& refT,
                    const Vec3& eA, const Vec3& eB, float edgeW, int inst) {
                    if (globalMinDelta < 0.0f || d < globalMinDelta) globalMinDelta = d;
                    // 笘・滋繧√・豺ｱ縺輔°繧画擂繧矩㍾縺ｿ繧呈寺縺代ｋ縲よ滋繧√・蜿ｯ蜷ｦ繧帝明蛟､縺ｧ莠悟､縺ｫ縺吶ｋ縺ｨ縲・                    //   蛟呵｣懊′貅鬘阪〒逕溘∪繧後※豸医∴繧九・縺ｧ繧ｯ繝ｩ繧ｹ繧ｿ縺ｮ驥榊ｿ・′鬟帙・
                    //   ・・ryEdge 縺ｮ penNearWeight 縺ｮ繧ｳ繝｡繝ｳ繝亥盾辣ｧ・峨・                    const float w = maekawa::apertureWeight(occ ? d : -d) * edgeW;
                    if (w < 1e-6f) return;
                    const float sw = slitWidthAt(P, edgeDir, travelDir);
                    const Vec3 dir = normalized(P - listener);
                    int best = -1; float bestDot = cosThresh;
                    for (int i = 0; i < ncl; ++i) {
                        const float dt = dot(dir, cl[i].dir);
                        if (dt > bestDot) { bestDot = dt; best = i; }
                    }
                    if (best >= 0) {
                        cl[best].pAcc = cl[best].pAcc + P * w;
                        cl[best].gAcc = cl[best].gAcc + P * edgeW;
                        cl[best].gw += edgeW;
                        cl[best].dir = normalized(cl[best].dir * cl[best].w + dir * w);
                        cl[best].w += w;
                        if (d < cl[best].minDelta) cl[best].minDelta = d;
                        if (cl[best].npts < kClPts) {
                            const int k = cl[best].npts;
                            cl[best].edges[k] = edgeDir;
                            cl[best].eA[k] = eA; cl[best].eB[k] = eB; cl[best].eRefT[k] = refT;
                            cl[best].pts[cl[best].npts++] = P;
                        }
                    } else if (ncl < kMaxCl) {
                        cl[ncl].pAcc = P * w;
                        cl[ncl].gAcc = P * edgeW; cl[ncl].gw = edgeW;
                        cl[ncl].dir = dir; cl[ncl].w = w; cl[ncl].minDelta = d;
                        cl[ncl].pts[0] = P; cl[ncl].edges[0] = edgeDir;
                        cl[ncl].eA[0] = eA; cl[ncl].eB[0] = eB; cl[ncl].eRefT[0] = refT;
                        cl[ncl].npts = 1;
                        cl[ncl].inst = inst;
                        ++ncl;
                    }
                }, margin, bothEnds);

            // 笘・*蜷後§髢句哨繧貞峇縺・ｨ懃ｷ壹←縺・＠**縺ｯ 1 譛ｬ縺ｫ縺ｾ縺ｨ繧√ｋ縲・            //
            //   譁ｹ蜷・25ﾂｰ 縺ｧ蛻・￠縺ｦ縺・ｋ縺ｮ縺ｧ縲∵虻蜿｣繧呈ｨｪ縺九ｉ隕励￥縺ｨ蟾ｦ蜿ｳ縺ｮ邵√′蛻･繧ｯ繝ｩ繧ｹ繧ｿ縺ｫ縺ｪ繧九・            //   縺吶ｋ縺ｨ蜷後§荵ｾ縺・◆菫｡蜿ｷ縺ｮ繧ｳ繝斐・縺・*驕輔≧驕・ｻｶ縺ｧ 2 譛ｬ**魑ｴ繧九ょ屓謚倥ち繝・・縺ｯ蟷ｳ蝮ｦ
            //   ・・iffractionDistanceOnly・峨↑縺ｮ縺ｧ蜈ｨ蟶ｯ蝓溘〒蜷檎嶌縺ｫ蜉ｹ縺阪∵ｫ帛ｽ｢繝輔ぅ繝ｫ繧ｿ縺ｫ縺ｪ繧九・            //   螳滓ｸｬ・・est_DiffractionGap 縺ｨ蜷悟ｽ｢迥ｶ・・
            //     x=2.0 縺ｧ邨瑚ｷｯ蟾ｮ 3.18m 竊・隨ｬ1繝弱ャ繝・54Hz・井ｻ･髯・162/270/378Hz 窶ｦ・・            //     x=5.0 縺ｧ邨瑚ｷｯ蟾ｮ 0.90m 竊・隨ｬ1繝弱ャ繝・192Hz
            //   菴主沺縺九ｉ菴惹ｸｭ蝓溘↓繝弱ャ繝√′荳ｦ縺ｶ縺ｮ縺ｧ縲後ラ繝ｩ繝縺悟炎繧後※菴朱浹縺悟｣翫ｌ繧九阪→閨槭％縺医ｋ縲・            //   笘・碁幕蜿｣繧定､・焚轤ｹ縺ｸ謨｣繧峨＠縺ｦ蝓九ａ繧九阪・**騾・柑譫・*縺縺｣縺滂ｼ亥ｮ滓ｸｬ: 遲蛾俣髫斐↑縺ｮ縺ｧ
            //     蜻ｨ譛溽噪縺ｪ繝弱ャ繝√′豺ｱ縺上↑繧翫・轤ｹ縺ｧ -27.4dB縲・轤ｹ縺ｧ -28.3dB・峨ゅ∪縺ｨ繧√ｋ譁ｹ縺梧ｭ｣縺励＞縲・            //
            //   縲悟酔縺倬幕蜿｣縺九阪・謇峨ｄ謌ｸ蜿｣繧貞錐謖・＠縺帙★縺ｫ豎ｺ繧√ｋ:
            //     竭 遞懃ｷ壹′荵励ｋ邂ｱ縺・*蜷御ｸ蟷ｳ髱｢**・亥酔縺倅ｻ募・繧翫・荳驛ｨ・・            //     竭｡ 2 轤ｹ縺ｮ髢薙ｒ**驕ｮ繧九ｂ縺ｮ縺檎┌縺・*・茨ｼ昴◎縺ｮ髢薙′髢句哨縺昴・繧ゅ・・・            //   蛻･縲・・謌ｸ蜿｣縺ｪ繧・竭｡ 縺ｧ髢薙↓螢√′縺ゅｋ縺ｮ縺ｧ豺ｷ縺悶ｉ縺ｪ縺・ょ髄縺九＞蜷医≧螢√↑繧・竭 縺ｧ螟悶ｌ繧九・            for (int i = 0; i < ncl; ++i) {
                for (int j = ncl - 1; j > i; --j) {
                    if (!sameApertureCluster(cl[i], cl[j])) continue;
                    // j 繧・i 縺ｸ逡ｳ繧縲る㍾縺ｿ縺､縺阪・驥上・雜ｳ縺励∫せ縺ｯ蜈･繧九□縺大叙繧願ｾｼ繧縲・                    cl[i].pAcc = cl[i].pAcc + cl[j].pAcc;
                    cl[i].gAcc = cl[i].gAcc + cl[j].gAcc;
                    cl[i].gw += cl[j].gw;
                    cl[i].dir = normalized(cl[i].dir * cl[i].w + cl[j].dir * cl[j].w);
                    cl[i].w += cl[j].w;
                    cl[i].minDelta = std::min(cl[i].minDelta, cl[j].minDelta);
                    for (int k = 0; k < cl[j].npts && cl[i].npts < kClPts; ++k) {
                        const int m = cl[i].npts;
                        cl[i].pts[m] = cl[j].pts[k];     cl[i].edges[m] = cl[j].edges[k];
                        cl[i].eA[m] = cl[j].eA[k];       cl[i].eB[m] = cl[j].eB[k];
                        cl[i].eRefT[m] = cl[j].eRefT[k];
                        ++cl[i].npts;
                    }
                    cl[j] = cl[ncl - 1];
                    --ncl;
                }
            }
        };

        // 縺ｾ縺壹・ 1 谺｡縺ｨ縺励※謗｢縺呻ｼ亥屓謚倡せ縺・listener 縺ｨ source 縺ｮ荳｡譁ｹ縺九ｉ隕矩壹○繧九ｂ縺ｮ・峨・        gather(true);

        // 笘・ 譛ｬ繧ら┌縺・→縺阪□縺代・ 谺｡繧定ｩｦ縺吶・        //   縺薙・縺ｨ縺阪茎ource 縺九ｉ繧りｦ矩壹○繧九肴擅莉ｶ繧貞､悶☆ 笏笏 逵溘・ 2 谺｡蝗樊釜縺ｧ縺ｯ縲√←縺ｮ遞懃ｷ壹ｂ
        //   荳｡譁ｹ縺九ｉ縺ｯ隕矩壹○縺ｪ縺・ｼ医◎繧後′縺ｾ縺輔↓ 2 谺｡縺ｧ縺励°螻翫°縺ｪ縺・→縺・≧縺薙→・峨・縺ｧ縲・        //   譚｡莉ｶ繧剃ｻ倥￠縺溘∪縺ｾ縺縺ｨ蛟呵｣懊′ 0 譛ｬ縺ｫ縺ｪ繧翫∝・蟶ｰ縺ｫ蜈･繧句燕縺ｫ繝ｫ繝ｼ繝励′遨ｺ縺ｫ縺ｪ繧九・        //   荳譁ｹ縲・ 谺｡縺瑚ｦ九▽縺九▲縺ｦ縺・ｋ縺ｨ縺阪↓邱ｩ繧√※縺ｯ縺・￠縺ｪ縺・Ｔource 蛛ｴ縺ｮ雋ｫ騾壼愛螳壹′螟悶ｌ縲・        //   螢√ｒ遯√″謚懊￠繧狗ｵ瑚ｷｯ縺碁幕蜿｣縺ｨ縺励※豺ｷ縺悶ｋ・亥ｮ滓ｸｬ縺ｧ縲碁幕蜿｣蛛ｴ繧呈欠縺吶阪ユ繧ｹ繝医′關ｽ縺｡縺滂ｼ峨・        bool secondOrder = false;
        if (ncl == 0 && order > 1 && occ) {
            gather(false);
            secondOrder = true;
        }

        if (scene_detail::diffDebug())
            std::fprintf(stderr, "  [謗｢邏｢] L(%.2f,%.2f,%.2f) 驕ｮ阡ｽ=%d 繧ｯ繝ｩ繧ｹ繧ｿ=%d 2谺｡=%d\n",
                         listener.x, listener.y, listener.z, occ ? 1 : 0, ncl,
                         secondOrder ? 1 : 0);
        if (ncl == 0) return 0;   // 蝗樊釜縺ｮ逶ｸ謇九′辟｡縺・ょ他縺ｳ蜃ｺ縺怜・縺ｧ縲碁・阡ｽ縺ｪ繧・/隕矩壹＠縺ｪ繧・.0縲阪ｒ豎ｺ繧√ｋ

        // 隕矩壹＠縺ｦ縺・ｋ縺ｨ縺阪・縲∝屓謚倥・縲檎峩謗･邨瑚ｷｯ縺ｸ縺ｮ陬懈ｭ｣縲阪〒縺ゅ▲縺ｦ蛻･邨瑚ｷｯ縺ｧ縺ｯ縺ｪ縺・・        //   蛻･邨瑚ｷｯ縺ｨ縺励※隍・焚霑斐＠縺ｦ繧ｨ繝阪Ν繧ｮ繝ｼ蜉邂励☆繧九→ 1.0 繧定ｶ・∴縺ｦ驕主､ｧ險井ｸ翫↓縺ｪ繧具ｼ郁ｨｭ險・ﾂｧ5-6・峨・        //   譁ｹ蜷代ｂ髻ｳ貅舌◎縺ｮ繧ゅ・縺ｪ縺ｮ縺ｧ縲・ 譛ｬ縺ｫ縺ｾ縺ｨ繧√※霑斐☆縲・        if (!occ) {
            out[0] = DiffractionPath{};
            out[0].aperture = source;
            out[0].pathLength = directDist;
            out[0].delta = -globalMinDelta;
            maekawa::gainBands(out[0].delta, out[0].gain);
            // 笘・ｦ矩壹○縺ｦ縺・ｋ蛛ｴ縺ｫ繧ょ酔縺倬幕蜿｣縺ｮ蛻ｶ髯舌ｒ謗帙￠繧九・            //   謗帙￠縺ｪ縺・→縲∝ｽｱ蠅・阜繧定ｷｨ縺・□迸ｬ髢薙↓繧ｲ繝ｼ繝医′豸医∴縺ｦ髻ｳ驥上′霍ｳ縺ｶ
            //   ・亥ｮ滓ｸｬ: 驕ｮ阡ｽ蛛ｴ 0.285 竊・隕矩壹＠蛛ｴ 0.563・峨ら黄逅・噪縺ｫ繧ゅ∝｢・阜縺ｮ縺吶＄螟悶〒縺ｯ
            //   繝輔Ξ繝阪Ν繧ｾ繝ｼ繝ｳ縺ｯ縺ｾ縺蜊雁・螢√↓髫繧後※縺・ｋ縺ｮ縺ｧ縲∝宛髯舌′邯壹￥縺ｮ縺梧ｭ｣縺励＞縲・            //   驕縺悶°繧後・髢句哨邇・・ 1 縺ｫ霑代▼縺阪∬・辟ｶ縺ｫ蛻ｶ髯舌′豸医∴繧九・            float fr[kNumBands];
            if (useFresnelAperture_ && apertureFresnelBands(listener, source, fr)) {
                for (int b = 0; b < kNumBands; ++b) {
                    out[0].gain[b] *= fr[b];
                    out[0].openBand[b] = fr[b];
                }
                static const float w6[kNumBands] = {3.0f, 2.5f, 2.0f, 1.3f, 1.0f, 0.8f};
                float gs = 0.0f, ws = 0.0f;
                for (int b = 0; b < kNumBands; ++b) { gs += w6[b] * fr[b]; ws += w6[b]; }
                out[0].openGain = (ws > 0.0f) ? gs / ws : 1.0f;
            }
            return 1;
        }

        // 笘・・繝ｼ繧ｿ繝ｫ縺後≠繧九↑繧峨∫ｨ懃ｷ壽爾邏｢縺ｧ縺ｯ縺ｪ縺・*繝昴・繧ｿ繝ｫ縺九ｉ邨瑚ｷｯ繧剃ｽ懊ｋ**縲・        //   謗｢邏｢縺檎┌縺・・縺ｧ縲∵爾邏｢逕ｱ譚･縺ｮ荳埼｣邯夲ｼ育ｵ瑚ｷｯ縺檎函縺ｾ繧後ｋ/豸医∴繧具ｼ峨′襍ｷ縺阪↑縺・・        //   譁ｹ蜷托ｼ磯幕縺・※縺・ｋ驛ｨ蛻・・驥榊ｿ・ｼ峨→繧ｨ繝阪Ν繧ｮ繝ｼ・磯幕蜿｣邇・ｼ峨′蜷後§遨榊・縺九ｉ蜃ｺ繧九・縺ｧ縲・        //   荳｡閠・′縺壹ｌ縺ｦ鬟帙・縺薙→繧ゅ↑縺・ゅ％縺薙′莉頑律縺壹▲縺ｨ謌ｦ縺｣縺ｦ縺・◆蝠城｡後・隗｣縲・        //   繝昴・繧ｿ繝ｫ縺・譫壹ｂ蜿榊ｿ懊＠縺ｪ縺代ｌ縺ｰ縲∽ｸ九・遞懃ｷ壽爾邏｢縺ｸ關ｽ縺｡繧具ｼ育ｩｴ繧堤ｩｺ縺代↑縺・ｼ峨・        if (!portals_.empty()) {
            int np2 = 0;
            for (const Portal& pt : portals_) {
                if (np2 >= maxPaths) break;
                if (!pt.active) continue;
                float f[kNumBands]; Vec3 cp(0, 0, 0);
                if (!portalCoupling(pt, listener, source, f, &cp)) continue;
                // 菴主沺蜉驥阪・蠎・ｸｯ蝓溘せ繧ｫ繝ｩ・井ｻ悶・邨瑚ｷｯ縺ｨ蜷後§逡ｳ縺ｿ譁ｹ・峨・                static const float w6[kNumBands] = {3.0f, 2.5f, 2.0f, 1.3f, 1.0f, 0.8f};
                float gs = 0.0f, ws = 0.0f;
                for (int b = 0; b < kNumBands; ++b) { gs += w6[b] * f[b]; ws += w6[b]; }
                const float bb = (ws > 0.0f) ? gs / ws : 0.0f;
                if (bb < 1e-5f) continue;          // 螳悟・縺ｫ蝪槭′縺｣縺ｦ縺・ｋ・晞ｳｴ繧峨＆縺ｪ縺・                DiffractionPath& p = out[np2];
                p.aperture = cp;
                p.spread[0] = cp; p.nSpread = 1;
                p.pathLength = length(cp - listener) + length(source - cp);
                p.delta = std::max(p.pathLength - directDist, 0.0f);
                p.openGain = bb;
                p.slitWidth = 0.0f;
                const float cmul = apertureContrastMap(bb);
                for (int b = 0; b < kNumBands; ++b) {
                    // 笘・燕蟾昴・ ﾎｴ 貂幄｡ｰ縺ｯ謗帙￠縺ｪ縺・る幕蜿｣繧堤ｴ騾壹ｊ縺吶ｋ髻ｳ縺ｯ譖ｲ縺後▲縺ｦ縺・↑縺・・                    //   謗帙￠繧九→莠碁㍾縺ｫ縺ｪ繧具ｼ亥ｮ滓ｸｬ縺ｧ 1kHz 縺ｫ 4蛟阪・蟾ｮ縺悟・縺滂ｼ峨・                    p.gain[b] = f[b] * cmul;
                    p.openBand[b] = f[b];
                }
                ++np2;
            }
            // 笘・・繝ｼ繧ｿ繝ｫ縺後≠繧九↑繧・*縺昴ｌ縺悟髪荳縺ｮ遲斐∴**縲・譛ｬ縺ｧ繧ゅ％縺薙〒霑斐☆縲・            //   莉･蜑阪・縲・譛ｬ縺ｪ繧画立邨瑚ｷｯ・育ｨ懃ｷ壽爾邏｢・峨∈關ｽ縺｡繧九阪→縺励※縺・◆縺後√◎繧後□縺ｨ
            //   繝昴・繧ｿ繝ｫ縺碁哩縺倥※縺・ｋ縺ｨ縺搾ｼ晏屓謚倥′辟｡縺・・縺壹・縺ｨ縺阪↓譌ｧ邨瑚ｷｯ縺瑚ｵｰ繧翫・            //   髢峨§縺滄Κ螻九・荳ｭ縺ｧ蟷ｻ縺ｮ邨瑚ｷｯ繧定ｦ九▽縺代※魑ｴ繧峨＠縺ｦ縺・◆
            //   ・亥ｮ滓ｸｬ: 髢画演縺ｧ螢√・蜑阪↓遶九▽縺ｨ gain 0.32 縺ｮ蝗樊釜繧ｿ繝・・縺檎ｫ九■縲・            //     謇峨・蜑阪ｈ繧雁､ｧ縺阪￥繝ｻ蟷ｳ蝮ｦ縺ｫ縺ｪ縺｣縺ｦ縺・◆・峨・            //   繝帙せ繝医′繝昴・繧ｿ繝ｫ繧堤ｽｮ縺上・縺ｯ縲碁幕蜿｣縺ｯ縺薙％縺縺代阪→縺・≧螳｣險縺ｪ縺ｮ縺ｧ縲・            //   縺ｩ繧後ｂ髢九＞縺ｦ縺・↑縺代ｌ縺ｰ髢句哨邨檎罰縺ｮ髻ｳ縺ｯ辟｡縺・・            return np2;
        }

        // 驕ｮ阡ｽ譎ゅ・髢句哨縺斐→縺ｫ迢ｬ遶九＠縺溽ｵ瑚ｷｯ縲る㍾縺ｿ荳贋ｽ阪°繧・maxPaths 譛ｬ縲・        bool used[kMaxCl] = {false};
        int n = 0;
        while (n < maxPaths && n < ncl) {
            int bi = -1; float bw = -1.0f;
            for (int i = 0; i < ncl; ++i) if (!used[i] && cl[i].w > bw) { bw = cl[i].w; bi = i; }
            if (bi < 0) break;
            used[bi] = true;
            // 驥榊ｿ・ｒ蜃ｺ縺励√◎縺薙∈譛繧りｿ代＞螳溷惠蛟呵｣懊∈繧ｹ繝翫ャ繝励☆繧具ｼ井ｸ翫・ Cl 縺ｮ繧ｳ繝｡繝ｳ繝亥盾辣ｧ・峨・            //   笘・せ繝翫ャ繝励ｒ繧・ａ縺ｦ驥榊ｿ・・縺ｾ縺ｾ菴ｿ縺・｡医・**謔ｪ蛹悶＠縺・*
            //     ・域髪驟埼幕蜿｣縺ｮ遘ｻ蜍・15.81m 竊・21.95m縲∝屓蟶ｰ繧・1竊・ 莉ｶ FAIL・峨・            //     霍ｳ縺ｳ縺ｮ蜴溷屏縺ｯ繧ｹ繝翫ャ繝励〒縺ｯ縺ｪ縺上・*繧ｯ繝ｩ繧ｹ繧ｿ縺昴・繧ゅ・縺ｮ蜈･繧梧崛繧上ｊ**縲・            // 笘・ｳｴ繧峨☆轤ｹ縺ｯ縲・*髢句哨縺ｮ荳ｭ蠢・→繝ｪ繧ｹ繝翫・蛛ｴ縺ｮ轤ｹ縺ｮ荳ｭ髢・*縲阪↓鄂ｮ縺上・            //
            //   驥阪∩莉倥″驥榊ｿ・pAcc/w 縺ｯ apertureWeight(ﾎｴ) 驥阪∩縺ｪ縺ｮ縺ｧ縲∬ｿょ屓縺ｮ蟆代↑縺・            //   **繝ｪ繧ｹ繝翫・蛛ｴ縺ｮ邵・*縺ｸ蠑ｷ縺丞ｯ・ｋ縲よｨｪ縺九ｉ隕励￥縺ｨ謌ｸ蜿｣縺ｮ遶ｯ縺ｫ蠑ｵ繧贋ｻ倥″縲・            //   縲碁幕蜿｣縺九ｉ魑ｴ縺｣縺ｦ縺・ｋ縲阪→縺・≧諢溘§縺瑚埋繧後ｋ縲・            //   荳譁ｹ縲∫せ縺ｮ蜊倡ｴ泌ｹｳ蝮・・髢句哨縺ｮ蟷ｾ菴慕噪縺ｪ荳ｭ蠢・〒縲√Μ繧ｹ繝翫・縺悟虚縺・※繧・            //   縺ｻ縺ｨ繧薙←蜍輔°縺ｪ縺・ｼ茨ｼ昴←縺薙°繧芽◇縺・※繧ら悄繧謎ｸｭ縺九ｉ魑ｴ繧具ｼ峨・            //   縺昴・荳ｭ髢薙ｒ謗｡繧九→縲∵ｭ｣髱｢縺ｧ縺ｯ荳ｭ蠢・・讓ｪ縺九ｉ隕励￥縺ｨ邵∝ｯ・ｊ縲√→騾｣邯壹↓遘ｻ繧九・            //   縺ｩ縺｡繧峨ｂ騾｣邯夐㍼縺ｪ縺ｮ縺ｧ縲∽ｸｭ髢薙ｂ騾｣邯壹・            const Vec3 wCentroid = cl[bi].pAcc * (1.0f / std::max(cl[bi].w, 1e-6f));
            const Vec3 geoCenter = (cl[bi].gw > 1e-6f)
                                 ? cl[bi].gAcc * (1.0f / cl[bi].gw) : wCentroid;
            const Vec3 centroid = (geoCenter + wCentroid) * 0.5f;
            Vec3 Pc = centroid;
            Vec3 PcEdge(0, 1, 0);
            Vec3 PcRefT(1, 0, 0);   // 髱｢縺ｮ謗･邱夲ｼ遺冠遞懃ｷ壹・螟門髄縺搾ｼ峨・谺｡縺ｮ騾√ｊ蜷代″縺ｫ菴ｿ縺・            {
                float bd = -1.0f;
                for (int k = 0; k < cl[bi].npts; ++k) {
                    const Vec3 v = cl[bi].pts[k] - centroid;
                    const float dd = dot(v, v);
                    if (bd < 0.0f || dd < bd) {
                        bd = dd; Pc = cl[bi].pts[k]; PcEdge = cl[bi].edges[k];
                        PcRefT = cl[bi].eRefT[k];
                    }
                }
            }

            // 縺薙・髢句哨縺後←繧後□縺鷹幕縺・※縺・ｋ縺九・*蝗樊釜縺ｮ髻ｳ驥上・縺薙ｌ縺梧ｱｺ繧√ｋ**・育ｨ懃ｷ壽爾邏｢縺ｯ螳壻ｽ肴球蠖難ｼ峨・            //   蜑榊ｷ昴・蠑上・ ﾎｴ 縺励°隕九↑縺・・縺ｧ縲・.8cm 縺ｮ髫咎俣縺ｧ繧・1.8m 縺ｮ髢句哨縺ｧ繧ょ酔縺伜､繧定ｿ斐☆縲・            //
            // 笘・ヵ繝ｬ繝阪Ν繧ｾ繝ｼ繝ｳ縺ｯ**縺薙・髢句哨縺ｮ荳・*縺ｧ貂ｬ繧九らｵ瑚ｷｯ縺斐→縺ｫ髢句哨縺碁＆縺・・縺ｧ縲・            //   繝ｫ繝ｼ繝励・螟悶∈諡ｬ繧雁・縺吶％縺ｨ縺ｯ縺ｧ縺阪↑縺・ｼ域峡繧九→蜈ｨ髢句哨縺悟酔縺伜､縺ｫ縺ｪ繧翫・            //   貂ｬ縺｣縺ｦ縺・ｋ繧ゅ・縺後梧虻蜿｣縺ｨ繧ｾ繝ｼ繝ｳ縺ｮ髱｢遨肴ｯ斐阪→縺・≧蝗ｺ螳壼､縺ｫ蛹悶￠繧具ｼ峨・            //   險育ｮ鈴㍼縺ｯ髢句哨縺ｮ譛ｬ謨ｰ縺ｶ繧灘｢励∴繧九′縲∝屓謚俶ｮｵ縺ｯ螳滓ｸｬ 0.26ms 縺ｪ縺ｮ縺ｧ險ｱ螳ｹ遽・峇縲・            // 笘・ｪ薙・荳ｭ蠢・↓ Pc・育ｨ懃ｷ夂罰譚･縺ｮ髢句哨轤ｹ・峨ｒ貂｡縺励※縺ｯ縺・￠縺ｪ縺・１c 縺ｯ霍ｳ縺ｶ縺ｮ縺ｧ
            //   ・亥ｮ滓ｸｬ 3m・峨∫ｪ薙＃縺ｨ鬟帙ｓ縺ｧ蛟､縺御ｹｱ鬮倅ｸ九☆繧九らｪ薙・貂ｬ螳壹・蝓ｺ貅悶〒縺ｯ縺ｪ縺・            //   遨榊・縺ｮ遽・峇縺ｪ縺ｮ縺ｧ縲・*騾｣邯壹↓蜍輔￥轤ｹ**・育峩邱壹→蟷ｳ髱｢縺ｮ莠､轤ｹ・峨↓謐ｮ縺医ｋ縲・            float fres[kNumBands];
            // 笘・・繝ｼ繧ｿ繝ｫ蠑上ｒ菴ｿ縺・↑繧峨ヵ繝ｬ繝阪Ν遨榊・縺ｯ**蝗槭＆縺ｪ縺・*縲らｵ先棡繧剃ｸ蛻・ｽｿ繧上↑縺・≧縺医・            //   縺薙％縺ｯ蝗樊釜谿ｵ縺ｧ縺・■縺ｰ繧馴㍾縺・・逅・ｼ磯擇縺ｸ蜈ｨ繧､繝ｳ繧ｹ繧ｿ繝ｳ繧ｹ縺ｮ蠖ｱ繧呈兜蠖ｱ縺励※
            //   32陦古・蟶ｯ蝓淌・2轤ｹ縺ｧ遨阪・・峨ゆｽｿ繧上↑縺・ｭ斐∴縺ｫ謇輔≧逅・罰縺後↑縺・・            const bool haveFres = !apertureAsPortal_ && useFresnelAperture_
                                && apertureFresnelBands(listener, source, fres);
            float openGainHere = 1.0f;
            float slitHere = 0.0f;                 // 險ｺ譁ｭ逕ｨ縲ゅヵ繝ｬ繝阪Ν菴ｿ逕ｨ譎ゅ・貂ｬ繧峨↑縺・
            // 笘・TM 邨瑚ｷｯ縲ょ燕蟾晢ｼ矩幕蜿｣遨榊・縺ｧ縺ｯ縺ｪ縺上・*譛蛾剞遞懃ｷ壹・遨榊・**縺ｧ蟶ｯ蝓溘ご繧､繝ｳ繧貞・縺吶・            //   髢句哨縺ｨ縺・≧驥上ｒ蛻･縺ｫ謖√◆縺ｪ縺・・縺ｧ縲√碁擇縺ｮ縺・■縺ｩ縺薙∪縺ｧ隕九ｋ縺九阪→縺・≧
            //   ・・騾壹ｊ隧ｦ縺励※蜈ｨ驛ｨ蛻･縺ｮ逅・罰縺ｧ螟ｱ謨励＠縺滂ｼ牙撫縺・′逋ｺ逕溘＠縺ｪ縺・・            bool btmDone = false;
            float btmBands[kNumBands] = {0, 0, 0, 0, 0, 0};
            if (useBtm_) {
                // 笘・ｨ懃ｷ壹ｒ1譛ｬ**驕ｸ縺ｰ縺ｪ縺・*縲ゅ％縺ｮ髢句哨縺ｫ螻槭☆繧狗ｨ懃ｷ壹☆縺ｹ縺ｦ縺ｮ蟇・ｸ弱ｒ隍・ｴ縺ｧ雜ｳ縺吶・                //   驕ｸ縺ｶ譁ｹ蠑上□縺ｨ縲∬ｧ貞ｺｦ縺ｫ繧医▲縺ｦ驕ｸ謚槭′蛻･縺ｮ遞懃ｷ壹∈蛻・ｊ譖ｿ繧上▲縺溽椪髢薙↓蛟､縺碁｣帙・
                //   ・亥ｮ滓ｸｬ: 謇峨・謗・ｼ輔〒 0.0433 竊・0.0154 縺ｨ髫｣謗･縺ｧ 9dB 霍ｳ縺ｭ縺滂ｼ峨・                //   蜑榊ｷ昴・蛟､縺ｧ蝮・＆繧後※縺・◆縺ｨ縺阪・逶ｮ遶九◆縺ｪ縺九▲縺溘ｂ縺ｮ縺後。TM 縺ｫ縺励※髴ｲ蜃ｺ縺励◆縲・                //   驕ｸ縺ｶ縺ｮ繧偵ｄ繧√ｌ縺ｰ縲・∈謚槭′蛻・ｊ譖ｿ繧上ｋ縺ｨ縺・≧莠玖ｱ｡閾ｪ菴薙′豸医∴繧九・                //   謌ｸ蜿｣繧偵檎ｨ懃ｷ壹・髮・粋縲阪→縺励※謇ｱ縺・・縺・BTM 譛ｬ譚･縺ｮ菴ｿ縺・婿縺ｧ繧ゅ≠繧九・                static const double hz[kNumBands] = {125, 250, 500, 1000, 2000, 4000};
                double sre[kNumBands] = {0}, sim[kNumBands] = {0};
                int nOk = 0;
                for (int k = 0; k < cl[bi].npts; ++k) {
                    btm::Edge be;
                    be.a = cl[bi].eA[k];
                    be.b = cl[bi].eB[k];
                    be.faceDir0 = cl[bi].eRefT[k];
                    be.openAngleRad = btmWedgeAngle_;   // 譌｢螳・1.5ﾏ・育ｮｱ縺ｮ蜃ｸ遞懃ｷ夲ｼ・                    if (length(be.b - be.a) < 1e-4f) continue;
                    float g1[kNumBands] = {0, 0, 0, 0, 0, 0};
                    double re1[kNumBands] = {0}, im1[kNumBands] = {0};
                    if (!btm::edgeBands(be, source, listener, hz, kNumBands, 343.0,
                                        g1, nullptr, 96, re1, im1)) continue;
                    for (int b = 0; b < kNumBands; ++b) { sre[b] += re1[b]; sim[b] += im1[b]; }
                    ++nOk;
                }
                if (nOk > 0) {
                    for (int b = 0; b < kNumBands; ++b)
                        btmBands[b] = static_cast<float>(
                            std::sqrt(sre[b] * sre[b] + sim[b] * sim[b]));
                    btmDone = true;
                }
            }
            if (haveFres) {
                // 蠎・ｸｯ蝓溘せ繧ｫ繝ｩ縺ｯ菴主沺蜉驥阪〒逡ｳ繧・亥屓謚倥′驕九・縺ｮ縺ｯ螳壻ｽ阪→霍晞屬縲√→縺・≧譁ｹ驥昴・縺溘ａ・峨・                static const float w6[kNumBands] = {3.0f, 2.5f, 2.0f, 1.3f, 1.0f, 0.8f};
                float gs = 0.0f, ws = 0.0f;
                for (int b = 0; b < kNumBands; ++b) { gs += w6[b] * fres[b]; ws += w6[b]; }
                openGainHere = (ws > 0.0f) ? gs / ws : 1.0f;
                // 笘・さ繝ｳ繝医Λ繧ｹ繝医る幕蜿｣邇・・**蠖｢**縺ｯ迚ｩ逅・°繧牙・縺ｦ縺・ｋ縺後・*驥・*縺瑚ｶｳ繧翫↑縺・                //   ・亥ｮ滓ｸｬ縺ｧ謇峨・蜈ｨ謗・ｼ輔′ 1.3dB縲ら樟螳溘・蜷域・騾城℃邇・・ 125Hz 縺ｧ 11dB, 4kHz 縺ｧ 26dB・峨・                //   蠖｢縺ｯ谿九＠縺溘∪縺ｾ蟷・□縺代ｒ髢九￥縲る幕蜿｣縺ｨ縺・≧荳闊ｬ縺ｮ驥上∈縺ｮ蜀吝ワ縺ｪ縺ｮ縺ｧ縲・                //   謇峨ｒ迚ｹ蛻･隕悶＠縺ｪ縺・らｵｶ蟇ｾ蛟､縺ｯ貍泌・縺ｧ豎ｺ繧√ｋ縲√→縺・≧譁ｹ驥昴・驕ｩ逕ｨ蜈医′縺薙％縲・                //   笘・腰縺ｫ邏ｯ荵励＠縺ｦ縺ｯ縺・￠縺ｪ縺・る幕蜿｣邇・・ 1 譛ｪ貅縺ｪ縺ｮ縺ｧ蜈ｨ菴薙′邵ｮ繧縺縺代〒縲・                //     螳滓ｸｬ縺ｧ蜈ｨ髢九′ 0.0209 竊・0.0147・磯哩縺ｨ蜷後§・峨∪縺ｧ關ｽ縺｡縺溘・                //     蝓ｺ貅・ref 繧呈ｱｺ繧√※**縺昴・蜻ｨ繧翫〒**髢九￥縺薙→縲Ｓef 縺ｧ縺ｯ蛟､縺悟､峨ｏ繧峨↑縺・・                //   笘・渕貅・ref 縺ｧ 1.0 縺ｫ縺ｪ繧九ｈ縺・ｭ｣隕丞喧縺励※縺九ｉ邏ｯ荵励☆繧九・                //     縲罫ef 縺ｮ縺ｾ繧上ｊ縺ｧ髢九￥縲榊ｽ｢・・ef*(x/ref)^p・峨ｂ隧ｦ縺励◆縺後・幕蜿｣邇・′
                //     ref 縺ｫ螻翫°縺ｪ縺・・縺ｧ蜈ｨ髢九∪縺ｧ邵ｮ縺ｿ縲∝ｮ滓ｸｬ縺ｧ 0.0209 竊・0.0152 縺ｫ縺ｪ縺｣縺溘・                //     邏騾壹＠縺ｮ荳企剞繧貞・髢九↓蜷医ｏ縺帙↑縺・→縲∝ｹ・ｒ髢九＞縺ｦ繧る浹縺悟ｰ上＆縺上↑繧九□縺代・                if (apertureContrast_ != 1.0f) {
                    const float ref = apertureContrastRef_;   // 縺薙％縺ｧ邏騾壹＠(1.0)縺ｫ縺ｪ繧矩幕蜿｣邇・                    const float x = std::max(openGainHere, 1e-4f) / std::max(ref, 1e-4f);
                    openGainHere = std::min(std::pow(x, apertureContrast_), 1.0f);
                }
            } else {
                // 繝輔Ξ繝阪Ν縺御ｽｿ縺医↑縺・→縺阪□縺代∝ｾ捺擂縺ｮ縲悟屓謚倡せ縺ｾ繧上ｊ繧堤ｨ懃ｷ・霆ｸ縺ｧ貂ｬ繧九肴婿蠑・                //   ・医％縺｡繧峨・謇峨′譁懊ａ縺縺ｨ迢ｭ遯・Κ繧貞､悶☆縲よｯ碑ｼ・畑縺ｫ谿九＠縺ｦ縺ゅｋ・峨・                slitHere = slitWidthAt(Pc, PcEdge, normalized(source - listener));
                openGainHere = slitWidthGain(slitHere);
            }

            if (scene_detail::diffDebug()) {
                std::fprintf(stderr,
                    "    [髢句哨] L(%.2f) 驥榊ｿ・%.2f,%.2f,%.2f) Pc(%.2f,%.2f,%.2f) 轤ｹ%d "
                    "minﾎｴ=%.3f fres=%.3f openGain=%.3f\n",
                    listener.x, centroid.x, centroid.y, centroid.z, Pc.x, Pc.y, Pc.z,
                    cl[bi].npts, cl[bi].minDelta, haveFres ? fres[0] : -1.0f, openGainHere);
                for (int k = 0; k < cl[bi].npts; ++k)
                    std::fprintf(stderr, "             轤ｹ%d (%.2f,%.2f,%.2f)\n",
                                 k, cl[bi].pts[k].x, cl[bi].pts[k].y, cl[bi].pts[k].z);
            }

            // 笘・谺｡蝗樊釜縲る幕蜿｣縺九ｉ髻ｳ貅舌′隕矩壹○縺ｪ縺・↑繧峨√◎縺ｮ髢句哨繧呈眠縺励＞蟋狗せ縺ｫ縺励※繧ゅ≧荳谿ｵ謗｢縺吶・            //   L 蟄励・蟒贋ｸ九ｄ縲∵焔邯壹″逕滓・縺輔ｌ縺滓峇縺後ｊ隗偵′縺薙ｌ縺ｫ蠖薙◆繧九・            //   **魑ｴ繧峨☆菴咲ｽｮ縺ｯ 1 谿ｵ逶ｮ縺ｮ髢句哨縺ｮ縺ｾ縺ｾ**・井ｺｺ縺ｯ縲梧焔蜑阪・隗偵°繧芽◇縺薙∴繧九阪→諢溘§繧九◆繧√りｨｭ險・竭･・峨・            //   邨瑚ｷｯ髟ｷ縺ｨ繧ｲ繧､繝ｳ縺縺代ｒ 2 谿ｵ逶ｮ縺ｮ縺ｶ繧灘ｻｶ髟ｷ縺吶ｋ縲・            //   1 谺｡縺ｧ螻翫￠縺ｰ謗｢邏｢縺励↑縺・・縺ｧ縲・幕縺代◆蝣ｴ謇縺ｧ縺ｯ繧ｳ繧ｹ繝医ぞ繝ｭ縲・            float pathLen2 = 0.0f, delta2 = 0.0f;
            bool  have2 = false;          // 2 谿ｵ逶ｮ縺梧・遶九＠縺溘°・域・遶区凾縺ｯ邨瑚ｷｯ髟ｷ縺ｨ ﾎｴ 繧貞ｷｮ縺玲崛縺医ｋ・・            if (secondOrder && isOccluded(Pc, source)) {
                // 笘・谿ｵ逶ｮ縺ｮ襍ｷ轤ｹ縺ｯ髢句哨縺ｮ**蜷代％縺・・**縺ｫ鄂ｮ縺上・                //   髢句哨縺ｮ驥榊ｿ・・遞懃ｷ壹ｒ縺ｪ繧峨＠縺溽せ縺ｪ縺ｮ縺ｧ縲∝｣√・謇句燕縺ｮ髱｢縺ｫ荵励ｋ縺薙→縺後≠繧九・                //   縺昴％縺九ｉ髻ｳ貅舌∈蜷代°縺・→**縺吶＄蜷後§螢√↓蜀咲ｪ∝・**縺励・谿ｵ逶ｮ縺ｮ蛟呵｣懊′蜈ｨ驛ｨ譽・唆縺輔ｌ繧・                //   ・亥ｮ滓ｸｬ縺ｧ 2 谿ｵ逶ｮ縺ｮ繧ｯ繝ｩ繧ｹ繧ｿ縺・0 譛ｬ縺ｫ縺ｪ縺｣縺ｦ縺・◆・峨・                //   蟆代＠蜈医∈騾√▲縺ｦ螢√ｒ謚懊￠縺滉ｽ咲ｽｮ縺九ｉ謗｢縺吶ゅ←繧後□縺鷹√ｌ縺ｰ謚懊￠繧九°縺ｯ螢√・蜴壹∩谺｡隨ｬ
                //   縺ｪ縺ｮ縺ｧ縲√＞縺上▽縺玖ｩｦ縺励※譛蛻昴↓邨瑚ｷｯ縺瑚ｦ九▽縺九▲縺溯ｷ晞屬繧呈治繧九・                //
                //   笘・√ｊ縺ｯ螳滉ｽ薙ｒ雋ｫ縺阪≧繧九る幕蜿｣縺ｮ閼・・螢・擇縺ｫ襍ｷ轤ｹ縺御ｹ励ｋ縺ｮ縺ｧ縲・                //     豁｣蠖薙↑ 2 谺｡蝗樊釜・亥鴻魑･驟咲ｽｮ縺ｮ謌ｸ蜿｣・峨〒繧ょ｣√・蜴壹∩縺ｶ繧薙・雋ｫ縺上・                //     縺縺九ｉ縲御ｸ蛻・ｲｫ縺上↑縲阪↓縺ｯ縺ｧ縺阪↑縺・笏笏 螳滄圀縲∵ｬ｡縺ｮ3譯医・縺ｩ繧後ｂ
                //     豁｣蠖薙↑ 2 谺｡蝗樊釜縺ｾ縺ｧ谿ｺ縺励◆:
                //       (a) 騾√ｊ縺ｮ騾比ｸｭ縺碁・阡ｽ縺輔ｌ縺ｦ縺・◆繧画｣・唆
                //       (b) 騾√ｊ繧・5cm 蛻ｻ縺ｿ縺ｾ縺ｧ邏ｰ縺九￥縺励※ (a)
                //       (c) 逶ｴ謗･邱壹ｒ蝪槭＄髫懷ｮｳ迚ｩ縺縺題ｲｫ騾夂ｦ∵ｭ｢
                //     蜉ｹ縺・◆縺ｮ縺ｯ荳九・縲檎ｴ騾壹＠縺ｪ繧芽ｲｫ騾壹ｒ險ｱ縺輔↑縺・榊愛螳夲ｼ医Ν繝ｼ繝怜・・峨・                DiffractionPath sub[4];
                int ns = 0;
                const Vec3 dirS = normalized(source - Pc);
                // 笘・√ｋ蜷代″縺ｯ髻ｳ貅先婿蜷代□縺代〒縺ｯ雜ｳ繧翫↑縺・・                //   **蜴壹＞髫懷ｮｳ迚ｩ**繧貞屓繧願ｾｼ繧蝣ｴ蜷医∬ｿ代＞邵√°繧蛾浹貅先婿蜷代∈騾√ｋ縺ｨ
                //   譚ｿ縺ｮ荳ｭ縺ｸ遯√▲霎ｼ繧縺ｮ縺ｧ縲・谿ｵ逶ｮ縺ｮ蛟呵｣懊′隕九▽縺九ｉ縺ｪ縺・                //   ・・谺｡縺ｫ縲瑚官繧呈ｨｪ蛻・▲縺溘ｉ譽・唆縲阪ｒ隱ｲ縺励◆騾皮ｫｯ縲∝字縺・｡晉ｫ九・蝗槭ｊ霎ｼ縺ｿ縺・                //     蜈ｨ驛ｨ豸医∴縺溘・縺後％繧後・谺｡縺瑚か莉｣繧上ｊ縺ｧ縺阪※縺・↑縺九▲縺滂ｼ峨・                //   蜴壹∩繧貞屓繧九↓縺ｯ**譚ｿ縺ｫ豐ｿ縺｣縺ｦ驕縺・ｸ√・蛛ｴ縺ｸ**騾√ｋ蠢・ｦ√′縺ゅｋ縲・                //   遞懃ｷ壹・蜷代″ PcEdge 縺ｨ譚ｿ縺ｮ髱｢蜀・婿蜷代・荳｡譁ｹ繧貞呵｣懊↓縺吶ｋ縲・                //   縺ｩ繧後〒謚懊￠繧峨ｌ繧九°縺ｯ蠖｢迥ｶ谺｡隨ｬ縺ｪ縺ｮ縺ｧ縲・・↓隧ｦ縺励※譛蛻昴↓騾壹▲縺溘ｂ縺ｮ繧呈治繧九・                // 笘・字縺ｿ繧呈ｸ｡繧句髄縺阪る浹貅先婿蜷・dirS 縺九ｉ**髱｢縺ｮ螟門髄縺肴・蛻・ｒ謚懊￥**縲・                //   陦晉ｫ九・荳顔ｫｯ縺ｧ縺ｯ dirS 縺梧万繧∽ｸ九ｒ蜷代￥縺ｮ縺ｧ縲√◎縺ｮ縺ｾ縺ｾ騾√ｋ縺ｨ譚ｿ縺ｮ荳ｭ縺ｸ蜈･繧・                //   ・亥ｮ滓ｸｬ: 荳顔ｫｯ y=4.02 縺九ｉ 0.25 騾√ｋ縺ｨ y=3.896 縺ｧ陦晉ｫ九・荳ｭ・峨・                //   螟門髄縺搾ｼ・cRefT・峨・謌仙・繧呈栢縺代・縲∝酔縺倬ｫ倥＆縺ｮ縺ｾ縺ｾ蜴壹∩繧呈ｸ｡繧後ｋ縲・                //   縺昴％縺九ｉ驕縺・ｸ√′隕九∴縲・ 谿ｵ逶ｮ縺梧・遶九☆繧九・                const Vec3 across = [&] {
                    Vec3 t = dirS - PcRefT * dot(dirS, PcRefT);
                    t = t - PcEdge * dot(t, PcEdge);          // 遞懃ｷ壽婿蜷代↓繧るｲ縺ｾ縺ｪ縺・                    return (length(t) > 1e-4f) ? normalized(t) : dirS;
                }();
                struct Push { Vec3 dir; float dist; };
                const Push pushes[8] = {
                    {dirS, 0.0f},  {dirS, 0.25f}, {dirS, 0.75f}, {dirS, 1.5f},
                    {across, 0.15f}, {across, 0.35f}, {across, 0.7f}, {across, 1.4f},
                };
                // 笘・梧怙蛻昴↓謌仙粥縺励◆騾√ｊ縲阪〒縺ｯ縺ｪ縺上・*邱冗ｵ瑚ｷｯ髟ｷ縺梧怙遏ｭ縺ｮ騾√ｊ**縲阪ｒ謗｡繧九・                //   譛蛻昴↓謌仙粥縺励◆繧ゅ・繧呈治繧九→縲・√ｊ 0・茨ｼ抉c 縺昴・繧ゅ・・峨′蜈磯ｭ縺ｫ縺ゅｋ縺帙＞縺ｧ
                //   繧ｴ繝溽ｵ瑚ｷｯ縺ｧ遒ｺ螳壹＠縺ｦ縺励∪縺・１c 縺ｯ閹ｨ繧峨∪縺帙◆邂ｱ縺ｮ荳翫↓荵励ｋ縺ｮ縺ｧ**螢√・謇句燕**縺ｫ
                //   蜃ｺ繧九％縺ｨ縺後≠繧奇ｼ亥ｮ滓ｸｬ z=-0.26縲∝｣√・ z竏・-0.15,0.15]・峨√◎縺薙°繧・2 谿ｵ逶ｮ縺ｮ
                //   謌ｸ蜿｣縺ｸ蠑輔＞縺溽ｷ壹′ 1 谿ｵ逶ｮ縺ｮ謌ｸ蜿｣繧呈焚 cm 螟悶＠縺ｦ螢√↓蠖薙◆繧九らｵ先棡縲・ 谿ｵ逶ｮ縺ｯ
                //   驕蝗槭ｊ縺ｮ邨瑚ｷｯ縺励°隕九▽縺代ｉ繧後↑縺・
                //     Pc(-0.89) 竊・騾√ｊ0 竊・2谿ｵ逶ｮ 8.17m  竊・蜷郁ｨ・10.92m・域ｭ｣縺励＞・・                //     Pc(-0.11) 竊・騾√ｊ0 竊・2谿ｵ逶ｮ16.57m 竊・蜷郁ｨ・19.39m・磯□蝗槭ｊ・・                //   Pc 縺ｯ謌ｸ蜿｣縺ｮ蟾ｦ蜿ｳ縺ｩ縺｡繧峨・邵√↓繧ゆｹ励ｊ縺・ｋ縺ｮ縺ｧ縲√Μ繧ｹ繝翫・縺・5cm 蜍輔＞縺溘□縺代〒
                //   10.92 竍・19.39 縺ｨ蜈･繧梧崛繧上ｊ縲∝芦譚･譁ｹ蜷代′ 70ﾂｰ 鬟帙ｓ縺ｧ縺・◆縲・                //   譛遏ｭ霍ｯ縺ｪ繧峨∵髪驟咲ｵ瑚ｷｯ縺ｨ縺励※迚ｩ逅・噪縺ｫ豁｣縺励＞縺・∴縲√Μ繧ｹ繝翫・縺ｮ蠕ｮ蟆冗ｧｻ蜍輔↓蟇ｾ縺励※
                //   騾｣邯壹↓蜍輔￥・亥・繧頑崛繧上▲縺ｦ繧る聞縺輔′霑代＞縺ｮ縺ｧ譁ｹ蜷代ｂ霑代＞・峨・                DiffractionPath bestSub{};
                float bestExtra = 0.0f, bestTotal = 1e30f;
                Vec3 bestStart = Pc;
                int usedPush = -1, pushIdx = -1;
                for (const Push& pu : pushes) {
                    ++pushIdx;
                    const float e = pu.dist;
                    const Vec3 st = Pc + pu.dir * e;
                    const int nsHere = findDiffractionPaths(st, source, sub, 4, order - 1);
                    if (nsHere <= 0) continue;
                    // 笘・√▲縺溷・縺九ｉ髻ｳ貅舌′**邏騾壹＠縺ｫ隕九∴繧・*縺ｪ繧峨√◎繧後・ 2 谿ｵ逶ｮ縺ｧ縺ｯ縺ｪ縺・                    //   縲碁幕蜿｣繧呈栢縺代◆縲阪→縺・≧縺薙→縲ゅ↑繧峨・騾√ｊ縺ｮ騾比ｸｭ縺ｫ驕ｮ阡ｽ縺檎┌縺・・縺壹〒縲・                    //   蝨ｨ繧九↑繧牙ｮ滉ｽ薙ｒ雋ｫ縺・※縺・ｋ・昴◎縺薙↓騾壹ｊ驕薙・辟｡縺・・                    //   縺薙ｌ繧定ｦ九↑縺・→髢峨§縺滓演繧偵☆繧頑栢縺代ｋ・亥ｮ滓ｸｬ 125Hz 0.551・峨・                    //   荳譁ｹ縲∵ｭ｣蠖薙↑ 2 谿ｵ逶ｮ・亥鴻魑･驟咲ｽｮ縺ｮ謌ｸ蜿｣・峨・騾√▲縺溷・縺ｧ繧る浹貅舌′隕九∴縺ｪ縺・・縺ｧ
                    //   縺薙・蛻､螳壹↓謗帙°繧峨↑縺・ょ｣√・閼・∈騾√ｋ驛ｽ蜷医〒雋ｫ騾壹・襍ｷ縺阪ｋ縺後√◎縺｡繧峨・險ｱ縺吶・                    if (e > 0.0f && !isOccluded(st, source) && isOccluded(Pc, st)) continue;
                    int k0 = 0;                      // 2谿ｵ逶ｮ縺ｯ譛繧る壹ｋ・茨ｼ晄怙遏ｭ縺ｮ・峨ｂ縺ｮ繧・譛ｬ縺縺・                    for (int k = 1; k < nsHere; ++k)
                        if (maekawa::apertureWeight(sub[k].pathLength) >
                            maekawa::apertureWeight(sub[k0].pathLength)) k0 = k;
                    const float total = e + sub[k0].pathLength;
                    if (total < bestTotal) {
                        bestTotal = total; bestExtra = e; bestSub = sub[k0];
                        bestStart = st; usedPush = pushIdx;
                    }
                }
                if (usedPush < 0) continue;         // 縺昴・髢句哨縺ｮ蜈医・陦後″豁｢縺ｾ繧翫らｵ瑚ｷｯ縺ｨ縺励※謗｡繧峨↑縺・                const Vec3 start = bestStart;
                sub[0] = bestSub;
                const int bs = 0;
                (void)bestExtra; (void)ns;
                if (scene_detail::diffDebug())
                    std::fprintf(stderr,
                        "    [2谺｡] L(%.2f) Pc(%.2f,%.2f,%.2f) push#%d start(%.2f,%.2f,%.2f) "
                        "ns=%d sub[%d].len=%.2f 蜷郁ｨ・%.2f\n",
                        listener.x, Pc.x, Pc.y, Pc.z, usedPush,
                        start.x, start.y, start.z, ns, bs, sub[bs].pathLength,
                        length(Pc - listener) + length(start - Pc) + sub[bs].pathLength);
                // 笘・ 谿ｵ逶ｮ縺ｯ**蟷ｾ菴輔□縺・*繧呈戟縺｡蟶ｰ繧具ｼ医←縺薙°繧芽◇縺薙∴繧九°繝ｻ縺ｩ繧後□縺鷹□縺・°・峨・                //   繧ｲ繧､繝ｳ縺ｯ荳九・ 1 谺｡縺ｨ**蜷後§蠑・*縺ｧ菴懊ｋ縲ゆｻ･蜑阪・縺薙％縺ｧ
                //     蜑榊ｷ・ﾎｴ1) ﾃ・sub.gain(= 蜑榊ｷ・ﾎｴ2)ﾃ鈴幕蜿｣邇・) ﾃ・髢句哨邇・
                //   縺ｨ 4 縺､謗帙￠縺ｦ縺・◆縺後・幕蜿｣邇・・ apertureFresnelBands 縺・                //   **繝ｪ繧ｹ繝翫・隕也せ縺ｧ蜈ｨ繧､繝ｳ繧ｹ繧ｿ繝ｳ繧ｹ縺ｮ蠖ｱ繧・1 譫壹・髱｢縺ｸ謚募ｽｱ**縺励◆驥上↑縺ｮ縺ｧ
                //   ・井ｸ翫・謚募ｽｱ繝ｫ繝ｼ繝怜盾辣ｧ・峨・ 蝗槭・遨榊・縺ｫ 1 谿ｵ逶ｮ繝ｻ2 谿ｵ逶ｮ縺ｮ驕ｮ阡ｽ縺梧里縺ｫ荳｡譁ｹ蜈･縺｣縺ｦ縺・ｋ縲・                //   縺､縺ｾ繧雁酔縺倬㍼繧剃ｺ碁㍾縺ｫ謇輔＞縲√＆繧峨↓蜑榊ｷ昴∪縺ｧ莠碁㍾縺ｫ謇輔▲縺ｦ縺・◆縲・                //   螳溷ｮｳ縺ｯ 1谺｡竍・谺｡ 縺ｮ蠅・阜縺ｫ蜃ｺ繧九ょ鴻魑･驟咲ｽｮ縺ｮ謌ｸ蜿｣繧呈万繧√↓隕励￥縺ｨ縲・                //   縲・ 譫夂岼縺ｮ邵√′繝ｪ繧ｹ繝翫・縺九ｉ逶ｴ謗･隕九∴繧九°縲阪→縺・≧**莠悟､蛻､螳・*縺ｧ謇ｱ縺・′蜈･繧梧崛繧上ｊ縲・                //   蜷後§迚ｩ逅・ｵ瑚ｷｯ縺ｪ縺ｮ縺ｫ蛟､縺碁｣帙・・亥ｮ滓ｸｬ: x=-1.60 縺ｧ 0.0893 竊・x=-1.55 縺ｧ 0.0252縲・.5蛟搾ｼ峨・                //   蜷郁ｨ・ﾎｴ 縺ｧ蜑榊ｷ昴ｒ 1 蝗槭・髢句哨邇・ｒ 1 蝗槭↓縺吶ｌ縺ｰ縲∝｢・阜縺ｧ縺ｯ ﾎｴ1竊・ 縺ｪ縺ｮ縺ｧ
                //   蜷郁ｨ・ﾎｴ 縺・1 谺｡縺ｮ ﾎｴ 縺ｫ荳閾ｴ縺励・*荳｡蛛ｴ縺悟酔縺伜､**縺ｫ縺ｪ縺｣縺ｦ谿ｵ蟾ｮ縺梧ｶ医∴繧九・                pathLen2 = length(centroid - listener) + length(start - Pc) + sub[bs].pathLength;
                delta2   = cl[bi].minDelta + std::max(sub[bs].delta, 0.0f);
                have2    = true;
            }

            // 笘・*螳壻ｽ阪・驥榊ｿ・∝ｹｾ菴輔・繧ｹ繝翫ャ繝礼せ**縺ｨ蠖ｹ蜑ｲ繧貞・縺代ｋ縲・            //   Pc 縺ｯ縲碁㍾蠢・↓縺・■縺ｰ繧楢ｿ代＞螳溷惠縺ｮ蛟呵｣懃せ縲阪□縺後∵虻蜿｣縺ｮ蟾ｦ蜿ｳ縺ｮ遞懃ｷ壹・
            //   縺ｩ縺｡繧峨ｂ蛟呵｣懊↑縺ｮ縺ｧ縲√Μ繧ｹ繝翫・縺悟虚縺上→縺ｩ縺｡繧峨′霑代＞縺九′蜈･繧梧崛繧上ｊ縲・            //   髢句哨轤ｹ縺梧虻蜿｣縺ｮ蟷・・繧馴｣帙・・亥ｮ滓ｸｬ: 5cm 蜍輔＞縺溘□縺代〒 x=-0.14 竍・-0.89縲・            //   蛻ｰ譚･譁ｹ蜷代′ 15ﾂｰ 謖ｯ繧後◆・峨ゆｺｺ縺瑚◇縺上・縺ｯ譁ｹ蜷代↑縺ｮ縺ｧ縲√％縺薙・鬟帙・縺帙↑縺・・            //   驥榊ｿ・・蛟呵｣懊・驥阪∩莉倥″蟷ｳ蝮・↑縺ｮ縺ｧ**騾｣邯壹↓蜍輔￥**縲・m 蟷・・謌ｸ蜿｣縺ｪ繧・            //   縺昴・逵溘ｓ荳ｭ繧呈欠縺・笏笏 縲梧虻蜿｣縺九ｉ閨槭％縺医ｋ縲阪→縺励※豁｣縺励＞縲・            //   荳譁ｹ Pc・亥ｮ溷惠縺ｮ轤ｹ・峨・關ｽ縺ｨ縺帙↑縺・・ 谺｡縺ｮ襍ｷ轤ｹ繧・ｹ・・貂ｬ螳壹・螳滉ｽ薙・荳翫↓
            //   荵励▲縺ｦ縺・ｋ蠢・ｦ√′縺ゅｊ縲・㍾蠢・・譚ｿ縺ｮ荳ｭ繧・髄縺薙≧蛛ｴ縺ｸ蜃ｺ繧九％縺ｨ縺後≠繧・            //   ・磯哩謇峨〒髢句哨蟷・3.03m 繧呈ｸｬ縺｣縺ｦ縺励∪縺｣縺滉ｻｶ・峨ゅ□縺九ｉ蟷ｾ菴輔・ Pc 縺ｮ縺ｾ縺ｾ縲・            out[n].aperture = centroid;
            // 髢句哨縺ｮ蠎・′繧翫ｒ陦ｨ縺咏せ繧呈鏡縺・ｼ医・繧､繝倥Φ繧ｹ縲・縺､縺縺ｨ謌ｸ蜿｣縺檎せ縺ｫ閨槭％縺医ｋ・峨・            //   Pc 縺ｫ蜉縺医※縲訓c 縺九ｉ譛繧る□縺・呵｣懊阪→縲後◎縺ｮ荳｡閠・°繧画怙繧る□縺・呵｣懊阪ｒ謗｡繧翫・            //   髢句哨縺ｮ遶ｯ縺ｾ縺ｧ蠑ｵ繧九ｈ縺・↓縺吶ｋ縲ょ呵｣懊・蜈ｨ驛ｨ**螳溷惠縺ｮ遞懃ｷ壻ｸ翫・轤ｹ**縺ｪ縺ｮ縺ｧ縲・            //   髢句哨縺ｮ螟悶∈縺ｯ縺ｿ蜃ｺ縺吶％縺ｨ縺ｯ縺ｪ縺・・            out[n].spread[0] = Pc;
            out[n].nSpread = 1;
            for (int pass = 0; pass < 2 && out[n].nSpread < DiffractionPath::kMaxSpread; ++pass) {
                int far = -1; float farD = 0.0f;
                for (int k = 0; k < cl[bi].npts; ++k) {
                    float d = 1e30f;
                    for (int q = 0; q < out[n].nSpread; ++q)
                        d = std::min(d, length(cl[bi].pts[k] - out[n].spread[q]));
                    if (d > farD) { farD = d; far = k; }
                }
                if (far < 0 || farD < 0.05f) break;   // 5cm 譛ｪ貅縺励°髮｢繧後※縺・↑縺・↑繧画淵繧峨☆諢丞袖縺後↑縺・                out[n].spread[out[n].nSpread++] = cl[bi].pts[far];
            }
            // 2 谿ｵ逶ｮ縺梧・遶九＠縺溘↑繧峨∫ｵ瑚ｷｯ髟ｷ縺ｨ ﾎｴ 縺縺大ｷｮ縺玲崛縺医ｋ・医ご繧､繝ｳ縺ｮ菴懊ｊ譁ｹ縺ｯ 1 谺｡縺ｨ蜷後§・峨・            //   邨瑚ｷｯ髟ｷ縺ｯ listener竊単c竊・騾√▲縺溷・)竊・谿ｵ逶ｮ 縺ｮ邱丞柱縲る√▲縺溷・繧定ｶｳ縺怜ｿ倥ｌ繧九→
            //   邨瑚ｷｯ髟ｷ縺檎峩邱夊ｷ晞屬繧医ｊ遏ｭ縺上↑繧翫∃ｴ 縺・0 縺ｫ繧ｯ繝ｩ繝ｳ繝励＆繧後※蜈ｨ蟶ｯ蝓溘′
            //   蠅・阜蛟､(0.562)縺ｫ蠑ｵ繧贋ｻ倥￥・亥ｮ滓ｸｬ縺ｧ縺昴≧縺ｪ縺｣縺滂ｼ峨・            // 笘・ｵ瑚ｷｯ髟ｷ繧・*驥榊ｿ・*縺ｧ貂ｬ繧九ょｮ壻ｽ阪ｒ驥榊ｿ・↓縺励◆縺ｮ縺ｨ蜷後§逅・罰縲・            //   Pc・磯㍾蠢・↓縺・■縺ｰ繧楢ｿ代＞螳溷惠縺ｮ蛟呵｣懃せ・峨〒貂ｬ繧九→縲・幕蜿｣縺ｮ蟾ｦ蜿ｳ縺ｮ邵√′
            //   荳｡譁ｹ蛟呵｣懊↓縺ｪ縺｣縺ｦ縺・ｋ縺ｨ縺肴髪驟咲せ縺後◎縺ｮ髢薙〒蜈･繧梧崛繧上ｊ縲∫ｵ瑚ｷｯ髟ｷ縺碁｣帙・
            //   ・亥ｮ滓ｸｬ: 髢句哨縺ｮ邵√ｒ荳｡譁ｹ諡ｾ縺医ｋ繧医≧縺ｫ縺励◆騾皮ｫｯ 3.94m 縺ｮ霍ｳ縺ｳ縺悟・縺滂ｼ峨・            //   邨瑚ｷｯ髟ｷ縺ｯ驕・ｻｶ縺ｨ霍晞屬貂幄｡ｰ縺ｮ荳｡譁ｹ繧呈ｱｺ繧√ｋ縺ｮ縺ｧ縲・｣帙∋縺ｰ縺昴・縺ｾ縺ｾ髻ｳ縺ｮ鬟帙・縺ｫ縺ｪ繧九・            //   驥榊ｿ・・髢句哨縺ｮ逵溘ｓ荳ｭ縺ｪ縺ｮ縺ｧ縲碁幕蜿｣繧帝壹▲縺溯ｷ晞屬縲阪→縺励※繧らｴ逶ｴ縲・            out[n].pathLength = have2 ? pathLen2
                                      : length(centroid - listener) + length(source - centroid);
            out[n].delta = have2 ? delta2 : cl[bi].minDelta;
            maekawa::gainBands(out[n].delta, out[n].gain);
            // 髢句哨縺後←繧後□縺鷹幕縺・※縺・ｋ縺九〒謚代∴繧九・*蝗樊釜縺ｮ髻ｳ驥上・縺薙％縺梧ｱｺ繧√ｋ**縲・            //   蜑榊ｷ昴・蠑上・ ﾎｴ 縺励°隕九↑縺・・縺ｧ縲√％繧後′辟｡縺・→謇峨ｒ髢九￠縺ｦ繧る浹縺悟､峨ｏ繧峨↑縺・            //   ・亥ｮ滓ｸｬ: Test_Full 縺ｮ蠖｢迥ｶ縺ｧ 0ﾂｰ竊・0ﾂｰ 謖ｯ縺｣縺ｦ繧らｷ城㍼ 1.000 縺ｮ縺ｾ縺ｾ縺縺｣縺滂ｼ峨・            //   遞懃ｷ壽爾邏｢縺悟ｹｻ縺ｮ邨瑚ｷｯ繧定ｿ斐＠縺ｦ繧ゅ・幕蜿｣邇・′ 0 縺ｪ繧峨％縺薙〒 0 縺ｫ縺ｪ繧九・            out[n].openGain = openGainHere;
            out[n].slitWidth = slitHere;
            // 髫咎俣縺ｮ蟷・↓繧医ｋ**蟶ｯ蝓溘＃縺ｨ**縺ｮ騾壹ｊ繧・☆縺輔ら強縺・⊇縺ｩ鬮伜沺縺縺代′騾壹ｋ縲・            //   縲碁幕縺上→髻ｳ濶ｲ縺碁幕縺上阪・縺薙％縺ｧ菴懊ｉ繧後ｋ・・litWidthBandGain 縺ｮ繧ｳ繝｡繝ｳ繝亥盾辣ｧ・峨・            if (haveFres) for (int b = 0; b < kNumBands; ++b) out[n].openBand[b] = fres[b];
            else slitWidthBandGain(slitHere, out[n].openBand);
            if (apertureAsPortal_) {
                // 笘・・ 隕九▽縺代◆遞懃ｷ夂せ繧偵碁幕蜿｣縲阪→縺励※**繝昴・繧ｿ繝ｫ縺ｨ蜷後§蠑上〒**隧穂ｾ｡縺吶ｋ 笘・・
                //
                //   遞懃ｷ壽爾邏｢縺ｯ縲後←縺薙↓髢句哨縺後≠繧九°縲阪ｒ隕九▽縺代ｋ蠖ｹ縺縺代↓縺励※縲・ｳｴ繧峨☆驥上・
                //   繝昴・繧ｿ繝ｫ邨瑚ｷｯ縺ｨ蜷後§繧ゅ・繧剃ｽｿ縺・ゅ・繝ｼ繧ｿ繝ｫ繧堤ｽｮ縺擾ｼ・uthoring・峨・縺ｧ縺ｯ縺ｪ縺上・                //   謗｢邏｢縺ｮ邨先棡繧偵・繝ｼ繧ｿ繝ｫ縺ｨ縺励※**隱ｭ縺ｿ譖ｿ縺医ｋ**縲・                //
                //   繝昴・繧ｿ繝ｫ縺ｮ蠑・= 髢句哨邇・ﾃ・cos(繝ｪ繧ｹ繝翫・蛛ｴ) ﾃ・cos(髻ｳ貅仙・) ﾃ・霑ょ屓縺ｮ蠎・′繧頑錐螟ｱ縲・                //   縺薙・縺・■**髢句哨邇・・譛螟ｧ(1.0)縺ｫ蝗ｺ螳・*縺吶ｋ 笏笏 遞懃ｷ壹′蛟呵｣懊→縺励※騾壹▲縺・                //   譎らせ縺ｧ縲後◎縺薙・騾壹ｌ繧九阪→蛻・°縺｣縺ｦ縺・ｋ縺ｮ縺ｧ縲∵隼繧√※髱｢遨阪ｒ貂ｬ繧顔峩縺輔↑縺・・                //   谿九ｋ3縺､縺ｯ蜈ｨ驛ｨ**騾｣邯夐㍼**縺ｪ縺ｮ縺ｧ縲∫ｨ懃ｷ壽爾邏｢逕ｱ譚･縺ｮ荳埼｣邯壹′繧ｲ繧､繝ｳ縺ｫ蜃ｺ縺ｪ縺・・                //   ・亥燕蟾昴・ ﾎｴ 貂幄｡ｰ繧る幕蜿｣遨榊・繧よ寺縺代↑縺・ゅ←縺｡繧峨ｂ蜷後§謳榊､ｱ縺ｮ蛻･陦ｨ迴ｾ縺ｪ縺ｮ縺ｧ縲・                //     繝昴・繧ｿ繝ｫ邨瑚ｷｯ縺ｨ謠・∴繧具ｼ昜ｺ碁㍾縺ｫ謇輔ｏ縺ｪ縺・√→縺・≧縺薙ｌ縺ｾ縺ｧ縺ｮ譁ｹ驥昴・邯壹″・・                //
                //   蠢・ｦ√↑縺ｮ縺ｯ髢句哨縺ｮ荳ｭ蠢・→髱｢縺ｮ豕慕ｷ壹□縺代〒縲√←縺｡繧峨ｂ譌｢縺ｫ謖√▲縺ｦ縺・ｋ
                //   ・井ｸｭ蠢・= 髢句哨荳ｭ蠢・→繝ｪ繧ｹ繝翫・蛛ｴ轤ｹ縺ｮ荳ｭ髢薙∵ｳ慕ｷ・= 遞懃ｷ壹′荵励ｋ邂ｱ縺ｮ譛繧り埋縺・ｻｸ・峨・                Vec3 nrm(0, 0, 1);
                if (cl[bi].inst >= 0 && cl[bi].inst < instanceCount()) {
                    const Obb& ob = instances_[static_cast<std::size_t>(cl[bi].inst)].obb;
                    const float hx = ob.halfExtents.x, hy2 = ob.halfExtents.y, hz = ob.halfExtents.z;
                    Vec3 t2 = ob.axisZ;
                    if (hx <= hy2 && hx <= hz) t2 = ob.axisX;
                    else if (hy2 <= hx && hy2 <= hz) t2 = ob.axisY;
                    if (length(t2) > 1e-4f) nrm = normalized(t2);
                }
                // 笘・*謖・髄諤ｧ(cos)繧りｿょ屓縺ｮ蠎・′繧頑錐螟ｱ繧よ寺縺代↑縺・りｷ晞屬縺ｨ蠎・＆縺縺・*縺ｫ縺吶ｋ縲・                //
                //   霑ょ屓縺ｮ謳榊､ｱ detour = 逶ｴ邱夊ｷ晞屬/(髢句哨縺ｾ縺ｧ縺ｮ霍晞屬+髢句哨縺九ｉ髻ｳ貅舌∪縺ｧ縺ｮ霍晞屬) 縺ｯ
                //   縲檎ｵ瑚ｷｯ縺御ｼｸ縺ｳ縺溘・繧灘ｼｱ縺上↑繧九阪→縺・≧驥上□縺後√・繧ｹ繝亥・縺ｯ譌｢縺ｫ**邨瑚ｷｯ髟ｷ縺昴・繧ゅ・**縺ｧ
                //   霍晞屬貂幄｡ｰ繧呈寺縺代※縺・ｋ・・cousticFlowSceneDemo 縺ｮ DistAtten(pathLen)・峨・                //   荳｡譁ｹ謗帙￠繧九→邨瑚ｷｯ髟ｷ繧・*莠碁㍾縺ｫ謇輔≧**縲ゅ％縺薙〒縺ｯ謗帙￠縺ｪ縺・・                //
                //   cos 縺ｮ謖・髄諤ｧ繧ょ､悶☆縲ら黄逅・→縺励※縺ｯ豁｣縺励＞縺後・幕蜿｣縺ｮ豁｣髱｢縺九ｉ螟悶ｌ繧九⊇縺ｩ
                //   證励￥縺ｪ繧九・縺ｧ縲∵ｨｪ縺ｫ螟悶ｌ縺滉ｽ咲ｽｮ縺ｧ縺ｮ縲梧ｰ鈴・縲阪′豸医∴繧・                //   ・亥ｮ滓ｸｬ: 蝗樊釜繧ｷ繝ｼ繝ｳ縺ｮ謗・ｼ輔′ 16dB 竊・41dB 縺ｫ蠎・′縺｣縺滂ｼ峨・                //   蝗樊釜縺碁°縺ｶ諠・ｱ縺ｯ縲弱←縺薙°繧画栢縺代※縺上ｋ縺九上→縲弱←繧後□縺鷹□縺・°縲上↓邨槭ｋ縲・                //   縺ｨ縺・≧ diffractionDistanceOnly 縺ｮ譁ｹ驥昴ｒ縺薙％縺ｧ繧る壹☆縲・                //   谿九ｋ縺ｮ縺ｯ髢句哨縺ｮ蠎・＆・育強縺・囮髢薙・騾壹ｉ縺ｪ縺・ｼ峨□縺代・                // 笘・幕蜿｣縺ｮ**蠎・＆**縺ｯ縲・幕蜿｣轤ｹ縺九ｉ髱｢蜀・∈繝ｬ繧､繧帝｣帙・縺励※貂ｬ繧九・                //   縲碁幕蜿｣邇・怙螟ｧ縺ｫ蝗ｺ螳壹阪□縺ｨ遯薙ｂ騾夊ｷｯ繧ょ酔縺倬浹驥上↓縺ｪ縺｣縺ｦ縺励∪縺・・縺ｧ縲・                //   蠎・＆縺縺代・縺薙％縺ｧ謌ｻ縺吶ゅヵ繝ｬ繝阪Ν遨榊・・磯擇縺ｸ蠖ｱ繧呈兜蠖ｱ縺励※遨阪・・峨ｈ繧・                //   縺壹▲縺ｨ霆ｽ縺上√＠縺九ｂ**騾｣邯夐㍼**縺ｪ縺ｮ縺ｧ謗｢邏｢逕ｱ譚･縺ｮ鬟帙・繧呈戟縺｡霎ｼ縺ｾ縺ｪ縺・・                //   笘・ｸｦ讓ｪ縺ｮ2霆ｸ縺縺代〒縺ｪ縺・*蟇ｾ隗偵ｂ**貂ｬ繧九よ万繧√↓迢ｭ縺・幕蜿｣繧堤ｸｦ讓ｪ縺縺代〒
                //     隕九ｋ縺ｨ邏騾壹＠縺ｫ隕九∴縺ｦ縺励∪縺・ょｯｾ縺ｫ縺ｪ繧句髄縺阪・霍晞屬繧定ｶｳ縺励※蟾ｮ縺玲ｸ｡縺励↓縺励・                //     縺・■縺ｰ繧鍋強縺・ｂ縺ｮ繧呈治繧具ｼ医ム繧ｯ繝医・譛繧ら強縺・妙髱｢縺ｧ豎ｺ縺ｾ繧具ｼ峨・                //   笘・°縺､縺ｦ slitWidthAt 縺御ｸｻ邨瑚ｷｯ縺九ｉ髯阪ｍ縺輔ｌ縺溘・縺ｯ縲梧演縺梧万繧√□縺ｨ
                //     迢ｭ遯・Κ縺檎ｸ√・轤ｹ縺九ｉ螟悶ｌ繧九阪◆繧√よ演縺ｯ繝昴・繧ｿ繝ｫ諡・ｽ薙↓縺ｪ縺｣縺溘・縺ｧ縲・                //     蝗ｺ螳壹・髢句哨・育ｪ薙・騾夊ｷｯ繝ｻ謌ｸ蜿｣・峨↑繧臥強遯・Κ縺ｯ髢句哨縺昴・繧ゅ・縺ｫ縺ゅｋ縲・                float span = kApertureMaxSpan * 2.0f;
                {
                    Vec3 u2 = cross(nrm, Vec3(0, 1, 0));
                    if (length(u2) < 1e-3f) u2 = cross(nrm, Vec3(1, 0, 0));
                    u2 = normalized(u2);
                    const Vec3 v2 = normalized(cross(nrm, u2));
                    auto freeD = [&](const Vec3& dd) {
                        const SceneHit h = raycastClosest(centroid, dd, kApertureMaxSpan);
                        return h.hit ? h.t : kApertureMaxSpan;
                    };
                    for (int a = 0; a < 4; ++a) {          // 0/45/90/135ﾂｰ・亥ｯｾ隗定ｾｼ縺ｿ・・                        const float th = scene_detail::kPiF * a * 0.25f;
                        const Vec3 d2 = normalized(u2 * std::cos(th) + v2 * std::sin(th));
                        span = std::min(span,
                                        freeD(d2) + freeD(Vec3(-d2.x, -d2.y, -d2.z)));
                    }
                }
                const float k = slitWidthGain(span);   // 蠎・＆縺縺代りｷ晞屬縺ｯ繝帙せ繝医′邨瑚ｷｯ髟ｷ縺ｧ謇輔≧
                if (scene_detail::diffDebug())
                    std::fprintf(stderr, "    [蟷・ L(%.2f) 荳ｭ蠢・%.2f,%.2f,%.2f) span=%.3f k=%.4f\n",
                                 listener.x, centroid.x, centroid.y, centroid.z, span, k);
                out[n].openGain = k;
                out[n].slitWidth = span;
                for (int b = 0; b < kNumBands; ++b) {
                    out[n].gain[b] = k;          // 蟶ｯ蝓溘〒濶ｲ縺ｯ莉倥￠縺ｪ縺・ｼ・PF 縺ｯ騾城℃髻ｳ縺梧球蠖難ｼ・                    out[n].openBand[b] = 1.0f;
                }
            } else if (btmDone && !have2) {
                // 笘・TM 縺ｯ縲悟燕蟾昴・ ﾎｴ 貂幄｡ｰ ﾃ・髢句哨邇・阪ｒ**縺ｾ縺ｨ繧√※鄂ｮ縺肴鋤縺医ｋ**縲・                //   荳｡譁ｹ謗帙￠繧九→莠碁㍾縺ｫ縺ｪ繧具ｼ医◎縺薙′莉翫∪縺ｧ縺ｮ隧ｰ縺ｾ繧翫□縺｣縺滂ｼ峨・                //   2 谿ｵ逶ｮ縺檎ｵ｡繧縺ｨ縺阪・菴ｿ繧上↑縺・笏笏 btmBands 縺ｯ 1 谿ｵ逶ｮ縺ｮ遞懃ｷ壹□縺代ｒ
                //   遨榊・縺励◆蛟､縺ｪ縺ｮ縺ｧ縲・ 谿ｵ逶ｮ縺ｶ繧薙′謚懊￠關ｽ縺｡繧具ｼ亥ｾ捺擂繧・2 谺｡縺ｯ BTM 繧帝壹＠縺ｦ縺・↑縺・ｼ峨・                for (int b = 0; b < kNumBands; ++b) {
                    out[n].gain[b] = btmBands[b];
                    out[n].openBand[b] = 1.0f;
                }
                out[n].openGain = 1.0f;
            } else if (haveFres && apertureIsTransmission_) {
                // 笘・幕蜿｣繧帝壹ｋ蛻・・**蜑榊ｷ昴・ ﾎｴ 貂幄｡ｰ繧呈鴛繧上↑縺・*縲・                //   f・医ヵ繝ｬ繝阪Ν/蠖ｱ縺ｮ遨榊・・峨・繧ｭ繝ｫ繝偵・繝・ヵ蛛ｴ縺ｮ驥上〒縲・□蝗槭ｊ縺ｮ蜉ｹ譫懊・
                //   縺昴・荳ｭ縺ｮ驥阪∩縺ｫ譌｢縺ｫ蜈･縺｣縺ｦ縺・ｋ縲ゅ％縺薙∈蜑榊ｷ昴ｒ謗帙￠繧九→莠碁㍾縺ｫ縺ｪ繧・                //   ・亥ｮ滓ｸｬ: 莠碁㍾謗帙￠繧貞､悶☆縺ｨ 1kHz 縺ｧ 4蛟榊ｷｮ縺悟・縺滂ｼ峨・                //   蜷域・騾城℃邇・・蜑ｲ蜷亥ｽ｢ ﾏЮeff = ﾏЮ螢・1竏断) + f 縺ｮ縲・*f 縺ｮ鬆・*縺後％繧後・                //   髢句哨繧帝壹ｋ謌仙・縺ｯ髢句哨縺ｮ譁ｹ蜷代°繧牙ｱ翫￥縺ｮ縺ｧ縲√％縺ｮ繧ｿ繝・・縺ｸ蜈･繧後ｋ縲・                //   髻ｳ貅先婿蜷代・逶ｴ謗･髻ｳ縺ｸ雜ｳ縺吶→螳壻ｽ阪′螢翫ｌ繧九・                for (int b = 0; b < kNumBands; ++b)
                    out[n].gain[b] = fres[b] * apertureContrastMap(fres[b]);
            } else {
                for (int b = 0; b < kNumBands; ++b) out[n].gain[b] *= out[n].openGain;
            }
            ++n;
        }
        return n;
    }

    // 縲仙屓謚倥ｒ莠梧ｬ｡髻ｳ貅舌→縺励※魑ｴ繧峨☆・・TD/繝帙う繝倥Φ繧ｹ・峨鷹・阡ｽ譎ゅ∝屓繧願ｾｼ縺ｿ繧ｨ繝・ず繧偵後お繝・ず・昜ｺ梧ｬ｡髻ｳ貅舌阪→縺励※
    // 譛螟ｧ maxN 蛟九・莉ｮ諠ｳ髻ｳ貅撰ｼ域婿蜷代▽縺搾ｼ峨↓譚溘・縺ｦ霑斐☆縲りｿ代＞譁ｹ蜷代・繧ｨ繝・ず縺ｯ繧ｯ繝ｩ繧ｹ繧ｿ邨ｱ蜷医☆繧九・縺ｧ縲∽ｸ｡蛛ｴ縺ｫ
    // 髢句哨縺後≠繧後・蟾ｦ蜿ｳ2髻ｳ貅絶ｦ縺ｮ繧医≧縺ｫ蛻・°繧後√Μ繧ｹ繝翫・遘ｻ蜍輔〒蜷・ご繧､繝ｳ縺梧ｻ代ｉ縺九↓螟峨ｏ繧具ｼ茨ｼ・轤ｹ蜷域・縺ｮ鬟帙・繧呈賜髯､・峨・    //   outPos[k]  : 莠梧ｬ｡髻ｳ貅舌・繝ｯ繝ｼ繝ｫ繝我ｽ咲ｽｮ・・ listener + 譁ｹ蜷・ﾃ・髻ｳ貅占ｷ晞屬縲８wise 縺後％縺ｮ譁ｹ蜷代∈螳壻ｽ搾ｼ・    //   outGain[k] : 逶ｸ蟇ｾ繧ｲ繧､繝ｳ・亥・繧ｯ繝ｩ繧ｹ繧ｿ蜷郁ｨ医〒豁｣隕丞喧, 遏ｭ縺・ｿょ屓縺ｻ縺ｩ螟ｧ・峨らｷ丞柱 竕､ 1
    // 謌ｻ繧雁､ = 譖ｸ縺崎ｾｼ繧薙□髻ｳ貅先焚縲る・阡ｽ縺ｪ縺・霑ょ屓縺ｪ縺励・ 0縲・    //   窶ｻ findDiffractionPaths 縺ｸ縺ｮ阮・＞繝ｩ繝・ヱ縲よ爾邏｢縺ｯ 1 邂・園縺ｫ荳譛ｬ蛹悶＠縺ｦ縺ゅｋ縲・    //     隕矩壹＠譎ゅ↓霑斐ｋ縲檎峩謗･邨瑚ｷｯ縺ｸ縺ｮ陬懈ｭ｣縲阪・莠梧ｬ｡髻ｳ貅舌〒縺ｯ縺ｪ縺・・縺ｧ縲√％縺薙〒縺ｯ 0 譛ｬ縺ｫ縺吶ｋ
    //     ・域婿蜷代・髻ｳ貅舌◎縺ｮ繧ゅ・縺ｪ縺ｮ縺ｧ縲∝他縺ｳ蜃ｺ縺怜・縺ｯ逶ｴ謗･髻ｳ縺ｨ縺励※魑ｴ繧峨○縺ｰ繧医＞・峨・    /// 荳翫→蜷後§縺縺後・幕蜿｣縺斐→縺ｫ**6蟶ｯ蝓溘・繧ｲ繧､繝ｳ**繧定ｿ斐☆・・utBand 縺ｯ maxN*6 隕∫ｴ・峨・    ///   髫咎俣縺檎強縺・⊇縺ｩ鬮伜沺縺縺代′騾壹ｋ縺ｮ縺ｧ縲・幕縺榊・蜷医′髻ｳ濶ｲ縺ｫ蜃ｺ繧九・    ///   outBand 縺・null 縺ｪ繧・computeDiffractionSources 縺ｨ蜷後§縲・    int computeDiffractionSourceBands(const Vec3& listener, const Vec3& source,
                                      Vec3* outPos, float* outGain, float* outBand,
                                      int maxN) const {
        return computeDiffractionSources(listener, source, outPos, outGain, maxN, outBand);
    }

    int computeDiffractionSources(const Vec3& listener, const Vec3& source,
                                  Vec3* outPos, float* outGain, int maxN,
                                  float* outBand = nullptr) const {
        if (!outPos || !outGain || maxN <= 0) return 0;
        if (!isOccluded(listener, source)) return 0;

        constexpr int kMaxOut = 24;
        DiffractionPath paths[kMaxOut];
        const int np = findDiffractionPaths(listener, source, paths, std::min(maxN, kMaxOut));
        if (np <= 0) return 0;

        // 菴主沺蜉驥阪〒 6 蟶ｯ蝓溘ｒ 1 繧ｹ繧ｫ繝ｩ縺ｸ・亥屓謚倥・菴主沺縺悟屓繧願ｾｼ繧縺ｮ縺ｧ菴主沺繧帝㍾縺剰ｦ九ｋ・峨・        auto bbGain = [](const float g[kNumBands]) {
            const float w[kNumBands] = {3.0f, 2.5f, 2.0f, 1.3f, 1.0f, 0.8f};
            float gs = 0.0f, ws = 0.0f;
            for (int b = 0; b < kNumBands; ++b) { gs += w[b] * g[b]; ws += w[b]; }
            return ws > 0.0f ? gs / ws : 0.0f;
        };
        // 驥阪∩縺ｯ鬆ｭ謇薙■縺励↑縺・燕蟾晏､・亥ｮ壻ｽ阪ｒ縺ｼ繧・￠縺輔○縺ｪ縺・◆繧√ＧindDiffractionPaths 縺ｨ蜷後§逅・罰・峨・        const float directDist = std::max(length(source - listener), 1e-4f);
        float sum = 0.0f;
        for (int i = 0; i < np; ++i) sum += maekawa::apertureWeight(paths[i].pathLength - directDist);
        int n = 0;
        for (int i = 0; i < np && n < maxN; ++i) {
            const float w = maekawa::apertureWeight(paths[i].pathLength - directDist);
            // 笘・ｿ斐☆縺ｮ縺ｯ**邨ｶ蟇ｾ驥・*縲ゅ碁・蛻・ｯ・ﾃ・縺昴・髢句哨繧偵←繧後□縺鷹壹ｋ縺九阪・            //   莉･蜑阪・驟榊・豈斐□縺代ｒ霑斐＠縺ｦ縺・◆縲る・蛻・ｯ斐・蜷郁ｨ・1 縺ｫ豁｣隕丞喧縺輔ｌ繧九・縺ｧ縲・幕蜿｣縺・            //   1 縺､縺ｪ繧牙ｸｸ縺ｫ 1.0 笏笏 **謇峨ｒ髢九￠縺ｦ繧る浹驥上′螟峨ｏ繧峨↑縺九▲縺・*・亥ｮ滓ｸｬ: 0ﾂｰ竊・0ﾂｰ 縺ｧ
            //   邱城㍼ 1.000 縺ｮ縺ｾ縺ｾ・峨ゅ後←縺薙∈驟阪ｋ縺九阪・霑斐○縺ｦ縺・◆縺後後←繧後□縺鷹壹ｋ縺九阪ｒ
            //   謐ｨ縺ｦ縺ｦ縺・◆縲・            //
            //   騾壹ｊ繧・☆縺輔・ ﾎｴ・磯幕蜿｣繧貞屓繧願ｾｼ繧縺ｶ繧薙・菴吝臆霍晞屬・峨°繧牙・縺吶よ演縺悟｣√→縺励※蜉ｹ縺代・
            //   ﾎｴ 縺ｯ謇峨↓縺､縺・※蜍輔￥縺ｮ縺ｧ縲・幕縺榊・蜷医′縺昴・縺ｾ縺ｾ髻ｳ驥上↓縺ｪ繧九・            //   笘・ｽ主沺蜉驥阪〒 1 繧ｹ繧ｫ繝ｩ縺ｫ逡ｳ繧薙〒謗帙￠繧具ｼ亥ｸｯ蝓溷挨縺ｫ謗帙￠縺ｪ縺・ｼ峨・            //     蟶ｯ蝓溷挨縺ｫ謗帙￠繧九・縺ｯ LPF 縺ｧ縺ゅｊ縲√％繧ゅｊ縺ｯ騾城℃縺梧球蠖薙→縺・≧蠖ｹ蜑ｲ蛻・球繧貞｣翫☆縲・            //     蝗樊釜縺碁°縺ｶ縺ｮ縺ｯ縲碁幕蜿｣縺ｸ縺ｮ螳壻ｽ阪阪→縲悟屓繧願ｾｼ繧縺ｶ繧薙・霍晞屬縲阪・ 2 縺､縺縺代・            const float thru = bbGain(paths[i].gain);
            const float g = ((sum > 1e-6f) ? w / sum : 0.0f) * thru;

            // 笘・幕蜿｣繧・*髱｢**縺ｨ縺励※魑ｴ繧峨☆・医・繧､繝倥Φ繧ｹ・峨・            //   轤ｹ1縺､縺縺ｨ謌ｸ蜿｣縺後ヴ繝ｳ繝昴う繝ｳ繝医↓閨槭％縺医※縲檎峩邱夂噪縲阪↓縺ｪ繧九る幕蜿｣縺ｮ蠎・′繧翫↓
            //   豐ｿ縺｣縺ｦ謨ｰ轤ｹ縺ｸ謨｣繧峨☆縺ｨ縲∫せ縺斐→縺ｫ邨瑚ｷｯ髟ｷ縺ｨ譁ｹ蜷代′繧上★縺九↓驕輔≧縺ｮ縺ｧ縲・            //   驕・ｻｶ縺ｨ繝代Φ縺後・繧峨￠縺ｦ縲梧虻蜿｣繧ｵ繧､繧ｺ縺ｮ髱｢縲阪→縺励※閨槭％縺医ｋ縲・            //   螳壻ｽ阪・螟ｱ繧上↑縺・ｼ亥・轤ｹ縺悟酔縺倬幕蜿｣縺ｮ荳翫↓縺ゅｋ・峨・縺ｧ縲∬ｨｭ險医・蜆ｪ蜈磯・ｽ・            //   縲碁・阡ｽ驥上・邊ｾ蠎ｦ繧医ｊ髢句哨縺ｸ縺ｮ螳壻ｽ阪阪ｒ蟠ｩ縺輔★縺ｫ蟷・□縺代′蜉繧上ｋ縲・            //   繧ｲ繧､繝ｳ縺ｯ 1/k縲ゆｽ主沺・磯≦蟒ｶ蟾ｮ縺梧ｳ｢髟ｷ繧医ｊ蟆上＆縺・ｼ峨〒縺ｯ蜷檎嶌縺ｧ雜ｳ縺輔ｌ縺ｦ蜈・・驥上↓謌ｻ繧翫・            //   鬮伜沺縺ｧ縺ｯ謨｣繧峨・縺｣縺ｦ蟆上＆縺上↑繧・笏笏 髱｢髻ｳ貅舌・蜻ｨ豕｢謨ｰ迚ｹ諤ｧ縺ｨ縺励※縺昴ｌ縺ｧ豁｣縺励＞縲・            const int k = std::min(apertureSpread_, paths[i].nSpread);
            const float inv = (k > 1) ? 1.0f / static_cast<float>(k) : 1.0f;
            const int nq = (k > 1) ? k : 1;
            for (int q = 0; q < nq && n < maxN; ++q) {
                const Vec3 sp = (k > 1) ? paths[i].spread[q] : paths[i].aperture;
                const Vec3 dir = normalized(sp - listener);
                // 邨瑚ｷｯ髟ｷ繧らせ縺斐→縺ｫ貂ｬ繧顔峩縺吶ゅ％縺薙′驕輔≧縺九ｉ驕・ｻｶ縺後・繧峨￠繧九・                const float plen = (k > 1) ? (length(sp - listener) + length(source - sp))
                                           : paths[i].pathLength;
                outPos[n] = listener + dir * std::max(plen, 0.5f);
                outGain[n] = g * inv;
                if (outBand)
                    for (int b = 0; b < kNumBands; ++b)
                        outBand[n * kNumBands + b] = g * inv * paths[i].openBand[b];
                ++n;
            }
        }
        return n;
    }

    /// 髢句哨繧剃ｽ慕せ縺ｫ謨｣繧峨☆縺具ｼ・=轤ｹ縺ｮ縺ｾ縺ｾ・・縲・=髱｢縺ｨ縺励※魑ｴ繧峨☆・峨よ里螳・1縲・    void setApertureSpread(int n) { apertureSpread_ = std::min(std::max(n, 1), DiffractionPath::kMaxSpread); }
    int apertureSpread() const { return apertureSpread_; }

    // 縲仙屓謚・Phase 1.5)縲素rom->to 縺ｮ蟶ｯ蝓溷挨蝗樊釜繧ｲ繧､繝ｳ(0..1)縲・    //   驕ｮ阡ｽ縺ｪ縺・竊・蜈ｨ蟶ｯ蝓・1.0 / 霑ょ屓霍ｯ縺ゅｊ 竊・Maekawa・井ｽ主沺縺ｻ縺ｩ蝗槭ｊ霎ｼ繧・・ 霑ょ屓霍ｯ縺ｪ縺・竊・0縲・    // 蠖ｱ蠅・阜繧定ｷｨ縺・〒繧る｣邯壹↓縺ｪ繧九ｈ縺・∫・繧峨＆繧後◆鬆伜沺縺ｧ繧ょ屓謚伜ｴ繧定ｨ育ｮ励＠縺ｦ逶ｴ謗･髻ｳ縺ｨ蜷域・縺吶ｋ縲・    //
    //   莉･蜑阪・ isOccluded 縺ｮ莠悟､蛻､螳壹〒縲碁撼驕ｮ阡ｽ 竊・蜈ｨ蟶ｯ蝓・1.0 / 驕ｮ阡ｽ 竊・UTD蛟､縲阪→蛻・ｊ譖ｿ縺医※縺・◆縲・    //   UTD 縺ｯ蠖ｱ蠅・阜縺ｧ邏・0.5(-6dB) 繧定ｿ斐☆縺ｮ縺ｧ縲∝｢・阜繧定ｷｨ縺・□迸ｬ髢薙↓ 0.5 竍・1.0 縺ｮ谿ｵ蟾ｮ
    //   ・亥ｮ滓ｸｬ縺ｧ譛螟ｧ 12.5dB・峨′蜃ｺ縺ｦ縺・◆縲・    //   迚ｩ逅・噪縺ｫ縺ｯ辣ｧ繧峨＆繧後◆鬆伜沺縺ｫ繧ょ屓謚伜ｴ縺ｯ蟄伜惠縺励∫峩謗･髻ｳ縺ｨ雜ｳ縺吶→蠅・阜縺ｧ騾｣邯壹↓縺ｪ繧・    //   笏笏 縺昴ｌ縺・UTD 縺・GTD 繧偵御ｸ讒伜喧縲阪＠縺溽岼逧・◎縺ｮ繧ゅ・・・td.h 縺ｮ utdTotalGain 蜿ら・・峨・    void computeDiffraction(const Vec3& from, const Vec3& to, float outGain[kNumBands]) const {
        diffractionContinuous(from, to, isOccluded(from, to), outGain);
    }

    // 蝗樊釜繧ｲ繧､繝ｳ・亥ｽｱ蠅・阜縺ｧ騾｣邯夲ｼ峨Ｐcc 縺ｯ蜻ｼ縺ｳ蜃ｺ縺怜・縺梧里縺ｫ謖√▲縺ｦ縺・ｋ驕ｮ阡ｽ蛻､螳壹ｒ貂｡縺・    // ・・omputeDirectSoft 縺ｯ蜷後§蛻､螳壹ｒ菴ｿ縺・屓縺吶◆繧√∽ｺ碁㍾縺ｫ raycast 縺励↑縺・ｼ峨・    //
    // 繝｢繝・Ν縺ｯ蜑榊ｷ昴・蠑擾ｼ・ore/maekawa.h・峨らｬｦ蜿ｷ莉倥″ ﾎｴ 縺縺代〒豎ｺ縺ｾ繧・
    //     蠖ｱ縺ｮ蛛ｴ ﾎｴ>0 / 蠖ｱ蠅・阜 ﾎｴ=0・井ｸ｡蛛ｴ 5dB・・ 辣ｧ繧峨＆繧後◆蛛ｴ ﾎｴ<0
    //
    //   笘・｣邯壽ｧ縺梧ｧ矩逧・↓菫晁ｨｼ縺輔ｌ繧狗炊逕ｱ:
    //     ﾎｴ 縺ｯ縲悟・蛟呵｣懃ｨ懃ｷ壹・譛蟆丞､縲阪ｒ蜿悶▲縺ｦ繧る｣邯夲ｼ・in 縺ｯ騾｣邯夐未謨ｰ繧剃ｿ昴▽縲ゅ←縺ｮ遞懃ｷ壹′
    //     譛蟆上°縺悟・繧梧崛繧上▲縺ｦ繧ゅ∝､縺昴・繧ゅ・縺ｯ鬟帙・縺ｪ縺・ｼ峨ゅ＠縺溘′縺｣縺ｦ繧ｲ繧､繝ｳ繧る｣帙・縺ｪ縺・・    //     蠖ｱ蠅・阜縺ｧ縺ｯ逶ｴ邱壹′遞懃ｷ壹ｒ謗繧√ｋ縺ｮ縺ｧ ﾎｴ竊・ 縺ｫ縺ｪ繧翫∫ｬｦ蜿ｷ縺縺代′蜿崎ｻ｢縺励※貊代ｉ縺九↓郢九′繧九・    //
    //   UTD(Core/utd.h) 繧帝浹螢ｰ邨瑚ｷｯ縺ｫ菴ｿ繧上↑縺・炊逕ｱ縺ｯ maekawa.h 縺ｮ蜀帝ｭ縺ｫ險倩ｼ峨・    //   隕∫せ縺縺・ UTD 縺ｯ蝗樊釜轤ｹ縺ｮ 3D 蟷ｾ菴包ｼ委・,ﾏ・n,ﾎｲ0・峨↓萓晏ｭ倥☆繧九・縺ｧ縲∫ｨ懃ｷ壹′蜈･繧梧崛繧上ｋ縺ｨ
    //   蟷ｾ菴輔′荳埼｣邯壹↓螟峨ｏ縺｣縺ｦ鬟帙・縲る㍾縺ｿ莉倥″蟷ｳ蝮・・隍・ｴ蜥後・繧ｨ繝阪Ν繧ｮ繝ｼ蜉邂励ｒ縺・★繧後ｂ隧ｦ縺励◆縺・    //   隗｣豸医〒縺阪★縲∵怙濶ｯ縺ｧ繧・3.8dB縲∵怙謔ｪ 17.6dB 縺ｮ谿ｵ蟾ｮ縺梧ｮ九▲縺溘・    //   UTD 縺ｯ diffractionUtd() 縺ｨ縺励※豈碑ｼ・・讀懆ｨｼ逕ｨ縺ｫ谿九＠縺ｦ縺ゅｋ縲・    //   繧ｲ繧､繝ｳ縺ｯ**譛蟆・ﾎｴ 縺九ｉ**蜃ｺ縺吶・    //     險ｭ險・ﾂｧ5-6 縺ｧ縺ｯ髢句哨縺斐→縺ｮ繧ｨ繝阪Ν繧ｮ繝ｼ蜉邂励→縺励※縺・◆縺後∝ｮ滓ｸｬ縺吶ｋ縺ｨ騾｣邯壽ｧ縺梧が蛹悶＠縺・    //     ・亥ｽｱ蠅・阜 0.033 竊・0.093 / 繝峨い 0.191 竊・0.234・峨ら炊逕ｱ縺ｯ縲∝刈邂励・蜑肴署縺ｧ縺ゅｋ
    //     縲悟酔縺倡黄逅・ｵ瑚ｷｯ繧帝㍾隍・＠縺ｦ謨ｰ縺医↑縺・阪′繧ｯ繝ｩ繧ｹ繧ｿ繝ｪ繝ｳ繧ｰ縺ｮ髮｢謨｣諤ｧ縺ｧ貅縺溘○縺ｪ縺・◆繧√・    //     繧ｯ繝ｩ繧ｹ繧ｿ縺悟・陬ゅ・邨ｱ蜷医☆繧狗椪髢薙↓蜥後′鬟帙・縲・    //     min 縺ｯ騾｣邯壹↑縺ｮ縺ｧ鬟帙・縺ｪ縺・よ悽菴懊・蜆ｪ蜈磯・ｽ搾ｼ磯・阡ｽ驥上・邊ｾ蠎ｦ繧医ｊ螳壻ｽ搾ｼ峨°繧峨ｂ縲・    //     繧ｲ繧､繝ｳ縺ｯ騾｣邯壹〒縺ゅｊ縺輔∴縺吶ｌ縺ｰ繧医＞縲・*譁ｹ蜷代・髢句哨縺斐→縺ｫ蜃ｺ繧九・縺ｧ諠・ｱ縺ｯ螟ｱ繧上ｌ縺ｪ縺・・*
    void diffractionContinuous(const Vec3& from, const Vec3& to, bool occ,
                               float outGain[kNumBands]) const {
        constexpr int kMaxOut = 8;
        DiffractionPath paths[kMaxOut];
        const int np = findDiffractionPaths(from, to, paths, kMaxOut);
        if (np <= 0) {
            // 蝗樊釜縺ｮ逶ｸ謇九′辟｡縺・る・阡ｽ縺ｪ繧牙ｮ悟・縺ｫ螻翫°縺ｪ縺・・撼驕ｮ阡ｽ縺ｪ繧臥ｴ騾壹ｊ縲・            const float g = occ ? 0.0f : 1.0f;
            for (int b = 0; b < kNumBands; ++b) outGain[b] = g;
            return;
        }
        // 邨瑚ｷｯ縺斐→縺ｫ maekawa(ﾎｴ) ﾃ・髢句哨蟷・ご繧､繝ｳ 繧貞・縺励√◎縺ｮ譛螟ｧ繧呈治繧九・        //
        //   繝ｻﾎｴ 縺九ｉ菴懊ｊ逶ｴ縺吶％縺ｨ閾ｪ菴薙・豁｣縺励＞・井ｸ願ｨ倥・騾｣邯壽ｧ縺ｮ逅・罰・峨・        //     縺溘□縺・*髢句哨蟷・ご繧､繝ｳ繧呈寺縺醍峩縺輔↑縺・→縲・哩縺倥◆謇峨・繧上★縺九↑髫咎俣縺九ｉ邏騾壹ｊ縺吶ｋ**
        //     ・亥ｮ滓ｸｬ: 3cm 縺ｮ髫咎俣縺ｧ繧・0.56 縺ｮ縺ｾ縺ｾ縺縺｣縺滂ｼ峨ょｹ・・ ﾎｴ 縺ｫ迴ｾ繧後↑縺・◆繧√・        //   繝ｻ邨瑚ｷｯ縺ｮ gain[] 繧偵◎縺ｮ縺ｾ縺ｾ菴ｿ縺｣縺ｦ縺ｯ縺・￠縺ｪ縺・・ 谺｡蝗樊釜縺ｮ gain[] 縺ｯ谿ｵ縺斐→縺ｮ遨・        //     ・・1ﾃ揚2・峨〒菴懊▲縺ｦ縺ゅｊ縲［aekawa(ﾎｴ1+ﾎｴ2) 繧医ｊ蟆上＆縺・・ 谺｡縺ｨ 2 谺｡縺悟・繧梧崛繧上ｋ
        //     迸ｬ髢薙↓谿ｵ蟾ｮ縺悟・繧具ｼ亥ｮ滓ｸｬ: 謇・6.5ﾂｰ 縺ｧ 0.191・峨・        //   繝ｻ蟷・ご繝ｼ繝医′蜈ｨ縺ｦ 1 縺ｮ縺ｨ縺阪・ max_i maekawa(ﾎｴ_i) = maekawa(min_i ﾎｴ_i) 縺ｨ縺ｪ繧翫・        //     蠕捺擂縺ｮ縲梧怙蟆・ﾎｴ縲阪→蜴ｳ蟇・↓荳閾ｴ縺吶ｋ・・aekawa 縺ｯ ﾎｴ 縺ｮ蜊倩ｪｿ貂帛ｰ鷹未謨ｰ・峨・        for (int b = 0; b < kNumBands; ++b) outGain[b] = 0.0f;
        for (int i = 0; i < np; ++i) {
            float gi[kNumBands];
            maekawa::gainBands(paths[i].delta, gi);
            for (int b = 0; b < kNumBands; ++b)
                outGain[b] = std::max(outGain[b], gi[b] * paths[i].openGain);
        }
    }

    // 蜈ｨ蛟呵｣懃ｨ懃ｷ壹ｒ蝗槭ｋ霑ょ屓縺ｮ菴吝臆髟ｷ ﾎｴ(m, 髱櫁ｲ) 縺ｮ譛蟆丞､縲ょ呵｣懊′辟｡縺代ｌ縺ｰ false縲・    //   辣ｧ繧峨＆繧後◆鬆伜沺縺ｧ縺ｯ逶ｴ謗･邱壹ｒ蝪槭＄邂ｱ縺檎┌縺・・縺ｧ縲∝呵｣懈爾邏｢繧定ｿ大ｍ縺ｾ縺ｧ蠎・￡繧句ｿ・ｦ√′縺ゅｋ縲・    //   繝槭・繧ｸ繝ｳ縺ｯ蜑榊ｷ昴・蠑上′蜉ｹ縺冗ｯ・峇繧医ｊ蠎・￠繧後・繧医＞縲ょ柑縺九↑縺上↑繧九・縺ｯ N<-0.2縲・    //   縺吶↑繧上■ ﾎｴ > 0.1ﾎｻ縲よ怙菴主沺 125Hz 縺ｮ ﾎｻ=2.7m 縺ｧ繧・ﾎｴ 0.28m 遞句ｺｦ縺ｪ縺ｮ縺ｧ 4m 縺ゅｌ縺ｰ蜊∝・縲・    bool minDetour(const Vec3& from, const Vec3& to, bool occ, float& outDelta) const {
        const float margin = occ ? 0.0f : 4.0f;
        bool any = false;
        float best = 0.0f;
        forEachDiffractionCandidate(from, to,
            [&](const Vec3&, float d, const Vec3&, const Vec3&, const Vec3&, const Vec3&, float, int) {
                if (!any || d < best) { any = true; best = d; }
            }, margin);
        outDelta = best;
        return any;
    }

    // UTD 迚医・蝗樊釜繧ｲ繧､繝ｳ・域ｯ碑ｼ・・讀懆ｨｼ逕ｨ縲る浹螢ｰ邨瑚ｷｯ縺ｧ縺ｯ菴ｿ繧上↑縺・ｼ峨・    // 蜴ｳ蟇・ｧ｣縺縺悟ｽｱ蠅・阜縺ｧ荳埼｣邯壹↓縺ｪ繧九◆繧∵治逕ｨ縺励※縺・↑縺・笏笏 蝗槫ｸｰ繝・せ繝医〒荳｡閠・ｒ荳ｦ縺ｹ縺ｦ蜃ｺ縺吶・    void diffractionUtd(const Vec3& from, const Vec3& to, bool occ,
                        float outGain[kNumBands]) const {
        const float margin = occ ? 0.0f : 8.0f;
        constexpr int kMaxEdges = 8;
        struct Cand { Vec3 P; Vec3 edgeDir; Vec3 refT; float delta; };
        Cand cands[kMaxEdges];
        int nc = 0;
        float bestDelta = -1.0f;
        forEachDiffractionCandidate(from, to,
            [&](const Vec3& P, float d, const Vec3& edgeDir, const Vec3& refT, const Vec3&, const Vec3&, float, int) {
                if (bestDelta < 0.0f || d < bestDelta) bestDelta = d;
                if (nc < kMaxEdges) cands[nc++] = Cand{P, edgeDir, refT, d};
            }, margin);
        if (nc == 0) {
            const float g = occ ? 0.0f : 1.0f;
            for (int b = 0; b < kNumBands; ++b) outGain[b] = g;
            return;
        }
        float direct[kNumBands];
        for (int b = 0; b < kNumBands; ++b) direct[b] = 1.0f;
        float acc[kNumBands] = {0, 0, 0, 0, 0, 0};
        float wSum = 0.0f;
        const float kSigma = 0.15f;   // ﾎｴ 縺ｫ繧医ｋ霆溘ｉ縺九＞驥阪∩莉倥￠・育ｨ懃ｷ壹・荵励ｊ謠帙∴繧貞插縺呵ｩｦ縺ｿ・・        for (int i = 0; i < nc; ++i) {
            const float w = std::exp(-(cands[i].delta - bestDelta) / kSigma);
            if (w < 1e-3f) continue;
            float g[kNumBands];
            utd::utdWedgeTotal(to, cands[i].P, from, cands[i].edgeDir, cands[i].refT,
                               1.5f, !occ, direct, g);
            for (int b = 0; b < kNumBands; ++b) acc[b] += w * g[b];
            wSum += w;
        }
        const float inv = (wSum > 1e-6f) ? 1.0f / wSum : 0.0f;
        for (int b = 0; b < kNumBands; ++b) outGain[b] = acc[b] * inv;
    }

    // 縲仙ｽｹ蜑ｲ1(Phase 2)・壹た繝輔ヨ驕ｮ阡ｽ縲醍峩謗･髻ｳ繧偵碁浹貅仙捉繧翫・隍・焚繧ｵ繝ｳ繝励Ν縲阪〒貂ｬ繧翫・・繧峨ｌ縺溷牡蜷医ｒ
    // 貊代ｉ縺九↓蜃ｺ縺呻ｼ茨ｼ晏濠蠖ｱ・峨ゆｺ悟､縺ｮ隕矩壹＠蛻､螳壹→驕輔＞縲∝｣√・邵√ｒ縺ｾ縺溘＄縺ｨ縺肴ｮｵ蟾ｮ縺ｧ縺ｪ縺城｣邯壹↓螟峨ｏ繧九・    //   outGain[b] = 逶ｴ謗･騾城℃(繧ｽ繝輔ヨ) 縺ｨ 蝗樊釜繝輔Ο繧｢(驕ｮ阡ｽ蜑ｲ蜷医〒繝輔ぉ繝ｼ繝峨う繝ｳ) 縺ｮ螟ｧ縺阪＞譁ｹ縲・    //   numSamples : 髻ｳ貅仙捉繧翫・繧ｵ繝ｳ繝励Ν謨ｰ・井ｾ・8・・/ sourceRadius : 繧ｵ繝ｳ繝励Ν蜊雁ｾ・m, 萓・0.4)
    // 谿ｵ蟾ｮ縺ｮ荳ｻ蝗・育峩謗･ 1.0竊呈攝雉ｪ騾城℃ 縺ｨ 蝗樊釜 1.0竊樽aekawa 縺ｮ荳譁牙・譖ｿ・峨ｒ騾｣邯壼喧縺吶ｋ縲・    // 窶ｻ 蠖ｱ蠅・阜縺ｮ蜴ｳ蟇・↑騾｣邯壼喧縺ｯ Phase 4・・TD 驕ｷ遘ｻ髢｢謨ｰ・峨〒縲ゅ％縺薙・縺昴・謇句燕縺ｮ邱ｩ蜥後・    //   outDetourDelta : null 縺ｧ縺ｪ縺代ｌ縺ｰ 蝗樊釜縺ｮ霑ょ屓菴吝臆髟ｷ ﾎｴ(m) 繧呈嶌縺擾ｼ磯撼驕ｮ阡ｽ=0 / 螳悟・驕ｮ阡ｽ=螟ｧ・峨・    //                    繧ｹ繝・い縺ｮ逶ｴ謗･鬆・ｒ縲悟ｮ溷柑邨瑚ｷｯ=逶ｴ謗･霍晞屬+ﾎｴ縲阪〒驥阪∩莉倥￠縺吶ｋ縺ｮ縺ｫ菴ｿ縺・・    void computeDirectSoft(const Vec3& listener, const Vec3& source, float outGain[kNumBands],
                           int numSamples, float sourceRadius, float* outDetourDelta) const {
        using namespace scene_detail;
        Vec3 dir = source - listener;
        const float dist = length(dir);
        if (dist < 1e-4f) { for (int b = 0; b < kNumBands; ++b) outGain[b] = 1.0f; return; }
        dir = dir * (1.0f / dist);
        // 隕也ｷ壹↓蝙ら峩縺ｪ蝓ｺ蠎包ｼ磯浹貅仙捉繧翫・蜀・乢繧ｵ繝ｳ繝励Ν逕ｨ・峨・        const Vec3 t = (std::fabs(dir.x) > 0.9f) ? Vec3(0, 1, 0) : Vec3(1, 0, 0);
        const Vec3 u = normalized(cross(dir, t));
        const Vec3 v = cross(dir, u);

        const int N = numSamples < 1 ? 1 : numSamples;
        float transAccum[kNumBands] = {0, 0, 0, 0, 0, 0};
        int occCount = 0;
        uint32_t rng = static_cast<uint32_t>(dist * 1000.0f) * 2654435761u + 98765u;
        for (int i = 0; i < N; ++i) {
            const float rr = sourceRadius * std::sqrt(hashRand01(rng));
            const float aa = 2.0f * kPiF * hashRand01(rng);
            const Vec3 p = source + u * (rr * std::cos(aa)) + v * (rr * std::sin(aa));
            float g[kNumBands];
            computeTransmission(listener, p, g);
            for (int b = 0; b < kNumBands; ++b) transAccum[b] += g[b];
            if (g[0] < 1.0f) ++occCount;  // 縺薙・繧ｵ繝ｳ繝励Ν縺ｯ螢√ｒ騾壹▲縺・        }
        const float inv = 1.0f / static_cast<float>(N);
        const float occFrac = static_cast<float>(occCount) * inv;

        // 蝗樊釜・壼ｽｱ蠅・阜縺ｧ騾｣邯壹↓縺ｪ繧狗沿繧剃ｽｿ縺・ｼ・iffractionContinuous 蜿ら・・峨・        //   莉･蜑阪・縺薙％縺ｧ縲碁撼驕ｮ阡ｽ竊貞・蟶ｯ蝓・.0 / 驕ｮ阡ｽ竊旦TD蛟､縲阪→莠悟､縺ｧ蛻・ｊ譖ｿ縺医※縺翫ｊ縲・        //   蠖ｱ蠅・阜繧定ｷｨ縺千椪髢薙↓邏・2dB縺ｮ谿ｵ蟾ｮ縺悟・縺ｦ縺・◆縲ＰccFrac 縺ｧ邱ｩ蜥後ｒ隧ｦ縺ｿ縺ｦ縺・◆縺後・        //   dif 閾ｪ菴薙′霍ｳ縺ｶ縺ｮ縺ｧ蜉ｹ縺・※縺・↑縺九▲縺溘・        //   窶ｻ髻ｳ螢ｰ邨瑚ｷｯ縺ｯ useReflections=ON 縺ｮ縺ｨ縺・occlusionReflectedMulti 邨檎罰縺ｧ
        //     縺薙％繧帝壹ｋ縲ＤomputeDiffraction 繧堤峩縺励◆縺縺代〒縺ｯ髻ｳ縺ｫ蜉ｹ縺九↑縺・・        const bool centerOcc = isOccluded(listener, source);
        float dif[kNumBands];
        // 笘・・繝ｼ繧ｿ繝ｫ縺後％縺ｮ邨瑚ｷｯ繧呈髪驟阪＠縺ｦ縺・ｋ縺ｪ繧峨∝屓繧願ｾｼ縺ｿ驥上・**繝昴・繧ｿ繝ｫ縺悟髪荳縺ｮ遲斐∴**縲・        //   譌ｧ邨瑚ｷｯ・亥燕蟾昴・繝ｼ繧ｹ・峨ｒ菴ｵ逕ｨ縺吶ｋ縺ｨ縲・哩縺倥◆驛ｨ螻九〒邨瑚ｷｯ縺檎┌縺・・縺ｫ
        //   蝗樊釜縺・竏・9dB 縺ｮ蠎翫ｒ菴懊ｊ縲√◎縺薙°繧我ｸ翫・譚占ｳｪ縺悟柑縺九↑縺上↑繧・        //   ・亥ｮ滓ｸｬ: 螢√・陬上□縺・500Hz 莉･荳翫′謇峨→蜷後§ 0.0115 縺ｫ蠑ｵ繧贋ｻ倥＞縺ｦ縺・◆・峨・        //   繝昴・繧ｿ繝ｫ縺碁哩縺倥※縺・ｋ(f=0)縺ｪ繧牙屓繧願ｾｼ縺ｿ繧・0 縺ｧ縺ｪ縺代ｌ縺ｰ霎ｻ隍・′蜷医ｏ縺ｪ縺・・        if (!portals_.empty()) {
            // 繝昴・繧ｿ繝ｫ繧堤ｽｮ縺・◆繧ｷ繝ｼ繝ｳ縺ｧ縺ｯ縲・幕蜿｣邨檎罰縺ｮ髻ｳ縺ｯ繝昴・繧ｿ繝ｫ縺縺代′豎ｺ繧√ｋ縲・            //   笘・檎峩邱壹′遏ｩ蠖｢繧呈ｨｪ蛻・ｋ縺九阪〒菴ｿ縺・ｼ丈ｽｿ繧上↑縺・ｒ蛻・ｊ譖ｿ縺医※縺ｯ縺・￠縺ｪ縺・・            //     髢句哨邇・◎縺ｮ繧ゅ・縺ｯ騾｣邯壹↑縺ｮ縺ｫ縲・*菴ｿ縺・婿縺御ｺ悟､**縺ｫ縺ｪ縺｣縺ｦ蟠悶′蜃ｺ繧・            //     ・亥ｮ滓ｸｬ: 髻ｳ貅舌ｒ讓ｪ縺ｸ 5cm 蜍輔°縺励◆縺縺代〒 8.8dB 關ｽ縺｡縲√◎縺ｮ蠕・            //      +6.6 / -2.4 / -3.6 dB 縺ｨ證ｴ繧後◆縲る幕蜿｣邇・・ 0.9656 縺ｧ蟷ｳ繧峨↑縺ｾ縺ｾ・峨・            //     閠ｳ縺ｫ縺ｯ縲梧･縺ｫ髻ｳ縺悟ｰ上＆縺上↑繧九阪→閨槭％縺医ｋ縲・            //   蟶ｸ縺ｫ蜈ｨ繝昴・繧ｿ繝ｫ繧呈ｸｬ繧翫∵怙繧る壹ｋ繧ゅ・繧呈治繧九る幕蜿｣邇・・繧ｾ繝ｼ繝ｳ縺ｮ荳ｭ蠢・ｒ
            //   遏ｩ蠖｢蜀・∈荳ｸ繧√※縺・ｋ縺ｮ縺ｧ縲∫ｷ壹′螟悶ｌ縺ｦ繧る｣邯壹↓關ｽ縺｡繧九・            for (int b = 0; b < kNumBands; ++b) dif[b] = 0.0f;
            for (const Portal& pt : portals_) {
                if (!pt.active) continue;
                float pf[kNumBands]; Vec3 cp(0, 0, 0);
                if (!portalCoupling(pt, listener, source, pf, &cp)) continue;
                for (int b = 0; b < kNumBands; ++b) dif[b] = std::max(dif[b], pf[b]);
            }
        } else {
            diffractionContinuous(listener, source, centerOcc, dif);
        }

        // 霑ょ屓菴吝臆髟ｷ ﾎｴ・医せ繝・い縺ｮ驥阪∩莉倥￠縺ｫ菴ｿ縺・ｼ峨る撼驕ｮ阡ｽ=0 / 霑ょ屓霍ｯ縺ｪ縺・螟ｧ縲・        float detourDelta = 0.0f;
        if (centerOcc) {
            Vec3 dp;
            const float delta = diffractionDetour(listener, source, dp);
            detourDelta = (delta < 0.0f) ? 1e9f : delta;
        }

        for (int b = 0; b < kNumBands; ++b) {
            // 騾城℃縺ｯ繧ｨ繝阪Ν繧ｮ繝ｼ縲∝屓謚・蜑榊ｷ・縺ｯ謖ｯ蟷・・*縺昴・縺ｾ縺ｾ豈斐∋縺ｦ縺ｯ縺・￠縺ｪ縺・*縺ｮ縺ｧ
            //   騾城℃蛛ｴ繧呈険蟷・∈謠・∴繧具ｼ・aterial.h 縺ｮ蜊倅ｽ崎ｦ冗ｴ・ｼ峨・            //   謠・∴縺ｪ縺・→螢∬ｶ翫＠縺ｮ謌仙・縺御ｺ御ｹ励・繧灘ｰ上＆縺剰ｩ穂ｾ｡縺輔ｌ縲∝ｮ滓ｸｬ縺ｧ 9dB 驕主ｰ上□縺｣縺溘・            const float soft = std::sqrt(transAccum[b] * inv);   // 貊代ｉ縺九↑逶ｴ謗･騾城℃・域険蟷・ｼ・            // 螢√ｒ謚懊￠繧区・蛻・→蝗槭ｊ霎ｼ繧謌仙・縺ｮ螟ｧ縺阪＞譁ｹ繧呈治繧九・            //   蝗樊釜蛛ｴ縺ｯ譌｢縺ｫ縲檎・繧峨＆繧後※縺・ｌ縺ｰ逶ｴ謗･髻ｳ霎ｼ縺ｿ縲阪・邱丞粋蛟､縺ｪ縺ｮ縺ｧ縲・            //   occFrac 縺ｮ繧医≧縺ｪ蠕御ｻ倥￠縺ｮ繝輔ぉ繝ｼ繝峨・隕√ｉ縺ｪ縺・・            outGain[b] = clamp01(std::max(soft, dif[b]));
        }
        if (outDetourDelta) *outDetourDelta = detourDelta;
    }

    // 縲舌た繝輔ヨ驕ｮ阡ｽ縺ｮ邏譚舌醍峩謗･邨瑚ｷｯ縺ｮ騾城℃(謖ｯ蟷・縺ｨ縲後←繧後□縺鷹・繧峨ｌ縺ｦ縺・ｋ縺・0..1)縲阪ｒ蛻･縲・↓霑斐☆縲・    //
    //   笘・％繧後′縺ゅｋ縺ｨ `occ` 縺ｮ莠悟､蛻・ｲ舌ｒ蜈ｨ驛ｨ豸医○繧九・    //     莉･蜑阪・縲瑚ｦ矩壹○縺ｦ縺・ｋ縺九阪〒逶ｴ謗･繧ｿ繝・・縺ｮ諢丞袖繧貞・繧頑崛縺医※縺・◆:
    //       lit  竊・逶ｴ謗･繧ｿ繝・・ = 蝗樊釜繧ｲ繧､繝ｳ・亥｢・阜縺ｧ 0.56・・    //       !lit 竊・逶ｴ謗･繧ｿ繝・・ = 騾城℃・亥｣∵攝縺ｪ繧・0.12・会ｼ・F 繧ｿ繝・・縺碁幕蜿｣縺ｫ蜃ｺ迴ｾ
    //     蠅・阜繧定ｷｨ縺・□迸ｬ髢薙↓ 13dB 關ｽ縺｡縲∝酔譎ゅ↓髻ｳ縺碁浹貅先婿蜷代°繧蛾幕蜿｣譁ｹ蜷代∈繝ｯ繝ｼ繝励＠縺ｦ縺・◆縲・    //
    //   髻ｳ貅舌∪繧上ｊ縺ｮ蜀・乢繧偵し繝ｳ繝励Ν縺吶ｋ縺ｮ縺ｧ縲∵滋繧√ｋ菴咲ｽｮ縺ｧ縺ｯ縲御ｸ驛ｨ縺縺鷹・繧峨ｌ繧九咲憾諷九′
    //   縺昴・縺ｾ縺ｾ謨ｰ蛟､縺ｫ縺ｪ繧九る城℃繧る・阡ｽ蜑ｲ蜷医ｂ騾｣邯壹↓蜍輔￥縺ｮ縺ｧ縲∝・蟯舌↑縺励〒譖ｸ縺代ｋ:
    //       逶ｴ謗･繧ｿ繝・・ = softTrans          ・郁ｦ矩壹＠縺ｧ 1.0縲∝｢・阜縺ｧ邏・0.5縲∝ｽｱ縺ｧ譚占ｳｪ縺ｮ騾城℃・・    //       F 繧ｿ繝・・   = 蝗樊釜繧ｲ繧､繝ｳ ﾃ・occFrac・郁ｦ矩壹＠縺ｧ 0 縺ｫ縺ｪ繧九・縺ｧ莠碁㍾險井ｸ翫＠縺ｪ縺・ｼ・    //   sourceRadius 縺・*驕ｷ遘ｻ縺ｮ蟷・*繧呈ｱｺ繧√ｋ縲ら黄逅・噪縺ｫ縺ｯ驕ｷ遘ｻ蟷・・繝輔Ξ繝阪Ν繧ｾ繝ｼ繝ｳ・井ｽ主沺縺ｻ縺ｩ蠎・＞縲・    //   謨ｰ繝｡繝ｼ繝医Ν隕乗ｨ｡・峨□縺後√％縺薙・蟶ｯ蝓溷・騾壹・1縺､縺ｮ蜀・乢縺ｧ霑台ｼｼ縺励※縺・ｋ縲ょｺ・￡繧九⊇縺ｩ貊代ｉ縺九↓
    //   縺ｪ繧倶ｻ｣繧上ｊ縺ｫ蠖ｱ縺ｮ邵√′縺ｼ繧・￠繧九・縺ｧ縲∬ｳ縺ｧ豎ｺ繧√ｋ繝√Η繝ｼ繝九Φ繧ｰ蛟､縲・    // outLeakPoint 窶ｦ **縺ｩ縺薙°繧画怙繧ょ､壹￥貍上ｌ縺ｦ縺・ｋ縺・*・磯城℃縺ｧ驥阪∩莉倥￠縺溘・・阡ｽ髱｢荳翫・驥榊ｿ・ｼ峨・    //   髢峨§縺滓演縺ｮ蜷代％縺・〒魑ｴ縺｣縺ｦ縺・ｋ髻ｳ縺ｯ縲∫樟螳溘↓縺ｯ謇峨・譚ｿ縺悟・謾ｾ蟆・＠縺ｦ螻翫￥縲・    //   縺縺九ｉ驛ｨ螻九・縺ｩ縺薙↓縺・※繧ゅ梧演縺九ｉ閨槭％縺医ｋ縲阪ゅ→縺薙ｍ縺御ｻ翫・螳壻ｽ阪・
    //   驕ｮ阡ｽ縺ｮ豺ｱ縺・ﾎｴ 縺ｧ繝悶Ξ繝ｳ繝峨＠縺ｦ縺・※縲・哩謇峨・ ﾎｴ 縺・0.023 縺ｨ蟆上＆縺・◆繧・    //   **縺ｻ縺ｼ髻ｳ貅先婿蜷代・縺ｾ縺ｾ**縺ｫ縺ｪ繧具ｼ亥｣√・荳ｭ縺九ｉ魑ｴ縺｣縺ｦ縺・ｋ繧医≧縺ｫ閨槭％縺医ｋ・峨・    //
    //   笘・檎峩邱壹→螢√・莠､轤ｹ縲阪ｒ菴ｿ縺｣縺ｦ繧よэ蜻ｳ縺後↑縺・ゆｺ､轤ｹ縺ｯ逶ｴ邱壻ｸ翫↓縺ゅｋ縺ｮ縺ｧ譁ｹ蜷代′螟峨ｏ繧峨↑縺・・    //     隕√ｋ縺ｮ縺ｯ髱｢蜈ｨ菴薙・縺・■**譛繧る壹☆蝣ｴ謇**縺ｧ縲√◎繧後・騾城℃縺ｮ驥阪∩莉倥″驥榊ｿ・・    //     髻ｳ貅舌∪繧上ｊ縺ｫ謨｣繧峨＠縺滓ｨ呎悽縺ｮ繝ｬ繧､縺悟ｽ薙◆繧倶ｽ咲ｽｮ繧偵√◎縺ｮ讓呎悽縺ｮ騾城℃驥上〒
    //     驥阪∩莉倥￠縺ｦ蟷ｳ蝮・☆繧後・蜃ｺ繧九ょ｣√ｈ繧雁ｼｱ縺・演縺後≠繧後・驥榊ｿ・・謇峨∈蟇・ｋ縲・    //     迚ｩ菴薙′謇峨°縺ｩ縺・°縺ｯ隕九↑縺・笏笏 蠑ｱ縺・園縺ｸ蟇・ｋ縲√→縺・≧縺縺代・    void computeSoftOcclusion(const Vec3& listener, const Vec3& source,
                              float outTrans[kNumBands], float& outOccFrac,
                              int numSamples = 32, float sourceRadius = 0.9f,
                              Vec3* outLeakPoint = nullptr) const {
        using namespace scene_detail;
        Vec3 dir = source - listener;
        const float dist = length(dir);
        if (dist < 1e-4f) {
            for (int b = 0; b < kNumBands; ++b) outTrans[b] = 1.0f;
            outOccFrac = 0.0f;
            return;
        }
        dir = dir * (1.0f / dist);
        const Vec3 t = (std::fabs(dir.x) > 0.9f) ? Vec3(0, 1, 0) : Vec3(1, 0, 0);
        const Vec3 u = normalized(cross(dir, t));
        const Vec3 v = cross(dir, u);

        const int N = numSamples < 1 ? 1 : numSamples;
        float acc[kNumBands] = {0, 0, 0, 0, 0, 0};
        float blockedAcc[kNumBands] = {0, 0, 0, 0, 0, 0};
        int blockedN = 0;
        float occWeighted = 0.0f;
        Vec3 leakAcc(0, 0, 0);
        float leakW = 0.0f;

        // 笘・し繝ｳ繝励Ν驟咲ｽｮ縺ｯ**豎ｺ螳夂噪**縺ｫ縺吶ｋ・医ヵ繧｣繝懊リ繝・メ蜀・乢・峨・        //   荵ｱ謨ｰ縺ｧ謦偵￥縺ｨ縲√Μ繧ｹ繝翫・縺悟ｰ代＠蜍輔￥縺縺代〒繝代ち繝ｼ繝ｳ縺悟､峨ｏ繧翫・・阡ｽ蜑ｲ蜷医′繧ｬ繧ｿ縺､縺・        //   ・亥ｮ滓ｸｬ縺ｧ 0.511 竊・0.371 竊・0.794 縺ｨ髱槫腰隱ｿ縺ｫ縺ｪ縺｣縺滂ｼ峨・        //   菴咲ｽｮ縺ｫ萓晏ｭ倥＠縺ｪ縺・崋螳夐・鄂ｮ縺ｪ繧峨∝､牙喧縺吶ｋ縺ｮ縺ｯ蟷ｾ菴輔□縺代↑縺ｮ縺ｧ貊代ｉ縺九↓蜍輔￥縲・        constexpr float kGolden = 2.39996323f;
        for (int i = 0; i < N; ++i) {
            const float rr = sourceRadius * std::sqrt((i + 0.5f) / static_cast<float>(N));
            const float aa = kGolden * static_cast<float>(i);
            const Vec3 p = source + u * (rr * std::cos(aa)) + v * (rr * std::sin(aa));
            float g[kNumBands];
            computeTransmission(listener, p, g);      // 繧ｨ繝阪Ν繧ｮ繝ｼ
            for (int b = 0; b < kNumBands; ++b) acc[b] += g[b];
            // 貍上ｌ縺ｮ驥榊ｿ・る・繧峨ｌ縺ｦ縺・ｋ讓呎悽縺ｫ縺､縺・※縲∝ｽ薙◆縺｣縺滉ｽ咲ｽｮ繧帝城℃驥上〒驥阪∩莉倥￠繧九・            if (outLeakPoint && g[0] < 0.5f) {
                const Vec3 sd = p - listener;
                const float sl = length(sd);
                if (sl > 1e-4f) {
                    const SceneHit hh = raycastClosest(listener, sd * (1.0f / sl), sl);
                    if (hh.hit) {
                        const float w = g[0];        // 菴主沺縺ｮ騾城℃驥上〒驥阪∩莉倥￠・域ｼ上ｌ縺ｮ荳ｻ謌仙・・・                        leakAcc = leakAcc + hh.point * w;
                        leakW += w;
                    }
                }
            }
            // 縲悟｡槭′繧後※縺・ｋ蛛ｴ縲阪・譚占ｳｪ縲・*螳滄圀縺ｫ驕ｮ繧峨ｌ縺ｦ縺・ｋ讓呎悽縺縺・*繧貞ｹｳ蝮・☆繧九・            //   笘・怙蟆丞､繧呈治縺｣縺ｦ縺ｯ縺・￠縺ｪ縺・よ怙蟆上・縲梧怙繧ゅｈ縺城・繧区攝雉ｪ縲搾ｼ晏｣√ｒ諡ｾ縺・・縺ｧ縲・            //     謌ｸ蜿｣繧定ｦ・▲縺ｦ縺・ｋ縺ｮ縺梧演(TL15)縺ｧ繧ょ｣・TL34)縺ｮ蛟､縺ｫ縺ｪ繧翫・            //     髢峨§縺滓演縺悟｣√→縺励※魑ｴ繧具ｼ亥ｮ滓ｸｬ縺ｧ逶ｴ謗･髻ｳ縺・19dB 豐医ｓ縺・峨・            //     隕・▲縺ｦ縺・ｋ縺ｮ縺御ｽ輔°縺ｯ讓呎悽縺檎､ｺ縺励※縺・ｋ縺ｮ縺ｧ縲√◎繧後ｒ邏逶ｴ縺ｫ菴ｿ縺・・            if (g[0] < 0.5f) {
                for (int b = 0; b < kNumBands; ++b) blockedAcc[b] += g[b];
                ++blockedN;
            }
            // 驕ｮ阡ｽ蜑ｲ蜷医ｂ縲御ｽ墓悽蠖薙◆縺｣縺溘°縲阪・莠悟､繧ｫ繧ｦ繝ｳ繝医〒縺ｯ縺ｪ縺上・*菴主沺縺ｮ貂幄｡ｰ驥・*縺ｧ貂ｬ繧九・            //   莠悟､縺縺ｨ 1/N 蛻ｻ縺ｿ縺ｮ髫取ｮｵ縺ｫ縺ｪ繧九よｸ幄｡ｰ驥上↑繧蛾Κ蛻・噪縺ｪ驕ｮ阡ｽ縺碁｣邯壹↓蜃ｺ繧九・            occWeighted += 1.0f - std::sqrt(g[0]);
        }
        const float inv = 1.0f / static_cast<float>(N);
        // 繧ｨ繝阪Ν繧ｮ繝ｼ縺ｧ蟷ｳ蝮・＠縺ｦ縺九ｉ謖ｯ蟷・∈・・aterial.h 縺ｮ蜊倅ｽ崎ｦ冗ｴ・ｼ峨・        for (int b = 0; b < kNumBands; ++b) outTrans[b] = std::sqrt(acc[b] * inv);
        outOccFrac = clamp01(occWeighted * inv);
        if (outLeakPoint)
            *outLeakPoint = (leakW > 1e-9f) ? leakAcc * (1.0f / leakW) : source;

        // 笘・ｵ瑚ｷｯ縺後・繝ｼ繧ｿ繝ｫ繧呈ｨｪ蛻・ｋ縺ｪ繧峨・*髢句哨邇・〒鄂ｮ縺肴鋤縺医ｋ**縲・        //   蜀・乢縺ｮ讓呎悽蛹悶・縲梧栢縺代◆縺句｡槭′繧後◆縺九阪・莠悟､繧・N 蛟区焚縺医ｋ縺ｮ縺ｧ縲・        //   驕ｮ阡ｽ蛛ｴ(0.003)縺ｨ邏騾壹＠蛛ｴ(1.0)縺ｮ豈斐′ 316蛟阪≠繧九→ 1/N 縺ｮ邊貞ｺｦ縺悟ｷｨ螟ｧ縺ｪ谿ｵ縺ｫ縺ｪ繧九・        //   螳滓ｸｬ: 謇・20ﾂｰ竊・0ﾂｰ 縺ｧ 32轤ｹ荳ｭ3轤ｹ縺梧栢縺代◆縺縺代〒 **+14.9dB** 霍ｳ繧薙□縲・        //   繝昴・繧ｿ繝ｫ縺ｮ髢句哨邇・f 縺ｯ隗｣譫千噪縺ｧ騾｣邯壹↑縺ｮ縺ｧ縲√％繧後ｒ縲梧栢縺代◆蜑ｲ蜷医阪↓菴ｿ縺・・        //     ﾏ・ｲ = f + (1竏断)ﾂｷﾏЮ蝪槭′繧後◆ﾂｲ
        //   ﾏЮ蝪槭′繧後◆ 縺ｯ讓呎悽縺ｮ**譛蟆丞､**繧呈治繧具ｼ域攝雉ｪ縺昴・繧ゅ・縺ｪ縺ｮ縺ｧ隗貞ｺｦ縺ｧ蜍輔°縺ｪ縺・ｼ晄ｮｵ縺悟・縺ｪ縺・ｼ峨・        //   笘・檎ｷ壼・縺檎洸蠖｢繧帝壹ｋ縺九阪〒菴ｿ縺・ｼ丈ｽｿ繧上↑縺・ｒ蛻・ｊ譖ｿ縺医※縺ｯ縺・￠縺ｪ縺・・        //     髢句哨邇・◎縺ｮ繧ゅ・縺ｯ騾｣邯壹↑縺ｮ縺ｫ**菴ｿ縺・婿縺御ｺ悟､**縺ｫ縺ｪ縺｣縺ｦ蟠悶′蜃ｺ繧・        //     ・亥ｮ滓ｸｬ: 髻ｳ貅舌ｒ讓ｪ縺ｸ 5cm 蜍輔°縺励◆縺縺代〒 8.8dB 關ｽ縺｡縲√◎縺ｮ蠕・+6.6 / -2.4 dB 縺ｨ
        //      證ｴ繧後◆縲る幕蜿｣邇・・ 0.9656 縺ｧ蟷ｳ繧峨↑縺ｾ縺ｾ・峨りｳ縺ｫ縺ｯ縲梧･縺ｫ蟆上＆縺上↑繧九阪→閨槭％縺医ｋ縲・        //     蜈ｨ繝昴・繧ｿ繝ｫ繧呈ｸｬ縺｣縺ｦ譛繧る壹ｋ繧ゅ・繧呈治繧九る幕蜿｣邇・・繧ｾ繝ｼ繝ｳ縺ｮ荳ｭ蠢・ｒ遏ｩ蠖｢蜀・∈
        //     荳ｸ繧√※縺・ｋ縺ｮ縺ｧ縲∫ｷ壹′螟悶ｌ縺ｦ繧る｣邯壹↓關ｽ縺｡繧九・        if (!portals_.empty()) {
            float best[kNumBands] = {0, 0, 0, 0, 0, 0};
            bool anyPortal = false;
            for (const Portal& pt : portals_) {
                if (!pt.active) continue;
                float f[kNumBands]; Vec3 cp(0, 0, 0);
                if (!portalCoupling(pt, listener, source, f, &cp)) continue;
                for (int b = 0; b < kNumBands; ++b) best[b] = std::max(best[b], f[b]);
                anyPortal = true;
            }
            if (anyPortal) {
                const float binv = (blockedN > 0) ? 1.0f / static_cast<float>(blockedN) : 0.0f;
                for (int b = 0; b < kNumBands; ++b) {
                    // 驕ｮ繧峨ｌ縺ｦ縺・ｋ讓呎悽縺・縺､繧ら┌縺代ｌ縺ｰ邏騾壹＠謇ｱ縺・ｼ亥｡槭＄繧ゅ・縺檎┌縺・ｼ峨・                    const float blocked = (blockedN > 0) ? clamp01(blockedAcc[b] * binv) : 1.0f;
                    outTrans[b] = std::sqrt(clamp01(best[b] + (1.0f - best[b]) * blocked));
                }
                outOccFrac = clamp01(1.0f - outTrans[0]);
            }
        }
    }

    // 縲仙ｽｹ蜑ｲ2(Phase 5)・壼渚蟆・ｾｼ縺ｿ驕ｮ阡ｽ縲代Μ繧ｹ繝翫・襍ｷ轤ｹ縺ｧ numRays 譛ｬ縺ｮ繝ｬ繧､繧呈鋳縺阪∝｣√〒蜿榊ｰ・    // 縺輔○縺ｪ縺後ｉ蜷・ヰ繧ｦ繝ｳ繧ｹ轤ｹ縺九ｉ髻ｳ貅舌∈ next-event 縺ｧ縺､縺ｪ縺舌ら峩謗･(騾城℃竓募屓謚・・句渚蟆・〒蝗槭ｊ霎ｼ繧
    // 謌仙・繧貞ｸｯ蝓溷挨縺ｫ遨阪∩縲・・阡ｽ驥・0..1)繧定ｿ斐☆縲ょ渚蟆・ｵ瑚ｷｯ縺後≠繧九・縺ｧ螢∬｣上〒繧・1.0 縺ｫ蠑ｵ繧贋ｻ倥°縺ｪ縺・・    //   outBands6 : null 縺ｧ縺ｪ縺代ｌ縺ｰ 逶ｴ謗･竓募屓謚倪兜蜿榊ｰ・縺ｮ蟶ｯ蝓溷挨逕溷ｭ・0..1) 繧呈嶌縺上・    //   窶ｻ 逶ｸ蟇ｾ貂幄｡ｰ(驕蝗槭ｊ縺ｻ縺ｩ蠑ｱ縺・縺ｮ縺ｿ縲らｵｶ蟇ｾ霍晞屬貂幄｡ｰ縺ｯ Wwise 蛛ｴ縲よ立 acoustic_world 縺九ｉ遘ｻ讀阪・    float occlusionReflected(const Vec3& source, const Vec3& listener,
                             float* outBands6, int numRays, int maxBounces) const {
        using namespace scene_detail;
        const float kEps = 1e-3f;
        const float refDist = std::max(length(source - listener), 1e-3f);

        // 1) 逶ｴ謗･邨瑚ｷｯ・昴た繝輔ヨ驕ｮ阡ｽ・磯城℃竓募屓謚倥ｒ蜊雁ｽｱ縺ｧ騾｣邯壼喧・峨・        float total[kNumBands];
        computeDirectSoft(listener, source, total, 8, 0.4f, nullptr);

        // 2) 蜿榊ｰ・〒蝗槭ｊ霎ｼ繧謌仙・・医Μ繧ｹ繝翫・繝ｬ繧､・杵ext-event・峨・        if (numRays > 0 && maxBounces > 0 && instanceCount() > 0) {
            float reflected[kNumBands] = {0, 0, 0, 0, 0, 0};
            const float maxDist = refDist * 8.0f + 50.0f;
            for (int i = 0; i < numRays; ++i) {
                Vec3 o = listener;
                Vec3 d = fibonacciSphereDir(i, numRays);
                uint32_t rng = static_cast<uint32_t>(i) * 2654435761u + 12345u;
                float carry[kNumBands] = {1, 1, 1, 1, 1, 1};
                float totalLen = 0.0f;
                float remaining = maxDist;
                for (int bounce = 0; bounce < maxBounces; ++bounce) {
                    const SceneHit hit = raycastClosest(o, d, remaining);
                    if (!hit.hit) break;
                    totalLen += hit.t;
                    remaining -= hit.t;
                    if (remaining <= kEps) break;
                    const AcousticMaterial& mat = materialOf(hit.materialId);
                    const Vec3 q = hit.point + hit.normal * 0.02f;  // 閾ｪ蟾ｱ繝偵ャ繝磯亟豁｢縺ｫ豬ｮ縺九○繧・                    float seg[kNumBands];
                    computeTransmission(q, source, seg);
                    const float pathLen = totalLen + length(source - q);
                    float atten = refDist / pathLen;
                    atten *= atten;
                    if (atten > 1.0f) atten = 1.0f;
                    for (int b = 0; b < kNumBands; ++b) {
                        const float refl = clamp01(1.0f - mat.absorption[b] - mat.transmission[b]);
                        reflected[b] += carry[b] * refl * seg[b] * atten;
                        carry[b] *= refl;
                    }
                    d = scatteredDir(d, hit.normal, scatteringMean(mat), rng);
                    o = q;
                }
            }
            const float inv = 1.0f / static_cast<float>(numRays);
            // total 縺ｯ謖ｯ蟷・ｼ・omputeDirectSoft 縺ｮ蜃ｺ蜉幢ｼ峨〉eflected 縺ｯ繧ｨ繝阪Ν繧ｮ繝ｼ縲・            //   逶ｴ謗･縺ｨ蜿榊ｰ・・辟｡逶ｸ髢｢縺ｪ蛻･邨瑚ｷｯ縺ｪ縺ｮ縺ｧ**繧ｨ繝阪Ν繧ｮ繝ｼ縺ｧ雜ｳ縺励※縺九ｉ謖ｯ蟷・∈謌ｻ縺・*縲・            //   蜊倅ｽ阪ｒ謠・∴縺壹↓雜ｳ縺吶→縲∝渚蟆・・繧薙′莠御ｹ励・蛻・□縺鷹℃螟ｧ縺ｫ蜉ｹ縺上・            for (int b = 0; b < kNumBands; ++b) {
                const float e = total[b] * total[b] + reflected[b] * inv;
                total[b] = clamp01(std::sqrt(e));
            }
        }

        if (outBands6) for (int b = 0; b < kNumBands; ++b) outBands6[b] = total[b];
        float mean = 0.0f;
        for (int b = 0; b < kNumBands; ++b) mean += total[b];
        mean /= kNumBands;
        return clamp01(1.0f - mean);
    }

    // 縲仙ｽｹ蜑ｲ2繝ｻ隍・焚髻ｳ貅撰ｼ医Μ繧ｹ繝翫・繝吶・繧ｹ蜈ｱ譛会ｼ峨代Μ繧ｹ繝翫・繝ｬ繧､繧・numRays 譛ｬ縺縺第鋳縺搾ｼ・aycast 縺ｯ
    // 髻ｳ貅先焚縺ｫ萓晏ｭ倥＠縺ｪ縺・ｼ・蝗槭・繧難ｼ峨∝推繝舌え繝ｳ繧ｹ轤ｹ縺九ｉ蜈ｨ髻ｳ貅舌∈ next-event 縺ｧ縺､縺ｪ縺舌・    //   outOcc[j]   : 髻ｳ貅・j 縺ｮ驕ｮ阡ｽ驥・0..1)・・ull 蜿ｯ・・    //   outBands    : null 縺ｧ縺ｪ縺代ｌ縺ｰ j*kNumBands+b 縺ｫ 逶ｴ謗･竓募屓謚倪兜蜿榊ｰ・縺ｮ蟶ｯ蝓溷挨逕溷ｭ倥ｒ譖ｸ縺・    // 螟ｧ驥城浹貅舌〒繧・raycast 繧ｳ繧ｹ繝医′蠅励∴縺ｪ縺・ｼ・ext-event 縺ｮ縺ｿ髻ｳ貅先焚縺ｶ繧難ｼ峨よ立螳溯｣・°繧臥ｧｻ讀阪・    //   outDir : null 縺ｧ縺ｪ縺代ｌ縺ｰ j*3+{0,1,2} 縺ｫ縲後お繝阪Ν繧ｮ繝ｼ縺悟ｱ翫￥謾ｯ驟肴婿蜷・蜊倅ｽ阪・繧ｯ繝医Ν)縲阪ｒ譖ｸ縺上・    //            驕ｮ阡ｽ譎ゅ・蜿榊ｰ・蝗樊釜縺梧髪驟阪＠縲・浹貅千悄譁ｹ蜷代〒縺ｪ縺鞘懷屓繧願ｾｼ繧薙〒螻翫￥譁ｹ蜷鯛昴↓縺ｪ繧九・    //            Wwise 蛛ｴ縺ｧ縺薙・譁ｹ蜷代↓莉ｮ諠ｳ繧ｨ繝溘ャ繧ｿ繧堤ｽｮ縺咲峩縺吶→縲悟ｱ翫￥譁ｹ蜷代°繧芽◇縺薙∴繧九阪↓縺ｪ繧九・    //   directWeight : 繧ｹ繝・い縺ｮ縲檎峩謗･鬆・阪・驥阪∩菫よ焚縲ょ､ｧ縺阪＞縺ｻ縺ｩ髻ｳ貅先婿蜷代∈螳壻ｽ阪′蠑ｵ繧贋ｻ倥￥縲・    //                  逶ｴ謗･鬆・・ directGain ﾃ・(逶ｴ謗･霍晞屬/螳溷柑邨瑚ｷｯ)ﾂｲ ﾃ・directWeight 縺ｧ驥阪∩莉倥￠縲・    //                  螳溷柑邨瑚ｷｯ = 逶ｴ謗･霍晞屬 + 蝗樊釜ﾎｴ・育ｵ瑚ｷｯ陬懷ｮ後・縺ｶ繧灘刈邂暦ｼ会ｼ晞□蝗槭ｊ縺ｻ縺ｩ逶ｴ謗･縺悟ｼｱ縺ｾ繧翫せ繝・い縺碁幕縺上・    void occlusionReflectedMulti(const Vec3& listener, const Vec3* sources, int count,
                                 float* outOcc, float* outBands, float* outDir,
                                 float directWeight, int numRays, int maxBounces) const {
        using namespace scene_detail;
        if (!sources || count <= 0) return;
        const float kEps = 1e-3f;
        std::vector<float> total(static_cast<size_t>(count) * kNumBands);
        std::vector<float> reflected(static_cast<size_t>(count) * kNumBands, 0.0f);
        std::vector<float> refDist(static_cast<size_t>(count));
        std::vector<Vec3> dirAccum(static_cast<size_t>(count), Vec3(0.0f, 0.0f, 0.0f));

        // 1) 逶ｴ謗･・磯浹貅舌＃縺ｨ・会ｼ昴た繝輔ヨ驕ｮ阡ｽ縲ょｮ壻ｽ阪ｒ諡・≧縲檎ｬｬ荳豕｢髱｢縲阪・蛻ｰ譚･譁ｹ蜷代ｒ菴懊ｋ・・        //    隕矩壹○繧・竊・髻ｳ貅先婿蜷・/ 驕ｮ阡ｽ 竊・蝗槭ｊ霎ｼ繧隗抵ｼ亥屓謚倥・謗繧√ｋ轤ｹ・画婿蜷代ょ渚蟆・・譁ｹ蜷代↓蜉ｹ縺九○縺ｪ縺・        //    ・亥渚蟆・・迴ｾ螳溘〒繧ょ・陦碁浹蜉ｹ譫懊〒縺ｻ縺ｼ螳壻ｽ阪○縺壹∝ｹ・・蠎・′繧翫↓蛹悶￠繧九◆繧√Ｅocs/EARLY_REFLECTIONS.md・峨・        for (int j = 0; j < count; ++j) {
            float detourDelta = 0.0f;
            computeDirectSoft(listener, sources[j], &total[j * kNumBands], 8, 0.4f, &detourDelta);
            refDist[j] = std::max(length(sources[j] - listener), 1e-3f);
            float directMean = 0.0f;
            for (int b = 0; b < kNumBands; ++b) directMean += total[j * kNumBands + b];
            directMean /= kNumBands;

            // 隨ｬ荳豕｢髱｢縺ｮ譁ｹ蜷代→驥阪∩縲りｦ矩壹○繧後・髻ｳ貅先婿蜷代∝｡槭′繧後※縺・ｌ縺ｰ隍・焚繧ｨ繝・ず蜷域・縺ｮ蝗槭ｊ霎ｼ縺ｿ譁ｹ蜷・            //・域ｻ代ｉ縺九↓蛻・崛繧上ｊ縲∬､・焚髢句哨縺後≠繧後・荳｡蛛ｴ縺九ｉ・峨ょｮ溷柑邨瑚ｷｯ縺碁聞縺・⊇縺ｩ螳壻ｽ阪ｒ蠑ｱ繧√ｋ縲・            Vec3 firstDir = normalized(sources[j] - listener);
            float firstW = directMean;  // 隕矩壹○繧具ｼ昴◎縺ｮ縺ｾ縺ｾ蠑ｷ縺・ｮ壻ｽ・            if (isOccluded(listener, sources[j])) {
                // 笘・・ 譛ｪ螳梧・: 螢√ｒ謚懊￠縺ｦ縺上ｋ髻ｳ繧偵碁擇縺悟・謾ｾ蟆・☆繧九榊ｽ｢縺ｫ縺励◆縺・笘・・
                //   髻ｳ貅先婿蜷代・縺ｾ縺ｾ魑ｴ繧峨☆縺ｨ螢√・荳ｭ縺九ｉ閨槭％縺医∵ｭｩ縺・※繧・                //   縲梧演縺九ｉ貍上ｌ縺ｦ縺・ｋ縲肴─縺倥↓縺ｪ繧峨↑縺・ｼ郁ｳ縺ｧ遒ｺ隱肴ｸ医∩・峨・                //   貍上ｌ縺ｮ驥榊ｿ・ｼ・omputeSoftOcclusion 縺ｮ outLeakPoint・峨ｒ螳壻ｽ阪↓菴ｿ縺・｡医ｒ
                //   螳溯｣・＠縺溘′縲・*驥榊ｿ・′蜍輔°縺ｪ縺九▲縺・*・亥ｮ滓ｸｬ: 謇峨・譚占ｳｪ繧貞､峨∴縺ｦ繧・                //   驥榊ｿ・・ x=-1.45 縺ｮ縺ｾ縺ｾ・峨・                //   逅・罰: 驥榊ｿ・・縲碁浹貅舌・縺ｾ繧上ｊ縺ｫ謨｣繧峨＠縺滓ｨ呎悽縲阪・繝ｬ繧､縺悟ｽ薙◆繧倶ｽ咲ｽｮ繧帝寔繧√ｋ縲・                //   繝ｪ繧ｹ繝翫・縺梧虻蜿｣縺九ｉ螟悶ｌ縺ｦ縺・ｋ縺ｨ縲∵ｨ呎悽縺ｮ繝ｬ繧､縺ｯ蜈ｨ驛ｨ**螢・*縺ｫ蠖薙◆繧翫・                //   謇峨↓螻翫￥讓呎悽縺・1 縺､繧ら┌縺・ょｼｱ轤ｹ縺悟挨縺ｮ蝣ｴ謇縺ｫ縺ゅｋ蝣ｴ蜷医ｒ諡ｾ縺医↑縺・・                //   竊・髱｢荳翫・蜈ｨ轤ｹ繧貞・謾ｾ蟆・ｺ舌→縺励※遨榊・縺吶ｋ・磯浹髻ｿ繝ｩ繧ｸ繧ｪ繧ｷ繝・ぅ・峨°縲・                //     蠑ｱ轤ｹ繧貞ｮ｣險縺励※繝昴・繧ｿ繝ｫ蜷梧ｧ倥↓謇ｱ縺・°縲ょ燕閠・・驥阪￥縲∝ｾ瑚・・ authoring縲・                Vec3 cdir;
                const float dd = diffractionComposite(listener, sources[j], cdir);
                if (dd >= 0.0f) {
                    // 驕ｮ阡ｽ縺ｮ窶懈ｷｱ縺補斟ｴ縺ｧ 髻ｳ貅先婿蜷鯛・蜷域・蝗槭ｊ霎ｼ縺ｿ譁ｹ蜷・繧帝｣邯壹ヶ繝ｬ繝ｳ繝峨・                    // ﾎｴ=0・域滋繧√ｋ・晞・阡ｽ縺ｮ蠅・阜・峨〒髻ｳ貅先婿蜷代↓荳閾ｴ縺吶ｋ縺ｮ縺ｧ縲・・阡ｽ縺ｫ蜈･繧狗椪髢薙・鬟帙・縺悟・縺ｪ縺・・                    const Vec3 srcDir = normalized(sources[j] - listener);
                    const float tau = 1.5f;             // 繝悶Ξ繝ｳ繝峨・豺ｱ縺輔せ繧ｱ繝ｼ繝ｫ(m)縲ょｰ・縺吶＄蝗槭ｊ霎ｼ縺ｿ蛛ｴ縺ｸ
                    const float t = dd / (dd + tau);    // 0(蠅・阜)竊・(豺ｱ縺・・阡ｽ)
                    firstDir = normalized(srcDir * (1.0f - t) + cdir * t);
                    const float distFactor = refDist[j] / std::max(refDist[j] + dd, 1e-3f);
                    firstW = directMean * distFactor * distFactor;
                } else {
                    firstW = 0.0f;  // 霑ょ屓霍ｯ縺ｪ縺暦ｼ晏ｮ壻ｽ阪⊇縺ｼ辟｡縺暦ｼ医・繧ｹ繝亥・縺ｧ逵滓婿蜷代↓繝輔か繝ｼ繝ｫ繝舌ャ繧ｯ・・                }
            }
            dirAccum[j] = firstDir * (firstW * directWeight);
        }

        // 2) 蜿榊ｰ・ｼ医Μ繧ｹ繝翫・繝ｬ繧､縺ｯ1蝗槭□縺托ｼ晞浹貅先焚髱樔ｾ晏ｭ假ｼ峨ょ芦譚･譁ｹ蜷代・蛻晄悄繝ｬ繧､譁ｹ蜷・d0縲・        if (numRays > 0 && maxBounces > 0 && instanceCount() > 0) {
            float maxRef = 1e-3f;
            for (int j = 0; j < count; ++j) maxRef = std::max(maxRef, refDist[j]);
            const float maxDist = maxRef * 8.0f + 50.0f;
            for (int i = 0; i < numRays; ++i) {
                Vec3 o = listener;
                Vec3 d = fibonacciSphereDir(i, numRays);
                uint32_t rng = static_cast<uint32_t>(i) * 2654435761u + 12345u;
                float carry[kNumBands] = {1, 1, 1, 1, 1, 1};
                float totalLen = 0.0f;
                float remaining = maxDist;
                for (int bounce = 0; bounce < maxBounces; ++bounce) {
                    const SceneHit hit = raycastClosest(o, d, remaining);  // 竊・蜈ｱ譛・                    if (!hit.hit) break;
                    totalLen += hit.t;
                    remaining -= hit.t;
                    if (remaining <= kEps) break;
                    const AcousticMaterial& mat = materialOf(hit.materialId);
                    const Vec3 q = hit.point + hit.normal * 0.02f;
                    float refl[kNumBands];
                    for (int b = 0; b < kNumBands; ++b)
                        refl[b] = clamp01(1.0f - mat.absorption[b] - mat.transmission[b]);
                    for (int j = 0; j < count; ++j) {  // 縺薙％縺縺鷹浹貅先焚縺ｶ繧・                        float seg[kNumBands];
                        computeTransmission(q, sources[j], seg);
                        const float pathLen = totalLen + length(sources[j] - q);
                        float atten = refDist[j] / pathLen;
                        atten *= atten;
                        if (atten > 1.0f) atten = 1.0f;
                        // 蜿榊ｰ・お繝阪Ν繧ｮ繝ｼ縺ｯ蟶ｯ蝓溽函蟄・髻ｳ驥上・蠎・′繧・縺ｫ縺ｯ蜉ｹ縺上′縲∵婿蜷・dirAccum)縺ｫ縺ｯ
                        // 蜉ｹ縺九○縺ｪ縺・ｼ亥渚蟆・・螳壻ｽ阪ｒ謖√◆縺帙↑縺・ｼ晄僑謨｣謇ｱ縺・ｼ峨・                        for (int b = 0; b < kNumBands; ++b)
                            reflected[j * kNumBands + b] += carry[b] * refl[b] * seg[b] * atten;
                    }
                    for (int b = 0; b < kNumBands; ++b) carry[b] *= refl[b];  // 邯咏ｶ壹Ξ繧､縺ｮ貂幄｡ｰ
                    d = scatteredDir(d, hit.normal, scatteringMean(mat), rng);
                    o = q;
                }
            }
            const float inv = 1.0f / static_cast<float>(numRays);
            // total 縺ｯ謖ｯ蟷・〉eflected 縺ｯ繧ｨ繝阪Ν繧ｮ繝ｼ縲ら┌逶ｸ髢｢縺ｪ蛻･邨瑚ｷｯ縺ｪ縺ｮ縺ｧ
            //   繧ｨ繝阪Ν繧ｮ繝ｼ縺ｧ雜ｳ縺励※縺九ｉ謖ｯ蟷・∈謌ｻ縺呻ｼ・aterial.h 縺ｮ蜊倅ｽ崎ｦ冗ｴ・ｼ峨・            for (int j = 0; j < count; ++j)
                for (int b = 0; b < kNumBands; ++b) {
                    const float amp = total[j * kNumBands + b];
                    const float e = amp * amp + reflected[j * kNumBands + b] * inv;
                    total[j * kNumBands + b] = clamp01(std::sqrt(e));
                }
        }

        // 蜃ｺ蜉帙・        for (int j = 0; j < count; ++j) {
            if (outBands)
                for (int b = 0; b < kNumBands; ++b)
                    outBands[j * kNumBands + b] = total[j * kNumBands + b];
            float mean = 0.0f;
            for (int b = 0; b < kNumBands; ++b) mean += total[j * kNumBands + b];
            mean /= kNumBands;
            if (outOcc) outOcc[j] = clamp01(1.0f - mean);
            if (outDir) {
                Vec3 dv = dirAccum[j];
                if (length(dv) < 1e-6f) dv = sources[j] - listener;  // 繧ｨ繝阪Ν繧ｮ繝ｼ辟｡縺代ｌ縺ｰ逵滓婿蜷・                dv = normalized(dv);
                outDir[j * 3 + 0] = dv.x;
                outDir[j * 3 + 1] = dv.y;
                outDir[j * 3 + 2] = dv.z;
            }
        }
    }

    // 縲先ｮ矩涸(Phase 6)・壹お繧ｳ繧ｰ繝ｩ繝縲代Μ繧ｹ繝翫・縺ｫ螻翫￥繧ｨ繝阪Ν繧ｮ繝ｼ繧貞芦驕疲凾髢薙ン繝ｳ縺ｫ遨阪・縲・    // 逶ｴ謗･髻ｳ・句渚蟆・ｼ医Μ繧ｹ繝翫・繝ｬ繧､1蝗橸ｼ晏・譛峨・螟壹ヰ繧ｦ繝ｳ繧ｹ縲∝推繝舌え繝ｳ繧ｹ縺九ｉ蜈ｨ髻ｳ貅舌∈ next-event・峨・    //   outBins[k] : 譎る俣 [k*binSeconds, (k+1)*binSeconds) 縺ｫ螻翫￥蜷郁ｨ医お繝阪Ν繧ｮ繝ｼ・亥ｺ・ｸｯ蝓溷ｹｳ蝮・ｼ・    // 逶ｴ謗･髻ｳ縺ｮ螟ｧ繝斐・繧ｯ竊貞・譛溷渚蟆・・謖・焚貂幄｡ｰ縺ｮ蟆ｾ縲√→縺・≧谿矩涸縺ｮ蠖｢縺悟・繧九３T60/wet 邂怜・縺ｮ蝨溷床縲・    // 貂幄｡ｰ縺ｯ蜷ｸ蜿・refl^n・・arry・峨′諡・＞縲∫ｵｶ蟇ｾ霍晞屬縺ｮ 1/rﾂｲ 縺ｯ謗帙￠縺ｪ縺・ｼ育嶌蟇ｾ縺ｮ窶懷ｽ｢窶昴よ立螳溯｣・°繧臥ｧｻ讀搾ｼ峨・    void computeEchogram(const Vec3& listener, const Vec3* sources, int count,
                         float* outBins, int numBins, float binSeconds, float speedOfSound,
                         int numRays, int maxBounces) const {
        // 蟶ｯ蝓溽沿繧定ｨ育ｮ励＠縺ｦ蠎・ｸｯ蝓溷ｹｳ蝮・↓貎ｰ縺呻ｼ亥ｮ溯｣・・1譛ｬ縺ｫ菫昴▽・峨・        if (!outBins || numBins <= 0) return;
        std::vector<float> bands(static_cast<size_t>(numBins) * kNumBands, 0.0f);
        // 譌ｧAPI縺ｯ縲檎嶌蟇ｾ縺ｮ蠖｢縲咲畑騾費ｼ・T60謗ｨ螳夲ｼ峨↑縺ｮ縺ｧ蠎・′繧頑錐螟ｱ縺ｪ縺暦ｼ晏ｾ捺擂縺ｩ縺翫ｊ縲・        computeEchogramBands(listener, sources, count, bands.data(), numBins,
                             binSeconds, speedOfSound, numRays, maxBounces, 0.0f);
        for (int k = 0; k < numBins; ++k) {
            float e = 0.0f;
            for (int b = 0; b < kNumBands; ++b) e += bands[static_cast<size_t>(k) * kNumBands + b];
            outBins[k] = e / kNumBands;
        }
    }

    // 縲先ｮ矩涸・壼ｸｯ蝓溷挨繧ｨ繧ｳ繧ｰ繝ｩ繝縲台ｸ翫→蜷後§縺縺後∝ｸｯ蝓溘ｒ貎ｰ縺輔★ outBins[k*kNumBands + b] 縺ｫ譖ｸ縺上・    //   螳滄圀縺ｮ驛ｨ螻九・鬮伜沺縺ｻ縺ｩ騾溘￥貂幄｡ｰ縺吶ｋ・亥精蜿弱′鬮伜沺縺ｧ螟ｧ縺阪＞・峨ょｺ・ｸｯ蝓溷ｹｳ蝮・□縺ｨ縺昴・蟾ｮ縺梧ｶ医∴縲・    //   蠕梧悄蟆ｾ縺ｮ縲梧囓縺上↑縺｣縺ｦ縺・￥縲肴嫌蜍輔ｒ蛻･騾斐ム繝ｳ繝斐Φ繧ｰ縺ｧ謐城縺吶ｋ縺薙→縺ｫ縺ｪ繧九・    //   IR 逡ｳ縺ｿ霎ｼ縺ｿ縺ｧ窶懷ｮ滓ｸｬ縺輔ｌ縺溷ｰｾ窶昴ｒ魑ｴ繧峨☆縺ｫ縺ｯ縲√％縺ｮ蟶ｯ蝓溷挨縺ｮ貂幄｡ｰ繧ｫ繝ｼ繝悶′隕√ｋ縲・    // distanceRef: 髻ｳ貅舌°繧峨・蠎・′繧頑錐螟ｱ縺ｮ蝓ｺ貅冶ｷ晞屬縲・ 莉･荳九〒辟｡蜉ｹ・亥ｾ捺擂縺ｩ縺翫ｊ謳榊､ｱ縺ｪ縺暦ｼ峨・    //   貂幄｡ｰ縺ｯ host 蛛ｴ縺ｮ譌ｩ譛溷渚蟆・ち繝・・縺ｨ蜷後§隕冗ｴ・atten = distanceRef / max(d, distanceRef)縲・    //   繧ｨ繝阪Ν繧ｮ繝ｼ縺ｫ縺ｯ縺昴・2荵励ｒ謗帙￠繧九・    //
    //   窶ｻ 謗帙￠繧狗嶌謇九・縲碁浹貅絶・蜿榊ｰ・せ縲阪・蛹ｺ髢馴聞縺ｧ縺ゅ▲縺ｦ縲√Μ繧ｹ繝翫・縺ｾ縺ｧ縺ｮ邱冗ｵ瑚ｷｯ髟ｷ縺ｧ縺ｯ縺ｪ縺・・    //     蜿榊ｰ・せ竊偵Μ繧ｹ繝翫・蛛ｴ縺ｮ蠎・′繧翫・縲√Ξ繧､1譛ｬ縺檎ｫ倶ｽ楢ｧ偵ｒ莉｣陦ｨ縺励※縺・ｋ縺薙→閾ｪ菴薙′諡・▲縺ｦ縺・ｋ縲・    //     邱冗ｵ瑚ｷｯ髟ｷ縺ｧ謗帙￠繧九→縲∵凾髢薙′邨後▽縺ｻ縺ｩ・茨ｼ晉ｵ瑚ｷｯ縺碁聞縺・⊇縺ｩ・我ｸ蠕九↓貂幄｡ｰ縺悟ｼｷ縺ｾ繧翫・    //     蟆ｾ縺ｫ譛ｬ譚･蟄伜惠縺励↑縺・1/tﾂｲ 縺ｮ貂幄｡ｰ縺御ｹ励ｋ縲よ僑謨｣髻ｳ蝣ｴ縺ｮ繧ｨ繝阪Ν繧ｮ繝ｼ蟇・ｺｦ縺ｯ遨ｺ髢鍋噪縺ｫ縺ｻ縺ｼ荳讒倥〒縲・    //     譎る俣貂幄｡ｰ縺ｯ蜷ｸ髻ｳ縺縺代′諡・≧縺ｮ縺梧ｭ｣縺励＞縲ょｺ・＞驛ｨ螻九・荳ｭ螟ｮ縺ｻ縺ｩ邨瑚ｷｯ縺碁聞縺・・縺ｧ縲・    //     邱冗ｵ瑚ｷｯ髟ｷ縺ｧ謗帙￠繧九→縺昴％縺ｮ蜿埼涸縺縺代′逞ｩ縺帙ｋ縲・    void computeEchogramBands(const Vec3& listener, const Vec3* sources, int count,
                              float* outBins, int numBins, float binSeconds, float speedOfSound,
                              int numRays, int maxBounces, float distanceRef) const {
        using namespace scene_detail;
        if (!outBins || numBins <= 0 || !sources || count <= 0) return;
        for (int k = 0; k < numBins * kNumBands; ++k) outBins[k] = 0.0f;

        const float kEps = 1e-3f;
        const float invC = (speedOfSound > 1e-3f) ? 1.0f / speedOfSound : 0.0f;
        const float invBin = (binSeconds > 1e-6f) ? 1.0f / binSeconds : 0.0f;

        // 髻ｳ貅舌°繧峨・霍晞屬 d 縺ｫ蟇ｾ縺吶ｋ蠎・′繧頑錐螟ｱ・医お繝阪Ν繧ｮ繝ｼ・峨・        auto spreadEnergy = [distanceRef](float d) -> float {
            if (distanceRef <= 0.0f) return 1.0f;
            const float a = distanceRef / std::max(d, distanceRef);
            return a * a;
        };

        // 蟶ｯ蝓溷挨縺ｫ繝薙Φ縺ｸ遨阪・縲Ｆnergy6 縺ｯ kNumBands 隕∫ｴ縲・        auto addBin = [&](float dist, const float* energy6, float scale) {
            const int k = static_cast<int>(dist * invC * invBin);
            if (k < 0 || k >= numBins) return;
            float* dst = outBins + static_cast<size_t>(k) * kNumBands;
            for (int b = 0; b < kNumBands; ++b) {
                const float e = energy6[b] * scale;
                if (e > 0.0f) dst[b] += e;
            }
        };

        // 逶ｴ謗･髻ｳ・育峩邱壹・騾城℃・峨る浹貅絶・繝ｪ繧ｹ繝翫・縺ｮ蠎・′繧頑錐螟ｱ繧呈寺縺代ｋ縲・        for (int j = 0; j < count; ++j) {
            float g[kNumBands];
            computeTransmission(listener, sources[j], g);
            const float d = length(sources[j] - listener);
            addBin(d, g, spreadEnergy(d));
        }

        // 蜿榊ｰ・ｼ亥・譛峨Ξ繧､繝ｻ蟆ｾ縺檎ｪ灘・縺ｫ蜈･繧九∪縺ｧ繝ｬ繧､繧剃ｼｸ縺ｰ縺呻ｼ峨・        if (numRays > 0 && maxBounces > 0 && instanceCount() > 0) {
            float maxRef = 1e-3f;
            for (int j = 0; j < count; ++j)
                maxRef = std::max(maxRef, std::max(length(sources[j] - listener), 1e-3f));
            const float windowDist = static_cast<float>(numBins) * binSeconds * speedOfSound;
            const float maxDist = std::max(maxRef * 8.0f + 50.0f, windowDist);
            // 蜿榊ｰ・ｴ縺ｯ逅・擇蜈ｨ譁ｹ蜷代°繧峨・蜈･蟆・ｒ遨榊・縺励◆繧ゅ・縲らｫ倶ｽ楢ｧ剃ｿよ焚 2ﾏ 縺ｧ邨仙粋・域僑謨｣蝣ｴ繧堤ｫ九※繧具ｼ峨・            const float kDiffuseCoupling = 6.2831853f;
            const float inv = kDiffuseCoupling / static_cast<float>(numRays);

            for (int i = 0; i < numRays; ++i) {
                Vec3 o = listener;
                Vec3 d = fibonacciSphereDir(i, numRays);
                uint32_t rng = static_cast<uint32_t>(i) * 2654435761u + 12345u;
                float carry[kNumBands] = {1, 1, 1, 1, 1, 1};
                float totalLen = 0.0f;
                float remaining = maxDist;

                for (int bounce = 0; bounce < maxBounces; ++bounce) {
                    const SceneHit hit = raycastClosest(o, d, remaining);
                    if (!hit.hit) break;
                    totalLen += hit.t;
                    remaining -= hit.t;
                    if (remaining <= kEps) break;
                    const AcousticMaterial& mat = materialOf(hit.materialId);
                    const Vec3 q = hit.point + hit.normal * 0.02f;
                    float refl[kNumBands];
                    for (int b = 0; b < kNumBands; ++b)
                        refl[b] = clamp01(1.0f - mat.absorption[b] - mat.transmission[b]);

                    for (int j = 0; j < count; ++j) {
                        float seg[kNumBands];
                        computeTransmission(q, sources[j], seg);
                        const float srcLeg = length(sources[j] - q);   // 髻ｳ貅絶・蜿榊ｰ・せ
                        const float pathLen = totalLen + srcLeg;
                        float e[kNumBands];
                        for (int b = 0; b < kNumBands; ++b) e[b] = carry[b] * refl[b] * seg[b];
                        // 蠎・′繧頑錐螟ｱ縺ｯ縲碁浹貅絶・蜿榊ｰ・せ縲阪・蛹ｺ髢薙↓縺縺第寺縺代ｋ・育ｷ冗ｵ瑚ｷｯ髟ｷ縺ｧ縺ｯ縺ｪ縺・ｼ峨・                        addBin(pathLen, e, inv * spreadEnergy(srcLeg));
                    }
                    for (int b = 0; b < kNumBands; ++b) carry[b] *= refl[b];
                    d = scatteredDir(d, hit.normal, scatteringMean(mat), rng);
                    o = q;
                }
            }
        }
    }

    // 縲先婿蜷代・繝ｭ繝ｼ繝悶双rigin 縺九ｉ蜷・婿蜷代∈繝ｬ繧､繧帝｣帙・縺励√後◎縺ｮ譁ｹ蜷代°繧峨←繧後□縺第ｮ矩涸縺瑚ｿ斐ｋ縺九阪ｒ
    // 蟶ｯ蝓溷挨縺ｫ霑斐☆縲ＰutEnergy 縺ｯ dirCount*kNumBands 隕∫ｴ縲・    //
    //   蠕梧悄谿矩涸縺ｯ諡｡謨｣縺ｪ縺ｮ縺ｧ譎る俣讒矩縺ｯ繧ｨ繧ｳ繝ｼ繧ｰ繝ｩ繝縺梧戟縺ｦ縺ｰ繧医￥縲・*譁ｹ蜷大・蟶・*縺縺代′雜ｳ繧翫↑縺・・    //   髻ｳ貅舌↓萓晏ｭ倥＠縺ｪ縺・㍼縺ｪ縺ｮ縺ｧ髻ｳ貅舌＃縺ｨ縺ｫ險育ｮ励☆繧句ｿ・ｦ√′縺ｪ縺上√Μ繧ｹ繝翫・菴咲ｽｮ縺縺代〒豎ｺ縺ｾ繧・    //   ・・髻ｳ貅先焚縺悟｢励∴縺ｦ繧ゅさ繧ｹ繝医′蠅励∴縺ｪ縺・ｼ域･ｭ逡後′蠕梧悄谿矩涸繧貞・譛峨ヰ繧ｹ縺ｫ縺励※縺・ｋ縺ｮ縺ｨ蜷後§逅・ｱ茨ｼ峨・    //
    //   驥上・螳夂ｾｩ: 蜷・婿蜷代∈謦・▲縺溘Ξ繧､縺後∝渚蟆・・縺溘・縺ｫ谿九ｋ繧ｨ繝阪Ν繧ｮ繝ｼ carry 繧堤ｩ咲ｮ励＠縺溘ｂ縺ｮ縲・    //     繝ｻ逕溘″縺滄Κ螻九∈蜷代°縺・婿蜷・窶ｦ 菴募ｺｦ繧ょ渚蟆・＠縺ｦ遨咲ｮ励′螟ｧ縺阪＞
    //     繝ｻ蜷ｸ髻ｳ縺ｮ蠑ｷ縺・擇・城幕縺代◆遨ｺ縺ｸ蜷代°縺・婿蜷・窶ｦ 縺吶＄蟆ｽ縺阪ｋ縲√∪縺溘・菴輔↓繧ょｽ薙◆繧峨★ 0
    //   邨ｶ蟇ｾ蛟､縺ｧ縺ｯ縺ｪ縺乗婿蜷鷹俣縺ｮ逶ｸ蟇ｾ蛻・ｸ・→縺励※菴ｿ縺・ｼ亥他縺ｳ蜃ｺ縺怜・縺ｧ豁｣隕丞喧縺吶ｋ・峨・    void probeDirectionalEnergy(const Vec3& origin, const Vec3* dirs, int dirCount,
                                int maxBounces, float* outEnergy) const {
        using namespace scene_detail;
        if (!dirs || !outEnergy || dirCount <= 0) return;
        for (int k = 0; k < dirCount * kNumBands; ++k) outEnergy[k] = 0.0f;
        if (maxBounces <= 0 || instanceCount() == 0) return;

        const float kEps = 1e-3f;
        const float kMaxDist = 500.0f;   // 縺薙ｌ莉･荳雁・縺ｮ蜿榊ｰ・・谿矩涸縺ｨ縺励※諢丞袖繧呈戟縺溘↑縺・        for (int i = 0; i < dirCount; ++i) {
            Vec3 o = origin;
            Vec3 d = dirs[i];
            const float dl = length(d);
            if (dl < 1e-6f) continue;
            d = d * (1.0f / dl);
            uint32_t rng = static_cast<uint32_t>(i) * 2654435761u + 9781u;
            float carry[kNumBands] = {1, 1, 1, 1, 1, 1};
            float remaining = kMaxDist;
            float* dst = outEnergy + static_cast<size_t>(i) * kNumBands;

            for (int bounce = 0; bounce < maxBounces; ++bounce) {
                const SceneHit hit = raycastClosest(o, d, remaining);
                if (!hit.hit) break;             // 菴輔↓繧ょｽ薙◆繧峨↑縺・ｼ晞幕縺代※縺・ｋ・晄ｮ矩涸繧定ｿ斐＆縺ｪ縺・                remaining -= hit.t;
                if (remaining <= kEps) break;
                const AcousticMaterial& mat = materialOf(hit.materialId);
                for (int b = 0; b < kNumBands; ++b) {
                    carry[b] *= clamp01(1.0f - mat.absorption[b] - mat.transmission[b]);
                    dst[b] += carry[b];
                }
                d = scatteredDir(d, hit.normal, scatteringMean(mat), rng);
                o = hit.point + hit.normal * 0.02f;
            }
        }
    }

    // 縲先掠譛溷渚蟆・ち繝・・(A)縲壮ource竊停ｦ竊値istener 縺ｮ荳ｻ隕√↑蛻晄悄蜿榊ｰ・ｒ譛螟ｧ maxTaps 譛ｬ謚ｽ蜃ｺ縺吶ｋ縲・    //   蜃ｺ蜉帙ち繝・・ = imageSourcePos・・ listener + 蛻ｰ譚･譁ｹ蜷妥礼ｵ瑚ｷｯ髟ｷ縲ょｮ壻ｽ・霍晞屬貂幄｡ｰ逕ｨ・会ｼ・蟶ｯ蝓溘ご繧､繝ｳ縲・    //   繝ｪ繧ｹ繝翫・襍ｷ轤ｹ繝ｬ繧､ﾃ溶ext-event 縺ｧ蜷・渚蟆・・ {蛻ｰ譚･譁ｹ蜷・d0, 邨瑚ｷｯ髟ｷ, 蟶ｯ蝓溘ご繧､繝ｳ} 繧帝寔繧√・    //   繧ｨ繝阪Ν繧ｮ繝ｼ蠑ｷ縺・・↓縲∵婿蜷代′霑代＞繧ゅ・縺ｯ縺ｾ縺ｨ繧√※荳贋ｽ阪ｒ霑斐☆縲８wise Reflect 縺ｮ image source
    //   繧・ｻｮ諠ｳ繧ｨ繝溘ャ繧ｿ縺ｧ縲梧婿蜷代▽縺榊渚蟆・浹縲阪→縺励※魑ｴ繧峨☆諠ｳ螳壹よ綾繧雁､=譖ｸ縺崎ｾｼ繧薙□繧ｿ繝・・謨ｰ縲・    int computeEarlyReflections(const Vec3& listener, const Vec3& source,
                                Vec3* outImagePos, float* outGain, int maxTaps,
                                int numRays, int maxBounces) const {
        using namespace scene_detail;
        if (maxTaps <= 0 || !outImagePos || !outGain || instanceCount() == 0) return 0;
        struct Tap { Vec3 dir; float len; float g[kNumBands]; float e; };
        std::vector<Tap> taps;
        const float kEps = 1e-3f;
        const float refDist = std::max(length(source - listener), 1e-3f);
        const float maxDist = refDist * 8.0f + 50.0f;
        for (int i = 0; i < numRays; ++i) {
            Vec3 o = listener;
            Vec3 d = fibonacciSphereDir(i, numRays);
            const Vec3 d0 = d;  // 繝ｪ繧ｹ繝翫・縺ｫ螻翫￥譁ｹ蜷托ｼ育ｬｬ1繝ｬ繧ｰ・・            uint32_t rng = static_cast<uint32_t>(i) * 2654435761u + 12345u;
            float carry[kNumBands] = {1, 1, 1, 1, 1, 1};
            float totalLen = 0.0f;
            float remaining = maxDist;
            for (int bounce = 0; bounce < maxBounces; ++bounce) {
                const SceneHit hit = raycastClosest(o, d, remaining);
                if (!hit.hit) break;
                totalLen += hit.t;
                remaining -= hit.t;
                if (remaining <= kEps) break;
                const AcousticMaterial& mat = materialOf(hit.materialId);
                const Vec3 q = hit.point + hit.normal * 0.02f;
                float seg[kNumBands];
                computeTransmission(q, source, seg);
                Tap t;
                t.dir = d0;
                t.len = totalLen + length(source - q);
                float e = 0.0f;
                for (int b = 0; b < kNumBands; ++b) {
                    const float refl = clamp01(1.0f - mat.absorption[b] - mat.transmission[b]);
                    t.g[b] = carry[b] * refl * seg[b];
                    e += t.g[b];
                    carry[b] *= refl;
                }
                t.e = e / kNumBands;
                if (t.e > 1e-4f) taps.push_back(t);
                d = scatteredDir(d, hit.normal, scatteringMean(mat), rng);
                o = q;
            }
        }
        // 繧ｨ繝阪Ν繧ｮ繝ｼ髯埼・↓縲∵婿蜷代′霑代＞(>~25ﾂｰ縺ｧ蜷御ｸ縺ｨ縺ｿ縺ｪ縺・繧ゅ・縺ｯ縺ｾ縺ｨ繧√※荳贋ｽ阪ｒ謗｡繧九・        std::sort(taps.begin(), taps.end(), [](const Tap& a, const Tap& b) { return a.e > b.e; });
        Vec3 pickedDir[64];
        int n = 0;
        for (const Tap& t : taps) {
            if (n >= maxTaps || n >= 64) break;
            bool dup = false;
            for (int k = 0; k < n; ++k)
                if (dot(t.dir, pickedDir[k]) > 0.9f) { dup = true; break; }
            if (dup) continue;
            pickedDir[n] = t.dir;
            outImagePos[n] = listener + t.dir * t.len;
            // 蜀・Κ縺ｯ繧ｨ繝阪Ν繧ｮ繝ｼ・・efl = 1-ﾎｱ-ﾏ・繧る城℃繧ゑｼ峨√ち繝・・縺ｮ繧ｲ繧､繝ｳ縺ｯ謖ｯ蟷・〒霑斐☆
            //   ・・aterial.h 縺ｮ蜊倅ｽ崎ｦ冗ｴ・ｼ峨ゅ％縺薙ｒ邏騾壹＠縺ｫ縺吶ｋ縺ｨ蜿榊ｰ・′莠御ｹ励・繧灘ｰ上＆縺上↑繧翫・            //   鬮俶ｬ｡縺ｻ縺ｩ隱､蟾ｮ縺檎ｩ阪∩荳翫′縺｣縺ｦ蟆ｾ縺檎掠縺帙ｋ縲・            for (int b = 0; b < kNumBands; ++b)
                outGain[n * kNumBands + b] = std::sqrt(t.g[b]);
            ++n;
        }
        return n;
    }

    // 縲仙庄隕門喧縲双rigin 縺九ｉ dir 譁ｹ蜷代∈髀｡髱｢蜿榊ｰ・〒 maxBounces 蝗槭∪縺ｧ霑ｽ縺・・夐℃轤ｹ繧・outPoints 縺ｫ譖ｸ縺上・    //   outPoints[0]=origin縲∽ｻ･髯・蜿榊ｰ・せ縲∵怙蠕・邨らｫｯ・磯幕謾ｾ遨ｺ髢薙〒縺ｮ蛻ｰ驕皮せ or 譛邨ょ渚蟆・せ・峨・    //   霑斐ｊ蛟､=譖ｸ縺崎ｾｼ繧薙□轤ｹ謨ｰ縲ょ渚髻ｿ邨瑚ｷｯ(reflection path)繧・Unity 縺ｧ邱壽緒逕ｻ縺吶ｋ縺溘ａ縺ｮ蝨溷床縲・    int traceReflectionPath(const Vec3& origin, const Vec3& dir, float maxDist, int maxBounces,
                            Vec3* outPoints, int maxPoints) const {
        using namespace scene_detail;
        if (!outPoints || maxPoints < 2) return 0;
        Vec3 o = origin;
        Vec3 d = normalized(dir);
        int n = 0;
        outPoints[n++] = o;
        float remaining = maxDist;
        for (int b = 0; b < maxBounces && n < maxPoints; ++b) {
            const SceneHit hit = raycastClosest(o, d, remaining);
            if (!hit.hit) {
                if (n < maxPoints) outPoints[n++] = o + d * remaining;  // 髢区叛遨ｺ髢薙∈蟒ｶ髟ｷ
                return n;
            }
            if (n < maxPoints) outPoints[n++] = hit.point;
            remaining -= hit.t;
            if (remaining <= 1e-3f) return n;
            d = reflect(d, hit.normal);
            o = hit.point + hit.normal * 0.02f;  // 閾ｪ蟾ｱ繝偵ャ繝磯亟豁｢
        }
        return n;
    }

    // --- 繝ｪ繧ｹ繝翫・ / 髻ｳ貅舌・菫晄戟・・PI遘ｻ陦・谿ｵ1: docs/API_MIGRATION_PLAN.md・・--
    //
    // 縺薙ｌ縺ｾ縺ｧ listener/source 縺ｯ縲後け繧ｨ繝ｪ縺ｮ縺溘・縺ｫ蠑墓焚縺ｧ貂｡縺吶阪ｂ縺ｮ縺縺｣縺溘・    // 繝帙せ繝医′髻ｳ貅宣・蛻励ｒ謖√■縲・浹貅舌・繧薙Ν繝ｼ繝励☆繧九・繧ゅ・繧ｹ繝医・莉穂ｺ九↓縺ｪ縺｣縺ｦ縺・◆縺後・    // 縺薙ｌ縺ｯ SPEC ﾂｧ2 縺ｮ縲後お繝ｳ繧ｸ繝ｳ縺碁浹貅舌ｒ逋ｻ骭ｲ縺ｧ菫晄戟縺励∝・驛ｨ縺ｧ繝ｫ繝ｼ繝励☆繧九阪↓蜿阪☆繧九・    //
    // 縺薙％縺ｧ菫晄戟縺吶ｋ繧医≧縺ｫ縺励※縺翫￥縺ｨ縲∝ｾ梧ｮｵ縺ｮ af_Update・・逋ｺ縺ｧ蜈ｨ髻ｳ貅舌ｒ蝗槭☆・峨→
    // 繝ｯ繝ｼ繧ｫ繝ｼ繧ｹ繝ｬ繝・ラ蛹厄ｼ亥・蜉帙ｒ繧ｹ繝翫ャ繝励す繝ｧ繝・ヨ縺励※謚輔￡繧具ｼ峨′邏逶ｴ縺ｫ荵励ｋ縲・    // 谿ｵ1縺ｧ縺ｯ菫晄戟縺吶ｋ縺縺代〒縲∵里蟄倥け繧ｨ繝ｪ縺ｯ蠑墓焚迚医・縺ｾ縺ｾ・晄嫌蜍輔・荳蛻・､峨ｏ繧峨↑縺・・    struct SourceEntry {
        unsigned long long id = 0;
        Vec3 pos{};
        bool active = true;
    };

    void setListener(const Vec3& pos) { listenerPos_ = pos; }
    const Vec3& listenerPos() const { return listenerPos_; }

    // 髻ｳ貅舌ｒ逋ｻ骭ｲ/譖ｴ譁ｰ縺吶ｋ縲ょ酔縺・id 縺ｪ繧我ｽ咲ｽｮ縺縺第峩譁ｰ・域ｯ弱ヵ繝ｬ繝ｼ繝蜻ｼ縺ｰ繧後ｋ諠ｳ螳夲ｼ峨・    void setSource(unsigned long long id, const Vec3& pos) {
        for (auto& s : sources_) {
            if (s.id == id) { s.pos = pos; s.active = true; return; }
        }
        SourceEntry e;
        e.id = id;
        e.pos = pos;
        sources_.push_back(e);
    }

    void removeSource(unsigned long long id) {
        for (size_t i = 0; i < sources_.size(); ++i) {
            if (sources_[i].id == id) {
                sources_.erase(sources_.begin() + static_cast<long>(i));
                return;
            }
        }
    }

    void clearSources() { sources_.clear(); }

    int sourceCount() const { return static_cast<int>(sources_.size()); }

    // index 縺ｧ縺ｮ繧｢繧ｯ繧ｻ繧ｹ・亥・驛ｨ繝ｫ繝ｼ繝礼畑・峨らｯ・峇螟悶・蜴溽せ繧定ｿ斐☆縲・    const Vec3& sourcePos(int index) const {
        if (index >= 0 && index < sourceCount()) return sources_[static_cast<size_t>(index)].pos;
        static const Vec3 origin{};
        return origin;
    }

    unsigned long long sourceId(int index) const {
        if (index >= 0 && index < sourceCount()) return sources_[static_cast<size_t>(index)].id;
        return 0;
    }

    // id 竊・index縲りｦ九▽縺九ｉ縺ｪ縺代ｌ縺ｰ -1・・f_Get* 縺・id 縺ｧ蠑輔￥縺ｨ縺阪↓菴ｿ縺・ｼ峨・    int sourceIndexOf(unsigned long long id) const {
        for (size_t i = 0; i < sources_.size(); ++i)
            if (sources_[i].id == id) return static_cast<int>(i);
        return -1;
    }

    // ========================================================================
    // 繝舌ャ繝∵峩譁ｰ・・PI遘ｻ陦・谿ｵ2: docs/API_MIGRATION_PLAN.md・・    //
    // SPEC ﾂｧ4 縺ｮ繝輔Ξ繝ｼ繝蜀・ヱ繧､繝励Λ繧､繝ｳ繧・1 髢｢謨ｰ縺ｫ縺ｾ縺ｨ繧√ｋ縲・    //   蠖ｹ蜑ｲ1・井ｸｭ鬆ｻ蠎ｦ・・ 髻ｳ貅舌＃縺ｨ縺ｮ驕ｮ阡ｽ繝ｻ蝗樊釜繝ｻ騾城℃繝ｻ蛻ｰ譚･譁ｹ蜷・    //   蠖ｹ蜑ｲ2・井ｽ朱ｻ蠎ｦ・・ 繧ｨ繧ｳ繧ｰ繝ｩ繝・域ｮ矩涸・峨・譌ｩ譛溷渚蟆・・蝗樊釜莠梧ｬ｡髻ｳ貅・    //
    // 縺薙ｌ縺ｾ縺ｧ繝帙せ繝医′縲後←繧後ｒ縺・▽蜻ｼ縺ｶ縺九阪ｒ _erCountdown 遲峨・繧ｫ繧ｦ繝ｳ繧ｿ縺ｧ邂｡逅・＠縺ｦ縺・◆縺後・    // 縺昴ｌ縺ｯ繧ｨ繝ｳ繧ｸ繝ｳ縺ｮ遏･隴倥〒縺ゅ▲縺ｦ繝帙せ繝医↓鄂ｮ縺上∋縺阪ｂ縺ｮ縺ｧ縺ｯ縺ｪ縺・ｼ育ｧｻ讀阪・縺溘・縺ｫ譖ｸ縺咲峩縺励↓縺ｪ繧具ｼ峨・    // 縺薙％縺ｧ蜀・Κ繝ｬ繝ｼ繝医→縺励※謖√▽縲・    //
    // 邨先棡縺ｯ results_ 縺ｫ鄂ｮ縺阪“etSource*/getEarly* 遲峨〒隱ｭ繧縲・    // 谿ｵ5縺ｧ繝ｯ繝ｼ繧ｫ繝ｼ繧ｹ繝ｬ繝・ラ蛹悶☆繧九→縺阪√％縺ｮ髢｢謨ｰ縺斐→繝ｯ繝ｼ繧ｫ繝ｼ縺ｸ遘ｻ縺励※繝繝悶Ν繝舌ャ繝輔ぃ蛹悶☆繧九・    // ========================================================================

    struct UpdateConfig {
        // 蠖ｹ蜑ｲ縺斐→縺ｮ譖ｴ譁ｰ髢馴囈・医ヵ繝ｬ繝ｼ繝・峨・=豈弱ヵ繝ｬ繝ｼ繝縲・        int role1EveryN = 1;      // 驕ｮ阡ｽ繝ｻ蝗樊釜
        int role2EveryN = 4;      // 谿矩涸・磯㍾縺・・縺ｧ菴弱Ξ繝ｼ繝茨ｼ・        int earlyEveryN = 3;      // 譌ｩ譛溷渚蟆・        int diffSrcEveryN = 2;    // 蝗樊釜莠梧ｬ｡髻ｳ貅・        int catalogEveryN = 3;    // 繧ｨ繝・ず繧ｫ繧ｿ繝ｭ繧ｰ

        // 蠖ｹ蜑ｲ1
        int reflectionRays = 256;
        int reflectionBounces = 3;
        float directWeight = 1.0f;
        bool useReflections = true;

        // 繧ｨ繝・ず繧ｫ繧ｿ繝ｭ繧ｰ
        bool useEdgeCatalog = true;
        int edgeCatalogRes = 16;
        float edgeCatalogMaxDist = 40.0f;

        // 蠖ｹ蜑ｲ2: 繧ｨ繧ｳ繧ｰ繝ｩ繝
        bool enableReverb = true;
        int echogramBins = 100;
        float echogramBinSeconds = 0.01f;
        int echogramRays = 512;
        int echogramBounces = 24;
        float speedOfSound = 343.0f;
        float distanceRef = 0.0f;   // 0=蠎・′繧頑錐螟ｱ縺ｪ縺・
        // 蠖ｹ蜑ｲ2: 譌ｩ譛溷渚蟆・/ 蝗樊釜莠梧ｬ｡髻ｳ貅・        bool enableEarlyReflections = true;
        int earlyTaps = 4;
        int earlyRays = 512;
        int earlyBounces = 2;
        bool enableDiffractionSources = true;
        int diffSources = 3;
    };

    // 谿ｵ縺ｮ**菴咲嶌**繧偵★繧峨☆縺薙→縺ｯ隧ｦ縺励◆縺後∵ｸｬ縺｣縺溘ｉ蜉ｹ縺九↑縺九▲縺滂ｼ域怙謔ｪ 18.65 竊・19.35ms・峨・    //   螻ｱ縺ｮ豁｣菴薙・縲碁㍾縺・ｮｵ縺碁㍾縺ｪ繧九％縺ｨ縲阪〒縺ｯ縺ｪ縺上・*繧ｨ繧ｳ繧ｰ繝ｩ繝縺悟腰迢ｬ縺ｧ莠育ｮ励ｒ雜・∴繧九％縺ｨ**
    //   ・・12譛ｬﾃ・4蜿榊ｰ・ｒ 1 繝輔Ξ繝ｼ繝縺ｧ謦・▽縺ｮ縺ｧ縲∬ｵｰ繧句屓縺ｮ繝輔Ξ繝ｼ繝縺縺醍ｴ・14ms 縺九°繧具ｼ峨・    //   荳ｦ縺ｹ逶ｴ縺励※繧・14ms 縺ｮ蝪翫・豸医∴縺ｪ縺・・縺ｧ縲∽ｽ咲嶌縺壹ｉ縺励・蜈･繧後※縺・↑縺・・    void setUpdateConfig(const UpdateConfig& c) { cfg_ = c; }
    const UpdateConfig& updateConfig() const { return cfg_; }

    // 豈弱ヵ繝ｬ繝ｼ繝 1 逋ｺ縲ょ・驛ｨ繝ｬ繝ｼ繝医↓蠕薙▲縺ｦ蜷・ｽｹ蜑ｲ繧貞ｮ溯｡後＠縲∫ｵ先棡繧・results_ 縺ｫ鄂ｮ縺上・    void update(float /*dt*/) {
        const int n = sourceCount();
        results_.resize(n, cfg_);
        if (n == 0) return;

        // 髻ｳ貅蝉ｽ咲ｽｮ繧帝・蛻励∈・域里蟄倥・ multi 邉ｻ API 縺後・繧､繝ｳ繧ｿ驟榊・繧貞叙繧九◆繧・ｼ峨・        srcScratch_.resize(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i) srcScratch_[static_cast<size_t>(i)] = sources_[static_cast<size_t>(i)].pos;

        // a) 繧ｨ繝・ず繧ｫ繧ｿ繝ｭ繧ｰ・医Μ繧ｹ繝翫・荳ｭ蠢・・蜈ｨ髻ｳ貅仙・譛会ｼ峨ょ屓謚倥′菴ｿ縺・・        if (cfg_.useEdgeCatalog) {
            if (--catalogCountdown_ <= 0) {
                catalogCountdown_ = (cfg_.catalogEveryN > 0) ? cfg_.catalogEveryN : 1;
                buildEdgeCatalog(listenerPos_, cfg_.edgeCatalogRes, cfg_.edgeCatalogMaxDist);
            }
        } else {
            clearEdgeCatalog();
        }

        // b) 蠖ｹ蜑ｲ1: 驕ｮ阡ｽ繝ｻ蝗樊釜繝ｻ騾城℃繝ｻ蛻ｰ譚･譁ｹ蜷托ｼ磯浹貅舌＃縺ｨ・峨・        if (--role1Countdown_ <= 0) {
            role1Countdown_ = (cfg_.role1EveryN > 0) ? cfg_.role1EveryN : 1;
            runRole1(n);
        }

        // c) 蠖ｹ蜑ｲ2: 譌ｩ譛溷渚蟆・ｼ磯浹貅舌＃縺ｨ繝ｻ菴弱Ξ繝ｼ繝茨ｼ峨・        if (cfg_.enableEarlyReflections) {
            if (--earlyCountdown_ <= 0) {
                earlyCountdown_ = (cfg_.earlyEveryN > 0) ? cfg_.earlyEveryN : 1;
                runEarlyReflections(n);
            }
        }

        // d) 蠖ｹ蜑ｲ2: 蝗樊釜莠梧ｬ｡髻ｳ貅撰ｼ磯浹貅舌＃縺ｨ繝ｻ菴弱Ξ繝ｼ繝茨ｼ峨・        if (cfg_.enableDiffractionSources) {
            if (--diffSrcCountdown_ <= 0) {
                diffSrcCountdown_ = (cfg_.diffSrcEveryN > 0) ? cfg_.diffSrcEveryN : 1;
                runDiffractionSources(n);
            }
        }

        // e) 蠖ｹ蜑ｲ2: 繧ｨ繧ｳ繧ｰ繝ｩ繝・亥・髻ｳ貅舌∪縺ｨ繧√※繝ｻ譛菴弱Ξ繝ｼ繝茨ｼ峨・        if (cfg_.enableReverb) {
            if (--role2Countdown_ <= 0) {
                role2Countdown_ = (cfg_.role2EveryN > 0) ? cfg_.role2EveryN : 1;
                runEchogram(n);
            }
        }
    }

    // --- 邨先棡蜿門ｾ暦ｼ亥燕蝗・update 縺ｶ繧難ｼ・--
    // index 縺ｯ sourceIndexOf(id) 縺ｧ蠑輔￥縲らｯ・峇螟悶・菴輔ｂ縺励↑縺・ｼ亥他縺ｳ謇九・繝舌ャ繝輔ぃ縺ｯ荳榊､会ｼ峨・
    void getSourceOcclusion(int index, float* out6) const {
        if (!out6 || !validResult(index)) return;
        const float* src = &results_.bands[static_cast<size_t>(index) * kNumBands];
        for (int b = 0; b < kNumBands; ++b) out6[b] = src[b];
    }

    float getSourceOcclusionScalar(int index) const {
        return validResult(index) ? results_.occ[static_cast<size_t>(index)] : 0.0f;
    }

    void getSourceArrivalDir(int index, float* out3) const {
        if (!out3 || !validResult(index)) return;
        const float* d = &results_.dir[static_cast<size_t>(index) * 3];
        out3[0] = d[0]; out3[1] = d[1]; out3[2] = d[2];
    }

    // 譌ｩ譛溷渚蟆・ち繝・・縲よ嶌縺崎ｾｼ繧薙□譛ｬ謨ｰ繧定ｿ斐☆縲・    int getEarlyReflections(int index, Vec3* outPos, float* outGain6, int maxTaps) const {
        if (!outPos || !outGain6 || maxTaps <= 0 || !validResult(index)) return 0;
        const int cap = std::min(maxTaps, results_.earlyCap);
        const int n = std::min(cap, results_.earlyCount[static_cast<size_t>(index)]);
        const size_t base = static_cast<size_t>(index) * results_.earlyCap;
        for (int t = 0; t < n; ++t) {
            outPos[t] = results_.earlyPos[base + static_cast<size_t>(t)];
            const float* g = &results_.earlyGain[(base + static_cast<size_t>(t)) * kNumBands];
            for (int b = 0; b < kNumBands; ++b) outGain6[t * kNumBands + b] = g[b];
        }
        return n;
    }

    // 蝗樊釜莠梧ｬ｡髻ｳ貅舌よ嶌縺崎ｾｼ繧薙□譛ｬ謨ｰ繧定ｿ斐☆縲・    int getDiffractionSources(int index, Vec3* outPos, float* outGain, int maxSrc) const {
        if (!outPos || !outGain || maxSrc <= 0 || !validResult(index)) return 0;
        const int cap = std::min(maxSrc, results_.diffCap);
        const int n = std::min(cap, results_.diffCount[static_cast<size_t>(index)]);
        const size_t base = static_cast<size_t>(index) * results_.diffCap;
        for (int t = 0; t < n; ++t) {
            outPos[t] = results_.diffPos[base + static_cast<size_t>(t)];
            outGain[t] = results_.diffGain[base + static_cast<size_t>(t)];
        }
        return n;
    }

    // 蟶ｯ蝓溷挨繧ｨ繧ｳ繧ｰ繝ｩ繝縲ＯumBins*kNumBands 隕∫ｴ繧呈嶌縺上よ嶌縺代◆繝薙Φ謨ｰ繧定ｿ斐☆縲・    int getEchogramBands(float* outBins, int numBins) const {
        if (!outBins || numBins <= 0 || results_.echogramBins <= 0) return 0;
        const int n = std::min(numBins, results_.echogramBins);
        for (int i = 0; i < n * kNumBands; ++i) outBins[i] = results_.echogram[static_cast<size_t>(i)];
        return n;
    }

    // --- 蜿ら・ ---
    int instanceCount() const { return static_cast<int>(instances_.size()); }
    int materialCount() const { return static_cast<int>(materials_.size()); }

private:
    // update() 縺檎ｽｮ縺冗ｵ先棡縲よｮｵ5縺ｧ縺薙ｌ繧偵ム繝悶Ν繝舌ャ繝輔ぃ蛹悶☆繧九・    struct Results {
        int count = 0;
        std::vector<float> occ;     // [count] 驕ｮ阡ｽ繧ｹ繧ｫ繝ｩ
        std::vector<float> bands;   // [count*6] 蟶ｯ蝓溷挨逕溷ｭ・        std::vector<float> dir;     // [count*3] 蛻ｰ譚･譁ｹ蜷・
        int earlyCap = 0;
        std::vector<Vec3> earlyPos;    // [count*earlyCap]
        std::vector<float> earlyGain;  // [count*earlyCap*6]
        std::vector<int> earlyCount;   // [count]

        int diffCap = 0;
        std::vector<Vec3> diffPos;     // [count*diffCap]
        std::vector<float> diffGain;   // [count*diffCap]
        std::vector<int> diffCount;    // [count]

        int echogramBins = 0;
        std::vector<float> echogram;   // [echogramBins*6]

        void resize(int n, const UpdateConfig& c) {
            const int eCap = (c.earlyTaps > 0) ? c.earlyTaps : 1;
            const int dCap = (c.diffSources > 0) ? c.diffSources : 1;
            if (count == n && earlyCap == eCap && diffCap == dCap &&
                echogramBins == c.echogramBins) return;
            count = n;
            earlyCap = eCap;
            diffCap = dCap;
            echogramBins = c.echogramBins;
            occ.assign(static_cast<size_t>(n), 0.0f);
            bands.assign(static_cast<size_t>(n) * kNumBands, 1.0f);
            dir.assign(static_cast<size_t>(n) * 3, 0.0f);
            earlyPos.assign(static_cast<size_t>(n) * eCap, Vec3(0, 0, 0));
            earlyGain.assign(static_cast<size_t>(n) * eCap * kNumBands, 0.0f);
            earlyCount.assign(static_cast<size_t>(n), 0);
            diffPos.assign(static_cast<size_t>(n) * dCap, Vec3(0, 0, 0));
            diffGain.assign(static_cast<size_t>(n) * dCap, 0.0f);
            diffCount.assign(static_cast<size_t>(n), 0);
            echogram.assign(static_cast<size_t>(std::max(0, c.echogramBins)) * kNumBands, 0.0f);
        }
    };

    bool validResult(int index) const {
        return index >= 0 && index < results_.count;
    }

    void runRole1(int n) {
        if (cfg_.useReflections) {
            occlusionReflectedMulti(listenerPos_, srcScratch_.data(), n,
                                    results_.occ.data(), results_.bands.data(),
                                    results_.dir.data(), cfg_.directWeight,
                                    cfg_.reflectionRays, cfg_.reflectionBounces);
        } else {
            // 蜿榊ｰ・ｒ菴ｿ繧上↑縺・ｴ蜷医・逶ｴ謗･邨瑚ｷｯ縺ｮ縺ｿ・磯城℃竓募屓謚假ｼ峨・            //   笘・omputeDirectSoft 縺ｨ**蜷後§隕冗ｴ・*繧帝壹☆縲・            //     莉･蜑阪・縺薙％縺ｧ computeTransmission・医お繝阪Ν繧ｮ繝ｼ・峨→
            //     computeDiffraction・域険蟷・ｼ峨ｒ逶ｴ謗･ max 縺ｧ豈斐∋縺ｦ縺・◆縲ょ腰菴阪′驕輔≧縺ｮ縺ｧ
            //     豈碑ｼ・↓縺ｪ縺｣縺ｦ縺翫ｉ縺壹・城℃縺御ｺ御ｹ励・繧灘ｰ上＆縺剰ｩ穂ｾ｡縺輔ｌ縺ｦ蝗樊釜縺悟ｸｸ縺ｫ蜍昴■縲・            //     500Hz 莉･荳翫′譚占ｳｪ縺ｫ髢｢菫ゅ↑縺丞酔縺伜､・亥ｮ滓ｸｬ 0.01156・峨↓蠑ｵ繧贋ｻ倥＞縺ｦ縺・◆縲・            //     繝昴・繧ｿ繝ｫ繧りｦ九※縺・↑縺九▲縺溘・縺ｧ縲・哩縺倥◆髢句哨縺ｧ繧ょ屓謚倥・蠎翫′谿九▲縺ｦ縺・◆縲・            for (int i = 0; i < n; ++i) {
                float g[kNumBands];
                computeDirectSoft(listenerPos_, srcScratch_[static_cast<size_t>(i)],
                                  g, 8, 0.4f, nullptr);
                float sum = 0.0f;
                for (int b = 0; b < kNumBands; ++b) {
                    results_.bands[static_cast<size_t>(i) * kNumBands + b] = g[b];
                    sum += g[b];
                }
                results_.occ[static_cast<size_t>(i)] = 1.0f - sum / kNumBands;
            }
        }
    }

    void runEarlyReflections(int n) {
        const int cap = results_.earlyCap;
        for (int i = 0; i < n; ++i) {
            const size_t base = static_cast<size_t>(i) * cap;
            const int got = computeEarlyReflections(
                listenerPos_, srcScratch_[static_cast<size_t>(i)],
                &results_.earlyPos[base], &results_.earlyGain[base * kNumBands],
                cap, cfg_.earlyRays, cfg_.earlyBounces);
            results_.earlyCount[static_cast<size_t>(i)] = got;
        }
    }

    void runDiffractionSources(int n) {
        const int cap = results_.diffCap;
        for (int i = 0; i < n; ++i) {
            const size_t base = static_cast<size_t>(i) * cap;
            const int got = computeDiffractionSources(
                listenerPos_, srcScratch_[static_cast<size_t>(i)],
                &results_.diffPos[base], &results_.diffGain[base], cap);
            results_.diffCount[static_cast<size_t>(i)] = got;
        }
    }

    // 繝昴・繧ｿ繝ｫ繧・*莉ｮ縺ｮ繝ｪ繧ｹ繝翫・**縺ｫ縺励※縲∝･･蛛ｴ縺ｧ閨槭％縺医※縺・ｋ繧ゅ・繧偵お繧ｳ繧ｰ繝ｩ繝縺ｸ雜ｳ縺吶・    //
    // 縺ｪ縺懆ｦ√ｋ縺具ｼ亥ｮ滓ｸｬ・・
    //   謇峨ｒ髢峨§繧九→縲∝･･縺ｮ驛ｨ螻九・譚占ｳｪ繧貞､峨∴縺ｦ繧ゅ％縺｡繧峨・繧ｨ繧ｳ繧ｰ繝ｩ繝縺・**1繝薙ャ繝医ｂ螟峨ｏ繧峨↑縺・*
    //     髢峨・髻ｿ縺・0.07049 / 髢峨・蜷ｸ縺・0.07049   竊・螳悟・縺ｫ蜷後§
    //     髢九・髻ｿ縺・128.59  / 髢九・蜷ｸ縺・73.33     竊・1.75蛟榊虚縺・    //   髢峨§縺ｦ縺・ｋ縺ｨ繝ｬ繧､縺梧演縺ｧ霍ｳ縺ｭ霑斐＆繧後∝･･縺ｮ驛ｨ螻九ｒ荳蠎ｦ繧りｨｪ繧後↑縺・◆繧√・    //   螻翫＞縺ｦ縺・ｋ縺ｮ縺ｯ逶ｴ邱壹・騾城℃縺縺代〒縲√◎繧後・谿矩涸繧呈戟縺溘↑縺・・    //   縺縺九ｉ縲悟･･縺ｧ魑ｴ縺｣縺ｦ縺・ｋ髻ｳ縺梧ｼ上ｌ縺ｦ縺上ｋ縲阪〒縺ｯ縺ｪ縺上悟｣√・蜷代％縺・・轤ｹ髻ｳ貅舌阪↓閨槭％縺医ｋ縲・    //
    // 菴輔ｒ縺吶ｋ縺・
    //   繝昴・繧ｿ繝ｫ縺ｮ菴咲ｽｮ縺九ｉ**螂･蛛ｴ縺ｮ蜊顔帥縺縺・*縺ｸ繝ｬ繧､繧呈鋳縺阪√◎縺薙〒閨槭％縺医ｋ驥上ｒ貂ｬ繧九・    //   縺昴ｌ縺ｫ (1竏断)ﾂｷﾏЮ隕・▲縺ｦ縺・ｋ繧ゅ・ 繧呈寺縺代※縲√・繝ｼ繧ｿ繝ｫ縺ｮ菴咲ｽｮ縺九ｉ謇句燕縺ｸ豬√☆縲・    //
    //   笘・濠逅・↓髯舌ｋ縺ｮ縺ｯ縲∝・譁ｹ菴阪↓謦偵￥縺ｨ謇句燕縺ｮ驛ｨ螻九ｂ諡ｾ縺・√◎繧後ｒ謇句燕縺ｸ霑斐＠縺ｦ縺励∪縺・◆繧・    //     ・郁・蛻・・髻ｳ繧定・蛻・〒諡ｾ縺・ｼ晄ｮ矩涸縺御ｺ碁㍾縺ｫ謗帙°繧具ｼ峨・    //   笘・1竏断) 繧呈寺縺代ｋ縺ｮ縺ｯ莠碁㍾險井ｸ翫ｒ驕ｿ縺代ｋ縺溘ａ縲る幕縺・※縺・ｋ蛻・・繝ｬ繧､縺ｯ謇句燕縺ｮ繧ｨ繧ｳ繧ｰ繝ｩ繝縺・    //     譌｢縺ｫ謌ｸ蜿｣繧帝壹▲縺ｦ諡ｾ縺｣縺ｦ縺・ｋ縲ゅ・繝ｼ繧ｿ繝ｫ縺瑚ｶｳ縺吶・縺ｯ**譚ｿ繧呈栢縺代※縺上ｋ蛻・□縺・*縲・    //     髢峨§縺ｦ縺・ｋ縺ｨ縺阪□縺大柑縺阪・幕縺上↓縺､繧後※蠖ｹ蜑ｲ縺後Ξ繧､蛛ｴ縺ｸ遘ｻ繧九ょ粋險医・騾｣邯壹・    void addPortalEchogram(const Portal& pt, const Vec3* sources, int count,
                           float* outBins, int numBins, float binSeconds,
                           float speedOfSound, int numRays, int maxBounces,
                           float distanceRef) const {
        using namespace scene_detail;
        if (!outBins || numBins <= 0 || count <= 0 || numRays <= 0) return;

        // 髢句哨邇・る幕縺・※縺・ｋ蛻・・謇句燕縺ｮ繝ｬ繧､縺梧里縺ｫ諡ｾ縺｣縺ｦ縺・ｋ縺ｮ縺ｧ縲√◎縺ｮ陬憺寔蜷医□縺題ｶｳ縺吶・        float f[kNumBands]; Vec3 cp(0, 0, 0);
        if (!portalOpenBands(pt, listenerPos_, sources[0], f, &cp)) return;

        // 蝪槭＞縺ｧ縺・ｋ繧ゅ・縺ｮ騾城℃縲ゅ・繝ｼ繧ｿ繝ｫ繧呈ｳ慕ｷ壽婿蜷代↓遏ｭ縺剰ｲｫ縺・※貂ｬ繧九・        Vec3 nrm = normalized(cross(pt.axisU, pt.axisV));
        if (dot(nrm, listenerPos_ - pt.center) < 0.0f) nrm = Vec3(-nrm.x, -nrm.y, -nrm.z);
        float tau[kNumBands];
        computeTransmission(pt.center + nrm * 0.5f, pt.center - nrm * 0.5f, tau);

        float scale[kNumBands];
        bool any = false;
        for (int b = 0; b < kNumBands; ++b) {
            scale[b] = (1.0f - clamp01(f[b])) * clamp01(tau[b]);
            if (scale[b] > 1e-7f) any = true;
        }
        if (!any) return;                       // 譚ｿ縺悟ｮ悟・縺ｫ驕ｮ繧具ｼ丞・髢九〒譌｢縺ｫ諡ｾ縺医※縺・ｋ

        // 繝昴・繧ｿ繝ｫ竊偵Μ繧ｹ繝翫・縺ｮ閼壹る≦蟒ｶ縺ｨ蠎・′繧頑錐螟ｱ繧貞ｾ後〒雜ｳ縺吶・        const Vec3 origin = pt.center;
        const float legLen = std::max(length(listenerPos_ - origin), 1e-3f);
        const float invC = (speedOfSound > 1e-3f) ? 1.0f / speedOfSound : 0.0f;
        const float invBin = (binSeconds > 1e-6f) ? 1.0f / binSeconds : 0.0f;
        auto spreadEnergy = [distanceRef](float d) -> float {
            if (distanceRef <= 0.0f) return 1.0f;
            const float a = distanceRef / std::max(d, distanceRef);
            return a * a;
        };
        const float legSpread = spreadEnergy(legLen);

        auto addBin = [&](float distFromSource, const float* e6, float extra) {
            const float total = distFromSource + legLen;      // 繝昴・繧ｿ繝ｫ縺ｾ縺ｧ・九％縺｡繧峨∈
            const int k = static_cast<int>(total * invC * invBin);
            if (k < 0 || k >= numBins) return;
            float* dst = outBins + static_cast<size_t>(k) * kNumBands;
            for (int b = 0; b < kNumBands; ++b) {
                const float v = e6[b] * extra * scale[b] * legSpread;
                if (v > 0.0f) dst[b] += v;
            }
        };

        // 笘・峩謗･謌仙・・亥･･縺ｮ髻ｳ貅・竊・繝昴・繧ｿ繝ｫ・峨・**雜ｳ縺輔↑縺・*縲・        //   縺昴ｌ縺ｯ譌｢縺ｫ繝昴・繧ｿ繝ｫ縺ｮ繧ｿ繝・・縺碁ｳｴ繧峨＠縺ｦ縺・ｋ縲ゅお繧ｳ繧ｰ繝ｩ繝縺ｫ繧ょ・繧後ｋ縺ｨ縲・        //   蟆代＠縺壹ｌ縺滄≦蟒ｶ縺ｫ繧ゅ≧1縺､譌ｩ縺・芦驕斐′遶九■縲・*髮｢謨｣逧・↑繧ｨ繧ｳ繝ｼ・晞浹縺後ム繝悶ｋ**縲・        //   縺輔ｉ縺ｫ譌ｩ譛溘↓繧ｨ繝阪Ν繧ｮ繝ｼ繧貞叙繧峨ｌ縺ｦ蟆ｾ縺ｮ豁｣隕丞喧縺ｧ蠕悟濠縺御ｸ九′繧翫・        //   縲梧ｮ矩涸縺梧ｸ帙▲縺溘阪→閨槭％縺医ｋ・・++ 邨瑚ｷｯ縺ｯ蟆ｾ繧偵お繧ｳ繧ｰ繝ｩ繝縺九ｉ邨・∩逶ｴ縺吶・縺ｧ鬘戊送・峨・        //   縺薙％縺碁°縺ｶ縺ｹ縺阪・**螂･縺ｮ驛ｨ螻九・蜿榊ｰ・□縺・*縲・
        // 螂･蛛ｴ縺ｮ蜊顔帥縺縺代∈謦偵＞縺ｦ縲∝･･縺ｮ驛ｨ螻九・蜿榊ｰ・ｒ諡ｾ縺・・        const float kEps = 1e-3f;
        float refDist = 1e-3f;
        for (int j = 0; j < count; ++j)
            refDist = std::max(refDist, length(sources[j] - origin));
        const float maxDist = refDist * 8.0f + 50.0f;
        for (int i = 0; i < numRays; ++i) {
            Vec3 d = fibonacciSphereDir(i, numRays);
            if (dot(d, nrm) > 0.0f) d = Vec3(-d.x, -d.y, -d.z);   // 螂･蛛ｴ縺ｸ謚倥ｊ霑斐☆
            Vec3 o = origin - nrm * 0.05f;                        // 譚ｿ縺ｮ螂･蛛ｴ縺九ｉ蜃ｺ縺・            uint32_t rng = static_cast<uint32_t>(i) * 2654435761u + 777u;
            float carry[kNumBands] = {1, 1, 1, 1, 1, 1};
            float totalLen = 0.0f;
            float remaining = maxDist;
            for (int bounce = 0; bounce < maxBounces; ++bounce) {
                const SceneHit hit = raycastClosest(o, d, remaining);
                if (!hit.hit) break;
                totalLen += hit.t;
                remaining -= hit.t;
                if (remaining <= kEps) break;
                const AcousticMaterial& mat = materialOf(hit.materialId);
                const Vec3 q = hit.point + hit.normal * 0.02f;
                for (int j = 0; j < count; ++j) {
                    float seg[kNumBands];
                    computeTransmission(q, sources[j], seg);
                    const float pathLen = totalLen + length(sources[j] - q);
                    float e[kNumBands];
                    for (int b = 0; b < kNumBands; ++b) {
                        const float refl = clamp01(1.0f - mat.absorption[b] - mat.transmission[b]);
                        e[b] = carry[b] * refl * seg[b] / static_cast<float>(numRays);
                    }
                    addBin(pathLen, e, spreadEnergy(pathLen));
                }
                for (int b = 0; b < kNumBands; ++b) {
                    const float refl = clamp01(1.0f - mat.absorption[b] - mat.transmission[b]);
                    carry[b] *= refl;
                }
                d = scatteredDir(d, hit.normal, scatteringMean(mat), rng);
                o = q;
            }
        }
    }

    void runEchogram(int n) {
        if (results_.echogramBins <= 0) return;
        computeEchogramBands(listenerPos_, srcScratch_.data(), n,
                             results_.echogram.data(), results_.echogramBins,
                             cfg_.echogramBinSeconds, cfg_.speedOfSound,
                             cfg_.echogramRays, cfg_.echogramBounces, cfg_.distanceRef);
        // 髢峨§縺滄幕蜿｣縺ｮ蜷代％縺・・髻ｿ縺阪ｒ雜ｳ縺吶ゅΞ繧､謨ｰ縺ｯ關ｽ縺ｨ縺励※繧医＞ 笏笏
        //   隕√ｋ縺ｮ縺ｯ蟆ｾ縺ｮ蛹・ｵ｡縺ｧ縺ゅ▲縺ｦ邏ｰ縺九＞讒矩縺ｧ縺ｯ縺ｪ縺・・縺ｧ・域悽菴薙・ 1/4・峨・        for (const Portal& pt : portals_) {
            if (!pt.active) continue;
            addPortalEchogram(pt, srcScratch_.data(), n,
                              results_.echogram.data(), results_.echogramBins,
                              cfg_.echogramBinSeconds, cfg_.speedOfSound,
                              std::max(cfg_.echogramRays / 4, 32), cfg_.echogramBounces,
                              cfg_.distanceRef);
        }
    }

    UpdateConfig cfg_;
    Results results_;
    std::vector<Vec3> srcScratch_;
    int role1Countdown_ = 1;
    int role2Countdown_ = 1;
    int earlyCountdown_ = 1;
    int diffSrcCountdown_ = 1;
    int catalogCountdown_ = 1;

    bool validInstance(int id) const { return id >= 0 && id < instanceCount(); }

    int clampMaterialId(int id) const {
        if (materials_.empty()) return 0;
        if (id < 0 || id >= materialCount()) return 0;
        return id;
    }

    // materialId 縺九ｉ譚占ｳｪ繧貞叙繧九ゅユ繝ｼ繝悶Ν遨ｺ/遽・峇螟悶↑繧画里螳壼｣√ｒ霑斐☆・亥ｮ牙・蛛ｴ・峨・    const AcousticMaterial& materialOf(int id) const {
        if (id >= 0 && id < materialCount()) return materials_[id];
        static const AcousticMaterial fallback = AcousticMaterial::defaultWall();
        return fallback;
    }

    // 笏笏 BVH-of-OBB・・road-phase / TLAS・峨ゅう繝ｳ繧ｹ繧ｿ繝ｳ繧ｹ螟画峩縺ｧ lazy 縺ｫ蜀肴ｧ狗ｯ会ｼ・[dynamic-ray-architecture]] 霑ｽ蜉繝ｭ繝・け竭｡・峨や楳笏
    struct BvhNode {
        Aabb bounds;
        int leftFirst;  // 蜀・Κ繝弱・繝・蟾ｦ蟄進ndex / 闡・bvhOrder_ 縺ｮ髢句ｧ喫ndex
        int count;      // 0=蜀・Κ繝弱・繝・/ >0=闡会ｼ医う繝ｳ繧ｹ繧ｿ繝ｳ繧ｹ謨ｰ・・    };

    // OBB 縺ｮ繝ｯ繝ｼ繝ｫ繝芽ｻｸ荳ｦ陦悟｢・阜・亥推霆ｸ縺ｸ蜊翫し繧､繧ｺ繧貞ｰ・ｽｱ縺励※閹ｨ繧峨∪縺帙ｋ・峨・    static Aabb obbWorldAabb(const Obb& b) {
        const Vec3 e(
            std::fabs(b.axisX.x) * b.halfExtents.x + std::fabs(b.axisY.x) * b.halfExtents.y + std::fabs(b.axisZ.x) * b.halfExtents.z,
            std::fabs(b.axisX.y) * b.halfExtents.x + std::fabs(b.axisY.y) * b.halfExtents.y + std::fabs(b.axisZ.y) * b.halfExtents.z,
            std::fabs(b.axisX.z) * b.halfExtents.x + std::fabs(b.axisY.z) * b.halfExtents.y + std::fabs(b.axisZ.z) * b.halfExtents.z);
        Aabb a;
        a.min = b.center - e;
        a.max = b.center + e;
        return a;
    }

    void ensureBvh() const {
        if (!bvhDirty_) return;
        buildBvh();
        bvhDirty_ = false;
    }

    void buildBvh() const {
        bvhNodes_.clear();
        bvhOrder_.clear();
        for (int i = 0; i < instanceCount(); ++i)
            if (instances_[i].active) bvhOrder_.push_back(i);
        const int n = static_cast<int>(bvhOrder_.size());
        if (n == 0) return;
        bvhNodes_.reserve(static_cast<size_t>(2 * n));
        bvhNodes_.push_back(BvhNode{});
        buildNode(0, 0, n);
    }

    void buildNode(int nodeIdx, int start, int count) const {
        // 繝弱・繝牙｢・阜・晉ｯ・峇蜀・う繝ｳ繧ｹ繧ｿ繝ｳ繧ｹ縺ｮ繝ｯ繝ｼ繝ｫ繝陰ABB蜷井ｽｵ縲・        Aabb b = obbWorldAabb(instances_[bvhOrder_[start]].obb);
        for (int k = 1; k < count; ++k) {
            const Aabb a = obbWorldAabb(instances_[bvhOrder_[start + k]].obb);
            b.min = Vec3(std::min(b.min.x, a.min.x), std::min(b.min.y, a.min.y), std::min(b.min.z, a.min.z));
            b.max = Vec3(std::max(b.max.x, a.max.x), std::max(b.max.y, a.max.y), std::max(b.max.z, a.max.z));
        }
        bvhNodes_[nodeIdx].bounds = b;
        if (count <= 2) {  // 闡・            bvhNodes_[nodeIdx].leftFirst = start;
            bvhNodes_[nodeIdx].count = count;
            return;
        }
        // 譛髟ｷ霆ｸ縺ｧ荳ｭ螟ｮ蛟､蛻・牡縲・        const Vec3 ext = b.max - b.min;
        const int axis = (ext.x > ext.y) ? (ext.x > ext.z ? 0 : 2) : (ext.y > ext.z ? 1 : 2);
        const int mid = start + count / 2;
        auto centroidOnAxis = [&](int idx) {
            const Aabb a = obbWorldAabb(instances_[idx].obb);
            const Vec3 c = a.min + a.max;  // 2ﾃ鈴㍾蠢・ｼ域ｯ碑ｼ・・縺ｿ縺ｪ縺ｮ縺ｧ菫よ焚荳崎ｦ・ｼ・            return axis == 0 ? c.x : (axis == 1 ? c.y : c.z);
        };
        std::nth_element(bvhOrder_.begin() + start, bvhOrder_.begin() + mid, bvhOrder_.begin() + start + count,
                         [&](int lhs, int rhs) { return centroidOnAxis(lhs) < centroidOnAxis(rhs); });
        const int left = static_cast<int>(bvhNodes_.size());
        bvhNodes_.push_back(BvhNode{});
        bvhNodes_.push_back(BvhNode{});
        bvhNodes_[nodeIdx].leftFirst = left;
        bvhNodes_[nodeIdx].count = 0;
        buildNode(left, start, mid - start);
        buildNode(left + 1, mid, start + count - mid);
    }

    // 笏笏 繧ｨ繝・ず繧ｫ繧ｿ繝ｭ繧ｰ陬懷勧 笏笏
    // 繧ｭ繝･繝ｼ繝悶・繝・・縺ｮ繝・け繧ｻ繝ｫ譁ｹ蜷托ｼ磯擇f=0..5:+X,-X,+Y,-Y,+Z,-Z縲「,v竏・-1,1]・峨・    static Vec3 cubeTexelDir(int face, float u, float v) {
        switch (face) {
            case 0:  return normalized(Vec3(1.0f, -v, -u));   // +X
            case 1:  return normalized(Vec3(-1.0f, -v, u));   // -X
            case 2:  return normalized(Vec3(u, 1.0f, v));     // +Y
            case 3:  return normalized(Vec3(u, -1.0f, -v));   // -Y
            case 4:  return normalized(Vec3(u, -v, 1.0f));    // +Z
            default: return normalized(Vec3(-u, -v, -1.0f));  // -Z
        }
    }

    // 繧ｷ繝ｫ繧ｨ繝・ヨ縺ｮ繝偵ャ繝育せ繧偵√◎縺ｮ邂ｱ縺ｮ譛霑大ｍ遞懃ｷ壹↓ snap 縺励※繧ｫ繧ｿ繝ｭ繧ｰ縺ｸ・郁ｿ第磁驥崎､・・髯､螟厄ｼ峨・    void addCatalogEdge(int instanceIdx, const Vec3& hitPoint) const {
        if (instanceIdx < 0 || instanceIdx >= instanceCount()) return;
        const Obb& b = instances_[instanceIdx].obb;
        const Vec3 ax[3] = {b.axisX, b.axisY, b.axisZ};
        const float h[3] = {std::max(b.halfExtents.x, 1e-4f), std::max(b.halfExtents.y, 1e-4f),
                            std::max(b.halfExtents.z, 1e-4f)};
        const Vec3 lp = obbToLocalPoint(hitPoint, b);
        const float lc[3] = {lp.x, lp.y, lp.z};
        // face霆ｸ=|lc|/h 譛螟ｧ縲《ide霆ｸ=谺｡轤ｹ縲‘dge霆ｸ=谿九ｊ縲・        int faceAxis = 0;
        float mx = std::fabs(lc[0]) / h[0];
        for (int k = 1; k < 3; ++k) { const float r = std::fabs(lc[k]) / h[k]; if (r > mx) { mx = r; faceAxis = k; } }
        const int a1 = (faceAxis + 1) % 3, a2 = (faceAxis + 2) % 3;
        const int sideAxis = (std::fabs(lc[a1]) / h[a1] >= std::fabs(lc[a2]) / h[a2]) ? a1 : a2;
        const int edgeAxis = (sideAxis == a1) ? a2 : a1;
        const float faceSign = lc[faceAxis] >= 0.0f ? 1.0f : -1.0f;
        const float sideSign = lc[sideAxis] >= 0.0f ? 1.0f : -1.0f;
        // 遞懃ｷ壹ｒ螟門髄縺・2髱｢豕慕ｷ壹・蟇ｾ隗・縺ｫ蟆代＠閹ｨ繧峨∪縺帙√お繝・ず竊帝浹貅舌′邂ｱ縺ｫ蜀咲ｪ∝・縺励↑縺・ｈ縺・↓縲・        const Vec3 outward = normalized(ax[faceAxis] * faceSign + ax[sideAxis] * sideSign);
        const float margin = 0.15f;
        const Vec3 base = b.center + ax[faceAxis] * (faceSign * h[faceAxis])
                        + ax[sideAxis] * (sideSign * h[sideAxis]) + outward * margin;
        DiffEdge e;
        e.p0 = base - ax[edgeAxis] * h[edgeAxis];
        e.p1 = base + ax[edgeAxis] * h[edgeAxis];
        e.edgeDir = ax[edgeAxis];
        e.refTangent = ax[sideAxis] * (-sideSign);  // 0髱｢(face髱｢)謗･邱夲ｼ昴お繝・ず縺九ｉ螟悶∈
        e.n = 1.5f;
        e.instance = instanceIdx;  // 閾ｪ蟾ｱ髯､螟也畑
        const Vec3 mid = (e.p0 + e.p1) * 0.5f;
        for (const DiffEdge& x : edgeCatalog_)
            if (length((x.p0 + x.p1) * 0.5f - mid) < 0.05f) return;  // 蜷御ｸ遞懃ｷ・        edgeCatalog_.push_back(e);
    }

    std::vector<AcousticMaterial> materials_;  // 譚占ｳｪ繝・・繝悶Ν・医う繝ｳ繧ｹ繧ｿ繝ｳ繧ｹ縺・matId 縺ｧ蜿ら・・・    std::vector<MeshGeometry> meshes_;         // 蠖｢迥ｶ(BLAS)縲よｷｻ蟄励′ geomId縲りｩｰ繧∫峩縺輔↑縺・    // 髢句哨蟷・・蝓ｺ貅・m)縲ゅ％繧後ｈ繧雁ｺ・＞髢句哨縺ｯ邏騾壹ｊ縲∫強縺・⊇縺ｩ騾壹ｉ縺ｪ縺・・ 縺ｧ辟｡蜉ｹ縲・    //   迚ｩ逅・噪縺ｪ蠅・岼縺ｯ豕｢髟ｷ縺ｮ蜊雁・縺ゅ◆繧奇ｼ・00Hz 縺ｧ 0.34m・峨りｳ縺ｧ豎ｺ繧√ｋ繝√Η繝ｼ繝九Φ繧ｰ蛟､縲・    // 髢句哨縺ｮ髢九″蜈ｷ蜷茨ｼ亥屓謚倥・髻ｳ驥上ｒ豎ｺ繧√ｋ・峨ＢpertureOpenness/apertureOpenGain 蜿ら・縲・    float apertureOpenRef_ = 0.30f;      // 縺薙ｌ縺縺鷹幕縺・※縺・ｌ縺ｰ邏騾壹＠縺ｨ縺ｿ縺ｪ縺吝牡蜷・    float apertureOpenPower_ = 1.0f;     // 繧ｫ繝ｼ繝悶・.0 = 蜑ｲ蜷医↓豈比ｾ・    float apertureOpenRadius_ = 1.0f;    // 譁ｭ髱｢繧定ｦ九ｋ蜀・乢縺ｮ蜊雁ｾ・m)
    // 譌｢螳・0 ・晉┌蜉ｹ縲よ演縺ｯ縲後◆縺縺ｮ螢√阪→縺励※遞懃ｷ壽爾邏｢縺ｫ隕九○繧九・縺梧悽遲九〒縲・    // 髢九″蜈ｷ蜷医・螢√→謇峨・髢薙↓縺ｧ縺阪ｋ髢句哨驛ｨ縺ｮ蟷ｾ菴包ｼ育ｨ懃ｷ壹・菴咲ｽｮ縺ｨ ﾎｴ・峨′縺昴・縺ｾ縺ｾ陦ｨ縺吶・    // 蛻･謖・ｨ吶→縺励※蜑ｲ蜷医ｒ謖√▽縺ｮ縺ｯ莠碁㍾險井ｸ翫↓縺ｪ繧九◆繧√∵ｯ碑ｼ・畑縺ｫ縺縺第ｮ九☆縲・    int   apertureOpenSamples_ = 0;      // 蜀・乢縺ｮ繧ｵ繝ｳ繝励Ν謨ｰ縲・ 縺ｧ辟｡蜉ｹ蛹・
    // 髫咎俣縺ｮ蟷・ご繝ｼ繝茨ｼ亥屓謚倥・髻ｳ驥上ｒ豎ｺ繧√ｋ・峨ＴlitWidthAt/slitWidthGain 蜿ら・縲・    // 髢句哨縺ｮ遨榊・縺ｧ ﾎｴ・磯□蝗槭ｊ・峨ｒ縺ｩ繧後□縺大柑縺九○繧九°縲・=縺昴・縺ｾ縺ｾ / 0=蜉ｹ縺九○縺ｪ縺・・    //   笘・ｴ 貂幄｡ｰ縺ｯ蜑榊ｷ昴・蠑上→縺励※**蝗樊釜繧ｿ繝・・縺ｫ譌｢縺ｫ謗帙°縺｣縺ｦ縺・ｋ**縲る幕蜿｣縺ｮ貂ｬ螳壹〒繧よ寺縺代ｋ縺ｨ
    //     莠碁㍾縺ｫ縺ｪ繧九ょｽｹ蜑ｲ縺ｧ險縺医・縲後←繧後□縺鷹幕縺・※縺・ｋ縺九阪・髢句哨縺ｮ莉穂ｺ九・    //     縲碁□蝗槭ｊ縺ｧ縺ｩ繧後□縺第ｸ帙ｋ縺九阪・蟷ｾ菴輔・莉穂ｺ九↑縺ｮ縺ｧ縲∵悽譚･縺薙％縺ｯ蜉ｹ縺九○縺ｪ縺上※繧医＞縺ｯ縺壹・    //     蛻・ｊ蛻・￠繧貞ｮ滓ｸｬ縺吶ｋ縺溘ａ縺ｫ螟悶°繧牙､峨∴繧峨ｌ繧九ｈ縺・↓縺励※縺ゅｋ縲・    float deltaWeight_ = 1.0f;

    // 髢句哨邇・x 縺ｫ謗帙￠繧句咲紫縲ょｽ｢繧貞､峨∴縺壼ｹ・□縺代ｒ髢九￥・・.0 縺ｪ繧・1 蛟搾ｼ晉ｴ騾壹＠・峨・    //   ref 縺ｧ 1.0 縺ｫ縺ｪ繧九ｈ縺・ｭ｣隕丞喧縺励※縺九ｉ邏ｯ荵励☆繧九ょ腰邏斐↑邏ｯ荵励・髢句哨邇・′ 1 譛ｪ貅縺ｪ縺ｮ縺ｧ
    //   蜈ｨ菴薙′邵ｮ繧縺縺代↓縺ｪ繧具ｼ亥ｮ滓ｸｬ縺ｧ蜈ｨ髢九′ 0.0209 竊・0.0152 縺ｫ關ｽ縺｡縺滂ｼ峨・    float apertureContrastMap(float x) const {
        if (apertureContrast_ == 1.0f) return 1.0f;
        const float t = std::max(x, 1e-4f) / std::max(apertureContrastRef_, 1e-4f);
        const float y = std::min(std::pow(t, apertureContrast_), 1.0f);
        return y / std::max(x, 1e-4f);
    }

    bool  apertureIsTransmission_ = true;  // 髢句哨繧帝城℃縺ｮ荳驛ｨ縺ｨ縺励※謇ｱ縺・ｼ域里螳・ON・・    // 笘・・ 譌｢螳・OFF 縺ｫ謌ｻ縺励※縺ゅｋ縲ら炊逕ｱ繧呈ｮ九☆ 笘・・
    //   縲瑚ｷ晞屬・句ｹ・阪ｒ蜚ｯ荳縺ｮ豕募援縺ｫ縺励ｈ縺・→縺励※**螟ｱ謨励＠縺・*縲ょｹ・・豕募援縺ｯ
    //   縲悟｣√↓遨ｺ縺・◆遨ｴ縲阪・驥上〒縲・*陦晉ｫ具ｼ域怏髯舌・驕ｮ阡ｽ迚ｩ・峨↓縺ｯ螳夂ｾｩ縺輔ｌ縺ｪ縺・*縲・    //   螳滓ｸｬ: 陦晉ｫ九・蠖ｱ縺ｮ荳ｭ縺ｧ span 縺・0.000・井ｸｭ蠢・′螳滉ｽ薙↓關ｽ縺｡繧具ｼ峨ｄ 4.020・郁・逕ｱ遨ｺ髢難ｼ峨→
    //   縺ｰ繧峨▽縺阪∝ｽｱ縺ｮ荳ｭ縺ｮ蝗樊釜縺悟・驛ｨ 0 縺ｫ縺ｪ縺｣縺滂ｼ亥ｽｱ蠅・阜縺ｮ髫｣謗･蟾ｮ 0.562縲・/151 FAIL・峨・    //   蜑榊ｷ昴・ ﾎｴ 縺ｯ縲後←繧後□縺大ｽｱ縺ｮ螂･縺ｫ縺・ｋ縺九阪↑縺ｮ縺ｧ遨ｴ縺ｧ繧り｡晉ｫ九〒繧ょｮ夂ｾｩ縺輔ｌ繧九・    //   ﾎｴ 繧貞､悶＠縺溘・縺ｯ陦後″驕弱℃縺縺｣縺溘よｭ｣縺励＞讒区・縺ｯ荳九・縲湖ｴ ﾃ・蟷・ご繝ｼ繝医阪・    // 笘・立: 髢句哨縺ｮ蛻ｩ蠕励ｒ**縺薙・蠑上・縺ｨ縺､**縺ｫ縺吶ｋ譯茨ｼ域里螳・ON 縺ｫ縺励※縺・◆・峨・    //   莉･蜑阪・縲悟燕蟾晢ｼ九ヵ繝ｬ繝阪Ν遨榊・縲阪→菴ｵ蟄倥＠縺ｦ縺・◆縺後∝酔縺伜撫縺・↓ 2 縺､縺ｮ遲斐∴縺後≠繧狗憾諷九・
    //   迚・婿縺悟商縺上↑縺｣縺ｦ遏帷崟縺吶ｋ・医％縺ｮ繝輔ぃ繧､繝ｫ縺ｮ莉悶・邂・園縺ｧ繧ょ酔縺伜､ｱ謨励ｒ郢ｰ繧願ｿ斐＠縺ｦ縺・ｋ・峨・    //   繝昴・繧ｿ繝ｫ蠑上ｒ謗｡縺｣縺溽炊逕ｱ:
    //     繝ｻ邨瑚ｷｯ髟ｷ縺ｮ**莠碁㍾謇輔＞**縺梧ｧ矩逧・↓豸医∴繧具ｼ・etour 繧偵お繝ｳ繧ｸ繝ｳ縺後∬ｷ晞屬貂幄｡ｰ繧偵・繧ｹ繝医′
    //       謇輔▲縺ｦ縺・◆縲ゅ・繧ｹ繝医・ DistAtten(pathLen) 縺檎ｵ瑚ｷｯ髟ｷ縺昴・繧ゅ・繧定ｦ九※縺・ｋ・・    //     繝ｻ縲悟屓謚倥・螳壻ｽ搾ｼ玖ｷ晞屬貂幄｡ｰ縺縺代阪→縺・≧譁ｹ驥昴↓荳閾ｴ縺吶ｋ
    //     繝ｻ螳滓ｸｬ縺ｧ騾｣邯壽ｧ縺御ｸ奇ｼ磯團螳､縺ｸ豁ｩ縺丞｢・阜縺ｮ谿ｵ蟾ｮ 5.6dB 竊・2.7dB縲∵険繧悟ｹ・16.2 竊・11.4dB・・    //     繝ｻ繝輔Ξ繝阪Ν遨榊・縺ｯ驥阪＞縺ｮ縺ｫ縲・*蟶ｯ蝓溘・蠖｢縺ｯ繝帙せ繝医′謐ｨ縺ｦ縺ｦ縺・ｋ**縺ｮ縺ｧ謇輔＞謳阪□縺｣縺・    //   0 縺ｫ縺吶ｋ縺ｨ蠕捺擂縺ｮ縲悟燕蟾晢ｼ九ヵ繝ｬ繝阪Ν遨榊・縲阪↓謌ｻ繧具ｼ・/B 豈碑ｼ・畑縺ｫ谿九＠縺ｦ縺ゅｋ・峨・    bool  apertureAsPortal_ = false;
    float apertureContrast_ = 1.0f;        // 髢句哨邇・・蟷・ｒ髢九￥謖・焚・・=邏騾壹＠・・    // 縺薙％縺ｧ邏騾壹＠(1.0)縺ｫ縺ｪ繧矩幕蜿｣邇・・*蜈ｨ髢九・縺ｨ縺阪・螳滓ｸｬ蛟､**縺ｫ蜷医ｏ縺帙ｋ縲・    //   菴主沺蜉驥阪〒逡ｳ繧薙□蠎・ｸｯ蝓溘せ繧ｫ繝ｩ縺ｯ縲∝・髢九・謌ｸ蜿｣縺ｧ 0.278・亥ｸｯ蝓溷挨 125Hz 0.408縲・kHz 0.094・峨・    //   縺薙％繧貞､ｧ縺阪￥蜿悶ｋ縺ｨ蜈ｨ髢九〒繧・1.0 縺ｫ螻翫°縺壹∝ｹ・ｒ髢九＞縺溘▽繧ゅｊ縺悟・菴薙′邵ｮ繧
    //   ・・.40 縺ｫ縺励※縺・◆縺ｨ縺阪・螳滓ｸｬ縺ｧ 0.0209 竊・0.0155 縺ｾ縺ｧ關ｽ縺｡縺滂ｼ峨・    float apertureContrastRef_ = 0.278f;

    std::vector<Portal> portals_;          // 繝帙せ繝医′鄂ｮ縺城幕蜿｣縺ｮ遏ｩ蠖｢・医ヨ繝昴Ο繧ｸ縺ｯ繝帙せ繝医・諡・ｽ難ｼ・    bool  useBtm_ = false;                 // BTM 邨瑚ｷｯ・域里螳・OFF・・    float btmWedgeAngle_ = 4.712389f;      // 1.5ﾏ・晉ｮｱ縺ｮ蜃ｸ遞懃ｷ・
    static constexpr float kApertureMaxSpan = 4.0f;  // 縺薙ｌ莉･荳雁ｺ・￠繧後・髢句哨縺ｯ蠕矩溘＠縺ｪ縺・m)
    float slitWidthRef_ = 0.35f;         // 縺薙ｌ繧医ｊ蠎・￠繧後・邏騾壹ｊ(m)縲・00Hz 縺ｮ蜊頑ｳ｢髟ｷ縺ゅ◆繧・    float slitWidthPower_ = 1.0f;        // 繧ｫ繝ｼ繝悶・.0 = 蟷・↓豈比ｾ・    // 髫咎俣縺ｮ繝上う繝代せ縺ｮ蛯ｾ縺阪・.0 = 6dB/oct縲ょｰ上＆縺上☆繧九→髻ｳ濶ｲ縺ｮ螟牙喧縺檎ｩ上ｄ縺九↓縺ｪ繧・    // ・亥ｮ滓ｸｬ縺ｫ蜷医ｏ縺帙ｋ縺ｮ縺ｧ縺ｯ縺ｪ縺上∬◇縺九○縺溘＞髻ｳ縺ｫ蜷医ｏ縺帙※豎ｺ繧√※繧医＞蛟､・峨・    float slitBandSlope_ = 1.0f;
    // 髢句哨縺ｮ繧ｨ繝阪Ν繧ｮ繝ｼ繧偵ヵ繝ｬ繝阪Ν繧ｾ繝ｼ繝ｳ縺ｧ蜃ｺ縺吶°・・alse 縺ｧ蠕捺擂縺ｮ蟷・ご繝ｼ繝茨ｼ峨・    bool  useFresnelAperture_ = true;
    // 髢句哨縺ｮ螳溷柑蟷・ｒ譬ｼ蟄舌〒貂ｬ繧玖ｨｭ螳夲ｼ・pertureWidthByGrid・峨・ 縺ｧ辟｡蜉ｹ・晏ｾ捺擂縺ｮ遞懃ｷ・霆ｸ縺ｫ謌ｻ縺吶・    int   gridSamples_ = 7;      // 迚・ｾｺ縺ｮ繧ｻ繝ｫ謨ｰ・・ 竊・49 繧ｻ繝ｫ・・    float gridRadius_ = 1.5f;    // 髱｢荳翫〒隕九ｋ蜊雁ｾ・m)縲よ虻蜿｣繧定ｦ・≧螟ｧ縺阪＆縺ｫ
    float gridDepth_ = 0.5f;     // 髱｢縺ｮ蜑榊ｾ後←繧後□縺題ｦ九ｋ縺・m)縲よ攸縺悟･･縺ｫ縺・※繧よ黒縺ｾ縺医ｋ
    // 髢句哨繧帝擇縺ｨ縺励※魑ｴ繧峨☆轤ｹ謨ｰ縲・ 縺ｧ蠕捺擂縺ｩ縺翫ｊ・育せ1縺､・峨・    int   apertureSpread_ = 1;
    std::vector<Instance> instances_;          // 蜊譛臥黄・域ｯ弱ヵ繝ｬ繝ｼ繝譖ｴ譁ｰ蜿ｯ閭ｽ・・    Vec3 listenerPos_{};                       // 菫晄戟繝ｪ繧ｹ繝翫・・域ｮｵ1縲懊Ｂf_Update 縺御ｽｿ縺・ｼ・    std::vector<SourceEntry> sources_;         // 菫晄戟髻ｳ貅撰ｼ亥酔荳奇ｼ・
    mutable std::vector<BvhNode> bvhNodes_;    // BVH 繝弱・繝牙・・・azy 讒狗ｯ会ｼ・    mutable std::vector<int> bvhOrder_;        // 繧｢繧ｯ繝・ぅ繝悶↑繧､繝ｳ繧ｹ繧ｿ繝ｳ繧ｹ index 縺ｮ荳ｦ縺ｳ
    mutable bool bvhDirty_ = true;             // 繧､繝ｳ繧ｹ繧ｿ繝ｳ繧ｹ螟画峩縺ｧ遶九▽蜀肴ｧ狗ｯ峨ヵ繝ｩ繧ｰ
    mutable std::vector<DiffEdge> edgeCatalog_;  // 繧ｭ繝･繝ｼ繝悶・繝・・逕ｱ譚･縺ｮ繧ｷ繝ｫ繧ｨ繝・ヨ遞懃ｷ・};

}  // namespace acoustic

#endif  // ACOUSTICFLOW_CORE_SCENE_H
