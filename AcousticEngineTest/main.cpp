/* main.cpp
 * AcousticEngine DLL の C++ 動作確認。
 *   1) 境界疎通（バージョン取得）
 *   2) 可視性判定（ステップ3）: 壁の有無で遮蔽結果が変わることを確認
 */
#include <cmath>
#include <cstdio>

#include "acoustic_engine.h"

namespace {

AF_Vector3 V(float x, float y, float z) { return AF_Vector3{ x, y, z }; }

int g_failures = 0;

// 期待値と実測を比較して結果を表示する小さなチェックヘルパ。
void expect(const char* label, int actual, int expected) {
    const bool ok = (actual == expected);
    std::printf("  [%s] %-28s actual=%d expected=%d\n",
                ok ? "OK" : "FAIL", label, actual, expected);
    if (!ok) ++g_failures;
}

// 浮動小数の近似比較版。
void expectNear(const char* label, float actual, float expected, float tol) {
    const bool ok = (std::fabs(actual - expected) <= tol);
    std::printf("  [%s] %-28s actual=%.3f expected=%.3f\n",
                ok ? "OK" : "FAIL", label, actual, expected);
    if (!ok) ++g_failures;
}

}  // namespace

int main() {
    // --- 1) 境界疎通 ---
    const int version = AcousticEngine_GetVersion();
    std::printf("AcousticEngine version raw=%d\n", version);
    if (version <= 0) {
        std::printf("[FAIL] バージョン取得に失敗。\n");
        return 1;
    }

    // --- 2) 可視性判定 ---
    AcousticEngineHandle engine = AcousticEngine_Create();
    if (!engine) {
        std::printf("[FAIL] エンジン生成に失敗。\n");
        return 1;
    }

    const AF_Vector3 listener = V(0, 0, 0);
    const AF_Vector3 source   = V(10, 0, 0);  // x 軸上の 10m 先

    std::printf("シナリオ: listener(0,0,0) <-> source(10,0,0)\n");

    // 障害物なし -> 見通せる(0)
    expect("障害物なし", AcousticEngine_IsOccluded(engine, listener, source), 0);

    // 経路の中央(x=5)に壁を置く -> 遮蔽(1)
    AcousticEngine_AddBox(engine, V(5, 0, 0), V(0.5f, 2, 2));
    expect("中央に壁あり", AcousticEngine_IsOccluded(engine, listener, source), 1);

    // 壁を消す -> 再び見通せる(0)
    AcousticEngine_ClearGeometry(engine);
    expect("壁を撤去後", AcousticEngine_IsOccluded(engine, listener, source), 0);

    // 経路から外れた位置(y=10)の壁 -> 遮蔽しない(0)
    AcousticEngine_AddBox(engine, V(5, 10, 0), V(0.5f, 2, 2));
    expect("経路外の壁", AcousticEngine_IsOccluded(engine, listener, source), 0);

    // --- レイキャスト（ステップ5の最小ユニット）---
    // 原点(0,0,0)から +X 方向。中央(5,0,0,half=0.5)の壁の手前面は x=4.5。
    AcousticEngine_ClearGeometry(engine);
    AcousticEngine_AddBox(engine, V(5, 0, 0), V(0.5f, 2, 2));
    std::printf("レイキャスト: origin(0,0,0)\n");
    expectNear("+X方向で壁にヒット", AcousticEngine_DebugRaycast(engine, listener, V(1, 0, 0), 100.0f), 4.5f, 0.001f);
    expectNear("+Y方向は外れ(-1)", AcousticEngine_DebugRaycast(engine, listener, V(0, 1, 0), 100.0f), -1.0f, 0.001f);
    expectNear("maxDist手前で届かず(-1)", AcousticEngine_DebugRaycast(engine, listener, V(1, 0, 0), 3.0f), -1.0f, 0.001f);

    // --- 反射経路（ステップ5本体）---
    // 壁(手前面x=4.5, 法線-X)に +X で当てると、まっすぐ -X へ跳ね返る。
    // 経路: [origin(0,0,0), 反射点(4.5,0,0), 終端(-X方向)]
    {
        AF_Vector3 path[8];
        const int n = AcousticEngine_TraceReflectionPath(engine, listener, V(1, 0, 0),
                                                         100.0f, 1, path, 8);
        expect("反射経路の点数", n, 3);
        expectNear("反射点X=4.5", path[1].x, 4.5f, 0.001f);
        // 反射後は -X 方向へ戻るので、終端は反射点より手前(x<4.5)。
        expect("反射後は-X方向へ戻る", (path[2].x < path[1].x) ? 1 : 0, 1);
    }

    // --- 材質ベースの遮蔽（連続値）---
    // 既定マテリアル(defaultWall)の壁を経路に置くと、二値ではなく 0..1 の値になる。
    AcousticEngine_ClearGeometry(engine);
    std::printf("材質ベース遮蔽: listener(0,0,0) <-> source(10,0,0)\n");
    expectNear("壁なし=遮蔽0", AcousticEngine_ComputeOcclusion(engine, listener, source), 0.0f, 0.001f);

    AcousticEngine_AddBox(engine, V(5, 0, 0), V(0.5f, 2, 2));  // 既定マテリアル
    const float occ1 = AcousticEngine_ComputeOcclusion(engine, listener, source);
    std::printf("  壁1枚の遮蔽 = %.3f （0でも1でもない連続値）\n", occ1);
    expect("壁1枚は0<occ<1", (occ1 > 0.0f && occ1 < 1.0f) ? 1 : 0, 1);

    // 壁を2枚重ねると透過がさらに減り、遮蔽が強くなる。
    AcousticEngine_AddBox(engine, V(7, 0, 0), V(0.5f, 2, 2));
    const float occ2 = AcousticEngine_ComputeOcclusion(engine, listener, source);
    std::printf("  壁2枚の遮蔽 = %.3f （1枚より大きいはず）\n", occ2);
    expect("2枚>1枚", (occ2 > occ1) ? 1 : 0, 1);

    // --- レイ積分の遮蔽（反射で回り込む成分を数える）---
    // 直接線を壁で完全に塞ぎつつ、床・天井で反射して回り込めるよう開けておく。
    // 直線版は 1.0 付近に張り付くが、レイ積分版は反射の回り込みで緩むはず。
    AcousticEngine_ClearGeometry(engine);
    std::printf("レイ積分の遮蔽: listener(0,0,0) <-> source(10,0,0)\n");

    // 壁なし → レイ積分でも遮蔽ほぼ0。
    expectNear("壁なし=遮蔽ほぼ0",
               AcousticEngine_ComputeOcclusionRayIntegrated(engine, source, listener, 128, 2),
               0.0f, 0.05f);

    // 直接線を塞ぐ壁（高さ低めで上下に回り込める）＋ 反射面（床・天井）。
    AcousticEngine_AddBox(engine, V(5, 0, 0),  V(0.5f, 1.5f, 4));   // 直接遮蔽壁
    AcousticEngine_AddBox(engine, V(0, -3, 0), V(20, 0.5f, 20));    // 床
    AcousticEngine_AddBox(engine, V(0, 3, 0),  V(20, 0.5f, 20));    // 天井

    const float straightOcc = AcousticEngine_ComputeOcclusion(engine, source, listener);
    const float rayOcc = AcousticEngine_ComputeOcclusionRayIntegrated(engine, source, listener, 256, 3);
    std::printf("  直線版=%.3f  レイ積分版=%.3f （レイ積分の方が小さいはず）\n", straightOcc, rayOcc);
    expect("レイ積分<直線（反射で緩む）", (rayOcc < straightOcc) ? 1 : 0, 1);
    expect("レイ積分は0<occ<1", (rayOcc > 0.0f && rayOcc < 1.0f) ? 1 : 0, 1);

    AcousticEngine_Destroy(engine);

    // --- 音源指向性（directivity・ハンドル不要のステートレス計算）---
    // 音源を原点に置き、主放射方向 forward=+X とする。
    //   listener=+X 前方 / +Y 側面 / -X 背面 で値の出方を確認。
    std::printf("音源指向性: source(0,0,0) forward=+X\n");
    const AF_Vector3 srcPos  = V(0, 0, 0);
    const AF_Vector3 fwdX    = V(1, 0, 0);
    const AF_Vector3 front   = V(1, 0, 0);   // 正面 (cosθ=+1)
    const AF_Vector3 side    = V(0, 1, 0);   // 側面 (cosθ=0)
    const AF_Vector3 back    = V(-1, 0, 0);  // 背面 (cosθ=-1)
    float lp = -1.0f;

    // Omni: 向きに関係なく 1、ローパスなし。
    expectNear("Omni正面ゲイン", AcousticEngine_ComputeDirectivity(srcPos, fwdX, front, AF_DIRECTIVITY_OMNI, &lp), 1.0f, 0.001f);
    expectNear("Omni背面ゲイン", AcousticEngine_ComputeDirectivity(srcPos, fwdX, back, AF_DIRECTIVITY_OMNI, &lp), 1.0f, 0.001f);
    expectNear("Omniローパス0",  lp, 0.0f, 0.001f);

    // Cardioid: 正面=1 / 側面・背面は小さく、ローパスが乗る。
    expectNear("Cardioid正面=1",  AcousticEngine_ComputeDirectivity(srcPos, fwdX, front, AF_DIRECTIVITY_CARDIOID, &lp), 1.0f, 0.001f);
    const float cardSide = AcousticEngine_ComputeDirectivity(srcPos, fwdX, side, AF_DIRECTIVITY_CARDIOID, &lp);
    std::printf("  Cardioid側面ゲイン=%.3f ローパス=%.3f\n", cardSide, lp);
    expect("Cardioid側面<正面", (cardSide < 1.0f) ? 1 : 0, 1);
    expect("Cardioid側面でローパス>0", (lp > 0.0f) ? 1 : 0, 1);
    const float cardBack = AcousticEngine_ComputeDirectivity(srcPos, fwdX, back, AF_DIRECTIVITY_CARDIOID, &lp);
    expect("Cardioid背面<=側面", (cardBack <= cardSide + 0.001f) ? 1 : 0, 1);

    // Bidirectional: 正面と背面はどちらも大きく、側面で落ちる。
    const float biFront = AcousticEngine_ComputeDirectivity(srcPos, fwdX, front, AF_DIRECTIVITY_BIDIRECTIONAL, &lp);
    const float biBack  = AcousticEngine_ComputeDirectivity(srcPos, fwdX, back, AF_DIRECTIVITY_BIDIRECTIONAL, &lp);
    const float biSide  = AcousticEngine_ComputeDirectivity(srcPos, fwdX, side, AF_DIRECTIVITY_BIDIRECTIONAL, &lp);
    std::printf("  Bidir 正面=%.3f 背面=%.3f 側面=%.3f\n", biFront, biBack, biSide);
    expectNear("Bidir 正面≒背面", biFront, biBack, 0.001f);
    expect("Bidir 側面<正面", (biSide < biFront) ? 1 : 0, 1);

    // Beam: Cardioid より側面の落ち込みが急（より指向性が鋭い）。
    const float beamSide = AcousticEngine_ComputeDirectivity(srcPos, fwdX, V(0.5f, 0.866f, 0), AF_DIRECTIVITY_BEAM, &lp);
    const float cardSide2 = AcousticEngine_ComputeDirectivity(srcPos, fwdX, V(0.5f, 0.866f, 0), AF_DIRECTIVITY_CARDIOID, &lp);
    std::printf("  60deg Beam=%.3f Cardioid=%.3f\n", beamSide, cardSide2);
    expect("Beamの方が鋭い(60度で小)", (beamSide < cardSide2) ? 1 : 0, 1);

    // --- 3) 音声バックエンド(Wwise)の起動・終了 ---
    std::printf("Wwise available: %d\n", AcousticEngine_IsWwiseAvailable());
    if (AcousticEngine_IsWwiseAvailable()) {
        const int inited = AcousticEngine_InitAudio();
        expect("Wwise初期化", inited, 1);
        expect("初期化状態", AcousticEngine_IsAudioInitialized(), 1);
        AcousticEngine_ShutdownAudio();
        expect("終了後の状態", AcousticEngine_IsAudioInitialized(), 0);
    } else {
        std::printf("  (Wwise 未導入ビルドのため音声初期化はスキップ)\n");
    }

    std::printf("----\n");
    if (g_failures == 0) {
        std::printf("[OK] すべてのチェックに合格しました。\n");
        return 0;
    }
    std::printf("[FAIL] %d 件のチェックに失敗しました。\n", g_failures);
    return 1;
}
