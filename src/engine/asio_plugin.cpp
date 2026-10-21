#include "asio_plugin.h"

#include <windows.h>
#include <spdlog/spdlog.h>

#include <array>
#include <mutex>

#include "audio_devices.h"  // to_utf8

namespace denpachan::engine {

namespace {

constexpr wchar_t kDllName[] = L"denpachan-asio-driver.dll";

// ドライバを触る呼び出し (probe / open / close) を順にする (asio_plugin.h の open_asio_output)
std::mutex driver_mutex;

std::wstring exe_dir() {
    wchar_t buf[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, buf, DWORD(std::size(buf)));
    std::wstring path(buf, n);
    const auto slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : path.substr(0, slash);
}

AsioPlugin load() {
    AsioPlugin plugin;
    const std::wstring path = exe_dir() + L"\\" + kDllName;
    // 実行ファイルの横のものだけを読み込む (フルパス + 検索しない)
    HMODULE module = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!module) {
        spdlog::info("asio: {} not found next to the engine; ASIO output is not available", to_utf8(kDllName));
        return plugin;
    }
    const auto abi = reinterpret_cast<decltype(&denpachan_asio_abi_version)>(GetProcAddress(module, "denpachan_asio_abi_version"));
    const int version = abi ? abi() : 0;
    if (version != DENPACHAN_ASIO_ABI_VERSION) {
        spdlog::error("asio: {} has ABI version {} (expected {}); ASIO output is not available", to_utf8(path), version,
                      DENPACHAN_ASIO_ABI_VERSION);
        FreeLibrary(module);
        return plugin;
    }
    plugin.list_drivers = reinterpret_cast<decltype(&denpachan_asio_list_drivers)>(GetProcAddress(module, "denpachan_asio_list_drivers"));
    plugin.probe_driver = reinterpret_cast<decltype(&denpachan_asio_probe_driver)>(GetProcAddress(module, "denpachan_asio_probe_driver"));
    plugin.open = reinterpret_cast<decltype(&denpachan_asio_open)>(GetProcAddress(module, "denpachan_asio_open"));
    plugin.close = reinterpret_cast<decltype(&denpachan_asio_close)>(GetProcAddress(module, "denpachan_asio_close"));
    if (!plugin.list_drivers || !plugin.probe_driver || !plugin.open || !plugin.close) {
        spdlog::error("asio: {} is missing functions; ASIO output is not available", to_utf8(path));
        FreeLibrary(module);
        return AsioPlugin{};
    }
    spdlog::info("asio: loaded {}", to_utf8(path));
    return plugin;  // module はプロセスが終わるまで持ったまま
}

}  // namespace

const AsioPlugin& asio_plugin() {
    static const AsioPlugin plugin = load();
    return plugin;
}

std::vector<AsioDriverInfo> list_asio_drivers() {
    std::vector<AsioDriverInfo> result;
    const auto& plugin = asio_plugin();
    if (!plugin) return result;
    std::array<denpachan_asio_driver, 32> drivers{};
    const int count = std::min(plugin.list_drivers(drivers.data(), int(drivers.size())), int(drivers.size()));
    for (int i = 0; i < count; ++i) result.push_back({drivers[size_t(i)].name, drivers[size_t(i)].clsid});
    return result;
}

std::optional<AsioDriverProbe> probe_asio_driver(const std::wstring& clsid) {
    const auto& plugin = asio_plugin();
    if (!plugin) return std::nullopt;
    denpachan_asio_probe probe{};
    std::lock_guard lk(driver_mutex);
    if (!plugin.probe_driver(clsid.c_str(), &probe)) return std::nullopt;
    return AsioDriverProbe{probe.channels, probe.sample_rate};
}

denpachan_asio_output* open_asio_output(const std::wstring& clsid, int preferred_sample_rate, const denpachan_asio_callbacks* callbacks) {
    const auto& plugin = asio_plugin();
    if (!plugin) return nullptr;
    std::lock_guard lk(driver_mutex);
    return plugin.open(clsid.c_str(), preferred_sample_rate, callbacks);
}

void close_asio_output(denpachan_asio_output* output) {
    std::lock_guard lk(driver_mutex);
    asio_plugin().close(output);
}

}  // namespace denpachan::engine
