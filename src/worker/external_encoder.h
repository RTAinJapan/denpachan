#pragma once
// CPU で再エンコードするための外部 ffmpeg (設計書 §8.4)。
//
// x264 / x265 は GPL なので denpachan には入れない。ユーザーが用意した ffmpeg.exe を子プロセスで起動し、
// 受信したストリーム (時刻を直したもの) を標準入力に NUT で流して、エンコードとファイルへの書き出しを任せる。
// - 優先度は「通常以下」にして、受信・出力の処理より後回しにさせる
// - 標準入力を閉じると ffmpeg は書き終えて自分で終わる。worker が落ちても同じ (ファイルは壊れない)
// - ffmpeg のエラー出力は worker のログに書き、最後の 1 行を録画のエラー表示に使う

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace denpachan::worker {

class ExternalEncoder {
public:
    ExternalEncoder();
    ~ExternalEncoder();  // 標準入力を閉じる。ffmpeg は残りを書き終えてから自分で終わる
    ExternalEncoder(const ExternalEncoder&) = delete;
    ExternalEncoder& operator=(const ExternalEncoder&) = delete;

    // ffmpeg.exe を探す。受信に使っている FFmpeg の DLL (shared 版の bin) と同じフォルダを先に見て、
    // 無ければ PATH。受信と録画で同じ FFmpeg を使うため (別に指定する設定は持たない)。見つからなければ空
    static std::wstring find();
    // 動いている ffmpeg をすべて止める (録画スレッドが書き込みで固まったときの最後の手段)
    static void kill_all();

    bool start(const std::wstring& exe, const std::vector<std::string>& args, std::string& error);
    bool write(const uint8_t* data, size_t size);  // ffmpeg が終わっていたら false
    void close_input();
    bool exited(DWORD& code) const;
    void kill();
    DWORD pid() const { return pid_; }
    std::string last_message() const;

private:
    void read_stderr();

    HANDLE process_ = nullptr;
    HANDLE stdin_ = nullptr;
    HANDLE stderr_ = nullptr;
    DWORD pid_ = 0;
    std::thread reader_;
    std::atomic<bool> reader_done_{false};
    mutable std::mutex mutex_;
    std::string last_message_;
};

}  // namespace denpachan::worker
