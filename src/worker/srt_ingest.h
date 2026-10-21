#pragma once
// SRT 受信 (libsrt を直接使う。FFmpeg の srt:// では統計が取れないため)。
//
// - listener: 待ち受け。接続中に新しい接続が来たら新しい方に切り替える
//   (送出側が再起動したとき、古い接続のタイムアウトを待たずに復帰するため)。
// - caller: こちらから接続。切れたら指数バックオフで再接続。

#include <atomic>
#include <string>
#include <thread>

#include "byte_queue.h"
#include "denpachan/win_util.h"
#include "metrics.h"

namespace denpachan::worker {

struct SrtOptions {
    std::string mode = "listener";  // listener / caller
    std::string host = "0.0.0.0";   // listener: 待受アドレス / caller: 接続先
    int port = 0;
    int latency_ms = 120;
    std::string passphrase;
    int pbkeylen = 0;  // 0 (既定) / 16 / 24 / 32
    std::string streamid;
};

class SrtIngest {
public:
    SrtIngest(SrtOptions options, ByteQueue& queue, ChannelMetrics& metrics);
    ~SrtIngest();

    bool start();
    void stop();

private:
    void run_listener();
    void run_caller();
    bool configure(int sock, bool listener);
    void update_stats(int sock);
    bool sleep_interruptible(int ms);

    // listener: 今の相手からこの時間内にデータが届いていれば、新しい接続を断る
    static constexpr int64_t kActiveSenderMs = 1000;

    const SrtOptions options_;
    ByteQueue& queue_;
    ChannelMetrics& metrics_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    UniqueHandle stop_event_;
    SrtLatencyAdvisor advisor_;  // 受信スレッドだけが触る
    int advisor_sock_ = -1;      // 接続が変わったら統計を捨てる
};

}  // namespace denpachan::worker
