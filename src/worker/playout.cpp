#include "playout.h"

#include <avrt.h>
#include <spdlog/spdlog.h>

#include <algorithm>

namespace denpachan::worker {

using namespace std::chrono_literals;

Playout::Playout(PlayoutOptions options, PlayoutClock& clock, FrameQueue& queue, VcamWriter* writer, PreviewWriter* preview,
                 ChannelMetrics& metrics)
    : options_(options), clock_(clock), queue_(queue), writer_(writer), preview_(preview), metrics_(metrics) {
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
    PlayoutCore core(clock_, stall);  // フレームごとの判断 (試験と同じコード)

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
                // 時間軸を捨てるのは音声も来ていないときだけ。音声が届き続けているなら、映像が戻ったときも同じ時間軸で
                // 出せる (捨てると、次に届いた音声で取り直されて、音声がフェードして合わせ直しになる)
                const bool audio_flowing = clock_.audio_reported_within(stall);
                if (!audio_flowing) clock_.invalidate();
                if (writer_) writer_->set_state(vcam::WriterState::Hold);
                metrics_.video_state = uint32_t(worker_status::VideoState::Hold);
                spdlog::info("playout: stalled, holding last frame{}", audio_flowing ? " (audio continues)" : "");
            }
            if (writer_) writer_->refresh(held.get());
            if (preview_) preview_->refresh(held.get());
            continue;
        }

        const FrameMeta meta{f->session, f->pts_us, f->arrival_qpc};
        int64_t target = 0;
        if (core.on_frame(meta, now, queue_.size(), target) != PlayoutCore::Decision::Present) {
            ++metrics_.frames_dropped_late;  // 取り残された、または遅れたフレーム (PlayoutCore)
            continue;
        }
        // 待っている間に音声側の都合で時間軸が遅らされることがあるので、待ち終わったら確かめ直す
        bool stopped = false;
        while (target > qpc_now()) {
            if (!wait_until(target)) {
                stopped = true;
                break;
            }
            target = core.target(meta);
        }
        if (stopped) break;

        if (writer_) writer_->write(f->frame.get());
        if (preview_) preview_->submit(f->frame.get());  // 縮小は別スレッド
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
