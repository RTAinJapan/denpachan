#pragma once
// 画面に出すメッセージ (繋がらない理由、録画のエラー、worker を再起動した理由など)。
//
// 文は C++ では作らない。ID と引数だけを JSON の文字列 {"id":"net.dns","host":"example.com"} にして運び、
// 画面が言語ファイル (src/ui/Denpachan.Ui/Lang/*.json) で文にする。ID を足したら言語ファイルにも足す。
// engine は状態 (getState) にこれをオブジェクトのまま載せる (空なら null)。
//
// 共有メモリの固定長の欄にも入れるので、引数の値は kMaxValueBytes で切る (UTF-8 の途中では切らない)。

#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>

namespace denpachan {

inline constexpr size_t kUiMessageMaxValueBytes = 96;

namespace ui_message_detail {

inline void append_json_string(std::string& out, std::string_view s) {
    out += '"';
    for (const char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    static constexpr char kHex[] = "0123456789abcdef";
                    out += "\\u00";
                    out += kHex[(c >> 4) & 0xf];
                    out += kHex[c & 0xf];
                } else {
                    out += c;
                }
        }
    }
    out += '"';
}

// UTF-8 の文字の途中で切らない
inline std::string_view truncate_utf8(std::string_view s, size_t max_bytes) {
    if (s.size() <= max_bytes) return s;
    size_t n = max_bytes;
    while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;
    return s.substr(0, n);
}

}  // namespace ui_message_detail

using UiMessageArgs = std::initializer_list<std::pair<std::string_view, std::string_view>>;

// ui_message("net.dns", {{"host", host}}) → {"id":"net.dns","host":"example.com"}
inline std::string ui_message(std::string_view id, UiMessageArgs args = {}) {
    std::string out = "{\"id\":";
    ui_message_detail::append_json_string(out, id);
    for (const auto& [key, value] : args) {
        out += ',';
        ui_message_detail::append_json_string(out, key);
        out += ':';
        ui_message_detail::append_json_string(out, ui_message_detail::truncate_utf8(value, kUiMessageMaxValueBytes));
    }
    out += '}';
    return out;
}

}  // namespace denpachan
