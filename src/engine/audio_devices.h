#pragma once
// 音声デバイス (WASAPI エンドポイント) の列挙と検索。

#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <optional>
#include <string>
#include <vector>

namespace denpachan::engine {

struct AudioDeviceInfo {
    std::wstring id;    // WASAPI: エンドポイント ID、ASIO: "asio:{CLSID}"
    std::wstring name;  // ASIO は "ASIO: ドライバ名"
    int channels = 0;   // WASAPI: 共有モードのミックス形式、ASIO: 出力チャンネル数 (分からなければ 0)
    int sample_rate = 0;
    bool asio = false;
};

// 有効な出力 (eRender) / 入力 (eCapture) デバイス (WASAPI)
std::vector<AudioDeviceInfo> list_audio_devices(EDataFlow flow);

// ID の完全一致、なければ名前の部分一致 (大文字小文字を区別しない) で探す (WASAPI)
std::optional<AudioDeviceInfo> find_audio_device(EDataFlow flow, const std::wstring& query);

// 音声の出力先 (WASAPI と ASIO を合わせたもの) は output_devices.h

Microsoft::WRL::ComPtr<IMMDevice> open_audio_device(const std::wstring& id);

std::string to_utf8(const std::wstring& s);
std::wstring from_utf8(const std::string& s);

}  // namespace denpachan::engine
