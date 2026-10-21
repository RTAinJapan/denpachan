#pragma once
// 共有メモリからフレームを読む側 (DirectShow フィルタのストリーミングスレッドから使う)。

#include <cstdint>
#include <vector>

#include "denpachan/vcam_shm.h"
#include "denpachan/win_util.h"
#include "formats.h"

namespace denpachan::vcam {

class FrameSource {
public:
    FrameSource(int cam, const OutputFormat& format);
    ~FrameSource();
    FrameSource(const FrameSource&) = delete;
    FrameSource& operator=(const FrameSource&) = delete;

    // 次に出すフレームを dst (format.image_bytes() 以上) に書く。
    // 映像が来ていれば新しいフレームを待ち、来なければ直前のフレームを出す。待つのは受信している映像の
    // フレーム間隔の 1.5 倍 (最低 2 フレーム期間、最大 250 ms)。
    // 途絶中・書き手が落ちている間も最後のフレームを出し続ける。
    // 一度もフレームが書かれていない (またはリセットされた) ときだけ NO SIGNAL スレートを出す。
    // 戻り値: 新しいフレームを書いたら true
    bool fill(uint8_t* dst);

    // fill の待ちを解除する (停止時に別スレッドから呼ぶ)
    void interrupt();

private:
    bool ensure_open();
    void ensure_reader_slot();
    void release_reader_slot();
    bool writer_live() const;
    bool has_frame() const;
    bool copy_latest(uint8_t* dst);
    void fill_slate(uint8_t* dst);
    void measure_interval(uint64_t prev_seq);
    DWORD live_wait_ms() const;

    static constexpr DWORD kMaxLiveWaitMs = 250;

    const int cam_;
    const OutputFormat format_;
    const DWORD frame_period_ms_;
    const uint64_t token_;

    SharedMapping mapping_;
    Header* header_ = nullptr;
    int64_t last_open_attempt_ = 0;

    int slot_ = -1;
    UniqueHandle frame_event_;
    UniqueHandle interrupt_event_;
    uint64_t last_seq_ = 0;
    bool first_fill_ = true;
    int64_t last_fresh_qpc_ = 0;      // 最後に新しいフレームを読んだ時刻
    int64_t source_interval_us_ = 0;  // 受信している映像のフレーム間隔 (移動平均、0 = まだ測っていない)

    std::vector<uint8_t> slate_;  // 出力形式に変換済みのスレート
};

}  // namespace denpachan::vcam
