#include "audio_devices.h"

#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>

#include <algorithm>
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

}  // namespace

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
