// denpachan-ctl: engine を IPC で操作する開発・試験用のコマンド (UI ができるまでの代わり)。
//
//   denpachan-ctl state                     状態 (JSON)
//   denpachan-ctl summary                   状態 (1 チャンネル 1 行)
//   denpachan-ctl config                    設定 (JSON)
//   denpachan-ctl devices                   音声出力デバイス
//   denpachan-ctl start <ch> / stop <ch>    待ち受け開始 / 停止
//   denpachan-ctl mute <ch> on|off
//   denpachan-ctl reset-cam <cam>           仮想カメラを NO SIGNAL に戻す
//   denpachan-ctl set <ch> <json>           チャンネル設定を部分変更 (JSON Merge Patch)
//   denpachan-ctl set-general <json>
//   denpachan-ctl watch [seconds]           1 秒ごとの状態を 1 行ずつ表示

#include <windows.h>

#include <nlohmann/json.hpp>

#include <cstdio>
#include <string>
#include <vector>

using nlohmann::json;

namespace {

constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\denpachan";

std::string to_utf8(const std::wstring& s) {
    if (s.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), int(s.size()), out.data(), n, nullptr, nullptr);
    return out;
}

class Connection {
public:
    bool open() {
        for (int i = 0; i < 50; ++i) {
            pipe_ = CreateFileW(kPipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            if (pipe_ != INVALID_HANDLE_VALUE) return true;
            if (GetLastError() != ERROR_PIPE_BUSY) return false;
            WaitNamedPipeW(kPipeName, 100);
        }
        return false;
    }
    ~Connection() {
        if (pipe_ != INVALID_HANDLE_VALUE) CloseHandle(pipe_);
    }

    bool send(const json& j) {
        const std::string line = j.dump() + "\n";
        DWORD n = 0;
        return WriteFile(pipe_, line.data(), DWORD(line.size()), &n, nullptr) && n == line.size();
    }

    // 1 行読む。切断なら false
    bool read_line(std::string& out) {
        for (;;) {
            if (const size_t pos = buffer_.find('\n'); pos != std::string::npos) {
                out = buffer_.substr(0, pos);
                buffer_.erase(0, pos + 1);
                return true;
            }
            char chunk[4096];
            DWORD n = 0;
            if (!ReadFile(pipe_, chunk, sizeof(chunk), &n, nullptr) || n == 0) return false;
            buffer_.append(chunk, n);
        }
    }

private:
    HANDLE pipe_ = INVALID_HANDLE_VALUE;
    std::string buffer_;
};

// 要求を送り、同じ id の応答を待つ (途中の通知は読み飛ばす)
bool call(Connection& c, const std::string& method, const json& params, json& result) {
    static int next_id = 1;
    const int id = next_id++;
    if (!c.send({{"id", id}, {"method", method}, {"params", params}})) return false;
    std::string line;
    while (c.read_line(line)) {
        const json j = json::parse(line, nullptr, false);
        if (j.is_discarded() || !j.contains("id") || j.at("id") != id) continue;
        if (j.contains("error")) {
            std::fprintf(stderr, "error: %s\n", j.at("error").get<std::string>().c_str());
            return false;
        }
        result = j.at("result");
        return true;
    }
    std::fputs("error: disconnected\n", stderr);
    return false;
}

std::string summary_line(const json& ch) {
    char buf[512];
    const json& w = ch.at("worker");
    std::string line = "ch" + std::to_string(ch.at("id").get<int>()) + " " + w.at("state").get<std::string>();
    if (w.at("pid").get<uint32_t>() != 0) line += " pid " + std::to_string(w.at("pid").get<uint32_t>());
    line += " restarts " + std::to_string(w.at("restarts").get<uint64_t>());
    if (ch.contains("video")) {
        const json& v = ch.at("video");
        snprintf(buf, sizeof(buf), " | video %s %s %.0f fps delay %.0f ms %.1f Mbps", v.at("state").get<std::string>().c_str(),
                 v.at("connected").get<bool>() ? "connected" : "waiting", v.at("outputFps").get<double>(),
                 v.at("delayMs").get<double>(), v.at("bitrateMbps").get<double>());
        line += buf;
    }
    if (ch.contains("audioOut") && !ch.at("audioOut").is_null()) {
        const json& a = ch.at("audioOut");
        if (a.contains("error")) {
            line += " | audio " + a.at("error").get<std::string>();
        } else if (a.value("playing", false)) {
            snprintf(buf, sizeof(buf), " | audio %.1f ms slips %llu underruns %llu%s", a.at("latencyMs").get<double>(),
                     (unsigned long long)a.at("slips").get<uint64_t>(), (unsigned long long)a.at("underruns").get<uint64_t>(),
                     a.value("mute", false) ? " muted" : "");
            line += buf;
        } else {
            line += a.value("open", false) ? " | audio idle" : " | audio closed";
        }
    }
    if (!w.at("lastEvent").get<std::string>().empty()) line += " | last: " + w.at("lastEvent").get<std::string>();
    return line;
}

void usage() {
    std::fputs(
        "usage: denpachan-ctl <command>\n"
        "  state | summary | config | devices\n"
        "  start <ch> | stop <ch>\n"
        "  mute <ch> on|off\n"
        "  reset-cam <cam>\n"
        "  set <ch> <json-merge-patch>\n"
        "  set-general <json-merge-patch>\n"
        "  watch [seconds]\n",
        stderr);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) args.push_back(to_utf8(argv[i]));
    if (args.empty()) {
        usage();
        return 2;
    }
    Connection c;
    if (!c.open()) {
        std::fputs("error: cannot connect to denpachan-engine\n", stderr);
        return 1;
    }

    const std::string& cmd = args[0];
    json result;
    auto print = [&](const json& j) { std::printf("%s\n", j.dump(2).c_str()); };
    try {
        if (cmd == "state" || cmd == "config" || cmd == "devices") {
            const char* method = cmd == "state" ? "getState" : cmd == "config" ? "getConfig" : "listAudioDevices";
            if (!call(c, method, json::object(), result)) return 1;
            print(result);
        } else if (cmd == "summary") {
            if (!call(c, "getState", json::object(), result)) return 1;
            for (const auto& ch : result.at("channels")) std::printf("%s\n", summary_line(ch).c_str());
        } else if ((cmd == "start" || cmd == "stop") && args.size() == 2) {
            if (!call(c, cmd == "start" ? "startChannel" : "stopChannel", {{"ch", std::stoi(args[1])}}, result)) return 1;
        } else if (cmd == "mute" && args.size() == 3) {
            if (!call(c, "setMute", {{"ch", std::stoi(args[1])}, {"mute", args[2] == "on"}}, result)) return 1;
        } else if (cmd == "reset-cam" && args.size() == 2) {
            if (!call(c, "resetCam", {{"cam", std::stoi(args[1])}}, result)) return 1;
        } else if (cmd == "set" && args.size() == 3) {
            if (!call(c, "patchChannel", {{"ch", std::stoi(args[1])}, {"patch", json::parse(args[2])}}, result)) return 1;
            print(result);
        } else if (cmd == "set-general" && args.size() == 2) {
            if (!call(c, "patchGeneral", {{"patch", json::parse(args[1])}}, result)) return 1;
            print(result);
        } else if (cmd == "watch") {
            const int seconds = args.size() >= 2 ? std::stoi(args[1]) : 0;
            const ULONGLONG end = GetTickCount64() + ULONGLONG(seconds) * 1000;
            std::string line;
            while (c.read_line(line)) {
                const json j = json::parse(line, nullptr, false);
                if (j.is_discarded() || j.value("event", "") != "state") continue;
                SYSTEMTIME t;
                GetLocalTime(&t);
                for (const auto& ch : j.at("data").at("channels")) {
                    if (!ch.at("enabled").get<bool>() && ch.at("worker").at("state") == "stopped") continue;
                    std::printf("%02u:%02u:%02u %s\n", t.wHour, t.wMinute, t.wSecond, summary_line(ch).c_str());
                }
                std::fflush(stdout);
                if (seconds > 0 && GetTickCount64() >= end) break;
            }
        } else {
            usage();
            return 2;
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    return 0;
}
