#pragma once
// デコード済みフレームを、vcam が要求する解像度の NV12 に変換する (アスペクト比維持、黒帯)。
// 今後: D3D11 Video Processor で GPU 上に移す (設計書 §13)

#include <cstdint>

#include "ffmpeg_util.h"

namespace denpachan::worker {

class Scaler {
public:
    Scaler() = default;
    ~Scaler();
    Scaler(const Scaler&) = delete;
    Scaler& operator=(const Scaler&) = delete;

    // dst: dw x dh の NV12 (Y の直後に UV、stride = dw)
    bool scale(const AVFrame* src, uint8_t* dst, int dw, int dh);

private:
    SwsContext* ctx_ = nullptr;
};

}  // namespace denpachan::worker
