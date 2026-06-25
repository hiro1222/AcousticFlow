# AcousticFlow — トップレベル CMake
# 空間音響エンジン本体(DLL)と C++ 動作確認テストをまとめてビルドする。
cmake_minimum_required(VERSION 3.20)
project(AcousticFlow LANGUAGES CXX)

# 使用する C++ の規格。17 は std::optional など実用的な機能が揃っていて、
# Wwise SDK や各種ライブラリとの相性も良い無難な選択。
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# MSVC は既定でソースをシステムのコードページ(日本語環境では CP932)として読む。
# 日本語コメントを含む UTF-8 ソースが文字化けして構文エラーになるため、
# ソース・実行文字セットを UTF-8 に固定する。Windows+日本語環境のお約束。
if(MSVC)
    add_compile_options(/utf-8)
endif()

# DLL と exe を同じ bin/ に出力する。
# こうしておくと Windows ではテスト実行時に DLL を自動で見つけられる
# （exe と同じフォルダが既定の DLL 探索先になるため、PATH 設定が不要）。
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# サブプロジェクトを取り込む。それぞれに CMakeLists.txt がある。
add_subdirectory(AcousticEngine)      # DLL 本体
add_subdirectory(AcousticEngineTest)  # C++ 動作確認
