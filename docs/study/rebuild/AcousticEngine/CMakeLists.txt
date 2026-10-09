# AcousticEngine ── DLL 本体（作り直し版。旧コア scene_api.cpp / scene.h は入れない）
set(ACOUSTIC_SOURCES
    src/Export/world_api.cpp   # 世界（Flow）の C API
    src/Export/voice_api.cpp   # 音源 1 本の DSP（Voice）・FDN・方向バス・HRTF の C API
    src/Export/host_api.cpp    # Wwise のプラグインとの台帳
    src/Core/material.cpp      # 材質の表（唯一の .cpp の中身）
    src/Gpu/compute_d3d11.cpp  # GPU（D3D11 コンピュート）
)
file(GLOB_RECURSE ACOUSTIC_HEADERS CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h" "${CMAKE_CURRENT_SOURCE_DIR}/include/*.h")
add_library(AcousticEngine SHARED ${ACOUSTIC_SOURCES} ${ACOUSTIC_HEADERS})
target_include_directories(AcousticEngine PUBLIC include)    # 使う側にも見せる（C API）
target_include_directories(AcousticEngine PRIVATE src)       # 中だけ（"Flow/world.h" などで引く）
target_compile_definitions(AcousticEngine PRIVATE ACOUSTICENGINE_EXPORTS)   # dllexport を有効に
target_link_libraries(AcousticEngine PRIVATE d3d11 d3dcompiler dxgi)
