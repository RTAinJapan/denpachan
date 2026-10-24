// 音声の時間軸の試験 (音声デバイスなし)。設計書 §6.3。
//
// worker の書き手 (AudioWriter + PlayoutClock、A/V 同期) と engine の読み手 (ChannelSource) を、仮想の時計で動かす。
// 送出元から音声のかたまり (AAC の 1024 サンプル) が届き、デバイスが 10 ms ごとに描画する。場合ごとに、
// 鳴らし始めてからの重複・間引きと合わせ直しの回数を数え、上限を超えたら終了コード 1。
//
//   audio_timing_test            すべての場合
//   audio_timing_test -v         時間軸の動き (worker の debug ログ) も出す
//   audio_timing_test -v N       N 番目 (1 始まり) の場合だけ
//
// 音声リングは engine と同じ名前の共有メモリ (Local\denpachan_audio_N) を使うので、denpachan が動いているときは回さない
// (動いていれば、その共有メモリがすでにあるので止める)。

#include <spdlog/spdlog.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "../engine/channel_source.h"
#include "../worker/audio_writer.h"
#include "../worker/metrics.h"
#include "../worker/playout_clock.h"
#include "denpachan/audio_shm.h"

namespace denpachan::engine {
// FFmpeg は使わない (デバイスは 48 kHz なのでリサンプルしない)
bool ffmpeg_available() { return false; }
}  // namespace denpachan::engine

namespace {

using namespace denpachan;

int64_t g_now = 0;  // 仮想の時計 (QPC)
bool g_verbose = false;
int64_t fake_now() { return g_now; }

constexpr int kChunkFrames = 1024;     // AAC の 1 かたまり
constexpr int kDeviceFrames = 480;     // デバイスの 1 回の描画 (10 ms)
constexpr int64_t kNetDelayMs = 30;    // 送出元から届くまで
constexpr int64_t kLeadMs = 10;        // デバイスで、これから書くバッファが鳴るまで (共有モードの padding)
constexpr int64_t kBaseDelayMs = 32;   // engine の基本遅延 (バッファ 22 ms + 余裕 10 ms。DVS の実測)
constexpr int kPlayoutBufferMs = 100;  // チャンネルの再生バッファ (既定)

struct Case {
    std::string name;
    double seconds = 60;
    double sender_ppm = 0;       // 送出元の時計の速さ (正: 速い)
    double device_ppm = 0;       // デバイスの時計の速さ (正: 速い)
    double device_open_sec = 0;  // engine がデバイスを開く時刻 (0 = worker より先)
    // かたまり k が、いつもより何 ms 遅れて届くか
    std::function<double(int k, double t)> late_ms = [](int, double) { return 0.0; };
    uint64_t max_slips = 0;
    uint64_t max_resyncs = 0;  // 鳴らし始めた 1 回は数えない
    uint64_t min_resyncs = 0;
};

struct Result {
    uint64_t slips = 0;
    uint64_t resyncs = 0;
    std::string last_resync;
    bool ok = false;
};

Result run(const Case& c, int ch) {
    Result r;
    {
        // 試験の前に、同じ名前の共有メモリが無いこと (denpachan が動いていないこと) を確かめる
        SharedMapping probe;
        bool created = false;
        if (!probe.open_or_create(audio::mapping_name(ch), audio::kMappingBytes, created) || !created) {
            std::printf("SKIP %s: the audio ring of ch%d already exists (is denpachan running?)\n", c.name.c_str(), ch);
            return r;
        }
    }
    g_now = qpc_from_ms(1000);
    const int64_t start = g_now;
    worker::PlayoutClock clock(kPlayoutBufferMs, &fake_now);
    worker::ChannelMetrics metrics;
    worker::AudioWriter writer(clock, metrics, true);
    if (!writer.open(ch)) {
        std::printf("FAIL %s: cannot open the audio ring\n", c.name.c_str());
        return r;
    }

    std::unique_ptr<engine::ChannelSource> source;
    engine::RouteOptions route;
    route.ch = ch;
    auto open_device = [&] { source = std::make_unique<engine::ChannelSource>(route, audio::kSampleRate, 2, qpc_from_ms(kBaseDelayMs)); };
    if (c.device_open_sec <= 0) open_device();

    std::vector<float> chunk(size_t(kChunkFrames) * 2);
    std::vector<float> mix(size_t(kDeviceFrames) * 2);
    const double chunk_sec = double(kChunkFrames) / audio::kSampleRate;
    const double device_sec = double(kDeviceFrames) / audio::kSampleRate;
    int k = 0;
    int64_t render_start = start;  // デバイスを開いた時刻
    int64_t next_render = start;
    int renders = 0;
    uint64_t base_slips = 0, base_resyncs = 0;
    bool started = false;
    const int64_t end = start + qpc_from_us(int64_t(c.seconds * 1e6));
    for (; g_now < end; g_now += qpc_from_us(250)) {
        const double t = double(qpc_to_us(g_now - start)) / 1e6;
        // 送出元: かたまり k の時刻 (送出元の時計) と、届く時刻
        for (;;) {
            const double sent = k * chunk_sec / (1 + c.sender_ppm * 1e-6);
            const int64_t arrival = start + qpc_from_us(int64_t((sent + (kNetDelayMs + c.late_ms(k, sent)) / 1000.0) * 1e6));
            if (arrival > g_now) break;
            for (int i = 0; i < kChunkFrames; ++i) {
                const float v = 0.25f * float(std::sin(2 * 3.14159265 * 440.0 * (double(k) * kChunkFrames + i) / audio::kSampleRate));
                chunk[2 * size_t(i)] = v;
                chunk[2 * size_t(i) + 1] = v;
            }
            const int64_t pts_us = int64_t(double(k) * kChunkFrames * 1e6 / audio::kSampleRate);
            writer.write(1, pts_us, arrival, chunk.data(), kChunkFrames);
            ++k;
        }
        if (!source && c.device_open_sec > 0 && t >= c.device_open_sec) {
            open_device();
            render_start = next_render = g_now;
            renders = 0;
        }
        // デバイス: 10 ms (デバイスの時計) ごとに描画する。これから書くバッファは kLeadMs 後に鳴る
        if (source && g_now >= next_render) {
            std::fill(mix.begin(), mix.end(), 0.0f);
            source->render(mix.data(), kDeviceFrames, g_now + qpc_from_ms(kLeadMs), qpc_from_ms(kBaseDelayMs));
            ++renders;
            next_render = render_start + qpc_from_us(int64_t(renders * device_sec / (1 + c.device_ppm * 1e-6) * 1e6));
            auto& st = source->stats();
            if (g_verbose && renders % 100 == 0) {
                std::lock_guard lk(st.reason_mutex);
                std::printf("  t %6.2f s: playing %d, error %+6.2f ms, slips %llu, resyncs %llu (%s)\n", t, int(st.playing.load()),
                            st.error_ms.load(), (unsigned long long)st.slips.load(), (unsigned long long)st.resyncs.load(),
                            st.last_resync_reason.c_str());
            }
            if (!started && st.playing.load()) {
                started = true;  // 鳴らし始めた。ここから数える
                base_slips = st.slips.load();
                base_resyncs = st.resyncs.load();
            }
        }
    }
    if (!source || !started) {
        std::printf("FAIL %s: never started playing\n", c.name.c_str());
        return r;
    }
    auto& st = source->stats();
    r.slips = st.slips.load() - base_slips;
    r.resyncs = st.resyncs.load() - base_resyncs;
    {
        std::lock_guard lk(st.reason_mutex);
        r.last_resync = st.last_resync_reason;
    }
    r.ok = r.slips <= c.max_slips && r.resyncs <= c.max_resyncs && r.resyncs >= c.min_resyncs;
    std::printf("%s %s: slips %llu (max %llu), resyncs %llu (max %llu)%s%s\n", r.ok ? "OK  " : "FAIL", c.name.c_str(),
                (unsigned long long)r.slips, (unsigned long long)c.max_slips, (unsigned long long)r.resyncs,
                (unsigned long long)c.max_resyncs, r.last_resync.empty() ? "" : ", last resync: ", r.last_resync.c_str());
    return r;
}

}  // namespace

int main(int argc, char** argv) {
    const bool verbose = argc > 1 && std::strcmp(argv[1], "-v") == 0;
    g_verbose = verbose;
    const int only = argc > 2 ? std::atoi(argv[2]) : 0;
    spdlog::set_level(verbose ? spdlog::level::debug : spdlog::level::warn);
    spdlog::set_pattern("  %v");

    // 鳴らし始めた直後に、届くのが何度か止まり、止まっていた分がまとめて届く (RTMP pull の起動直後。
    // 揺れの備えが 10 ms から 45 ms ほどまで増える)。stalls: {止まり始める時刻 (秒), 止まる長さ (ms)}
    auto stalls = [](std::vector<std::pair<double, double>> list) {
        return [list](int, double sent) {
            const double at = sent + kNetDelayMs / 1000.0;  // いつもなら届く時刻
            for (const auto& [from, ms] : list) {
                const double until = from + ms / 1000.0;
                if (at >= from && at < until) return (until - at) * 1000.0;  // 止まっていた間の分は、止まり終わりにまとめて届く
            }
            return 0.0;
        };
    };
    const auto startup_late = stalls({{2.0, 34}, {3.0, 58}, {4.5, 79}});
    const std::vector<Case> cases = {
        {.name = "steady, no clock difference", .seconds = 60, .max_slips = 0, .max_resyncs = 0},
        {.name = "sender clock 150 ppm fast", .seconds = 120, .sender_ppm = 150, .max_slips = 1000, .max_resyncs = 0},
        {.name = "device clock 150 ppm fast", .seconds = 120, .device_ppm = 150, .max_slips = 1000, .max_resyncs = 0},
        {.name = "late arrivals right after start (delay grows ~35 ms)",
         .seconds = 90,
         .late_ms = startup_late,
         .max_slips = 100,
         .max_resyncs = 3},
        {.name = "late arrivals right after start, with 24 ppm drift",
         .seconds = 90,
         .sender_ppm = 24,
         .late_ms = startup_late,
         .max_slips = 300,
         .max_resyncs = 3},
        {.name = "device opens 5 s after the stream started", .seconds = 60, .device_open_sec = 5, .max_slips = 100, .max_resyncs = 3},
        // 時間軸がゆっくり動き続ける場合の engine の歯止め: 500 ppm (24 回/秒) までは重複・間引きで追い、それを超えて
        // 動き続けたら合わせ直す (重複・間引きを何十秒も続けない)
        {.name = "sender clock 400 ppm fast (below the slip-rate limit)",
         .seconds = 60,
         .sender_ppm = 400,
         .max_slips = 1500,
         .max_resyncs = 0},
        {.name = "sender clock 1000 ppm fast (above the slip-rate limit)",
         .seconds = 60,
         .sender_ppm = 1000,
         .max_slips = 1500,
         .max_resyncs = 40,
         .min_resyncs = 1},
    };
    bool ok = true;
    int ch = 1;
    for (size_t i = 0; i < cases.size(); ++i) {
        if (only == 0 || only == int(i) + 1) ok &= run(cases[i], ch % 8 + 1).ok;
        ++ch;
    }
    std::printf("%s\n", ok ? "all OK" : "FAILED");
    return ok ? 0 : 1;
}
