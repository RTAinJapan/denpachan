#pragma once
// 映像と音声で共有する時間軸 (メディア時刻 → 出すべき時刻)。設計書 §3.5。
//
// 最初に来たフレーム (映像でも音声でも) の到着時刻 + 再生バッファに合わせる。
// 遅延の補正 (shift) や取り直し (reanchor) は映像の Playout が行い、音声はそれに従う。

#include <cstdint>
#include <mutex>

#include "denpachan/win_util.h"

namespace denpachan::worker {

class PlayoutClock {
public:
    explicit PlayoutClock(int buffer_ms) : buffer_(qpc_from_ms(buffer_ms)) {}

    // pts_us のフレームを出すべき時刻 (QPC)。時間軸がなければ (別セッションなら) ここで作る。
    int64_t due(uint64_t session, int64_t pts_us, int64_t arrival_qpc) {
        std::lock_guard lk(mutex_);
        if (!anchored_ || session != session_) anchor_locked(session, pts_us, arrival_qpc);
        return anchor_qpc_ + qpc_from_us(pts_us - anchor_pts_us_);
    }

    // 時間軸を取り直す (タイムスタンプの飛びなど)
    void reanchor(uint64_t session, int64_t pts_us, int64_t arrival_qpc) {
        std::lock_guard lk(mutex_);
        anchor_locked(session, pts_us, arrival_qpc);
    }

    // 出す時刻を step だけ早める (負なら遅らせる)
    void shift(int64_t step_qpc) {
        std::lock_guard lk(mutex_);
        anchor_qpc_ -= step_qpc;
    }

    // 時間軸を捨てる (映像の途絶)。次に来たフレームで作り直す。
    void invalidate() {
        std::lock_guard lk(mutex_);
        anchored_ = false;
    }

    // 取り直しのたびに増える
    uint64_t generation() {
        std::lock_guard lk(mutex_);
        return generation_;
    }

    bool anchored_for(uint64_t session) {
        std::lock_guard lk(mutex_);
        return anchored_ && session_ == session;
    }

    int64_t buffer_qpc() const { return buffer_; }

private:
    void anchor_locked(uint64_t session, int64_t pts_us, int64_t arrival_qpc) {
        anchored_ = true;
        session_ = session;
        anchor_pts_us_ = pts_us;
        anchor_qpc_ = arrival_qpc + buffer_;
        ++generation_;
    }

    const int64_t buffer_;
    std::mutex mutex_;
    bool anchored_ = false;
    uint64_t session_ = 0;
    int64_t anchor_pts_us_ = 0;
    int64_t anchor_qpc_ = 0;
    uint64_t generation_ = 0;
};

}  // namespace denpachan::worker
