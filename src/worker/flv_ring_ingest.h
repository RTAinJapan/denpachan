#pragma once
// RTMP push の受信: rtmpd がチャンネルの FLV リング (共有メモリ) に書いたバイト列を ByteQueue に移す。
// rtmpd の publish の区切り (セッション) を ByteQueue のセッションにそのまま対応させる。

#include <atomic>
#include <thread>

#include "byte_queue.h"
#include "denpachan/flv_ring.h"
#include "denpachan/win_util.h"
#include "metrics.h"

namespace denpachan::worker {

class FlvRingIngest {
public:
    FlvRingIngest(int ch, ByteQueue& queue, ChannelMetrics& metrics);
    ~FlvRingIngest();

    bool start();
    void stop();
    // 今の publish を区切り直す (次のキーフレームから新しいセッションにする)。他のスレッドから呼べる
    void request_resync() { resync_.store(true); }

private:
    void run();

    const int ch_;
    ByteQueue& queue_;
    ChannelMetrics& metrics_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> resync_{false};
    // 書き手 (rtmpd) のハートビート (1 秒ごと) がこれだけ止まったら、publish は終わったとみなす
    static constexpr int64_t kWriterTimeoutMs = 3000;
};

}  // namespace denpachan::worker
