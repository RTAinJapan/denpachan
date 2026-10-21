#include "scaler.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace denpachan::worker {

namespace {

constexpr uint8_t kBlackY = 16;
constexpr uint8_t kNeutralUV = 128;

int even(double v) { return std::max(2, int(std::lround(v / 2.0)) * 2); }

}  // namespace

Scaler::~Scaler() { sws_freeContext(ctx_); }

bool Scaler::scale(const AVFrame* src, uint8_t* dst, int dw, int dh) {
    if (!src || src->width <= 0 || src->height <= 0) return false;

    // 表示アスペクト比 (非正方画素を考慮) を保って dw x dh に収める
    double sar = 1.0;
    if (src->sample_aspect_ratio.num > 0 && src->sample_aspect_ratio.den > 0) sar = av_q2d(src->sample_aspect_ratio);
    const double dar = src->width * sar / src->height;
    int rw = dw;
    int rh = dh;
    if (double(dw) / dh > dar) {
        rw = std::min(dw, even(dh * dar));
    } else {
        rh = std::min(dh, even(dw / dar));
    }
    const int x0 = ((dw - rw) / 2) & ~1;
    const int y0 = ((dh - rh) / 2) & ~1;

    ctx_ = sws_getCachedContext(ctx_, src->width, src->height, AVPixelFormat(src->format), rw, rh, AV_PIX_FMT_NV12, SWS_BILINEAR, nullptr,
                                nullptr, nullptr);
    if (!ctx_) return false;

    uint8_t* y_plane = dst;
    uint8_t* uv_plane = dst + size_t(dw) * dh;
    uint8_t* planes[4] = {y_plane + size_t(y0) * dw + x0, uv_plane + size_t(y0 / 2) * dw + x0, nullptr, nullptr};
    const int strides[4] = {dw, dw, 0, 0};
    if (sws_scale(ctx_, src->data, src->linesize, 0, src->height, planes, strides) <= 0) return false;

    // 黒帯 (上下 / 左右)
    if (rw != dw || rh != dh) {
        for (int y = 0; y < dh; ++y) {
            uint8_t* row = y_plane + size_t(y) * dw;
            if (y < y0 || y >= y0 + rh) {
                memset(row, kBlackY, dw);
            } else {
                memset(row, kBlackY, x0);
                memset(row + x0 + rw, kBlackY, dw - x0 - rw);
            }
        }
        for (int y = 0; y < dh / 2; ++y) {
            uint8_t* row = uv_plane + size_t(y) * dw;
            if (y < y0 / 2 || y >= (y0 + rh) / 2) {
                memset(row, kNeutralUV, dw);
            } else {
                memset(row, kNeutralUV, x0);
                memset(row + x0 + rw, kNeutralUV, dw - x0 - rw);
            }
        }
    }
    return true;
}

}  // namespace denpachan::worker
