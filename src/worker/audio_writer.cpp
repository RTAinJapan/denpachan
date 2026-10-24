#include "audio_writer.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdlib>
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
// 1 回 (kTightenWindowMs) に詰める量の上限 (送出元とのクロック差の吸収)。2 ms / 10 秒 = 200 ppm まで追える。
// 0.5 ms (50 ppm) では、送出元の時計が 100 ppm 速いと余裕が溜まり続け、50 ms を超えるたびに一度に詰めて
// 音が飛んだ (約 17 分ごと)。engine はこの程度の動きをサンプルの重複・間引きで追う (合わせ直しになるのは 50 ms 超)
constexpr int64_t kTightenMaxStepUs = 2000;
constexpr int64_t kTightenJumpMs = 50;  // これ以上の余裕は一度に詰める (読み手は合わせ直す)

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

int64_t AudioWriter::update_synced(uint64_t session, int64_t pts_us, int64_t arrival_qpc) {
    // 映像と同じ時計 C(pts) に鳴るよう、engine の出力遅延を差し引いた時刻に engine へ渡す
    const int64_t output_delay = header_->output_delay_qpc.load(std::memory_order_relaxed);
    if (std::llabs(output_delay - last_output_delay_) > qpc_from_ms(1)) {
        // 出力遅延が変わった (engine がデバイスを開いた・開き直した): 渡す時刻がその分動き、時計の目標とのずれも
        // 残るので、しばらく大きなずれを一度に詰めてもらう。読み手には 1 回で合わせ直させる
        // 0 になった (出力先が無くなった) ときは、読み手がいないので何もしない
        if (published_) {
            spdlog::info("audio: output delay {:.1f} -> {:.1f} ms", double(qpc_to_us(last_output_delay_)) / 1000,
                         double(qpc_to_us(output_delay)) / 1000);
            if (output_delay != 0) header_->epoch.fetch_add(1, std::memory_order_release);
        }
        if (output_delay != 0) {
            // 前に鳴らしていたときの出力遅延との差だけ、時計を一度に動かす (「なし」を挟んでも、その前の値と比べる)
            if (published_ && last_nonzero_output_delay_ != 0)
                video_clock_.shift_for_output_delay(output_delay - last_nonzero_output_delay_);
            video_clock_.allow_fast_adjust();
            last_nonzero_output_delay_ = output_delay;
        }
        last_output_delay_ = output_delay;
    }
    auto handoff = [&] { return video_clock_.due(session, pts_us, arrival_qpc) - output_delay; };

    // 接続直後のまとめ届きの間は時計に報告しない (到着が実際より遅く見えるため)
    if (!published_ && arrival_qpc < settle_end_qpc_) return handoff();

    // 出力先が無い (engine の出力遅延が 0): 音声の余裕で時計を動かさない (「なし」にしただけで映像の遅延が変わらないように)
    if (output_delay == 0) {
        video_clock_.note_audio_without_output();
        if (!published_ && arrival_qpc >= warmup_end_qpc_ && video_clock_.settled()) {
            published_ = true;
            clock_jumps_ = video_clock_.jumps();
            spdlog::info("audio: synced to video (no output yet)");
        }
        return handoff();
    }

    // 余裕を報告する。足りなければ (音声が遅れて届いた) 時計が遅らされ、映像も待つ。
    video_clock_.report_audio(handoff() - arrival_qpc);
    // 時計の最初の調整が済んでから鳴らし始める (鳴り始めてから時間軸が大きく動かないように)
    if (!published_ && arrival_qpc >= warmup_end_qpc_ && video_clock_.settled()) {
        published_ = true;
        clock_jumps_ = video_clock_.jumps();
        spdlog::info("audio: synced to video (output delay {:.1f} ms)", double(qpc_to_us(output_delay)) / 1000);
    } else if (published_) {
        // 時計が一度に大きく動いた: 読み手が重複・間引きで少しずつ追わずに、1 回で合わせ直すよう epoch を進める
        const uint64_t jumps = video_clock_.jumps();
        if (jumps != clock_jumps_) {
            clock_jumps_ = jumps;
            header_->epoch.fetch_add(1, std::memory_order_release);
        }
    }
    return handoff();
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
    const int64_t due = av_sync_ ? update_synced(session, pts_us, arrival_qpc) : update_clock(pts_us, arrival_qpc);
    if (published_) write_time_map(header_, start, due);
    metrics_.audio_frames += uint64_t(frames);

    // 到着から engine に渡すまでの余裕 (= worker 内での音声の遅延) と、
    // 実際に鳴る時刻と同じ時刻の映像を出す時刻のずれ (正: 音声が遅い)
    const int64_t output_delay = header_->output_delay_qpc.load(std::memory_order_relaxed);
    metrics_.audio_delay_us = qpc_to_us(due - arrival_qpc);
    metrics_.av_offset_us = qpc_to_us(due + output_delay - video_clock_.due(session, pts_us, arrival_qpc));

    next_pts_us_ = pts_us + us_from_frames(frames);
    have_next_ = true;
}

}  // namespace denpachan::worker
