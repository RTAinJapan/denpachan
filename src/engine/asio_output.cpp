#include "asio_output.h"

#include <spdlog/spdlog.h>

#include "audio_devices.h"  // to_utf8
#include "output_devices.h"
#include "denpachan/win_util.h"
#include "render_log.h"

namespace denpachan::engine {

namespace {

constexpr int kPreferredRate = 48000;  // 48 kHz ならリサンプルせずにそのまま出せる (ChannelSource)
constexpr int64_t kStalledMs = 1000;   // ドライバからこれだけ呼ばれなければ、止まっているとして状態に出す

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
    output_ = open_asio_output(clsid, kPreferredRate, &callbacks_);
}

void AsioOutput::stop() {
    if (!output_) return;
    stopping_ = true;
    close_asio_output(output_);  // 戻るまでにコールバックは止まる
    output_ = nullptr;
    // 出力先が無くなった (ルーティングが変わった、音声を「なし」にした) ことを worker に知らせる
    std::lock_guard lk(sources_mutex_);
    for (auto& s : sources_) s->clear_output_delay();
}

void AsioOutput::set_mute(int ch, bool mute) {
    std::lock_guard lk(sources_mutex_);
    for (auto& r : routes_)
        if (r.ch == ch) r.mute = mute;
    for (auto& s : sources_)
        if (s->options().ch == ch) s->set_mute(mute);
}

void AsioOutput::update_routes(std::vector<RouteOptions> routes) {
    std::lock_guard update(update_mutex_);
    for (;;) {
        // 今の形式を写し、変わったチャンネルの ChannelSource をロックの外で作る (共有メモリを開くので、コールバックを待たせない)
        uint32_t rate = 0;
        int channels = 0;
        uint64_t opened = 0;
        int64_t base_delay = 0;
        std::vector<RouteOptions> current;
        {
            std::lock_guard lk(sources_mutex_);
            if (opened_ == 0) {
                routes_ = std::move(routes);  // まだ開いていない: 開いたときにこの振り分けで作る
                return;
            }
            rate = source_rate_, channels = source_channels_, opened = opened_, base_delay = base_delay_qpc_;
            for (const auto& s : sources_) current.push_back(s->options());
        }
        std::vector<std::unique_ptr<ChannelSource>> fresh(routes.size());
        for (size_t i = 0; i < routes.size(); ++i) {
            if (!needs_source(current, routes[i])) continue;
            if (routes[i].start_channel < 1 || routes[i].start_channel + 1 > channels) {
                spdlog::warn("audio [{}]: ch{} start channel {} is out of range (device has {} ch)", to_utf8(options_.name), routes[i].ch,
                             routes[i].start_channel, channels);
            }
            fresh[i] = std::make_unique<ChannelSource>(routes[i], rate, channels, base_delay);
        }
        std::vector<std::unique_ptr<ChannelSource>> old;
        {
            std::lock_guard lk(sources_mutex_);
            if (opened_ != opened) continue;  // その間にドライバが開き直した: 作り直す
            old = swap_sources(sources_, routes, fresh);
            routes_ = routes;
        }
        spdlog::info("audio [{}]: routing updated without reopening", to_utf8(options_.name));
        return;  // old はロックの外で捨てる
    }
}

DeviceStatus AsioOutput::status() {
    DeviceStatus st;
    {
        std::lock_guard lk(status_mutex_);
        st = status_;
        st.stalled = st.open && qpc_now() - last_render_qpc_.load() > qpc_from_ms(kStalledMs);
    }
    if (st.stalled) {
        // ドライバから呼ばれていない: 鳴っていないので、worker は音声で時間軸を動かさない (呼ばれたら描画が 1 に戻す)
        std::lock_guard lk(sources_mutex_);
        for (auto& s : sources_) s->set_output_active(false);
    }
    return st;
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
            sources_.push_back(std::make_unique<ChannelSource>(r, uint32_t(format.sample_rate), format.channels, base_delay_qpc_));
        }
        source_rate_ = uint32_t(format.sample_rate);
        source_channels_ = format.channels;
        ++opened_;
    }
    note_asio_opened(options_.id, format.channels, format.sample_rate);  // デバイス一覧のチャンネル数 (開いている間は確かめられない)
    last_render_qpc_ = qpc_now();
    std::lock_guard lk(status_mutex_);
    status_.open = true;
    status_.ever_opened = true;
    status_.exclusive = false;
    status_.sample_rate = format.sample_rate;
    status_.channels = format.channels;
    status_.sample_format = format.sample_format ? format.sample_format : "";
    status_.buffer_ms = buffer_ms;
    status_.delay_ms = double(qpc_to_us(base_delay_qpc_)) / 1000.0;
}

void AsioOutput::on_closed() {
    {
        std::lock_guard lk(sources_mutex_);
        for (auto& s : sources_) {
            s->stats().playing = false;   // 開き直すまで鳴っていない
            s->set_output_active(false);  // worker も、開き直すまで音声で時間軸を動かさない
        }
    }
    std::lock_guard lk(status_mutex_);
    status_.open = false;
    if (!stopping_) ++status_.reopens;
}

void AsioOutput::on_render(float* mix, int frames, int64_t first_hear_qpc) {
    last_render_qpc_.store(qpc_now(), std::memory_order_relaxed);
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
