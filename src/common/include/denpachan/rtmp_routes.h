#pragma once
// RTMP の振り分け表 (engine → rtmpd) の共有メモリ。設計書 §3.3。
//
// engine が設定から書き、rtmpd は publish のたびと 1 秒ごとに読む。チャンネルの追加・停止で
// rtmpd を再起動しないため (他のチャンネルの RTMP 接続を切らない)。

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>

#include "denpachan/channels.h"
#include "denpachan/win_util.h"

namespace denpachan::rtmp_routes {

inline constexpr uint32_t kMagic = 0x52525044;  // "DPRR"
inline constexpr uint32_t kLayoutVersion = 2;   // 2: チャンネル 8 つ分
inline constexpr int kMaxRoutes = kMaxChannels;

struct Route {
    uint32_t enabled;
    uint32_t ch;
    char app[64];
    char key[128];
};

struct Header {
    std::atomic<uint32_t> magic;
    uint32_t layout_version;
    std::atomic<uint32_t> seq;  // 書き込み中は奇数
    Route routes[kMaxRoutes];
};

inline constexpr size_t kMappingBytes = 4096;
static_assert(sizeof(Header) <= kMappingBytes);

inline const wchar_t* mapping_name() { return L"Local\\denpachan_rtmp_routes"; }

inline Header* open_shared(SharedMapping& mapping) {
    bool created = false;
    if (!mapping.open_or_create(mapping_name(), kMappingBytes, created)) return nullptr;
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

inline void write_routes(Header* h, const Route (&routes)[kMaxRoutes]) {
    const uint32_t s = h->seq.load(std::memory_order_relaxed);
    h->seq.store(s + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    memcpy(h->routes, routes, sizeof(routes));
    h->seq.store(s + 2, std::memory_order_release);
}

inline bool read_routes(const Header* h, Route (&out)[kMaxRoutes]) {
    for (int i = 0; i < 1000; ++i) {
        const uint32_t s1 = h->seq.load(std::memory_order_acquire);
        if (s1 & 1) continue;
        memcpy(out, h->routes, sizeof(out));
        std::atomic_thread_fence(std::memory_order_acquire);
        if (h->seq.load(std::memory_order_relaxed) == s1) return true;
    }
    return false;
}

}  // namespace denpachan::rtmp_routes
