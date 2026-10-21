#include "flv_ring_ingest.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <vector>

namespace denpachan::worker {

using namespace denpachan::flv_ring;

FlvRingIngest::FlvRingIngest(int ch, ByteQueue& queue, ChannelMetrics& metrics) : ch_(ch), queue_(queue), metrics_(metrics) {}

FlvRingIngest::~FlvRingIngest() { stop(); }

bool FlvRingIngest::start() {
    stop_ = false;
    thread_ = std::thread(&FlvRingIngest::run, this);
    return true;
}

void FlvRingIngest::stop() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
}

void FlvRingIngest::run() {
    SharedMapping mapping;
    Header* h = nullptr;
    UniqueHandle event;
    std::vector<uint8_t> chunk(64 * 1024);

    uint64_t session = 0;  // 読んでいるセッション
    bool active = false;   // ByteQueue のセッションを開いているか
    uint64_t read_pos = 0;
    int64_t last_stats = 0;

    auto close_session = [&] {
        if (!active) return;
        active = false;
        queue_.end_session();
        metrics_.connected = false;
        spdlog::info("RTMP: publish ended");
    };

    while (!stop_) {
        metrics_.heartbeat_ingest();
        if (!h) {
            h = open_shared(mapping, ch_);
            if (!h) {
                Sleep(500);
                continue;
            }
            SharedSecurity sec;
            event.reset(CreateEventW(sec.get(), FALSE, FALSE, event_name(ch_).c_str()));
            session = h->session.load(std::memory_order_acquire);
            if (h->session_open.load(std::memory_order_acquire)) {
                // publish の途中 (worker の再起動など): 次のキーフレームから読めるよう rtmpd に頼む
                h->resync_request.store(1, std::memory_order_release);
                spdlog::info("RTMP: publish in progress, requesting resync at the next keyframe");
            } else {
                spdlog::info("RTMP: waiting for publish (ch {})", ch_);
            }
        }
        WaitForSingleObject(event.get(), 100);

        // 新しい publish
        const uint64_t s = h->session.load(std::memory_order_acquire);
        if (s != session) {
            close_session();
            session = s;
            if (h->session_open.load(std::memory_order_acquire)) {
                // 前の publish のために頼まれていた再同期は捨てる。残っていると、新しい publish を始めた途端に切ってしまい、
                // その publish でもデコーダが読み始められずにまた頼む、を繰り返す (キーフレームごとにデコーダを開き直す)
                resync_.store(false);
                read_pos = h->session_start_pos.load(std::memory_order_relaxed);
                active = true;
                queue_.begin_session();
                metrics_.connected = true;
                ++metrics_.connections;
                {
                    std::lock_guard lk(metrics_.mutex);
                    metrics_.peer = h->peer;
                    metrics_.srt = SrtStats{};
                    metrics_.last_error.clear();
                }
                spdlog::info("RTMP: publish from {}", h->peer);
            }
        }
        if (!active) continue;

        if (resync_.exchange(false)) {
            // デコーダが読み始められなかった (FLV は途中からでは読めない): 次のキーフレームからやり直す
            spdlog::info("RTMP: requesting resync at the next keyframe (decoder could not start)");
            close_session();
            h->resync_request.store(1, std::memory_order_release);
            continue;
        }

        const uint64_t write_pos = h->write_pos.load(std::memory_order_acquire);
        if (write_pos - read_pos > kCapacity) {
            // 追い越された (デコードが詰まっている)。FLV の区切りを見失うので、次のキーフレームからやり直す。
            spdlog::warn("RTMP: ring overrun, requesting resync at the next keyframe");
            metrics_.overflow_bytes += write_pos - read_pos;
            close_session();
            h->resync_request.store(1, std::memory_order_release);
            continue;
        }
        while (read_pos < write_pos) {
            const size_t offset = size_t(read_pos & (kCapacity - 1));
            const size_t n = std::min<size_t>({size_t(write_pos - read_pos), chunk.size(), size_t(kCapacity) - offset});
            memcpy(chunk.data(), ring_data(mapping.data()) + offset, n);
            if (!queue_.push(chunk.data(), n)) metrics_.overflow_bytes += n;
            metrics_.bytes_in += n;
            read_pos += n;
        }
        if (!h->session_open.load(std::memory_order_acquire)) close_session();

        // 書き手 (rtmpd) が落ちて起動し直されないと、セッションは「publish 中」のまま残る。ハートビート (1 秒ごと) が
        // kWriterTimeoutMs 止まったら、publish は終わったとみなす (受信中のまま固まらないように)。
        // rtmpd が止まっていただけ (システム全体の一時停止など) で同じ publish が続いていれば、
        // 区切り直しを頼んでおくと、次のキーフレームから新しいセッションとして読み直せる
        if (active && qpc_now() - h->writer_heartbeat_qpc.load(std::memory_order_relaxed) > qpc_from_ms(kWriterTimeoutMs)) {
            spdlog::warn("RTMP: rtmpd is not responding; ending the publish (resync at the next keyframe if it resumes)");
            close_session();
            h->resync_request.store(1, std::memory_order_release);
            continue;
        }

        if (const int64_t now = qpc_now(); now - last_stats > qpc_from_ms(1000)) {
            last_stats = now;
            metrics_.tcp_rtt_us = h->tcp_rtt_us.load();
            metrics_.tcp_retrans_bytes = h->tcp_retrans_bytes.load();
        }
    }
    close_session();
}

}  // namespace denpachan::worker
