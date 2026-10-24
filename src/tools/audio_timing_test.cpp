// 音声の時間軸の試験 (音声デバイスなし)。設計書 §6.3。
//
// worker の書き手 (AudioWriter + PlayoutClock、A/V 同期) と engine の読み手 (ChannelSource) を、仮想の時計で動かす。
// 送出元から音声のかたまり (AAC の 1024 サンプル) が届き、デバイスが 10 ms ごとに描画する。場合ごとに、
// 鳴らし始めてからの重複・間引きと合わせ直しの回数、落ち着いた後 (kSettledSec 以降) に時間軸を動かした回数を数え、
// 上限を超えたら終了コード 1。重複・間引きの上限は、桁が変わっていないかを見るためのもの (回数そのものに実害はほぼ無い)。
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
    // 届く時刻の揺れ: 0.5〜1.5 秒ごとに変わる ±jitter_ms の一様乱数 (決まった種)。窓 (1 秒) ごとの最小の余裕が揺れる
    double jitter_ms = 0;
    // engine の出力遅延の変わり方 {時刻 (秒), 出力遅延 (ms)}。0 = 出力先なし (音声を「なし」にした)、それ以外は
    // その遅延でデバイスを開き直す (WDM ↔ ASIO の切り替えなど)
    std::vector<std::pair<double, double>> delay_changes = {};
    uint64_t max_slips = 0;
    uint64_t max_resyncs = 0;  // 鳴らし始めた 1 回は数えない (開き直したあとの鳴らし始めは数える)
    uint64_t min_resyncs = 0;
    double max_moved_ms = 1e9;  // 時間軸 (映像も同じ) を鳴らし始めてから動かした量の合計 (向きによらず)
    // 落ち着いた後 (kSettledSec 以降) に時間軸を動かした回数の上限。揺れだけなら動かさない
    int max_moves = 1'000'000;
    // 落ち着いた後に、前と逆向きに動かした回数の上限 (揺れでの +1/−1 の行き来)。クロック差や揺れの備えを減らした分は同じ向き
    int max_reversals = 1'000'000;
};

constexpr double kSettledSec = 15;  // これより後を「落ち着いた後」として数える

struct Result {
    uint64_t slips = 0;
    uint64_t resyncs = 0;
    double moved_ms = 0;
    int moves = 0;                   // 落ち着いた後に時間軸を動かした回数
    int reversals = 0;               // そのうち、前に動かしたのと逆向きのもの
    double min_slack_ms = 1e9;       // 落ち着いた後の音声の余裕 (鳴らすべき時刻 - 出力遅延 - 到着) の最小
    double max_window_slack_ms = 0;  // 同じく、1 秒ごとの最小の余裕の最大 (遅延が余っている分)
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
    int64_t base_delay = qpc_from_ms(kBaseDelayMs);
    // 開き直す前のデバイスの回数 (ChannelSource ごとに数えるので足しておく)
    uint64_t done_slips = 0, done_resyncs = 0;
    auto close_device = [&] {
        if (!source) return;
        done_slips += source->stats().slips.load();
        done_resyncs += source->stats().resyncs.load();
        source.reset();
    };
    auto open_device = [&] { source = std::make_unique<engine::ChannelSource>(route, audio::kSampleRate, 2, base_delay); };
    if (c.device_open_sec <= 0) open_device();
    size_t next_change = 0;
    uint32_t rng = 12345;  // 到着の揺れ (決まった種の線形合同法)
    auto jitter = [&] {
        rng = rng * 1664525u + 1013904223u;
        return (double(rng >> 8) / double(1u << 24) * 2 - 1) * c.jitter_ms;
    };
    double jitter_now = jitter();
    double jitter_until = 0;  // 揺れの値を次に変える時刻 (送出元の時計、秒)
    // 時間軸の動き: pts 0 を出すべき時刻が、鳴らし始めてからどれだけ動いたか (合計)
    int64_t last_due0 = 0;
    int last_dir = 0;  // 前に動かした向き
    double moved_ms = 0;
    double window_slack_ms = 1e9;  // 今の 1 秒の最小の余裕
    double window_until = kSettledSec + 1;

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
            const int64_t arrival = start + qpc_from_us(int64_t((sent + (kNetDelayMs + c.late_ms(k, sent) + jitter_now) / 1000.0) * 1e6));
            if (arrival > g_now) break;
            if (sent >= jitter_until) {
                jitter_now = jitter();
                jitter_until = sent + 1.0 + jitter() / std::max(1.0, c.jitter_ms) * 0.5;
            }
            for (int i = 0; i < kChunkFrames; ++i) {
                const float v = 0.25f * float(std::sin(2 * 3.14159265 * 440.0 * (double(k) * kChunkFrames + i) / audio::kSampleRate));
                chunk[2 * size_t(i)] = v;
                chunk[2 * size_t(i) + 1] = v;
            }
            const int64_t pts_us = int64_t(double(k) * kChunkFrames * 1e6 / audio::kSampleRate);
            writer.write(1, pts_us, arrival, chunk.data(), kChunkFrames);
            if (started && t >= kSettledSec) {
                const double slack = double(qpc_to_us(clock.due(1, pts_us, arrival) - base_delay - arrival)) / 1000.0;
                r.min_slack_ms = std::min(r.min_slack_ms, slack);
                window_slack_ms = std::min(window_slack_ms, slack);
            }
            ++k;
        }
        if (!source && c.device_open_sec > 0 && t >= c.device_open_sec && next_change == 0) {
            open_device();
            render_start = next_render = g_now;
            renders = 0;
        }
        if (next_change < c.delay_changes.size() && t >= c.delay_changes[next_change].first) {
            const double ms = c.delay_changes[next_change++].second;
            if (source && ms == 0) source->clear_output_delay();  // 出力先なし (engine がデバイスの出力を止めたとき)
            close_device();
            if (ms > 0) {
                base_delay = qpc_from_us(int64_t(ms * 1000));
                open_device();
                render_start = next_render = g_now;
                renders = 0;
            }
        }
        if (started) {
            const int64_t due0 = clock.due(1, 0, g_now);
            if (last_due0 != 0) moved_ms += std::fabs(double(qpc_to_us(due0 - last_due0)) / 1000.0);
            if (last_due0 != 0 && due0 != last_due0) {
                const int dir = due0 > last_due0 ? 1 : -1;
                if (t >= kSettledSec) {
                    ++r.moves;
                    if (last_dir != 0 && dir != last_dir) ++r.reversals;
                    last_dir = dir;  // 落ち着く前の動き (開始直後の遅れなど) とは比べない
                }
            }
            last_due0 = due0;
        }
        if (t >= window_until) {
            if (window_slack_ms < 1e9) r.max_window_slack_ms = std::max(r.max_window_slack_ms, window_slack_ms);
            window_slack_ms = 1e9;
            window_until += 1;
        }
        // デバイス: 10 ms (デバイスの時計) ごとに描画する。これから書くバッファは kLeadMs 後に鳴る
        if (source && g_now >= next_render) {
            std::fill(mix.begin(), mix.end(), 0.0f);
            source->render(mix.data(), kDeviceFrames, g_now + qpc_from_ms(kLeadMs), base_delay);
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
                base_slips = done_slips + st.slips.load();
                base_resyncs = done_resyncs + st.resyncs.load();
            }
        }
    }
    if (!source || !started) {
        std::printf("FAIL %s: never started playing\n", c.name.c_str());
        return r;
    }
    auto& st = source->stats();
    r.slips = done_slips + st.slips.load() - base_slips;
    r.resyncs = done_resyncs + st.resyncs.load() - base_resyncs;
    r.moved_ms = moved_ms;
    {
        std::lock_guard lk(st.reason_mutex);
        r.last_resync = st.last_resync_reason;
    }
    r.ok = r.slips <= c.max_slips && r.resyncs <= c.max_resyncs && r.resyncs >= c.min_resyncs && r.moved_ms <= c.max_moved_ms &&
           r.moves <= c.max_moves && r.reversals <= c.max_reversals;
    std::printf(
        "%s %s: slips %llu (max %llu), resyncs %llu (max %llu), time base moved %.1f ms, %d moves after %.0f s (%d reversed), "
        "audio slack after that %.1f ms min / %.1f ms max of 1 s minimums%s%s\n",
        r.ok ? "OK  " : "FAIL", c.name.c_str(), (unsigned long long)r.slips, (unsigned long long)c.max_slips, (unsigned long long)r.resyncs,
        (unsigned long long)c.max_resyncs, r.moved_ms, r.moves, kSettledSec, r.reversals, r.min_slack_ms, r.max_window_slack_ms,
        r.last_resync.empty() ? "" : ", last resync: ", r.last_resync.c_str());
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
        // 設計で追うクロック差 (200 ppm) より大きい場合: 時間軸は 3 秒に 1 ms (333 ppm) までしか追わない
        // (毎秒動かすと engine の重複・間引きが 1 秒に 48 回になり、2 秒続くと合わせ直しになる)。追いきれない遅延の余りは
        // 3 フレームを超えたら一度に戻す (合わせ直し 1 回)
        {.name = "sender clock 400 ppm fast (a little faster than the time base follows)",
         .seconds = 60,
         .sender_ppm = 400,
         .max_slips = 1500,
         .max_resyncs = 0},
        {.name = "sender clock 1000 ppm fast (far faster than the time base follows)",
         .seconds = 120,
         .sender_ppm = 1000,
         .max_slips = 3000,
         .max_resyncs = 4,
         .min_resyncs = 1},
        // 届く時刻が揺れる (RTMP の到着、DVS が 10 ms 前後の単位で音を取る): 窓ごとの最小の余裕も揺れるが、
        // それで時間軸を一度に行き来させない
        // 落ち着いた後の揺れ (実機の DVS で窓ごとに ±2〜4 ms): 揺れに反応して時間軸を 1 ms ずつ行き来させない。
        // クロック差は、揺れがあっても合わせ直しなしで追う
        {.name = "arrivals jitter +-4 ms for 5 min",
         .seconds = 300,
         .jitter_ms = 4,
         .max_slips = 1000,
         .max_resyncs = 0,
         .max_moves = 5,
         .max_reversals = 0},
        // 開始直後に増えた揺れの備えは 30 秒に 1 ms ずつ減る。減った分 (5 分で約 9 ms) は、幅を超えてから 1 ms ずつ詰める
        {.name = "arrivals jitter +-4 ms for 5 min, late arrivals right after start",
         .seconds = 300,
         .late_ms = startup_late,
         .jitter_ms = 4,
         .max_slips = 1000,
         .max_resyncs = 4,
         .max_moves = 10,
         .max_reversals = 0},
        {.name = "arrivals jitter +-8 ms for 5 min",
         .seconds = 300,
         .jitter_ms = 8,
         .max_slips = 1000,
         .max_resyncs = 1,
         .max_moves = 5,
         .max_reversals = 0},
        {.name = "arrivals jitter +-4 ms, sender clock 150 ppm fast",
         .seconds = 300,
         .sender_ppm = 150,
         .jitter_ms = 4,
         .max_slips = 3000,
         .max_resyncs = 0,
         .max_reversals = 0},
        {.name = "arrivals jitter +-4 ms, sender clock 200 ppm fast",
         .seconds = 300,
         .sender_ppm = 200,
         .jitter_ms = 4,
         .max_slips = 4000,
         .max_resyncs = 0,
         .max_reversals = 0},
        {.name = "arrivals jitter +-4 ms, sender clock 200 ppm slow",
         .seconds = 300,
         .sender_ppm = -200,
         .jitter_ms = 4,
         .max_slips = 4000,
         .max_resyncs = 0,
         .max_reversals = 0},
        {.name = "arrivals jitter +-4 ms, device clock 200 ppm fast",
         .seconds = 300,
         .device_ppm = 200,
         .jitter_ms = 4,
         .max_slips = 4000,
         .max_resyncs = 0,
         .max_reversals = 0},
        {.name = "arrivals jitter +-5 ms", .seconds = 60, .jitter_ms = 5, .max_slips = 100, .max_resyncs = 2},
        {.name = "arrivals jitter +-5 ms, late arrivals right after start",
         .seconds = 60,
         .late_ms = startup_late,
         .jitter_ms = 5,
         .max_slips = 100,
         .max_resyncs = 4},
        // 出力遅延が変わる (WDM → ASIO の切り替えなど。開き直したあとの鳴らし始めも 1 回と数える)
        {.name = "output delay 32 -> 29 ms at 20 s",
         .seconds = 60,
         .jitter_ms = 5,
         .delay_changes = {{20, 29}},
         .max_slips = 100,
         .max_resyncs = 2},
        // 音声を「なし」にして戻す: なしの間は時間軸 (映像) を動かさない
        {.name = "output delay 32 -> none -> 32 ms (20 s / 30 s)",
         .seconds = 60,
         .jitter_ms = 5,
         .delay_changes = {{20, 0}, {30, 32}},
         .max_slips = 100,
         .max_resyncs = 2,
         .max_moved_ms = 40},
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
