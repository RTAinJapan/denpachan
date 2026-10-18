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

#include "config.h"
#include "device_output.h"

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
        std::unique_ptr<DeviceOutput> output;
    };
    std::map<std::wstring, Device> devices_;  // デバイス ID ごと
    std::map<int, std::string> missing_;      // チャンネル → 見つからないデバイス名
};

}  // namespace denpachan::engine
