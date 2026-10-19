// SPDX-License-Identifier: GPL-3.0-only
// denpachan-asio.dll: ASIO のドライバへの出力 (Dante Virtual Soundcard の ASIO モードなど)。設計書 §6.4。
// Copyright (c) 2026 RTA in Japan
//
// Steinberg の ASIO SDK (GPL-3.0) のヘッダを使うので、この DLL は GPL-3.0 (src/asio/LICENSE)。
// engine (MIT) とは denpachan_asio.h の C の関数だけで受け渡す。
//
// - ドライバはレジストリ (HKLM\SOFTWARE\ASIO) で列挙し、COM で読み込む (SDK のヘッダだけを使い、host のソースは使わない)。
// - ASIO の作法に合わせ、同時に開く ASIO ドライバは 1 つだけ (コールバックにどの出力かを渡す引数が無いため)。
// - ドライバを開いたスレッド (STA) でメッセージを回し、ドライバのリセット要求 (設定の変更など) が来たら開き直す。
//   開けない・落ちたときは 1 秒ごとに開き直す。
// - 音はドライバのコールバック (bufferSwitch) の中で engine の render に作ってもらい、ドライバのサンプル形式に変換して書く。
//   遅延を一定に保つ・クロックずれの補正・フェード・ミュートは engine (ChannelSource) が行う。

#include "denpachan_asio.h"

#include <windows.h>
#include <objbase.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

// ASIO SDK (GPL-3.0、cmake/FetchAsioSdk.cmake)。インターフェースの定義だけを使う
#include "asiosys.h"
#include "asio.h"
#include "iasiodrv.h"

namespace {

constexpr int64_t kReanchorMs = 200;    // 予測した時刻と呼ばれた時刻がこれ以上ずれたら測り直す
constexpr int64_t kStallMs = 2000;      // コールバックがこれだけ来なければ「止まっている」とログに出す
constexpr int64_t kFollowDivisor = 32;  // 呼ばれた時刻へ寄せる割合 (コールバックごとに 1/32)

// ---- QPC (単調時刻) ----

int64_t qpc_freq() {
    static const int64_t freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f.QuadPart;
    }();
    return freq;
}

int64_t qpc_now() {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return c.QuadPart;
}

// 整数部と余りを分けて計算する (先に掛けると int64 が溢れる)
int64_t qpc_from_us(int64_t us) {
    const int64_t f = qpc_freq();
    return (us / 1'000'000) * f + (us % 1'000'000) * f / 1'000'000;
}
int64_t qpc_from_ms(int64_t ms) { return qpc_from_us(ms * 1000); }

class UniqueHandle {
public:
    explicit UniqueHandle(HANDLE h) : h_(h) {}
    ~UniqueHandle() {
        if (h_) CloseHandle(h_);
    }
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;
    HANDLE get() const { return h_; }

private:
    HANDLE h_;
};

}  // namespace

// コールバックには「どの出力か」を渡す引数が無いので、開いている出力を 1 つだけ覚えておく
static std::atomic<denpachan_asio_output*> g_active{nullptr};

static std::mutex g_probe_mutex;
static std::map<std::wstring, denpachan_asio_probe> g_probe_cache;  // CLSID ごと

struct denpachan_asio_output {
    denpachan_asio_output(std::wstring clsid, int preferred_rate, const denpachan_asio_callbacks& callbacks);
    ~denpachan_asio_output();

    // ドライバのコールバックから呼ぶ
    void on_buffer_switch(long index);
    void request_reset(const char* reason);

private:
    void run();
    bool open();
    void close();
    void write_channel(int channel, void* dst, int frames) const;
    void log(int level, const char* format, ...) const;

    const std::wstring clsid_;
    const int preferred_rate_;
    const denpachan_asio_callbacks cb_;

    IASIO* asio_ = nullptr;
    bool buffers_created_ = false;
    bool output_ready_ = false;  // ドライバが outputReady に対応している
    bool opened_ = false;        // opened を呼んだ (closed を呼ぶ)
    int open_failures_ = 0;
    int channels_ = 0;
    uint32_t rate_ = 0;
    long buffer_frames_ = 0;
    long output_latency_ = 0;      // フレーム (bufferSwitch から最初のサンプルが鳴るまで)
    int64_t next_first_hear_ = 0;  // 次のバッファの先頭が鳴る時刻 (予測。0 = まだ測っていない)
    uint64_t callbacks_ = 0;       // 診断: コールバックの回数
    int64_t callbacks_since_qpc_ = 0;
    std::atomic<int64_t> last_callback_qpc_{0};  // 診断: 最後にコールバックが来た時刻 (0 = 開いてからまだ来ていない)
    std::vector<long> sample_types_;             // チャンネルごとの ASIOSampleType
    std::vector<void*> buffers_[2];              // チャンネルごとのダブルバッファ
    std::vector<float> mix_;                     // channels_ x buffer_frames_ (インターリーブ)

    std::thread thread_;
    std::atomic<bool> stop_{false};
    UniqueHandle stop_event_;
    UniqueHandle reset_event_;
};

namespace {

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
        return 1;  // 位置合わせは engine が時刻で行う
    case kAsioSupportsTimeInfo:
        return 0;  // 時刻の情報は使わない (bufferSwitch を呼んでもらう)
    default:
        return 0;
    }
}

ASIOCallbacks g_callbacks{&cb_buffer_switch, &cb_sample_rate_changed, &cb_asio_message, &cb_buffer_switch_time_info};

// ドライバを読み込んで初期化する (呼ぶ側のスレッドで COM を初期化しておく)
IASIO* load_driver(const std::wstring& clsid_str, std::string& error) {
    CLSID clsid{};
    if (FAILED(CLSIDFromString(clsid_str.c_str(), &clsid))) {
        error = "bad CLSID";
        return nullptr;
    }
    IASIO* asio = nullptr;
    // ASIO のドライバは CLSID をそのまま IID として渡す決まり
    const HRESULT hr = CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, clsid, reinterpret_cast<void**>(&asio));
    if (FAILED(hr) || !asio) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "cannot load the driver (0x%08lx)", static_cast<unsigned long>(hr));
        error = buf;
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

denpachan_asio_output::denpachan_asio_output(std::wstring clsid, int preferred_rate,
                                             const denpachan_asio_callbacks& callbacks)
    : clsid_(std::move(clsid)),
      preferred_rate_(preferred_rate),
      cb_(callbacks),
      stop_event_(CreateEventW(nullptr, TRUE, FALSE, nullptr)),
      reset_event_(CreateEventW(nullptr, FALSE, FALSE, nullptr)) {
    thread_ = std::thread(&denpachan_asio_output::run, this);
}

denpachan_asio_output::~denpachan_asio_output() {
    stop_ = true;
    SetEvent(stop_event_.get());
    if (thread_.joinable()) thread_.join();
}

void denpachan_asio_output::log(int level, const char* format, ...) const {
    char buf[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);
    cb_.log(cb_.user, level, buf);
}

void denpachan_asio_output::request_reset(const char* reason) {
    log(DENPACHAN_ASIO_LOG_INFO, "%s; reopening", reason);
    SetEvent(reset_event_.get());
}

bool denpachan_asio_output::open() {
    // 開けない理由は最初と 1 分ごとに出す (1 秒ごとに試すので、毎回出すとログの履歴が埋まる)
    const int fail_level = open_failures_ == 0 || open_failures_ % 60 == 0 ? DENPACHAN_ASIO_LOG_ERROR : DENPACHAN_ASIO_LOG_DEBUG;

    {
        std::lock_guard lk(g_probe_mutex);
        denpachan_asio_output* expected = nullptr;
        if (!g_active.compare_exchange_strong(expected, this)) {
            log(fail_level, "another ASIO driver is in use (only one ASIO driver at a time)");
            return false;
        }
    }

    std::string error;
    asio_ = load_driver(clsid_, error);
    if (!asio_) {
        log(fail_level, "%s", error.c_str());
        return false;
    }

    long in = 0, out = 0;
    if (asio_->getChannels(&in, &out) != ASE_OK || out <= 0) {
        log(fail_level, "no output channels");
        return false;
    }
    channels_ = int(out);

    ASIOSampleRate rate = 0;
    asio_->getSampleRate(&rate);
    if (preferred_rate_ > 0 && rate != preferred_rate_ && asio_->canSampleRate(preferred_rate_) == ASE_OK) {
        asio_->setSampleRate(preferred_rate_);  // 変えられない (Dante が決めている) ときはそのままのレートで出す
        asio_->getSampleRate(&rate);
    }
    if (rate <= 0) {
        log(fail_level, "unknown sample rate");
        return false;
    }
    rate_ = uint32_t(std::lround(rate));

    long min_size = 0, max_size = 0, preferred = 0, granularity = 0;
    if (asio_->getBufferSize(&min_size, &max_size, &preferred, &granularity) != ASE_OK || preferred <= 0) {
        log(fail_level, "cannot get the buffer size");
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
            log(fail_level, "unsupported sample type %ld (ch %d)", sample_types_[size_t(c)], c + 1);
            return false;
        }
    }

    std::vector<ASIOBufferInfo> infos(static_cast<size_t>(channels_));
    for (int c = 0; c < channels_; ++c) {
        infos[size_t(c)].isInput = ASIOFalse;
        infos[size_t(c)].channelNum = c;
    }
    if (asio_->createBuffers(infos.data(), channels_, buffer_frames_, &g_callbacks) != ASE_OK) {
        log(fail_level, "cannot create buffers (%d ch, %ld frames)", channels_, buffer_frames_);
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

    mix_.assign(size_t(channels_) * size_t(buffer_frames_), 0.0f);
    next_first_hear_ = 0;
    // 無音で埋めてから開始する
    for (int i = 0; i < 2; ++i) {
        for (int c = 0; c < channels_; ++c) write_channel(c, buffers_[i][size_t(c)], int(buffer_frames_));
    }
    output_ready_ = asio_->outputReady() == ASE_OK;

    // 音を出し始める前に engine に形式を知らせる (render の準備をしてもらう)
    const denpachan_asio_format format{channels_, int(rate_), int(buffer_frames_), int(output_latency_),
                                       sample_type_name(sample_types_[0])};
    cb_.opened(cb_.user, &format);
    opened_ = true;

    if (asio_->start() != ASE_OK) {
        log(fail_level, "cannot start");
        return false;
    }
    {
        std::lock_guard lk(g_probe_mutex);
        g_probe_cache[clsid_] = denpachan_asio_probe{channels_, int(rate_)};
    }
    log(DENPACHAN_ASIO_LOG_INFO, "opened ASIO %u Hz %d ch %s buffer %ld frames (%.2f ms), output latency %ld frames", rate_,
        channels_, format.sample_format, buffer_frames_, 1000.0 * double(buffer_frames_) / rate_, output_latency_);
    return true;
}

void denpachan_asio_output::close() {
    if (asio_) {
        asio_->stop();
        if (buffers_created_) asio_->disposeBuffers();
        buffers_created_ = false;
        asio_->Release();
        asio_ = nullptr;
    }
    {
        std::lock_guard lk(g_probe_mutex);
        denpachan_asio_output* self = this;
        g_active.compare_exchange_strong(self, nullptr);
    }
    if (opened_) {
        opened_ = false;
        cb_.closed(cb_.user);
    }
}

void denpachan_asio_output::write_channel(int channel, void* dst, int frames) const {
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

void denpachan_asio_output::on_buffer_switch(long index) {
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
    last_callback_qpc_.store(now);
    if (callbacks_since_qpc_ == 0) callbacks_since_qpc_ = now;
    if (now - callbacks_since_qpc_ >= qpc_from_ms(5000)) {
        const double per_sec = double(callbacks_) * double(qpc_freq()) / double(now - callbacks_since_qpc_);
        const double expected = double(rate_) / double(buffer_frames_);
        if (std::abs(per_sec / expected - 1.0) > 0.02) {
            log(DENPACHAN_ASIO_LOG_WARN, "the device calls back %.1f times/s (expected %.1f); it does not play in real time",
                per_sec, expected);
        }
        callbacks_ = 0;
        callbacks_since_qpc_ = now;
    }
    std::fill(mix_.begin(), mix_.end(), 0.0f);
    cb_.render(cb_.user, mix_.data(), frames, first_hear);
    for (int c = 0; c < channels_; ++c) write_channel(c, buffers_[index][size_t(c)], frames);
    if (output_ready_) asio_->outputReady();
}

void denpachan_asio_output::run() {
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
        // 診断: 開始したのにコールバックが来ない (止まった) ときはログに出す。
        // ドライバの先が動いていない (Voicemeeter なら出力 A1 のデバイスが無いなど) ときに起こる。来始めたらそれも出す
        last_callback_qpc_.store(0);
        const int64_t started_qpc = qpc_now();
        bool stalled = false;
        while (!stop_) {
            HANDLE handles[2] = {stop_event_.get(), reset_event_.get()};
            const DWORD w = MsgWaitForMultipleObjects(2, handles, FALSE, 500, QS_ALLINPUT);
            if (w == WAIT_OBJECT_0 || w == WAIT_OBJECT_0 + 1) break;
            const int64_t last = last_callback_qpc_.load();
            const bool silent = qpc_now() - (last ? last : started_qpc) > qpc_from_ms(kStallMs);
            if (silent != stalled) {
                stalled = silent;
                if (stalled) {
                    log(DENPACHAN_ASIO_LOG_WARN,
                        "the driver has not called back for %lld ms (the driver is not running; "
                        "for Voicemeeter, check that it has an output device)",
                        static_cast<long long>(kStallMs));
                } else {
                    log(DENPACHAN_ASIO_LOG_INFO, "the driver calls back again");
                }
            }
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
        close();
    }
    CoUninitialize();
}

// ---- C の関数 (denpachan_asio.h) ----

extern "C" {

int denpachan_asio_abi_version(void) { return DENPACHAN_ASIO_ABI_VERSION; }

int denpachan_asio_list_drivers(denpachan_asio_driver* drivers, int max_drivers) {
    HKEY root = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ASIO", 0, KEY_READ, &root) != ERROR_SUCCESS) return 0;
    int count = 0;
    for (DWORD i = 0;; ++i) {
        wchar_t name[256];
        DWORD name_len = DWORD(std::size(name));
        if (RegEnumKeyExW(root, i, name, &name_len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        wchar_t clsid[64] = {};
        DWORD size = sizeof(clsid);
        if (RegGetValueW(root, name, L"CLSID", RRF_RT_REG_SZ, nullptr, clsid, &size) != ERROR_SUCCESS) continue;
        if (count < max_drivers) {
            wcsncpy_s(drivers[count].name, name, _TRUNCATE);
            wcsncpy_s(drivers[count].clsid, clsid, _TRUNCATE);
        }
        ++count;
    }
    RegCloseKey(root);
    return count;
}

int denpachan_asio_probe_driver(const wchar_t* clsid_str, denpachan_asio_probe* probe) {
    const std::wstring clsid = clsid_str;
    {
        std::lock_guard lk(g_probe_mutex);
        if (auto it = g_probe_cache.find(clsid); it != g_probe_cache.end()) {
            *probe = it->second;
            return 1;
        }
        if (g_active.load() != nullptr) return 0;  // 別のドライバを開いている間は 2 つ目を開かない
    }
    // ドライバは STA で扱う。呼んだスレッドの COM の状態に左右されないよう、別のスレッドで開く
    std::optional<denpachan_asio_probe> result;
    std::thread([&] {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        std::string error;
        if (IASIO* asio = load_driver(clsid, error)) {
            long in = 0, out = 0;
            ASIOSampleRate rate = 0;
            if (asio->getChannels(&in, &out) == ASE_OK) {
                asio->getSampleRate(&rate);
                result = denpachan_asio_probe{int(out), int(rate)};
            }
            asio->Release();
        }
        CoUninitialize();
    }).join();
    if (!result) return 0;
    std::lock_guard lk(g_probe_mutex);
    g_probe_cache[clsid] = *result;
    *probe = *result;
    return 1;
}

denpachan_asio_output* denpachan_asio_open(const wchar_t* clsid, int preferred_sample_rate,
                                           const denpachan_asio_callbacks* callbacks) {
    if (!clsid || !callbacks || !callbacks->opened || !callbacks->closed || !callbacks->render || !callbacks->log)
        return nullptr;
    return new denpachan_asio_output(clsid, preferred_sample_rate, *callbacks);
}

void denpachan_asio_close(denpachan_asio_output* output) { delete output; }

}  // extern "C"
