#include "convert.h"

#include <cstring>
#include <string_view>

namespace denpachan::vcam {

namespace {

// dst の各列 / 各行が参照する src の座標 (最近傍)
std::vector<uint32_t> nearest_map(uint32_t dst_len, uint32_t src_len) {
    std::vector<uint32_t> m(dst_len);
    for (uint32_t i = 0; i < dst_len; ++i) m[i] = uint32_t(uint64_t(i) * src_len / dst_len);
    return m;
}

void nv12_to_nv12_scaled(const uint8_t* src, uint32_t sw, uint32_t sh, uint8_t* dst, uint32_t dw, uint32_t dh) {
    const uint8_t* src_uv = src + size_t(sw) * sh;
    uint8_t* dst_uv = dst + size_t(dw) * dh;
    const auto xm = nearest_map(dw, sw);
    const auto ym = nearest_map(dh, sh);
    for (uint32_t y = 0; y < dh; ++y) {
        const uint8_t* s = src + size_t(ym[y]) * sw;
        uint8_t* d = dst + size_t(y) * dw;
        for (uint32_t x = 0; x < dw; ++x) d[x] = s[xm[x]];
    }
    const auto cxm = nearest_map(dw / 2, sw / 2);
    const auto cym = nearest_map(dh / 2, sh / 2);
    for (uint32_t y = 0; y < dh / 2; ++y) {
        const uint8_t* s = src_uv + size_t(cym[y]) * sw;
        uint8_t* d = dst_uv + size_t(y) * dw;
        for (uint32_t x = 0; x < dw / 2; ++x) {
            d[2 * x] = s[2 * cxm[x]];
            d[2 * x + 1] = s[2 * cxm[x] + 1];
        }
    }
}

void nv12_to_yuy2(const uint8_t* src, uint32_t sw, uint32_t sh, uint8_t* dst, uint32_t dw, uint32_t dh) {
    const uint8_t* src_uv = src + size_t(sw) * sh;
    const bool same = sw == dw && sh == dh;
    const auto xm = same ? std::vector<uint32_t>{} : nearest_map(dw, sw);
    const auto ym = same ? std::vector<uint32_t>{} : nearest_map(dh, sh);
    for (uint32_t y = 0; y < dh; ++y) {
        const uint32_t sy = same ? y : ym[y];
        const uint8_t* sY = src + size_t(sy) * sw;
        const uint8_t* sUV = src_uv + size_t(sy / 2) * sw;
        uint8_t* d = dst + size_t(y) * dw * 2;
        for (uint32_t x = 0; x < dw; x += 2) {
            const uint32_t sx0 = same ? x : xm[x];
            const uint32_t sx1 = same ? x + 1 : xm[x + 1];
            const uint32_t cx = (sx0 / 2) * 2;
            d[2 * x + 0] = sY[sx0];
            d[2 * x + 1] = sUV[cx];  // U
            d[2 * x + 2] = sY[sx1];
            d[2 * x + 3] = sUV[cx + 1];  // V
        }
    }
}

// 5x7 ビットマップフォント (スレート表示に必要な文字だけ)
struct Glyph {
    char c;
    uint8_t rows[7];
};
constexpr Glyph kFont[] = {
    {'A', {0b01110, 0b10001, 0b10001, 0b11111, 0b10001, 0b10001, 0b10001}},
    {'C', {0b01110, 0b10001, 0b10000, 0b10000, 0b10000, 0b10001, 0b01110}},
    {'D', {0b11110, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b11110}},
    {'E', {0b11111, 0b10000, 0b10000, 0b11110, 0b10000, 0b10000, 0b11111}},
    {'G', {0b01110, 0b10001, 0b10000, 0b10111, 0b10001, 0b10001, 0b01111}},
    {'H', {0b10001, 0b10001, 0b10001, 0b11111, 0b10001, 0b10001, 0b10001}},
    {'I', {0b01110, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b01110}},
    {'L', {0b10000, 0b10000, 0b10000, 0b10000, 0b10000, 0b10000, 0b11111}},
    {'M', {0b10001, 0b11011, 0b10101, 0b10101, 0b10001, 0b10001, 0b10001}},
    {'N', {0b10001, 0b11001, 0b10101, 0b10011, 0b10001, 0b10001, 0b10001}},
    {'O', {0b01110, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01110}},
    {'P', {0b11110, 0b10001, 0b10001, 0b11110, 0b10000, 0b10000, 0b10000}},
    {'S', {0b01111, 0b10000, 0b10000, 0b01110, 0b00001, 0b00001, 0b11110}},
    {'1', {0b00100, 0b01100, 0b00100, 0b00100, 0b00100, 0b00100, 0b01110}},
    {'2', {0b01110, 0b10001, 0b00001, 0b00010, 0b00100, 0b01000, 0b11111}},
    {'3', {0b11110, 0b00001, 0b00001, 0b01110, 0b00001, 0b00001, 0b11110}},
    {'4', {0b00010, 0b00110, 0b01010, 0b10010, 0b11111, 0b00010, 0b00010}},
};

const Glyph* find_glyph(char c) {
    for (const auto& g : kFont)
        if (g.c == c) return &g;
    return nullptr;  // 空白など
}

// Y 平面に文字列を中央揃えで描く。1 文字 = 6x8 セル x scale
void draw_text(uint8_t* y_plane, uint32_t w, uint32_t h, std::string_view text, uint32_t scale, uint32_t center_y, uint8_t luma) {
    const uint32_t text_w = uint32_t(text.size()) * 6 * scale;
    if (text_w >= w || 8 * scale >= h) return;
    const uint32_t x0 = (w - text_w) / 2;
    const uint32_t y0 = center_y > 4 * scale ? center_y - 4 * scale : 0;
    for (size_t i = 0; i < text.size(); ++i) {
        const Glyph* g = find_glyph(text[i]);
        if (!g) continue;
        for (uint32_t row = 0; row < 7; ++row) {
            for (uint32_t col = 0; col < 5; ++col) {
                if (!(g->rows[row] & (0b10000 >> col))) continue;
                const uint32_t px = x0 + uint32_t(i) * 6 * scale + col * scale;
                const uint32_t py = y0 + row * scale;
                for (uint32_t dy = 0; dy < scale && py + dy < h; ++dy) {
                    memset(y_plane + size_t(py + dy) * w + px, luma, scale);
                }
            }
        }
    }
}

}  // namespace

void convert_nv12(const uint8_t* src, uint32_t src_w, uint32_t src_h, uint8_t* dst, const OutputFormat& out) {
    const uint32_t dw = out.fmt.width;
    const uint32_t dh = out.fmt.height;
    if (out.pix == PixelFormat::YUY2) {
        nv12_to_yuy2(src, src_w, src_h, dst, dw, dh);
    } else if (src_w == dw && src_h == dh) {
        memcpy(dst, src, out.fmt.nv12_bytes());
    } else {
        nv12_to_nv12_scaled(src, src_w, src_h, dst, dw, dh);
    }
}

std::vector<uint8_t> make_slate_nv12(uint32_t width, uint32_t height, int cam) {
    std::vector<uint8_t> buf(size_t(width) * height * 3 / 2);
    uint8_t* y = buf.data();
    memset(y, 32, size_t(width) * height);                                         // 暗いグレー (limited range)
    memset(y + size_t(width) * height, 128, buf.size() - size_t(width) * height);  // 無彩色

    // 注: small は Windows ヘッダのマクロなので変数名に使えない
    const uint32_t title_scale = height / 110 > 0 ? height / 110 : 1;
    const uint32_t label_scale = title_scale / 3 > 0 ? title_scale / 3 : 1;
    draw_text(y, width, height, "NO SIGNAL", title_scale, height / 2, 200);
    const char label[] = {'D', 'E', 'N', 'P', 'A', 'C', 'H', 'A', 'N', ' ', 'C', 'A', 'M', ' ', char('0' + cam), 0};
    draw_text(y, width, height, label, label_scale, height / 2 + 8 * title_scale, 120);
    return buf;
}

}  // namespace denpachan::vcam
