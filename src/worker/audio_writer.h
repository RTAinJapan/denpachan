#pragma once
// 音声リング (共有メモリ) の書き手。プロトコルは denpachan/audio_shm.h を参照。
//
// デコード済み音声 (48kHz / float32 / ステレオ) を書き、チャンクごとに「このフレームを鳴らすべき時刻」を書く。
//
// 鳴らす時刻の決め方は 2 通り:
//
// A/V 同期 (既定): 映像と同じ時計 C(pts) にデバイスで鳴るようにする。engine の出力遅延
//   (header の output_delay_qpc) を差し引いた時刻に engine へ渡す。音声が映像より遅れて届く送出元
//   では、届く前に鳴らせないので時計が遅らされ、その分映像が待つ (PlayoutClock::report_audio)。
//
// 低遅延 (A/V 非同期): 音声専用の時間軸で、音声だけを最小遅延で鳴らす。
//   MPEG-TS の音声は数フレーム分まとめて届くことがあるので (送出側の多重化設定次第)、
//   - 接続直後のまとめ届き (ストリーム解析中に溜まった分) が落ち着いてから 1 秒間の到着を見て
//     「いちばん遅く届くチャンクでも間に合う」時間軸に合わせ (それまでは鳴らさない)、
//   - それより遅れて届いたときだけ遅延を増やし (即時)、
//   - 10 秒間ずっと余裕があれば少しずつ詰める (送出元とのクロック差の吸収)。
//   映像との時刻のずれ (A/V オフセット) はメトリクスに出る。
//
// どちらも接続直後の約 1.5 秒は鳴らさない (到着の様子を見る)。
//
// 小さな抜け (10ms 超) は無音で埋め、重なりは捨てて、サンプル番号と時刻の対応を保つ。

#include <climits>
#include <cstdint>

#include "denpachan/audio_shm.h"
#include "denpachan/win_util.h"
#include "metrics.h"
#include "playout_clock.h"

namespace denpachan::worker {

class AudioWriter {
public:
    AudioWriter(PlayoutClock& video_clock, ChannelMetrics& metrics, bool av_sync)
        : video_clock_(video_clock), metrics_(metrics), av_sync_(av_sync) {}
    ~AudioWriter() { close(); }
    AudioWriter(const AudioWriter&) = delete;
    AudioWriter& operator=(const AudioWriter&) = delete;

    bool open(int ch);
    void close();

    // data: 48kHz / float32 / ステレオ (インターリーブ)
    void write(uint64_t session, int64_t pts_us, int64_t arrival_qpc, const float* data, int frames);

private:
    void append(const float* data, int frames);                                    // data == nullptr なら無音
    int64_t update_clock(int64_t pts_us, int64_t arrival_qpc);                     // 低遅延
    int64_t update_synced(uint64_t session, int64_t pts_us, int64_t arrival_qpc);  // A/V 同期
    void note_slack(int64_t slack, int64_t arrival_qpc);

    PlayoutClock& video_clock_;
    ChannelMetrics& metrics_;
    const bool av_sync_;
    SharedMapping mapping_;
    audio::Header* header_ = nullptr;
    float* ring_ = nullptr;

    uint64_t session_ = 0;
    bool have_next_ = false;
    int64_t next_pts_us_ = 0;

    // 音声の時間軸: due(pts) = anchor_qpc_ + (pts - anchor_pts_us_)
    int64_t anchor_pts_us_ = 0;
    int64_t anchor_qpc_ = 0;
    int64_t settle_end_qpc_ = 0;
    int64_t warmup_end_qpc_ = 0;
    bool published_ = false;  // ウォームアップが終わって読み手に時刻を見せているか
    int64_t window_start_qpc_ = 0;
    int64_t window_min_slack_ = 0;  // 窓内で最も余裕のなかったチャンクの (due - arrival)
    // A/V 同期: 最後に見た engine の出力遅延と、時計が一度に大きく動いた回数 (変わったら epoch を進める)
    int64_t last_output_delay_ = 0;
    int64_t last_nonzero_output_delay_ = 0;  // 最後に出力先があったときの出力遅延 (0 = まだ無い)
    uint64_t clock_jumps_ = 0;
    // engine がいま鳴らしているか (audio::Header::output_active。前回見た値と、一度でも鳴ったか)
    bool last_output_active_ = false;
    bool seen_active_ = false;
    // 音声の余裕の 1 秒ごとの最小 (今と前の 1 秒) と、出力が止まった・戻った後の 1 秒の最小 (ログ用)
    int64_t slack_bucket_start_qpc_ = 0;
    int64_t slack_cur_min_ = LLONG_MAX;
    int64_t slack_prev_min_ = LLONG_MAX;
    int64_t after_change_until_qpc_ = 0;
    int64_t after_change_min_ = LLONG_MAX;
    bool after_change_resumed_ = false;
};

}  // namespace denpachan::worker
