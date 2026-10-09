// AcousticEngine.cs ── エンジンと共有する定数だけを置く場所（帯域の数と中心周波数）。
//
// ■ 全体の中の位置
//   C# のどの層からも参照される最下層。ここに式や状態は置かない（置くと engine と二重の答えになる）。
//   帯域の数を変えるときは engine の world_rules.h（kNumBands）と**同時に**変えること。
//
//
// ★2026-09-03: ここにあった旧インスタンス API（AcousticEngine_Create/AddBox/IsOccluded…）と
//   Wwise 風の再生 API（InitAudio/PostEvent/RTPC/像源エミッタ…）を削除した。
//   DLL 側は既定でスタブ Adapter（AF_USE_WWISE=OFF）だったので、あの道は何も鳴らしていなかった。
//   音は各音源の AudioSource → IrConvolver / VoiceConvolver（畳み込み）だけで出す。
//   幾何と音響のクエリは AcousticScene（AF_Scene*）、音の生成は AF_Voice*/AF_TailBus* を使う。
namespace AcousticFlow
{
    public static class AcousticEngine
    {
        // 帯域数。エンジン（scene.h の kNumBands）と同じ 6。オクターブ帯 125〜4000 Hz。
        public const int NumBands = 6;
        public static readonly int[] BandFreqs = { 125, 250, 500, 1000, 2000, 4000 };
    }
}
