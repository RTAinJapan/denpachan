#pragma once
// worker の状態 (worker → engine) の共有メモリ。設計書 §4 / §10.2。
//
// - pid と args_hash: engine が再起動したとき、同じ設定で動いている worker をそのまま引き継ぐために使う。
// - ハートビート: スレッドごとに更新する。engine は途絶えたら固まったとみなして強制終了・再起動する。
//   デコーダはネットワーク待ちと区別するため「デコード処理に入った時刻」を置き、終わったら 0 に戻す。
// - メトリクス: 1 秒ごとのスナップショット (seqlock)。
//
// 停止要求は名前付きイベント (stop_event_name) で送る。

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>

#include "denpachan/win_util.h"

namespace denpachan::worker_status {

inline constexpr uint32_t kMagic = 0x53575044;  // "DPWS"
inline constexpr uint32_t kLayoutVersion = 2;
inline constexpr size_t kMappingBytes = 16384;

enum class VideoState : uint32_t { Waiting = 0, Live = 1, Hold = 2 };

// 1 秒ごとのスナップショット (POD)
struct Metrics {
    uint32_t connected;
    uint32_t connections;
    char peer[64];
    char codec[32];
    char last_error[128];
    int32_t width;
    int32_t height;
    uint32_t hw_decode;
    uint32_t video_state;  // VideoState

    double bitrate_mbps;
    double decode_fps;
    double output_fps;
    double video_delay_ms;  // デコード完了から仮想カメラ出力まで (A/V 同期の待ちを含む)
    uint64_t dropped_late;
    uint64_t dropped_queue;
    uint64_t decode_errors;
    uint64_t overflow_bytes;

    int32_t srt_latency_ms;
    int32_t srt_rcv_buf_ms;
    double srt_rtt_ms;
    double srt_recv_mbps;
    int64_t srt_lost;
    int64_t srt_retrans;
    int64_t srt_dropped;

    double tcp_rtt_ms;  // RTMP push
    uint64_t tcp_retrans_bytes;

    double audio_peak_l_db;
    double audio_peak_r_db;
    double audio_delay_ms;
    double av_offset_ms;
    uint64_t audio_gaps;

    uint32_t rec_state;  // 0 オフ / 1 映像待ち / 2 録画中 / 3 エラー
    double rec_seconds;
    uint64_t rec_bytes;
    uint64_t rec_dropped;
    char rec_file[512];
    char rec_error[160];
};

struct Header {
    std::atomic<uint32_t> magic;
    uint32_t layout_version;
    std::atomic<uint32_t> pid;
    std::atomic<uint64_t> args_hash;
    std::atomic<int64_t> started_qpc;

    std::atomic<int64_t> ingest_heartbeat_qpc;
    std::atomic<int64_t> playout_heartbeat_qpc;
    std::atomic<int64_t> decoder_busy_since_qpc;  // 0 = デコード処理中でない

    std::atomic<uint32_t> metrics_seq;  // 書き込み中は奇数
    Metrics metrics;

    // engine → worker: 録画の設定 (JSON)。再起動せずに変えられるよう、コマンドラインでなくここで渡す。
    std::atomic<uint32_t> control_seq;  // 書き込み中は奇数
    char record_json[4096];
};

static_assert(sizeof(Header) <= kMappingBytes);

// engine 側 (書き手は 1 つ)
inline void write_record_config(Header* h, const std::string& json) {
    const uint32_t s = h->control_seq.load(std::memory_order_relaxed);
    h->control_seq.store(s + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    strncpy_s(h->record_json, json.c_str(), _TRUNCATE);
    h->control_seq.store(s + 2, std::memory_order_release);
}

// worker 側: 前回読んだ seq から変わっていれば out に読んで true
inline bool read_record_config(const Header* h, uint32_t& last_seq, std::string& out) {
    for (int i = 0; i < 100; ++i) {
        const uint32_t s1 = h->control_seq.load(std::memory_order_acquire);
        if (s1 & 1) continue;
        if (s1 == last_seq) return false;
        std::string copy(h->record_json, strnlen(h->record_json, sizeof(h->record_json)));
        std::atomic_thread_fence(std::memory_order_acquire);
        if (h->control_seq.load(std::memory_order_relaxed) != s1) continue;
        last_seq = s1;
        out = std::move(copy);
        return true;
    }
    return false;
}

inline std::wstring mapping_name(int ch) { return L"Local\\denpachan_worker_" + std::to_wstring(ch); }
inline std::wstring stop_event_name(int ch) { return L"Local\\denpachan_worker_" + std::to_wstring(ch) + L"_stop"; }

// コマンドライン全体のハッシュ (FNV-1a)。設定が同じかどうかの判定に使う。
inline uint64_t hash_args(const std::wstring& s) {
    uint64_t h = 1469598103934665603ull;
    for (wchar_t c : s) {
        h ^= uint64_t(c);
        h *= 1099511628211ull;
    }
    return h;
}

inline Header* open_shared(SharedMapping& mapping, int ch) {
    bool created = false;
    if (!mapping.open_or_create(mapping_name(ch), kMappingBytes, created)) return nullptr;
    auto* h = static_cast<Header*>(mapping.data());
    if (created) {
        h->layout_version = kLayoutVersion;
        h->magic.store(kMagic, std::memory_order_release);
    } else {
        for (int i = 0; i < 100 && h->magic.load(std::memory_order_acquire) != kMagic; ++i) Sleep(5);
        if (h->magic.load(std::memory_order_acquire) != kMagic || h->layout_version != kLayoutVersion) {
            mapping.close();
            return nullptr;
        }
    }
    return h;
}

inline void write_metrics(Header* h, const Metrics& m) {
    const uint32_t s = h->metrics_seq.load(std::memory_order_relaxed);
    h->metrics_seq.store(s + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    memcpy(&h->metrics, &m, sizeof(m));
    h->metrics_seq.store(s + 2, std::memory_order_release);
}

inline bool read_metrics(const Header* h, Metrics& out) {
    for (int i = 0; i < 100; ++i) {
        const uint32_t s1 = h->metrics_seq.load(std::memory_order_acquire);
        if (s1 & 1) continue;
        memcpy(&out, &h->metrics, sizeof(out));
        std::atomic_thread_fence(std::memory_order_acquire);
        if (h->metrics_seq.load(std::memory_order_relaxed) == s1) return s1 != 0;
    }
    return false;
}

}  // namespace denpachan::worker_status
