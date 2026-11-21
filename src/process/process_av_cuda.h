#pragma once
#include "process_av_hardware.h"

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
  inline int prepareHardwareEncodeFrame(AVFrame *input, AVCodecContext *encoder, AVFrame *upload, const AVFrame *& prepared,
                                        int (*acquire)(AVCodecContext *, AVFrame *) = acquireHardwareUploadFrame,
                                        int (*transfer)(AVFrame *, const AVFrame *, int) = av_hwframe_transfer_data) {
    prepared = 0;
    if (!input || !encoder || !upload) { return AVERROR(EINVAL); }
    if (input->format == encoder->pix_fmt && input->hw_frames_ctx &&
        (input->format != AV_PIX_FMT_CUDA ||
         (encoder->hw_frames_ctx && input->hw_frames_ctx->data == encoder->hw_frames_ctx->data))) {
      prepared = input;
      return 0;
    }
    int result = acquire(encoder, upload);
    if (result < 0) { return result; }
    result = transfer(upload, input, 0);
    if (result < 0) { return result; }
    result = av_frame_copy_props(upload, input);
    if (result < 0) { return result; }
    prepared = upload;
    return 0;
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
