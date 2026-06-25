# AcousticEngine — DLL 本体
#   Core/    物理計算（Wwise 非依存）
#   Adapter/ ミドルウェア連携層（Wwise を include するのはここだけ）
#   Export/  C API 窓口（extern "C"）

# --- Wwise SDK の場所 ---
# 既定はこの開発機での実際のパス。家の PC など別環境では
#   cmake -S . -B build -DWWISE_SDK_DIR="<別パス>/SDK"
# のように上書きできる。
set(WWISE_SDK_DIR "C:/Audiokinetic/Wwise_2024.1.14.9084/SDK"
    CACHE PATH "Wwise SDK のルート（include と x64_vc170 を含むフォルダ）")

# include フォルダの有無で Wwise の利用可否を判定する。
if(EXISTS "${WWISE_SDK_DIR}/include/AK/SoundEngine/Common/AkSoundEngine.h")
    set(WITH_WWISE ON)
    message(STATUS "AcousticEngine: Wwise SDK を検出 -> 本物の Adapter をビルド (${WWISE_SDK_DIR})")
else()
    set(WITH_WWISE OFF)
    message(STATUS "AcousticEngine: Wwise SDK 未検出 -> スタブ Adapter をビルド")
endif()

# --- ソース構成 ---
set(ACOUSTIC_SOURCES
    src/Export/acoustic_api.cpp
    src/Core/acoustic_world.cpp
    src/Core/material.cpp
    src/Core/source_directivity.cpp
)
if(WITH_WWISE)
    list(APPEND ACOUSTIC_SOURCES src/Adapter/wwise_adapter.cpp)
else()
    list(APPEND ACOUSTIC_SOURCES src/Adapter/null_adapter.cpp)
endif()

add_library(AcousticEngine SHARED ${ACOUSTIC_SOURCES})

# 公開ヘッダ（C API）の場所を利用側にも伝える（PUBLIC）。
target_include_directories(AcousticEngine PUBLIC include)
# Core/Adapter のヘッダは DLL 内部のみ（PRIVATE）。"Core/..." 等で include 可能に。
target_include_directories(AcousticEngine PRIVATE src)

# DLL ビルド側でだけ dllexport を有効化。
target_compile_definitions(AcousticEngine PRIVATE ACOUSTICENGINE_EXPORTS)

# --- Wwise との接続（検出できたときだけ）---
if(WITH_WWISE)
    # ヘッダ。
    target_include_directories(AcousticEngine PRIVATE "${WWISE_SDK_DIR}/include")

    # --- Wwise サンプルの Low-Level I/O ---
    # バンクをディスクから読むための実装。SDK同梱のサンプルcppを取り込む。
    set(WWISE_SAMPLES "${WWISE_SDK_DIR}/samples/SoundEngine")
    target_sources(AcousticEngine PRIVATE
        "${WWISE_SAMPLES}/Win32/AkDefaultIOHookDeferred.cpp"
        "${WWISE_SAMPLES}/Common/AkFileLocationBase.cpp"
        "${WWISE_SAMPLES}/Common/AkMultipleFileLocation.cpp"
        "${WWISE_SAMPLES}/Common/AkFilePackage.cpp"
        "${WWISE_SAMPLES}/Common/AkFilePackageLUT.cpp"
        "${WWISE_SAMPLES}/Common/AkGeneratedSoundBanksResolver.cpp"
    )
    target_include_directories(AcousticEngine PRIVATE
        "${WWISE_SAMPLES}/Common"
        "${WWISE_SAMPLES}/Win32"
    )

    # Wwise サンプル I/O は AkOSChar=wchar_t（UNICODE ビルド）前提で
    # GetFileAttributes 等の TCHAR マクロをワイド版に解決させる必要がある。
    target_compile_definitions(AcousticEngine PRIVATE UNICODE _UNICODE)

    # lib は構成別フォルダ。CMake 既定の動的 CRT に合わせ (StaticCRT) 無しを使う。
    #   Debug ビルド   -> x64_vc170/Debug/lib
    #   それ以外       -> x64_vc170/Release/lib
    set(WWISE_LIB_DIR "${WWISE_SDK_DIR}/x64_vc170/$<IF:$<CONFIG:Debug>,Debug,Release>/lib")
    target_link_directories(AcousticEngine PRIVATE "${WWISE_LIB_DIR}")

    # 最小構成で必要な Wwise ライブラリ。
    target_link_libraries(AcousticEngine PRIVATE
        AkSoundEngine
        AkMemoryMgr
        AkStreamMgr
        AkMusicEngine
        # ソースプラグイン: Tone Generator（無いと再生しても無音）
        AkToneSource
        # エフェクトプラグイン: RoomVerb（残響。使うバンクの読込/再生に必須）
        AkRoomVerbFX
        # Windows システムライブラリ（デバイス列挙などで必要）
        Ole32
    )

    # Debug/Profile の SoundEngine は Authoring プロファイラ接続用の
    # 通信機能を参照する。その実体が CommunicationCentral。
    # Winsock(ws2_32) にも依存する。Release(AK_OPTIMIZED) では不要。
    target_link_libraries(AcousticEngine PRIVATE
        $<$<NOT:$<CONFIG:Release>>:CommunicationCentral>
        $<$<NOT:$<CONFIG:Release>>:ws2_32>
    )

    # Release ライブラリはプロファイリング機能を除いた最適化版。
    # ヘッダ側の挙動を揃えるため Release では AK_OPTIMIZED を定義する。
    target_compile_definitions(AcousticEngine PRIVATE $<$<CONFIG:Release>:AK_OPTIMIZED>)
endif()
