#pragma once
// UI ⇔ engine の IPC サーバ (名前付きパイプ、改行区切りの JSON)。設計書 §10.2。
//
//   要求:   {"id": 1, "method": "startChannel", "params": {"ch": 1}}
//   応答:   {"id": 1, "result": ...}  /  {"id": 1, "error": "..."}
//   通知:   {"event": "state", "data": ...}   (engine から全クライアントへ)
//
// パイプは現在のユーザーと SYSTEM だけが開ける。リモートからの接続は拒否する。

#include <nlohmann/json.hpp>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "denpachan/win_util.h"

namespace denpachan::engine {

inline constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\denpachan";

class IpcServer {
public:
    using Handler = std::function<nlohmann::json(const std::string& method, const nlohmann::json& params)>;

    explicit IpcServer(Handler handler);
    ~IpcServer();

    bool start();
    void stop();
    void broadcast(const nlohmann::json& event);

private:
    struct Client {
        UniqueHandle pipe;
        std::mutex write_mutex;
        std::atomic<bool> alive{true};
        std::atomic<bool> finished{false};  // client_loop を抜けた (join してよい)
        std::thread thread;
    };

    void accept_loop();
    void client_loop(std::shared_ptr<Client> client);
    bool write_line(Client& client, const std::string& line);
    static void drop_client(Client& client);
    HANDLE create_pipe();

    Handler handler_;
    UniqueHandle stop_event_;
    std::thread accept_thread_;
    std::mutex clients_mutex_;
    std::vector<std::shared_ptr<Client>> clients_;
};

}  // namespace denpachan::engine
