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
//     2) 空きボクセルの連結成分 ＝ 部屋
//     3) 部屋どうしを繋ぐくびれ ＝ 開口（次の段階。ここではまだ作らない）
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
// ■ 実装の段階
//   まずは密なグリッドで作る。八分木は同じアルゴリズムの省メモリ版なので、
//   「部屋が正しく出るか」を確かめてから移す。
//
// ■ コスト（実測。12×4×16m の検証シーン）
//                            全再構築    内訳（確保 / 塗り / 連結）
//     0.50m   11,340 ボクセル   0.09 ms   0.00 / 0.03 / 0.05
//     0.25m   74,256 ボクセル   0.42 ms   0.02 / 0.12 / 0.28   ← 既定
//     0.15m  307,692 ボクセル   1.58 ms   0.04 / 0.35 / 1.17
//     0.10m  990,000 ボクセル   4.52 ms   0.21 / 1.35 / 2.94
//   ボクセル数にほぼ比例（約 4.0ns/個）。
//
//   ※ 経緯：最初は 14ns/個で、その 92% が連結成分だった。整数除算を消して 11.4ns、
//     走査線方式にして 4.0ns。「連結が重い」を測ってから手を打った結果で、
//     推測で塗りを速くしていたら 1 割も縮まなかった。
//
//   それでも 40×10×40m を 0.25m で切ると約 400 万個 = **約 16ms** で、
//   全再構築は 1 フレームに収まらない。変わった領域だけ塗り直す差分更新が要る。未対応。
//
// ■ LOD / ストリーミングとの関係（既知の限界）
//   active=false のインスタンスは静的な塗り分けに入らない。つまり LOD で壁を降ろすと
//   **その壁が囲っていた部屋が「外の世界」に繋がって部屋でなくなる**。連結成分なので
//   隣の部屋まで巻き込んで消えることがある。
//   エンジンからは「LOD で降ろされた」のか「壊されて無くなった」のか区別できない。
//   音響的には後者の解釈が一貫している（無い壁は音を止めない）が、
//   運用としては「部屋を囲う壁は LOD で落とさない」という制約が要る。
//   ※ setInstanceActive では再構築フラグを立ててあるので、結果が古くなることはない。

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <vector>

#include "Core/aabb.h"
#include "Core/vec3.h"

namespace acoustic {
namespace rooms {

// ボクセルの状態。
enum : std::uint8_t {
    kSolid     = 0xFF, // 実体が入っている
    kUnlabeled = 0xFE, // 空いているがまだ部屋番号が付いていない
    kOutside   = 0xFD, // 空いているが部屋ではない（外の世界／小さすぎる隙間）
    // 0x00..0xFC は部屋番号
    kMaxRooms  = 0xFD,
};

struct Grid {
    Vec3 origin{0, 0, 0};      // 格子の原点（最小コーナー）
    float cell = 0.25f;        // 一辺(m)
    int nx = 0, ny = 0, nz = 0;
    std::vector<std::uint8_t> v;   // nx*ny*nz

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

struct Result {
    Grid grid;
    std::vector<Room> rooms;   // 添字がそのまま部屋番号
    int  discarded = 0;        // 小さすぎて捨てた連結成分の数
    int  outsideVoxels = 0;    // 「外の世界」に落ちたボクセル数（格子の外周に届いた成分）
    // 段別の所要時間(ms)。どこを削るべきかを推測でなく数字で決めるため。
    double msAlloc = 0.0, msFill = 0.0, msLabel = 0.0;
};

// 点が OBB の中にあるか。
inline bool pointInObb(const Vec3& p, const Obb& b) {
    const Vec3 d = p - b.center;
    return std::fabs(dot(d, b.axisX)) <= b.halfExtents.x
        && std::fabs(dot(d, b.axisY)) <= b.halfExtents.y
        && std::fabs(dot(d, b.axisZ)) <= b.halfExtents.z;
}

// ボクセル化 ＋ 連結成分（部屋）。
//   boxes      : 静的な形状（呼び出し側で「動くもの」を除いてから渡す）
//   cell       : ボクセル一辺(m)。戸口の幅を数ボクセルで割れる大きさにすること
//   minVoxels  : これ未満の連結成分は部屋として扱わない（隙間のノイズを捨てる）
//   maxVoxels  : 格子の総数の上限。超えるなら cell を粗くして収める
//
// ★格子は形状の境界そのものではなく **1 ボクセルぶん外側**まで取る。
//   部屋の外側に空きの層があると「外の世界」がひとつの連結成分になり、
//   閉じた部屋と区別できる（外に繋がっている＝部屋ではない、と判定できる）。
inline Result buildRooms(const std::vector<Obb>& boxes, float cell,
                         int minVoxels = 16, std::size_t maxVoxels = 4000000) {
    Result r;
    if (boxes.empty() || cell <= 1e-3f) return r;

    Vec3 lo(1e30f, 1e30f, 1e30f), hi(-1e30f, -1e30f, -1e30f);
    for (const Obb& b : boxes) {
        // OBB の 8 頂点で境界を取る（回転していても正しく囲む）。
        for (int i = 0; i < 8; ++i) {
            const float sx = (i & 1) ? 1.0f : -1.0f;
            const float sy = (i & 2) ? 1.0f : -1.0f;
            const float sz = (i & 4) ? 1.0f : -1.0f;
            const Vec3 p = b.center + b.axisX * (b.halfExtents.x * sx)
                                    + b.axisY * (b.halfExtents.y * sy)
                                    + b.axisZ * (b.halfExtents.z * sz);
            lo = Vec3(std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z));
            hi = Vec3(std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z));
        }
    }
    // 外側に 1 ボクセルの余白。
    lo = Vec3(lo.x - cell, lo.y - cell, lo.z - cell);
    hi = Vec3(hi.x + cell, hi.y + cell, hi.z + cell);

    Grid& g = r.grid;
    g.cell = cell;
    g.origin = lo;
    auto dim = [&](float a, float b) {
        return std::max(1, static_cast<int>(std::ceil((b - a) / cell)));
    };
    g.nx = dim(lo.x, hi.x); g.ny = dim(lo.y, hi.y); g.nz = dim(lo.z, hi.z);

    // 総数が上限を超えるなら、収まるまで粗くする。
    while (static_cast<std::size_t>(g.nx) * g.ny * g.nz > maxVoxels) {
        g.cell *= 1.5f;
        g.nx = dim(lo.x, hi.x); g.ny = dim(lo.y, hi.y); g.nz = dim(lo.z, hi.z);
    }
    cell = g.cell;

    const auto tA0 = std::chrono::high_resolution_clock::now();
    g.v.assign(static_cast<std::size_t>(g.nx) * g.ny * g.nz, kUnlabeled);
    const auto tA1 = std::chrono::high_resolution_clock::now();

    // ── 実体を塗る ──
    //   ボクセル中心が OBB の中なら詰まっている扱い。
    //   ★膨らませる量は**ボクセルの 1/4**にしてある。
    //     半ボクセル膨らませると薄い壁は確実に塗れるが、**狭い戸口も塞いでしまう**
    //     （実測: 0.25m 刻みで幅 0.3m の戸口が両側から 0.125m ずつ食われて
    //       残り 0.05m となり、繋がっているはずの 2 部屋が分断された）。
    //     薄い壁の抜けは膨張ではなく**格子を細かくして**防ぐ方が筋が良い。
    //     目安: cell は「いちばん薄い壁の厚み」と「いちばん狭い戸口の幅」の
    //     どちらよりも小さくすること。
    const float grow = cell * 0.25f;
    for (const Obb& b : boxes) {
        Obb fat = b;
        fat.halfExtents = fat.halfExtents + Vec3(grow, grow, grow);
        // 走査範囲は膨らませた OBB の AABB。
        Vec3 blo(1e30f, 1e30f, 1e30f), bhi(-1e30f, -1e30f, -1e30f);
        for (int i = 0; i < 8; ++i) {
            const float sx = (i & 1) ? 1.0f : -1.0f;
            const float sy = (i & 2) ? 1.0f : -1.0f;
            const float sz = (i & 4) ? 1.0f : -1.0f;
            const Vec3 p = fat.center + fat.axisX * (fat.halfExtents.x * sx)
                                      + fat.axisY * (fat.halfExtents.y * sy)
                                      + fat.axisZ * (fat.halfExtents.z * sz);
            blo = Vec3(std::min(blo.x, p.x), std::min(blo.y, p.y), std::min(blo.z, p.z));
            bhi = Vec3(std::max(bhi.x, p.x), std::max(bhi.y, p.y), std::max(bhi.z, p.z));
        }
        auto lohi = [&](float a, float b2, float o, int n, int& i0, int& i1) {
            i0 = std::max(0, static_cast<int>(std::floor((a - o) / cell)));
            i1 = std::min(n - 1, static_cast<int>(std::ceil((b2 - o) / cell)));
        };
        int x0, x1, y0, y1, z0, z1;
        lohi(blo.x, bhi.x, g.origin.x, g.nx, x0, x1);
        lohi(blo.y, bhi.y, g.origin.y, g.ny, y0, y1);
        lohi(blo.z, bhi.z, g.origin.z, g.nz, z0, z1);
        for (int z = z0; z <= z1; ++z)
            for (int y = y0; y <= y1; ++y)
                for (int x = x0; x <= x1; ++x)
                    if (pointInObb(g.center(x, y, z), fat))
                        g.v[static_cast<std::size_t>(g.index(x, y, z))] = kSolid;
    }

    const auto tF1 = std::chrono::high_resolution_clock::now();

    // ── 空きの連結成分（6近傍）──
    //   ★成分の voxel を控えてから採否を決める。塗ってから捨てると、そのラベルが
    //     次の成分に再利用されて**別々の部屋が同じ番号になる**（一度これを踏んだ）。
    //
    //   ★走査線(scanline)で塗る。1 ボクセルずつ積む素直な塗りつぶしは、取り出すたびに
    //     近傍 6 個を**バラバラの番地**で読み、そのぶんスタックにも積む。1 個 14.8ns
    //     掛かっていて、除算を消しても 11.4ns にしかならなかった＝演算ではなく
    //     メモリ律速だった。X 方向に連続する区間をひとまとめの単位にすると、
    //       ・X 方向の探索と塗りが連続アクセス（memset）になる
    //       ・スタックに積む数が区間長ぶん（実測で 20〜60 分の 1）減る
    //       ・重心と境界も区間ごとに O(1) で足せる（Σx = (xl+xr)*n/2）
    //     6近傍なので、隣の行は [xl,xr] の範囲だけ見ればよい（8近傍のような
    //     斜め漏れの補正が要らない）。
    struct Run  { int i, n; };      // 線形添字の先頭と長さ
    struct Seed { int x, y, z; };
    std::vector<Seed> stack;
    std::vector<Run>  runs;
    const int nx = g.nx, ny = g.ny, nz = g.nz;
    const int sy = nx, sz = nx * ny;
    std::uint8_t* V = g.v.data();

    // 行 (y,z) の x∈[x0,x1] を走り、未訪問の区間の先頭を種として積む。
    auto scanLine = [&](int x0, int x1, int y, int z) {
        const int base = z * sz + y * sy;
        int x = x0;
        while (x <= x1) {
            while (x <= x1 && V[base + x] != kUnlabeled) ++x;
            if (x > x1) break;
            stack.push_back(Seed{x, y, z});
            while (x <= x1 && V[base + x] == kUnlabeled) ++x;
        }
    };

    std::uint8_t label = 0;
    int start = 0;
    for (int z = 0; z < nz; ++z)
    for (int y = 0; y < ny; ++y)
    for (int x = 0; x < nx; ++x, ++start) {
        if (V[start] != kUnlabeled) continue;

        stack.clear(); runs.clear();
        stack.push_back(Seed{x, y, z});
        bool touchesBoundary = false;
        long long ax = 0, ay = 0, az = 0;
        int count = 0;
        int miX = nx, miY = ny, miZ = nz, maX = -1, maY = -1, maZ = -1;
        while (!stack.empty()) {
            const Seed s = stack.back(); stack.pop_back();
            const int base = s.z * sz + s.y * sy;
            if (V[base + s.x] != kUnlabeled) continue;   // 先に別の区間に取られていた
            int xl = s.x; while (xl > 0      && V[base + xl - 1] == kUnlabeled) --xl;
            int xr = s.x; while (xr < nx - 1 && V[base + xr + 1] == kUnlabeled) ++xr;
            const int n = xr - xl + 1;
            std::memset(V + base + xl, kOutside, static_cast<std::size_t>(n));  // 訪問済みの印（採否は後で）
            runs.push_back(Run{base + xl, n});
            count += n;
            ax += (static_cast<long long>(xl) + xr) * n / 2;   // (xl+xr)*n は必ず偶数
            ay += static_cast<long long>(s.y) * n;
            az += static_cast<long long>(s.z) * n;
            if (xl  < miX) miX = xl;   if (xr  > maX) maX = xr;
            if (s.y < miY) miY = s.y;  if (s.y > maY) maY = s.y;
            if (s.z < miZ) miZ = s.z;  if (s.z > maZ) maZ = s.z;
            if (xl == 0 || xr == nx - 1 || s.y == 0 || s.y == ny - 1
                || s.z == 0 || s.z == nz - 1) touchesBoundary = true;
            if (s.y > 0)      scanLine(xl, xr, s.y - 1, s.z);
            if (s.y < ny - 1) scanLine(xl, xr, s.y + 1, s.z);
            if (s.z > 0)      scanLine(xl, xr, s.y, s.z - 1);
            if (s.z < nz - 1) scanLine(xl, xr, s.y, s.z + 1);
        }

        // ★格子の外周に届いた成分は「外の世界」であって部屋ではない。
        //   外側に 1 ボクセルの余白を取ってあるので、屋外や囲われていない空間は
        //   必ずここへ落ちる。これで「閉じた空間かどうか」が判定できる。
        if (touchesBoundary) { r.outsideVoxels += count; continue; }
        if (count < minVoxels) { r.discarded++; continue; }
        if (label >= kMaxRooms) { r.discarded++; continue; }   // 部屋番号を使い切った

        for (const Run& rn : runs)
            std::memset(V + rn.i, label, static_cast<std::size_t>(rn.n));
        const float inv = 1.0f / static_cast<float>(count);
        Room rm;
        rm.voxels = count;
        rm.centroid = Vec3(g.origin.x + (static_cast<float>(ax) * inv + 0.5f) * cell,
                           g.origin.y + (static_cast<float>(ay) * inv + 0.5f) * cell,
                           g.origin.z + (static_cast<float>(az) * inv + 0.5f) * cell);
        rm.boundsMin = g.center(miX, miY, miZ);
        rm.boundsMax = g.center(maX, maY, maZ);
        r.rooms.push_back(rm);
        ++label;
    }
    const auto tL1 = std::chrono::high_resolution_clock::now();
    r.msAlloc = std::chrono::duration<double, std::milli>(tA1 - tA0).count();
    r.msFill  = std::chrono::duration<double, std::milli>(tF1 - tA1).count();
    r.msLabel = std::chrono::duration<double, std::milli>(tL1 - tF1).count();
    return r;
}

}  // namespace rooms
}  // namespace acoustic
