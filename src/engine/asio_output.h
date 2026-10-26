#pragma once
// ASIO の出力デバイス (Dante Virtual Soundcard の ASIO モードなど)。設計書 §6.4。
//
// ドライバの扱い (列挙、読み込み、コールバック、サンプル形式の変換、開き直し) は denpachan-asio-driver.dll
// (別のリポジトリ denpachan-asio-driver、GPL-3.0) が行い、engine はその C の関数 (asio_plugin.h) を使う。
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
    void update_routes(std::vector<RouteOptions> routes) override;

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
    std::vector<RouteOptions> routes_;  // 開いたときに使う振り分け (sources_mutex_ で保護。set_mute、update_routes で変わる)

    denpachan_asio_output* output_ = nullptr;
    denpachan_asio_callbacks callbacks_{};  // open に渡すコールバックの表 (output_ が開いている間は残す)
    std::atomic<bool> stopping_{false};
    int64_t base_delay_qpc_ = 0;
    std::atomic<int64_t> last_render_qpc_{0};  // 最後にドライバから呼ばれた時刻 (止まったのを状態に出す)

    std::mutex sources_mutex_;
    std::vector<std::unique_ptr<ChannelSource>> sources_;
    // sources_ を作ったときのドライバの形式 (sources_mutex_ で保護。update_routes が同じ形式で作る)。開くたびに opened_ が増える
    uint32_t source_rate_ = 0;
    int source_channels_ = 0;
    uint64_t opened_ = 0;
    std::mutex update_mutex_;  // update_routes を 1 つずつ

    std::mutex status_mutex_;
    DeviceStatus status_;
};

}  // namespace denpachan::engine
