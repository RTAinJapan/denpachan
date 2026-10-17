#pragma once
// 音声リング (共有メモリ) の書き手。プロトコルは denpachan/audio_shm.h を参照。
//
// デコード済み音声 (48kHz / float32 / ステレオ) を書き、チャンクごとに「このフレームを鳴らすべき時刻」を書く。
//
// 鳴らすべき時刻は音声専用の時間軸で決める (映像の時計には合わせない):
//   MPEG-TS の音声は数フレーム分まとめて、同じ時刻の映像より遅れて届くことが多い (送出側のマルチプレクス次第で
//   数十〜百数十 ms)。届く前に鳴らすことはできないので、映像に合わせても音声は遅れる上に、遅れ量の揺れで
//   遅延が動いてしまう。そこで音声は
//   - 接続直後のまとめ届き (ストリーム解析中に溜まった分) が落ち着いてから 1 秒間の到着を見て
//     「いちばん遅く届くチャンクでも間に合う」時間軸に合わせ (それまでは鳴らさない)、
//   - それより遅れて届いたときだけ遅延を増やし (即時)、
//   - 10 秒間ずっと余裕があれば少しずつ詰める (送出元とのクロック差の吸収)。
//   これで送出側の届き方に対して最小、かつ一定の遅延になる。
//   映像との時刻のずれ (A/V オフセット) はメトリクスに出し、運用で一度合わせる。
//
// 小さな抜け (10ms 超) は無音で埋め、重なりは捨てて、サンプル番号と時刻の対応を保つ。

#include <cstdint>

#include "denpachan/audio_shm.h"
#include "denpachan/win_util.h"
#include "metrics.h"
#include "playout_clock.h"

namespace denpachan::worker {

class AudioWriter {
public:
    AudioWriter(PlayoutClock& video_clock, ChannelMetrics& metrics) : video_clock_(video_clock), metrics_(metrics) {}
    ~AudioWriter() { close(); }
    AudioWriter(const AudioWriter&) = delete;
    AudioWriter& operator=(const AudioWriter&) = delete;

    bool open(int ch);
    void close();

    // data: 48kHz / float32 / ステレオ (インターリーブ)
    void write(uint64_t session, int64_t pts_us, int64_t arrival_qpc, const float* data, int frames);

private:
    void append(const float* data, int frames);  // data == nullptr なら無音
    int64_t update_clock(int64_t pts_us, int64_t arrival_qpc);

    PlayoutClock& video_clock_;  // A/V オフセットの計測だけに使う
    ChannelMetrics& metrics_;
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
};

}  // namespace denpachan::worker
