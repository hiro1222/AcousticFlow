# AfCapture ── ゲームの窓と Wwise の録音を MP4 にする録画の道具（Windows の Media Foundation だけ）。
#   使い方は tools/record_unreal_demo.ps1。エンジンには繋がない（エンジンの DLL も要らない）。
add_executable(AfCapture af_capture.cpp)
target_compile_definitions(AfCapture PRIVATE UNICODE _UNICODE)
target_link_libraries(AfCapture PRIVATE mfplat mfreadwrite mfuuid winmm user32 gdi32 ole32)
set_property(TARGET AfCapture PROPERTY FOLDER "道具")
