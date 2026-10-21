#include "recorder.h"

#include <windows.h>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <climits>
#include <ctime>

#include "denpachan/ui_message.h"
#include "filename.h"

namespace denpachan::worker {

namespace {

constexpr size_t kMaxQueueBytes = 256u * 1024 * 1024;  // これを超えたら録画側で捨てる
constexpr AVRational kUs{1, 1'000'000};
constexpr AVRational kEncoderTimeBase{1, 90'000};
constexpr int kAudioRate = 48000;

enum class RecState : uint32_t { Off = 0, Waiting = 1, Recording = 2, Error = 3 };

std::string default_dir() {
    wchar_t buf[MAX_PATH] = {};
    const DWORD n = GetEnvironmentVariableW(L"USERPROFILE", buf, MAX_PATH);
    std::filesystem::path p = (n > 0 && n < MAX_PATH) ? std::filesystem::path(buf) : std::filesystem::path(L".");
    return path_utf8(p / L"Videos" / L"denpachan");
}

double free_gb(const std::string& dir) {
    std::filesystem::path p = utf8_path(dir);
    std::error_code ec;
    while (!p.empty() && !std::filesystem::exists(p, ec)) p = p.parent_path();  // まだないフォルダは親で測る
    ULARGE_INTEGER avail{};
    if (p.empty() || !GetDiskFreeSpaceExW(p.c_str(), &avail, nullptr, nullptr)) return 1e9;
    return double(avail.QuadPart) / (1024.0 * 1024 * 1024);
}

std::vector<std::string> encoder_candidates(const RecordConfig& c) {
    const std::string codec = c.video_codec == "hevc" || c.video_codec == "av1" ? c.video_codec : "h264";
    if (c.video_encoder != "auto" && !c.video_encoder.empty()) return {codec + "_" + c.video_encoder};
    return {codec + "_nvenc", codec + "_qsv", codec + "_amf"};  // 自動は GPU だけ (CPU は明示的に選んだときだけ)
}

int preset_index(const std::string& p) { return p.size() == 2 && p[0] == 'p' && p[1] >= '1' && p[1] <= '7' ? p[1] - '0' : 4; }

// p1 (速い) 〜 p7 (高画質) を x264 / x265 のプリセット名に
const char* x264_preset(const std::string& p) {
    static const char* names[] = {"", "ultrafast", "superfast", "veryfast", "faster", "fast", "medium", "slow"};
    return names[preset_index(p)];
}

// p1 (速い) 〜 p7 (高画質) を SVT-AV1 のプリセット (数字が小さいほど遅く高画質) に
const char* svtav1_preset(const std::string& p) {
    static const char* names[] = {"", "12", "11", "10", "9", "8", "7", "6"};
    return names[preset_index(p)];
}

// 外部 ffmpeg の引数。入力は標準入力の NUT (時刻は直したもの)
std::vector<std::string> external_args(const RecordConfig& c, const std::string& path, bool has_audio, int fps) {
    std::vector<std::string> a = {"-hide_banner", "-loglevel", "error", "-nostats", "-f", "nut", "-i", "pipe:0", "-map", "0:v:0"};
    if (has_audio) a.insert(a.end(), {"-map", "0:a:0"});
    const std::string kbps = std::to_string(c.bitrate_kbps) + "k";
    const bool cqp = c.rate_control == "cqp";
    if (c.video_codec == "av1") {
        a.insert(a.end(), {"-c:v", "libsvtav1", "-preset", svtav1_preset(c.preset)});
        if (cqp) a.insert(a.end(), {"-svtav1-params", "rc=0:enable-tpl-la=0:qp=" + std::to_string(c.qp)});
        else if (c.rate_control == "cbr") a.insert(a.end(), {"-svtav1-params", "rc=2:pred-struct=1", "-b:v", kbps});
        else a.insert(a.end(), {"-svtav1-params", "rc=1", "-b:v", kbps});
    } else {
        const bool hevc = c.video_codec == "hevc";
        a.insert(a.end(), {"-c:v", hevc ? "libx265" : "libx264", "-preset", x264_preset(c.preset)});
        if (cqp) {
            if (hevc) a.insert(a.end(), {"-x265-params", "log-level=error:qp=" + std::to_string(c.qp)});
            else a.insert(a.end(), {"-qp", std::to_string(c.qp)});
        } else {
            const int max_kbps = c.rate_control == "vbr" ? c.bitrate_kbps * 3 / 2 : c.bitrate_kbps;
            a.insert(a.end(),
                     {"-b:v", kbps, "-maxrate", std::to_string(max_kbps) + "k", "-bufsize", std::to_string(c.bitrate_kbps * 2) + "k"});
            if (hevc) a.insert(a.end(), {"-x265-params", "log-level=error"});
        }
        a.insert(a.end(), {"-pix_fmt", "yuv420p"});
    }
    a.insert(a.end(), {"-g", std::to_string(std::max(1, c.gop_sec * fps))});
    if (has_audio) a.insert(a.end(), {"-c:a", "aac", "-b:a", std::to_string(c.audio_bitrate_kbps) + "k", "-ac", "2"});
    if (c.container == "ts") {
        a.insert(a.end(), {"-f", "mpegts"});
    } else {
        a.insert(a.end(), {"-f", "matroska", "-cluster_time_limit", "1000"});  // 1 秒ごとにクラスタを書く
    }
    a.insert(a.end(), {"-flush_packets", "1", "-y", path});
    return a;
}

int pipe_write(void* opaque, const uint8_t* buf, int size) {
    return static_cast<ExternalEncoder*>(opaque)->write(buf, size_t(size)) ? size : AVERROR(EPIPE);
}

uint64_t file_size(const std::string& path) {
    std::error_code ec;
    const auto n = std::filesystem::file_size(utf8_path(path), ec);
    return ec ? 0 : uint64_t(n);
}

// p1 (速い) 〜 p7 (高画質) を QSV のプリセット名に
const char* qsv_preset(const std::string& p) {
    static const char* names[] = {"", "veryfast", "faster", "fast", "medium", "slow", "slower", "veryslow"};
    return names[preset_index(p)];
}

}  // namespace

// ---- 設定 ----

RecordConfig parse_record_config(const std::string& text, int ch) {
    RecordConfig c;
    try {
        const auto j = nlohmann::json::parse(text);
        auto get = [&](const nlohmann::json& o, const char* k, auto def) {
            return o.contains(k) && !o.at(k).is_null() ? o.at(k).get<decltype(def)>() : def;
        };
        c.enabled = get(j, "enabled", false);
        c.dir = get(j, "dir", std::string());
        c.tmpl = get(j, "template", c.tmpl);
        c.container = get(j, "container", c.container);
        c.mode = get(j, "mode", c.mode);
        c.gap_timeout_sec = get(j, "gapTimeoutSec", c.gap_timeout_sec);
        c.max_segment_min = get(j, "maxSegmentMin", c.max_segment_min);
        c.replacement = get(j, "invalidCharReplacement", c.replacement);
        c.min_free_gb = get(j, "minFreeGb", c.min_free_gb);
        c.name = get(j, "name", "CH" + std::to_string(ch));
        if (j.contains("encode")) {
            const auto& e = j.at("encode");
            if (e.contains("video")) {
                const auto& v = e.at("video");
                c.video_codec = get(v, "codec", c.video_codec);
                c.video_encoder = get(v, "encoder", c.video_encoder);
                c.rate_control = get(v, "rc", c.rate_control);
                c.bitrate_kbps = get(v, "bitrateKbps", c.bitrate_kbps);
                c.qp = get(v, "qp", c.qp);
                c.preset = get(v, "preset", c.preset);
                c.gop_sec = get(v, "gopSec", c.gop_sec);
            }
            if (e.contains("audio")) c.audio_bitrate_kbps = get(e.at("audio"), "bitrateKbps", c.audio_bitrate_kbps);
        }
    } catch (const std::exception& e) {
        spdlog::warn("record: invalid settings ({})", e.what());
        c = RecordConfig{};
    }
    if (c.dir.empty()) c.dir = default_dir();
    c.gap_timeout_sec = std::clamp(c.gap_timeout_sec, 1, 3600);
    return c;
}

// ---- 内部の型 ----

struct Recorder::StreamInfo {
    uint64_t session = 0;
    AVCodecParameters* video = nullptr;
    AVRational video_tb{1, 1000};
    AVRational video_rate{0, 1};
    AVCodecParameters* audio = nullptr;
    AVRational audio_tb{1, 1000};

    ~StreamInfo() {
        avcodec_parameters_free(&video);
        avcodec_parameters_free(&audio);
    }
};

struct Recorder::Item {
    enum class Kind { Configure, Session, VideoPacket, AudioPacket, VideoFrame, AudioBlock } kind;
    RecordConfig config;
    std::shared_ptr<StreamInfo> info;
    PacketPtr packet;
    FramePtr frame;
    std::vector<float> samples;
    int64_t pts_us = 0;
    size_t bytes = 0;
};

struct Recorder::Segment {
    AVFormatContext* oc = nullptr;
    int video_index = -1;
    int audio_index = -1;
    std::vector<int64_t> last_dts;
    std::string path;
    int64_t started_qpc = 0;
    int width = 0;
    int height = 0;
    // 再エンコード
    CodecContextPtr venc;
    CodecContextPtr aenc;
    SwsContext* sws = nullptr;
    FramePtr converted;
    int64_t last_vpts = INT64_MIN;
    SwrContext* swr = nullptr;
    AVAudioFifo* fifo = nullptr;
    int64_t audio_next_us = AV_NOPTS_VALUE;
    // 外部 ffmpeg (oc は NUT で、pb は ffmpeg の標準入力へのパイプ)
    std::unique_ptr<ExternalEncoder> ext;

    ~Segment() {
        sws_freeContext(sws);
        swr_free(&swr);
        if (fifo) av_audio_fifo_free(fifo);
        if (oc) {
            if (oc->pb && ext) {
                av_freep(&oc->pb->buffer);
                avio_context_free(&oc->pb);
            } else if (oc->pb) {
                avio_closep(&oc->pb);
            }
            avformat_free_context(oc);
        }
    }
};

// ---- 公開 API ----

Recorder::Recorder(int ch, ChannelMetrics& metrics) : ch_(ch), metrics_(metrics) {}

Recorder::~Recorder() { stop(); }

void Recorder::start() {
    stop_ = false;
    thread_done_ = false;
    thread_ = std::thread(&Recorder::run, this);
}

void Recorder::stop() {
    {
        std::lock_guard lk(mutex_);
        stop_ = true;
    }
    cv_.notify_all();
    if (!thread_.joinable()) return;
    {
        // 外部 ffmpeg が入力を読まずに固まっていると書き込みで止まったままになる。engine に強制終了される前に ffmpeg を止める
        std::unique_lock lk(mutex_);
        if (!done_cv_.wait_for(lk, std::chrono::milliseconds(2000), [&] { return thread_done_; })) {
            spdlog::warn("record: the recorder thread is blocked; stopping ffmpeg");
            lk.unlock();
            ExternalEncoder::kill_all();
        }
    }
    thread_.join();
}

void Recorder::configure(const RecordConfig& config) {
    enabled_ = config.enabled;
    packets_ = uses_packets(config);
    auto item = std::make_unique<Item>();
    item->kind = Item::Kind::Configure;
    item->config = config;
    push(std::move(item), false);
}

void Recorder::push(std::unique_ptr<Item> item, bool media) {
    {
        std::lock_guard lk(mutex_);
        if (media && queued_bytes_ + item->bytes > kMaxQueueBytes) {
            // 録画が追いつかない: 録画側で捨てる (受信と出力は止めない)
            ++metrics_.rec_dropped;
            if (item->kind == Item::Kind::VideoPacket) need_keyframe_ = true;
            return;
        }
        queued_bytes_ += item->bytes;
        queue_.push_back(std::move(item));
    }
    cv_.notify_one();
}

void Recorder::begin_session(uint64_t session, const AVStream* video, const AVStream* audio) {
    auto info = std::make_shared<StreamInfo>();
    info->session = session;
    if (video) {
        info->video = avcodec_parameters_alloc();
        avcodec_parameters_copy(info->video, video->codecpar);
        info->video_tb = video->time_base;
        info->video_rate = video->avg_frame_rate.num ? video->avg_frame_rate : video->r_frame_rate;
    }
    if (audio) {
        info->audio = avcodec_parameters_alloc();
        avcodec_parameters_copy(info->audio, audio->codecpar);
        info->audio_tb = audio->time_base;
    }
    auto item = std::make_unique<Item>();
    item->kind = Item::Kind::Session;
    item->info = std::move(info);
    push(std::move(item), false);
}

void Recorder::video_packet(const AVPacket* pkt) {
    if (!wants_packets()) return;
    auto item = std::make_unique<Item>();
    item->kind = Item::Kind::VideoPacket;
    item->packet.reset(av_packet_clone(pkt));
    if (!item->packet) return;
    item->bytes = size_t(pkt->size) + 256;
    push(std::move(item), true);
}

void Recorder::audio_packet(const AVPacket* pkt) {
    if (!wants_packets()) return;
    auto item = std::make_unique<Item>();
    item->kind = Item::Kind::AudioPacket;
    item->packet.reset(av_packet_clone(pkt));
    if (!item->packet) return;
    item->bytes = size_t(pkt->size) + 256;
    push(std::move(item), true);
}

void Recorder::video_frame(const AVFrame* frame, int64_t pts_us) {
    if (!wants_frames()) return;
    auto item = std::make_unique<Item>();
    item->kind = Item::Kind::VideoFrame;
    item->frame.reset(av_frame_alloc());
    if (!item->frame || av_frame_ref(item->frame.get(), frame) < 0) return;
    item->pts_us = pts_us;
    item->bytes = size_t(frame->width) * frame->height * 2;
    push(std::move(item), true);
}

void Recorder::audio_samples(const float* data, int frames, int64_t pts_us) {
    if (!wants_frames() || frames <= 0) return;
    auto item = std::make_unique<Item>();
    item->kind = Item::Kind::AudioBlock;
    item->samples.assign(data, data + size_t(frames) * 2);
    item->pts_us = pts_us;
    item->bytes = item->samples.size() * sizeof(float);
    push(std::move(item), true);
}

// ---- 録画スレッド ----

void Recorder::run() {
    for (;;) {
        std::unique_ptr<Item> item;
        {
            std::unique_lock lk(mutex_);
            cv_.wait_for(lk, std::chrono::milliseconds(500), [&] { return stop_ || !queue_.empty(); });
            if (stop_ && queue_.empty()) break;
            if (!queue_.empty()) {
                item = std::move(queue_.front());
                queue_.pop_front();
                queued_bytes_ -= item->bytes;
            }
        }
        try {
            if (item) process(*item);
            housekeeping();
        } catch (const std::exception& e) {
            set_error(ui_message("rec.failed", {{"detail", e.what()}}));
            close_segment("error");
        }
    }
    close_segment("stopped");
    reap_external();
    finishing_.clear();  // 書き終わっていない ffmpeg は待たない (入力を閉じてあるので自分で書き終えて終わる)
    {
        std::lock_guard lk(mutex_);
        thread_done_ = true;
    }
    done_cv_.notify_all();
}

void Recorder::reap_external() {
    for (auto it = finishing_.begin(); it != finishing_.end();) {
        DWORD code = 0;
        if (!(*it)->exited(code)) {
            ++it;
            continue;
        }
        if (code != 0) {
            set_error(ui_message("rec.ffmpegCrashed", {{"code", std::to_string(code)}, {"detail", (*it)->last_message()}}));
        } else {
            spdlog::info("record: ffmpeg (pid {}) finished", (*it)->pid());
        }
        it = finishing_.erase(it);
    }
}

bool Recorder::same_params(const StreamInfo& a, const StreamInfo& b) const {
    auto same = [](const AVCodecParameters* x, const AVCodecParameters* y) {
        if (!x || !y) return x == y;
        return x->codec_id == y->codec_id && x->width == y->width && x->height == y->height && x->sample_rate == y->sample_rate &&
               x->ch_layout.nb_channels == y->ch_layout.nb_channels && x->extradata_size == y->extradata_size &&
               (x->extradata_size == 0 || memcmp(x->extradata, y->extradata, size_t(x->extradata_size)) == 0);
    };
    return same(a.video, b.video) && same(a.audio, b.audio);
}

void Recorder::process(Item& item) {
    switch (item.kind) {
    case Item::Kind::Configure: {
        const RecordConfig& n = item.config;
        const RecordConfig& o = config_;
        // 書き出し方が変わるならファイルを閉じる (次に映像が来たら新しい設定で開く)
        const bool output_changed = !n.enabled || n.mode != o.mode || n.container != o.container || n.dir != o.dir || n.tmpl != o.tmpl ||
                                    n.video_codec != o.video_codec || n.video_encoder != o.video_encoder ||
                                    n.rate_control != o.rate_control || n.bitrate_kbps != o.bitrate_kbps || n.qp != o.qp ||
                                    n.preset != o.preset || n.gop_sec != o.gop_sec || n.audio_bitrate_kbps != o.audio_bitrate_kbps;
        if (segment_ && output_changed) close_segment(n.enabled ? "settings changed" : "recording turned off");
        if (n.enabled && !o.enabled) spdlog::info("record: enabled ({}, {})", n.mode, n.container);
        config_ = n;
        disk_full_ = false;
        retry_open_qpc_ = 0;
        open_failures_ = 0;
        need_keyframe_ = true;  // 途中から書き始めるときはキーフレームから
        {
            std::lock_guard lk(metrics_.mutex);
            metrics_.rec_error.clear();
        }
        break;
    }
    case Item::Kind::Session:
        // パケットをそのまま使う方式 (パススルー、外部 ffmpeg) では、コーデックや解像度が変わったら同じファイルには続けられない
        if (segment_ && uses_packets(config_) && segment_info_ && !same_params(*segment_info_, *item.info)) {
            close_segment("stream parameters changed");
        }
        if (segment_) {
            continuation_pending_ = true;  // 同じファイルに続けて書く (タイムスタンプをつなぐ)
            need_keyframe_ = true;         // 途中から届いたストリームはキーフレームまで書かない
        }
        info_ = item.info;
        have_last_in_video_ = false;
        retry_open_qpc_ = 0;
        open_failures_ = 0;
        break;
    case Item::Kind::VideoPacket: {
        if (!config_.enabled || !uses_packets(config_) || disk_full_ || !info_ || !info_->video) break;
        AVPacket* pkt = item.packet.get();
        const bool key = (pkt->flags & AV_PKT_FLAG_KEY) != 0;
        const int64_t in_ts = pkt->dts != AV_NOPTS_VALUE ? pkt->dts : pkt->pts;
        if (in_ts != AV_NOPTS_VALUE && video_ts_jumped(av_rescale_q(in_ts, info_->video_tb, kUs)) && segment_) {
            continuation_pending_ = true;
            need_keyframe_ = true;
        }
        if (need_keyframe_) {
            if (!key) break;
            need_keyframe_ = false;
        }
        if (!segment_) {
            if (!key) break;
            const int64_t ts = pkt->dts != AV_NOPTS_VALUE ? pkt->dts : pkt->pts;
            if (ts == AV_NOPTS_VALUE || !try_open_segment(nullptr)) break;
            offset_us_ = -av_rescale_q(ts, info_->video_tb, kUs);
        }
        if (write_packet(segment_->video_index, pkt, info_->video_tb)) last_video_qpc_ = qpc_now();
        break;
    }
    case Item::Kind::AudioPacket:
        if (segment_ && segment_->audio_index >= 0 && info_ && info_->audio) {
            write_packet(segment_->audio_index, item.packet.get(), info_->audio_tb);
        }
        break;
    case Item::Kind::VideoFrame: {
        if (!config_.enabled || uses_packets(config_) || disk_full_) break;
        const AVFrame* f = item.frame.get();
        if (video_ts_jumped(item.pts_us) && segment_) continuation_pending_ = true;
        if (segment_ && (f->width != segment_->width || f->height != segment_->height)) close_segment("resolution changed");
        if (!segment_) {
            if (!try_open_segment(f)) break;
            offset_us_ = -item.pts_us;
        }
        const int64_t out_us = remap_us(item.pts_us, true);
        if (out_us >= 0) last_video_qpc_ = qpc_now();
        encode_video(f, out_us);
        break;
    }
    case Item::Kind::AudioBlock:
        if (segment_ && segment_->aenc) {
            encode_audio(item.samples.data(), int(item.samples.size() / 2), remap_us(item.pts_us, false));
        }
        break;
    }
}

bool Recorder::try_open_segment(const AVFrame* first_frame) {
    const int64_t now = qpc_now();
    if (retry_open_qpc_ != 0 && now < retry_open_qpc_) return false;
    if (open_segment(first_frame)) {
        retry_open_qpc_ = 0;
        open_failures_ = 0;
        return true;
    }
    schedule_retry(now);
    return false;
}

// 開けなかったら間を空けてまた試す (GPU の空き待ちなど)。10 秒から始めて倍々に、最長 5 分。
// 同じ失敗を 10 秒ごとに繰り返すと、1 日に数千個の空ファイルと ffmpeg の起動になる
void Recorder::schedule_retry(int64_t now) {
    ++open_failures_;
    const int64_t delay_ms = std::min<int64_t>(10'000LL << std::min(open_failures_ - 1, 5), 300'000);
    retry_open_qpc_ = now + qpc_from_ms(delay_ms);
}

int64_t Recorder::frame_us() const {
    const AVRational rate = info_ && info_->video_rate.num > 0 ? info_->video_rate : AVRational{60, 1};
    return av_rescale(1'000'000, rate.den, rate.num);
}

int64_t Recorder::remap_us(int64_t in_us, bool video) {
    if (continuation_pending_) {
        // 再接続: 新しいセッションの最初の映像を、前の映像の 1 フレーム後ろに置く。
        // 映像の刻みを保つので、再生側でフレームの重なりや詰まりが起きない。それまでの音声は捨てる。
        if (!video) return -1;
        offset_us_ = last_video_out_us_ + frame_us() - in_us;
        continuation_pending_ = false;
        spdlog::info("record: continuing the same file after reconnect");
    }
    return in_us + offset_us_;
}

// 同じ接続のまま、映像のタイムスタンプが戻った・実時間より大きく進んだ (送出側の再起動、MPEG-TS の 33 bit の折り返し、
// RTMP の publish 中の巻き戻りなど)。そのまま書くと、戻った分を 1 tick ずつ詰めて書き、音声は捨て続けるので、
// 再接続と同じく、次の映像を前の映像の 1 フレーム後ろに置いてつなぐ
bool Recorder::video_ts_jumped(int64_t in_us) {
    constexpr int64_t kBackUs = 500'000;       // これより戻ったら
    constexpr int64_t kForwardUs = 5'000'000;  // 実際に経った時間よりこれ以上進んだら
    const int64_t now = qpc_now();
    bool jumped = false;
    if (have_last_in_video_) {
        const int64_t delta = in_us - last_in_video_us_;
        jumped = delta < -kBackUs || delta > qpc_to_us(now - last_in_video_qpc_) + kForwardUs;
        if (jumped && segment_) spdlog::info("record: timestamps jumped by {} ms in the same connection; continuing", delta / 1000);
    }
    have_last_in_video_ = true;
    last_in_video_us_ = in_us;
    last_in_video_qpc_ = now;
    return jumped;
}

void Recorder::housekeeping() {
    const int64_t now = qpc_now();
    if (segment_ && now - last_video_qpc_ > qpc_from_ms(int64_t(config_.gap_timeout_sec) * 1000)) {
        close_segment("no video");
    }
    if (segment_ && config_.max_segment_min > 0 && now - segment_->started_qpc > qpc_from_ms(int64_t(config_.max_segment_min) * 60'000)) {
        close_segment("maximum length reached");
        need_keyframe_ = true;
    }
    if (config_.enabled && now - last_disk_check_qpc_ > qpc_from_ms(10'000)) {
        last_disk_check_qpc_ = now;
        if (free_gb(config_.dir) < config_.min_free_gb) {
            if (!disk_full_) set_error(ui_message("rec.diskFull", {{"gb", std::to_string(config_.min_free_gb)}}));
            disk_full_ = true;
            close_segment("disk almost full");
        } else if (disk_full_) {
            // 空きができたら録画を再開する
            spdlog::info("record: disk space is available again");
            disk_full_ = false;
            need_keyframe_ = true;
            std::lock_guard lk(metrics_.mutex);
            metrics_.rec_error.clear();
        }
    }
    if (segment_ && segment_->ext) {
        // 外部 ffmpeg: 書いているのは ffmpeg なので、ファイルの大きさを 1 秒ごとに見る。落ちていたらエラーにする
        DWORD code = 0;
        if (segment_->ext->exited(code)) {
            set_error(ui_message("rec.ffmpegExited", {{"code", std::to_string(code)}, {"detail", segment_->ext->last_message()}}));
            const std::string path8 = segment_->path;
            close_segment("ffmpeg exited");
            if (file_size(path8) == 0) {  // 何も書けていない
                std::error_code ec;
                std::filesystem::remove(utf8_path(path8), ec);
            }
            schedule_retry(now);
        } else if (now - last_size_check_qpc_ > qpc_from_ms(1000)) {
            last_size_check_qpc_ = now;
            metrics_.rec_bytes = file_size(segment_->path);
        }
    } else if (segment_) {
        metrics_.rec_bytes = uint64_t(std::max<int64_t>(0, avio_tell(segment_->oc->pb)));
    }
    if (!finishing_.empty()) reap_external();
    RecState state = !config_.enabled                     ? RecState::Off
                     : segment_                           ? RecState::Recording
                     : disk_full_ || retry_open_qpc_ != 0 ? RecState::Error
                                                          : RecState::Waiting;
    metrics_.rec_state = uint32_t(state);
}

void Recorder::set_error(const std::string& message) {
    spdlog::error("record: {}", message);
    std::lock_guard lk(metrics_.mutex);
    metrics_.rec_error = message;
}

bool Recorder::open_segment(const AVFrame* first_frame) {
    if (!info_) return false;
    if (free_gb(config_.dir) < config_.min_free_gb) {
        disk_full_ = true;
        set_error(ui_message("rec.diskLow", {{"gb", std::to_string(config_.min_free_gb)}}));
        return false;
    }
    const bool encode = config_.mode == "encode";
    const bool external = uses_external_encoder(config_);
    const std::time_t t = std::time(nullptr);
    FilenameFields f;
    f.ch = ch_;
    f.name = config_.name;
    localtime_s(&f.time, &t);
    f.seq = ++seq_;
    if (external && info_->video) {
        f.codec = config_.video_codec;
        f.width = info_->video->width;
        f.height = info_->video->height;
    } else if (encode) {
        f.codec = config_.video_codec;
        f.width = first_frame ? first_frame->width : 0;
        f.height = first_frame ? first_frame->height : 0;
    } else if (info_->video) {
        f.codec = avcodec_get_name(info_->video->codec_id);
        f.width = info_->video->width;
        f.height = info_->video->height;
    }
    const std::string ext = config_.container == "ts" ? ".ts" : ".mkv";
    const auto path = make_unique_path(config_.dir, expand_template(config_.tmpl, f, config_.replacement), ext);
    const std::string path8 = path_utf8(path);

    auto seg = std::make_unique<Segment>();
    seg->path = path8;
    seg->width = f.width;
    seg->height = f.height;
    if (external) {
        if (!open_external(*seg)) return false;
    } else {
        if (avformat_alloc_output_context2(&seg->oc, nullptr, config_.container == "ts" ? "mpegts" : "matroska", path8.c_str()) < 0) {
            set_error(ui_message("rec.cannotCreate", {{"path", path8}}));
            return false;
        }
        if (encode) {
            if (!open_encoders(*seg, first_frame)) return false;
        } else {
            add_copy_streams(*seg);
        }
        seg->oc->flags |= AVFMT_FLAG_FLUSH_PACKETS;  // 落ちても書けたところまで残す
        int err = avio_open(&seg->oc->pb, path8.c_str(), AVIO_FLAG_WRITE);
        if (err < 0) {
            set_error(ui_message("rec.cannotOpen", {{"path", path8}, {"detail", av_error_string(err)}}));
            return false;
        }
        AVDictionary* opts = nullptr;
        if (config_.container != "ts") av_dict_set(&opts, "cluster_time_limit", "1000", 0);  // 1 秒ごとにクラスタを書く
        err = avformat_write_header(seg->oc, &opts);
        av_dict_free(&opts);
        if (err < 0) {
            set_error(ui_message("rec.cannotWriteHeader", {{"detail", av_error_string(err)}}));
            avio_closep(&seg->oc->pb);
            std::error_code ec;
            std::filesystem::remove(path, ec);  // 空のファイルを残さない
            return false;
        }
    }
    seg->last_dts.assign(seg->oc->nb_streams, AV_NOPTS_VALUE);
    seg->started_qpc = qpc_now();
    segment_ = std::move(seg);
    segment_info_ = info_;
    continuation_pending_ = false;
    last_video_out_us_ = 0;
    metrics_.rec_started_qpc = segment_->started_qpc;
    metrics_.rec_bytes = 0;
    {
        std::lock_guard lk(metrics_.mutex);
        metrics_.rec_file = path8;
        metrics_.rec_error.clear();
    }
    spdlog::info("record: started {}", path8);
    return true;
}

void Recorder::add_copy_streams(Segment& seg) {
    AVStream* vs = avformat_new_stream(seg.oc, nullptr);
    avcodec_parameters_copy(vs->codecpar, info_->video);
    vs->codecpar->codec_tag = 0;
    vs->time_base = info_->video_tb;
    seg.video_index = vs->index;
    if (info_->audio) {
        AVStream* as = avformat_new_stream(seg.oc, nullptr);
        avcodec_parameters_copy(as->codecpar, info_->audio);
        as->codecpar->codec_tag = 0;
        as->time_base = info_->audio_tb;
        seg.audio_index = as->index;
    }
}

bool Recorder::open_external(Segment& seg) {
    const std::wstring exe = ExternalEncoder::find();
    if (exe.empty()) {
        set_error(ui_message("rec.ffmpegNotFound"));
        return false;
    }
    const AVRational rate = info_->video_rate.num > 0 ? info_->video_rate : AVRational{60, 1};
    const int fps = std::max(1, int(std::lround(av_q2d(rate))));
    auto ext = std::make_unique<ExternalEncoder>();
    std::string error;
    if (!ext->start(exe, external_args(config_, seg.path, info_->audio != nullptr, fps), error)) {
        set_error(ui_message("rec.ffmpegStartFailed", {{"detail", error}}));
        return false;
    }
    if (avformat_alloc_output_context2(&seg.oc, nullptr, "nut", nullptr) < 0) {
        set_error(ui_message("rec.failed", {{"detail", "cannot create the NUT muxer"}}));
        return false;
    }
    add_copy_streams(seg);
    constexpr int kBufferSize = 256 * 1024;
    auto* buffer = static_cast<unsigned char*>(av_malloc(kBufferSize));
    seg.oc->pb = avio_alloc_context(buffer, kBufferSize, 1, ext.get(), nullptr, &pipe_write, nullptr);
    seg.oc->flags |= AVFMT_FLAG_FLUSH_PACKETS;  // パケットごとに ffmpeg へ渡す (遅れて書かれないように)
    seg.ext = std::move(ext);
    const int err = avformat_write_header(seg.oc, nullptr);
    if (err < 0) {
        set_error(ui_message("rec.ffmpegSendFailed", {{"detail", av_error_string(err) + " " + seg.ext->last_message()}}));
        return false;
    }
    spdlog::info("record: external ffmpeg (pid {}) {} {} {}", seg.ext->pid(), path_utf8(exe), config_.video_codec,
                 config_.rate_control == "cqp" ? "QP " + std::to_string(config_.qp) : std::to_string(config_.bitrate_kbps) + " kbps");
    return true;
}

bool Recorder::open_encoders(Segment& seg, const AVFrame* frame) {
    if (!frame) return false;
    AVRational rate = info_ && info_->video_rate.num > 0 ? info_->video_rate : AVRational{60, 1};
    const int fps = std::max(1, int(std::lround(av_q2d(rate))));
    std::string tried;
    for (const auto& name : encoder_candidates(config_)) {
        const AVCodec* codec = avcodec_find_encoder_by_name(name.c_str());
        if (!codec) {
            tried += name + " (not in this FFmpeg) ";
            continue;
        }
        CodecContextPtr ctx(avcodec_alloc_context3(codec));
        ctx->width = frame->width;
        ctx->height = frame->height;
        ctx->time_base = kEncoderTimeBase;
        ctx->framerate = rate;
        ctx->gop_size = std::max(1, config_.gop_sec * fps);
        ctx->max_b_frames = 0;
        ctx->sample_aspect_ratio = frame->sample_aspect_ratio;

        // 入力の画素形式を受け付けなければ NV12 に変換する
        ctx->pix_fmt = AV_PIX_FMT_NV12;
        const void* configs = nullptr;
        int count = 0;
        if (avcodec_get_supported_config(ctx.get(), codec, AV_CODEC_CONFIG_PIX_FORMAT, 0, &configs, &count) >= 0 && configs) {
            const auto* fmts = static_cast<const AVPixelFormat*>(configs);
            for (int i = 0; i < count; ++i) {
                if (fmts[i] == frame->format) ctx->pix_fmt = AVPixelFormat(frame->format);
            }
        }

        const int64_t bps = int64_t(config_.bitrate_kbps) * 1000;
        AVDictionary* opts = nullptr;
        const bool cqp = config_.rate_control == "cqp";
        if (name.find("nvenc") != std::string::npos) {
            av_dict_set(&opts, "preset", config_.preset.c_str(), 0);
            av_dict_set(&opts, "tune", "hq", 0);
            av_dict_set(&opts, "rc", cqp ? "constqp" : config_.rate_control == "vbr" ? "vbr" : "cbr", 0);
            if (cqp) av_dict_set_int(&opts, "qp", config_.qp, 0);
        } else if (name.find("qsv") != std::string::npos) {
            av_dict_set(&opts, "preset", qsv_preset(config_.preset), 0);
            if (cqp) {
                ctx->flags |= AV_CODEC_FLAG_QSCALE;
                ctx->global_quality = config_.qp * FF_QP2LAMBDA;
            }
        } else if (name.find("amf") != std::string::npos) {
            av_dict_set(&opts, "rc", cqp ? "cqp" : config_.rate_control == "vbr" ? "vbr_peak" : "cbr", 0);
            if (cqp) {
                av_dict_set_int(&opts, "qp_i", config_.qp, 0);
                av_dict_set_int(&opts, "qp_p", config_.qp, 0);
            }
        }
        if (!cqp) {
            ctx->bit_rate = bps;
            ctx->rc_max_rate = config_.rate_control == "vbr" ? bps * 3 / 2 : bps;
            ctx->rc_buffer_size = int(std::min<int64_t>(bps * 2, INT_MAX));
        }
        if (seg.oc->oformat->flags & AVFMT_GLOBALHEADER) ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        const int err = avcodec_open2(ctx.get(), codec, &opts);
        av_dict_free(&opts);
        if (err < 0) {
            tried += name + " (" + av_error_string(err) + ") ";
            continue;
        }
        AVStream* vs = avformat_new_stream(seg.oc, nullptr);
        avcodec_parameters_from_context(vs->codecpar, ctx.get());
        vs->time_base = ctx->time_base;
        vs->avg_frame_rate = rate;
        seg.video_index = vs->index;
        spdlog::info("record: video encoder {} {}x{} {}", name, ctx->width, ctx->height,
                     cqp ? "QP " + std::to_string(config_.qp) : std::to_string(config_.bitrate_kbps) + " kbps");
        seg.venc = std::move(ctx);
        break;
    }
    if (!seg.venc) {
        set_error(ui_message("rec.noGpuEncoder", {{"tried", tried}}));
        return false;
    }

    // 音声 (AAC)。受信に音声がなければ作らない
    if (info_ && info_->audio) {
        const AVCodec* aac = avcodec_find_encoder(AV_CODEC_ID_AAC);
        CodecContextPtr actx(aac ? avcodec_alloc_context3(aac) : nullptr);
        if (actx) {
            actx->sample_fmt = AV_SAMPLE_FMT_FLTP;
            actx->sample_rate = kAudioRate;
            AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
            av_channel_layout_copy(&actx->ch_layout, &stereo);
            actx->bit_rate = int64_t(config_.audio_bitrate_kbps) * 1000;
            actx->time_base = AVRational{1, kAudioRate};
            if (seg.oc->oformat->flags & AVFMT_GLOBALHEADER) actx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
            if (avcodec_open2(actx.get(), aac, nullptr) >= 0) {
                AVStream* as = avformat_new_stream(seg.oc, nullptr);
                avcodec_parameters_from_context(as->codecpar, actx.get());
                as->time_base = actx->time_base;
                seg.audio_index = as->index;
                swr_alloc_set_opts2(&seg.swr, &stereo, AV_SAMPLE_FMT_FLTP, kAudioRate, &stereo, AV_SAMPLE_FMT_FLT, kAudioRate, 0, nullptr);
                swr_init(seg.swr);
                seg.fifo = av_audio_fifo_alloc(AV_SAMPLE_FMT_FLTP, 2, kAudioRate);
                seg.aenc = std::move(actx);
            }
        }
    }
    return true;
}

bool Recorder::write_packet(int stream_index, AVPacket* pkt, AVRational src_tb) {
    AVStream* st = segment_->oc->streams[stream_index];
    const int64_t dts_in = pkt->dts != AV_NOPTS_VALUE ? pkt->dts : pkt->pts;
    if (dts_in == AV_NOPTS_VALUE) return false;
    const int64_t pts_in = pkt->pts != AV_NOPTS_VALUE ? pkt->pts : dts_in;
    const int64_t dts_us = av_rescale_q(dts_in, src_tb, kUs);
    const int64_t pts_us = av_rescale_q(pts_in, src_tb, kUs);
    const bool video = stream_index == segment_->video_index;
    const int64_t out_dts = remap_us(dts_us, video);
    const int64_t out_pts = out_dts + (pts_us - dts_us);
    if (out_dts < 0) return false;  // 映像より前の音声など

    PacketPtr p(av_packet_clone(pkt));
    if (!p) return false;
    p->stream_index = stream_index;
    p->dts = av_rescale_q(out_dts, kUs, st->time_base);
    p->pts = av_rescale_q(out_pts, kUs, st->time_base);
    p->duration = av_rescale_q(pkt->duration, src_tb, st->time_base);
    p->pos = -1;
    int64_t& last = segment_->last_dts[size_t(stream_index)];
    if (last != AV_NOPTS_VALUE && p->dts <= last) {
        if (!video) return false;  // 再接続のつなぎ目で前の音声と重なる分は捨てる
        p->dts = last + 1;         // 単調増加を保つ
        if (p->pts < p->dts) p->pts = p->dts;
    }
    if (video) last_video_out_us_ = std::max(last_video_out_us_, out_dts);
    last = p->dts;
    const int err = av_interleaved_write_frame(segment_->oc, p.get());
    if (err < 0) {
        const std::string detail = segment_->ext ? "ffmpeg: " + segment_->ext->last_message() : av_error_string(err);
        set_error(ui_message("rec.writeFailed", {{"detail", detail}}));
        close_segment("write error");
        retry_open_qpc_ = qpc_now() + qpc_from_ms(10'000);
        return false;
    }
    return true;
}

void Recorder::drain_encoder(AVCodecContext* ctx, int stream_index) {
    PacketPtr pkt(av_packet_alloc());
    AVStream* st = segment_->oc->streams[stream_index];
    while (avcodec_receive_packet(ctx, pkt.get()) == 0) {
        av_packet_rescale_ts(pkt.get(), ctx->time_base, st->time_base);
        pkt->stream_index = stream_index;
        if (av_interleaved_write_frame(segment_->oc, pkt.get()) < 0) {
            av_packet_unref(pkt.get());
            return;
        }
    }
}

void Recorder::encode_video(const AVFrame* frame, int64_t out_us) {
    if (out_us < 0) return;
    Segment& seg = *segment_;
    AVCodecContext* enc = seg.venc.get();
    FramePtr f(av_frame_alloc());
    if (frame->format != enc->pix_fmt) {
        if (!seg.converted) {
            seg.converted.reset(av_frame_alloc());
            seg.converted->format = enc->pix_fmt;
            seg.converted->width = enc->width;
            seg.converted->height = enc->height;
            av_frame_get_buffer(seg.converted.get(), 0);
        }
        seg.sws = sws_getCachedContext(seg.sws, frame->width, frame->height, AVPixelFormat(frame->format), enc->width, enc->height,
                                       enc->pix_fmt, SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!seg.sws || av_frame_make_writable(seg.converted.get()) < 0) return;
        sws_scale(seg.sws, frame->data, frame->linesize, 0, frame->height, seg.converted->data, seg.converted->linesize);
        av_frame_ref(f.get(), seg.converted.get());
    } else {
        av_frame_ref(f.get(), frame);
    }
    int64_t pts = av_rescale_q(out_us, kUs, enc->time_base);
    if (pts <= seg.last_vpts) pts = seg.last_vpts + 1;
    seg.last_vpts = pts;
    f->pts = pts;
    f->pict_type = AV_PICTURE_TYPE_NONE;
    last_video_out_us_ = std::max(last_video_out_us_, out_us);
    if (avcodec_send_frame(enc, f.get()) < 0) return;
    drain_encoder(enc, seg.video_index);
}

void Recorder::encode_audio(const float* data, int frames, int64_t out_us) {
    if (out_us < 0) return;
    Segment& seg = *segment_;
    AVCodecContext* enc = seg.aenc.get();
    // 時刻の飛び (音声の抜け、再接続) が大きければ、送られてきた時刻に合わせ直す
    const int64_t queued_us = int64_t(av_audio_fifo_size(seg.fifo)) * 1'000'000 / kAudioRate;
    if (seg.audio_next_us == AV_NOPTS_VALUE || std::abs(out_us - (seg.audio_next_us + queued_us)) > 100'000) {
        av_audio_fifo_reset(seg.fifo);
        seg.audio_next_us = out_us;
    }
    std::vector<float> left(static_cast<size_t>(frames)), right(static_cast<size_t>(frames));
    uint8_t* planes[2] = {reinterpret_cast<uint8_t*>(left.data()), reinterpret_cast<uint8_t*>(right.data())};
    const uint8_t* in[1] = {reinterpret_cast<const uint8_t*>(data)};
    const int n = swr_convert(seg.swr, planes, frames, in, frames);
    if (n <= 0) return;
    void* write_planes[2] = {left.data(), right.data()};
    av_audio_fifo_write(seg.fifo, write_planes, n);

    const int frame_size = enc->frame_size > 0 ? enc->frame_size : 1024;
    while (av_audio_fifo_size(seg.fifo) >= frame_size) {
        FramePtr f(av_frame_alloc());
        f->nb_samples = frame_size;
        f->format = enc->sample_fmt;
        f->sample_rate = kAudioRate;
        av_channel_layout_copy(&f->ch_layout, &enc->ch_layout);
        if (av_frame_get_buffer(f.get(), 0) < 0) return;
        av_audio_fifo_read(seg.fifo, reinterpret_cast<void**>(f->data), frame_size);
        f->pts = av_rescale_q(seg.audio_next_us, kUs, enc->time_base);
        seg.audio_next_us += int64_t(frame_size) * 1'000'000 / kAudioRate;
        if (avcodec_send_frame(enc, f.get()) < 0) return;
        drain_encoder(enc, seg.audio_index);
    }
}

void Recorder::close_segment(const char* reason) {
    if (!segment_) return;
    Segment& seg = *segment_;
    if (seg.venc) {
        avcodec_send_frame(seg.venc.get(), nullptr);
        drain_encoder(seg.venc.get(), seg.video_index);
    }
    if (seg.aenc) {
        avcodec_send_frame(seg.aenc.get(), nullptr);
        drain_encoder(seg.aenc.get(), seg.audio_index);
    }
    av_write_trailer(seg.oc);
    const double seconds = double(qpc_to_us(qpc_now() - seg.started_qpc)) / 1e6;
    if (seg.ext) {
        // 入力を閉じれば ffmpeg は残りをエンコードして書き終える。待たずに次へ進む
        avio_flush(seg.oc->pb);
        seg.ext->close_input();
        spdlog::info("record: closed {} ({:.0f} s, ffmpeg pid {} is finishing): {}", seg.path, seconds, seg.ext->pid(), reason);
        finishing_.push_back(std::move(seg.ext));
        av_freep(&seg.oc->pb->buffer);
        avio_context_free(&seg.oc->pb);
    } else {
        spdlog::info("record: closed {} ({:.0f} s, {}): {}", seg.path, seconds,
                     seg.oc->pb ? std::to_string(avio_tell(seg.oc->pb) / (1024 * 1024)) + " MB" : "?", reason);
    }
    segment_.reset();
    segment_info_.reset();
    continuation_pending_ = false;
    metrics_.rec_started_qpc = 0;
}

}  // namespace denpachan::worker
