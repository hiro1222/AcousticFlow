# Middleware — AcousticFlow の開発環境（ホスト）
#
# ★このプロジェクトは音響エンジンに **リンクしていません。**
#
#   トップの CMakeLists.txt からも呼ばれません。独立して構成・ビルドできます:
#
#       cmake -S Middleware -B Middleware/build -A x64
#       cmake --build Middleware/build --config Release
#
#   意図的にそうしてあります。ミドルウェアにとってエンジンは
#   **依存先ではなく、実行時に読み込む対象**だからです。
#   `target_link_libraries(... AcousticEngine)` を足したくなったら、その前に
#   「エンジンが無いとビルドできない道具になっていないか」を確かめてください。
#
# ⚠ 実行ファイル名に開発環境の名前（現在は仮称）を入れていません。
#   仮称のうちは資料と連絡板にしか名前を書かない決まりのためです。

cmake_minimum_required(VERSION 3.20)
project(AcousticFlowMiddleware LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ★ランタイムを実行ファイルに入れてしまう（/MT）。
#
#   既定（/MD）だと MSVCP140.dll / VCRUNTIME140.dll に依存します。開発機には
#   Visual Studio が入っているので気づけませんが、**入っていない機械では起動しません**。
#   道具は「exe を 1 つ渡せば動く」でありたいので、ここは大きさより可搬性を採ります
#   （実測 646KB → 700KB 前後。この規模なら誤差）。
#
#   ⚠ エンジン(DLL)側は /MD のままです。DLL 越しに CRT の物（FILE*・new した領域）を
#     やり取りしなければ混在して構いません。この境目は C API なので、やり取りするのは
#     POD だけです。
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# MSVC は既定でソースをシステムのコードページ(日本語環境では CP932)として読む。
# 日本語コメントを含む UTF-8 ソースが文字化けするので UTF-8 に固定する。
if(MSVC)
    add_compile_options(/utf-8)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# ══════════════════════════════════════════════════════════════════════════
#  的の割り方 ── 依存の向きを**ビルドに守らせる**
#
#  ★2026-09-01 に 1 つの `AfHostCore` から割りました。
#    include グラフを見たら**設計は既に割れていて**、ビルドがそれを言っていなかっただけです。
#    規律で守っている間は、うっかり 1 行 include すれば壊れます。的を割れば**ビルドが落ちます**。
#
#      AfRuntime    ゲームが載せる側。音を出す。⚠ 台帳もプロジェクトもファイル読みも知らない
#      AfAuthoring  作る側。台帳・イベント・プロジェクト・WAV
#      AfTooling    道具専用。影武者ごしの DLL 読み込み
#      AfCheck      自己検査。↑ 3 つを使う（使う側なので知っていてよい）
#
#  ⚠ **守るべき線は 2 本**
#    ① AfRuntime は AfAuthoring を link しない ── ゲームが .afproj の構文解析器を
#       抱え込むのはおかしい。将来ここに焼いた資産を読む `AfAsset` が入る
#    ② どの的も AcousticEngine を link しない ── 音響エンジンを知るのは Bridge だけ。
#       これが資料のピラー「エンジン非依存の層設計」の証明そのもの
# ══════════════════════════════════════════════════════════════════════════

# ── 鳴らす側（ゲームが載せる）
#    ヘッダのみの部品: block_source.h（1 本ぶんの音源）/ voice_bus.h（束ね）
add_library(AfRuntime STATIC
    src/afr_api.cpp
    src/audio_device.cpp
)
target_include_directories(AfRuntime PUBLIC src)
# 外部ライブラリは 0 本。OS が持っているものだけを使う。
#   ole32 : COM（デバイス列挙）
#   avrt  : 実時間スレッドの優先度（Pro Audio）
target_link_libraries(AfRuntime PUBLIC ole32 avrt)

# ── 作る側（道具と、将来の焼く道具が使う）
add_library(AfAuthoring STATIC
    src/wav.cpp
    src/library.cpp
    src/events.cpp
    src/project.cpp
)
target_include_directories(AfAuthoring PUBLIC src)

# ── 道具専用
#    ★影武者ごしの DLL 読み込み。**このミドルウェアが存在する理由**そのもの
#      （Unity が原本を掴むせいで、調整のたびに Unity を閉じる必要があった）。
add_library(AfTooling STATIC
    src/module.cpp
)
target_include_directories(AfTooling PUBLIC src)

# ── 自己検査（GUI 版とコンソール版が同じものを呼ぶ）
#    ★答えを 2 つ持たせないため、検査の実体はここ 1 つ。
add_library(AfCheck STATIC
    src/selftest.cpp
)
target_link_libraries(AfCheck PUBLIC AfRuntime AfAuthoring AfTooling)

# ── Dear ImGui（同梱・GUI 版だけが使う）
#
# ★資料の「外部ライブラリなし」は**エンジンの主張**。ここは道具なので既製品を使う。
#   どの的も ImGui に依存しない ── コンソール版の検査は GUI 抜きで回せる。
add_library(imgui STATIC
    third_party/imgui/imgui.cpp
    third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp
    third_party/imgui/imgui_widgets.cpp
    third_party/imgui/backends/imgui_impl_win32.cpp
    third_party/imgui/backends/imgui_impl_dx11.cpp
)
target_include_directories(imgui PUBLIC third_party/imgui)
target_link_libraries(imgui PUBLIC d3d11 dxgi d3dcompiler)

# 同梱物の日本語コメント警告まで拾っても仕方がないので、警告は既定のまま。
if(MSVC)
    target_compile_options(imgui PRIVATE /wd4996)
endif()

# ── GUI 版（Windows アプリケーション）
# app.rc がアイコンと版情報を実行ファイルへ埋め込む。
#
# ⚠ .rc の中の相対パスは **.rc 自身からの相対**（作業フォルダからではない）。
#
# ⚠ rc.exe は既定でソースをシステムのコードページ（日本語環境では CP932）として読む。
#   ソースは UTF-8 なので、日本語が化けて**閉じ引用符を食い、次の行で構文エラー**になる
#   （実際 `RC2133: unexpected value in value data` が出た）。/c65001 で UTF-8 を指定する。
#   ソース側の /utf-8 と同じ話が、資源コンパイラにも要るということ。
set(CMAKE_RC_FLAGS "${CMAKE_RC_FLAGS} /c65001")

add_executable(AfHost WIN32 src/app_main.cpp src/app.rc)
# windowscodecs (WIC) は --shot で自分の画面を PNG に書き出すのに使う。
# ★画面キャプチャに頼らないのは、他人の窓を巻き込むため（実際に巻き込んだ）。
target_link_libraries(AfHost PRIVATE AfCheck imgui windowscodecs comdlg32)

# ── コンソール版（検査用）
#
# ★GUI 版に --selftest を持たせず別の実行ファイルにしてある。
#   キーボードも画面も無い所（CI・自動化）で検査を回したいため。
add_executable(AfHostCheck src/console_main.cpp)
target_link_libraries(AfHostCheck PRIVATE AfCheck)
