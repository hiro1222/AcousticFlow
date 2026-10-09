// afr_api.cpp — いまは版だけ。中身は積む口・枠・バスができてから。
//
// ★空の実装を並べません。呼べるのに効かない関数は、
//   「無い」より害が大きい（呼んだ側は効いたと思う）。

#include "afr_api.h"

extern "C" int AFR_AbiVersion(void) { return AFR_ABI_VERSION; }
