#include "../src/process/process_av_rotation.h"

#include <mist/mp4_generic.h>
#include <mist/util.h>

#include <cassert>
#include <cstdarg>
extern "C" {
#include <libavutil/log.h>
}
static std::string scaleLog;
static void captureScaleLog(void *context, int level, const char *format, va_list args) {
  char line[4096];
  int prefix = 1;
  av_log_format_line(context, level, format, args, line, sizeof(line), &prefix);
  scaleLog += line;
}

extern "C" {
#include <libavutil/display.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}
static const Mist::Orientation orientations[] = {
  Mist::Orientation(),    Mist::Orientation(),          Mist::Orientation(0, true),
  Mist::Orientation(180), Mist::Orientation(180, true), Mist::Orientation(90, true),
  Mist::Orientation(90),  Mist::Orientation(270, true), Mist::Orientation(270)};

int main(int argc, char **argv) {
  if (argc > 1 && std::string(argv[1]) == "scale") {
    AVFrame *input = av_frame_alloc();
    input->format = AV_PIX_FMT_YUV420P;
    input->width = 3840;
    input->height = 2160;
    if (av_frame_get_buffer(input, 32) < 0) { return 2; }
    for (int plane = 0; plane < 3; ++plane) {
      int width = plane ? 1920 : 3840, height = plane ? 1080 : 2160;
      for (int y = 0; y < height; ++y) {
        if (fread(input->data[plane] + y * input->linesize[plane], 1, width, stdin) != (size_t)width) { return 2; }
      }
    }
    Mist::VideoTransform transform;
    AVFrame *out = transform.apply(input, AV_PIX_FMT_YUV420P, 640, 360, Mist::Orientation());
    if (!out) { return 2; }
    for (int plane = 0; plane < 3; ++plane) {
      int width = plane ? 320 : 640, height = plane ? 180 : 360;
      for (int y = 0; y < height; ++y) {
        if (fwrite(out->data[plane] + y * out->linesize[plane], 1, width, stdout) != (size_t)width) { return 2; }
      }
    }
    av_frame_free(&input);
    return 0;
  }

  for (AVPixelFormat format : {AV_PIX_FMT_YUV422P, AV_PIX_FMT_YUYV422, AV_PIX_FMT_UYVY422}) {
    AVFrame *packed = av_frame_alloc();
    packed->format = format;
    packed->width = 32;
    packed->height = 18;
    assert(av_frame_get_buffer(packed, 32) >= 0);
    scaleLog.clear();
    const int oldLogLevel = av_log_get_level();
    av_log_set_level(AV_LOG_VERBOSE);
    av_log_set_callback(captureScaleLog);
    Mist::VideoTransform rotate;
    AVFrame *result = rotate.apply(packed, AV_PIX_FMT_YUV420P, 18, 32, Mist::Orientation(90));
    av_log_set_callback(av_log_default_callback);
    av_log_set_level(oldLogLevel);
    assert(result);
    bool automatic = false;
    size_t start = 0;
    while (start < scaleLog.size()) {
      const size_t end = scaleLog.find('\n', start);
      const std::string line = scaleLog.substr(start, end == std::string::npos ? end : end - start);
      if (line.find("auto_scale_") != std::string::npos && line.find("flags:") != std::string::npos) {
        automatic = true;
        if (line.find("flags:0x2") == std::string::npos && line.find("flags:0x00000002") == std::string::npos &&
            line.find("flags:bilinear") == std::string::npos && line.find("flags:'bilinear'") == std::string::npos) {
          fprintf(stderr, "Automatic rotation conversion ignored bilinear scaling: %s\n", line.c_str());
          return 1;
        }
      }
      if (end == std::string::npos) { break; }
      start = end + 1;
    }
    if (!automatic) {
      fprintf(stderr, "Rotation fixture did not exercise an automatic scaler: %s\n", scaleLog.c_str());
      return 1;
    }
    av_frame_free(&packed);
  }

  // Distinct, asymmetric corner values detect every D4 element independently.
  AVFrame *input = av_frame_alloc();
  input->format = AV_PIX_FMT_GRAY8;
  input->width = 32;
  input->height = 18;
  assert(av_frame_get_buffer(input, 32) >= 0);
  for (int y = 0; y < 18; ++y) {
    for (int x = 0; x < 32; ++x) {
      input->data[0][y * input->linesize[0] + x] = (x * x * 11 + y * y * 17 + x * y * 3) % 251;
    }
  }
  Mist::VideoTransform transform;
  {
    AVFrame *expected = av_frame_alloc();
    expected->format = AV_PIX_FMT_GRAY8;
    expected->width = 19;
    expected->height = 11;
    assert(av_frame_get_buffer(expected, 32) >= 0);
    SwsContext *scale = sws_getContext(32, 18, AV_PIX_FMT_GRAY8, 19, 11, AV_PIX_FMT_GRAY8, SWS_BILINEAR, 0, 0, 0);
    assert(scale);
    sws_scale(scale, input->data, input->linesize, 0, 18, expected->data, expected->linesize);
    AVFrame *actual = transform.apply(input, AV_PIX_FMT_GRAY8, 19, 11, Mist::Orientation());
    assert(actual);
    for (int y = 0; y < 11; ++y) {
      if (memcmp(actual->data[0] + y * actual->linesize[0], expected->data[0] + y * expected->linesize[0], 19)) {
        fprintf(stderr, "CPU video scaling changed from SWS_BILINEAR pixel output\n");
        return 1;
      }
    }
    sws_freeContext(scale);
    av_frame_free(&expected);
  }
  {
    Mist::VideoTransform rebuild;
    if (!rebuild.apply(input, AV_PIX_FMT_GRAY8, 19, 11, Mist::Orientation())) { return 1; }
    if (rebuild.apply(input, AV_PIX_FMT_CUDA, 19, 11, Mist::Orientation())) { return 1; }
    if (!rebuild.apply(input, AV_PIX_FMT_GRAY8, 19, 11, Mist::Orientation())) {
      fprintf(stderr, "failed graph rebuild poisoned a later valid frame\n");
      return 1;
    }
  }
  for (unsigned exif = 1; exif <= 8; ++exif) {
    Mist::Orientation orientation = orientations[exif], decoded;
    int32_t matrix[9];
    av_display_rotation_set(matrix, orientation.rotation);
    av_display_matrix_flip(matrix, orientation.mirror, 0);
    assert(Mist::Orientation::fromMatrix(matrix[0], matrix[1], matrix[3], matrix[4], decoded));
    assert(decoded == orientation);
    const int width = exif >= 5 ? 18 : 32, height = exif >= 5 ? 32 : 18;
    AVFrame *out = transform.apply(input, AV_PIX_FMT_GRAY8, width, height, orientation);
    assert(out && out->width == width && out->height == height);
    for (int y = 0; y < 18; ++y) {
      for (int x = 0; x < 32; ++x) {
        int dx = x, dy = y;
        // Golden EXIF pixel positions are independent of Orientation's angle,
        // matrix decomposition and filter construction.
        switch (exif) {
          case 2: dx = 31 - x; break;
          case 3:
            dx = 31 - x;
            dy = 17 - y;
            break;
          case 4: dy = 17 - y; break;
          case 5:
            dx = y;
            dy = x;
            break;
          case 6:
            dx = 17 - y;
            dy = x;
            break;
          case 7:
            dx = 17 - y;
            dy = 31 - x;
            break;
          case 8:
            dx = y;
            dy = 31 - x;
            break;
        }
        assert(out->data[0][dy * out->linesize[0] + dx] == input->data[0][y * input->linesize[0] + x]);
      }
    }
    for (AVPixelFormat format : {AV_PIX_FMT_YUV420P, AV_PIX_FMT_NV12, AV_PIX_FMT_YUYV422, AV_PIX_FMT_UYVY422,
                                 AV_PIX_FMT_RGB24, AV_PIX_FMT_YUV420P10LE}) {
      out = transform.apply(input, format, width * 2, height * 2, orientation);
      assert(out && out->width == width * 2 && out->height == height * 2 && out->format == format);
    }
    assert(orientation.exif() == exif);
    static const char *hardwareDirections[] = {"",      "",           "hflip", "reversal", "vflip", "cclock_flip",
                                               "clock", "clock_flip", "cclock"};
    for (const std::string & backend : {std::string("vt"), std::string("vaapi")}) {
      const std::string expected = exif == 1 ? "" : "transpose_" + backend + "=dir=" + hardwareDirections[exif];
      assert(orientation.filters(backend) == expected);
    }
    // The native matrix matches FFmpeg for mirrored 0/180 as well as quarter turns.
    MP4::TKHD tkhd(1, 0, 32, 18);
    tkhd.setOrientation(orientation);
    assert(tkhd.getOrientation() == orientation);
    for (int idx : {0, 1, 3, 4}) { assert(tkhd.getMatrix(idx) == matrix[idx] / 65536); }
  }
  assert(Mist::Orientation(180).filters() == "hflip,vflip");
  assert(Mist::Orientation(90, true).filters() == "transpose=cclock_flip");
  assert(Mist::Orientation(270, true).filters() == "transpose=clock_flip");
  Mist::Orientation invalid;
  assert(!Mist::Orientation::fromMatrix(1, 0.25, 0, 1, invalid));
  DTSC::Meta meta;
  meta.reInit("", true);
  const size_t track = meta.addTrack();
  meta.setType(track, "video");
  meta.setWidth(track, 32);
  meta.setHeight(track, 18);
  for (unsigned angle : {0, 90, 180, 270}) {
    meta.setRotation(track, angle);
    meta.setMirror(track, true);
    MP4::TKHD progressive(meta, track), fragmented(meta, track, false);
    assert(progressive.getOrientation() == Mist::Orientation(angle, true));
    assert(fragmented.getOrientation() == Mist::Orientation(angle, false));
    for (unsigned movieCode = 1; movieCode <= 8; ++movieCode) {
      const Mist::Orientation movie = orientations[movieCode], original(angle, true);
      double a, b, c, d, e, f, g, h, oa, ob, oc, od;
      original.matrix(a, b, c, d);
      movie.matrix(e, f, g, h);
      original.then(movie).matrix(oa, ob, oc, od);
      assert(oa == a * e + b * g && ob == a * f + b * h && oc == c * e + d * g && od == c * f + d * h);
    }
  }
  av_frame_free(&input);
  return 0;
}
