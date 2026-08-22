// LayoutStats.cs
// 配置シーンの重さを画面に出すだけの部品。**切り分け用で、本編には持ち込まない。**
//
// ★世界観・ステージ構成レーンが置いた。描画も音響も一切変えない。
//
// 出す数字を 4 つに絞ってある。重いときにどこを回せばいいかが、この 4 つで決まる:
//   FPS / フレーム時間  … そもそも重いのか
//   描いている草の本数  … density と radius をどれだけ落とせばいいか
//   描画呼び出しの回数  … CPU 側で詰まっているか（Built-in RP では効く）
//   組み立て済みの総数  … メモリの目安（1 本あたり 96 バイト）
//
// F3 で表示を切り替え。
using UnityEngine;

namespace BellGame
{
    [DisallowMultipleComponent]
    public sealed class LayoutStats : MonoBehaviour
    {
        public KeyCode toggleKey = KeyCode.F3;
        public bool show = true;

        private InstancedGrassField _grass;
        private InstancedCanopy[] _canopies;
        private float _smoothMs = 16f;
        private GUIStyle _style;

        private void Awake()
        {
            _grass = GetComponent<InstancedGrassField>();
            if (_grass == null) _grass = FindAnyObjectByType<InstancedGrassField>();
            _canopies = FindObjectsByType<InstancedCanopy>(FindObjectsSortMode.None);
        }

        private void Update()
        {
            if (Input.GetKeyDown(toggleKey)) show = !show;
            // 1 フレームの値は跳ねるので、指数移動平均で均す。
            _smoothMs = Mathf.Lerp(_smoothMs, Time.unscaledDeltaTime * 1000f, 0.1f);
        }

        private void OnGUI()
        {
            if (!show) return;
            _style ??= new GUIStyle(GUI.skin.label)
            {
                fontSize = 14,
                normal = { textColor = Color.white },
            };

            int drawnTiles = _grass != null ? _grass.tilesDrawnLastFrame : 0;
            int built = _grass != null ? _grass.instanceCount : 0;
            int leaves = 0;
            foreach (var c in _canopies) if (c != null) leaves += c.instanceCount;

            // ★見積りはやめた。前は総タイル数を計算で出していて、描いている数のほうが
            //   多く出る（119/113）という壊れた表示になっていた。実測を持ってくる。
            int tilesTotal = _grass != null ? _grass.tilesTotal : 0;
            int drawn = _grass != null ? _grass.instancesDrawnLastFrame : 0;

            var sb = new System.Text.StringBuilder();
            sb.AppendLine($"{1000f / Mathf.Max(_smoothMs, 0.001f):0} FPS   {_smoothMs:0.0} ms");
            sb.AppendLine($"草 組み立て {built:N0} 本 / 描画 {drawn:N0} 本");
            sb.AppendLine($"タイル {drawnTiles} / {tilesTotal}   描画呼び出し 約 {Calls(drawn) + Calls(leaves)} 回");
            sb.AppendLine($"葉 {leaves:N0} 枚   三角形 約 {(drawn * 7 + leaves * 2) / 10000f:0.0} 万");
            sb.AppendLine($"メモリ目安 {built * 96f / 1048576f:0.0} MB（草の行列と色）");
            sb.AppendLine($"[{toggleKey}] 表示切替");

            GUI.Box(new Rect(8, 8, 360, 122), GUIContent.none);
            GUI.Label(new Rect(16, 12, 350, 118), sb.ToString(), _style);
        }

        // 描画呼び出しの回数。1023 本で 1 回。
        private static int Calls(int instances) => Mathf.CeilToInt(instances / 1023f);
    }
}
