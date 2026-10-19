// denpachan-rtmpd: RTMP の publish を 1 ポートで受け、ストリームキーでチャンネルに振り分ける (設計書 §3.3)。
// engine が起動・監視する。振り分け表は engine が共有メモリに書く。
//
//   denpachan-rtmpd --port 1935 [--bind 0.0.0.0] [--idle-timeout-ms 5000] [--log-file PATH]

#include "rtmp_server.h"  // winsock2.h を先に

#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <cstdio>
#include <optional>
#include <string>
#include <vector>

#include "denpachan/crash_dump.h"
#include "denpachan/worker_status.h"

using namespace denpachan;
using namespace denpachan::rtmpd;

namespace {

constexpr int kStatusSlot = 0;  // worker_status の 0 番を rtmpd 用に使う

struct Options {
    ServerOptions server;
    std::string log_file;
};

std::optional<Options> parse_args(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto value = [&]() -> std::optional<std::string> {
            if (i + 1 >= argc) return std::nullopt;
            return std::string(argv[++i]);
        };
        try {
            if (a == "--port") o.server.port = std::stoi(value().value());
            else if (a == "--bind") o.server.bind = value().value();
            else if (a == "--idle-timeout-ms") o.server.idle_timeout_ms = std::stoi(value().value());
            else if (a == "--log-file") o.log_file = value().value();
            else return std::nullopt;
        } catch (...) {
            return std::nullopt;
        }
    }
    if (o.server.port < 1 || o.server.port > 65535) return std::nullopt;
    return o;
}

UniqueHandle g_stop_event;

BOOL WINAPI on_console_ctrl(DWORD) {
    SetEvent(g_stop_event.get());
    return TRUE;
}

}  // namespace

int main(int argc, char** argv) {
    const auto opts = parse_args(argc, argv);
    if (!opts) {
        std::fputs("usage: denpachan-rtmpd --port N [--bind ADDR] [--idle-timeout-ms MS] [--log-file PATH]\n", stderr);
        return 2;
    }
    std::vector<spdlog::sink_ptr> sinks{std::make_shared<spdlog::sinks::stdout_color_sink_mt>()};
    if (!opts->log_file.empty()) {
        sinks.push_back(std::make_shared<spdlog::sinks::rotating_file_sink_mt>(opts->log_file, 10 * 1024 * 1024, 5));
    }
    auto logger = std::make_shared<spdlog::logger>("rtmpd", sinks.begin(), sinks.end());
    logger->set_pattern("%Y-%m-%d %H:%M:%S.%e [%n] %^%l%$ %v");
    logger->flush_on(spdlog::level::info);
    spdlog::set_default_logger(logger);
    install_crash_dump(local_app_dir(L"dumps"), L"rtmpd");

    SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);
    g_stop_event.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    SetConsoleCtrlHandler(on_console_ctrl, TRUE);
    SharedSecurity sec;
    UniqueHandle engine_stop(CreateEventW(sec.get(), TRUE, FALSE, worker_status::stop_event_name(kStatusSlot).c_str()));
    ResetEvent(engine_stop.get());

    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    // engine に見せる状態 (worker と同じ仕組みで監視される)
    SharedMapping status_mapping;
    worker_status::Header* status = worker_status::open_shared(status_mapping, kStatusSlot);
    if (status) {
        status->pid.store(GetCurrentProcessId());
        status->args_hash.store(worker_status::hash_args(GetCommandLineW()));
        status->started_qpc.store(qpc_now());
        status->ingest_heartbeat_qpc.store(qpc_now());
        status->playout_heartbeat_qpc.store(qpc_now());
        status->decoder_busy_since_qpc.store(0);
    }

    Server server(opts->server);
    if (!server.start()) {
        WSACleanup();
        return 1;  // engine が間隔を空けて再起動する
    }

    HANDLE handles[2] = {g_stop_event.get(), engine_stop.get()};
    int ticks = 0;
    while (WaitForMultipleObjects(2, handles, FALSE, 200) == WAIT_TIMEOUT) {
        if (status) {
            status->ingest_heartbeat_qpc.store(server.accept_heartbeat_qpc());
            status->playout_heartbeat_qpc.store(qpc_now());
        }
        if (++ticks % 5 == 0) server.housekeeping();
    }

    spdlog::info("rtmpd stopping");
    server.stop();
    if (status && status->pid.load() == GetCurrentProcessId()) status->pid.store(0);
    WSACleanup();
    return 0;
}
