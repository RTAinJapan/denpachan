#pragma once
// 1 つの出力デバイス (WASAPI) に、ルーティングされた複数チャンネルの音声をまとめて書く。設計書 §6。
//
// - 共有モード: IAudioClient3 で最小周期を使えれば使う (低遅延)。だめなら既定周期。
// - 排他モード: Windows の音声エンジンを通らない (通知音が混ざらない、音量スライダーの影響なし)。
//   その間は他のアプリがこのデバイスを使えない。デバイスからの呼び出しが周期に追いつかなければ
//   周期を延ばし、それでもだめなら共有モードに切り替える。
// - デバイスが消えたら (DVS の再起動など) 1 秒ごとに開き直す。その間も他のデバイスは止まらない。
// - バッファがいつ鳴るかは、再生位置 (IAudioClock::GetPosition) から求める (設計書 §6.3)。

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "audio_output.h"
#include "channel_source.h"
#include "denpachan/win_util.h"

namespace denpachan::engine {

class DeviceOutput : public AudioOutput {
public:
    DeviceOutput(DeviceOptions options, std::vector<RouteOptions> routes);
    ~DeviceOutput() override;

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
    enum class SampleType { Float32, Int16, Int24, Int24In32, Int32 };

    void run();
    bool open();
    void close();
    bool render();
    void write_samples(BYTE* dst, int frames);

    const DeviceOptions options_;
    std::vector<RouteOptions> routes_;  // mute だけ変わる (sources_mutex_ で保護)

    Microsoft::WRL::ComPtr<IAudioClient> client_;
    Microsoft::WRL::ComPtr<IAudioRenderClient> render_;
    UniqueHandle event_;
    int open_failures_ = 0;                // 続けて開けなかった回数 (ログを毎秒出さないため)
    bool exclusive_ = false;               // いま排他モードで開いているか
    bool exclusive_failed_ = false;        // 排他モードの周期に追いつけなかった (以後は共有モード)
    REFERENCE_TIME exclusive_period_ = 0;  // 0 = デバイスの最小周期
    SampleType sample_type_ = SampleType::Float32;
    int channels_ = 0;
    uint32_t rate_ = 0;
    uint32_t buffer_frames_ = 0;
    int64_t base_delay_qpc_ = 0;
    std::vector<float> mix_;

    // 鳴る時刻の測り方 (render)。GetCurrentPadding (「今 + 溜まっている量」) は仮想デバイスなどで数 ms 揺れるので、
    // IAudioClock::GetPosition (再生位置とその時刻) から求める。平均の時刻 (遅延) は今までと変えないよう、
    // 開いた直後に padding との差を測っておいて差し引く。
    int64_t heard_time_locked(int64_t now, uint32_t lead_frames);
    Microsoft::WRL::ComPtr<IAudioClock> clock_;
    uint64_t clock_freq_ = 0;
    uint64_t written_frames_ = 0;      // デバイスのバッファに書いたフレーム数 (開いてから)
    int64_t position_offset_qpc_ = 0;  // GetPosition による時刻 - padding による時刻 (測り終えたら固定)
    bool position_ready_ = false;      // 差を測り終えた
    int64_t calibrate_until_qpc_ = 0;
    std::vector<int64_t> calibrate_samples_;

    // 診断: デバイスからの呼び出し回数と最大間隔
    int64_t last_render_qpc_ = 0;
    std::atomic<uint64_t> render_count_{0};
    std::atomic<int64_t> max_gap_us_{0};

public:
    uint64_t take_render_count() { return render_count_.exchange(0); }
    int64_t take_max_gap_us() { return max_gap_us_.exchange(0); }

private:
    std::mutex sources_mutex_;
    std::vector<std::unique_ptr<ChannelSource>> sources_;

    std::mutex status_mutex_;
    DeviceStatus status_;

    std::thread thread_;
    std::atomic<bool> stop_{false};
    UniqueHandle stop_event_;
};

}  // namespace denpachan::engine
