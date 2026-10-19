#include "vcam_writer.h"

#include <spdlog/spdlog.h>

namespace denpachan::worker {

using namespace denpachan::vcam;

VcamWriter::~VcamWriter() { close(); }

bool VcamWriter::open(int cam) {
    close();
    header_ = open_shared(mapping_, cam);
    if (!header_) {
        spdlog::error("vcam {}: cannot open shared memory", cam);
        return false;
    }
    const uint32_t pid = GetCurrentProcessId();
    const uint32_t other = header_->writer_pid.load();
    if (other != 0 && other != pid && heartbeat_alive(header_->writer_heartbeat_qpc.load(), kWriterTimeoutMs)) {
        // 設計上は 1 つの Cam に書き手は 1 つ (ルーティングの重複は UI で弾く)
        spdlog::warn("vcam {}: another writer (pid {}) is active; taking over", cam, other);
    }
    cam_ = cam;
    header_->writer_pid.store(pid);
    // 前の書き手 (再起動前の自分など) が書いたフレームは消さない。読み手はそれを出し続ける。
    const bool has_frame = header_->write_seq.load() != 0;
    header_->writer_state.store(uint32_t(has_frame ? WriterState::Hold : WriterState::NoSignal));
    heartbeat();
    spdlog::info("vcam {}: opened", cam);
    return true;
}

void VcamWriter::close() {
    if (header_) {
        if (header_->writer_pid.load() == GetCurrentProcessId()) {
            header_->writer_state.store(uint32_t(WriterState::Absent));
            header_->writer_heartbeat_qpc.store(0);
            header_->writer_pid.store(0);
            notify_readers();  // すぐスレートに切り替えてもらう
        }
        header_ = nullptr;
    }
    for (auto& e : events_) e = ReaderEvent{};
    mapping_.close();
}

void VcamWriter::heartbeat() {
    if (header_) header_->writer_heartbeat_qpc.store(qpc_now());
}

void VcamWriter::set_state(WriterState state) {
    if (!header_) return;
    if (header_->writer_state.exchange(uint32_t(state)) != uint32_t(state)) notify_readers();
}

std::optional<Format> VcamWriter::requested_format() const {
    if (!header_) return std::nullopt;
    const int64_t now = qpc_now();
    std::optional<Format> best;
    int64_t best_qpc = 0;
    for (const auto& r : header_->readers) {
        if (!heartbeat_alive(r.heartbeat_qpc.load(), kReaderTimeoutMs, now)) continue;
        const Format f = Format::unpack(r.format.load());
        if (!f.valid()) continue;
        const int64_t t = r.format_qpc.load();
        if (!best || t > best_qpc) {
            best = f;
            best_qpc = t;
        }
    }
    return best;
}

void VcamWriter::write(const AVFrame* frame) {
    const auto fmt = requested_format();
    if (!fmt) {
        skipped_ = true;  // 誰も見ていない。読み手が来たら refresh で書き直す
        return;
    }

    const uint64_t seq = header_->write_seq.load(std::memory_order_relaxed) + 1;
    const int slot = int(seq % kNumSlots);
    if (!scaler_.scale(frame, slot_data(mapping_.data(), slot), int(fmt->width), int(fmt->height))) return;
    skipped_ = false;
    written_format_ = *fmt;

    auto& info = header_->slots[slot];
    info.width.store(fmt->width, std::memory_order_relaxed);
    info.height.store(fmt->height, std::memory_order_relaxed);
    info.pts_qpc.store(qpc_now(), std::memory_order_relaxed);
    info.seq.store(seq, std::memory_order_relaxed);
    header_->write_seq.store(seq, std::memory_order_release);
    notify_readers();
}

void VcamWriter::refresh(const AVFrame* held) {
    if (!header_ || !held) return;
    const auto fmt = requested_format();
    if (!fmt) return;
    // 読み手がいない間に省いたフレームがある / 読み手の要求解像度が変わった
    if (skipped_ || !(*fmt == written_format_)) write(held);
}

void VcamWriter::notify_readers() {
    const int64_t now = qpc_now();
    for (int i = 0; i < kMaxReaders; ++i) {
        const auto& r = header_->readers[i];
        if (!heartbeat_alive(r.heartbeat_qpc.load(), kReaderTimeoutMs, now)) continue;
        const uint32_t gen = r.generation.load(std::memory_order_acquire);
        auto& e = events_[i];
        if (gen != e.generation) {
            e.generation = gen;
            e.event.reset(OpenEventW(EVENT_MODIFY_STATE, FALSE, reader_event_name(cam_, i, gen).c_str()));
        }
        if (e.event) SetEvent(e.event.get());
    }
}

}  // namespace denpachan::worker
