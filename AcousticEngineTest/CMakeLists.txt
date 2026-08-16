# AcousticEngineTest — C++ 動作確認ハーネス
# DLL のエクスポート関数を呼べることを確認する最小の実行ファイル。
add_executable(AcousticEngineTest main.cpp)

# AcousticEngine にリンクする。これで include パスも引き継がれ、
# ビルド時にインポートライブラリ(.lib)経由で DLL の関数に解決される。
target_link_libraries(AcousticEngineTest PRIVATE AcousticEngine)

# AF_Scene*(新アーキ) の数値回帰テスト。
# API 移行（docs/API_MIGRATION_PLAN.md）で内部構造を動かす前後に実行し、
# 物理として保たれるべき関係が壊れていないかを確認する。
add_executable(SceneRegressionTest scene_regression.cpp)
target_link_libraries(SceneRegressionTest PRIVATE AcousticEngine)

# エンジン単体で音を作って WAV へ書き出すツール。
#   Unity はネイティブ DLL をロックするので、音の調整のたびに閉じる必要があって遅い。
#   これなら数秒で回る。デモ動画の素材作りにも使える。
add_executable(AfRenderWav render_wav.cpp)
target_link_libraries(AfRenderWav PRIVATE AcousticEngine)

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
