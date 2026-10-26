// 音声の時間軸の試験 (音声デバイスなし)。設計書 §6.3。
//
// worker の書き手 (AudioWriter + PlayoutClock、A/V 同期) と engine の読み手 (ChannelSource) を、仮想の時計で動かす。
// 送出元から音声のかたまり (AAC の 1024 サンプル) と、場合によっては映像のフレーム (60 fps) が届き、デバイスが 10 ms ごとに
// 描画する。届き方は回線のモデル (揺れ、裾の重い揺れ、途切れ、欠落、帯域不足、映像だけの遅れ) を組み合わせて決める。
// 場合ごとに、合わせ直し・音切れの回数、落ち着いた後に時間軸を動かした回数と行き来の回数、音声の遅延と揺れの備え、
// 乱れが止んでから遅延が戻るまでの時間を数え、条件を外れたら終了コード 1。重複・間引きの上限は、桁が変わっていないかを
// 見るためのもの (回数そのものに実害はほぼ無い)。乱数を使う場合は、種を変えて何度か回し、すべてで条件を満たすことを求める。
//
//   audio_timing_test                 すべての場合
//   audio_timing_test -v              時間軸の動き (worker の debug ログ) も出す
//   audio_timing_test -v N            N 番目 (1 始まり) の場合だけ
//   audio_timing_test --csv DIR       場合・種ごとに 1 秒ごとの CSV を DIR に書く (列は write_csv)
//
// 音声リングは engine と同じ名前の共有メモリ (Local\denpachan_audio_N) を使うので、denpachan が動いているときは回さない
// (動いていれば、その共有メモリがすでにあるので止める)。

#include <spdlog/sinks/callback_sink.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "../engine/channel_source.h"
#include "../worker/audio_writer.h"
#include "../worker/basic_frame_queue.h"
#include "../worker/metrics.h"
#include "../worker/playout_clock.h"
#include "../worker/playout_core.h"
#include "denpachan/audio_shm.h"

namespace denpachan::engine {
// FFmpeg は使わない (デバイスは 48 kHz なのでリサンプルしない)
bool ffmpeg_available() { return false; }
}  // namespace denpachan::engine

namespace {

using namespace denpachan;

int64_t g_now = 0;  // 仮想の時計 (QPC)
bool g_verbose = false;
int64_t fake_now() { return g_now; }
std::vector<std::string> g_log;  // worker のログ (この 1 秒の分。時間軸を動かした理由を CSV に書く)

constexpr int kChunkFrames = 1024;     // AAC の 1 かたまり
constexpr int64_t kNetDelayMs = 30;    // 送出元から届くまで
constexpr int kPlayoutBufferMs = 100;  // チャンネルの再生バッファ (既定)
// 映像は、同じ pts の音声より早く届く。実機 (DVS + RTMP) で、時間軸を決めていたのは音声で、映像側の目標との余りは
// +60〜76 ms だった (再生バッファ 100 ms、音声の出力遅延 32 ms + 揺れの備え 10 ms から、約 120 ms 早い)
constexpr int64_t kVideoLeadMs = 120;
constexpr double kSpareMs = 5;    // これを超えて余っている秒は「余っている」(動かさない幅の下限と同じ)
constexpr double kQuietSec = 60;  // 乱れが止んでから、これだけ経てば「落ち着いた後」

// 到着の遅れ (ms)。k: かたまりの番号 (映像は -1)、sent: 送った時刻 (送出元の時計、秒)、seed: 乱数の種
using LateFn = std::function<double(int k, double sent, uint32_t seed)>;

// デバイスがデータを取る仕方
enum class Consume {
    Steady,     // 1 ms ごとに少しずつ (排他モード・ASIO の短い周期)
    Blocks,     // 10 ms ごとにまとめて (共有モードの既定の周期。DVS もこれ)
    Irregular,  // 10 ms ごとだが、取る時刻が ±3 ms 不規則に揺れる (仮想デバイス、USB など)
};
const char* consume_name(Consume c) { return c == Consume::Steady ? "1ms" : c == Consume::Blocks ? "10ms" : "irregular"; }

struct Case {
    std::string name;
    double seconds = 60;
    double sender_ppm = 0;       // 送出元の時計の速さ (正: 速い)
    double device_ppm = 0;       // デバイスの時計の速さ (正: 速い)
    double device_open_sec = 0;  // engine がデバイスを開く時刻 (0 = worker より先)
    // engine の基本遅延 (デバイスのバッファ + 余裕)。10 ms: 低遅延の仮想ケーブル、32 ms: DVS の共有モード (実測)、
    // 100 ms: USB や Bluetooth のデバイス
    double base_delay_ms = 32;
    Consume consume = Consume::Blocks;
    int seeds = 1;       // 種を変えて回す回数
    bool video = false;  // 映像のフレームも届け、worker と同じく待ち行列に入れて出力スレッドの判断 (PlayoutCore) で出す
    double video_fps = 60;
    // 映像の待ち行列の時間の幅 (ms。0 = worker と同じ設定)。保険 (出せない状態の打ち切り) を試すときに狭める
    double queue_span_ms = 0;
    // --- 回線のモデル ---
    // J1: 届く時刻の揺れ。0.5〜1.5 秒ごとに変わる ±jitter_ms の一様乱数。窓 (1 秒) ごとの最小の余裕が揺れる
    double jitter_ms = 0;
    // J2: 裾の重い揺れ。かたまりごとに ±chunk_jitter_ms、そのうち tail_prob の割合は +tail_min_ms〜tail_max_ms 遅れる
    double chunk_jitter_ms = 0;
    double tail_prob = 0;
    double tail_min_ms = 50;
    double tail_max_ms = 200;
    // S1 / S2 / R1 など: かたまりごとの遅れ (stalls、repeated_stalls、throttle)。映像にも同じだけ効く
    LateFn late_ms = [](int, double, uint32_t) { return 0.0; };
    // V1: 映像だけの遅れ (音声は late_ms のまま)
    LateFn video_late_ms = [](int, double, uint32_t) { return 0.0; };
    // L1: 欠落 (SRT で再送が間に合わなかった)。かたまりが届かない。単独 (loss_prob) と、まとめて (loss_burst_every_sec ごとに
    // loss_burst_chunks 個)
    double loss_prob = 0;
    double loss_burst_every_sec = 0;
    int loss_burst_chunks = 0;
    // 送った順にしか届かない (TCP。RTMP): 前のかたまりより先には届かない
    bool in_order = false;
    // 音声をこの長さずつまとめて届ける (送り側の mpegts が AAC を何フレームかずつ 1 つの PES にまとめる。ffmpeg の既定で
    // 約 149 ms、128 kbps ステレオで約 180 ms)。まとまりの最後のかたまりを送った時刻に、まとめて届く
    double audio_bunch_ms = 0;
    // engine の出力遅延の変わり方 {時刻 (秒), 出力遅延 (ms)}。0 = 出力先なし (音声を「なし」にした)、それ以外は
    // その遅延でデバイスを開き直す (WDM ↔ ASIO の切り替えなど)
    std::vector<std::pair<double, double>> delay_changes = {};
    // 出力の停止 {始まり (秒), 長さ (秒)}: デバイスを失って開き直している間 (描画が来ない)。同じ出力遅延で戻る。
    // その間、音声の余裕を output_off_slack_ms だけずらす (実機で、失っている間だけ約 20 ms 小さく報告された)
    std::pair<double, double> output_off = {0, 0};
    double output_off_slack_ms = 0;
    // --- 落ち着いた後 ---
    // settle_sec より後で、disturbances の各 {始まり, 終わり} (秒) から kQuietSec 経った後
    double settle_sec = 15;
    std::vector<std::pair<double, double>> disturbances = {};
    // --- 条件 ---
    uint64_t max_slips = 0;
    uint64_t max_resyncs = 0;  // 鳴らし始めた 1 回は数えない (開き直したあとの鳴らし始めは数える)
    uint64_t min_resyncs = 0;
    uint64_t max_settled_resyncs = 1'000'000;    // 落ち着いた後の合わせ直し
    uint64_t max_disturbed_resyncs = 1'000'000;  // 乱れの間 (と、その後 kQuietSec) の合わせ直し
    uint64_t max_underruns = 1'000'000;          // 鳴らし始めてからの音切れ
    uint64_t max_settled_underruns = 1'000'000;  // 落ち着いた後の音切れ
    // underruns_from_sec 以降の、乱れの間の音切れの上限 (繰り返す途切れで、備えが増えた後は音切れが出ない)
    double underruns_from_sec = 0;
    uint64_t max_underruns_from = 1'000'000;
    double max_moved_ms = 1e9;  // 時間軸 (映像も同じ) を鳴らし始めてから動かした量の合計 (向きによらず)
    // 落ち着いた後に時間軸を動かした回数の上限。揺れだけなら動かさない
    int max_moves = 1'000'000;
    // 落ち着いた後に、前と逆向きに動かした回数の上限 (揺れでの +1/−1 の行き来)。クロック差や揺れの備えを減らした分は同じ向き
    int max_reversals = 1'000'000;
    int max_settled_jumps = 1'000'000;  // 落ち着いた後に、時間軸を一度に (5 ms を超えて) 動かした回数
    // 遅延 (時間軸) の増え方: delay_check = {前の時刻, 後の時刻} (秒) の間に遅延が増えた量の上限
    std::pair<double, double> delay_check = {0, 0};
    double max_delay_growth_ms = 1e9;
    // 落ち着いた後 (settle_sec 以降) に、遅れて届いた (余裕が負の) かたまりの数の上限
    int max_late_chunks = 1'000'000;
    double max_settled_delay_ms = 1e9;  // 落ち着いた後の音声の遅延 (1 秒ごとの平均) の最大
    // 増え続けないか: 最後の 60 秒の遅延の平均が、半ばの 60 秒の平均からこれを超えて増えない
    double max_late_delay_growth_ms = 1e9;
    // 最後の乱れが止んでから、遅延が乱れの前の +20 ms 以内に戻るまでの時間 (秒) の上限
    double max_recovery_sec = 1e9;
    // delay_check の間の遅延の変化の下限 (± で見るとき)
    double min_delay_growth_ms = -1e9;
    // 映像: 出力が min_fps を下回る秒は fps_grace の各 {始まり, 終わり} (秒) の中だけ (0 = 見ない)
    double min_fps = 0;
    std::vector<std::pair<double, double>> fps_grace = {};
    // 「途切れが繰り返している」とみなした回数、出せない状態の打ち切り (取り直し) の回数
    int max_repeats = 1'000'000;
    int min_reanchors = 0;
    int max_reanchors = 1'000'000;
    double max_reanchor_delay_sec = 1e9;  // 出力が受信の半分を下回ってから、取り直すまでの時間の上限
    double max_headroom_ms = 1e9;         // 音声の揺れの備えの上限 (開始直後も含む)
    // この時刻 (秒) の 1 秒の、音声の余裕の最小から揺れの備えを除いた余りの絶対値の上限 (0 = 見ない)
    double excess_check_sec = 0;
    double max_excess_ms = 1e9;
    // {始まり, 終わり} (秒) の間に、余っている (その秒の余りが kSpareMs を超えている) のに遅らせる向きに続けて動かした回数の上限
    std::pair<double, double> spare_window = {0, 0};
    int max_up_while_spare = 1'000'000;
    // 余りを一度に戻す (caught up) のが、{この時刻 (秒), から何秒以内} にあること
    std::pair<double, double> catch_up_within = {0, 0};
    // {始まり, 終わり} (秒) の間の時間軸の移動の回数の上限と、同じ向きに続いた回数の上限
    std::pair<double, double> moves_window = {0, 0};
    int max_moves_in_window = 1'000'000;
    int max_same_dir_run = 1'000'000;
    // 出力の停止の間の時間軸の移動の回数の上限、戻ってから resume_settle_sec 秒の間に一度に動かした (fast) 回数の上限
    int max_moves_while_off = 1'000'000;
    double resume_settle_sec = 15;
    int max_fast_after_resume = 1'000'000;
};

struct Result {
    uint64_t slips = 0;
    uint64_t resyncs = 0;
    uint64_t settled_resyncs = 0;
    uint64_t disturbed_resyncs = 0;
    uint64_t underruns = 0;
    uint64_t settled_underruns = 0;
    uint64_t underruns_from = 0;
    double moved_ms = 0;
    int moves = 0;                   // 落ち着いた後に時間軸を動かした回数
    int reversals = 0;               // そのうち、前に動かしたのと逆向きのもの
    int settled_jumps = 0;           // そのうち、一度に 5 ms を超えて動かしたもの
    int late_chunks = 0;             // 落ち着いた後に遅れて届いたかたまりの数
    double delay_growth_ms = 0;      // delay_check の間に遅延が増えた量
    double min_slack_ms = 1e9;       // 落ち着いた後の音声の余裕 (鳴らすべき時刻 - 出力遅延 - 到着) の最小
    double max_window_slack_ms = 0;  // 同じく、1 秒ごとの最小の余裕の最大 (遅延が余っている分)
    // 音声の遅延 (いつもの到着から engine に渡すまで、1 秒ごとの平均) と揺れの備え: 落ち着いた後の最小・最大と、最後の値
    double delay_min = 1e9, delay_max = -1e9, delay_last = 0;
    double headroom_min = 1e9, headroom_max = -1e9, headroom_last = 0;
    double late_delay_growth_ms = 0;
    double recovery_sec = -1;  // 戻るまでの時間 (-1: 乱れなし、1e9: 戻らなかった)
    // 映像
    int low_fps_seconds = 0;  // 出力が min_fps を下回った秒 (fps_grace の外)
    double fps_min = 1e9;     // 1 秒ごとの出力 fps の最小 (落ち着いた後)
    uint64_t queue_drops = 0;
    double video_delay_max = 0;
    int repeats = 0;
    int reanchors = 0;
    double reanchor_delay_sec = -1;  // 出力が受信の半分を下回ってから取り直すまで (-1: 取り直していない)
    double catch_up_at = -1;         // catch_up_within の後、最初に一度に戻した時刻
    int moves_in_window = 0, same_dir_run = 0, moves_while_off = 0, fast_after_resume = 0;
    double excess_at = 0;    // excess_check_sec の 1 秒の余り
    int up_while_spare = 0;  // 余っているのに遅らせる向きに続けて動かした回数の最大
    std::string last_resync;
    bool ok = false;
};

// 1 秒ごとの記録 (CSV)
struct Second {
    double t = 0;
    double delay_ms = 0;     // 音声の遅延の平均 (いつもの到着から engine に渡すまで。デバイスの基本遅延は含めない)
    double headroom_ms = 0;  // 音声の揺れの備え (取れないときは負)
    double moved_ms = 0;     // 時間軸を動かした量 (正: 遅らせた)
    std::string reasons;
    uint64_t duplicated = 0, dropped = 0, resyncs = 0, underruns = 0;
    double min_slack_ms = 0;
    int video_fps = 0;          // 映像の出力の枚数 (この 1 秒)
    int video_received = 0;     // 届いた枚数
    uint64_t queue_drops = 0;   // 待ち行列から溢れて捨てた枚数
    double video_delay_ms = 0;  // 映像の遅延 (出した時刻 - 届いた時刻) の平均
};

// 音声の揺れの備え (ms)。この版の PlayoutClock に無ければ -1 (修正前のコードとも組んで回せるように)
template <typename Clock>
double audio_headroom_ms(Clock& clock) {
    if constexpr (requires { clock.audio_headroom_qpc(); }) {
        return double(qpc_to_us(clock.audio_headroom_qpc())) / 1000.0;
    } else {
        return -1;
    }
}

// 映像のフレーム (DecodedFrame の代わり。時刻だけ)
struct SimFrame {
    uint64_t session = 0;
    int64_t pts_us = 0;
    int64_t arrival_qpc = 0;
};
using VideoQueue = worker::BasicFrameQueue<SimFrame>;

// 映像の待ち行列を worker と同じ設定で作る (時間の幅で持つ版でなければ、前と同じ 24 枚)
std::unique_ptr<VideoQueue> make_video_queue(double span_ms) {
#ifdef DENPACHAN_FRAME_QUEUE_SPAN
    auto limits = worker::video_queue_limits(kPlayoutBufferMs);
    if (span_ms > 0) {
        limits.max_span_qpc = qpc_from_us(int64_t(span_ms * 1000));
        limits.max_frames = size_t(span_ms * 240 / 1000) + 2;  // 枚数では溢れさせない
    }
    return std::make_unique<VideoQueue>(limits);
#else
    (void)span_ms;
    return std::make_unique<VideoQueue>(24);
#endif
}

// 修正前のコードとも組めるように、無ければ何もしない呼び出し
template <typename Clock>
void note_video_arrival(Clock& clock, int64_t arrival_qpc) {
    if constexpr (requires { clock.note_video_arrival(arrival_qpc); }) clock.note_video_arrival(arrival_qpc);
}
template <typename Source>
void set_output_active(Source& source, bool active) {
    if constexpr (requires { source.set_output_active(active); }) source.set_output_active(active);
}
template <typename Core>
void check_progress(Core& core, int64_t now, uint64_t received, uint64_t queue_dropped, uint64_t presented) {
    if constexpr (requires { core.check_progress(now, received, queue_dropped, presented); }) {
        core.check_progress(now, received, queue_dropped, presented);
    }
}

// worker のログ 1 行から、時間軸を動かした理由を取り出す (無ければ空)
std::string move_reason(const std::string& line) {
    if (line.rfind("playout: moved ", 0) == 0 && line.rfind("playout: moved the delay", 0) != 0) {
        const auto open = line.find('(');
        const auto close = line.find(')', open);
        if (open != std::string::npos && close != std::string::npos) return line.substr(open + 1, close - open - 1);
    }
    if (line.find("cannot keep up") != std::string::npos) return "re-anchor";
    if (line.find("outages are repeating") != std::string::npos) return "repeating";
    if (line.find("arrived") != std::string::npos && line.find("late") != std::string::npos) {
        return line.find("(outage)") != std::string::npos ? "late (outage)" : "late";
    }
    return "";
}

void write_csv(const std::string& path, const Case& c, const std::vector<Second>& rows) {
    FILE* f = std::fopen(path.c_str(), "w");
    if (!f) {
        std::printf("  cannot write %s\n", path.c_str());
        return;
    }
    std::fprintf(f,
                 "t_s,base_delay_ms,consume,audio_delay_ms,audio_headroom_ms,moved_ms,reasons,duplicated,dropped,resyncs,underruns,"
                 "min_slack_ms,video_fps,video_received,queue_drops,video_delay_ms\n");
    for (const auto& s : rows) {
        std::fprintf(f, "%.0f,%.0f,%s,%.2f,%.1f,%.2f,%s,%llu,%llu,%llu,%llu,%.2f,%d,%d,%llu,%.1f\n", s.t, c.base_delay_ms,
                     consume_name(c.consume), s.delay_ms, s.headroom_ms, s.moved_ms, s.reasons.c_str(), (unsigned long long)s.duplicated,
                     (unsigned long long)s.dropped, (unsigned long long)s.resyncs, (unsigned long long)s.underruns, s.min_slack_ms,
                     s.video_fps, s.video_received, (unsigned long long)s.queue_drops, s.video_delay_ms);
    }
    std::fclose(f);
}

// 決まった種の乱数 (線形合同法)。[0, 1)
struct Rng {
    uint32_t state;
    double next() {
        state = state * 1664525u + 1013904223u;
        return double(state >> 8) / double(1u << 24);
    }
};

Result run(const Case& c, int ch, uint32_t seed, const std::string& csv_path) {
    Result r;
    {
        // 試験の前に、同じ名前の共有メモリが無いこと (denpachan が動いていないこと) を確かめる
        SharedMapping probe;
        bool created = false;
        if (!probe.open_or_create(audio::mapping_name(ch), audio::kMappingBytes, created) || !created) {
            std::printf("SKIP %s: the audio ring of ch%d already exists (is denpachan running?)\n", c.name.c_str(), ch);
            return r;
        }
    }
    g_now = qpc_from_ms(1000);
    g_log.clear();
    const int64_t start = g_now;
    worker::PlayoutClock clock(kPlayoutBufferMs, &fake_now);
    worker::ChannelMetrics metrics;
    worker::AudioWriter writer(clock, metrics, true);
    // 映像: 届いたら待ち行列に入れ、出力スレッドと同じ判断で取り出して、出す時刻まで待って出す
    auto video_queue = make_video_queue(c.queue_span_ms);
    worker::PlayoutCore core(clock, qpc_from_ms(2000));
    const int64_t frame_us = int64_t(1e6 / c.video_fps);
    std::optional<SimFrame> holding;  // 出す時刻を待っているフレーム
    uint64_t video_received = 0, video_presented = 0, video_queue_drops = 0;
    if (!writer.open(ch)) {
        std::printf("FAIL %s: cannot open the audio ring\n", c.name.c_str());
        return r;
    }

    std::unique_ptr<engine::ChannelSource> source;
    engine::RouteOptions route;
    route.ch = ch;
    int64_t base_delay = qpc_from_us(int64_t(c.base_delay_ms * 1000));
    // デバイスの 1 回の描画: 取る量と間隔。これから書くバッファが鳴るまでの時間 (padding) は、基本遅延のうちデバイスの
    // バッファの分 (余裕を除いた残り。少なくとも 1 回分の間隔)
    const int device_frames = c.consume == Consume::Steady ? 48 : 480;
    const double device_sec = double(device_frames) / audio::kSampleRate;
    const int64_t lead = std::max(qpc_from_us(int64_t(device_sec * 1e6)), qpc_from_us(int64_t((c.base_delay_ms - 10) * 1000)) / 2);
    int64_t render_start = start;  // デバイスを開いた時刻
    Rng device_rng{seed ^ 0x9e3779b9u};
    auto render_at = [&](int n) {
        // n 回目の描画の時刻。Irregular では、デバイスの時計どおりの時刻から ±3 ms ずれる (ずれは積み重ならない)
        int64_t at = render_start + qpc_from_us(int64_t(n * device_sec / (1 + c.device_ppm * 1e-6) * 1e6));
        if (c.consume == Consume::Irregular && n > 0) at += qpc_from_us(int64_t((device_rng.next() * 2 - 1) * 3000));
        return at;
    };
    // 開き直す前のデバイスの回数 (ChannelSource ごとに数えるので足しておく)
    uint64_t done_slips = 0, done_resyncs = 0, done_underruns = 0, done_dup = 0, done_drop = 0;
    auto close_device = [&] {
        if (!source) return;
        auto& st = source->stats();
        done_slips += st.slips.load();
        done_resyncs += st.resyncs.load();
        done_underruns += st.underruns.load();
        done_dup += st.duplicated.load();
        done_drop += st.dropped.load();
        source.reset();
    };
    auto open_device = [&] { source = std::make_unique<engine::ChannelSource>(route, audio::kSampleRate, 2, base_delay); };
    // 今までの合計 (開き直した分も足す)
    auto totals = [&](uint64_t& resyncs, uint64_t& underruns, uint64_t& dup, uint64_t& drop) {
        resyncs = done_resyncs, underruns = done_underruns, dup = done_dup, drop = done_drop;
        if (!source) return;
        auto& st = source->stats();
        resyncs += st.resyncs.load(), underruns += st.underruns.load(), dup += st.duplicated.load(), drop += st.dropped.load();
    };
    if (c.device_open_sec <= 0) open_device();
    size_t next_change = 0;
    Rng rng{seed};                      // J1 の揺れ
    Rng chunk_rng{~seed};               // J2 (音声)
    Rng video_rng{seed ^ 0x5bd1e995u};  // J2 (映像)
    Rng loss_rng{seed * 2654435761u};   // L1
    auto jitter = [&] { return (rng.next() * 2 - 1) * c.jitter_ms; };
    double jitter_now = jitter();
    double jitter_until = 0;  // 揺れの値を次に変える時刻 (送出元の時計、秒)
    auto chunk_jitter = [&](Rng& g) {
        if (c.tail_prob > 0 && g.next() < c.tail_prob) return c.tail_min_ms + g.next() * (c.tail_max_ms - c.tail_min_ms);
        return c.chunk_jitter_ms > 0 ? (g.next() * 2 - 1) * c.chunk_jitter_ms : 0.0;
    };
    auto settled_at = [&](double t) {
        if (t < c.settle_sec) return false;
        for (const auto& [from, until] : c.disturbances) {
            if (t >= from && t < until + kQuietSec) return false;
        }
        return true;
    };
    // 時間軸の動き: pts 0 を出すべき時刻が、鳴らし始めてからどれだけ動いたか (合計)
    int64_t last_due0 = 0;
    int last_dir = 0;                         // 前に動かした向き
    int64_t due0_before = 0, due0_after = 0;  // delay_check の時刻の時間軸
    double moved_ms = 0;
    std::vector<std::pair<double, double>> all_moves;  // 時間軸を動かした時刻 (秒) と量 (ms。正: 遅らせた)
    double window_slack_ms = 1e9;                      // 今の 1 秒の最小の余裕 (落ち着いた後)

    std::vector<float> chunk(size_t(kChunkFrames) * 2);
    std::vector<float> mix(size_t(device_frames) * 2);
    const double chunk_sec = double(kChunkFrames) / audio::kSampleRate;
    int k = 0;   // 次に送る音声のかたまり
    int vf = 0;  // 次に送る映像のフレーム
    double next_chunk_jitter = chunk_jitter(chunk_rng);
    double next_video_jitter = chunk_jitter(video_rng);
    int64_t last_audio_arrival = 0, last_video_arrival = 0;  // 送った順にしか届かないとき
    int64_t next_render = start;
    int renders = 0;
    bool was_off = false;
    uint64_t base_resyncs = 0, base_underruns = 0, base_slips = 0;
    bool started = false;
    // 1 秒ごとの記録
    std::vector<Second> seconds;
    Second sec;
    double sec_delay_sum = 0;
    int sec_delay_n = 0;
    double sec_moved = 0;
    double sec_min_slack = 1e9;
    int sec_video_fps = 0, sec_video_received = 0;
    uint64_t sec_queue_drops = 0;
    double sec_video_delay_sum = 0;
    uint64_t prev_resyncs = 0, prev_underruns = 0, prev_dup = 0, prev_drop = 0;
    double next_sec = 1;
    const int64_t end = start + qpc_from_us(int64_t(c.seconds * 1e6));
    for (; g_now < end; g_now += qpc_from_us(250)) {
        const double t = double(qpc_to_us(g_now - start)) / 1e6;
        const bool settled = started && settled_at(t);
        // 送出元: かたまり k の時刻 (送出元の時計) と、届く時刻
        for (;;) {
            const double sent = k * chunk_sec / (1 + c.sender_ppm * 1e-6);
            const double at = sent + kNetDelayMs / 1000.0;
            const bool off = c.output_off.second > 0 && at >= c.output_off.first && at < c.output_off.first + c.output_off.second;
            const double off_ms = off ? -c.output_off_slack_ms : 0.0;  // 停止中は余裕が小さく報告される (K の再現)
            const double bunch_wait =
                c.audio_bunch_ms > 0 ? (std::floor(sent * 1000 / c.audio_bunch_ms) + 1) * c.audio_bunch_ms / 1000 - sent : 0.0;
            int64_t arrival =
                start + qpc_from_us(int64_t((sent + bunch_wait +
                                             (kNetDelayMs + c.late_ms(k, sent, seed) + jitter_now + next_chunk_jitter + off_ms) / 1000.0) *
                                            1e6));
            if (c.in_order) arrival = std::max(arrival, last_audio_arrival);
            if (arrival > g_now) break;
            last_audio_arrival = arrival;
            next_chunk_jitter = chunk_jitter(chunk_rng);
            if (sent >= jitter_until) {
                jitter_now = jitter();
                jitter_until = sent + 1.0 + jitter() / std::max(1.0, c.jitter_ms) * 0.5;
            }
            bool lost = c.loss_prob > 0 && loss_rng.next() < c.loss_prob;
            if (c.loss_burst_every_sec > 0) {
                const double phase = std::fmod(sent, c.loss_burst_every_sec);
                if (sent >= c.loss_burst_every_sec && phase < c.loss_burst_chunks * chunk_sec) lost = true;
            }
            const int64_t pts_us = int64_t(double(k) * kChunkFrames * 1e6 / audio::kSampleRate);
            ++k;
            if (lost) continue;
            for (int i = 0; i < kChunkFrames; ++i) {
                const float v = 0.25f * float(std::sin(2 * 3.14159265 * 440.0 * (double(k - 1) * kChunkFrames + i) / audio::kSampleRate));
                chunk[2 * size_t(i)] = v;
                chunk[2 * size_t(i) + 1] = v;
            }
            // 余裕は書く前の時間軸で測る (遅れて届くと、書き手が時間軸をその分すぐ遅らせる)
            const int64_t due = clock.due(1, pts_us, arrival);
            const double slack = double(qpc_to_us(due - base_delay - arrival)) / 1000.0;
            writer.write(1, pts_us, arrival, chunk.data(), kChunkFrames);
            if (started) {
                // 音声の遅延: いつもの到着 (揺れも乱れも無いとき) から engine に渡すまで (書いた後の時間軸で)。
                // デバイスの基本遅延は含めない (デバイスによらず同じ基準で見る)
                const int64_t nominal = start + qpc_from_us(int64_t((sent + kNetDelayMs / 1000.0) * 1e6));
                sec_delay_sum += double(qpc_to_us(clock.due(1, pts_us, arrival) - base_delay - nominal)) / 1000.0;
                ++sec_delay_n;
                sec_min_slack = std::min(sec_min_slack, slack);
            }
            if (settled) {
                r.min_slack_ms = std::min(r.min_slack_ms, slack);
                if (slack < 0) ++r.late_chunks;
                window_slack_ms = std::min(window_slack_ms, slack);
            }
        }
        // 映像: 同じ回線で届く (V1 では映像だけさらに遅れる)。デコードしたら待ち行列に入れる (worker のデコーダと同じ)
        while (c.video) {
            const double sent = double(vf) * double(frame_us) / 1e6 / (1 + c.sender_ppm * 1e-6);
            const double late = c.late_ms(-1, sent, seed) + c.video_late_ms(-1, sent, seed);
            int64_t arrival =
                start + qpc_from_us(int64_t((sent + (kNetDelayMs - kVideoLeadMs + late + jitter_now + next_video_jitter) / 1000.0) * 1e6));
            if (c.in_order) arrival = std::max(arrival, last_video_arrival);
            if (arrival > g_now) break;
            last_video_arrival = arrival;
            next_video_jitter = chunk_jitter(video_rng);
            const int64_t pts_us = int64_t(vf) * frame_us;
            ++vf;
            ++video_received, ++sec_video_received;
            note_video_arrival(clock, arrival);
            if (video_queue->push(SimFrame{1, pts_us, arrival})) ++video_queue_drops, ++sec_queue_drops;
        }
        // 映像の出力スレッド: 取り出して判断し (PlayoutCore)、出す時刻が来たら出す。出したらすぐ次を取り出す
        while (c.video) {
            check_progress(core, g_now, video_received, video_queue_drops, video_presented);
            if (!holding) {
                auto f = video_queue->pop(std::chrono::milliseconds(0));
                if (!f) break;
                int64_t target = 0;
                if (core.on_frame(worker::FrameMeta{f->session, f->pts_us, f->arrival_qpc}, g_now, video_queue->size(), target) !=
                    worker::PlayoutCore::Decision::Present) {
                    continue;  // 取り残された、または遅れたフレーム
                }
                holding = *f;
            }
            const worker::FrameMeta meta{holding->session, holding->pts_us, holding->arrival_qpc};
            if (core.target(meta) > g_now) break;  // まだ出す時刻ではない (待っている間に時間軸が動いたら取り直す)
            ++video_presented, ++sec_video_fps;
            sec_video_delay_sum += double(qpc_to_us(g_now - holding->arrival_qpc)) / 1000.0;
            holding.reset();
        }
        if (!source && c.device_open_sec > 0 && t >= c.device_open_sec && next_change == 0) {
            open_device();
            render_start = next_render = g_now;
            renders = 0;
        }
        if (next_change < c.delay_changes.size() && t >= c.delay_changes[next_change].first) {
            const double ms = c.delay_changes[next_change++].second;
            if (source && ms == 0) source->clear_output_delay();  // 出力先なし (engine がデバイスの出力を止めたとき)
            close_device();
            if (ms > 0) {
                base_delay = qpc_from_us(int64_t(ms * 1000));
                open_device();
                render_start = next_render = g_now;
                renders = 0;
            }
        }
        if (started) {
            const int64_t due0 = clock.due(1, 0, g_now);
            if (last_due0 != 0) {
                const double d = double(qpc_to_us(due0 - last_due0)) / 1000.0;
                moved_ms += std::fabs(d);
                sec_moved += d;
                if (due0 != last_due0) {
                    all_moves.push_back({t, d});
                    const int dir = due0 > last_due0 ? 1 : -1;
                    if (settled) {
                        ++r.moves;
                        if (last_dir != 0 && dir != last_dir) ++r.reversals;
                        if (std::fabs(d) > 5) ++r.settled_jumps;
                        last_dir = dir;  // 落ち着く前の動き (開始直後の遅れなど) とは比べない
                    }
                }
            }
            last_due0 = due0;
            if (c.delay_check.second > 0) {
                if (due0_before == 0 && t >= c.delay_check.first) due0_before = due0;
                if (due0_after == 0 && t >= c.delay_check.second) due0_after = due0;
            }
        }
        // 出力の停止 (デバイスを失って開き直している間): 描画が来ない。戻ったら同じ出力遅延で描画を始め直す
        const bool output_off = c.output_off.second > 0 && t >= c.output_off.first && t < c.output_off.first + c.output_off.second;
        if (source && output_off != was_off) {
            set_output_active(*source, !output_off);
            if (!output_off) render_start = next_render = g_now, renders = 0;
            was_off = output_off;
        }
        // デバイス: デバイスの時計で決まった間隔 (Consume) ごとに描画する。これから書くバッファは lead 後に鳴る
        if (source && !output_off && g_now >= next_render) {
            std::fill(mix.begin(), mix.end(), 0.0f);
            // これから書くバッファが鳴る時刻 (engine は再生位置から求める。取る時刻が揺れるデバイスでは、これも揺れる)
            source->render(mix.data(), device_frames, g_now + lead, base_delay);
            ++renders;
            next_render = render_at(renders);
            auto& st = source->stats();
            if (g_verbose && renders % 100 == 0) {
                std::lock_guard lk(st.reason_mutex);
                std::printf("  t %6.2f s: playing %d, error %+6.2f ms, slips %llu, resyncs %llu, underruns %llu (%s)\n", t,
                            int(st.playing.load()), st.error_ms.load(), (unsigned long long)st.slips.load(),
                            (unsigned long long)st.resyncs.load(), (unsigned long long)st.underruns.load(), st.last_resync_reason.c_str());
            }
            if (!started && st.playing.load()) {
                started = true;  // 鳴らし始めた。ここから数える
                base_slips = done_slips + st.slips.load();
                uint64_t dup, drop;
                totals(base_resyncs, base_underruns, dup, drop);
                prev_resyncs = base_resyncs, prev_underruns = base_underruns, prev_dup = dup, prev_drop = drop;
            }
        }
        // 1 秒ごと: 記録と、落ち着いた後の数え
        if (t >= next_sec) {
            uint64_t resyncs, underruns, dup, drop;
            totals(resyncs, underruns, dup, drop);
            if (started) {
                sec.t = next_sec;
                sec.delay_ms = sec_delay_n > 0 ? sec_delay_sum / sec_delay_n : (seconds.empty() ? 0 : seconds.back().delay_ms);
                sec.headroom_ms = audio_headroom_ms(clock);
                sec.moved_ms = sec_moved;
                sec.reasons.clear();
                for (const auto& line : g_log) {
                    const std::string reason = move_reason(line);
                    if (reason.empty() || sec.reasons.find(reason) != std::string::npos) continue;
                    if (!sec.reasons.empty()) sec.reasons += '|';
                    sec.reasons += reason;
                }
                sec.duplicated = dup - prev_dup, sec.dropped = drop - prev_drop;
                sec.resyncs = resyncs - prev_resyncs, sec.underruns = underruns - prev_underruns;
                sec.min_slack_ms = sec_min_slack < 1e9 ? sec_min_slack : 0;
                sec.video_fps = sec_video_fps, sec.video_received = sec_video_received, sec.queue_drops = sec_queue_drops;
                sec.video_delay_ms = sec_video_fps > 0 ? sec_video_delay_sum / sec_video_fps : 0;
                seconds.push_back(sec);
                const bool sec_settled = settled_at(next_sec - 1);
                if (sec_settled) {
                    r.settled_resyncs += sec.resyncs;
                    r.settled_underruns += sec.underruns;
                    r.delay_min = std::min(r.delay_min, sec.delay_ms);
                    r.delay_max = std::max(r.delay_max, sec.delay_ms);
                    if (sec.headroom_ms >= 0) {
                        r.headroom_min = std::min(r.headroom_min, sec.headroom_ms);
                        r.headroom_max = std::max(r.headroom_max, sec.headroom_ms);
                    }
                    if (window_slack_ms < 1e9) r.max_window_slack_ms = std::max(r.max_window_slack_ms, window_slack_ms);
                } else if (next_sec - 1 >= c.settle_sec) {
                    r.disturbed_resyncs += sec.resyncs;
                    if (next_sec - 1 >= c.underruns_from_sec && c.underruns_from_sec > 0) r.underruns_from += sec.underruns;
                }
                r.delay_last = sec.delay_ms;
                r.headroom_last = sec.headroom_ms;
            }
            prev_resyncs = resyncs, prev_underruns = underruns, prev_dup = dup, prev_drop = drop;
            sec_delay_sum = 0, sec_delay_n = 0, sec_moved = 0, sec_min_slack = 1e9;
            sec_video_fps = 0, sec_video_received = 0, sec_queue_drops = 0, sec_video_delay_sum = 0;
            window_slack_ms = 1e9;
            g_log.clear();
            next_sec += 1;
        }
    }
    if (!source || !started) {
        std::printf("FAIL %s: never started playing\n", c.name.c_str());
        return r;
    }
    auto& st = source->stats();
    uint64_t resyncs, underruns, dup, drop;
    totals(resyncs, underruns, dup, drop);
    r.slips = done_slips + st.slips.load() - base_slips;
    r.resyncs = resyncs - base_resyncs;
    r.underruns = underruns - base_underruns;
    r.moved_ms = moved_ms;
    if (due0_before != 0 && due0_after != 0) r.delay_growth_ms = double(qpc_to_us(due0_after - due0_before)) / 1000.0;
    // 増え続けないか: 最後の 60 秒と、半ばの 60 秒の平均
    auto average = [&](double from, double until) {
        double sum = 0;
        int n = 0;
        for (const auto& s : seconds) {
            if (s.t > from && s.t <= until) sum += s.delay_ms, ++n;
        }
        return n > 0 ? sum / n : 0.0;
    };
    r.late_delay_growth_ms = average(c.seconds - 60, c.seconds) - average(c.seconds / 2 - 60, c.seconds / 2);
    // 戻るまでの時間: 最後の乱れが止んでから、遅延が乱れの前 (始まる前 10 秒の平均) の +20 ms を最後に超えていた時刻まで
    if (!c.disturbances.empty()) {
        const double first = c.disturbances.front().first;
        double last_end = 0;
        for (const auto& d : c.disturbances) last_end = std::max(last_end, d.second);
        const double before = average(first - 10, first);
        double last_over = last_end;
        for (const auto& s : seconds) {
            if (s.t > last_end && s.delay_ms > before + 20) last_over = s.t;
        }
        r.recovery_sec = last_over >= c.seconds - 1 ? 1e9 : last_over - last_end;
    }
    // 映像と、乱れの前後の動き
    auto in_any = [](double t, const std::vector<std::pair<double, double>>& list) {
        for (const auto& [from, until] : list) {
            if (t > from && t <= until) return true;
        }
        return false;
    };
    double first_low_sec = -1;
    for (const auto& s : seconds) {
        if (s.headroom_ms > r.headroom_max) r.headroom_max = s.headroom_ms;
        r.queue_drops += s.queue_drops;
        r.video_delay_max = std::max(r.video_delay_max, s.video_delay_ms);
        if (s.reasons.find("repeating") != std::string::npos) ++r.repeats;
        if (s.reasons.find("re-anchor") != std::string::npos) {
            ++r.reanchors;
            if (first_low_sec >= 0 && r.reanchor_delay_sec < 0) r.reanchor_delay_sec = s.t - first_low_sec;
        }
        if (!c.video || s.t <= 10) continue;  // 開始直後は見ない
        if (s.video_received >= 10 && s.video_fps * 2 < s.video_received && first_low_sec < 0) first_low_sec = s.t;
        if (c.min_fps > 0 && s.video_fps < c.min_fps && !in_any(s.t, c.fps_grace)) ++r.low_fps_seconds;
        if (settled_at(s.t - 1)) r.fps_min = std::min(r.fps_min, double(s.video_fps));
    }
    if (c.catch_up_within.second > 0) {
        for (const auto& s : seconds) {
            if (s.t > c.catch_up_within.first && s.reasons.find("catch up") != std::string::npos) {
                r.catch_up_at = s.t - c.catch_up_within.first;
                break;
            }
        }
    }
    // 開始直後の余り (備えを除いた分) と、余っているのに遅らせ続けたか
    auto excess_of = [&](double t) {
        for (const auto& s : seconds) {
            if (s.t >= t) return s.min_slack_ms - s.headroom_ms;
        }
        return 0.0;
    };
    if (c.excess_check_sec > 0) r.excess_at = excess_of(c.excess_check_sec);
    if (c.spare_window.second > 0) {
        int up = 0;
        for (const auto& [t, d] : all_moves) {
            if (t < c.spare_window.first || t >= c.spare_window.second) continue;
            up = d > 0 && excess_of(t) > kSpareMs ? up + 1 : 0;
            r.up_while_spare = std::max(r.up_while_spare, up);
        }
    }
    int run = 0, last_sign = 0;
    for (const auto& [t, d] : all_moves) {
        if (c.output_off.second > 0 && t >= c.output_off.first && t < c.output_off.first + c.output_off.second) ++r.moves_while_off;
        if (c.moves_window.second > 0 && t >= c.moves_window.first && t < c.moves_window.second) {
            ++r.moves_in_window;
            const int sign = d > 0 ? 1 : -1;
            run = sign == last_sign ? run + 1 : 1;
            last_sign = sign;
            r.same_dir_run = std::max(r.same_dir_run, run);
        }
    }
    if (c.output_off.second > 0) {
        const double resume = c.output_off.first + c.output_off.second;
        for (const auto& s : seconds) {
            if (s.t > resume && s.t <= resume + c.resume_settle_sec && s.reasons.find("fast") != std::string::npos) ++r.fast_after_resume;
        }
    }
    {
        std::lock_guard lk(st.reason_mutex);
        r.last_resync = st.last_resync_reason;
    }
    if (r.delay_min > r.delay_max) r.delay_min = r.delay_max = 0;
    if (r.headroom_min > r.headroom_max) r.headroom_min = r.headroom_max = -1;
    struct Check {
        const char* what;
        bool ok;
    };
    const Check checks[] = {
        {"slips", r.slips <= c.max_slips},
        {"resyncs", r.resyncs <= c.max_resyncs && r.resyncs >= c.min_resyncs},
        {"settled resyncs", r.settled_resyncs <= c.max_settled_resyncs},
        {"disturbed resyncs", r.disturbed_resyncs <= c.max_disturbed_resyncs},
        {"underruns", r.underruns <= c.max_underruns},
        {"settled underruns", r.settled_underruns <= c.max_settled_underruns},
        {"underruns later in the disturbance", r.underruns_from <= c.max_underruns_from},
        {"moved", r.moved_ms <= c.max_moved_ms},
        {"moves", r.moves <= c.max_moves},
        {"reversals", r.reversals <= c.max_reversals},
        {"settled jumps", r.settled_jumps <= c.max_settled_jumps},
        {"late chunks", r.late_chunks <= c.max_late_chunks},
        {"delay growth", r.delay_growth_ms <= c.max_delay_growth_ms},
        {"settled delay", r.delay_max <= c.max_settled_delay_ms},
        {"delay keeps growing", r.late_delay_growth_ms <= c.max_late_delay_growth_ms},
        {"recovery", r.recovery_sec <= c.max_recovery_sec},
        {"delay change", c.delay_check.second == 0 || r.delay_growth_ms >= c.min_delay_growth_ms},
        {"video fps", r.low_fps_seconds == 0},
        {"repeating", r.repeats <= c.max_repeats},
        {"re-anchors", r.reanchors >= c.min_reanchors && r.reanchors <= c.max_reanchors},
        {"re-anchor delay", r.reanchors == 0 || r.reanchor_delay_sec <= c.max_reanchor_delay_sec},
        {"headroom", r.headroom_max <= c.max_headroom_ms},
        {"excess after start", std::fabs(r.excess_at) <= c.max_excess_ms},
        {"delayed while spare", r.up_while_spare <= c.max_up_while_spare},
        {"catch up", c.catch_up_within.second == 0 || (r.catch_up_at >= 0 && r.catch_up_at <= c.catch_up_within.second)},
        {"moves in window", r.moves_in_window <= c.max_moves_in_window && r.same_dir_run <= c.max_same_dir_run},
        {"moves while output stopped", r.moves_while_off <= c.max_moves_while_off},
        {"fast after resume", r.fast_after_resume <= c.max_fast_after_resume},
    };
    std::string failed;
    for (const auto& check : checks) {
        if (check.ok) continue;
        if (!failed.empty()) failed += ", ";
        failed += check.what;
    }
    r.ok = failed.empty();
    std::string extra;
    if (c.delay_check.second > 0) {
        extra +=
            fmt::format(", delay grew {:+.1f} ms from {:.0f} s to {:.0f} s", r.delay_growth_ms, c.delay_check.first, c.delay_check.second);
    }
    if (r.recovery_sec >= 0) extra += r.recovery_sec >= 1e9 ? ", never recovered" : fmt::format(", recovered in {:.0f} s", r.recovery_sec);
    if (c.video) {
        extra += fmt::format(", video fps min {:.0f} ({} low s), queue drops {}, video delay max {:.0f} ms",
                             r.fps_min < 1e9 ? r.fps_min : 0.0, r.low_fps_seconds, r.queue_drops, r.video_delay_max);
    }
    if (r.repeats > 0) extra += fmt::format(", repeating {}", r.repeats);
    if (r.reanchors > 0) extra += fmt::format(", re-anchored {} ({:.0f} s after output fell)", r.reanchors, r.reanchor_delay_sec);
    if (c.catch_up_within.second > 0)
        extra += r.catch_up_at >= 0 ? fmt::format(", caught up after {:.0f} s", r.catch_up_at) : ", no catch up";
    if (c.moves_window.second > 0) extra += fmt::format(", {} moves in window (run {})", r.moves_in_window, r.same_dir_run);
    if (c.excess_check_sec > 0) extra += fmt::format(", excess {:+.1f} ms at {:.0f} s", r.excess_at, c.excess_check_sec);
    if (c.spare_window.second > 0) extra += fmt::format(", delayed {} times in a row while spare", r.up_while_spare);
    if (c.output_off.second > 0)
        extra += fmt::format(", {} moves while stopped, {} fast after resume", r.moves_while_off, r.fast_after_resume);
    if (!r.last_resync.empty()) extra += ", last resync: " + r.last_resync;
    if (!failed.empty()) extra += " [failed: " + failed + "]";
    std::printf(
        "%s %s%s: slips %llu, resyncs %llu (settled %llu, disturbed %llu), underruns %llu (settled %llu), time base moved %.1f ms, "
        "%d settled moves (%d reversed, %d jumps), audio slack %.1f ms min / %.1f ms max of 1 s minimums, %d late chunks, "
        "audio delay %.0f-%.0f ms (last %.0f), headroom %.0f-%.0f ms (last %.0f)%s\n",
        r.ok ? "OK  " : "FAIL", c.name.c_str(), c.seeds > 1 ? fmt::format(" [seed {}]", seed).c_str() : "", (unsigned long long)r.slips,
        (unsigned long long)r.resyncs, (unsigned long long)r.settled_resyncs, (unsigned long long)r.disturbed_resyncs,
        (unsigned long long)r.underruns, (unsigned long long)r.settled_underruns, r.moved_ms, r.moves, r.reversals, r.settled_jumps,
        r.min_slack_ms < 1e9 ? r.min_slack_ms : 0.0, r.max_window_slack_ms, r.late_chunks, r.delay_min, r.delay_max, r.delay_last,
        r.headroom_min, r.headroom_max, r.headroom_last, extra.c_str());
    if (!csv_path.empty()) write_csv(csv_path, c, seconds);
    return r;
}

// 1 回の途切れで、いつもなら at に届くものの遅れ (ms)。from から ms の間止まり、止まっていた分は speed 倍の速さで
// 順に届く (RTMP / TCP。実機では 790 ms の途切れの後に 62 ms の遅れが続いた)。speed = 0 なら止まり終わりに一度に届く
double stall_late(double at, double from, double ms, double speed) {
    const double until = from + ms / 1000.0;
    if (at < from) return 0.0;
    if (speed <= 0) return at < until ? (until - at) * 1000.0 : 0.0;
    const double delivered = until + (at - from) / speed;  // 止まり終わりから speed 倍で届け直す
    return delivered > at ? (delivered - at) * 1000.0 : 0.0;
}

// 途切れ (S1、TCP 型): {止まり始める時刻 (秒), 止まる長さ (ms)}。止まっていた間の分は、止まり終わりにまとめて届く
// (speed > 0 なら、その速さで順に届く)
LateFn stalls(std::vector<std::pair<double, double>> list, double speed = 0) {
    return [list, speed](int, double sent, uint32_t) {
        const double at = sent + kNetDelayMs / 1000.0;  // いつもなら届く時刻
        double late = 0;
        for (const auto& [from, ms] : list) late = std::max(late, stall_late(at, from, ms, speed));
        return late;
    };
}

// 繰り返す途切れ (S2): from から until まで every 秒ごとに、min_ms〜max_ms (種で決まる) 止まる (speed は stalls と同じ)
LateFn repeated_stalls(double from, double every, double until, double min_ms, double max_ms, double speed = 0) {
    return [=](int, double sent, uint32_t seed) {
        const double at = sent + kNetDelayMs / 1000.0;
        if (at < from) return 0.0;
        const int i = int((at - from) / every);
        const double begin = from + i * every;
        if (begin >= until) return 0.0;
        Rng g{seed * 2246822519u + uint32_t(i) * 3266489917u};
        g.next();
        const double ms = min_ms + g.next() * (max_ms - min_ms);
        return stall_late(at, begin, ms, speed);
    };
}

// 帯域不足 (R1): from から seconds 秒の間、届く速さが rate 倍になり、その後に溜まった分がまとめて届く
LateFn throttle(double from, double seconds, double rate) {
    return [=](int, double sent, uint32_t) {
        const double at = sent + kNetDelayMs / 1000.0;
        const double caught = from + seconds * rate;  // 詰まりが終わるまでに届けられる、いつもの到着時刻
        if (at < from || at >= from + seconds) return 0.0;
        if (at < caught) return (from + (at - from) / rate - at) * 1000.0;
        return (from + seconds - at) * 1000.0;
    };
}

}  // namespace

int main(int argc, char** argv) {
    int only = 0;
    std::string csv_dir;
    std::string name_filter;  // --name: 名前がこれで始まる場合だけ回す
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "-v") == 0) {
            g_verbose = true;
            if (i + 1 < argc && std::isdigit((unsigned char)argv[i + 1][0])) only = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--csv") == 0 && i + 1 < argc) {
            csv_dir = argv[++i];
        } else if (std::strcmp(argv[i], "--name") == 0 && i + 1 < argc) {
            name_filter = argv[++i];
        }
    }
    // worker のログは、時間軸を動かした理由を CSV に書くために debug まで集める (表示は -v のときだけ)
    auto logger =
        std::make_shared<spdlog::logger>("test", std::make_shared<spdlog::sinks::callback_sink_mt>([](const spdlog::details::log_msg& msg) {
                                             const std::string line(msg.payload.data(), msg.payload.size());
                                             g_log.push_back(line);
                                             if (g_verbose || msg.level >= spdlog::level::warn) std::printf("  %s\n", line.c_str());
                                         }));
    logger->set_level(spdlog::level::debug);
    spdlog::set_default_logger(logger);
    if (!csv_dir.empty()) std::filesystem::create_directories(csv_dir);

    // 鳴らし始めた直後に、届くのが何度か止まり、止まっていた分がまとめて届く (RTMP pull の起動直後。
    // 揺れの備えが 10 ms から 45 ms ほどまで増え、60 秒遅れて届かなければ一度に下げる: 合わせ直しが 1 回増える)。
    const auto startup_late = stalls({{2.0, 34}, {3.0, 58}, {4.5, 79}});
    std::vector<Case> cases = {
        {.name = "steady, no clock difference", .seconds = 60, .max_slips = 0, .max_resyncs = 0},
        {.name = "sender clock 150 ppm fast", .seconds = 120, .sender_ppm = 150, .max_slips = 1000, .max_resyncs = 0},
        {.name = "device clock 150 ppm fast", .seconds = 120, .device_ppm = 150, .max_slips = 1000, .max_resyncs = 0},
        {.name = "late arrivals right after start (delay grows ~35 ms)",
         .seconds = 90,
         .late_ms = startup_late,
         .max_slips = 100,
         .max_resyncs = 4},
        {.name = "late arrivals right after start, with 24 ppm drift",
         .seconds = 90,
         .sender_ppm = 24,
         .late_ms = startup_late,
         .max_slips = 300,
         .max_resyncs = 4},
        {.name = "device opens 5 s after the stream started", .seconds = 60, .device_open_sec = 5, .max_slips = 100, .max_resyncs = 3},
        // 設計で追うクロック差 (200 ppm) より大きい場合: 時間軸は 2 秒に 1 ms (500 ppm) までしか追わない
        // (毎秒動かすと engine の重複・間引きが 1 秒に 48 回になり、2 秒続くと合わせ直しになる)。余っている側は直近 2 分の
        // 窓で見るので、詰め始めるのは遅れる。追いきれない遅延の余りは 3 フレームを超えたら一度に戻す (合わせ直し 1 回)
        {.name = "sender clock 400 ppm fast (within what the time base follows)",
         .seconds = 60,
         .sender_ppm = 400,
         .max_slips = 1500,
         .max_resyncs = 0},
        {.name = "sender clock 1000 ppm fast (far faster than the time base follows)",
         .seconds = 120,
         .sender_ppm = 1000,
         .max_slips = 3000,
         .max_resyncs = 4,
         .min_resyncs = 1},
        // 落ち着いた後の揺れ (実機の DVS + RTMP で窓ごとに ±2〜4 ms): 揺れに反応して時間軸を 1 ms ずつ行き来させない。
        // クロック差は、揺れがあっても合わせ直しなしで追う
        {.name = "arrivals jitter +-4 ms for 5 min",
         .seconds = 300,
         .jitter_ms = 4,
         .max_slips = 1000,
         .max_resyncs = 0,
         .max_moves = 5,
         .max_reversals = 0},
        // 開始直後に増えた揺れの備えは 30 秒に 1 ms ずつ減る。減った分 (5 分で約 9 ms) は、幅を超えてから 1 ms ずつ詰める
        {.name = "arrivals jitter +-4 ms for 5 min, late arrivals right after start",
         .seconds = 300,
         .jitter_ms = 4,
         .late_ms = startup_late,
         .max_slips = 1000,
         .max_resyncs = 4,
         .max_moves = 10,
         .max_reversals = 0},
        {.name = "arrivals jitter +-8 ms for 5 min",
         .seconds = 300,
         .jitter_ms = 8,
         .max_slips = 1000,
         .max_resyncs = 1,
         .max_moves = 5,
         .max_reversals = 0},
        // ネットワークの大きな遅れ (途切れ 900 ms と、その後のまとめ届きの遅れ): 時間軸はすぐ遅らせ、余りは一度に戻す。
        // 揺れの備えは途切れで増やさない (増やすと、30 秒に 1 ms ずつしか減らず、約 1 時間遅延が 100 ms 以上多いままだった)
        {.name = "arrivals jitter +-4 ms, a 900 ms outage at 60 s",
         .seconds = 240,
         .jitter_ms = 4,
         .late_ms = stalls({{60.0, 900}, {61.5, 25}, {62.5, 42}}),
         .max_slips = 1000,
         .max_resyncs = 3,
         .delay_check = {55, 180},
         .max_delay_growth_ms = 20},
        // 揺れが本当に大きい回線 (±30 ms が続く): 揺れの備えはそれに合った大きさに保ち、遅れて届くのを続けない
        {.name = "arrivals jitter +-30 ms for 5 min",
         .seconds = 300,
         .jitter_ms = 30,
         .max_slips = 3000,
         .max_resyncs = 10,
         .max_late_chunks = 200},
        {.name = "arrivals jitter +-4 ms, sender clock 150 ppm fast",
         .seconds = 300,
         .sender_ppm = 150,
         .jitter_ms = 4,
         .max_slips = 3000,
         .max_resyncs = 0,
         .max_reversals = 0},
        {.name = "arrivals jitter +-4 ms, sender clock 200 ppm fast",
         .seconds = 300,
         .sender_ppm = 200,
         .jitter_ms = 4,
         .max_slips = 4000,
         .max_resyncs = 0,
         .max_reversals = 0},
        {.name = "arrivals jitter +-4 ms, sender clock 200 ppm slow",
         .seconds = 300,
         .sender_ppm = -200,
         .jitter_ms = 4,
         .max_slips = 4000,
         .max_resyncs = 0,
         .max_reversals = 0},
        {.name = "arrivals jitter +-4 ms, device clock 200 ppm fast",
         .seconds = 300,
         .device_ppm = 200,
         .jitter_ms = 4,
         .max_slips = 4000,
         .max_resyncs = 0,
         .max_reversals = 0},
        {.name = "arrivals jitter +-5 ms", .seconds = 60, .jitter_ms = 5, .max_slips = 100, .max_resyncs = 2},
        {.name = "arrivals jitter +-5 ms, late arrivals right after start",
         .seconds = 60,
         .jitter_ms = 5,
         .late_ms = startup_late,
         .max_slips = 100,
         .max_resyncs = 4},
        // 出力遅延が変わる (WDM → ASIO の切り替えなど。開き直したあとの鳴らし始めも 1 回と数える)
        {.name = "output delay 32 -> 29 ms at 20 s",
         .seconds = 60,
         .jitter_ms = 5,
         .delay_changes = {{20, 29}},
         .max_slips = 100,
         .max_resyncs = 2},
        // 音声を「なし」にして戻す: なしの間は時間軸 (映像) を動かさない
        {.name = "output delay 32 -> none -> 32 ms (20 s / 30 s)",
         .seconds = 60,
         .jitter_ms = 5,
         .delay_changes = {{20, 0}, {30, 32}},
         .max_slips = 100,
         .max_resyncs = 2,
         .max_moved_ms = 40},

        // ---- 不安定な回線 (「不安定な回線での時間軸の検証手順」の場合 1〜11)。映像も届け、種を 5 つ回す。
        // 落ち着いた後 = 開始と各乱れが止んでから 60 秒以降。落ち着いた後は一度に動かさない (settled jumps 0)
        {.name = "1. J1 jitter +-10 ms, 10 min",
         .seconds = 600,
         .seeds = 5,
         .video = true,
         .jitter_ms = 10,
         .in_order = true,
         .settle_sec = 60,
         .max_slips = 3000,
         .max_resyncs = 1'000,
         .max_settled_resyncs = 0,
         .max_settled_underruns = 0,
         .max_moves = 3,
         .max_reversals = 0,
         .max_settled_jumps = 0},
        // 揺れの幅が大きいと、時間軸がその幅を覆う所まで (直近の窓で見えた悪い揺れに合わせて) 1 ms ずつ遅らせ終えるのに
        // 80 秒ほどかかる種がある。その分の同じ向きの移動は許す (行き来は 0 のまま)
        {.name = "2. J1 jitter +-30 ms, 10 min",
         .seconds = 600,
         .seeds = 5,
         .video = true,
         .jitter_ms = 30,
         .in_order = true,
         .settle_sec = 60,
         .max_slips = 6000,
         .max_resyncs = 1'000,
         .max_settled_resyncs = 0,
         .max_settled_underruns = 0,
         .max_moves = 8,
         .max_reversals = 0,
         .max_settled_jumps = 0,
         .max_settled_delay_ms = 100},
        {.name = "3. J1 jitter +-100 ms, 10 min",
         .seconds = 600,
         .seeds = 5,
         .video = true,
         .jitter_ms = 100,
         .in_order = true,
         .settle_sec = 60,
         .max_slips = 20000,
         .max_resyncs = 1'000,
         .max_settled_underruns = 0,
         .max_reversals = 2,
         .max_settled_jumps = 0,
         .max_settled_delay_ms = 250,
         .max_late_delay_growth_ms = 20},
        // J2: 95% は ±10 ms、5% は +50〜200 ms (送った順にしか届かないので、遅れたかたまりの後ろも待たされる)
        {.name = "4. J2 heavy-tailed jitter, 10 min",
         .seconds = 600,
         .seeds = 5,
         .video = true,
         .chunk_jitter_ms = 10,
         .tail_prob = 0.05,
         .in_order = true,
         .settle_sec = 60,
         .max_slips = 20000,
         .max_resyncs = 1'000,
         .max_settled_underruns = 2,  // 開始 60 秒以降の 9 分で、5 分あたり 1 回まで
         .max_reversals = 2,
         .max_settled_jumps = 0},
        {.name = "5. J1 +-4 ms with an S1 900 ms outage at 60 s",
         .seconds = 300,
         .seeds = 5,
         .video = true,
         .jitter_ms = 4,
         .late_ms = stalls({{60.0, 900}}),
         .in_order = true,
         .settle_sec = 60,
         .disturbances = {{60, 61}},
         .max_slips = 3000,
         .max_resyncs = 1'000,
         .max_settled_resyncs = 0,
         .max_disturbed_resyncs = 3,
         .max_underruns = 1,
         .max_settled_jumps = 0,
         .delay_check = {55, 181},
         .max_delay_growth_ms = 20},
        // V1: 映像だけが 850 ms 遅れ、音声は 40 ms 遅れる (実機の「映像 853 ms・音声 42 ms」)
        {.name = "6. J1 +-4 ms with a V1 video-only 850 ms outage at 60 s",
         .seconds = 300,
         .seeds = 5,
         .video = true,
         .jitter_ms = 4,
         .late_ms = stalls({{60.0, 40}}),
         .video_late_ms = stalls({{60.0, 850}}),
         .in_order = true,
         .settle_sec = 60,
         .disturbances = {{60, 61}},
         .max_slips = 3000,
         .max_resyncs = 1'000,
         .max_settled_resyncs = 0,
         .max_disturbed_resyncs = 3,
         .max_underruns = 1,
         .max_settled_jumps = 0,
         .delay_check = {55, 181},
         .max_delay_growth_ms = 20},
        // S2: 300〜800 ms の途切れが 30 秒ごとに 5 分続き、その後 5 分は落ち着いている
        {.name = "7. J1 +-4 ms with S2 300-800 ms outages every 30 s for 5 min",
         .seconds = 660,
         .seeds = 5,
         .video = true,
         .jitter_ms = 4,
         .late_ms = repeated_stalls(60, 30, 360, 300, 800),
         .in_order = true,
         .settle_sec = 60,
         .disturbances = {{60, 361}},
         .max_slips = 20000,
         .max_resyncs = 1'000,
         .max_settled_resyncs = 1,  // 戻すときの 1 回
         .max_settled_underruns = 0,
         .underruns_from_sec = 120,  // 3 回目の途切れから
         .max_underruns_from = 2,
         .max_settled_jumps = 1,
         .max_recovery_sec = 180},
        {.name = "8a. J1 +-30 ms, sender clock 200 ppm fast, 10 min",
         .seconds = 600,
         .sender_ppm = 200,
         .seeds = 5,
         .video = true,
         .jitter_ms = 30,
         .in_order = true,
         .settle_sec = 60,
         .max_slips = 10000,
         .max_resyncs = 1'000,
         .max_settled_resyncs = 0,
         .max_settled_underruns = 0,
         .max_reversals = 0,
         .max_settled_jumps = 0},
        {.name = "8b. J1 +-30 ms, sender clock 200 ppm slow, 10 min",
         .seconds = 600,
         .sender_ppm = -200,
         .seeds = 5,
         .video = true,
         .jitter_ms = 30,
         .in_order = true,
         .settle_sec = 60,
         .max_slips = 10000,
         .max_resyncs = 1'000,
         .max_settled_resyncs = 0,
         .max_settled_underruns = 0,
         .max_reversals = 0,
         .max_settled_jumps = 0},
        // L1: 単独の欠落 1% と、10 個まとめての欠落を 1 分に 1 回 (SRT。欠落は無音で埋まる)
        {.name = "9. J1 +-4 ms with L1 losses, 10 min",
         .seconds = 600,
         .seeds = 5,
         .video = true,
         .jitter_ms = 4,
         .loss_prob = 0.01,
         .loss_burst_every_sec = 60,
         .loss_burst_chunks = 10,
         .settle_sec = 60,
         .max_slips = 3000,
         .max_resyncs = 1'000,
         .max_settled_resyncs = 0,
         .max_moves = 3,
         .max_settled_jumps = 0},
        // R1: 60 秒のところで 10 秒間、届く速さが 0.9 倍になり、その後に追いつく
        {.name = "10. J1 +-4 ms with R1 10 s at 0.9x from 60 s",
         .seconds = 300,
         .seeds = 5,
         .video = true,
         .jitter_ms = 4,
         .late_ms = throttle(60, 10, 0.9),
         .in_order = true,
         .settle_sec = 60,
         .disturbances = {{60, 70}},
         .max_slips = 10000,
         .max_resyncs = 1'000,
         .max_settled_resyncs = 1,  // 追いついた後に、余分な遅延を一度に戻す 1 回
         .max_underruns = 1,
         .max_settled_jumps = 1,
         .max_recovery_sec = 120},
        // 出力先の切り替え (出力遅延 32 → 29 ms): 切り替え直後の乱れは問わない
        {.name = "11. J1 +-30 ms, output delay 32 -> 29 ms at 60 s",
         .seconds = 300,
         .seeds = 5,
         .video = true,
         .jitter_ms = 30,
         .in_order = true,
         .delay_changes = {{60, 29}},
         .settle_sec = 60,
         .disturbances = {{60, 61}},
         .max_slips = 6000,
         .max_resyncs = 1'000,
         .max_settled_underruns = 0,
         .max_reversals = 0,
         .max_settled_jumps = 0},
    };
    // ---- 途切れの後の映像 (「b7ea6e9: DVS 実機確認の結果と修正案」の T1〜T7)。映像は待ち行列と出力スレッドの判断を通して出す。
    // RTMP は止まっていた分を 4 倍の速さで順に届け、SRT は止まり終わりに一度に届ける
    auto t1 = [&](const char* name, double fps, double stall_speed, bool rtmp) {
        Case c{.name = name,
               .seconds = 190,
               .seeds = 5,
               .video = true,
               .video_fps = fps,
               .jitter_ms = 4,
               // 790 ms 止まり、届き直した 84 ms 後にもう一度 62 ms 遅れる (実機の ch1)
               .late_ms = stalls({{60.0, 790}, {60.874, 62}}, stall_speed),
               .in_order = rtmp,
               .settle_sec = 60,
               .disturbances = {{60, 61}},
               .max_slips = 1'000'000,
               .max_resyncs = 1'000,
               .delay_check = {59, 70},
               .max_delay_growth_ms = 10,
               .min_delay_growth_ms = -10,
               .min_fps = fps * 55 / 60,
               .fps_grace = {{60, 66}},
               .max_repeats = 0,
               .max_reanchors = 0,
               .catch_up_within = {60, 5}};
        return c;
    };
    auto t2 = [&](const char* name, double stall_speed, bool rtmp) {
        Case c{.name = name,
               .seconds = 190,
               .seeds = 5,
               .video = true,
               .jitter_ms = 4,
               .late_ms = stalls({{60.0, 70}}, stall_speed),
               .in_order = rtmp,
               .settle_sec = 60,
               .disturbances = {{60, 61}},
               .max_slips = 1'000'000,
               .max_resyncs = 1'000,
               .max_reanchors = 0,
               .catch_up_within = {60, 5},
               .moves_window = {65, 185},
               .max_moves_in_window = 6};
        return c;
    };
    auto t3 = [&](const char* name, double fps) {
        Case c{.name = name,
               .seconds = 660,
               .seeds = 5,
               .video = true,
               .video_fps = fps,
               .jitter_ms = 4,
               .late_ms = repeated_stalls(60, 30, 360, 500, 500, 4),
               .in_order = true,
               .settle_sec = 60,
               .disturbances = {{60, 361}},
               .max_slips = 1'000'000,
               .max_resyncs = 1'000,
               .max_recovery_sec = 180,
               .min_fps = fps * 55 / 60,
               .max_reanchors = 0,
               .max_headroom_ms = 1000};
        for (int i = 0; i < 10; ++i) c.fps_grace.push_back({60.0 + 30 * i, 62.0 + 30 * i});  // 各途切れから 2 秒
        return c;
    };
    cases.push_back(t1("T1. RTMP 790 ms + 62 ms outage, 60 fps", 60, 4, true));
    cases.push_back(t1("T1s. SRT 790 ms + 62 ms outage, 60 fps", 60, 0, false));
    cases.push_back(t2("T2. RTMP 70 ms outage", 4, true));
    cases.push_back(t2("T2s. SRT 70 ms outage", 0, false));
    cases.push_back(t3("T3. RTMP 500 ms outages every 30 s for 5 min, 60 fps", 60));
    // 音声がまとめて届く送り側 (mpegts の PES に AAC を何フレームかずつ入れる): ふだんの届く間隔が 100 ms を超えても、
    // それを途切れと数えない (数えると「繰り返している」から抜けず、本当の途切れの後に余りを戻せなかった。burst_test.sh)
    for (const double bunch : {149.0, 180.0}) {
        for (const bool rtmp : {false, true}) {
            Case c{.name = fmt::format("T8{}. {} audio in {:.0f} ms PES, 600 ms outage", rtmp ? "" : "s", rtmp ? "RTMP" : "SRT", bunch),
                   .seconds = 190,
                   .seeds = 5,
                   .video = true,
                   .jitter_ms = 4,
                   .late_ms = stalls({{60.0, 600}}, rtmp ? 4 : 0),
                   .in_order = rtmp,
                   .audio_bunch_ms = bunch,
                   .settle_sec = 60,
                   .disturbances = {{60, 61}},
                   .max_slips = 1'000'000,
                   .max_resyncs = 1'000,
                   .delay_check = {59, 70},
                   .max_delay_growth_ms = 10,
                   .min_delay_growth_ms = -10,
                   .min_fps = 55,
                   .fps_grace = {{60, 66}},
                   .max_repeats = 0,
                   .max_reanchors = 0,
                   .catch_up_within = {60, 5}};
            cases.push_back(c);
        }
    }
    {
        // 帯域不足: 一度に 1 秒遅らせる瞬間は、映像もその分止まる (A/V を保つ)。その後は待ち行列から 60 fps で出す
        Case c{.name = "T4. R1 10 s at 0.9x from 60 s",
               .seconds = 300,
               .seeds = 5,
               .video = true,
               .jitter_ms = 4,
               .late_ms = throttle(60, 10, 0.9),
               .in_order = true,
               .settle_sec = 60,
               .disturbances = {{60, 70}},
               .max_slips = 1'000'000,
               .max_resyncs = 1'000,
               .max_settled_resyncs = 1,
               .max_underruns = 1,
               .max_settled_jumps = 1,
               .max_recovery_sec = 120,
               .min_fps = 55,
               .fps_grace = {{60, 62}},
               .max_reanchors = 0};
        cases.push_back(c);
    }
    {
        // 出力の停止 (デバイスを失って開き直す。同じ出力遅延で戻る): 停止の 1 秒前に小さな遅れが 4 回 (1〜8 ms)。
        // 停止中は音声の余裕が 20 ms 小さく報告される
        Case c{.name = "T5. output stopped for 30 s, back with the same output delay",
               .seconds = 240,
               .seeds = 5,
               .video = true,
               .jitter_ms = 4,
               .late_ms = stalls({{59.0, 11}, {59.25, 18}, {59.5, 15}, {59.75, 11}}),
               .in_order = true,
               .output_off = {60, 30},
               .output_off_slack_ms = 20,
               .settle_sec = 60,
               .disturbances = {{59, 90}},
               .max_slips = 1'000'000,
               .max_resyncs = 1'000,
               .max_reanchors = 0,
               .moves_window = {105, 225},
               .max_moves_in_window = 6,
               .max_same_dir_run = 5,
               .max_moves_while_off = 0,
               .max_fast_after_resume = 2};
        cases.push_back(c);
    }
    // 接続直後の最初の窓で、音声と映像の届き方が大きくずれる (RTMP でサーバーに溜まった分を受け取るとき)。その 4.5 秒後に
    // 音声が 5 ms 遅れて届く。最初の窓を揺れと数えて備えを跳ね上げない、余っているのに遅らせ続けない (N)
    for (const double gap : {30.0, 100.0, 230.0, 500.0}) {
        Case c{.name = fmt::format("T9. first window audio {:.0f} ms ahead of video, then 5 ms late", gap),
               .seconds = 180,
               .seeds = 5,
               .video = true,
               .jitter_ms = 3,
               .late_ms = stalls({{6.0, 17}}),
               .video_late_ms = [gap](int, double sent, uint32_t) { return sent < 2.0 ? gap : 0.0; },
               .in_order = true,
               .max_slips = 1'000'000,
               .max_resyncs = 1'000,
               .max_headroom_ms = 30,
               .excess_check_sec = 17,
               .max_excess_ms = 10,
               .spare_window = {6, 180},
               .max_up_while_spare = 3};
        cases.push_back(c);
    }
    {
        // RTMP で小さな途切れ (TCP の再送待ちなど) が 1 分ごとに続き、ずっと「繰り返している」間に、1 回だけ 1 秒の途切れ。
        // 備えは直近の途切れを覆う大きさに戻す (1 秒の途切れに合わせた備えを、小さな途切れが続く限り持ち続けない)。
        // 6 時間の試験 (RTMP 1536x864) で、1 秒の途切れの後、遅延が約 1 秒のまま何時間も戻らなかった
        std::vector<std::pair<double, double>> list;
        // 1 秒の途切れを覆う備えは、繰り返している間は kOutageNeedMemoryMs (4 分) 保ち、その後は小さな途切れに見合う大きさに戻す
        for (double t = 30; t < 660; t += 60) list.push_back({t, 150});
        list.push_back({200, 1000});
        std::sort(list.begin(), list.end());
        Case c{.name = "T10. RTMP 150 ms outages every 60 s, one 1000 ms outage at 200 s",
               .seconds = 660,
               .seeds = 5,
               .video = true,
               .jitter_ms = 4,
               .late_ms = stalls(list, 4),
               .in_order = true,
               .settle_sec = 60,
               .max_slips = 1'000'000,
               .max_resyncs = 1'000,
               .delay_check = {195, 560},
               .max_delay_growth_ms = 250,
               .min_fps = 55,
               .max_reanchors = 0};
        for (const auto& [t, ms] : list) c.fps_grace.push_back({t, t + (ms > 500 ? 6 : 2)});
        cases.push_back(c);
    }
    cases.push_back(t1("T6a. RTMP 790 ms + 62 ms outage, 30 fps", 30, 4, true));
    cases.push_back(t3("T6b. RTMP 500 ms outages every 30 s for 5 min, 30 fps", 30));
    {
        // 保険: 待ち行列を 300 ms にして、900 ms の途切れで遅延を約 900 ms にする (余りを戻すまでの約 5 秒、待ち行列に
        // 収まらない)。出せない状態を打ち切って取り直す
        Case c{.name = "T7. queue of 300 ms, delay of ~900 ms after an outage",
               .seconds = 200,
               .seeds = 1,
               .video = true,
               .queue_span_ms = 300,
               .jitter_ms = 4,
               .late_ms = stalls({{60.0, 900}}, 4),
               .in_order = true,
               .settle_sec = 60,
               .disturbances = {{60, 61}},
               .max_slips = 1'000'000,
               .max_resyncs = 1'000,
               .min_fps = 55,
               .fps_grace = {{60, 66}},
               .min_reanchors = 1,
               .max_reanchors = 1,
               .max_reanchor_delay_sec = 4};
        cases.push_back(c);
    }

    // デバイスの違い: 安定した回線 (1)、裾の重い揺れ (4)、繰り返す途切れ (7)、帯域不足 (10) を、基本遅延 10 / 32 / 100 ms と
    // 取る仕方 3 つの組み合わせでも回す。条件はどのデバイスでも同じ (デバイスに合わせて緩めない)
    const size_t base_cases = cases.size();
    for (size_t i = 0; i < base_cases; ++i) {
        const std::string& name = cases[i].name;
        // 途切れの後の映像 (T1〜T5) も同じ
        bool matrix = false;
        for (const char* prefix :
             {"1. ", "4. ", "7. ", "10. ", "T1. ", "T1s. ", "T2. ", "T2s. ", "T3. ", "T4. ", "T5. ", "T9. ", "T10. "}) {
            matrix |= name.rfind(prefix, 0) == 0;
        }
        if (!matrix) continue;
        for (double base : {10.0, 32.0, 100.0}) {
            for (Consume consume : {Consume::Steady, Consume::Blocks, Consume::Irregular}) {
                if (base == 32 && consume == Consume::Blocks) continue;  // 元の場合と同じ
                Case d = cases[i];
                d.base_delay_ms = base;
                d.consume = consume;
                d.name += fmt::format(" [device {:.0f} ms, {}]", base, consume_name(consume));
                cases.push_back(d);
            }
        }
    }
    bool ok = true;
    int ch = 1;
    for (size_t i = 0; i < cases.size(); ++i) {
        const auto& c = cases[i];
        if ((only == 0 || only == int(i) + 1) && c.name.rfind(name_filter, 0) == 0) {
            for (int s = 0; s < c.seeds; ++s) {
                const uint32_t seed = 12345u + 7919u * uint32_t(s);  // 種 1 つ目は前と同じ (今までの場合の結果を変えない)
                std::string csv;
                if (!csv_dir.empty()) {
                    std::string slug;
                    for (char ch2 : c.name) slug += std::isalnum((unsigned char)ch2) ? ch2 : '_';
                    csv = fmt::format("{}/{:02}-{}-seed{}.csv", csv_dir, i + 1, slug.substr(0, 60), s + 1);
                }
                ok &= run(c, ch % 8 + 1, seed, csv).ok;
                ++ch;
            }
        }
    }
    std::printf("%s\n", ok ? "all OK" : "FAILED");
    return ok ? 0 : 1;
}
