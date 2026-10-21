#include "render_log.h"

#include <spdlog/async.h>

#include <memory>

namespace denpachan::engine {

namespace {

std::shared_ptr<spdlog::logger> g_render_log;

}  // namespace

void setup_render_log() {
    auto base = spdlog::default_logger();
    spdlog::init_thread_pool(1024, 1);
    auto logger = std::make_shared<spdlog::async_logger>(base->name(), base->sinks().begin(), base->sinks().end(), spdlog::thread_pool(),
                                                         spdlog::async_overflow_policy::overrun_oldest);
    logger->set_pattern("%Y-%m-%d %H:%M:%S.%e [%n] %^%l%$ %v");
    logger->set_level(base->level());
    // 警告以上はすぐディスクに書き出す (engine が落ちる直前の ASIO ドライバのエラーなどを残すため)。
    // 非同期のロガーでは書き出しも別のスレッドで行うので、描画スレッドは待たない
    logger->flush_on(spdlog::level::warn);
    g_render_log = std::move(logger);
}

spdlog::logger& render_log() { return g_render_log ? *g_render_log : *spdlog::default_logger_raw(); }

}  // namespace denpachan::engine
