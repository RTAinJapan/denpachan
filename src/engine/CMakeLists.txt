find_package(FFMPEG REQUIRED)
find_package(spdlog CONFIG REQUIRED)

add_executable(denpachan-engine
    main.cpp
    audio_devices.cpp
    device_output.cpp
    channel_source.cpp
)
target_include_directories(denpachan-engine PRIVATE ${FFMPEG_INCLUDE_DIRS})
target_link_directories(denpachan-engine PRIVATE ${FFMPEG_LIBRARY_DIRS})
target_link_libraries(denpachan-engine PRIVATE
    denpachan_common
    ${FFMPEG_LIBRARIES}
    spdlog::spdlog
    ole32
    avrt
    winmm
)
