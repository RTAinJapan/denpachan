// COM サーバとしての入口と、DirectShow のビデオ入力デバイスとしての登録。
//
//   regsvr32 denpachan-vcam.dll                 マシン全体に登録 (管理者権限が必要)
//   regsvr32 /u denpachan-vcam.dll              その解除
//   regsvr32 /n /i:user denpachan-vcam.dll      現在のユーザーだけに登録 (管理者権限不要)
//   regsvr32 /u /n /i:user denpachan-vcam.dll   その解除
//
// 管理者権限で動くアプリは COM のユーザー単位登録を見ないので、本番はマシン全体に登録する。

#include <dshow.h>
#include <olectl.h>

#include <new>
#include <string>

#include "filter.h"
#include "guids.h"
#include "module.h"

using namespace denpachan::vcam;

namespace {

constexpr wchar_t kVideoInputCategory[] = L"{860BB310-5D01-11d0-BD3B-00A0C911CE86}";

class ClassFactory final : public IClassFactory {
public:
    explicit ClassFactory(int cam) : cam_(cam) {}

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IClassFactory) {
            *ppv = static_cast<IClassFactory*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++ref_; }
    STDMETHODIMP_(ULONG) Release() override {
        const ULONG r = --ref_;
        if (r == 0) delete this;
        return r;
    }

    STDMETHODIMP CreateInstance(IUnknown* pUnkOuter, REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (pUnkOuter) return CLASS_E_NOAGGREGATION;
        return VcamFilter::Create(cam_, riid, ppv);
    }
    STDMETHODIMP LockServer(BOOL fLock) override {
        if (fLock)
            ++g_module_refs;
        else
            --g_module_refs;
        return S_OK;
    }

private:
    ~ClassFactory() = default;

    ModuleRef module_ref_;
    std::atomic<ULONG> ref_{1};
    const int cam_;
};

std::wstring guid_string(const GUID& g) {
    wchar_t buf[64] = {};
    StringFromGUID2(g, buf, 64);
    return buf;
}

std::wstring module_path() {
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(g_module, path.data(), DWORD(path.size()));
        if (n == 0) return {};
        if (n < path.size()) {
            path.resize(n);
            return path;
        }
        path.resize(path.size() * 2);
    }
}

HRESULT set_string(HKEY root, const std::wstring& key, const wchar_t* name, const std::wstring& value) {
    const LSTATUS st = RegSetKeyValueW(root, key.c_str(), name, REG_SZ, value.c_str(),
                                      DWORD((value.size() + 1) * sizeof(wchar_t)));
    return HRESULT_FROM_WIN32(st);
}

std::wstring clsid_key(int cam) { return L"Software\\Classes\\CLSID\\" + guid_string(kCamClsid[cam - 1]); }

std::wstring instance_key(int cam) {
    return std::wstring(L"Software\\Classes\\CLSID\\") + kVideoInputCategory + L"\\Instance\\" +
           guid_string(kCamClsid[cam - 1]);
}

std::wstring device_path(int cam) { return L"denpachan:vcam" + std::to_wstring(cam); }

HRESULT register_com_server(HKEY root, int cam) {
    const auto key = clsid_key(cam);
    HRESULT hr = set_string(root, key, nullptr, cam_name(cam));
    if (SUCCEEDED(hr)) hr = set_string(root, key + L"\\InprocServer32", nullptr, module_path());
    if (SUCCEEDED(hr)) hr = set_string(root, key + L"\\InprocServer32", L"ThreadingModel", L"Both");
    return hr;
}

// マシン全体: IFilterMapper2 で登録する (FilterData も正しく書かれる)
HRESULT register_machine(int cam) {
    HRESULT hr = register_com_server(HKEY_LOCAL_MACHINE, cam);
    if (FAILED(hr)) return hr;

    IFilterMapper2* mapper = nullptr;
    hr = CoCreateInstance(CLSID_FilterMapper2, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&mapper));
    if (FAILED(hr)) return hr;

    REGPINTYPES types[] = {{&MEDIATYPE_Video, &MEDIASUBTYPE_NV12}, {&MEDIATYPE_Video, &MEDIASUBTYPE_YUY2}};
    REGFILTERPINS2 pin{};
    pin.dwFlags = REG_PINFLAG_B_OUTPUT;
    pin.cInstances = 1;
    pin.nMediaTypes = ARRAYSIZE(types);
    pin.lpMediaType = types;
    pin.clsPinCategory = &PIN_CATEGORY_CAPTURE;
    REGFILTER2 rf{};
    rf.dwVersion = 2;
    rf.dwMerit = MERIT_DO_NOT_USE;
    rf.cPins2 = 1;
    rf.rgPins2 = &pin;

    const auto name = cam_name(cam);
    hr = mapper->RegisterFilter(kCamClsid[cam - 1], name.c_str(), nullptr, &CLSID_VideoInputDeviceCategory,
                                nullptr, &rf);
    mapper->Release();
    if (FAILED(hr)) return hr;

    // OBS はデバイスを DevicePath で識別するので固定値を入れておく
    return set_string(HKEY_LOCAL_MACHINE, instance_key(cam), L"DevicePath", device_path(cam));
}

HRESULT unregister_machine(int cam) {
    IFilterMapper2* mapper = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FilterMapper2, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&mapper)))) {
        mapper->UnregisterFilter(&CLSID_VideoInputDeviceCategory, nullptr, kCamClsid[cam - 1]);
        mapper->Release();
    }
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, instance_key(cam).c_str());
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, clsid_key(cam).c_str());
    return S_OK;
}

// ユーザー単位: レジストリを直接書く (IFilterMapper2 は書き先を選べないため)
HRESULT register_user(int cam) {
    HRESULT hr = register_com_server(HKEY_CURRENT_USER, cam);
    const auto key = instance_key(cam);
    if (SUCCEEDED(hr)) hr = set_string(HKEY_CURRENT_USER, key, L"CLSID", guid_string(kCamClsid[cam - 1]));
    if (SUCCEEDED(hr)) hr = set_string(HKEY_CURRENT_USER, key, L"FriendlyName", cam_name(cam));
    if (SUCCEEDED(hr)) hr = set_string(HKEY_CURRENT_USER, key, L"DevicePath", device_path(cam));
    return hr;
}

HRESULT unregister_user(int cam) {
    RegDeleteTreeW(HKEY_CURRENT_USER, instance_key(cam).c_str());
    RegDeleteTreeW(HKEY_CURRENT_USER, clsid_key(cam).c_str());
    return S_OK;
}

HRESULT for_all_cams(bool user, bool install) {
    const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    HRESULT result = S_OK;
    for (int cam = 1; cam <= kNumCams; ++cam) {
        HRESULT hr;
        if (install)
            hr = user ? register_user(cam) : register_machine(cam);
        else
            hr = user ? unregister_user(cam) : unregister_machine(cam);
        if (FAILED(hr) && SUCCEEDED(result)) result = hr;
    }
    if (SUCCEEDED(co)) CoUninitialize();
    if (FAILED(result) && install) for_all_cams(user, false);  // 中途半端な登録を残さない
    return result;
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = module;
        DisableThreadLibraryCalls(module);
    }
    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID* ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    for (int cam = 1; cam <= kNumCams; ++cam) {
        if (rclsid != kCamClsid[cam - 1]) continue;
        auto* factory = new (std::nothrow) ClassFactory(cam);
        if (!factory) return E_OUTOFMEMORY;
        const HRESULT hr = factory->QueryInterface(riid, ppv);
        factory->Release();
        return hr;
    }
    return CLASS_E_CLASSNOTAVAILABLE;
}

STDAPI DllCanUnloadNow() { return g_module_refs.load() == 0 ? S_OK : S_FALSE; }

STDAPI DllRegisterServer() { return for_all_cams(false, true); }

STDAPI DllUnregisterServer() { return for_all_cams(false, false); }

STDAPI DllInstall(BOOL bInstall, LPCWSTR pszCmdLine) {
    const bool user = pszCmdLine && _wcsicmp(pszCmdLine, L"user") == 0;
    return for_all_cams(user, bInstall != FALSE);
}
