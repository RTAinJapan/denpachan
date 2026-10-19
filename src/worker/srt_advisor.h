#pragma once
// SRT の latency の推奨値を、受信状況 (RTT とパケットロス) から出す (設計書 §3.2)。
//
// 目安は Haivision の SRT Deployment Guide: latency ≥ RTT × 倍率。倍率はパケットロス率で決まる
// (〜1%: 3、〜3%: 4、〜7%: 6、〜10%: 8、それ以上: 10)。
// - RTT は直近の窓 (60 秒) の最大値を使う (揺れても足りるように)
// - 窓の中で取りこぼし (再送が間に合わずに捨てたパケット) があれば、今の latency では足りないので 1.5 倍以上にする
// - 下限 20 ms (送出側の送り方や処理の揺れの分)。10 ms 単位に切り上げる
// - 接続直後の 5 秒は使わない (接続時にロスがまとめて数えられることがある。MediaConnect で 1 秒目に 254)
// - 10 秒分の統計が溜まるまでは出さない (計測中)

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>

namespace denpachan::worker {

struct SrtAdvice {
    int latency_ms = 0;  // 0 = 計測中
    double rtt_ms = 0;   // 計算に使った RTT (窓の最大)
    double loss_pct = 0;
    int multiplier = 0;
    int64_t drops = 0;   // 窓の中の取りこぼし
    int window_sec = 0;
};

class SrtLatencyAdvisor {
public:
    static constexpr size_t kWindowSec = 60;
    static constexpr size_t kMinSamples = 10;
    static constexpr int kMinLatencyMs = 20;
    static constexpr int kWarmupSec = 5;

    void reset() {
        samples_.clear();
        warmup_ = kWarmupSec;
    }

    // 1 秒ごとの区間の統計 (srt_bstats の clear = 1 で取った値)
    void add(double rtt_ms, int64_t received, int64_t lost, int64_t dropped) {
        if (warmup_ > 0) {
            --warmup_;
            return;
        }
        samples_.push_back({rtt_ms, received, lost, dropped});
        while (samples_.size() > kWindowSec) samples_.pop_front();
    }

    // effective_latency_ms: いま実際に使われている latency (送出側との大きい方)
    SrtAdvice advice(int effective_latency_ms) const {
        SrtAdvice a;
        a.window_sec = int(samples_.size());
        int64_t received = 0;
        int64_t lost = 0;
        for (const auto& s : samples_) {
            a.rtt_ms = std::max(a.rtt_ms, s.rtt_ms);
            received += s.received;
            lost += s.lost;
            a.drops += s.dropped;
        }
        a.loss_pct = received + lost > 0 ? 100.0 * double(lost) / double(received + lost) : 0.0;
        if (samples_.size() < kMinSamples || received == 0) return a;

        a.multiplier = a.loss_pct <= 1.0 ? 3 : a.loss_pct <= 3.0 ? 4 : a.loss_pct <= 7.0 ? 6 : a.loss_pct <= 10.0 ? 8 : 10;
        double latency = std::max(a.rtt_ms * a.multiplier, double(kMinLatencyMs));
        if (a.drops > 0) latency = std::max(latency, effective_latency_ms * 1.5);
        a.latency_ms = int(std::ceil(latency / 10.0)) * 10;
        return a;
    }

private:
    struct Sample {
        double rtt_ms;
        int64_t received;
        int64_t lost;
        int64_t dropped;
    };
    std::deque<Sample> samples_;
    int warmup_ = kWarmupSec;
};

}  // namespace denpachan::worker
