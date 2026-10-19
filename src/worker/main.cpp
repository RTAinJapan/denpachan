// denpachan-worker: 1 チャンネル分の受信 → デコード → 仮想カメラ出力。
//
// 普段は engine が起動・監視する。試験や調べものでは単体でも起動できる。
//   denpachan-worker --ch 1 --cam 1 --port 9001
//   denpachan-worker --ch 2 --cam 2 --srt-mode caller --host 203.0.113.10 --port 5000 --passphrase xxxxxxxxxx

#include <srt/srt.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <atomic>
#include <charconv>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <cmath>

#include "audio_writer.h"
#include "byte_queue.h"
#include "denpachan/crash_dump.h"
#include "denpachan/ffmpeg_runtime.h"
#include "denpachan/worker_status.h"
#include "decoder.h"
#include "filename.h"
#include "playout_clock.h"
#include "denpachan/win_util.h"
#include "flv_ring_ingest.h"
#include "recorder.h"
#include "frame_queue.h"
#include "metrics.h"
#include "playout.h"
#include "srt_ingest.h"
#include "vcam_writer.h"

using namespace denpachan;
using namespace denpachan::worker;

namespace {

struct Options {
    int ch = 1;
    int cam = 1;  // 0 = vcam に出さない
    std::string input = "srt";  // srt / rtmp-push / url
    std::string url;            // input = url のとき (rtmp:// など)
    SrtOptions srt;
    PlayoutOptions playout;
    bool hw = true;
    bool audio = true;
    bool av_sync = true;
    std::string log_file;
};

void print_usage() {
    std::fputs(
        "usage: denpachan-worker [options]\n"
        "  --ch N              channel number (log label)         [1]\n"
        "  --cam N             virtual camera 1-4, 0 = none       [1]\n"
        "  --input I           srt | rtmp-push (from rtmpd) | url  [srt]\n"
        "  --url URL           input = url: e.g. rtmp://host/app/key (RTMP pull)\n"
        "  --srt-mode M        listener | caller                  [listener]\n"
        "  --host H            listener: bind address / caller: remote host [0.0.0.0]\n"
        "  --port P            SRT port (required)\n"
        "  --latency MS        SRT latency; the larger of ours and the sender's is used [120]\n"
        "  --passphrase S      SRT passphrase (10-79 chars)\n"
        "  --pbkeylen N        16 | 24 | 32\n"
        "  --streamid S        SRT stream id (caller)\n"
        "  --buffer MS         playout buffer                     [0]\n"
        "  --stall-ms MS       time without video before state becomes Hold [2000]\n"
        "  --no-hw             disable hardware decoding\n"
        "  --no-audio          do not output audio\n"
        "  --no-av-sync        output audio and video each with minimum delay (A/V may drift apart)\n"
        "  --log-file PATH     also log to a rotating file\n"
        "\n"
        "usage: denpachan-worker --reset-cam N\n"
        "  reset camera N to NO SIGNAL (until video is received again)\n",
        stderr);
}

std::optional<int> to_int(std::string_view s) {
    int v = 0;
    const auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (ec != std::errc{} || p != s.data() + s.size()) return std::nullopt;
    return v;
}

std::optional<Options> parse_args(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        auto value = [&]() -> std::optional<std::string_view> {
            if (i + 1 >= argc) return std::nullopt;
            return std::string_view(argv[++i]);
        };
        auto int_value = [&](int& out) {
            const auto v = value();
            const auto n = v ? to_int(*v) : std::nullopt;
            if (n) out = *n;
            return n.has_value();
        };
        auto str_value = [&](std::string& out) {
            const auto v = value();
            if (v) out = std::string(*v);
            return v.has_value();
        };

        bool ok = true;
        if (a == "--ch") ok = int_value(o.ch);
        else if (a == "--cam") ok = int_value(o.cam);
        else if (a == "--input") ok = str_value(o.input);
        else if (a == "--url") ok = str_value(o.url);
        else if (a == "--srt-mode") ok = str_value(o.srt.mode);
        else if (a == "--host") ok = str_value(o.srt.host);
        else if (a == "--port") ok = int_value(o.srt.port);
        else if (a == "--latency") ok = int_value(o.srt.latency_ms);
        else if (a == "--passphrase") ok = str_value(o.srt.passphrase);
        else if (a == "--pbkeylen") ok = int_value(o.srt.pbkeylen);
        else if (a == "--streamid") ok = str_value(o.srt.streamid);
        else if (a == "--buffer") ok = int_value(o.playout.buffer_ms);
        else if (a == "--stall-ms") ok = int_value(o.playout.stall_ms);
        else if (a == "--no-hw") o.hw = false;
        else if (a == "--no-audio") o.audio = false;
        else if (a == "--no-av-sync") o.av_sync = false;
        else if (a == "--test-hang-playout") ok = int_value(o.playout.test_hang_after_sec);
        else if (a == "--log-file") ok = str_value(o.log_file);
        else ok = false;

        if (!ok) {
            std::fprintf(stderr, "invalid argument: %s\n", argv[i]);
            return std::nullopt;
        }
    }
    if (o.cam < 0 || o.cam > vcam::kNumCams) return std::nullopt;
    if (o.input == "srt") {
        if (o.srt.port <= 0 || (o.srt.mode != "listener" && o.srt.mode != "caller")) return std::nullopt;
    } else if (o.input == "url") {
        if (o.url.empty()) return std::nullopt;
    } else if (o.input != "rtmp-push") {
        return std::nullopt;
    }
    return o;
}

UniqueHandle g_stop_event;

BOOL WINAPI on_console_ctrl(DWORD) {
    SetEvent(g_stop_event.get());
    return TRUE;
}

void setup_logging(const Options& o) {
    std::vector<spdlog::sink_ptr> sinks{std::make_shared<spdlog::sinks::stdout_color_sink_mt>()};
    if (!o.log_file.empty()) {
        sinks.push_back(std::make_shared<spdlog::sinks::rotating_file_sink_mt>(o.log_file, 10 * 1024 * 1024, 5));
    }
    auto logger = std::make_shared<spdlog::logger>("ch" + std::to_string(o.ch), sinks.begin(), sinks.end());
    logger->set_pattern("%Y-%m-%d %H:%M:%S.%e [%n] %^%l%$ %v");
    logger->flush_on(spdlog::level::warn);
    spdlog::set_default_logger(logger);
    spdlog::flush_every(std::chrono::seconds(1));
}

// 1 秒ごとのメトリクス出力 (ログと、engine に見せる共有メモリ)
class MetricsReporter {
public:
    explicit MetricsReporter(ChannelMetrics& m) : m_(m) {}

    void report() {
        const uint64_t bytes = m_.bytes_in.load();
        const uint64_t decoded = m_.frames_decoded.load();
        const uint64_t presented = m_.frames_presented.load();
        std::string peer, codec, last_error;
        SrtStats srt;
        {
            std::lock_guard lk(m_.mutex);
            peer = m_.peer;
            codec = m_.codec;
            last_error = m_.last_error;
            srt = m_.srt;
        }
        const uint64_t audio_frames = m_.audio_frames.load();
        const float peak_l = m_.audio_peak_l.exchange(0);
        const float peak_r = m_.audio_peak_r.exchange(0);
        auto dbfs = [](float v) { return v > 1e-5f ? 20.0 * std::log10(v) : -100.0; };
        if (m_.connected) {
            spdlog::info(
                "{} {} {}x{} {} | {:.2f} Mbps | dec {} fps, out {} fps, delay {} ms | drop late {} queue {} | "
                "err {} | audio {} Hz peak {:.1f}/{:.1f} dBFS gaps {} delay {} ms a/v {:+} ms | "
                "srt latency {} ms rtt {:.1f} ms buf {} ms lost {} retrans {} drop {}",
                peer, codec, m_.width.load(), m_.height.load(), m_.hw_decode ? "hw" : "sw",
                double(bytes - last_bytes_) * 8 / 1e6, decoded - last_decoded_, presented - last_presented_,
                m_.delay_us.load() / 1000, m_.frames_dropped_late.load(), m_.frames_dropped_queue.load(),
                m_.decode_errors.load(), audio_frames - last_audio_frames_, dbfs(peak_l), dbfs(peak_r),
                m_.audio_gaps.load(), m_.audio_delay_us.load() / 1000, m_.av_offset_us.load() / 1000,
                srt.tsbpd_delay_ms, srt.rtt_ms, srt.rcv_buf_ms, srt.lost_total,
                srt.retrans_total, srt.dropped_total);
        }

        if (m_.status) {
            worker_status::Metrics s{};
            s.connected = m_.connected ? 1 : 0;
            s.connections = m_.connections.load();
            strncpy_s(s.peer, peer.c_str(), _TRUNCATE);
            strncpy_s(s.codec, codec.c_str(), _TRUNCATE);
            strncpy_s(s.last_error, last_error.c_str(), _TRUNCATE);
            s.width = m_.width.load();
            s.height = m_.height.load();
            s.hw_decode = m_.hw_decode ? 1 : 0;
            s.video_state = m_.video_state.load();
            s.bitrate_mbps = double(bytes - last_bytes_) * 8 / 1e6;
            s.decode_fps = double(decoded - last_decoded_);
            s.output_fps = double(presented - last_presented_);
            s.video_delay_ms = double(m_.delay_us.load()) / 1000;
            s.dropped_late = m_.frames_dropped_late.load();
            s.dropped_queue = m_.frames_dropped_queue.load();
            s.decode_errors = m_.decode_errors.load();
            s.overflow_bytes = m_.overflow_bytes.load();
            s.srt_latency_ms = srt.tsbpd_delay_ms;
            s.srt_rcv_buf_ms = srt.rcv_buf_ms;
            s.srt_rtt_ms = srt.rtt_ms;
            s.srt_recv_mbps = srt.recv_mbps;
            s.srt_lost = srt.lost_total;
            s.srt_retrans = srt.retrans_total;
            s.srt_dropped = srt.dropped_total;
            s.srt_rec_latency_ms = srt.advice.latency_ms;
            s.srt_rec_multiplier = srt.advice.multiplier;
            s.srt_rec_rtt_ms = srt.advice.rtt_ms;
            s.srt_rec_loss_pct = srt.advice.loss_pct;
            s.srt_rec_drops = srt.advice.drops;
            s.srt_rec_window_sec = srt.advice.window_sec;
            s.tcp_rtt_ms = double(m_.tcp_rtt_us.load()) / 1000;
            s.tcp_retrans_bytes = m_.tcp_retrans_bytes.load();
            s.audio_peak_l_db = dbfs(peak_l);
            s.audio_peak_r_db = dbfs(peak_r);
            s.audio_delay_ms = double(m_.audio_delay_us.load()) / 1000;
            s.av_offset_ms = double(m_.av_offset_us.load()) / 1000;
            s.audio_gaps = m_.audio_gaps.load();
            s.rec_state = m_.rec_state.load();
            const int64_t rec_started = m_.rec_started_qpc.load();
            s.rec_seconds = rec_started ? double(qpc_to_us(qpc_now() - rec_started)) / 1e6 : 0;
            s.rec_bytes = m_.rec_bytes.load();
            s.rec_dropped = m_.rec_dropped.load();
            {
                std::lock_guard lk(m_.mutex);
                strncpy_s(s.rec_file, m_.rec_file.c_str(), _TRUNCATE);
                strncpy_s(s.rec_error, m_.rec_error.c_str(), _TRUNCATE);
            }
            worker_status::write_metrics(m_.status, s);
        }
        last_bytes_ = bytes;
        last_decoded_ = decoded;
        last_presented_ = presented;
        last_audio_frames_ = audio_frames;
    }

private:
    ChannelMetrics& m_;
    uint64_t last_bytes_ = 0;
    uint64_t last_decoded_ = 0;
    uint64_t last_presented_ = 0;
    uint64_t last_audio_frames_ = 0;
};

}  // namespace

// 仮想カメラの表示を NO SIGNAL に戻す (試験用。普段は画面 → engine の resetCam で行う)
int reset_cam(std::string_view arg) {
    const auto cam = to_int(arg);
    if (!cam || *cam < 1 || *cam > vcam::kNumCams) {
        print_usage();
        return 2;
    }
    SharedMapping mapping;
    vcam::Header* header = vcam::open_shared(mapping, *cam);
    if (!header) {
        std::fprintf(stderr, "cannot open vcam %d\n", *cam);
        return 1;
    }
    vcam::reset_display(header);
    std::fprintf(stderr, "vcam %d reset to NO SIGNAL\n", *cam);
    return 0;
}

int main(int argc, char** argv) {
    if (argc == 3 && std::string_view(argv[1]) == "--reset-cam") return reset_cam(argv[2]);

    const auto opts = parse_args(argc, argv);
    if (!opts) {
        print_usage();
        return 2;
    }
    setup_logging(*opts);
    install_crash_dump(local_app_dir(L"dumps"), L"worker-ch" + std::to_wstring(opts->ch));

    // FFmpeg は同梱しない (遅延ロード)。最初に使う前に、DLL があって版が合うか確かめる
    if (const auto ff = ffmpeg_runtime::check_all(); !ff.ok) {
        spdlog::critical("{}", ff.message);
        return int(worker_status::kExitFfmpegUnavailable);
    }
    spdlog::info("FFmpeg: avcodec {}.{}, avformat {}.{}: {}", avcodec_version() >> 16, (avcodec_version() >> 8) & 0xff,
                 avformat_version() >> 16, (avformat_version() >> 8) & 0xff, path_utf8(ffmpeg_runtime::loaded_dir()));

    // 配信中にスリープ・省電力で止まらないようにする (設計書 §4)
    SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);
    SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED);
    PROCESS_POWER_THROTTLING_STATE throttling{};
    throttling.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    throttling.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    throttling.StateMask = 0;  // EcoQoS を使わない
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &throttling, sizeof(throttling));

    g_stop_event.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    SetConsoleCtrlHandler(on_console_ctrl, TRUE);
    // engine からの停止要求 (コンソールがないので Ctrl+C は届かない)
    SharedSecurity sec;
    UniqueHandle engine_stop(CreateEventW(sec.get(), TRUE, FALSE, worker_status::stop_event_name(opts->ch).c_str()));
    ResetEvent(engine_stop.get());

    if (srt_startup() < 0) {
        spdlog::critical("srt_startup failed");
        return 1;
    }

    ChannelMetrics metrics;

    // engine に見せる状態。engine は pid と args_hash で、再起動後にこの worker を引き継げるか判断する。
    SharedMapping status_mapping;
    if (auto* status = worker_status::open_shared(status_mapping, opts->ch)) {
        status->metrics_seq.store(0);
        status->pid.store(GetCurrentProcessId());
        status->args_hash.store(worker_status::hash_args(GetCommandLineW()));
        status->started_qpc.store(qpc_now());
        status->ingest_heartbeat_qpc.store(qpc_now());
        status->playout_heartbeat_qpc.store(qpc_now());
        status->decoder_busy_since_qpc.store(0);
        metrics.status = status;
    } else {
        spdlog::warn("cannot open worker status shared memory");
    }
    ByteQueue bytes(16 * 1024 * 1024);
    FrameQueue frames(24);

    std::unique_ptr<VcamWriter> writer;
    if (opts->cam > 0) {
        writer = std::make_unique<VcamWriter>();
        if (!writer->open(opts->cam)) return 1;
    }

    PlayoutClock clock(opts->playout.buffer_ms);
    std::unique_ptr<AudioWriter> audio;
    if (opts->audio) {
        audio = std::make_unique<AudioWriter>(clock, metrics, opts->av_sync);
        if (!audio->open(opts->ch)) return 1;
    }

    // 入力: SRT (MPEG-TS) / rtmpd からの FLV / URL を直接 (RTMP pull)
    std::unique_ptr<SrtIngest> srt_ingest;
    std::unique_ptr<FlvRingIngest> flv_ingest;
    const char* input_format = "mpegts";
    if (opts->input == "srt") {
        srt_ingest = std::make_unique<SrtIngest>(opts->srt, bytes, metrics);
    } else if (opts->input == "rtmp-push") {
        flv_ingest = std::make_unique<FlvRingIngest>(opts->ch, bytes, metrics);
        input_format = "flv";
    }
    Decoder decoder(input_format, bytes, frames, audio.get(), metrics, opts->hw);
    if (opts->input == "url") decoder.set_url(opts->url);
    if (flv_ingest) decoder.set_resync([&] { flv_ingest->request_resync(); });
    Recorder recorder(opts->ch, metrics);
    decoder.set_recorder(&recorder);
    recorder.start();
    // 録画の設定は engine が共有メモリに書く (受信を止めずに変えられるように)
    uint32_t record_seq = 0;
    auto apply_record_config = [&] {
        std::string json;
        if (metrics.status && worker_status::read_record_config(metrics.status, record_seq, json)) {
            recorder.configure(parse_record_config(json, opts->ch));
        }
    };
    apply_record_config();
    PreviewWriter preview;
    preview.open(opts->ch);
    Playout playout(opts->playout, clock, frames, writer.get(), &preview, metrics);

    spdlog::info("worker start: ch {} ({}) -> cam {}", opts->ch, opts->input, opts->cam);
    playout.start();
    decoder.start();
    if ((srt_ingest && !srt_ingest->start()) || (flv_ingest && !flv_ingest->start())) {
        bytes.stop();
        frames.stop();
        decoder.stop();
        playout.stop();
        return 1;
    }

    MetricsReporter reporter(metrics);
    HANDLE stop_handles[2] = {g_stop_event.get(), engine_stop.get()};
    // 録画の設定は 100 ms ごとに見る (オン・オフをすぐ反映する)。状態の報告は 1 秒ごと。
    int64_t next_report = qpc_now() + qpc_from_ms(1000);
    while (WaitForMultipleObjects(2, stop_handles, FALSE, 100) == WAIT_TIMEOUT) {
        apply_record_config();
        if (qpc_now() >= next_report) {
            next_report += qpc_from_ms(1000);
            reporter.report();
        }
    }

    spdlog::info("worker stopping");
    if (srt_ingest) srt_ingest->stop();
    if (flv_ingest) flv_ingest->stop();
    bytes.stop();
    decoder.stop();
    recorder.stop();  // 開いているファイルを閉じる
    frames.stop();
    playout.stop();
    if (writer) writer->close();
    if (audio) audio->close();
    if (metrics.status && metrics.status->pid.load() == GetCurrentProcessId()) metrics.status->pid.store(0);
    srt_cleanup();
    spdlog::info("worker stopped");
    return 0;
}
