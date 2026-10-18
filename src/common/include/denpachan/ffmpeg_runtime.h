#pragma once
// FFmpeg は同梱しない (docs/dependencies.md)。ユーザーが用意した shared 版の DLL を遅延ロードで使うので、
// 最初に呼ぶ前にここで有無と版を確かめる (無いまま呼ぶと遅延ロードの例外で落ちる)。
//
// DLL は通常の順で探す: 実行ファイルと同じフォルダ → システム → PATH。
// worker と engine から使う (FFmpeg のヘッダが必要。DLL は CMake の denpachan_delayload_ffmpeg で遅延ロードにする)。

#include <windows.h>

#include <initializer_list>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

namespace denpachan::ffmpeg_runtime {

// ユーザーに案内する FFmpeg の版。ビルドに使うヘッダの版を変えたらここも変える (worker の static_assert)
inline constexpr const char* kSeries = "9.x";
inline constexpr unsigned kAvcodecMajor = 63;

struct Lib {
    const char* name;       // "avcodec" など
    unsigned major;         // ビルドに使ったヘッダの版 (LIBAVCODEC_VERSION_MAJOR など)
    unsigned minor;
    unsigned (*version)();  // avcodec_version など。DLL を読めてから呼ぶ
};

struct Result {
    bool ok = true;
    std::string message;  // ok でなければ、ユーザー向けの説明
};

inline std::string dll_name(const Lib& lib) { return std::string(lib.name) + "-" + std::to_string(lib.major) + ".dll"; }

inline std::string missing_message(const std::string& dlls) {
    return std::string("FFmpeg ") + kSeries + " (shared 版) が見つかりません (" + dlls +
           ")。FFmpeg の shared 版を入れ、その bin フォルダを PATH に通すか denpachan と同じフォルダに置いてください";
}

// libs は依存される側から並べる (avutil → swresample → swscale → avcodec → avformat)
inline Result check(std::initializer_list<Lib> libs) {
    std::string missing;
    for (const Lib& lib : libs) {
        const std::string dll = dll_name(lib);
        if (!LoadLibraryW(std::wstring(dll.begin(), dll.end()).c_str())) {  // 読めたものはそのまま残す (後で使う)
            if (!missing.empty()) missing += ", ";
            missing += dll;
        }
    }
    if (!missing.empty()) return {false, missing_message(missing)};
    for (const Lib& lib : libs) {
        // 同じ major なら互換。ヘッダより古い minor には無い関数があるかもしれないので断る
        const unsigned v = lib.version();
        const unsigned major = v >> 16;
        const unsigned minor = (v >> 8) & 0xff;
        if (major != lib.major || minor < lib.minor) {
            return {false, std::string("FFmpeg の版が合いません: lib") + lib.name + " " + std::to_string(major) + "." +
                               std::to_string(minor) + " (" + std::to_string(lib.major) + "." + std::to_string(lib.minor) +
                               " 以上が必要)。FFmpeg " + kSeries + " の shared 版を使ってください"};
        }
    }
    return {};
}

// denpachan が使う FFmpeg のライブラリすべて
inline Result check_all() {
    static_assert(LIBAVCODEC_VERSION_MAJOR == kAvcodecMajor, "update kSeries / kAvcodecMajor for the new FFmpeg");
    return check({
        {"avutil", LIBAVUTIL_VERSION_MAJOR, LIBAVUTIL_VERSION_MINOR, &avutil_version},
        {"swresample", LIBSWRESAMPLE_VERSION_MAJOR, LIBSWRESAMPLE_VERSION_MINOR, &swresample_version},
        {"swscale", LIBSWSCALE_VERSION_MAJOR, LIBSWSCALE_VERSION_MINOR, &swscale_version},
        {"avcodec", LIBAVCODEC_VERSION_MAJOR, LIBAVCODEC_VERSION_MINOR, &avcodec_version},
        {"avformat", LIBAVFORMAT_VERSION_MAJOR, LIBAVFORMAT_VERSION_MINOR, &avformat_version},
    });
}

}  // namespace denpachan::ffmpeg_runtime
