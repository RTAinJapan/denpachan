# ASIO 出力 (denpachan-asio.dll)。設計書 §6.4。
# Steinberg の ASIO SDK (GPL-3.0) のヘッダを使うので、この DLL だけ GPL-3.0 (このフォルダの LICENSE)。
# engine (MIT) は denpachan_asio.h の C の関数で、実行時にこの DLL を読み込む (無ければ ASIO の出力先は出ない)。
# 本体の他のコード (src/common など) には依存しない。
add_library(denpachan-asio SHARED
    asio_output.cpp
    denpachan_asio.h
)
target_compile_definitions(denpachan-asio PRIVATE DENPACHAN_ASIO_EXPORTS)
target_include_directories(denpachan-asio SYSTEM PRIVATE ${ASIOSDK_INCLUDE_DIRS})
target_link_libraries(denpachan-asio PRIVATE ole32)
