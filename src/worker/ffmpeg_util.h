#pragma once

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

#include <memory>
#include <string>

namespace denpachan::worker {

struct AVFrameDeleter {
    void operator()(AVFrame* f) const { av_frame_free(&f); }
};
struct AVPacketDeleter {
    void operator()(AVPacket* p) const { av_packet_free(&p); }
};
struct AVCodecContextDeleter {
    void operator()(AVCodecContext* c) const { avcodec_free_context(&c); }
};
struct AVBufferRefDeleter {
    void operator()(AVBufferRef* b) const { av_buffer_unref(&b); }
};

using FramePtr = std::unique_ptr<AVFrame, AVFrameDeleter>;
using PacketPtr = std::unique_ptr<AVPacket, AVPacketDeleter>;
using CodecContextPtr = std::unique_ptr<AVCodecContext, AVCodecContextDeleter>;
using BufferRefPtr = std::unique_ptr<AVBufferRef, AVBufferRefDeleter>;

inline std::string av_error_string(int err) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(err, buf, sizeof(buf));
    return buf;
}

}  // namespace denpachan::worker
