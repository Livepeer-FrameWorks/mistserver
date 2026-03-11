#pragma once

#include <algorithm>
#include <string>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/error.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/log.h>
#include <libavutil/pixfmt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

namespace FFmpeg {

  class FFmpegUtils {
    public:
      // Error handling
      static void printAvError(const std::string & preamble, int code);

      // Frame management
      static void freeFrame(AVFrame **frame);

      // Logging
      static void setLogLevel(int level);
      static int getLogLevel();
      static void logCallback(void *ptr, int level, const char *fmt, va_list vargs);

      // Other helpers
      static bool isRawFormat(const std::string format) {
        std::string lowerFormat = format;
        std::transform(lowerFormat.begin(), lowerFormat.end(), lowerFormat.begin(), ::tolower);
        return lowerFormat == "yuyv" || lowerFormat == "uyvy" || lowerFormat == "nv12";
      }

      static AVPixelFormat getRawPixelFormat(const std::string format) {
        std::string lowerFormat = format;
        std::transform(lowerFormat.begin(), lowerFormat.end(), lowerFormat.begin(), ::tolower);
        if (lowerFormat == "uyvy") {
          return AV_PIX_FMT_UYVY422;
        } else if (lowerFormat == "nv12") {
          return AV_PIX_FMT_NV12;
        } else if (lowerFormat == "yuyv") {
          return AV_PIX_FMT_YUYV422;
        }
        return AV_PIX_FMT_NONE;
      }

      // Safe option helpers (guard option lookups for codec private data)
      static bool hasOption(void *priv, const char *name);
      static int setOption(void *priv, const char *key, const char *val, int flags);
      static int setOptionInt(void *priv, const char *key, int64_t val, int flags);

    private:
      static int logLevel;
  };

} // namespace FFmpeg
