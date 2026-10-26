#pragma once
// 音声デバイス (WASAPI エンドポイント) の列挙と検索。

#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <cstdint>
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

// WASAPI のデバイスの増減・有効 / 無効 (DVS のモードの切り替え、サウンドの設定での無効化など) を数える。
// engine の状態に載せ、画面は数が変わったら一覧を読み直す。start は engine の起動時に 1 回 (MTA のスレッドで)
void start_audio_device_watch();
void stop_audio_device_watch();
uint64_t audio_device_changes();
// 出力先の一覧が変わったことにする (ASIO のドライバを開いてチャンネル数が分かったときなど。画面が一覧を読み直す)
void note_audio_devices_changed();

std::string to_utf8(const std::wstring& s);
std::wstring from_utf8(const std::string& s);

}  // namespace denpachan::engine
