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
    // 映像が来ていれば新しいフレームを最大 2 フレーム期間待ち、来なければ直前のフレームを出す。
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

    std::vector<uint8_t> slate_;  // 出力形式に変換済みのスレート
};

}  // namespace denpachan::vcam
