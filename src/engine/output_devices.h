#pragma once
// 音声の出力先の一覧と検索: WASAPI の出力デバイスと ASIO のドライバを合わせたもの。設計書 §6。

#include <optional>
#include <string>
#include <vector>

#include "audio_devices.h"

namespace denpachan::engine {

// ASIO のドライバの表示名の頭 ("ASIO: ドライバ名")。設定にはこの表示名を書く
inline constexpr wchar_t kAsioNamePrefix[] = L"ASIO: ";

// WASAPI の出力デバイスと ASIO のドライバ。ASIO のチャンネル数はドライバを開いて確かめる
// (別の ASIO ドライバを使っている間は分からないことがあり、そのときは 0)
std::vector<AudioDeviceInfo> list_output_devices();

// 出力先を設定の値 (表示名または ID) で探す。"ASIO: " で始まる名前は ASIO のドライバ
std::optional<AudioDeviceInfo> find_output_device(const std::wstring& query);

// ASIO のドライバを開いた (AsioOutput)。開いている間は確かめに開けないので、一覧のチャンネル数はこの値を使う
// (分からないと、画面の開始チャンネルの一覧が 64 ch 分になった)
void note_asio_opened(const std::wstring& id, int channels, int sample_rate);

}  // namespace denpachan::engine
