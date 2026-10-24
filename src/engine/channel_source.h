#pragma once
// 1 チャンネル分の音声を、音声リング (共有メモリ) から読んで出力デバイスのバッファに置く。
//
// 音はミキサーへの素材送りなので、データには極力手を加えない (設計書 §6.2):
// - 音量の変更はしない (トリム・ミュートなし。調整はミキサーで行う)。
// - デバイスが 48kHz ならリサンプラを通さず、サンプルをそのままコピーする。
//   48kHz 以外のデバイス (96kHz 等) のときだけリサンプルする。
//
// 遅延を一定に保つ (設計書 §3.5):
//   リングの時刻対応 (サンプル番号 ↔ 鳴らすべき時刻) から、デバイスでいま鳴る位置にあるべき
//   サンプル番号 (ideal) を求め、実際の読み位置とのずれ (1 秒程度で平滑化) を詰める。
//   - 48kHz のデバイス: ずれが 2ms を超えたら 1 サンプルだけ重複 / 間引きする (送出元とデバイスの
//     クロック差は数十 ppm なので、数秒に 1 回程度)。
//   - それ以外: リサンプル比を微調整する (最大 ±0.5%)。
//   ずれが 50ms を超えたら (接続し直しなど)、平滑化したずれが 5ms を超えたら (遅延が一度に動いたとき)、
//   または同じ向きの重複・間引きが 1 秒に 24 回を超えるのが 2 秒続いたら (時間軸がゆっくり動き続けたとき)、
//   フェードアウト → 読み位置を合わせ直す → フェードイン。
//
// ポップノイズを出さない: 開始・途絶・合わせ直しの瞬間だけ 10ms のフェードをかける (通常の音には触れない)。

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "denpachan/audio_shm.h"
#include "denpachan/win_util.h"

extern "C" {
#include <libswresample/swresample.h>
}

namespace denpachan::engine {

struct RouteOptions {
    int ch = 1;             // denpachan のチャンネル (1-8)
    int start_channel = 1;  // デバイスの出力チャンネル (1 始まり、L を置く位置)
    int offset_ms = 0;      // 音声の遅延に足す (A/V 合わせ用。負も可)
    bool fade = true;       // 開始・途絶・合わせ直し・ミュート切替の瞬間のフェード
    bool mute = false;      // 初期状態 (稼働中は set_mute で切り替える)
};

struct ChannelStats {
    std::atomic<bool> playing{false};
    std::atomic<double> latency_ms{0};    // 鳴らすべき時刻から実際に鳴るまで
    std::atomic<double> error_ms{0};      // 目標位置とのずれ (正: 早く鳴っている)
    std::atomic<double> ratio_ppm{0};     // リサンプル補正 (48kHz 以外のデバイス)
    std::atomic<uint64_t> slips{0};       // 重複 / 間引きしたサンプル数 (48kHz のデバイス)
    std::atomic<uint64_t> duplicated{0};  // そのうち重複
    std::atomic<uint64_t> dropped{0};     // そのうち間引き
    std::atomic<bool> passthrough{false};
    std::mutex reason_mutex;
    std::string last_resync_reason;
    std::atomic<uint64_t> underruns{0};
    std::atomic<uint64_t> resyncs{0};
    std::atomic<float> peak_l{0};  // 出力ごとにリセットする
    std::atomic<float> peak_r{0};
};

class ChannelSource {
public:
    // base_delay_qpc: render に渡す基本遅延 (作った時点で worker に知らせる)
    ChannelSource(const RouteOptions& options, uint32_t device_rate, int device_channels, int64_t base_delay_qpc);
    ~ChannelSource();
    ChannelSource(const ChannelSource&) = delete;
    ChannelSource& operator=(const ChannelSource&) = delete;

    // out (device_channels x frames, float) に加算する。
    // first_hear_qpc: このバッファの先頭サンプルが実際に鳴る時刻
    // base_delay_qpc: 鳴らすべき時刻からの音声の遅延 (デバイスのバッファ + 余裕)
    void render(float* out, int frames, int64_t first_hear_qpc, int64_t base_delay_qpc);

    const RouteOptions& options() const { return options_; }
    ChannelStats& stats() { return stats_; }

    // 出力先が無くなった (デバイスの出力を止めた) ことを worker に知らせる (出力遅延を 0 にする)。
    // 開き直すときは呼ばない (開き直した結果の値だけを知らせる。設計書 §6.3)
    void clear_output_delay() {
        if (header_) header_->output_delay_qpc.store(0);
    }

    // ミュート (遅延には影響しない。切り替えの瞬間だけフェードする)
    void set_mute(bool mute) { mute_ = mute; }
    bool muted() const { return mute_; }

private:
    bool ensure_open();
    bool ensure_swr();
    void start_at(uint64_t pos, uint64_t epoch);
    void copy_from_ring(float* dst, uint64_t pos, int frames) const;
    // デバイスのレートで最大 frames 出す (足りなければ少なく返す)
    int pull_passthrough(float* dst, int frames, int slip);  // slip: +1 重複 / -1 間引き / 0
    int pull_resampled(float* dst, int frames);
    void count_slip(int slip, int frames);

    const RouteOptions options_;
    const uint32_t device_rate_;
    const int device_channels_;
    const int fade_frames_;
    const bool passthrough_;  // デバイスが 48kHz ならリサンプルしない

    SharedMapping mapping_;
    audio::Header* header_ = nullptr;
    const float* ring_ = nullptr;
    int64_t last_open_attempt_ = 0;

    SwrContext* swr_ = nullptr;
    int64_t last_swr_attempt_ = 0;
    std::vector<float> in_buf_;
    std::vector<float> fifo_;  // リサンプル後 (デバイスのレート、ステレオ)
    size_t fifo_frames_ = 0;
    std::vector<float> tmp_;

    bool playing_ = false;
    bool resync_pending_ = false;  // フェードアウトが終わったら合わせ直す
    uint64_t read_pos_ = 0;
    uint64_t epoch_ = 0;
    std::atomic<bool> mute_{false};
    float fade_ = 0.0f;       // フェードの包絡 (0..1)
    double error_avg_ = 0.0;  // 目標位置とのずれ (平滑化、ソースのフレーム数)
    int frames_since_slip_ = 0;
    // 直近 1 秒の重複・間引き (同じ向きが続いたら合わせ直す)
    int slip_window_frames_ = 0;
    int slip_window_dup_ = 0;
    int slip_window_drop_ = 0;
    int slip_rate_seconds_ = 0;  // 同じ向きが kSlipRateResync を超えた秒が続いた数

    ChannelStats stats_;
};

}  // namespace denpachan::engine
