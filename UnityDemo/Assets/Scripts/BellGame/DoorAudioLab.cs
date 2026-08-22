// DoorAudioLab.cs
// 「扉の奥の世界の音」だけを聞くための操作台。検証シーン専用。
//
// ★確かめること（発注者の指定 ①②③）
//   ① 繋がっていない扉は**ただの扉**。奥から何も鳴らない
//   ② 繋いだ瞬間、扉の奥に**その世界の音**が出現する
//      → 繋ぎ先を変えると音が変わる ＝ 何処に繋がっているかが音で分かる
//   ③ 扉を開くほど取り込まれる
//      → 音量も帯域も誰も触っていない。§4.3 のフレネル帯域積分だけがやっている
//
// ★ここは**音だけ**の場所。
//   ポータル描画も世界の入れ替えも入れない。絵と 25ms が混ざると、
//   「いま聞こえ方が変わったのは扉のせいか」が判断できなくなる。
//   含まれない物は本当に含まれていないので、変化があれば扉が原因だと言い切れる。
//
// ★行き先の切り替えは **Tab で一巡**（None も輪の中に入れてある）。
//   5/6 は扉の開閉に使われているので数字キーは避けた。
//   None を輪に入れてあるのは、①を**いつでも 1 回で聞き直せる**ようにするため ──
//   「鳴っていない」を確かめられないと、「鳴っている」の意味も決まらない。
using UnityEngine;

namespace BellGame
{
    [DisallowMultipleComponent]
    public sealed class DoorAudioLab : MonoBehaviour
    {
        [Header("配線")]
        public WorldDoor worldDoor;
        [Tooltip("扉の奥の音。F5 の切り替えもこれが持っている。")]
        public BeyondAmbience beyond;

        [Header("操作")]
        [Tooltip("行き先を一巡させるキー。None も輪に入っている。")]
        public KeyCode cycleKey = KeyCode.Tab;

        // 輪。**None を先頭に置く** ── 起動直後は「ただの扉」から始まる。
        private static readonly WorldId[] Ring =
        {
            WorldId.None, WorldId.Grassland, WorldId.Temple,
            WorldId.Cave, WorldId.Snow, WorldId.Sea,
        };

        private int _at;

        private void Start()
        {
            if (worldDoor == null) worldDoor = FindFirstObjectByType<WorldDoor>();
            if (beyond == null) beyond = FindFirstObjectByType<BeyondAmbience>();

            // ★①から始める。繋がっていない状態を先に聞かせる。
            if (worldDoor != null) worldDoor.destination = WorldId.None;
            _at = 0;
        }

        private void Update()
        {
            if (worldDoor == null || !Input.GetKeyDown(cycleKey)) return;

            _at = (_at + 1) % Ring.Length;
            worldDoor.destination = Ring[_at];

            Debug.Log(Ring[_at] == WorldId.None
                ? "[BellGame] 扉の行き先: なし ── ただの扉。奥から何も鳴らないのが正しい"
                : $"[BellGame] 扉の行き先: {BellVoices.DisplayName(Ring[_at])} ── "
                  + $"奥にその世界の音が出現する（{BeyondAmbience.SignatureOf(Ring[_at])}）");
        }

        private void OnGUI()
        {
            var dest = (worldDoor != null) ? worldDoor.destination : WorldId.None;
            float ang = (worldDoor != null && worldDoor.door != null)
                        ? worldDoor.door.AngleDeg : 0f;

            var sb = new System.Text.StringBuilder();
            sb.AppendLine("── 音の検証：扉の奥の世界 ──────────────");
            sb.AppendLine($"  行き先 = {(dest == WorldId.None ? "なし（ただの扉）" : BellVoices.DisplayName(dest))}"
                          + $"   扉の開き = {ang:F0}°");
            sb.AppendLine($"  奥の信号 = {(beyond != null && beyond.useTestSignal ? "持続する広帯域（§4.3 の連続性を聞く）" : "世界の声（繋ぎ先が分かるかを聞く）")}");
            sb.AppendLine();
            sb.AppendLine("  Tab  行き先を一巡（なし → 草原 → 崩壊都市 → 洞窟 → 雪山 → 海）");
            sb.AppendLine("  5/6  扉を開く／閉じる（押している間）");
            sb.AppendLine("  F5   奥の信号を切り替え");
            sb.AppendLine();
            sb.AppendLine("  ① Tab で「なし」にして、奥が無音か");
            sb.AppendLine("  ② 繋ぐと音が出るか。行き先を変えると**別の世界の音**になるか");
            sb.AppendLine("  ③ 5/6 で少しずつ開けて、通る帯域が**連続に**広がるか");
            sb.AppendLine("     （左の Band Monitor が帯域ごとの通過ゲイン）");

            GUI.Label(new Rect(12, 12, 780, 220), sb.ToString());
        }
    }
}
