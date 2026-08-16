// AcousticPortal.cs
// 開口（戸口・窓・通路の口）をシーンに置くためのコンポーネント。
//
// ★このコンポーネントが持つのは「ここが開口である」という**トポロジだけ**。
//   開き具合は一切持たない。エンジンが毎フレーム実際の形状から測る。
//
//   既存のゲームは扉の開き角を 0..1 に正規化して一律にゲインを掛けるので、
//   奥に何がどこにあっても同じカーブになる。ここが違う点で、実測では
//   同じ扉角度でも音源が戸口に正対していれば 26.2dB、2m 外れていれば 2.9dB と
//   まったく別の結果になる。位置関係が音を決めている。
//
// なぜ矩形を置く必要があるのか:
//   開口の測り方は「面のうちどこまでを積分するか」に答えが無く、
//   窓の置き方を4通り試して全部別の理由で失敗した
//   （直線上＝壁に沈む／稜線上＝跳ぶ／重心＝部屋の外を数える／複数面＝薄い板が見えない）。
//   ポータルはその答えそのもので、積分範囲＝この矩形になる。
//   人が「ここが戸口」と置くだけで、測る側の曖昧さが消える。
//
// 使い方:
//   戸口の**開口部そのもの**（扉ではなく、扉がはまる枠の内側）に空の GameObject を置き、
//   このコンポーネントを付けて width/height を合わせる。
//   面の向きは transform の forward（青軸）が開口の法線になるよう回す。
using UnityEngine;

namespace AcousticFlow
{
    [DisallowMultipleComponent]
    public class AcousticPortal : MonoBehaviour
    {
        [Tooltip("開口の幅(m)。扉ではなく**枠の内側**の寸法。")]
        public float width = 1.2f;
        [Tooltip("開口の高さ(m)。")]
        public float height = 2.4f;
        [Tooltip("OFF にすると測定対象から外れる（塞がれた通路など）。")]
        public bool active = true;

        /// エンジンに登録されたときの id。-1 は未登録。
        [System.NonSerialized] public int engineId = -1;

        /// 矩形の横軸（ワールド）。transform の right をそのまま使う。
        public Vector3 AxisU => transform.right;
        /// 矩形の縦軸（ワールド）。
        public Vector3 AxisV => transform.up;
        public float HalfU => Mathf.Max(width * 0.5f, 0.01f);
        public float HalfV => Mathf.Max(height * 0.5f, 0.01f);

        // シーンビューで開口の範囲を出す。置き間違いは音で気づきにくいので、
        // 見て分かるようにしておく（枠の内側に合っているかが要点）。
        private void OnDrawGizmos()
        {
            Gizmos.color = active ? new Color(0.3f, 1f, 0.6f, 0.9f)
                                  : new Color(0.5f, 0.5f, 0.5f, 0.5f);
            Vector3 c = transform.position;
            Vector3 u = AxisU * HalfU, v = AxisV * HalfV;
            Vector3 a = c - u - v, b = c + u - v, d = c + u + v, e = c - u + v;
            Gizmos.DrawLine(a, b); Gizmos.DrawLine(b, d);
            Gizmos.DrawLine(d, e); Gizmos.DrawLine(e, a);
            Gizmos.DrawLine(a, d); Gizmos.DrawLine(b, e);   // 対角。面の向きが分かるように
            Gizmos.color = new Color(0.3f, 1f, 0.6f, 0.35f);
            Gizmos.DrawLine(c, c + transform.forward * 0.5f);   // 法線
        }
    }
}
