/* Core/source_directivity.h
 * 音源指向性（directivity）の計算。
 *
 * 設計:
 *   - Core 層 = Wwise にも C API にも依存しない純計算。
 *   - 状態を持たないステートレス関数（音源/リスナーをエンティティとして
 *     登録しない）。音源ごとに毎フレーム呼ぶ想定（doc「音源単位は毎フレーム更新」）。
 *
 * 現段階は「B プラン」= 広帯域ゲイン + 背面ローパス で表現する。
 *   - 直接音にのみ効く。反射音への指向性適用はレイのエネルギー積分（後続）とセット。
 *   - 周波数 6 帯域それぞれに鋭さを変える「A プラン」は将来の拡張余地（ここを差し替える）。
 */
#ifndef ACOUSTICFLOW_CORE_SOURCE_DIRECTIVITY_H
#define ACOUSTICFLOW_CORE_SOURCE_DIRECTIVITY_H

#include "Core/vec3.h"

namespace acoustic {

// 指向性タイプ。doc 177行の分類に対応する。
// 値は C API (AF_DirectivityType) と一致させること（int で境界を越える）。
enum class DirectivityType {
    Omni = 0,           // 無指向：全方向に均一
    Cardioid = 1,       // 前方主体（cos^1）。ボーカルマイク等
    Supercardioid = 2,  // 前方に鋭い（cos^n, n大）。PA スピーカー等
    Bidirectional = 3,  // 前後に放射（|cos|）。ドラムキック/シンバル等
    Beam = 4,           // 強い前方ビーム（cos^n, n 非常に大）。ラインアレイ等
};

// 音源指向性の「広帯域ゲイン」を 0..1 で返す（B プラン）。
//   sourcePos     : 音源の位置
//   sourceForward : 音源の主放射方向（正規化は内部で行う。Transform.forward 相当）
//   listenerPos   : リスナーの位置
//   type          : 指向性タイプ
//   outLowpass    : [出力] 背面/側面ほど大きくなる「こもり量」0..1。
//                   Wwise 側でローパスに渡す想定。null 可。
//
// 戻り値は完全無音にはしない（側面/背面は「小さくなる + こもる」であって
// 「無音」ではない、という物理的直感に合わせ minGain で下限を設ける）。
float computeDirectivity(const Vec3& sourcePos,
                         const Vec3& sourceForward,
                         const Vec3& listenerPos,
                         DirectivityType type,
                         float* outLowpass);

}  // namespace acoustic

#endif  // ACOUSTICFLOW_CORE_SOURCE_DIRECTIVITY_H
