#pragma once
// 設定から音声出力 (DeviceOutput) を組み立てる。設計書 §6。
//
// - 設定が変わったら、ルーティングの変わったデバイスだけを開き直す (他のデバイスの音は止めない)。
// - ミュートは開き直さずに切り替える。
// - 見つからないデバイス (DVS がまだ起動していない等) は定期的に探し直す。

#include <nlohmann/json.hpp>

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "audio_output.h"
#include "config.h"

namespace denpachan::engine {

class AudioRouter {
public:
    AudioRouter() = default;
    ~AudioRouter();

    // 有効なチャンネルのルーティングを反映する
    void apply(const Config& config);
    void set_mute(int ch, bool mute);

    // チャンネルの出力状態 (デバイス名、開いているか、遅延など)
    nlohmann::json channel_status(int ch);
    nlohmann::json devices_status();

    // 見つからなかったデバイスがあるか (あれば定期的に apply し直す)
    bool has_missing() const { return !missing_.empty(); }

private:
    struct Device {
        DeviceOptions options;
        std::vector<RouteOptions> routes;
        std::unique_ptr<AudioOutput> output;  // WASAPI (DeviceOutput) または ASIO (AsioOutput)
    };
    std::map<std::wstring, Device> devices_;       // デバイス ID ごと
    std::map<int, std::string> missing_;           // チャンネル → 見つからないデバイス名
    std::map<int, std::string> asio_busy_;         // チャンネル → 使えない ASIO ドライバ (別の ASIO ドライバを使っている)
    std::map<int, std::string> logged_missing_;    // ログに出した missing_ (変わったときだけ出す)
    std::map<int, std::string> logged_asio_busy_;  // ログに出した asio_busy_ (変わったときだけ出す)
};

}  // namespace denpachan::engine
