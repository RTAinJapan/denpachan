#include "metrics_view.h"

#include <windows.h>

#include <cstdio>
#include <string>

#include "denpachan/win_util.h"

using nlohmann::json;

namespace denpachan::engine {

namespace {

const json& field(const json& j, const char* key) {
    static const json null_json;
    if (!j.is_object()) return null_json;
    const auto it = j.find(key);
    return it == j.end() ? null_json : *it;
}

double num(const json& j, const char* key, double def = 0) {
    const json& v = field(j, key);
    return v.is_number() ? v.get<double>() : def;
}

// 数ならそのまま (整数は整数のまま) 返す。無ければ def
json pick(const json& j, const char* key, json def = 0) {
    const json& v = field(j, key);
    return v.is_number() ? v : def;
}

std::string str(const json& j, const char* key) {
    const json& v = field(j, key);
    return v.is_string() ? v.get<std::string>() : std::string();
}

int64_t seconds_since(int64_t since_qpc, int64_t now_qpc) { return since_qpc == 0 ? 0 : qpc_to_us(now_qpc - since_qpc) / 1'000'000; }

}  // namespace

std::string utc_now_iso8601() {
    SYSTEMTIME t;
    GetSystemTime(&t);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond,
                  t.wMilliseconds);
    return buf;
}

double ChannelsView::Window::add(double cumulative, int64_t now_qpc) {
    if (!samples_.empty() && cumulative < samples_.back().second) samples_.clear();  // カウンタが戻った (worker の再起動)
    samples_.emplace_back(now_qpc, cumulative);
    while (samples_.size() > 1 && now_qpc - samples_.front().first > qpc_from_ms(kWindowMs)) samples_.pop_front();
    return cumulative - samples_.front().second;
}

json ChannelsView::build(const json& state, const Config& config, int64_t now_qpc) {
    json channels = json::array();
    for (const auto& s : field(state, "channels")) {
        const int id = int(num(s, "id"));
        if (id < 1 || id > kMaxChannels) continue;
        const ChannelConfig& cfg = config.channels[size_t(id - 1)];
        Watch& w = watch_[size_t(id - 1)];
        const json& video = field(s, "video");
        const json& srt = field(s, "srt");
        const json& tcp = field(s, "tcp");
        const json& audio_in = field(s, "audioIn");
        const json& record = field(s, "record");
        const std::string worker_state = str(field(s, "worker"), "state");
        const std::string& protocol = cfg.source.protocol;
        const std::string& mode = cfg.source.mode;

        // 状態 (画面のタイルと同じ区分。ChannelViewModel.ApplyState)
        const bool has_video = video.is_object();
        const bool connected = has_video && field(video, "connected").is_boolean() && video.at("connected").get<bool>();
        const std::string video_state = str(video, "state");
        const bool outgoing = (protocol == "rtmp" && mode == "pull") || (protocol == "srt" && mode == "caller");
        const json& last_error = has_video ? field(video, "lastError") : field(s, "none");
        const bool unreachable = outgoing && !connected && !last_error.is_null();
        std::string status;
        if (worker_state == "stopped" || worker_state.empty()) {
            status = "stopped";
        } else if (worker_state == "unavailable") {
            status = "unavailable";
        } else if (worker_state == "restarting") {
            status = "restarting";
        } else if (!has_video) {
            status = "starting";
        } else if (video_state == "live") {
            status = "live";
        } else if (video_state == "hold") {
            status = "hold";
        } else if (unreachable) {
            status = "unreachable";
        } else if (connected) {
            status = "waiting";  // 繋がったが映像がまだ
        } else if (outgoing) {
            status = "connecting";
        } else {
            status = "listening";
        }

        if (connected) {
            if (w.connected_since_qpc == 0) w.connected_since_qpc = now_qpc;
        } else {
            w.connected_since_qpc = 0;
        }

        json source = {{"protocol", protocol}, {"mode", mode}};
        if (protocol == "srt") {
            if (mode == "caller") source["host"] = cfg.source.host;
            source["port"] = cfg.source.port;
        } else if (mode == "push") {
            source["app"] = cfg.source.app;
        }
        source["peer"] = str(video, "peer");
        source["connectedSec"] = seconds_since(w.connected_since_qpc, now_qpc);

        json video_out = nullptr;
        if (has_video) {
            video_out = {{"codec", str(video, "codec")},
                         {"width", pick(video, "width")},
                         {"height", pick(video, "height")},
                         {"fps", pick(video, "decodeFps")},
                         {"outputFps", pick(video, "outputFps")},
                         {"bitrateMbps", pick(video, "bitrateMbps")},
                         {"hwDecode", field(video, "hwDecode").is_boolean() && video.at("hwDecode").get<bool>()},
                         {"decodeErrors", pick(video, "decodeErrors")},
                         {"droppedLate", pick(video, "droppedLate")},
                         {"droppedQueue", pick(video, "droppedQueue")}};
        }

        // 経路の判定 (画面の経路の行の色と同じ規則)
        json latency = nullptr;
        if (srt.is_object()) {
            const json& rec = field(srt, "recommend");
            const double effective = num(srt, "latencyMs");
            const double recommend = num(rec, "latencyMs");
            const double dropped = w.dropped.add(num(srt, "dropped"), now_qpc);
            const char* verdict = dropped > 0                  ? "insufficient"
                                  : recommend <= 0             ? "measuring"
                                  : recommend > effective      ? "tight"
                                  : effective >= 2 * recommend ? "generous"
                                                               : "ok";
            latency = {{"effectiveMs", pick(srt, "latencyMs")},
                       {"receiverMs", pick(srt, "configuredLatencyMs")},
                       {"rttMs", pick(rec, "rttMs")},
                       {"lossPct", pick(rec, "lossPct")},
                       {"drops", pick(rec, "drops")},
                       {"windowSec", pick(rec, "windowSec")},
                       {"recommendMs", pick(rec, "latencyMs")},
                       {"multiplier", pick(rec, "multiplier")},
                       {"headroomMs", recommend > 0 ? json(int64_t(effective - recommend)) : json(nullptr)},
                       {"droppedLastMin", int64_t(dropped)},
                       {"verdict", verdict}};
        } else {
            w.dropped.reset();
        }
        json tcp_out = nullptr;
        if (tcp.is_object()) {
            const double retrans = w.retrans.add(num(tcp, "retransBytes"), now_qpc);
            tcp_out = {{"rttMs", pick(tcp, "rttMs")},
                       {"retransBytes", pick(tcp, "retransBytes")},
                       {"retransBytesLastMin", int64_t(retrans)},
                       {"verdict", retrans > 0 ? "tight" : "ok"}};
        } else {
            w.retrans.reset();
        }

        json audio = nullptr;
        if (audio_in.is_object()) {
            const json& peak = field(audio_in, "peakDb");
            const double l = peak.is_array() && peak.size() == 2 && peak[0].is_number() ? peak[0].get<double>() : -100.0;
            const double r = peak.is_array() && peak.size() == 2 && peak[1].is_number() ? peak[1].get<double>() : -100.0;
            if (l < kSilentDb && r < kSilentDb) {
                if (w.silent_since_qpc == 0) w.silent_since_qpc = now_qpc;
            } else {
                w.silent_since_qpc = 0;
            }
            audio = {{"peakDb", {l, r}}, {"gaps", pick(audio_in, "gaps")}, {"silentSec", seconds_since(w.silent_since_qpc, now_qpc)}};
        } else {
            w.silent_since_qpc = 0;
        }

        json record_out = nullptr;
        if (record.is_object()) {
            record_out = {{"state", str(record, "state")}, {"file", str(record, "file")}, {"seconds", pick(record, "seconds")}};
        }

        channels.push_back({{"ch", id},
                            {"name", cfg.name},
                            {"enabled", cfg.enabled},
                            {"inUse", id <= config.general.channel_count},
                            {"testSlot", cfg.test_slot},
                            {"status", status},
                            {"source", source},
                            {"video", video_out},
                            {"latency", latency},
                            {"tcp", tcp_out},
                            {"audio", audio},
                            {"record", record_out},
                            {"rejectedSenders", pick(video, "rejectedSenders")},
                            {"lastError", last_error}});
    }
    return {{"event", "channels"}, {"time", utc_now_iso8601()}, {"channels", channels}};
}

}  // namespace denpachan::engine
