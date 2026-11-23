#pragma once

#include <algorithm>
#include <string>
#include <vector>
#ifndef _WIN32
#include <glob.h>
#endif
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/version.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
}

namespace Mist {
  struct HardwareCodecCandidate {
      const AVCodec *codec;
      AVHWDeviceType device;
      AVPixelFormat format;
      AVPixelFormat software;
      bool hardware;
      std::string selector;
  };

  inline bool hardwarePixelFormat(AVPixelFormat format) {
    const AVPixFmtDescriptor *description = av_pix_fmt_desc_get(format);
    return description && (description->flags & AV_PIX_FMT_FLAG_HWACCEL);
  }

  inline std::vector<std::string> hardwareDeviceSelectors(AVHWDeviceType type) {
    std::vector<std::string> result(1);
#ifndef _WIN32
    const char *pattern = type == AV_HWDEVICE_TYPE_CUDA                ? "/dev/nvidia[0-9]*"
      : type == AV_HWDEVICE_TYPE_QSV || type == AV_HWDEVICE_TYPE_VAAPI ? "/dev/dri/renderD*"
                                                                       : 0;
    if (pattern) {
      glob_t matches = {};
      if (!glob(pattern, 0, 0, &matches)) {
        for (size_t i = 0; i < matches.gl_pathc; ++i) {
          std::string value = matches.gl_pathv[i];
          if (type == AV_HWDEVICE_TYPE_CUDA) {
            value.erase(0, std::string("/dev/nvidia").size());
            if (value.empty() || value.find_first_not_of("0123456789") != std::string::npos) { continue; }
          }
          result.push_back(value);
        }
      }
      globfree(&matches);
    }
#endif
    return result;
  }

  inline AVPixelFormat softwareEncoderFormat(const AVCodec *codec) {
    const AVPixelFormat *formats = 0;
#if LIBAVCODEC_VERSION_MAJOR >= 62
    const void *values = 0;
    int count = 0;
    if (avcodec_get_supported_config(0, codec, AV_CODEC_CONFIG_PIX_FORMAT, 0, &values, &count) >= 0) {
      formats = static_cast<const AVPixelFormat *>(values);
    }
#else
    formats = codec->pix_fmts;
#endif
    for (AVPixelFormat preferred :
         {codec->id == AV_CODEC_ID_MJPEG ? AV_PIX_FMT_YUVJ420P : AV_PIX_FMT_YUV420P, AV_PIX_FMT_NV12, AV_PIX_FMT_YUV420P}) {
      if (!formats) { return preferred; }
      for (const AVPixelFormat *p = formats; *p != AV_PIX_FMT_NONE; ++p) {
        if (*p == preferred) { return preferred; }
      }
    }
    return formats ? formats[0] : AV_PIX_FMT_YUV420P;
  }

  inline std::vector<HardwareCodecCandidate> videoCodecCandidates(AVCodecID id, bool encode, bool allowHardware, bool allowSoftware) {
    std::vector<HardwareCodecCandidate> result;
    const AVCodec *preferred = encode ? avcodec_find_encoder(id) : avcodec_find_decoder(id);
    void *iterator = 0;
    while (const AVCodec *codec = av_codec_iterate(&iterator)) {
      if (codec->id != id || (encode ? !av_codec_is_encoder(codec) : !av_codec_is_decoder(codec))) { continue; }
      bool offeredHardware = false;
      if (allowHardware) {
        for (int i = 0; const AVCodecHWConfig *config = avcodec_get_hw_config(codec, i); ++i) {
          if (config->device_type == AV_HWDEVICE_TYPE_NONE ||
              !(config->methods & (AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX | AV_CODEC_HW_CONFIG_METHOD_HW_FRAMES_CTX))) {
            continue;
          }
          offeredHardware = true;
          const AVPixelFormat format = hardwarePixelFormat(config->pix_fmt) ? config->pix_fmt : softwareEncoderFormat(codec);
          const AVPixelFormat software = config->device_type == AV_HWDEVICE_TYPE_CUDA ? AV_PIX_FMT_YUV420P : AV_PIX_FMT_NV12;
          for (const std::string & selector : hardwareDeviceSelectors(config->device_type)) {
            result.push_back({codec, config->device_type, format, software, true, selector});
          }
        }
      }
      const bool hardwareOnly = codec->capabilities & AV_CODEC_CAP_HARDWARE;
      if (allowHardware && hardwareOnly && !offeredHardware) {
        result.push_back({codec, AV_HWDEVICE_TYPE_NONE, softwareEncoderFormat(codec), AV_PIX_FMT_NONE, true, ""});
      }
      if (allowSoftware && !hardwareOnly) {
        result.push_back({codec, AV_HWDEVICE_TYPE_NONE, softwareEncoderFormat(codec), AV_PIX_FMT_NONE, false, ""});
      }
    }
    const auto rank = [preferred](const HardwareCodecCandidate & candidate) {
      if (!candidate.hardware) { return candidate.codec == preferred ? 100 : 101; }
      switch (candidate.device) {
        case AV_HWDEVICE_TYPE_QSV: return 0;
        case AV_HWDEVICE_TYPE_CUDA: return 1;
        case AV_HWDEVICE_TYPE_VAAPI: return 2;
        case AV_HWDEVICE_TYPE_VIDEOTOOLBOX: return 3;
        default: return 4;
      }
    };
    std::stable_sort(result.begin(), result.end(), [&](const HardwareCodecCandidate & a, const HardwareCodecCandidate & b) {
      return rank(a) < rank(b);
    });
    result.erase(std::unique(result.begin(), result.end(),
                             [](const HardwareCodecCandidate & a, const HardwareCodecCandidate & b) {
      return a.codec == b.codec && a.device == b.device && a.format == b.format && a.selector == b.selector &&
        a.hardware == b.hardware;
    }),
                 result.end());
    return result;
  }

  inline int createHardwareDevice(AVBufferRef **device, AVHWDeviceType type, const std::string & selector) {
    AVDictionary *options = 0;
    const char *name = selector.empty() ? 0 : selector.c_str();
    if (type == AV_HWDEVICE_TYPE_QSV && !selector.empty()) {
      av_dict_set(&options, "child_device", selector.c_str(), 0);
#ifndef _WIN32
      av_dict_set(&options, "child_device_type", "vaapi", 0);
#endif
      name = "hw";
    }
    const int result = av_hwdevice_ctx_create(device, type, name, options, 0);
    av_dict_free(&options);
    if (result < 0) { av_buffer_unref(device); }
    return result;
  }
} // namespace Mist
