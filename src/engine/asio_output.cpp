#include "asio_output.h"

#include <spdlog/spdlog.h>

#include "audio_devices.h"  // to_utf8
#include "denpachan/win_util.h"
#include "render_log.h"

namespace denpachan::engine {

namespace {

constexpr int kPreferredRate = 48000;  // 48 kHz ならリサンプルせずにそのまま出せる (ChannelSource)

}  // namespace

AsioOutput::AsioOutput(DeviceOptions options, std::vector<RouteOptions> routes)
    : options_(std::move(options)), routes_(std::move(routes)) {}

AsioOutput::~AsioOutput() { stop(); }

void AsioOutput::start() {
    if (output_) return;
    const auto& plugin = asio_plugin();
    if (!plugin) {
        spdlog::error("audio [{}]: ASIO output is not available (denpachan-asio-driver.dll)", to_utf8(options_.name));
        return;
    }
    stopping_ = false;
    const std::wstring clsid = options_.id.substr(options_.id.find(L':') + 1);
    // コールバックの表はメンバに置く (DLL が値で写していても、ポインタを持ち続ける作りに変わっても壊れないように)
    callbacks_ = {};
    callbacks_.user = this;
    callbacks_.opened = [](void* user, const denpachan_asio_format* format) { static_cast<AsioOutput*>(user)->on_opened(*format); };
    callbacks_.closed = [](void* user) { static_cast<AsioOutput*>(user)->on_closed(); };
    callbacks_.render = [](void* user, float* mix, int frames, int64_t first_hear_qpc) {
        static_cast<AsioOutput*>(user)->on_render(mix, frames, first_hear_qpc);
    };
    callbacks_.log = [](void* user, int level, const char* message) { static_cast<AsioOutput*>(user)->on_log(level, message); };
    output_ = plugin.open(clsid.c_str(), kPreferredRate, &callbacks_);
}

void AsioOutput::stop() {
    if (!output_) return;
    stopping_ = true;
    asio_plugin().close(output_);  // 戻るまでにコールバックは止まる
    output_ = nullptr;
}

void AsioOutput::set_mute(int ch, bool mute) {
    std::lock_guard lk(sources_mutex_);
    for (auto& r : routes_)
        if (r.ch == ch) r.mute = mute;
    for (auto& s : sources_)
        if (s->options().ch == ch) s->set_mute(mute);
}

DeviceStatus AsioOutput::status() {
    std::lock_guard lk(status_mutex_);
    return status_;
}

void AsioOutput::on_opened(const denpachan_asio_format& format) {
    const std::string name = to_utf8(options_.name);
    // 音声の基本遅延: コールバックで渡したサンプルが鳴り終わるまで (出力の遅延 + バッファ 1 つ分) + 余裕
    const double buffer_ms = 1000.0 * format.buffer_frames / format.sample_rate;
    const double pipeline_ms = 1000.0 * double(format.output_latency_frames + format.buffer_frames) / format.sample_rate;
    base_delay_qpc_ = qpc_from_us(int64_t(pipeline_ms * 1000)) + qpc_from_ms(options_.margin_ms);
    {
        std::lock_guard lk(sources_mutex_);
        sources_.clear();
        for (const auto& r : routes_) {
            if (r.start_channel < 1 || r.start_channel + 1 > format.channels) {
                spdlog::warn("audio [{}]: ch{} start channel {} is out of range (device has {} ch)", name, r.ch, r.start_channel,
                             format.channels);
            }
            sources_.push_back(std::make_unique<ChannelSource>(r, uint32_t(format.sample_rate), format.channels));
        }
    }
    std::lock_guard lk(status_mutex_);
    status_.open = true;
    status_.exclusive = false;
    status_.sample_rate = format.sample_rate;
    status_.channels = format.channels;
    status_.sample_format = format.sample_format ? format.sample_format : "";
    status_.buffer_ms = buffer_ms;
    status_.delay_ms = double(qpc_to_us(base_delay_qpc_)) / 1000.0;
}

void AsioOutput::on_closed() {
    std::lock_guard lk(status_mutex_);
    status_.open = false;
    if (!stopping_) ++status_.reopens;
}

void AsioOutput::on_render(float* mix, int frames, int64_t first_hear_qpc) {
    std::lock_guard lk(sources_mutex_);
    for (auto& s : sources_) s->render(mix, frames, first_hear_qpc, base_delay_qpc_);
}

void AsioOutput::on_log(int level, const char* message) {
    const auto lvl = level >= DENPACHAN_ASIO_LOG_ERROR  ? spdlog::level::err
                     : level == DENPACHAN_ASIO_LOG_WARN ? spdlog::level::warn
                     : level == DENPACHAN_ASIO_LOG_INFO ? spdlog::level::info
                                                        : spdlog::level::debug;
    render_log().log(lvl, "audio [{}]: {}", to_utf8(options_.name), message);  // ドライバのスレッドから呼ばれる
}

}  // namespace denpachan::engine
