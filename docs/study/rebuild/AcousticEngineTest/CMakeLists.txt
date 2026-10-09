# 作り直し版の検査（docs/study/10_rebuild.md）。写したファイルがある検査だけを作るので、段ごとに同じこの 1 枚で済む。
#   段 1: dsp_regression.cpp  → DspRegressionTest  （DSP だけ。DLL は要らない）        160 件
#   段 2: flow_regression.cpp → FlowRegressionTest （Core・Flow・Gpu・DSP をソースのまま）196 件
#   段 3: host_regression.cpp → HostRegressionTest （DLL を通して台帳を確かめる）        10 件

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/dsp_regression.cpp)
    add_executable(DspRegressionTest dsp_regression.cpp)
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/flow_regression.cpp)
    # エンジンはほぼヘッダだけなので、.cpp の 2 本（材質の表・GPU）を一緒に入れれば DLL 無しで回る。
    add_executable(FlowRegressionTest flow_regression.cpp
                   ../AcousticEngine/src/Core/material.cpp ../AcousticEngine/src/Gpu/compute_d3d11.cpp)
    target_include_directories(FlowRegressionTest PRIVATE ${CMAKE_SOURCE_DIR}/AcousticEngine/src)
    target_link_libraries(FlowRegressionTest PRIVATE d3d11 d3dcompiler dxgi)
endif()

if(TARGET AcousticEngine AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/host_regression.cpp)
    add_executable(HostRegressionTest host_regression.cpp)
    target_link_libraries(HostRegressionTest PRIVATE AcousticEngine)
endif()
