#include "audio_devices.h"

#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>

#include <algorithm>
#include <atomic>
#include <cwctype>

using Microsoft::WRL::ComPtr;

namespace denpachan::engine {

std::string to_utf8(const std::wstring& s) {
    if (s.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), int(s.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring from_utf8(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring out(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), out.data(), n);
    return out;
}

namespace {

ComPtr<IMMDeviceEnumerator> enumerator() {
    ComPtr<IMMDeviceEnumerator> e;
    CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&e));
    return e;
}

std::wstring lower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return wchar_t(std::towlower(c)); });
    return s;
}

std::atomic<uint64_t> g_device_changes{0};

// デバイスの通知を受けて数える (通知は Windows のスレッドから来るので、ここでは数えるだけ)
class DeviceWatcher final : public IMMNotificationClient {
public:
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG n = --refs_;
        if (n == 0) delete this;
        return n;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMMNotificationClient)) {
            *out = static_cast<IMMNotificationClient*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { return changed(); }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return changed(); }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return changed(); }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow, ERole, LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY key) override {
        // 名前が変わったら一覧も変わる
        const bool name = key.pid == PKEY_Device_FriendlyName.pid && IsEqualGUID(key.fmtid, PKEY_Device_FriendlyName.fmtid);
        return name ? changed() : S_OK;
    }

private:
    static HRESULT changed() {
        ++g_device_changes;
        return S_OK;
    }
    std::atomic<ULONG> refs_{1};
};

ComPtr<IMMDeviceEnumerator> g_watch_enumerator;
ComPtr<DeviceWatcher> g_watcher;

}  // namespace

void start_audio_device_watch() {
    if (g_watcher) return;
    g_watch_enumerator = enumerator();
    if (!g_watch_enumerator) return;
    g_watcher.Attach(new DeviceWatcher());
    if (FAILED(g_watch_enumerator->RegisterEndpointNotificationCallback(g_watcher.Get()))) {
        g_watcher.Reset();
        g_watch_enumerator.Reset();
    }
}

void stop_audio_device_watch() {
    if (g_watcher && g_watch_enumerator) g_watch_enumerator->UnregisterEndpointNotificationCallback(g_watcher.Get());
    g_watcher.Reset();
    g_watch_enumerator.Reset();
}

uint64_t audio_device_changes() { return g_device_changes.load(); }

ComPtr<IMMDevice> open_audio_device(const std::wstring& id) {
    ComPtr<IMMDevice> dev;
    if (auto e = enumerator()) e->GetDevice(id.c_str(), &dev);
    return dev;
}

std::vector<AudioDeviceInfo> list_audio_devices(EDataFlow flow) {
    std::vector<AudioDeviceInfo> result;
    auto e = enumerator();
    if (!e) return result;
    ComPtr<IMMDeviceCollection> devices;
    if (FAILED(e->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &devices))) return result;
    UINT count = 0;
    devices->GetCount(&count);
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> dev;
        if (FAILED(devices->Item(i, &dev))) continue;
        AudioDeviceInfo info;
        LPWSTR id = nullptr;
        if (SUCCEEDED(dev->GetId(&id))) {
            info.id = id;
            CoTaskMemFree(id);
        }
        ComPtr<IPropertyStore> props;
        if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props))) {
            PROPVARIANT v;
            PropVariantInit(&v);
            if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR) info.name = v.pwszVal;
            PropVariantClear(&v);
        }
        ComPtr<IAudioClient> client;
        if (SUCCEEDED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client))) {
            WAVEFORMATEX* mix = nullptr;
            if (SUCCEEDED(client->GetMixFormat(&mix))) {
                info.channels = mix->nChannels;
                info.sample_rate = int(mix->nSamplesPerSec);
                CoTaskMemFree(mix);
            }
        }
        result.push_back(std::move(info));
    }
    return result;
}

std::optional<AudioDeviceInfo> find_audio_device(EDataFlow flow, const std::wstring& query) {
    const auto devices = list_audio_devices(flow);
    for (const auto& d : devices)
        if (d.id == query) return d;
    const auto q = lower(query);
    for (const auto& d : devices)
        if (lower(d.name).find(q) != std::wstring::npos) return d;
    return std::nullopt;
}

}  // namespace denpachan::engine
