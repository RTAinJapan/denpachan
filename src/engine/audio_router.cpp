#include "audio_router.h"

#include <spdlog/spdlog.h>

#include <cmath>

#include "audio_devices.h"

using nlohmann::json;

namespace denpachan::engine {

namespace {

// ミュート以外が同じか (ミュートは開き直さずに切り替えられる)
bool same_routes(const std::vector<RouteOptions>& a, const std::vector<RouteOptions>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].ch != b[i].ch || a[i].start_channel != b[i].start_channel || a[i].offset_ms != b[i].offset_ms ||
            a[i].fade != b[i].fade) {
            return false;
        }
    }
    return true;
}

bool same_options(const DeviceOptions& a, const DeviceOptions& b) {
    return a.id == b.id && a.exclusive == b.exclusive && a.margin_ms == b.margin_ms;
}

double dbfs(float v) { return v > 1e-5f ? 20.0 * std::log10(v) : -100.0; }

}  // namespace

AudioRouter::~AudioRouter() {
    for (auto& [id, d] : devices_) d.output->stop();
}

void AudioRouter::apply(const Config& config) {
    // 有効なチャンネルのルーティングをデバイスごとにまとめる
    std::map<std::wstring, Device> plans;
    missing_.clear();
    for (const auto& ch : config.channels) {
        if (!ch.enabled || ch.audio.device.empty()) continue;
        const auto dev = find_audio_device(eRender, from_utf8(ch.audio.device));
        if (!dev) {
            missing_[ch.id] = ch.audio.device;
            continue;
        }
        auto& plan = plans[dev->id];
        if (plan.options.id.empty()) {
            plan.options.id = dev->id;
            plan.options.name = dev->name;
            plan.options.margin_ms = config.general.audio_margin_ms;
            plan.options.exclusive = ch.audio.mode == "exclusive";
        }
        RouteOptions r;
        r.ch = ch.id;
        r.start_channel = ch.audio.start_channel;
        r.offset_ms = ch.audio.offset_ms;
        r.fade = ch.audio.fade;
        r.mute = ch.audio.mute;
        plan.routes.push_back(r);
    }

    // 不要になった / 変わったデバイスを止める
    for (auto it = devices_.begin(); it != devices_.end();) {
        auto p = plans.find(it->first);
        if (p == plans.end() || !same_options(p->second.options, it->second.options) ||
            !same_routes(p->second.routes, it->second.routes)) {
            spdlog::info("audio [{}]: stopping (routing changed)", to_utf8(it->second.options.name));
            it->second.output->stop();
            it = devices_.erase(it);
        } else {
            ++it;
        }
    }
    // 新しい / 変わったデバイスを開く。変わっていないデバイスはミュートだけ反映する。
    for (auto& [id, plan] : plans) {
        auto it = devices_.find(id);
        if (it == devices_.end()) {
            plan.output = std::make_unique<DeviceOutput>(plan.options, plan.routes);
            plan.output->start();
            devices_.emplace(id, std::move(plan));
        } else {
            for (const auto& r : plan.routes) it->second.output->set_mute(r.ch, r.mute);
            it->second.routes = plan.routes;
        }
    }
    for (const auto& [ch, name] : missing_) spdlog::warn("ch{}: audio device not found: {}", ch, name);
}

void AudioRouter::set_mute(int ch, bool mute) {
    for (auto& [id, d] : devices_) {
        d.output->set_mute(ch, mute);
        for (auto& r : d.routes)
            if (r.ch == ch) r.mute = mute;
    }
}

json AudioRouter::channel_status(int ch) {
    if (auto it = missing_.find(ch); it != missing_.end()) {
        return {{"device", it->second}, {"open", false}, {"error", "device not found"}};
    }
    for (auto& [id, d] : devices_) {
        for (const auto& r : d.routes) {
            if (r.ch != ch) continue;
            const DeviceStatus st = d.output->status();
            json j = {{"device", to_utf8(d.options.name)},
                      {"deviceId", to_utf8(id)},
                      {"open", st.open},
                      {"exclusive", st.exclusive},
                      {"startChannel", r.start_channel},
                      {"mute", r.mute}};
            d.output->for_each_source([&](ChannelSource& s) {
                if (s.options().ch != ch) return;
                auto& stats = s.stats();
                j["playing"] = stats.playing.load();
                j["latencyMs"] = stats.latency_ms.load();
                j["errorMs"] = stats.error_ms.load();
                j["slips"] = stats.slips.load();
                j["underruns"] = stats.underruns.load();
                j["resyncs"] = stats.resyncs.load();
                j["peakDb"] = {dbfs(stats.peak_l.exchange(0)), dbfs(stats.peak_r.exchange(0))};
            });
            return j;
        }
    }
    return nullptr;
}

json AudioRouter::devices_status() {
    json arr = json::array();
    for (auto& [id, d] : devices_) {
        const DeviceStatus st = d.output->status();
        arr.push_back({{"name", to_utf8(d.options.name)},
                       {"id", to_utf8(id)},
                       {"open", st.open},
                       {"exclusive", st.exclusive},
                       {"sampleRate", st.sample_rate},
                       {"channels", st.channels},
                       {"format", st.sample_format},
                       {"bufferMs", st.buffer_ms},
                       {"delayMs", st.delay_ms},
                       {"reopens", st.reopens}});
    }
    return arr;
}

}  // namespace denpachan::engine
