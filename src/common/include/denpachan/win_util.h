#pragma once
// Windows 共通ユーティリティ (ヘッダオンリー)。
// vcam DLL (静的 CRT) と worker (動的 CRT) の両方から使うため、ライブラリ化せずヘッダだけにしている。

#include <windows.h>
#include <sddl.h>

#include <cstdint>
#include <string>
#include <utility>

namespace denpachan {

// ---- QPC (単調時刻) ----

inline int64_t qpc_freq() {
    static const int64_t freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f.QuadPart;
    }();
    return freq;
}

inline int64_t qpc_now() {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return c.QuadPart;
}

// 整数部と余りを分けて計算する (先に掛けると、10 MHz の QPC では 10.7 日分の差で int64 が溢れる)
inline int64_t qpc_from_us(int64_t us) {
    const int64_t f = qpc_freq();
    return (us / 1'000'000) * f + (us % 1'000'000) * f / 1'000'000;
}
inline int64_t qpc_to_us(int64_t ticks) {
    const int64_t f = qpc_freq();
    return (ticks / f) * 1'000'000 + (ticks % f) * 1'000'000 / f;
}
inline int64_t qpc_from_ms(int64_t ms) { return qpc_from_us(ms * 1000); }

// ---- HANDLE の RAII ----

class UniqueHandle {
public:
    UniqueHandle() = default;
    explicit UniqueHandle(HANDLE h) : h_(h == INVALID_HANDLE_VALUE ? nullptr : h) {}
    ~UniqueHandle() { reset(); }
    UniqueHandle(UniqueHandle&& o) noexcept : h_(std::exchange(o.h_, nullptr)) {}
    UniqueHandle& operator=(UniqueHandle&& o) noexcept {
        if (this != &o) {
            reset();
            h_ = std::exchange(o.h_, nullptr);
        }
        return *this;
    }
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    HANDLE get() const { return h_; }
    explicit operator bool() const { return h_ != nullptr; }
    void reset(HANDLE h = nullptr) {
        if (h_) CloseHandle(h_);
        h_ = (h == INVALID_HANDLE_VALUE) ? nullptr : h;
    }

private:
    HANDLE h_ = nullptr;
};

// ---- プロセス間で共有するカーネルオブジェクトのセキュリティ記述子 ----
// 同じユーザーなら整合性レベルが違っても (例: OBS を管理者で起動) 開けるようにする。
// DACL: SYSTEM と現在のユーザーにフルアクセス / SACL: 低整合性ラベル (書き込み可)。
class SharedSecurity {
public:
    SharedSecurity() {
        std::wstring sddl = L"D:(A;;GA;;;SY)";
        if (auto sid = current_user_sid(); !sid.empty()) sddl += L"(A;;GA;;;" + sid + L")";
        sddl += L"S:(ML;;NW;;;LW)";
        if (ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sd_, nullptr)) {
            sa_.nLength = sizeof(sa_);
            sa_.lpSecurityDescriptor = sd_;
            sa_.bInheritHandle = FALSE;
        }
    }
    ~SharedSecurity() {
        if (sd_) LocalFree(sd_);
    }
    SharedSecurity(const SharedSecurity&) = delete;
    SharedSecurity& operator=(const SharedSecurity&) = delete;

    // 作成に失敗していたら nullptr (= 既定のセキュリティ) を返す
    SECURITY_ATTRIBUTES* get() { return sd_ ? &sa_ : nullptr; }

private:
    static std::wstring current_user_sid() {
        HANDLE token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return {};
        UniqueHandle guard(token);
        DWORD len = 0;
        GetTokenInformation(token, TokenUser, nullptr, 0, &len);
        std::string buf(len, '\0');
        if (!GetTokenInformation(token, TokenUser, buf.data(), len, &len)) return {};
        LPWSTR str = nullptr;
        if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buf.data())->User.Sid, &str)) return {};
        std::wstring result(str);
        LocalFree(str);
        return result;
    }

    PSECURITY_DESCRIPTOR sd_ = nullptr;
    SECURITY_ATTRIBUTES sa_{};
};

// ---- 名前付き共有メモリ ----

class SharedMapping {
public:
    SharedMapping() = default;
    ~SharedMapping() { close(); }
    SharedMapping(const SharedMapping&) = delete;
    SharedMapping& operator=(const SharedMapping&) = delete;

    // 既にあれば開き、なければ作る。作った場合は created = true (中身はゼロ初期化されている)。
    bool open_or_create(const std::wstring& name, size_t bytes, bool& created) {
        close();
        SharedSecurity sec;
        HANDLE h = CreateFileMappingW(INVALID_HANDLE_VALUE, sec.get(), PAGE_READWRITE,
                                      static_cast<DWORD>(uint64_t(bytes) >> 32), static_cast<DWORD>(bytes),
                                      name.c_str());
        if (!h) return false;
        created = GetLastError() != ERROR_ALREADY_EXISTS;
        handle_.reset(h);
        view_ = MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, bytes);
        if (!view_) {
            handle_.reset();
            return false;
        }
        size_ = bytes;
        return true;
    }

    void close() {
        if (view_) UnmapViewOfFile(view_);
        view_ = nullptr;
        size_ = 0;
        handle_.reset();
    }

    void* data() const { return view_; }
    size_t size() const { return size_; }
    explicit operator bool() const { return view_ != nullptr; }

private:
    UniqueHandle handle_;
    void* view_ = nullptr;
    size_t size_ = 0;
};

}  // namespace denpachan
