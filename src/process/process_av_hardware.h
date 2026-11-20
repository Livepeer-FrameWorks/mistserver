#pragma once
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
}

namespace Mist {
  inline int configureHardwareUploadPool(AVCodecContext *encoder, AVBufferRef *device, AVPixelFormat hardware,
                                         AVPixelFormat software, int width, int height) {
    if (!encoder || !device || width <= 0 || height <= 0) { return AVERROR(EINVAL); }
    AVBufferRef *frames = av_hwframe_ctx_alloc(device);
    if (!frames) { return AVERROR(ENOMEM); }
    AVHWFramesContext *pool = (AVHWFramesContext *)frames->data;
    pool->initial_pool_size = 8;
    pool->format = hardware;
    pool->sw_format = software;
    pool->width = width;
    pool->height = height;
    int result = av_hwframe_ctx_init(frames);
    if (result >= 0) {
      av_buffer_unref(&encoder->hw_frames_ctx);
      encoder->hw_frames_ctx = av_buffer_ref(frames);
      if (!encoder->hw_frames_ctx) { result = AVERROR(ENOMEM); }
    }
    av_buffer_unref(&frames);
    return result;
  }

  inline int acquireHardwareUploadFrame(AVCodecContext *encoder, AVFrame *frame) {
    if (!encoder || !encoder->hw_frames_ctx || !frame) { return AVERROR(EINVAL); }
    // An asynchronous encoder may still retain the previous pixel buffer.
    av_frame_unref(frame);
    return av_hwframe_get_buffer(encoder->hw_frames_ctx, frame, 0);
  }
} // namespace Mist
