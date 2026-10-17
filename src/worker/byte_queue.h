#pragma once
// 受信スレッド → デマックススレッドのバイト列キュー。
//
// 受信側は絶対にブロックしない (溢れたら捨てる)。
// 接続ごとに「セッション」を区切り、新しい接続が来たら読み手には EOF を返して
// デマクサを作り直させる (送出側の再起動で PID やコーデックが変わっても追従するため)。

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

namespace denpachan::worker {

class ByteQueue {
public:
    explicit ByteQueue(size_t capacity) : buf_(capacity) {}

    // ---- 書き手 (ingest) ----

    // 新しい接続。それまでのデータは捨てる。
    void begin_session() {
        std::lock_guard lk(mutex_);
        ++session_;
        session_open_ = true;
        head_ = size_ = 0;
        cv_.notify_all();
    }

    // 接続が切れた。読み手は残りを読み切ったら EOF になる。
    void end_session() {
        std::lock_guard lk(mutex_);
        session_open_ = false;
        cv_.notify_all();
    }

    // 入りきらなければ false (データは捨てる)
    bool push(const uint8_t* data, size_t n) {
        std::lock_guard lk(mutex_);
        if (n > buf_.size() - size_) return false;
        size_t tail = (head_ + size_) % buf_.size();
        const size_t first = std::min(n, buf_.size() - tail);
        memcpy(&buf_[tail], data, first);
        memcpy(&buf_[0], data + first, n - first);
        size_ += n;
        cv_.notify_one();
        return true;
    }

    // ---- 読み手 (demux) ----

    // after より新しいセッションが始まるまで待つ。停止なら 0。
    uint64_t wait_session(uint64_t after) {
        std::unique_lock lk(mutex_);
        cv_.wait(lk, [&] { return stopped_ || (session_ > after && session_open_); });
        return stopped_ ? 0 : session_;
    }

    // session のデータを最大 size バイト読む。0 = そのセッションの終わり (または停止)。
    int read(uint8_t* out, int size, uint64_t session) {
        std::unique_lock lk(mutex_);
        for (;;) {
            if (stopped_ || session_ != session) return 0;
            if (size_ > 0) {
                const size_t n = std::min(size_, size_t(size));
                const size_t first = std::min(n, buf_.size() - head_);
                memcpy(out, &buf_[head_], first);
                memcpy(out + first, &buf_[0], n - first);
                head_ = (head_ + n) % buf_.size();
                size_ -= n;
                return int(n);
            }
            if (!session_open_) return 0;
            cv_.wait_for(lk, std::chrono::milliseconds(100));
        }
    }

    bool session_alive(uint64_t session) {
        std::lock_guard lk(mutex_);
        return !stopped_ && session_ == session;
    }

    void stop() {
        std::lock_guard lk(mutex_);
        stopped_ = true;
        cv_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<uint8_t> buf_;
    size_t head_ = 0;
    size_t size_ = 0;
    uint64_t session_ = 0;
    bool session_open_ = false;
    bool stopped_ = false;
};

}  // namespace denpachan::worker
