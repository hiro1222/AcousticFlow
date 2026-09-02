// CaptureSelfTest.cs ── 自己診断のメニュー口だけ。中身は CaptureDiagnostics（ランタイム側）。
//
// ★なぜ中身がここに無いのか
//   診断は `Native`（P/Invoke の宣言）を直接叩く。`Native` は **internal** で、
//   Unity は `Editor/` を**別アセンブリ**にするので、ここからは見えない
//   （最初ここに全部書いて CS0122 で 33 件落ちた）。
//   `Native` を public に広げるより、診断本体を同じアセンブリへ置くほうが素直。
using UnityEditor;
using UnityEngine;

namespace AcousticFlow
{
    public static class CaptureSelfTest
    {
        [MenuItem("AcousticFlow/キャプチャの自己診断")]
        public static void Run()
        {
            string report;
            int pass, fail;
            bool ok = CaptureDiagnostics.Run(out report, out pass, out fail);

            if (ok) Debug.Log(report); else Debug.LogError(report);
            EditorUtility.DisplayDialog("キャプチャの自己診断",
                (ok ? "通りました（" + pass + " 件）" : "落ちました（" + fail + " 件）")
                + "\n\n詳細は Console を見てください。", "OK");
        }
    }
}
