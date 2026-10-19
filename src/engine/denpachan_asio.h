// SPDX-License-Identifier: MIT
#pragma once
// denpachan-asio-driver.dll (ASIO 出力。GPL-3.0) と denpachan の engine (MIT) の間の C の受け渡し。
//
// このヘッダは denpachan-asio-driver (この DLL) と denpachan 本体 (src/engine/denpachan_asio.h) の両方に同じものを置く。
// 変えるときは DENPACHAN_ASIO_ABI_VERSION を上げ、両方を合わせる (engine は版が違う DLL を読み込まない)。
// engine は ASIO SDK のヘッダに触れず、実行ファイルと同じフォルダからこの DLL を読み込んで使う
// (denpachan の src/engine/asio_plugin.cpp)。DLL が無ければ ASIO の出力先は出ない。
// 受け渡す型は C のものだけ (SDK の型も C++ の型も渡さない)。このヘッダ自体は SDK の内容を含まないので MIT。
//
// 使い方 (engine 側):
//   1. denpachan_asio_abi_version() が DENPACHAN_ASIO_ABI_VERSION と同じことを確かめる
//   2. denpachan_asio_list_drivers() / denpachan_asio_probe_driver() で出力先の一覧を作る
//   3. denpachan_asio_open() で開く。DLL が自分のスレッドでドライバを開き (開けなければ 1 秒ごとに試す)、
//      開いたら opened、ドライバのコールバックごとに render、閉じたら closed を呼ぶ。
//      ドライバがリセットを求めたら closed → opened と開き直す
//   4. denpachan_asio_close() で閉じる (戻るまでにコールバックは止まる)

#include <stdint.h>
#include <wchar.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DENPACHAN_ASIO_ABI_VERSION 1

#if defined(DENPACHAN_ASIO_EXPORTS)
#define DENPACHAN_ASIO_API __declspec(dllexport)
#else
#define DENPACHAN_ASIO_API
#endif

// インストールされている ASIO ドライバ (HKLM\SOFTWARE\ASIO)
typedef struct denpachan_asio_driver {
    wchar_t name[256];  // ドライバ名 (レジストリのキー名)
    wchar_t clsid[64];  // "{...}"
} denpachan_asio_driver;

// ドライバの出力チャンネル数とサンプルレート
typedef struct denpachan_asio_probe {
    int channels;
    int sample_rate;
} denpachan_asio_probe;

// 開いたドライバの形式 (opened で渡す。文字列は closed まで有効)
typedef struct denpachan_asio_format {
    int channels;
    int sample_rate;
    int buffer_frames;
    int output_latency_frames;  // コールバックで渡したサンプルが鳴り始めるまでの遅延 (フレーム)
    const char* sample_format;  // "float32" など (表示用)
} denpachan_asio_format;

// ログの level
enum {
    DENPACHAN_ASIO_LOG_DEBUG = 0,
    DENPACHAN_ASIO_LOG_INFO = 1,
    DENPACHAN_ASIO_LOG_WARN = 2,
    DENPACHAN_ASIO_LOG_ERROR = 3,
};

typedef struct denpachan_asio_callbacks {
    void* user;
    // ドライバを開いて音を出し始める前に呼ぶ (DLL のスレッドから)
    void (*opened)(void* user, const denpachan_asio_format* format);
    // 閉じたとき (開き直す前にも、denpachan_asio_close の中でも呼ぶ。opened を呼んだ後にだけ呼ぶ)
    void (*closed)(void* user);
    // ドライバのコールバックの中で呼ぶ (ドライバのスレッドから)。mix はインターリーブの float
    // (channels x frames) で無音で埋めてあるので、足し込む。first_hear_qpc は mix の先頭が鳴る時刻 (QPC)
    void (*render)(void* user, float* mix, int frames, int64_t first_hear_qpc);
    // ログ (UTF-8。どのスレッドからも呼ぶことがある)
    void (*log)(void* user, int level, const char* message);
} denpachan_asio_callbacks;

typedef struct denpachan_asio_output denpachan_asio_output;

DENPACHAN_ASIO_API int denpachan_asio_abi_version(void);

// ドライバを drivers に max_drivers 個まで書き、見つかった数を返す (max_drivers より多いこともある)
DENPACHAN_ASIO_API int denpachan_asio_list_drivers(denpachan_asio_driver* drivers, int max_drivers);

// ドライバを開いてチャンネル数とサンプルレートを確かめる (結果は覚えておく)。分かれば 1、分からなければ 0
// (別の ASIO ドライバを開いている間は、覚えている値が無ければ 0。ASIO ドライバは同時に 1 つしか開かない)
DENPACHAN_ASIO_API int denpachan_asio_probe_driver(const wchar_t* clsid, denpachan_asio_probe* probe);

// ドライバを開いて音を出す。preferred_sample_rate にできればそのレートにする (0 = ドライバのまま)。
// callbacks は閉じるまで有効であること。同時に開けるのは 1 つだけ (2 つ目は開けずにログに出し続ける)
DENPACHAN_ASIO_API denpachan_asio_output* denpachan_asio_open(const wchar_t* clsid, int preferred_sample_rate,
                                                              const denpachan_asio_callbacks* callbacks);
DENPACHAN_ASIO_API void denpachan_asio_close(denpachan_asio_output* output);

#ifdef __cplusplus
}
#endif
