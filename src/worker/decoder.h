#pragma once
// デマックス + 映像デコード。
//
// ByteQueue のセッション (接続) ごとに libavformat の入力を開き直す。
// デコードは D3D11VA を優先し、使えない / エラーが続く場合はソフトウェアに切り替える。
// デコード結果は CPU 上のフレームとして FrameQueue に渡す。

#include <atomic>
#include <string>
#include <thread>

#include "byte_queue.h"
#include "ffmpeg_util.h"
#include "frame_queue.h"
#include "metrics.h"

namespace denpachan::worker {

class Decoder {
public:
    Decoder(std::string input_format, ByteQueue& input, FrameQueue& output, ChannelMetrics& metrics, bool allow_hw);
    ~Decoder();

    void start();
    void stop();

private:
    void run();
    void run_session(uint64_t session);
    bool open_decoder(const AVStream* stream);
    void handle_frame(AVFrame* frame, const AVStream* stream, uint64_t session);

    static int read_packet(void* opaque, uint8_t* buf, int size);
    static int interrupt_cb(void* opaque);
    static AVPixelFormat get_format(AVCodecContext* ctx, const AVPixelFormat* formats);

    const std::string input_format_;
    ByteQueue& input_;
    FrameQueue& output_;
    ChannelMetrics& metrics_;

    bool hw_enabled_;
    BufferRefPtr hw_device_;
    CodecContextPtr codec_;
    int consecutive_errors_ = 0;

    uint64_t session_ = 0;  // read_packet が読むセッション
    int64_t last_pts_us_ = AV_NOPTS_VALUE;

    std::thread thread_;
    std::atomic<bool> stop_{false};
};

}  // namespace denpachan::worker
