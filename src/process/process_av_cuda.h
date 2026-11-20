#pragma once
#include <string>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
}

namespace Mist {
  inline bool keepVideoOnDevice(const AVFrame *input, const AVCodecContext *encoder, bool unchanged) {
    if (!input || !encoder || input->format != encoder->pix_fmt) { return false; }
    if (input->format == AV_PIX_FMT_VIDEOTOOLBOX) { return true; }
    if (input->format != AV_PIX_FMT_CUDA || !unchanged || !input->hw_frames_ctx || !encoder->hw_frames_ctx) { return false; }
    const AVHWFramesContext *source = (const AVHWFramesContext *)input->hw_frames_ctx->data;
    const AVHWFramesContext *target = (const AVHWFramesContext *)encoder->hw_frames_ctx->data;
    return source->sw_format != AV_PIX_FMT_NONE && source->sw_format == target->sw_format;
  }
  inline bool cudaDownloadFilters(const AVFrame *frame, std::string & filters) {
    filters.clear();
    if (!frame) { return false; }
    if (frame->format != AV_PIX_FMT_CUDA) { return true; }
    if (!frame->hw_frames_ctx) { return false; }
    const AVHWFramesContext *pool = (const AVHWFramesContext *)frame->hw_frames_ctx->data;
    const char *format = av_get_pix_fmt_name(pool->sw_format);
    if (!format) { return false; }
    filters = std::string("hwdownload,format=") + format;
    return true;
  }
} // namespace Mist
