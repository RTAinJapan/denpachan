#pragma once
// 録画ファイル名のテンプレート (設計書 §8.3)。
//
//   {ch} {name} {YYYY} {YY} {MM} {DD} {hh} {mm} {ss} {seq} {codec} {res}
//
// - "/" と "\" はフォルダの区切り。
// - Windows のファイル名に使えない文字 (: * ? " < > | と制御文字) は replacement に置き換える。
//   予約名 (CON, NUL, COM1 など) や ".." は "_" を付けて避ける。末尾のピリオド・空白は取り除く。

#include <cstdint>
#include <ctime>
#include <filesystem>
#include <string>

namespace denpachan::worker {

struct FilenameFields {
    int ch = 1;
    std::string name;
    std::tm time{};  // セグメントを始めた時刻 (ローカル時刻)
    uint64_t seq = 1;
    std::string codec;
    int width = 0;
    int height = 0;
};

// テンプレートを展開した相対パス (拡張子なし、区切りは "\")。UTF-8。
std::string expand_template(const std::string& tmpl, const FilenameFields& f, const std::string& replacement);

// dir + 相対パス + ext。既に同名のファイルがあれば " (2)" などを付けて重ならないようにする。親フォルダも作る。
std::filesystem::path make_unique_path(const std::string& dir, const std::string& relative, const std::string& ext);

std::filesystem::path utf8_path(const std::string& s);
std::string path_utf8(const std::filesystem::path& p);

}  // namespace denpachan::worker
