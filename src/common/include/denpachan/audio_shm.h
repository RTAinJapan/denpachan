#pragma once
// チャンネルの音声リング (worker → engine) の共有メモリレイアウト。設計書 §6 を参照。
//
//   [Header (4KB)] [float32 ステレオ x kCapacityFrames]
//
// - 書き手 (worker) はデコードした音声を 48kHz / float32 / ステレオにそろえて書き、write_pos を進める。
//   読み手を待たない (読み手が遅れていれば古いデータを上書きする)。
// - 書き手は「フレーム番号 map_frame のサンプルを、時刻 map_due_qpc に読み手へ渡すべき」という対応を
//   チャンクごとに更新する。読み手はそこから output_delay_qpc 後にデバイスで鳴らす。
//   A/V 同期モードでは map_due_qpc + output_delay_qpc が同じ時刻の映像を出す瞬間になる。
//   どのチャンクも届いた時点で間に合うように決める。接続直後のウォームアップ中は更新しない
//   (読み手は鳴らさずに待つ)。
//   読み手はこれを見て、デバイスから実際に鳴る時刻が「due + 音声遅延」になるように読み位置を合わせる。
// - 接続し直しなどで時間軸が不連続になったら epoch を進める。読み手は読み位置を合わせ直す。

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

#include "denpachan/channels.h"
#include "denpachan/win_util.h"

namespace denpachan::audio {

inline constexpr uint32_t kMagic = 0x41505044;  // "DPPA"
inline constexpr uint32_t kLayoutVersion = 1;
inline constexpr int kNumChannels = kMaxChannels;
inline constexpr uint32_t kSampleRate = 48000;
inline constexpr uint32_t kChannels = 2;
inline constexpr uint32_t kCapacityFrames = 1u << 17;  // 約 2.7 秒 (2 の累乗)
inline constexpr size_t kHeaderBytes = 4096;
inline constexpr size_t kMappingBytes = kHeaderBytes + size_t(kCapacityFrames) * kChannels * sizeof(float);

struct Header {
    std::atomic<uint32_t> magic;
    uint32_t layout_version;
    uint32_t sample_rate;
    uint32_t channels;
    uint32_t capacity_frames;
    uint32_t reserved;

    std::atomic<uint32_t> writer_pid;
    std::atomic<int64_t> writer_heartbeat_qpc;
    std::atomic<uint64_t> write_pos;  // 書き込み済みフレーム数 (単調増加)
    std::atomic<uint64_t> epoch;      // 時間軸が不連続になるたびに増える

    // 読み手 (engine) が書く: map_due_qpc から実際にデバイスで鳴るまでの遅延
    // (デバイスのバッファ + 余裕 + 音声オフセット)。書き手は A/V 同期のときこれを差し引いて、
    // 映像と同じ瞬間に鳴るように時刻を決める。0 = 読み手なし。
    std::atomic<int64_t> output_delay_qpc;

    // フレーム番号 ↔ 鳴らすべき時刻 (seqlock: 書き込み中は map_seq が奇数)
    std::atomic<uint32_t> map_seq;
    std::atomic<uint64_t> map_frame;
    std::atomic<int64_t> map_due_qpc;
};

static_assert(sizeof(Header) <= kHeaderBytes);

inline std::wstring mapping_name(int ch) { return L"Local\\denpachan_audio_" + std::to_wstring(ch); }

inline float* ring_data(void* base) {
    return reinterpret_cast<float*>(static_cast<uint8_t*>(base) + kHeaderBytes);
}

// 書き手専用 (書き手は 1 つ)
inline void write_time_map(Header* h, uint64_t frame, int64_t due_qpc) {
    const uint32_t s = h->map_seq.load(std::memory_order_relaxed);
    h->map_seq.store(s + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    h->map_frame.store(frame, std::memory_order_relaxed);
    h->map_due_qpc.store(due_qpc, std::memory_order_relaxed);
    h->map_seq.store(s + 2, std::memory_order_release);
}

// 読み手。まだ一度も書かれていなければ false
inline bool read_time_map(const Header* h, uint64_t& frame, int64_t& due_qpc) {
    for (int i = 0; i < 100; ++i) {
        const uint32_t s1 = h->map_seq.load(std::memory_order_acquire);
        if (s1 & 1) continue;
        frame = h->map_frame.load(std::memory_order_relaxed);
        due_qpc = h->map_due_qpc.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (h->map_seq.load(std::memory_order_relaxed) == s1) return s1 != 0;
    }
    return false;
}

// 共有メモリを開く (なければ作って初期化する)。どちらの側から先に開いてもよい。
inline Header* open_shared(SharedMapping& mapping, int ch) {
    bool created = false;
    if (!mapping.open_or_create(mapping_name(ch), kMappingBytes, created)) return nullptr;
    auto* h = static_cast<Header*>(mapping.data());
    if (created) {
        h->layout_version = kLayoutVersion;
        h->sample_rate = kSampleRate;
        h->channels = kChannels;
        h->capacity_frames = kCapacityFrames;
        h->magic.store(kMagic, std::memory_order_release);
    } else {
        for (int i = 0; i < 100 && h->magic.load(std::memory_order_acquire) != kMagic; ++i) Sleep(5);
        if (h->magic.load(std::memory_order_acquire) != kMagic) {
            mapping.close();
            return nullptr;
        }
    }
    if (h->layout_version != kLayoutVersion || h->capacity_frames != kCapacityFrames) {
        mapping.close();
        return nullptr;
    }
    return h;
}

}  // namespace denpachan::audio
