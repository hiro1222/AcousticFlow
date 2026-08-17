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
#include "Core/vec3.h"

namespace acoustic {
namespace rooms {

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
};

// 部屋どうしを繋ぐくびれ（戸口・窓・壊れた壁の穴）。
//   塗り戻した後、別の部屋どうしが face で接している所がそのまま開口になる。
//   壁で隔てられているだけの所には出ない（実体が伝播を遮るので接する face が無い）。
//   ★扉そのものは入っていない。扉は「動くもの」として静的な塗り分けから外れているので、
//     ここに出るのは**戸口**（開口の器）であって、その開き具合ではない。
//     開き具合は既存の回折・透過の経路が連続量として出しているので、そちらと組む。
struct Aperture {
    int   roomA = -1, roomB = -1;   // 繋いでいる部屋（roomA < roomB）
    float area = 0.0f;              // 断面積(m2)
    Vec3  center{0, 0, 0};          // 断面の重心
    Vec3  normal{0, 0, 0};          // 面の向き（A→B が正）。面積で重み付けした平均
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
    float cell() const { return cell_; }

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
    const Result& build(const std::vector<Obb>& boxes) {
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

private:
    // ── 設定 ──
    float cell_ = 0.25f;
    int   minVoxels_ = 16;
    int   brick_ = 16;
    float seedRadius_ = 0.6f;
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
    std::vector<int> roomOfSlot_;                     // 集計スロット → 部屋番号 (-1)

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
    void rasterize_(const Obb& b, const int* clip) {
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
        for (int z = z0; z <= z1; ++z)
            for (int y = y0; y <= y1; ++y)
                for (int x = x0; x <= x1; ++x)
                    if (pointInObb(g.center(x, y, z), fat))
                        g.v[static_cast<std::size_t>(g.index(x, y, z))] = kSolid;
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
                        if (r < 0) continue;                 // 外の世界／小さすぎて捨てた
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
        faceIdx_.clear(); faceAxis_.clear();
        const int step[3] = { 1, sy, sz };
        for (int z = 0; z < nz; ++z)
            for (int y = 0; y < ny; ++y) {
                const int base = z * sz + y * sy;
                for (int x = 0; x < nx; ++x) {
                    const int i = base + x;
                    if (V[i] == kSolid) continue;
                    const int ra = R[i];
                    if (ra < 0) continue;
                    const int lim[3] = { nx - 1, ny - 1, nz - 1 };
                    const int pos[3] = { x, y, z };
                    for (int a = 0; a < 3; ++a) {
                        if (pos[a] >= lim[a]) continue;
                        const int j = i + step[a];
                        if (V[j] == kSolid) continue;
                        const int rb = R[j];
                        if (rb < 0 || rb == ra) continue;
                        faceIdx_.push_back(i);
                        faceAxis_.push_back(static_cast<std::uint8_t>(a));
                    }
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
            ap.area = static_cast<float>(area);
            ap.center = Vec3(g.origin.x + static_cast<float>(cx / cnt) * g.cell,
                             g.origin.y + static_cast<float>(cy / cnt) * g.cell,
                             g.origin.z + static_cast<float>(cz / cnt) * g.cell);
            const Vec3 nv(static_cast<float>(nvx), static_cast<float>(nvy),
                          static_cast<float>(nvz));
            const float nl = length(nv);
            ap.normal = (nl > 1e-6f) ? nv * (1.0f / nl) : Vec3(0, 1, 0);
            res_.apertures.push_back(ap);
        }
        // 大きい順。残響の結合では効く口から順に見たい。
        std::sort(res_.apertures.begin(), res_.apertures.end(),
                  [](const Aperture& a, const Aperture& b) { return a.area > b.area; });
        // 次回のために掃除（格子ぶんの配列なので持ち越さない）。
        for (int f = 0; f < nf; ++f) faceOf_[static_cast<std::size_t>(faceIdx_[f])] = -1;
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
            if (a.touchesBoundary) { res_.outsideVoxels += a.count; continue; }
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
    void rebuildAll_(const std::vector<Obb>& boxes) {
        res_ = Result();
        loc_.clear(); brickStat_.clear(); facePairs_.clear();
        first_.clear(); parent_.clear(); rootSlot_.clear(); roomOfSlot_.clear();
        bx_ = by_ = bz_ = 0;
        if (boxes.empty() || cell_ <= 1e-3f) return;

        Vec3 lo(1e30f, 1e30f, 1e30f), hi(-1e30f, -1e30f, -1e30f);
        for (const Obb& b : boxes) {
            const Aabb bb = obbBounds(b);
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
        bx_ = (g.nx + brick_ - 1) / brick_;
        by_ = (g.ny + brick_ - 1) / brick_;
        bz_ = (g.nz + brick_ - 1) / brick_;
        const int nb = bx_ * by_ * bz_;
        brickStat_.assign(static_cast<std::size_t>(nb), {});
        facePairs_.assign(static_cast<std::size_t>(nb) * 3, {});
        const auto tA1 = std::chrono::high_resolution_clock::now();

        for (const Obb& b : boxes) rasterize_(b, nullptr);
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
    void rebuildDirty_(const std::vector<Obb>& boxes) {
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
                    std::memset(g.v.data() + (z * g.nx * g.ny + y * g.nx + X0), kEmpty,
                                static_cast<std::size_t>(X1 - X0 + 1));
            const Vec3 wlo = Vec3(g.origin.x + X0 * g.cell,
                                  g.origin.y + Y0 * g.cell,
                                  g.origin.z + Z0 * g.cell);
            const Vec3 whi = Vec3(g.origin.x + (X1 + 1) * g.cell,
                                  g.origin.y + (Y1 + 1) * g.cell,
                                  g.origin.z + (Z1 + 1) * g.cell);
            const int clip[6] = { X0, X1, Y0, Y1, Z0, Z1 };
            for (const Obb& box : boxes) {
                const Aabb bb = obbBounds(box);
                const float grow = g.cell * 0.25f;
                if (bb.max.x + grow < wlo.x || bb.min.x - grow > whi.x) continue;
                if (bb.max.y + grow < wlo.y || bb.min.y - grow > whi.y) continue;
                if (bb.max.z + grow < wlo.z || bb.min.z - grow > whi.z) continue;
                rasterize_(box, clip);
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
