#include "rtmp_server.h"  // winsock2.h を先に

#include <mstcpip.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstring>
#include <random>

namespace denpachan::rtmpd {

namespace {

constexpr size_t kHandshakeSize = 1536;
constexpr uint32_t kMaxMessageSize = 16 * 1024 * 1024;
constexpr uint32_t kMaxChunkSize = 16 * 1024 * 1024;
constexpr uint32_t kOutChunkSize = 4096;
constexpr uint32_t kWindowAckSize = 2'500'000;
constexpr uint32_t kPublishStreamId = 1;
constexpr size_t kMaxChunkStreams = 64;  // 1 接続あたりのチャンクストリーム数 (送出側は数個しか使わない)

// メッセージ種別
constexpr uint8_t kSetChunkSize = 1;
constexpr uint8_t kAbort = 2;
constexpr uint8_t kAck = 3;
constexpr uint8_t kUserControl = 4;
constexpr uint8_t kWindowAck = 5;
constexpr uint8_t kSetPeerBandwidth = 6;
constexpr uint8_t kAudio = 8;
constexpr uint8_t kVideo = 9;
constexpr uint8_t kDataAmf3 = 15;
constexpr uint8_t kCommandAmf3 = 17;
constexpr uint8_t kDataAmf0 = 18;
constexpr uint8_t kCommandAmf0 = 20;

uint32_t be24(const uint8_t* p) { return (uint32_t(p[0]) << 16) | (uint32_t(p[1]) << 8) | p[2]; }
uint32_t be32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
uint32_t le32(const uint8_t* p) { return (uint32_t(p[3]) << 24) | (uint32_t(p[2]) << 16) | (uint32_t(p[1]) << 8) | p[0]; }
void put_be32(uint8_t* p, uint32_t v) {
    p[0] = uint8_t((v >> 24) & 0xFF);
    p[1] = uint8_t((v >> 16) & 0xFF);
    p[2] = uint8_t((v >> 8) & 0xFF);
    p[3] = uint8_t(v & 0xFF);
}

// "live/" や "live?x=y" を "live" に
std::string normalize_app(std::string app) {
    if (const size_t q = app.find('?'); q != std::string::npos) app.resize(q);
    while (!app.empty() && app.back() == '/') app.pop_back();
    return app;
}

std::string normalize_key(std::string key) {
    if (const size_t q = key.find('?'); q != std::string::npos) key.resize(q);
    return key;
}

}  // namespace

Connection::Connection(Server& server, SOCKET sock, std::string peer) : server_(server), sock_(sock), peer_(std::move(peer)) {}

Connection::~Connection() {
    if (sock_ != INVALID_SOCKET) closesocket(sock_);
}

void Connection::kick() {
    kicked_ = true;
    shutdown(sock_, SD_BOTH);
}

bool Connection::send_all(const uint8_t* data, size_t size) {
    while (size > 0) {
        const int n = send(sock_, reinterpret_cast<const char*>(data), int(std::min<size_t>(size, 1 << 20)), 0);
        if (n <= 0) return false;
        data += n;
        size -= size_t(n);
    }
    return true;
}

bool Connection::recv_exact(uint8_t* buf, size_t n) {
    while (n > 0) {
        if (kicked_ || server_.stopping()) return false;
        const int r = recv(sock_, reinterpret_cast<char*>(buf), int(std::min<size_t>(n, 1 << 20)), 0);
        const int64_t now = qpc_now();
        if (r > 0) {
            buf += r;
            n -= size_t(r);
            last_data_qpc_ = now;
            total_in_ += uint64_t(r);
            if (ack_window_ != 0 && total_in_ - last_ack_ >= ack_window_) {
                last_ack_ = total_in_;
                if (!send_control(kAck, uint32_t(total_in_))) return false;
            }
        } else if (r == 0) {
            return false;  // 切断
        } else if (WSAGetLastError() != WSAETIMEDOUT) {
            return false;
        } else if (now - last_data_qpc_ > qpc_from_ms(server_.options().idle_timeout_ms)) {
            spdlog::info("RTMP: {} idle timeout", peer_);
            return false;
        }
        if (publishing_ && now - last_stats_qpc_ > qpc_from_ms(1000)) {
            last_stats_qpc_ = now;
            update_tcp_stats();
        }
    }
    return true;
}

void Connection::update_tcp_stats() {
    DWORD version = 0;
    TCP_INFO_v0 info{};
    DWORD bytes = 0;
    if (WSAIoctl(sock_, SIO_TCP_INFO, &version, sizeof(version), &info, sizeof(info), &bytes, nullptr, nullptr) == 0) {
        server_.update_tcp_stats(ch_, this, int32_t(info.RttUs), info.BytesRetrans);
    }
}

bool Connection::handshake() {
    // C0 + C1 → S0 + S1 + S2 → C2 (単純ハンドシェイク。S1 の版数欄を 0 にすると、
    // complex ハンドシェイクを送ってくるクライアント (FFmpeg、nginx-rtmp、OBS) も受け入れる)
    std::vector<uint8_t> c0c1(1 + kHandshakeSize);
    if (!recv_exact(c0c1.data(), c0c1.size())) return false;
    if (c0c1[0] != 3) {
        spdlog::warn("RTMP: {} unsupported handshake version {}", peer_, c0c1[0]);
        return false;
    }
    std::vector<uint8_t> s(1 + 2 * kHandshakeSize, 0);
    s[0] = 3;
    std::mt19937 rng(std::random_device{}());
    for (size_t i = 9; i < 1 + kHandshakeSize; ++i) s[i] = uint8_t(rng());   // S1: time(4) zero(4) random
    memcpy(s.data() + 1 + kHandshakeSize, c0c1.data() + 1, kHandshakeSize);  // S2 = C1
    if (!send_all(s.data(), s.size())) return false;
    std::vector<uint8_t> c2(kHandshakeSize);
    return recv_exact(c2.data(), c2.size());
}

bool Connection::send_message(uint32_t csid, uint8_t type, uint32_t stream_id, uint32_t timestamp, const uint8_t* data, size_t size) {
    std::vector<uint8_t> out;
    out.reserve(size + 32);
    const bool ext = timestamp >= 0xFFFFFF;
    const uint32_t ts = ext ? 0xFFFFFF : timestamp;
    out.push_back(uint8_t(csid & 0x3F));  // fmt 0
    out.insert(out.end(), {uint8_t(ts >> 16), uint8_t(ts >> 8), uint8_t(ts)});
    out.insert(out.end(), {uint8_t(size >> 16), uint8_t(size >> 8), uint8_t(size)});
    out.push_back(type);
    out.insert(out.end(), {uint8_t(stream_id), uint8_t(stream_id >> 8), uint8_t(stream_id >> 16), uint8_t(stream_id >> 24)});
    auto put_ext = [&] {
        if (ext) out.insert(out.end(), {uint8_t(timestamp >> 24), uint8_t(timestamp >> 16), uint8_t(timestamp >> 8), uint8_t(timestamp)});
    };
    put_ext();
    size_t offset = 0;
    while (true) {
        const size_t n = std::min<size_t>(size - offset, out_chunk_size_);
        out.insert(out.end(), data + offset, data + offset + n);
        offset += n;
        if (offset >= size) break;
        out.push_back(uint8_t(0xC0 | (csid & 0x3F)));  // fmt 3
        put_ext();
    }
    return send_all(out.data(), out.size());
}

bool Connection::send_control(uint8_t type, uint32_t value) {
    const uint8_t data[4] = {uint8_t(value >> 24), uint8_t(value >> 16), uint8_t(value >> 8), uint8_t(value)};
    return send_message(2, type, 0, 0, data, sizeof(data));
}

bool Connection::send_command(uint32_t csid, uint32_t stream_id, const AmfWriter& w) {
    return send_message(csid, kCommandAmf0, stream_id, 0, w.data().data(), w.data().size());
}

bool Connection::read_chunk() {
    uint8_t b0;
    if (!recv_exact(&b0, 1)) return false;
    const uint8_t fmt = b0 >> 6;
    uint32_t csid = b0 & 0x3F;
    if (csid == 0) {
        uint8_t b;
        if (!recv_exact(&b, 1)) return false;
        csid = 64 + b;
    } else if (csid == 1) {
        uint8_t b[2];
        if (!recv_exact(b, 2)) return false;
        csid = 64 + b[0] + (uint32_t(b[1]) << 8);
    }
    if (streams_.size() >= kMaxChunkStreams && !streams_.contains(csid)) {
        spdlog::warn("RTMP: {} too many chunk streams", peer_);
        return false;
    }
    auto& cs = streams_[csid];
    const bool new_message = cs.payload.empty();

    uint8_t h[11];
    const size_t header_len = fmt == 0 ? 11 : fmt == 1 ? 7 : fmt == 2 ? 3 : 0;
    if (header_len && !recv_exact(h, header_len)) return false;
    uint32_t ts_field = 0;
    if (fmt <= 2) {
        ts_field = be24(h);
        cs.extended = ts_field == 0xFFFFFF;
    }
    if (fmt <= 1) {
        cs.length = be24(h + 3);
        cs.type = h[6];
        if (cs.length >= kMaxMessageSize) {  // FLV のタグ長は 24 bit
            spdlog::warn("RTMP: {} message too large ({} bytes)", peer_, cs.length);
            return false;
        }
    }
    if (fmt == 0) cs.stream_id = le32(h + 7);
    if (cs.extended) {
        uint8_t e[4];
        if (!recv_exact(e, 4)) return false;
        if (fmt <= 2) ts_field = be32(e);
    }
    if (new_message) {
        switch (fmt) {
        case 0:
            cs.timestamp = ts_field;
            cs.delta = ts_field;  // 仕様: fmt 0 の後の fmt 3 は、この値を差分として使う
            break;
        case 1:
        case 2:
            cs.delta = ts_field;
            cs.timestamp += ts_field;
            break;
        default:
            cs.timestamp += cs.delta;
            break;
        }
    }

    const uint32_t remaining = cs.length - uint32_t(cs.payload.size());
    const uint32_t n = std::min(remaining, in_chunk_size_);
    const size_t old = cs.payload.size();
    cs.payload.resize(old + n);
    if (n > 0 && !recv_exact(cs.payload.data() + old, n)) return false;
    if (cs.payload.size() < cs.length) return true;

    std::vector<uint8_t> payload;
    payload.swap(cs.payload);
    return handle_message(cs.type, cs.stream_id, cs.timestamp, payload);
}

bool Connection::handle_message(uint8_t type, uint32_t stream_id, uint32_t timestamp, std::vector<uint8_t>& payload) {
    (void)stream_id;
    switch (type) {
    case kSetChunkSize:
        if (payload.size() >= 4) {
            const uint32_t size = be32(payload.data()) & 0x7FFFFFFF;
            if (size < 1 || size > kMaxChunkSize) return false;
            in_chunk_size_ = size;
        }
        return true;
    case kAbort:
        if (payload.size() >= 4) {
            if (auto it = streams_.find(be32(payload.data())); it != streams_.end()) it->second.payload.clear();
        }
        return true;
    case kWindowAck:
        if (payload.size() >= 4) ack_window_ = be32(payload.data());
        return true;
    case kUserControl:
        // Ping Request (6) には Ping Response (7) を返す
        if (payload.size() >= 6 && payload[0] == 0 && payload[1] == 6) {
            uint8_t pong[6] = {0, 7, payload[2], payload[3], payload[4], payload[5]};
            return send_message(2, kUserControl, 0, 0, pong, sizeof(pong));
        }
        return true;
    case kAck:
    case kSetPeerBandwidth:
        return true;
    case kAudio:
    case kVideo:
        if (!publishing_) return true;
        return server_.write_media(ch_, this, type, timestamp, payload.data(), uint32_t(payload.size()));
    case kDataAmf3:
    case kDataAmf0: {
        if (!publishing_) return true;
        const uint8_t* p = payload.data();
        size_t size = payload.size();
        if (type == kDataAmf3 && size > 0) {
            ++p;
            --size;
        }
        // "@setDataFrame" を外して onMetaData から始まるスクリプトタグにする
        static const char kSetDataFrame[] = "@setDataFrame";
        const size_t marker_len = 3 + sizeof(kSetDataFrame) - 1;
        if (size > marker_len && p[0] == 0x02 && memcmp(p + 3, kSetDataFrame, sizeof(kSetDataFrame) - 1) == 0) {
            p += marker_len;
            size -= marker_len;
        }
        return server_.write_media(ch_, this, kDataAmf0, timestamp, p, uint32_t(size));
    }
    case kCommandAmf3:
        if (payload.empty()) return true;
        return handle_command(amf0_decode(payload.data() + 1, payload.size() - 1));
    case kCommandAmf0:
        return handle_command(amf0_decode(payload.data(), payload.size()));
    default:
        return true;
    }
}

bool Connection::handle_command(const std::vector<AmfValue>& args) {
    if (args.size() < 2 || args[0].type != AmfValue::Type::String) return true;
    const std::string& name = args[0].str;
    const double txn = args[1].type == AmfValue::Type::Number ? args[1].number : 0;

    if (name == "connect") {
        if (args.size() >= 3 && args[2].type == AmfValue::Type::Object) app_ = normalize_app(args[2].get_string("app"));
        out_chunk_size_ = kOutChunkSize;
        uint8_t bw[5];
        put_be32(bw, kWindowAckSize);
        bw[4] = 2;  // dynamic
        if (!send_control(kWindowAck, kWindowAckSize) || !send_message(2, kSetPeerBandwidth, 0, 0, bw, sizeof(bw))) return false;
        if (!send_control(kSetChunkSize, kOutChunkSize)) return false;
        AmfWriter w;
        w.string("_result").number(txn);
        w.begin_object().key("fmsVer").string("FMS/3,0,1,123").key("capabilities").number(31).end_object();
        w.begin_object()
            .key("level")
            .string("status")
            .key("code")
            .string("NetConnection.Connect.Success")
            .key("description")
            .string("Connection succeeded.")
            .key("objectEncoding")
            .number(0)
            .end_object();
        return send_command(3, 0, w);
    }

    if (name == "releaseStream" || name == "FCPublish" || name == "_checkbw") {
        AmfWriter w;
        w.string("_result").number(txn).null().undefined();
        return send_command(3, 0, w);
    }

    if (name == "createStream") {
        AmfWriter w;
        w.string("_result").number(txn).null().number(kPublishStreamId);
        return send_command(3, 0, w);
    }

    if (name == "publish") {
        const std::string key = args.size() >= 4 && args[3].type == AmfValue::Type::String ? normalize_key(args[3].str) : "";
        const int ch = server_.resolve(app_, key);
        if (ch == 0) {
            // キーを丸ごとログに出さない (合言葉なので)
            spdlog::warn("RTMP: {} publish rejected (app \"{}\", unknown key)", peer_, app_);
            AmfWriter w;
            w.string("onStatus").number(0).null();
            w.begin_object()
                .key("level")
                .string("error")
                .key("code")
                .string("NetStream.Publish.BadName")
                .key("description")
                .string("Unknown stream key.")
                .end_object();
            send_command(5, kPublishStreamId, w);
            return false;
        }
        ch_ = ch;
        publishing_ = true;
        server_.begin_publish(ch, this);
        spdlog::info("ch{}: RTMP publish from {} (app \"{}\")", ch, peer_, app_);
        AmfWriter w;
        w.string("onStatus").number(0).null();
        w.begin_object()
            .key("level")
            .string("status")
            .key("code")
            .string("NetStream.Publish.Start")
            .key("description")
            .string("Publishing.")
            .end_object();
        return send_command(5, kPublishStreamId, w);
    }

    if (name == "FCUnpublish" || name == "deleteStream" || name == "closeStream") {
        return !publishing_;  // publish 中なら終わる
    }
    return true;
}

void Connection::run() {
    DWORD timeout = 500;  // ms。この間隔で切断要求と無通信を確かめる
    setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    DWORD send_timeout = 5000;  // 受け取らない相手で send() に固まらない
    setsockopt(sock_, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&send_timeout), sizeof(send_timeout));
    last_data_qpc_ = qpc_now();
    try {
        if (handshake()) {
            while (read_chunk()) {}
        }
    } catch (const std::exception& e) {
        spdlog::warn("RTMP: {} error: {}", peer_, e.what());
    }
    if (publishing_) {
        server_.end_publish(ch_, this);
        spdlog::info("ch{}: RTMP publish from {} ended", ch_, peer_);
    }
    finished_ = true;
}

}  // namespace denpachan::rtmpd
