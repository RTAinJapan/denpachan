#pragma once
// 1 チャンネル分の worker プロセスの起動・監視・再起動。設計書 §4。
//
// - 落ちたら再起動する。連続で落ちるときは間隔を 0.5 → 1 → 2 → 5 秒と延ばす (60 秒動けばリセット)。
// - 固まったら (受信・出力スレッドのハートビートが 5 秒途絶える / デコード処理が 5 秒戻らない)
//   強制終了して再起動する。起動直後 5 秒は猶予を置く。
// - engine が落ちても worker は動き続ける (映像は止まらない)。engine が起動し直したとき、
//   同じコマンドライン (= 同じ設定) で動いている worker がいればそのまま引き継ぐ。
//   設定の違う worker が残っていれば止めてから起動する (ポートや仮想カメラの取り合いを防ぐ)。

#include <cstdint>
#include <mutex>
#include <string>

#include "denpachan/win_util.h"
#include "denpachan/worker_status.h"

namespace denpachan::engine {

enum class WorkerState { Stopped, Running, Restarting };

struct WorkerSnapshot {
    WorkerState state = WorkerState::Stopped;
    uint32_t pid = 0;
    uint64_t restarts = 0;
    double uptime_sec = 0;
    std::string last_event;  // 直近の異常 (終了コード、固まった等)
    bool has_metrics = false;
    worker_status::Metrics metrics{};
    uint64_t rejected_senders = 0;  // worker_status::Header の末尾の欄
};

class WorkerProcess {
public:
    // ch: worker_status のスロット (チャンネル番号。rtmpd は 0) / label: ログに出す名前 (省略時 "chN")
    WorkerProcess(int ch, std::wstring exe, std::string label = "");
    ~WorkerProcess();
    WorkerProcess(const WorkerProcess&) = delete;
    WorkerProcess& operator=(const WorkerProcess&) = delete;

    // args (exe を除く) で動かす。同じ args で動いていれば何もしない。違えば止めて起動し直す。
    void start(const std::wstring& args);
    // 停止要求を送り、3 秒で止まらなければ強制終了する
    void stop();
    // 前の engine が起動したプロセスが残っていれば止める (別のフォルダにある前の版も含む)。
    // 前の版の rtmpd が古い版の共有メモリを持ったままのときに、engine の起動時に使う
    void stop_leftover();
    // 監視 (定期的に呼ぶ)
    void poll();
    // 録画の設定を共有メモリに書く (動いている worker が 1 秒以内に読む。再起動はしない)
    void set_record_config(const std::string& json);

    WorkerSnapshot snapshot();
    // worker が「FFmpeg が無い」で終わったか (読んだら戻す)
    bool take_ffmpeg_unavailable();

private:
    bool spawn_locked();
    bool try_adopt_locked();
    void kill_orphan_locked();
    void terminate_locked(const std::string& reason);
    void schedule_restart_locked(const std::string& event);  // event は画面に出すメッセージ (ui_message.h)
    worker_status::Header* status_locked();

    const int ch_;
    const std::wstring exe_;
    const std::string label_;
    std::mutex mutex_;

    bool wanted_ = false;
    std::wstring cmdline_;
    uint64_t hash_ = 0;

    UniqueHandle process_;
    DWORD pid_ = 0;
    int64_t started_qpc_ = 0;
    int64_t next_restart_qpc_ = 0;
    int failures_ = 0;
    uint64_t restarts_ = 0;
    std::string last_event_;
    bool ffmpeg_unavailable_ = false;

    SharedMapping status_mapping_;
    worker_status::Header* status_ = nullptr;
};

// Windows のコマンドライン規則に従って 1 引数をクォートする
std::wstring quote_arg(const std::wstring& arg);

}  // namespace denpachan::engine
