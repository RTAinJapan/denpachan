#pragma once
// 仮想カメラ (vcam) の共有メモリレイアウト。
//
// 書き手: worker (受信映像を書く) / 読み手: denpachan-vcam.dll (OBS 等のプロセス内)。
// 設計書 §5.2 を参照。
//
//   [Header (64KB)] [slot0: NV12 最大 4K] [slot1] [slot2]
//
// - 書き手は write_seq+1 番目のフレームを slots[(write_seq+1) % 3] に書き、最後に write_seq を進める。
// - 読み手は write_seq を読んで最新スロットをコピーし、コピー後に write_seq を読み直す。
//   その間に書き手が 2 枚以上進んでいたらスロットが上書きされた可能性があるので捨てる (seqlock 方式)。
// - 読み手は自分の要求フォーマット (OBS が選んだ解像度) を ReaderSlot に書く。
//   書き手は最後に要求された解像度で書くので、コピー量は選ばれた解像度ぶんだけになる。
// - 読み手は ReaderSlot ごとに名前付きイベントを作り、書き手はフレームを書くたびにそれをセットする。
// - 映像が途絶えても、書き手が落ちても、読み手は最後のフレームを出し続ける。
//   NO SIGNAL を出すのは、一度もフレームが書かれていない (write_seq == 0) ときだけ。

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

#include "denpachan/win_util.h"

namespace denpachan::vcam {

inline constexpr uint32_t kMagic = 0x43565044;  // "DPVC"
inline constexpr uint32_t kLayoutVersion = 1;
inline constexpr int kNumCams = 4;
inline constexpr uint32_t kMaxWidth = 3840;
inline constexpr uint32_t kMaxHeight = 2160;
inline constexpr int kNumSlots = 3;
inline constexpr int kMaxReaders = 8;

constexpr size_t align_up(size_t v, size_t a) { return (v + a - 1) / a * a; }

inline constexpr size_t kMaxFrameBytes = size_t(kMaxWidth) * kMaxHeight * 3 / 2;  // NV12
inline constexpr size_t kHeaderBytes = 64 * 1024;
inline constexpr size_t kSlotStride = align_up(kMaxFrameBytes, 64 * 1024);
inline constexpr size_t kMappingBytes = kHeaderBytes + kSlotStride * kNumSlots;

// 読み手 / 書き手のハートビートがこれだけ途絶えたら死んだとみなす
inline constexpr int64_t kReaderTimeoutMs = 3000;
inline constexpr int64_t kWriterTimeoutMs = 1000;

// 書き手の状態 (表示用)。読み手の出す絵は状態ではなく write_seq で決まる:
//   write_seq != 0 → 最新スロットのフレーム (途絶中・書き手が落ちている間も最後のフレームを出し続ける)
//   write_seq == 0 → NO SIGNAL (起動後一度も受信していない / reset_display で明示的にリセットした)
enum class WriterState : uint32_t {
    Absent = 0,    // 書き手なし
    Live = 1,      // 受信中。新しいフレームが来る
    NoSignal = 2,  // 書き手はいるが、まだ一度も映像を受信していない
    Hold = 3,      // 映像が途絶えている。最後のフレームを出し続ける
};

// 要求フォーマット (16bit x4 に詰めて 1 つの atomic で読み書きする)
struct Format {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t fps_num = 0;
    uint32_t fps_den = 0;

    bool valid() const {
        return width >= 16 && height >= 16 && width <= kMaxWidth && height <= kMaxHeight && width % 2 == 0 &&
               height % 2 == 0 && fps_num > 0 && fps_den > 0;
    }
    uint64_t pack() const {
        return (uint64_t(width & 0xFFFF) << 48) | (uint64_t(height & 0xFFFF) << 32) |
               (uint64_t(fps_num & 0xFFFF) << 16) | uint64_t(fps_den & 0xFFFF);
    }
    static Format unpack(uint64_t v) {
        return Format{uint32_t(v >> 48) & 0xFFFF, uint32_t(v >> 32) & 0xFFFF, uint32_t(v >> 16) & 0xFFFF,
                      uint32_t(v) & 0xFFFF};
    }
    size_t nv12_bytes() const { return size_t(width) * height * 3 / 2; }
    bool operator==(const Format&) const = default;
};

inline constexpr Format kDefaultFormat{1920, 1080, 60, 1};

struct SlotInfo {
    std::atomic<uint64_t> seq;  // このスロットに書かれたフレーム番号
    std::atomic<uint32_t> width;
    std::atomic<uint32_t> height;
    std::atomic<int64_t> pts_qpc;  // 書き手が出力した時刻 (QPC)
};

// 読み手スロットの取得は heartbeat_qpc の CAS で行う (0 か期限切れの値 → 現在時刻)。
// 同じプロセス内に読み手が複数いることがある (OBS で同じ Cam を 2 つのソースから開く) ので、
// 持ち主は PID ではなく読み手ごとのトークンで識別する。
struct ReaderSlot {
    std::atomic<int64_t> heartbeat_qpc;  // 0 = 空き
    std::atomic<uint64_t> owner;         // (pid << 32) | プロセス内の連番
    std::atomic<uint32_t> generation;    // イベント作成後に更新する。書き手はこれでイベント名を作る
    std::atomic<uint64_t> format;        // Format::pack()。0 = 未指定
    std::atomic<int64_t> format_qpc;     // format を書いた時刻 (最新の要求を選ぶため)
};

struct Header {
    std::atomic<uint32_t> magic;
    uint32_t layout_version;
    uint32_t header_bytes;
    uint32_t num_slots;
    uint64_t slot_stride;
    uint32_t max_width;
    uint32_t max_height;

    // 書き手
    std::atomic<uint32_t> writer_pid;
    std::atomic<uint32_t> writer_state;  // WriterState
    std::atomic<int64_t> writer_heartbeat_qpc;
    std::atomic<uint64_t> write_seq;  // 0 = まだ 1 枚も書かれていない
    SlotInfo slots[kNumSlots];

    // 読み手
    ReaderSlot readers[kMaxReaders];
};

static_assert(sizeof(Header) <= kHeaderBytes);
static_assert(std::atomic<uint64_t>::is_always_lock_free);
static_assert(std::atomic<int64_t>::is_always_lock_free);

inline std::wstring mapping_name(int cam) { return L"Local\\denpachan_vcam_" + std::to_wstring(cam); }

inline std::wstring reader_event_name(int cam, int slot, uint32_t generation) {
    return L"Local\\denpachan_vcam_" + std::to_wstring(cam) + L"_r" + std::to_wstring(slot) + L"_g" +
           std::to_wstring(generation);
}

inline uint8_t* slot_data(void* base, int slot) {
    return static_cast<uint8_t*>(base) + kHeaderBytes + kSlotStride * size_t(slot);
}

inline bool heartbeat_alive(int64_t heartbeat_qpc, int64_t timeout_ms, int64_t now = qpc_now()) {
    return heartbeat_qpc != 0 && now - heartbeat_qpc < qpc_from_ms(timeout_ms);
}

// 表示を NO SIGNAL に戻す (ユーザーの明示的なリセット)。次に映像を受信したら、その映像に戻る。
inline void reset_display(Header* h) { h->write_seq.store(0, std::memory_order_release); }

// 共有メモリを開く (なければ作って初期化する)。どちらの側から先に開いてもよい。
// 失敗 (別バージョンのレイアウト等) なら nullptr。
inline Header* open_shared(SharedMapping& mapping, int cam) {
    bool created = false;
    if (!mapping.open_or_create(mapping_name(cam), kMappingBytes, created)) return nullptr;
    auto* h = static_cast<Header*>(mapping.data());
    if (created) {
        h->layout_version = kLayoutVersion;
        h->header_bytes = uint32_t(kHeaderBytes);
        h->num_slots = kNumSlots;
        h->slot_stride = kSlotStride;
        h->max_width = kMaxWidth;
        h->max_height = kMaxHeight;
        h->magic.store(kMagic, std::memory_order_release);
    } else {
        // 作成側の初期化を待つ (通常は一瞬)
        for (int i = 0; i < 100 && h->magic.load(std::memory_order_acquire) != kMagic; ++i) Sleep(5);
        if (h->magic.load(std::memory_order_acquire) != kMagic) {
            mapping.close();
            return nullptr;
        }
    }
    if (h->layout_version != kLayoutVersion || h->slot_stride != kSlotStride) {
        mapping.close();
        return nullptr;
    }
    return h;
}

}  // namespace denpachan::vcam
