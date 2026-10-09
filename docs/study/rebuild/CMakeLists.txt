# 作り直し版の根の CMake（docs/study/10_rebuild.md の段 0 で置く）
#   本家の CMakeLists.txt から、作り直しに要る所だけを残した物。
#   ★/utf-8 を外さないこと。ソースのコメントと文字列は UTF-8 の日本語で、無いと MSVC が cp932 で読んで壊れる。
cmake_minimum_required(VERSION 3.20)
project(AcousticFlowRebuild LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(MSVC)
    add_compile_options(/utf-8)
endif()

# 出来上がりを 1 か所（build/bin/<構成>）に集める。DLL と検査の exe が同じ場所に並ぶので、検査が DLL を見つけられる。
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# 段 3 で AcousticEngine/CMakeLists.txt を置くまでは DLL を作らない（段 1・2 は検査の exe だけで確かめる）。
if(EXISTS ${CMAKE_SOURCE_DIR}/AcousticEngine/CMakeLists.txt)
    add_subdirectory(AcousticEngine)
endif()
add_subdirectory(AcousticEngineTest)
