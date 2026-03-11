#include "audio_frame_context.h"
extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}
#include <stdexcept>

namespace FFmpeg {

  AudioFrameContext::AudioFrameContext()
    : frame(av_frame_alloc()), sampleFormat(AV_SAMPLE_FMT_NONE), channelCount(0), sampleRate(0), ownsFrame(true),
      isRawMode(false) {
    VERYHIGH_MSG("Audio Frame Context: Creating empty audio frame context");
    if (!frame) {
      ERROR_MSG("Audio Frame Context: Failed to allocate frame");
      throw std::runtime_error("Audio Frame Context: Failed to allocate frame");
    }
  }

  AudioFrameContext::AudioFrameContext(AVFrame *f, AVSampleFormat sampleFmt, int channels, int sampleRate)
    : frame(f), sampleFormat(sampleFmt), channelCount(channels), sampleRate(sampleRate), ownsFrame(true), isRawMode(false) {
    if (!frame) {
      ERROR_MSG("Audio Frame Context: Null frame provided");
      throw std::runtime_error("Audio Frame Context: Null frame provided");
    }
    VERYHIGH_MSG("Audio Frame Context: Creating audio frame context (format=%d, channels=%d, rate=%d)", sampleFmt, channels, sampleRate);
  }

  AudioFrameContext::AudioFrameContext(const uint8_t *data, size_t size, AVSampleFormat sampleFmt, int channels, int sampleRate)
    : frame(nullptr), sampleFormat(sampleFmt), channelCount(channels), sampleRate(sampleRate), ownsFrame(true),
      isRawMode(true) {
    INFO_MSG("Audio Frame Context: Creating raw audio frame context (format=%d, channels=%d, rate=%d)", sampleFmt, channels, sampleRate);
    if (!createRawFrame(data, size, sampleFmt, channels, sampleRate)) {
      throw std::runtime_error("Audio Frame Context: Failed to create raw audio frame");
    }
  }

  AudioFrameContext::~AudioFrameContext() {
    VERYHIGH_MSG("Audio Frame Context: Destroying audio frame context");
    if (frame && ownsFrame) { av_frame_free(&frame); }
  }

  const uint8_t *AudioFrameContext::getData() const {
    if (isRawMode) { return reinterpret_cast<const uint8_t *>((const char *)rawData); }
    return frame ? frame->data[0] : nullptr;
  }

  int AudioFrameContext::getSize() const {
    if (isRawMode) { return rawData.size(); }
    if (!frame) { return 0; }
    return frame->linesize[0] * frame->nb_samples;
  }

  int64_t AudioFrameContext::getPts() const {
    return frame ? frame->pts : 0;
  }

  int AudioFrameContext::getSampleCount() const {
    if (isRawMode) { return rawData.size() / (av_get_bytes_per_sample(sampleFormat) * channelCount); }
    return frame ? frame->nb_samples : 0;
  }

  bool AudioFrameContext::createRawFrame(const uint8_t *data, size_t size, AVSampleFormat fmt, int channels, int rate) {
    if (!data || !size || fmt == AV_SAMPLE_FMT_NONE || channels <= 0 || rate <= 0) {
      ERROR_MSG("Audio Frame Context: Invalid raw audio frame parameters");
      return false;
    }

    // Store frame parameters
    sampleFormat = fmt;
    channelCount = channels;
    sampleRate = rate;

    // Allocate and copy raw data
    if (!rawData.assign(data, size)) {
      ERROR_MSG("Audio Frame Context: Failed to allocate raw audio buffer");
      return false;
    }
    isRawMode = true;

    return true;
  }

  bool AudioFrameContext::getRawData(const uint8_t *& data, size_t & size, AVSampleFormat & fmt, int & channels, int & rate) const {
    if (!isRawMode) { return false; }

    data = reinterpret_cast<const uint8_t *>((const char *)rawData);
    size = rawData.size();
    fmt = sampleFormat;
    channels = channelCount;
    rate = sampleRate;

    return true;
  }

  void AudioFrameContext::reset() {
    // Clean up existing frame if we own it
    if (ownsFrame && frame) { av_frame_free(&frame); }

    // Reset state
    frame = nullptr;
    ownsFrame = false;
    isRawMode = false;
    rawData.allocate(0);
    sampleFormat = AV_SAMPLE_FMT_NONE;
    channelCount = 0;
    sampleRate = 0;
    bitDepth = 0;
    sourceNodeId = 0;

    INFO_MSG("Audio Frame Context: Reset audio frame context");
  }

} // namespace FFmpeg
