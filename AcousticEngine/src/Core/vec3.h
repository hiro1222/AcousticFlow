/* Core/vec3.h
 * Core 層が内部で使う 3 次元ベクトル。
 *
 * 注意: これは「計算層の内部表現」であり、C API の AF_Vector3 とは別物。
 *       境界(Export 層)で AF_Vector3 -> Vec3 に変換する。
 *       こうして計算層を境界仕様から独立させ、後で境界が変わっても
 *       Core を触らずに済むようにしている。
 */
#ifndef ACOUSTICFLOW_CORE_VEC3_H
#define ACOUSTICFLOW_CORE_VEC3_H

#include <cmath>

namespace acoustic {

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;

    Vec3() = default;
    Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
};

inline Vec3 operator-(const Vec3& a, const Vec3& b) {
    return Vec3(a.x - b.x, a.y - b.y, a.z - b.z);
}
inline Vec3 operator+(const Vec3& a, const Vec3& b) {
    return Vec3(a.x + b.x, a.y + b.y, a.z + b.z);
}
// ベクトル × スカラー（レイ上の点 origin + dir*t を求めるのに使う）。
inline Vec3 operator*(const Vec3& v, float s) {
    return Vec3(v.x * s, v.y * s, v.z * s);
}

// 内積。反射計算や指向性（cosθ）で多用する。
inline float dot(const Vec3& a, const Vec3& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

// 外積。面の基底（法線×right で up を作る）などに使う。
inline Vec3 cross(const Vec3& a, const Vec3& b) {
    return Vec3(a.y * b.z - a.z * b.y,
                a.z * b.x - a.x * b.z,
                a.x * b.y - a.y * b.x);
}

inline float length(const Vec3& v) {
    return std::sqrt(dot(v, v));
}

// 単位ベクトル化。長さ0のときは零ベクトルを返す（ゼロ除算回避）。
inline Vec3 normalized(const Vec3& v) {
    const float len = length(v);
    if (len < 1e-12f) return Vec3(0.0f, 0.0f, 0.0f);
    const float inv = 1.0f / len;
    return Vec3(v.x * inv, v.y * inv, v.z * inv);
}

// 鏡面反射。入射ベクトル d を法線 n の面で反射させる。
//   r = d - 2(d・n)n
// d, n が単位ベクトルなら r も単位ベクトルになる。
inline Vec3 reflect(const Vec3& d, const Vec3& n) {
    return d - n * (2.0f * dot(d, n));
}

}  // namespace acoustic

#endif  // ACOUSTICFLOW_CORE_VEC3_H
