#pragma once
// デコードスレッド → 出力スレッドのフレームキュー (フレームの型によらない部分。frame_queue.h)。溢れたら古いものから捨てる。
// フレームの型は arrival_qpc (デコード完了時刻) を持つ。試験 (audio_timing_test) は FFmpeg のフレームの代わりに軽い型で使う。
//
// 溢れの判定は、枚数ではなく時間の幅 (先頭と末尾の到着時刻の差) で行う (枚数の上限も置く)。出力スレッドは先頭を取り出して
// 出す時刻まで待つので、遅延がキューの長さを超えると、待つ間に後ろのフレームが押し出され、毎秒 1〜2 枚しか出なくなる
// (24 枚 = 60 fps で 400 ms のキューで、遅延 900 ms のとき)。キューは、とりうる最大の遅延を覆う幅にする (video_queue_limits)。

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>

namespace denpachan::worker {

struct FrameQueueLimits {
    size_t max_frames = 24;
    int64_t max_span_qpc = 0;  // 先頭と末尾の到着時刻の差の上限 (0 = 枚数だけで見る)
};

template <typename Frame>
class BasicFrameQueue {
public:
    explicit BasicFrameQueue(FrameQueueLimits limits) : limits_(limits) {}
    explicit BasicFrameQueue(size_t capacity) : limits_{capacity, 0} {}

    // 入れるたびに、そのフレームの到着時刻を知らせる (時計が「受信が続いているか」を見る。PlayoutClock::note_video_arrival)。
    // 溢れて捨てるフレームも知らせる (出力スレッドで待っている間に捨てたものを、途切れと数えないように)
    void set_on_push(std::function<void(int64_t arrival_qpc)> on_push) { on_push_ = std::move(on_push); }

    // 溢れて捨てたら true
    bool push(Frame&& f) {
        const int64_t arrival = f.arrival_qpc;
        bool dropped = false;
        {
            std::lock_guard lk(mutex_);
            while (!queue_.empty() && (queue_.size() >= limits_.max_frames ||
                                       (limits_.max_span_qpc > 0 && arrival - queue_.front().arrival_qpc > limits_.max_span_qpc))) {
                queue_.pop_front();
                ++dropped_;
                dropped = true;
            }
            queue_.push_back(std::move(f));
            ++pushed_;
            cv_.notify_one();
        }
        if (on_push_) on_push_(arrival);
        return dropped;
    }

    // 入れた枚数と、溢れて捨てた枚数 (出力が追いついているか。PlayoutCore::check_progress)
    uint64_t pushed() {
        std::lock_guard lk(mutex_);
        return pushed_;
    }
    uint64_t dropped() {
        std::lock_guard lk(mutex_);
        return dropped_;
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
    const FrameQueueLimits limits_;
    std::function<void(int64_t)> on_push_;
    uint64_t pushed_ = 0;
    uint64_t dropped_ = 0;
    bool stopped_ = false;
};

}  // namespace denpachan::worker
