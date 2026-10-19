#include "ipc_server.h"

#include <spdlog/spdlog.h>

using nlohmann::json;

namespace denpachan::engine {

namespace {

constexpr DWORD kBufferSize = 64 * 1024;
constexpr size_t kMaxLine = 1024 * 1024;
constexpr DWORD kWriteTimeoutMs = 2000;  // 読まないクライアントで engine を止めない

}  // namespace

IpcServer::IpcServer(Handler handler)
    : handler_(std::move(handler)), stop_event_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}

IpcServer::~IpcServer() { stop(); }

HANDLE IpcServer::create_pipe() {
    SharedSecurity sec;
    return CreateNamedPipeW(kPipeName, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                            PIPE_UNLIMITED_INSTANCES, kBufferSize, kBufferSize, 0, sec.get());
}

bool IpcServer::start() {
    ResetEvent(stop_event_.get());
    accept_thread_ = std::thread(&IpcServer::accept_loop, this);
    return true;
}

void IpcServer::stop() {
    SetEvent(stop_event_.get());
    if (accept_thread_.joinable()) accept_thread_.join();
    std::vector<std::shared_ptr<Client>> clients;
    {
        std::lock_guard lk(clients_mutex_);
        clients.swap(clients_);
    }
    for (auto& c : clients) {
        c->alive = false;
        CancelIoEx(c->pipe.get(), nullptr);
        if (c->thread.joinable()) c->thread.join();
    }
}

void IpcServer::accept_loop() {
    while (WaitForSingleObject(stop_event_.get(), 0) == WAIT_TIMEOUT) {
        HANDLE pipe = create_pipe();
        if (pipe == INVALID_HANDLE_VALUE) {
            spdlog::error("ipc: cannot create pipe (error {})", GetLastError());
            if (WaitForSingleObject(stop_event_.get(), 1000) != WAIT_TIMEOUT) return;
            continue;
        }
        UniqueHandle pipe_guard(pipe);
        UniqueHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        OVERLAPPED ov{};
        ov.hEvent = event.get();
        bool connected = false;
        if (ConnectNamedPipe(pipe, &ov)) {
            connected = true;
        } else if (GetLastError() == ERROR_PIPE_CONNECTED) {
            connected = true;
        } else if (GetLastError() == ERROR_IO_PENDING) {
            HANDLE handles[2] = {event.get(), stop_event_.get()};
            if (WaitForMultipleObjects(2, handles, FALSE, INFINITE) != WAIT_OBJECT_0) {
                CancelIoEx(pipe, &ov);
                return;
            }
            DWORD n = 0;
            connected = GetOverlappedResult(pipe, &ov, &n, FALSE) != FALSE;
        }
        if (!connected) continue;

        auto client = std::make_shared<Client>();
        client->pipe = std::move(pipe_guard);
        client->thread = std::thread(&IpcServer::client_loop, this, client);
        std::lock_guard lk(clients_mutex_);
        // 終わったクライアントを片付ける。alive だけを見て join すると、書き込みタイムアウトで
        // alive を下ろされたが ReadFile で待っているクライアントを待つことになり、
        // clients_mutex_ を持ったまま止まる (broadcast() も止まる)
        for (auto it = clients_.begin(); it != clients_.end();) {
            if ((*it)->finished) {
                if ((*it)->thread.joinable()) (*it)->thread.join();
                it = clients_.erase(it);
            } else {
                ++it;
            }
        }
        clients_.push_back(client);
    }
}

// 書けなくなったクライアントを切る。読み取りスレッドが ReadFile で待っていても、
// 進行中の I/O を全部取り消して切断すれば client_loop が抜ける
void IpcServer::drop_client(Client& client) {
    client.alive = false;
    CancelIoEx(client.pipe.get(), nullptr);
    DisconnectNamedPipe(client.pipe.get());
}

bool IpcServer::write_line(Client& client, const std::string& line) {
    if (!client.alive) return false;
    std::lock_guard lk(client.write_mutex);
    const std::string data = line + "\n";
    UniqueHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    OVERLAPPED ov{};
    ov.hEvent = event.get();
    DWORD written = 0;
    if (!WriteFile(client.pipe.get(), data.data(), DWORD(data.size()), nullptr, &ov)) {
        if (GetLastError() != ERROR_IO_PENDING) {
            drop_client(client);
            return false;
        }
        if (WaitForSingleObject(event.get(), kWriteTimeoutMs) != WAIT_OBJECT_0) {
            CancelIoEx(client.pipe.get(), &ov);
            GetOverlappedResult(client.pipe.get(), &ov, &written, TRUE);
            drop_client(client);
            return false;
        }
    }
    if (!GetOverlappedResult(client.pipe.get(), &ov, &written, FALSE) || written != data.size()) {
        drop_client(client);
        return false;
    }
    return true;
}

void IpcServer::client_loop(std::shared_ptr<Client> client) {
    std::string buffer;
    char chunk[4096];
    UniqueHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));

    while (client->alive) {
        OVERLAPPED ov{};
        ov.hEvent = event.get();
        ResetEvent(event.get());
        DWORD n = 0;
        if (!ReadFile(client->pipe.get(), chunk, sizeof(chunk), nullptr, &ov)) {
            if (GetLastError() != ERROR_IO_PENDING) break;  // 切断
            HANDLE handles[2] = {event.get(), stop_event_.get()};
            if (WaitForMultipleObjects(2, handles, FALSE, INFINITE) != WAIT_OBJECT_0) {
                CancelIoEx(client->pipe.get(), &ov);
                GetOverlappedResult(client->pipe.get(), &ov, &n, TRUE);
                break;
            }
        }
        if (!GetOverlappedResult(client->pipe.get(), &ov, &n, FALSE) || n == 0) break;
        buffer.append(chunk, n);
        if (buffer.size() > kMaxLine) break;

        size_t pos;
        while ((pos = buffer.find('\n')) != std::string::npos) {
            const std::string line = buffer.substr(0, pos);
            buffer.erase(0, pos + 1);
            if (line.find_first_not_of(" \r\t") == std::string::npos) continue;
            json response;
            json id = nullptr;
            try {
                const json req = json::parse(line);
                if (req.contains("id")) id = req.at("id");
                const std::string method = req.value("method", "");
                const json params = req.contains("params") ? req.at("params") : json::object();
                response = {{"id", id}, {"result", handler_(method, params)}};
            } catch (const std::exception& e) {
                response = {{"id", id}, {"error", e.what()}};
            }
            if (!write_line(*client, response.dump(-1, ' ', false, json::error_handler_t::replace))) break;
        }
    }
    client->alive = false;
    DisconnectNamedPipe(client->pipe.get());
    client->finished = true;
}

void IpcServer::broadcast(const json& event) {
    const std::string line = event.dump(-1, ' ', false, json::error_handler_t::replace);
    std::vector<std::shared_ptr<Client>> clients;
    {
        std::lock_guard lk(clients_mutex_);
        clients = clients_;
    }
    for (auto& c : clients) write_line(*c, line);
}

}  // namespace denpachan::engine
