#include "preview_writer.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace denpachan::worker {

using namespace denpachan::preview;

namespace {

constexpr int64_t kIntervalMs = 66;        // 最大 15fps
constexpr int64_t kRefreshAfterMs = 1000;  // 途絶中、これだけ書いていなければ保持中のフレームを書く

}  // namespace

PreviewWriter::~PreviewWriter() { close(); }

bool PreviewWriter::open(int ch) {
    header_ = open_shared(mapping_, ch);
    if (!header_) {
        spdlog::warn("preview: cannot open shared memory");
        return false;
    }
    stop_ = false;
    thread_ = std::thread(&PreviewWriter::run, this);
    return true;
}

void PreviewWriter::close() {
    {
        std::lock_guard lk(mutex_);
        stop_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    sws_freeContext(sws_);
    sws_ = nullptr;
    header_ = nullptr;
    mapping_.close();
}

bool PreviewWriter::reader_active() const {
    if (!header_) return false;
    const int64_t hb = header_->reader_heartbeat_qpc.load(std::memory_order_relaxed);
    return hb != 0 && qpc_now() - hb < qpc_from_ms(kReaderTimeoutMs);
}

void PreviewWriter::submit(const AVFrame* frame) {
    if (!frame || !reader_active()) return;
    const int64_t now = qpc_now();
    if (now - last_submit_qpc_ < qpc_from_ms(kIntervalMs)) return;
    last_submit_qpc_ = now;
    FramePtr ref(av_frame_alloc());
    if (!ref || av_frame_ref(ref.get(), frame) < 0) return;
    {
        std::lock_guard lk(mutex_);
        pending_ = std::move(ref);  // 前のが残っていれば捨てる (最新だけ書けばよい)
    }
    cv_.notify_one();
}

void PreviewWriter::refresh(const AVFrame* held) {
    if (!header_ || !held || !reader_active()) return;
    if (qpc_now() - header_->writer_qpc.load(std::memory_order_relaxed) < qpc_from_ms(kRefreshAfterMs)) return;
    submit(held);
}

bool PreviewWriter::convert(const AVFrame* src, uint8_t* dst) {
    // 640x360 にアスペクト比を保って収める (黒帯)
    double sar = 1.0;
    if (src->sample_aspect_ratio.num > 0 && src->sample_aspect_ratio.den > 0) sar = av_q2d(src->sample_aspect_ratio);
    const double dar = src->width * sar / src->height;
    int rw = int(kWidth);
    int rh = int(kHeight);
    if (double(kWidth) / kHeight > dar) {
        rw = std::max(2, int(std::lround(kHeight * dar)) & ~1);
    } else {
        rh = std::max(2, int(std::lround(kWidth / dar)) & ~1);
    }
    const int x0 = (int(kWidth) - rw) / 2;
    const int y0 = (int(kHeight) - rh) / 2;

    sws_ = sws_getCachedContext(sws_, src->width, src->height, AVPixelFormat(src->format), rw, rh, AV_PIX_FMT_BGRA, SWS_BILINEAR, nullptr,
                                nullptr, nullptr);
    if (!sws_) return false;
    if (rw != int(kWidth) || rh != int(kHeight)) {
        for (uint32_t y = 0; y < kHeight; ++y) {
            uint32_t* row = reinterpret_cast<uint32_t*>(dst + size_t(y) * kStride);
            std::fill(row, row + kWidth, 0xFF000000u);  // 不透明の黒
        }
    }
    uint8_t* planes[4] = {dst + size_t(y0) * kStride + size_t(x0) * 4, nullptr, nullptr, nullptr};
    const int strides[4] = {int(kStride), 0, 0, 0};
    return sws_scale(sws_, src->data, src->linesize, 0, src->height, planes, strides) > 0;
}

void PreviewWriter::run() {
    for (;;) {
        FramePtr frame;
        {
            std::unique_lock lk(mutex_);
            cv_.wait(lk, [&] { return stop_ || pending_; });
            if (stop_) return;
            frame = std::move(pending_);
        }
        const uint64_t seq = header_->seq.load(std::memory_order_relaxed) + 1;
        if (!convert(frame.get(), slot_data(mapping_.data(), int(seq % kSlots)))) continue;
        header_->writer_qpc.store(qpc_now(), std::memory_order_relaxed);
        header_->seq.store(seq, std::memory_order_release);
    }
}

}  // namespace denpachan::worker
