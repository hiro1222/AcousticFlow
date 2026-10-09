# AcousticEngine — DLL 本体
#   Core/    物理計算（幾何・部屋グラフ・経路・尾の形）
#   Dsp/     音の生成（FFT・畳み込み・HRTF・尾IR）。ヘッダオンリー
#   Debug/   計測・キャプチャ。ヘッダオンリー
#   Export/  C API 窓口（extern "C"）
#
# ★2026-09-03: Wwise 連携（Adapter/ と Export/acoustic_api.cpp、Core/acoustic_world・source_directivity）を削除した。
#   既定ビルド（AF_USE_WWISE=OFF）ではスタブ Adapter が入っていて、Unity 側の「Wwise 風の再生経路」は
#   何も鳴らさない死んだ道だった。音は Unity の AudioSource → 畳み込み器（Dsp/）だけで出す。
#   道を 2 本持っていると、聞こえている音がどちらの物か切り分けられない。

set(ACOUSTIC_SOURCES
    src/Gpu/compute_d3d11.cpp
    src/Export/scene_api.cpp
    src/Export/voice_api.cpp
    src/Export/world_api.cpp
    src/Export/host_api.cpp    # 外の器（Wwise のプラグイン）との台帳（2026-09-29）
    src/Core/material.cpp
)

# ── IDE に見せるヘッダ（2026-09-24）──
#   このエンジンはほぼ全部がヘッダ（59 本）で、.cpp は 5 本しかない。ヘッダを target に載せないと
#   Visual Studio のソリューション エクスプローラにはその 5 本しか出ず、中身をたどれなかった。
#   ★載せてもコンパイル対象は増えない（ヘッダは翻訳単位にならない）。出来上がる DLL は 1 バイトも変わらない。
#   ★CONFIGURE_DEPENDS: ファイルを足したときに cmake を手で回し直さなくて済む。代わりにビルドのたびに
#     glob が 1 回走るが、64 ファイルなので費用は無視できる。明示列挙にすると、新しい段を書くたびに
#     ここへ足す手間が増え、忘れると「IDE から見えないファイル」が生まれる（それが今の状態だった）。
file(GLOB_RECURSE ACOUSTIC_HEADERS CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h"
     "${CMAKE_CURRENT_SOURCE_DIR}/include/*.h")

add_library(AcousticEngine SHARED ${ACOUSTIC_SOURCES} ${ACOUSTIC_HEADERS})

# 置いてあるフォルダの形（include / src/Core / src/Dsp / src/Flow / src/Debug / src/Export / src/Gpu）
# のままツリーに出す。既定の「ソース ファイル / ヘッダー ファイル」の 2 つ山だと、59 本が一列に並んで探せない。
set(ACOUSTIC_SOURCES_ABS "")
foreach(s IN LISTS ACOUSTIC_SOURCES)
    list(APPEND ACOUSTIC_SOURCES_ABS "${CMAKE_CURRENT_SOURCE_DIR}/${s}")
endforeach()
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" PREFIX "AcousticEngine"
             FILES ${ACOUSTIC_SOURCES_ABS} ${ACOUSTIC_HEADERS})

# 公開ヘッダ（C API）の場所を利用側にも伝える（PUBLIC）。
target_include_directories(AcousticEngine PUBLIC include)
# Core/Dsp/Debug のヘッダは DLL 内部のみ（PRIVATE）。"Core/..." 等で include 可能に。
target_include_directories(AcousticEngine PRIVATE src)

# DLL ビルド側でだけ dllexport を有効化。
target_compile_definitions(AcousticEngine PRIVATE ACOUSTICENGINE_EXPORTS)

# GPU（D3D11 コンピュート）。エンジンが自前でデバイスを持つ ── ホストの物は借りない。
#   ★借りない理由: 検査と AfFlowWav が Unity 無しで回せる。この作品の作り方の根っこ。
#   dxgi は アダプタ名を読むためだけ（診断）。
target_link_libraries(AcousticEngine PRIVATE d3d11 d3dcompiler dxgi)
