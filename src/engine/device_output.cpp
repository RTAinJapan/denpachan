#include "device_output.h"

#include <avrt.h>
#include <ks.h>
#include <ksmedia.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "audio_devices.h"
#include "render_log.h"

using Microsoft::WRL::ComPtr;

namespace denpachan::engine {

namespace {

constexpr REFERENCE_TIME kMinExclusivePeriod = 30'000;     // 3ms (100ns 単位)
constexpr REFERENCE_TIME kRetryExclusivePeriod = 100'000;  // 周期に追いつけなかったら 10ms から試し直す
constexpr REFERENCE_TIME kMaxExclusivePeriod = 200'000;    // 20ms を超えるなら共有モードにする
// 再生位置 (GetPosition) と padding による時刻の差 (設計書 §6.3)。開いた直後は落ち着くまで (kCalibrateSkipMs) 使わず、
// 続く kCalibrateMs の中央値を差とする。DVS では、開いた直後の差が落ち着いた後と 10 ms 違うことがあった
constexpr int64_t kCalibrateSkipMs = 1000;
constexpr int64_t kCalibrateMs = 2000;
constexpr int64_t kPositionMaxOffMs = 30;  // 差がいつもよりこれ以上ずれたら (位置のリセットなど) 測り直す
// 測り終えた後も差の中央値を kOffsetCheckMs ごとに見て、kOffsetMovedMs 以上ずれたままなのが kOffsetMovedChecks 回続いたら、
// その値に替える (開いた直後に測った値が偏っていたとき)
constexpr int64_t kOffsetCheckMs = 1000;
constexpr int64_t kOffsetMovedMs = 3;
constexpr int kOffsetMovedChecks = 3;
constexpr int64_t kNoPositionLogMs = 5000;  // 再生位置が取れないのがこれだけ続いたらログに出す

int64_t median(std::vector<int64_t>& v) {
    std::nth_element(v.begin(), v.begin() + ptrdiff_t(v.size() / 2), v.end());
    return v[v.size() / 2];
}

struct CoTaskMemDeleter {
    void operator()(void* p) const { CoTaskMemFree(p); }
};

bool is_float_format(const WAVEFORMATEX* f) {
    if (f->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return true;
    if (f->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        return reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(f)->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    }
    return false;
}

const char* sample_type_name(int type) {
    static const char* names[] = {"float32", "int16", "int24", "int24in32", "int32"};
    return names[type];
}

std::string join(const std::vector<std::string>& items) {
    std::string out;
    for (const auto& s : items) out += (out.empty() ? "" : "; ") + s;
    return out;
}

}  // namespace

DeviceOutput::DeviceOutput(DeviceOptions options, std::vector<RouteOptions> routes)
    : options_(std::move(options)), routes_(std::move(routes)), stop_event_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}

DeviceOutput::~DeviceOutput() { stop(); }

void DeviceOutput::start() {
    stop_ = false;
    ResetEvent(stop_event_.get());
    thread_ = std::thread(&DeviceOutput::run, this);
}

void DeviceOutput::stop() {
    stop_ = true;
    SetEvent(stop_event_.get());
    if (thread_.joinable()) thread_.join();
}

void DeviceOutput::set_mute(int ch, bool mute) {
    std::lock_guard lk(sources_mutex_);
    for (auto& r : routes_)
        if (r.ch == ch) r.mute = mute;
    for (auto& s : sources_)
        if (s->options().ch == ch) s->set_mute(mute);
}

DeviceStatus DeviceOutput::status() {
    std::lock_guard lk(status_mutex_);
    return status_;
}

// 排他モードで開く。デバイスが受け付ける形式ごとに、周期 (最小 → 既定 → 10 ms → 20 ms) とチャンネルの配置を変えて試す。
// DVS 4.5 は IsFormatSupported が通るのに Initialize が E_INVALIDARG を返したので、試したものをすべてログに出す。
// どれでも開けなければ E_FAIL、試している間にデバイスが消えたら AUDCLNT_E_DEVICE_INVALIDATED
HRESULT DeviceOutput::initialize_exclusive(const WAVEFORMATEX* mix, const std::function<bool()>& activate) {
    const std::string name = to_utf8(options_.name);
    struct Candidate {
        SampleType type;
        WORD bits;
        WORD valid;
        GUID subformat;
    };
    const Candidate candidates[] = {
        {SampleType::Float32, 32, 32, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT}, {SampleType::Int32, 32, 32, KSDATAFORMAT_SUBTYPE_PCM},
        {SampleType::Int24In32, 32, 24, KSDATAFORMAT_SUBTYPE_PCM},      {SampleType::Int24, 24, 24, KSDATAFORMAT_SUBTYPE_PCM},
        {SampleType::Int16, 16, 16, KSDATAFORMAT_SUBTYPE_PCM},
    };
    REFERENCE_TIME def_period = 0, min_period = 0;
    client_->GetDevicePeriod(&def_period, &min_period);

    // 周期の候補: 前に決めた周期 (追いつけなくて延ばしたもの) 以上のものだけ
    const REFERENCE_TIME lowest = std::max({min_period, kMinExclusivePeriod, exclusive_period_});
    std::vector<REFERENCE_TIME> periods;
    for (const REFERENCE_TIME p : {lowest, def_period, kRetryExclusivePeriod, kMaxExclusivePeriod}) {
        if (p >= lowest && p <= kMaxExclusivePeriod && std::find(periods.begin(), periods.end(), p) == periods.end()) periods.push_back(p);
    }
    // チャンネルの配置: ミックス形式のもの。先頭から順に埋めた配置と違えば (0 = 指定なし も含む)、そちらも試す
    const DWORD mix_mask =
        mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE ? reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(mix)->dwChannelMask : 0;
    const DWORD plain_mask = channels_ >= 1 && channels_ <= 18 ? DWORD((1u << channels_) - 1) : 0;
    std::vector<DWORD> masks{mix_mask};
    if (plain_mask != mix_mask) masks.push_back(plain_mask);

    std::vector<std::string> tried;
    bool any_supported = false;
    for (const auto& c : candidates) {
        for (const DWORD mask : masks) {
            WAVEFORMATEXTENSIBLE fmt{};
            fmt.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
            fmt.Format.nChannels = WORD(channels_);
            fmt.Format.nSamplesPerSec = rate_;
            fmt.Format.wBitsPerSample = c.bits;
            fmt.Format.nBlockAlign = WORD(channels_ * c.bits / 8);
            fmt.Format.nAvgBytesPerSec = rate_ * fmt.Format.nBlockAlign;
            fmt.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
            fmt.Samples.wValidBitsPerSample = c.valid;
            fmt.dwChannelMask = mask;
            fmt.SubFormat = c.subformat;
            if (!activate()) return AUDCLNT_E_DEVICE_INVALIDATED;  // Initialize に失敗したクライアントは使い回せない
            if (client_->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, &fmt.Format, nullptr) != S_OK) continue;
            any_supported = true;
            for (REFERENCE_TIME period : periods) {
                if (!activate()) return AUDCLNT_E_DEVICE_INVALIDATED;
                HRESULT hr = test_fail_exclusive ? E_INVALIDARG
                                                 : client_->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                                                       period, period, &fmt.Format, nullptr);
                if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
                    UINT32 frames = 0;
                    client_->GetBufferSize(&frames);
                    period = REFERENCE_TIME(std::llround(10'000'000.0 * frames / rate_));
                    if (!activate()) return AUDCLNT_E_DEVICE_INVALIDATED;
                    hr = client_->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, period, period, &fmt.Format,
                                             nullptr);
                }
                if (SUCCEEDED(hr)) {
                    sample_type_ = c.type;
                    if (!tried.empty()) {
                        spdlog::info("audio [{}]: exclusive mode opened with {} {}/{} bit, mask 0x{:x}, period {:.1f} ms (after: {})", name,
                                     sample_type_name(int(c.type)), c.bits, c.valid, mask, double(period) / 10'000, join(tried));
                    }
                    return hr;
                }
                tried.push_back(fmt::format("{} {}/{} bit mask 0x{:x} {:.1f} ms -> 0x{:08x}", sample_type_name(int(c.type)), c.bits,
                                            c.valid, mask, double(period) / 10'000, uint32_t(hr)));
            }
        }
    }
    spdlog::warn(
        "audio [{}]: cannot open in exclusive mode ({} Hz, {} ch, mix mask 0x{:x}, device period default {:.1f} ms / minimum "
        "{:.1f} ms; {}); falling back to shared mode",
        name, rate_, channels_, mix_mask, double(def_period) / 10'000, double(min_period) / 10'000,
        any_supported ? fmt::format("tried {}", join(tried)) : std::string("no exclusive-mode format supported"));
    return E_FAIL;
}

bool DeviceOutput::open() {
    const std::string name = to_utf8(options_.name);
    // 開けない理由は最初と 1 分ごとに出す (1 秒ごとに試すので、毎回出すとログの履歴が埋まる)
    const auto fail_level = open_failures_ == 0 || open_failures_ % 60 == 0 ? spdlog::level::err : spdlog::level::debug;
    auto dev = open_audio_device(options_.id);
    if (!dev) return false;
    auto activate = [&]() -> bool {
        client_.Reset();
        return SUCCEEDED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client_));
    };
    if (!activate()) return false;

    WAVEFORMATEX* mix_raw = nullptr;
    if (FAILED(client_->GetMixFormat(&mix_raw))) return false;
    std::unique_ptr<WAVEFORMATEX, CoTaskMemDeleter> mix(mix_raw);
    channels_ = mix->nChannels;
    rate_ = mix->nSamplesPerSec;

    // 排他モードで開けない・周期に追いつけないデバイスは共有モードに切り替える (追いつけないのは run() で検出)
    exclusive_ = options_.exclusive && !exclusive_failed_;
    HRESULT hr = E_FAIL;
    if (exclusive_) {
        hr = initialize_exclusive(mix.get(), activate);
        if (hr == AUDCLNT_E_DEVICE_INVALIDATED) return false;  // 試している間にデバイスが消えた (戻ったら排他モードでまた試す)
        if (FAILED(hr)) {
            if (!activate()) return false;
            exclusive_failed_ = true;
            exclusive_fallback_ = "initFailed";
            exclusive_ = false;
        }
    }
    if (!exclusive_) {
        // 共有モード: ミックス形式のまま (通常 float32)
        if (is_float_format(mix.get()) && mix->wBitsPerSample == 32) {
            sample_type_ = SampleType::Float32;
        } else if (mix->wBitsPerSample == 16) {
            sample_type_ = SampleType::Int16;
        } else {
            spdlog::log(fail_level, "audio [{}]: unsupported mix format ({} bit)", name, mix->wBitsPerSample);
            return false;
        }
        ComPtr<IAudioClient3> client3;
        if (SUCCEEDED(client_.As(&client3))) {
            UINT32 def_frames = 0, fundamental = 0, min_frames = 0, max_frames = 0;
            if (SUCCEEDED(client3->GetSharedModeEnginePeriod(mix.get(), &def_frames, &fundamental, &min_frames, &max_frames))) {
                hr = client3->InitializeSharedAudioStream(AUDCLNT_STREAMFLAGS_EVENTCALLBACK, min_frames, mix.get(), nullptr);
            }
        }
        if (FAILED(hr)) {
            if (!activate()) return false;
            hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 0, 0, mix.get(), nullptr);
        }
    }
    if (FAILED(hr)) {
        spdlog::log(fail_level, "audio [{}]: initialize ({}) failed: 0x{:08x}", name, exclusive_ ? "exclusive" : "shared", uint32_t(hr));
        return false;
    }

    event_.reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    if (FAILED(client_->SetEventHandle(event_.get())) || FAILED(client_->GetBufferSize(&buffer_frames_)) ||
        FAILED(client_->GetService(IID_PPV_ARGS(&render_)))) {
        return false;
    }

    // 無音で埋めてから開始する
    BYTE* data = nullptr;
    written_frames_ = 0;
    if (SUCCEEDED(render_->GetBuffer(buffer_frames_, &data))) {
        render_->ReleaseBuffer(buffer_frames_, AUDCLNT_BUFFERFLAGS_SILENT);
        written_frames_ = buffer_frames_;
    }
    // 鳴る時刻は再生位置から求める (取れなければ padding のまま)
    clock_.Reset();
    clock_freq_ = 0;
    if (FAILED(client_->GetService(IID_PPV_ARGS(&clock_))) || FAILED(clock_->GetFrequency(&clock_freq_)) || clock_freq_ == 0) {
        clock_.Reset();
        spdlog::info("audio [{}]: no audio clock; using the buffer padding for timing", name);
    }
    position_ready_ = false;
    calibrate_start_qpc_ = 0;
    calibrate_samples_.clear();
    offset_check_samples_.clear();
    offset_check_end_qpc_ = 0;
    offset_moved_checks_ = 0;
    no_position_since_qpc_ = 0;
    no_position_calls_ = 0;
    no_position_logged_ = false;

    // 音声の基本遅延 = 書いてから鳴り終わるまでの最大 + 余裕。
    // 共有モード: 書く量 + 先行して鳴る量 = バッファ 1 つ分
    // 排他モード: いま鳴っているバッファの次にバッファを丸ごと書く = バッファ 2 つ分
    const double buffer_ms = 1000.0 * buffer_frames_ / rate_;
    const double pipeline_ms = exclusive_ ? 2 * buffer_ms : buffer_ms;
    base_delay_qpc_ = qpc_from_us(int64_t(pipeline_ms * 1000)) + qpc_from_ms(options_.margin_ms);
    {
        std::lock_guard lk(sources_mutex_);
        sources_.clear();
        for (const auto& r : routes_) {
            if (r.start_channel < 1 || r.start_channel > channels_) {
                spdlog::warn("audio [{}]: ch{} start channel {} is out of range (device has {} ch)", name, r.ch, r.start_channel,
                             channels_);
            }
            sources_.push_back(std::make_unique<ChannelSource>(r, rate_, channels_, base_delay_qpc_));
        }
    }

    if (FAILED(client_->Start())) return false;

    {
        std::lock_guard lk(status_mutex_);
        status_.open = true;
        status_.ever_opened = true;
        status_.exclusive = exclusive_;
        status_.exclusive_fallback = options_.exclusive && !exclusive_ ? exclusive_fallback_ : "";
        status_.sample_rate = int(rate_);
        status_.channels = channels_;
        status_.sample_format = sample_type_name(int(sample_type_));
        status_.buffer_ms = buffer_ms;
        status_.delay_ms = double(qpc_to_us(base_delay_qpc_)) / 1000.0;
    }
    spdlog::info("audio [{}]: opened {} {} Hz {} ch {} buffer {:.2f} ms", name, exclusive_ ? "exclusive" : "shared", rate_, channels_,
                 sample_type_name(int(sample_type_)), buffer_ms);
    return true;
}

void DeviceOutput::close() {
    if (client_) client_->Stop();
    {
        std::lock_guard lk(sources_mutex_);
        for (auto& s : sources_) s->stats().playing = false;  // 開き直すまで鳴っていない (描画が呼ばれないので残ってしまう)
    }
    clock_.Reset();
    render_.Reset();
    client_.Reset();
    event_.reset();
    std::lock_guard lk(status_mutex_);
    status_.open = false;
}

void DeviceOutput::write_samples(BYTE* dst, int frames) {
    const size_t n = size_t(frames) * channels_;
    const float* src = mix_.data();
    switch (sample_type_) {
    case SampleType::Float32: {
        auto* d = reinterpret_cast<float*>(dst);
        for (size_t i = 0; i < n; ++i) d[i] = std::clamp(src[i], -1.0f, 1.0f);
        break;
    }
    case SampleType::Int16: {
        auto* d = reinterpret_cast<int16_t*>(dst);
        for (size_t i = 0; i < n; ++i) d[i] = int16_t(std::lrint(std::clamp(src[i], -1.0f, 1.0f) * 32767.0f));
        break;
    }
    case SampleType::Int32:
    case SampleType::Int24In32: {
        auto* d = reinterpret_cast<int32_t*>(dst);
        for (size_t i = 0; i < n; ++i) {
            d[i] = int32_t(std::llrint(double(std::clamp(src[i], -1.0f, 1.0f)) * 2147483647.0)) & ~0xFF;
        }
        break;
    }
    case SampleType::Int24: {
        for (size_t i = 0; i < n; ++i) {
            const int32_t v = int32_t(std::lrint(std::clamp(src[i], -1.0f, 1.0f) * 8388607.0f));
            dst[3 * i] = BYTE(v);
            dst[3 * i + 1] = BYTE(v >> 8);
            dst[3 * i + 2] = BYTE(v >> 16);
        }
        break;
    }
    }
}

// これから書くバッファの先頭のフレームが鳴る時刻 (QPC)。lead_frames: そのフレームより前にまだ鳴っていない量
int64_t DeviceOutput::heard_time_locked(int64_t now, uint32_t lead_frames) {
    // padding による時刻: 「今 + 溜まっている量」。仮想デバイス (VB-Cable、VoiceMeeter など) は音をまとめて取るので、
    // 呼ばれた時刻との関係が数 ms 揺れ (1 回だけ 10 ms 近く外れることもある)、ずれの補正が 1 秒に何十回も働いてしまう
    const int64_t by_padding = now + qpc_from_us(int64_t(lead_frames) * 1'000'000 / rate_);
    if (!clock_) return by_padding;

    // 再生位置による時刻: 位置 pos のフレームが qpc_pos に鳴っていたので、書いた量との差の分だけ後に鳴る。
    // 揺れが 1 ms 未満で、デバイスのクロックにも追従する
    UINT64 pos = 0, qpc_pos = 0;
    const HRESULT hr = clock_->GetPosition(&pos, &qpc_pos);
    if (FAILED(hr) || qpc_pos == 0) {
        // 取れない (開いた直後など): padding のまま。続くときはログに出す (DVS で、測り終えるまで 4 分かかったことがある)
        if (no_position_since_qpc_ == 0) no_position_since_qpc_ = now;
        ++no_position_calls_;
        if (!no_position_logged_ && now - no_position_since_qpc_ > qpc_from_ms(kNoPositionLogMs)) {
            no_position_logged_ = true;
            render_log().info(
                "audio [{}]: no position from the audio clock for {} s (GetPosition 0x{:08x}, time {}); using the buffer "
                "padding",
                to_utf8(options_.name), kNoPositionLogMs / 1000, uint32_t(hr), qpc_pos);
        }
        return by_padding;
    }
    if (no_position_since_qpc_ != 0) {
        if (no_position_logged_) {
            render_log().info("audio [{}]: the audio clock gives a position again after {:.1f} s ({} calls)", to_utf8(options_.name),
                              double(qpc_to_us(now - no_position_since_qpc_)) / 1e6, no_position_calls_);
        }
        no_position_since_qpc_ = 0;
        no_position_calls_ = 0;
        no_position_logged_ = false;
    }
    const double pos_frames = double(pos) * rate_ / double(clock_freq_);
    const int64_t at = qpc_from_us(int64_t(qpc_pos / 10));  // qpc_pos は 100ns 単位に直した QPC
    const int64_t by_position = at + qpc_from_us(int64_t((double(written_frames_) - pos_frames) * 1'000'000 / rate_));

    // 平均の時刻 (= 遅延) は今までと同じにする: padding との差 (中央値) を測って差し引く
    const int64_t diff = by_position - by_padding;
    if (!position_ready_) {
        if (calibrate_start_qpc_ == 0) calibrate_start_qpc_ = now;
        if (now < calibrate_start_qpc_ + qpc_from_ms(kCalibrateSkipMs)) return by_padding;  // 開いた直後は使わない
        calibrate_samples_.push_back(diff);
        if (now < calibrate_start_qpc_ + qpc_from_ms(kCalibrateSkipMs + kCalibrateMs)) return by_padding;
        position_offset_qpc_ = median(calibrate_samples_);
        calibrate_samples_.clear();
        position_ready_ = true;
        offset_check_end_qpc_ = now + qpc_from_ms(kOffsetCheckMs);
        offset_moved_checks_ = 0;
        render_log().info("audio [{}]: timing from the audio clock (offset {:+.2f} ms from the buffer padding)", to_utf8(options_.name),
                          double(qpc_to_us(position_offset_qpc_)) / 1000.0);
    }
    if (std::llabs(diff - position_offset_qpc_) > qpc_from_ms(kPositionMaxOffMs)) {
        // 位置が飛んだ (デバイス側のリセットなど): padding に戻して測り直す
        render_log().info("audio [{}]: audio clock moved {:+.1f} ms; measuring again", to_utf8(options_.name),
                          double(qpc_to_us(diff - position_offset_qpc_)) / 1000.0);
        position_ready_ = false;
        calibrate_start_qpc_ = 0;
        calibrate_samples_.clear();
        offset_check_samples_.clear();
        return by_padding;
    }
    // 測った差が偏っていないか: 中央値が kOffsetMovedMs 以上ずれたままなら、その値に替える
    offset_check_samples_.push_back(diff);
    if (now >= offset_check_end_qpc_) {
        const int64_t m = median(offset_check_samples_);
        offset_check_samples_.clear();
        offset_check_end_qpc_ = now + qpc_from_ms(kOffsetCheckMs);
        offset_moved_checks_ = std::llabs(m - position_offset_qpc_) >= qpc_from_ms(kOffsetMovedMs) ? offset_moved_checks_ + 1 : 0;
        if (offset_moved_checks_ >= kOffsetMovedChecks) {
            render_log().info("audio [{}]: offset from the buffer padding moved {:+.2f} -> {:+.2f} ms; using the new offset",
                              to_utf8(options_.name), double(qpc_to_us(position_offset_qpc_)) / 1000.0, double(qpc_to_us(m)) / 1000.0);
            position_offset_qpc_ = m;
            offset_moved_checks_ = 0;
        }
    }
    return by_position - position_offset_qpc_;
}

bool DeviceOutput::render() {
    UINT32 frames = buffer_frames_;
    UINT32 lead = buffer_frames_;  // 排他: いま鳴っているバッファの次に鳴る
    if (!exclusive_) {
        UINT32 padding = 0;
        if (FAILED(client_->GetCurrentPadding(&padding))) return false;
        frames = buffer_frames_ - padding;
        lead = padding;
    }
    const int64_t now = qpc_now();
    if (last_render_qpc_ != 0) {
        const int64_t gap = qpc_to_us(now - last_render_qpc_);
        if (gap > max_gap_us_.load()) max_gap_us_ = gap;
    }
    last_render_qpc_ = now;
    ++render_count_;
    if (frames == 0) return true;

    const int64_t first_hear = heard_time_locked(now, lead);
    written_frames_ += frames;
    mix_.assign(size_t(frames) * channels_, 0.0f);
    {
        std::lock_guard lk(sources_mutex_);
        for (auto& s : sources_) s->render(mix_.data(), int(frames), first_hear, base_delay_qpc_);
    }

    BYTE* data = nullptr;
    if (FAILED(render_->GetBuffer(frames, &data))) return false;
    write_samples(data, int(frames));
    return SUCCEEDED(render_->ReleaseBuffer(frames, 0));
}

void DeviceOutput::run() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    DWORD task_index = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task_index);
    const std::string name = to_utf8(options_.name);

    while (!stop_) {
        if (!open()) {
            close();
            ++open_failures_;
            WaitForSingleObject(stop_event_.get(), 1000);  // デバイスが戻るまで 1 秒ごとに試す
            continue;
        }
        open_failures_ = 0;
        const int64_t opened_qpc = qpc_now();
        bool lost = false;
        // 排他モードで、デバイスからの呼び出しが周期どおりに来ているかを見る
        const double expected_calls = double(rate_) / buffer_frames_;
        int64_t check_start = qpc_now();
        uint64_t check_calls = 0;
        while (!stop_) {
            HANDLE handles[2] = {event_.get(), stop_event_.get()};
            const DWORD w = WaitForMultipleObjects(2, handles, FALSE, 500);
            if (w == WAIT_OBJECT_0 + 1) break;
            if (!render()) {
                // 開いてすぐ失うのが続くときは、最初の数回と 10 回ごとにだけ出す
                if (quick_losses_ < 3 || quick_losses_ % 10 == 0) {
                    spdlog::warn("audio [{}]: device lost, reopening{}", name,
                                 quick_losses_ > 0 ? fmt::format(" ({} times in a row soon after opening)", quick_losses_ + 1) : "");
                }
                lost = true;
                break;
            }
            ++check_calls;
            if (exclusive_ && qpc_now() - check_start >= qpc_from_ms(1000)) {
                const double calls = double(check_calls) * qpc_freq() / double(qpc_now() - check_start);
                if (calls < expected_calls * 0.9) {
                    // 周期を延ばして排他モードのまま試し、上限を超えたら共有モードにする
                    const REFERENCE_TIME current = REFERENCE_TIME(std::llround(10'000'000.0 * buffer_frames_ / rate_));
                    exclusive_period_ = std::max(kRetryExclusivePeriod, current * 2);
                    exclusive_failed_ = exclusive_period_ > kMaxExclusivePeriod;
                    if (exclusive_failed_) exclusive_fallback_ = "tooSlow";
                    spdlog::warn(
                        "audio [{}]: device does not keep up with the exclusive-mode period ({:.0f}/{:.0f} "
                        "calls/s); {}",
                        name, calls, expected_calls,
                        exclusive_failed_ ? std::string("falling back to shared mode")
                                          : fmt::format("retrying with {:.0f} ms period", double(exclusive_period_) / 10'000));
                    break;
                }
                check_start = qpc_now();
                check_calls = 0;
            }
        }
        close();
        if (!stop_) {
            std::lock_guard lk(status_mutex_);
            ++status_.reopens;
        }
        // 開いてすぐ (5 秒以内) 失うのが続くなら、開き直すまでの間を延ばす (1 → 2 → 4 → 8 → 10 秒)。排他モードのデバイスが
        // 止まったときなどに、500 ms ごとに開き直しとログを繰り返さないように
        quick_losses_ = lost && qpc_now() - opened_qpc < qpc_from_ms(kQuickLossMs) ? quick_losses_ + 1 : 0;
        if (quick_losses_ > 0 && !stop_) {
            WaitForSingleObject(stop_event_.get(), DWORD(std::min<int64_t>(500LL << std::min(quick_losses_, 5), 10'000)));
        }
    }

    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
    CoUninitialize();
}

}  // namespace denpachan::engine
