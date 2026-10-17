#include "decoder.h"

#include <spdlog/spdlog.h>

#include "denpachan/win_util.h"

namespace denpachan::worker {

namespace {

constexpr int kIoBufferSize = 64 * 1024;
constexpr int kMaxConsecutiveErrors = 60;  // これを超えたら HW デコードをやめる

int64_t frame_duration_us(const AVStream* st) {
    AVRational rate = st->avg_frame_rate.num ? st->avg_frame_rate : st->r_frame_rate;
    if (rate.num <= 0 || rate.den <= 0) return 16'667;
    return av_rescale(1'000'000, rate.den, rate.num);
}

}  // namespace

Decoder::Decoder(std::string input_format, ByteQueue& input, FrameQueue& output, ChannelMetrics& metrics,
                 bool allow_hw)
    : input_format_(std::move(input_format)), input_(input), output_(output), metrics_(metrics), hw_enabled_(allow_hw) {
    if (hw_enabled_) {
        AVBufferRef* dev = nullptr;
        const int err = av_hwdevice_ctx_create(&dev, AV_HWDEVICE_TYPE_D3D11VA, nullptr, nullptr, 0);
        if (err < 0) {
            spdlog::warn("D3D11VA unavailable ({}), using software decoding", av_error_string(err));
            hw_enabled_ = false;
        } else {
            hw_device_.reset(dev);
        }
    }
}

Decoder::~Decoder() { stop(); }

void Decoder::start() {
    stop_ = false;
    thread_ = std::thread(&Decoder::run, this);
}

void Decoder::stop() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
}

int Decoder::read_packet(void* opaque, uint8_t* buf, int size) {
    auto* self = static_cast<Decoder*>(opaque);
    const int n = self->input_.read(buf, size, self->session_);
    return n > 0 ? n : AVERROR_EOF;
}

int Decoder::interrupt_cb(void* opaque) { return static_cast<Decoder*>(opaque)->stop_ ? 1 : 0; }

AVPixelFormat Decoder::get_format(AVCodecContext* ctx, const AVPixelFormat* formats) {
    for (const AVPixelFormat* p = formats; *p != AV_PIX_FMT_NONE; ++p) {
        if (*p == AV_PIX_FMT_D3D11 && ctx->hw_device_ctx) return *p;
    }
    // HW 非対応のプロファイル (4:2:2 等) はソフトウェアで
    for (const AVPixelFormat* p = formats; *p != AV_PIX_FMT_NONE; ++p) {
        const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(*p);
        if (desc && !(desc->flags & AV_PIX_FMT_FLAG_HWACCEL)) return *p;
    }
    return AV_PIX_FMT_NONE;
}

void Decoder::run() {
    uint64_t last = 0;
    while (!stop_) {
        const uint64_t session = input_.wait_session(last);
        if (session == 0) break;  // 停止
        last = session;
        run_session(session);
    }
}

bool Decoder::open_decoder(const AVStream* stream) {
    codec_.reset();
    const AVCodec* dec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!dec) {
        spdlog::error("decoder: no decoder for codec id {}", int(stream->codecpar->codec_id));
        return false;
    }

    for (const bool use_hw : {hw_enabled_, false}) {
        CodecContextPtr ctx(avcodec_alloc_context3(dec));
        if (!ctx || avcodec_parameters_to_context(ctx.get(), stream->codecpar) < 0) return false;
        ctx->pkt_timebase = stream->time_base;
        ctx->opaque = this;
        if (use_hw) {
            ctx->hw_device_ctx = av_buffer_ref(hw_device_.get());
            ctx->get_format = &Decoder::get_format;
            ctx->thread_count = 1;
        } else {
            ctx->thread_count = 0;  // 自動
            ctx->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
        }
        const int err = avcodec_open2(ctx.get(), dec, nullptr);
        if (err < 0) {
            spdlog::warn("decoder: open {} ({}) failed: {}", dec->name, use_hw ? "hw" : "sw", av_error_string(err));
            if (!use_hw) return false;
            continue;
        }
        codec_ = std::move(ctx);
        consecutive_errors_ = 0;
        {
            std::lock_guard lk(metrics_.mutex);
            metrics_.codec = dec->name;
        }
        spdlog::info("decoder: {} {}x{} ({})", dec->name, stream->codecpar->width, stream->codecpar->height,
                     use_hw ? "D3D11VA" : "software");
        return true;
    }
    return false;
}

void Decoder::handle_frame(AVFrame* frame, const AVStream* stream, uint64_t session) {
    FramePtr out;
    if (frame->format == AV_PIX_FMT_D3D11) {
        out.reset(av_frame_alloc());
        const int err = av_hwframe_transfer_data(out.get(), frame, 0);
        if (err < 0) {
            ++metrics_.decode_errors;
            return;
        }
        av_frame_copy_props(out.get(), frame);
        metrics_.hw_decode = true;
    } else {
        out.reset(av_frame_clone(frame));
        metrics_.hw_decode = false;
    }
    if (!out) return;

    int64_t ts = frame->best_effort_timestamp;
    if (ts == AV_NOPTS_VALUE) ts = frame->pts;
    int64_t pts_us;
    if (ts != AV_NOPTS_VALUE) {
        pts_us = av_rescale_q(ts, stream->time_base, AVRational{1, 1'000'000});
    } else {
        pts_us = last_pts_us_ == AV_NOPTS_VALUE ? 0 : last_pts_us_ + frame_duration_us(stream);
    }
    last_pts_us_ = pts_us;

    ++metrics_.frames_decoded;
    metrics_.width = frame->width;
    metrics_.height = frame->height;
    if (output_.push(DecodedFrame{std::move(out), pts_us, qpc_now(), session})) ++metrics_.frames_dropped_queue;
}

void Decoder::run_session(uint64_t session) {
    session_ = session;
    last_pts_us_ = AV_NOPTS_VALUE;

    auto* io_buffer = static_cast<uint8_t*>(av_malloc(kIoBufferSize));
    AVIOContext* pb = avio_alloc_context(io_buffer, kIoBufferSize, 0, this, &Decoder::read_packet, nullptr, nullptr);
    AVFormatContext* fmt = avformat_alloc_context();
    if (!io_buffer || !pb || !fmt) {
        spdlog::error("decoder: out of memory");
        return;
    }
    fmt->pb = pb;
    fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
    fmt->interrupt_callback = AVIOInterruptCB{&Decoder::interrupt_cb, this};
    fmt->probesize = 2 * 1024 * 1024;
    fmt->max_analyze_duration = 2 * AV_TIME_BASE;

    auto cleanup = [&] {
        codec_.reset();
        avformat_close_input(&fmt);  // 失敗時は open_input が解放済みで nullptr
        if (pb) av_freep(&pb->buffer);
        avio_context_free(&pb);
    };

    const AVInputFormat* ifmt = av_find_input_format(input_format_.c_str());
    int err = avformat_open_input(&fmt, nullptr, ifmt, nullptr);
    if (err < 0) {
        if (!stop_) spdlog::warn("decoder: open input failed: {}", av_error_string(err));
        cleanup();
        return;
    }
    err = avformat_find_stream_info(fmt, nullptr);
    if (err < 0) {
        if (!stop_) spdlog::warn("decoder: stream info failed: {}", av_error_string(err));
        cleanup();
        return;
    }
    const int vidx = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (vidx < 0) {
        spdlog::warn("decoder: no video stream");
        cleanup();
        return;
    }
    const AVStream* stream = fmt->streams[vidx];
    if (!open_decoder(stream)) {
        cleanup();
        return;
    }

    PacketPtr pkt(av_packet_alloc());
    FramePtr frame(av_frame_alloc());

    auto on_error = [&](int e) {
        ++metrics_.decode_errors;
        if (++consecutive_errors_ == kMaxConsecutiveErrors && hw_enabled_) {
            spdlog::warn("decoder: too many errors ({}), falling back to software", av_error_string(e));
            hw_enabled_ = false;
            open_decoder(stream);
        }
    };
    auto drain = [&] {
        int r;
        while ((r = avcodec_receive_frame(codec_.get(), frame.get())) == 0) {
            consecutive_errors_ = 0;
            handle_frame(frame.get(), stream, session);
            av_frame_unref(frame.get());
        }
        return r;
    };

    while (!stop_ && codec_) {
        err = av_read_frame(fmt, pkt.get());
        if (err < 0) break;  // セッション終了 (EOF) / 停止
        if (pkt->stream_index != vidx) {
            av_packet_unref(pkt.get());
            continue;
        }
        err = avcodec_send_packet(codec_.get(), pkt.get());
        if (err == AVERROR(EAGAIN)) {
            drain();
            err = avcodec_send_packet(codec_.get(), pkt.get());
        }
        av_packet_unref(pkt.get());
        if (err < 0) on_error(err);
        if (!codec_) break;
        const int r = drain();
        if (r != AVERROR(EAGAIN) && r != AVERROR_EOF) on_error(r);
    }
    cleanup();
}

}  // namespace denpachan::worker
