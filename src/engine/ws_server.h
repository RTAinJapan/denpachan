#pragma once
// メトリクスの受信口 (WebSocket)。配信テストの bot が状態を受け取り、配信テスト用のチャンネルを操作する。
// docs/stream-test-automation.md §3.2、設計書 §10.2。
//
//   接続:   GET /ws (Upgrade: websocket)。Authorization: Bearer <token> か ?token=<token>。合わなければ 401
//   繋がると hello を 1 つ送り、以後は broadcast() で channels を送る (1 秒ごと)
//   要求:   {"id": 1, "method": "getState", "params": {}}  →  {"id": 1, "result": ...} / {"id": 1, "error": "..."}
//
// - 名前付きパイプの IPC (ipc_server.h) と同じ Handler の形。どのメソッドを通すかは Handler の側で絞る (Engine::handle_remote)。
// - engine の tick を止めない: broadcast() はクライアントごとのキューに積むだけで、書き込みはクライアントごとのスレッドが行う。
//   キューが溢れたら古い通知から捨てる。書き込みが 1 秒進まない相手、30 秒何も返さない相手 (ping に pong が無い) は切る。
// - 外部のライブラリは使わない (テキストフレームの送受信だけ。握手の SHA-1 は BCrypt)。

#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace denpachan::engine {

struct WsOptions {
    std::string bind = "127.0.0.1";
    int port = 9100;
    std::string token;  // 空なら認証しない (loopback のときだけ。呼び出し側で確かめる)
};

class WsServer {
public:
    using Handler = std::function<nlohmann::json(const std::string& method, const nlohmann::json& params)>;
    using HelloFn = std::function<nlohmann::json()>;

    WsServer(Handler handler, HelloFn hello);
    ~WsServer();
    WsServer(const WsServer&) = delete;
    WsServer& operator=(const WsServer&) = delete;

    // 待ち受けを始める。待ち受けられなければ (ポートが使われているなど) false
    bool start(const WsOptions& options);
    // 待ち受けをやめ、全クライアントを切る
    void stop();
    bool running() const { return running_; }
    // 全クライアントに送る (キューに積むだけで待たない)
    void broadcast(const nlohmann::json& event);

    static constexpr size_t kMaxClients = 8;

private:
    struct Client;

    void accept_loop();
    void serve(std::shared_ptr<Client> client);
    void write_loop(std::shared_ptr<Client> client);
    bool handshake(Client& client);
    void reap_finished_locked();

    Handler handler_;
    HelloFn hello_;
    WsOptions options_;
    uintptr_t listen_ = ~uintptr_t(0);  // SOCKET (winsock2.h をヘッダに持ち込まないため)
    std::atomic<bool> stop_{false};
    std::atomic<bool> running_{false};
    std::thread accept_thread_;
    std::mutex clients_mutex_;
    std::vector<std::shared_ptr<Client>> clients_;
    bool wsa_ok_ = false;
};

}  // namespace denpachan::engine
