#include "../src/process/process_av_cuda.h"

#include <cassert>
#include <cstdio>

static int acquired = 0, transferred = 0;
static int acquireFrame(AVCodecContext *encoder, AVFrame *frame) {
  ++acquired;
  av_frame_unref(frame);
  frame->format = encoder->pix_fmt;
  frame->hw_frames_ctx = av_buffer_ref(encoder->hw_frames_ctx);
  return frame->hw_frames_ctx ? 0 : AVERROR(ENOMEM);
}
static int transferFrame(AVFrame *to, const AVFrame *from, int flags) {
  ++transferred;
  if (to->format == AV_PIX_FMT_QSV || from->format == AV_PIX_FMT_QSV) { return 0; }
  assert(flags == 0 && to->format == AV_PIX_FMT_CUDA && from->format == AV_PIX_FMT_CUDA);
  assert(to->hw_frames_ctx->data != from->hw_frames_ctx->data);
  return 0;
}

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
    encoder->pix_fmt = AV_PIX_FMT_CUDA;
    target->sw_format = pool->sw_format = AV_PIX_FMT_NV12;
    AVFrame *upload = av_frame_alloc();
    const AVFrame *prepared = 0;
    const int result = Mist::prepareHardwareEncodeFrame(frame, encoder, upload, prepared, acquireFrame, transferFrame);
    if (result < 0 || acquired != 1 || transferred != 1 || prepared != upload ||
        prepared->hw_frames_ctx->data != encoder->hw_frames_ctx->data) {
      fprintf(stderr, "NVDEC surface bypassed transfer into the NVENC encoder pool: acquired=%d transferred=%d\n",
              acquired, transferred);
      return 1;
    }
    frame->format = encoder->pix_fmt = AV_PIX_FMT_QSV;
    acquired = transferred = 0;
    const int qsvResult = Mist::prepareHardwareEncodeFrame(frame, encoder, upload, prepared, acquireFrame, transferFrame);
    if (qsvResult < 0 || prepared == frame || acquired != 1 || transferred != 2) {
      fprintf(stderr, "Distinct QSV pools must download then upload: acquired=%d transferred=%d\n", acquired, transferred);
      return 1;
    }
    frame->format = encoder->pix_fmt = AV_PIX_FMT_CUDA;
    av_buffer_unref(&encoder->hw_frames_ctx);
    encoder->hw_frames_ctx = av_buffer_ref(frame->hw_frames_ctx);
    acquired = transferred = 0;
    assert(Mist::prepareHardwareEncodeFrame(frame, encoder, upload, prepared, acquireFrame, transferFrame) == 0);
    assert(prepared == frame && !acquired && !transferred);
    av_frame_free(&upload);
    puts("PASS CUDA encoder transfers differing pools on device and reuses a shared pool directly");
    avcodec_free_context(&encoder);
  }
  pool->sw_format = AV_PIX_FMT_NONE;
  assert(!Mist::cudaDownloadFilters(frame, filters));
  av_frame_free(&frame);
  return 0;
}
