// FollowTransform.cs
// 対象の位置に一定のオフセットで追従する。残響検証シーンで「音源をリスナーに追従させる」用。
//
// なぜオフセットを持たせるか（0 にしない理由）:
//   残響の量は物理式 残響/直接 = (r/r_c)² で決めている（r = 音源とリスナーの距離）。
//   r=0 だと比が 0 になり、尾が完全に消える。これは式の不備ではなく、
//   無指向の点音源に耳を密着させれば直接音しか聞こえない、という物理的に正しい帰結。
//
//   現実に自分の声が響いて聞こえるのは、声が前方に指向性を持ち（部屋には強い前方成分が入る）、
//   自分の耳は口の横〜後方にあって直接音を弱くしか受け取らないため。
//   本システムはまだ音源の指向性を実装していないので、その非対称性を再現できない。
//   → 検証シーンでは一定距離を空けて、直接音を一定に保ちつつ残響の変化だけを見る。
//
// 実行順について:
//   LateUpdate で動かす。AcousticFlowSceneDemo は Update 内でリスナーを移動させたあと
//   音源位置を読むので、音響計算に使われるのは「1フレーム前の追従結果」になる。
//   16ms の遅れだが、この用途（部屋に入ったときの残響の立ち上がりを聴く）では問題ない。
//   逆に Update で先回りさせると見た目がリスナーから1フレーム遅れて不自然になる。
using UnityEngine;

namespace AcousticFlow
{
    public sealed class FollowTransform : MonoBehaviour
    {
        [Tooltip("追従する対象。未設定なら AcousticFlowSceneDemo の listener を自動で使う。")]
        public Transform target;

        [Tooltip("対象からのオフセット(m, ワールド座標)。\n"
                 + "0 にすると音源とリスナーが重なり、残響が計算上ゼロになるので注意。")]
        public Vector3 offset = new Vector3(0f, 0f, 0.3f);

        [Tooltip("ON: オフセットを対象の向きに合わせて回す（相対的な位置関係が保たれる）。\n"
                 + "OFF: ワールド座標でずらす（振り向くと音源が周囲を回るので定位の確認に向く）。")]
        public bool useTargetRotation = false;

        private void LateUpdate()
        {
            if (target == null)
            {
                var demo = FindObjectOfType<AcousticFlowSceneDemo>();
                if (demo != null) target = demo.listener;
                if (target == null) return;
            }
            transform.position = useTargetRotation
                ? target.TransformPoint(offset)
                : target.position + offset;
        }
    }
}
