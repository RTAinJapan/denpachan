// denpachan-engine: 常駐の本体 (設計書 §2)。
//
// - 設定 (%APPDATA%\denpachan\config.json) を読み、前回動いていたチャンネルの worker を起動・監視する
// - 各チャンネルの音声を設定どおりのデバイスに出す
// - 仮想カメラの共有メモリを保持する
// - UI からの操作を名前付きパイプ (\\.\pipe\denpachan) で受ける
//
//   denpachan-engine                  通常起動 (終了時に worker も止める)
//   denpachan-engine --keep-workers   終了しても worker を止めない (engine を入れ替えるとき)
//   denpachan-engine --list-devices

#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "audio_devices.h"
#include "config.h"
#include "denpachan/crash_dump.h"
#include "denpachan/win_util.h"
#include "engine.h"
#include "ipc_server.h"
#include "output_devices.h"
#include "render_log.h"
#include "ws_server.h"

#include <timeapi.h>  // windows.h の後に置く

using namespace denpachan;
using namespace denpachan::engine;

namespace {

struct Options {
    bool list_devices = false;
    bool keep_workers = false;
    std::wstring config_path;
    std::wstring log_file;
};

void print_usage() {
    std::fputs(
        "usage: denpachan-engine [options]\n"
        "  --config PATH      config file          [%APPDATA%\\denpachan\\config.json]\n"
        "  --log-file PATH    log file             [%LOCALAPPDATA%\\denpachan\\logs\\engine.log]\n"
        "  --keep-workers     leave workers running on exit\n"
        "  --list-devices     list audio output devices\n",
        stderr);
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
        } else if (a == L"--keep-workers") {
            o.keep_workers = true;
        } else if (a == L"--config") {
            const auto v = value();
            if (!v) return std::nullopt;
            o.config_path = *v;
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

void setup_logging(const std::wstring& log_file) {
    std::vector<spdlog::sink_ptr> sinks{std::make_shared<spdlog::sinks::stdout_color_sink_mt>()};
    sinks.push_back(std::make_shared<spdlog::sinks::rotating_file_sink_mt>(to_utf8(log_file), 10 * 1024 * 1024, 5));
    auto logger = std::make_shared<spdlog::logger>("engine", sinks.begin(), sinks.end());
    logger->set_pattern("%Y-%m-%d %H:%M:%S.%e [%n] %^%l%$ %v");
    logger->flush_on(spdlog::level::warn);
    spdlog::set_default_logger(logger);
    spdlog::flush_every(std::chrono::seconds(1));
    setup_render_log();  // 音声の描画スレッド用 (非同期。render_log.h)
}

std::wstring exe_dir() {
    wchar_t buf[MAX_PATH * 2] = {};
    const DWORD n = GetModuleFileNameW(nullptr, buf, DWORD(std::size(buf)));
    std::wstring path(buf, n);
    return path.substr(0, path.find_last_of(L"\\/"));
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
        for (const auto& d : list_output_devices()) {
            std::printf("%s\n    %d ch, %d Hz\n    %s\n", to_utf8(d.name).c_str(), d.channels, d.sample_rate, to_utf8(d.id).c_str());
        }
        CoUninitialize();
        return 0;
    }

    // 二重起動しない
    UniqueHandle instance(CreateMutexW(nullptr, TRUE, L"Local\\denpachan_engine"));
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        std::fputs("denpachan-engine is already running\n", stderr);
        return 1;
    }

    const std::wstring log_dir = local_app_dir(L"logs");
    setup_logging(opts->log_file.empty() ? log_dir + L"\\engine.log" : opts->log_file);
    install_crash_dump(local_app_dir(L"dumps"), L"engine");

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

    Engine engine(opts->config_path.empty() ? default_config_path() : opts->config_path, exe_dir() + L"\\denpachan-worker.exe", log_dir);
    spdlog::info("engine start");
    engine.init();

    IpcServer ipc([&](const std::string& method, const nlohmann::json& params) { return engine.handle(method, params); });
    ipc.start();

    // メトリクスの受信口 (WebSocket)。設定 (general.metrics) が変わったら開き直す
    WsServer ws([&](const std::string& method, const nlohmann::json& params) { return engine.handle_remote(method, params); },
                [&] { return engine.metrics_hello(); });
    MetricsConfig ws_config;
    bool ws_applied = false;
    int64_t ws_retry_qpc = 0;
    auto apply_metrics = [&] {
        MetricsConfig m = engine.metrics_config();
        m.allow_control = false;  // 操作を許すかは要求ごとに見るので、開き直さない
        const bool changed = !ws_applied || !(m == ws_config);
        if (!changed && (!m.enabled || ws.running() || qpc_now() < ws_retry_qpc)) return;
        if (changed) ws.stop();
        ws_config = m;
        ws_applied = true;
        if (!m.enabled) return;
        if (!is_loopback_bind(m.bind) && m.token.empty()) {
            if (changed) spdlog::error("metrics: a token is required to listen on {} (not a loopback address); not listening", m.bind);
            ws_retry_qpc = INT64_MAX;
            return;
        }
        // 待ち受けられなければ (ポートが使われているなど) 10 秒ごとにやり直す
        if (!ws.start({m.bind, m.port, m.token})) ws_retry_qpc = qpc_now() + qpc_from_ms(10'000);
    };

    // 200ms ごとに監視、1 秒ごとに状態を全クライアントへ通知
    int ticks = 0;
    bool keep_workers = opts->keep_workers;
    while (WaitForSingleObject(g_stop_event.get(), 200) == WAIT_TIMEOUT) {
        if (const int req = engine.shutdown_request(); req != 0) {
            keep_workers = req == 2;
            break;
        }
        engine.tick();
        apply_metrics();
        if (++ticks % 5 == 0) {
            auto [state, channels] = engine.state_with_channels();
            ipc.broadcast({{"event", "state"}, {"data", std::move(state)}});
            ws.broadcast(channels);
        }
    }

    spdlog::info("engine stopping{}", keep_workers ? " (workers keep running)" : "");
    ws.stop();
    ipc.stop();
    engine.shutdown(!keep_workers);
    timeEndPeriod(1);
    CoUninitialize();
    spdlog::info("engine stopped");
    spdlog::shutdown();  // 非同期のロガー (render_log) に溜まっている分を書き出す
    return 0;
}
