// AcousticFlowSpace.h ── Unreal の座標とエンジンの座標の橋。
//
// ■ 役割
//   Unreal: X 前 / Y 右 / Z 上、cm。エンジン: x 右 / y 上 / z 前、m（Unity と同じ）。どちらも左手系。
//   並べ替え（Unreal の X,Y,Z → エンジンの z,x,y）は巡回の置換なので、向き（右手・左手）は裏返らない。
//   だから箱の軸も「右と上」を写せば、エンジンが作る 3 本目（前）も正しくなる。
// ■ 壊れる所
//   ・並べ替えを (X,Y,Z)→(x,y,z) のまま使うと、上下と前後が入れ替わって部屋が横倒しになる（床が壁になる）。
//   ・長さの 1/100 を忘れると、部屋が 100 倍になり、響きが何十秒も続く。
#pragma once

#include "CoreMinimal.h"
#include "acoustic_world.h"

namespace AcousticFlowSpace
{
	/// 位置（cm）→ エンジンの位置（m）。
	inline AF_Vector3 ToEngine(const FVector& U) { return AF_Vector3{ float(U.Y * 0.01), float(U.Z * 0.01), float(U.X * 0.01) }; }
	/// 向き（長さを変えない）→ エンジンの向き。
	inline AF_Vector3 DirToEngine(const FVector& U) { return AF_Vector3{ float(U.Y), float(U.Z), float(U.X) }; }
	/// エンジンの位置（m）→ Unreal の位置（cm）。場面を組むときに使う（場面はエンジンの座標で書く）。
	inline FVector ToUnreal(float X, float Y, float Z) { return FVector(double(Z) * 100.0, double(X) * 100.0, double(Y) * 100.0); }
	inline FVector ToUnreal(const float* P) { return ToUnreal(P[0], P[1], P[2]); }
	/// エンジンの向き → Unreal の向き（長さを変えない）。エンジンが返した箱の軸・戸口の軸を描くときに使う。
	inline FVector DirToUnreal(const float* D) { return FVector(D[2], D[0], D[1]); }
	/// エンジンの大きさ（右・上・前、m）→ Unreal の 1 m の立方体（/Engine/BasicShapes/Cube は 100 cm）に掛ける拡大率。
	inline FVector SizeToCubeScale(float SX, float SY, float SZ) { return FVector(SZ, SX, SY); }
}
