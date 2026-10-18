#pragma once
// プレビュー画像を共有メモリに書く (設計書 §7.1)。
//
// 出力スレッドからフレームを受け取り (参照を取るだけ)、縮小と書き込みは別スレッドで行う
// (仮想カメラ出力の遅延に影響させない)。UI が見ていない間は何もしない。最大 15fps。

#include <condition_variable>
#include <mutex>
#include <thread>

#include "denpachan/preview_shm.h"
#include "denpachan/win_util.h"
#include "ffmpeg_util.h"

namespace denpachan::worker {

class PreviewWriter {
public:
    PreviewWriter() = default;
    ~PreviewWriter();
    PreviewWriter(const PreviewWriter&) = delete;
    PreviewWriter& operator=(const PreviewWriter&) = delete;

    bool open(int ch);
    void close();

    // 出力したフレーム (出力スレッドから呼ぶ。UI が見ていなければすぐ戻る)
    void submit(const AVFrame* frame);
    // 映像の途絶中に呼ぶ: UI が後から開いたときのために、保持中のフレームを書く
    void refresh(const AVFrame* held);

private:
    bool reader_active() const;
    void run();
    bool convert(const AVFrame* src, uint8_t* dst);

    SharedMapping mapping_;
    preview::Header* header_ = nullptr;

    std::mutex mutex_;
    std::condition_variable cv_;
    FramePtr pending_;
    bool stop_ = false;
    int64_t last_submit_qpc_ = 0;
    std::thread thread_;

    SwsContext* sws_ = nullptr;
};

}  // namespace denpachan::worker
