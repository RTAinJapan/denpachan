#pragma once
// プロセスが止まった (スケジュールされなかった) ことを見つける (engine・worker・rtmpd。設計書 §6.3「乱れの調べ方」)。
//
// 10 ms ごとに起きるだけのスレッドを置き、起きるのが kStallLogMs を超えて遅れたら warn で記録する。全チャンネルが
// 同時に乱れたとき、全部のプロセスで同じ時刻に出ていれば PC 全体が止まった (OS・ドライバ・ほかのプロセス)、
// 1 つのプロセスだけなら、そのプロセスが止まった (denpachan) と見分けられる。どちらにも出ていなければ、
// プロセスは動いていた (データが届かなかった、デコードが遅れたなど。受信とデコードのログで見る)。
// 1 秒ごとの最大の遅れは take_max_ms() で取れる (worker の 1 秒ごとの統計行に出す)。

#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>

#include "denpachan/win_util.h"

namespace denpachan {

class StallWatch {
public:
    explicit StallWatch(std::string who) : who_(std::move(who)), thread_([this] { run(); }) {}
    ~StallWatch() {
        stop_ = true;
        if (thread_.joinable()) thread_.join();
    }
    StallWatch(const StallWatch&) = delete;
    StallWatch& operator=(const StallWatch&) = delete;

    // 前に呼んでからの、起きるのが遅れた最大 (ms)
    int64_t take_max_ms() { return max_late_ms_.exchange(0); }

    static constexpr int64_t kTickMs = 10;
    // これを超えて遅れたら記録する。Windows の既定のタイマー (15.6 ms) やふだんの負荷で起きる遅れより十分大きく、
    // 映像・音声の途切れとして見える大きさより小さい
    static constexpr int64_t kStallLogMs = 100;

private:
    void run() {
        int64_t prev = qpc_now();
        while (!stop_) {
            Sleep(DWORD(kTickMs));
            const int64_t now = qpc_now();
            // 予定より遅れた分 (タイマーの粒度の分も含む。小さい値は気にしない)
            const int64_t late_ms = std::max<int64_t>(qpc_to_us(now - prev) / 1000 - kTickMs, 0);
            prev = now;
            int64_t cur = max_late_ms_.load();
            while (late_ms > cur && !max_late_ms_.compare_exchange_weak(cur, late_ms)) {}
            if (late_ms > kStallLogMs) {
                spdlog::warn("stall: {} did not run for {} ms (a thread that wakes every {} ms woke late)", who_, late_ms + kTickMs,
                             kTickMs);
            }
        }
    }

    const std::string who_;
    std::atomic<bool> stop_{false};
    std::atomic<int64_t> max_late_ms_{0};
    std::thread thread_;
};

}  // namespace denpachan
