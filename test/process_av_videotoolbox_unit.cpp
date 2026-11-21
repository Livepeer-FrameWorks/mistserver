#include "../src/process/process_av_videotoolbox.h"

#include <cassert>
#include <cstdio>

int main() {
  if (Mist::videoToolboxDecodeRequested(true, true) || Mist::videoToolboxDecodeRequested(false, true) ||
      !Mist::videoToolboxDecodeRequested(true, false)) {
    fprintf(stderr, "VideoToolbox decode must require accel=hw; auto must retain software decode\n");
    return 1;
  }
  bool declined = false;
  const AVPixelFormat hardware[] = {AV_PIX_FMT_VIDEOTOOLBOX, AV_PIX_FMT_YUV420P10LE, AV_PIX_FMT_NONE};
  const AVPixelFormat software[] = {AV_PIX_FMT_CUDA, AV_PIX_FMT_YUV420P10LE, AV_PIX_FMT_NV12, AV_PIX_FMT_NONE};
  const AVPixelFormat unavailable[] = {AV_PIX_FMT_CUDA, AV_PIX_FMT_NONE};
  assert(Mist::videoToolboxDecoderFormat(hardware, false, declined) == AV_PIX_FMT_VIDEOTOOLBOX && !declined);
  assert(Mist::videoToolboxDecoderFormat(software, true, declined) == AV_PIX_FMT_YUV420P10LE && !declined);
  assert(Mist::videoToolboxDecoderFormat(software, false, declined) == AV_PIX_FMT_NONE && declined);
  declined = false;
  assert(Mist::videoToolboxDecoderFormat(unavailable, true, declined) == AV_PIX_FMT_NONE && declined);
  puts("PASS automatic decode retains software and explicit hardware enables VideoToolbox");
  return 0;
}
