// OutdoorTailGate.cs
// 屋外にいる間だけ後期残響の尾を絞る。
//
// ★これは回避策であって修正ではない。
//   本来の問題はホスト側にある。AcousticFlowSceneDemo.UpdateReverbTargetRatio は
//   部屋が取れないとき（屋外・囲われていない場所）に
//     ・体積 = 全オクルーダーの外接箱
//     ・RT60 = エコグラムの「−60dB を超える最後のビン」
//   で尾の量を作る。前者は部屋ではないし、後者は仕様書 §9 が名指しで
//   「やってはいけない」と書いている測り方（窓 1 秒に縛られて鈍く、吸音率が
//   17.6 倍違う 2 部屋を 0.50s と 0.51s としか区別できなかった実測がある）。
//   つまり屋外の尾は**根拠が幾何に無い**。
//
//   直すべきはそのフォールバックだが、いまはサウンドシステムに触らない約束なので、
//   ゲーム側から尾の音量だけを連続に絞って辻褄を合わせる。
//   草案 §4.2「屋外には部屋がない。尾も立たない」に寄せるための当て木。
//
// ★二値で切り替えないこと（決めごと #2）。
//   RoomVolumeAt は roomBlendRadius（既定 2m）の球で混ぜた値なので、部屋へ近づくと
//   0 から連続に立ち上がる。そこへさらに時間方向の平滑を掛けて、
//   戸口をまたぐ瞬間に尾が出現しないようにする。
using UnityEngine;
using AcousticFlow;

namespace BellGame
{
    [DisallowMultipleComponent]
    public sealed class OutdoorTailGate : MonoBehaviour
    {
        [Tooltip("対象。空なら実行時にシーンから拾う。")]
        public VoiceConvolver[] voices;
        public IrConvolver[] irVoices;

        [Tooltip("部屋の中にいるときの尾の音量。既存テストシーンと同じ 0.3。")]
        [Range(0f, 2f)] public float indoorTailLevel = 0.3f;
        [Tooltip("屋外にいるときの尾の音量。0 で完全に消える（§4.2 の『尾も立たない』）。")]
        [Range(0f, 2f)] public float outdoorTailLevel = 0f;
        [Tooltip("屋外⇄屋内の入れ替わりにかける秒数。短いと戸口で段差になる。")]
        [Range(0.05f, 3f)] public float smoothSeconds = 0.6f;

        [Header("診断（読み取り専用）")]
        [Tooltip("0=屋外 / 1=部屋の中。RoomVolume を平滑したもの。")]
        public float roomness;
        [Tooltip("いま尾に掛けている音量。")]
        public float appliedTailLevel;
        [Tooltip("ホストが報告している部屋の体積。0 ならフォールバックが効いている。")]
        public float roomVolume;

        private void Awake()
        {
            if (voices == null || voices.Length == 0)
                voices = FindObjectsByType<VoiceConvolver>(FindObjectsSortMode.None);
            if (irVoices == null || irVoices.Length == 0)
                irVoices = FindObjectsByType<IrConvolver>(FindObjectsSortMode.None);
        }

        private void Update()
        {
            roomVolume = AcousticFlowSceneDemo.Status.RoomVolume;

            // RoomVolume が 0 ＝ 部屋が取れず外形箱のフォールバックに落ちている＝屋外。
            float target = roomVolume > 0f ? 1f : 0f;
            roomness = Mathf.MoveTowards(roomness, target,
                                         Time.deltaTime / Mathf.Max(0.01f, smoothSeconds));

            appliedTailLevel = Mathf.Lerp(outdoorTailLevel, indoorTailLevel, roomness);

            // VoiceConvolver は毎フレーム Update で AF_VoiceSetTailLevel を呼ぶので、
            // フィールドを書き換えておけば次の呼び出しでそのまま効く。
            if (voices != null)
                for (int i = 0; i < voices.Length; i++)
                    if (voices[i] != null) voices[i].tailLevel = appliedTailLevel;
            if (irVoices != null)
                for (int i = 0; i < irVoices.Length; i++)
                    if (irVoices[i] != null) irVoices[i].tailLevel = appliedTailLevel;
        }
    }
}
