#pragma once
// 仮想カメラが提示する出力フォーマット一覧と AM_MEDIA_TYPE の変換。

#include <dshow.h>

#include <vector>

#include "denpachan/vcam_shm.h"

namespace denpachan::vcam {

enum class PixelFormat { NV12, YUY2 };

struct OutputFormat {
    Format fmt;
    PixelFormat pix = PixelFormat::NV12;

    REFERENCE_TIME frame_duration() const { return 10'000'000LL * fmt.fps_den / fmt.fps_num; }
    size_t image_bytes() const { return pix == PixelFormat::NV12 ? fmt.nv12_bytes() : size_t(fmt.width) * fmt.height * 2; }
    bool operator==(const OutputFormat&) const = default;
};

// 先頭が既定フォーマット (1920x1080 60fps NV12)
const std::vector<OutputFormat>& supported_formats();

// AM_MEDIA_TYPE が提示フォーマットのどれかに一致すればそれを返す
bool media_type_to_format(const AM_MEDIA_TYPE& mt, OutputFormat& out);

// mt の中身を作る (pbFormat は CoTaskMemAlloc)。使い終わったら free_media_type。
HRESULT format_to_media_type(const OutputFormat& f, AM_MEDIA_TYPE& mt);
AM_MEDIA_TYPE* create_media_type(const OutputFormat& f);  // 失敗時 nullptr

void free_media_type(AM_MEDIA_TYPE& mt);
void delete_media_type(AM_MEDIA_TYPE* pmt);
HRESULT copy_media_type(AM_MEDIA_TYPE& dst, const AM_MEDIA_TYPE& src);

}  // namespace denpachan::vcam
