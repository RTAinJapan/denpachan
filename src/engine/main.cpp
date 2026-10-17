// denpachan-engine: 常駐の本体 (設計書 §2)。
//
// M2 の時点では次だけを行う。ワーカーの起動・監視、IPC、設定ファイルは M3 で追加する。
// - 音声出力: 各チャンネルの音声リングを、指定したデバイスの指定チャンネルに出す
// - 仮想カメラの共有メモリを常に開いておく (誰も開いていないと最後のフレームが消えるため)
//
//   denpachan-engine --list-devices
//   denpachan-engine --out "ch=1,dev=Dante Virtual Soundcard,start=1,mode=exclusive" --out "ch=2,dev=Dante,start=3"

#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "audio_devices.h"
#include "denpachan/vcam_shm.h"
#include "denpachan/win_util.h"
#include "device_output.h"

#include <timeapi.h>  // windows.h の後に置く

using namespace denpachan;
using namespace denpachan::engine;

namespace {

struct OutSpec {
    RouteOptions route;
    std::wstring device_query;
    std::optional<bool> exclusive;
};

struct Options {
    bool list_devices = false;
    std::vector<OutSpec> outs;
    int margin_ms = 10;
    std::wstring log_file;
};

void print_usage() {
    std::fputs(
        "usage: denpachan-engine [options]\n"
        "  --list-devices        list audio output devices\n"
        "  --out SPEC            route a channel's audio to a device (repeatable)\n"
        "                        SPEC = ch=N,dev=NAME|ID[,start=N][,mode=shared|exclusive][,offset=MS][,fade=0|1][,mute=0|1]\n"
        "                          ch     channel 1-4\n"
        "                          dev    device id, or part of its name\n"
        "                          start  first device channel for L (R = start+1)  [1]\n"
        "                          mode   WASAPI share mode (per device)           [shared]\n"
        "                          offset added audio delay in ms (may be negative) [0]\n"
        "                          fade   10ms fade at start / dropout / resync / mute toggle [1]\n"
        "                          mute   start muted                               [0]\n"
        "  --margin MS           audio margin added to the device buffer          [10]\n"
        "  --log-file PATH       also log to a rotating file\n",
        stderr);
}

std::optional<int> to_int(const std::wstring& s) {
    try {
        size_t pos = 0;
        const int v = std::stoi(s, &pos);
        if (pos != s.size()) return std::nullopt;
        return v;
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<OutSpec> parse_out(const std::wstring& spec) {
    OutSpec o;
    size_t begin = 0;
    while (begin <= spec.size()) {
        size_t end = spec.find(L',', begin);
        if (end == std::wstring::npos) end = spec.size();
        const std::wstring item = spec.substr(begin, end - begin);
        begin = end + 1;
        if (item.empty()) continue;
        const size_t eq = item.find(L'=');
        if (eq == std::wstring::npos) return std::nullopt;
        const std::wstring key = item.substr(0, eq);
        const std::wstring value = item.substr(eq + 1);
        if (key == L"ch") {
            const auto v = to_int(value);
            if (!v || *v < 1 || *v > audio::kNumChannels) return std::nullopt;
            o.route.ch = *v;
        } else if (key == L"dev") {
            o.device_query = value;
        } else if (key == L"start") {
            const auto v = to_int(value);
            if (!v || *v < 1) return std::nullopt;
            o.route.start_channel = *v;
        } else if (key == L"mode") {
            if (value != L"shared" && value != L"exclusive") return std::nullopt;
            o.exclusive = value == L"exclusive";
        } else if (key == L"offset") {
            const auto v = to_int(value);
            if (!v) return std::nullopt;
            o.route.offset_ms = *v;
        } else if (key == L"fade") {
            o.route.fade = value != L"0";
        } else if (key == L"mute") {
            o.route.mute = value != L"0";
        } else {
            return std::nullopt;
        }
    }
    if (o.device_query.empty()) return std::nullopt;
    return o;
}

std::optional<Options> parse_args(int argc, wchar_t** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        auto value = [&]() -> std::optional<std::wstring> {
            if (i + 1 >= argc) return std::nullopt;
            return std::wstring(argv[++i]);
        };
        if (a == L"--list-devices") {
            o.list_devices = true;
        } else if (a == L"--out") {
            const auto v = value();
            const auto spec = v ? parse_out(*v) : std::nullopt;
            if (!spec) {
                std::fprintf(stderr, "invalid --out: %s\n", v ? to_utf8(*v).c_str() : "");
                return std::nullopt;
            }
            o.outs.push_back(*spec);
        } else if (a == L"--margin") {
            const auto v = value();
            const auto n = v ? to_int(*v) : std::nullopt;
            if (!n || *n < 0) return std::nullopt;
            o.margin_ms = *n;
        } else if (a == L"--log-file") {
            const auto v = value();
            if (!v) return std::nullopt;
            o.log_file = *v;
        } else {
            std::fprintf(stderr, "invalid argument: %s\n", to_utf8(a).c_str());
            return std::nullopt;
        }
    }
    return o;
}

UniqueHandle g_stop_event;

BOOL WINAPI on_console_ctrl(DWORD) {
    SetEvent(g_stop_event.get());
    return TRUE;
}

void setup_logging(const Options& o) {
    std::vector<spdlog::sink_ptr> sinks{std::make_shared<spdlog::sinks::stdout_color_sink_mt>()};
    if (!o.log_file.empty()) {
        sinks.push_back(std::make_shared<spdlog::sinks::rotating_file_sink_mt>(to_utf8(o.log_file), 10 * 1024 * 1024, 5));
    }
    auto logger = std::make_shared<spdlog::logger>("engine", sinks.begin(), sinks.end());
    logger->set_pattern("%Y-%m-%d %H:%M:%S.%e [%n] %^%l%$ %v");
    logger->flush_on(spdlog::level::warn);
    spdlog::set_default_logger(logger);
    spdlog::flush_every(std::chrono::seconds(1));
}

double dbfs(float v) { return v > 1e-5f ? 20.0 * std::log10(v) : -100.0; }

void report(std::vector<std::unique_ptr<DeviceOutput>>& outputs) {
    for (auto& out : outputs) {
        const DeviceStatus st = out->status();
        std::string line = "[" + to_utf8(out->options().name) + "] ";
        if (!st.open) {
            line += "closed (retrying)";
        } else {
            line += fmt::format("{} {} Hz {} ch {} buf {:.2f} ms calls {}/s max gap {:.1f} ms",
                                st.exclusive ? "exclusive" : "shared", st.sample_rate, st.channels, st.sample_format,
                                st.buffer_ms, out->take_render_count(), double(out->take_max_gap_us()) / 1000);
        }
        out->for_each_source([&](ChannelSource& s) {
            auto& stats = s.stats();
            const auto& r = s.options();
            line += fmt::format(" | ch{}->{}-{} ", r.ch, r.start_channel, r.start_channel + 1);
            if (!stats.playing) {
                line += "idle";
            } else {
                line += fmt::format("latency {:.1f} ms err {:+.2f} ms ", stats.latency_ms.load(), stats.error_ms.load());
                line += stats.passthrough ? fmt::format("slips {}", stats.slips.load())
                                          : fmt::format("ratio {:+.0f} ppm", stats.ratio_ppm.load());
            }
            line += fmt::format(" underruns {} resyncs {} peak {:.1f}/{:.1f} dBFS", stats.underruns.load(),
                                stats.resyncs.load(), dbfs(stats.peak_l.exchange(0)), dbfs(stats.peak_r.exchange(0)));
            std::lock_guard lk(stats.reason_mutex);
            if (!stats.last_resync_reason.empty()) {
                line += " last resync: " + stats.last_resync_reason;
                stats.last_resync_reason.clear();
            }
        });
        spdlog::info("{}", line);
    }
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    const auto opts = parse_args(argc, argv);
    if (!opts) {
        print_usage();
        return 2;
    }
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    if (opts->list_devices) {
        for (const auto& d : list_audio_devices(eRender)) {
            std::printf("%s\n    %d ch, %d Hz\n    %s\n", to_utf8(d.name).c_str(), d.channels, d.sample_rate,
                        to_utf8(d.id).c_str());
        }
        CoUninitialize();
        return 0;
    }

    setup_logging(*opts);
    SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);
    SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED);
    PROCESS_POWER_THROTTLING_STATE throttling{};
    throttling.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    // EcoQoS を使わない / タイマー分解能の要求を無視させない
    throttling.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
    throttling.StateMask = 0;
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &throttling, sizeof(throttling));
    // タイマー分解能を 1ms にする。仮想オーディオデバイス (VB-Cable、DVS など) は OS のタイマーで
    // 周期を作っていることがあり、既定の 15.6ms では短い周期の排他モードに追いつけない。
    timeBeginPeriod(1);

    g_stop_event.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    SetConsoleCtrlHandler(on_console_ctrl, TRUE);

    // 仮想カメラの共有メモリを保持する
    SharedMapping vcams[vcam::kNumCams];
    for (int cam = 1; cam <= vcam::kNumCams; ++cam) {
        if (!vcam::open_shared(vcams[cam - 1], cam)) spdlog::warn("vcam {}: cannot open shared memory", cam);
    }

    // デバイスごとにまとめる
    struct DevicePlan {
        DeviceOptions options;
        std::vector<RouteOptions> routes;
    };
    std::map<std::wstring, DevicePlan> plans;
    for (const auto& o : opts->outs) {
        const auto dev = find_audio_device(eRender, o.device_query);
        if (!dev) {
            spdlog::error("audio device not found: {}", to_utf8(o.device_query));
            return 1;
        }
        auto& plan = plans[dev->id];
        if (plan.options.id.empty()) {
            plan.options.id = dev->id;
            plan.options.name = dev->name;
            plan.options.margin_ms = opts->margin_ms;
            plan.options.exclusive = o.exclusive.value_or(false);
        } else if (o.exclusive && *o.exclusive != plan.options.exclusive) {
            spdlog::warn("[{}]: conflicting mode for ch{}, using {}", to_utf8(dev->name), o.route.ch,
                         plan.options.exclusive ? "exclusive" : "shared");
        }
        for (const auto& r : plan.routes) {
            const bool overlap = o.route.start_channel <= r.start_channel + 1 && r.start_channel <= o.route.start_channel + 1;
            if (overlap) {
                spdlog::warn("[{}]: ch{} and ch{} overlap on output channels", to_utf8(dev->name), r.ch, o.route.ch);
            }
        }
        plan.routes.push_back(o.route);
    }

    std::vector<std::unique_ptr<DeviceOutput>> outputs;
    for (auto& [id, plan] : plans) {
        outputs.push_back(std::make_unique<DeviceOutput>(plan.options, plan.routes));
        outputs.back()->start();
    }
    spdlog::info("engine start: {} audio device(s)", outputs.size());

    while (WaitForSingleObject(g_stop_event.get(), 1000) == WAIT_TIMEOUT) report(outputs);

    spdlog::info("engine stopping");
    for (auto& out : outputs) out->stop();
    outputs.clear();
    timeEndPeriod(1);
    CoUninitialize();
    spdlog::info("engine stopped");
    return 0;
}
