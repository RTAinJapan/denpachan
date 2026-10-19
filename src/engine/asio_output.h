#pragma once
// ASIO の出力デバイス (Dante Virtual Soundcard の ASIO モードなど)。設計書 §6.4。
//
// ドライバの扱い (列挙、読み込み、コールバック、サンプル形式の変換、開き直し) は denpachan-asio.dll (GPL-3.0、src/asio/)
// が行い、engine はその C の関数 (asio_plugin.h) を使う。
// WASAPI と同じく、チャンネルごとの音は ChannelSource が作る (遅延を一定に保つ・クロックずれの補正・フェード・ミュート)。
// DLL が開いたら (opened) ChannelSource を作り、ドライバのコールバックごとに (render) ミックスに足し込む。

#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

#include "asio_plugin.h"
#include "audio_output.h"

namespace denpachan::engine {

class AsioOutput : public AudioOutput {
public:
    AsioOutput(DeviceOptions options, std::vector<RouteOptions> routes);
    ~AsioOutput() override;

    void start() override;
    void stop() override;

    DeviceStatus status() override;
    const DeviceOptions& options() const override { return options_; }

    void set_mute(int ch, bool mute) override;

    void for_each_source(const std::function<void(ChannelSource&)>& f) override {
        std::lock_guard lk(sources_mutex_);
        for (auto& s : sources_) f(*s);
    }

private:
    // DLL からのコールバック
    void on_opened(const denpachan_asio_format& format);
    void on_closed();
    void on_render(float* mix, int frames, int64_t first_hear_qpc);
    void on_log(int level, const char* message);

    const DeviceOptions options_;
    std::vector<RouteOptions> routes_;  // mute だけ変わる (sources_mutex_ で保護)

    denpachan_asio_output* output_ = nullptr;
    std::atomic<bool> stopping_{false};
    int64_t base_delay_qpc_ = 0;

    std::mutex sources_mutex_;
    std::vector<std::unique_ptr<ChannelSource>> sources_;

    std::mutex status_mutex_;
    DeviceStatus status_;
};

}  // namespace denpachan::engine
