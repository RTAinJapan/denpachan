#pragma once
// 1 つの出力デバイスに、ルーティングされた複数チャンネルの音声をまとめて書く出力の共通部分。設計書 §6。
// WASAPI (device_output.h) と ASIO (asio_output.h) がある。どちらもチャンネルごとの音は ChannelSource が作る
// (遅延を一定に保つ、クロックずれの補正、フェード、ミュートは共通)。

#include <cstdint>
#include <functional>
#include <string>

#include "channel_source.h"

namespace denpachan::engine {

struct DeviceOptions {
    std::wstring id;    // WASAPI: エンドポイント ID、ASIO: "asio:{CLSID}"
    std::wstring name;  // 表示名 (ASIO は "ASIO: ドライバ名")
    bool asio = false;
    bool exclusive = false;  // WASAPI のみ
    int margin_ms = 10;      // デバイスのバッファに足す音声の余裕 (到着の揺れの吸収)
};

struct DeviceStatus {
    bool open = false;
    bool exclusive = false;
    int sample_rate = 0;
    int channels = 0;
    std::string sample_format;
    double buffer_ms = 0;
    double delay_ms = 0;  // 鳴らすべき時刻からの基本遅延 (バッファ + 余裕)
    uint64_t reopens = 0;
};

class AudioOutput {
public:
    virtual ~AudioOutput() = default;

    virtual void start() = 0;
    virtual void stop() = 0;

    virtual DeviceStatus status() = 0;
    virtual const DeviceOptions& options() const = 0;

    // ミュートの切り替え (デバイスを開き直しても保たれる)
    virtual void set_mute(int ch, bool mute) = 0;

    // 各チャンネルの ChannelSource に f を適用する (デバイスを開いたことがなければ何もしない)
    virtual void for_each_source(const std::function<void(ChannelSource&)>& f) = 0;
};

}  // namespace denpachan::engine
