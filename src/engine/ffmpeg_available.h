#pragma once
// FFmpeg の DLL を使えるか (engine.cpp が確かめる)。ChannelSource はリサンプルの前にこれを見る。
// engine.h から分けてあるのは、ChannelSource を engine の外 (音声の時間軸の試験) でも組めるようにするため

namespace denpachan::engine {

bool ffmpeg_available();

}  // namespace denpachan::engine
