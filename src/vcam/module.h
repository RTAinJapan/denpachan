#pragma once

#include <windows.h>

#include <atomic>

namespace denpachan::vcam {

// DllCanUnloadNow 用: 生きている COM オブジェクト数 + LockServer 数
inline std::atomic<long> g_module_refs{0};
inline HMODULE g_module = nullptr;

struct ModuleRef {
    ModuleRef() { ++g_module_refs; }
    ~ModuleRef() { --g_module_refs; }
    ModuleRef(const ModuleRef&) : ModuleRef() {}
    ModuleRef& operator=(const ModuleRef&) = default;
};

}  // namespace denpachan::vcam
