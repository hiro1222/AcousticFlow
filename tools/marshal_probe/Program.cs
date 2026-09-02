// Program.cs ── P/Invoke のマーシャリングを**実行して**確かめる小道具。
//
// ★なぜ要るか
//   `tools/check_csharp.ps1` は型検査までしか見ない。マーシャリングの属性
//   （[In, Out] / ByValArray / CharSet）は**型としては正しい**ので、
//   型検査では絶対に捕まらない。しかも間違っても例外は出ず、
//   **値が 0 になる／文字列が空になる**だけなので、目で見るまで気づけない。
//
//   実際にこれで自己診断が「印 1 件・型紙名『』」になった。
//   原因は AF_CaptureMark が byte[] を持つため **blittable でなく**、
//   blittable でない構造体の配列は既定で **In だけ**マーシャリングされること。
//
// ここでは同じエクスポートを **[In,Out] 有り／無し** の 2 通りで宣言して、
// 実際に差が出ることを見せる。差が出なくなったら、この道具は用済み。
using System;
using System.IO;
using System.Runtime.InteropServices;

internal static class Program
{
    private const string Dll = "AcousticEngine";
    private const CallingConvention Cc = CallingConvention.Cdecl;

    [StructLayout(LayoutKind.Sequential)]
    private struct Vec3 { public float x, y, z; public Vec3(float a, float b, float c) { x = a; y = b; z = c; } }

    [StructLayout(LayoutKind.Sequential)]
    private struct CaptureInfo
    {
        public int frames, sampleRate, maxSources, preroll, postroll, workerThreads, markedFrame;
        public uint dllHash;
        public int boxCount, meshCount, materialCount, pcmFrames, sourceCount;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct Mark
    {
        public ulong sourceId;
        public int frame;
        public float seconds;
        public float amount;
        public int reserved;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 40)] public byte[] templateName;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 24)] public byte[] what;
    }

    [DllImport(Dll, CallingConvention = Cc)] private static extern IntPtr AF_SceneCreate();
    [DllImport(Dll, CallingConvention = Cc)] private static extern void AF_SceneDestroy(IntPtr s);
    [DllImport(Dll, CallingConvention = Cc)]
    private static extern int AF_SceneAddMaterial(IntPtr s, float[] t, float[] a, float[] sc, int n);
    [DllImport(Dll, CallingConvention = Cc)]
    private static extern int AF_SceneAddInstanceBox(IntPtr s, Vec3 c, Vec3 h, Vec3 r, Vec3 u, int mat);
    [DllImport(Dll, CallingConvention = Cc)]
    private static extern void AF_SceneUpdateInstance(IntPtr s, int id, Vec3 c, Vec3 h, Vec3 r, Vec3 u);
    [DllImport(Dll, CallingConvention = Cc)] private static extern void AF_SceneSetListener(IntPtr s, Vec3 p);
    [DllImport(Dll, CallingConvention = Cc)] private static extern void AF_SceneSetSource(IntPtr s, ulong id, Vec3 p);
    [DllImport(Dll, CallingConvention = Cc)] private static extern void AF_SceneUpdate(IntPtr s, float dt);
    [DllImport(Dll, CallingConvention = Cc)]
    private static extern void AF_SceneCaptureBegin(IntPtr s, int pre, int post, int maxSrc, int sr, int pcm);
    [DllImport(Dll, CallingConvention = Cc)] private static extern void AF_SceneCaptureMark(IntPtr s);
    [DllImport(Dll, CallingConvention = Cc, CharSet = CharSet.Ansi)]
    private static extern int AF_SceneCaptureWrite(IntPtr s, string path, string scene, uint hash);
    [DllImport(Dll, CallingConvention = Cc, CharSet = CharSet.Ansi)]
    private static extern IntPtr AF_CaptureOpen(string path);
    [DllImport(Dll, CallingConvention = Cc)] private static extern void AF_CaptureClose(IntPtr c);
    [DllImport(Dll, CallingConvention = Cc)] private static extern int AF_CaptureGetInfo(IntPtr c, out CaptureInfo i);

    // ★同じエクスポートを 2 通りで宣言する。ここが実験の全部。
    [DllImport(Dll, CallingConvention = Cc, EntryPoint = "AF_CaptureScan")]
    private static extern int ScanNoAttr(IntPtr c, Mark[] outMarks, int maxOut);
    [DllImport(Dll, CallingConvention = Cc, EntryPoint = "AF_CaptureScan")]
    private static extern int ScanInOut(IntPtr c, [In, Out] Mark[] outMarks, int maxOut);

    private static string Utf8(byte[] raw)
    {
        if (raw == null) return "";
        int n = 0;
        while (n < raw.Length && raw[n] != 0) ++n;
        return n > 0 ? System.Text.Encoding.UTF8.GetString(raw, 0, n) : "";
    }

    private static int Main(string[] args)
    {
        if (args.Length > 0) NativeLibrary.SetDllImportResolver(typeof(Program).Assembly,
            (name, asm, path) => name == Dll
                ? NativeLibrary.Load(Path.Combine(args[0], "AcousticEngine.dll"))
                : IntPtr.Zero);

        string file = Path.Combine(Path.GetTempPath(), "af_marshal_probe.afcap");
        IntPtr s = AF_SceneCreate();
        int mat = AF_SceneAddMaterial(s, null, null, null, 0);
        const float h = 3f;
        AF_SceneAddInstanceBox(s, new Vec3(-2.25f, h * .5f, 0), new Vec3(1.75f, h * .5f, .1f),
                               new Vec3(1, 0, 0), new Vec3(0, 1, 0), mat);
        AF_SceneAddInstanceBox(s, new Vec3(2.25f, h * .5f, 0), new Vec3(1.75f, h * .5f, .1f),
                               new Vec3(1, 0, 0), new Vec3(0, 1, 0), mat);
        int door = AF_SceneAddInstanceBox(s, new Vec3(0, h * .5f, 0), new Vec3(.5f, h * .5f, .05f),
                                          new Vec3(1, 0, 0), new Vec3(0, 1, 0), mat);
        AF_SceneAddInstanceBox(s, new Vec3(0, -.1f, 0), new Vec3(6, .1f, 8),
                               new Vec3(1, 0, 0), new Vec3(0, 1, 0), mat);
        AF_SceneAddInstanceBox(s, new Vec3(0, h + .1f, 0), new Vec3(6, .1f, 8),
                               new Vec3(1, 0, 0), new Vec3(0, 1, 0), mat);

        AF_SceneCaptureBegin(s, 60, 20, 8, 48000, 0);
        for (int k = 0; k < 130; ++k)
        {
            AF_SceneUpdateInstance(s, door, new Vec3(.002f * k, h * .5f, 0),
                                   new Vec3(.5f, h * .5f, .05f), new Vec3(1, 0, 0), new Vec3(0, 1, 0));
            float lz = -4f + .02f * k;
            if (k >= 60) lz += 3f;                       // 仕込んだ跳び
            AF_SceneSetListener(s, new Vec3(.2f, 1.5f, lz));
            AF_SceneSetSource(s, 9001, new Vec3(.2f, 1.5f, 3f));
            if (k == 100) AF_SceneCaptureMark(s);
            AF_SceneUpdate(s, 1f / 60f);
        }
        int wrote = AF_SceneCaptureWrite(s, file, "MarshalProbe", 0xABCDu);
        AF_SceneDestroy(s);
        if (wrote != 1) { Console.WriteLine("[NG] キャプチャを書けませんでした"); return 2; }

        IntPtr cap = AF_CaptureOpen(file);
        if (cap == IntPtr.Zero) { Console.WriteLine("[NG] 開けませんでした"); return 2; }
        CaptureInfo info;
        AF_CaptureGetInfo(cap, out info);
        int total = ScanInOut(cap, null, 0);
        Console.WriteLine($"キャプチャ: {info.frames} フレーム / 箱 {info.boxCount} / 印 {total} 件");

        var a = new Mark[Math.Max(total, 1)];
        var b = new Mark[Math.Max(total, 1)];
        ScanNoAttr(cap, a, total);
        ScanInOut(cap, b, total);
        AF_CaptureClose(cap);
        try { File.Delete(file); } catch (Exception) { }

        Console.WriteLine($"  属性なし  : frame={a[0].frame} 音源={a[0].sourceId} 型紙=「{Utf8(a[0].templateName)}」");
        Console.WriteLine($"  [In,Out]  : frame={b[0].frame} 音源={b[0].sourceId} 型紙=「{Utf8(b[0].templateName)}」");

        bool ok = total > 0 && b[0].frame != 0 && Utf8(b[0].templateName).StartsWith("型紙");
        Console.WriteLine(ok
            ? "[OK] [In,Out] を付けると印が C# 側へ書き戻る（型紙名も UTF-8 で読める）"
            : "[NG] [In,Out] を付けても書き戻っていません");
        if (a[0].frame == b[0].frame && Utf8(a[0].templateName) == Utf8(b[0].templateName))
            Console.WriteLine("  ※この環境では属性なしでも同じでした（実装依存。属性は付けたままにすること）");
        return ok ? 0 : 1;
    }
}
