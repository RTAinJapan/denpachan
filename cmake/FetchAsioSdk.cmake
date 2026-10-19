# ASIO 出力に使う Steinberg の ASIO SDK (docs/dependencies.md §3)。
#
# SDK は GPL-3.0 (または Steinberg のライセンス契約) で配られている。denpachan-asio.dll (src/asio/) が GPL-3.0 で使う
# (本体は MIT のまま。DENPACHAN_ASIO=OFF なら取らない)。
# Steinberg の配布物を固定の URL と SHA256 で取り、ヘッダ (common/asio.h、common/iasiodrv.h など) だけを使う。
# ドライバの列挙と読み込みは DLL が自分で行うので、SDK の host/ のソースは使わない (src/asio/asio_output.cpp)。
#
# - DENPACHAN_ASIOSDK_DIR を指定すれば、その (展開済みの) フォルダを使う (取得しない)
# - 取得したものは out/deps に置く (FFmpeg と同じ。CI ではこのフォルダをキャッシュする)
#
# 結果: ASIOSDK_INCLUDE_DIRS / ASIOSDK_LICENSE

set(DENPACHAN_ASIOSDK_VERSION "2.3.4")
set(DENPACHAN_ASIOSDK_URL "https://download.steinberg.net/sdk_downloads/ASIO-SDK_${DENPACHAN_ASIOSDK_VERSION}_2025-10-15.zip")
set(DENPACHAN_ASIOSDK_SHA256 "d5ebf0c20dd2c5f43771fd0c1418f4b361bf52434ee670097cfa6b3a335e2eca")
set(DENPACHAN_ASIOSDK_DIR "" CACHE PATH "Extracted ASIO SDK (common/, host/). Empty = download")

if(NOT DENPACHAN_ASIOSDK_DIR)
    set(deps "${CMAKE_SOURCE_DIR}/out/deps")
    set(dir "${deps}/asiosdk-${DENPACHAN_ASIOSDK_VERSION}")
    if(NOT EXISTS "${dir}/ASIOSDK/common/asio.h")
        set(archive "${deps}/download/ASIO-SDK_${DENPACHAN_ASIOSDK_VERSION}.zip")
        message(STATUS "Downloading the ASIO SDK ${DENPACHAN_ASIOSDK_VERSION}")
        file(DOWNLOAD "${DENPACHAN_ASIOSDK_URL}" "${archive}" EXPECTED_HASH SHA256=${DENPACHAN_ASIOSDK_SHA256} STATUS status)
        list(GET status 0 code)
        if(NOT code EQUAL 0)
            message(FATAL_ERROR "cannot download ${DENPACHAN_ASIOSDK_URL}: ${status}")
        endif()
        file(ARCHIVE_EXTRACT INPUT "${archive}" DESTINATION "${dir}")
    endif()
    set(DENPACHAN_ASIOSDK_DIR "${dir}/ASIOSDK")
endif()

if(NOT EXISTS "${DENPACHAN_ASIOSDK_DIR}/common/asio.h")
    message(FATAL_ERROR "ASIO SDK not found: ${DENPACHAN_ASIOSDK_DIR}/common/asio.h")
endif()
set(ASIOSDK_INCLUDE_DIRS "${DENPACHAN_ASIOSDK_DIR}/common" "${DENPACHAN_ASIOSDK_DIR}/host" "${DENPACHAN_ASIOSDK_DIR}/host/pc")
set(ASIOSDK_LICENSE "${DENPACHAN_ASIOSDK_DIR}/common/LICENSE.txt")
message(STATUS "ASIO SDK: ${DENPACHAN_ASIOSDK_DIR}")
