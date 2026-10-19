#pragma once
// engine の中心: 設定・worker・音声出力をまとめ、IPC の要求を処理する。設計書 §2 / §10。

#include <nlohmann/json.hpp>

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>

#include "audio_router.h"
#include "config.h"
#include "denpachan/rtmp_routes.h"
#include "denpachan/vcam_shm.h"
#include "denpachan/win_util.h"
#include "worker_process.h"

namespace denpachan::engine {

class Engine {
public:
    Engine(std::wstring config_path, std::wstring worker_exe, std::wstring log_dir);
    ~Engine();

    // 設定を読み、前回動いていたチャンネルを再開する
    void init();
    // 定期処理 (worker の監視、見つからない音声デバイスの探し直し)。200ms ごとに呼ぶ。
    void tick();
    // IPC の要求を処理する。エラーは例外で返す。
    nlohmann::json handle(const std::string& method, const nlohmann::json& params);
    nlohmann::json state();
    // stop_workers = false なら worker は動かしたまま終わる (engine を入れ替えるとき用)
    void shutdown(bool stop_workers);
    // IPC で終了を求められたか (0: なし / 1: worker も止める / 2: worker は残す)
    int shutdown_request() const { return shutdown_request_; }

private:
    std::wstring worker_args(const ChannelConfig& ch) const;
    void apply_channel_locked(int ch);
    // RTMP の振り分け表を書き、push のチャンネルがあれば rtmpd を動かす
    void apply_rtmp_locked();
    void save_locked();
    nlohmann::json state_locked();
    // FFmpeg (同梱しない) の有無を確かめ直す。見つかったら止めていたチャンネルを起動する
    void check_ffmpeg_locked();

    std::mutex mutex_;
    const std::wstring config_path_;
    const std::wstring worker_exe_;
    const std::wstring log_dir_;
    Config config_;
    std::array<std::unique_ptr<WorkerProcess>, kChannelCount> workers_;
    std::unique_ptr<WorkerProcess> rtmpd_;
    SharedMapping routes_mapping_;
    rtmp_routes::Header* routes_ = nullptr;
    AudioRouter audio_;
    SharedMapping vcam_mappings_[vcam::kNumCams];
    vcam::Header* vcam_headers_[vcam::kNumCams] = {};
    int64_t last_audio_retry_qpc_ = 0;
    std::atomic<int> shutdown_request_{0};
    bool ffmpeg_ok_ = false;
    std::string ffmpeg_message_;  // 見つからないときの説明
    int64_t last_ffmpeg_check_qpc_ = 0;
};

// FFmpeg の DLL を使えるか (音声のリサンプルが確かめてから使う)
bool ffmpeg_available();

}  // namespace denpachan::engine
