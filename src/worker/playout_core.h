#pragma once
// 出力スレッド (Playout) の判断のうち、時計と時刻だけで決まる部分 (待つ・書くは Playout)。
// 試験 (audio_timing_test) も同じコードで映像の出力を動かす。

#include <spdlog/spdlog.h>

#include <cstdint>
#include <cstdlib>

#include "playout_clock.h"

namespace denpachan::worker {

// 出力スレッドが見るフレームの情報 (DecodedFrame のうち、時刻の部分)
struct FrameMeta {
    uint64_t session = 0;
    int64_t pts_us = 0;
    int64_t arrival_qpc = 0;  // デコード完了時刻
};

class PlayoutCore {
public:
    enum class Decision {
        Present,    // target_qpc に出す (待つ間に時間軸が動いたら target() で取り直す)
        DropStale,  // 取り残されたフレーム (下)
        DropLate,   // 出す時刻を過ぎていて、後ろに新しいフレームが待っている
    };

    PlayoutCore(PlayoutClock& clock, int64_t stall_qpc) : clock_(clock), stall_(stall_qpc) {}

    // キューから取り出したフレームをどうするか。queued: 後ろに待っているフレームの数
    Decision on_frame(const FrameMeta& f, int64_t now, size_t queued, int64_t& target_qpc) {
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

        // 余裕を報告すると、時計が遅延を目標に保つよう時間軸を動かすことがある (設計書 §3.5)
        const int64_t first_target = clock_.due(f.session, f.pts_us, f.arrival_qpc);
        clock_.report_video(first_target - f.arrival_qpc, frame_us_);
        target_qpc = target(f);

        // 遅れたフレームは、後ろに新しいフレームが待っていれば捨てる
        if (now - target_qpc > qpc_from_us(frame_us_) && queued > 0) return Decision::DropLate;
        return Decision::Present;
    }

    // 出す時刻 (待っている間に音声側の都合で時間軸が遅らされることがあるので、待ち終わったら確かめ直す)
    int64_t target(const FrameMeta& f) { return clock_.due(f.session, f.pts_us, f.arrival_qpc); }

    int64_t frame_us() const { return frame_us_; }

private:
    // タイムスタンプが飛んでも、今の時間軸で出す時刻のずれ (余裕と再生バッファの差) がこれ以内なら時間軸を取り直さない
    static constexpr int64_t kOffTimelineUs = 500'000;

    PlayoutClock& clock_;
    const int64_t stall_;
    uint64_t last_session_ = 0;
    int64_t last_pts_us_ = 0;
    int64_t last_arrival_qpc_ = 0;
    int64_t frame_us_ = 16'667;
};

}  // namespace denpachan::worker
