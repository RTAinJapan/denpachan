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

private:
    void run();

    const int ch_;
    ByteQueue& queue_;
    ChannelMetrics& metrics_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
};

}  // namespace denpachan::worker
