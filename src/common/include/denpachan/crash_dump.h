#pragma once
// 未処理例外でプロセスが落ちるときにミニダンプを書く (設計書 §4)。
// 使う側は dbghelp をリンクする。

#include <windows.h>
#include <dbghelp.h>

#include <string>

namespace denpachan {

namespace crash_dump_detail {
inline std::wstring g_dir;
inline std::wstring g_prefix;

inline LONG WINAPI on_unhandled(EXCEPTION_POINTERS* info) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t name[128];
    swprintf_s(name, L"%s-%04u%02u%02u-%02u%02u%02u-%lu.dmp", g_prefix.c_str(), t.wYear, t.wMonth, t.wDay, t.wHour,
               t.wMinute, t.wSecond, GetCurrentProcessId());
    const std::wstring path = g_dir + L"\\" + name;
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION mei{};
        mei.ThreadId = GetCurrentThreadId();
        mei.ExceptionPointers = info;
        mei.ClientPointers = FALSE;
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file,
                          MINIDUMP_TYPE(MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory), &mei, nullptr,
                          nullptr);
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
