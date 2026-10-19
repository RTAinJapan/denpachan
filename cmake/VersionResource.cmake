# exe / DLL に版の情報 (VERSIONINFO) を埋め込む。
#
# 版は project(denpachan ... VERSION X.Y.Z) だけに書く (docs/development.md の「リリース」)。
# プレリリースのラベル (rc.1 など) は configure で DENPACHAN_VERSION_LABEL を渡したときだけ、製品版の文字列に付ける
# (build.ps1 -VersionLabel。CI がタグ vX.Y.Z-rc.1 のときに渡す)。数字の版 (X.Y.Z.0) にはラベルを入れられない。
set(DENPACHAN_VERSION_LABEL "" CACHE STRING "Pre-release label added to the product version (e.g. rc.1)")

# denpachan_version_resource(target description [license]): license は LegalCopyright に付ける (既定は MIT)
function(denpachan_version_resource target description)
    set(DENPACHAN_LICENSE "MIT")
    if(ARGC GREATER 2)
        set(DENPACHAN_LICENSE "${ARGV2}")
    endif()
    get_target_property(type ${target} TYPE)
    if(type STREQUAL "EXECUTABLE")
        set(DENPACHAN_FILE_TYPE "VFT_APP")
        set(DENPACHAN_FILE_NAME "${target}.exe")
    else()
        set(DENPACHAN_FILE_TYPE "VFT_DLL")
        set(DENPACHAN_FILE_NAME "${target}.dll")
    endif()
    set(DENPACHAN_TARGET "${target}")
    set(DENPACHAN_FILE_DESCRIPTION "${description}")
    if(DENPACHAN_VERSION_LABEL)
        set(DENPACHAN_PRODUCT_VERSION "${denpachan_VERSION}-${DENPACHAN_VERSION_LABEL}")
        set(DENPACHAN_FILE_FLAGS "VS_FF_PRERELEASE")
    else()
        set(DENPACHAN_PRODUCT_VERSION "${denpachan_VERSION}")
        set(DENPACHAN_FILE_FLAGS "0")
    endif()
    set(rc "${CMAKE_BINARY_DIR}/version/${target}.rc")
    configure_file("${CMAKE_SOURCE_DIR}/cmake/version.rc.in" "${rc}" @ONLY)
    target_sources(${target} PRIVATE "${rc}")
endfunction()
