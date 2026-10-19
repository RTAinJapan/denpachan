#include "output_devices.h"

#include <algorithm>
#include <cwctype>

#include "asio_plugin.h"

namespace denpachan::engine {

namespace {

std::wstring lower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return wchar_t(std::towlower(c)); });
    return s;
}

AudioDeviceInfo asio_info(const AsioDriverInfo& driver, bool probe) {
    AudioDeviceInfo info;
    info.id = L"asio:" + driver.clsid;
    info.name = kAsioNamePrefix + driver.name;
    info.asio = true;
    if (probe) {
        if (const auto p = probe_asio_driver(driver.clsid)) {
            info.channels = p->channels;
            info.sample_rate = p->sample_rate;
        }
    }
    return info;
}

}  // namespace

std::vector<AudioDeviceInfo> list_output_devices() {
    auto devices = list_audio_devices(eRender);
    for (const auto& driver : list_asio_drivers()) devices.push_back(asio_info(driver, true));
    return devices;
}

std::optional<AudioDeviceInfo> find_output_device(const std::wstring& query) {
    if (query.rfind(kAsioNamePrefix, 0) == 0 || query.rfind(L"asio:", 0) == 0) {
        // ASIO は表示名か ID の完全一致 (大文字小文字は区別しない) で探す。チャンネル数は開くときに分かるので確かめない
        const std::wstring q = lower(query);
        for (const auto& driver : list_asio_drivers()) {
            const auto info = asio_info(driver, false);
            if (lower(info.name) == q || lower(info.id) == q) return info;
        }
        return std::nullopt;
    }
    return find_audio_device(eRender, query);
}

}  // namespace denpachan::engine
