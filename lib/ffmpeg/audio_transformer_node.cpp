#include "audio_transformer_node.h"

#include "../defines.h"
#include "../timing.h"
#include "audio_frame_context.h"
#include "node_pipeline.h"

#include <cstring>
#include <sstream>
extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/time.h>
}

namespace FFmpeg {

  AudioTransformerNode::AudioTransformerNode(int sampleRate, int channelCount, AVSampleFormat format) {
    // Initialize audio parameters
    targetSampleRate = sampleRate;
    targetChannelCount = channelCount;
    targetFormat = format;
    updateLongName();
  }

  AudioTransformerNode::~AudioTransformerNode() {
    // Clean up resampler
    if (swrCtx) { swr_free(&swrCtx); }
    INFO_MSG("Audio Transformer: Destroyed audio transformer node");
  }

  bool AudioTransformerNode::init() {
    std::lock_guard<std::mutex> lock(mutex);

    // Initialize resampler if needed
    if (targetSampleRate > 0 || targetChannelCount > 0 || targetFormat != AV_SAMPLE_FMT_NONE) {
      needsResampling = true;
    }
    return true;
  }

  bool AudioTransformerNode::setInput(void * inData, size_t idx) {
    AudioFrameContext * frame = (AudioFrameContext*)inData;

    std::lock_guard<std::mutex> lock(mutex);

    VERYHIGH_MSG("Audio Transformer: Setting input frame @ pts=%" PRIu64 "", frame->getPts());

    // Now that we have both input and target formats, make smart decisions
    AVFrame *inFrame = frame->getAVFrame();
    if (!inFrame) {
      ERROR_MSG("Audio Transformer: Null input frame");
      droppedFrames++;
      return false;
    }

    analyzeFormatRequirements(inFrame);

    // Start processing time tracking
    uint64_t startTime = Util::getMicros();

    // Get input frame
    // Debug: Log input frame format
    VERYHIGH_MSG("Audio Transformer: Input frame format=%s, target format=%s",
                 av_get_sample_fmt_name(static_cast<AVSampleFormat>(inFrame->format)), av_get_sample_fmt_name(targetFormat));

    // Initialize resampler if needed
    if (!swrCtx && !initResampler(inFrame)) {
      ERROR_MSG("Audio Transformer: Failed to initialize resampler");
      droppedFrames++;
      return false;
    }

    // Get output frame from pool if enabled
    AVFrame *outFrame = av_frame_alloc();
    if (!outFrame) {
      ERROR_MSG("Audio Transformer: Failed to allocate output frame");
      droppedFrames++;
      return false;
    }

    // Use the transformFrame function to do the actual work
    if (!transformFrame(inFrame, outFrame)) {
      ERROR_MSG("Audio Transformer: Failed to transform frame");
      av_frame_free(&outFrame);
      droppedFrames++;
      return false;
    }

    // Debug: Log actual output frame format after resampling
    VERYHIGH_MSG("Audio Transformer: Output frame format after resampling: %s, channels=%d",
                 av_get_sample_fmt_name(static_cast<AVSampleFormat>(outFrame->format)), outFrame->ch_layout.nb_channels);

    VERYHIGH_MSG("Audio Transformer: Output frame pts=%" PRIu64 "", outFrame->pts);

    // Create output frame context
    AudioFrameContext outCtx(outFrame, (AVSampleFormat)outFrame->format, outFrame->ch_layout.nb_channels, outFrame->sample_rate);

    totalProcessingTime.fetch_add(Util::getMicros(startTime));

    for (auto & cb : callbacks){cb(&outCtx);}
    return true;
  }

  bool AudioTransformerNode::initResampler(AVFrame *inFrame) {
    // Set output properties
    int outSampleRate = targetSampleRate > 0 ? targetSampleRate : inFrame->sample_rate;
    AVSampleFormat outFormat = targetFormat != AV_SAMPLE_FMT_NONE ? targetFormat : (AVSampleFormat)inFrame->format;

    // Create output channel layout
    AVChannelLayout outLayout;

    if (targetChannelCount > 0) {
      // Use target channel count
      if (targetChannelCount == 1) {
        outLayout = AV_CHANNEL_LAYOUT_MONO;
      } else if (targetChannelCount == 2) {
        outLayout = AV_CHANNEL_LAYOUT_STEREO;
      } else if (targetChannelCount == 6) {
        outLayout = AV_CHANNEL_LAYOUT_5POINT1;
      } else {
        // For other channel counts, create a default layout
        av_channel_layout_default(&outLayout, targetChannelCount);
      }
    } else {
      // Use input layout
      outLayout = inFrame->ch_layout;
      targetChannelCount = inFrame->ch_layout.nb_channels;
    }

    // Debug: Log resampler configuration
    VERYHIGH_MSG("Audio Transformer: Resampler config - input: %s@%dHz, output: %s@%dHz",
                 av_get_sample_fmt_name(static_cast<AVSampleFormat>(inFrame->format)), inFrame->sample_rate,
                 av_get_sample_fmt_name(outFormat), outSampleRate);

    // Create resampler context using the same method as backup implementation
    swr_alloc_set_opts2(&swrCtx, &outLayout, outFormat, outSampleRate, &inFrame->ch_layout,
                        (AVSampleFormat)inFrame->format, inFrame->sample_rate, 0, NULL);
    if (!swrCtx) {
      ERROR_MSG("Audio Transformer: Failed to allocate resampler context");
      return false;
    }

    // Apply channel mapping if specified (using backup implementation approach)
    if (!channelMap.empty()) {
      int ret = swr_set_channel_mapping(swrCtx, channelMap.data());
      if (ret < 0) {
        FFmpegUtils::printAvError("Audio Transformer: Failed to set channel mapping", ret);
        swr_free(&swrCtx);
        return false;
      }

      INFO_MSG("Audio Transformer: Applied channel mapping: %d→%zu channels", inFrame->ch_layout.nb_channels, channelMap.size());

      // Debug: Log the mapping
      for (size_t i = 0; i < channelMap.size(); ++i) { INFO_MSG("Mapping: out[%zu]←in[%d]", i, channelMap[i]); }
    }

    // Initialize resampler
    int ret = swr_init(swrCtx);
    if (ret < 0) {
      FFmpegUtils::printAvError("Audio Transformer: Failed to initialize resampler", ret);
      swr_free(&swrCtx);
      return false;
    }

    INFO_MSG("Audio Transformer: Resampler initialized successfully");
    return true;
  }

  bool AudioTransformerNode::transformFrame(AVFrame *inFrame, AVFrame *outFrame) {
    if (!inFrame || !outFrame || !swrCtx) {
      ERROR_MSG("Audio Transformer: Invalid parameters for frame transformation");
      return false;
    }

    // Set output frame properties
    outFrame->format = targetFormat != AV_SAMPLE_FMT_NONE ? targetFormat : inFrame->format;
    outFrame->sample_rate = targetSampleRate;
    outFrame->pts = inFrame->pts;
    outFrame->pkt_dts = inFrame->pkt_dts;
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 30, 100)
    outFrame->duration = inFrame->duration;
#else
    outFrame->pkt_duration = inFrame->pkt_duration;
#endif

    // Set channel layout
    outFrame->ch_layout.nb_channels = targetChannelCount;
    if (targetChannelCount == 1) {
      outFrame->ch_layout = AV_CHANNEL_LAYOUT_MONO;
    } else if (targetChannelCount == 2) {
      outFrame->ch_layout = AV_CHANNEL_LAYOUT_STEREO;
    } else if (targetChannelCount == 6) {
      outFrame->ch_layout = AV_CHANNEL_LAYOUT_5POINT1;
    } else {
      outFrame->ch_layout = inFrame->ch_layout;
    }

    // Calculate output sample count
    int outSamples = swr_get_out_samples(swrCtx, inFrame->nb_samples);
    if (outSamples <= 0) {
      ERROR_MSG("Audio Transformer: Invalid output sample count: %d", outSamples);
      return false;
    }

    // Set output frame sample count
    outFrame->nb_samples = outSamples;

    VERYHIGH_MSG("Audio Transformer: Converting frame with %d input channels to %d output "
                 "channels, %d→%d samples",
                 inFrame->ch_layout.nb_channels, outFrame->ch_layout.nb_channels, inFrame->nb_samples, outSamples);

    // Allocate output frame buffer
    int ret = av_frame_get_buffer(outFrame, 0);
    if (ret < 0) {
      FFmpegUtils::printAvError("Audio Transformer: Could not allocate output frame buffer", ret);
      return false;
    }

    // Convert directly from input frame to output frame
    int convertedSampleCount =
      swr_convert(swrCtx, outFrame->data, outFrame->nb_samples, (const uint8_t **)inFrame->data, inFrame->nb_samples);

    if (convertedSampleCount < 0) {
      FFmpegUtils::printAvError("Audio Transformer: swr_convert failed with error", convertedSampleCount);
      return false;
    }

    // Update the actual number of samples converted
    outFrame->nb_samples = convertedSampleCount;

    VERYHIGH_MSG("Audio Transformer: Successfully converted %d samples", convertedSampleCount);
    return true;
  }

  void AudioTransformerNode::updateLongName() {
    std::stringstream ss;
    ss << "Audio Transformer (";
    if (targetSampleRate > 0) { ss << targetSampleRate << " Hz, "; }
    if (targetChannelCount > 0) { ss << targetChannelCount << " ch, "; }
    if (targetFormat != AV_SAMPLE_FMT_NONE) { ss << av_get_sample_fmt_name(targetFormat) << ", "; }
    if (channelMap.size() > 0) {
      ss << "split: [";
      ss << channelMap[0];
      for (int i = 1; i < channelMap.size(); i++) { ss << "," << channelMap[i]; }
      ss << "]";
    }
    ss << ")";
    longName = ss.str();
  }

  // Metrics methods

  bool AudioTransformerNode::updateChannelMap(const std::vector<int> & map) {
    channelMap = map;
    return true;
  }

  void AudioTransformerNode::setTargetSampleRate(int rate) {
    targetSampleRate = rate;
  }

  void AudioTransformerNode::setTargetChannelCount(int count) {
    targetChannelCount = count;
  }

  void AudioTransformerNode::setTargetFormat(AVSampleFormat format) {
    targetFormat = format;
  }

  void AudioTransformerNode::setTargetChannelLayout(uint64_t layout) {
    targetChannelLayout = layout;
  }

  int AudioTransformerNode::getTargetSampleRate() const {
    return targetSampleRate;
  }

  int AudioTransformerNode::getTargetChannelCount() const {
    return targetChannelCount;
  }

  AVSampleFormat AudioTransformerNode::getTargetFormat() const {
    return targetFormat;
  }

  const std::vector<int> & AudioTransformerNode::getChannelMap() const {
    return channelMap;
  }

  uint64_t AudioTransformerNode::getTargetChannelLayout() const {
    return targetChannelLayout;
  }

  void AudioTransformerNode::analyzeFormatRequirements(AVFrame *inFrame) {
    // Get input format properties
    AVSampleFormat inputFormat = static_cast<AVSampleFormat>(inFrame->format);
    int inputSampleRate = inFrame->sample_rate;
    int inputChannels = inFrame->ch_layout.nb_channels;

    // Determine what transformations are actually needed
    bool needsFormatConversion = (targetFormat != AV_SAMPLE_FMT_NONE && targetFormat != inputFormat);
    bool needsSampleRateConversion = (targetSampleRate > 0 && targetSampleRate != inputSampleRate);
    bool needsChannelConversion = (targetChannelCount > 0 && targetChannelCount != inputChannels);

    // Log the analysis
    VERYHIGH_MSG("Audio Transformer: Format analysis - Input: %s@%dHz %dch, Target: %s@%dHz %dch",
                 av_get_sample_fmt_name(inputFormat), inputSampleRate, inputChannels,
                 targetFormat != AV_SAMPLE_FMT_NONE ? av_get_sample_fmt_name(targetFormat) : "unchanged",
                 targetSampleRate > 0 ? targetSampleRate : inputSampleRate,
                 targetChannelCount > 0 ? targetChannelCount : inputChannels);

    if (needsFormatConversion) {
      INSANE_MSG("Audio Transformer: Format conversion needed: %s → %s", av_get_sample_fmt_name(inputFormat),
                 av_get_sample_fmt_name(targetFormat));
    }

    if (needsSampleRateConversion) {
      INSANE_MSG("Audio Transformer: Sample rate conversion needed: %d → %d Hz", inputSampleRate, targetSampleRate);
    }

    if (needsChannelConversion) {
      INSANE_MSG("Audio Transformer: Channel conversion needed: %d → %d channels", inputChannels, targetChannelCount);
    }

    // Update internal flags based on analysis
    needsResampling = needsFormatConversion || needsSampleRateConversion || needsChannelConversion;
  }

} // namespace FFmpeg
