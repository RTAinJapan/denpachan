#pragma once
// 映像と音声で共有する時間軸 (メディア時刻 → 出すべき時刻)。設計書 §3.5。
//
// C(pts) = anchor_qpc + (pts - anchor_pts)。映像は C(pts) に仮想カメラへ出し、
// A/V 同期モードでは音声も C(pts) にデバイスから鳴るようにする。
//
// 時間軸の調整:
// - 最初に来たフレーム (映像でも音声でも) の到着時刻 + 再生バッファに合わせる。
// - 映像・音声それぞれ「到着から出すべき時刻までの余裕 (slack)」を報告してもらい、
//   1 秒ごとに、余裕の最も小さかった側が目標 (映像: 再生バッファ + 揺れの備え、音声: 揺れの備え) になるように動かす。
// - 時間軸が動くと、engine は鳴らす位置を合わせ直す。大きく動くと音が途切れる (フェードして合わせ直す) ので、
//   鳴り始めてからは 1 秒に 1 ms までしか動かさない (engine はサンプルの重複・間引きで追える)。
//   一度に動かすのは、時間軸を作った直後の最初の調整と、遅れて届いたとき、遅延の余りが続いたときだけ。
// - 遅延の余り: 短い途切れのあとにまとめて届くと、途切れた長さの分だけ遅らされ、揺れの備えを超えた分が余る。
//   1 秒に 1 ms では何分も戻らず、その間は映像がキュー (24 フレーム) に収まらずコマ送りになる。
//   余りが 3 フレームを超えたまま kCatchUpWindows 回続いたら一度に戻す (音声は 1 回合わせ直しになる)。
//   1 回だけの余りで戻さないのは、遅れて届いた直後の窓はまとめ届きの残りで余裕が大きく見えることがあるため。
// - 音声が遅れて届いた (余裕が負) ときは、届く前に鳴らすことはできないので即座に遅らせる。
//   映像はその分待たされる (A/V 同期を保つため)。映像が 3 フレームを超えて遅れたときも一度に遅らせる。
// - 揺れの備え (headroom): 遅れて届くたびに、その遅れの分だけ増やす (同じ大きさの揺れが次は間に合う)。
//   遅れがしばらくなければ少しずつ減らす。これがないと、余裕を 0 まで詰めては次の揺れで遅れて
//   時間軸が飛ぶ (音が途切れる) のを繰り返す (RTMP では約 1 分に 1 回)。

#include <spdlog/spdlog.h>

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
        const int64_t now = qpc_now();
        last_audio_report_qpc_ = now;
        if (now < window_start_qpc_) return;  // 接続直後のまとめ届きの間は使わない
        if (slack_qpc < 0) {
            // 遅れて届いた: 即座に遅らせ、同じ大きさの揺れが次は間に合うよう備えを増やす。
            // このかたまりは窓の最小には入れない (入れると、増やした備えとの差で次の調整がもう一度動かす)
            anchor_qpc_ -= slack_qpc;
            ++audio_late_count_;
            grow_headroom_locked(audio_headroom_, -slack_qpc, kAudioHeadroomMaxMs, now, last_audio_late_qpc_);
            spdlog::info("playout: audio arrived {:.1f} ms late; delay increased, audio headroom {:.0f} ms", qpc_to_us(-slack_qpc) / 1000.0,
                         qpc_to_us(audio_headroom_) / 1000.0);
        } else {
            min_audio_slack_ = std::min(min_audio_slack_, slack_qpc);
        }
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

    // 音声がこの時間内に届いているか (映像が途切れても、音声が時間軸を使い続けているか)
    bool audio_reported_within(int64_t qpc) {
        std::lock_guard lk(mutex_);
        return last_audio_report_qpc_ != 0 && qpc_now() - last_audio_report_qpc_ < qpc;
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
        excess_windows_ = 0;
        // 接続直後はストリーム解析中に溜まったフレームがまとめて届く (到着が実際より遅く見える)。
        // その間の報告は使わず、落ち着いてから最初の窓を始める。
        window_start_qpc_ = qpc_now() + qpc_from_ms(kSettleMs);
        min_video_slack_ = LLONG_MAX;
        min_audio_slack_ = LLONG_MAX;
    }

    void adjust_if_due_locked() {
        const int64_t now = qpc_now();
        if (now - window_start_qpc_ < qpc_from_ms(1000)) return;
        decay_headroom_locked(video_headroom_, 0, now, last_video_late_qpc_);
        decay_headroom_locked(audio_headroom_, kAudioHeadroomMinMs, now, last_audio_late_qpc_);
        // 目標からの余り (正: 遅延が大きすぎる / 負: 小さすぎる)。報告のなかった側は無視する。
        const int64_t video_error = min_video_slack_ != LLONG_MAX ? min_video_slack_ - buffer_ - video_headroom_ : LLONG_MAX;
        const int64_t audio_error = min_audio_slack_ != LLONG_MAX ? min_audio_slack_ - audio_headroom_ : LLONG_MAX;
        const int64_t error = std::min(video_error, audio_error);
        if (error != LLONG_MAX) {
            const int64_t jump = qpc_from_us(frame_us_ * 3);
            const int64_t limit = qpc_from_ms(kMaxStepMs);
            int64_t step = std::clamp(error, -limit, limit);
            excess_windows_ = error > jump ? excess_windows_ + 1 : 0;
            if (!settled_) {
                // 時間軸を作った直後 (まだ鳴らしていない): 大きなずれは一度に合わせる
                if (std::abs(error) > jump) step = error;
            } else if (excess_windows_ >= kCatchUpWindows) {
                // 遅延の余りが続いている (短い途切れのあとのまとめ届きなど): 一度に戻す
                step = error;
                excess_windows_ = 0;
                spdlog::info("playout: delay {:.1f} ms more than needed; caught up", qpc_to_us(error) / 1000.0);
            } else if (std::abs(error) <= qpc_from_ms(kDeadbandMs)) {
                // 1 秒ごとの最小の揺れには反応しない (反応すると時間軸が 1 秒に 1 ms ずつ行き来し続け、
                // engine がそれを追うサンプルの重複・間引きが絶えない)
                step = 0;
            } else if (video_error < -jump && video_error <= audio_error) {
                // 映像が 3 フレームを超えて遅れた: 一度に遅らせ、備えを増やす (音声は合わせ直しになる)
                step = video_error;
                grow_headroom_locked(video_headroom_, -video_error, kVideoHeadroomMaxMs, now, last_video_late_qpc_);
                spdlog::info("playout: video arrived {:.1f} ms late; delay increased, video headroom {:.0f} ms",
                             qpc_to_us(-video_error) / 1000.0, qpc_to_us(video_headroom_) / 1000.0);
            }
            anchor_qpc_ -= step;
            settled_ = true;
        }
        window_start_qpc_ = now;
        min_video_slack_ = LLONG_MAX;
        min_audio_slack_ = LLONG_MAX;
    }

    // 揺れの備えを、遅れた分 (1 回に kHeadroomGrowMaxMs まで) 増やす
    static void grow_headroom_locked(int64_t& headroom, int64_t late, int64_t max_ms, int64_t now, int64_t& last_late) {
        headroom = std::min(headroom + std::min(late, qpc_from_ms(kHeadroomGrowMaxMs)), qpc_from_ms(max_ms));
        last_late = now;
    }

    // 遅れがしばらくなければ、揺れの備えを少しずつ減らす (遅延はそれに合わせて 1 秒に 1 ms ずつ詰まる)
    static void decay_headroom_locked(int64_t& headroom, int64_t min_ms, int64_t now, int64_t& last_late) {
        if (headroom <= qpc_from_ms(min_ms)) return;
        if (now - last_late < qpc_from_ms(kHeadroomDecayMs)) return;
        headroom = std::max(headroom - qpc_from_ms(1), qpc_from_ms(min_ms));
        last_late = now;  // 次に減らすのはまた kHeadroomDecayMs 後
    }

    static constexpr int64_t kSettleMs = 500;
    static constexpr int64_t kMaxStepMs = 1;   // 鳴り始めてから 1 回 (1 秒) に動かす上限
    static constexpr int64_t kDeadbandMs = 2;  // 鳴り始めてからは、目標とのずれがこれ以下なら動かさない
    // 遅延の余り (3 フレーム超) がこの回数続いたら一度に戻す
    static constexpr int kCatchUpWindows = 2;
    static constexpr int64_t kAudioHeadroomMinMs = 10;  // 音声の揺れの備え (最初の値と下限)
    static constexpr int64_t kAudioHeadroomMaxMs = 200;
    static constexpr int64_t kVideoHeadroomMaxMs = 200;
    static constexpr int64_t kHeadroomGrowMaxMs = 50;  // 1 回の遅れで増やす上限 (一時的な大きな途切れで増やしすぎない)
    // 遅れがこの間なければ備えを 1 ms 減らす。速く減らすと、減っては次の小さな遅れで増える、を繰り返して時間軸が
    // 動き続け、engine がそれを追うサンプルの重複・間引きも増える (5 秒では 1 秒に十数回)
    static constexpr int64_t kHeadroomDecayMs = 30'000;

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
    int excess_windows_ = 0;  // 遅延の余りが 3 フレームを超えた窓が続いた数
    uint64_t audio_late_count_ = 0;
    int64_t last_audio_report_qpc_ = 0;

    // 揺れの備え (接続をまたいで覚えておく: 同じ送出元の揺れ方は変わらないことが多い)
    int64_t audio_headroom_ = qpc_from_ms(kAudioHeadroomMinMs);
    int64_t video_headroom_ = 0;
    int64_t last_audio_late_qpc_ = 0;
    int64_t last_video_late_qpc_ = 0;
};

}  // namespace denpachan::worker
