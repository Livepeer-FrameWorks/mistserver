#include "../src/process/process_av_cuda.h"

#include <cassert>
#include <cstdio>

int main() {
  AVFrame *frame = av_frame_alloc();
  assert(frame);
  std::string filters;
  frame->format = AV_PIX_FMT_YUV420P;
  assert(Mist::cudaDownloadFilters(frame, filters) && filters.empty());
  frame->format = AV_PIX_FMT_CUDA;
  assert(!Mist::cudaDownloadFilters(frame, filters));
  frame->hw_frames_ctx = av_buffer_allocz(sizeof(AVHWFramesContext));
  assert(frame->hw_frames_ctx);
  AVHWFramesContext *pool = (AVHWFramesContext *)frame->hw_frames_ctx->data;
  for (const auto format : {AV_PIX_FMT_NV12, AV_PIX_FMT_YUV420P10LE, AV_PIX_FMT_P010LE}) {
    pool->sw_format = format;
    assert(Mist::cudaDownloadFilters(frame, filters));
    assert(filters == std::string("hwdownload,format=") + av_get_pix_fmt_name(format));
  }
  {
    AVCodecContext *encoder = avcodec_alloc_context3(0);
    assert(encoder);
    encoder->pix_fmt = AV_PIX_FMT_CUDA;
    encoder->hw_frames_ctx = av_buffer_allocz(sizeof(AVHWFramesContext));
    assert(encoder->hw_frames_ctx);
    AVHWFramesContext *target = (AVHWFramesContext *)encoder->hw_frames_ctx->data;
    pool->sw_format = target->sw_format = AV_PIX_FMT_NV12;
    if (!Mist::keepVideoOnDevice(frame, encoder, true)) {
      fprintf(stderr, "matching CUDA decode/encode pools unnecessarily download to CPU\n");
      return 1;
    }
    assert(!Mist::keepVideoOnDevice(frame, encoder, false));
    target->sw_format = AV_PIX_FMT_P010LE;
    assert(!Mist::keepVideoOnDevice(frame, encoder, true));
    encoder->pix_fmt = AV_PIX_FMT_YUV420P;
    assert(!Mist::keepVideoOnDevice(frame, encoder, true));
    avcodec_free_context(&encoder);
  }
  pool->sw_format = AV_PIX_FMT_NONE;
  assert(!Mist::cudaDownloadFilters(frame, filters));
  av_frame_free(&frame);
  return 0;
}
