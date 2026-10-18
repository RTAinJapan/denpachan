#include "external_encoder.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <filesystem>
#include <set>

#include "filename.h"

namespace denpachan::worker {

namespace {

std::mutex g_registry_mutex;
std::set<ExternalEncoder*> g_registry;

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

// CommandLineToArgvW の規則でくくる
std::wstring quote_arg(const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\"") == std::wstring::npos) return arg;
    std::wstring out = L"\"";
    size_t backslashes = 0;
    for (wchar_t c : arg) {
        if (c == L'\\') {
            ++backslashes;
        } else if (c == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out += c;
            backslashes = 0;
        } else {
            out.append(backslashes, L'\\');
            out += c;
            backslashes = 0;
        }
    }
    out.append(backslashes * 2, L'\\');
    out += L'"';
    return out;
}

std::wstring exe_dir() {
    wchar_t buf[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return std::filesystem::path(std::wstring(buf, n)).parent_path().wstring();
}

}  // namespace

ExternalEncoder::ExternalEncoder() {
    std::lock_guard lk(g_registry_mutex);
    g_registry.insert(this);
}

ExternalEncoder::~ExternalEncoder() {
    {
        std::lock_guard lk(g_registry_mutex);
        g_registry.erase(this);
    }
    close_input();
    if (reader_.joinable()) {
        // ffmpeg がまだ動いていれば、エラー出力を読むのをやめて別れる (ffmpeg は動き続けて書き終える)
        while (!reader_done_) {
            CancelSynchronousIo(reader_.native_handle());
            Sleep(5);
        }
        reader_.join();
    }
    if (stderr_) CloseHandle(stderr_);
    if (process_) CloseHandle(process_);
}

std::wstring ExternalEncoder::find(const std::string& configured) {
    std::error_code ec;
    if (!configured.empty()) {
        const std::filesystem::path p = utf8_path(configured);
        return std::filesystem::is_regular_file(p, ec) ? p.wstring() : std::wstring();
    }
    const std::filesystem::path local = std::filesystem::path(exe_dir()) / L"ffmpeg.exe";
    if (std::filesystem::is_regular_file(local, ec)) return local.wstring();
    wchar_t buf[MAX_PATH] = {};
    const DWORD n = SearchPathW(nullptr, L"ffmpeg.exe", nullptr, MAX_PATH, buf, nullptr);
    return n > 0 && n < MAX_PATH ? std::wstring(buf, n) : std::wstring();
}

void ExternalEncoder::kill_all() {
    std::lock_guard lk(g_registry_mutex);
    for (ExternalEncoder* e : g_registry) e->kill();
}

bool ExternalEncoder::start(const std::wstring& exe, const std::vector<std::string>& args, std::string& error) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE in_read = nullptr, in_write = nullptr, err_read = nullptr, err_write = nullptr;
    if (!CreatePipe(&in_read, &in_write, &sa, 1 << 20)) {  // 1 MB (ffmpeg が一瞬詰まっても待たされないように)
        error = "CreatePipe failed";
        return false;
    }
    if (!CreatePipe(&err_read, &err_write, &sa, 0)) {
        CloseHandle(in_read);
        CloseHandle(in_write);
        error = "CreatePipe failed";
        return false;
    }
    SetHandleInformation(in_write, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(err_read, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);

    // 子に渡すハンドルを 3 つに限る (worker の他のハンドルを持たせない)
    HANDLE inherit[3] = {in_read, nul, err_write};
    SIZE_T attr_size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
    std::vector<uint8_t> attr_buf(attr_size);
    auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_buf.data());
    InitializeProcThreadAttributeList(attrs, 1, 0, &attr_size);
    UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, sizeof(inherit), nullptr, nullptr);

    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = in_read;
    si.StartupInfo.hStdOutput = nul;
    si.StartupInfo.hStdError = err_write;
    si.lpAttributeList = attrs;

    std::wstring cmd = quote_arg(exe);
    for (const auto& a : args) cmd += L" " + quote_arg(widen(a));
    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE,
                                   CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS | EXTENDED_STARTUPINFO_PRESENT, nullptr,
                                   nullptr, &si.StartupInfo, &pi);
    const DWORD last_error = GetLastError();
    DeleteProcThreadAttributeList(attrs);
    CloseHandle(in_read);
    CloseHandle(err_write);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) {
        CloseHandle(in_write);
        CloseHandle(err_read);
        error = "CreateProcess failed (" + std::to_string(last_error) + ")";
        return false;
    }
    CloseHandle(pi.hThread);
    process_ = pi.hProcess;
    pid_ = pi.dwProcessId;
    stdin_ = in_write;
    stderr_ = err_read;
    reader_ = std::thread(&ExternalEncoder::read_stderr, this);
    return true;
}

bool ExternalEncoder::write(const uint8_t* data, size_t size) {
    while (size > 0) {
        if (!stdin_) return false;
        DWORD n = 0;
        if (!WriteFile(stdin_, data, DWORD(std::min<size_t>(size, 1 << 20)), &n, nullptr) || n == 0) return false;
        data += n;
        size -= n;
    }
    return true;
}

void ExternalEncoder::close_input() {
    if (stdin_) {
        CloseHandle(stdin_);
        stdin_ = nullptr;
    }
}

bool ExternalEncoder::exited(DWORD& code) const {
    if (!process_ || WaitForSingleObject(process_, 0) != WAIT_OBJECT_0) return false;
    GetExitCodeProcess(process_, &code);
    return true;
}

void ExternalEncoder::kill() {
    if (process_) TerminateProcess(process_, 1);
}

std::string ExternalEncoder::last_message() const {
    std::lock_guard lk(mutex_);
    return last_message_;
}

void ExternalEncoder::read_stderr() {
    std::string line;
    char buf[4096];
    DWORD n = 0;
    auto flush = [&] {
        if (line.empty()) return;
        spdlog::warn("record: ffmpeg (pid {}): {}", pid_, line);
        std::lock_guard lk(mutex_);
        last_message_ = line.substr(0, 120);
        line.clear();
    };
    while (ReadFile(stderr_, buf, sizeof(buf), &n, nullptr) && n > 0) {
        for (DWORD i = 0; i < n; ++i) {
            if (buf[i] == '\n' || buf[i] == '\r') flush();
            else if (line.size() < 1000) line += buf[i];
        }
    }
    flush();
    reader_done_ = true;
}

}  // namespace denpachan::worker
