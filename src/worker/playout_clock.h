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
//   一度に動かすのは、時間軸を作った直後の最初の調整と、遅れて届いたとき、遅延の余りが続いたとき、
//   鳴らし始めてから kFastAdjustMs の間 (下)。
// - 鳴らし始めてすぐは、揺れの備えが増えたり engine の出力遅延が決まったりして、目標とのずれが大きく残りやすい。
//   1 秒に 1 ms ずつ詰めると数十秒かかり、その間 engine は 1 秒に 48 サンプルずつ重複・間引きし続ける
//   (DVS で起動直後に 900〜2,400 回)。そこで、時間軸を作ってから、または engine の出力遅延が変わってから
//   kFastAdjustMs の間は、はっきり大きいずれだけを一度に動かす:
//   - 同じ向きに kFastJumpMs を超えるずれが 2 窓続いたら、2 窓の小さい方だけ動かす。窓ごとの最小の余裕は
//     到着の揺れで ±数 ms 動くので (RTMP、DVS が 10 ms 前後の単位で音を取る)、1 窓や 3 ms で動かすと、
//     足りない・余っているを交互に判定して、そのたびに音声が合わせ直しになった (開いた直後の 10 秒に 4〜8 回)。
//   - 遅延が kFastImmediateMs を超えて足りないときは、待たずに動かす (音が間に合わない。出力遅延が 0 → 32 ms など)。
//   - 一度に動かすのは kFastMaxMoves 回まで。残りは 1 ms ずつ。
//   それより後は 1 ms ずつのまま (動き続けると重複・間引きが増える。kHeadroomDecayMs のコメント)。
// - 一度に kEpochJumpMs を超えて動かしたら (遅れて届いて遅らせたときも) jumps() が増える。音声の書き手は
//   それを見て音声の epoch を進め、engine に (重複・間引きで追わずに) 1 回で合わせ直させる。
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
#include <string>
#include <utility>
#include <vector>

#include "denpachan/win_util.h"

namespace denpachan::worker {

class PlayoutClock {
public:
    // now: 今の時刻 (QPC)。試験では仮想の時計を渡す (tools の audio_timing_test)
    using NowFn = int64_t (*)();
    explicit PlayoutClock(int buffer_ms, NowFn now = &qpc_now) : buffer_(qpc_from_ms(buffer_ms)), now_(now) {}

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
        std::unique_lock lk(mutex_);
        frame_us_ = frame_us;
        if (now_() < window_start_qpc_) return;  // 接続直後のまとめ届きの間は使わない
        min_video_slack_ = std::min(min_video_slack_, slack_qpc);
        adjust_if_due_locked();
        flush_log(lk);
    }

    // 音声チャンクの余裕 (鳴らすべき時刻から出力遅延を引いたもの - 到着時刻) を報告する
    void report_audio(int64_t slack_qpc) {
        std::unique_lock lk(mutex_);
        const int64_t now = now_();
        last_audio_report_qpc_ = now;
        if (now < window_start_qpc_) return;  // 接続直後のまとめ届きの間は使わない
        if (slack_qpc < 0) {
            // 遅れて届いた: 即座に遅らせ、同じ大きさの揺れが次は間に合うよう備えを増やす。
            // このかたまりは窓の最小には入れない (入れると、増やした備えとの差で次の調整がもう一度動かす)
            anchor_qpc_ -= slack_qpc;
            // この窓でそれまでに測った余裕も、時間軸を遅らせた分だけ大きくなる。直さないと、遅らせる前の小さな余裕で
            // 次の調整が「まだ足りない」と見て、前の揺れの備えの分だけさらに遅らせてしまう (そして 1 秒に 1 ms ずつ戻す。
            // DVS の起動直後の重複・間引きの多くはこれだった)
            if (min_audio_slack_ != LLONG_MAX) min_audio_slack_ -= slack_qpc;
            if (min_video_slack_ != LLONG_MAX) min_video_slack_ -= slack_qpc;
            if (-slack_qpc > qpc_from_ms(kEpochJumpMs)) ++jumps_;
            grow_headroom_locked(audio_headroom_, -slack_qpc, kAudioHeadroomMaxMs, now, last_audio_late_qpc_);
            log_.push_back(fmt::format("playout: audio arrived {:.1f} ms late; delay increased, audio headroom {:.0f} ms",
                                       qpc_to_us(-slack_qpc) / 1000.0, qpc_to_us(audio_headroom_) / 1000.0));
        } else if (now - last_audio_late_report_qpc_ > qpc_from_ms(kBurstMs)) {
            min_audio_slack_ = std::min(min_audio_slack_, slack_qpc);
        }
        // 遅れて届いたかたまりと一緒に届いたもの (途切れのあとのまとめ届きの残り) も窓の最小に入れない。
        // 入れると、そのぶん余裕が小さく見えて時間軸をさらに遅らせ、そのあと何十秒もかけて 1 ms ずつ戻すことになる
        if (slack_qpc < 0) last_audio_late_report_qpc_ = now;
        adjust_if_due_locked();
        flush_log(lk);
    }

    // engine の出力遅延が変わった (デバイスを開いた、開き直した): しばらく大きなずれを一度に詰める
    void allow_fast_adjust() {
        std::lock_guard lk(mutex_);
        start_fast_locked(now_());
    }

    // 時間軸を一度に kEpochJumpMs を超えて動かした回数 (音声の書き手が epoch を進める)
    uint64_t jumps() {
        std::lock_guard lk(mutex_);
        return jumps_;
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
        return last_audio_report_qpc_ != 0 && now_() - last_audio_report_qpc_ < qpc;
    }

    int64_t buffer_qpc() const { return buffer_; }

private:
    // 溜めたログをロックを外してから書く (ログはファイルに書くので、ロックを持ったまま書くと、出力スレッドと
    // 音声のスレッドがその間待たされる)
    void flush_log(std::unique_lock<std::mutex>& lk) {
        if (log_.empty() && debug_.empty()) return;
        std::vector<std::string> lines, debug;
        lines.swap(log_);
        debug.swap(debug_);
        lk.unlock();
        for (const auto& line : lines) spdlog::info("{}", line);
        for (const auto& line : debug) spdlog::debug("{}", line);
    }

    static std::string fmt_ms(int64_t v) { return v == LLONG_MAX ? std::string("-") : fmt::format("{:+.1f}", qpc_to_us(v) / 1000.0); }

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
        window_start_qpc_ = now_() + qpc_from_ms(kSettleMs);
        min_video_slack_ = LLONG_MAX;
        min_audio_slack_ = LLONG_MAX;
    }

    void adjust_if_due_locked() {
        const int64_t now = now_();
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
            const char* reason = "";
            const int64_t fast_step = fast_step_locked(error, now, video_error < -jump && video_error <= audio_error);
            if (!settled_) {
                // 時間軸を作った直後 (まだ鳴らしていない): 大きなずれは一度に合わせる
                if (std::abs(error) > jump) step = error;
                start_fast_locked(now);
                reason = "first";
            } else if (fast_step != 0) {
                // 鳴らし始めてすぐ / 出力遅延が変わってすぐ: はっきり大きいずれだけを一度に合わせる (fast_step_locked)
                step = fast_step;
                ++fast_moves_;
                reason = "fast";
                log_.push_back(fmt::format("playout: moved the delay {:+.1f} ms at once (soon after starting or an output delay change)",
                                           qpc_to_us(-step) / 1000.0));
            } else if (excess_windows_ >= kCatchUpWindows) {
                // 遅延の余りが続いている (短い途切れのあとのまとめ届きなど): 一度に戻す
                step = error;
                excess_windows_ = 0;
                reason = "catch up";
                log_.push_back(fmt::format("playout: delay {:.1f} ms more than needed; caught up", qpc_to_us(error) / 1000.0));
            } else if (std::abs(error) <= qpc_from_ms(kDeadbandMs)) {
                // 1 秒ごとの最小の揺れには反応しない (反応すると時間軸が 1 秒に 1 ms ずつ行き来し続け、
                // engine がそれを追うサンプルの重複・間引きが絶えない)
                step = 0;
                reason = "deadband";
            } else if (video_error < -jump && video_error <= audio_error) {
                // 映像が 3 フレームを超えて遅れた: 一度に遅らせ、備えを増やす (音声は合わせ直しになる)
                step = video_error;
                grow_headroom_locked(video_headroom_, -video_error, kVideoHeadroomMaxMs, now, last_video_late_qpc_);
                reason = "video late";
                log_.push_back(fmt::format("playout: video arrived {:.1f} ms late; delay increased, video headroom {:.0f} ms",
                                           qpc_to_us(-video_error) / 1000.0, qpc_to_us(video_headroom_) / 1000.0));
            } else {
                reason = "step";
            }
            if (std::abs(step) > qpc_from_ms(kEpochJumpMs) && settled_) ++jumps_;
            // 1 秒ごとの要約 (debug): 時間軸を動かした量と理由。engine の重複・間引きと並べて見る
            if (step != 0) {
                debug_.push_back(
                    fmt::format("playout: moved {:+.2f} ms ({}); error video {} / audio {} ms, headroom video {:.0f} / audio {:.0f} ms",
                                qpc_to_us(-step) / 1000.0, reason, fmt_ms(video_error), fmt_ms(audio_error),
                                qpc_to_us(video_headroom_) / 1000.0, qpc_to_us(audio_headroom_) / 1000.0));
            }
            anchor_qpc_ -= step;
            settled_ = true;
        }
        window_start_qpc_ = now;
        min_video_slack_ = LLONG_MAX;
        min_audio_slack_ = LLONG_MAX;
    }

    void start_fast_locked(int64_t now) {
        fast_until_qpc_ = now + qpc_from_ms(kFastAdjustMs);
        fast_moves_ = 0;
        fast_prev_error_ = LLONG_MAX;
    }

    // 鳴らし始めてすぐ / 出力遅延が変わってすぐの間に、一度に動かす量 (0 = 動かさない)。窓ごとに呼ぶ
    int64_t fast_step_locked(int64_t error, int64_t now, bool video_late) {
        const int64_t prev = fast_prev_error_;
        fast_prev_error_ = error;
        if (!settled_ || now >= fast_until_qpc_ || fast_moves_ >= kFastMaxMoves) return 0;
        if (video_late) return 0;                                  // 映像の遅れは、備えを増やして動かす (adjust_if_due_locked)
        if (error < -qpc_from_ms(kFastImmediateMs)) return error;  // 大きく足りない: 待たない
        const int64_t limit = qpc_from_ms(kFastJumpMs);
        if (prev == LLONG_MAX || std::abs(error) <= limit || std::abs(prev) <= limit) return 0;
        if ((error > 0) != (prev > 0)) return 0;                           // 向きが揃っていない: 揺れ
        return error > 0 ? std::min(error, prev) : std::max(error, prev);  // 2 窓の小さい方だけ
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
    static constexpr int64_t kFastAdjustMs = 10'000;  // 鳴らし始めてから / 出力遅延が変わってから、一度に詰める間
    static constexpr int64_t kFastJumpMs = 8;         // その間、同じ向きにこれを超えるずれが 2 窓続いたら一度に詰める
    static constexpr int64_t kFastImmediateMs = 20;   // その間、遅延がこれを超えて足りなければすぐに詰める
    static constexpr int kFastMaxMoves = 2;           // その間に一度に詰める回数の上限
    // 一度にこれを超えて動かしたら (遅れて届いて遅らせたときも) 音声の epoch を進める。engine は平滑化したずれが
    // 5 ms を超えると合わせ直すので、それより小さい動きは重複・間引きで追わせる
    static constexpr int64_t kEpochJumpMs = 5;
    static constexpr int64_t kBurstMs = 5;     // 遅れて届いたかたまりからこの間に届いたものは、まとめ届きの残り
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
    const NowFn now_;
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
    int excess_windows_ = 0;                  // 遅延の余りが 3 フレームを超えた窓が続いた数
    std::vector<std::string> log_;            // ロックを外してから書くログ (flush_log)
    std::vector<std::string> debug_;          // 同じく debug のログ
    int64_t fast_until_qpc_ = 0;              // この時刻までは、大きなずれを一度に詰める
    int fast_moves_ = 0;                      // その間に一度に詰めた回数
    int64_t fast_prev_error_ = LLONG_MAX;     // 前の窓の目標とのずれ (向きが続いているかを見る)
    int64_t last_audio_late_report_qpc_ = 0;  // 最後に音声が遅れて届いた時刻 (まとめ届きの残りを窓に入れない)
    uint64_t jumps_ = 0;                      // 一度に kEpochJumpMs を超えて動かした回数
    int64_t last_audio_report_qpc_ = 0;

    // 揺れの備え (接続をまたいで覚えておく: 同じ送出元の揺れ方は変わらないことが多い)
    int64_t audio_headroom_ = qpc_from_ms(kAudioHeadroomMinMs);
    int64_t video_headroom_ = 0;
    int64_t last_audio_late_qpc_ = 0;
    int64_t last_video_late_qpc_ = 0;
};

}  // namespace denpachan::worker
