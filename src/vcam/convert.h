#pragma once
// 共有メモリの NV12 フレームを、DirectShow の出力バッファ (NV12 / YUY2) へ変換する。
// 通常は書き手が要求サイズで書いてくるので同サイズ NV12 のコピーで済む。
// サイズが違うのは、複数アプリが同じ Cam を別解像度で開いたときだけ (最近傍で簡易拡縮)。

#include <cstdint>
#include <vector>

#include "formats.h"

namespace denpachan::vcam {

void convert_nv12(const uint8_t* src, uint32_t src_w, uint32_t src_h, uint8_t* dst, const OutputFormat& out);

// 「NO SIGNAL」スレート (NV12) を作る
std::vector<uint8_t> make_slate_nv12(uint32_t width, uint32_t height, int cam);

}  // namespace denpachan::vcam
