# FFmpeg は同梱しない (docs/dependencies.md)。ビルドにはヘッダと import lib だけを使い、
# 実行時はユーザーが用意した FFmpeg の shared 版の DLL を読み込む。
#
# - denpachan_ffmpeg_dll(<var> <lib>): libavcodec などの DLL 名 (例: avcodec-63.dll) をヘッダの版から求める
# - denpachan_delayload_ffmpeg(<target> <lib>...): DLL を遅延ロードにする。
#   FFmpeg が無くても起動でき、使う前に ffmpeg_runtime.h で有無と版を確かめる
# - denpachan_strip_ffmpeg_dlls(<target>...): 出力フォルダに FFmpeg の DLL があれば消す (配布物に紛れ込まないように。
#   以前 vcpkg の FFmpeg を使っていたときのビルド出力にも残っている)。
#   開発中に試すときは FetchFfmpeg.cmake が取ってきた FFmpeg の bin を PATH に足す (tools/testsrc/dev_env.sh)

function(denpachan_ffmpeg_dll out lib)
    # libavutil だけ版の定義が version.h にある
    set(candidates "${lib}/version_major.h" "${lib}/version.h")
    string(TOUPPER "${lib}" upper)
    foreach(header IN LISTS candidates)
        find_file(_dp_header NAMES "${header}" PATHS ${FFMPEG_INCLUDE_DIRS} NO_DEFAULT_PATH NO_CACHE)
        if(_dp_header)
            file(STRINGS "${_dp_header}" line REGEX "^#define ${upper}_VERSION_MAJOR +[0-9]+")
            if(line)
                string(REGEX REPLACE ".* ([0-9]+).*" "\\1" major "${line}")
                string(REGEX REPLACE "^lib" "" base "${lib}")
                set(${out} "${base}-${major}.dll" PARENT_SCOPE)
                return()
            endif()
        endif()
        unset(_dp_header)
    endforeach()
    message(FATAL_ERROR "cannot find the version of ${lib} in ${FFMPEG_INCLUDE_DIRS}")
endfunction()

function(denpachan_delayload_ffmpeg target)
    foreach(lib IN LISTS ARGN)
        denpachan_ffmpeg_dll(dll ${lib})
        target_link_options(${target} PRIVATE "/DELAYLOAD:${dll}")
    endforeach()
    target_link_libraries(${target} PRIVATE delayimp)
endfunction()

function(denpachan_strip_ffmpeg_dlls)
    set(names libvpl.dll)  # 以前の vcpkg の FFmpeg (QSV) が使っていたもの
    foreach(lib libavcodec libavformat libavutil libswresample libswscale)
        denpachan_ffmpeg_dll(dll ${lib})
        list(APPEND names ${dll})
    endforeach()
    set(files "")
    foreach(name IN LISTS names)
        list(APPEND files "${CMAKE_BINARY_DIR}/bin/$<CONFIG>/${name}")
    endforeach()
    add_custom_target(denpachan_strip_ffmpeg ALL
        COMMAND ${CMAKE_COMMAND} -E rm -f ${files}
        DEPENDS ${ARGN}
        COMMENT "Removing FFmpeg DLLs from the output (FFmpeg is not bundled)"
        VERBATIM)
endfunction()
