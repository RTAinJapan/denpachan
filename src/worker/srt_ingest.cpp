#include "srt_ingest.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <srt/srt.h>
#include <spdlog/spdlog.h>

#include <algorithm>

namespace denpachan::worker {

namespace {

// srt.h の operator| は constexpr ではないので const にする
const int kEpollEvents = SRT_EPOLL_IN | SRT_EPOLL_ERR;

bool resolve(const std::string& host, int port, bool passive, sockaddr_storage& out, int& len) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags = passive ? AI_PASSIVE : 0;
    addrinfo* res = nullptr;
    const std::string service = std::to_string(port);
    if (getaddrinfo(host.empty() ? nullptr : host.c_str(), service.c_str(), &hints, &res) != 0 || !res) return false;
    memcpy(&out, res->ai_addr, res->ai_addrlen);
    len = int(res->ai_addrlen);
    freeaddrinfo(res);
    return true;
}

std::string address_string(const sockaddr_storage& addr) {
    char host[INET6_ADDRSTRLEN] = {};
    int port = 0;
    if (addr.ss_family == AF_INET) {
        const auto* in = reinterpret_cast<const sockaddr_in*>(&addr);
        inet_ntop(AF_INET, &in->sin_addr, host, sizeof(host));
        port = ntohs(in->sin_port);
    } else if (addr.ss_family == AF_INET6) {
        const auto* in6 = reinterpret_cast<const sockaddr_in6*>(&addr);
        inet_ntop(AF_INET6, &in6->sin6_addr, host, sizeof(host));
        port = ntohs(in6->sin6_port);
    }
    return std::string(host) + ":" + std::to_string(port);
}

const char* last_error() { return srt_getlasterror_str(); }

}  // namespace

SrtIngest::SrtIngest(SrtOptions options, ByteQueue& queue, ChannelMetrics& metrics)
    : options_(std::move(options)),
      queue_(queue),
      metrics_(metrics),
      stop_event_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}

SrtIngest::~SrtIngest() { stop(); }

bool SrtIngest::start() {
    if (options_.port <= 0 || options_.port > 65535) {
        spdlog::error("SRT: invalid port {}", options_.port);
        return false;
    }
    stop_ = false;
    ResetEvent(stop_event_.get());
    thread_ = std::thread([this] {
        if (options_.mode == "caller")
            run_caller();
        else
            run_listener();
    });
    return true;
}

void SrtIngest::stop() {
    stop_ = true;
    SetEvent(stop_event_.get());
    if (thread_.joinable()) thread_.join();
}

bool SrtIngest::sleep_interruptible(int ms) {
    // 待っている間もハートビートを出す (engine に固まったと思われないように)
    while (ms > 0) {
        metrics_.heartbeat_ingest();
        const int chunk = std::min(ms, 500);
        if (WaitForSingleObject(stop_event_.get(), DWORD(chunk)) != WAIT_TIMEOUT) return false;
        ms -= chunk;
    }
    metrics_.heartbeat_ingest();
    return true;
}

bool SrtIngest::configure(int sock, bool listener) {
    auto set_int = [&](SRT_SOCKOPT opt, int value, const char* name) {
        if (srt_setsockflag(sock, opt, &value, sizeof(value)) == SRT_ERROR) {
            spdlog::error("SRT: setsockflag {} failed: {}", name, last_error());
            return false;
        }
        return true;
    };

    bool ok = set_int(SRTO_TRANSTYPE, SRTT_LIVE, "TRANSTYPE") && set_int(SRTO_LATENCY, options_.latency_ms, "LATENCY");
    if (ok && listener) ok = set_int(SRTO_RCVSYN, 0, "RCVSYN");
    if (ok && !listener) ok = set_int(SRTO_CONNTIMEO, 3000, "CONNTIMEO");
    if (ok && !options_.passphrase.empty()) {
        if (srt_setsockflag(sock, SRTO_PASSPHRASE, options_.passphrase.data(), int(options_.passphrase.size())) ==
            SRT_ERROR) {
            spdlog::error("SRT: passphrase rejected (10-79 chars): {}", last_error());
            ok = false;
        }
        if (ok && options_.pbkeylen) ok = set_int(SRTO_PBKEYLEN, options_.pbkeylen, "PBKEYLEN");
    }
    if (ok && !listener && !options_.streamid.empty()) {
        ok = srt_setsockflag(sock, SRTO_STREAMID, options_.streamid.data(), int(options_.streamid.size())) !=
             SRT_ERROR;
    }
    return ok;
}

void SrtIngest::update_stats(int sock) {
    SRT_TRACEBSTATS perf{};
    if (srt_bstats(sock, &perf, 1) == SRT_ERROR) return;
    // latency の推奨値 (接続ごと。1 秒ごとの区間の値を溜める)
    if (sock != advisor_sock_) {
        advisor_.reset();
        advisor_sock_ = sock;
    }
    advisor_.add(perf.msRTT, perf.pktRecv, perf.pktRcvLoss, perf.pktRcvDrop);
    const SrtAdvice advice = advisor_.advice(perf.msRcvTsbPdDelay);
    std::lock_guard lk(metrics_.mutex);
    metrics_.srt.advice = advice;
    auto& s = metrics_.srt;
    s.rtt_ms = perf.msRTT;
    s.recv_mbps = perf.mbpsRecvRate;
    s.bandwidth_mbps = perf.mbpsBandwidth;
    s.lost_total = perf.pktRcvLossTotal;
    s.retrans_total += perf.pktRcvRetrans;
    s.dropped_total = perf.pktRcvDropTotal;
    s.rcv_buf_ms = perf.msRcvBuf;
    s.tsbpd_delay_ms = perf.msRcvTsbPdDelay;
}

namespace {

// 読めるだけ読んでキューに入れる。接続が壊れていたら false。
bool pump(int sock, ByteQueue& queue, ChannelMetrics& metrics) {
    char buf[1500];
    for (;;) {
        const int r = srt_recvmsg(sock, buf, sizeof(buf));
        if (r > 0) {
            metrics.bytes_in += uint64_t(r);
            if (!queue.push(reinterpret_cast<const uint8_t*>(buf), size_t(r))) metrics.overflow_bytes += uint64_t(r);
            continue;
        }
        if (r == SRT_ERROR) {
            if (srt_getlasterror(nullptr) == SRT_EASYNCRCV) return true;
            spdlog::warn("SRT: recv failed: {}", last_error());
            return false;
        }
        return true;
    }
}

bool broken(int sock) { return srt_getsockstate(sock) >= SRTS_BROKEN; }

}  // namespace

void SrtIngest::run_listener() {
    while (!stop_) {
        const SRTSOCKET lsn = srt_create_socket();
        sockaddr_storage addr{};
        int addr_len = 0;
        if (lsn == SRT_INVALID_SOCK || !configure(lsn, true) ||
            !resolve(options_.host, options_.port, true, addr, addr_len) ||
            srt_bind(lsn, reinterpret_cast<sockaddr*>(&addr), addr_len) == SRT_ERROR ||
            srt_listen(lsn, 4) == SRT_ERROR) {
            spdlog::error("SRT: cannot listen on {}:{}: {}", options_.host, options_.port, last_error());
            {
                std::lock_guard lk(metrics_.mutex);
                metrics_.last_error = std::string("cannot listen: ") + last_error();
            }
            if (lsn != SRT_INVALID_SOCK) srt_close(lsn);
            if (!sleep_interruptible(2000)) return;
            continue;
        }

        const int eid = srt_epoll_create();
        srt_epoll_add_usock(eid, lsn, &kEpollEvents);
        spdlog::info("SRT: listening on {}:{} (latency {} ms)", options_.host, options_.port, options_.latency_ms);

        SRTSOCKET client = SRT_INVALID_SOCK;
        auto close_client = [&] {
            if (client == SRT_INVALID_SOCK) return;
            srt_epoll_remove_usock(eid, client);
            srt_close(client);
            client = SRT_INVALID_SOCK;
            queue_.end_session();
            metrics_.connected = false;
        };

        int64_t last_stats = qpc_now();
        while (!stop_) {
            metrics_.heartbeat_ingest();
            SRT_EPOLL_EVENT events[4];
            const int n = srt_epoll_uwait(eid, events, 4, 100);
            for (int i = 0; i < n; ++i) {
                if (events[i].fd == lsn) {
                    sockaddr_storage peer{};
                    int peer_len = sizeof(peer);
                    const SRTSOCKET c = srt_accept(lsn, reinterpret_cast<sockaddr*>(&peer), &peer_len);
                    if (c == SRT_INVALID_SOCK) continue;
                    if (client != SRT_INVALID_SOCK) {
                        spdlog::warn("SRT: new connection from {} replaces the current one", address_string(peer));
                        close_client();
                    }
                    client = c;
                    const int no = 0;
                    srt_setsockflag(client, SRTO_RCVSYN, &no, sizeof(no));
                    srt_epoll_add_usock(eid, client, &kEpollEvents);
                    queue_.begin_session();
                    metrics_.connected = true;
                    ++metrics_.connections;
                    {
                        std::lock_guard lk(metrics_.mutex);
                        metrics_.peer = address_string(peer);
                        metrics_.srt = SrtStats{};
                    }
                    spdlog::info("SRT: connected from {}", address_string(peer));
                } else if (events[i].fd == client) {
                    if ((events[i].events & SRT_EPOLL_ERR) || !pump(client, queue_, metrics_)) {
                        spdlog::info("SRT: disconnected");
                        close_client();
                    }
                }
            }
            if (client != SRT_INVALID_SOCK && broken(client)) {
                spdlog::info("SRT: connection broken");
                close_client();
            }
            if (const int64_t now = qpc_now(); now - last_stats >= qpc_from_ms(1000)) {
                last_stats = now;
                if (client != SRT_INVALID_SOCK) update_stats(client);
            }
        }
        close_client();
        srt_epoll_release(eid);
        srt_close(lsn);
    }
}

void SrtIngest::run_caller() {
    int backoff_ms = 500;
    while (!stop_) {
        metrics_.heartbeat_ingest();  // srt_connect は最大 3 秒 (CONNTIMEO) ブロックする
        const SRTSOCKET sock = srt_create_socket();
        sockaddr_storage addr{};
        int addr_len = 0;
        // 繋がらない理由 (画面に出す)。名前が引けない / 応答がない / 拒否された を分ける
        std::string reason;
        if (sock == SRT_INVALID_SOCK || !configure(sock, false)) {
            reason = std::string("SRT の準備に失敗しました: ") + last_error();
        } else if (!resolve(options_.host, options_.port, false, addr, addr_len)) {
            reason = "名前を引けません (" + options_.host + ")";
        } else if (srt_connect(sock, reinterpret_cast<sockaddr*>(&addr), addr_len) == SRT_ERROR) {
            const int code = srt_getlasterror(nullptr);
            if (code == SRT_ECONNREJ) {
                reason = std::string("接続を拒否されました: ") + srt_rejectreason_str(srt_getrejectreason(sock)) +
                         " (パスフレーズ、Stream ID、相手の許可設定を確かめる)";
            } else if (code == SRT_ENOSERVER) {
                reason = "応答がありません (相手が動いていない、アドレスやポートが違う、ファイアウォール)";
            } else {
                reason = std::string("接続できません: ") + last_error();
            }
        }
        if (!reason.empty()) {
            spdlog::warn("SRT: connect to {}:{} failed: {} (retry in {} ms)", options_.host, options_.port, reason,
                         backoff_ms);
            {
                std::lock_guard lk(metrics_.mutex);
                metrics_.last_error = reason;
            }
            if (sock != SRT_INVALID_SOCK) srt_close(sock);
            if (!sleep_interruptible(backoff_ms)) return;
            backoff_ms = std::min(backoff_ms * 2, 5000);
            continue;
        }
        backoff_ms = 500;

        const int no = 0;
        srt_setsockflag(sock, SRTO_RCVSYN, &no, sizeof(no));
        const int eid = srt_epoll_create();
        srt_epoll_add_usock(eid, sock, &kEpollEvents);
        queue_.begin_session();
        metrics_.connected = true;
        ++metrics_.connections;
        {
            std::lock_guard lk(metrics_.mutex);
            metrics_.peer = options_.host + ":" + std::to_string(options_.port);
            metrics_.srt = SrtStats{};
            metrics_.last_error.clear();
        }
        spdlog::info("SRT: connected to {}:{}", options_.host, options_.port);

        int64_t last_stats = qpc_now();
        while (!stop_) {
            metrics_.heartbeat_ingest();
            SRT_EPOLL_EVENT events[1];
            const int n = srt_epoll_uwait(eid, events, 1, 100);
            if (n > 0 && ((events[0].events & SRT_EPOLL_ERR) || !pump(sock, queue_, metrics_))) break;
            if (broken(sock)) break;
            if (const int64_t now = qpc_now(); now - last_stats >= qpc_from_ms(1000)) {
                last_stats = now;
                update_stats(sock);
            }
        }
        spdlog::info("SRT: disconnected");
        queue_.end_session();
        metrics_.connected = false;
        srt_epoll_release(eid);
        srt_close(sock);
    }
}

}  // namespace denpachan::worker
