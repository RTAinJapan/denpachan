#pragma once
// 音声デバイス (WASAPI エンドポイント) の列挙と検索。

#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <optional>
#include <string>
#include <vector>

namespace denpachan::engine {

struct AudioDeviceInfo {
    std::wstring id;
    std::wstring name;
    int channels = 0;      // 共有モードのミックス形式
    int sample_rate = 0;
};

// 有効な出力 (eRender) / 入力 (eCapture) デバイス
std::vector<AudioDeviceInfo> list_audio_devices(EDataFlow flow);

// ID の完全一致、なければ名前の部分一致 (大文字小文字を区別しない) で探す
std::optional<AudioDeviceInfo> find_audio_device(EDataFlow flow, const std::wstring& query);

Microsoft::WRL::ComPtr<IMMDevice> open_audio_device(const std::wstring& id);

std::string to_utf8(const std::wstring& s);
std::wstring from_utf8(const std::string& s);

}  // namespace denpachan::engine
