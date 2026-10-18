#include "filename.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <vector>

namespace denpachan::worker {

namespace {

std::string two(int v) {
    char b[8];
    snprintf(b, sizeof(b), "%02d", v);
    return b;
}

bool is_reserved(std::string s) {
    if (const size_t dot = s.find('.'); dot != std::string::npos) s.resize(dot);
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::toupper(c)); });
    static const char* names[] = {"CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7",
                                  "COM8", "COM9", "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"};
    for (const char* n : names)
        if (s == n) return true;
    return false;
}

// フォルダ / ファイル名 1 つ分を Windows で使える形にする
std::string sanitize_component(const std::string& in, const std::string& replacement) {
    std::string out;
    for (unsigned char c : in) {
        if (c < 0x20 || std::string_view(":*?\"<>|").find(char(c)) != std::string_view::npos) {
            out += replacement;
        } else {
            out += char(c);
        }
    }
    while (!out.empty() && (out.back() == '.' || out.back() == ' ')) out.pop_back();
    while (!out.empty() && out.front() == ' ') out.erase(out.begin());
    if (out == "." || out == ".." || is_reserved(out)) out = "_" + out;
    return out;
}

}  // namespace

std::string expand_template(const std::string& tmpl, const FilenameFields& f, const std::string& replacement) {
    std::string expanded;
    for (size_t i = 0; i < tmpl.size(); ++i) {
        if (tmpl[i] != '{') {
            expanded += tmpl[i];
            continue;
        }
        const size_t close = tmpl.find('}', i);
        if (close == std::string::npos) {
            expanded += tmpl.substr(i);
            break;
        }
        const std::string key = tmpl.substr(i + 1, close - i - 1);
        const auto& t = f.time;
        if (key == "ch") expanded += std::to_string(f.ch);
        else if (key == "name") expanded += f.name;
        else if (key == "YYYY") expanded += std::to_string(t.tm_year + 1900);
        else if (key == "YY") expanded += two((t.tm_year + 1900) % 100);
        else if (key == "MM") expanded += two(t.tm_mon + 1);
        else if (key == "DD") expanded += two(t.tm_mday);
        else if (key == "hh") expanded += two(t.tm_hour);
        else if (key == "mm") expanded += two(t.tm_min);
        else if (key == "ss") expanded += two(t.tm_sec);
        else if (key == "seq") expanded += std::to_string(f.seq);
        else if (key == "codec") expanded += f.codec;
        else if (key == "res") expanded += f.width > 0 ? std::to_string(f.width) + "x" + std::to_string(f.height) : "";
        else expanded += tmpl.substr(i, close - i + 1);  // 知らないトークンはそのまま
        i = close;
    }

    // フォルダの区切りで分けて、それぞれを使える名前にする
    std::string result;
    std::string part;
    auto flush = [&] {
        const std::string clean = sanitize_component(part, replacement);
        part.clear();
        if (clean.empty()) return;
        if (!result.empty()) result += '\\';
        result += clean;
    };
    for (char c : expanded) {
        if (c == '/' || c == '\\') flush();
        else part += c;
    }
    flush();
    if (result.empty()) result = "ch" + std::to_string(f.ch);
    return result;
}

std::filesystem::path utf8_path(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return std::filesystem::path(w);
}

std::string path_utf8(const std::filesystem::path& p) {
    const std::wstring w = p.wstring();
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::filesystem::path make_unique_path(const std::string& dir, const std::string& relative, const std::string& ext) {
    const std::filesystem::path base = utf8_path(dir) / utf8_path(relative);
    std::error_code ec;
    std::filesystem::create_directories(base.parent_path(), ec);
    std::filesystem::path candidate = base;
    candidate += utf8_path(ext);
    for (int n = 2; std::filesystem::exists(candidate, ec) && n < 10000; ++n) {
        candidate = base;
        candidate += utf8_path(" (" + std::to_string(n) + ")" + ext);
    }
    return candidate;
}

}  // namespace denpachan::worker
