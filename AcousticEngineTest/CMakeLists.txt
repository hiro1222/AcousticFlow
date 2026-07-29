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
