#pragma once
// メトリクスの受信口 (WebSocket) で 1 秒ごとに送る channels を作る。docs/stream-test-automation.md §3.2。
//
// engine の状態 (Engine::state_locked) を、bot が使いやすい安定した形にまとめる:
// - status は画面のタイルとほぼ同じ区分 (stopped / starting / listening / connecting / waiting / live / hold /
//   unreachable / restarting / unavailable)。違いは 2 つ: worker が動いていてまだメトリクスが届かない間は starting、
//   listener (SRT listener、RTMP push) が繋がって映像がまだのときは waiting (画面は「待ち受け中」のまま)。
//   bot は「繋がったか」を知りたいので、画面より細かく分ける
// - latency.verdict は画面の経路の行の色と同じ規則 (直近 60 秒に取りこぼし = insufficient、推奨値に足りない = tight)
// - connectedSec (繋がってからの秒数) と audio.silentSec (両チャンネルとも -60 dBFS 未満が続いた秒数) は、
//   ここで数える (build を 1 秒ごとに呼ぶ前提。呼ぶ間隔が揺れても QPC の差で数える)

#include <nlohmann/json.hpp>

#include <array>
#include <cstdint>
#include <deque>
#include <utility>

#include "config.h"

namespace denpachan::engine {

class ChannelsView {
public:
    // state: Engine::state_locked() の形。now_qpc: qpc_now()
    nlohmann::json build(const nlohmann::json& state, const Config& config, int64_t now_qpc);

    static constexpr double kSilentDb = -60.0;
    static constexpr int64_t kWindowMs = 60'000;  // 取りこぼし・再送を見る窓 (画面と同じ)

private:
    // 累積カウンタの、直近 kWindowMs の増え分 (画面の RateTracker と同じ。カウンタが戻ったら数え直す)
    class Window {
    public:
        double add(double cumulative, int64_t now_qpc);
        void reset() { samples_.clear(); }

    private:
        std::deque<std::pair<int64_t, double>> samples_;
    };

    struct Watch {
        int64_t connected_since_qpc = 0;
        int64_t silent_since_qpc = 0;
        Window dropped;
        Window retrans;
    };
    std::array<Watch, kMaxChannels> watch_;
};

// UTC の今の時刻 (ISO 8601、ミリ秒まで。例: 2026-11-01T12:34:56.789Z)
std::string utc_now_iso8601();

}  // namespace denpachan::engine
