# DirectShow 仮想カメラ (OBS などのプロセス内にロードされる)
add_library(denpachan-vcam SHARED
    dllmain.cpp
    filter.cpp
    formats.cpp
    frame_source.cpp
    convert.cpp
    vcam.def
)
target_link_libraries(denpachan-vcam PRIVATE denpachan_common strmiids ole32 oleaut32 avrt)

# ホストプロセスの CRT に依存しないよう静的 CRT にする
set_property(TARGET denpachan-vcam PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
