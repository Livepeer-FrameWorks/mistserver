#pragma once
extern "C" {
#include <libavutil/pixdesc.h>
}

namespace Mist {
  inline bool videoToolboxDecodeRequested(bool allowHardware, bool allowSoftware) {
    return allowHardware && !allowSoftware;
  }

  inline AVPixelFormat videoToolboxDecoderFormat(const AVPixelFormat *formats, bool allowSoftware, bool & declined) {
    for (const AVPixelFormat *format = formats; *format != AV_PIX_FMT_NONE; ++format) {
      if (*format == AV_PIX_FMT_VIDEOTOOLBOX) { return *format; }
    }
    if (allowSoftware) {
      for (const AVPixelFormat *format = formats; *format != AV_PIX_FMT_NONE; ++format) {
        const AVPixFmtDescriptor *description = av_pix_fmt_desc_get(*format);
        if (description && !(description->flags & AV_PIX_FMT_FLAG_HWACCEL)) { return *format; }
      }
    }
    declined = true;
    return AV_PIX_FMT_NONE;
  }
} // namespace Mist
