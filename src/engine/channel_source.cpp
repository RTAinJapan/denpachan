#include "channel_source.h"

#include "ffmpeg_available.h"
#include "render_log.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>

namespace denpachan::engine {

using namespace denpachan::audio;

namespace {

constexpr double kResyncThresholdSec = 0.050;  // これ以上ずれたら (平滑化の前の値で) すぐ合わせ直す
// 平滑化したずれがこれ以上なら合わせ直す。開いた直後に出力遅延が決まって worker の時間軸が動いたときなど、一度に動いた
// ずれを 1 サンプルずつ詰めると (2 ms/秒まで) 数十秒かかり、その間ずっと重複・間引きが続くため (DVS で 30 ms を詰めるのに
// 1,300〜2,500 回)。クロック差によるゆっくりしたずれは、重複・間引きで kSlipThresholdSec 付近に保たれるので届かない
constexpr double kOffsetResyncThresholdSec = 0.005;
constexpr double kSlipThresholdSec = 0.002;  // 48kHz デバイス: これ以上ずれたら 1 サンプル重複 / 間引き
// 同じ向きの重複・間引きが 1 秒に kSlipRateResync 回を超えるのが kSlipRateSeconds 秒続いたら合わせ直す。
// 時間軸がゆっくり動き続けると (1 秒に 1 ms = 48 回)、重複・間引きが追いついてずれは 2 ms 付近のまま
// kOffsetResyncThresholdSec に届かず、何十秒も続く (DVS の起動直後に 900〜2,400 回)。
// 24 回/秒 = 500 ppm。送出元とデバイスのクロック差 (実測 24 ppm ≈ 1.2 回/秒、試験の 150 ppm ≈ 7 回/秒) では届かない
constexpr int kSlipRateResync = 24;
constexpr int kSlipRateSeconds = 2;
constexpr int kSlipLogPerSec = 5;  // 1 秒の重複・間引きがこれ以上なら debug ログに出す
// 読むデータが足りないとき、続けてこの長さまでは無音で埋めて鳴らし続ける (読む位置も進める)。送出元からの欠落 (SRT で
// 再送が間に合わなかった、数個まとめて落ちた) は、worker が次のかたまりが届いたときに無音で埋めるので、それまでの抜けも
// どのみち無音になる。止めて鳴らし直すと、合わせ直し (フェード) と音切れに数えることになる。これより長く足りなければ
// 途切れとして止める (時間軸は worker が遅らせる)。どのデバイスでも、音声のかたまり (20 ms 前後) の 10 個分ほどの長さ
constexpr double kBridgeSec = 0.250;
// 48kHz デバイス: 重複 / 間引きは 10ms に 1 回まで (最大約 2000ppm のクロック差まで追従できる)。
// 実際に起きる回数はクロック差の分だけ (例: 50ppm なら 2〜3 回/秒)。
constexpr int kMinSlipIntervalFrames = 480;
constexpr double kMaxCompensation = 0.005;  // 48kHz 以外: リサンプル補正の上限 (±0.5%)
constexpr double kCorrectionPerSec = 0.5;   // 48kHz 以外: ずれのうち 1 秒あたりに詰める割合
constexpr double kErrorSmoothingSec = 1.0;  // ずれの平滑化 (スレッドの起床の揺れで反応しないように)
constexpr int kReadChunk = 256;             // リサンプル時にリングから一度に読むフレーム数
// 書き手が上書きしている途中の領域を読まないよう、リング末尾はこれだけ避ける
constexpr uint64_t kRingSafety = 4096;

std::string fmt_reason(const char* reason, double error_ms) {
    char buf[96];
    snprintf(buf, sizeof(buf), "%s (error %+.1f ms)", reason, error_ms);
    return buf;
}

}  // namespace

ChannelSource::ChannelSource(const RouteOptions& options, uint32_t device_rate, int device_channels, int64_t base_delay_qpc)
    : options_(options),
      device_rate_(device_rate),
      device_channels_(device_channels),
      fade_frames_(std::max<int>(1, int(device_rate / 100))),
      passthrough_(device_rate == kSampleRate) {
    stats_.passthrough = passthrough_;
    mute_ = options.mute;
    // 描画を始める前に済ませておく (描画コールバックでの最初の 1 回をなくす)
    ensure_swr();
    ensure_open();
    // 出力遅延は鳴らし始める前に worker に知らせる (鳴り始めてから worker の時間軸が動いて合わせ直すことを減らす)
    if (header_) header_->output_delay_qpc.store(base_delay_qpc + qpc_from_ms(options_.offset_ms), std::memory_order_relaxed);
}

ChannelSource::~ChannelSource() {
    // 出力遅延は 0 にしない: デバイスを開き直すときは作り直すので、0 を経由すると worker が「出力先なし」と
    // 見てしまう。出力先が無くなるときは、出力 (DeviceOutput / AsioOutput) が止めるときに clear_output_delay を呼ぶ
    if (swr_) swr_free(&swr_);  // FFmpeg が無いときに呼ばない (遅延ロード)
}

bool ChannelSource::ensure_swr() {
    if (passthrough_ || swr_) return true;
    // 48 kHz 以外のデバイスはリサンプルが要る。FFmpeg (同梱しない) が無ければ出せない。1 秒ごとに試す
    const int64_t now = qpc_now();
    if (last_swr_attempt_ != 0 && now - last_swr_attempt_ < qpc_from_ms(1000)) return false;
    last_swr_attempt_ = now;
    if (!ffmpeg_available()) return false;
    AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
    swr_alloc_set_opts2(&swr_, &stereo, AV_SAMPLE_FMT_FLT, int(device_rate_), &stereo, AV_SAMPLE_FMT_FLT, int(kSampleRate), 0, nullptr);
    if (swr_ && swr_init(swr_) < 0) swr_free(&swr_);
    return swr_ != nullptr;
}

bool ChannelSource::ensure_open() {
    if (header_) return true;
    const int64_t now = qpc_now();
    if (last_open_attempt_ != 0 && now - last_open_attempt_ < qpc_from_ms(1000)) return false;
    last_open_attempt_ = now;
    // 描画コールバックの中から呼ぶので、worker が作った直後でも待たない (1 秒後にまた試す)
    header_ = open_shared(mapping_, options_.ch, false);
    if (header_) ring_ = ring_data(mapping_.data());
    return header_ != nullptr;
}

// 重複・間引きを 1 秒ごとに数える (同じ向きが続いたら合わせ直す。多いときは debug ログに出す)
void ChannelSource::count_slip(int slip, int frames) {
    if (slip > 0) {
        ++slip_window_dup_;
        ++stats_.duplicated;
    } else if (slip < 0) {
        ++slip_window_drop_;
        ++stats_.dropped;
    }
    slip_window_frames_ += frames;
    if (slip_window_frames_ < int(device_rate_)) return;
    const int net = slip_window_dup_ - slip_window_drop_;
    slip_rate_seconds_ = std::abs(net) > kSlipRateResync ? slip_rate_seconds_ + 1 : 0;
    if (slip_window_dup_ + slip_window_drop_ >= kSlipLogPerSec) {
        render_log().debug("audio ch{}: {} duplicated / {} dropped in the last second, error {:+.2f} ms", options_.ch, slip_window_dup_,
                           slip_window_drop_, error_avg_ * 1000.0 / kSampleRate);
    }
    slip_window_frames_ = 0;
    slip_window_dup_ = 0;
    slip_window_drop_ = 0;
}

void ChannelSource::start_at(uint64_t pos, uint64_t epoch) {
    read_pos_ = pos;
    epoch_ = epoch;
    bridged_frames_ = 0;
    if (swr_) swr_init(swr_);  // 内部バッファと補正を捨てる
    fifo_frames_ = 0;
    error_avg_ = 0.0;
    fade_ = options_.fade ? 0.0f : 1.0f;  // フェードインから
    playing_ = true;
    resync_pending_ = false;
    slip_window_frames_ = 0;
    slip_window_dup_ = 0;
    slip_window_drop_ = 0;
    slip_rate_seconds_ = 0;
    ++stats_.resyncs;
}

void ChannelSource::copy_from_ring(float* dst, uint64_t pos, int frames) const {
    const uint32_t offset = uint32_t(pos & (kCapacityFrames - 1));
    const int first = std::min<int>(frames, int(kCapacityFrames - offset));
    memcpy(dst, ring_ + size_t(offset) * kChannels, size_t(first) * kChannels * sizeof(float));
    if (frames > first) {
        memcpy(dst + size_t(first) * kChannels, ring_, size_t(frames - first) * kChannels * sizeof(float));
    }
}

int ChannelSource::pull_passthrough(float* dst, int frames, int slip) {
    const uint64_t write_pos = header_->write_pos.load(std::memory_order_acquire);
    int64_t avail = int64_t(write_pos - read_pos_);
    int produced = 0;
    if (slip > 0 && avail > 0) {
        // 1 サンプル重複: 先頭のサンプルを 2 回出す (読み位置は進めない)
        copy_from_ring(dst, read_pos_, 1);
        dst += kChannels;
        produced = 1;
    } else if (slip < 0 && avail > 1) {
        // 1 サンプル間引き
        ++read_pos_;
        --avail;
    }
    const int n = int(std::min<int64_t>(avail, frames - produced));
    if (n > 0) {
        copy_from_ring(dst, read_pos_, n);
        read_pos_ += uint64_t(n);
        produced += n;
    }
    return produced;
}

int ChannelSource::pull_resampled(float* dst, int frames) {
    const uint64_t write_pos = header_->write_pos.load(std::memory_order_acquire);
    while (fifo_frames_ < size_t(frames) && write_pos > read_pos_) {
        const int chunk = int(std::min<uint64_t>(write_pos - read_pos_, kReadChunk));
        in_buf_.resize(size_t(chunk) * kChannels);
        copy_from_ring(in_buf_.data(), read_pos_, chunk);
        read_pos_ += uint64_t(chunk);

        const int max_out = swr_get_out_samples(swr_, chunk);
        if (max_out <= 0) continue;
        if (fifo_.size() < (fifo_frames_ + size_t(max_out)) * kChannels) {
            fifo_.resize((fifo_frames_ + size_t(max_out)) * kChannels);
        }
        uint8_t* out = reinterpret_cast<uint8_t*>(fifo_.data() + fifo_frames_ * kChannels);
        const uint8_t* in = reinterpret_cast<const uint8_t*>(in_buf_.data());
        const int n = swr_convert(swr_, &out, max_out, &in, chunk);
        if (n > 0) fifo_frames_ += size_t(n);
    }

    const int n = int(std::min(fifo_frames_, size_t(frames)));
    memcpy(dst, fifo_.data(), size_t(n) * kChannels * sizeof(float));
    memmove(fifo_.data(), fifo_.data() + size_t(n) * kChannels, (fifo_frames_ - size_t(n)) * kChannels * sizeof(float));
    fifo_frames_ -= size_t(n);
    return n;
}

void ChannelSource::render(float* out, int frames, int64_t first_hear_qpc, int64_t base_delay_qpc) {
    if (!ensure_swr() || !ensure_open()) {
        stats_.playing = false;
        return;
    }

    uint64_t map_frame = 0;
    int64_t map_due = 0;
    const bool has_map = read_time_map(header_, map_frame, map_due);
    const uint64_t write_pos = header_->write_pos.load(std::memory_order_acquire);
    const uint64_t epoch = header_->epoch.load(std::memory_order_acquire);
    const int64_t delay = base_delay_qpc + qpc_from_ms(options_.offset_ms);
    header_->output_delay_qpc.store(delay, std::memory_order_relaxed);  // worker の A/V 同期に使う
    header_->output_active.store(1, std::memory_order_relaxed);         // 鳴らしている (描画が呼ばれた)

    const double src_per_out = double(kSampleRate) / device_rate_;
    const double need = frames * src_per_out;
    // デバイスでこのバッファの先頭が鳴る瞬間に、鳴っているべきサンプル番号
    const double ideal = double(map_frame) + double(first_hear_qpc - delay - map_due) * kSampleRate / double(qpc_freq());
    const double oldest = double(write_pos) - double(kCapacityFrames - kRingSafety);

    int slip = 0;
    if (playing_) {
        // いま鳴ろうとしているサンプル番号 (リサンプル時は内部と FIFO に溜まっている分を差し引く)
        double current = double(read_pos_);
        if (!passthrough_) current -= double(swr_get_delay(swr_, kSampleRate)) + double(fifo_frames_) * src_per_out;
        const double error = current - ideal;  // 正: 予定より先の位置 = 早く鳴っている
        error_avg_ += (error - error_avg_) * std::min(1.0, double(frames) / device_rate_ / kErrorSmoothingSec);
        stats_.error_ms = error_avg_ * 1000.0 / kSampleRate;
        stats_.latency_ms = double(qpc_to_us(delay)) / 1000.0 - error_avg_ * 1000.0 / kSampleRate;

        if (!resync_pending_) {
            // 合わせ直しの理由 (統計に残す)
            const char* reason = !has_map                                                         ? "no time map"
                                 : epoch != epoch_                                                ? "new epoch"
                                 : std::abs(error) > kResyncThresholdSec * kSampleRate            ? "position error"
                                 : std::abs(error_avg_) > kOffsetResyncThresholdSec * kSampleRate ? "position offset"
                                 : slip_rate_seconds_ >= kSlipRateSeconds                         ? "slip rate"
                                 : double(read_pos_) < oldest                                     ? "overrun"
                                                                                                  : nullptr;
            if (reason) {
                resync_pending_ = true;  // このバッファでフェードアウトしてから合わせ直す
                std::lock_guard lk(stats_.reason_mutex);
                stats_.last_resync_reason = fmt_reason(reason, error * 1000.0 / kSampleRate);
            }
        }
        if (!resync_pending_) {
            if (passthrough_) {
                const double threshold = kSlipThresholdSec * kSampleRate;
                frames_since_slip_ = std::min(frames_since_slip_ + frames, kMinSlipIntervalFrames);  // 溢れさせない
                if (frames_since_slip_ >= kMinSlipIntervalFrames) {
                    if (error_avg_ > threshold) slip = 1;         // 早く鳴っている → 1 サンプル重複
                    else if (error_avg_ < -threshold) slip = -1;  // 遅れている → 1 サンプル間引き
                }
                if (slip != 0) frames_since_slip_ = 0;
                error_avg_ -= slip;
                count_slip(slip, frames);
            } else {
                // 早く鳴っている (error > 0) なら出力サンプルを増やしてゆっくり進める
                const double max_delta = device_rate_ * kMaxCompensation;
                const double delta = std::clamp(error_avg_ / src_per_out * kCorrectionPerSec, -max_delta, max_delta);
                swr_set_compensation(swr_, int(std::lround(delta)), int(device_rate_));
                stats_.ratio_ppm = delta / device_rate_ * 1e6;
            }
        }
    }

    if (!playing_) {
        // 目標位置のデータが揃っていれば鳴らし始める
        if (has_map && ideal >= 0 && ideal >= oldest && ideal + need <= double(write_pos)) {
            start_at(uint64_t(ideal), epoch);
        } else {
            stats_.playing = false;
            return;
        }
    }
    stats_.playing = true;

    tmp_.resize(size_t(frames) * kChannels);
    const int got = passthrough_ ? pull_passthrough(tmp_.data(), frames, slip) : pull_resampled(tmp_.data(), frames);
    if (slip != 0) ++stats_.slips;
    const bool short_of_data = got < frames;
    bool underrun = false;
    bool bridged = false;
    if (short_of_data) {
        std::fill(tmp_.begin() + ptrdiff_t(got) * kChannels, tmp_.end(), 0.0f);
        const int missing = frames - got;
        if (bridged_frames_ + missing <= int(kBridgeSec * device_rate_)) {
            // 短い抜け: 無音で埋めて読む位置も進め、鳴らし続ける (kBridgeSec)
            bridged_frames_ += missing;
            read_pos_ += uint64_t(std::llround(missing * src_per_out));
            bridged = true;
        } else {
            underrun = true;
        }
    } else {
        bridged_frames_ = 0;
    }

    // フェードは開始・途絶・合わせ直し・ミュート切替の瞬間だけ。それ以外はデータをそのまま置く。
    const float fade_target = (resync_pending_ || mute_) ? 0.0f : 1.0f;
    if (!options_.fade) fade_ = fade_target;
    const bool fading = options_.fade && (fade_ != fade_target || short_of_data);
    const bool silent = !fading && fade_ <= 0.0f;  // ミュート中
    const int fade_out_start = short_of_data ? std::max(0, got - fade_frames_) : INT_MAX;
    const float fade_step = 1.0f / float(fade_frames_);

    const int sc = options_.start_channel - 1;
    float peak_l = 0, peak_r = 0;
    for (int i = 0; i < frames && !silent; ++i) {
        float l = tmp_[2 * size_t(i)];
        float r = tmp_[2 * size_t(i) + 1];
        if (fading) {
            fade_ = fade_ < fade_target ? std::min(fade_target, fade_ + fade_step) : std::max(fade_target, fade_ - fade_step);
            float g = fade_;
            if (i >= fade_out_start) g *= float(std::max(0, got - i)) / float(got - fade_out_start);
            l *= g;
            r *= g;
        }
        float* frame = out + size_t(i) * device_channels_;
        if (sc + 1 < device_channels_) {
            frame[sc] += l;
            frame[sc + 1] += r;
        } else if (sc < device_channels_) {
            frame[sc] += 0.5f * (l + r);  // 1ch しか残っていなければモノラルに
        }
        peak_l = std::max(peak_l, std::fabs(l));
        peak_r = std::max(peak_r, std::fabs(r));
    }
    stats_.peak_l = std::max(stats_.peak_l.load(), peak_l);
    stats_.peak_r = std::max(stats_.peak_r.load(), peak_r);

    if (bridged) {
        fade_ = 0.0f;  // データが戻ったらフェードインする
    } else if (underrun) {
        ++stats_.underruns;
        playing_ = false;
        fade_ = 0.0f;
    } else if (resync_pending_ && (fade_ <= 0.0f || !options_.fade)) {
        playing_ = false;  // 次のバッファで目標位置から鳴らし直す
        resync_pending_ = false;
    }
}

}  // namespace denpachan::engine
