find_package(FFMPEG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_path(SRT_INCLUDE_DIR srt/srt.h REQUIRED)
find_library(SRT_LIBRARY NAMES srt srt_static REQUIRED)

add_executable(denpachan-worker
    main.cpp
    srt_ingest.cpp
    decoder.cpp
    playout.cpp
    scaler.cpp
    vcam_writer.cpp
)
target_include_directories(denpachan-worker PRIVATE ${FFMPEG_INCLUDE_DIRS} ${SRT_INCLUDE_DIR})
target_link_directories(denpachan-worker PRIVATE ${FFMPEG_LIBRARY_DIRS})
target_link_libraries(denpachan-worker PRIVATE
    denpachan_common
    ${FFMPEG_LIBRARIES}
    ${SRT_LIBRARY}
    spdlog::spdlog
    ws2_32
    avrt
)
