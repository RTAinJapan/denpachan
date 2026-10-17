#pragma once
// チャンネルのメトリクス。各スレッドが更新し、1 秒ごとに集計して出力する。

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

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
};

struct ChannelMetrics {
    // ingest
    std::atomic<bool> connected{false};
    std::atomic<uint32_t> connections{0};
    std::atomic<uint64_t> bytes_in{0};
    std::atomic<uint64_t> overflow_bytes{0};  // デコーダが詰まって捨てた受信データ

    // decode
    std::atomic<uint64_t> frames_decoded{0};
    std::atomic<uint64_t> decode_errors{0};
    std::atomic<bool> hw_decode{false};
    std::atomic<int> width{0};
    std::atomic<int> height{0};

    // playout
    std::atomic<uint64_t> frames_presented{0};
    std::atomic<uint64_t> frames_dropped_late{0};
    std::atomic<uint64_t> frames_dropped_queue{0};
    std::atomic<int64_t> delay_us{0};  // 受信 (デコード完了) から vcam 出力までの遅延

    std::mutex mutex;  // 以下を保護
    std::string peer;
    std::string codec;
    SrtStats srt;
};

}  // namespace denpachan::worker
