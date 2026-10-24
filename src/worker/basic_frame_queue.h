#pragma once
// デコードスレッド → 出力スレッドのフレームキュー (フレームの型によらない部分。frame_queue.h)。溢れたら古いものから捨てる。
// フレームの型は arrival_qpc (デコード完了時刻) を持つ。試験 (audio_timing_test) は FFmpeg のフレームの代わりに軽い型で使う。

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>

namespace denpachan::worker {

template <typename Frame>
class BasicFrameQueue {
public:
    explicit BasicFrameQueue(size_t capacity) : capacity_(capacity) {}

    // 溢れて捨てたら true
    bool push(Frame&& f) {
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

    std::optional<Frame> pop(std::chrono::milliseconds timeout) {
        std::unique_lock lk(mutex_);
        if (!cv_.wait_for(lk, timeout, [&] { return stopped_ || !queue_.empty(); })) return std::nullopt;
        if (queue_.empty()) return std::nullopt;
        Frame f = std::move(queue_.front());
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
    std::deque<Frame> queue_;
    const size_t capacity_;
    bool stopped_ = false;
};

}  // namespace denpachan::worker
