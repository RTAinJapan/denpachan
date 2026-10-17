# 開発用ツール
add_executable(audio_probe
    audio_probe.cpp
    ../engine/audio_devices.cpp
)
target_link_libraries(audio_probe PRIVATE denpachan_common ole32)
