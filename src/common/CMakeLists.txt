# ヘッダオンリー。vcam DLL (静的 CRT) と worker (動的 CRT) の両方から使う。
add_library(denpachan_common INTERFACE)
target_include_directories(denpachan_common INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(denpachan_common INTERFACE advapi32)

# exe のアイコン。リンクした exe に .rc が加わる
set(DENPACHAN_ICON "${CMAKE_SOURCE_DIR}/assets/denpachan.ico")
configure_file(icon.rc.in "${CMAKE_CURRENT_BINARY_DIR}/icon.rc" @ONLY)
add_library(denpachan_icon INTERFACE)
target_sources(denpachan_icon INTERFACE "${CMAKE_CURRENT_BINARY_DIR}/icon.rc")
