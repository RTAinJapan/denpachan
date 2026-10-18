find_package(spdlog CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

add_executable(denpachan-engine
    main.cpp
    engine.cpp
    config.cpp
    worker_process.cpp
    audio_router.cpp
    ipc_server.cpp
    audio_devices.cpp
    device_output.cpp
    channel_source.cpp
)
target_include_directories(denpachan-engine SYSTEM PRIVATE ${FFMPEG_INCLUDE_DIRS})
target_link_directories(denpachan-engine PRIVATE ${FFMPEG_LIBRARY_DIRS})
target_link_libraries(denpachan-engine PRIVATE
    denpachan_common
    ${FFMPEG_LIBRARIES}
    spdlog::spdlog
    nlohmann_json::nlohmann_json
    ole32
    avrt
    winmm
    crypt32
    dbghelp
)
# engine が FFmpeg を使うのは 48 kHz 以外の音声デバイスへのリサンプルだけ。FFmpeg が無くても engine は動き、
# worker を起動する前に DLL の有無と版を確かめる (ffmpeg_runtime.h)
denpachan_delayload_ffmpeg(denpachan-engine libavcodec libavformat libavutil libswresample libswscale)
