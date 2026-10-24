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
// - 方針: 見ている側に違和感の無い合わせ方にする。時間軸を一度に動かすと、engine はフェードして合わせ直し
//   (音が数十 ms 途切れる)、映像は動かした分だけ止まるか飛ぶ。1 ms ずつ動かすと、engine はサンプルの重複・間引き
//   (10 ms に 1 サンプルまで) で追い、映像も音声もほぼ分からない。
//   - すぐに直さないとどうしようもない大きさのずれは、一度に直す (1 回の短い乱れを許して、長く引きずらない)。
//     時間軸を作った直後の最初の調整、鳴らし始めてから / 出力先を変えてから kFastAdjustMs の間 (下)、音が間に合わない
//     とき (遅れて届いた、映像が遅れた)、遅延の余りが続いたとき。
//   - 落ち着いた後は、揺れに反応して動かさない。本当にずれているときだけ 1 ms ずつ追う (下)。
//   - DVS の停止・開始や出力先の切り替えの直後は、多少乱れてもよい (頻繁にする操作ではない)。その後に落ち着けばよい。
//   重複・間引きの回数そのものに実害はほぼ無いので、回数を減らすために一度に動かす場面を増やさない。
// - 落ち着いた後の 1 ms ずつの調整 (送出元とのクロック差、揺れの備えを減らした分): その窓のずれではなく、直近
//   kHistoryWindows 窓のずれの最小 (揺れの悪い側の包絡。前の窓も今の時間軸と揺れの備えで計算し直す) で決める。
//   包絡が kDeadbandMs を超えて足りなければ遅らせ、超えて余っていれば (= 直近の窓がすべて余っていれば) 詰める。
//   前に動かしてから kStepWindows 窓は空ける (engine の重複・間引きが同じ向きに続いて合わせ直しにならないように)。
//   - 窓ごとの最小の余裕は到着の揺れで ±2〜4 ms (大きな遅れの後は ±5〜20 ms) 動く。幅を 2 ms にしていたときは揺れの
//     たびに ±1 ms ずつ行き来した (DVS で約 1 分 45 秒に 30 回以上)。その窓のずれで、足りない側だけすぐに動かすと、
//     揺れが下に振れるたびに +1 ms し、数秒後に良い窓に釣られて −1 ms で戻した。足りない側も同じ向きが続くのを待つと、
//     揺れの良い窓まで幅を超えるまで動かず、悪い窓が揺れの備えを超えて遅れた (200 ppm 遅い送出元)。
//     包絡なら、悪い窓が幅の内側に収まった所で止まり、良い窓に釣られて戻らない。
// - 鳴らし始めてすぐは、揺れの備えが増えたり engine の出力遅延が決まったりして、目標とのずれが大きく残りやすい。
//   1 秒に 1 ms ずつ詰めると数十秒かかり、その間 engine は 1 秒に 48 サンプルずつ重複・間引きし続ける
//   (DVS で起動直後に 900〜2,400 回)。そこで、時間軸を作ってから、または engine の出力遅延が変わってから
//   kFastAdjustMs の間は、はっきり大きいずれだけを一度に動かす:
//   - 同じ向きに kFastJumpMs を超えるずれが 2 窓続いたら、2 窓の小さい方だけ動かす。窓ごとの最小の余裕は
//     到着の揺れで ±数 ms 動くので (RTMP、DVS が 10 ms 前後の単位で音を取る)、1 窓や 3 ms で動かすと、
//     足りない・余っているを交互に判定して、そのたびに音声が合わせ直しになった (開いた直後の 10 秒に 4〜8 回)。
//   - 遅延が kFastImmediateMs を超えて足りないときは、待たずに動かす (音が間に合わない。出力遅延が 0 → 32 ms など)。
//   - 一度に動かすのは kFastMaxMoves 回まで。残りは 1 ms ずつ。
//   それより後は、上の 1 ms ずつのまま。一度に動かしたあとに残る数 ms のずれも、幅を超えていればそれで詰める。
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
//   時間軸が飛ぶ (音が途切れる) のを繰り返す (RTMP では約 1 分に 1 回)。減らした分 (30 秒に 1 ms) は遅延の余りになり、
//   溜まって幅を超えたら、上の 1 ms ずつで詰める。
//   - 途切れ (kOutageLateMs を超える遅れと、その直後 kOutageTailMs の間の遅れ) では増やさない。時間軸は遅らせ、余りは
//     caught up で一度に戻す。数百 ms の途切れで備えを増やすと、30 秒に 1 ms ずつしか減らず、約 1 時間、遅延が
//     100 ms 以上多いままになった (DVS + RTMP で 868 / 853 ms の遅れの後、備えが 10 → 145 ms)。
//   - 音声が kHeadroomReviewMs の間遅れて届かなければ、その間の窓ごとの最小の余裕の散らばり (時間軸の動きを
//     差し引いたもの) に上乗せした値で決め直す。今の備えがそれより kHeadroomCutMs を超えて多ければ、一度に下げて
//     時間軸も一度に詰める (100 ms 以上の余分な遅延を長く引きずらない)。それより小さい余りは 30 秒に 1 ms ずつ減らす。

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
            review_start_locked(now);  // 遅れて届いた: 揺れの備えを決め直す間を数え直す
            if (outage_locked(-slack_qpc, now)) {
                // 途切れ (とその直後のまとめ届き): 時間軸は遅らせるが、揺れの備えは増やさない (余りは caught up で一度に戻す)
                log_.push_back(fmt::format("playout: audio arrived {:.1f} ms late (outage); delay increased, audio headroom kept {:.0f} ms",
                                           qpc_to_us(-slack_qpc) / 1000.0, qpc_to_us(audio_headroom_) / 1000.0));
            } else {
                grow_headroom_locked(audio_headroom_, -slack_qpc, kAudioHeadroomMaxMs, now, last_audio_late_qpc_);
                log_.push_back(fmt::format("playout: audio arrived {:.1f} ms late; delay increased, audio headroom {:.0f} ms",
                                           qpc_to_us(-slack_qpc) / 1000.0, qpc_to_us(audio_headroom_) / 1000.0));
            }
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

    // engine の出力遅延が delta だけ変わった (デバイスを開き直した): 時間軸を同じだけ動かす。音声の余裕は変わらず、
    // 映像は A/V を合わせるためにその分待つ (または早く出る)。残ったずれを 1 秒に 1 ms ずつ詰めて重複・間引きを続けないように
    void shift_for_output_delay(int64_t delta_qpc) {
        std::lock_guard lk(mutex_);
        if (!anchored_ || delta_qpc == 0) return;
        anchor_qpc_ += delta_qpc;
        if (min_video_slack_ != LLONG_MAX) min_video_slack_ += delta_qpc;
        history_count_ = 0;  // 音声の余裕と時間軸の関係が変わったので、前の窓とは比べない
        review_start_locked(now_());
        if (std::llabs(delta_qpc) > qpc_from_ms(kEpochJumpMs)) ++jumps_;
        debug_.push_back(fmt::format("playout: moved {:+.2f} ms (output delay changed)", qpc_to_us(delta_qpc) / 1000.0));
    }

    // 音声は届いているが、出力先が無い (engine の出力遅延が 0)。時間軸は音声の余裕で動かさない
    // (「なし」にしただけで映像の遅延が変わらないように)。届いていることだけ覚える (audio_reported_within)
    void note_audio_without_output() {
        std::lock_guard lk(mutex_);
        last_audio_report_qpc_ = now_();
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
        history_count_ = 0;
        windows_since_move_ = kStepWindows;
        review_start_locked(now_());
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
            push_history_locked();
            ++windows_since_move_;
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
            } else if (const int64_t cut = review_headroom_locked(now); cut > 0) {
                // 揺れの備えを、実際の揺れに合わせて一度に下げた (review_headroom_locked): 余った遅延も一度に詰める
                // (engine は 1 回合わせ直す)。映像側の余裕が足りなければ、その分は詰めない
                const int64_t envelope = history_envelope_locked();
                step = envelope != LLONG_MAX ? std::clamp(envelope, int64_t(0), cut) : 0;
                reason = "headroom cut";
                log_.push_back(
                    fmt::format("playout: audio headroom {:.0f} -> {:.0f} ms (no late audio for {} s); delay reduced {:.1f} ms at once",
                                qpc_to_us(audio_headroom_ + cut) / 1000.0, qpc_to_us(audio_headroom_) / 1000.0, kHeadroomReviewMs / 1000,
                                qpc_to_us(step) / 1000.0));
            } else if (video_error < -jump && video_error <= audio_error) {
                // 映像が 3 フレームを超えて遅れた: 一度に遅らせ、備えを増やす (音声は合わせ直しになる)
                step = video_error;
                reason = "video late";
                if (outage_locked(-video_error, now)) {
                    log_.push_back(
                        fmt::format("playout: video arrived {:.1f} ms late (outage); delay increased, video headroom kept {:.0f} ms",
                                    qpc_to_us(-video_error) / 1000.0, qpc_to_us(video_headroom_) / 1000.0));
                } else {
                    grow_headroom_locked(video_headroom_, -video_error, kVideoHeadroomMaxMs, now, last_video_late_qpc_);
                    log_.push_back(fmt::format("playout: video arrived {:.1f} ms late; delay increased, video headroom {:.0f} ms",
                                               qpc_to_us(-video_error) / 1000.0, qpc_to_us(video_headroom_) / 1000.0));
                }
            } else {
                // 落ち着いた後: その窓のずれではなく、直近 kHistoryWindows 窓のずれの最小 (揺れの悪い側の包絡) で決める。
                // 足りない側は、悪い窓が幅の内側に収まるまで 1 ms ずつ遅らせる。余っている側は、直近の窓がすべて幅を
                // 超えて余っているときだけ詰める。揺れの良い窓に釣られて詰め、次の悪い窓でまた遅らせる、の行き来をしない
                const int64_t envelope = history_envelope_locked();
                const int64_t band = qpc_from_ms(kDeadbandMs);
                step = std::clamp(envelope, -limit, limit);
                if (envelope == LLONG_MAX || history_count_ < kStepWindows || std::llabs(envelope) <= band) {
                    step = 0;
                    reason = "deadband";
                } else if (windows_since_move_ < kStepWindows) {
                    step = 0;  // 前に動かしてから kStepWindows 窓は空ける
                    reason = "wait";
                } else {
                    // クロック差、揺れの備えを減らした分、悪い窓が幅を超えた分: 1 ms 動かす
                    reason = "step";
                }
            }
            if (std::abs(step) > qpc_from_ms(kEpochJumpMs) && settled_) ++jumps_;
            // 1 秒ごとの要約 (debug): 時間軸を動かした量と理由。engine の重複・間引きと並べて見る
            if (step != 0) {
                debug_.push_back(
                    fmt::format("playout: moved {:+.2f} ms ({}); error video {} / audio {} ms, headroom video {:.0f} / audio {:.0f} ms",
                                qpc_to_us(-step) / 1000.0, reason, fmt_ms(video_error), fmt_ms(audio_error),
                                qpc_to_us(video_headroom_) / 1000.0, qpc_to_us(audio_headroom_) / 1000.0));
            }
            if (step != 0) windows_since_move_ = 0;
            anchor_qpc_ -= step;
            settled_ = true;
        }
        window_start_qpc_ = now;
        min_video_slack_ = LLONG_MAX;
        min_audio_slack_ = LLONG_MAX;
    }

    // この窓の最小の余裕を、時間軸によらない形 (余裕 - anchor_qpc_) で覚える。時間軸を動かしても、遅れて届いて遅らせても、
    // 揺れの備えが変わっても、前の窓のずれを今の時間軸で計算し直せる (history_envelope_locked)
    void push_history_locked() {
        auto& w = history_[history_next_];
        w.video = min_video_slack_ != LLONG_MAX ? min_video_slack_ - anchor_qpc_ : LLONG_MAX;
        w.audio = min_audio_slack_ != LLONG_MAX ? min_audio_slack_ - anchor_qpc_ : LLONG_MAX;
        history_next_ = (history_next_ + 1) % kHistoryWindows;
        history_count_ = std::min(history_count_ + 1, kHistoryWindows);
        if (w.audio != LLONG_MAX) {
            review_min_ = std::min(review_min_, w.audio);
            review_max_ = std::max(review_max_, w.audio);
            ++review_windows_;
        }
    }

    // 途切れか (時間軸は遅らせるが、揺れの備えは増やさない)。kOutageLateMs を超える遅れは揺れではなく一時的な途切れで、
    // その直後 kOutageTailMs の間の遅れは、途切れのあとのまとめ届きの残り
    bool outage_locked(int64_t late, int64_t now) {
        if (late > qpc_from_ms(kOutageLateMs)) {
            last_outage_qpc_ = now;
            return true;
        }
        return last_outage_qpc_ != 0 && now - last_outage_qpc_ < qpc_from_ms(kOutageTailMs);
    }

    // 揺れの備えを決め直す間を、今から数え直す
    void review_start_locked(int64_t now) {
        review_start_qpc_ = now;
        review_min_ = LLONG_MAX;
        review_max_ = LLONG_MIN;
        review_windows_ = 0;
    }

    // 音声が kHeadroomReviewMs の間遅れて届かなければ、その間の窓ごとの最小の余裕の散らばりから、要る揺れの備えを
    // 決め直す。今の備えがそれより kHeadroomCutMs を超えて多ければ一度に下げ、下げた量を返す (0 = 下げない)
    int64_t review_headroom_locked(int64_t now) {
        if (now - review_start_qpc_ < qpc_from_ms(kHeadroomReviewMs)) return 0;
        const bool enough = review_windows_ >= kHeadroomReviewMs / 1000 / 2;
        const int64_t spread = review_max_ - review_min_;
        review_start_locked(now);
        if (!enough) return 0;
        // 散らばりに、その半分 (少なくとも kHeadroomMarginMs) を上乗せする。ちょうどにすると、次の揺れで遅れて届いて
        // また増やす、を繰り返す (kHeadroomDecayMs のコメント)
        const int64_t needed = std::max(spread + std::max(spread / 2, qpc_from_ms(kHeadroomMarginMs)), qpc_from_ms(kAudioHeadroomMinMs));
        const int64_t cut = audio_headroom_ - needed;
        if (cut <= qpc_from_ms(kHeadroomCutMs)) return 0;  // 小さい余りは今どおり少しずつ減らす (decay_headroom_locked)
        audio_headroom_ = needed;
        return cut;
    }

    // 直近の窓の、今の時間軸と揺れの備えでの目標とのずれの最小 (LLONG_MAX = 報告なし)
    int64_t history_envelope_locked() const {
        int64_t envelope = LLONG_MAX;
        for (int i = 0; i < history_count_; ++i) {
            const auto& w = history_[(history_next_ + kHistoryWindows - 1 - i) % kHistoryWindows];
            if (w.video != LLONG_MAX) envelope = std::min(envelope, w.video + anchor_qpc_ - buffer_ - video_headroom_);
            if (w.audio != LLONG_MAX) envelope = std::min(envelope, w.audio + anchor_qpc_ - audio_headroom_);
        }
        return envelope;
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
    static constexpr int64_t kBurstMs = 5;    // 遅れて届いたかたまりからこの間に届いたものは、まとめ届きの残り
    static constexpr int64_t kMaxStepMs = 1;  // 鳴り始めてから 1 回 (1 秒) に動かす上限
    // 鳴り始めてからは、目標とのずれがこれ以下なら動かさない。窓 (1 秒) ごとの最小の余裕は、到着の揺れで ±2〜4 ms 動く
    // (DVS + RTMP の実測)。2 ms では揺れのたびに 1 ms ずつ行き来し、同じ向きが重なると engine が 1 秒に数十回重複・間引きした
    static constexpr int64_t kDeadbandMs = 5;
    // 1 ms ずつ動かすのは、前に動かしてからこの窓数空けてから (直近の窓もこの数は要る)。追える速さは 3 秒に 1 ms (333 ppm) で、
    // 設計で追うクロック差 200 ppm (1 秒に 0.2 ms) を超える。毎秒続けて動かすと、engine の重複・間引きが 1 秒に 48 回になり、
    // 2 秒続くと合わせ直しになる (ChannelSource の kSlipRateResync)
    static constexpr int kStepWindows = 3;
    // 落ち着いた後の 1 ms ずつの調整は、直近のこの窓数のずれの最小で決める (揺れの悪い側の包絡)。短いと揺れの悪い窓が
    // 抜けたときに余っていると見て詰めてしまい、長いとクロック差の後追いが遅れる (200 ppm で 10 秒に 2 ms)
    static constexpr int kHistoryWindows = 10;
    // 遅延の余り (3 フレーム超) がこの回数続いたら一度に戻す
    static constexpr int kCatchUpWindows = 2;
    static constexpr int64_t kAudioHeadroomMinMs = 10;  // 音声の揺れの備え (最初の値と下限)
    static constexpr int64_t kAudioHeadroomMaxMs = 200;
    static constexpr int64_t kVideoHeadroomMaxMs = 200;
    static constexpr int64_t kHeadroomGrowMaxMs = 50;  // 1 回の遅れで増やす上限 (一時的な大きな途切れで増やしすぎない)
    // 遅れがこの間なければ備えを 1 ms 減らす。速く減らすと、減っては次の小さな遅れで増える、を繰り返して時間軸が
    // 動き続け、engine がそれを追うサンプルの重複・間引きも増える (5 秒では 1 秒に十数回)
    static constexpr int64_t kHeadroomDecayMs = 30'000;
    // 遅れがこれを超えたら途切れとみなし、揺れの備えを増やさない。回線の揺れは数十 ms まで (実測 ±2〜20 ms)。
    // 備えの 1 回の増分の上限 (kHeadroomGrowMaxMs) の 2 倍を超える遅れ (実機で 868 / 853 ms) は、揺れではなく一時的な途切れ。
    // 増やすと、30 秒に 1 ms ずつしか減らないので、約 1 時間、遅延が 100 ms 以上多いままになった
    static constexpr int64_t kOutageLateMs = 100;
    // 途切れからこの間の遅れも途切れの一部 (途切れの直後のまとめ届きで、24 / 42 ms などの遅れが続いた)
    static constexpr int64_t kOutageTailMs = 3'000;
    // 音声がこの間遅れて届かなければ、揺れの備えを実際の揺れに合わせて決め直す。約 1 分ごとに起きる小さな途切れ (RTMP) で、
    // 下げては遅れて届いて増やす、を毎回繰り返さない長さ
    static constexpr int64_t kHeadroomReviewMs = 60'000;
    static constexpr int64_t kHeadroomMarginMs = 5;  // 決め直す備えの、散らばりへの上乗せの下限
    // 決め直した備えが今よりこれを超えて少なければ、一度に下げて時間軸も一度に詰める (それ以下は decay で少しずつ)
    static constexpr int64_t kHeadroomCutMs = 20;

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
    int excess_windows_ = 0;  // 遅延の余りが 3 フレームを超えた窓が続いた数
    struct WindowSlack {
        int64_t video = LLONG_MAX;  // 窓の最小の余裕 - anchor_qpc_ (push_history_locked)
        int64_t audio = LLONG_MAX;
    };
    WindowSlack history_[kHistoryWindows];  // 直近の窓 (環状)
    int history_next_ = 0;
    int history_count_ = 0;
    int64_t last_outage_qpc_ = 0;     // 最後に途切れた時刻 (outage_locked)
    int64_t review_start_qpc_ = 0;    // 揺れの備えを決め直す間の始まり (review_headroom_locked)
    int64_t review_min_ = LLONG_MAX;  // その間の、窓の最小の音声の余裕 - anchor_qpc_ の最小と最大
    int64_t review_max_ = LLONG_MIN;
    int review_windows_ = 0;
    int windows_since_move_ = kStepWindows;   // 前に時間軸を 1 ms ずつの調整などで動かしてからの窓の数
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
