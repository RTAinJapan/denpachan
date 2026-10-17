#include "audio_writer.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>

namespace denpachan::worker {

using namespace denpachan::audio;

namespace {

constexpr int64_t kGapToleranceUs = 10'000;      // これを超える抜けは無音で埋める
constexpr int64_t kDiscontinuityUs = 1'000'000;  // これを超える飛びは時間軸の不連続
// 接続直後は、ストリーム解析中に溜まった音声がまとめて届く (到着が実際より遅く見える)。
// その期間 (kSettleMs) は時間軸の決定に使わず、続く kMeasureMs の到着から決める。
constexpr int64_t kSettleMs = 500;
constexpr int64_t kMeasureMs = 1'000;
constexpr int64_t kTightenWindowMs = 10'000;  // この間ずっと余裕があれば遅延を詰める
constexpr int64_t kTightenKeepMs = 1;         // 詰めるときに残す余裕
constexpr int64_t kTightenMaxStepUs = 500;    // 1 回に詰める量の上限 (送出元とのクロック差の吸収)
constexpr int64_t kTightenJumpMs = 50;        // これ以上の余裕は一度に詰める (読み手は合わせ直す)

int64_t frames_from_us(int64_t us) { return us * kSampleRate / 1'000'000; }
int64_t us_from_frames(int64_t frames) { return frames * 1'000'000 / kSampleRate; }

}  // namespace

bool AudioWriter::open(int ch) {
    close();
    header_ = open_shared(mapping_, ch);
    if (!header_) {
        spdlog::error("audio {}: cannot open shared memory", ch);
        return false;
    }
    ring_ = ring_data(mapping_.data());
    header_->writer_pid.store(GetCurrentProcessId());
    header_->writer_heartbeat_qpc.store(qpc_now());
    header_->epoch.fetch_add(1);  // 前の書き手 (再起動前) とは時間軸がつながらない
    have_next_ = false;
    spdlog::info("audio {}: opened", ch);
    return true;
}

void AudioWriter::close() {
    if (header_ && header_->writer_pid.load() == GetCurrentProcessId()) {
        header_->writer_pid.store(0);
        header_->writer_heartbeat_qpc.store(0);
    }
    header_ = nullptr;
    ring_ = nullptr;
    mapping_.close();
}

void AudioWriter::append(const float* data, int frames) {
    uint64_t pos = header_->write_pos.load(std::memory_order_relaxed);
    while (frames > 0) {
        const uint32_t offset = uint32_t(pos & (kCapacityFrames - 1));
        const int n = std::min<int>(frames, int(kCapacityFrames - offset));
        float* dst = ring_ + size_t(offset) * kChannels;
        if (data) {
            memcpy(dst, data, size_t(n) * kChannels * sizeof(float));
            data += size_t(n) * kChannels;
        } else {
            memset(dst, 0, size_t(n) * kChannels * sizeof(float));
        }
        pos += uint64_t(n);
        frames -= n;
    }
    header_->write_pos.store(pos, std::memory_order_release);
}

int64_t AudioWriter::update_clock(int64_t pts_us, int64_t arrival_qpc) {
    int64_t due = anchor_qpc_ + qpc_from_us(pts_us - anchor_pts_us_);

    if (!published_) {
        // ウォームアップ: 落ち着くまで (kSettleMs) は見送り、その後 kMeasureMs の間で
        // 最も余裕のなかったチャンクがちょうど間に合うように時間軸を決める
        if (arrival_qpc < settle_end_qpc_) return due;
        window_min_slack_ = std::min(window_min_slack_, due - arrival_qpc);
        if (arrival_qpc < warmup_end_qpc_) return due;
        anchor_qpc_ -= window_min_slack_;
        due -= window_min_slack_;
        published_ = true;
        window_start_qpc_ = arrival_qpc;
        window_min_slack_ = LLONG_MAX;
        spdlog::info("audio: delay settled at {:.1f} ms after arrival", double(qpc_to_us(due - arrival_qpc)) / 1000);
        return due;
    }

    const int64_t slack = due - arrival_qpc;  // 負: 鳴らすべき時刻より遅れて届いた
    if (slack < 0) {
        // 遅れて届いた: 間に合うように遅延を増やす
        anchor_qpc_ -= slack;
        due -= slack;
        spdlog::info("audio: arrived {:.1f} ms late, delay increased", double(qpc_to_us(-slack)) / 1000);
        window_min_slack_ = 0;
    } else {
        window_min_slack_ = std::min(window_min_slack_, slack);
    }

    if (arrival_qpc - window_start_qpc_ >= qpc_from_ms(kTightenWindowMs)) {
        // 窓の間ずっと余裕があった: 詰める
        const int64_t spare = window_min_slack_ - qpc_from_ms(kTightenKeepMs);
        if (window_min_slack_ != LLONG_MAX && spare > 0) {
            const int64_t step = spare > qpc_from_ms(kTightenJumpMs) ? spare : std::min(spare, qpc_from_us(kTightenMaxStepUs));
            anchor_qpc_ -= step;
            due -= step;
        }
        window_start_qpc_ = arrival_qpc;
        window_min_slack_ = LLONG_MAX;
    }
    return due;
}

void AudioWriter::write(uint64_t session, int64_t pts_us, int64_t arrival_qpc, const float* data, int frames) {
    if (!header_ || frames <= 0) return;
    header_->writer_heartbeat_qpc.store(qpc_now(), std::memory_order_relaxed);

    bool discontinuity = !have_next_ || session != session_;
    if (!discontinuity) {
        const int64_t gap = pts_us - next_pts_us_;
        if (gap > kDiscontinuityUs || gap < -kDiscontinuityUs) {
            discontinuity = true;
        } else if (gap > kGapToleranceUs) {
            append(nullptr, int(frames_from_us(gap)));  // 抜けを無音で埋める
            ++metrics_.audio_gaps;
            pts_us = next_pts_us_ + us_from_frames(frames_from_us(gap));
        } else if (gap < -kGapToleranceUs) {
            const int skip = int(frames_from_us(-gap));  // 重なった分を捨てる
            if (skip >= frames) return;
            data += size_t(skip) * kChannels;
            frames -= skip;
            pts_us += us_from_frames(skip);
        }
    }
    if (discontinuity) {
        // 新しい時間軸: ウォームアップが終わるまで読み手には時刻を見せない (鳴らさない)
        session_ = session;
        anchor_pts_us_ = pts_us;
        anchor_qpc_ = arrival_qpc;
        settle_end_qpc_ = arrival_qpc + qpc_from_ms(kSettleMs);
        warmup_end_qpc_ = settle_end_qpc_ + qpc_from_ms(kMeasureMs);
        published_ = false;
        window_min_slack_ = LLONG_MAX;
        header_->epoch.fetch_add(1, std::memory_order_release);
    }

    // レベル (ピーク) を測る
    float peak_l = 0, peak_r = 0;
    for (int i = 0; i < frames; ++i) {
        peak_l = std::max(peak_l, std::fabs(data[2 * i]));
        peak_r = std::max(peak_r, std::fabs(data[2 * i + 1]));
    }
    metrics_.audio_peak_l = std::max(metrics_.audio_peak_l.load(), peak_l);
    metrics_.audio_peak_r = std::max(metrics_.audio_peak_r.load(), peak_r);

    const uint64_t start = header_->write_pos.load(std::memory_order_relaxed);
    append(data, frames);
    const int64_t due = update_clock(pts_us, arrival_qpc);
    if (published_) write_time_map(header_, start, due);
    metrics_.audio_frames += uint64_t(frames);

    // 到着から鳴らすべき時刻までの余裕 (= worker 内での音声の遅延) と、映像との時刻のずれ
    metrics_.audio_delay_us = qpc_to_us(due - arrival_qpc);
    metrics_.av_offset_us = qpc_to_us(due - video_clock_.due(session, pts_us, arrival_qpc));

    next_pts_us_ = pts_us + us_from_frames(frames);
    have_next_ = true;
}

}  // namespace denpachan::worker
