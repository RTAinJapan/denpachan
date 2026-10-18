#include "engine.h"

#include <spdlog/spdlog.h>

#include <stdexcept>

#include "audio_devices.h"

using nlohmann::json;

namespace denpachan::engine {

namespace {

constexpr int64_t kAudioRetryMs = 5000;

std::wstring num(int v) { return std::to_wstring(v); }

const char* worker_state_name(WorkerState s) {
    switch (s) {
    case WorkerState::Stopped: return "stopped";
    case WorkerState::Running: return "running";
    case WorkerState::Restarting: return "restarting";
    }
    return "unknown";
}

const char* video_state_name(uint32_t s) {
    switch (worker_status::VideoState(s)) {
    case worker_status::VideoState::Waiting: return "waiting";
    case worker_status::VideoState::Live: return "live";
    case worker_status::VideoState::Hold: return "hold";
    }
    return "unknown";
}

int channel_param(const json& params) {
    const int ch = params.at("ch").get<int>();
    if (ch < 1 || ch > kChannelCount) throw std::runtime_error("ch must be 1-4");
    return ch;
}

}  // namespace

Engine::Engine(std::wstring config_path, std::wstring worker_exe, std::wstring log_dir)
    : config_path_(std::move(config_path)), worker_exe_(std::move(worker_exe)), log_dir_(std::move(log_dir)) {
    for (int i = 0; i < kChannelCount; ++i) workers_[size_t(i)] = std::make_unique<WorkerProcess>(i + 1, worker_exe_);
    const std::wstring dir = worker_exe_.substr(0, worker_exe_.find_last_of(L"\\/"));
    rtmpd_ = std::make_unique<WorkerProcess>(0, dir + L"\\denpachan-rtmpd.exe", "rtmpd");
}

Engine::~Engine() = default;

void Engine::init() {
    std::lock_guard lk(mutex_);
    std::string error;
    if (load_config(config_path_, config_, error)) {
        spdlog::info("config loaded: {}", to_utf8(config_path_));
    } else if (error == "not found") {
        config_ = default_config();
        save_locked();
        spdlog::info("config created: {}", to_utf8(config_path_));
    } else {
        // 壊れた設定は退避して既定値で起動する (全チャンネル停止)
        spdlog::error("config is invalid ({}); starting with defaults", error);
        MoveFileExW(config_path_.c_str(), (config_path_ + L".broken").c_str(), MOVEFILE_REPLACE_EXISTING);
        config_ = default_config();
        save_locked();
    }

    // 仮想カメラの共有メモリを保持する (誰も開いていないと最後のフレームが消えるため)
    for (int cam = 1; cam <= vcam::kNumCams; ++cam) {
        vcam_headers_[cam - 1] = vcam::open_shared(vcam_mappings_[cam - 1], cam);
        if (!vcam_headers_[cam - 1]) spdlog::warn("vcam {}: cannot open shared memory", cam);
    }

    routes_ = rtmp_routes::open_shared(routes_mapping_);
    if (!routes_) spdlog::error("cannot open RTMP routes shared memory");

    // 前回動いていたチャンネルを再開する (同じ設定で動いている worker / rtmpd は引き継ぐ)
    apply_rtmp_locked();
    for (const auto& ch : config_.channels) apply_channel_locked(ch.id);
    audio_.apply(config_);
}

void Engine::apply_rtmp_locked() {
    rtmp_routes::Route routes[rtmp_routes::kMaxRoutes] = {};
    bool any_push = false;
    for (const auto& c : config_.channels) {
        auto& r = routes[c.id - 1];
        r.ch = uint32_t(c.id);
        if (!c.enabled || c.source.protocol != "rtmp" || c.source.mode != "push") continue;
        r.enabled = 1;
        strncpy_s(r.app, c.source.app.c_str(), _TRUNCATE);
        strncpy_s(r.key, c.source.key.c_str(), _TRUNCATE);
        any_push = true;
    }
    if (routes_) rtmp_routes::write_routes(routes_, routes);

    if (any_push) {
        const auto& g = config_.general;
        rtmpd_->start(L"--port " + num(g.rtmp_port) + L" --bind " + quote_arg(from_utf8(g.rtmp_bind)) +
                      L" --idle-timeout-ms " + num(g.rtmp_idle_timeout_ms) + L" --log-file " +
                      quote_arg(log_dir_ + L"\\rtmpd.log"));
    } else {
        rtmpd_->stop();
    }
}

void Engine::tick() {
    std::lock_guard lk(mutex_);
    rtmpd_->poll();
    for (auto& w : workers_) w->poll();
    if (audio_.has_missing() && qpc_now() - last_audio_retry_qpc_ > qpc_from_ms(kAudioRetryMs)) {
        last_audio_retry_qpc_ = qpc_now();
        audio_.apply(config_);
    }
}

void Engine::shutdown(bool stop_workers) {
    std::lock_guard lk(mutex_);
    if (stop_workers) {
        for (auto& w : workers_) w->stop();
        rtmpd_->stop();
    }
}

void Engine::save_locked() {
    std::string error;
    if (!save_config(config_path_, config_, error)) spdlog::error("cannot save config: {}", error);
}

std::wstring Engine::worker_args(const ChannelConfig& ch) const {
    const auto& s = ch.source;
    std::wstring a = L"--ch " + num(ch.id) + L" --cam " + num(ch.vcam) + L" --buffer " + num(ch.playout_buffer_ms) +
                     L" --stall-ms " + num(config_.general.stall_ms);
    if (s.protocol == "rtmp" && s.mode == "push") {
        a += L" --input rtmp-push";  // rtmpd がこのチャンネルの FLV リングに書く
    } else if (s.protocol == "rtmp") {
        a += L" --input url --url " + quote_arg(from_utf8(s.url));
    } else {
        a += L" --input srt --srt-mode " + from_utf8(s.mode) + L" --host " + quote_arg(from_utf8(s.host)) +
             L" --port " + num(s.port) + L" --latency " + num(s.latency_ms);
        if (!s.passphrase.empty()) a += L" --passphrase " + quote_arg(from_utf8(s.passphrase));
        if (s.pbkeylen != 0) a += L" --pbkeylen " + num(s.pbkeylen);
        if (!s.streamid.empty()) a += L" --streamid " + quote_arg(from_utf8(s.streamid));
    }
    if (!config_.general.av_sync) a += L" --no-av-sync";
    a += L" --log-file " + quote_arg(log_dir_ + L"\\ch" + num(ch.id) + L".log");
    if (!ch.debug_args.empty()) a += L" " + from_utf8(ch.debug_args);
    return a;
}

void Engine::apply_channel_locked(int ch) {
    const auto& c = config_.channels[size_t(ch - 1)];
    auto& w = *workers_[size_t(ch - 1)];
    if (c.enabled) {
        w.start(worker_args(c));  // 同じ設定で動いていれば何もしない
    } else {
        w.stop();
    }
}

json Engine::handle(const std::string& method, const json& params) {
    std::lock_guard lk(mutex_);

    if (method == "getConfig") return to_json(config_, JsonForm::Ipc);
    if (method == "getState") return state_locked();

    if (method == "listAudioDevices") {
        json arr = json::array();
        for (const auto& d : list_audio_devices(eRender)) {
            arr.push_back({{"id", to_utf8(d.id)}, {"name", to_utf8(d.name)}, {"channels", d.channels},
                           {"sampleRate", d.sample_rate}});
        }
        return arr;
    }

    if (method == "startChannel" || method == "stopChannel") {
        const int ch = channel_param(params);
        config_.channels[size_t(ch - 1)].enabled = method == "startChannel";
        save_locked();
        apply_rtmp_locked();
        apply_channel_locked(ch);
        audio_.apply(config_);
        return {{"ok", true}};
    }

    if (method == "patchChannel") {
        // JSON Merge Patch (RFC 7396) で一部だけ変更する
        const int ch = channel_param(params);
        json j = to_json(config_.channels[size_t(ch - 1)], JsonForm::Internal);
        j.merge_patch(params.at("patch"));
        Config candidate = config_;
        candidate.channels[size_t(ch - 1)] = channel_from_json(j, ch);
        if (auto err = validate(candidate)) throw std::runtime_error(*err);
        config_ = candidate;
        save_locked();
        apply_rtmp_locked();
        apply_channel_locked(ch);  // worker に渡す設定が変わっていれば再起動される
        audio_.apply(config_);
        return to_json(config_.channels[size_t(ch - 1)], JsonForm::Ipc);
    }

    if (method == "patchGeneral") {
        json j = to_json(config_.general);
        j.merge_patch(params.at("patch"));
        Config candidate = config_;
        candidate.general = general_from_json(j);
        if (auto err = validate(candidate)) throw std::runtime_error(*err);
        config_ = candidate;
        save_locked();
        apply_rtmp_locked();
        for (const auto& c : config_.channels) apply_channel_locked(c.id);
        audio_.apply(config_);
        return to_json(config_.general);
    }

    if (method == "setMute") {
        const int ch = channel_param(params);
        const bool mute = params.at("mute").get<bool>();
        config_.channels[size_t(ch - 1)].audio.mute = mute;
        save_locked();
        audio_.set_mute(ch, mute);
        return {{"ok", true}};
    }

    if (method == "resetCam") {
        const int cam = params.at("cam").get<int>();
        if (cam < 1 || cam > vcam::kNumCams || !vcam_headers_[cam - 1]) throw std::runtime_error("invalid cam");
        vcam::reset_display(vcam_headers_[cam - 1]);
        return {{"ok", true}};
    }

    if (method == "shutdown") {
        // UI の「終了」: 応答を返してから main ループが止める
        shutdown_request_ = params.value("keepWorkers", false) ? 2 : 1;
        return {{"ok", true}};
    }

    throw std::runtime_error("unknown method: " + method);
}

json Engine::state() {
    std::lock_guard lk(mutex_);
    return state_locked();
}

json Engine::state_locked() {
    json channels = json::array();
    for (const auto& c : config_.channels) {
        const WorkerSnapshot w = workers_[size_t(c.id - 1)]->snapshot();
        json j = {
            {"id", c.id},
            {"name", c.name},
            {"enabled", c.enabled},
            {"vcam", c.vcam},
            {"source",
             c.source.protocol == "rtmp"
                 ? json{{"protocol", "rtmp"}, {"mode", c.source.mode}, {"app", c.source.app}}
                 : json{{"protocol", "srt"}, {"mode", c.source.mode}, {"host", c.source.host}, {"port", c.source.port}}},
            {"worker",
             {{"state", worker_state_name(w.state)},
              {"pid", w.pid},
              {"restarts", w.restarts},
              {"uptimeSec", w.uptime_sec},
              {"lastEvent", w.last_event}}},
        };
        if (w.has_metrics) {
            const auto& m = w.metrics;
            j["video"] = {{"state", video_state_name(m.video_state)},
                          {"connected", m.connected != 0},
                          {"connections", m.connections},
                          {"peer", m.peer},
                          {"codec", m.codec},
                          {"width", m.width},
                          {"height", m.height},
                          {"hwDecode", m.hw_decode != 0},
                          {"bitrateMbps", m.bitrate_mbps},
                          {"decodeFps", m.decode_fps},
                          {"outputFps", m.output_fps},
                          {"delayMs", m.video_delay_ms},
                          {"droppedLate", m.dropped_late},
                          {"droppedQueue", m.dropped_queue},
                          {"decodeErrors", m.decode_errors},
                          {"lastError", m.last_error}};
            if (c.source.protocol == "srt") {
                j["srt"] = {{"latencyMs", m.srt_latency_ms}, {"rttMs", m.srt_rtt_ms}, {"rcvBufMs", m.srt_rcv_buf_ms},
                            {"recvMbps", m.srt_recv_mbps},   {"lost", m.srt_lost},    {"retrans", m.srt_retrans},
                            {"dropped", m.srt_dropped}};
            } else if (c.source.mode == "push") {
                j["tcp"] = {{"rttMs", m.tcp_rtt_ms}, {"retransBytes", m.tcp_retrans_bytes}};
            }
            j["audioIn"] = {{"peakDb", {m.audio_peak_l_db, m.audio_peak_r_db}},
                            {"delayMs", m.audio_delay_ms},
                            {"avOffsetMs", m.av_offset_ms},
                            {"gaps", m.audio_gaps}};
        }
        j["audioOut"] = audio_.channel_status(c.id);
        channels.push_back(j);
    }
    const WorkerSnapshot r = rtmpd_->snapshot();
    json rtmp = {{"state", worker_state_name(r.state)},
                 {"pid", r.pid},
                 {"restarts", r.restarts},
                 {"uptimeSec", r.uptime_sec},
                 {"lastEvent", r.last_event},
                 {"port", config_.general.rtmp_port}};
    return {{"channels", channels}, {"rtmpServer", rtmp}, {"audioDevices", audio_.devices_status()}};
}

}  // namespace denpachan::engine
