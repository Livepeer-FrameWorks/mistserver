#include "../src/process/process_av_rotation.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

static const Mist::Orientation orientations[] = {
  Mist::Orientation(),    Mist::Orientation(),          Mist::Orientation(0, true),
  Mist::Orientation(180), Mist::Orientation(180, true), Mist::Orientation(90, true),
  Mist::Orientation(90),  Mist::Orientation(270, true), Mist::Orientation(270)};

int main(int argc, char **argv) {
  if (argc > 1 && std::string(argv[1]) == "registry") {
    for (const char *name : {"buffer", "buffersink", "scale", "format", "transpose", "hflip", "vflip", "hwdownload"}) {
      if (!avfilter_get_by_name(name)) {
        fprintf(stderr, "Required orientation filter missing: %s\n", name);
        return 1;
      }
    }
#ifdef __APPLE__
    for (const char *name : {"transpose_vt", "scale_vt"}) {
      if (!avfilter_get_by_name(name)) {
        fprintf(stderr, "Required VideoToolbox filter missing: %s\n", name);
        return 1;
      }
    }
#endif
    return 0;
  }
  const AVPixelFormat hardwareFormat = AV_PIX_FMT_VIDEOTOOLBOX;
  const AVHWDeviceType deviceType = AV_HWDEVICE_TYPE_VIDEOTOOLBOX;
  if (!avfilter_get_by_name("transpose_vt")) {
    fprintf(stderr, "Hardware transpose filter is not built\n");
    return 77;
  }
  AVBufferRef *device = 0;
  if (av_hwdevice_ctx_create(&device, deviceType, 0, 0, 0) < 0) {
    fprintf(stderr, "Hardware device is unavailable\n");
    return 77;
  }
  AVBufferRef *frames = av_hwframe_ctx_alloc(device);
  assert(frames);
  AVHWFramesContext *ctx = (AVHWFramesContext *)frames->data;
  ctx->format = hardwareFormat;
  ctx->sw_format = AV_PIX_FMT_NV12;
  ctx->width = 128;
  ctx->height = 72;
  assert(av_hwframe_ctx_init(frames) >= 0);
  AVFrame *software = av_frame_alloc(), *hardware = av_frame_alloc(), *downloaded = av_frame_alloc();
  assert(software && hardware && downloaded);
  software->format = AV_PIX_FMT_NV12;
  software->width = 128;
  software->height = 72;
  assert(av_frame_get_buffer(software, 32) >= 0);
  for (int y = 0; y < 72; ++y) {
    for (int x = 0; x < 128; ++x) {
      software->data[0][y * software->linesize[0] + x] = y < 36 ? (x < 64 ? 32 : 80) : (x < 64 ? 224 : 144);
    }
  }
  for (int y = 0; y < 36; ++y) { memset(software->data[1] + y * software->linesize[1], 128, 128); }
  assert(av_hwframe_get_buffer(frames, hardware, 0) >= 0);
  assert(av_hwframe_transfer_data(hardware, software, 0) >= 0);
  Mist::VideoTransform cpu, gpu;
  for (unsigned exif = 1; exif <= 8; ++exif) {
    const Mist::Orientation orientation = orientations[exif];
    const int width = orientation.rotation % 180 ? 36 : 64, height = orientation.rotation % 180 ? 64 : 36;
    AVFrame *reference = cpu.apply(software, AV_PIX_FMT_NV12, width, height, orientation);
    AVFrame *result = gpu.apply(hardware, hardwareFormat, width, height, orientation);
    if (!result) {
      char error[128];
      av_strerror(gpu.error, error, sizeof(error));
      fprintf(stderr, "%s\n", error);
    }
    assert(reference && result && result->hw_frames_ctx && result->format == hardwareFormat);
    assert(result->width == width && result->height == height);
    av_frame_unref(downloaded);
    assert(av_hwframe_transfer_data(downloaded, result, 0) >= 0);
    for (int y : {height / 8, height * 7 / 8}) {
      for (int x : {width / 8, width * 7 / 8}) {
        assert(std::abs(int(downloaded->data[0][y * downloaded->linesize[0] + x]) -
                        int(reference->data[0][y * reference->linesize[0] + x])) < 5);
      }
    }
  }
  av_frame_free(&software);
  av_frame_free(&hardware);
  av_frame_free(&downloaded);
  av_buffer_unref(&frames);
  av_buffer_unref(&device);
  return 0;
}
