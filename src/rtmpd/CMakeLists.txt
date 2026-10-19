find_package(spdlog CONFIG REQUIRED)

# RTMP サーバ (engine が起動・監視する)
add_executable(denpachan-rtmpd
    main.cpp
    rtmp_server.cpp
    rtmp_connection.cpp
    amf0.cpp
)
target_link_libraries(denpachan-rtmpd PRIVATE denpachan_common denpachan_icon spdlog::spdlog ws2_32 dbghelp)
