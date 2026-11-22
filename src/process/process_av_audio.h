#pragma once

#include <algorithm>
#include <cstdint>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/version.h>
#include <libswresample/swresample.h>
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

  inline int prepareAudioResampleFrame(AVFrame *& frame, int & capacity, AVCodecContext *encoder, SwrContext *resampler,
                                       int inputSamples) {
    const int pending = swr_get_out_samples(resampler, inputSamples);
    if (pending < 0) { return pending; }
    const int required = std::max(pending, encoder->frame_size > 0 ? encoder->frame_size : encoder->sample_rate / 50);
    if (!frame || required > capacity) {
      AVFrame *replacement = av_frame_alloc();
      if (!replacement) { return AVERROR(ENOMEM); }
      replacement->sample_rate = encoder->sample_rate;
      replacement->format = encoder->sample_fmt;
      replacement->nb_samples = required;
#if LIBAVUTIL_VERSION_MAJOR < 57 || (LIBAVUTIL_VERSION_MAJOR == 57 && LIBAVUTIL_VERSION_MINOR < 24)
      replacement->channels = encoder->channels;
      replacement->channel_layout = encoder->channel_layout;
#else
      int result = av_channel_layout_copy(&replacement->ch_layout, &encoder->ch_layout);
      if (result < 0) {
        av_frame_free(&replacement);
        return result;
      }
#endif
      int allocated = av_frame_get_buffer(replacement, 0);
      if (allocated < 0) {
        av_frame_free(&replacement);
        return allocated;
      }
      av_frame_free(&frame);
      frame = replacement;
      capacity = required;
    }
    // Encoding may retain the previous samples and change nb_samples to its frame size.
    frame->nb_samples = capacity;
    return av_frame_make_writable(frame);
  }

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

  inline unsigned int limitedAudioChannels(const std::string & codec, unsigned int sourceChannels, bool sourceHasLFE,
                                           unsigned int maximum) {
    return encodedAudioChannels(codec, maximum ? std::min(sourceChannels, maximum) : sourceChannels, sourceHasLFE);
  }

  inline void setAudioChannelLayout(AVCodecContext *context, unsigned int channels, const std::string & codec = "", AVDictionary **options = 0) {
    if (codec == "opus" && options && channels > 2) {
      av_dict_set_int(options, "mapping_family", channels > 8 ? 255 : 1, 0);
    }
    // Family 1 uses Vorbis speaker positions, which differ from FFmpeg's default
    // three- and four-channel layouts (2.1 and 4.0).
    uint64_t opusLayout = 0;
    if (codec == "opus" && channels == 3) { opusLayout = AV_CH_LAYOUT_SURROUND; }
    if (codec == "opus" && channels == 4) { opusLayout = AV_CH_LAYOUT_QUAD; }
#if LIBAVUTIL_VERSION_MAJOR < 57 || (LIBAVUTIL_VERSION_MAJOR == 57 && LIBAVUTIL_VERSION_MINOR < 24)
    context->channels = channels;
    context->channel_layout = opusLayout ? opusLayout
      : codec == "AAC" && channels == 3  ? AV_CH_LAYOUT_SURROUND
                                         : av_get_default_channel_layout(channels);
#else
    av_channel_layout_uninit(&context->ch_layout);
    if (opusLayout) {
      av_channel_layout_from_mask(&context->ch_layout, opusLayout);
    } else if (codec == "AAC" && channels == 3) {
      av_channel_layout_from_mask(&context->ch_layout, AV_CH_LAYOUT_SURROUND);
    } else {
      av_channel_layout_default(&context->ch_layout, channels);
    }
#endif
  }
} // namespace Mist
