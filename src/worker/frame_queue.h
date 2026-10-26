#pragma once
// デコードスレッド → 出力スレッドのフレームキュー。溢れたら古いものから捨てる (basic_frame_queue.h)。

#include <cstdint>

#include "basic_frame_queue.h"
#include "ffmpeg_util.h"

namespace denpachan::worker {

struct DecodedFrame {
    FramePtr frame;           // CPU 上のフレーム (NV12 / P010 / yuv420p など)
    int64_t pts_us = 0;       // メディア時刻 (マイクロ秒)
    int64_t arrival_qpc = 0;  // デコード完了時刻
    uint64_t session = 0;     // 接続ごとの番号 (変わったら時間軸を取り直す)
};

using FrameQueue = BasicFrameQueue<DecodedFrame>;

}  // namespace denpachan::worker
