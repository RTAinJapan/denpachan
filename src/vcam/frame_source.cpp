#include "frame_source.h"

#include <algorithm>
#include <atomic>

#include "convert.h"

namespace denpachan::vcam {

namespace {

uint64_t next_token() {
    static std::atomic<uint32_t> counter{0};
    return (uint64_t(GetCurrentProcessId()) << 32) | (counter.fetch_add(1) + 1);
}

}  // namespace

FrameSource::FrameSource(int cam, const OutputFormat& format)
    : cam_(cam),
      format_(format),
      frame_period_ms_(std::max<DWORD>(1, DWORD(format.frame_duration() / 10'000))),
      token_(next_token()),
      interrupt_event_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}

FrameSource::~FrameSource() { release_reader_slot(); }

void FrameSource::interrupt() { SetEvent(interrupt_event_.get()); }

bool FrameSource::ensure_open() {
    if (header_) return true;
    const int64_t now = qpc_now();
    if (last_open_attempt_ != 0 && now - last_open_attempt_ < qpc_from_ms(1000)) return false;
    last_open_attempt_ = now;
    header_ = open_shared(mapping_, cam_);
    return header_ != nullptr;
}

void FrameSource::ensure_reader_slot() {
    const int64_t now = qpc_now();
    if (slot_ >= 0) {
        auto& r = header_->readers[slot_];
        if (r.owner.load() == token_) {
            r.heartbeat_qpc.store(now);
            return;
        }
        // 期限切れの間に他の読み手に取られた。取り直す。
        slot_ = -1;
        frame_event_.reset();
    }

    for (int i = 0; i < kMaxReaders; ++i) {
        auto& r = header_->readers[i];
        int64_t hb = r.heartbeat_qpc.load();
        if (heartbeat_alive(hb, kReaderTimeoutMs, now)) continue;
        if (!r.heartbeat_qpc.compare_exchange_strong(hb, now)) continue;

        r.owner.store(token_);
        const uint32_t gen = r.generation.load() + 1;
        SharedSecurity sec;
        frame_event_.reset(CreateEventW(sec.get(), FALSE, FALSE, reader_event_name(cam_, i, gen).c_str()));
        r.format.store(format_.fmt.pack());
        r.format_qpc.store(now);
        r.generation.store(gen, std::memory_order_release);
        slot_ = i;
        return;
    }
    // 空きなし: イベントなしで動く (タイムアウトで最新フレームを拾う)
}

void FrameSource::release_reader_slot() {
    if (!header_ || slot_ < 0) return;
    auto& r = header_->readers[slot_];
    if (r.owner.load() == token_) {
        r.format.store(0);
        r.owner.store(0);
        r.heartbeat_qpc.store(0);
    }
    slot_ = -1;
    frame_event_.reset();
}

bool FrameSource::writer_live() const {
    return header_ && header_->writer_state.load() == uint32_t(WriterState::Live) &&
           heartbeat_alive(header_->writer_heartbeat_qpc.load(), kWriterTimeoutMs) &&
           header_->write_seq.load(std::memory_order_acquire) != 0;
}

bool FrameSource::copy_latest(uint8_t* dst) {
    // 書き手に追い越されたら読み直す。回数を少なくすると、高負荷でこのスレッドが止められたときに
    // フレームがあるのに NO SIGNAL を 1 枚出してしまう
    for (int attempt = 0; attempt < 8; ++attempt) {
        const uint64_t seq = header_->write_seq.load(std::memory_order_acquire);
        if (seq == 0) return false;
        const int slot = int(seq % kNumSlots);
        const auto& info = header_->slots[slot];
        const uint32_t w = info.width.load(std::memory_order_relaxed);
        const uint32_t h = info.height.load(std::memory_order_relaxed);
        if (info.seq.load(std::memory_order_relaxed) != seq) continue;
        if (w < 16 || h < 16 || w > kMaxWidth || h > kMaxHeight || (w | h) & 1) return false;

        convert_nv12(slot_data(mapping_.data(), slot), w, h, dst, format_);

        // コピー中に書き手が 2 枚以上進んでいたら、このスロットが上書きされた可能性がある
        std::atomic_thread_fence(std::memory_order_acquire);
        if (header_->write_seq.load(std::memory_order_acquire) - seq < 2) {
            last_seq_ = seq;
            return true;
        }
    }
    return false;
}

void FrameSource::fill_slate(uint8_t* dst) {
    if (slate_.empty()) {
        const auto nv12 = make_slate_nv12(format_.fmt.width, format_.fmt.height, cam_);
        slate_.resize(format_.image_bytes());
        convert_nv12(nv12.data(), format_.fmt.width, format_.fmt.height, slate_.data(), format_);
    }
    memcpy(dst, slate_.data(), slate_.size());
}

bool FrameSource::has_frame() const { return header_ && header_->write_seq.load(std::memory_order_acquire) != 0; }

bool FrameSource::fill(uint8_t* dst) {
    if (ensure_open()) ensure_reader_slot();

    // 受信中なら新しいフレームを待つ (live_wait_ms)。それ以外は 1 フレーム期間ごとに出す。
    // 開いた直後でまだフレームがなく、書き手が生きているときは、書き手が保持しているフレームを
    // 書いてくれるのを少し待つ (誰も見ていない間は書き手が書き込みを省いているため)。
    const bool live = writer_live();
    const bool wait_for_writer =
        first_fill_ && !has_frame() && header_ && heartbeat_alive(header_->writer_heartbeat_qpc.load(), kWriterTimeoutMs);
    first_fill_ = false;
    if (!live || header_->write_seq.load(std::memory_order_acquire) == last_seq_) {
        const DWORD timeout = wait_for_writer ? 200 : live ? live_wait_ms() : frame_period_ms_;
        HANDLE handles[2] = {interrupt_event_.get(), frame_event_.get()};
        WaitForMultipleObjects(frame_event_ ? 2 : 1, handles, FALSE, timeout);
    }

    // 一度でもフレームが書かれていれば、途絶中でも書き手が落ちていても最後のフレームを出す
    if (has_frame()) {
        const uint64_t prev_seq = last_seq_;
        const bool fresh = header_->write_seq.load(std::memory_order_acquire) != last_seq_;
        if (copy_latest(dst)) {
            if (fresh) measure_interval(prev_seq);
            return fresh;
        }
    }
    fill_slate(dst);
    return false;
}

void FrameSource::measure_interval(uint64_t prev_seq) {
    const int64_t now = qpc_now();
    if (last_fresh_qpc_ != 0 && prev_seq != 0 && last_seq_ > prev_seq) {
        // 前に読んでから書き手が何枚進んだかで割る (読み手が間に合わず飛ばしたフレームの分)
        const int64_t us = qpc_to_us(now - last_fresh_qpc_) / int64_t(last_seq_ - prev_seq);
        // 途絶をまたいだ間隔は映像の間隔ではないので使わない
        if (us < kMaxLiveWaitMs * 1000) {
            source_interval_us_ = source_interval_us_ == 0 ? us : source_interval_us_ + (us - source_interval_us_) / 8;
        }
    }
    last_fresh_qpc_ = now;
}

DWORD FrameSource::live_wait_ms() const {
    // 映像のフレーム間隔の 1.5 倍 (届く時刻の揺れの分の余裕)。出力の fps より遅い映像 (60 fps で出す 30 fps の映像など) で、
    // 次のフレームが届く直前に待ちが切れて同じ絵をもう 1 枚挟まないように。最低 2 フレーム期間、最大 kMaxLiveWaitMs。
    const DWORD min_ms = frame_period_ms_ * 2;
    const DWORD source_ms = DWORD(source_interval_us_ * 3 / 2 / 1000);
    return std::max(min_ms, std::min(source_ms, kMaxLiveWaitMs));
}

}  // namespace denpachan::vcam
