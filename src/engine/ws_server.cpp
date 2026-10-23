#include <winsock2.h>  // windows.h より先に
#include <ws2tcpip.h>

#include "ws_server.h"

#include <bcrypt.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>

#include "denpachan/win_util.h"

using nlohmann::json;

namespace denpachan::engine {

namespace {

constexpr size_t kMaxQueue = 64;                // クライアントごとに溜める送信の数 (溢れたら古い通知から捨てる)
constexpr size_t kMaxMessage = 1024 * 1024;     // 受け取る 1 メッセージの上限
constexpr size_t kMaxRequestHeader = 8 * 1024;  // 握手の HTTP の要求の上限
constexpr DWORD kSendTimeoutMs = 1000;          // 書き込みがこれだけ進まない相手は切る
constexpr DWORD kHandshakeTimeoutMs = 5000;
constexpr int64_t kPingIntervalMs = 10'000;
constexpr int64_t kIdleTimeoutMs = 30'000;  // これだけ何も届かない相手 (ping に pong が無い) は切る
constexpr char kAcceptGuid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

enum Opcode : uint8_t { kContinuation = 0x0, kText = 0x1, kBinary = 0x2, kClose = 0x8, kPing = 0x9, kPong = 0xA };

std::string base64(const uint8_t* p, size_t n) {
    static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (size_t i = 0; i < n; i += 3) {
        const uint32_t b = uint32_t(p[i]) << 16 | (i + 1 < n ? uint32_t(p[i + 1]) << 8 : 0) | (i + 2 < n ? uint32_t(p[i + 2]) : 0);
        out += table[(b >> 18) & 63];
        out += table[(b >> 12) & 63];
        out += i + 1 < n ? table[(b >> 6) & 63] : '=';
        out += i + 2 < n ? table[b & 63] : '=';
    }
    return out;
}

bool sha1(const std::string& in, uint8_t (&out)[20]) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA1_ALGORITHM, nullptr, 0) < 0) return false;
    BCRYPT_HASH_HANDLE hash = nullptr;
    bool ok = BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) >= 0;
    ok = ok && BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(in.data())), ULONG(in.size()), 0) >= 0;
    ok = ok && BCryptFinishHash(hash, out, ULONG(sizeof(out)), 0) >= 0;
    if (hash) BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

std::string frame(uint8_t opcode, std::string_view payload) {
    std::string f;
    f.reserve(payload.size() + 10);
    f.push_back(char(0x80 | opcode));  // FIN
    const uint64_t n = payload.size();
    if (n < 126) {
        f.push_back(char(n));
    } else if (n <= 0xFFFF) {
        f.push_back(char(126));
        f.push_back(char((n >> 8) & 0xFF));
        f.push_back(char(n & 0xFF));
    } else {
        f.push_back(char(127));
        for (int i = 7; i >= 0; --i) f.push_back(char((n >> (8 * i)) & 0xFF));
    }
    f.append(payload);
    return f;
}

std::string close_frame(uint16_t code) {
    const char payload[2] = {char(code >> 8), char(code & 0xFF)};
    return frame(kClose, std::string_view(payload, 2));
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

std::string trim(const std::string& s) {
    const size_t b = s.find_first_not_of(" \t");
    if (b == std::string::npos) return {};
    return s.substr(b, s.find_last_not_of(" \t") - b + 1);
}

// トークンの比較 (長さ以外で時間が変わらないように)
bool same_secret(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    unsigned char diff = 0;
    for (size_t i = 0; i < a.size(); ++i) diff |= static_cast<unsigned char>(a[i] ^ b[i]);
    return diff == 0;
}

std::string percent_decode(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() && std::isxdigit(static_cast<unsigned char>(s[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
            out += char(std::stoi(s.substr(i + 1, 2), nullptr, 16));
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

bool send_all(SOCKET s, std::string_view data) {
    while (!data.empty()) {
        const int n = ::send(s, data.data(), int(std::min<size_t>(data.size(), 1 << 20)), 0);
        if (n <= 0) return false;  // SO_SNDTIMEO で 1 秒進まなければ失敗で戻る
        data.remove_prefix(size_t(n));
    }
    return true;
}

void send_http(SOCKET s, const char* status, const char* extra_headers = "") {
    std::string r = std::string("HTTP/1.1 ") + status + "\r\n" + extra_headers + "Content-Length: 0\r\nConnection: close\r\n\r\n";
    send_all(s, r);
}

void set_timeout(SOCKET s, int option, DWORD ms) { setsockopt(s, SOL_SOCKET, option, reinterpret_cast<const char*>(&ms), sizeof(ms)); }

}  // namespace

struct WsServer::Client {
    SOCKET sock = INVALID_SOCKET;
    std::string peer;
    std::thread thread;   // serve (読み取り。書き込みは write_loop の別スレッド)
    std::string pending;  // 握手のあとに続けて届いていたバイト (読み取りスレッドだけが触る)

    struct Item {
        std::string data;
        bool droppable = false;  // 溢れたら捨ててよい (1 秒ごとの通知)
        bool last = false;       // 送ったら切る (close フレーム)
    };
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<Item> queue;
    bool closing = false;
    std::atomic<bool> ready{false};     // 握手が済み、hello を積んだ (broadcast を受ける)
    std::atomic<bool> finished{false};  // スレッドが終わった (join してよい)
    std::atomic<int64_t> last_rx_qpc{0};

    void enqueue(std::string data, bool droppable, bool last = false) {
        {
            std::lock_guard lk(mutex);
            if (closing) return;
            if (queue.size() >= kMaxQueue) {
                // 読むのが遅い相手: 古い通知から捨てる (応答と制御フレームは捨てない)
                auto it = std::find_if(queue.begin(), queue.end(), [](const Item& i) { return i.droppable; });
                if (it != queue.end()) queue.erase(it);
            }
            queue.push_back({std::move(data), droppable, last});
        }
        cv.notify_one();
    }

    // 切る (読み取りの recv も、書き込みの send も戻る)
    void close_async() {
        {
            std::lock_guard lk(mutex);
            closing = true;
        }
        cv.notify_all();
        ::shutdown(sock, SD_BOTH);
    }
};

WsServer::WsServer(Handler handler, HelloFn hello) : handler_(std::move(handler)), hello_(std::move(hello)) {
    WSADATA wsa;
    wsa_ok_ = WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
}

WsServer::~WsServer() {
    stop();
    if (wsa_ok_) WSACleanup();
}

bool WsServer::start(const WsOptions& options) {
    stop();
    if (!wsa_ok_) {
        spdlog::error("metrics: Winsock is not available");
        return false;
    }
    options_ = options;
    std::string host = options.bind;
    if (host == "localhost") host = "127.0.0.1";
    if (host.size() > 2 && host.front() == '[' && host.back() == ']') host = host.substr(1, host.size() - 2);

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = AI_PASSIVE | AI_NUMERICHOST | AI_NUMERICSERV;
    addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), std::to_string(options.port).c_str(), &hints, &res) != 0 || !res) {
        spdlog::error("metrics: invalid bind address {}", options.bind);
        return false;
    }
    SOCKET s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    bool ok = s != INVALID_SOCKET;
    if (ok) {
        BOOL exclusive = TRUE;
        setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
        ok = ::bind(s, res->ai_addr, int(res->ai_addrlen)) == 0 && ::listen(s, SOMAXCONN) == 0;
    }
    freeaddrinfo(res);
    if (!ok) {
        spdlog::error("metrics: cannot listen on {}:{} (error {})", options.bind, options.port, WSAGetLastError());
        if (s != INVALID_SOCKET) closesocket(s);
        return false;
    }
    listen_ = uintptr_t(s);
    stop_ = false;
    running_ = true;
    accept_thread_ = std::thread(&WsServer::accept_loop, this);
    spdlog::info("metrics: listening on {}:{}{}", options.bind, options.port, options.token.empty() ? " (no token)" : "");
    return true;
}

void WsServer::stop() {
    if (!running_ && !accept_thread_.joinable()) return;
    stop_ = true;
    if (accept_thread_.joinable()) accept_thread_.join();
    std::vector<std::shared_ptr<Client>> clients;
    {
        std::lock_guard lk(clients_mutex_);
        clients.swap(clients_);
    }
    for (auto& c : clients) c->close_async();
    for (auto& c : clients) {
        if (c->thread.joinable()) c->thread.join();
        closesocket(c->sock);
    }
    if (listen_ != ~uintptr_t(0)) closesocket(SOCKET(listen_));
    listen_ = ~uintptr_t(0);
    running_ = false;
    stop_ = false;
    spdlog::info("metrics: stopped");
}

void WsServer::reap_finished_locked() {
    for (auto it = clients_.begin(); it != clients_.end();) {
        if ((*it)->finished) {
            if ((*it)->thread.joinable()) (*it)->thread.join();
            closesocket((*it)->sock);
            it = clients_.erase(it);
        } else {
            ++it;
        }
    }
}

void WsServer::accept_loop() {
    const SOCKET listener = SOCKET(listen_);
    while (!stop_) {
        fd_set set;
        FD_ZERO(&set);
        FD_SET(listener, &set);
        timeval tv{0, 500'000};
        const int r = select(0, &set, nullptr, nullptr, &tv);
        if (r <= 0) {
            if (r < 0) Sleep(100);  // select の失敗で空回りしない
            std::lock_guard lk(clients_mutex_);
            reap_finished_locked();
            continue;
        }
        sockaddr_storage addr{};
        int len = sizeof(addr);
        const SOCKET s = accept(listener, reinterpret_cast<sockaddr*>(&addr), &len);
        if (s == INVALID_SOCKET) continue;
        char host[NI_MAXHOST] = {};
        char port[NI_MAXSERV] = {};
        getnameinfo(reinterpret_cast<sockaddr*>(&addr), len, host, DWORD(sizeof(host)), port, DWORD(sizeof(port)),
                    NI_NUMERICHOST | NI_NUMERICSERV);
        set_timeout(s, SO_SNDTIMEO, kSendTimeoutMs);
        BOOL nodelay = TRUE;
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));

        std::lock_guard lk(clients_mutex_);
        reap_finished_locked();
        if (clients_.size() >= kMaxClients) {
            spdlog::warn("metrics: refused {}:{} (already {} clients)", host, port, kMaxClients);
            send_http(s, "503 Service Unavailable");
            closesocket(s);
            continue;
        }
        auto client = std::make_shared<Client>();
        client->sock = s;
        client->peer = std::string(host) + ":" + port;
        client->thread = std::thread(&WsServer::serve, this, client);
        clients_.push_back(client);
    }
}

bool WsServer::handshake(Client& c) {
    set_timeout(c.sock, SO_RCVTIMEO, kHandshakeTimeoutMs);
    std::string req;
    char buf[2048];
    size_t end;
    while ((end = req.find("\r\n\r\n")) == std::string::npos) {
        if (req.size() > kMaxRequestHeader) {
            send_http(c.sock, "431 Request Header Fields Too Large");
            return false;
        }
        const int n = recv(c.sock, buf, sizeof(buf), 0);
        if (n <= 0) return false;
        req.append(buf, size_t(n));
    }
    c.pending = req.substr(end + 4);
    req.resize(end);

    // 要求行と見出し
    std::map<std::string, std::string> headers;
    std::string request_line;
    size_t pos = 0;
    while (pos <= req.size()) {
        size_t eol = req.find("\r\n", pos);
        if (eol == std::string::npos) eol = req.size();
        const std::string line = req.substr(pos, eol - pos);
        if (request_line.empty()) {
            request_line = line;
        } else if (const size_t colon = line.find(':'); colon != std::string::npos) {
            headers[lower(trim(line.substr(0, colon)))] = trim(line.substr(colon + 1));
        }
        pos = eol + 2;
    }
    const size_t sp1 = request_line.find(' ');
    const size_t sp2 = request_line.find(' ', sp1 + 1);
    if (sp1 == std::string::npos || sp2 == std::string::npos) {
        send_http(c.sock, "400 Bad Request");
        return false;
    }
    const std::string method = request_line.substr(0, sp1);
    const std::string target = request_line.substr(sp1 + 1, sp2 - sp1 - 1);
    const size_t q = target.find('?');
    const std::string path = target.substr(0, q);
    if (method != "GET") {
        send_http(c.sock, "405 Method Not Allowed");
        return false;
    }
    if (path != "/ws") {
        send_http(c.sock, "404 Not Found");
        return false;
    }
    // ブラウザからの接続は断る。WebSocket には CORS が無いので、断らないと、ユーザーのブラウザで開いた任意のページが
    // 既定の 127.0.0.1 (トークン無し) に繋げ、allowControl が on なら配信テスト用のチャンネルを操作できてしまう。
    // ブラウザは必ず Origin を付け、bot (Node の WebSocket、Python の websockets) は付けない
    if (headers.count("origin")) {
        spdlog::warn("metrics: {} was refused (browser connection, Origin: {})", c.peer, headers["origin"]);
        send_http(c.sock, "403 Forbidden");
        return false;
    }

    // 認証: Authorization: Bearer <token> か ?token=<token>
    if (!options_.token.empty()) {
        std::string given;
        if (auto it = headers.find("authorization"); it != headers.end() && lower(it->second).rfind("bearer ", 0) == 0) {
            given = trim(it->second.substr(7));
        } else if (q != std::string::npos) {
            const std::string query = target.substr(q + 1);
            size_t p = 0;
            while (p < query.size()) {
                size_t amp = query.find('&', p);
                if (amp == std::string::npos) amp = query.size();
                const std::string kv = query.substr(p, amp - p);
                if (kv.rfind("token=", 0) == 0) given = percent_decode(kv.substr(6));
                p = amp + 1;
            }
        }
        if (!same_secret(given, options_.token)) {
            spdlog::warn("metrics: {} was refused (wrong or missing token)", c.peer);
            send_http(c.sock, "401 Unauthorized", "WWW-Authenticate: Bearer\r\n");
            return false;
        }
    }

    const std::string key = headers.count("sec-websocket-key") ? headers["sec-websocket-key"] : "";
    const bool upgrade = lower(headers["upgrade"]).find("websocket") != std::string::npos &&
                         lower(headers["connection"]).find("upgrade") != std::string::npos;
    if (!upgrade || key.empty() || headers["sec-websocket-version"] != "13") {
        send_http(c.sock, "400 Bad Request", "Sec-WebSocket-Version: 13\r\n");
        return false;
    }
    uint8_t digest[20];
    if (!sha1(key + kAcceptGuid, digest)) {
        send_http(c.sock, "500 Internal Server Error");
        return false;
    }
    const std::string response = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " +
                                 base64(digest, sizeof(digest)) + "\r\n\r\n";
    if (!send_all(c.sock, response)) return false;
    set_timeout(c.sock, SO_RCVTIMEO, 0);  // 以後は待ち続ける (切るのは書き込み側の時間切れか close_async)
    return true;
}

void WsServer::serve(std::shared_ptr<Client> c) {
    if (!handshake(*c)) {
        ::shutdown(c->sock, SD_BOTH);
        c->finished = true;
        return;
    }
    spdlog::info("metrics: {} connected", c->peer);
    c->last_rx_qpc = qpc_now();
    try {
        c->enqueue(frame(kText, hello_().dump(-1, ' ', false, json::error_handler_t::replace)), false);
    } catch (const std::exception& e) {
        spdlog::error("metrics: hello failed: {}", e.what());
    }
    c->ready = true;
    std::thread writer(&WsServer::write_loop, this, c);

    // 握手のあとに届いていた分から先に読む
    auto read = [&](void* dst, size_t n) {
        auto* out = static_cast<char*>(dst);
        const size_t from_pending = std::min(n, c->pending.size());
        std::copy_n(c->pending.data(), from_pending, out);
        c->pending.erase(0, from_pending);
        size_t got = from_pending;
        while (got < n) {
            const int r = recv(c->sock, out + got, int(std::min<size_t>(n - got, 1 << 20)), 0);
            if (r <= 0) return false;
            got += size_t(r);
        }
        return true;
    };

    std::string message;
    uint8_t message_opcode = 0;
    uint16_t close_code = 0;     // 0 = 相手が切った / こちらで切る (close フレームを送らない)
    bool close_replied = false;  // 相手の close に応答を積んだ (書き込みスレッドが送ってから切る)
    while (!stop_) {
        uint8_t h[2];
        if (!read(h, 2)) break;
        const bool fin = (h[0] & 0x80) != 0;
        const uint8_t op = h[0] & 0x0F;
        const bool masked = (h[1] & 0x80) != 0;
        uint64_t len = h[1] & 0x7F;
        if (len == 126) {
            uint8_t b[2];
            if (!read(b, 2)) break;
            len = uint64_t(b[0]) << 8 | b[1];
        } else if (len == 127) {
            uint8_t b[8];
            if (!read(b, 8)) break;
            len = 0;
            for (uint8_t x : b) len = len << 8 | x;
        }
        const bool control = (op & 0x8) != 0;
        if ((h[0] & 0x70) != 0 || !masked || (control && (len > 125 || !fin))) {
            close_code = 1002;  // protocol error (クライアントからのフレームは必ずマスクされる)
            break;
        }
        if (len > kMaxMessage || message.size() + len > kMaxMessage) {
            close_code = 1009;  // too big
            break;
        }
        uint8_t mask[4];
        if (!read(mask, 4)) break;
        std::string payload(size_t(len), '\0');
        if (len > 0 && !read(payload.data(), size_t(len))) break;
        for (size_t i = 0; i < payload.size(); ++i) payload[i] = char(payload[i] ^ mask[i % 4]);
        c->last_rx_qpc = qpc_now();

        if (op == kClose) {
            c->enqueue(frame(kClose, std::string_view(payload).substr(0, std::min<size_t>(payload.size(), 2))), false, true);
            close_replied = true;
            break;
        }
        if (op == kPing) {
            c->enqueue(frame(kPong, payload), false);
            continue;
        }
        if (op == kPong) continue;
        if (op == kText || op == kBinary) {
            if (message_opcode != 0) {
                close_code = 1002;
                break;
            }
            message_opcode = op;
            message = std::move(payload);
        } else if (op == kContinuation) {
            if (message_opcode == 0) {
                close_code = 1002;
                break;
            }
            message += payload;
        } else {
            close_code = 1002;
            break;
        }
        if (!fin) continue;
        if (message_opcode == kBinary) {
            close_code = 1003;  // テキストしか受けない
            break;
        }
        message_opcode = 0;

        // 要求: {"id": 1, "method": "...", "params": {...}}
        json response;
        json id = nullptr;
        try {
            const json req = json::parse(message);
            if (req.contains("id")) id = req.at("id");
            const std::string method = req.value("method", "");
            const json params = req.contains("params") ? req.at("params") : json::object();
            response = {{"id", id}, {"result", handler_(method, params)}};
        } catch (const std::exception& e) {
            response = {{"id", id}, {"error", e.what()}};
        }
        message.clear();
        c->enqueue(frame(kText, response.dump(-1, ' ', false, json::error_handler_t::replace)), false);
    }
    if (close_code != 0) {
        c->enqueue(close_frame(close_code), false, true);  // 書き込みスレッドが送ってから切る
    } else if (!close_replied) {
        // ここで切ると、積んだ close の応答を送らずに切ってしまう (相手には 1006 = 異常な切断に見える)
        c->close_async();
    }
    writer.join();
    c->close_async();
    spdlog::info("metrics: {} disconnected", c->peer);
    c->finished = true;
}

void WsServer::write_loop(std::shared_ptr<Client> c) {
    int64_t last_ping = qpc_now();
    for (;;) {
        std::deque<Client::Item> batch;
        {
            std::unique_lock lk(c->mutex);
            c->cv.wait_for(lk, std::chrono::seconds(1), [&] { return c->closing || !c->queue.empty() || stop_; });
            if (c->closing || stop_) break;
            batch.swap(c->queue);
        }
        const int64_t now = qpc_now();
        if (now - c->last_rx_qpc > qpc_from_ms(kIdleTimeoutMs)) {
            spdlog::info("metrics: {} did not answer for {} s; disconnecting", c->peer, kIdleTimeoutMs / 1000);
            break;
        }
        if (now - last_ping > qpc_from_ms(kPingIntervalMs)) {
            batch.push_front({frame(kPing, {}), false, false});
            last_ping = now;
        }
        bool done = false;
        for (const auto& item : batch) {
            if (!send_all(c->sock, item.data)) {
                spdlog::info("metrics: {} is not reading; disconnecting", c->peer);
                done = true;
                break;
            }
            if (item.last) {
                done = true;
                break;
            }
        }
        if (done) break;
    }
    c->close_async();
}

void WsServer::broadcast(const json& event) {
    if (!running_) return;
    const std::string f = frame(kText, event.dump(-1, ' ', false, json::error_handler_t::replace));
    std::vector<std::shared_ptr<Client>> clients;
    {
        std::lock_guard lk(clients_mutex_);
        clients = clients_;
    }
    for (auto& c : clients) {
        if (c->ready && !c->finished) c->enqueue(f, true);
    }
}

}  // namespace denpachan::engine
