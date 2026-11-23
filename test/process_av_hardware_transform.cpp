#include "../src/process/process_av_acceleration.h"
#include "../src/process/process_av_rotation.h"

#include <cstdio>
#include <cstdlib>

int main() {
  const char *kind = getenv("MIST_TEST_HW_DEVICE");
  if (!kind) { return 77; }
  const AVHWDeviceType type = av_hwdevice_find_type_by_name(kind);
  const AVPixelFormat format = type == AV_HWDEVICE_TYPE_QSV ? AV_PIX_FMT_QSV
    : type == AV_HWDEVICE_TYPE_CUDA                         ? AV_PIX_FMT_CUDA
                                                            : AV_PIX_FMT_VAAPI;
  AVBufferRef *device = 0;
  AVCodecContext *poolOwner = avcodec_alloc_context3(0);
  AVFrame *cpu = av_frame_alloc(), *gpu = av_frame_alloc();
  int error = Mist::createHardwareDevice(&device, type, getenv("MIST_TEST_HW_SELECTOR") ? getenv("MIST_TEST_HW_SELECTOR") : "");
  if (error >= 0) { error = Mist::configureHardwareUploadPool(poolOwner, device, format, AV_PIX_FMT_NV12, 320, 180); }
  if (error >= 0) { error = Mist::acquireHardwareUploadFrame(poolOwner, gpu); }
  if (error < 0 || !cpu) {
    fprintf(stderr, "hardware fixture setup failed: %d\n", error);
    return 2;
  }
  cpu->format = AV_PIX_FMT_NV12;
  cpu->width = 320;
  cpu->height = 180;
  if (av_frame_get_buffer(cpu, 32) < 0) { return 2; }
  for (int y = 0; y < 180; ++y) {
    for (int x = 0; x < 320; ++x) { cpu->data[0][y * cpu->linesize[0] + x] = x < 80 && y < 80 ? 82 : 41; }
  }
  for (int y = 0; y < 90; ++y) {
    for (int x = 0; x < 320; x += 2) {
      cpu->data[1][y * cpu->linesize[1] + x] = x < 80 && y < 40 ? 90 : 240;
      cpu->data[1][y * cpu->linesize[1] + x + 1] = x < 80 && y < 40 ? 240 : 110;
    }
  }
  if (av_hwframe_transfer_data(gpu, cpu, 0) < 0) { return 2; }
  int result = 0;
  {
    Mist::VideoTransform transform;
    for (int i = 0; i < 100; ++i) {
      gpu->pts = i * 100;
      AVFrame *output = transform.apply(gpu, AV_PIX_FMT_RGB24, 96, 160, Mist::Orientation(90));
      if (!output || output->width != 96 || output->height != 160 || output->pts != gpu->pts) {
        fprintf(stderr, "%s frame %d must scale, download and retain PTS: error=%d\n", kind, i, transform.error);
        if (output) {
          fprintf(stderr, "actual size=%dx%d pts=%ld expected=%ld\n", output->width, output->height, (long)output->pts,
                  (long)gpu->pts);
        }
        result = 1;
        break;
      }
      const int points[4][2] = {{10, 16}, {86, 16}, {86, 144}, {10, 144}};
      for (int corner = 0; corner < 4; ++corner) {
        const uint8_t *pixel = output->data[0] + output->linesize[0] * points[corner][1] + 3 * points[corner][0];
        const bool red = pixel[0] > 160 && pixel[0] > pixel[1] + 80 && pixel[0] > pixel[2] + 80;
        if (red != (corner == 1)) {
          fprintf(stderr, "incorrect hardware orientation corner=%d\n", corner);
          result = 1;
        }
      }
      if (result) { break; }
    }
  }
  av_frame_free(&cpu);
  av_frame_free(&gpu);
  avcodec_free_context(&poolOwner);
  av_buffer_unref(&device);
  if (!result) { printf("PASS real %s upload, orientation, scale and download: 100 frames\n", kind); }
  return result;
}
