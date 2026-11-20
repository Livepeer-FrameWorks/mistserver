#include "../src/process/process_av_hardware.h"

#include <cassert>
#include <cstdio>
#include <cstring>

int main(int argc, char **argv) {
  AVCodecContext *encoder = avcodec_alloc_context3(NULL);
  AVFrame *frame = av_frame_alloc();
  assert(Mist::acquireHardwareUploadFrame(encoder, frame) == AVERROR(EINVAL));
  if (argc < 2 || strcmp(argv[1], "vt")) {
    av_frame_free(&frame);
    avcodec_free_context(&encoder);
    return 0;
  }
  AVBufferRef *device = 0;
  if (av_hwdevice_ctx_create(&device, AV_HWDEVICE_TYPE_VIDEOTOOLBOX, 0, 0, 0) < 0) { return 77; }
  assert(Mist::configureHardwareUploadPool(encoder, device, AV_PIX_FMT_VIDEOTOOLBOX, AV_PIX_FMT_NV12, 0, 72) == AVERROR(EINVAL));
  assert(Mist::configureHardwareUploadPool(encoder, device, AV_PIX_FMT_VIDEOTOOLBOX, AV_PIX_FMT_NV12, 128, 72) == 0);
  assert(((AVHWFramesContext *)encoder->hw_frames_ctx->data)->initial_pool_size == 8);
  assert(Mist::acquireHardwareUploadFrame(encoder, frame) == 0);
  AVFrame *retained = av_frame_clone(frame);
  assert(retained);
  assert(Mist::acquireHardwareUploadFrame(encoder, frame) == 0);
  const bool reused = retained->data[3] == frame->data[3];
  av_frame_free(&retained);
  av_frame_free(&frame);
  avcodec_free_context(&encoder);
  av_buffer_unref(&device);
  if (reused) {
    fprintf(stderr, "hardware upload reused a pixel buffer still retained by the encoder\n");
    return 1;
  }
  return 0;
}
