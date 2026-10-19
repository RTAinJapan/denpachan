#pragma once
// 映像と音声で共有する時間軸 (メディア時刻 → 出すべき時刻)。設計書 §3.5。
//
// C(pts) = anchor_qpc + (pts - anchor_pts)。映像は C(pts) に仮想カメラへ出し、
// A/V 同期モードでは音声も C(pts) にデバイスから鳴るようにする。
//
// 時間軸の調整:
// - 最初に来たフレーム (映像でも音声でも) の到着時刻 + 再生バッファに合わせる。
// - 映像・音声それぞれ「到着から出すべき時刻までの余裕 (slack)」を報告してもらい、
//   1 秒ごとに、余裕の最も小さかった側が目標 (映像: 再生バッファ、音声: 0) になるように動かす。
//   ずれが 3 フレームを超えれば一度に、それ以下なら 1ms ずつ動かす。
// - 音声が遅れて届いた (余裕が負) ときは、届く前に鳴らすことはできないので即座に遅らせる。
//   映像はその分待たされる (A/V 同期を保つため)。

#include <algorithm>
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <utility>

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

    // 時間軸を捨てる (映像の途絶)。次に来たフレームで作り直す。
    void invalidate() {
        std::lock_guard lk(mutex_);
        anchored_ = false;
    }

    // 映像フレームの余裕 (出すべき時刻 - 到着時刻) を報告する
    void report_video(int64_t slack_qpc, int64_t frame_us) {
        std::lock_guard lk(mutex_);
        frame_us_ = frame_us;
        if (qpc_now() < window_start_qpc_) return;  // 接続直後のまとめ届きの間は使わない
        min_video_slack_ = std::min(min_video_slack_, slack_qpc);
        adjust_if_due_locked();
    }

    // 音声チャンクの余裕 (鳴らすべき時刻から出力遅延を引いたもの - 到着時刻) を報告する
    void report_audio(int64_t slack_qpc) {
        std::lock_guard lk(mutex_);
        if (qpc_now() < window_start_qpc_) return;  // 接続直後のまとめ届きの間は使わない
        if (slack_qpc < 0) {
            anchor_qpc_ -= slack_qpc;  // 遅れて届いた: 即座に遅らせる
            ++audio_late_count_;
            slack_qpc = 0;
        }
        min_audio_slack_ = std::min(min_audio_slack_, slack_qpc);
        adjust_if_due_locked();
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

    // 時間軸を作ってから最初の調整が済んだか (音声はこれを待ってから鳴らし始める)
    bool settled() {
        std::lock_guard lk(mutex_);
        return anchored_ && settled_;
    }

    uint64_t take_audio_late_count() {
        std::lock_guard lk(mutex_);
        return std::exchange(audio_late_count_, 0);
    }

    int64_t buffer_qpc() const { return buffer_; }

private:
    void anchor_locked(uint64_t session, int64_t pts_us, int64_t arrival_qpc) {
        anchored_ = true;
        session_ = session;
        anchor_pts_us_ = pts_us;
        anchor_qpc_ = arrival_qpc + buffer_;
        ++generation_;
        settled_ = false;
        // 接続直後はストリーム解析中に溜まったフレームがまとめて届く (到着が実際より遅く見える)。
        // その間の報告は使わず、落ち着いてから最初の窓を始める。
        window_start_qpc_ = qpc_now() + qpc_from_ms(kSettleMs);
        min_video_slack_ = LLONG_MAX;
        min_audio_slack_ = LLONG_MAX;
    }

    void adjust_if_due_locked() {
        const int64_t now = qpc_now();
        if (now - window_start_qpc_ < qpc_from_ms(1000)) return;
        // 目標からの余り (正: 遅延が大きすぎる / 負: 小さすぎる)。報告のなかった側は無視する。
        int64_t error = LLONG_MAX;
        if (min_video_slack_ != LLONG_MAX) error = std::min(error, min_video_slack_ - buffer_);
        if (min_audio_slack_ != LLONG_MAX) error = std::min(error, min_audio_slack_);
        if (error != LLONG_MAX) {
            const int64_t step = std::abs(error) > qpc_from_us(frame_us_ * 3)
                                     ? error
                                     : std::clamp(error, -qpc_from_ms(1), qpc_from_ms(1));
            anchor_qpc_ -= step;
            settled_ = true;
        }
        window_start_qpc_ = now;
        min_video_slack_ = LLONG_MAX;
        min_audio_slack_ = LLONG_MAX;
    }

    static constexpr int64_t kSettleMs = 500;

    const int64_t buffer_;
    std::mutex mutex_;
    bool anchored_ = false;
    bool settled_ = false;
    uint64_t session_ = 0;
    int64_t anchor_pts_us_ = 0;
    int64_t anchor_qpc_ = 0;
    uint64_t generation_ = 0;

    int64_t window_start_qpc_ = 0;
    int64_t min_video_slack_ = LLONG_MAX;
    int64_t min_audio_slack_ = LLONG_MAX;
    int64_t frame_us_ = 16'667;
    uint64_t audio_late_count_ = 0;
};

}  // namespace denpachan::worker
