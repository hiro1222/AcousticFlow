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

# ── Visual Studio での見え方（2026-09-24）──
#   ソリューションを開いたときに、置いてある形のまま全部たどれるようにする。
#   ★ここで足すのは「IDE に見せる」指定だけで、コンパイルされる物は 1 つも増えない。
#     ヘッダや .md を target に足しても翻訳単位にはならず、ソリューション エクスプローラに出るだけ。
set_property(GLOBAL PROPERTY USE_FOLDERS ON)                       # ソリューションのフォルダ分けを使う
set_property(GLOBAL PROPERTY PREDEFINED_TARGETS_FOLDER "CMake")    # ALL_BUILD / ZERO_CHECK を隅へ寄せる

# サブプロジェクトを取り込む。それぞれに CMakeLists.txt がある。
add_subdirectory(AcousticEngine)      # DLL 本体
add_subdirectory(AcousticEngineTest)  # C++ 動作確認
if(WIN32)
    add_subdirectory(tools/AfCapture) # 録画の道具（ゲームの窓＋Wwise の録音 → MP4）
endif()

# ── 読む物（ビルドしない。同じソリューションから開けるようにするための入れ物）──
#   add_custom_target(... SOURCES ...) は「並べるだけ」の的。決めごとの文書と、Unity 側の
#   ホスト（C#）をここに載せておくと、エンジンを読みながら行き来できる。
#   ★C# の実体は Unity が作る UnityDemo.sln 側でビルドされる。こちらは閲覧専用。
file(GLOB AF_DOCS CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/docs/*.md")
if(AF_DOCS)
    add_custom_target(Docs SOURCES ${AF_DOCS})
    source_group(TREE "${CMAKE_SOURCE_DIR}/docs" PREFIX "docs" FILES ${AF_DOCS})
    set_property(TARGET Docs PROPERTY FOLDER "読む物")
endif()

file(GLOB_RECURSE AF_HOST_CS CONFIGURE_DEPENDS
     "${CMAKE_SOURCE_DIR}/Projects/UnityDemo/Assets/Scripts/AcousticFlow/*.cs")
if(AF_HOST_CS)
    add_custom_target(HostCSharp SOURCES ${AF_HOST_CS})
    source_group(TREE "${CMAKE_SOURCE_DIR}/Projects/UnityDemo/Assets/Scripts/AcousticFlow"
                 PREFIX "AcousticFlow" FILES ${AF_HOST_CS})
    set_property(TARGET HostCSharp PROPERTY FOLDER "読む物")
endif()

# F5 で立ち上がる的を、いちばんよく回す検査にしておく（人が毎回選び直さなくて済む）。
set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT FlowRegressionTest)
