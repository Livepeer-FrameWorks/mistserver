#include "utils.h"

#include "../defines.h"

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
}

namespace FFmpeg {

  int FFmpegUtils::logLevel = AV_LOG_WARNING;

  void FFmpegUtils::printAvError(const std::string & preamble, int code) {
    char err[128];
    av_strerror(code, err, sizeof(err));
    ERROR_MSG("%s: `%s` (%i)", preamble.c_str(), err, code);
  }

  void FFmpegUtils::freeFrame(AVFrame **frame) {
    if (frame && *frame) {
      av_frame_free(frame);
      *frame = nullptr;
    }
  }

  void FFmpegUtils::setLogLevel(int level) {
    logLevel = level;
    av_log_set_level(level);
  }

  int FFmpegUtils::getLogLevel() {
    return logLevel;
  }

  void FFmpegUtils::logCallback(void *ptr, int level, const char *fmt, va_list vargs) {
    if (level > logLevel) { return; }

    char line[1024];
    static int print_prefix = 1;
    av_log_format_line(ptr, level, fmt, vargs, line, sizeof(line), &print_prefix);

    // Suppress messages about deprecated pixel formats: we know
    if (std::string(line).find("deprecated pixel format used") != std::string::npos) { return; }

    switch (level) {
      case AV_LOG_PANIC:
      case AV_LOG_FATAL: FAIL_MSG("%s", line); break;
      case AV_LOG_ERROR: ERROR_MSG("%s", line); break;
      case AV_LOG_WARNING: WARN_MSG("%s", line); break;
      case AV_LOG_INFO: INFO_MSG("%s", line); break;
      case AV_LOG_VERBOSE:
      case AV_LOG_DEBUG:
      case AV_LOG_TRACE: HIGH_MSG("%s", line); break;
      default: MEDIUM_MSG("%s", line); break;
    }
  }

  bool FFmpegUtils::hasOption(void *priv, const char *name) {
    if (!priv || !name) { return false; }
    const AVOption *opt = av_opt_find(priv, name, nullptr, 0, AV_OPT_SEARCH_CHILDREN);
    return opt != nullptr;
  }

  int FFmpegUtils::setOption(void *priv, const char *key, const char *val, int flags) {
    if (!priv || !key) { return AVERROR(EINVAL); }
    if (!hasOption(priv, key)) {
      // Keep this at MEDIUM to avoid noisy error logs for unsupported options
      MEDIUM_MSG("FFmpeg: Option '%s' not supported by this encoder; ignoring.", key);
      return 0;
    }
    return av_opt_set(priv, key, val, flags);
  }

  int FFmpegUtils::setOptionInt(void *priv, const char *key, int64_t val, int flags) {
    if (!priv || !key) { return AVERROR(EINVAL); }
    if (!hasOption(priv, key)) {
      MEDIUM_MSG("FFmpeg: Option '%s' not supported by this encoder; ignoring.", key);
      return 0;
    }
    return av_opt_set_int(priv, key, val, flags);
  }

} // namespace FFmpeg
