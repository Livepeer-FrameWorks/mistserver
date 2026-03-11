#pragma once

#include "audio_frame_context.h"
#include "processing_node.h"

#include <algorithm>
#include <vector>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
}

namespace FFmpeg {

  /// @brief Format requirements for audio encoding
  struct AudioFormatRequirements {
      std::vector<AVSampleFormat> supportedFormats; ///< Supported sample formats
      std::vector<AVChannelLayout> supportedLayouts; ///< Supported channel layouts
      std::vector<int> supportedSampleRates; ///< Supported sample rates
      std::vector<int> supportedChannelCounts;
      int minBitrate{0}; ///< Minimum bitrate
      int maxBitrate{0}; ///< Maximum bitrate
      int minQuality{0}; ///< Minimum quality value
      int maxQuality{0}; ///< Maximum quality value
      bool supportsVBR; ///< Whether VBR is supported

      /// @brief Check if a format is supported
      /// @param format Sample format to check
      /// @return True if format is supported
      bool isFormatSupported(AVSampleFormat format) const {
        return std::find(supportedFormats.begin(), supportedFormats.end(), format) != supportedFormats.end();
      }

      /// @brief Check if a bitrate is supported
      /// @param bitrate Bitrate to check
      /// @return True if bitrate is supported
      bool isBitrateSupported(int bitrate) const {
        return (minBitrate == 0 || bitrate >= minBitrate) && (maxBitrate == 0 || bitrate <= maxBitrate);
      }

      /// @brief Check if a quality value is supported
      /// @param quality Quality value to check
      /// @return True if quality is supported
      bool isQualitySupported(int quality) const { return quality >= minQuality && quality <= maxQuality; }
  };

  /// @brief Node for encoding audio frames into packets
  class AudioEncoderNode : public ProcessingNode {
    public:
      /// @brief Create an audio encoder node
      /// @param codecName Codec name to use
      /// @param sampleRate Target sample rate
      /// @param channelCount Target channel count
      /// @param bitrate Target bitrate
      /// @param bitDepth Target bit depth
      AudioEncoderNode(const char *codecName, uint64_t sampleRate, uint64_t channels, uint64_t bitrate, uint64_t bitDepth);

      /// @brief Virtual destructor
      ~AudioEncoderNode() override;

      /// @brief Initialize the encoder
      /// @return True if initialization succeeded
      bool init() override;

      /// @brief Set input frame at specified index
      /// @param frame Input frame
      /// @param idx Input index
      /// @return True if input was accepted
      bool setInput(void * frame, size_t idx = 0) override;

      /// @brief Get encoded packet
      /// @param[out] packet Output packet
      /// @return True if packet was received
      bool receivePacket(AVPacket *packet);

      /// @brief Set target quality
      /// @param quality Quality value (codec specific)
      void setQuality(int quality) { targetQuality = quality; }

      /// @brief Get target quality
      /// @return Quality value
      int getQuality() const { return targetQuality; }

      /// @brief Get target sample format
      /// @return Sample format
      AVSampleFormat getSampleFormat() const { return targetFormat; }

      /// @brief Get target sample rate
      /// @return Sample rate
      uint64_t getSampleRate() const { return sampleRate; }

      /// @brief Get the current bitrate
      /// @return Current bitrate in bits per second
      uint64_t getCurrentBitrate() const;

      /// @brief Get the expected frame size (number of samples per frame)
      /// @return Expected number of samples per frame, or 0 if not initialized
      int getExpectedFrameSize() const;

      bool initHW(AVBufferRef *hwContext) override { return false; }
      bool isHWAccelerated() const override { return false; }

    private:
      AVAudioFifo *audioBuffer{nullptr}; // Frame buffering for variable input sizes
      uint64_t totalSamplesProcessed{0}; // Total samples encoded for PTS calculation

      // Smart timestamp tracking for input/output sync
      int64_t lastInputTimestampMs{-1}; // Last input frame timestamp we processed
      int64_t lastOutputTimestampMs{-1}; // Last output packet timestamp we generated

      // Continuous timeline tracking
      int64_t baselineTimestampMs{-1}; // Reference timestamp for continuous timeline
      uint64_t baselineSamplePosition{0}; // Sample position at baseline timestamp
      uint64_t totalInputSamples{0}; // Total samples received from all input frames

      // Configuration
      std::string codecName;
      const AVCodec * codec{0};
      AVCodecContext * codecCtx{0};
      uint64_t targetBitDepth;
      int targetQuality{-1};

      // Format requirements
      AudioFormatRequirements formatRequirements;
      AVSampleFormat targetFormat{AV_SAMPLE_FMT_NONE};
      void initFormatRequirements();
      std::vector<AVSampleFormat> getSupportedSampleFormats() const;
      std::vector<AVChannelLayout> getSupportedChannelLayouts() const;
      std::vector<int> getSupportedSampleRates() const;

      // Private helper method
      void updateEncodingMetrics(uint64_t startTime);
      bool validateFormat(const AudioFrameContext *frame) const;
      void updateLongName();

      // Frame processing methods
      AVFrame * encoderFrame{0};
      bool processWithFrameBuffering(AVFrame *inputFrame);
      bool processDirectly(AVFrame *inputFrame);

      // Codec management
  };

} // namespace FFmpeg
