#include "asio_output.h"

#include <objbase.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>

// ASIO SDK (GPL-3.0、cmake/FetchAsioSdk.cmake)。インターフェースの定義だけを使う
#include "asiosys.h"
#include "asio.h"
#include "iasiodrv.h"

#include "audio_devices.h"

namespace denpachan::engine {

namespace {

constexpr double kPreferredRate = 48000.0;  // 48 kHz ならリサンプルせずにそのまま出せる (ChannelSource)
constexpr int64_t kReanchorMs = 200;        // 予測した時刻と呼ばれた時刻がこれ以上ずれたら測り直す
constexpr int64_t kFollowDivisor = 32;      // 呼ばれた時刻へ寄せる割合 (コールバックごとに 1/32)

// コールバックには「どの出力か」を渡す引数が無いので、開いている出力を 1 つだけ覚えておく
std::atomic<AsioOutput*> g_active{nullptr};

std::mutex g_probe_mutex;
std::map<std::wstring, AsioDriverProbe> g_probe_cache;  // CLSID ごと

void cb_buffer_switch(long index, ASIOBool) {
    if (auto* out = g_active.load()) out->on_buffer_switch(index);
}

ASIOTime* cb_buffer_switch_time_info(ASIOTime* params, long index, ASIOBool) {
    if (auto* out = g_active.load()) out->on_buffer_switch(index);
    return params;
}

void cb_sample_rate_changed(ASIOSampleRate) {
    if (auto* out = g_active.load()) out->request_reset("sample rate changed");
}

long cb_asio_message(long selector, long value, void*, double*) {
    switch (selector) {
    case kAsioSelectorSupported:
        return value == kAsioResetRequest || value == kAsioEngineVersion || value == kAsioResyncRequest ||
               value == kAsioLatenciesChanged || value == kAsioSupportsTimeInfo;
    case kAsioEngineVersion:
        return 2;
    case kAsioResetRequest:
        if (auto* out = g_active.load()) out->request_reset("driver requested a reset");
        return 1;
    case kAsioLatenciesChanged:
        if (auto* out = g_active.load()) out->request_reset("latencies changed");
        return 1;
    case kAsioResyncRequest:
        return 1;  // 位置合わせは ChannelSource が時刻で行う
    case kAsioSupportsTimeInfo:
        return 0;  // 時刻の情報は使わない (bufferSwitch を呼んでもらう)
    default:
        return 0;
    }
}

ASIOCallbacks g_callbacks{&cb_buffer_switch, &cb_sample_rate_changed, &cb_asio_message, &cb_buffer_switch_time_info};

bool parse_clsid(const std::wstring& s, CLSID& out) { return SUCCEEDED(CLSIDFromString(s.c_str(), &out)); }

// ドライバを読み込んで初期化する (呼ぶ側のスレッドで COM を初期化しておく)
IASIO* load_driver(const std::wstring& clsid_str, std::string& error) {
    CLSID clsid{};
    if (!parse_clsid(clsid_str, clsid)) {
        error = "bad CLSID";
        return nullptr;
    }
    IASIO* asio = nullptr;
    // ASIO のドライバは CLSID をそのまま IID として渡す決まり
    const HRESULT hr = CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, clsid, reinterpret_cast<void**>(&asio));
    if (FAILED(hr) || !asio) {
        error = fmt::format("cannot load the driver (0x{:08x})", uint32_t(hr));
        return nullptr;
    }
    if (!asio->init(GetDesktopWindow())) {
        char message[128] = {};
        asio->getErrorMessage(message);
        error = message[0] ? message : "init failed";
        asio->Release();
        return nullptr;
    }
    return asio;
}

const char* sample_type_name(long type) {
    switch (type) {
    case ASIOSTInt16LSB: return "int16";
    case ASIOSTInt24LSB: return "int24";
    case ASIOSTInt32LSB: return "int32";
    case ASIOSTFloat32LSB: return "float32";
    case ASIOSTFloat64LSB: return "float64";
    case ASIOSTInt32LSB16: return "int32lsb16";
    case ASIOSTInt32LSB18: return "int32lsb18";
    case ASIOSTInt32LSB20: return "int32lsb20";
    case ASIOSTInt32LSB24: return "int32lsb24";
    default: return nullptr;  // 対応しない (MSB、DSD)
    }
}

}  // namespace

std::vector<AsioDriverInfo> list_asio_drivers() {
    std::vector<AsioDriverInfo> result;
    HKEY root = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ASIO", 0, KEY_READ, &root) != ERROR_SUCCESS) return result;
    for (DWORD i = 0;; ++i) {
        wchar_t name[256];
        DWORD name_len = DWORD(std::size(name));
        if (RegEnumKeyExW(root, i, name, &name_len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        wchar_t clsid[64] = {};
        DWORD size = sizeof(clsid);
        if (RegGetValueW(root, name, L"CLSID", RRF_RT_REG_SZ, nullptr, clsid, &size) != ERROR_SUCCESS) continue;
        result.push_back({name, clsid});
    }
    RegCloseKey(root);
    return result;
}

std::optional<AsioDriverProbe> probe_asio_driver(const std::wstring& clsid) {
    {
        std::lock_guard lk(g_probe_mutex);
        if (auto it = g_probe_cache.find(clsid); it != g_probe_cache.end()) return it->second;
        if (g_active.load() != nullptr) return std::nullopt;  // 別のドライバを開いている間は 2 つ目を開かない
    }
    // ドライバは STA で扱う。呼んだスレッドの COM の状態に左右されないよう、別のスレッドで開く
    std::optional<AsioDriverProbe> result;
    std::thread([&] {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        std::string error;
        if (IASIO* asio = load_driver(clsid, error)) {
            long in = 0, out = 0;
            ASIOSampleRate rate = 0;
            if (asio->getChannels(&in, &out) == ASE_OK) {
                asio->getSampleRate(&rate);
                result = AsioDriverProbe{int(out), int(rate)};
            }
            asio->Release();
        } else {
            spdlog::debug("asio: cannot probe {}: {}", to_utf8(clsid), error);
        }
        CoUninitialize();
    }).join();
    if (result) {
        std::lock_guard lk(g_probe_mutex);
        g_probe_cache[clsid] = *result;
    }
    return result;
}

AsioOutput::AsioOutput(DeviceOptions options, std::vector<RouteOptions> routes)
    : options_(std::move(options)),
      routes_(std::move(routes)),
      stop_event_(CreateEventW(nullptr, TRUE, FALSE, nullptr)),
      reset_event_(CreateEventW(nullptr, FALSE, FALSE, nullptr)) {}

AsioOutput::~AsioOutput() { stop(); }

void AsioOutput::start() {
    stop_ = false;
    ResetEvent(stop_event_.get());
    thread_ = std::thread(&AsioOutput::run, this);
}

void AsioOutput::stop() {
    stop_ = true;
    SetEvent(stop_event_.get());
    if (thread_.joinable()) thread_.join();
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

void AsioOutput::request_reset(const char* reason) {
    spdlog::info("audio [{}]: {}; reopening", to_utf8(options_.name), reason);
    SetEvent(reset_event_.get());
}

bool AsioOutput::open() {
    const std::string name = to_utf8(options_.name);
    // 開けない理由は最初と 1 分ごとに出す (1 秒ごとに試すので、毎回出すとログの履歴が埋まる)
    const auto fail_level = open_failures_ == 0 || open_failures_ % 60 == 0 ? spdlog::level::err : spdlog::level::debug;
    const std::wstring clsid = options_.id.substr(options_.id.find(L':') + 1);

    {
        std::lock_guard lk(g_probe_mutex);
        AsioOutput* expected = nullptr;
        if (!g_active.compare_exchange_strong(expected, this)) {
            spdlog::log(fail_level, "audio [{}]: another ASIO driver is in use (only one ASIO driver at a time)", name);
            return false;
        }
    }

    std::string error;
    asio_ = load_driver(clsid, error);
    if (!asio_) {
        spdlog::log(fail_level, "audio [{}]: {}", name, error);
        return false;
    }

    long in = 0, out = 0;
    if (asio_->getChannels(&in, &out) != ASE_OK || out <= 0) {
        spdlog::log(fail_level, "audio [{}]: no output channels", name);
        return false;
    }
    channels_ = int(out);

    ASIOSampleRate rate = 0;
    asio_->getSampleRate(&rate);
    if (rate != kPreferredRate && asio_->canSampleRate(kPreferredRate) == ASE_OK) {
        asio_->setSampleRate(kPreferredRate);  // 変えられない (Dante が決めている) ときはそのままのレートで出す
        asio_->getSampleRate(&rate);
    }
    if (rate <= 0) {
        spdlog::log(fail_level, "audio [{}]: unknown sample rate", name);
        return false;
    }
    rate_ = uint32_t(std::lround(rate));

    long min_size = 0, max_size = 0, preferred = 0, granularity = 0;
    if (asio_->getBufferSize(&min_size, &max_size, &preferred, &granularity) != ASE_OK || preferred <= 0) {
        spdlog::log(fail_level, "audio [{}]: cannot get the buffer size", name);
        return false;
    }
    buffer_frames_ = preferred;

    sample_types_.assign(size_t(channels_), -1);
    for (int c = 0; c < channels_; ++c) {
        ASIOChannelInfo info{};
        info.channel = c;
        info.isInput = ASIOFalse;
        if (asio_->getChannelInfo(&info) == ASE_OK) sample_types_[size_t(c)] = info.type;
        if (!sample_type_name(sample_types_[size_t(c)])) {
            spdlog::log(fail_level, "audio [{}]: unsupported sample type {} (ch {})", name, sample_types_[size_t(c)], c + 1);
            return false;
        }
    }

    std::vector<ASIOBufferInfo> infos(static_cast<size_t>(channels_));
    for (int c = 0; c < channels_; ++c) {
        infos[size_t(c)].isInput = ASIOFalse;
        infos[size_t(c)].channelNum = c;
    }
    if (asio_->createBuffers(infos.data(), channels_, buffer_frames_, &g_callbacks) != ASE_OK) {
        spdlog::log(fail_level, "audio [{}]: cannot create buffers ({} ch, {} frames)", name, channels_, buffer_frames_);
        return false;
    }
    buffers_created_ = true;
    for (int i = 0; i < 2; ++i) {
        buffers_[i].resize(size_t(channels_));
        for (int c = 0; c < channels_; ++c) buffers_[i][size_t(c)] = infos[size_t(c)].buffers[i];
    }

    long input_latency = 0;
    output_latency_ = buffer_frames_;
    asio_->getLatencies(&input_latency, &output_latency_);
    if (output_latency_ <= 0) output_latency_ = buffer_frames_;

    // 音声の基本遅延: bufferSwitch で読んだサンプルが鳴り終わるまで (出力の遅延 + バッファ 1 つ分) + 余裕
    const double buffer_ms = 1000.0 * buffer_frames_ / rate_;
    const double pipeline_ms = 1000.0 * double(output_latency_ + buffer_frames_) / rate_;
    base_delay_qpc_ = qpc_from_us(int64_t(pipeline_ms * 1000)) + qpc_from_ms(options_.margin_ms);

    mix_.assign(size_t(channels_) * size_t(buffer_frames_), 0.0f);
    next_first_hear_ = 0;
    {
        std::lock_guard lk(sources_mutex_);
        sources_.clear();
        for (const auto& r : routes_) {
            if (r.start_channel < 1 || r.start_channel + 1 > channels_) {
                spdlog::warn("audio [{}]: ch{} start channel {} is out of range (device has {} ch)", name, r.ch,
                             r.start_channel, channels_);
            }
            sources_.push_back(std::make_unique<ChannelSource>(r, rate_, channels_));
        }
    }
    // 無音で埋めてから開始する
    for (int i = 0; i < 2; ++i) {
        for (int c = 0; c < channels_; ++c) write_channel(c, buffers_[i][size_t(c)], int(buffer_frames_));
    }
    output_ready_ = asio_->outputReady() == ASE_OK;

    if (asio_->start() != ASE_OK) {
        spdlog::log(fail_level, "audio [{}]: cannot start", name);
        return false;
    }
    {
        std::lock_guard lk(g_probe_mutex);
        g_probe_cache[clsid] = AsioDriverProbe{channels_, int(rate_)};
    }
    {
        std::lock_guard lk(status_mutex_);
        status_.open = true;
        status_.exclusive = false;
        status_.sample_rate = int(rate_);
        status_.channels = channels_;
        status_.sample_format = sample_type_name(sample_types_[0]);
        status_.buffer_ms = buffer_ms;
        status_.delay_ms = double(qpc_to_us(base_delay_qpc_)) / 1000.0;
    }
    spdlog::info("audio [{}]: opened ASIO {} Hz {} ch {} buffer {} frames ({:.2f} ms), output latency {} frames", name,
                 rate_, channels_, sample_type_name(sample_types_[0]), buffer_frames_, buffer_ms, output_latency_);
    return true;
}

void AsioOutput::close() {
    if (asio_) {
        asio_->stop();
        if (buffers_created_) asio_->disposeBuffers();
        buffers_created_ = false;
        asio_->Release();
        asio_ = nullptr;
    }
    {
        std::lock_guard lk(g_probe_mutex);
        AsioOutput* self = this;
        g_active.compare_exchange_strong(self, nullptr);
    }
    std::lock_guard lk(status_mutex_);
    status_.open = false;
}

void AsioOutput::write_channel(int channel, void* dst, int frames) const {
    const float* src = mix_.data() + channel;
    const size_t step = size_t(channels_);
    auto sample = [&](int i) { return std::clamp(src[size_t(i) * step], -1.0f, 1.0f); };
    switch (sample_types_[size_t(channel)]) {
    case ASIOSTFloat32LSB: {
        auto* d = static_cast<float*>(dst);
        for (int i = 0; i < frames; ++i) d[i] = sample(i);
        break;
    }
    case ASIOSTFloat64LSB: {
        auto* d = static_cast<double*>(dst);
        for (int i = 0; i < frames; ++i) d[i] = sample(i);
        break;
    }
    case ASIOSTInt16LSB: {
        auto* d = static_cast<int16_t*>(dst);
        for (int i = 0; i < frames; ++i) d[i] = int16_t(std::lrint(sample(i) * 32767.0f));
        break;
    }
    case ASIOSTInt24LSB: {
        auto* d = static_cast<uint8_t*>(dst);
        for (int i = 0; i < frames; ++i) {
            const int32_t v = int32_t(std::lrint(sample(i) * 8388607.0f));
            d[3 * i] = uint8_t(v);
            d[3 * i + 1] = uint8_t(v >> 8);
            d[3 * i + 2] = uint8_t(v >> 16);
        }
        break;
    }
    case ASIOSTInt32LSB: {
        auto* d = static_cast<int32_t*>(dst);
        for (int i = 0; i < frames; ++i) d[i] = int32_t(std::llrint(double(sample(i)) * 2147483647.0));
        break;
    }
    case ASIOSTInt32LSB16:
    case ASIOSTInt32LSB18:
    case ASIOSTInt32LSB20:
    case ASIOSTInt32LSB24: {
        // 32 ビットの下位に 16 / 18 / 20 / 24 ビットの値を置く
        const long type = sample_types_[size_t(channel)];
        const int bits = type == ASIOSTInt32LSB16 ? 16 : type == ASIOSTInt32LSB18 ? 18 : type == ASIOSTInt32LSB20 ? 20 : 24;
        const double scale = double((1 << (bits - 1)) - 1);
        auto* d = static_cast<int32_t*>(dst);
        for (int i = 0; i < frames; ++i) d[i] = int32_t(std::lrint(double(sample(i)) * scale));
        break;
    }
    default:
        std::memset(dst, 0, size_t(frames) * 4);
        break;
    }
}

void AsioOutput::on_buffer_switch(long index) {
    if (index < 0 || index > 1 || !buffers_created_) return;
    const int frames = int(buffer_frames_);
    // いま書くバッファの先頭が鳴る時刻 = 呼ばれた時刻 + 出力の遅延。
    // ドライバによってはコールバックの間隔が揺れる (固まって来る) ので、呼ばれた時刻をそのまま使わず、
    // 「前のバッファの時刻 + バッファ 1 つ分」で進めて、呼ばれた時刻へは少しずつ寄せる
    // (デバイスと PC のクロックのずれには追いつき、呼び出しの揺れは均す)。大きく外れたら測り直す。
    const int64_t buffer_qpc = qpc_from_us(int64_t(buffer_frames_) * 1'000'000 / rate_);
    const int64_t measured = qpc_now() + qpc_from_us(int64_t(output_latency_) * 1'000'000 / rate_);
    if (next_first_hear_ == 0 || std::llabs(measured - next_first_hear_) > qpc_from_ms(kReanchorMs)) {
        next_first_hear_ = measured;
    } else {
        next_first_hear_ += (measured - next_first_hear_) / kFollowDivisor;
    }
    const int64_t first_hear = next_first_hear_;
    next_first_hear_ += buffer_qpc;

    // 診断: デバイスが実時間どおりに音を消費しているか (コールバックの回数を 5 秒ごとに数え、2% 以上ずれていればログ)
    ++callbacks_;
    const int64_t now = qpc_now();
    if (callbacks_since_qpc_ == 0) callbacks_since_qpc_ = now;
    if (now - callbacks_since_qpc_ >= qpc_from_ms(5000)) {
        const double per_sec = double(callbacks_) * double(qpc_freq()) / double(now - callbacks_since_qpc_);
        const double expected = double(rate_) / double(buffer_frames_);
        if (std::abs(per_sec / expected - 1.0) > 0.02) {
            spdlog::warn("audio [{}]: the device calls back {:.1f} times/s (expected {:.1f}); it does not play in real time",
                         to_utf8(options_.name), per_sec, expected);
        }
        callbacks_ = 0;
        callbacks_since_qpc_ = now;
    }
    std::fill(mix_.begin(), mix_.end(), 0.0f);
    {
        std::lock_guard lk(sources_mutex_);
        for (auto& s : sources_) s->render(mix_.data(), frames, first_hear, base_delay_qpc_);
    }
    for (int c = 0; c < channels_; ++c) write_channel(c, buffers_[index][size_t(c)], frames);
    if (output_ready_) asio_->outputReady();
}

void AsioOutput::run() {
    // ASIO のドライバは STA で扱い、このスレッドでメッセージを回す (ウィンドウを使うドライバがある)
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    while (!stop_) {
        if (!open()) {
            close();
            ++open_failures_;
            WaitForSingleObject(stop_event_.get(), 1000);  // ドライバが使えるようになるまで 1 秒ごとに試す
            continue;
        }
        open_failures_ = 0;
        ResetEvent(reset_event_.get());
        while (!stop_) {
            HANDLE handles[2] = {stop_event_.get(), reset_event_.get()};
            const DWORD w = MsgWaitForMultipleObjects(2, handles, FALSE, 500, QS_ALLINPUT);
            if (w == WAIT_OBJECT_0 || w == WAIT_OBJECT_0 + 1) break;
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
        close();
        if (!stop_) {
            std::lock_guard lk(status_mutex_);
            ++status_.reopens;
        }
    }
    CoUninitialize();
}

}  // namespace denpachan::engine
