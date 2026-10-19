#pragma once
// プレビュー画像 (worker → UI) の共有メモリ。設計書 §7.1。
//
//   [Header (4KB)] [BGRA 640x360] x 2 (ダブルバッファ)
//
// - UI はプレビューを表示している間 reader_heartbeat_qpc を更新する。途絶えたら worker は縮小処理自体を止める。
// - worker は seq+1 番目の画像を slot (seq+1) % 2 に書き、最後に seq を進める。
//   UI は seq を読んでコピーし、コピー後に seq が 2 以上進んでいたら捨てる。
//
// C# 側 (Denpachan.Ui の PreviewReader) は下のオフセットを直接読むので、レイアウトを変えたら合わせること。

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

#include "denpachan/win_util.h"

namespace denpachan::preview {

inline constexpr uint32_t kMagic = 0x56505044;  // "DPPV"
inline constexpr uint32_t kLayoutVersion = 1;
inline constexpr uint32_t kWidth = 640;
inline constexpr uint32_t kHeight = 360;
inline constexpr uint32_t kStride = kWidth * 4;
inline constexpr size_t kFrameBytes = size_t(kStride) * kHeight;
inline constexpr int kSlots = 2;
inline constexpr size_t kHeaderBytes = 4096;
inline constexpr size_t kMappingBytes = kHeaderBytes + kFrameBytes * kSlots;
inline constexpr int64_t kReaderTimeoutMs = 2000;

struct Header {
    std::atomic<uint32_t> magic;               // 0
    uint32_t layout_version;                   // 4
    uint32_t width;                            // 8
    uint32_t height;                           // 12
    uint32_t stride;                           // 16
    uint32_t slots;                            // 20
    std::atomic<uint64_t> seq;                 // 24: 書き込み完了した画像の番号
    std::atomic<int64_t> reader_heartbeat_qpc; // 32: UI が表示している間更新する
    std::atomic<int64_t> writer_qpc;           // 40: 最後に書いた時刻
};

static_assert(offsetof(Header, seq) == 24);
static_assert(offsetof(Header, reader_heartbeat_qpc) == 32);
static_assert(offsetof(Header, writer_qpc) == 40);

inline std::wstring mapping_name(int ch) { return L"Local\\denpachan_preview_" + std::to_wstring(ch); }

inline uint8_t* slot_data(void* base, int slot) {
    return static_cast<uint8_t*>(base) + kHeaderBytes + kFrameBytes * size_t(slot);
}

inline Header* open_shared(SharedMapping& mapping, int ch) {
    bool created = false;
    if (!mapping.open_or_create(mapping_name(ch), kMappingBytes, created)) return nullptr;
    auto* h = static_cast<Header*>(mapping.data());
    if (h->magic.load(std::memory_order_acquire) != kMagic) {
        // どちらが先に作ってもよいように、まだ初期化されていなければここで初期化する
        h->layout_version = kLayoutVersion;
        h->width = kWidth;
        h->height = kHeight;
        h->stride = kStride;
        h->slots = kSlots;
        h->magic.store(kMagic, std::memory_order_release);
    }
    if (h->layout_version != kLayoutVersion || h->width != kWidth || h->height != kHeight) {
        mapping.close();
        return nullptr;
    }
    return h;
}

}  // namespace denpachan::preview
