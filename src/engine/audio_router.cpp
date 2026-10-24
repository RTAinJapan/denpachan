#include "audio_router.h"

#include <spdlog/spdlog.h>

#include <cmath>

#include "asio_output.h"
#include "audio_devices.h"
#include "device_output.h"
#include "output_devices.h"

using nlohmann::json;

namespace denpachan::engine {

namespace {

// ミュート以外が同じか (ミュートは開き直さずに切り替えられる)
bool same_routes(const std::vector<RouteOptions>& a, const std::vector<RouteOptions>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].ch != b[i].ch || a[i].start_channel != b[i].start_channel || a[i].offset_ms != b[i].offset_ms || a[i].fade != b[i].fade) {
            return false;
        }
    }
    return true;
}

bool same_options(const DeviceOptions& a, const DeviceOptions& b) {
    return a.id == b.id && a.asio == b.asio && a.exclusive == b.exclusive && a.margin_ms == b.margin_ms;
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
    asio_busy_.clear();
    std::wstring asio_id;  // 使う ASIO ドライバ (同時に 1 つだけ。最初のチャンネルのものを使う)
    std::wstring asio_name;
    for (const auto& ch : config.channels) {
        if (!ch.enabled || ch.audio.device.empty()) continue;
        const auto dev = find_output_device(from_utf8(ch.audio.device));
        if (!dev) {
            missing_[ch.id] = ch.audio.device;
            continue;
        }
        if (dev->asio) {
            if (asio_id.empty()) {
                asio_id = dev->id;
                asio_name = dev->name;
            }
            if (dev->id != asio_id) {
                asio_busy_[ch.id] = ch.audio.device;
                continue;
            }
        }
        auto& plan = plans[dev->id];
        if (plan.options.id.empty()) {
            plan.options.id = dev->id;
            plan.options.name = dev->name;
            plan.options.asio = dev->asio;
            plan.options.margin_ms = config.general.audio_margin_ms;
            plan.options.exclusive = !dev->asio && ch.audio.mode == "exclusive";
        }
        RouteOptions r;
        r.ch = ch.id;
        r.start_channel = ch.audio.start_channel;
        r.offset_ms = ch.audio.offset_ms;
        r.fade = ch.audio.fade;
        r.mute = ch.audio.mute;
        plan.routes.push_back(r);
    }

    // 不要になった / 変わったデバイスを止める (ASIO を別のドライバに替えるときは、先に止めてから開く)
    for (auto it = devices_.begin(); it != devices_.end();) {
        auto p = plans.find(it->first);
        if (p == plans.end() || !same_options(p->second.options, it->second.options) || !same_routes(p->second.routes, it->second.routes)) {
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
            if (plan.options.asio) {
                plan.output = std::make_unique<AsioOutput>(plan.options, plan.routes);
            } else {
                plan.output = std::make_unique<DeviceOutput>(plan.options, plan.routes);
            }
            plan.output->start();
            devices_.emplace(id, std::move(plan));
        } else {
            for (const auto& r : plan.routes) it->second.output->set_mute(r.ch, r.mute);
            it->second.routes = plan.routes;
        }
    }
    if (missing_ != logged_missing_) {  // 5 秒ごとの apply で毎回出さない
        for (const auto& [ch, name] : missing_) spdlog::warn("ch{}: audio device not found: {}", ch, name);
        logged_missing_ = missing_;
    }
    if (asio_busy_ != logged_asio_busy_) {  // 同じく、変わったときだけ出す
        for (const auto& [ch, name] : asio_busy_) {
            spdlog::warn("ch{}: {} is not used: only one ASIO driver at a time ({} is in use)", ch, name, to_utf8(asio_name));
        }
        logged_asio_busy_ = asio_busy_;
    }
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
        return {{"device", it->second}, {"open", false}, {"error", {{"id", "audio.deviceNotFound"}}}};
    }
    if (auto it = asio_busy_.find(ch); it != asio_busy_.end()) {
        return {{"device", it->second}, {"open", false}, {"error", {{"id", "audio.asioOneDriver"}}}};
    }
    for (auto& [id, d] : devices_) {
        for (const auto& r : d.routes) {
            if (r.ch != ch) continue;
            const DeviceStatus st = d.output->status();
            json j = {{"device", to_utf8(d.options.name)},
                      {"deviceId", to_utf8(id)},
                      {"open", st.open},
                      {"api", d.options.asio ? "asio" : "wasapi"},
                      {"exclusive", st.exclusive},
                      {"startChannel", r.start_channel},
                      {"mute", r.mute}};
            // 排他モードを頼まれたが共有モードで開いている (開けない・周期に追いつけない)
            if (st.open && !st.exclusive_fallback.empty()) j["exclusiveFallback"] = st.exclusive_fallback;
            // for_each_source は描画スレッドと同じロック (sources_mutex_) を持って呼ぶので、その中では値を写すだけにして、
            // JSON はロックの外で作る (8ch 分を 1 秒ごとに作る間、描画を待たせない)
            struct Snapshot {
                bool found = false;
                bool playing = false;
                double latency_ms = 0, error_ms = 0;
                uint64_t slips = 0, underruns = 0, resyncs = 0;
                std::string last_resync;
                float peak_l = 0, peak_r = 0;
            } snap;
            d.output->for_each_source([&](ChannelSource& s) {
                if (s.options().ch != ch) return;
                auto& stats = s.stats();
                snap.found = true;
                snap.playing = stats.playing.load();
                snap.latency_ms = stats.latency_ms.load();
                snap.error_ms = stats.error_ms.load();
                snap.slips = stats.slips.load();
                snap.underruns = stats.underruns.load();
                snap.resyncs = stats.resyncs.load();
                {
                    std::lock_guard lk(stats.reason_mutex);
                    snap.last_resync = stats.last_resync_reason;
                }
                snap.peak_l = stats.peak_l.exchange(0);
                snap.peak_r = stats.peak_r.exchange(0);
            });
            if (!st.open && st.ever_opened) {
                j["error"] = {{"id", "audio.deviceLost"}};  // 開けていたのに失った (DVS の停止、モードの切り替えなど)。開き直している
            } else if (st.stalled) {
                j["error"] = {{"id", "audio.driverStalled"}};  // ASIO のドライバから呼ばれていない
            } else if (st.open && (r.start_channel < 1 || r.start_channel > st.channels)) {
                // 出力チャンネルがデバイスに無い (2ch ずつのデバイスで 3-4 を選んだままなど)。何も出ていない
                j["error"] = {{"id", "audio.startChannelOutOfRange"},
                              {"channels", std::to_string(r.start_channel) + "-" + std::to_string(r.start_channel + 1)},
                              {"deviceChannels", st.channels}};
            }
            if (snap.found) {
                j["playing"] = snap.playing && st.open && !st.stalled;
                j["latencyMs"] = snap.latency_ms;
                j["errorMs"] = snap.error_ms;
                j["slips"] = snap.slips;
                j["underruns"] = snap.underruns;
                j["resyncs"] = snap.resyncs;
                j["lastResync"] = snap.last_resync;  // 直近の合わせ直しの理由 (調べもの用)
                j["peakDb"] = {dbfs(snap.peak_l), dbfs(snap.peak_r)};
            }
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
                       {"api", d.options.asio ? "asio" : "wasapi"},
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
