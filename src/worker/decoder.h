#pragma once
// デマックス + 映像・音声デコード。
//
// ByteQueue のセッション (接続) ごとに libavformat の入力を開き直す。
// 映像は D3D11VA を優先し、使えない / エラーが続く場合はソフトウェアに切り替える。
// デコード結果は CPU 上のフレームとして FrameQueue に渡す。
// 音声は 48kHz / float32 / ステレオに変換して AudioWriter に渡す。

#include <atomic>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "audio_writer.h"
#include "byte_queue.h"
#include "ffmpeg_util.h"
#include "frame_queue.h"
#include "metrics.h"
#include "recorder.h"

extern "C" {
#include <libswresample/swresample.h>
}

namespace denpachan::worker {

class Decoder {
public:
    Decoder(std::string input_format, ByteQueue& input, FrameQueue& output, AudioWriter* audio, ChannelMetrics& metrics, bool allow_hw);
    ~Decoder();

    // URL (rtmp:// など) を直接読む。切れたら間隔を空けて繋ぎ直す。start() の前に呼ぶ。
    void set_url(std::string url);
    // 録画に受信パケット / デコード結果を渡す。start() の前に呼ぶ。
    void set_recorder(Recorder* recorder) { recorder_ = recorder; }
    // 接続は生きているのにストリームを読み始められなかったとき (FLV は途中からでは読めない) に、
    // 受信側に区切り直し (次のキーフレームから新しいセッション) を頼む。start() の前に呼ぶ。
    void set_resync(std::function<void()> resync) { resync_ = std::move(resync); }
    // 試験用: セッションが始まってからこの秒数後に、受信したパケットのタイムスタンプを kTestRewindSec 秒戻す
    // (同じ接続のまま送出側が再起動した、MPEG-TS の折り返しなどの代わり)。音声は kTestRewindAudioLagMs 遅れて戻す。
    // start() の前に呼ぶ。
    void set_test_ts_rewind(int after_sec) { test_ts_rewind_after_sec_ = after_sec; }
    // 試験用: セッションが始まってからこの秒数後から kTestDropVideoMs の間、受信した映像のパケットを捨てる
    // (映像だけが途切れて音声は届き続ける送出元の代わり)。start() の前に呼ぶ。
    void set_test_drop_video(int after_sec) { test_drop_video_after_sec_ = after_sec; }

    void start();
    void stop();

private:
    void run();
    void run_url_loop();
    bool run_session(uint64_t session);  // false = 読み始められずに終わった
    bool open_decoder(const AVStream* stream);
    void handle_frame(AVFrame* frame, const AVStream* stream, uint64_t session);
    bool open_audio_decoder(const AVStream* stream);
    void decode_audio(const AVPacket* pkt, const AVStream* stream, uint64_t session);
    void handle_audio_frame(const AVFrame* frame, const AVStream* stream, uint64_t session);

    static int read_packet(void* opaque, uint8_t* buf, int size);
    static int interrupt_cb(void* opaque);
    static AVPixelFormat get_format(AVCodecContext* ctx, const AVPixelFormat* formats);

    const std::string input_format_;
    ByteQueue& input_;
    FrameQueue& output_;
    AudioWriter* audio_;  // nullptr = 音声を出さない
    Recorder* recorder_ = nullptr;
    ChannelMetrics& metrics_;
    std::function<void()> resync_;

    CodecContextPtr audio_codec_;
    FramePtr audio_frame_;
    SwrContext* swr_ = nullptr;
    AVChannelLayout swr_in_layout_{};
    int swr_in_format_ = -1;
    int swr_in_rate_ = 0;
    std::vector<float> audio_buf_;

    const bool allow_hw_;
    bool hw_enabled_;  // このセッションで HW デコードを使うか (キーフレームの失敗が続いたらやめる)
    BufferRefPtr hw_device_;
    CodecContextPtr codec_;
    int consecutive_errors_ = 0;  // キーフレームのデコードが続けて失敗した回数 (成功で 0)

    uint64_t session_ = 0;  // read_packet が読むセッション

    std::string url_;          // 空 = ByteQueue から読む
    std::string display_url_;  // ストリームキーを伏せたもの (ログ・メトリクス用)
    std::atomic<int64_t> io_deadline_qpc_{0};
    std::atomic<bool> io_timed_out_{false};  // interrupt_cb が時間切れで打ち切った (繋がらない理由の表示用)
    int64_t last_pts_us_ = AV_NOPTS_VALUE;

    // HW デコードの結果を取り出す先のバッファのプール (transfer_frame)
    FramePtr transfer_frame(const AVFrame* hw);
    AVBufferPool* transfer_pool_ = nullptr;
    AVPixelFormat pool_format_ = AV_PIX_FMT_NONE;
    int pool_width_ = 0;
    int pool_height_ = 0;
    static constexpr int kTransferAlign = 64;
    int test_ts_rewind_after_sec_ = 0;
    static constexpr int64_t kTestRewindSec = 8;
    static constexpr int64_t kTestRewindAudioLagMs = 300;
    int test_drop_video_after_sec_ = 0;
    static constexpr int64_t kTestDropVideoMs = 3000;

    std::thread thread_;
    std::atomic<bool> stop_{false};
};

}  // namespace denpachan::worker
