/* Gpu/trace_kernel.h ── レイ 1 本を GPU で追う HLSL（文字列で持つ）
 *
 * ■ 役割
 *   energy_trace.h の traceRay を、そのまま HLSL に写した物。1 スレッド ＝ レイ 1 本。
 *   ★CPU 版と**同じ順序・同じ式**で書く。速さのために式を変えない。
 *     変えると、答えが違ったときに「GPU だから」なのか「式を変えたから」なのか分からなくなる。
 *
 * ■ 中の仕組み（CPU 版との対応）
 *   rand01 / uniformSphere / cosineHemisphere … 同じ式。種も同じ（音源 × レイ番号）
 *   rayIntersectsObb                          … 箱のローカル軸へ写してスラブ法。CPU と同じ順で
 *   木の降り方                                … 配列のスタック。CPU の sceneNearest と同じ形
 *   次イベント推定（NEE）                      … 当たり点ごとにリスナーへ 1 本、cosθ/(πd²)
 *   反射か透過か                              … ロシアンルーレット。重みは r+t
 *   打ち切り                                  … 帯域の最大が出発の 1e-4 を切ったら残りを帳簿へ
 *
 * ■ ビット一致しないこと（先に書いておく）
 *   GPU と CPU は**一致しない**。丸めも sin/cos の実装も違う。守るのは
 *     ・静止していれば毎フレーム同じ（種が本の番号だけで決まるので GPU でも保てる）
 *     ・保存則（放射 = 吸収 + 残り + 逃げ）
 *     ・初期・後期が CPU と dB で近い
 *   の 3 つ。突き合わせはこの 3 つで行う。
 *
 * ■ 壊れる所
 *   ・numthreads とディスパッチの組数が食い違うと、後ろのレイが走らない（静かに欠ける）。
 *   ・組 → 音源の表と blockFirst がずれると、音源の枠を越えて書き、**隣の音源の答えが化ける**。
 *     切り上げで余った本は「書かずに帰る」こと（0 を書くと隣の 1 本目を潰す）。
 *   ・構造化バッファの並びが C++ 側の struct と 1 バイトでもずれると、全部が化ける。
 *     Obb は float3 が 4 つ + float3 で 15 float。HLSL 側も float の並びで受ける。
 *   ・スタックの深さが足りないと木の奥が見えない（CPU と同じ 64）。
 *   ・部屋の引き方（roomAtP、段 2-f）は CPU の sceneRoomAt と同じ切り捨て。(int) は 0 へ向けて切るので floor にしないこと。
 */
#ifndef ACOUSTICFLOW_GPU_TRACE_KERNEL_H
#define ACOUSTICFLOW_GPU_TRACE_KERNEL_H

namespace acoustic {
namespace gpu {

/// レイの本体（cs_5_0）。入口は "main"。
///   t0 箱（float 15 個ずつ）／t1 木のノード（float 6 + int 4）／t2 葉の添字と材質と印
///   t3 材質の表（反射・透過・吸収 × 6 帯域 + 散乱率）／t4 音源ごとの設定／t5 組 → 音源の表／t6 部屋の格子
///   u0 レイごとの取り分（音源ごとに rayBase から rays 本ぶん）
inline const char* kTraceKernelHlsl() {
    return R"HLSL(
// ★6 帯域は **float3 を 2 本**で持つ（lo = 125/250/500、hi = 1k/2k/4k）。
//   HLSL の局所配列は動的な添字で書けず、跳ね返りの繰り返し（回数が動的）の中で
//   e[b] のように書くと「展開を強制 → 展開できない」で翻訳が落ちる。
//   ベクトルなら成分ごとの演算がそのまま書けて、速さでも素直。
struct GObb   { float3 c; float3 he; float3 ax; float3 ay; float3 az; int material; int active; };
struct GNode  { float3 bmin; float3 bmax; int left; int right; int first; int count; };
struct GItem  { int box; int pad0; int pad1; int pad2; };
struct GMat   { float3 reflLo; float3 reflHi; float3 tranLo; float3 tranHi;
                float3 absbLo; float3 absbHi; float scat; float3 pad; };
struct GOut   { float3 earlyLo; float3 earlyHi; float3 lateLo; float3 lateHi;
                float3 emitLo;  float3 emitHi;  float3 absbLo; float3 absbHi;
                float3 remLo;   float3 remHi;   float3 escLo;  float3 escHi;
                float firstSec; int hits; int nee; int pad;
                // 段 2-f: 後期のうち戸口越しの面から来た分（late の内訳）と、その向きの重み付き和（ワールド）
                float3 lateOtherLo; float3 lateOtherHi; float3 otherDir; float pad2; };
// ★音源ごとに違う物だけを集めた欄。定数（cbuffer）に置くと 1 音源しか入らないので、
//   ここへ移して 1 回のディスパッチで全音源を流せるようにした（本数・跳ね返り・境・種は音源ごとに違う）。
//   rays は「このフレームに**実際に走る本数**」（フレーム分散で 1/groups に減った後の数）。
//   走らない本を出力に並べると、その分だけ読み戻しが太る（既定 群 4 なら 4 倍）。
struct GEmit  { float3 source; float e0; uint rayBase; uint rays; uint seed; uint groups;
                uint group; uint maxBounces; uint blockFirst; float mixingSec; };

StructuredBuffer<GObb>  gObb  : register(t0);
StructuredBuffer<GNode> gNode : register(t1);
StructuredBuffer<GItem> gItem : register(t2);
StructuredBuffer<GMat>  gMat  : register(t3);
StructuredBuffer<GEmit> gEmit : register(t4);
StructuredBuffer<uint>  gBlock : register(t5);   // 組 → 音源の番号（組ごとに 1 個）
StructuredBuffer<int>   gRoomVox : register(t6); // 部屋の格子（段 2-f）。(z*ny + y)*nx + x → 部屋番号
RWStructuredBuffer<GOut> gOut : register(u0);

cbuffer Cb : register(b0) {
    float3 gListener; float gPad0;
    uint   gBoxCount; uint gNodeCount; uint gItemCount; uint gBlockCount;
    float3 gRoomOrigin; float gRoomCell;
    int    gRoomNx; int gRoomNy; int gRoomNz; int gListenerRoom;   // −1 なら出どころを分けない（−2 は外の耳）
    int    gWallReflect; int gPad1; int gPad2; int gPad3;          // 0 なら壁を横切る影の線を数えない（CPU の wallReflect）
};

static const float kPi = 3.14159265358979f;
static const float kEps = 1e-4f;
static const float kSpeed = 343.0f;
// 空気吸収（dB/m）。CPU 側 room_graph.h の kAirDbPerM と同じ値。
static const float3 kAirLo = float3(0.0003f, 0.0008f, 0.0017f);
static const float3 kAirHi = float3(0.0030f, 0.0085f, 0.0250f);
float3 airLo(float d) { return pow(10.0f, -kAirLo * d * 0.1f); }
float3 airHi(float d) { return pow(10.0f, -kAirHi * d * 0.1f); }

// ── 点がどの部屋か（段 2-f。CPU の sceneRoomAt と同じ切り捨て）──
int roomAtP(float3 p) {
    if (gRoomNx <= 0 || gRoomCell <= 0.0f) return -1;
    int x = (int)((p.x - gRoomOrigin.x) / gRoomCell);
    int y = (int)((p.y - gRoomOrigin.y) / gRoomCell);
    int z = (int)((p.z - gRoomOrigin.z) / gRoomCell);
    if (x < 0 || y < 0 || z < 0 || x >= gRoomNx || y >= gRoomNy || z >= gRoomNz) return -1;
    return gRoomVox[(z * gRoomNy + y) * gRoomNx + x];
}

// ── 乱数（CPU と同じ式）──
float rand01(inout uint s) {
    s = s * 747796405u + 2891336453u;
    uint w = ((s >> ((s >> 28) + 4u)) ^ s) * 277803737u;
    w = (w >> 22) ^ w;
    return (float)w * (1.0f / 4294967296.0f);
}
float3 uniformSphere(inout uint s) {
    float z = 1.0f - 2.0f * rand01(s);
    float th = 2.0f * kPi * rand01(s);
    float r = sqrt(max(0.0f, 1.0f - z * z));
    return float3(r * cos(th), z, r * sin(th));
}
float3 cosineHemisphere(float3 n, inout uint s) {
    float u1 = rand01(s), u2 = rand01(s);
    float r = sqrt(u1);
    float th = 2.0f * kPi * u2;
    float x = r * cos(th), y = r * sin(th);
    float z = sqrt(max(0.0f, 1.0f - u1));
    float3 t = (abs(n.x) > 0.9f) ? float3(0, 1, 0) : float3(1, 0, 0);
    float3 b1 = normalize(cross(n, t));
    float3 b2 = cross(n, b1);
    return normalize(b1 * x + b2 * y + n * z);
}

// ── 箱との交差（CPU の rayIntersectsObb と同じ順・同じ規約）──
bool hitObb(float3 o, float3 d, GObb b, float maxT, out float outT, out float3 outN) {
    outT = 0.0f; outN = float3(0, 0, 1);
    float3 p = o - b.c;
    float3 lo = float3(dot(p, b.ax), dot(p, b.ay), dot(p, b.az));
    float3 ld = float3(dot(d, b.ax), dot(d, b.ay), dot(d, b.az));
    float tmin = 0.0f, tmax = maxT;
    int axis = -1; float sgn = 0.0f;
    for (int i = 0; i < 3; ++i) {
        float oi = (i == 0) ? lo.x : ((i == 1) ? lo.y : lo.z);
        float di = (i == 0) ? ld.x : ((i == 1) ? ld.y : ld.z);
        float hi = (i == 0) ? b.he.x : ((i == 1) ? b.he.y : b.he.z);
        if (abs(di) < 1e-8f) { if (oi < -hi || oi > hi) return false; continue; }
        float inv = 1.0f / di;
        float t1 = (-hi - oi) * inv, t2 = (hi - oi) * inv;
        float sg = -1.0f;
        if (t1 > t2) { float tt = t1; t1 = t2; t2 = tt; sg = 1.0f; }
        if (t1 > tmin) { tmin = t1; axis = i; sgn = sg; }
        tmax = min(tmax, t2);
        if (tmin > tmax) return false;
    }
    if (axis < 0) { outT = 0.0f; outN = b.ay; return true; }   // 起点が箱の中（CPU と同じ扱い）
    outT = tmin;
    float3 a = (axis == 0) ? b.ax : ((axis == 1) ? b.ay : b.az);
    outN = a * sgn;
    return true;
}
bool hitAabb(float3 o, float3 d, float3 bmin, float3 bmax, float maxT) {
    float tmin = 0.0f, tmax = maxT;
    for (int i = 0; i < 3; ++i) {
        float oi = (i == 0) ? o.x : ((i == 1) ? o.y : o.z);
        float di = (i == 0) ? d.x : ((i == 1) ? d.y : d.z);
        float lo = (i == 0) ? bmin.x : ((i == 1) ? bmin.y : bmin.z);
        float hi = (i == 0) ? bmax.x : ((i == 1) ? bmax.y : bmax.z);
        if (abs(di) < 1e-8f) { if (oi < lo || oi > hi) return false; continue; }
        float inv = 1.0f / di;
        float t1 = (lo - oi) * inv, t2 = (hi - oi) * inv;
        if (t1 > t2) { float tt = t1; t1 = t2; t2 = tt; }
        tmin = max(tmin, t1); tmax = min(tmax, t2);
        if (tmin > tmax) return false;
    }
    return true;
}

struct Hit { bool hit; float t; float3 n; float3 p; int index; };
Hit nearestHit(float3 o, float3 d, float maxDist, int skip) {
    Hit best; best.hit = false; best.t = maxDist; best.n = float3(0, 0, 1); best.p = o; best.index = -1;
    int stack[64]; int sp = 0;
    if (gNodeCount > 0) stack[sp++] = 0;
    while (sp > 0) {
        GNode nd = gNode[stack[--sp]];
        if (!hitAabb(o, d, nd.bmin, nd.bmax, best.t)) continue;
        if (nd.count > 0) {
            for (int k = 0; k < nd.count; ++k) {
                int i = gItem[nd.first + k].box;
                if (i == skip || gObb[i].active == 0) continue;
                float t; float3 nn;
                if (hitObb(o, d, gObb[i], best.t, t, nn) && t < best.t && t > 0.0f) {
                    best.hit = true; best.t = t; best.n = nn; best.index = i;
                }
            }
        } else if (sp + 2 <= 64) {
            stack[sp++] = nd.left; stack[sp++] = nd.right;
        }
    }
    if (best.hit) best.p = o + d * best.t;
    return best;
}

// τ の積。★CPU は添字の昇順に掛けるが、GPU は歩いた順にそのまま掛ける。
//   掛け算は値として可換なので、違いは丸めの末尾だけ（相対 1e-7 くらい）。
//   局所配列を動的に書かない形にするため、ここは順を揃えない。
void transmitTau(float3 p0, float3 p1, int skip, out float3 tauLo, out float3 tauHi, out int crossed) {
    tauLo = float3(1, 1, 1); tauHi = float3(1, 1, 1); crossed = 0;
    float3 dv = p1 - p0;
    float len = length(dv);
    if (len <= kEps) return;
    float3 d = dv / len;
    int stack[64]; int sp = 0;
    if (gNodeCount > 0) stack[sp++] = 0;
    while (sp > 0) {
        GNode nd = gNode[stack[--sp]];
        if (!hitAabb(p0, d, nd.bmin, nd.bmax, len)) continue;
        if (nd.count > 0) {
            for (int k = 0; k < nd.count; ++k) {
                int i = gItem[nd.first + k].box;
                if (i == skip || gObb[i].active == 0) continue;
                float t; float3 nn;
                if (!hitObb(p0, d, gObb[i], len, t, nn)) continue;
                if (t <= 0.0f || t >= len) continue;
                int mi = gObb[i].material;
                tauLo *= gMat[mi].tranLo; tauHi *= gMat[mi].tranHi;
                crossed = 1;
            }
        } else if (sp + 2 <= 64) {
            stack[sp++] = nd.left; stack[sp++] = nd.right;
        }
    }
}

[numthreads(64, 1, 1)]
void main(uint3 gid : SV_GroupID, uint3 gtid : SV_GroupThreadID) {
    // ★どの音源の何本目か、を「組の番号」から引く。
    //   音源ごとに本数が違うので、組を音源の境で切り上げて並べ、組 → 音源の表を CPU が作る。
    //   通し番号から割り算で求める形にすると、本数が音源ごとに違う時点で成り立たない。
    uint blk = gid.x;
    if (blk >= gBlockCount) return;
    uint j = gBlock[blk];
    GEmit em = gEmit[j];
    uint l = (blk - em.blockFirst) * 64u + gtid.x;
    if (l >= em.rays) return;              // 切り上げで余った本（書かずに帰る＝隣の音源を踏まない）
    uint slot = em.rayBase + l;
    // ★通し番号は詰めた番号から戻す。種は**通し番号**で作るので、
    //   どの組に入っていても同じレイは同じ道を通る（静止していれば揺れない、の根拠）。
    uint i = em.group + l * em.groups;
    GOut o = (GOut)0;
    o.firstSec = -1.0f;

    uint rng = (em.seed * 2654435761u + 0x9E3779B9u) ^ (i * 2246822519u);
    rng = rng * 747796405u + 2891336453u;
    float3 eLo = float3(em.e0, em.e0, em.e0), eHi = eLo;
    o.emitLo = eLo; o.emitHi = eHi;
    float3 pos = em.source;
    float3 dir = uniformSphere(rng);
    float pathLen = 0.0f;
    int skip = -1;
    bool terminated = false;
    for (uint bounce = 0; bounce < em.maxBounces; ++bounce) {
        Hit h = nearestHit(pos, dir, 1e4f, skip);
        if (!h.hit) { o.escLo += eLo; o.escHi += eHi; terminated = true; break; }
        o.hits += 1;
        pathLen += h.t;
        int mi = gObb[h.index].material;
        float3 rLo = gMat[mi].reflLo, rHi = gMat[mi].reflHi;
        float3 tLo = gMat[mi].tranLo, tHi = gMat[mi].tranHi;
        o.absbLo += eLo * gMat[mi].absbLo;
        o.absbHi += eHi * gMat[mi].absbHi;
        float rMean = (rLo.x + rLo.y + rLo.z + rHi.x + rHi.y + rHi.z) / 6.0f;
        float tMean = (tLo.x + tLo.y + tLo.z + tHi.x + tHi.y + tHi.z) / 6.0f;
        float3 nFace = (dot(h.n, dir) < 0.0f) ? h.n : -h.n;
        // ── NEE ──
        float3 toL = gListener - h.p;
        float dL = length(toL);
        if (dL > kEps) {
            float3 u = toL / dL;
            float cosF = dot(nFace, u);
            bool front = cosF > 0.0f;
            float cosT = abs(cosF);
            float3 side = front ? nFace : -nFace;            // リスナーの側（放射する面の向き）
            float3 org = h.p + side * kEps;
            float3 trLo, trHi; int crossed; transmitTau(org, gListener, h.index, trLo, trHi, crossed);
            if (gWallReflect == 0 && (crossed != 0 || !front)) { trLo = float3(0, 0, 0); trHi = float3(0, 0, 0); }   // 壁越しと面の裏側は通さない
            float tSec = (pathLen + dL) / kSpeed;
            float geo = cosT / (kPi * dL * dL);
            float3 sideLo = eLo * (front ? rLo : tLo);
            float3 sideHi = eHi * (front ? rHi : tHi);
            float3 cLo = max(sideLo * geo * trLo * airLo(pathLen + dL), 0.0f);
            float3 cHi = max(sideHi * geo * trHi * airHi(pathLen + dL), 0.0f);
            bool any = (cLo.x + cLo.y + cLo.z + cHi.x + cHi.y + cHi.z) > 0.0f;
            if (tSec < em.mixingSec) { o.earlyLo += cLo; o.earlyHi += cHi; }
            else                   { o.lateLo  += cLo; o.lateHi  += cHi; }
            // ── 後期の出どころ（段 2-f。CPU の traceRay と同じ式）──
            if (any && gListenerRoom != -1 && tSec >= em.mixingSec) {   // −2 ＝ 外の耳（kListenerOutside）: どの部屋の面も別の部屋
                int room = roomAtP(h.p + side * max(0.3f, 1.25f * gRoomCell));
                if (room >= 0 && room != gListenerRoom) {
                    o.lateOtherLo += cLo; o.lateOtherHi += cHi;
                    float cs = cLo.x + cLo.y + cLo.z + cHi.x + cHi.y + cHi.z;
                    o.otherDir -= cs * u;
                }
            }
            if (any) {
                o.nee += 1;
                if (o.firstSec < 0.0f || tSec < o.firstSec) o.firstSec = tSec;
            }
        }
        // wallReflect=0 では透過を引かない（CPU の traceRay と同じ。抜けるはずだった分は escaped へ）
        float carry = (gWallReflect != 0) ? (rMean + tMean) : rMean;
        if (gWallReflect == 0) { o.escLo += eLo * tLo; o.escHi += eHi * tHi; }
        if (carry <= 1e-6f) { terminated = true; break; }
        bool goReflect = (gWallReflect != 0) ? (rand01(rng) < (rMean / carry)) : true;
        if (gWallReflect != 0) { eLo *= (rLo + tLo); eHi *= (rHi + tHi); } else { eLo *= rLo; eHi *= rHi; }
        float eMax = max(max(max(eLo.x, eLo.y), max(eLo.z, eHi.x)), max(eHi.y, eHi.z));
        if (eMax < em.e0 * 1e-4f) { o.remLo += eLo; o.remHi += eHi; terminated = true; break; }
        if (goReflect) {
            float sc = gMat[mi].scat;
            dir = (rand01(rng) < sc) ? cosineHemisphere(nFace, rng) : (dir - nFace * (2.0f * dot(dir, nFace)));
            if (dot(dir, nFace) <= 0.0f) dir = cosineHemisphere(nFace, rng);
            pos = h.p + nFace * kEps;
        } else {
            pos = h.p - nFace * kEps;
        }
        skip = h.index;
    }
    if (!terminated) { o.remLo += eLo; o.remHi += eHi; }
    gOut[slot] = o;
}
)HLSL";
}

}  // namespace gpu
}  // namespace acoustic

#endif  // ACOUSTICFLOW_GPU_TRACE_KERNEL_H
