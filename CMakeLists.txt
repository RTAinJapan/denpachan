cmake_minimum_required(VERSION 3.25)

project(denpachan LANGUAGES CXX VERSION 0.1.0)

if(NOT WIN32)
    message(FATAL_ERROR "denpachan supports Windows only")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_definitions(
    UNICODE _UNICODE
    WIN32_LEAN_AND_MEAN NOMINMAX
    _WIN32_WINNT=0x0A00  # Windows 10+
)
if(MSVC)
    add_compile_options(/W4 /permissive- /utf-8 /Zc:__cplusplus /EHsc /MP)
    # Release でも PDB を出す (クラッシュダンプ解析用)
    add_compile_options("$<$<CONFIG:Release>:/Zi>")
    add_link_options("$<$<CONFIG:Release>:/DEBUG>" "$<$<CONFIG:Release>:/OPT:REF>" "$<$<CONFIG:Release>:/OPT:ICF>")
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# ASIO 出力 (denpachan-asio.dll)。Steinberg の ASIO SDK (GPL-3.0) を使うので、この DLL だけ GPL-3.0 (src/asio/LICENSE)。
# OFF にすると SDK を取らず DLL も作らない (配布物に GPL のものが入らなくなり、ASIO の出力先は出ない)
option(DENPACHAN_ASIO "Build the ASIO output (denpachan-asio.dll, GPL-3.0; downloads the ASIO SDK)" ON)

include(cmake/FetchFfmpeg.cmake)  # ヘッダと import lib だけ使う (DLL は同梱しない)
if(DENPACHAN_ASIO)
    include(cmake/FetchAsioSdk.cmake)  # ヘッダだけ使う
endif()
include(cmake/FfmpegRuntime.cmake)

add_subdirectory(src/common)
add_subdirectory(src/vcam)
add_subdirectory(src/worker)
add_subdirectory(src/engine)
add_subdirectory(src/rtmpd)
add_subdirectory(src/tools)
if(DENPACHAN_ASIO)
    add_subdirectory(src/asio)
endif()

denpachan_strip_ffmpeg_dlls(denpachan-worker denpachan-engine)

# 版の情報 (エクスプローラーのプロパティに出る。版は上の project の VERSION)
include(cmake/VersionResource.cmake)
denpachan_version_resource(denpachan-engine "denpachan engine")
denpachan_version_resource(denpachan-worker "denpachan receiver (worker)")
denpachan_version_resource(denpachan-rtmpd "denpachan RTMP server")
denpachan_version_resource(denpachan-ctl "denpachan control tool")
denpachan_version_resource(denpachan-vcam "denpachan virtual camera")
if(DENPACHAN_ASIO)
    denpachan_version_resource(denpachan-asio "denpachan ASIO output" "GPL-3.0")
endif()

include(cmake/ThirdPartyNotices.cmake)
denpachan_copy_licenses(libsrt openssl spdlog fmt nlohmann-json)
