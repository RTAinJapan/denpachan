#include "output_devices.h"

#include <algorithm>
#include <cwctype>
#include <map>
#include <mutex>

#include <spdlog/spdlog.h>

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
        // 開いたことがあれば、開いたときの値 (実際に出力に使っている形式) を使う。使っている間の確かめは、開けないか、
        // 開けても同じ値になるとは限らない (16 ch のドライバで、開いた後も 17-18 以降が一覧に出た)。どちらも無ければ 0
        std::optional<AsioDriverProbe> opened;
        {
            std::lock_guard lk(g_asio_mutex);
            if (auto it = g_asio_opened.find(info.id); it != g_asio_opened.end()) opened = it->second;
        }
        const auto probed = probe_asio_driver(driver.clsid);
        const char* source = "unknown";
        if (opened && opened->channels > 0) {
            info.channels = opened->channels;
            info.sample_rate = opened->sample_rate;
            source = "opened";
        } else if (probed && probed->channels > 0) {
            info.channels = probed->channels;
            info.sample_rate = probed->sample_rate;
            source = "probed";
        }
        spdlog::debug("audio devices: {} {} ch ({}; probed {}, opened {})", to_utf8(info.name), info.channels, source,
                      probed ? std::to_string(probed->channels) : std::string("-"),
                      opened ? std::to_string(opened->channels) : std::string("-"));
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
    bool changed = false;
    {
        std::lock_guard lk(g_asio_mutex);
        auto& known = g_asio_opened[id];
        changed = known.channels != channels || known.sample_rate != sample_rate;
        known = AsioDriverProbe{channels, sample_rate};
    }
    // 画面に一覧を読み直させる (一覧は WASAPI のデバイスの増減のときにしか読み直さないので、開いただけでは画面に届かなかった)
    if (changed) note_audio_devices_changed();
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
