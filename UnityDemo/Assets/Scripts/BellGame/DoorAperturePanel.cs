// DoorAperturePanel.cs
// 扉の**向こう側**に、吸音率ほぼ 100 の閉じた設えを置く。見えない。
//
// ★立ち位置（発注者の指示）
//     現世   … **ドアがあるだけ。** 壁も袖も足さない
//     接続世界 … そのドアを開口として、音を取り込む側の設えがある
//
//   だから箱は**扉の面より奥だけ**に置く。手前へも横へもはみ出さない。
//   現世に立って見回しても、足されたものは何も無い。
//
// ★なぜ要るのか
//   いまの扉は野原に枠だけで立っていて、エンジンから見ると**屋外 ⇄ 屋外**。
//   隔てるものが無いので「開口」が成立せず、音は扉の脇から素通しで回り込む。
//   結果、**扉を開けても閉めても大差が無い**。
//   §0 の芯（隙間が狭いほど低域だけ通り、開くほど高域まで通る）が、
//   いちばん最初の扉で一度も動いていなかった。
//
//   向こう側を閉じれば「閉空間 ⇄ 屋外」に上がり、開口が立つ。
//
// ★なぜ α≈0.99 なのか（見えない物を置いてよい理由）
//   §7.2 で「見えないコライダーを置くな」と書いた。理由は
//   **何も無いはずの場所から早期反射が返るから**。
//   吸音率をほぼ 1 にすると**何も返さない**ので、その反論が立たない。
//   遮るだけで、鳴らさない。
//
//   響きを持たせないのが正しくもある ── 行き先の残響は WorldVoice が
//   漏れる音そのものへ焼いているので、この箱まで響くと**二重になる**。
//
// ★体は素通りさせる。
//   WorldBounds の押し出しがこれを拾うと、扉の奥に見えない壁ができてぶつかる。
//   AcousticOnly を付けて押し出しから外してある。
using UnityEngine;
using AcousticFlow;

namespace BellGame
{
    /// この当たり判定は**音響専用**。体は素通りする。
    public sealed class AcousticOnly : MonoBehaviour { }

    [DisallowMultipleComponent]
    public sealed class DoorAperturePanel : MonoBehaviour
    {
        [Header("開口（MakeDoorFrame と揃える）")]
        public float apertureWidth = 1.1f;
        public float apertureHeight = 2.2f;

        [Header("どこまでを『向こうの世界』とみなすか")]
        // ★半径で決める（発注者の指示）。
        //
        //   ここが**一度失敗した所**です。4×4×3m の決め打ちだったせいで、
        //   扉から 6m 奥に置いた音源が**箱の外**に出てしまい、
        //   箱が「リスナーと音源のあいだの殻」になっていました
        //  （開けたら小さくなる／くぐったら小さくなる、の正体）。
        //
        //   囲うのは**音源を中に入れるため**であって、間に壁を挟むためではありません。
        //   だから寸法は「扉の周りどこまでを向こうの世界とみなすか」＝半径で決めます。
        //   この半径より遠い音源は囲いの外＝扉ごしには聞こえません。それも仕様です
        //  （遠すぎる音まで開口ごしに引っぱると、扉が何処にでも繋がってしまう）。
        [Tooltip("扉の周りこの距離までを『向こうの世界』として囲う(m)。"
                 + "この中の音源だけが開口ごしに入ってくる。")]
        [Range(3f, 40f)] public float radius = 12f;
        [Tooltip("高さ。囲いの天井まで。")]
        [Range(2.5f, 20f)] public float height = 8f;
        [Range(0.05f, 1f)] public float thickness = 0.3f;

        // 半径から導く。手で持たない（同じ問いに答えが二つできる）。
        private float width => radius * 2f;
        private float depth => radius;

        [Header("材質")]
        // ★吸音率の上限は 0.99（AcousticMaterial.cs:189 の Clamp）。
        //   Default(平均 α0.208) を 4.8 倍すると上限に張り付く。
        //   4×4×3m の箱で RT60 ≈ 0.10s ＝ 実質デッド。
        [Tooltip("Default の吸音率に掛ける倍率。上限 0.99 に張り付く値。")]
        [Range(1f, 5f)] public float absorptionScale = 4.8f;

        private void Start()
        {
            // ★部屋になるには**天井が要る**（`2e63c46`：壁だけでは足りない）。
            //   床・天井・奥・左右・手前（開口の穴あき）で閉じる。
            float cz = -(depth * 0.5f);           // 扉の面(z=0)より奥だけ
            float t = thickness;

            Wall("Floor", new Vector3(0f, -t * 0.5f, cz), new Vector3(width, t, depth));
            Wall("Ceil", new Vector3(0f, height + t * 0.5f, cz), new Vector3(width, t, depth));
            Wall("Back", new Vector3(0f, height * 0.5f, cz - depth * 0.5f),
                 new Vector3(width, height, t));
            Wall("Left", new Vector3(-width * 0.5f, height * 0.5f, cz),
                 new Vector3(t, height, depth));
            Wall("Right", new Vector3(width * 0.5f, height * 0.5f, cz),
                 new Vector3(t, height, depth));

            // 手前の面。扉の枠のぶんだけ穴を空ける。
            //   ★面は扉の面より**奥へ半厚みずらす**。手前へはみ出させない。
            float side = (width - apertureWidth) * 0.5f;
            float fz = -t * 0.5f;
            Wall("FrontL", new Vector3(-(apertureWidth * 0.5f + side * 0.5f), height * 0.5f, fz),
                 new Vector3(side, height, t));
            Wall("FrontR", new Vector3(apertureWidth * 0.5f + side * 0.5f, height * 0.5f, fz),
                 new Vector3(side, height, t));
            if (height > apertureHeight + 0.05f)
                Wall("FrontTop", new Vector3(0f, apertureHeight + (height - apertureHeight) * 0.5f, fz),
                     new Vector3(apertureWidth, height - apertureHeight, t));
        }

        private void Wall(string name, Vector3 localPos, Vector3 size)
        {
            var go = new GameObject(name);
            go.transform.SetParent(transform, false);
            go.transform.localPosition = localPos;
            go.transform.localRotation = Quaternion.identity;

            // ★Renderer は付けない。見えない。
            var box = go.AddComponent<BoxCollider>();
            box.size = size;

            // ★「塞ぐが、鳴らさない」。2 つの意味があって、つまみも 2 つ要ります。
            //     透過を殺す(+30dB) … 壁を抜けてこない ＝ 開口が唯一の入口になる
            //     吸音を上げる      … 何も無いはずの場所から早期反射を返さない
            //   前は吸音だけ上げていました。透過は既定のままだったので、
            //   **囲いの壁を抜けて音が来ていた**（開口の意味が薄れていた）。
            var surf = go.AddComponent<AcousticSurface>();
            surf.mode = AcousticSurfaceMode.Adjusted;
            surf.material = AcousticMaterialPreset.Default;
            surf.absorptionScale = absorptionScale;
            surf.transmissionLossOffsetDb = 30f;

            go.AddComponent<AcousticOnly>();      // 体は素通り
        }
    }
}
