#pragma once
// 録画 (設計書 §8)。
//
// - 受信・出力とは別のスレッドで動く。デコーダからは参照を渡すだけで、キューが溢れたら録画側で捨てる
//   (仮想カメラと音声出力は止めない)。パススルーで捨てたときは次のキーフレームから書き直す。
// - パススルー: 受信したパケットをそのまま書く。
//   再エンコード (GPU): デコード済みのフレームを NVENC / QSV / AMF で書く。
//   再エンコード (CPU): 受信したパケットを外部の ffmpeg に流し、x264 / x265 / SVT-AV1 で書かせる (external_encoder.h)。
// - 区切り: 映像が gap_timeout_sec 来なかったらファイルを閉じ、次に映像が来たら新しいファイルにする。
//   それより短い再接続で、コーデックと解像度が同じなら同じファイルに続けて書く (タイムスタンプをつなぐ)。
//   最大長 (max_segment_min) に達したときと、パラメータが変わったときも区切る。
// - 空き容量が min_free_gb を切ったら録画を止める。
// - 設定は動いている間に変えられる (configure)。録画のオン・オフで受信は止めない。

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "external_encoder.h"
#include "ffmpeg_util.h"
#include "metrics.h"

extern "C" {
#include <libavutil/audio_fifo.h>
#include <libswresample/swresample.h>
}

namespace denpachan::worker {

struct RecordConfig {
    bool enabled = false;
    std::string dir;
    std::string tmpl = "{ch} - {YYYY}{MM}/{DD} {hh}-{mm}-{ss}";
    std::string container = "mkv";     // mkv / ts
    std::string mode = "passthrough";  // passthrough / encode
    int gap_timeout_sec = 10;
    int max_segment_min = 0;        // 0 = 無制限
    std::string replacement = "-";  // ファイル名に使えない文字の置き換え
    int min_free_gb = 5;
    std::string name;  // {name} (チャンネル名)
    // 再エンコード
    std::string video_codec = "h264";    // h264 / hevc / av1
    std::string video_encoder = "auto";  // auto / nvenc / qsv / amf (GPU)、cpu (外部 ffmpeg)
    std::string rate_control = "cbr";    // cbr / vbr / cqp
    int bitrate_kbps = 12000;
    int qp = 23;
    std::string preset = "p4";
    int gop_sec = 2;
    int audio_bitrate_kbps = 192;

    bool operator==(const RecordConfig&) const = default;
};

// JSON (engine の設定ファイルの record と同じ形) から読む。壊れていれば無効 (enabled = false)。
RecordConfig parse_record_config(const std::string& json, int ch);

// 外部 ffmpeg で再エンコードするか
inline bool uses_external_encoder(const RecordConfig& c) { return c.mode == "encode" && c.video_encoder == "cpu"; }
// 受信したパケットを使うか (パススルーと外部 ffmpeg)。そうでなければデコード済みのフレームを使う
inline bool uses_packets(const RecordConfig& c) { return c.mode != "encode" || uses_external_encoder(c); }

class Recorder {
public:
    Recorder(int ch, ChannelMetrics& metrics);
    ~Recorder();
    Recorder(const Recorder&) = delete;
    Recorder& operator=(const Recorder&) = delete;

    void start();
    void stop();

    void configure(const RecordConfig& config);

    // ---- デコーダのスレッドから呼ぶ (ブロックしない) ----
    bool wants_packets() const { return enabled_ && packets_; }
    bool wants_frames() const { return enabled_ && !packets_; }
    void begin_session(uint64_t session, const AVStream* video, const AVStream* audio);
    void video_packet(const AVPacket* pkt);
    void audio_packet(const AVPacket* pkt);
    void video_frame(const AVFrame* frame, int64_t pts_us);
    void audio_samples(const float* data, int frames, int64_t pts_us);  // 48kHz / float / ステレオ

private:
    struct StreamInfo;
    struct Item;
    struct Segment;

    void push(std::unique_ptr<Item> item, bool media);
    void run();
    void process(Item& item);
    void housekeeping();

    bool open_segment(const AVFrame* first_frame);
    bool try_open_segment(const AVFrame* first_frame);
    void close_segment(const char* reason);
    bool open_encoders(Segment& seg, const AVFrame* frame);
    bool open_external(Segment& seg);
    void add_copy_streams(Segment& seg);
    void reap_external();
    bool write_packet(int stream_index, AVPacket* pkt, AVRational src_tb);  // 書いたら true
    bool video_ts_jumped(int64_t in_us);
    void encode_video(const AVFrame* frame, int64_t out_us);
    void encode_audio(const float* data, int frames, int64_t out_us);
    void drain_encoder(AVCodecContext* ctx, int stream_index);
    int64_t remap_us(int64_t in_us, bool video);
    int64_t frame_us() const;
    bool same_params(const StreamInfo& a, const StreamInfo& b) const;
    void set_error(const std::string& message);

    const int ch_;
    ChannelMetrics& metrics_;

    std::atomic<bool> enabled_{false};
    std::atomic<bool> packets_{true};         // uses_packets(設定)
    std::atomic<bool> need_keyframe_{false};  // キューから捨てたので次のキーフレームまで待つ

    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::unique_ptr<Item>> queue_;
    size_t queued_bytes_ = 0;
    bool stop_ = false;
    bool thread_done_ = false;
    std::condition_variable done_cv_;
    std::thread thread_;

    // ---- 録画スレッドだけが触る ----
    RecordConfig config_;
    std::shared_ptr<StreamInfo> info_;          // 現在のセッションのストリーム情報
    std::shared_ptr<StreamInfo> segment_info_;  // 開いているファイルのストリーム情報
    std::unique_ptr<Segment> segment_;
    bool continuation_pending_ = false;  // 新しいセッションの最初の映像でタイムスタンプをつなぐ
    int64_t offset_us_ = 0;
    int64_t last_video_out_us_ = 0;  // 書いた映像の最後の時刻 (パススルーは dts)
    int64_t last_video_qpc_ = 0;     // 最後に映像を書いた時刻 (途切れでファイルを閉じる判定)
    // 受信した映像の直前のタイムスタンプと、それを処理した時刻 (同じ接続のままタイムスタンプが戻った・飛んだのを見つける)
    bool have_last_in_video_ = false;
    int64_t last_in_video_us_ = 0;
    int64_t last_in_video_qpc_ = 0;
    int64_t last_disk_check_qpc_ = 0;
    bool disk_full_ = false;
    int64_t retry_open_qpc_ = 0;  // ファイルやエンコーダを開けなかったとき、次に試す時刻 (毎フレーム試さない)
    int open_failures_ = 0;       // 続けて開けなかった回数 (間隔を伸ばす。設定・接続が変われば 0 に)
    void schedule_retry(int64_t now);
    int64_t last_size_check_qpc_ = 0;
    uint64_t seq_ = 0;
    std::vector<std::unique_ptr<ExternalEncoder>> finishing_;  // 入力を閉じて書き終わりを待っている ffmpeg
};

}  // namespace denpachan::worker
