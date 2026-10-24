#include "output_devices.h"

#include <algorithm>
#include <cwctype>
#include <map>
#include <mutex>

#include "asio_plugin.h"

namespace denpachan::engine {

namespace {

std::wstring lower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return wchar_t(std::towlower(c)); });
    return s;
}

// 開いたことのある ASIO ドライバの形式 (note_asio_opened)
std::mutex g_asio_mutex;
std::map<std::wstring, AsioDriverProbe> g_asio_opened;

AudioDeviceInfo asio_info(const AsioDriverInfo& driver, bool probe) {
    AudioDeviceInfo info;
    info.id = L"asio:" + driver.clsid;
    info.name = kAsioNamePrefix + driver.name;
    info.asio = true;
    if (probe) {
        if (const auto p = probe_asio_driver(driver.clsid); p && p->channels > 0) {
            info.channels = p->channels;
            info.sample_rate = p->sample_rate;
        } else {
            // 確かめに開けない (使っている、別の ASIO ドライバを使っている): 前に開いたときの値
            std::lock_guard lk(g_asio_mutex);
            if (auto it = g_asio_opened.find(info.id); it != g_asio_opened.end()) {
                info.channels = it->second.channels;
                info.sample_rate = it->second.sample_rate;
            }
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

void note_asio_opened(const std::wstring& id, int channels, int sample_rate) {
    std::lock_guard lk(g_asio_mutex);
    g_asio_opened[id] = AsioDriverProbe{channels, sample_rate};
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
