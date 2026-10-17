#include "formats.h"

#include <dvdmedia.h>

#include <algorithm>
#include <cstdlib>

namespace denpachan::vcam {

namespace {

GUID subtype_of(PixelFormat pix) { return pix == PixelFormat::NV12 ? MEDIASUBTYPE_NV12 : MEDIASUBTYPE_YUY2; }

}  // namespace

const std::vector<OutputFormat>& supported_formats() {
    static const std::vector<OutputFormat> formats = [] {
        const Format sizes[] = {{1920, 1080}, {3840, 2160}, {2560, 1440}, {1280, 720}};
        const std::pair<uint32_t, uint32_t> rates[] = {{60, 1}, {60000, 1001}, {30, 1}, {30000, 1001}};
        std::vector<OutputFormat> v;
        for (auto pix : {PixelFormat::NV12, PixelFormat::YUY2}) {
            for (const auto& s : sizes) {
                for (const auto& [num, den] : rates) {
                    v.push_back({Format{s.width, s.height, num, den}, pix});
                }
            }
        }
        return v;
    }();
    return formats;
}

bool media_type_to_format(const AM_MEDIA_TYPE& mt, OutputFormat& out) {
    if (mt.majortype != MEDIATYPE_Video || !mt.pbFormat) return false;

    PixelFormat pix;
    if (mt.subtype == MEDIASUBTYPE_NV12) {
        pix = PixelFormat::NV12;
    } else if (mt.subtype == MEDIASUBTYPE_YUY2) {
        pix = PixelFormat::YUY2;
    } else {
        return false;
    }

    const BITMAPINFOHEADER* bmi = nullptr;
    REFERENCE_TIME avg = 0;
    if (mt.formattype == FORMAT_VideoInfo && mt.cbFormat >= sizeof(VIDEOINFOHEADER)) {
        auto* vih = reinterpret_cast<const VIDEOINFOHEADER*>(mt.pbFormat);
        bmi = &vih->bmiHeader;
        avg = vih->AvgTimePerFrame;
    } else if (mt.formattype == FORMAT_VideoInfo2 && mt.cbFormat >= sizeof(VIDEOINFOHEADER2)) {
        auto* vih = reinterpret_cast<const VIDEOINFOHEADER2*>(mt.pbFormat);
        bmi = &vih->bmiHeader;
        avg = vih->AvgTimePerFrame;
    } else {
        return false;
    }

    const uint32_t w = uint32_t(bmi->biWidth);
    const uint32_t h = uint32_t(std::abs(bmi->biHeight));

    // 同じ解像度・ピクセル形式の中から、フレーム間隔が最も近いもの (0.5% 以内) を選ぶ。
    // AvgTimePerFrame = 0 (指定なし) なら最初の候補 (60fps)。
    const OutputFormat* best = nullptr;
    REFERENCE_TIME best_diff = 0;
    for (const auto& f : supported_formats()) {
        if (f.pix != pix || f.fmt.width != w || f.fmt.height != h) continue;
        if (avg == 0) {
            best = &f;
            break;
        }
        const REFERENCE_TIME diff = std::abs(f.frame_duration() - avg);
        if (diff * 200 > f.frame_duration()) continue;
        if (!best || diff < best_diff) {
            best = &f;
            best_diff = diff;
        }
    }
    if (!best) return false;
    out = *best;
    return true;
}

HRESULT format_to_media_type(const OutputFormat& f, AM_MEDIA_TYPE& mt) {
    mt = AM_MEDIA_TYPE{};
    auto* vih = static_cast<VIDEOINFOHEADER*>(CoTaskMemAlloc(sizeof(VIDEOINFOHEADER)));
    if (!vih) return E_OUTOFMEMORY;
    *vih = VIDEOINFOHEADER{};

    const auto bytes = static_cast<DWORD>(f.image_bytes());
    vih->AvgTimePerFrame = f.frame_duration();
    // 4K60 の非圧縮は DWORD を超えるので頭打ちにする
    vih->dwBitRate = static_cast<DWORD>(
        std::min<uint64_t>(uint64_t(bytes) * 8 * f.fmt.fps_num / f.fmt.fps_den, MAXDWORD));
    vih->bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    vih->bmiHeader.biWidth = LONG(f.fmt.width);
    vih->bmiHeader.biHeight = LONG(f.fmt.height);
    vih->bmiHeader.biPlanes = 1;
    vih->bmiHeader.biBitCount = f.pix == PixelFormat::NV12 ? 12 : 16;
    vih->bmiHeader.biCompression = subtype_of(f.pix).Data1;  // FOURCC
    vih->bmiHeader.biSizeImage = bytes;

    mt.majortype = MEDIATYPE_Video;
    mt.subtype = subtype_of(f.pix);
    mt.bFixedSizeSamples = TRUE;
    mt.bTemporalCompression = FALSE;
    mt.lSampleSize = bytes;
    mt.formattype = FORMAT_VideoInfo;
    mt.cbFormat = sizeof(VIDEOINFOHEADER);
    mt.pbFormat = reinterpret_cast<BYTE*>(vih);
    return S_OK;
}

AM_MEDIA_TYPE* create_media_type(const OutputFormat& f) {
    auto* pmt = static_cast<AM_MEDIA_TYPE*>(CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE)));
    if (!pmt) return nullptr;
    if (FAILED(format_to_media_type(f, *pmt))) {
        CoTaskMemFree(pmt);
        return nullptr;
    }
    return pmt;
}

void free_media_type(AM_MEDIA_TYPE& mt) {
    if (mt.cbFormat != 0 && mt.pbFormat) CoTaskMemFree(mt.pbFormat);
    mt.cbFormat = 0;
    mt.pbFormat = nullptr;
    if (mt.pUnk) {
        mt.pUnk->Release();
        mt.pUnk = nullptr;
    }
}

void delete_media_type(AM_MEDIA_TYPE* pmt) {
    if (!pmt) return;
    free_media_type(*pmt);
    CoTaskMemFree(pmt);
}

HRESULT copy_media_type(AM_MEDIA_TYPE& dst, const AM_MEDIA_TYPE& src) {
    dst = src;
    dst.pbFormat = nullptr;
    dst.cbFormat = 0;
    if (src.cbFormat != 0 && src.pbFormat) {
        dst.pbFormat = static_cast<BYTE*>(CoTaskMemAlloc(src.cbFormat));
        if (!dst.pbFormat) {
            dst.pUnk = nullptr;
            return E_OUTOFMEMORY;
        }
        memcpy(dst.pbFormat, src.pbFormat, src.cbFormat);
        dst.cbFormat = src.cbFormat;
    }
    if (dst.pUnk) dst.pUnk->AddRef();
    return S_OK;
}

}  // namespace denpachan::vcam
