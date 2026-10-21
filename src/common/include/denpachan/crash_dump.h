#pragma once
// 未処理例外でプロセスが落ちるときにミニダンプを書く (設計書 §4)。
// 使う側は dbghelp をリンクする。

#include <windows.h>
#include <dbghelp.h>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace denpachan {

namespace crash_dump_detail {
inline std::wstring g_dir;
inline std::wstring g_prefix;

constexpr int kKeepDumps = 10;                                  // 同じ prefix のダンプをいくつ残すか
constexpr ULONGLONG kMinFreeBytes = 2ULL * 1024 * 1024 * 1024;  // 空きがこれを切ったらダンプを書かない

// 同じ prefix のダンプを新しい順に kKeepDumps 個だけ残す。クラッシュを繰り返しても (engine は数秒ごとに
// 再起動する) ディスクを埋めないように、起動のたびに呼ぶ。
inline void prune(const std::wstring& dir, const std::wstring& prefix) {
    std::vector<std::pair<ULONGLONG, std::wstring>> dumps;  // (更新時刻, 名前)
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\" + prefix + L"-*.dmp").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        const ULONGLONG t = (ULONGLONG(fd.ftLastWriteTime.dwHighDateTime) << 32) | fd.ftLastWriteTime.dwLowDateTime;
        dumps.emplace_back(t, fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    if (dumps.size() <= size_t(kKeepDumps)) return;
    std::sort(dumps.begin(), dumps.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    for (size_t i = kKeepDumps; i < dumps.size(); ++i) DeleteFileW((dir + L"\\" + dumps[i].second).c_str());
}

inline LONG WINAPI on_unhandled(EXCEPTION_POINTERS* info) {
    ULARGE_INTEGER free_bytes{};
    if (GetDiskFreeSpaceExW(g_dir.c_str(), &free_bytes, nullptr, nullptr) && free_bytes.QuadPart < kMinFreeBytes) {
        return EXCEPTION_EXECUTE_HANDLER;  // 空きが無いのにダンプで埋めない
    }
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t name[128];
    swprintf_s(name, L"%s-%04u%02u%02u-%02u%02u%02u-%lu.dmp", g_prefix.c_str(), t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond,
               GetCurrentProcessId());
    const std::wstring path = g_dir + L"\\" + name;
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION mei{};
        mei.ThreadId = GetCurrentThreadId();
        mei.ExceptionPointers = info;
        mei.ClientPointers = FALSE;
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file,
                          MINIDUMP_TYPE(MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory), &mei, nullptr, nullptr);
        CloseHandle(file);
    }
    return EXCEPTION_EXECUTE_HANDLER;  // ダイアログを出さずに終了する (engine が再起動する)
}
}  // namespace crash_dump_detail

// dir: ダンプの置き場所 (なければ作る) / prefix: ファイル名の先頭
inline void install_crash_dump(const std::wstring& dir, const std::wstring& prefix) {
    crash_dump_detail::g_dir = dir;
    crash_dump_detail::g_prefix = prefix;
    CreateDirectoryW(dir.c_str(), nullptr);
    crash_dump_detail::prune(dir, prefix);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);  // 「動作を停止しました」ダイアログで止まらないように
    SetUnhandledExceptionFilter(&crash_dump_detail::on_unhandled);
}

// %LOCALAPPDATA%\denpachan\<sub> (なければ作る)
inline std::wstring local_app_dir(const wchar_t* sub) {
    wchar_t buf[MAX_PATH] = {};
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    std::wstring dir = (n > 0 && n < MAX_PATH) ? std::wstring(buf) : std::wstring(L".");
    dir += L"\\denpachan";
    CreateDirectoryW(dir.c_str(), nullptr);
    if (sub && *sub) {
        dir += L"\\";
        dir += sub;
        CreateDirectoryW(dir.c_str(), nullptr);
    }
    return dir;
}

}  // namespace denpachan
