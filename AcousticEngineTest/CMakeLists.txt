# AcousticEngineTest — C++ 動作確認ハーネス
# DLL のエクスポート関数を呼べることを確認する最小の実行ファイル。
add_executable(AcousticEngineTest main.cpp)

# AcousticEngine にリンクする。これで include パスも引き継がれ、
# ビルド時にインポートライブラリ(.lib)経由で DLL の関数に解決される。
target_link_libraries(AcousticEngineTest PRIVATE AcousticEngine)
