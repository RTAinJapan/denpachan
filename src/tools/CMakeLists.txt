# 開発用ツール
find_package(nlohmann_json CONFIG REQUIRED)

add_executable(audio_probe
    audio_probe.cpp
    ../engine/audio_devices.cpp
)
target_link_libraries(audio_probe PRIVATE denpachan_common ole32)

add_executable(denpachan-ctl ctl.cpp)
target_link_libraries(denpachan-ctl PRIVATE denpachan_icon nlohmann_json::nlohmann_json)

# 音声の時間軸の試験 (worker の書き手と engine の読み手を仮想の時計で動かす。音声デバイスは要らない。CI で回す)。
# 配布物には入れない (tools/package.ps1)
find_package(spdlog CONFIG REQUIRED)
add_executable(audio_timing_test
    audio_timing_test.cpp
    ../engine/channel_source.cpp
    ../engine/render_log.cpp
    ../worker/audio_writer.cpp
)
target_include_directories(audio_timing_test SYSTEM PRIVATE ${FFMPEG_INCLUDE_DIRS})
target_link_directories(audio_timing_test PRIVATE ${FFMPEG_LIBRARY_DIRS})
target_link_libraries(audio_timing_test PRIVATE denpachan_common ${FFMPEG_LIBRARIES} spdlog::spdlog)
# FFmpeg は呼ばない (48 kHz なのでリサンプルしない) ので、DLL が無くても動くよう遅延ロードにする
denpachan_delayload_ffmpeg(audio_timing_test libswresample)
