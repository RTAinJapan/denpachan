# ヘッダオンリー。vcam DLL (静的 CRT) と worker (動的 CRT) の両方から使う。
add_library(denpachan_common INTERFACE)
target_include_directories(denpachan_common INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(denpachan_common INTERFACE advapi32)
