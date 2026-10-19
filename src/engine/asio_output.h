#pragma once
// ASIO の出力デバイス (Dante Virtual Soundcard の ASIO モードなど)。設計書 §6.4。
//
// - ドライバはレジストリ (HKLM\SOFTWARE\ASIO) で列挙し、COM で読み込む (SDK のヘッダだけを使い、host のソースは使わない)。
// - ASIO の作法に合わせ、同時に開く ASIO ドライバは 1 つだけ (コールバックにどの出力かを渡す引数が無いため)。
// - ドライバを開いたスレッド (STA) でメッセージを回し、ドライバのリセット要求 (設定の変更など) が来たら開き直す。
//   開けない・落ちたときは 1 秒ごとに開き直す。その間も他のデバイスは止まらない。
// - 音はデバイスのコールバック (bufferSwitch) の中で ChannelSource から作り、ドライバのサンプル形式に変換して書く。
//   WASAPI と同じく、遅延を一定に保つ・クロックずれの補正・フェード・ミュートは ChannelSource が行う。

#include <windows.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "audio_output.h"
#include "denpachan/win_util.h"

struct IASIO;

namespace denpachan::engine {

// インストールされている ASIO ドライバ (HKLM\SOFTWARE\ASIO)
struct AsioDriverInfo {
    std::wstring name;   // ドライバ名 (レジストリのキー名)
    std::wstring clsid;  // "{...}"
};
std::vector<AsioDriverInfo> list_asio_drivers();

// ドライバの出力チャンネル数とサンプルレート。開いて確かめる (結果は覚えておく)。
// 別の ASIO ドライバを開いている間は、覚えている値だけを返す (2 つ目を開かない)
struct AsioDriverProbe {
    int channels = 0;
    int sample_rate = 0;
};
std::optional<AsioDriverProbe> probe_asio_driver(const std::wstring& clsid);

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

    // ドライバのコールバックから呼ぶ
    void on_buffer_switch(long index);
    void request_reset(const char* reason);

private:
    void run();
    bool open();
    void close();
    void write_channel(int channel, void* dst, int frames) const;

    const DeviceOptions options_;
    std::vector<RouteOptions> routes_;  // mute だけ変わる (sources_mutex_ で保護)

    IASIO* asio_ = nullptr;
    bool buffers_created_ = false;
    bool output_ready_ = false;  // ドライバが outputReady に対応している
    int open_failures_ = 0;
    int channels_ = 0;
    uint32_t rate_ = 0;
    long buffer_frames_ = 0;
    long output_latency_ = 0;  // フレーム (bufferSwitch から最初のサンプルが鳴るまで)
    int64_t base_delay_qpc_ = 0;
    int64_t next_first_hear_ = 0;  // 次のバッファの先頭が鳴る時刻 (予測。0 = まだ測っていない)
    uint64_t callbacks_ = 0;       // 診断: コールバックの回数
    int64_t callbacks_since_qpc_ = 0;
    std::vector<long> sample_types_;    // チャンネルごとの ASIOSampleType
    std::vector<void*> buffers_[2];     // チャンネルごとのダブルバッファ
    std::vector<float> mix_;            // channels_ x buffer_frames_ (インターリーブ)

    std::mutex sources_mutex_;
    std::vector<std::unique_ptr<ChannelSource>> sources_;

    std::mutex status_mutex_;
    DeviceStatus status_;

    std::thread thread_;
    std::atomic<bool> stop_{false};
    UniqueHandle stop_event_;
    UniqueHandle reset_event_;
};

}  // namespace denpachan::engine
