#include "worker_process.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdio>
#include <cwctype>

#include "audio_devices.h"

namespace denpachan::engine {

namespace {

constexpr int64_t kStartupGraceMs = 5000;   // 起動直後は固まり判定をしない
constexpr int64_t kHangTimeoutMs = 5000;    // ハートビートがこれだけ途絶えたら固まったとみなす
constexpr int64_t kHealthyRunMs = 60'000;   // これだけ動けば連続失敗の回数をリセットする
constexpr int64_t kStopTimeoutMs = 3000;    // 停止要求からこれだけ待って止まらなければ強制終了
constexpr int64_t kBackoffMs[] = {500, 1000, 2000, 5000};

std::wstring lower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return wchar_t(std::towlower(c)); });
    return s;
}

std::wstring image_path(HANDLE process) {
    wchar_t buf[MAX_PATH * 2] = {};
    DWORD n = DWORD(std::size(buf));
    if (!QueryFullProcessImageNameW(process, 0, buf, &n)) return {};
    return std::wstring(buf, n);
}

bool running(HANDLE process) { return process && WaitForSingleObject(process, 0) == WAIT_TIMEOUT; }

bool heartbeat_alive(int64_t heartbeat_qpc, int64_t timeout_ms) {
    return heartbeat_qpc != 0 && qpc_now() - heartbeat_qpc < qpc_from_ms(timeout_ms);
}

}  // namespace

std::wstring quote_arg(const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\"") == std::wstring::npos) return arg;
    std::wstring out = L"\"";
    for (size_t i = 0;; ++i) {
        size_t backslashes = 0;
        while (i < arg.size() && arg[i] == L'\\') {
            ++backslashes;
            ++i;
        }
        if (i == arg.size()) {
            out.append(backslashes * 2, L'\\');
            break;
        }
        if (arg[i] == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out.push_back(L'"');
        } else {
            out.append(backslashes, L'\\');
            out.push_back(arg[i]);
        }
    }
    out.push_back(L'"');
    return out;
}

WorkerProcess::WorkerProcess(int ch, std::wstring exe, std::string label)
    : ch_(ch), exe_(std::move(exe)), label_(label.empty() ? "ch" + std::to_string(ch) : std::move(label)) {}

WorkerProcess::~WorkerProcess() = default;  // worker は止めない (engine が落ちても映像を止めないため)

worker_status::Header* WorkerProcess::status_locked() {
    if (!status_) status_ = worker_status::open_shared(status_mapping_, ch_);
    return status_;
}

bool WorkerProcess::try_adopt_locked() {
    auto* st = status_locked();
    if (!st) return false;
    const DWORD pid = st->pid.load();
    if (pid == 0 || st->args_hash.load() != hash_) return false;
    HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return false;
    UniqueHandle process(h);
    if (!running(h) || lower(image_path(h)) != lower(exe_)) return false;
    if (!heartbeat_alive(st->playout_heartbeat_qpc.load(), kHangTimeoutMs)) return false;

    process_ = std::move(process);
    pid_ = pid;
    started_qpc_ = st->started_qpc.load();
    spdlog::info("[{}]adopted running worker (pid {})", label_, pid);
    return true;
}

void WorkerProcess::kill_orphan_locked() {
    // 前の engine が起動した、設定の違う worker が残っていれば止める
    auto* st = status_locked();
    if (!st) return;
    const DWORD pid = st->pid.load();
    if (pid == 0 || pid == pid_) return;
    HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return;
    UniqueHandle process(h);
    if (!running(h) || lower(image_path(h)) != lower(exe_)) return;
    spdlog::info("[{}]stopping leftover worker (pid {})", label_, pid);
    if (HANDLE ev = OpenEventW(EVENT_MODIFY_STATE, FALSE, worker_status::stop_event_name(ch_).c_str())) {
        SetEvent(ev);
        CloseHandle(ev);
    }
    if (WaitForSingleObject(h, DWORD(kStopTimeoutMs)) != WAIT_OBJECT_0) {
        TerminateProcess(h, 1);
        WaitForSingleObject(h, 2000);
    }
}

bool WorkerProcess::spawn_locked() {
    kill_orphan_locked();
    std::wstring cmd = cmdline_;
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe_.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si,
                        &pi)) {
        schedule_restart_locked("cannot start worker (error " + std::to_string(GetLastError()) + ")");
        return false;
    }
    CloseHandle(pi.hThread);
    process_.reset(pi.hProcess);
    pid_ = pi.dwProcessId;
    started_qpc_ = qpc_now();
    spdlog::info("[{}]worker started (pid {})", label_, pid_);
    return true;
}

void WorkerProcess::schedule_restart_locked(const std::string& event) {
    const int64_t now = qpc_now();
    if (started_qpc_ != 0 && now - started_qpc_ > qpc_from_ms(kHealthyRunMs)) failures_ = 0;
    ++failures_;
    const int64_t delay = kBackoffMs[std::min<size_t>(size_t(failures_ - 1), std::size(kBackoffMs) - 1)];
    next_restart_qpc_ = now + qpc_from_ms(delay);
    ++restarts_;
    last_event_ = event;
    spdlog::warn("[{}]{}; restarting in {} ms", label_, event, delay);
}

void WorkerProcess::terminate_locked(const char* reason) {
    TerminateProcess(process_.get(), 0xDEAD);
    WaitForSingleObject(process_.get(), 2000);
    process_.reset();
    pid_ = 0;
    schedule_restart_locked(reason);
}

void WorkerProcess::start(const std::wstring& args) {
    std::lock_guard lk(mutex_);
    const std::wstring cmdline = quote_arg(exe_) + L" " + args;
    const uint64_t hash = worker_status::hash_args(cmdline);
    wanted_ = true;
    if (running(process_.get())) {
        if (hash == hash_) return;  // 同じ設定で動いている
        spdlog::info("[{}]settings changed, restarting worker", label_);
        if (HANDLE ev = OpenEventW(EVENT_MODIFY_STATE, FALSE, worker_status::stop_event_name(ch_).c_str())) {
            SetEvent(ev);
            CloseHandle(ev);
        }
        if (WaitForSingleObject(process_.get(), DWORD(kStopTimeoutMs)) != WAIT_OBJECT_0) {
            TerminateProcess(process_.get(), 1);
            WaitForSingleObject(process_.get(), 2000);
        }
    }
    process_.reset();
    pid_ = 0;
    cmdline_ = cmdline;
    hash_ = hash;
    failures_ = 0;
    last_event_.clear();
    if (try_adopt_locked()) return;
    spawn_locked();
}

void WorkerProcess::stop() {
    std::lock_guard lk(mutex_);
    wanted_ = false;
    if (!process_) return;
    if (HANDLE ev = OpenEventW(EVENT_MODIFY_STATE, FALSE, worker_status::stop_event_name(ch_).c_str())) {
        SetEvent(ev);
        CloseHandle(ev);
    }
    if (WaitForSingleObject(process_.get(), DWORD(kStopTimeoutMs)) != WAIT_OBJECT_0) {
        spdlog::warn("[{}]worker did not stop in time, terminating", label_);
        TerminateProcess(process_.get(), 1);
        WaitForSingleObject(process_.get(), 2000);
    }
    process_.reset();
    pid_ = 0;
    spdlog::info("[{}]worker stopped", label_);
}

void WorkerProcess::poll() {
    std::lock_guard lk(mutex_);
    if (!wanted_) return;
    const int64_t now = qpc_now();

    if (!process_) {
        if (now >= next_restart_qpc_) spawn_locked();
        return;
    }
    if (!running(process_.get())) {
        DWORD code = 0;
        GetExitCodeProcess(process_.get(), &code);
        process_.reset();
        pid_ = 0;
        char buf[64];
        snprintf(buf, sizeof(buf), "worker exited (code 0x%lX)", code);
        schedule_restart_locked(buf);
        return;
    }

    // 固まっていないか
    if (now - started_qpc_ < qpc_from_ms(kStartupGraceMs)) return;
    auto* st = status_locked();
    if (!st || st->pid.load() != pid_) return;
    const int64_t limit = qpc_from_ms(kHangTimeoutMs);
    if (now - st->ingest_heartbeat_qpc.load() > limit) {
        terminate_locked("receive thread not responding");
    } else if (now - st->playout_heartbeat_qpc.load() > limit) {
        terminate_locked("output thread not responding");
    } else if (const int64_t busy = st->decoder_busy_since_qpc.load(); busy != 0 && now - busy > limit) {
        terminate_locked("decoder not responding");
    }
}

void WorkerProcess::set_record_config(const std::string& json) {
    std::lock_guard lk(mutex_);
    if (auto* st = status_locked()) {
        if (std::string_view(st->record_json, strnlen(st->record_json, sizeof(st->record_json))) != json) {
            worker_status::write_record_config(st, json);
        }
    }
}

WorkerSnapshot WorkerProcess::snapshot() {
    std::lock_guard lk(mutex_);
    WorkerSnapshot s;
    s.state = !wanted_ ? WorkerState::Stopped : process_ ? WorkerState::Running : WorkerState::Restarting;
    s.pid = pid_;
    s.restarts = restarts_;
    s.last_event = last_event_;
    if (process_) {
        s.uptime_sec = double(qpc_to_us(qpc_now() - started_qpc_)) / 1e6;
        if (auto* st = status_locked(); st && st->pid.load() == pid_) {
            s.has_metrics = worker_status::read_metrics(st, s.metrics);
        }
    }
    return s;
}

}  // namespace denpachan::engine
