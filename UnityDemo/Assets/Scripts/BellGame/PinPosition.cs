// PinPosition.cs
// 起動時の位置に固定し続けるだけのコンポーネント。
//
// ★なぜ要るか（Space キーの衝突）:
//   AcousticFlowSceneDemo は Space に「音源を元配置⇄1点に重ねるトグル」を割り当てている。
//   呼びかけを Space にすると、押すたびに ApplySourceLayout が走って
//   世界のベルが「元配置の重心」へ飛ばされる（ステージ2 なら 12m 以上動く）。
//
//   デモ側は触らない約束なので、こちらで押さえ返す。
//   FollowTransform と同じく LateUpdate で位置を書き戻す
//   （ApplySourceLayout は Update の中なので、後から上書きすれば勝てる）。
//
// ※ 1 フレームだけはデモが動かした位置がエンジンへ渡ってしまう。
//   これは BellCallResponse 側が「ベルを 1 フレーム遅らせて鳴らす」ことで避けている。
using UnityEngine;

namespace BellGame
{
    [DisallowMultipleComponent]
    public sealed class PinPosition : MonoBehaviour
    {
        [Tooltip("固定する位置。未設定(=Vector3.zero 以外)なら Awake 時の位置を使う。")]
        public Vector3 position;
        public bool captureOnAwake = true;

        private void Awake()
        {
            if (captureOnAwake) position = transform.position;
        }

        private void LateUpdate()
        {
            transform.position = position;
        }
    }
}
