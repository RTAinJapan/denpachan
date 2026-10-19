#pragma once
// vcam 共有メモリの書き手。プロトコルは denpachan/vcam_shm.h を参照。

#include <optional>

#include "denpachan/vcam_shm.h"
#include "denpachan/win_util.h"
#include "ffmpeg_util.h"
#include "scaler.h"

namespace denpachan::worker {

class VcamWriter {
public:
    VcamWriter() = default;
    ~VcamWriter();
    VcamWriter(const VcamWriter&) = delete;
    VcamWriter& operator=(const VcamWriter&) = delete;

    bool open(int cam);
    void close();
    bool is_open() const { return header_ != nullptr; }

    // 生存通知 (出力スレッドが 100ms 以内の間隔で呼ぶ)
    void heartbeat();
    void set_state(vcam::WriterState state);

    // 読み手 (OBS 等) が要求している解像度。読み手がいなければ nullopt (書き込みを省く)。
    std::optional<vcam::Format> requested_format() const;

    // フレームを要求解像度に変換して書き、読み手に通知する。読み手がいなければ何もしない。
    void write(const AVFrame* frame);

    // 映像の途絶中に呼ぶ。読み手がいない間に省いたフレームがあるか、読み手の要求解像度が
    // 変わっていれば、保持している最後のフレームを書き直す。
    void refresh(const AVFrame* held);

private:
    void notify_readers();

    bool skipped_ = false;
    vcam::Format written_format_{};

    int cam_ = 0;
    SharedMapping mapping_;
    vcam::Header* header_ = nullptr;
    Scaler scaler_;

    struct ReaderEvent {
        uint32_t generation = 0;
        UniqueHandle event;
    };
    ReaderEvent events_[vcam::kMaxReaders];
};

}  // namespace denpachan::worker
