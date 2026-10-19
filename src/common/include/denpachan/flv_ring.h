#pragma once
// rtmpd → worker の FLV バイト列リング (共有メモリ)。設計書 §3.3。
//
//   [Header (4KB)] [バイト列 kCapacity]
//
// - 書き手 (rtmpd) は publish ごとに「セッション」を始め、FLV ヘッダに続けて FLV タグを書く。
//   読み手を待たない (遅れていれば上書きする)。
// - 読み手 (worker) は session が変わったら session_start_pos から読み直す (FLV ヘッダから始まる)。
//   publish の途中から読みたいとき (worker の再起動、追い越された) は resync_request を立てると、
//   書き手が次のキーフレームで新しいセッションを始めてくれる (送出側の再接続を待たずに復帰できる)。
// - 書き込みのたびに名前付きイベントをセットする。

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>

#include "denpachan/win_util.h"

namespace denpachan::flv_ring {

inline constexpr uint32_t kMagic = 0x4C465044;  // "DPFL"
inline constexpr uint32_t kLayoutVersion = 1;
inline constexpr uint64_t kCapacity = 8ull * 1024 * 1024;  // 2 の累乗
inline constexpr size_t kHeaderBytes = 4096;
inline constexpr size_t kMappingBytes = kHeaderBytes + size_t(kCapacity);

struct Header {
    std::atomic<uint32_t> magic;
    uint32_t layout_version;
    uint64_t capacity;

    std::atomic<uint64_t> write_pos;          // 書き込み済みバイト数 (単調増加)
    std::atomic<uint64_t> session;            // publish ごとに増える
    std::atomic<uint64_t> session_start_pos;  // そのセッションの先頭 (FLV ヘッダ) の位置
    std::atomic<uint32_t> session_open;       // 1 = publish 中
    std::atomic<int64_t> writer_heartbeat_qpc;
    // 読み手が途中から読み始めたいとき (worker の再起動、追い越し) に 1 にする。書き手は次の
    // キーフレームで新しいセッションを始め、保持しているメタデータとコーデック設定を先に書く。
    std::atomic<uint32_t> resync_request;

    // 統計 (書き手が更新、表示用)
    char peer[64];
    std::atomic<uint64_t> bytes_in;
    std::atomic<int32_t> tcp_rtt_us;
    std::atomic<uint64_t> tcp_retrans_bytes;
};

static_assert(sizeof(Header) <= kHeaderBytes);

inline std::wstring mapping_name(int ch) { return L"Local\\denpachan_flv_" + std::to_wstring(ch); }
inline std::wstring event_name(int ch) { return L"Local\\denpachan_flv_" + std::to_wstring(ch) + L"_data"; }

inline uint8_t* ring_data(void* base) { return static_cast<uint8_t*>(base) + kHeaderBytes; }

inline Header* open_shared(SharedMapping& mapping, int ch) {
    bool created = false;
    if (!mapping.open_or_create(mapping_name(ch), kMappingBytes, created)) return nullptr;
    auto* h = static_cast<Header*>(mapping.data());
    if (created) {
        h->layout_version = kLayoutVersion;
        h->capacity = kCapacity;
        h->magic.store(kMagic, std::memory_order_release);
    } else {
        for (int i = 0; i < 100 && h->magic.load(std::memory_order_acquire) != kMagic; ++i) Sleep(5);
        if (h->magic.load(std::memory_order_acquire) != kMagic || h->layout_version != kLayoutVersion ||
            h->capacity != kCapacity) {
            mapping.close();
            return nullptr;
        }
    }
    return h;
}

// 書き手 (1 チャンネルにつき 1 つ)
class Writer {
public:
    bool open(int ch) {
        header_ = open_shared(mapping_, ch);
        if (!header_) return false;
        data_ = ring_data(mapping_.data());
        SharedSecurity sec;
        event_.reset(CreateEventW(sec.get(), FALSE, FALSE, event_name(ch).c_str()));
        return true;
    }

    // publish の開始: FLV ヘッダを書いてセッションを進める
    void begin_session(const std::string& peer) {
        const uint64_t start = header_->write_pos.load(std::memory_order_relaxed);
        static const uint8_t flv_header[13] = {'F', 'L', 'V', 1, 0x05, 0, 0, 0, 9, 0, 0, 0, 0};  // 音声+映像
        append(flv_header, sizeof(flv_header));
        strncpy_s(header_->peer, peer.c_str(), _TRUNCATE);
        header_->bytes_in.store(0);
        header_->resync_request.store(0);
        header_->session_start_pos.store(start, std::memory_order_relaxed);
        header_->session_open.store(1, std::memory_order_relaxed);
        header_->session.fetch_add(1, std::memory_order_release);
        signal();
    }

    void end_session() {
        header_->session_open.store(0, std::memory_order_release);
        signal();
    }

    // FLV タグ 1 つ (type: 8 音声 / 9 映像 / 18 スクリプト)
    void write_tag(uint8_t type, uint32_t timestamp, const uint8_t* payload, uint32_t size) {
        uint8_t tag[11] = {type,
                           uint8_t(size >> 16), uint8_t(size >> 8), uint8_t(size),
                           uint8_t(timestamp >> 16), uint8_t(timestamp >> 8), uint8_t(timestamp), uint8_t(timestamp >> 24),
                           0, 0, 0};
        const uint32_t prev = size + 11;
        const uint8_t trailer[4] = {uint8_t(prev >> 24), uint8_t(prev >> 16), uint8_t(prev >> 8), uint8_t(prev)};
        append(tag, sizeof(tag), false);
        append(payload, size, false);
        append(trailer, sizeof(trailer));
        header_->bytes_in.fetch_add(size + 15, std::memory_order_relaxed);
    }

    void heartbeat() { header_->writer_heartbeat_qpc.store(qpc_now(), std::memory_order_relaxed); }
    bool resync_requested() const { return header_->resync_request.load(std::memory_order_acquire) != 0; }
    Header* header() const { return header_; }

private:
    void append(const uint8_t* src, size_t n, bool publish = true) {
        uint64_t pos = pending_pos_ ? pending_pos_ : header_->write_pos.load(std::memory_order_relaxed);
        while (n > 0) {
            const size_t offset = size_t(pos & (kCapacity - 1));
            const size_t chunk = std::min<size_t>(n, size_t(kCapacity) - offset);
            memcpy(data_ + offset, src, chunk);
            src += chunk;
            n -= chunk;
            pos += chunk;
        }
        if (publish) {
            header_->write_pos.store(pos, std::memory_order_release);
            pending_pos_ = 0;
            signal();
        } else {
            pending_pos_ = pos;  // タグを書き終えるまで読み手に見せない
        }
    }
    void signal() {
        if (event_) SetEvent(event_.get());
    }

    SharedMapping mapping_;
    Header* header_ = nullptr;
    uint8_t* data_ = nullptr;
    UniqueHandle event_;
    uint64_t pending_pos_ = 0;
};

}  // namespace denpachan::flv_ring
