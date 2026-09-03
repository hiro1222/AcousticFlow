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
    src/Export/scene_api.cpp
    src/Export/voice_api.cpp
    src/Core/material.cpp
)

add_library(AcousticEngine SHARED ${ACOUSTIC_SOURCES})

# 公開ヘッダ（C API）の場所を利用側にも伝える（PUBLIC）。
target_include_directories(AcousticEngine PUBLIC include)
# Core/Dsp/Debug のヘッダは DLL 内部のみ（PRIVATE）。"Core/..." 等で include 可能に。
target_include_directories(AcousticEngine PRIVATE src)

# DLL ビルド側でだけ dllexport を有効化。
target_compile_definitions(AcousticEngine PRIVATE ACOUSTICENGINE_EXPORTS)
