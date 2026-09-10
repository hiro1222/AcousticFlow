# AcousticEngineTest — C++ 側の回帰テストと計測ツール群
#   ★2026-09-03: 旧 API（AcousticEngine_*）の疎通テスト main.cpp は、API ごと削除した。

# AF_Scene*(新アーキ) の数値回帰テスト。
# API 移行（docs/API_MIGRATION_PLAN.md）で内部構造を動かす前後に実行し、
# 物理として保たれるべき関係が壊れていないかを確認する。
add_executable(SceneRegressionTest scene_regression.cpp)
target_link_libraries(SceneRegressionTest PRIVATE AcousticEngine)

# ★配布先の DLL が古くないかを、テストの冒頭と末尾で見るための道。
#   DLL を Unity へ配るのは CMake に入れていない（手作業）。Unity が開いていると
#   DLL を掴むので、ビルド後の自動コピーはビルドを止めるか、黙って失敗するかの
#   どちらかになる。**ビルドは止めず、数字を読む直前に気づける**ほうを採った。
#   → 2026-08-22 に実際に 1 日ぶん配り忘れ、エディタでは前日のエンジンが鳴っていた。
#   パスはここで焼く（実行時の作業ディレクトリに依存させない）。
target_compile_definitions(SceneRegressionTest PRIVATE
    AF_BUILT_DLL_PATH="$<TARGET_FILE:AcousticEngine>"
    AF_UNITY_DLL_PATH="${CMAKE_SOURCE_DIR}/UnityDemo/Assets/Plugins/x86_64/AcousticEngine.dll"
    AF_DIST_DLL_PATH="${CMAKE_SOURCE_DIR}/DistDemo/Assets/Plugins/x86_64/AcousticEngine.dll")

# エンジン単体で音を作って WAV へ書き出すツール。
#   Unity はネイティブ DLL をロックするので、音の調整のたびに閉じる必要があって遅い。
#   これなら数秒で回る。デモ動画の素材作りにも使える。
add_executable(AfRenderWav render_wav.cpp)
target_link_libraries(AfRenderWav PRIVATE AcousticEngine)

# 同じ場面を**新コア（AF_World）だけ**で鳴らす版。旧と同じ寸法・同じ信号・同じ長さなので、
#   出た 2 本の WAV を AfAnalyzeWav でそのまま突き合わせられる（段 10 の前の突き合わせ用）。
#   ★旧コアの入口を 1 つも呼ばないので、段 10 で render_wav.cpp を消してもこちらは残る。
add_executable(AfFlowWav render_wav_flow.cpp)
target_link_libraries(AfFlowWav PRIVATE AcousticEngine)

# DSP(段4: C++移行) の数値回帰テスト。
# ヘッダを直接取り込むので DLL には依存しない（移行中は API がまだ生えていないため）。
add_executable(DspRegressionTest dsp_regression.cpp)
target_compile_features(DspRegressionTest PRIVATE cxx_std_17)

# BTM(有限楔の稜線積分) の性質チェック。
#   式は記憶から書いているので、値ではなく「物理として満たすべき性質」で検証する。
#   ヘッダを直接取り込むので DLL には依存しない。
add_executable(BtmRegressionTest btm_regression.cpp)
target_compile_features(BtmRegressionTest PRIVATE cxx_std_17)

# 録音した WAV を「エンジンと同じ6帯域」の数字に落とす解析ツール。
#   現実の扉の開閉を収録して、エンジンのカーブと重ねて比べるために使う。
add_executable(AfAnalyzeWav analyze_wav.cpp)
target_compile_features(AfAnalyzeWav PRIVATE cxx_std_17)

# 資料の「扉の開き具合」の図（patterns_all.svg）に載せる数値を実測する。
#   図と数字が同じ配置から出ていることを、コマンド 1 本で再現できるようにするためのもの。
add_executable(AfPatternTable pattern_table.cpp)
target_link_libraries(AfPatternTable PRIVATE AcousticEngine)

# 録れた .afcap をコマンド 1 本で走査する。
#   ★型紙は Unity のパネルが呼ぶのと同じ関数（Debug/detectors.h）。
#     走査の答えを 2 つ持たせないため。
add_executable(AfCapScan afcap_scan.cpp)
target_link_libraries(AfCapScan PRIVATE AcousticEngine)

# 畳み込み側（AF_Voice / AF_TailBus）だけの費用を測る。幾何は呼ばない。
#   「IR が畳み込みまで持っているから CPU が厳しい」を数字で確かめるための道具（2026-09-03）。
add_executable(AfDspBench dsp_bench.cpp)
target_link_libraries(AfDspBench PRIVATE AcousticEngine)

# Flow (new core, 2026-09-10) regression. Header-only + material.cpp; no DLL dependency yet.
add_executable(FlowRegressionTest flow_regression.cpp ../AcousticEngine/src/Core/material.cpp)
target_include_directories(FlowRegressionTest PRIVATE ${CMAKE_SOURCE_DIR}/AcousticEngine/src)
target_compile_features(FlowRegressionTest PRIVATE cxx_std_17)
