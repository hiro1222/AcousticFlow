// ★旧コア（Demo / Test_* の場面）。新コア（Flow_* の場面 ＝ AcousticWorld 系）では使っていない。
//   2026-09-24 の棚卸しで印を付けた。試聴で新コアへの乗り換えが決まったら、旧コアの C++ ごと消す（段 10）。
//   それまでは聞き比べの基準として残す。──代わり: 録音の自己診断（旧コアの AcousticScene 経由）
// CaptureDiagnostics.cs ── 「キャプチャの道が本当に通っているか」を Unity の中で確かめる本体。
//
//   呼び口: Editor メニュー `AcousticFlow / キャプチャの自己診断`（Editor/CaptureSelfTest.cs）
//
// ★なぜランタイム側に置くのか
//   ここは `Native`（P/Invoke の宣言）を直接叩く。`Native` は **internal** なので、
//   **Editor アセンブリからは見えない**（Unity は Editor/ を別アセンブリにする）。
//   最初 Editor 側に書いて CS0122 で全滅した。`Native` を public に広げるのではなく、
//   診断本体をこちら（同じアセンブリ）へ置いて、Editor は呼ぶだけにする。
//
// ★何を見ているか ── 「例外が出ないこと」ではない
//   例外が出ないまま**静かに壊れる**所だけを見る:
//     ・構造体の並び        → 数字が入れ替わって出る。例外なし
//     ・文字列の符号化      → 型紙名が化ける。例外なし
//     ・走査が何も見つけない → 黙るだけ
//     ・PCM が無音          → 「録れている」と思って空を配る
//     ・保存先に書けない    → 押しても何も落ちない
//
// ⚠ Play モードに入らない。シーンにも触らない（自分で小部屋を作って壊すだけ）。
using System.IO;
using UnityEngine;

namespace AcousticFlow
{
    public static class CaptureDiagnostics
    {
        /// 自己診断を回す。通れば true。report は Console に出す用の本文。
        public static bool Run(out string report, out int pass, out int fail)
        {
            int p = 0, f = 0;
            var log = new System.Text.StringBuilder();
            log.AppendLine("=== キャプチャの自己診断 ===");
            System.Action<string, bool, string> check = (label, ok, note) =>
            {
                if (ok) ++p; else ++f;
                log.AppendLine((ok ? "  [OK]   " : "  [NG]   ") + label
                               + (string.IsNullOrEmpty(note) ? "" : "   " + note));
            };

            string path = Path.Combine(Path.GetTempPath(), "af_selftest.afcap");
            AcousticScene scene = null;
            System.IntPtr cap = System.IntPtr.Zero;
            try
            {
                // ── 1. DLL が読めて、シーンが作れるか ──
                scene = new AcousticScene();
                check("DLL が読めてシーンが作れる", scene != null, "");

                // ── 2. 形を作る（戸口を扉が塞ぐ小部屋）──
                // ★材質は**エンジンから引く**（FromPreset）。
                //   `AcousticMaterial.DefaultWall()` は DLL が読めないときの**保険の表**で、
                //   正ではない。最初それを直に呼んでいて、C++ の回帰テストと違う壁で
                //   測っていた（壁の遮音が 6dB 甘い＝印の出方が変わる）。
                int mat = scene.AddMaterial(AcousticMaterial.FromPreset(AcousticMaterialPreset.Default));
                const float h = 3f;
                scene.AddInstanceBox(new Vector3(-2.25f, h * .5f, 0), new Vector3(1.75f, h * .5f, .1f),
                                     Vector3.right, Vector3.up, mat);
                scene.AddInstanceBox(new Vector3(2.25f, h * .5f, 0), new Vector3(1.75f, h * .5f, .1f),
                                     Vector3.right, Vector3.up, mat);
                int door = scene.AddInstanceBox(new Vector3(0, h * .5f, 0), new Vector3(.5f, h * .5f, .05f),
                                                Vector3.right, Vector3.up, mat);
                scene.AddInstanceBox(new Vector3(0, -.1f, 0), new Vector3(6, .1f, 8),
                                     Vector3.right, Vector3.up, mat);
                scene.AddInstanceBox(new Vector3(0, h + .1f, 0), new Vector3(6, .1f, 8),
                                     Vector3.right, Vector3.up, mat);
                check("箱を 5 個置けた", door >= 0, "扉 id=" + door);

                // ── 3. 録る。★Unity と同じ呼び順（実体 → リスナー → 音源 → Update）──
                scene.CaptureBegin(60, 20, 8, 48000, true);
                var pcm = new float[512 * 2];
                const ulong id = 9001;
                for (int k = 0; k < 130; ++k)
                {
                    scene.UpdateInstance(door, new Vector3(.002f * k, h * .5f, 0),
                                         new Vector3(.5f, h * .5f, .05f), Vector3.right, Vector3.up);
                    float lz = -4f + .02f * k;
                    if (k >= 60) lz += 3f;                    // ★本物の跳びを仕込む
                    scene.SetListener(new Vector3(.2f, 1.5f, lz));
                    scene.SetSource(id, new Vector3(.2f, 1.5f, 3f));
                    for (int i = 0; i < pcm.Length; ++i) pcm[i] = .2f * Mathf.Sin(.01f * i);
                    scene.CapturePushAudio(pcm, 512);
                    if (k == 100) scene.CaptureMark();
                    scene.Update(1f / 60f);
                }
                int held;
                int st = scene.CaptureStatus(out held);
                check("マーク後に「保存できる」状態になる", st == 2, "状態 " + st + " / 保持 " + held);

                bool wrote = scene.CaptureWrite(path, "SelfTest", 0xABCDu);
                check("ファイルを書ける", wrote && File.Exists(path),
                      wrote ? new FileInfo(path).Length + " バイト" : path);
                if (wrote)
                {
                    // ── 4. 開いて中身を読む。★構造体の並びが合っているかがここで出る ──
                    cap = Native.AF_CaptureOpen(path);
                    check("AF_CaptureOpen で開ける", cap != System.IntPtr.Zero, "");
                    if (cap != System.IntPtr.Zero)
                    {
                        Native.AF_CaptureInfo info;
                        check("概要が取れる", Native.AF_CaptureGetInfo(cap, out info) == 1, "");

                        var nameBuf = new byte[128];
                        int n = Native.AF_CaptureGetSceneName(cap, nameBuf, nameBuf.Length);
                        string capScene = n > 0
                            ? System.Text.Encoding.UTF8.GetString(nameBuf, 0, n) : "";
                        // ★並びがずれていると、ここの数字が入れ替わって出る。**値まで見る。**
                        check("構造体の並びが合っている（値が入れ替わっていない）",
                              capScene == "SelfTest" && info.dllHash == 0xABCDu
                              && info.frames == 81 && info.boxCount == 5 && info.materialCount >= 1
                              && info.sourceCount == 1 && info.pcmFrames > 0,
                              string.Format("場面 {0} / hash {1:x} / {2}f / 箱 {3} / 材質 {4} / 音源 {5} / PCM {6}",
                                            capScene, info.dllHash, info.frames, info.boxCount,
                                            info.materialCount, info.sourceCount, info.pcmFrames));

                        // ── 5. 走査。★仕込んだ跳びを見つけるか ──
                        int total = Native.AF_CaptureScan(cap, null, 0);
                        var marks = new Native.AF_CaptureMark[Mathf.Max(total, 1)];
                        if (total > 0) Native.AF_CaptureScan(cap, marks, total);
                        // ★印が **managed 側へ書き戻っているか**を先に見る。
                        //   AF_CaptureMark は blittable でないので、[In, Out] が無いと
                        //   ネイティブの書き込みが戻らず**全部 0／文字列は空**になる。
                        //   例外は出ないので、ここを分けておかないと原因が見えない。
                        bool filled = total > 0 && (marks[0].sourceId != 0 || marks[0].frame != 0
                                                    || marks[0].seconds != 0f);
                        check("走査の結果が C# 側へ書き戻っている（[In,Out] の検査）", filled,
                              total > 0 ? string.Format("先頭: f{0} {1:0.00}s 音源 {2}",
                                                        marks[0].frame, marks[0].seconds,
                                                        marks[0].sourceId)
                                        : "印が 0 件");

                        bool foundJump = false;
                        var seen = new System.Text.StringBuilder();
                        for (int i = 0; i < total; ++i)
                        {
                            string t = Native.Utf8(marks[i].templateName);
                            if (i < 4) seen.Append(string.Format("[f{0} {1}] ", marks[i].frame, t));
                            if (t.Contains("型紙1") && marks[i].frame >= 60 && marks[i].frame <= 62)
                                foundJump = true;
                        }
                        check("走査が仕込んだ跳び（瞬間移動）を見つける", foundJump,
                              "印 " + total + " 件  " + seen.ToString());

                        // ★文字化けの検査。**例外が出ないので、見るまで気づけない類**。
                        string tmpl = total > 0 ? Native.Utf8(marks[0].templateName) : "";
                        check("型紙名が UTF-8 で読めている（化けていない）",
                              total > 0 && tmpl.StartsWith("型紙"), "「" + tmpl + "」");

                        // ── 6. PCM が取れるか ──
                        var buf = new float[Mathf.Max(info.pcmFrames, 1) * 2];
                        int got = Native.AF_CaptureGetPcm(cap, buf, info.pcmFrames);
                        float peak = 0f;
                        for (int i = 0; i < got * 2 && i < buf.Length; ++i)
                            peak = Mathf.Max(peak, Mathf.Abs(buf[i]));
                        check("マスターPCM が取れて無音でない", got > 0 && peak > 0.01f,
                              string.Format("{0:0.00} 秒 / ピーク {1:0.000}",
                                            info.sampleRate > 0 ? got / (float)info.sampleRate : 0f,
                                            peak));

                        // ── 7. 回帰テストを吐けるか ──
                        var code = new byte[1 << 20];
                        int len = Native.AF_CaptureEmitCase(cap, 0, "testGenerated_SelfTest",
                                                            code, code.Length);
                        string src = Native.Utf8(code);
                        check("回帰テストを吐ける",
                              len > 0 && src.Contains("AF_SceneAddInstanceBox")
                              && src.Contains("AF_SceneAddMaterial") && src.Contains("af::detect::"),
                              len + " 文字");
                    }
                }

                // ── 8. 保存先に本当に書けるか（ビルドでも書ける場所か）──
                try
                {
                    Directory.CreateDirectory(CaptureRecorder.CaptureDir);
                    string probe = Path.Combine(CaptureRecorder.CaptureDir, ".probe");
                    File.WriteAllText(probe, "x");
                    File.Delete(probe);
                    check("録音の保存先に書ける", true, CaptureRecorder.CaptureDir);
                }
                catch (System.Exception e) { check("録音の保存先に書ける", false, e.Message); }
            }
            catch (System.Exception e)
            {
                check("例外が出ていない", false, e.GetType().Name + ": " + e.Message);
                log.AppendLine(e.StackTrace);
            }
            finally
            {
                if (cap != System.IntPtr.Zero) Native.AF_CaptureClose(cap);
                if (scene != null) scene.Dispose();
                try { if (File.Exists(path)) File.Delete(path); } catch (System.Exception) { }
            }

            log.AppendLine(f == 0
                ? string.Format("[OK] {0} 件すべて通りました。Unity の中でキャプチャの道は繋がっています。", p)
                : string.Format("[NG] {0} / {1} 件が落ちました。", f, p + f));
            report = log.ToString();
            pass = p; fail = f;
            return f == 0;
        }
    }
}
