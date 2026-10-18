# 開発用ツール
find_package(nlohmann_json CONFIG REQUIRED)

add_executable(audio_probe
    audio_probe.cpp
    ../engine/audio_devices.cpp
)
target_link_libraries(audio_probe PRIVATE denpachan_common ole32)

add_executable(denpachan-ctl ctl.cpp)
target_link_libraries(denpachan-ctl PRIVATE nlohmann_json::nlohmann_json)
