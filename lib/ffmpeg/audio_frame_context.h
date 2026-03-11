#pragma once

#include "../util.h"
#include "video_frame_context.h"

#include <chrono>
#include <memory>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/frame.h>
#include <libavutil/samplefmt.h>
}

namespace FFmpeg {

  /// @brief Manages audio frame data with sample format and channel layout support
  class AudioFrameContext : public Context {
    private:
      AVFrame *frame{nullptr};
      AVSampleFormat sampleFormat; ///< Audio sample format
      int channelCount; ///< Number of audio channels
      int sampleRate; ///< Sample rate in Hz
      bool ownsFrame; ///< Whether this context owns the frame
      bool isRawMode; ///< Whether this is a raw audio frame
      Util::ResizeablePointer rawData; ///< Raw audio data buffer
      std::chrono::steady_clock::time_point enqueueTime; // Time when frame was enqueued
      uint64_t bitDepth{0};
      size_t sourceNodeId{0};

    public:
      /// @brief Create an empty audio frame context
      AudioFrameContext();

      /// @brief Create an audio frame context from existing frame
      /// @param frame Audio frame
      /// @param sampleFmt Sample format
      /// @param channels Number of channels
      /// @param sampleRate Sample rate
      AudioFrameContext(AVFrame *frame, AVSampleFormat sampleFmt, int channels, int sampleRate);

      /// @brief Create an audio frame context from raw data
      /// @param data Raw audio data
      /// @param size Data size
      /// @param sampleFmt Sample format
      /// @param channels Number of channels
      /// @param sampleRate Sample rate
      AudioFrameContext(const uint8_t *data, size_t size, AVSampleFormat sampleFmt, int channels, int sampleRate);

      /// @brief Virtual destructor
      ~AudioFrameContext() override;

      // Implement Context interface
      const uint8_t *getData() const override;
      int getSize() const override;
      int64_t getPts() const override;

      /// @brief Get the sample format
      /// @return Sample format
      AVSampleFormat getSampleFormat() const { return sampleFormat; }

      /// @brief Get number of channels
      /// @return Channel count
      int getChannels() const { return channelCount; }

      /// @brief Get sample rate
      /// @return Sample rate in Hz
      int getSampleRate() const { return sampleRate; }

      /// @brief Get number of samples per channel
      /// @return Sample count
      int getSampleCount() const;

      /// @brief Get the underlying AVFrame
      /// @return Pointer to AVFrame
      AVFrame *getAVFrame() const { return frame; }

      /// @brief Reset frame context
      void reset();

      /// @brief Take ownership of the frame
      void takeOwnership() { ownsFrame = true; }

      /// @brief Release ownership of the frame
      void releaseOwnership() { ownsFrame = false; }

      /// @brief Check if this context owns the frame
      /// @return True if context owns the frame
      bool hasOwnership() const { return ownsFrame; }

      /// @brief Check if this context contains raw frame data
      /// @return True if context has raw data
      bool isRaw() const { return isRawMode; }

      /// @brief Get raw frame data
      /// @param data Output buffer data
      /// @param size Output buffer size
      /// @param fmt Output sample format
      /// @param channels Output channel count
      /// @param rate Output sample rate
      /// @return True if data was retrieved successfully
      bool getRawData(const uint8_t *& data, size_t & size, AVSampleFormat & fmt, int & channels, int & rate) const;

      /// @brief Create a raw frame from buffer data
      /// @param data Buffer data
      /// @param size Buffer size
      /// @param fmt Sample format
      /// @param channels Channel count
      /// @param rate Sample rate
      /// @return True if frame was created successfully
      bool createRawFrame(const uint8_t *data, size_t size, AVSampleFormat fmt, int channels, int rate);

      /// @brief Set frame enqueue time
      /// @param time Time point when frame was enqueued
      void setEnqueueTime(std::chrono::steady_clock::time_point time) { enqueueTime = time; }

      /// @brief Get bit depth
      /// @return Bit depth
      uint64_t getBitDepth() const { return bitDepth; }

      /// @brief Set bit depth
      /// @param depth Bit depth
      void setBitDepth(uint64_t depth) { bitDepth = depth; }

      /// @brief Check if this context has valid data
      /// @return True if context has valid data
      bool isValid() const { return frame != nullptr || (isRawMode && rawData.size() > 0); }

      /// @brief Get source node ID
      /// @return Source node ID
      size_t getSourceNodeId() const { return sourceNodeId; }

      /// @brief Set source node ID
      /// @param id Source node ID
      void setSourceNodeId(size_t id) { sourceNodeId = id; }

      /// @brief Create a deep copy of this frame context
      /// @return New frame context or nullptr on failure
      std::unique_ptr<AudioFrameContext> clone() const {
        if (!isValid()) return nullptr;

        if (isRawMode) {
          const uint8_t *data;
          size_t size;
          AVSampleFormat fmt;
          int channels, rate;
          if (!getRawData(data, size, fmt, channels, rate)) return nullptr;
          std::unique_ptr<AudioFrameContext> cloned(new AudioFrameContext());
          if (!cloned->createRawFrame(data, size, fmt, channels, rate)) return nullptr;
          cloned->setBitDepth(bitDepth);
          cloned->setSourceNodeId(sourceNodeId);
          return cloned;
        }

        if (!frame) return nullptr;
        AVFrame *clonedFrame = av_frame_clone(frame);
        if (!clonedFrame) return nullptr;
        std::unique_ptr<AudioFrameContext> cloned(new AudioFrameContext(clonedFrame, sampleFormat, channelCount, sampleRate));
        cloned->setBitDepth(bitDepth);
        cloned->setSourceNodeId(sourceNodeId);
        cloned->takeOwnership();
        return cloned;
      }
  };

  /// @brief Shared pointer type for audio frame context
  using AudioFrameContextPtr = std::shared_ptr<AudioFrameContext>;

} // namespace FFmpeg
