#include "playout.h"

#include <avrt.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <climits>

namespace denpachan::worker {

using namespace std::chrono_literals;

Playout::Playout(PlayoutOptions options, PlayoutClock& clock, FrameQueue& queue, VcamWriter* writer,
                 ChannelMetrics& metrics)
    : options_(options), clock_(clock), queue_(queue), writer_(writer), metrics_(metrics) {
    timer_.reset(CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS));
    if (!timer_) timer_.reset(CreateWaitableTimerW(nullptr, FALSE, nullptr));
}

Playout::~Playout() { stop(); }

void Playout::start() {
    stop_ = false;
    thread_ = std::thread(&Playout::run, this);
}

void Playout::stop() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
}

bool Playout::wait_until(int64_t target_qpc) {
    while (!stop_) {
        if (writer_) writer_->heartbeat();
        metrics_.heartbeat_playout();
        const int64_t remaining = target_qpc - qpc_now();
        if (remaining <= 0) return true;
        const int64_t chunk_us = std::min<int64_t>(qpc_to_us(remaining), 50'000);
        LARGE_INTEGER due;
        due.QuadPart = -chunk_us * 10;  // 相対時間 (100ns 単位)
        if (timer_ && SetWaitableTimer(timer_.get(), &due, 0, nullptr, nullptr, FALSE)) {
            WaitForSingleObject(timer_.get(), INFINITE);
        } else {
            Sleep(DWORD(std::max<int64_t>(1, chunk_us / 1000)));
        }
    }
    return false;
}

void Playout::run() {
    DWORD task_index = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Playback", &task_index);

    const int64_t stall = qpc_from_ms(options_.stall_ms);

    uint64_t last_session = 0;
    int64_t last_pts_us = 0;
    int64_t last_arrival_qpc = 0;
    int64_t frame_us = 16'667;

    bool live = false;
    int64_t last_output_qpc = 0;
    FramePtr held;  // 最後に出したフレーム (途絶中に読み手が来たら書き直す)

    const int64_t test_hang_at =
        options_.test_hang_after_sec > 0 ? qpc_now() + qpc_from_ms(int64_t(options_.test_hang_after_sec) * 1000) : 0;

    while (!stop_) {
        if (test_hang_at != 0 && qpc_now() >= test_hang_at) {
            spdlog::warn("playout: test hang");
            Sleep(INFINITE);
        }
        if (writer_) writer_->heartbeat();
        metrics_.heartbeat_playout();
        auto f = queue_.pop(50ms);
        const int64_t now = qpc_now();

        if (!f) {
            // 途絶中は最後のフレームを出し続ける (NO SIGNAL にはしない)
            if (live && now - last_output_qpc > stall) {
                live = false;
                clock_.invalidate();
                if (writer_) writer_->set_state(vcam::WriterState::Hold);
                metrics_.video_state = uint32_t(worker_status::VideoState::Hold);
                spdlog::info("playout: stalled, holding last frame");
            }
            if (writer_) writer_->refresh(held.get());
            continue;
        }

        // 取り残されたフレーム: タイムスタンプは直前と連続しているのに、到着が途絶判定より遅れた。
        // 送出停止時にデマクサ内に残っていた末尾が切断後に出てくるもので、出すと保持中の絵が
        // 一瞬だけ切り替わって live 扱いになる。
        const int64_t gap_us = f->pts_us - last_pts_us;
        if (f->session == last_session && gap_us > 0 && gap_us < 1'000'000 &&
            f->arrival_qpc - last_arrival_qpc > stall) {
            last_arrival_qpc = f->arrival_qpc;
            last_pts_us = f->pts_us;
            ++metrics_.frames_dropped_late;
            continue;
        }
        last_arrival_qpc = f->arrival_qpc;

        // 時間軸の取り直し: タイムスタンプの飛び (1 秒以上)。新しい接続は due() が取り直す。
        if (f->session == last_session && clock_.anchored_for(f->session) &&
            (gap_us < -1'000'000 || gap_us > 1'000'000)) {
            spdlog::info("playout: re-anchor (gap {} ms)", gap_us / 1000);
            clock_.reanchor(f->session, f->pts_us, f->arrival_qpc);
        } else if (gap_us > 0 && gap_us < 100'000) {
            frame_us = gap_us;
        }
        last_session = f->session;
        last_pts_us = f->pts_us;

        // 余裕を報告すると、時計が遅延を目標に保つよう時間軸を動かすことがある (設計書 §3.5)
        const int64_t first_target = clock_.due(f->session, f->pts_us, f->arrival_qpc);
        clock_.report_video(first_target - f->arrival_qpc, frame_us);
        int64_t target = clock_.due(f->session, f->pts_us, f->arrival_qpc);

        // 遅れたフレームは、後ろに新しいフレームが待っていれば捨てる
        if (now - target > qpc_from_us(frame_us) && queue_.size() > 0) {
            ++metrics_.frames_dropped_late;
            continue;
        }
        // 待っている間に音声側の都合で時間軸が遅らされることがあるので、待ち終わったら確かめ直す
        bool stopped = false;
        while (target > qpc_now()) {
            if (!wait_until(target)) {
                stopped = true;
                break;
            }
            target = clock_.due(f->session, f->pts_us, f->arrival_qpc);
        }
        if (stopped) break;

        if (writer_) writer_->write(f->frame.get());
        last_output_qpc = qpc_now();
        ++metrics_.frames_presented;
        metrics_.delay_us = qpc_to_us(last_output_qpc - f->arrival_qpc);
        held = std::move(f->frame);
        if (!live) {
            live = true;
            if (writer_) writer_->set_state(vcam::WriterState::Live);
            metrics_.video_state = uint32_t(worker_status::VideoState::Live);
            spdlog::info("playout: live");
        }
    }

    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
}

}  // namespace denpachan::worker
