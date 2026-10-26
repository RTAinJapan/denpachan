#pragma once
// 1 つの出力デバイスに、ルーティングされた複数チャンネルの音声をまとめて書く出力の共通部分。設計書 §6。
// WASAPI (device_output.h) と ASIO (asio_output.h) がある。どちらもチャンネルごとの音は ChannelSource が作る
// (遅延を一定に保つ、クロックずれの補正、フェード、ミュートは共通)。

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "channel_source.h"

namespace denpachan::engine {

struct DeviceOptions {
    std::wstring id;    // WASAPI: エンドポイント ID、ASIO: "asio:{CLSID}"
    std::wstring name;  // 表示名 (ASIO は "ASIO: ドライバ名")
    bool asio = false;
    bool exclusive = false;  // WASAPI のみ
    int margin_ms = 10;      // デバイスのバッファに足す音声の余裕 (到着の揺れの吸収)
};

struct DeviceStatus {
    bool open = false;
    bool ever_opened = false;  // 一度でも開けたか (開けないのと、失って開き直しているのを分ける)
    bool stalled = false;      // 開いているが、ドライバから呼ばれていない (ASIO。1 秒以上)
    bool exclusive = false;
    std::string exclusive_fallback;  // 排他モードを頼まれたが共有モードで開いている理由 ("initFailed" / "tooSlow")。無ければ空
    int sample_rate = 0;
    int channels = 0;
    std::string sample_format;
    double buffer_ms = 0;
    double delay_ms = 0;  // 鳴らすべき時刻からの基本遅延 (バッファ + 余裕)
    uint64_t reopens = 0;
};

class AudioOutput {
public:
    virtual ~AudioOutput() = default;

    virtual void start() = 0;
    virtual void stop() = 0;

    virtual DeviceStatus status() = 0;
    virtual const DeviceOptions& options() const = 0;

    // ミュートの切り替え (デバイスを開き直しても保たれる)
    virtual void set_mute(int ch, bool mute) = 0;

    // 各チャンネルの ChannelSource に f を適用する (デバイスを開いたことがなければ何もしない)
    virtual void for_each_source(const std::function<void(ChannelSource&)>& f) = 0;

    // 振り分け (開始チャンネル、チャンネルの追加・削除、offset、ミュート) を、デバイスを開き直さずに入れ替える。
    // 変わらないチャンネルの ChannelSource はそのまま使う (そのチャンネルの音は途切れず、出力遅延も変わらない)。
    // ASIO は 1 つのドライバを全チャンネルで共有するので、開き直すと他のチャンネルの音も止まる
    virtual void update_routes(std::vector<RouteOptions> routes) = 0;
};

// 振り分けが同じか (ミュートは除く。ミュートは ChannelSource をそのままに切り替えられる)
inline bool same_route(const RouteOptions& a, const RouteOptions& b) {
    return a.ch == b.ch && a.start_channel == b.start_channel && a.offset_ms == b.offset_ms && a.fade == b.fade;
}

// update_routes の入れ替え。sources を routes の順に並べ直す: 同じ振り分けのものはそのまま使い、無いものは fresh
// (routes と同じ並び。同じ振り分けがあれば空でよい) から取る。外れた ChannelSource を返す (ロックの外で捨てる)。
// チャンネルごと無くなったものは、出力先が無くなったことを worker に知らせる
inline std::vector<std::unique_ptr<ChannelSource>> swap_sources(std::vector<std::unique_ptr<ChannelSource>>& sources,
                                                                const std::vector<RouteOptions>& routes,
                                                                std::vector<std::unique_ptr<ChannelSource>>& fresh) {
    std::vector<std::unique_ptr<ChannelSource>> next;
    for (size_t i = 0; i < routes.size(); ++i) {
        auto it = std::find_if(sources.begin(), sources.end(), [&](const auto& s) { return s && same_route(s->options(), routes[i]); });
        if (it != sources.end()) {
            next.push_back(std::move(*it));
        } else {
            next.push_back(std::move(fresh[i]));
        }
        if (next.back()) next.back()->set_mute(routes[i].mute);
    }
    std::vector<std::unique_ptr<ChannelSource>> old;
    for (auto& s : sources) {
        if (!s) continue;
        const bool kept = std::any_of(routes.begin(), routes.end(), [&](const RouteOptions& r) { return r.ch == s->options().ch; });
        if (!kept) s->clear_output_delay();
        old.push_back(std::move(s));
    }
    sources = std::move(next);
    return old;
}

// fresh を作る必要があるか (今の sources に同じ振り分けが無い)
inline bool needs_source(const std::vector<RouteOptions>& current, const RouteOptions& r) {
    return std::none_of(current.begin(), current.end(), [&](const RouteOptions& c) { return same_route(c, r); });
}

}  // namespace denpachan::engine
