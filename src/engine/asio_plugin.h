#pragma once
// denpachan-asio-driver.dll (ASIO 出力。別のリポジトリ denpachan-asio-driver、GPL-3.0) の読み込み。設計書 §6.4。
//
// engine は ASIO SDK に触れず、この DLL の C の関数 (denpachan_asio.h。向こうのリポジトリと同じもの) だけを使う。
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

// 出力を開く / 閉じる (DLL が読み込めていなければ open は nullptr)。
// probe_asio_driver とは 1 つのロックで順に呼ぶ: ASIO ドライバは同じプロセスで 2 つ目のインスタンスを嫌うものが多く、
// 一覧を作る (画面・denpachan-ctl。engine のロックの外) のと出力の開け閉め (apply) が同時に走らないようにする
denpachan_asio_output* open_asio_output(const std::wstring& clsid, int preferred_sample_rate, const denpachan_asio_callbacks* callbacks);
void close_asio_output(denpachan_asio_output* output);

}  // namespace denpachan::engine
