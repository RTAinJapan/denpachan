#pragma once
// デコードスレッド → 出力スレッドのフレームキュー。溢れたら古いものから捨てる。

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>

#include "ffmpeg_util.h"

namespace denpachan::worker {

struct DecodedFrame {
    FramePtr frame;          // CPU 上のフレーム (NV12 / P010 / yuv420p など)
    int64_t pts_us = 0;      // メディア時刻 (マイクロ秒)
    int64_t arrival_qpc = 0; // デコード完了時刻
    uint64_t session = 0;    // 接続ごとの番号 (変わったら時間軸を取り直す)
};

class FrameQueue {
public:
    explicit FrameQueue(size_t capacity) : capacity_(capacity) {}

    // 溢れて捨てたら true
    bool push(DecodedFrame&& f) {
        std::lock_guard lk(mutex_);
        bool dropped = false;
        if (queue_.size() >= capacity_) {
            queue_.pop_front();
            dropped = true;
        }
        queue_.push_back(std::move(f));
        cv_.notify_one();
        return dropped;
    }

    std::optional<DecodedFrame> pop(std::chrono::milliseconds timeout) {
        std::unique_lock lk(mutex_);
        if (!cv_.wait_for(lk, timeout, [&] { return stopped_ || !queue_.empty(); })) return std::nullopt;
        if (queue_.empty()) return std::nullopt;
        DecodedFrame f = std::move(queue_.front());
        queue_.pop_front();
        return f;
    }

    size_t size() {
        std::lock_guard lk(mutex_);
        return queue_.size();
    }

    void stop() {
        std::lock_guard lk(mutex_);
        stopped_ = true;
        cv_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<DecodedFrame> queue_;
    const size_t capacity_;
    bool stopped_ = false;
};

}  // namespace denpachan::worker
