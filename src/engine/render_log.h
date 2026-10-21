#pragma once
// 音声の描画スレッド (WASAPI の描画ループ、ASIO ドライバのコールバック) から書くログ。
//
// 普段のロガーは同期で、ファイルに書き終わるまで (警告ならディスクへのフラッシュまで) 呼んだスレッドを待たせる。
// 描画スレッドが待たされると音が途切れるので、ここからは非同期のロガーに渡す (別のスレッドが書く)。
// 溜まりすぎたら (1024 行) 古いものから捨て、描画スレッドは決して待たない。捨てたことはログに残らないので、
// ASIO ドライバが異常で 1 周期に何行も出すようなときは、行が抜けていることがある。
// 普段のロガーを非同期にしないのは、engine が落ちたときに直前のログ (警告) を残すため。

#include <spdlog/spdlog.h>

namespace denpachan::engine {

// 普段のロガー (spdlog の既定) を作った後に呼ぶ。同じ出力先 (画面とファイル) に書く
void setup_render_log();
// setup_render_log の前は普段のロガーを返す
spdlog::logger& render_log();

}  // namespace denpachan::engine
