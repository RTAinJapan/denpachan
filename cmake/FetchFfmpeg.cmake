# ビルドに使う FFmpeg のヘッダと import lib (docs/dependencies.md §3)。
#
# FFmpeg は同梱しない。利用者が使うのと同じ形の配布物 (gyan.dev の FFmpeg 9.0.2 shared 版) を固定の URL と SHA256 で
# 取得し、include/ と lib/ をビルドに、bin/ の DLL を開発中の試験に使う (配布物には入れない)。
#
# - DENPACHAN_FFMPEG_DIR を指定すれば、その (展開済みの) フォルダを使う (取得しない)
# - 取得したものは out/deps に置く (ビルドフォルダを消しても取り直さない。CI ではこのフォルダをキャッシュする)
#
# 結果: FFMPEG_INCLUDE_DIRS / FFMPEG_LIBRARY_DIRS / FFMPEG_LIBRARIES / FFMPEG_BIN_DIR

set(DENPACHAN_FFMPEG_VERSION "9.0.2")
set(DENPACHAN_FFMPEG_NAME "ffmpeg-${DENPACHAN_FFMPEG_VERSION}-full_build-shared")
set(DENPACHAN_FFMPEG_URL
    "https://github.com/GyanD/codexffmpeg/releases/download/${DENPACHAN_FFMPEG_VERSION}/${DENPACHAN_FFMPEG_NAME}.zip")
set(DENPACHAN_FFMPEG_SHA256 "8d31e162f1616e37aab3fa2db991b97e1b8dbeb1c8465fd81a81be16ff91b328")
set(DENPACHAN_FFMPEG_DIR "" CACHE PATH "Extracted FFmpeg shared package (include/, lib/, bin/). Empty = download")

if(NOT DENPACHAN_FFMPEG_DIR)
    set(deps "${CMAKE_SOURCE_DIR}/out/deps")
    set(dir "${deps}/${DENPACHAN_FFMPEG_NAME}")
    if(NOT EXISTS "${dir}/include/libavcodec/avcodec.h")
        set(archive "${deps}/download/${DENPACHAN_FFMPEG_NAME}.zip")
        message(STATUS "Downloading FFmpeg ${DENPACHAN_FFMPEG_VERSION} (headers and import libs for building)")
        file(DOWNLOAD "${DENPACHAN_FFMPEG_URL}" "${archive}" EXPECTED_HASH SHA256=${DENPACHAN_FFMPEG_SHA256} STATUS status)
        list(GET status 0 code)
        if(NOT code EQUAL 0)
            message(FATAL_ERROR "cannot download ${DENPACHAN_FFMPEG_URL}: ${status}")
        endif()
        file(ARCHIVE_EXTRACT INPUT "${archive}" DESTINATION "${deps}")
    endif()
    set(DENPACHAN_FFMPEG_DIR "${dir}")
endif()

set(FFMPEG_INCLUDE_DIRS "${DENPACHAN_FFMPEG_DIR}/include")
set(FFMPEG_BIN_DIR "${DENPACHAN_FFMPEG_DIR}/bin")

# import lib は配布物の .def から MSVC の lib.exe で作り直す。配布物の .lib は MinGW のツールで作られていて、
# MSVC の /DELAYLOAD が効かない (通常の読み込みになり、FFmpeg が無いと起動できなくなる)。
set(FFMPEG_LIBRARY_DIRS "${CMAKE_BINARY_DIR}/ffmpeg-implib")
file(MAKE_DIRECTORY "${FFMPEG_LIBRARY_DIRS}")
set(FFMPEG_LIBRARIES "")
foreach(lib avcodec avformat avutil swresample swscale)
    file(GLOB def "${DENPACHAN_FFMPEG_DIR}/lib/${lib}-*.def")
    list(LENGTH def count)
    if(NOT count EQUAL 1)
        message(FATAL_ERROR "${DENPACHAN_FFMPEG_DIR}/lib/${lib}-*.def not found (DENPACHAN_FFMPEG_DIR=${DENPACHAN_FFMPEG_DIR})")
    endif()
    get_filename_component(dll_base "${def}" NAME_WE)  # avcodec-63
    set(implib "${FFMPEG_LIBRARY_DIRS}/${lib}.lib")
    if(NOT EXISTS "${implib}" OR "${def}" IS_NEWER_THAN "${implib}")
        execute_process(
            COMMAND "${CMAKE_AR}" /nologo "/def:${def}" "/name:${dll_base}.dll" /machine:x64 "/out:${implib}"
            RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE output)
        if(NOT result EQUAL 0)
            message(FATAL_ERROR "cannot create ${implib}: ${output}")
        endif()
    endif()
    list(APPEND FFMPEG_LIBRARIES "${implib}")
endforeach()
message(STATUS "FFmpeg (build only, not bundled): ${DENPACHAN_FFMPEG_DIR}")
