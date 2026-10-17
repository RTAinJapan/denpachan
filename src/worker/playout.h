#pragma once
// Playout Clock: デコード済みフレームをタイムスタンプどおりの時刻に vcam へ出す (設計書 §3.5)。
//
// - 接続ごと / タイムスタンプの飛びで、最初のフレームの到着時刻 + 再生バッファに時間軸を合わせる。
// - 到着から出力までの余裕 (slack) の最小値を 1 秒ごとに見て、再生バッファの値に寄せる。
//   ずれが大きければ一度に合わせ (起動直後のバースト等)、小さければ 1 ms ずつ動かす (カクつかせない)。
//   これで再接続や長時間運用でも遅延が設定値から動かない。
// - 映像が途絶えても最後のフレームを出し続ける (NO SIGNAL にはしない)。stall_ms 来なければ状態を Hold にする。

#include <atomic>
#include <thread>

#include "denpachan/win_util.h"
#include "frame_queue.h"
#include "metrics.h"
#include "playout_clock.h"
#include "vcam_writer.h"

namespace denpachan::worker {

struct PlayoutOptions {
    int buffer_ms = 0;  // RTA 向けに既定は 0 (デコードしたら即出す)。カクつくなら 1 フレーム分程度に上げる
    int stall_ms = 2000;  // 映像が来なくなってから「途絶」とみなすまで (状態表示用。表示は最後のフレームのまま)
};

class Playout {
public:
    Playout(PlayoutOptions options, PlayoutClock& clock, FrameQueue& queue, VcamWriter* writer,
            ChannelMetrics& metrics);
    ~Playout();

    void start();
    void stop();

private:
    void run();
    bool wait_until(int64_t target_qpc);

    const PlayoutOptions options_;
    PlayoutClock& clock_;
    FrameQueue& queue_;
    VcamWriter* writer_;  // nullptr = vcam 出力なし
    ChannelMetrics& metrics_;

    UniqueHandle timer_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
};

}  // namespace denpachan::worker
