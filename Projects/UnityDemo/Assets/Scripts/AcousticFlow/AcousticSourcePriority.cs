// ★旧コア（Demo / Test_* の場面）。新コア（Flow_* の場面 ＝ AcousticWorld 系）では使っていない。
//   2026-09-24 の棚卸しで印を付けた。試聴で新コアへの乗り換えが決まったら、旧コアの C++ ごと消す（段 10）。
//   それまでは聞き比べの基準として残す。──代わり: 新コアでは予算と順位が engine の中にある（budget.h、WorldVoice.operated）
// AcousticSourcePriority.cs ── 音源の順位のつまみ（段の予算）。音源の Transform に付ける。
//   無ければ重要度 1・固定なし。規則（音量 × 1/r × 生存 × 重要度 で並べ、上から厳密 → 簡易 → 保持）は
//   エンジンに 1 つだけ（docs/TIER_BUDGET.md）。ここは値を渡すだけ。
using UnityEngine;

namespace AcousticFlow
{
    /// 段の予算（AcousticFlowSceneDemo.tierBudgetExact / tierBudgetSimple）で落ちる順を動かすデザイナのつまみ。
    ///   重要度: 可聴性の見積もりに掛ける倍率。2 なら +6 dB ぶん落ちにくく、0.5 なら −6 dB ぶん落ちやすい。
    ///   固定: 予算に関わらず段を保つ（厳密の枠を 1 つ消費する）。探しているベルのような体験の芯だけに付ける。
    [DisallowMultipleComponent]
    [AddComponentMenu("AcousticFlow/Acoustic Source Priority")]
    public class AcousticSourcePriority : MonoBehaviour
    {
        [Tooltip("重要度（倍率）。可聴性の見積もり（音量 × 1/r × 生存）に掛ける。2 = +6 dB ぶん落ちにくい、0.5 = −6 dB。")]
        [Range(0f, 8f)] public float importance = 1f;

        [Tooltip("固定: 予算に関わらず段を保つ（厳密の枠を 1 つ消費する）。体験の芯だけに。全部に付けると予算が効かない。")]
        public bool pinned = false;
    }
}
