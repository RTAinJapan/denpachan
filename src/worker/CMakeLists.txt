find_package(spdlog CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_path(SRT_INCLUDE_DIR srt/srt.h REQUIRED)
find_library(SRT_LIBRARY NAMES srt srt_static REQUIRED)

add_executable(denpachan-worker
    main.cpp
    srt_ingest.cpp
    flv_ring_ingest.cpp
    decoder.cpp
    playout.cpp
    preview_writer.cpp
    recorder.cpp
    external_encoder.cpp
    filename.cpp
    audio_writer.cpp
    scaler.cpp
    vcam_writer.cpp
)
target_include_directories(denpachan-worker SYSTEM PRIVATE ${FFMPEG_INCLUDE_DIRS} ${SRT_INCLUDE_DIR})
target_link_directories(denpachan-worker PRIVATE ${FFMPEG_LIBRARY_DIRS})
target_link_libraries(denpachan-worker PRIVATE
    denpachan_common
    ${FFMPEG_LIBRARIES}
    ${SRT_LIBRARY}
    spdlog::spdlog
    nlohmann_json::nlohmann_json
    ws2_32
    avrt
    dbghelp
)
denpachan_delayload_ffmpeg(denpachan-worker libavcodec libavformat libavutil libswresample libswscale)
