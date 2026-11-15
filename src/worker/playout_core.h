#pragma once
// 出力スレッド (Playout) の判断のうち、時計と時刻だけで決まる部分 (待つ・書くは Playout)。
// 試験 (audio_timing_test) も同じコードで映像の出力を動かす。

#include <spdlog/spdlog.h>

#include <cstdint>
#include <cstdlib>

#include "basic_frame_queue.h"
#include "playout_clock.h"

// 映像の待ち行列を時間の幅で持つ (試験 audio_timing_test が、この版かどうかを見る)
#define DENPACHAN_FRAME_QUEUE_SPAN 1

namespace denpachan::worker {

// 映像の待ち行列 (FrameQueue) の大きさ。出力スレッドは先頭のフレームを出す時刻まで待つので、待ち行列はとりうる最大の遅延
// (再生バッファ + 時計が遅らせうる量 PlayoutClock::kMaxExtraDelayMs + 余裕) を覆う幅にする。足りないと、遅延がそれを
// 超えた間は、待つ間に後ろのフレームが押し出されて毎秒 1〜2 枚しか出ない (24 枚で 400 ms だったとき、遅延 900 ms で起きた)。
// 枚数の上限は、その幅を kVideoQueueMaxFps で覆う数 (再生バッファ 0 で 80 枚)。メモリは満杯のとき 1 枚 (NV12) ×枚数で、
// 1920×1080 で約 250 MB、1536×864 で約 160 MB。満杯になるのは遅延が大きい間だけ (落ち着いていれば遅延の分だけ)
inline constexpr int64_t kVideoQueueMarginMs = 100;
inline constexpr int64_t kVideoQueueMaxFps = 60;
constexpr int64_t video_queue_span_ms(int buffer_ms) { return buffer_ms + PlayoutClock::kMaxExtraDelayMs + kVideoQueueMarginMs; }
constexpr size_t video_queue_frames(int buffer_ms) { return size_t((video_queue_span_ms(buffer_ms) * kVideoQueueMaxFps + 999) / 1000) + 2; }
static_assert(video_queue_frames(0) * 1000 >= size_t(video_queue_span_ms(0) * kVideoQueueMaxFps), "frames must cover the span at 60 fps");

inline FrameQueueLimits video_queue_limits(int buffer_ms) {
    return FrameQueueLimits{video_queue_frames(buffer_ms), qpc_from_ms(video_queue_span_ms(buffer_ms))};
}

// 出力スレッドが見るフレームの情報 (DecodedFrame のうち、時刻の部分)
struct FrameMeta {
    uint64_t session = 0;
    int64_t pts_us = 0;
    int64_t arrival_qpc = 0;  // デコード完了時刻
};

class PlayoutCore {
public:
    enum class Decision {
        Present,          // target_qpc に出す (待つ間は keep_waiting で確かめる)
        DropStale,        // 取り残されたフレーム (下)
        DropLate,         // 出す時刻を過ぎていて、後ろに新しいフレームが待っている
        DropOffTimeline,  // 今の時間軸から外れたフレーム: 出す時刻が、待てる上限 (max_wait_) より先になる
    };

    PlayoutCore(PlayoutClock& clock, int64_t stall_qpc)
        : clock_(clock),
          stall_(stall_qpc),
          max_wait_(clock.buffer_qpc() + qpc_from_ms(PlayoutClock::kMaxExtraDelayMs + kVideoQueueMarginMs) + stall_qpc) {}

    // キューから取り出したフレームをどうするか。queued: 後ろに待っているフレームの数
    Decision on_frame(const FrameMeta& f, int64_t now, size_t queued, int64_t& target_qpc) {
        if (restart_pending_) {
            // 映像が遅延に追いつけなかった (check_progress): このフレームで時間軸を作り直す
            restart_pending_ = false;
            clock_.restart(f.session, f.pts_us, f.arrival_qpc);
            ++restarts_;
        }
        // 取り残されたフレーム: タイムスタンプは直前と連続しているのに、到着が途絶判定より遅れた。
        // 送出停止時にデマクサ内に残っていた末尾が切断後に出てくるもので、出すと保持中の絵が
        // 一瞬だけ切り替わって live 扱いになる。
        const int64_t gap_us = f.pts_us - last_pts_us_;
        if (f.session == last_session_ && gap_us > 0 && gap_us < 1'000'000 && f.arrival_qpc - last_arrival_qpc_ > stall_) {
            last_arrival_qpc_ = f.arrival_qpc;
            last_pts_us_ = f.pts_us;
            return Decision::DropStale;
        }
        last_arrival_qpc_ = f.arrival_qpc;

        // 時間軸の取り直し: タイムスタンプが 1 秒以上飛び、今の時間軸ではこのフレームを出す時刻が大きくずれるとき
        // (送出側の時計が飛んだ、巻き戻ったなど)。映像だけが途切れて音声は届き続けていた、送出側が止まってその分だけ
        // タイムスタンプが進んだ、などで、今の時間軸のままで出す時刻が合っている (余裕が再生バッファからずれていない) なら
        // 取り直さない (取り直すと音声がフェードして合わせ直しになる)。新しい接続は due() が取り直す。
        // 届いた間隔とは比べない (HW デコーダは次の入力まで前のフレームを返さないので、途切れの前のフレームの到着が遅く見える)
        const bool ts_jumped = gap_us < -1'000'000 || gap_us > 1'000'000;
        if (f.session == last_session_ && clock_.anchored_for(f.session) && ts_jumped) {
            const int64_t off_us = qpc_to_us(clock_.due(f.session, f.pts_us, f.arrival_qpc) - f.arrival_qpc - clock_.buffer_qpc());
            if (std::llabs(off_us) > kOffTimelineUs) {
                spdlog::info("playout: re-anchor (gap {} ms, {} ms off the current timeline)", gap_us / 1000, off_us / 1000);
                clock_.reanchor(f.session, f.pts_us, f.arrival_qpc);
            } else {
                spdlog::info("playout: timestamps jumped {} ms but still fit the current timeline ({:+} ms); keeping it", gap_us / 1000,
                             off_us / 1000);
            }
        } else if (gap_us > 0 && gap_us < 100'000) {
            frame_us_ = gap_us;
        }
        last_session_ = f.session;
        last_pts_us_ = f.pts_us;

        // 今の時間軸から外れたフレーム: 出す時刻が、待てる上限より先になる。捨てる (待つと、その間ずっと映像が止まる)。
        // 同じ接続のままタイムスタンプが巻き戻り (配信者が配信を止めて始め直したのを、中継が同じ接続のまま流す、など)、
        // 途絶のあと新しい音声が先に届いて時間軸を取り直していると、デコーダに残っていた巻き戻る前の映像が、そのあとに
        // 出てくる (デコーダは、後のフレームが入るまで前のフレームを返さない)。タイムスタンプは直前の映像と連続している
        // ので上の取り直しには当たらず、出す時刻は巻き戻った幅 (前の配信の長さ) だけ先になる。捨てれば、続いて届く新しい
        // 映像は今の時間軸でそのまま出せる (取り直すと、巻き戻る前の絵が一瞬出て、新しい映像でもう一度取り直しになる)。
        // 外れたフレームばかりが続くとき (映像と音声のタイムスタンプが大きく離れた送出元など) は、check_progress が
        // 時間軸を作り直す
        const int64_t first_target = clock_.due(f.session, f.pts_us, f.arrival_qpc);
        if (first_target - now > max_wait_) {
            note_off_timeline(first_target - now);
            return Decision::DropOffTimeline;
        }
        if (off_timeline_frames_ > 0) {
            spdlog::info("playout: dropped {} frames that were off the current timeline", off_timeline_frames_);
            off_timeline_frames_ = 0;
        }

        // 余裕を報告すると、時計が遅延を目標に保つよう時間軸を動かすことがある (設計書 §3.5)
        last_delay_qpc_ = first_target - f.arrival_qpc;
        clock_.report_video(first_target - f.arrival_qpc, frame_us_);
        target_qpc = clock_.due(f.session, f.pts_us, f.arrival_qpc);

        // 遅れたフレームは、後ろに新しいフレームが待っていれば捨てる
        if (now - target_qpc > qpc_from_us(frame_us_) && queued > 0) return Decision::DropLate;
        return Decision::Present;
    }

    // 出す時刻を待っている間に呼ぶ (出力スレッドは kWaitCheckMs ごとに起きて呼ぶ)。出す時刻を取り直す (待っている間に、
    // 音声側の都合で時間軸が動くことがある)。false: 待つのをやめて、このフレームを捨てる。
    // 出力スレッドが 1 枚のフレームを待って長く止まらないための保険 (止まっている間は、後ろのフレームが待ち行列から溢れ続ける):
    // - 映像が出せていない状態が続いて、時間軸を作り直すことになった (check_progress。待っている間も呼ぶ)
    // - 待っている間に時間軸が動いて、出す時刻が待てる上限より先になった
    bool keep_waiting(const FrameMeta& f, int64_t now, int64_t& target_qpc) {
        target_qpc = clock_.due(f.session, f.pts_us, f.arrival_qpc);
        if (restart_pending_) return false;
        if (target_qpc - now > max_wait_) {
            note_off_timeline(target_qpc - now);
            return false;
        }
        return true;
    }

    // フレームを出した。途絶から戻った (保持 → live) なら true
    bool on_presented(int64_t now) {
        last_output_qpc_ = now;
        const bool resumed = !live_;
        live_ = true;
        return resumed;
    }

    // 待ち行列が空だった (フレームが届いていない)。最後に出してから途絶とみなす時間が過ぎたら、途絶に入る (live → 保持)。
    // 時間軸を捨てるのは音声も来ていないときだけ。音声が届き続けているなら、映像が戻ったときも同じ時間軸で出せる
    // (捨てると、次に届いた音声で取り直されて、音声がフェードして合わせ直しになる)
    enum class Idle { None, Stalled, StalledAudioContinues };
    Idle on_idle(int64_t now) {
        if (!live_ || now - last_output_qpc_ <= stall_) return Idle::None;
        live_ = false;
        const bool audio_flowing = clock_.audio_reported_within(stall_);
        if (!audio_flowing) clock_.invalidate();
        return audio_flowing ? Idle::StalledAudioContinues : Idle::Stalled;
    }

    int64_t frame_us() const { return frame_us_; }

    // 保険: 映像が出せていない状態を打ち切る。1 秒ごとに、その間に届いた (デコードした) 枚数・待ち行列から溢れた枚数・
    // 出した枚数を比べ、溢れが届いた枚数の半分を超えたか、出した枚数が届いた枚数の半分を下回った秒が kStuckSeconds 続いたら、
    // 次のフレームで時間軸を作り直す (PlayoutClock::restart)。待ち行列はとりうる最大の遅延を覆う幅なので (video_queue_limits)、
    // 普段は動かない。received などは累計 (呼ぶたびに渡す)
    void check_progress(int64_t now, uint64_t received, uint64_t queue_dropped, uint64_t presented) {
        if (progress_at_ == 0) {
            progress_at_ = now, progress_received_ = received, progress_dropped_ = queue_dropped, progress_presented_ = presented;
            return;
        }
        if (now - progress_at_ < qpc_from_ms(1000)) return;
        const uint64_t got = received - progress_received_;
        const uint64_t dropped = queue_dropped - progress_dropped_;
        const uint64_t shown = presented - progress_presented_;
        progress_at_ = now, progress_received_ = received, progress_dropped_ = queue_dropped, progress_presented_ = presented;
        const bool stuck = got >= kStuckMinFrames && (dropped * 2 > got || shown * 2 < got);
        stuck_seconds_ = stuck ? stuck_seconds_ + 1 : 0;
        if (stuck_seconds_ >= kStuckSeconds && !restart_pending_) {
            stuck_seconds_ = 0;
            restart_pending_ = true;
            spdlog::info("playout: video cannot keep up with the delay ({} ms, output {} fps); re-anchoring",
                         qpc_to_us(last_delay_qpc_) / 1000, shown);
        }
    }

    // check_progress で時間軸を作り直した回数 (画面のイベント欄に出す)
    uint64_t restarts() const { return restarts_; }

    // 出す時刻を待っている間に、出力スレッドが起きて keep_waiting と check_progress を呼ぶ間隔
    static constexpr int64_t kWaitCheckMs = 50;

private:
    void note_off_timeline(int64_t ahead_qpc) {
        if (off_timeline_frames_++ == 0) {
            spdlog::info("playout: dropping frames that are off the current timeline (due {} ms ahead; the timestamps went back?)",
                         qpc_to_us(ahead_qpc) / 1000);
        }
    }

    static constexpr int kStuckSeconds = 3;          // 映像が出せていない秒がこれだけ続いたら作り直す
    static constexpr uint64_t kStuckMinFrames = 10;  // 1 秒に届いた枚数がこれ未満なら見ない (届いていない間は途絶の扱い)

    // タイムスタンプが飛んでも、今の時間軸で出す時刻のずれ (余裕と再生バッファの差) がこれ以内なら時間軸を取り直さない
    static constexpr int64_t kOffTimelineUs = 500'000;

    PlayoutClock& clock_;
    const int64_t stall_;
    // 1 枚のフレームを、出す時刻まで待てる上限: 待ち行列の幅 (再生バッファ + 時計が遅らせうる量 + 余裕。video_queue_span_ms)
    // に、1 回の途切れで一時的に遅らされうる量 (途絶とみなす時間まで。それより長ければ途絶になり、時間軸を捨てる) を足した
    // もの。出す時刻がこれより先のフレームは、今の時間軸のものではない
    const int64_t max_wait_;
    int off_timeline_frames_ = 0;  // 今の時間軸から外れたフレームが続いた枚数 (ログ用)
    uint64_t last_session_ = 0;
    int64_t last_pts_us_ = 0;
    int64_t last_arrival_qpc_ = 0;
    int64_t frame_us_ = 16'667;
    int64_t last_delay_qpc_ = 0;  // 最後のフレームの、届いてから出す時刻まで
    bool live_ = false;           // フレームを出している (false: まだ出していない、または途絶で最後のフレームを保持している)
    int64_t last_output_qpc_ = 0;
    int64_t progress_at_ = 0;  // check_progress で前に数えた時刻と、そのときの累計
    uint64_t progress_received_ = 0;
    uint64_t progress_dropped_ = 0;
    uint64_t progress_presented_ = 0;
    int stuck_seconds_ = 0;
    bool restart_pending_ = false;
    uint64_t restarts_ = 0;
};

}  // namespace denpachan::worker
