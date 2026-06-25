// Program.cs
// C#(.NET) から AcousticEngine.dll を P/Invoke で呼ぶ確認テスト。
//   1) バージョン取得（プリミティブ戻り値）
//   2) 可視性判定（構造体マーシャリング + ハンドル受け渡し）
// 本番の呼び出し元 Unity と同じ「C# -> C++」境界を検証する。
using System;
using System.Runtime.InteropServices;

// 境界で受け渡す POD 構造体。
// C 側 AF_Vector3 (float x,y,z) とメモリ配置を一致させる。
//   LayoutKind.Sequential = 宣言順にそのまま並べる（C 構造体と同じ規則）。
[StructLayout(LayoutKind.Sequential)]
internal struct Vector3
{
    public float x;
    public float y;
    public float z;
    public Vector3(float x, float y, float z) { this.x = x; this.y = y; this.z = z; }
}

internal static class Native
{
    private const string Dll = "AcousticEngine.dll";
    private const CallingConvention Cc = CallingConvention.Cdecl;

    [DllImport(Dll, CallingConvention = Cc)]
    public static extern int AcousticEngine_GetVersion();

    // C++ の不透明ハンドル(void*) は C# では IntPtr で受ける。
    [DllImport(Dll, CallingConvention = Cc)]
    public static extern IntPtr AcousticEngine_Create();

    [DllImport(Dll, CallingConvention = Cc)]
    public static extern void AcousticEngine_Destroy(IntPtr engine);

    // 構造体は値渡し。Sequential レイアウトなのでそのままマーシャリングされる。
    [DllImport(Dll, CallingConvention = Cc)]
    public static extern void AcousticEngine_AddBox(IntPtr engine, Vector3 center, Vector3 halfExtents);

    [DllImport(Dll, CallingConvention = Cc)]
    public static extern void AcousticEngine_ClearGeometry(IntPtr engine);

    [DllImport(Dll, CallingConvention = Cc)]
    public static extern int AcousticEngine_IsOccluded(IntPtr engine, Vector3 from, Vector3 to);

    // 音声バックエンド(Wwise)ライフサイクル。
    [DllImport(Dll, CallingConvention = Cc)]
    public static extern int AcousticEngine_IsWwiseAvailable();

    [DllImport(Dll, CallingConvention = Cc)]
    public static extern int AcousticEngine_InitAudio();

    [DllImport(Dll, CallingConvention = Cc)]
    public static extern int AcousticEngine_IsAudioInitialized();

    [DllImport(Dll, CallingConvention = Cc)]
    public static extern void AcousticEngine_ShutdownAudio();
}

internal static class Program
{
    private static int s_failures;

    private static void Expect(string label, int actual, int expected)
    {
        bool ok = actual == expected;
        Console.WriteLine($"  [{(ok ? "OK" : "FAIL")}] {label,-22} actual={actual} expected={expected}");
        if (!ok) s_failures++;
    }

    private static int Main()
    {
        // --- 1) 境界疎通 ---
        int v = Native.AcousticEngine_GetVersion();
        Console.WriteLine($"[C#] AcousticEngine version raw={v}");
        if (v <= 0)
        {
            Console.WriteLine("[FAIL] バージョン取得に失敗。");
            return 1;
        }

        // --- 2) 可視性判定 ---
        IntPtr engine = Native.AcousticEngine_Create();
        if (engine == IntPtr.Zero)
        {
            Console.WriteLine("[FAIL] エンジン生成に失敗。");
            return 1;
        }

        var listener = new Vector3(0, 0, 0);
        var source = new Vector3(10, 0, 0);
        Console.WriteLine("シナリオ: listener(0,0,0) <-> source(10,0,0)");

        Expect("障害物なし", Native.AcousticEngine_IsOccluded(engine, listener, source), 0);

        Native.AcousticEngine_AddBox(engine, new Vector3(5, 0, 0), new Vector3(0.5f, 2, 2));
        Expect("中央に壁あり", Native.AcousticEngine_IsOccluded(engine, listener, source), 1);

        Native.AcousticEngine_ClearGeometry(engine);
        Expect("壁を撤去後", Native.AcousticEngine_IsOccluded(engine, listener, source), 0);

        Native.AcousticEngine_AddBox(engine, new Vector3(5, 10, 0), new Vector3(0.5f, 2, 2));
        Expect("経路外の壁", Native.AcousticEngine_IsOccluded(engine, listener, source), 0);

        Native.AcousticEngine_Destroy(engine);

        // --- 3) 音声バックエンド(Wwise) ---
        Console.WriteLine($"Wwise available: {Native.AcousticEngine_IsWwiseAvailable()}");
        if (Native.AcousticEngine_IsWwiseAvailable() == 1)
        {
            Expect("Wwise初期化", Native.AcousticEngine_InitAudio(), 1);
            Expect("初期化状態", Native.AcousticEngine_IsAudioInitialized(), 1);
            Native.AcousticEngine_ShutdownAudio();
            Expect("終了後の状態", Native.AcousticEngine_IsAudioInitialized(), 0);
        }
        else
        {
            Console.WriteLine("  (Wwise 未導入ビルドのため音声初期化はスキップ)");
        }

        Console.WriteLine("----");
        if (s_failures == 0)
        {
            Console.WriteLine("[OK] C# -> C++ の全チェックに合格しました。");
            return 0;
        }
        Console.WriteLine($"[FAIL] {s_failures} 件のチェックに失敗しました。");
        return 1;
    }
}
