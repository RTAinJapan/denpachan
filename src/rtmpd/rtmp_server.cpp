#include "rtmp_server.h"

#include <ws2tcpip.h>

#include <spdlog/spdlog.h>

#include <algorithm>

namespace denpachan::rtmpd {

namespace {

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

// FLV の映像タグ: コーデック設定 (AVC/HEVC の sequence header、Enhanced RTMP の SequenceStart) か
bool is_video_config(const uint8_t* d, uint32_t n) {
    if (n < 2) return false;
    if (d[0] & 0x80) return (d[0] & 0x0F) == 0;  // Enhanced RTMP: PacketType SequenceStart
    const uint8_t codec = d[0] & 0x0F;
    return (codec == 7 || codec == 12) && d[1] == 0;  // AVC / HEVC (旧来の拡張) の sequence header
}

bool is_keyframe(const uint8_t* d, uint32_t n) {
    if (n < 1) return false;
    const uint8_t frame_type = (d[0] & 0x80) ? ((d[0] >> 4) & 0x07) : (d[0] >> 4);
    return frame_type == 1;
}

// FLV の音声タグ: AAC の AudioSpecificConfig か
bool is_audio_config(const uint8_t* d, uint32_t n) { return n >= 2 && (d[0] >> 4) == 10 && d[1] == 0; }

}  // namespace

Server::Server(ServerOptions options) : options_(std::move(options)) {}

Server::~Server() { stop(); }

bool Server::start() {
    routes_ = rtmp_routes::open_shared(routes_mapping_);
    if (!routes_) spdlog::error("cannot open routes shared memory");
    for (int i = 0; i < rtmp_routes::kMaxRoutes; ++i) {
        channels_[i].ring_open = channels_[i].ring.open(i + 1);
        if (!channels_[i].ring_open) spdlog::error("ch{}: cannot open FLV ring", i + 1);
    }

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    addrinfo* res = nullptr;
    const std::string port = std::to_string(options_.port);
    if (getaddrinfo(options_.bind.empty() ? nullptr : options_.bind.c_str(), port.c_str(), &hints, &res) != 0 || !res) {
        spdlog::error("cannot resolve {}:{}", options_.bind, options_.port);
        return false;
    }
    listen_ = socket(res->ai_family, SOCK_STREAM, IPPROTO_TCP);
    BOOL exclusive = TRUE;  // 他のプロセスにポートを横取りされないように
    setsockopt(listen_, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
    const bool ok = listen_ != INVALID_SOCKET && bind(listen_, res->ai_addr, int(res->ai_addrlen)) == 0 && listen(listen_, SOMAXCONN) == 0;
    freeaddrinfo(res);
    if (!ok) {
        spdlog::error("cannot listen on {}:{} (error {})", options_.bind, options_.port, WSAGetLastError());
        if (listen_ != INVALID_SOCKET) closesocket(listen_);
        listen_ = INVALID_SOCKET;
        return false;
    }
    spdlog::info("RTMP: listening on {}:{}", options_.bind, options_.port);
    stop_ = false;
    accept_thread_ = std::thread(&Server::accept_loop, this);
    return true;
}

void Server::stop() {
    stop_ = true;
    if (accept_thread_.joinable()) accept_thread_.join();
    if (listen_ != INVALID_SOCKET) {
        closesocket(listen_);
        listen_ = INVALID_SOCKET;
    }
    std::vector<std::shared_ptr<Connection>> conns;
    {
        std::lock_guard lk(conns_mutex_);
        conns.swap(conns_);
    }
    for (auto& c : conns) {
        c->kick();
        if (c->thread.joinable()) c->thread.join();
    }
}

void Server::accept_loop() {
    while (!stop_) {
        accept_heartbeat_qpc_ = qpc_now();
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(listen_, &fds);
        timeval tv{0, 200'000};
        if (select(0, &fds, nullptr, nullptr, &tv) <= 0) continue;

        sockaddr_storage addr{};
        int len = sizeof(addr);
        const SOCKET s = accept(listen_, reinterpret_cast<sockaddr*>(&addr), &len);
        if (s == INVALID_SOCKET) continue;

        std::lock_guard lk(conns_mutex_);
        // 終わった接続を片付ける
        for (auto it = conns_.begin(); it != conns_.end();) {
            if ((*it)->finished()) {
                if ((*it)->thread.joinable()) (*it)->thread.join();
                it = conns_.erase(it);
            } else {
                ++it;
            }
        }
        if (int(conns_.size()) >= options_.max_connections) {
            spdlog::warn("RTMP: too many connections, rejecting {}", address_string(addr));
            closesocket(s);
            continue;
        }
        BOOL nodelay = TRUE;
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));
        auto conn = std::make_shared<Connection>(*this, s, address_string(addr));
        conn->thread = std::thread([conn] { conn->run(); });
        conns_.push_back(conn);
    }
}

bool Server::read_routes(rtmp_routes::Route (&out)[rtmp_routes::kMaxRoutes]) { return routes_ && rtmp_routes::read_routes(routes_, out); }

int Server::resolve(const std::string& app, const std::string& key) {
    rtmp_routes::Route routes[rtmp_routes::kMaxRoutes];
    if (!read_routes(routes)) return 0;
    for (const auto& r : routes) {
        if (r.enabled && app == r.app && key == r.key && r.ch >= 1 && r.ch <= uint32_t(rtmp_routes::kMaxRoutes)) {
            return int(r.ch);
        }
    }
    return 0;
}

void Server::log_rejected_locked(int ch, Channel& channel, int64_t now) {
    spdlog::warn("ch{}: rejected {} publish(es) in the last {} s (latest from {}): {} is still sending", ch, channel.rejected,
                 kRejectLogMs / 1000, channel.rejected_peer, channel.publisher ? channel.publisher->peer() : "the current publisher");
    channel.rejected = 0;
    channel.rejected_log_qpc = now;
}

bool Server::begin_publish(int ch, Connection* c) {
    auto& channel = channels_[ch - 1];
    std::lock_guard lk(channel.mutex);
    const int64_t now = qpc_now();
    if (channel.publisher && channel.publisher != c) {
        // 今の送出元が送り続けていれば、新しい方を断る (2 つの送出元が同じキーに送ると、置き換えるたびに切られた方が
        // 繋ぎ直して、交互に切り替わり続けるため)。送ってこなくなっていれば (瞬断で半開きの TCP など) 置き換える
        if (now - channel.last_media_qpc < qpc_from_ms(kActivePublisherMs)) {
            // 断られた送出側は数秒ごとに繋ぎ直すので、ログは kRejectLogMs に 1 回、回数を添えてまとめる
            ++channel.rejected;
            channel.rejected_peer = c->peer();
            if (now - channel.rejected_log_qpc >= qpc_from_ms(kRejectLogMs)) log_rejected_locked(ch, channel, now);
            return false;
        }
        spdlog::warn("ch{}: new publish from {} replaces {} (no data for {} ms)", ch, c->peer(), channel.publisher->peer(),
                     qpc_to_us(now - channel.last_media_qpc) / 1000);
        channel.publisher->kick();
    }
    channel.publisher = c;
    channel.last_media_qpc = now;
    channel.metadata.clear();
    channel.audio_config.clear();
    channel.video_config.clear();
    if (channel.ring_open) channel.ring.begin_session(c->peer());
    rtmp_routes::Route routes[rtmp_routes::kMaxRoutes];
    if (read_routes(routes)) {
        channel.app = routes[ch - 1].app;
        channel.key = routes[ch - 1].key;
    }
    return true;
}

void Server::end_publish(int ch, Connection* c) {
    auto& channel = channels_[ch - 1];
    std::lock_guard lk(channel.mutex);
    if (channel.publisher != c) return;  // 新しい publish に置き換えられた
    channel.publisher = nullptr;
    if (channel.ring_open) channel.ring.end_session();
}

bool Server::write_media(int ch, Connection* c, uint8_t type, uint32_t timestamp, const uint8_t* data, uint32_t size) {
    auto& channel = channels_[ch - 1];
    std::lock_guard lk(channel.mutex);
    if (channel.publisher != c) return false;
    channel.last_media_qpc = qpc_now();
    if (!channel.ring_open) return true;

    // 途中から読み始める worker のために、メタデータとコーデック設定を保持する
    const bool video_config = type == 9 && is_video_config(data, size);
    if (type == 18) channel.metadata.assign(data, data + size);
    if (type == 8 && is_audio_config(data, size)) channel.audio_config.assign(data, data + size);
    if (video_config) channel.video_config.assign(data, data + size);

    // worker が途中から読みたがっていれば、キーフレームで新しいセッションを始める
    if (type == 9 && !video_config && is_keyframe(data, size) && channel.ring.resync_requested()) {
        channel.ring.begin_session(c->peer());
        if (!channel.metadata.empty()) channel.ring.write_tag(18, timestamp, channel.metadata.data(), uint32_t(channel.metadata.size()));
        if (!channel.video_config.empty())
            channel.ring.write_tag(9, timestamp, channel.video_config.data(), uint32_t(channel.video_config.size()));
        if (!channel.audio_config.empty())
            channel.ring.write_tag(8, timestamp, channel.audio_config.data(), uint32_t(channel.audio_config.size()));
        spdlog::info("ch{}: resynced the FLV ring at a keyframe", ch);
    }
    channel.ring.write_tag(type, timestamp, data, size);
    return true;
}

void Server::update_tcp_stats(int ch, Connection* c, int32_t rtt_us, uint64_t retrans_bytes) {
    auto& channel = channels_[ch - 1];
    std::lock_guard lk(channel.mutex);
    if (channel.publisher != c || !channel.ring_open) return;
    channel.ring.header()->tcp_rtt_us.store(rtt_us);
    channel.ring.header()->tcp_retrans_bytes.store(retrans_bytes);
}

void Server::housekeeping() {
    rtmp_routes::Route routes[rtmp_routes::kMaxRoutes] = {};
    const bool have_routes = read_routes(routes);
    for (int i = 0; i < rtmp_routes::kMaxRoutes; ++i) {
        auto& channel = channels_[i];
        std::lock_guard lk(channel.mutex);
        if (channel.ring_open) channel.ring.heartbeat();
        // まとめている「断った」の回数を出す (断られ続けていなくても、最後の分が残らないように)
        if (const int64_t now = qpc_now(); channel.rejected > 0 && now - channel.rejected_log_qpc >= qpc_from_ms(kRejectLogMs)) {
            log_rejected_locked(i + 1, channel, now);
        }
        if (!channel.publisher || !have_routes) continue;
        // チャンネルが止められた / キーが変わった: 今の publish を切る
        const auto& r = routes[i];
        if (!r.enabled || channel.app != r.app || channel.key != r.key) {
            spdlog::info("ch{}: route changed, disconnecting {}", i + 1, channel.publisher->peer());
            channel.publisher->kick();
        }
    }
}

}  // namespace denpachan::rtmpd
