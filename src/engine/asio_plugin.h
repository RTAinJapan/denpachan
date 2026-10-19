#pragma once
// denpachan-asio.dll (ASIO 出力。GPL-3.0、src/asio/) の読み込み。設計書 §6.4。
//
// engine は ASIO SDK に触れず、この DLL の C の関数 (src/asio/denpachan_asio.h) だけを使う。
// DLL は engine と同じフォルダからだけ読み込む (PATH は探さない)。無い・ABI の版が違うときは ASIO の出力先を出さない。

#include <optional>
#include <string>
#include <vector>

#include "denpachan_asio.h"

namespace denpachan::engine {

// DLL の関数。読み込めていなければ全部 nullptr
struct AsioPlugin {
    decltype(&denpachan_asio_list_drivers) list_drivers = nullptr;
    decltype(&denpachan_asio_probe_driver) probe_driver = nullptr;
    decltype(&denpachan_asio_open) open = nullptr;
    decltype(&denpachan_asio_close) close = nullptr;
    explicit operator bool() const { return open != nullptr; }
};

// 最初に呼んだときに読み込む (結果は覚えておく)
const AsioPlugin& asio_plugin();

// インストールされている ASIO ドライバ (DLL が無ければ空)
struct AsioDriverInfo {
    std::wstring name;   // ドライバ名 (レジストリのキー名)
    std::wstring clsid;  // "{...}"
};
std::vector<AsioDriverInfo> list_asio_drivers();

// ドライバの出力チャンネル数とサンプルレート (DLL が確かめる。分からなければ nullopt)
struct AsioDriverProbe {
    int channels = 0;
    int sample_rate = 0;
};
std::optional<AsioDriverProbe> probe_asio_driver(const std::wstring& clsid);

}  // namespace denpachan::engine
