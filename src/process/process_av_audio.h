#pragma once

#include <algorithm>
#include <cstdint>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/version.h>
}

namespace Mist {
  class AudioInputFormat {
      int rate = 0, format = -1;
#if LIBAVUTIL_VERSION_MAJOR < 57 || (LIBAVUTIL_VERSION_MAJOR == 57 && LIBAVUTIL_VERSION_MINOR < 24)
      uint64_t layout = 0;
      int channels = 0;
#else
      AVChannelLayout layout{};
#endif

    public:
      ~AudioInputFormat() {
#if LIBAVUTIL_VERSION_MAJOR >= 58 || (LIBAVUTIL_VERSION_MAJOR == 57 && LIBAVUTIL_VERSION_MINOR >= 24)
        av_channel_layout_uninit(&layout);
#endif
      }
      bool matches(const AVFrame *frame) const {
        if (frame->sample_rate != rate || frame->format != format) { return false; }
#if LIBAVUTIL_VERSION_MAJOR < 57 || (LIBAVUTIL_VERSION_MAJOR == 57 && LIBAVUTIL_VERSION_MINOR < 24)
        return layout == frame->channel_layout && channels == frame->channels;
#else
        return av_channel_layout_compare(&layout, &frame->ch_layout) == 0;
#endif
      }
      bool remember(const AVFrame *frame) {
        rate = frame->sample_rate;
        format = frame->format;
#if LIBAVUTIL_VERSION_MAJOR < 57 || (LIBAVUTIL_VERSION_MAJOR == 57 && LIBAVUTIL_VERSION_MINOR < 24)
        layout = frame->channel_layout;
        channels = frame->channels;
        return true;
#else
        av_channel_layout_uninit(&layout);
        return av_channel_layout_copy(&layout, &frame->ch_layout) >= 0;
#endif
      }
      AudioInputFormat() = default;
      AudioInputFormat(const AudioInputFormat &) = delete;
      AudioInputFormat & operator=(const AudioInputFormat &) = delete;
  };

  inline unsigned int encodedAudioChannels(const std::string & codec, unsigned int sourceChannels, bool sourceHasLFE = false) {
    if (!sourceChannels) { return 0; }
    if (codec == "opus") { return std::min(sourceChannels, 16u); }
    if (codec == "AAC") {
      if (sourceChannels == 7) { return 8; }
      if (sourceChannels == 3 && sourceHasLFE) { return 6; }
      return std::min(sourceChannels, 8u);
    }
    return sourceChannels;
  }

  inline void setAudioChannelLayout(AVCodecContext *context, unsigned int channels, const std::string & codec = "", AVDictionary **options = 0) {
    if (codec == "opus" && channels > 2 && options) { av_dict_set_int(options, "mapping_family", 255, 0); }
#if LIBAVUTIL_VERSION_MAJOR < 57 || (LIBAVUTIL_VERSION_MAJOR == 57 && LIBAVUTIL_VERSION_MINOR < 24)
    context->channels = channels;
    context->channel_layout = codec == "AAC" && channels == 3 ? AV_CH_LAYOUT_SURROUND : av_get_default_channel_layout(channels);
#else
    av_channel_layout_uninit(&context->ch_layout);
    if (codec == "AAC" && channels == 3) {
      av_channel_layout_from_mask(&context->ch_layout, AV_CH_LAYOUT_SURROUND);
    } else {
      av_channel_layout_default(&context->ch_layout, channels);
    }
#endif
  }
} // namespace Mist
