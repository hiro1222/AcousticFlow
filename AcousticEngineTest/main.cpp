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

    // --- OBB（回転壁）---
    // 経路(listener→source, +X)の脇に長い薄板を置く。長辺X(±5)・薄辺Z(±0.2)・
    // 中心z=4 の軸並行板は経路(z=0)を遮らないが、Y軸まわりに90°回して長辺をZ向きに
    // すると板が経路を横切って遮蔽する。回転が交差判定に効くことの確認。
    AcousticEngine_ClearGeometry(engine);
    std::printf("OBB(回転壁): listener(0,0,0) <-> source(10,0,0)\n");
    AcousticEngine_AddBox(engine, V(5, 0, 4), V(5, 2, 0.2f));  // 軸並行 → 経路外
    expect("軸並行の板は経路外", AcousticEngine_IsOccluded(engine, listener, source), 0);
    AcousticEngine_ClearGeometry(engine);
    // right=+Z / up=+Y → ローカルX(長辺)がワールドZを向く＝経路を横切る。
    AcousticEngine_AddBoxOriented(engine, V(5, 0, 4), V(5, 2, 0.2f),
                                  V(0, 0, 1), V(0, 1, 0), nullptr, nullptr, nullptr, 0);
    expect("90度回転で経路を遮る", AcousticEngine_IsOccluded(engine, listener, source), 1);

    // --- AddBoxOriented の帯域別マテリアル（6+6配列）---
    // 透過率の高い板は遮蔽が小さく、低い板は大きい。配列が境界を越えて効くことの確認。
    {
        const float absorp[6] = {0.05f, 0.05f, 0.05f, 0.05f, 0.05f, 0.05f};
        const float transHi[6] = {0.9f, 0.9f, 0.9f, 0.9f, 0.9f, 0.9f};
        const float transLo[6] = {0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f};
        AcousticEngine_ClearGeometry(engine);
        AcousticEngine_AddBoxOriented(engine, V(5, 0, 0), V(0.5f, 2, 2),
                                      V(1, 0, 0), V(0, 1, 0), transHi, absorp, nullptr, 6);
        const float occHi = AcousticEngine_ComputeOcclusion(engine, listener, source);
        AcousticEngine_ClearGeometry(engine);
        AcousticEngine_AddBoxOriented(engine, V(5, 0, 0), V(0.5f, 2, 2),
                                      V(1, 0, 0), V(0, 1, 0), transLo, absorp, nullptr, 6);
        const float occLo = AcousticEngine_ComputeOcclusion(engine, listener, source);
        std::printf("  透過0.9の遮蔽=%.3f  透過0.1の遮蔽=%.3f\n", occHi, occLo);
        expect("高透過マテリアルほど遮蔽が小さい", (occHi < occLo) ? 1 : 0, 1);
    }

    // --- メッシュ occluder（三角形＋BVH）---
    // x=5 に四角い壁（2三角形）を置き、リスナー(0,0,0)→音源(10,0,0)が貫く。
    // BVH レイキャスト・線分遮蔽・静的保持(ClearGeometryで消えない)を確認。
    AcousticEngine_ClearGeometry(engine);
    AcousticEngine_ClearMeshes(engine);
    std::printf("メッシュ occluder: listener(0,0,0) <-> source(10,0,0)\n");
    {
        const float verts[] = {
            5.f, -2.f, -2.f,   5.f, 2.f, -2.f,   5.f, 2.f, 2.f,   5.f, -2.f, 2.f,
        };
        const int idx[] = { 0, 1, 2, 0, 2, 3 };
        expect("メッシュ登録前は見通せる", AcousticEngine_IsOccluded(engine, listener, source), 0);
        const int meshId = AcousticEngine_AddMesh(engine, verts, 4, idx, 6, nullptr, nullptr, nullptr, 0);
        expect("AddMeshがID>=0を返す", (meshId >= 0) ? 1 : 0, 1);
        expect("メッシュが経路を遮る", AcousticEngine_IsOccluded(engine, listener, source), 1);
        // 無効化すると走査対象から外れて見通せる（BVHは保持＝再有効化で戻る）。
        AcousticEngine_SetMeshActive(engine, meshId, 0);
        expect("SetMeshActive(0)で見通せる", AcousticEngine_IsOccluded(engine, listener, source), 0);
        AcousticEngine_SetMeshActive(engine, meshId, 1);
        expect("SetMeshActive(1)で再び遮る", AcousticEngine_IsOccluded(engine, listener, source), 1);
        expectNear("メッシュ壁に+Xでヒット", AcousticEngine_DebugRaycast(engine, listener, V(1, 0, 0), 100.f), 5.0f, 0.001f);
        expectNear("メッシュ経路外は外れ(-1)", AcousticEngine_DebugRaycast(engine, listener, V(0, 1, 0), 100.f), -1.0f, 0.001f);
        const float occ = AcousticEngine_ComputeOcclusion(engine, listener, source);
        std::printf("  メッシュ壁の遮蔽 = %.3f （0でも1でもない連続値）\n", occ);
        expect("メッシュ遮蔽は0<occ<1", (occ > 0.0f && occ < 1.0f) ? 1 : 0, 1);
        // 箱用の ClearGeometry ではメッシュは消えない（静的）。
        AcousticEngine_ClearGeometry(engine);
        expect("ClearGeometryではメッシュ残る", AcousticEngine_IsOccluded(engine, listener, source), 1);
        // ClearMeshes で消える。
        AcousticEngine_ClearMeshes(engine);
        expect("ClearMeshesでメッシュ消える", AcousticEngine_IsOccluded(engine, listener, source), 0);
    }

    // --- 残響 RT60 較正（Eyring 解析解が基準）---
    // 閉じた 10x10x10 のコンクリート箱でエコグラムを作り、Schroeder 逆積分から RT60 を測って
    // Eyring RT60 = 0.161 V / (S·(-ln(1-a))) と比較する。a = 吸収+透過 の広帯域平均
    // （モデルは吸収でも透過でもエネルギーを失うので、実効吸収 = α+τ）。
    // ※ 反射重み較正の“客観的な的”。ここがズレていれば残響の decay が非物理。
    AcousticEngine_ClearGeometry(engine);
    AcousticEngine_ClearMeshes(engine);
    std::printf("残響RT60較正: 10x10x10 コンクリート箱\n");
    {
        const float H = 5.0f, T = 0.5f;                 // 内寸 ±5（=10m 立方）、壁厚 1
        const float trC[6] = { 0.15f, 0.10f, 0.06f, 0.03f, 0.02f, 0.01f };  // concrete
        const float abC[6] = { 0.02f, 0.02f, 0.03f, 0.04f, 0.05f, 0.07f };
        const float scC[6] = { 0.05f, 0.08f, 0.12f, 0.18f, 0.25f, 0.35f };  // concrete scattering
        auto addWall = [&](AF_Vector3 ctr, AF_Vector3 hlf) {
            AcousticEngine_AddBoxOriented(engine, ctr, hlf, V(1, 0, 0), V(0, 1, 0), trC, abC, scC, 6);
        };
        addWall(V(0, -(H + T), 0), V(H + T, T, H + T));  // 床
        addWall(V(0,  (H + T), 0), V(H + T, T, H + T));  // 天井
        addWall(V(-(H + T), 0, 0), V(T, H + T, H + T));  // 左
        addWall(V( (H + T), 0, 0), V(T, H + T, H + T));  // 右
        addWall(V(0, 0, -(H + T)), V(H + T, H + T, T));  // 奥
        addWall(V(0, 0,  (H + T)), V(H + T, H + T, T));  // 手前

        const AF_Vector3 lis = V(0, 0, 0);
        const AF_Vector3 src = V(2, 0, 0);
        const int NBINS = 400;
        const float BIN = 0.01f;                          // 10ms/bin → 4s 窓
        static float bins[NBINS];
        // RT60 の尾(-35dB)まで見るには多バウンス要（refl≈0.9 で -35dB ≒ 76 反射）。
        // これは診断用の一回きり計測なので重くてよい（実時間はこの本数/反射では回さない）。
        AcousticEngine_ComputeEchogram(engine, lis, &src, 1, 4096, 150, bins, NBINS, BIN, 343.0f);

        // Schroeder 逆積分 → dB 曲線 → RT30(-5..-35dB)×2 で RT60。
        static double sch[NBINS];
        double acc = 0.0;
        for (int k = NBINS - 1; k >= 0; --k) { acc += bins[k]; sch[k] = acc; }
        const double e0 = (sch[0] > 0.0) ? sch[0] : 1e-12;
        double t5 = -1.0, t35 = -1.0;
        for (int k = 0; k < NBINS; ++k) {
            const double L = 10.0 * std::log10((sch[k] / e0) + 1e-12);
            const double t = (k + 0.5) * BIN;
            if (t5 < 0.0 && L <= -5.0) t5 = t;
            if (t35 < 0.0 && L <= -35.0) { t35 = t; break; }
        }
        const double rtMeasured = (t5 >= 0.0 && t35 >= 0.0) ? (t35 - t5) * 2.0 : -1.0;

        double aMean = 0.0;
        for (int b = 0; b < 6; ++b) aMean += (abC[b] + trC[b]);
        aMean /= 6.0;
        const double Vroom = 1000.0, Sroom = 600.0;     // 10^3, 6*10^2
        const double rtEyring = 0.161 * Vroom / (Sroom * (-std::log(1.0 - aMean)));
        std::printf("  a(吸収+透過)平均=%.3f  Eyring RT60=%.2fs  測定RT60=%.2fs\n",
                    aMean, rtEyring, rtMeasured);
        if (rtMeasured > 0.0) {
            const double ratio = rtMeasured / rtEyring;
            std::printf("  比 測定/理論 = %.2f （1.0 に近いほど物理的）\n", ratio);
            // 反射重み較正の回帰ガード。鏡面のみだと自由行程が伸びて 1〜1.6 倍程度に出る。
            // scattering 導入で 1.0 に寄る想定。ここが大きく外れたら decay モデルが壊れたサイン。
            expect("RT60がEyringの0.7〜2.0倍", (ratio >= 0.7 && ratio <= 2.0) ? 1 : 0, 1);
        } else {
            std::printf("  [INFO] 尾が-35dBに届かず測定不可（バウンス/窓 or 減衰過多）\n");
        }
    }

    // --- 回折（Maekawa 近似・角迂回）---
    // L(0,0,0)↔S(10,0,0) の直線を、経路をまたぐ大きな壁で塞ぐ。壁の角を回り込む
    // 回折ゲインが「遮蔽ありでも 0<gain、かつ低域>高域（低い音ほど回り込む）」になるか。
    AcousticEngine_ClearGeometry(engine);
    AcousticEngine_ClearMeshes(engine);
    std::printf("回折: listener(0,0,0) <-> source(10,0,0)\n");
    {
        float dif[6];
        // 壁なし → 回折ゲイン全帯域1.0（素通り）。
        AcousticEngine_ComputeDiffractionBands(engine, listener, source, dif, 6);
        expectNear("壁なし回折=1(125Hz)", dif[0], 1.0f, 0.001f);
        // 経路中央に薄い衝立（厚み薄・縦横に広い→上/横の稜線から回り込むしかない）。
        AcousticEngine_AddBox(engine, V(5, 0, 0), V(0.05f, 3, 3));
        expect("壁で直接は遮蔽", AcousticEngine_IsOccluded(engine, listener, source), 1);
        AcousticEngine_ComputeDiffractionBands(engine, listener, source, dif, 6);
        std::printf("  回折ゲイン 125Hz=%.3f  500Hz=%.3f  4kHz=%.3f\n", dif[0], dif[2], dif[5]);
        expect("回折で0<gain(125Hz)", (dif[0] > 0.0f && dif[0] < 1.0f) ? 1 : 0, 1);
        expect("低域>高域(回り込みは低い音ほど)", (dif[0] > dif[5]) ? 1 : 0, 1);
    }

    // --- 材質ベースの遮蔽（連続値）---
    // 既定マテリアル(defaultWall)の壁を経路に置くと、二値ではなく 0..1 の値になる。
    AcousticEngine_ClearGeometry(engine);
    AcousticEngine_ClearMeshes(engine);
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
