// FloatSpin.cs
// ゆっくり回りながら浮かぶ。世界に在るベルの見た目に付ける。
//
// ★これは**見た目のオブジェクトにだけ**付けること。
//   音源（Bell_World）に付けてはいけない。理由が 2 つある:
//     1. 音源が動くと毎フレーム幾何が変わり、タップの組み替えが常時走る。
//        止まっているはずの物が動く理由は無いのに、コストだけ増える
//     2. Bell_World には PinPosition が付いている（Space キーがデモ側の
//        「音源を1点に重ねる」トグルと衝突するため）。両方が位置を書くと喧嘩する
//
//   浮遊は数 cm なので、音として聞き分けられる差は出ない。
//   見た目だけ動かして、音源は据え置くのが正しい分担。
using UnityEngine;

namespace BellGame
{
    [DisallowMultipleComponent]
    public sealed class FloatSpin : MonoBehaviour
    {
        [Tooltip("1 秒あたり何度回すか。速いと『浮かんでいる』より『回されている』に見える。")]
        [Range(0f, 90f)] public float degreesPerSecond = 16f;

        [Tooltip("上下の振れ幅(m)。")]
        [Range(0f, 0.4f)] public float bobAmplitude = 0.05f;

        [Tooltip("上下 1 往復の秒数。")]
        [Range(0.5f, 12f)] public float bobSeconds = 4.5f;

        [Tooltip("わずかに傾ける(度)。真っ直ぐだと回転が読みにくい。")]
        [Range(0f, 20f)] public float tiltDegrees = 6f;

        private Vector3 _home;
        private float _angle;
        private float _phase;

        private void Awake()
        {
            _home = transform.position;
            // 個体ごとに位相をずらす（同じシーンに複数置いたとき揃って動かないように）。
            _phase = Mathf.Repeat(_home.x * 0.7f + _home.z * 1.3f, Mathf.PI * 2f);
        }

        private void Update()
        {
            _angle = Mathf.Repeat(_angle + degreesPerSecond * Time.deltaTime, 360f);
            float t = Time.time * Mathf.PI * 2f / Mathf.Max(0.1f, bobSeconds) + _phase;
            float bob = Mathf.Sin(t) * bobAmplitude;

            transform.SetPositionAndRotation(
                _home + new Vector3(0f, bob, 0f),
                Quaternion.Euler(0f, _angle, 0f) * Quaternion.Euler(tiltDegrees, 0f, 0f));
        }
    }
}
