#pragma once
// チャンネルのメトリクス。各スレッドが更新し、1 秒ごとに集計して出力する。

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

#include "denpachan/win_util.h"
#include "denpachan/worker_status.h"
#include "srt_advisor.h"

namespace denpachan::worker {

struct SrtStats {
    double rtt_ms = 0;
    double recv_mbps = 0;
    double bandwidth_mbps = 0;
    int64_t lost_total = 0;
    int64_t retrans_total = 0;
    int64_t dropped_total = 0;
    int rcv_buf_ms = 0;
    int tsbpd_delay_ms = 0;
    SrtAdvice advice;  // latency の推奨値
};

struct ChannelMetrics {
    // ingest
    std::atomic<bool> connected{false};
    std::atomic<uint32_t> connections{0};
    std::atomic<uint64_t> bytes_in{0};
    std::atomic<uint64_t> overflow_bytes{0};  // デコーダが詰まって捨てた受信データ
    std::atomic<int32_t> tcp_rtt_us{0};       // RTMP push (rtmpd が測った TCP の RTT)
    std::atomic<uint64_t> tcp_retrans_bytes{0};

    // decode
    std::atomic<uint64_t> frames_decoded{0};
    std::atomic<uint64_t> decode_errors{0};
    std::atomic<bool> hw_decode{false};
    std::atomic<int> width{0};
    std::atomic<int> height{0};

    // audio
    std::atomic<uint64_t> audio_frames{0};
    std::atomic<uint64_t> audio_gaps{0};     // 無音で埋めた抜け
    std::atomic<int64_t> audio_delay_us{0};  // 到着から鳴らすべき時刻まで
    std::atomic<int64_t> av_offset_us{0};    // 同じ時刻の映像に対する音声の遅れ (正: 音声が遅い)
    std::atomic<float> audio_peak_l{0};      // 出力ごとにリセットする
    std::atomic<float> audio_peak_r{0};

    // playout
    std::atomic<uint64_t> frames_presented{0};
    std::atomic<uint64_t> frames_dropped_late{0};
    std::atomic<uint64_t> frames_dropped_queue{0};
    std::atomic<int64_t> delay_us{0};  // 受信 (デコード完了) から vcam 出力までの遅延

    std::atomic<uint32_t> video_state{0};  // worker_status::VideoState

    // 録画
    std::atomic<uint32_t> rec_state{0};  // 0 オフ / 1 映像待ち / 2 録画中 / 3 エラー
    std::atomic<uint64_t> rec_bytes{0};
    std::atomic<uint64_t> rec_dropped{0};
    std::atomic<int64_t> rec_started_qpc{0};

    std::mutex mutex;  // 以下を保護
    std::string peer;
    std::string codec;
    std::string last_error;
    std::string rec_file;
    std::string rec_error;
    SrtStats srt;

    // engine に見せる状態の書き先 (worker_status 共有メモリ)。スレッドごとのハートビートをここに書く。
    worker_status::Header* status = nullptr;

    void heartbeat_ingest() {
        if (status) status->ingest_heartbeat_qpc.store(qpc_now(), std::memory_order_relaxed);
    }
    void heartbeat_playout() {
        if (status) status->playout_heartbeat_qpc.store(qpc_now(), std::memory_order_relaxed);
    }
    // デコード処理の出入り (ネットワーク待ちとは区別する)
    void decoder_enter() {
        if (status) status->decoder_busy_since_qpc.store(qpc_now(), std::memory_order_relaxed);
    }
    void decoder_leave() {
        if (status) status->decoder_busy_since_qpc.store(0, std::memory_order_relaxed);
    }
};

// デコード処理の区間 (この間に固まったら engine が検出する)
class DecoderBusy {
public:
    explicit DecoderBusy(ChannelMetrics& m) : m_(m) { m_.decoder_enter(); }
    ~DecoderBusy() { m_.decoder_leave(); }
    DecoderBusy(const DecoderBusy&) = delete;
    DecoderBusy& operator=(const DecoderBusy&) = delete;

private:
    ChannelMetrics& m_;
};

}  // namespace denpachan::worker
