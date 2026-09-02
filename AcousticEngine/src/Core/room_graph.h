#pragma once
// Core/room_graph.h — 形状から「部屋」と「開口」を見つける。
//
// ■ なぜ要るか
//   未解決の筆頭が「部屋を移った瞬間に残響送出が 10dB 跳ねて上限に張り付く」で、
//   これは**部屋どうしの結合**の問題。ところがコアに「部屋」という概念が無いので
//   扱う土台が無かった。ここで幾何から部屋を作る。
//
// ■ 考え方
//   自由空間を塗り分ける。
//     1) 静的な形状をボクセル化して「詰まっている／空いている」に分ける
//     2) 各ボクセルの「壁からの距離」を測る
//     3) 距離が侵食半径以上のボクセル（＝種）の連結成分 ＝ 部屋
//     4) 種でないボクセルを最寄りの部屋へ塗り戻す（戸口の中にも部屋が付く）
//     5) 別の部屋どうしが接している面 ＝ 開口。壁で隔てられている所には出ない
//        （実体が塗り戻しを遮るので、壁の両側は必ず片方が実体になる）
//     6) 部屋ごとに境界面積と吸音率を集めて Sabine の RT60 を出す
//
// ■ なぜ残響時間を幾何から出すのか
//   エコグラムから測ると、窓の長さ（既定 100ビン×10ms = 1秒）に縛られて部屋の違いが
//   出ない。実測: 吸音率が 17.6 倍違う 2 部屋で、エコグラム由来の RT60 は 0.50 と 0.51 秒
//   ＝**区別できていない**。Sabine（RT60 = 0.161 V / Σ Sα）なら形と材質だけで決まるので、
//   同じ 2 部屋が 4.34 と 0.25 秒になる。しかも部屋ごとの定数なので、リスナーが動いても
//   値そのものは動かない ── 動くのは「どの部屋をどれだけ占めているか」だけになり、
//   そこが連続なら送出も連続になる。
//   ★開口（別の部屋へ抜けている面）は吸音率 1 として数える。そこから出た音はこの部屋に
//     戻らないので、音響的には穴＝完全吸音。部屋どうしの結合が「開口の面積」として
//     自動的に効く（結合のために別の仕組みを足さなくてよい）。
//
//   ★2〜3 の侵食が要る理由。素の連結成分だと、戸口で繋がった空間は全部ひとつの
//     部屋になる（＝扉の向こうも同じ部屋）。それでは残響を切り替える土台にならない。
//     壁から半径ぶん内側だけを残してから繋がりを見ると、幅が半径の 2 倍に満たない
//     **くびれ**が先に千切れるので、部屋が戸口で分かれる。
//     実測でも切り替わりは「幅 = 半径 × 2」にぴたり乗る（下表）。
//
//        戸口\半径   0.00   0.30   0.45   0.60   0.75   1.00     ← 格子 0.10m
//          0.0m        2      2      2      2      2      2
//          0.3m        1      2      2      2      2      2
//          0.6m        1      1      2      2      2      2
//          0.9m        1      1      1      2      2      2
//          1.2m        1      1      1      1      2      2
//          2.0m        1      1      1      1      1      1
//
//     半径 0.6m を既定にしてある。人が通る戸口（〜1.2m）は分かれ、
//     開けた口（2m〜）は分かれない。閾値は authoring ではなく幾何から決まる。
//
//   ★開口の実測（格子 0.10m、戸口は幅 w × 高さ 4.0m）
//        戸口 0.6m → 口 1 個・面積 2.00 m2（実寸 2.40）
//        戸口 0.9m → 口 1 個・面積 3.69 m2（実寸 3.60）  中心・法線とも実位置に一致
//        戸口 1.2m 以上 → 部屋が分かれないので口も出ない（＝ひと続きの空間）
//        仕切りに 0.9m と 0.6m の戸口 → 口 2 個（3.20 / 2.00 m2、中心も各戸口）
//     面積にはボクセル 1 個ぶんの丸めが乗る（幅が格子の刻みに量子化される）。
//     絶対値ではなく「実寸の 2 割以内」で扱うこと。
//
//   ★戸口以外の形でも、特別扱いを入れずに妥当な答えが出る（実測。格子 0.10m）
//        L字の曲がり角 幅2.0m/1.0m … 部屋1・口0   曲がるだけで広さは変わらない＝境界でない
//        大部屋に柱 1本/4本        … 部屋1・口0   回り込めるので境界でない
//        腰高の仕切り 高さ1.2m     … 部屋1・口0   上が開いているので境界でない
//        廊下(長さ6m)で繋いだ2部屋
//            幅 0.9m               … 部屋2・口1（3.2m2、廊下の中央）
//            幅 1.2m / 2.0m / 3.0m … 部屋1・口0   広い通路はひと続きの空間
//        食い違い壁 隙間1.0m       … 部屋2・口1（3.2m2）見通しは切れるが繋がっている
//        壁に窓 1.0×1.0m だけ      … 部屋2・口1（**1.0m2** ＝ 窓の実寸）
//     割れるのは「くびれ」だけで、曲がり角・柱・段差では割れない。
//     どこで割れるかは幅だけで決まるので、扉かどうかを見分ける必要がない。
//
//   ※ 幅の判定には半ボクセルぶんの甘さがある。実体は「ボクセル中心が入っていれば実体」
//     なので、自由空間が実寸より最大 1 ボクセル広く出る。幅 1.0m の廊下が半径 0.6m で
//     千切れないのはこれ（1.0m < 1.2m なのに残る）。格子を細かくすれば縮む。
//
//   ★ここに出るのは**戸口**（開口の器）であって、扉の開き具合ではない。扉は動くものとして
//     静的な塗り分けから外してある。残響の結合で部屋番号を使って切り替えると、
//     プレイヤーが必ず通る戸口のど真ん中に不連続を置くことになる。開き具合は回折・透過の
//     経路が連続量として出しているので、混合比はそちらから取ること。
//
//   リスナーに依存しないので、稜線探索から矩形を起こす案にあった
//   「見えた稜線しか持っていないので開口の高さが分からない・リスナーが動くと変わる」
//   という問題が出ない。幾何が変わったときだけ走らせればよく、毎フレームではない。
//
// ■ 静的とは何か
//   一度でも変換が更新されたインスタンスは「動くもの」として**除く**。
//   扉は部屋の仕切りではなく開口を塞ぐものなので、静的な塗り分けに入れると
//   閉扉時に戸口が消えてしまう（＝「そこに開口がある」という情報が幾何から失われる）。
//   これは authoring ではなく観測で決まるので、手続き生成でも自動で効く。
//
// ■ ブロック分割（差分更新の土台）
//   連結成分は本来「全体」の性質なので、素直に作ると壁が 1 枚壊れただけで
//   レベル全体を塗り直すことになる。そこで格子をブロック（既定 8³ ボクセル）に切り、
//     ・ブロックの中だけでラベリングする（ローカル番号 0,1,2…）
//     ・ブロック境界で「こちらの 2 番とあちらの 0 番は同じ空間」という対応表を作る
//     ・対応表を union-find で束ねたものが部屋
//   という形にした。壁が壊れたら、
//     ・触れたブロックだけ塗り直してローカル番号を振り直す
//     ・そのブロックの面の対応表だけ作り直す
//     ・union-find を張り直す（対応表の総数ぶんなので数千回＝誤差）
//   で済む。**他のブロックのローカル番号は一切変わらない**のがこの形の要点で、
//   更新コストがレベルの大きさに依存しなくなる。
//
//   「壊れた壁の向こうと繋がった」のような**全体の連結が変わる**変化でも、
//   変わるのは対応表だけなので同じコストで通る。八分木は空き空間を粗く持つので
//   初回構築は速くなるが、この性質は得られない（連結が変われば結局全部塗り直す）。
//   実行時に形状が変わる前提のエンジンなので、まず更新側を取った。
//
// ■ コスト（実測。12×4×16m の検証シーン。仕切りを 1 枚壊して 2 部屋 → 1 部屋）
//   既定（0.25m 格子・ブロック16・3-4-5・半径0.6m、74,256 ボクセル）:
//     全再構築 3.19 ms（確保0.10 / 塗り0.11 / 距離1.61 / 連結0.19 / 併合0.00 / 塗戻1.18）
//     差分更新 2.01 ms（         塗り0.05 / 距離0.78 / 連結0.10 / 併合0.00 / 塗戻1.09）
//   差分更新の結果は全再構築と一致する（部屋数・体積とも）。
//
//   ★段によって差分更新の効き方が違う。
//     塗り・連結・併合 … 触れたブロックだけ（16/40）。レベルの大きさに依存しない。
//     距離           … 汚れた範囲＋侵食半径ぶんの箱だけ。同上。
//     塗戻           … **全体**。戸口が開けば「どちらの部屋か」は遠くまで変わるし、
//                      部屋の番号自体が振り直されるので局所では閉じない。
//     結果として、差分更新のコストの半分以上が塗戻＝レベルの大きさに比例して残る。
//     40×10×40m を 0.25m で切ると約 102 万ボクセルで、全再構築 約43ms・
//     差分更新 約15ms の見込み。**まだ 1 フレームに収まらない。**
//     空き空間を粗く持つ八分木にすれば、距離も塗戻も空きの大部分を 1 ノードで
//     済ませられるのでここが縮む。未対応。
//
//   ブロック一辺の選び方（0.10m 格子・市街地距離で実測）:
//        8 : 全再構築 9.41 ms / 更新 0.407 ms
//       16 : 全再構築 5.44 ms / 更新 0.479 ms   ← 既定
//       32 : 全再構築 4.50 ms / 更新 0.790 ms
//     小さいほど更新は局所的になるが、走査線の区間がブロック幅で切り詰められて
//     全再構築が重くなる。8 は更新が 15% 速いだけで全再構築が 73% 遅い＝割に合わない。
//
//   ※ 連結成分の経緯：最初は 14ns/個で、その 92% が連結成分だった。整数除算を消して
//     11.4ns、走査線方式にして 4.0ns。「連結が重い」を測ってから手を打った結果で、
//     推測で塗りを速くしていたら 1 割も縮まなかった。
//     ブロック分割はそこから全再構築を 4.52→5.44ms に 2 割戻す代わりに、
//     更新を 11 倍安くしている。全再構築はロード時に 1 回、更新は遊んでいる最中に何度も
//     走るので、この交換は取る。
//
// ■ LOD / ストリーミングとの関係（既知の限界）
//   active=false のインスタンスは静的な塗り分けに入らない。つまり LOD で壁を降ろすと
//   **その壁が囲っていた部屋が「外の世界」に繋がって部屋でなくなる**。連結成分なので
//   隣の部屋まで巻き込んで消えることがある。
//   エンジンからは「LOD で降ろされた」のか「壊されて無くなった」のか区別できない。
//   音響的には後者の解釈が一貫している（無い壁は音を止めない）が、
//   運用としては「部屋を囲う壁は LOD で落とさない」という制約が要る。

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

#include "Core/aabb.h"
#include "Core/material.h"
#include "Core/vec3.h"

namespace acoustic {
namespace rooms {

// 塗り分けに渡す静的な形状。吸音率も一緒に持たせる。
//   部屋の残響時間を**形と材質から**出すため（Sabine）。エコグラムから測ると
//   レイのばらつきがそのまま乗るうえ、リスナー位置に依存して連続でなくなる。
struct SolidBox {
    Obb   obb;
    float absorption[kNumBands] = {0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f};
};

// 格子の中身（実体か空きか）。部屋番号はここには入れない（ブロックごとの
// ローカル番号 + union-find で引く。→ Builder::roomAtVoxel）。
enum : std::uint8_t {
    kEmpty = 0x00,
    kSolid = 0xFF,
};

// ブロック内のローカル番号。1 ブロックは既定 8³=512 ボクセルなので、
// 最悪（市松模様）でも 256 個までしか出ない。16bit で足りる。
enum : std::uint16_t {
    kLocNone  = 0xFFFE,   // 空きだがまだ番号なし
    kLocSolid = 0xFFFF,   // 実体
};

struct Grid {
    Vec3 origin{0, 0, 0};      // 格子の原点（最小コーナー）
    float cell = 0.25f;        // 一辺(m)
    int nx = 0, ny = 0, nz = 0;
    std::vector<std::uint8_t> v;   // nx*ny*nz。kEmpty / kSolid

    int index(int x, int y, int z) const { return (z * ny + y) * nx + x; }
    bool inside(int x, int y, int z) const {
        return x >= 0 && y >= 0 && z >= 0 && x < nx && y < ny && z < nz;
    }
    Vec3 center(int x, int y, int z) const {
        return Vec3(origin.x + (x + 0.5f) * cell,
                    origin.y + (y + 0.5f) * cell,
                    origin.z + (z + 0.5f) * cell);
    }
};

struct Room {
    int   voxels = 0;          // 体積（ボクセル数）
    Vec3  centroid{0, 0, 0};
    Vec3  boundsMin{0, 0, 0}, boundsMax{0, 0, 0};
    // ── 形と材質から出す残響（Sabine）──
    //   RT60 = 0.161 V / A、A = Σ(面積 × 吸音率)。
    //   ★開口（別の部屋へ抜けている面）は吸音率 1 として数える。そこから出た音は
    //     この部屋には戻らないので、音響的には穴＝完全吸音で正しい。部屋どうしの
    //     結合が「開口の面積」として自然に効く（結合のために別の仕組みを足さなくてよい）。
    float surface = 0.0f;                 // 境界の面積(m2)。開口を含む
    float openArea = 0.0f;                // そのうち開口ぶん(m2)
    float absorb[kNumBands] = {};         // 平均吸音率（帯域別）
    float rt60[kNumBands] = {};           // 残響時間(s)（帯域別）
};

// 部屋どうしを繋ぐくびれ（戸口・窓・壊れた壁の穴）。
//   塗り戻した後、別の部屋どうしが face で接している所がそのまま開口になる。
//   壁で隔てられているだけの所には出ない（実体が伝播を遮るので接する face が無い）。
//   ★扉そのものは入っていない。扉は「動くもの」として静的な塗り分けから外れているので、
//     ここに出るのは**戸口**（開口の器）であって、その開き具合ではない。
//     開き具合は既存の回折・透過の経路が連続量として出しているので、そちらと組む。
struct Aperture {
    int   roomA = -1, roomB = -1;   // 繋いでいる部屋（roomA < roomB）。外の世界なら roomA=-1
    bool  toOutside = false;        // 片側が「外の世界」か（Builder::setIncludeOutside が ON のときだけ出る）
    float area = 0.0f;              // 断面積(m2)
    Vec3  center{0, 0, 0};          // 断面の重心
    Vec3  normal{0, 0, 0};          // 面の向き（A→B が正）。面積で重み付けした平均
    // ── 外接矩形 ──
    //   ポータル（フレネル帯域積分の積分範囲）をここから自動で作るために持つ。
    //   法線のいちばん強い軸を面の向きとみなし、残り 2 軸で開口ボクセルの
    //   外接箱を取る。中心もこの箱の中心（重心ではない）── 積分範囲としては
    //   「開口全体を覆う矩形」が要るので、L 字の口でも取りこぼさないほうを採る。
    Vec3  axisU{1, 0, 0}, axisV{0, 1, 0};   // 矩形の 2 軸（world 軸のどれか）
    float halfU = 0.0f, halfV = 0.0f;       // 半幅(m)。セル 1 個ぶんの厚みを含む
    Vec3  rectCenter{0, 0, 0};              // 矩形の中心
};

struct Result {
    Grid grid;
    std::vector<Room> rooms;   // 添字がそのまま部屋番号
    std::vector<Aperture> apertures;
    int  discarded = 0;        // 小さすぎて捨てた連結成分の数
    int  outsideVoxels = 0;    // 「外の世界」に落ちたボクセル数（格子の外周に届いた成分）
    // 段別の所要時間(ms)。どこを削るべきかを推測でなく数字で決めるため。
    double msAlloc = 0.0, msFill = 0.0, msDist = 0.0, msLabel = 0.0,
           msMerge = 0.0, msGrow = 0.0, msAperture = 0.0;
    int  dirtyBricks = 0;      // 直近の更新で塗り直したブロック数（0 なら全再構築）
    int  totalBricks = 0;
};

// チャンファ距離の近傍。前進走査は「ラスタ順で既に確定した側」の近傍だけを見る。
//
// ■ 2 種類ある理由（実測で選んだ。既定は 3-4-5）
//   市街地距離（面だけ・3近傍・単位1）は 1 ボクセルあたり 6 回の参照で済んで速い。
//   3-4-5（面3/辺4/角5・13近傍・単位3）は 26 回掛かる。
//   最初は「戸口も壁も軸に並んだ板だから市街地で足りる」と踏んだが、実測すると
//   0.25m 格子で幅 0.9m の戸口が割れなかった（市街地=1部屋 / 3-4-5=2部屋）。
//   原因は斜め方向の精度ではなく**閾値の量子化**。市街地だと距離が整数ボクセルの
//   倍数にしかならず、0.6m/0.25m = 2.4 が 2 に丸まる。3-4-5 は斜めの歩幅（4,5）が
//   あるぶん 1/3 ボクセル刻みの値が出るので、2.33 ボクセルという閾値が表現できる。
//   0.15m 以下に細かくすれば両者は一致するが、**同じ品質での総コストは 3-4-5 の
//   粗い格子の方が安い**（実測: 3-4-5 @0.25m = 3.83ms/74k ボクセル、
//   市街地 @0.15m = 6.33ms/308k ボクセル）。メモリも 1/4 で済む。
struct ChamferNb { int dx, dy, dz, w; };

// 市街地距離（3近傍・単位 1）。
inline const ChamferNb* cityForward() {
    static const ChamferNb t[3] = { {-1,0,0,1}, {0,-1,0,1}, {0,0,-1,1} };
    return t;
}
inline const ChamferNb* cityBackward() {
    static const ChamferNb t[3] = { {1,0,0,1}, {0,1,0,1}, {0,0,1,1} };
    return t;
}
// 3-4-5（13近傍・単位 3）。比較用に残してある。
inline const ChamferNb* chamferForward() {
    static const ChamferNb t[13] = {
        {-1,-1,-1,5}, { 0,-1,-1,4}, { 1,-1,-1,5},
        {-1, 0,-1,4}, { 0, 0,-1,3}, { 1, 0,-1,4},
        {-1, 1,-1,5}, { 0, 1,-1,4}, { 1, 1,-1,5},
        {-1,-1, 0,4}, { 0,-1, 0,3}, { 1,-1, 0,4},
        {-1, 0, 0,3},
    };
    return t;
}
inline const ChamferNb* chamferBackward() {
    static const ChamferNb t[13] = {
        { 1, 1, 1,5}, { 0, 1, 1,4}, {-1, 1, 1,5},
        { 1, 0, 1,4}, { 0, 0, 1,3}, {-1, 0, 1,4},
        { 1,-1, 1,5}, { 0,-1, 1,4}, {-1,-1, 1,5},
        { 1, 1, 0,4}, { 0, 1, 0,3}, {-1, 1, 0,4},
        { 1, 0, 0,3},
    };
    return t;
}
enum : std::uint16_t { kFar = 0xFFFF };

// 点が OBB の中にあるか。
inline bool pointInObb(const Vec3& p, const Obb& b) {
    const Vec3 d = p - b.center;
    return std::fabs(dot(d, b.axisX)) <= b.halfExtents.x
        && std::fabs(dot(d, b.axisY)) <= b.halfExtents.y
        && std::fabs(dot(d, b.axisZ)) <= b.halfExtents.z;
}

// OBB を囲む軸並行境界（8 頂点から取るので回転していても正しい）。
inline Aabb obbBounds(const Obb& b) {
    Aabb r;
    r.min = Vec3( 1e30f,  1e30f,  1e30f);
    r.max = Vec3(-1e30f, -1e30f, -1e30f);
    for (int i = 0; i < 8; ++i) {
        const float sx = (i & 1) ? 1.0f : -1.0f;
        const float sy = (i & 2) ? 1.0f : -1.0f;
        const float sz = (i & 4) ? 1.0f : -1.0f;
        const Vec3 p = b.center + b.axisX * (b.halfExtents.x * sx)
                                + b.axisY * (b.halfExtents.y * sy)
                                + b.axisZ * (b.halfExtents.z * sz);
        r.min = Vec3(std::min(r.min.x, p.x), std::min(r.min.y, p.y), std::min(r.min.z, p.z));
        r.max = Vec3(std::max(r.max.x, p.x), std::max(r.max.y, p.y), std::max(r.max.z, p.z));
    }
    return r;
}

// ブロック内のローカル空間ひとつぶんの集計。ブロックを塗り直すまで作り直さない。
struct LocalStat {
    int count = 0;
    long long sx = 0, sy = 0, sz = 0;               // 格子座標の総和（重心用）
    int miX = 0, miY = 0, miZ = 0;
    int maX = 0, maY = 0, maZ = 0;
    bool touchesBoundary = false;                   // 格子の外周に接している
};

class Builder {
public:
    // ボクセル一辺(m)。戸口の幅を数ボクセルで割れる大きさにすること。
    void setCell(float m) {
        if (m > 1e-3f && m != cell_) { cell_ = m; needFull_ = true; }
    }
    /// 呼び出し側が**要求した**セル。実際に使われた値は result().grid.cell。
    ///   総ボクセル数が maxVoxels() を超えると、収まるまで 1.5 倍ずつ粗くするので
    ///   両者は食い違うことがある。**その降格は音の結果を変える**（戸口の幅を
    ///   数ボクセルで割れなくなると、部屋がそこで割れなくなる）ので、
    ///   ホストは 2 つを比べて気づけるようにしてある。
    float cell() const { return cell_; }
    std::size_t maxVoxels() const { return maxVoxels_; }

    // これ未満の連結成分は部屋として扱わない（隙間のノイズを捨てる）。
    void setMinVoxels(int n) {
        if (n != minVoxels_ && n > 0) { minVoxels_ = n; needFull_ = true; }
    }

    // ブロック一辺（ボクセル数）。差分更新の粒度。
    void setBrick(int n) {
        if (n >= 2 && n != brick_) { brick_ = n; needFull_ = true; }
    }

    // 【部屋を戸口で割る半径(m)】自由空間をこの半径ぶん侵食してから連結成分を取る。
    //   素の連結成分だと、戸口で繋がった空間は全部ひとつの部屋になってしまう
    //   （＝扉の向こうも同じ部屋。残響を切り替える土台にならない）。
    //   壁からこの距離より近い所を落としてから繋がりを見ると、幅がこの 2 倍に
    //   満たない**くびれ**は先に千切れるので、部屋が戸口で分かれる。
    //   落とした殻の部分は後で最寄りの部屋へ塗り戻すので、全ボクセルに部屋が付く。
    //   0 にすると侵食なし＝素の連結成分（従来の挙動）。
    //   目安: 戸口の幅の半分 < この値 < 部屋のいちばん狭い所の半分。
    void setSeedRadius(float m) {
        if (m >= 0.0f && m != seedRadius_) { seedRadius_ = m; needFull_ = true; }
    }
    float seedRadius() const { return seedRadius_; }
    /// 部屋と「外の世界」の間の口も開口として出すか（既定 OFF）。
    ///   洞窟の口・屋外へ開く戸口を「扉の定点」として使うために要る。
    ///   ★OFF のままなら従来と同じ（外へ開く口は出ない）。ON にすると roomA=-1 の開口が増え、
    ///     その部屋の Sabine の境界面積にも口が（吸音率 1 で）入る。
    void setIncludeOutside(bool on) {
        if (on != includeOutside_) { includeOutside_ = on; needFull_ = true; }
    }
    bool includeOutside() const { return includeOutside_; }

    // 距離の近似（1=3-4-5 の13近傍・既定 / 0=市街地距離の3近傍）。
    // 0 は 0.15m 以下の細かい格子でだけ使うこと（上の解説を参照）。
    void setChamferFull(bool on) {
        if (on != chamferFull_) { chamferFull_ = on; needFull_ = true; }
    }

    // 幾何が変わった領域を伝える。**変更前と変更後の両方**の境界を渡すこと
    // （動いた壁は「元居た所」も塗り直さないと実体が残る）。
    void touch(const Aabb& region) { dirtyRegions_.push_back(region); }
    // 全部作り直す（格子の刻みが変わった／形状が総入れ替えになった等）。
    void invalidateAll() { needFull_ = true; }
    bool dirty() const { return needFull_ || !dirtyRegions_.empty(); }

    // 最新の結果を返す。汚れていなければ何もしない。
    const Result& build(const std::vector<SolidBox>& boxes) {
        boxes_ = boxes;
        if (!dirty()) return res_;
        // 格子の範囲から外れる変更が来ていたら全部作り直すしかない。
        if (!needFull_ && !regionsFitGrid_()) needFull_ = true;
        if (needFull_) rebuildAll_(boxes);
        else           rebuildDirty_(boxes);
        dirtyRegions_.clear();
        needFull_ = false;
        return res_;
    }

    const Result& result() const { return res_; }

    // 格子座標がどの部屋か。-1 なら部屋の外／実体の中。
    int roomAtVoxel(int x, int y, int z) const {
        const Grid& g = res_.grid;
        if (!g.inside(x, y, z)) return -1;
        return room_[static_cast<std::size_t>(g.index(x, y, z))];
    }

    // 【点のまわりの部屋の占め方】半径 radius の球の中で、各部屋が占める割合を返す。
    //   書けた部屋数を返す（割合の大きい順、合計 1）。
    //
    // ★これが「部屋を音に使う」ための唯一の入口。部屋番号そのもので切り替えると、
    //   プレイヤーが必ず通る戸口のど真ん中に不連続を置くことになる ── このエンジンで
    //   潰してきた跳ねは全部「二値の判定が音に直結していた」ことが原因だった。
    //   割合なら、部屋の真ん中では 100:0、戸口では 50:50 と連続に変わる。
    //   残響の体積も減衰時間も、この割合で混ぜれば境界で跳ねない。
    //   radius は戸口の幅の 1〜2 倍が目安（そのぶんの距離をかけて入れ替わる）。
    //   ★shareOfSpace=true にすると「外の世界」（実体ではない部屋なし）も分母に入れる。
    //     既定（false）は部屋どうしの割合（合計 1）で、外へ開く戸口では球が部屋に触れた瞬間に
    //     0→1 と跳ぶ（実測: 戸口の 1.8 m 手前で 1 歩に 0→1.00）。屋外との境目で連続に
    //     混ぜたい量（扉の定点の (1−w)、外へ出るときの残響の量）は true で引くこと。
    int roomWeightsAt(const Vec3& p, float radius, int* outRooms, float* outWeights,
                      int maxOut, bool shareOfSpace = false) const {
        if (!outRooms || !outWeights || maxOut <= 0) return 0;
        const Grid& g = res_.grid;
        if (g.v.empty() || res_.rooms.empty()) return 0;
        const float r = std::max(radius, g.cell);
        // ★標本数と核の形は「連続かどうか」で決めた。
        //   最初は半径あたり 3 点（7³）＋核 (1-d²)² にしたが、0.1m 動くごとに割合が
        //   最大 0.364 も飛んだ。核が中心に尖りすぎていて実質 7 標本ぶんしか効いておらず、
        //   量子化が 1/7 で出ていた。半径あたり 6 点（13³）＋核 (1-d²) に変えて、
        //   球の中に約 900 標本が入るようにしてある。
        const int half = 6;
        const float step = r / static_cast<float>(half);
        const float inv = 1.0f / (r * r);

        // ★標本をずらす。等間隔のまま並べると、標本の間隔とボクセルの刻みが噛み合って
        //   リスナーが動いたときに**一斉に**ボクセル境界を跨ぐ（実測: 0.1m 動くごとに
        //   割合が 0.16 飛んだ）。位置をばらけさせると跨ぐ時刻がばらけて段が消える。
        //   ずれは (i,j,k) から決まるので、同じ点を何度引いても同じ答えになる。
        auto jitter = [](int a, int b, int c, int axis) -> float {
            unsigned h = 2166136261u;
            h = (h ^ static_cast<unsigned>(a * 73856093)) * 16777619u;
            h = (h ^ static_cast<unsigned>(b * 19349663)) * 16777619u;
            h = (h ^ static_cast<unsigned>(c * 83492791)) * 16777619u;
            h = (h ^ static_cast<unsigned>(axis * 2971215073u)) * 16777619u;
            return static_cast<float>((h >> 8) & 0xFFFF) / 65536.0f - 0.5f;
        };

        std::vector<float> acc(res_.rooms.size(), 0.0f);
        float totalW = 0.0f;
        for (int k = -half; k <= half; ++k)
            for (int j = -half; j <= half; ++j)
                for (int i = -half; i <= half; ++i) {
                    const float dx = (i + jitter(i, j, k, 0)) * step;
                    const float dy = (j + jitter(i, j, k, 1)) * step;
                    const float dz = (k + jitter(i, j, k, 2)) * step;
                    const float d2 = (dx*dx + dy*dy + dz*dz) * inv;
                    if (d2 >= 1.0f) continue;                 // 球の外
                    // 端で 0 になる重み。尖らせると中心の数標本で決まってしまう。
                    const float wgt = 1.0f - d2;
                    const int vx = static_cast<int>(std::floor((p.x + dx - g.origin.x) / g.cell));
                    const int vy = static_cast<int>(std::floor((p.y + dy - g.origin.y) / g.cell));
                    const int vz = static_cast<int>(std::floor((p.z + dz - g.origin.z) / g.cell));
                    const int rm = roomAtVoxel(vx, vy, vz);
                    if (rm < 0) {
                        // 実体の中は数えない。部屋の外（外の世界／格子の外）は shareOfSpace のときだけ分母へ。
                        if (shareOfSpace && (!g.inside(vx, vy, vz)
                                             || g.v[static_cast<std::size_t>(g.index(vx, vy, vz))] != kSolid))
                            totalW += wgt;
                        continue;
                    }
                    acc[static_cast<std::size_t>(rm)] += wgt;
                    totalW += wgt;
                }
        if (totalW <= 0.0f) return 0;

        // 大きい順に maxOut 個。
        int n = 0;
        std::vector<int> order(res_.rooms.size());
        for (std::size_t i = 0; i < order.size(); ++i) order[i] = static_cast<int>(i);
        std::sort(order.begin(), order.end(), [&](int a, int b) {
            return acc[static_cast<std::size_t>(a)] > acc[static_cast<std::size_t>(b)];
        });
        for (int i = 0; i < static_cast<int>(order.size()) && n < maxOut; ++i) {
            const float w = acc[static_cast<std::size_t>(order[i])];
            if (w <= 0.0f) break;
            outRooms[n] = order[i];
            outWeights[n] = w / totalW;
            ++n;
        }
        return n;
    }

private:
    // ── 設定 ──
    float cell_ = 0.25f;
    int   minVoxels_ = 16;
    int   brick_ = 16;
    float seedRadius_ = 0.6f;
    bool  includeOutside_ = false;             // 外の世界との口を開口にするか
    std::vector<std::uint8_t> outside_;        // ボクセルが「外の世界」か（-1 に均す前の印）
    bool  chamferFull_ = true;
    std::size_t maxVoxels_ = 4000000;

    int chamferCount_() const { return chamferFull_ ? 13 : 3; }
    int chamferUnit_()  const { return chamferFull_ ? 3 : 1; }
    const ChamferNb* nbFwd_() const { return chamferFull_ ? chamferForward()  : cityForward();  }
    const ChamferNb* nbBwd_() const { return chamferFull_ ? chamferBackward() : cityBackward(); }

    // ── 汚れ ──
    bool needFull_ = true;
    std::vector<Aabb> dirtyRegions_;

    // ── 状態 ──
    Result res_;
    // ボクセルを塗った箱の番号（0xFFFF=なし）。境界面の吸音率を引くために持つ。
    std::vector<std::uint16_t> boxOf_;
    std::vector<SolidBox> boxes_;                     // 直近に受け取った形状（吸音率つき）
    std::vector<std::uint16_t> dist_;                 // 実体までのチャンファ距離（×3）
    std::vector<std::uint16_t> gdist_;                // 種までのチャンファ距離（塗り戻し用）
    std::vector<std::int16_t>  room_;                 // ボクセル → 部屋番号（-1 なし）
    std::vector<int> roomOfGid_;                      // 通し番号 → 部屋番号
    int seedT_ = 1;                                   // 種の閾値（チャンファ単位）
    std::vector<std::uint16_t> loc_;                  // ボクセル → ブロック内ローカル番号
    int bx_ = 0, by_ = 0, bz_ = 0;                    // ブロック数
    std::vector<std::vector<LocalStat>> brickStat_;   // ブロック → ローカル空間の集計
    std::vector<std::vector<std::uint32_t>> facePairs_;  // (ブロック*3 + 面) → (la<<16|lb)
    std::vector<int> first_;                          // ブロック → 通し番号の先頭（累積）
    mutable std::vector<int> parent_;                 // union-find
    std::vector<int> rootSlot_;                       // 通し番号 → 集計スロット (-1)
    // 集計スロット → 部屋番号。-1 = 小さすぎて捨てた / kOutsideSlot = 外の世界。
    //   外の世界は塗り戻しで**種として競争させる**ので、-1 とは区別が要る。
    static constexpr int kOutsideSlot = -2;
    std::vector<int> roomOfSlot_;

    // ── union-find（経路半減）──
    int find_(int i) const {
        while (parent_[static_cast<std::size_t>(i)] != i) {
            const int p = parent_[static_cast<std::size_t>(i)];
            parent_[static_cast<std::size_t>(i)] = parent_[static_cast<std::size_t>(p)];
            i = parent_[static_cast<std::size_t>(i)];
        }
        return i;
    }
    void unite_(int a, int b) {
        a = find_(a); b = find_(b);
        if (a != b) parent_[static_cast<std::size_t>(a > b ? a : b)] = (a < b ? a : b);
    }

    int brickOf_(int x, int y, int z) const {
        return ((z / brick_) * by_ + (y / brick_)) * bx_ + (x / brick_);
    }

    // 汚れた領域が今の格子に収まっているか。
    bool regionsFitGrid_() const {
        const Grid& g = res_.grid;
        if (g.v.empty()) return false;
        const Vec3 hi(g.origin.x + g.nx * g.cell,
                      g.origin.y + g.ny * g.cell,
                      g.origin.z + g.nz * g.cell);
        for (const Aabb& a : dirtyRegions_) {
            if (a.min.x < g.origin.x || a.min.y < g.origin.y || a.min.z < g.origin.z) return false;
            if (a.max.x > hi.x || a.max.y > hi.y || a.max.z > hi.z) return false;
        }
        return true;
    }

    // ── 実体を塗る ──
    //   ボクセル中心が OBB の中なら詰まっている扱い。
    //   ★膨らませる量は**ボクセルの 1/4**にしてある。
    //     半ボクセル膨らませると薄い壁は確実に塗れるが、**狭い戸口も塞いでしまう**
    //     （実測: 0.25m 刻みで幅 0.3m の戸口が両側から 0.125m ずつ食われて
    //       残り 0.05m となり、繋がっているはずの 2 部屋が分断された）。
    //     薄い壁の抜けは膨張ではなく**格子を細かくして**防ぐ方が筋が良い。
    //     目安: cell は「いちばん薄い壁の厚み」と「いちばん狭い戸口の幅」の
    //     どちらよりも小さくすること。
    //   x/y/z の範囲を絞って塗る（clip が非 null ならそこと交差した範囲だけ）。
    void rasterize_(const Obb& b, const int* clip, int boxIndex) {
        Grid& g = res_.grid;
        const float grow = g.cell * 0.25f;
        Obb fat = b;
        fat.halfExtents = fat.halfExtents + Vec3(grow, grow, grow);
        const Aabb bb = obbBounds(fat);
        auto lohi = [&](float a, float b2, float o, int n, int& i0, int& i1) {
            i0 = std::max(0, static_cast<int>(std::floor((a - o) / g.cell)));
            i1 = std::min(n - 1, static_cast<int>(std::ceil((b2 - o) / g.cell)));
        };
        int x0, x1, y0, y1, z0, z1;
        lohi(bb.min.x, bb.max.x, g.origin.x, g.nx, x0, x1);
        lohi(bb.min.y, bb.max.y, g.origin.y, g.ny, y0, y1);
        lohi(bb.min.z, bb.max.z, g.origin.z, g.nz, z0, z1);
        if (clip) {
            x0 = std::max(x0, clip[0]); x1 = std::min(x1, clip[1]);
            y0 = std::max(y0, clip[2]); y1 = std::min(y1, clip[3]);
            z0 = std::max(z0, clip[4]); z1 = std::min(z1, clip[5]);
        }
        const std::uint16_t bi = static_cast<std::uint16_t>(
            (boxIndex >= 0 && boxIndex < 0xFFFF) ? boxIndex : 0xFFFF);
        for (int z = z0; z <= z1; ++z)
            for (int y = y0; y <= y1; ++y)
                for (int x = x0; x <= x1; ++x)
                    if (pointInObb(g.center(x, y, z), fat)) {
                        const std::size_t i = static_cast<std::size_t>(g.index(x, y, z));
                        g.v[i] = kSolid;
                        boxOf_[i] = bi;
                    }
    }

    // ── 実体までの距離を測る（3-4-5 チャンファ、前進＋後退の 2 走査）──
    //   「戸口かどうか」は幅で決まるので、まず各ボクセルが壁からどれだけ離れているかが要る。
    //   ★2 走査で済むのがこの方式の要点。侵食を 1 段ずつ繰り返すと半径ぶんの回数だけ
    //     格子を舐めることになるが、チャンファなら半径によらず 2 回で確定する。
    //   ★距離は整数（面3/辺4/角5）のまま持つ。3 で割ればボクセル単位。斜め方向の
    //     誤差が 4/3 ≒ 1.33 でなく 5/(3√3) ≒ 0.96 に収まるので、部屋の角で
    //     余計に千切れることがない。
    //   clip が非 null ならその範囲だけ（差分更新用。距離は半径ぶんしか伝わらないので、
    //   汚れた領域＋半径ぶんの余白を渡せば局所的に直せる）。
    void computeDistance_(const int* clip) {
        const Grid& g = res_.grid;
        const int nx = g.nx, ny = g.ny, nz = g.nz;
        const int sy = nx, sz = nx * ny;
        const int X0 = clip ? clip[0] : 0, X1 = clip ? clip[1] : nx - 1;
        const int Y0 = clip ? clip[2] : 0, Y1 = clip ? clip[3] : ny - 1;
        const int Z0 = clip ? clip[4] : 0, Z1 = clip ? clip[5] : nz - 1;
        std::uint16_t* D = dist_.data();
        const std::uint8_t* V = g.v.data();

        for (int z = Z0; z <= Z1; ++z)
            for (int y = Y0; y <= Y1; ++y) {
                const int base = z * sz + y * sy;
                for (int x = X0; x <= X1; ++x)
                    D[base + x] = (V[base + x] == kSolid) ? 0 : kFar;
            }

        const int kn = chamferCount_();
        auto sweep = [&](const ChamferNb* nb, bool forward) {
            for (int zi = Z0; zi <= Z1; ++zi) {
                const int z = forward ? zi : (Z1 - (zi - Z0));
                for (int yi = Y0; yi <= Y1; ++yi) {
                    const int y = forward ? yi : (Y1 - (yi - Y0));
                    const int base = z * sz + y * sy;
                    for (int xi = X0; xi <= X1; ++xi) {
                        const int x = forward ? xi : (X1 - (xi - X0));
                        const int i = base + x;
                        if (D[i] == 0) continue;
                        int m = D[i];
                        for (int k = 0; k < kn; ++k) {
                            const int ax = x + nb[k].dx, ay = y + nb[k].dy, az = z + nb[k].dz;
                            if (ax < 0 || ay < 0 || az < 0 || ax >= nx || ay >= ny || az >= nz)
                                continue;
                            const int c = static_cast<int>(D[az * sz + ay * sy + ax]) + nb[k].w;
                            if (c < m) m = c;
                        }
                        D[i] = static_cast<std::uint16_t>(m);
                    }
                }
            }
        };
        sweep(nbFwd_(), true);
        sweep(nbBwd_(), false);
    }

    // ── 種から自由空間へ塗り戻す（同じく 2 走査）──
    //   侵食で落とした殻（壁際と戸口）に、最寄りの部屋を配る。距離と部屋番号を
    //   一緒に伝播させるチャンファ・ボロノイ。実体は伝播を遮るので、壁の向こうへは漏れない。
    //   これで戸口に立っているリスナーにも必ずどちらかの部屋が付く。
    void growRooms_() {
        const Grid& g = res_.grid;
        const int nx = g.nx, ny = g.ny, nz = g.nz;
        const int sy = nx, sz = nx * ny;
        const std::size_t n = static_cast<std::size_t>(nx) * ny * nz;
        room_.assign(n, -1);
        gdist_.assign(n, kFar);

        // 通し番号 → 部屋番号の表を先に作る（ボクセルごとに union-find を引かない）。
        const int nb = bx_ * by_ * bz_;
        roomOfGid_.assign(static_cast<std::size_t>(first_[static_cast<std::size_t>(nb)]), -1);
        for (int b = 0; b < nb; ++b)
            for (int l = 0, k = static_cast<int>(brickStat_[static_cast<std::size_t>(b)].size());
                 l < k; ++l) {
                const int gid = first_[static_cast<std::size_t>(b)] + l;
                const int slot = rootSlot_[static_cast<std::size_t>(find_(gid))];
                roomOfGid_[static_cast<std::size_t>(gid)] =
                    (slot >= 0) ? roomOfSlot_[static_cast<std::size_t>(slot)] : -1;
            }

        const std::uint16_t* L = loc_.data();
        std::int16_t* R = room_.data();
        std::uint16_t* GD = gdist_.data();
        for (int b = 0; b < nb; ++b) {
            const int bxi = b % bx_, byi = (b / bx_) % by_, bzi = b / (bx_ * by_);
            const int X0 = bxi * brick_, X1 = std::min(nx, X0 + brick_) - 1;
            const int Y0 = byi * brick_, Y1 = std::min(ny, Y0 + brick_) - 1;
            const int Z0 = bzi * brick_, Z1 = std::min(nz, Z0 + brick_) - 1;
            const int f0 = first_[static_cast<std::size_t>(b)];
            for (int z = Z0; z <= Z1; ++z)
                for (int y = Y0; y <= Y1; ++y) {
                    const int base = z * sz + y * sy;
                    for (int x = X0; x <= X1; ++x) {
                        const std::uint16_t l = L[base + x];
                        if (l >= kLocNone) continue;
                        const int r = roomOfGid_[static_cast<std::size_t>(f0 + l)];
                        // r >= 0 は部屋、kOutsideSlot は「外の世界」。**どちらも種にする。**
                        //   外を種にしないと競争相手がいなくなり、屋外が部屋に吸われる。
                        //   -1（小さすぎて捨てた成分）だけは種にしない ── あれは
                        //   「隙間のノイズ」で、近くの部屋に配ってしまってよい。
                        if (r == -1) continue;
                        R[base + x] = static_cast<std::int16_t>(r);
                        GD[base + x] = 0;
                    }
                }
        }

        const std::uint8_t* V = g.v.data();
        const int kn = chamferCount_();
        auto sweep = [&](const ChamferNb* nb2, bool forward) {
            for (int zi = 0; zi < nz; ++zi) {
                const int z = forward ? zi : (nz - 1 - zi);
                for (int yi = 0; yi < ny; ++yi) {
                    const int y = forward ? yi : (ny - 1 - yi);
                    const int base = z * sz + y * sy;
                    for (int xi = 0; xi < nx; ++xi) {
                        const int x = forward ? xi : (nx - 1 - xi);
                        const int i = base + x;
                        if (V[i] == kSolid || GD[i] == 0) continue;   // 実体と種は動かさない
                        int m = GD[i];
                        int best = R[i];
                        for (int k = 0; k < kn; ++k) {
                            const int ax = x + nb2[k].dx, ay = y + nb2[k].dy, az = z + nb2[k].dz;
                            if (ax < 0 || ay < 0 || az < 0 || ax >= nx || ay >= ny || az >= nz)
                                continue;
                            const int j = az * sz + ay * sy + ax;
                            if (V[j] == kSolid) continue;             // 壁は伝播を遮る
                            const int c = static_cast<int>(GD[j]) + nb2[k].w;
                            if (c < m) { m = c; best = R[j]; }
                        }
                        GD[i] = static_cast<std::uint16_t>(m);
                        R[i] = static_cast<std::int16_t>(best);
                    }
                }
            }
        };
        sweep(nbFwd_(), true);
        sweep(nbBwd_(), false);

        // 「外の世界」は競争のためだけに置いた種なので、ここで普通の「部屋なし」に均す。
        //   以降の利用側（開口の抽出・roomAtVoxel・重み）は負値を一律に扱えばよい。
        //   ★ただし「外だった」印は別に残す。外へ開く口を開口として出すとき、
        //     「小さすぎて捨てた部屋」（同じ -1）と区別が要る。
        outside_.assign(n, 0);
        for (std::size_t i = 0; i < n; ++i)
            if (R[i] == kOutsideSlot) { outside_[i] = 1; R[i] = -1; }

        // 体積・重心・境界は塗り戻した後の姿で取り直す（種だけの体積は実際より小さい）。
        const std::size_t nr = res_.rooms.size();
        if (nr == 0) return;
        std::vector<long long> cnt(nr, 0), ax(nr, 0), ay(nr, 0), az(nr, 0);
        std::vector<int> miX(nr, nx), miY(nr, ny), miZ(nr, nz),
                         maX(nr, -1), maY(nr, -1), maZ(nr, -1);
        for (int z = 0; z < nz; ++z)
            for (int y = 0; y < ny; ++y) {
                const int base = z * sz + y * sy;
                for (int x = 0; x < nx; ++x) {
                    const int r = R[base + x];
                    if (r < 0) continue;
                    const std::size_t u = static_cast<std::size_t>(r);
                    ++cnt[u]; ax[u] += x; ay[u] += y; az[u] += z;
                    if (x < miX[u]) miX[u] = x;  if (x > maX[u]) maX[u] = x;
                    if (y < miY[u]) miY[u] = y;  if (y > maY[u]) maY[u] = y;
                    if (z < miZ[u]) miZ[u] = z;  if (z > maZ[u]) maZ[u] = z;
                }
            }
        for (std::size_t u = 0; u < nr; ++u) {
            if (cnt[u] <= 0) continue;
            const float inv = 1.0f / static_cast<float>(cnt[u]);
            Room& rm = res_.rooms[u];
            rm.voxels = static_cast<int>(cnt[u]);
            rm.centroid = Vec3(g.origin.x + (static_cast<float>(ax[u]) * inv + 0.5f) * g.cell,
                               g.origin.y + (static_cast<float>(ay[u]) * inv + 0.5f) * g.cell,
                               g.origin.z + (static_cast<float>(az[u]) * inv + 0.5f) * g.cell);
            rm.boundsMin = g.center(miX[u], miY[u], miZ[u]);
            rm.boundsMax = g.center(maX[u], maY[u], maZ[u]);
        }
    }

    // ── 部屋どうしの境界 ＝ 開口 ──
    //   塗り戻した後の部屋の場を見て、別の部屋が接している face を集める。
    //   ★壁で隔てられている所には出ない。実体は塗り戻しを遮るので、壁の両側の face は
    //     必ず片方が実体になり、この条件を満たさない。「繋がっている所」だけが残る。
    //   ★同じ 2 部屋を繋ぐ口が複数あることは普通にある（大部屋の 2 つの入口など）。
    //     部屋の組で 1 個にまとめず、face の繋がりで分ける。
    void findApertures_() {
        res_.apertures.clear();
        const Grid& g = res_.grid;
        const int nx = g.nx, ny = g.ny, nz = g.nz;
        const int sy = nx, sz = nx * ny;
        const std::int16_t* R = room_.data();
        const std::uint8_t* V = g.v.data();

        // 境界 face を集める。face は「手前側のボクセル + 軸」で表す。
        // 同じ走査で、部屋ごとの境界面積と吸音（Sabine の A）も貯める。
        //   ★開口（別の部屋へ抜けている面）は吸音率 1 で数える。そこから出た音はこの部屋へ
        //     戻らないので、音響的には穴＝完全吸音。これで部屋どうしの結合が「開口の面積」
        //     として自動的に効く（結合のために別の仕組みを足さなくてよい）。
        faceIdx_.clear(); faceAxis_.clear();
        const int step[3] = { 1, sy, sz };
        const float cellA = g.cell * g.cell;
        const std::size_t nr = res_.rooms.size();
        std::vector<double> area(nr, 0.0), openA(nr, 0.0);
        std::vector<double> absA(nr * kNumBands, 0.0);
        for (int z = 0; z < nz; ++z)
            for (int y = 0; y < ny; ++y) {
                const int base = z * sz + y * sy;
                for (int x = 0; x < nx; ++x) {
                    const int i = base + x;
                    if (V[i] == kSolid) continue;
                    const int ra = R[i];
                    if (ra < 0) continue;
                    const std::size_t u = static_cast<std::size_t>(ra);
                    // 6 近傍を見て境界面を数える（実体との境目＝壁、別部屋との境目＝開口）。
                    const int nb6[6] = { i - 1, i + 1, i - sy, i + sy, i - sz, i + sz };
                    const bool ok6[6] = { x > 0, x < nx - 1, y > 0, y < ny - 1, z > 0, z < nz - 1 };
                    for (int d = 0; d < 6; ++d) {
                        if (!ok6[d]) continue;
                        const int j = nb6[d];
                        if (V[j] == kSolid) {
                            area[u] += cellA;
                            const std::uint16_t bi = boxOf_[static_cast<std::size_t>(j)];
                            const float* ab = (bi < boxes_.size())
                                ? boxes_[bi].absorption : nullptr;
                            for (int b = 0; b < kNumBands; ++b)
                                absA[u * kNumBands + static_cast<std::size_t>(b)] +=
                                    cellA * (ab ? ab[b] : 0.1f);
                        } else if ((R[j] >= 0 && R[j] != ra)
                                   || (includeOutside_ && R[j] < 0
                                       && outside_[static_cast<std::size_t>(j)])) {
                            // 別部屋との境目、または（ON のとき）外の世界との境目＝開口。
                            area[u] += cellA;
                            openA[u] += cellA;
                            for (int b = 0; b < kNumBands; ++b)
                                absA[u * kNumBands + static_cast<std::size_t>(b)] += cellA;
                        }
                    }
                    // 開口の抽出は +X/+Y/+Z だけ見れば重複しない。
                    const int lim[3] = { nx - 1, ny - 1, nz - 1 };
                    const int pos[3] = { x, y, z };
                    for (int a = 0; a < 3; ++a) {
                        if (pos[a] >= lim[a]) continue;
                        const int j = i + step[a];
                        if (V[j] == kSolid) continue;
                        const int rb = R[j];
                        if (rb == ra) continue;
                        // 外の世界との face は includeOutside_ のときだけ口にする。
                        if (rb < 0 && !(includeOutside_ && outside_[static_cast<std::size_t>(j)]))
                            continue;
                        faceIdx_.push_back(i);
                        faceAxis_.push_back(static_cast<std::uint8_t>(a));
                    }
                    // ★外の世界が**負の側**にある face。部屋どうしなら小さい側の部屋のボクセルが
                    //   +方向を見れば必ず拾えるが、外の世界のボクセルはこのループに入らない
                    //   （ra < 0 で飛ばす）ので、部屋側から −方向も見る。face の記録は
                    //   「手前側＝外のボクセル j、軸 a」（R[j] < 0, R[j+step] = ra）。
                    if (includeOutside_) {
                        for (int a = 0; a < 3; ++a) {
                            if (pos[a] <= 0) continue;
                            const int j = i - step[a];
                            if (V[j] == kSolid) continue;
                            if (R[j] >= 0 || !outside_[static_cast<std::size_t>(j)]) continue;
                            faceIdx_.push_back(j);
                            faceAxis_.push_back(static_cast<std::uint8_t>(a));
                        }
                    }
                }
            }
        // Sabine（空気吸収込み）: RT60 = 0.161 V / (A + 4mV)。
        //   A は境界の吸音面積(m2 sabins)、4mV は空気そのものが吸うぶん。
        //   ★4mV を落としてはいけない。広い部屋・長い残響・高域ほど効き、実測では
        //     4kHz で RT60 が半分近くになる。これが無いと高域だけ不自然に長く伸びる。
        //     m の値はホスト側の空気吸収 dB/m と同じ表から換算（1 dB/m = 0.1151 Np/m）。
        static const double kAirDbPerM[kNumBands] =
            {0.0003, 0.0008, 0.0017, 0.003, 0.0085, 0.025};
        for (std::size_t u = 0; u < nr; ++u) {
            Room& rm = res_.rooms[u];
            rm.surface = static_cast<float>(area[u]);
            rm.openArea = static_cast<float>(openA[u]);
            const double vol = static_cast<double>(rm.voxels) * g.cell * g.cell * g.cell;
            for (int b = 0; b < kNumBands; ++b) {
                const double A = absA[u * kNumBands + static_cast<std::size_t>(b)];
                const double mAir = kAirDbPerM[b] * 0.1151;      // dB/m → ネーパ/m
                const double denom = A + 4.0 * mAir * vol;
                rm.absorb[b] = (area[u] > 1e-9) ? static_cast<float>(A / area[u]) : 0.0f;
                rm.rt60[b] = (denom > 1e-9) ? static_cast<float>(0.161 * vol / denom) : 0.0f;
            }
        }
        if (faceIdx_.empty()) return;

        // face を繋がりで分ける。手前側のボクセルが 6 近傍で繋がっていて、
        // かつ繋いでいる部屋の組が同じなら同じ口。
        const int nf = static_cast<int>(faceIdx_.size());
        faceOf_.assign(static_cast<std::size_t>(nx) * ny * nz, -1);
        for (int f = 0; f < nf; ++f) faceOf_[static_cast<std::size_t>(faceIdx_[f])] = f;

        std::vector<std::uint8_t> seen(static_cast<std::size_t>(nf), 0);
        std::vector<int> stack;
        const float cellArea = g.cell * g.cell;
        for (int f0 = 0; f0 < nf; ++f0) {
            if (seen[static_cast<std::size_t>(f0)]) continue;
            const int i0 = faceIdx_[f0];
            const int a0 = faceAxis_[f0];
            int pa = R[i0], pb = R[i0 + step[a0]];
            if (pa > pb) std::swap(pa, pb);

            stack.clear(); stack.push_back(f0);
            seen[static_cast<std::size_t>(f0)] = 1;
            double area = 0.0, cx = 0.0, cy = 0.0, cz = 0.0;
            double nvx = 0.0, nvy = 0.0, nvz = 0.0;
            // 外接箱（格子座標）。ポータルの矩形をここから作る。
            float bmin[3] = { 1e30f, 1e30f, 1e30f }, bmax[3] = { -1e30f, -1e30f, -1e30f };
            while (!stack.empty()) {
                const int f = stack.back(); stack.pop_back();
                const int i = faceIdx_[f];
                const int a = faceAxis_[f];
                const int x = i % nx, y = (i / nx) % ny, z = i / sz;
                area += cellArea;
                // face の中心は 2 ボクセルの中点。
                const float ax = (a == 0) ? 0.5f : 0.0f;
                const float ay = (a == 1) ? 0.5f : 0.0f;
                const float az = (a == 2) ? 0.5f : 0.0f;
                cx += x + 0.5 + ax; cy += y + 0.5 + ay; cz += z + 0.5 + az;
                const float fp[3] = { x + 0.5f + ax, y + 0.5f + ay, z + 0.5f + az };
                for (int k = 0; k < 3; ++k) {
                    bmin[k] = std::min(bmin[k], fp[k]);
                    bmax[k] = std::max(bmax[k], fp[k]);
                }
                // 法線は A→B 向き。手前が A ならその軸の正、逆なら負。
                const float s = (R[i] == pa) ? 1.0f : -1.0f;
                if (a == 0) nvx += s; else if (a == 1) nvy += s; else nvz += s;
                // 隣の face を辿る（手前側ボクセルの 6 近傍）。
                for (int d = 0; d < 6; ++d) {
                    const int ax2 = x + ((d == 0) ? 1 : (d == 1) ? -1 : 0);
                    const int ay2 = y + ((d == 2) ? 1 : (d == 3) ? -1 : 0);
                    const int az2 = z + ((d == 4) ? 1 : (d == 5) ? -1 : 0);
                    if (ax2 < 0 || ay2 < 0 || az2 < 0 || ax2 >= nx || ay2 >= ny || az2 >= nz)
                        continue;
                    const int ni = az2 * sz + ay2 * sy + ax2;
                    const int nf2 = faceOf_[static_cast<std::size_t>(ni)];
                    if (nf2 < 0 || seen[static_cast<std::size_t>(nf2)]) continue;
                    int qa = R[faceIdx_[nf2]], qb = R[faceIdx_[nf2] + step[faceAxis_[nf2]]];
                    if (qa > qb) std::swap(qa, qb);
                    if (qa != pa || qb != pb) continue;
                    seen[static_cast<std::size_t>(nf2)] = 1;
                    stack.push_back(nf2);
                }
            }
            const double cnt = area / cellArea;
            Aperture ap;
            ap.roomA = pa; ap.roomB = pb;
            ap.toOutside = (pa < 0);          // 小さい側が -1 なら外の世界との口
            ap.area = static_cast<float>(area);
            ap.center = Vec3(g.origin.x + static_cast<float>(cx / cnt) * g.cell,
                             g.origin.y + static_cast<float>(cy / cnt) * g.cell,
                             g.origin.z + static_cast<float>(cz / cnt) * g.cell);
            const Vec3 nv(static_cast<float>(nvx), static_cast<float>(nvy),
                          static_cast<float>(nvz));
            const float nl = length(nv);
            ap.normal = (nl > 1e-6f) ? nv * (1.0f / nl) : Vec3(0, 1, 0);

            // ── 外接矩形 ──
            //   法線がいちばん強い軸を「面の向き」とみなし、残り 2 軸で矩形を張る。
            //   半幅にセル半分ずつ足すのは、bmin/bmax が face の**中心**だから
            //   （ボクセル 1 枚ぶんの口だと箱の厚みが 0 になってしまう）。
            const float an[3] = { std::fabs(ap.normal.x), std::fabs(ap.normal.y),
                                  std::fabs(ap.normal.z) };
            int domA = 0;
            if (an[1] > an[domA]) domA = 1;
            if (an[2] > an[domA]) domA = 2;
            const int uA = (domA + 1) % 3, vA = (domA + 2) % 3;
            auto axisVec = [](int a) {
                return (a == 0) ? Vec3(1, 0, 0) : (a == 1) ? Vec3(0, 1, 0) : Vec3(0, 0, 1);
            };
            auto atIdx = [](const Vec3& v, int a) { return (a == 0) ? v.x : (a == 1) ? v.y : v.z; };
            ap.axisU = axisVec(uA);
            ap.axisV = axisVec(vA);
            ap.halfU = (bmax[uA] - bmin[uA]) * 0.5f * g.cell + g.cell * 0.5f;
            ap.halfV = (bmax[vA] - bmin[vA]) * 0.5f * g.cell + g.cell * 0.5f;
            float rc[3];
            for (int k = 0; k < 3; ++k)
                rc[k] = atIdx(g.origin, k) + (bmin[k] + bmax[k]) * 0.5f * g.cell;
            // 面に垂直な向きだけは重心を使う（矩形の「厚み」方向には広がりが無いので、
            // 箱の中心だと斜めの口でずれる）。
            rc[domA] = atIdx(ap.center, domA);
            ap.rectCenter = Vec3(rc[0], rc[1], rc[2]);

            // ── 縁を sub-voxel で詰める ──
            //
            //   voxel 化は「少しでも実体に重なったセル」を実体にするので、自由ボクセルの
            //   範囲は実際の開口より最大 1 セル内側に出る。実測: 幅 0.900m の戸口が 0.750m。
            //   ★その 17% は音量ではなく**扉の開き具合カーブの形**を変える。
            //     正規化カーブが重ならない（ずれ 0.066 @ 20°）＝形が違う。
            //     しかも狂うのは小角側 ── 開口率が 10° で 0.0207→0.0089（-7.3dB）。
            //     「扉が少し開いた瞬間」がいちばん削れるので、コンセプトの中心が痩せる。
            //   ★さらに誤差の量が戸口の格子上の位置で変わる。同じ扉を 5cm 動かすと
            //     矩形が 1 セルぶん変わる ── 物理的に何も変わっていないのに音が変わるので、
            //     「実行時に形状が変わる前提」という看板と食い違う。
            //   → 外側 1 セルぶんだけ、実体との境目を二分探索で詰める。
            refineRect_(ap, uA, vA, g.cell);
            res_.apertures.push_back(ap);
        }
        // 大きい順。残響の結合では効く口から順に見たい。
        std::sort(res_.apertures.begin(), res_.apertures.end(),
                  [](const Aperture& a, const Aperture& b) { return a.area > b.area; });
        // 次回のために掃除（格子ぶんの配列なので持ち越さない）。
        for (int f = 0; f < nf; ++f) faceOf_[static_cast<std::size_t>(faceIdx_[f])] = -1;
    }

    /// 点が実体の中か。縁を詰めるときだけ使う（格子ではなく**元の箱**に問う）。
    bool solidAt_(const Vec3& p) const {
        for (const SolidBox& sb : boxes_)
            if (pointInObb(p, sb.obb)) return true;
        return false;
    }

    // 開口の矩形の 4 辺を、外側 1 セルぶんだけ実体との境目まで広げる。
    //   前提: rectCenter は自由（開口の中）。そこから ±axisU / ±axisV へ探る。
    //   ★広げるだけで縮めない。自由ボクセルの範囲は必ず真の開口の内側なので、
    //     縮める方向の誤りは起きない。逆に外へ出しすぎると壁を開口に数えてしまうので、
    //     1 セルで頭打ちにする（voxel 化の誤差はそれ以上にはならない）。
    void refineRect_(Aperture& ap, int uA, int vA, float cell) {
        (void)uA; (void)vA;
        if (boxes_.empty() || cell <= 1e-4f) return;
        // 中心が自由でなければ（斜めの口・L 字など）触らない。前提が崩れているので。
        if (solidAt_(ap.rectCenter)) return;

        auto edgeOut = [&](const Vec3& dir, float from) {
            // from（自由）から外へ cell まで。境目までの追加ぶんを返す。
            const Vec3 base = ap.rectCenter + dir * from;
            if (!solidAt_(base)) {
                // 現在の縁がまだ自由。境目は外にある。
                if (!solidAt_(base + dir * cell)) return cell;      // 1 セル先も自由＝上限まで
                float lo = 0.0f, hi = cell;
                for (int it = 0; it < 12; ++it) {
                    const float mid = 0.5f * (lo + hi);
                    if (solidAt_(base + dir * mid)) hi = mid; else lo = mid;
                }
                return lo;
            }
            return 0.0f;   // 現在の縁が既に実体の中＝広げない
        };

        const float addU0 = edgeOut(ap.axisU * -1.0f, ap.halfU);
        const float addU1 = edgeOut(ap.axisU,          ap.halfU);
        const float addV0 = edgeOut(ap.axisV * -1.0f, ap.halfV);
        const float addV1 = edgeOut(ap.axisV,          ap.halfV);

        // 両側で違うぶんだけ中心もずれる。ここが左右差の残りを消す所でもある。
        ap.rectCenter = ap.rectCenter
                      + ap.axisU * ((addU1 - addU0) * 0.5f)
                      + ap.axisV * ((addV1 - addV0) * 0.5f);
        ap.halfU += (addU0 + addU1) * 0.5f;
        ap.halfV += (addV0 + addV1) * 0.5f;
    }

    // ── ブロック 1 個をラベリングする ──
    //   ★走査線(scanline)で塗る。1 ボクセルずつ積む素直な塗りつぶしは、取り出すたびに
    //     近傍 6 個を**バラバラの番地**で読み、そのぶんスタックにも積む。1 個 14.8ns
    //     掛かっていて、除算を消しても 11.4ns にしかならなかった＝演算ではなく
    //     メモリ律速だった。X 方向に連続する区間をひとまとめの単位にすると、
    //       ・X 方向の探索と塗りが連続アクセス（memset）になる
    //       ・スタックに積む数が区間長ぶん（実測で 20〜60 分の 1）減る
    //       ・重心と境界も区間ごとに O(1) で足せる（Σx = (xl+xr)*n/2）
    //     6近傍なので、隣の行は [xl,xr] の範囲だけ見ればよい（8近傍のような
    //     斜め漏れの補正が要らない）。
    void labelBrick_(int b) {
        const Grid& g = res_.grid;
        const int nx = g.nx, ny = g.ny;
        const int sy = nx, sz = nx * ny;
        const int bxi = b % bx_, byi = (b / bx_) % by_, bzi = b / (bx_ * by_);
        const int X0 = bxi * brick_, X1 = std::min(g.nx, X0 + brick_) - 1;
        const int Y0 = byi * brick_, Y1 = std::min(g.ny, Y0 + brick_) - 1;
        const int Z0 = bzi * brick_, Z1 = std::min(g.nz, Z0 + brick_) - 1;

        std::uint16_t* L = loc_.data();
        const std::uint16_t* D = dist_.data();
        // 下地：種（壁から seedRadius 以上離れた空き）だけを kLocNone にする。
        //   実体の距離は 0 なので、この 1 つの判定で実体も落ちる。
        for (int z = Z0; z <= Z1; ++z)
            for (int y = Y0; y <= Y1; ++y) {
                const int base = z * sz + y * sy;
                for (int x = X0; x <= X1; ++x)
                    L[base + x] = (D[base + x] >= seedT_) ? kLocNone : kLocSolid;
            }

        std::vector<LocalStat>& st = brickStat_[static_cast<std::size_t>(b)];
        st.clear();

        // 行 (y,z) の x∈[x0,x1] を走り、未訪問の区間の先頭を種として積む。
        auto scanLine = [&](int x0, int x1, int y, int z) {
            const int base = z * sz + y * sy;
            int x = x0;
            while (x <= x1) {
                while (x <= x1 && L[base + x] != kLocNone) ++x;
                if (x > x1) break;
                seeds_.push_back(x); seedY_.push_back(y); seedZ_.push_back(z);
                while (x <= x1 && L[base + x] == kLocNone) ++x;
            }
        };

        for (int z = Z0; z <= Z1; ++z)
        for (int y = Y0; y <= Y1; ++y) {
            const int rowBase = z * sz + y * sy;
            for (int x = X0; x <= X1; ++x) {
                if (L[rowBase + x] != kLocNone) continue;
                const std::uint16_t label = static_cast<std::uint16_t>(st.size());
                LocalStat cur;
                cur.miX = cur.maX = x; cur.miY = cur.maY = y; cur.miZ = cur.maZ = z;
                seeds_.clear(); seedY_.clear(); seedZ_.clear();
                seeds_.push_back(x); seedY_.push_back(y); seedZ_.push_back(z);
                while (!seeds_.empty()) {
                    const int sX = seeds_.back(), sY = seedY_.back(), sZ = seedZ_.back();
                    seeds_.pop_back(); seedY_.pop_back(); seedZ_.pop_back();
                    const int base = sZ * sz + sY * sy;
                    if (L[base + sX] != kLocNone) continue;   // 先に別の区間に取られていた
                    int xl = sX; while (xl > X0 && L[base + xl - 1] == kLocNone) --xl;
                    int xr = sX; while (xr < X1 && L[base + xr + 1] == kLocNone) ++xr;
                    const int n = xr - xl + 1;
                    for (int i = xl; i <= xr; ++i) L[base + i] = label;
                    cur.count += n;
                    cur.sx += (static_cast<long long>(xl) + xr) * n / 2;  // (xl+xr)*n は必ず偶数
                    cur.sy += static_cast<long long>(sY) * n;
                    cur.sz += static_cast<long long>(sZ) * n;
                    if (xl < cur.miX) cur.miX = xl;   if (xr > cur.maX) cur.maX = xr;
                    if (sY < cur.miY) cur.miY = sY;   if (sY > cur.maY) cur.maY = sY;
                    if (sZ < cur.miZ) cur.miZ = sZ;   if (sZ > cur.maZ) cur.maZ = sZ;
                    if (xl == 0 || xr == g.nx - 1 || sY == 0 || sY == g.ny - 1
                        || sZ == 0 || sZ == g.nz - 1) cur.touchesBoundary = true;
                    if (sY > Y0) scanLine(xl, xr, sY - 1, sZ);
                    if (sY < Y1) scanLine(xl, xr, sY + 1, sZ);
                    if (sZ > Z0) scanLine(xl, xr, sY, sZ - 1);
                    if (sZ < Z1) scanLine(xl, xr, sY, sZ + 1);
                }
                st.push_back(cur);
            }
        }
    }

    // ── ブロック b の +X/+Y/+Z 面の対応表を作る ──
    //   面の両側がどちらも空きなら「同じ空間」。同じ組み合わせが何度も出るので畳む。
    void buildFace_(int b, int f) {
        std::vector<std::uint32_t>& out = facePairs_[static_cast<std::size_t>(b) * 3 + f];
        out.clear();
        const Grid& g = res_.grid;
        const int bxi = b % bx_, byi = (b / bx_) % by_, bzi = b / (bx_ * by_);
        if (f == 0 && bxi + 1 >= bx_) return;
        if (f == 1 && byi + 1 >= by_) return;
        if (f == 2 && bzi + 1 >= bz_) return;
        const int X0 = bxi * brick_, X1 = std::min(g.nx, X0 + brick_) - 1;
        const int Y0 = byi * brick_, Y1 = std::min(g.ny, Y0 + brick_) - 1;
        const int Z0 = bzi * brick_, Z1 = std::min(g.nz, Z0 + brick_) - 1;
        const int sy = g.nx, sz = g.nx * g.ny;
        const std::uint16_t* L = loc_.data();
        const int step = (f == 0) ? 1 : (f == 1) ? sy : sz;

        auto add = [&](int ia) {
            const std::uint16_t la = L[ia], lb = L[ia + step];
            if (la >= kLocNone || lb >= kLocNone) return;
            out.push_back((static_cast<std::uint32_t>(la) << 16) | lb);
        };
        if (f == 0) {
            for (int z = Z0; z <= Z1; ++z)
                for (int y = Y0; y <= Y1; ++y) add(z * sz + y * sy + X1);
        } else if (f == 1) {
            for (int z = Z0; z <= Z1; ++z)
                for (int x = X0; x <= X1; ++x) add(z * sz + Y1 * sy + x);
        } else {
            for (int y = Y0; y <= Y1; ++y)
                for (int x = X0; x <= X1; ++x) add(Z1 * sz + y * sy + x);
        }
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
    }

    // ── 対応表を束ねて部屋にする ──
    //   ここはブロックの中身ではなくローカル番号の総数に比例する（＝数千）。
    //   ブロックが 1 個汚れただけでも毎回まるごとやり直してよい安さ。
    void merge_() {
        const int nb = bx_ * by_ * bz_;
        first_.assign(static_cast<std::size_t>(nb) + 1, 0);
        for (int b = 0; b < nb; ++b)
            first_[static_cast<std::size_t>(b) + 1] =
                first_[static_cast<std::size_t>(b)]
                + static_cast<int>(brickStat_[static_cast<std::size_t>(b)].size());
        const int total = first_[static_cast<std::size_t>(nb)];

        parent_.resize(static_cast<std::size_t>(total));
        for (int i = 0; i < total; ++i) parent_[static_cast<std::size_t>(i)] = i;

        for (int b = 0; b < nb; ++b) {
            const int bxi = b % bx_, byi = (b / bx_) % by_, bzi = b / (bx_ * by_);
            const int nbr[3] = {
                (bxi + 1 < bx_) ? b + 1             : -1,
                (byi + 1 < by_) ? b + bx_           : -1,
                (bzi + 1 < bz_) ? b + bx_ * by_     : -1,
            };
            for (int f = 0; f < 3; ++f) {
                if (nbr[f] < 0) continue;
                for (std::uint32_t p : facePairs_[static_cast<std::size_t>(b) * 3 + f])
                    unite_(first_[static_cast<std::size_t>(b)] + static_cast<int>(p >> 16),
                           first_[static_cast<std::size_t>(nbr[f])] + static_cast<int>(p & 0xFFFF));
            }
        }

        // 代表ごとに集計をまとめる。
        rootSlot_.assign(static_cast<std::size_t>(total), -1);
        std::vector<LocalStat> agg;
        for (int b = 0; b < nb; ++b) {
            const std::vector<LocalStat>& st = brickStat_[static_cast<std::size_t>(b)];
            for (int l = 0; l < static_cast<int>(st.size()); ++l) {
                const int r = find_(first_[static_cast<std::size_t>(b)] + l);
                int slot = rootSlot_[static_cast<std::size_t>(r)];
                if (slot < 0) {
                    slot = static_cast<int>(agg.size());
                    rootSlot_[static_cast<std::size_t>(r)] = slot;
                    agg.push_back(st[static_cast<std::size_t>(l)]);
                    continue;
                }
                LocalStat& a = agg[static_cast<std::size_t>(slot)];
                const LocalStat& s = st[static_cast<std::size_t>(l)];
                a.count += s.count;
                a.sx += s.sx; a.sy += s.sy; a.sz += s.sz;
                a.miX = std::min(a.miX, s.miX); a.maX = std::max(a.maX, s.maX);
                a.miY = std::min(a.miY, s.miY); a.maY = std::max(a.maY, s.maY);
                a.miZ = std::min(a.miZ, s.miZ); a.maZ = std::max(a.maZ, s.maZ);
                a.touchesBoundary = a.touchesBoundary || s.touchesBoundary;
            }
        }
        // 代表の集計は「先に見つけた方のスロット」に足し込むが、経路半減で
        // 代表が後から変わることはない（unite_ 後に find_ しているため）。

        res_.rooms.clear();
        res_.discarded = 0;
        res_.outsideVoxels = 0;
        roomOfSlot_.assign(agg.size(), -1);
        const Grid& g = res_.grid;
        for (std::size_t i = 0; i < agg.size(); ++i) {
            const LocalStat& a = agg[i];
            // ★格子の外周に届いた成分は「外の世界」であって部屋ではない。
            //   外側に 1 ボクセルの余白を取ってあるので、屋外や囲われていない空間は
            //   必ずここへ落ちる。これで「閉じた空間かどうか」が判定できる。
            // ★「外の世界」も**種として残す**（kOutsideSlot）。捨てて種にしないと、
            //   塗り戻しに競争相手がいなくなり、屋外の空気が戸口を通って部屋に吸われる。
            //   実測（地面46x70m＋建物20x18x8m・戸口1.1m）: 建物の中の実効体積が
            //   設計 2880m3 に対して **11467m3（4.0倍）**、戸口の外 19m まで「部屋0」だった。
            //   外を種にすれば、屋外ボクセルは外のほうが近いので吸われず、
            //   境目は戸口の所で距離が釣り合う位置に**連続に**決まる。
            if (a.touchesBoundary) {
                res_.outsideVoxels += a.count;
                roomOfSlot_[i] = kOutsideSlot;
                continue;
            }
            if (a.count < minVoxels_) { res_.discarded++; continue; }
            roomOfSlot_[i] = static_cast<int>(res_.rooms.size());
            const float inv = 1.0f / static_cast<float>(a.count);
            Room rm;
            rm.voxels = a.count;
            rm.centroid = Vec3(g.origin.x + (static_cast<float>(a.sx) * inv + 0.5f) * g.cell,
                               g.origin.y + (static_cast<float>(a.sy) * inv + 0.5f) * g.cell,
                               g.origin.z + (static_cast<float>(a.sz) * inv + 0.5f) * g.cell);
            rm.boundsMin = g.center(a.miX, a.miY, a.miZ);
            rm.boundsMax = g.center(a.maX, a.maY, a.maZ);
            res_.rooms.push_back(rm);
        }
    }

    // ── 全再構築 ──
    void rebuildAll_(const std::vector<SolidBox>& boxes) {
        res_ = Result();
        loc_.clear(); brickStat_.clear(); facePairs_.clear(); boxOf_.clear();
        first_.clear(); parent_.clear(); rootSlot_.clear(); roomOfSlot_.clear();
        bx_ = by_ = bz_ = 0;
        if (boxes.empty() || cell_ <= 1e-3f) return;

        Vec3 lo(1e30f, 1e30f, 1e30f), hi(-1e30f, -1e30f, -1e30f);
        for (const SolidBox& sb : boxes) {
            const Aabb bb = obbBounds(sb.obb);
            lo = Vec3(std::min(lo.x, bb.min.x), std::min(lo.y, bb.min.y), std::min(lo.z, bb.min.z));
            hi = Vec3(std::max(hi.x, bb.max.x), std::max(hi.y, bb.max.y), std::max(hi.z, bb.max.z));
        }
        // ★格子は形状の境界そのものではなく **1 ボクセルぶん外側**まで取る。
        //   部屋の外側に空きの層があると「外の世界」がひとつの連結成分になり、
        //   閉じた部屋と区別できる（外に繋がっている＝部屋ではない、と判定できる）。
        lo = Vec3(lo.x - cell_, lo.y - cell_, lo.z - cell_);
        hi = Vec3(hi.x + cell_, hi.y + cell_, hi.z + cell_);

        Grid& g = res_.grid;
        g.cell = cell_;
        g.origin = lo;
        auto dim = [&](float a, float b) {
            return std::max(1, static_cast<int>(std::ceil((b - a) / g.cell)));
        };
        g.nx = dim(lo.x, hi.x); g.ny = dim(lo.y, hi.y); g.nz = dim(lo.z, hi.z);
        // 総数が上限を超えるなら、収まるまで粗くする。
        //   ★これは**黙って起きる**。格子は登録された全ボックスの AABB 全体を覆うので、
        //     広い地面を 1 枚置くだけで要求したセルが無視される。
        //     0.25m 指定が 0.375m に降格すると、0.9m の戸口を割るのに必要な
        //     「幅 = 半径×2」の余裕が無くなって**部屋が割れなくなる**（§11 の目安）。
        //     ホストが気づけるよう、要求値 cell_ は残してある（cell() で読める）。
        while (static_cast<std::size_t>(g.nx) * g.ny * g.nz > maxVoxels_) {
            g.cell *= 1.5f;
            g.nx = dim(lo.x, hi.x); g.ny = dim(lo.y, hi.y); g.nz = dim(lo.z, hi.z);
        }

        // 種の閾値をチャンファ単位に落とす。0 なら侵食なし（素の連結成分）。
        seedT_ = std::max(1, static_cast<int>(seedRadius_ / g.cell * chamferUnit_() + 0.5f));

        const auto tA0 = std::chrono::high_resolution_clock::now();
        const std::size_t nv = static_cast<std::size_t>(g.nx) * g.ny * g.nz;
        g.v.assign(nv, kEmpty);
        loc_.assign(nv, kLocNone);
        dist_.assign(nv, kFar);
        boxOf_.assign(nv, 0xFFFF);
        bx_ = (g.nx + brick_ - 1) / brick_;
        by_ = (g.ny + brick_ - 1) / brick_;
        bz_ = (g.nz + brick_ - 1) / brick_;
        const int nb = bx_ * by_ * bz_;
        brickStat_.assign(static_cast<std::size_t>(nb), {});
        facePairs_.assign(static_cast<std::size_t>(nb) * 3, {});
        const auto tA1 = std::chrono::high_resolution_clock::now();

        for (std::size_t i = 0; i < boxes.size(); ++i)
            rasterize_(boxes[i].obb, nullptr, static_cast<int>(i));
        const auto tF1 = std::chrono::high_resolution_clock::now();

        computeDistance_(nullptr);
        const auto tD1 = std::chrono::high_resolution_clock::now();

        for (int b = 0; b < nb; ++b) labelBrick_(b);
        for (int b = 0; b < nb; ++b) for (int f = 0; f < 3; ++f) buildFace_(b, f);
        const auto tL1 = std::chrono::high_resolution_clock::now();

        merge_();
        const auto tM1 = std::chrono::high_resolution_clock::now();

        growRooms_();
        const auto tG1 = std::chrono::high_resolution_clock::now();

        findApertures_();
        const auto tP1 = std::chrono::high_resolution_clock::now();
        res_.msAperture = std::chrono::duration<double, std::milli>(tP1 - tG1).count();

        res_.msAlloc = std::chrono::duration<double, std::milli>(tA1 - tA0).count();
        res_.msFill  = std::chrono::duration<double, std::milli>(tF1 - tA1).count();
        res_.msDist  = std::chrono::duration<double, std::milli>(tD1 - tF1).count();
        res_.msLabel = std::chrono::duration<double, std::milli>(tL1 - tD1).count();
        res_.msMerge = std::chrono::duration<double, std::milli>(tM1 - tL1).count();
        res_.msGrow  = std::chrono::duration<double, std::milli>(tG1 - tM1).count();
        res_.dirtyBricks = 0;
        res_.totalBricks = nb;
    }

    // ── 差分更新 ──
    //   汚れた領域に触れるブロックだけ塗り直し、その面（自分の 3 面＋手前隣の 3 面）の
    //   対応表を作り直して、union-find を張り直す。
    void rebuildDirty_(const std::vector<SolidBox>& boxes) {
        Grid& g = res_.grid;
        const int nb = bx_ * by_ * bz_;
        std::vector<std::uint8_t> hot(static_cast<std::size_t>(nb), 0);
        int hotCount = 0;
        // ★侵食の半径ぶん余分に広げる。距離は半径を超えて伝わらないので、
        //   ここまで見れば「種かどうか」の判定は外側で変わらない。
        //   （壁が消えると距離は増えるだけなので、既に種だったボクセルは種のまま。
        //     壁が増えて距離が減るのは新しい壁から半径ぶんの範囲だけ。）
        const int halo = (seedT_ + chamferUnit_() - 1) / chamferUnit_() + 1;
        int cx0 = g.nx, cy0 = g.ny, cz0 = g.nz, cx1 = -1, cy1 = -1, cz1 = -1;
        for (const Aabb& a : dirtyRegions_) {
            const int x0 = std::max(0, static_cast<int>(std::floor((a.min.x - g.origin.x) / g.cell)) - halo);
            const int y0 = std::max(0, static_cast<int>(std::floor((a.min.y - g.origin.y) / g.cell)) - halo);
            const int z0 = std::max(0, static_cast<int>(std::floor((a.min.z - g.origin.z) / g.cell)) - halo);
            const int x1 = std::min(g.nx - 1, static_cast<int>(std::ceil((a.max.x - g.origin.x) / g.cell)) + halo);
            const int y1 = std::min(g.ny - 1, static_cast<int>(std::ceil((a.max.y - g.origin.y) / g.cell)) + halo);
            const int z1 = std::min(g.nz - 1, static_cast<int>(std::ceil((a.max.z - g.origin.z) / g.cell)) + halo);
            if (x1 < x0 || y1 < y0 || z1 < z0) continue;
            for (int bz = z0 / brick_; bz <= z1 / brick_; ++bz)
            for (int by = y0 / brick_; by <= y1 / brick_; ++by)
            for (int bxi = x0 / brick_; bxi <= x1 / brick_; ++bxi) {
                const int b = (bz * by_ + by) * bx_ + bxi;
                if (!hot[static_cast<std::size_t>(b)]) { hot[static_cast<std::size_t>(b)] = 1; ++hotCount; }
            }
        }
        // 汚れたブロックをまとめて囲む範囲（距離の測り直しはこの箱の中だけでよい）。
        for (int b = 0; b < nb; ++b) {
            if (!hot[static_cast<std::size_t>(b)]) continue;
            const int bxi = b % bx_, byi = (b / bx_) % by_, bzi = b / (bx_ * by_);
            cx0 = std::min(cx0, bxi * brick_); cx1 = std::max(cx1, std::min(g.nx, (bxi + 1) * brick_) - 1);
            cy0 = std::min(cy0, byi * brick_); cy1 = std::max(cy1, std::min(g.ny, (byi + 1) * brick_) - 1);
            cz0 = std::min(cz0, bzi * brick_); cz1 = std::max(cz1, std::min(g.nz, (bzi + 1) * brick_) - 1);
        }
        if (cx1 < cx0) { res_.dirtyBricks = 0; res_.totalBricks = nb; return; }

        const auto tF0 = std::chrono::high_resolution_clock::now();
        // 汚れたブロックの中身を白紙に戻してから、そこに掛かる箱だけを塗り直す。
        for (int b = 0; b < nb; ++b) {
            if (!hot[static_cast<std::size_t>(b)]) continue;
            const int bxi = b % bx_, byi = (b / bx_) % by_, bzi = b / (bx_ * by_);
            const int X0 = bxi * brick_, X1 = std::min(g.nx, X0 + brick_) - 1;
            const int Y0 = byi * brick_, Y1 = std::min(g.ny, Y0 + brick_) - 1;
            const int Z0 = bzi * brick_, Z1 = std::min(g.nz, Z0 + brick_) - 1;
            for (int z = Z0; z <= Z1; ++z)
                for (int y = Y0; y <= Y1; ++y)
                {
                    const std::size_t o = static_cast<std::size_t>(z * g.nx * g.ny + y * g.nx + X0);
                    const std::size_t n = static_cast<std::size_t>(X1 - X0 + 1);
                    std::memset(g.v.data() + o, kEmpty, n);
                    for (std::size_t k = 0; k < n; ++k) boxOf_[o + k] = 0xFFFF;
                }
            const Vec3 wlo = Vec3(g.origin.x + X0 * g.cell,
                                  g.origin.y + Y0 * g.cell,
                                  g.origin.z + Z0 * g.cell);
            const Vec3 whi = Vec3(g.origin.x + (X1 + 1) * g.cell,
                                  g.origin.y + (Y1 + 1) * g.cell,
                                  g.origin.z + (Z1 + 1) * g.cell);
            const int clip[6] = { X0, X1, Y0, Y1, Z0, Z1 };
            for (std::size_t bi = 0; bi < boxes.size(); ++bi) {
                const Aabb bb = obbBounds(boxes[bi].obb);
                const float grow = g.cell * 0.25f;
                if (bb.max.x + grow < wlo.x || bb.min.x - grow > whi.x) continue;
                if (bb.max.y + grow < wlo.y || bb.min.y - grow > whi.y) continue;
                if (bb.max.z + grow < wlo.z || bb.min.z - grow > whi.z) continue;
                rasterize_(boxes[bi].obb, clip, static_cast<int>(bi));
            }
        }
        const auto tF1 = std::chrono::high_resolution_clock::now();

        const int clipAll[6] = { cx0, cx1, cy0, cy1, cz0, cz1 };
        computeDistance_(clipAll);
        const auto tD1 = std::chrono::high_resolution_clock::now();

        for (int b = 0; b < nb; ++b) if (hot[static_cast<std::size_t>(b)]) labelBrick_(b);
        // 面は「自分の +面」と「手前隣の +面」の両方が影響を受ける。
        for (int b = 0; b < nb; ++b) {
            if (!hot[static_cast<std::size_t>(b)]) continue;
            const int bxi = b % bx_, byi = (b / bx_) % by_, bzi = b / (bx_ * by_);
            for (int f = 0; f < 3; ++f) buildFace_(b, f);
            if (bxi > 0) buildFace_(b - 1, 0);
            if (byi > 0) buildFace_(b - bx_, 1);
            if (bzi > 0) buildFace_(b - bx_ * by_, 2);
        }
        const auto tL1 = std::chrono::high_resolution_clock::now();

        merge_();
        const auto tM1 = std::chrono::high_resolution_clock::now();

        // ★塗り戻しだけは全体に及ぶ。戸口が開けば「どちらの部屋か」は遠くまで変わりうるし、
        //   部屋の番号自体が振り直されるので、局所では閉じない。2 走査で済むので抱えている。
        growRooms_();
        const auto tG1 = std::chrono::high_resolution_clock::now();

        findApertures_();
        const auto tP1 = std::chrono::high_resolution_clock::now();
        res_.msAperture = std::chrono::duration<double, std::milli>(tP1 - tG1).count();

        res_.msAlloc = 0.0;
        res_.msFill  = std::chrono::duration<double, std::milli>(tF1 - tF0).count();
        res_.msDist  = std::chrono::duration<double, std::milli>(tD1 - tF1).count();
        res_.msLabel = std::chrono::duration<double, std::milli>(tL1 - tD1).count();
        res_.msMerge = std::chrono::duration<double, std::milli>(tM1 - tL1).count();
        res_.msGrow  = std::chrono::duration<double, std::milli>(tG1 - tM1).count();
        res_.dirtyBricks = hotCount;
        res_.totalBricks = nb;
    }

    // 走査線の種（x,y,z を 3 本の配列で持つ。構造体より積み下ろしが軽い）。
    std::vector<int> seeds_, seedY_, seedZ_;
    // 開口を拾うときの作業領域（毎回確保し直さない）。
    std::vector<int> faceIdx_, faceOf_;
    std::vector<std::uint8_t> faceAxis_;
};

}  // namespace rooms
}  // namespace acoustic
