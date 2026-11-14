#pragma once
// 映像と音声で共有する時間軸 (メディア時刻 → 出すべき時刻)。設計書 §3.5。
//
// C(pts) = anchor_qpc + (pts - anchor_pts)。映像は C(pts) に仮想カメラへ出し、
// A/V 同期モードでは音声も C(pts) にデバイスから鳴るようにする。
//
// 時間軸の調整:
// - 最初に来たフレーム (映像でも音声でも) の到着時刻 + 再生バッファに合わせる。
// - 映像・音声それぞれ「到着から出すべき時刻までの余裕 (slack)」を報告してもらい、
//   1 秒ごとに、余裕の最も小さかった側が目標 (映像: 再生バッファ + 揺れの備え、音声: 揺れの備え) になるように動かす。
// - 方針: 見ている側に違和感の無い合わせ方にする。時間軸を一度に動かすと、engine はフェードして合わせ直し
//   (音が数十 ms 途切れる)、映像は動かした分だけ止まるか飛ぶ。1 ms ずつ動かすと、engine はサンプルの重複・間引き
//   (10 ms に 1 サンプルまで) で追い、映像も音声もほぼ分からない。
//   - すぐに直さないとどうしようもない大きさのずれは、一度に直す (1 回の短い乱れを許して、長く引きずらない)。
//     時間軸を作った直後の最初の調整、鳴らし始めてから / 出力先を変えてから kFastAdjustMs の間 (下)、音が間に合わない
//     とき (遅れて届いた、映像が遅れた、届く速さが足りない)、遅延の余りが続いたとき、揺れの備えが実際の揺れより
//     大きく余ったとき (途切れの後など)。
//   - 落ち着いた後は、揺れに反応して動かさない。本当にずれているときだけ 1 ms ずつ追う (下)。
//   - 出力先の停止・開始や切り替えの直後は、多少乱れてもよい (頻繁にする操作ではない)。その後に落ち着けばよい。
//   重複・間引きの回数そのものに実害はほぼ無いので、回数を減らすために一度に動かす場面を増やさない。
//   判断に使うのは、受け取ったデータの届き方と、engine が知らせる出力遅延だけ。デバイスの種類や名前にはよらない。
// - 落ち着いた後の 1 ms ずつの調整 (送出元とのクロック差、揺れの備えを減らした分): その窓のずれではなく、直近の窓の
//   ずれの最小 (揺れの悪い側の包絡。前の窓も今の時間軸と揺れの備えで計算し直す) で決める。直近 kHistoryWindows 窓の包絡が
//   動かさない幅 (band_) を超えて足りなければ遅らせ、直近 kLongHistoryWindows 窓の包絡が幅を超えて余っていれば
//   (= その間の窓がすべて余っていれば) 詰める。長い包絡は詰める側だけに使い、遅らせるのは直近 kHistoryWindows 窓で足りない
//   ときだけ。前に動かしてから kStepWindows 窓は空ける (engine の重複・間引きが同じ向きに続いて合わせ直しにならないように)。
//   - 最初の調整の窓と、鳴らし始めてすぐ (kFastAdjustMs) の窓は直近の窓に入れない。接続直後のまとめ届きで、音声と映像の
//     届き方が大きくずれる (RTMP で 234 ms) ので、揺れとして数えると備えを跳ね上げ、包絡に残って余りを戻せなかった。
//     測った揺れから備えの下限を決めるのも、kHistoryWindows 窓が溜まってから。
//   - 動かさない幅は、測った揺れ (直近の窓のずれの散らばり) の半分 (update_band_locked)。揺れの大きい回線ほど広い。
//   - これまでの形で起きたこと: 幅 2 ms では揺れのたびに ±1 ms ずつ行き来した (約 1 分 45 秒に 30 回以上)。その窓のずれで、
//     足りない側だけすぐに動かすと、揺れが下に振れるたびに +1 ms し、数秒後に良い窓に釣られて −1 ms で戻した。
//     足りない側も同じ向きが続くのを待つと、揺れの良い窓まで幅を超えるまで動かず、悪い窓が揺れの備えを超えて遅れた。
//     包絡なら、悪い窓が幅の内側に収まった所で止まり、良い窓に釣られて戻らない。
// - 鳴らし始めてすぐは、揺れの備えが増えたり engine の出力遅延が決まったりして、目標とのずれが大きく残りやすい。
//   1 秒に 1 ms ずつ詰めると数十秒かかり、その間 engine は 1 秒に 48 サンプルずつ重複・間引きし続ける
//   (DVS で起動直後に 900〜2,400 回)。そこで、時間軸を作ってから、または engine の出力遅延が変わってから
//   kFastAdjustMs の間は、はっきり大きいずれだけを一度に動かす:
//   - 同じ向きに kFastJumpMs を超えるずれが 2 窓続いたら、2 窓の小さい方だけ動かす。窓ごとの最小の余裕は
//     到着の揺れで ±数 ms 動くので、1 窓や 3 ms で動かすと、足りない・余っているを交互に判定して、そのたびに
//     音声が合わせ直しになった (開いた直後の 10 秒に 4〜8 回)。
//   - 遅延が kFastImmediateMs を超えて足りないときは、待たずに動かす (音が間に合わない。出力遅延が 0 → 32 ms など)。
//   - 一度に動かすのは kFastMaxMoves 回まで。残りは 1 ms ずつ。
//   それより後は、上の 1 ms ずつのまま。一度に動かしたあとに残る数 ms のずれも、幅を超えていればそれで詰める。
// - 一度に kEpochJumpMs を超えて動かしたら (遅れて届いて遅らせたときも) jumps() が増える。音声の書き手は
//   それを見て音声の epoch を進め、engine に (重複・間引きで追わずに) 1 回で合わせ直させる。
// - 遅延の余り: 短い途切れのあとにまとめて届くと、途切れた長さの分だけ遅らされ、揺れの備えを超えた分が余る。
//   1 秒に 1 ms では何分も戻らない。余り (直近 kLongHistoryWindows 窓の包絡) が 3 フレームを超えたまま kCatchUpWindows 回
//   続いたら一度に戻す (音声は 1 回合わせ直しになる)。途切れのあった窓とその後 kOutageTailMs の間の窓は数えないので、
//   途切れから約 5 秒で戻る。途切れが繰り返している間は戻さない (備えを保つ。下)。
// - 映像の待ち行列は、ここで遅らせうる最大 (kMaxExtraDelayMs) と再生バッファを覆う幅にしてある (playout_core.h)。
//   遅延が大きい間も、映像はコマ送りにならない。
// - 音声が遅れて届いた (余裕が負) ときは、届く前に鳴らすことはできないので即座に遅らせる。
//   映像はその分待たされる (A/V 同期を保つため)。映像が出す時刻 (再生バッファ) より 3 フレームを超えて遅れたときも、
//   遅れた分だけ一度に遅らせる (揺れの備えの内側で目標より小さいだけなら遅れではない)。
// - 届く速さが足りない (帯域不足: 1 窓で何度も遅れて届いた) ときは、一度に kShortfallShiftMs 遅らせ、追いついたら戻す。
// - engine の出力が鳴っていない間 (note_output_stopped) は、前の窓の音声のずれでは遅らせない。戻ったら前の窓を捨てて
//   開いた直後と同じく合わせ直す (restart_after_output_gap)。
// - 揺れの備え (headroom): 遅れて届くたびに、その遅れの分だけ増やす (同じ大きさの揺れが次は間に合う)。少なくとも
//   測った揺れの半分にする。遅れがしばらくなければ少しずつ減らす (30 秒に 1 ms。決め直した備えまで)。これがないと、
//   余裕を 0 まで詰めては次の揺れで遅れて時間軸が飛ぶ (音が途切れる) のを繰り返す (RTMP では約 1 分に 1 回)。
//   減らした分は遅延の余りになり、溜まって幅を超えたら、上の 1 ms ずつで詰める。
//   - 途切れ (届く間隔が kOutageGapMs とふだんの間隔の kOutageGapTimes 倍を超えて開いた、kOutageLateMs
//   を超えて遅れた、測った揺れより桁違いに遅れた
//     (kOutageSpreadTimes)。その直後 kOutageTailMs の間も):
//     1 回きりなら増やさない。時間軸は遅らせ、余りは caught up で一度に戻す。数百 ms の途切れで備えを増やすと、
//     30 秒に 1 ms ずつしか減らず、約 1 時間、遅延が 100 ms 以上多いままになった (868 / 853 ms の遅れの後、10 → 145 ms)。
//     前の途切れから kOutageMemoryMs 以内にまた途切れたら「繰り返している」とし、備えを途切れを覆う大きさに増やして保つ
//     (遅延が多少増えても、毎回途切れるよりよい)。途切れのあった窓とその後 kOutageTailMs の間の窓は、包絡にも決め直しにも
//     入れず、1 ms ずつも動かさない。映像の途切れは、フレームが届いた (デコードした) 時刻の間隔で見る (note_video_arrival)。
//   - 音声が kHeadroomReviewMs の間 (途切れではなく) 揺れで遅れて届かなければ、その間の窓ごとの最小の余裕の
//     散らばり (時間軸の動きを差し引いたもの) に上乗せした値で決め直す (音声・映像それぞれ)。今の備えがそれより
//     kHeadroomCutMs を超えて多ければ、一度に下げて時間軸も一度に詰める (余分な遅延を長く引きずらない)。
//     途切れが繰り返している間も決め直すが、直近 kOutageNeedMemoryMs にあった途切れを覆う大きさ (OutageNeeds) より下げない。
//     大きな途切れが 1 回だけで、あとは小さな途切れ (RTMP の再送待ちなど) が続くときは、小さな途切れに見合う大きさまで戻る。

#include <spdlog/spdlog.h>

#include <algorithm>
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "denpachan/win_util.h"

namespace denpachan::worker {

class PlayoutClock {
public:
    // now: 今の時刻 (QPC)。試験では仮想の時計を渡す (tools の audio_timing_test)
    using NowFn = int64_t (*)();
    explicit PlayoutClock(int buffer_ms, NowFn now = &qpc_now) : buffer_(qpc_from_ms(buffer_ms)), now_(now) {}

    // pts_us のフレームを出すべき時刻 (QPC)。時間軸がなければ (別セッションなら) ここで作る。
    int64_t due(uint64_t session, int64_t pts_us, int64_t arrival_qpc) {
        std::lock_guard lk(mutex_);
        if (!anchored_ || session != session_) anchor_locked(session, pts_us, arrival_qpc);
        return anchor_qpc_ + qpc_from_us(pts_us - anchor_pts_us_);
    }

    // 時間軸を取り直す (タイムスタンプの飛びなど)
    void reanchor(uint64_t session, int64_t pts_us, int64_t arrival_qpc) {
        std::lock_guard lk(mutex_);
        anchor_locked(session, pts_us, arrival_qpc);
    }

    // 時間軸を作り直す (映像が遅延に追いつけないとき。PlayoutCore::check_progress)。揺れの備えと途切れの記憶も捨てる
    // (残すと、作り直した時間軸もまた同じだけ遅らされ、同じことになる)
    void restart(uint64_t session, int64_t pts_us, int64_t arrival_qpc) {
        std::lock_guard lk(mutex_);
        audio_headroom_ = qpc_from_ms(kAudioHeadroomMinMs);
        video_headroom_ = 0;
        reviewed_headroom_ = 0;
        last_outage_qpc_ = 0;
        repeated_until_qpc_ = 0;
        audio_outage_needs_.clear();
        video_outage_needs_.clear();
        catch_up_pending_ = 0;
        anchor_locked(session, pts_us, arrival_qpc);
    }

    // 時間軸を捨てる (映像の途絶)。次に来たフレームで作り直す。
    void invalidate() {
        std::lock_guard lk(mutex_);
        anchored_ = false;
    }

    // 映像フレームの余裕 (出すべき時刻 - 到着時刻) を報告する
    void report_video(int64_t slack_qpc, int64_t frame_us) {
        std::unique_lock lk(mutex_);
        frame_us_ = frame_us;
        if (now_() < window_start_qpc_) return;  // 接続直後のまとめ届きの間は使わない
        min_video_slack_ = std::min(min_video_slack_, slack_qpc);
        adjust_if_due_locked();
        flush_log(lk);
    }

    // 映像のフレームが届いた (デコードした。キューに入れるたび。BasicFrameQueue::set_on_push)。届く間隔が、ふだんの間隔
    // より大きく開いたら途切れ (GapTracker)。出力スレッドが取り出す間隔では見ない: 遅延が大きい間は、出す時刻まで待つ分だけ取り出す間隔が
    // 開き、待つ間にキューから溢れたフレームもあるので、自分で「途切れ」を作り続けてしまった (取り出しが約 500 ms おきになり、
    // 毎回途切れと数えて「繰り返している」から抜けられなかった)
    void note_video_arrival(int64_t arrival_qpc) {
        std::unique_lock lk(mutex_);
        const int64_t prev = last_video_arrival_qpc_;
        last_video_arrival_qpc_ = arrival_qpc;
        const bool gap = prev != 0 && video_gaps_.outage(arrival_qpc - prev);
        if (!anchored_ || now_() < window_start_qpc_) return;  // 接続直後のまとめ届きの間は使わない
        if (gap) {
            debug_.push_back(fmt::format("playout: video arrival gap {:.0f} ms (outage)", qpc_to_us(arrival_qpc - prev) / 1000.0));
            note_outage_locked(now_());
        }
        flush_log(lk);
    }

    // 音声チャンクの余裕 (鳴らすべき時刻から出力遅延を引いたもの - 到着時刻) を報告する。途切れは、届いた時刻 (デコード
    // 完了) の間隔で見る (映像と同じ。音声はデコーダのスレッドから届いたときに呼ばれるので、呼ばれた時刻とほぼ同じ)
    void report_audio(int64_t slack_qpc, int64_t arrival_qpc) {
        std::unique_lock lk(mutex_);
        const int64_t now = now_();
        const int64_t prev_arrival = last_audio_arrival_qpc_;
        last_audio_report_qpc_ = now;
        last_audio_arrival_qpc_ = arrival_qpc;
        const bool gap = prev_arrival != 0 && audio_gaps_.outage(arrival_qpc - prev_arrival);
        if (now < window_start_qpc_) return;  // 接続直後のまとめ届きの間は使わない
        if (gap) {
            // 届く間隔が開いた: 途切れ。止まっていた間の分がまとめて届くので、続くかたまりも窓の最小に入れない
            // (揺れの備えが途切れを覆っていて遅れて届かなくても、途切れと分かるように)
            debug_.push_back(fmt::format("playout: audio arrival gap {:.0f} ms (outage)", qpc_to_us(arrival_qpc - prev_arrival) / 1000.0));
            note_outage_locked(now);
            last_audio_late_report_qpc_ = now;
        }
        ++window_audio_reports_;
        if (slack_qpc < -qpc_from_us(kShortfallMinLateUs)) {
            ++window_audio_lates_;
            window_audio_late_sum_ -= slack_qpc;
        }
        if (slack_qpc < 0) {
            // 遅れて届いた: 即座に遅らせ、同じ大きさの揺れが次は間に合うよう備えを増やす。
            // このかたまりは窓の最小には入れない (入れると、増やした備えとの差で次の調整がもう一度動かす)
            const int64_t late = -slack_qpc;
            const char* kind = "";
            if (!settled_) {
                // 最初の調整の前: 映像と音声の届き方の差 (同じ pts でも音声が後に届く) で、揺れではない。備えは増やさない
                kind = " before the first adjustment";
            } else if (outage_locked(late, now)) {
                // 途切れ: その途切れを覆うのに要る備えを覚える (繰り返している間の決め直しの下限。review_headroom_locked)
                audio_outage_needs_.add(now, std::min(audio_headroom_ + late, qpc_from_ms(kOutageHeadroomMaxMs)));
                if (repeated_outages_locked(now)) {
                    // 途切れが繰り返している: 遅延が増えても途切れない方がよいので、備えをその途切れを覆う大きさにする
                    grow_headroom_locked(audio_headroom_, late, kOutageHeadroomMaxMs, now, last_audio_late_qpc_, kOutageHeadroomMaxMs);
                    kind = " (repeated outages)";
                } else {
                    // 1 回きりの途切れ (とその直後のまとめ届き): 時間軸は遅らせるが、揺れの備えは増やさない (余りは caught up で一度に戻す)
                    kind = " (outage)";
                }
            } else {
                // 揺れで遅れて届いた: 揺れの備えを決め直す間を数え直す。途切れでは数え直さない (途切れが 1 分ごとに続くと、
                // 決め直しが来ず、大きな途切れに合わせた備えを持ち続けた)
                review_start_locked(now);
                grow_headroom_locked(audio_headroom_, late, kAudioHeadroomMaxMs, now, last_audio_late_qpc_, kHeadroomGrowMaxMs);
                // 備えが、測った揺れ (直近の窓のずれの散らばり) の半分より小さければ、そこまで増やす。遅れた分ずつでは、
                // 揺れの大きい回線で備えが小さいまま遅れて届き続け、決め直す (review_headroom_locked) 間も取れなかった
                // 揺れを測れるだけの窓 (kHistoryWindows) が溜まってから (それまでの散らばりは接続直後の届き方で、揺れではない)
                if (history_count_ >= kHistoryWindows) {
                    audio_headroom_ = std::max(audio_headroom_, std::min(history_spread_locked() / 2, qpc_from_ms(kAudioHeadroomMaxMs)));
                }
            }
            // 遅らせるのは遅れた分だけ (増やした備えの分まで一度に遅らせると、裾の重い揺れで遅らせすぎて戻すのを繰り返した)
            const int64_t shift = late;
            anchor_qpc_ += shift;
            // この窓でそれまでに測った余裕も、時間軸を遅らせた分だけ大きくなる。直さないと、遅らせる前の小さな余裕で
            // 次の調整が「まだ足りない」と見て、前の揺れの備えの分だけさらに遅らせてしまう (そして 1 秒に 1 ms ずつ戻す。
            // DVS の起動直後の重複・間引きの多くはこれだった)
            if (min_audio_slack_ != LLONG_MAX) min_audio_slack_ += shift;
            if (min_video_slack_ != LLONG_MAX) min_video_slack_ += shift;
            if (shift > qpc_from_ms(kEpochJumpMs)) ++jumps_;
            windows_since_move_ = 0;  // 1 ms ずつの移動は、ここからも kStepWindows 窓空ける (engine の重複・間引きが重ならないように)
            // 最初の調整の前は、遅らせた後のこのかたまりの余裕 (0) を窓の最小に入れる。入れないと、最初の調整が映像だけを見て
            // 遅延を詰め、音声がまた遅れて届く
            if (!settled_) min_audio_slack_ = std::min(min_audio_slack_, int64_t(0));
            log_.push_back(fmt::format("playout: audio arrived {:.1f} ms late{}; delay increased {:.1f} ms, audio headroom {:.0f} ms",
                                       qpc_to_us(late) / 1000.0, kind, qpc_to_us(shift) / 1000.0, qpc_to_us(audio_headroom_) / 1000.0));
        } else if (now - last_audio_late_report_qpc_ > qpc_from_ms(kBurstMs)) {
            min_audio_slack_ = std::min(min_audio_slack_, slack_qpc);
        }
        // 遅れて届いたかたまりと一緒に届いたもの (途切れのあとのまとめ届きの残り) も窓の最小に入れない。
        // 入れると、そのぶん余裕が小さく見えて時間軸をさらに遅らせ、そのあと何十秒もかけて 1 ms ずつ戻すことになる
        if (slack_qpc < 0) last_audio_late_report_qpc_ = now;
        // 届く速さが足りない (帯域不足): この窓で何度も遅れて届いた。窓の終わりを待たずに、その場で遅らせる
        if (settled_ && !shortfall_ && window_audio_lates_ >= kShortfallMinLates &&
            window_audio_lates_ * 100 >= window_audio_reports_ * kShortfallLatePercent &&
            window_audio_late_sum_ >= qpc_from_ms(kShortfallLateSumMs)) {
            start_shortfall_locked(now);
        }
        adjust_if_due_locked();
        flush_log(lk);
    }

    // engine の出力遅延が変わった (デバイスを開いた、開き直した): しばらく大きなずれを一度に詰める
    void allow_fast_adjust() {
        std::lock_guard lk(mutex_);
        start_fast_locked(now_());
    }

    // engine の出力遅延が delta だけ変わった (デバイスを開き直した): 時間軸を同じだけ動かす。音声の余裕は変わらず、
    // 映像は A/V を合わせるためにその分待つ (または早く出る)。残ったずれを 1 秒に 1 ms ずつ詰めて重複・間引きを続けないように
    void shift_for_output_delay(int64_t delta_qpc) {
        std::lock_guard lk(mutex_);
        if (!anchored_ || delta_qpc == 0) return;
        anchor_qpc_ += delta_qpc;
        if (min_video_slack_ != LLONG_MAX) min_video_slack_ += delta_qpc;
        history_count_ = 0;  // 音声の余裕と時間軸の関係が変わったので、前の窓とは比べない
        review_start_locked(now_());
        if (std::llabs(delta_qpc) > qpc_from_ms(kEpochJumpMs)) ++jumps_;
        debug_.push_back(fmt::format("playout: moved {:+.2f} ms (output delay changed)", qpc_to_us(delta_qpc) / 1000.0));
    }

    // engine の出力が鳴らなくなった (デバイスを失った、ドライバから呼ばれない、止めた)。その間は音声の余裕を報告しない
    // (AudioWriter)。前の窓の音声のずれでは遅らせない (鳴っていないので、音声のために遅らせる理由がない。止める前の
    // 「足りない」窓で 1 ms ずつ遅らせ続けた)。詰める側は前の窓の音声も見たまま (戻ったときに音声が間に合わなくならないように)
    void note_output_stopped() {
        std::lock_guard lk(mutex_);
        output_stopped_ = true;
        min_audio_slack_ = LLONG_MAX;  // この窓で止まる前に測った分も使わない
    }

    // engine の出力が鳴っていなかった (デバイスを失って開き直した、ドライバから呼ばれなかった) のが戻った: 出力遅延が
    // 変わっていなくても、止まっていた間の窓とは比べず (音声の余裕と時間軸の関係が前と同じとは限らない)、揺れの備えの
    // 決め直しも数え直し、開いた直後と同じく大きなずれをしばらく一度に詰める (allow_fast_adjust)。止まっていた間の
    // 「足りない」窓が残ると、余っていても 1 ms ずつ遅らせ続けた (約 4 分)
    void restart_after_output_gap() {
        std::lock_guard lk(mutex_);
        const int64_t now = now_();
        output_stopped_ = false;
        history_count_ = 0;
        excess_windows_ = 0;
        min_audio_slack_ = LLONG_MAX;
        review_start_locked(now);
        start_fast_locked(now);
        debug_.push_back("playout: output resumed; history cleared");
    }

    // 音声は届いているが、出力先が無い (engine の出力遅延が 0)。時間軸は音声の余裕で動かさない
    // (「なし」にしただけで映像の遅延が変わらないように)。届いていることだけ覚える (audio_reported_within)
    void note_audio_without_output(int64_t arrival_qpc) {
        std::lock_guard lk(mutex_);
        last_audio_report_qpc_ = now_();
        last_audio_arrival_qpc_ = arrival_qpc;
    }

    // 時間軸を一度に kEpochJumpMs を超えて動かした回数 (音声の書き手が epoch を進める)
    uint64_t jumps() {
        std::lock_guard lk(mutex_);
        return jumps_;
    }

    // 取り直しのたびに増える
    uint64_t generation() {
        std::lock_guard lk(mutex_);
        return generation_;
    }

    bool anchored_for(uint64_t session) {
        std::lock_guard lk(mutex_);
        return anchored_ && session_ == session;
    }

    // 時間軸を作ってから最初の調整が済んだか (音声はこれを待ってから鳴らし始める)
    bool settled() {
        std::lock_guard lk(mutex_);
        return anchored_ && settled_;
    }

    // 音声がこの時間内に届いているか (映像が途切れても、音声が時間軸を使い続けているか)
    bool audio_reported_within(int64_t qpc) {
        std::lock_guard lk(mutex_);
        return last_audio_report_qpc_ != 0 && now_() - last_audio_report_qpc_ < qpc;
    }

    int64_t buffer_qpc() const { return buffer_; }

    // 音声の揺れの備え (試験で推移を見る。audio_timing_test)
    int64_t audio_headroom_qpc() {
        std::lock_guard lk(mutex_);
        return audio_headroom_;
    }

private:
    // 溜めたログをロックを外してから書く (ログはファイルに書くので、ロックを持ったまま書くと、出力スレッドと
    // 音声のスレッドがその間待たされる)
    void flush_log(std::unique_lock<std::mutex>& lk) {
        if (log_.empty() && debug_.empty()) return;
        std::vector<std::string> lines, debug;
        lines.swap(log_);
        debug.swap(debug_);
        lk.unlock();
        for (const auto& line : lines) spdlog::info("{}", line);
        for (const auto& line : debug) spdlog::debug("{}", line);
    }

    static std::string fmt_ms(int64_t v) { return v == LLONG_MAX ? std::string("-") : fmt::format("{:+.1f}", qpc_to_us(v) / 1000.0); }

    void anchor_locked(uint64_t session, int64_t pts_us, int64_t arrival_qpc) {
        anchored_ = true;
        session_ = session;
        anchor_pts_us_ = pts_us;
        anchor_qpc_ = arrival_qpc + buffer_;
        ++generation_;
        settled_ = false;
        excess_windows_ = 0;
        history_count_ = 0;
        windows_since_move_ = kStepWindows;
        shortfall_ = false;
        window_audio_reports_ = 0;
        window_audio_lates_ = 0;
        window_audio_late_sum_ = 0;
        review_start_locked(now_());
        // 接続直後はストリーム解析中に溜まったフレームがまとめて届く (到着が実際より遅く見える)。
        // その間の報告は使わず、落ち着いてから最初の窓を始める。
        window_start_qpc_ = now_() + qpc_from_ms(kSettleMs);
        min_video_slack_ = LLONG_MAX;
        min_audio_slack_ = LLONG_MAX;
    }

    void adjust_if_due_locked() {
        const int64_t now = now_();
        // 途切れの後 kOutageTailMs が明けたら、その窓はそこで閉じる (次の窓を明けた時刻から始め、余りを早く戻せるように)
        const int64_t tail_end = last_outage_qpc_ != 0 ? last_outage_qpc_ + qpc_from_ms(kOutageTailMs) : 0;
        const bool tail_ended = window_start_qpc_ < tail_end && now >= tail_end;
        if (now - window_start_qpc_ < qpc_from_ms(1000) && !tail_ended) return;
        // 減らすのは、途切れが繰り返していない間だけ、決め直した備え (実際の揺れに見合う大きさ) まで
        if (!repeated_outages_locked(now)) {
            decay_headroom_locked(video_headroom_, 0, now, last_video_late_qpc_);
            decay_headroom_locked(audio_headroom_, std::max(qpc_from_ms(kAudioHeadroomMinMs), reviewed_headroom_), now,
                                  last_audio_late_qpc_);
        }
        // 目標からの余り (正: 遅延が大きすぎる / 負: 小さすぎる)。報告のなかった側は無視する。
        const int64_t video_error = min_video_slack_ != LLONG_MAX ? min_video_slack_ - buffer_ - video_headroom_ : LLONG_MAX;
        const int64_t audio_error = min_audio_slack_ != LLONG_MAX ? min_audio_slack_ - audio_headroom_ : LLONG_MAX;
        const int64_t error = std::min(video_error, audio_error);
        if (error != LLONG_MAX) {
            const int64_t jump = qpc_from_us(frame_us_ * 3);
            const int64_t limit = qpc_from_ms(kMaxStepMs);
            int64_t step = std::clamp(error, -limit, limit);
            // 途切れのあった窓 (届く間隔が開いた、大きく遅れた) は包絡にも決め直しにも入れない。遅らせた後の時間軸で見ると
            // 目標ちょうどに見え、その窓が残っている間は余りを詰められない (900 ms の途切れの後、戻るまで包絡の長さだけ待った)
            // 帯域不足で遅らせてから追いつくまで (音声の余裕が目標に戻るまで) の窓も同じ
            // 映像の遅れ: 出す時刻 (再生バッファ) に対して遅れた量 (正: 遅れた)。揺れの備えの内側で目標より小さいだけなら遅れではない
            // (目標との差で見ると、備えを増やしても途切れのたびに同じだけ足りなく見え、繰り返す途切れで備えが途切れの分ずつ増え続けた)
            const int64_t video_lateness = min_video_slack_ != LLONG_MAX ? buffer_ - min_video_slack_ : LLONG_MIN;
            if (video_lateness > qpc_from_ms(kOutageLateMs)) window_outage_ = true;
            // 途切れのあった窓と、途切れから kOutageTailMs の間に始まった窓 (まとめ届きの残りで、ずれが小さく見える) は、
            // 判断に使わない: 包絡に入れず、余りの数えも進めも戻しもせず、動かさない。映像が 3 フレームを超えて遅れたときだけ
            // 一度に遅らせる (音声が遅れて届いたときは report_audio で即座に遅らせる)。窓が明けたら、包絡は途切れより前の窓も
            // 今の時間軸で計算し直すので、余りがそのまま見え、2 窓で caught up になる (途切れから約 5 秒)。
            // 入れると、まとめ届きの窓が包絡を押し下げ、余りを一度に戻せなかった (70 ms の途切れの後 1 ms ずつ約 4 分、
            // 790 ms の後は戻せないまま遅延が待ち行列を超えてコマ送りになった)
            const bool after_outage = window_outage_ || window_start_qpc_ < tail_end;
            // 明けたら、次の窓で余りを一度に戻す (3 フレームに届かない余りも。2 窓続くのを待たない: 途切れより前の窓も包絡に
            // 入っているので、揺れの良い窓だけで余って見えることはない)。途切れが繰り返している間は戻さない (備えを保つ)
            if (tail_ended && !repeated_outages_locked(now)) catch_up_pending_ = std::max(catch_up_pending_, 2);
            // 帯域不足から追いついた窓も入れない (この窓の映像の余裕は、遅く届いていた間のもので小さい。入れると、
            // 追いついた後に戻す余りがその分だけ残り、その窓が包絡から抜けるまで約 2 分戻らなかった)
            const bool shortfall_window = shortfall_ || window_shortfall_;
            if (shortfall_ && audio_error >= 0) {
                // 追いついた: 帯域不足のときに増やした備えを戻す (余りは caught up で一度に戻す)
                shortfall_ = false;
                catch_up_pending_ = kCatchUpWindows + 1;
                audio_headroom_ = std::min(audio_headroom_, shortfall_headroom_);
                log_.push_back(
                    fmt::format("playout: audio caught up after arriving slower than real time; audio headroom back to {:.0f} ms",
                                qpc_to_us(audio_headroom_) / 1000.0));
            }
            // 最初の調整の窓 (!settled_) と、鳴らし始めてすぐ (kFastAdjustMs。大きなずれは一度に詰める間) の窓も入れない:
            // 接続直後のまとめ届きで、音声と映像の届き方が大きくずれ (RTMP で 234 ms)、それが最初の数窓に残る。入れると、
            // その窓が直近の窓の散らばりを広げて備えを跳ね上げ (10 → 119 ms)、包絡に残って、余っているのに 1 ms ずつ
            // 遅らせ続け、決め直しでも caught up でも余りを戻せなかった (約 2 分)。その間の大きなずれは fast で詰める
            const bool starting = !settled_ || now < fast_until_qpc_;
            push_history_locked(after_outage || shortfall_window || starting);
            ++windows_since_move_;
            update_band_locked();
            // 遅延の余りは、その窓だけでなく直近の窓すべてで見る (揺れの良い窓が続いただけで一度に戻さない。揺れが ±30 ms の
            // 回線では、良い窓のずれが 3 フレームを超え、戻した直後に遅れて届くのを繰り返した)。途切れのあとは、前の窓も
            // 遅らせた時間軸で計算し直すので、包絡も余っている
            const int64_t excess = std::min(error, history_envelope_locked(kLongHistoryWindows));
            // 途切れが繰り返している間は、余りを詰めない (備えは途切れを覆うためのもので、途切れの無い窓だけを見ると余って見える)
            if (!after_outage) excess_windows_ = excess > jump && !repeated_outages_locked(now) ? excess_windows_ + 1 : 0;
            const char* reason = "";
            const bool video_late = video_lateness > jump && video_error <= audio_error;
            if (after_outage) fast_prev_error_ = LLONG_MAX;  // 途切れの前の窓と向きを比べない
            const int64_t fast_step = after_outage ? 0 : fast_step_locked(error, now, video_late);
            if (!settled_) {
                // 時間軸を作った直後 (まだ鳴らしていない): 大きなずれは一度に合わせる
                if (std::abs(error) > jump) step = error;
                start_fast_locked(now);
                reason = "first";
            } else if (after_outage && !video_late) {
                step = 0;
                reason = "after outage";
            } else if (fast_step != 0) {
                // 鳴らし始めてすぐ / 出力遅延が変わってすぐ: はっきり大きいずれだけを一度に合わせる (fast_step_locked)
                step = fast_step;
                ++fast_moves_;
                reason = "fast";
                log_.push_back(fmt::format("playout: moved the delay {:+.1f} ms at once (soon after starting or an output delay change)",
                                           qpc_to_us(-step) / 1000.0));
            } else if (excess_windows_ >= kCatchUpWindows || (catch_up_pending_ > 0 && excess > band_)) {
                // 遅延の余りが続いている (短い途切れのあとのまとめ届きなど): 一度に戻す。帯域不足から追いついた直後は、
                // 3 フレームに届かない余りも一緒に戻す (追いつくときに一度合わせ直すので、そのあと何十秒も 1 ms ずつ詰めない)
                step = excess;
                excess_windows_ = 0;
                catch_up_pending_ = 0;
                reason = "catch up";
                log_.push_back(fmt::format("playout: delay {:.1f} ms more than needed; caught up", qpc_to_us(excess) / 1000.0));
            } else if (const int64_t cut = review_headroom_locked(now); cut > 0) {
                // 揺れの備えを、実際の揺れに合わせて一度に下げた (review_headroom_locked): 余った遅延も一度に詰める
                // (engine は 1 回合わせ直す)。映像側の余裕が足りなければ、その分は詰めない
                const int64_t envelope = history_envelope_locked(kLongHistoryWindows);
                step = envelope != LLONG_MAX ? std::max(envelope, int64_t(0)) : 0;  // 前からの余りも、この 1 回で一緒に戻す
                reason = "headroom cut";
                // 詰めなかったときは、どの窓が止めたかを出す (直近 kLongHistoryWindows 窓の包絡の最小)
                if (step == 0) debug_.push_back("playout: headroom cut moved nothing: " + envelope_source_locked(kLongHistoryWindows));
                log_.push_back(fmt::format(
                    "playout: headroom reduced to audio {:.0f} / video {:.0f} ms (no late audio for {} s); delay reduced {:.1f} ms at once",
                    qpc_to_us(audio_headroom_) / 1000.0, qpc_to_us(video_headroom_) / 1000.0, kHeadroomReviewMs / 1000,
                    qpc_to_us(step) / 1000.0));
            } else if (video_late) {
                // 映像が 3 フレームを超えて遅れた: 遅れた分だけ一度に遅らせ、備えを増やす (音声は合わせ直しになる)
                step = -video_lateness;
                reason = "video late";
                const bool outage = outage_locked(video_lateness, now);
                if (outage) video_outage_needs_.add(now, std::min(video_headroom_ + video_lateness, qpc_from_ms(kOutageHeadroomMaxMs)));
                if (outage && repeated_outages_locked(now)) {
                    grow_headroom_locked(video_headroom_, video_lateness, kOutageHeadroomMaxMs, now, last_video_late_qpc_,
                                         kOutageHeadroomMaxMs);
                    log_.push_back(
                        fmt::format("playout: video arrived {:.1f} ms late (repeated outages); delay increased, video headroom {:.0f} ms",
                                    qpc_to_us(video_lateness) / 1000.0, qpc_to_us(video_headroom_) / 1000.0));
                } else if (outage) {
                    log_.push_back(
                        fmt::format("playout: video arrived {:.1f} ms late (outage); delay increased, video headroom kept {:.0f} ms",
                                    qpc_to_us(video_lateness) / 1000.0, qpc_to_us(video_headroom_) / 1000.0));
                } else {
                    grow_headroom_locked(video_headroom_, video_lateness, kVideoHeadroomMaxMs, now, last_video_late_qpc_,
                                         kHeadroomGrowMaxMs);
                    log_.push_back(fmt::format("playout: video arrived {:.1f} ms late; delay increased, video headroom {:.0f} ms",
                                               qpc_to_us(video_lateness) / 1000.0, qpc_to_us(video_headroom_) / 1000.0));
                }
            } else {
                // 落ち着いた後: その窓のずれではなく、直近の窓のずれの最小 (揺れの悪い側の包絡) で決める。
                // 足りない側は、直近 kHistoryWindows 窓の悪い窓が幅の内側に収まるまで 1 ms ずつ遅らせる。余っている側は、
                // 直近 kLongHistoryWindows 窓がすべて幅を超えて余っているときだけ詰める。揺れの良い窓に釣られて詰め、
                // 次の悪い窓でまた遅らせる、の行き来をしない
                const int64_t short_envelope = history_envelope_locked(kHistoryWindows, !output_stopped_);
                const int64_t long_envelope = history_envelope_locked(kLongHistoryWindows);
                const int64_t band = band_;
                // 遅らせるのは直近 kHistoryWindows 窓で足りないときだけ。長い包絡は詰める側だけに使う (長い包絡の負の値で
                // 遅らせると、30 窓から抜けた古い窓 (接続直後の窓など) で、余っているのに 1 ms ずつ遅らせ続けた)
                const int64_t envelope = short_envelope < -band                                       ? short_envelope
                                         : repeated_outages_locked(now) || long_envelope == LLONG_MAX ? 0
                                                                                                      : std::max(long_envelope, int64_t(0));
                step = std::clamp(envelope, -limit, limit);
                if (envelope == LLONG_MAX || history_count_ < kStepWindows || std::llabs(envelope) <= band) {
                    step = 0;
                    reason = "deadband";
                } else if (windows_since_move_ < kStepWindows) {
                    step = 0;  // 前に動かしてから kStepWindows 窓は空ける
                    reason = "wait";
                } else {
                    // クロック差、揺れの備えを減らした分、悪い窓が幅を超えた分: 1 ms 動かす
                    reason = "step";
                    debug_.push_back(fmt::format("playout: step from the envelope {:+.1f} ms ({}), band {:.1f} ms",
                                                 qpc_to_us(envelope) / 1000.0,
                                                 envelope == short_envelope ? envelope_source_locked(kHistoryWindows, !output_stopped_)
                                                                            : envelope_source_locked(kLongHistoryWindows),
                                                 qpc_to_us(band) / 1000.0));
                }
            }
            if (std::abs(step) > qpc_from_ms(kEpochJumpMs) && settled_) ++jumps_;
            // 1 秒ごとの要約 (debug): 時間軸を動かした量と理由。engine の重複・間引きと並べて見る
            if (step != 0) {
                debug_.push_back(
                    fmt::format("playout: moved {:+.2f} ms ({}); error video {} / audio {} ms, headroom video {:.0f} / audio {:.0f} ms",
                                qpc_to_us(-step) / 1000.0, reason, fmt_ms(video_error), fmt_ms(audio_error),
                                qpc_to_us(video_headroom_) / 1000.0, qpc_to_us(audio_headroom_) / 1000.0));
            }
            if (step != 0) windows_since_move_ = 0;
            anchor_qpc_ -= step;
            settled_ = true;
        }
        window_start_qpc_ = now;
        min_video_slack_ = LLONG_MAX;
        min_audio_slack_ = LLONG_MAX;
        window_audio_reports_ = 0;
        window_audio_lates_ = 0;
        window_audio_late_sum_ = 0;
        window_outage_ = false;
        audio_gaps_.end_window();
        video_gaps_.end_window();
        window_shortfall_ = false;
        window_audio_headroom_ = audio_headroom_;
        if (catch_up_pending_ > 0) --catch_up_pending_;
    }

    // この窓の最小の余裕を、時間軸によらない形 (余裕 - anchor_qpc_) で覚える。時間軸を動かしても、遅れて届いて遅らせても、
    // 揺れの備えが変わっても、前の窓のずれを今の時間軸で計算し直せる (history_envelope_locked)
    void push_history_locked(bool outage) {
        auto& w = history_[history_next_];
        w.video = min_video_slack_ != LLONG_MAX && !outage ? min_video_slack_ - anchor_qpc_ : LLONG_MAX;
        w.audio = min_audio_slack_ != LLONG_MAX && !outage ? min_audio_slack_ - anchor_qpc_ : LLONG_MAX;
        history_next_ = (history_next_ + 1) % kLongHistoryWindows;
        history_count_ = std::min(history_count_ + 1, kLongHistoryWindows);
        if (w.audio != LLONG_MAX) {
            review_min_ = std::min(review_min_, w.audio);
            review_max_ = std::max(review_max_, w.audio);
            ++review_windows_;
        }
        if (w.video != LLONG_MAX) {
            review_video_min_ = std::min(review_video_min_, w.video);
            review_video_max_ = std::max(review_video_max_, w.video);
            ++review_video_windows_;
        }
    }

    // 途切れか (時間軸は遅らせるが、揺れの備えは増やさない)。kOutageLateMs を超える遅れは揺れではなく一時的な途切れで、
    // その直後 kOutageTailMs の間の遅れは、途切れのあとのまとめ届きの残り
    // 測った揺れ (直近の窓のずれの散らばり) より桁違いに大きい遅れも途切れ (揺れの小さい回線で 70 ms 止まったときなど。
    // 届く間隔は 90 ms 前後で kOutageGapMs に届かない)。揺れを測れるだけの窓が溜まるまでは見ない
    bool outage_locked(int64_t late, int64_t now) {
        const bool beyond_jitter = history_count_ >= kHistoryWindows && late > qpc_from_ms(kOutageMinLateMs) &&
                                   late > history_spread_locked() * kOutageSpreadTimes;
        if (late > qpc_from_ms(kOutageLateMs) || beyond_jitter) {
            note_outage_locked(now);
            return true;
        }
        return last_outage_qpc_ != 0 && now - last_outage_qpc_ < qpc_from_ms(kOutageTailMs);
    }

    // 途切れを覚える (届く間隔が開いた、大きく遅れた)。前の途切れから kOutageTailMs より後で kOutageMemoryMs 以内なら、
    // 途切れが繰り返している (repeated_outages_locked)。判断は受け取ったデータの届き方だけで、出力デバイスにはよらない
    void note_outage_locked(int64_t now) {
        window_outage_ = true;
        if (last_outage_qpc_ != 0 && now - last_outage_qpc_ < qpc_from_ms(kOutageTailMs)) return;  // 同じ途切れの続き
        if (last_outage_qpc_ != 0 && now - last_outage_qpc_ < qpc_from_ms(kOutageMemoryMs)) {
            if (!repeated_outages_locked(now)) log_.push_back("playout: outages are repeating; keeping enough headroom to cover them");
            repeated_until_qpc_ = now + qpc_from_ms(kOutageMemoryMs);
        }
        last_outage_qpc_ = now;
    }

    // 途切れが繰り返しているか (最後の途切れから kOutageMemoryMs の間)。この間は備えを途切れに合わせて増やし、減らさない
    bool repeated_outages_locked(int64_t now) const { return now < repeated_until_qpc_; }

    // 揺れの備えを決め直す間を、今から数え直す
    void review_start_locked(int64_t now) {
        review_start_qpc_ = now;
        review_min_ = LLONG_MAX;
        review_max_ = LLONG_MIN;
        review_windows_ = 0;
        review_video_min_ = LLONG_MAX;
        review_video_max_ = LLONG_MIN;
        review_video_windows_ = 0;
    }

    // 音声が kHeadroomReviewMs の間遅れて届かなければ、その間の窓ごとの最小の余裕の散らばりから、要る揺れの備えを
    // 決め直す (音声と映像それぞれ)。今の備えがそれより kHeadroomCutMs を超えて多ければ一度に下げ、下げた量の大きい方を
    // 返す (0 = 下げない)
    int64_t review_headroom_locked(int64_t now) {
        if (now - review_start_qpc_ < qpc_from_ms(kHeadroomReviewMs)) return 0;
        // 途切れが繰り返している間も決め直す。ただし、直近 kOutageNeedMemoryMs にあった途切れを覆う大きさより下げない
        // (繰り返している間は、その途切れに備えたい)。前は繰り返している間は決め直さなかったので、小さな途切れ
        // (RTMP の再送待ちなど) が 2 分以内ごとに続くと「繰り返している」が明けず、1 回きりの大きな途切れに合わせた備えを
        // 何時間も持ち続けた (6 時間の試験で、1 秒の途切れの後、遅延が約 1 秒のまま戻らなかった)
        const bool repeated = repeated_outages_locked(now);
        const int64_t audio_floor = repeated ? audio_outage_needs_.max_since(now - qpc_from_ms(kOutageNeedMemoryMs)) : 0;
        const int64_t video_floor = repeated ? video_outage_needs_.max_since(now - qpc_from_ms(kOutageNeedMemoryMs)) : 0;
        const int enough = int(kHeadroomReviewMs / 1000 / 2);
        const bool audio = review_windows_ >= enough;
        const bool video = review_video_windows_ >= enough;
        const int64_t audio_spread = review_max_ - review_min_;
        const int64_t video_spread = review_video_max_ - review_video_min_;
        review_start_locked(now);
        // 散らばりに、その半分 (少なくとも kHeadroomMarginMs) を上乗せする。ちょうどにすると、次の揺れで遅れて届いて
        // また増やす、を繰り返す (kHeadroomDecayMs のコメント)
        auto needed = [](int64_t spread, int64_t floor) {
            return std::max(spread + std::max(spread / 2, qpc_from_ms(kHeadroomMarginMs)), floor);
        };
        int64_t cut = 0;
        if (audio) {
            const int64_t n = needed(audio_spread, std::max(qpc_from_ms(kAudioHeadroomMinMs), audio_floor));
            reviewed_headroom_ = n;  // 少しずつ減らすのも、ここまで (decay_headroom_locked)
            if (audio_headroom_ - n > qpc_from_ms(kHeadroomCutMs)) {
                cut = audio_headroom_ - n;
                audio_headroom_ = n;
            }
        }
        if (video) {
            // 映像の備えは、繰り返す途切れのときだけ大きくなる。途切れが止んだら (大きな途切れが kOutageMemoryMs なければ)、
            // 映像の揺れ (と直近の途切れ) に見合う大きさに戻す
            const int64_t n = needed(video_spread, video_floor);
            if (video_headroom_ - n > qpc_from_ms(kHeadroomCutMs)) {
                cut = std::max(cut, video_headroom_ - n);
                video_headroom_ = n;
            }
        }
        return cut;  // 小さい余りは今どおり少しずつ減らす (decay_headroom_locked)
    }

    // 動かさない幅 (band_) を、窓ごとに測ったずれの散らばりから決める。散らばりの半分にし (揺れの大きい回線ほど広い)、
    // 広がったらすぐ広げ、狭まったら 1 窓に kBandShrinkUs ずつ狭める (散らばりは、どの窓が直近に入るかで数 ms 上下する。
    // そのたびに幅が変わると、同じ揺れでも動いたり動かなかったりする)。
    // 下限 kDeadbandMs: 時間軸は 1 回に 1 ms 動き、engine は 2 ms のずれから 1 サンプルずつ追う。それより狭い幅は、
    // 揺れの無い回線でも 1 ms 動かすたびに戻す向きの判定になりうる。
    // 上限は音声の揺れの備えの半分: 幅が備えより広いと、足りない側で動く前に遅れて届き、遅れて届いた処理で
    // 備えを増やし続ける (送出元の時計が遅いとき)
    void update_band_locked() {
        const int64_t target =
            std::clamp(history_spread_locked() / 2, qpc_from_ms(kDeadbandMs), std::max(qpc_from_ms(kDeadbandMs), audio_headroom_ / 2));
        band_ = target >= band_ ? target : std::max(target, band_ - qpc_from_us(kBandShrinkUs));
    }

    // 直近の窓の、窓ごとの目標とのずれの散らばり (最大 - 最小)。時間軸と揺れの備えの動きは差し引いてある
    int64_t history_spread_locked() const {
        int64_t lo = LLONG_MAX, hi = LLONG_MIN;
        for (int i = 0; i < std::min(history_count_, kHistoryWindows); ++i) {
            const auto& w = history_[(history_next_ + kLongHistoryWindows - 1 - i) % kLongHistoryWindows];
            int64_t e = LLONG_MAX;
            if (w.video != LLONG_MAX) e = std::min(e, w.video + anchor_qpc_ - buffer_ - video_headroom_);
            if (w.audio != LLONG_MAX) e = std::min(e, w.audio + anchor_qpc_ - audio_headroom_);
            if (e == LLONG_MAX) continue;
            lo = std::min(lo, e);
            hi = std::max(hi, e);
        }
        return lo == LLONG_MAX ? 0 : hi - lo;
    }

    // 直近 windows 窓の、今の時間軸と揺れの備えでの目標とのずれの最小 (LLONG_MAX = 報告なし)
    // 包絡の最小になった窓 (何窓前か、映像 / 音声のどちらか、その値)。動かした理由を debug ログで追えるように
    std::string envelope_source_locked(int windows, bool audio = true) const {
        int64_t envelope = LLONG_MAX;
        int age = -1;
        const char* side = "";
        for (int i = 0; i < std::min(history_count_, windows); ++i) {
            const auto& w = history_[(history_next_ + kLongHistoryWindows - 1 - i) % kLongHistoryWindows];
            if (w.video != LLONG_MAX && w.video + anchor_qpc_ - buffer_ - video_headroom_ < envelope) {
                envelope = w.video + anchor_qpc_ - buffer_ - video_headroom_;
                age = i + 1, side = "video";
            }
            if (audio && w.audio != LLONG_MAX && w.audio + anchor_qpc_ - audio_headroom_ < envelope) {
                envelope = w.audio + anchor_qpc_ - audio_headroom_;
                age = i + 1, side = "audio";
            }
        }
        return age < 0 ? std::string("no windows") : fmt::format("{} {} windows ago {}", side, age, fmt_ms(envelope));
    }

    // audio = false: 映像の窓だけで見る (engine の出力が鳴っていない間の、足りない側。note_output_stopped)
    int64_t history_envelope_locked(int windows, bool audio = true) const {
        int64_t envelope = LLONG_MAX;
        for (int i = 0; i < std::min(history_count_, windows); ++i) {
            const auto& w = history_[(history_next_ + kLongHistoryWindows - 1 - i) % kLongHistoryWindows];
            if (w.video != LLONG_MAX) envelope = std::min(envelope, w.video + anchor_qpc_ - buffer_ - video_headroom_);
            if (audio && w.audio != LLONG_MAX) envelope = std::min(envelope, w.audio + anchor_qpc_ - audio_headroom_);
        }
        return envelope;
    }

    void start_fast_locked(int64_t now) {
        fast_until_qpc_ = now + qpc_from_ms(kFastAdjustMs);
        fast_moves_ = 0;
        fast_prev_error_ = LLONG_MAX;
    }

    // 鳴らし始めてすぐ / 出力遅延が変わってすぐの間に、一度に動かす量 (0 = 動かさない)。窓ごとに呼ぶ
    int64_t fast_step_locked(int64_t error, int64_t now, bool video_late) {
        const int64_t prev = fast_prev_error_;
        fast_prev_error_ = error;
        if (!settled_ || now >= fast_until_qpc_ || fast_moves_ >= kFastMaxMoves) return 0;
        if (video_late) return 0;                                  // 映像の遅れは、備えを増やして動かす (adjust_if_due_locked)
        if (error < -qpc_from_ms(kFastImmediateMs)) return error;  // 大きく足りない: 待たない
        const int64_t limit = qpc_from_ms(kFastJumpMs);
        if (prev == LLONG_MAX || std::abs(error) <= limit || std::abs(prev) <= limit) return 0;
        if ((error > 0) != (prev > 0)) return 0;                           // 向きが揃っていない: 揺れ
        return error > 0 ? std::min(error, prev) : std::max(error, prev);  // 2 窓の小さい方だけ
    }

    // 届く速さが足りない (帯域不足): 遅れた分ずつ遅らせると、すぐにまた足りなくなって音切れを繰り返す (0.9 倍の速さが
    // 10 秒続いて 17 回)。一度に kShortfallShiftMs 遅らせ、備えも同じだけ増やす。追いついたら備えを戻し、余りを一度に戻す
    // (adjust_if_due_locked)
    void start_shortfall_locked(int64_t now) {
        shortfall_headroom_ = window_audio_headroom_;  // この窓で遅れて届くたびに増やした分も、追いついたら戻す
        const int64_t before = audio_headroom_;
        audio_headroom_ = std::min(audio_headroom_ + qpc_from_ms(kShortfallShiftMs), std::max(before, qpc_from_ms(kOutageHeadroomMaxMs)));
        const int64_t shift = audio_headroom_ - before;
        anchor_qpc_ += shift;
        if (min_audio_slack_ != LLONG_MAX) min_audio_slack_ += shift;
        if (min_video_slack_ != LLONG_MAX) min_video_slack_ += shift;
        if (shift > qpc_from_ms(kEpochJumpMs)) ++jumps_;
        last_audio_late_qpc_ = now;
        shortfall_ = true;
        window_shortfall_ = true;
        log_.push_back(fmt::format("playout: audio arrives slower than real time ({} of {} chunks late); delay increased {:.0f} ms at once",
                                   window_audio_lates_, window_audio_reports_, qpc_to_us(shift) / 1000.0));
        debug_.push_back(fmt::format("playout: moved {:+.2f} ms (shortfall)", qpc_to_us(shift) / 1000.0));
    }

    // 揺れの備えを、遅れた分 (1 回に step_max_ms まで、合わせて max_ms まで) 増やす
    static void grow_headroom_locked(int64_t& headroom, int64_t late, int64_t max_ms, int64_t now, int64_t& last_late,
                                     int64_t step_max_ms) {
        headroom = std::min(headroom + std::min(late, qpc_from_ms(step_max_ms)), qpc_from_ms(max_ms));
        last_late = now;
    }

    // 遅れがしばらくなければ、揺れの備えを少しずつ減らす (遅延はそれに合わせて 1 秒に 1 ms ずつ詰まる)
    static void decay_headroom_locked(int64_t& headroom, int64_t floor, int64_t now, int64_t& last_late) {
        if (headroom <= floor) return;
        if (now - last_late < qpc_from_ms(kHeadroomDecayMs)) return;
        headroom = std::max(headroom - qpc_from_ms(1), floor);
        last_late = now;  // 次に減らすのはまた kHeadroomDecayMs 後
    }

    static constexpr int64_t kSettleMs = 500;
    static constexpr int64_t kFastAdjustMs = 10'000;  // 鳴らし始めてから / 出力遅延が変わってから、一度に詰める間
    static constexpr int64_t kFastJumpMs = 8;         // その間、同じ向きにこれを超えるずれが 2 窓続いたら一度に詰める
    static constexpr int64_t kFastImmediateMs = 20;   // その間、遅延がこれを超えて足りなければすぐに詰める
    static constexpr int kFastMaxMoves = 2;           // その間に一度に詰める回数の上限
    // 一度にこれを超えて動かしたら (遅れて届いて遅らせたときも) 音声の epoch を進める。engine は平滑化したずれが
    // 5 ms を超えると合わせ直すので、それより小さい動きは重複・間引きで追わせる
    static constexpr int64_t kEpochJumpMs = 5;
    static constexpr int64_t kBurstMs = 5;    // 遅れて届いたかたまりからこの間に届いたものは、まとめ届きの残り
    static constexpr int64_t kMaxStepMs = 1;  // 鳴り始めてから 1 回 (1 秒) に動かす上限
    // 動かさない幅の下限 (幅そのものは測った揺れから決める。update_band_locked)。時間軸は 1 回に 1 ms 動き、engine は 2 ms の
    // ずれから 1 サンプルずつ追うので、それより十分広くしておかないと、揺れの無い回線でも動かした分を戻す向きの判定になりうる。
    // 2 ms にしていたときは、揺れのたびに 1 ms ずつ行き来した
    static constexpr int64_t kDeadbandMs = 5;
    // 1 ms ずつ動かすのは、前に動かしてからこの窓数空けてから (直近の窓もこの数は要る)。追える速さは 2 秒に 1 ms (500 ppm) で、
    // 設計で追うクロック差 200 ppm (1 秒に 0.2 ms) を超える。1 ms は engine が 1 サンプルずつ (10 ms に 1 回) 約 0.5 秒で追うので、
    // 2 窓おきなら同じ向きの重複・間引きが 1 秒に 24 回を超える秒は続かない (続けて毎秒動かすと 1 秒に 48 回が続き、2 秒で
    // 合わせ直しになる。ChannelSource の kSlipRateResync)
    static constexpr int kStepWindows = 2;
    // 落ち着いた後の 1 ms ずつの調整は、直近の窓のずれの最小で決める (揺れの悪い側の包絡)。足りない側 (遅らせる) と
    // 動かさない幅の散らばりは直近 kHistoryWindows 窓、余っている側 (詰める、caught up、決め直しで詰める) は直近
    // kLongHistoryWindows 窓で見る。短いと、揺れの悪い窓が抜けたときに余っていると見て詰め、次の悪い窓でまた遅らせる
    // (10 窓では揺れ ±30 ms の回線で 5 分に 9 回行き来し、30 窓では裾の重い揺れで、たまの深い遅れを忘れて一度に詰めていた)。
    // 余っている側を長くすると後追いが遅れる (200 ppm 速い送出元で 2 分に 24 ms) が、遅延が少し大きいだけ。
    // 足りない側は、悪い窓が入った時点で包絡が下がるので、短くても長くても遅れない
    static constexpr int kHistoryWindows = 30;
    static constexpr int kLongHistoryWindows = 120;
    static constexpr int64_t kBandShrinkUs = 100;  // 動かさない幅を狭める速さ (1 窓あたり。update_band_locked)
    // 遅延の余り (3 フレーム超) がこの回数続いたら一度に戻す
    static constexpr int kCatchUpWindows = 2;
    static constexpr int64_t kAudioHeadroomMinMs = 10;  // 音声の揺れの備え (最初の値と下限)
    static constexpr int64_t kAudioHeadroomMaxMs = 200;
    static constexpr int64_t kVideoHeadroomMaxMs = 200;
    static constexpr int64_t kHeadroomGrowMaxMs = 50;  // 1 回の遅れで増やす上限 (一時的な大きな途切れで増やしすぎない)
    // 遅れがこの間なければ備えを 1 ms 減らす。速く減らすと、減っては次の小さな遅れで増える、を繰り返して時間軸が
    // 動き続け、engine がそれを追うサンプルの重複・間引きも増える (5 秒では 1 秒に十数回)
    static constexpr int64_t kHeadroomDecayMs = 30'000;
    // 遅れがこれを超えたら途切れとみなし、揺れの備えを増やさない。回線の揺れは数十 ms まで (実測 ±2〜20 ms)。
    // 備えの 1 回の増分の上限 (kHeadroomGrowMaxMs) の 2 倍を超える遅れ (実機で 868 / 853 ms) は、揺れではなく一時的な途切れ。
    // 増やすと、30 秒に 1 ms ずつしか減らないので、約 1 時間、遅延が 100 ms 以上多いままになった
    static constexpr int64_t kOutageLateMs = 100;
    // 途切れからこの間の遅れも途切れの一部 (途切れの直後のまとめ届きで、24 / 42 ms などの遅れが続いた)
    static constexpr int64_t kOutageTailMs = 3'000;
    // 遅れがこれを超え、かつ測った揺れ (直近の窓のずれの散らばり) のこの倍を超えたら、kOutageLateMs に届かなくても途切れ
    static constexpr int64_t kOutageMinLateMs = 30;
    static constexpr int64_t kOutageSpreadTimes = 4;
    // 音声がこの間遅れて届かなければ、揺れの備えを実際の揺れに合わせて決め直す。約 1 分ごとに起きる小さな途切れ (RTMP) で、
    // 下げては遅れて届いて増やす、を毎回繰り返さない長さ
    static constexpr int64_t kHeadroomReviewMs = 60'000;
    static constexpr int64_t kHeadroomMarginMs = 5;  // 決め直す備えの、散らばりへの上乗せの下限
    // 決め直した備えが今よりこれを超えて少なければ、一度に下げて時間軸も一度に詰める (それ以下は decay で少しずつ)
    static constexpr int64_t kHeadroomCutMs = 20;
    // 届く間隔がこれを超えて開いたら途切れ。音声のかたまり (20 ms 前後) と映像のフレーム (17〜40 ms) の数個分で、
    // 回線の揺れで開く間隔より十分長い。ふだんからこれより開いて届く送り側では、ふだんの間隔の kOutageGapTimes 倍 (GapTracker)
    static constexpr int64_t kOutageGapMs = 100;
    static constexpr int64_t kOutageGapTimes = 3;
    static constexpr int kGapWindows = 10;    // ふだんの間隔を見る窓の数 (GapTracker)
    static constexpr int kGapMinWindows = 3;  // ふだんの間隔が分かるまでの窓の数。それまでは届く間隔では途切れと見ない
    // 途切れから、この間に次の途切れがあれば「繰り返している」(1 回きりなら備えを増やさず、繰り返すなら増やして保つ)。
    // 最後の途切れからこの間が過ぎたら、備えを決め直して一度に詰められる (途切れが止んでから 3 分以内に戻すため、
    // 決め直しの kHeadroomReviewMs と合わせて 3 分に収まる長さ)
    static constexpr int64_t kOutageMemoryMs = 120'000;
    // 途切れを覆うのに要った備えを覚えておく数 (OutageNeeds)。kOutageMemoryMs の間に起きうる途切れの数より多く
    // (途切れの続きは kOutageTailMs の間まとめて 1 回なので、2 分に 40 回を超えることはない)
    static constexpr int kOutageNeeds = 64;
    // 繰り返している間の決め直しで、途切れを覆うのに要った備えを見る長さ。映像が遅れる途切れは、音声ほど毎回は起きない
    // (300〜800 ms の途切れが 30 秒ごとでも、映像が遅れたのは 2 分以上空いて 2 回)。kOutageMemoryMs だけでは、繰り返している
    // 最中に映像の備えを下げ、次の途切れでまた増やして、一度に動かすのが増えた。止んだ後 (繰り返しが明けた後) の決め直しは
    // これを見ないので、戻る速さは変わらない
    static constexpr int64_t kOutageNeedMemoryMs = 2 * kOutageMemoryMs;
    // 繰り返す途切れに合わせて増やす備えの上限。数百 ms の途切れ (RTMP の再送待ち) を覆える大きさ
    static constexpr int64_t kOutageHeadroomMaxMs = 1'000;
    // 届く速さが足りないとみなす: 1 窓で、届いたかたまりの kShortfallLatePercent % 以上 (かつ kShortfallMinLates 個以上) が
    // 遅れて届いた。揺れや途切れでは、遅れて届くのは揺れが変わったとき・途切れが明けたときの 1 個目だけで、多くても数個
    static constexpr int kShortfallMinLates = 5;
    static constexpr int64_t kShortfallMinLateUs = 500;  // これより小さい遅れは数えない (余裕 0 の所でわずかに遅れただけ)
    static constexpr int kShortfallLatePercent = 20;
    // かつ、遅れの合計が 1 窓でこれ以上 (届く速さが 2% 以上足りない)。時計の遅い送出元 (追うのは 200 ppm = 1 秒に 0.2 ms まで)
    // では、余裕が 0 の所で毎回わずかに遅れて届くが、この量には届かない
    static constexpr int64_t kShortfallLateSumMs = 20;
    // そのとき一度に遅らせる量 (繰り返す途切れに合わせる備えの上限と同じ)
    static constexpr int64_t kShortfallShiftMs = 1'000;

public:
    // 時間軸が再生バッファより遅らされうる量の上限: 映像の揺れの備え + 音声側から遅らされる量 (音声の揺れの備え。繰り返す
    // 途切れでは kOutageHeadroomMaxMs まで、帯域不足では一度に kShortfallShiftMs)。映像の待ち行列は、これを覆う幅にする
    // (playout_core.h の video_queue_limits)。1 回きりの途切れでは、途切れた長さだけ一時的に遅らされ、約 5 秒で戻る
    static constexpr int64_t kMaxExtraDelayMs =
        kVideoHeadroomMaxMs + std::max({kAudioHeadroomMaxMs, kOutageHeadroomMaxMs, kShortfallShiftMs});
    static_assert(kMaxExtraDelayMs >= kVideoHeadroomMaxMs + kOutageHeadroomMaxMs &&
                      kMaxExtraDelayMs >= kVideoHeadroomMaxMs + kShortfallShiftMs,
                  "the video queue must cover every delay the clock can add");

private:
    const int64_t buffer_;
    const NowFn now_;
    std::mutex mutex_;
    bool anchored_ = false;
    bool settled_ = false;
    uint64_t session_ = 0;
    int64_t anchor_pts_us_ = 0;
    int64_t anchor_qpc_ = 0;
    uint64_t generation_ = 0;

    int64_t window_start_qpc_ = 0;
    int64_t min_video_slack_ = LLONG_MAX;
    int64_t min_audio_slack_ = LLONG_MAX;
    int64_t frame_us_ = 16'667;
    int excess_windows_ = 0;  // 遅延の余りが 3 フレームを超えた窓が続いた数
    struct WindowSlack {
        int64_t video = LLONG_MAX;  // 窓の最小の余裕 - anchor_qpc_ (push_history_locked)
        int64_t audio = LLONG_MAX;
    };
    WindowSlack history_[kLongHistoryWindows];  // 直近の窓 (環状)
    int history_next_ = 0;
    int history_count_ = 0;
    int64_t last_outage_qpc_ = 0;         // 最後に途切れた時刻 (note_outage_locked)
    int64_t repeated_until_qpc_ = 0;      // この時刻まで、途切れが繰り返しているとみなす
    int64_t reviewed_headroom_ = 0;       // 最後に決め直した音声の揺れの備え (少しずつ減らす下限)
    int64_t last_video_arrival_qpc_ = 0;  // 最後に映像のフレームが届いた時刻 (note_video_arrival)

    // 届く間隔で途切れを見る (音声・映像それぞれ)。途切れとみなすのは、kOutageGapMs を超え、かつ、ふだんの間隔の
    // kOutageGapTimes 倍を超えて開いたとき。ふだんの間隔は、直近 kGapWindows 窓それぞれの最大の間隔のうち最小
    // (途切れのあった窓が少しあっても引きずられない)。送り側の mpegts が AAC を何フレームかずつ 1 つの PES にまとめると
    // (ffmpeg の既定で約 149 ms、128 kbps ステレオで約 180 ms)、音声は毎回その間隔で届く。固定の 100 ms で見ていたときは、
    // 届くたびに途切れと数え、「繰り返している」から抜けられず、本当の途切れの後に余りを戻せなかった (burst_test.sh)。
    // 判断は届き方の実測だけで、送り側の種類にはよらない
    struct GapTracker {
        int64_t window_max = 0;
        int64_t maxima[kGapWindows] = {};
        int next = 0;
        int count = 0;

        // gap がふだんより大きく開いた (途切れ) か。ふだんの間隔が分かるまでは見ない
        bool outage(int64_t gap) {
            window_max = std::max(window_max, gap);
            if (count < kGapMinWindows) return false;
            int64_t usual = LLONG_MAX;
            for (int i = 0; i < count; ++i) usual = std::min(usual, maxima[i]);
            return gap > std::max(qpc_from_ms(kOutageGapMs), usual * kOutageGapTimes);
        }
        void end_window() {
            if (window_max == 0) return;  // 届かなかった窓は数えない
            maxima[next] = window_max;
            next = (next + 1) % kGapWindows;
            count = std::min(count + 1, kGapWindows);
            window_max = 0;
        }
    };
    GapTracker audio_gaps_;
    GapTracker video_gaps_;

    // 途切れを覆うのに要った備え (その時の備え + 遅れ) を、時刻と一緒に覚える (音声・映像それぞれ、直近 kOutageNeeds 回)。
    // 繰り返している間の決め直しで、直近 kOutageNeedMemoryMs のものの最大より下げない (review_headroom_locked)
    struct OutageNeeds {
        int64_t at[kOutageNeeds] = {};
        int64_t need[kOutageNeeds] = {};
        int next = 0;

        void add(int64_t now, int64_t n) {
            at[next] = now;
            need[next] = n;
            next = (next + 1) % kOutageNeeds;
        }
        int64_t max_since(int64_t since) const {
            int64_t m = 0;
            for (int i = 0; i < kOutageNeeds; ++i) {
                if (at[i] != 0 && at[i] >= since) m = std::max(m, need[i]);
            }
            return m;
        }
        void clear() { *this = OutageNeeds{}; }
    };
    OutageNeeds audio_outage_needs_;
    OutageNeeds video_outage_needs_;
    int64_t review_start_qpc_ = 0;    // 揺れの備えを決め直す間の始まり (review_headroom_locked)
    int64_t review_min_ = LLONG_MAX;  // その間の、窓の最小の音声の余裕 - anchor_qpc_ の最小と最大
    int64_t review_max_ = LLONG_MIN;
    int review_windows_ = 0;
    int64_t review_video_min_ = LLONG_MAX;  // 同じく映像
    int64_t review_video_max_ = LLONG_MIN;
    int review_video_windows_ = 0;
    int window_audio_reports_ = 0;  // この窓で届いた音声のかたまりと、そのうち遅れて届いたもの
    int window_audio_lates_ = 0;
    int64_t window_audio_late_sum_ = 0;  // そのうち遅れた量の合計
    bool window_outage_ = false;         // この窓で途切れた (push_history_locked)
    bool shortfall_ = false;             // 帯域不足で遅らせてから、追いつくまで
    bool output_stopped_ = false;        // engine の出力が鳴っていない (note_output_stopped から restart_after_output_gap まで)
    bool window_shortfall_ = false;      // この窓で帯域不足とみなした
    int64_t shortfall_headroom_ = 0;     // 帯域不足で増やす前の音声の揺れの備え
    int catch_up_pending_ = 0;           // 帯域不足から追いついた: この窓数のうちに余りを一度に戻す
    int64_t window_audio_headroom_ = qpc_from_ms(kAudioHeadroomMinMs);  // この窓の始めの音声の揺れの備え
    int64_t band_ = qpc_from_ms(kDeadbandMs);                           // 動かさない幅 (update_band_locked)
    int windows_since_move_ = kStepWindows;                             // 前に時間軸を 1 ms ずつの調整などで動かしてからの窓の数
    std::vector<std::string> log_;                                      // ロックを外してから書くログ (flush_log)
    std::vector<std::string> debug_;                                    // 同じく debug のログ
    int64_t fast_until_qpc_ = 0;                                        // この時刻までは、大きなずれを一度に詰める
    int fast_moves_ = 0;                                                // その間に一度に詰めた回数
    int64_t fast_prev_error_ = LLONG_MAX;                               // 前の窓の目標とのずれ (向きが続いているかを見る)
    int64_t last_audio_late_report_qpc_ = 0;                            // 最後に音声が遅れて届いた時刻 (まとめ届きの残りを窓に入れない)
    uint64_t jumps_ = 0;                                                // 一度に kEpochJumpMs を超えて動かした回数
    int64_t last_audio_report_qpc_ = 0;
    int64_t last_audio_arrival_qpc_ = 0;  // 最後に音声が届いた時刻 (report_audio、note_audio_without_output)

    // 揺れの備え (接続をまたいで覚えておく: 同じ送出元の揺れ方は変わらないことが多い)
    int64_t audio_headroom_ = qpc_from_ms(kAudioHeadroomMinMs);
    int64_t video_headroom_ = 0;
    int64_t last_audio_late_qpc_ = 0;
    int64_t last_video_late_qpc_ = 0;
};

}  // namespace denpachan::worker
