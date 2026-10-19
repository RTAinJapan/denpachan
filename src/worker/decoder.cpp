#include "decoder.h"

#include <spdlog/spdlog.h>

#include <algorithm>

#include "denpachan/win_util.h"

namespace denpachan::worker {

namespace {

constexpr int kIoBufferSize = 64 * 1024;
// キーフレームのデコードがこれだけ続けて失敗したら HW デコードをやめる (本当に HW で読めないとき)。
// キーフレーム以外の失敗 (途中から受けた、ロスで参照が欠けた) では数えない
constexpr int kMaxKeyframeErrors = 3;
constexpr int64_t kKeyframeWaitMs = 10'000;  // 受信開始・エラーのあと、これだけキーフレームが来なければそのまま渡す
constexpr int64_t kOpenTimeoutMs = 10'000;  // URL を開く・ストリームを解析するまでの上限
constexpr int64_t kReadTimeoutMs = 5'000;   // URL からこれだけ届かなければ切って繋ぎ直す

int64_t frame_duration_us(const AVStream* st) {
    AVRational rate = st->avg_frame_rate.num ? st->avg_frame_rate : st->r_frame_rate;
    if (rate.num <= 0 || rate.den <= 0) return 16'667;
    return av_rescale(1'000'000, rate.den, rate.num);
}

}  // namespace

Decoder::Decoder(std::string input_format, ByteQueue& input, FrameQueue& output, AudioWriter* audio,
                 ChannelMetrics& metrics, bool allow_hw)
    : input_format_(std::move(input_format)),
      input_(input),
      output_(output),
      audio_(audio),
      metrics_(metrics),
      audio_frame_(av_frame_alloc()),
      allow_hw_(allow_hw),
      hw_enabled_(allow_hw) {
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

Decoder::~Decoder() {
    stop();
    swr_free(&swr_);
    av_channel_layout_uninit(&swr_in_layout_);
}

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

int Decoder::interrupt_cb(void* opaque) {
    auto* self = static_cast<Decoder*>(opaque);
    if (!self->url_.empty()) {
        // URL を直接読んでいる間はこのスレッドが受信スレッドを兼ねる (ブロック中も頻繁に呼ばれる)
        self->metrics_.heartbeat_ingest();
        const int64_t deadline = self->io_deadline_qpc_.load();
        if (deadline != 0 && qpc_now() > deadline) return 1;  // 届かなくなった
    }
    return self->stop_ ? 1 : 0;
}

void Decoder::set_url(std::string url) {
    url_ = std::move(url);
    // ログやメトリクスにストリームキー (URL の最後の部分) を出さない
    const size_t slash = url_.find_last_of('/');
    display_url_ = slash != std::string::npos && slash + 1 < url_.size() ? url_.substr(0, slash + 1) + "***" : url_;
}

void Decoder::run_url_loop() {
    uint64_t session = 0;
    int backoff_ms = 500;
    while (!stop_) {
        const int64_t started = qpc_now();
        run_session(++session);
        if (stop_) break;
        if (qpc_now() - started > qpc_from_ms(10'000)) backoff_ms = 500;
        for (int waited = 0; waited < backoff_ms && !stop_; waited += 100) {
            metrics_.heartbeat_ingest();
            Sleep(100);
        }
        backoff_ms = std::min(backoff_ms * 2, 5000);
    }
}

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
    if (!url_.empty()) {
        run_url_loop();
        return;
    }
    uint64_t last = 0;
    while (!stop_) {
        const uint64_t session = input_.wait_session(last);
        if (session == 0) break;  // 停止
        last = session;
        // 接続が続いている間は、読めなくなっても (解析の時間内に映像が無い、デコーダを開けない、
        // 壊れたデータ) 同じ接続で読み直す。次の接続を待つだけだと、送出側が繋ぎ直すまで映像が出ない
        int64_t backoff_ms = 500;
        for (;;) {
            if (run_session(session)) backoff_ms = 500;  // 読めていたなら、間隔は最初から
            if (stop_ || !input_.session_alive(session)) break;
            for (int64_t waited = 0; waited < backoff_ms && !stop_ && input_.session_alive(session); waited += 100) Sleep(100);
            backoff_ms = std::min<int64_t>(backoff_ms * 2, 5000);
            if (resync_) {
                resync_();  // 新しいセッションになるので wait_session で待つ
                break;
            }
            spdlog::info("decoder: retrying the current connection");
        }
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
    if (recorder_ && recorder_->wants_frames()) recorder_->video_frame(out.get(), pts_us);  // 再エンコード録画
    if (output_.push(DecodedFrame{std::move(out), pts_us, qpc_now(), session})) ++metrics_.frames_dropped_queue;
}

bool Decoder::open_audio_decoder(const AVStream* stream) {
    audio_codec_.reset();
    const AVCodec* dec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!dec) {
        spdlog::warn("decoder: no audio decoder for codec id {}", int(stream->codecpar->codec_id));
        return false;
    }
    CodecContextPtr ctx(avcodec_alloc_context3(dec));
    if (!ctx || avcodec_parameters_to_context(ctx.get(), stream->codecpar) < 0) return false;
    ctx->pkt_timebase = stream->time_base;
    const int err = avcodec_open2(ctx.get(), dec, nullptr);
    if (err < 0) {
        spdlog::warn("decoder: open audio {} failed: {}", dec->name, av_error_string(err));
        return false;
    }
    spdlog::info("decoder: audio {} {} Hz {} ch", dec->name, ctx->sample_rate, ctx->ch_layout.nb_channels);
    audio_codec_ = std::move(ctx);
    return true;
}

void Decoder::handle_audio_frame(const AVFrame* frame, const AVStream* stream, uint64_t session) {
    // 入力形式が変わったら変換器を作り直す
    if (!swr_ || frame->format != swr_in_format_ || frame->sample_rate != swr_in_rate_ ||
        av_channel_layout_compare(&frame->ch_layout, &swr_in_layout_) != 0) {
        swr_free(&swr_);
        AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
        if (swr_alloc_set_opts2(&swr_, &stereo, AV_SAMPLE_FMT_FLT, int(audio::kSampleRate), &frame->ch_layout,
                                AVSampleFormat(frame->format), frame->sample_rate, 0, nullptr) < 0 ||
            swr_init(swr_) < 0) {
            swr_free(&swr_);
            ++metrics_.decode_errors;
            return;
        }
        av_channel_layout_uninit(&swr_in_layout_);
        av_channel_layout_copy(&swr_in_layout_, &frame->ch_layout);
        swr_in_format_ = frame->format;
        swr_in_rate_ = frame->sample_rate;
    }

    const int max_out = swr_get_out_samples(swr_, frame->nb_samples);
    if (max_out <= 0) return;
    audio_buf_.resize(size_t(max_out) * audio::kChannels);
    uint8_t* out[1] = {reinterpret_cast<uint8_t*>(audio_buf_.data())};
    const int n = swr_convert(swr_, out, max_out, frame->extended_data, frame->nb_samples);
    if (n <= 0) return;

    int64_t ts = frame->best_effort_timestamp;
    if (ts == AV_NOPTS_VALUE) ts = frame->pts;
    if (ts == AV_NOPTS_VALUE) return;  // 時刻がなければ合わせようがない
    const int64_t pts_us = av_rescale_q(ts, stream->time_base, AVRational{1, 1'000'000});
    if (audio_) audio_->write(session, pts_us, qpc_now(), audio_buf_.data(), n);
    if (recorder_ && recorder_->wants_frames()) recorder_->audio_samples(audio_buf_.data(), n, pts_us);
}

void Decoder::decode_audio(const AVPacket* pkt, const AVStream* stream, uint64_t session) {
    int err = avcodec_send_packet(audio_codec_.get(), pkt);
    if (err < 0 && err != AVERROR(EAGAIN)) {
        ++metrics_.decode_errors;
        return;
    }
    while (avcodec_receive_frame(audio_codec_.get(), audio_frame_.get()) == 0) {
        handle_audio_frame(audio_frame_.get(), stream, session);
        av_frame_unref(audio_frame_.get());
    }
}

bool Decoder::run_session(uint64_t session) {
    session_ = session;
    last_pts_us_ = AV_NOPTS_VALUE;
    hw_enabled_ = allow_hw_ && hw_device_ != nullptr;  // 前の接続で HW をやめていても、新しい接続では試し直す
    consecutive_errors_ = 0;
    const bool url_mode = !url_.empty();

    AVIOContext* pb = nullptr;
    AVFormatContext* fmt = avformat_alloc_context();
    if (!fmt) return false;
    if (!url_mode) {
        // ByteQueue から読む (SRT / RTMP push)
        auto* io_buffer = static_cast<uint8_t*>(av_malloc(kIoBufferSize));
        pb = avio_alloc_context(io_buffer, kIoBufferSize, 0, this, &Decoder::read_packet, nullptr, nullptr);
        if (!io_buffer || !pb) {
            spdlog::error("decoder: out of memory");
            avformat_free_context(fmt);
            return false;
        }
        fmt->pb = pb;
        fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
    }
    fmt->interrupt_callback = AVIOInterruptCB{&Decoder::interrupt_cb, this};
    fmt->probesize = 2 * 1024 * 1024;
    fmt->max_analyze_duration = 2 * AV_TIME_BASE;

    auto cleanup = [&] {
        codec_.reset();
        audio_codec_.reset();
        avformat_close_input(&fmt);  // 失敗時は open_input が解放済みで nullptr
        if (pb) av_freep(&pb->buffer);
        avio_context_free(&pb);
        if (url_mode && metrics_.connected) {
            metrics_.connected = false;
            spdlog::info("decoder: {} disconnected", display_url_);
        }
    };

    int err;
    if (url_mode) {
        // RTMP pull など: URL を直接開く。届かなくなったら interrupt_cb が打ち切る。
        io_deadline_qpc_ = qpc_now() + qpc_from_ms(kOpenTimeoutMs);
        err = avformat_open_input(&fmt, url_.c_str(), nullptr, nullptr);
    } else {
        err = avformat_open_input(&fmt, nullptr, av_find_input_format(input_format_.c_str()), nullptr);
    }
    if (err < 0) {
        if (!stop_) spdlog::warn("decoder: open input failed: {}", av_error_string(err));
        cleanup();
        return false;
    }
    if (url_mode) {
        metrics_.connected = true;
        ++metrics_.connections;
        std::lock_guard lk(metrics_.mutex);
        metrics_.peer = display_url_;
        metrics_.last_error.clear();
        spdlog::info("decoder: connected to {}", display_url_);
        io_deadline_qpc_ = qpc_now() + qpc_from_ms(kOpenTimeoutMs);
    }
    err = avformat_find_stream_info(fmt, nullptr);
    if (err < 0) {
        if (!stop_) spdlog::warn("decoder: stream info failed: {}", av_error_string(err));
        cleanup();
        return false;
    }
    if (url_mode) io_deadline_qpc_ = qpc_now() + qpc_from_ms(kReadTimeoutMs);

    // FLV (RTMP) は最初のパケットが来たときにストリームを作るので、映像が音声より遅れて届くと
    // (nginx-rtmp は映像を次のキーフレームから送る)、解析の時間内に音声しか見つからない。
    // そのときは切らずに読み続け、映像のストリームが現れた時点でデコーダを開く (下のループ)。
    // FLV は映像の設定 (SPS/PPS) を最初の映像パケットの前に読み込むので、最初のキーフレームから使える。
    int vidx = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    const AVStream* stream = nullptr;
    int aidx = -1;
    const AVStream* audio_stream = nullptr;
    auto start_video = [&](int index) {
        vidx = index;
        stream = fmt->streams[vidx];
        if (!open_decoder(stream)) return false;
        if (audio_ || recorder_) {
            aidx = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, vidx, nullptr, 0);
            if (aidx < 0) {
                spdlog::warn("decoder: no audio stream");
            } else if (!open_audio_decoder(fmt->streams[aidx])) {
                aidx = -1;
            }
        }
        audio_stream = aidx >= 0 ? fmt->streams[aidx] : nullptr;
        if (recorder_) recorder_->begin_session(session, stream, audio_stream);
        return true;
    };
    const int64_t video_deadline = qpc_now() + qpc_from_ms(kOpenTimeoutMs);
    if (vidx >= 0) {
        if (!start_video(vidx)) {
            cleanup();
            return false;
        }
    } else {
        spdlog::info("decoder: waiting for the video stream (only {} so far)",
                     fmt->nb_streams > 0 ? "audio" : "nothing");
    }

    PacketPtr pkt(av_packet_alloc());
    FramePtr frame(av_frame_alloc());

    // 途中から受けたとき (SRT は GOP の途中から届く) やロスのあとは、次のキーフレームまで映像をデコーダに渡さない。
    // 参照の欠けたフレームはどうせ読めず、エラーが続いて HW デコードをやめる判断を誤らせるため。
    bool need_key = true;
    int64_t key_deadline = qpc_now() + qpc_from_ms(kKeyframeWaitMs);
    bool sent_key = false;  // 直前にデコーダに渡したのがキーフレームか

    auto on_error = [&](int e) {
        ++metrics_.decode_errors;
        need_key = true;
        key_deadline = qpc_now() + qpc_from_ms(kKeyframeWaitMs);
        if (sent_key && ++consecutive_errors_ == kMaxKeyframeErrors && hw_enabled_) {
            spdlog::warn("decoder: keyframes keep failing ({}), falling back to software", av_error_string(e));
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

    bool started = false;  // 映像を 1 枚でもデコーダに渡せた
    while (!stop_ && (codec_ || vidx < 0)) {
        err = av_read_frame(fmt, pkt.get());  // ネットワーク待ちでブロックしうる
        if (err < 0) {                        // セッション終了 (EOF) / 停止
            if (url_mode && !stop_) spdlog::info("decoder: read ended: {}", av_error_string(err));
            break;
        }
        if (url_mode) {
            io_deadline_qpc_ = qpc_now() + qpc_from_ms(kReadTimeoutMs);
            metrics_.bytes_in += uint64_t(pkt->size);  // URL を直接読むときは受信量をここで数える
        }
        if (vidx < 0) {
            // 映像を待っている: 映像のストリームが現れたらデコーダを開く。それまでの音声は捨てる (映像と一緒に始める)
            const AVCodecParameters* par = fmt->streams[pkt->stream_index]->codecpar;
            if (par->codec_type == AVMEDIA_TYPE_VIDEO && par->codec_id != AV_CODEC_ID_NONE) {
                spdlog::info("decoder: video stream appeared");
                if (!start_video(pkt->stream_index)) break;
            } else {
                av_packet_unref(pkt.get());
                if (qpc_now() > video_deadline) {
                    spdlog::warn("decoder: no video stream");
                    break;
                }
                continue;
            }
        }
        // ここから先のデコード処理 (ブロックしないはず) で固まったら engine が検出する
        DecoderBusy busy(metrics_);
        if (recorder_ && recorder_->wants_packets()) {  // パススルー録画 (参照を渡すだけ)
            if (pkt->stream_index == vidx) recorder_->video_packet(pkt.get());
            else if (pkt->stream_index == aidx) recorder_->audio_packet(pkt.get());
        }
        if (pkt->stream_index == aidx) {
            decode_audio(pkt.get(), audio_stream, session);
            av_packet_unref(pkt.get());
            continue;
        }
        if (pkt->stream_index != vidx) {
            av_packet_unref(pkt.get());
            continue;
        }
        sent_key = (pkt->flags & AV_PKT_FLAG_KEY) != 0;
        if (need_key) {
            if (!sent_key && qpc_now() < key_deadline) {
                av_packet_unref(pkt.get());
                continue;
            }
            need_key = false;
        }
        err = avcodec_send_packet(codec_.get(), pkt.get());
        if (err == AVERROR(EAGAIN)) {
            drain();
            err = avcodec_send_packet(codec_.get(), pkt.get());
        }
        av_packet_unref(pkt.get());
        started = true;
        if (err < 0) on_error(err);
        if (!codec_) break;
        const int r = drain();
        if (r != AVERROR(EAGAIN) && r != AVERROR_EOF) on_error(r);
    }
    cleanup();
    return started;
}

}  // namespace denpachan::worker
