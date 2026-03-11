#pragma once

#include "processing_node.h"
#include <vector>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

namespace FFmpeg {

  /// @brief Node for transforming audio frames (resampling, channel mixing, etc)
  class AudioTransformerNode : public ProcessingNode {
    private:
      // Audio configuration
      int targetSampleRate{0};
      int targetChannelCount{0};
      AVSampleFormat targetFormat{AV_SAMPLE_FMT_NONE};
      uint64_t targetChannelLayout{0};
      std::vector<int> channelMap;

      // Resampler state
      SwrContext *swrCtx{nullptr};
      bool needsResampling{false};

      // Private helper methods
      bool initResampler(AVFrame *inFrame);
      bool transformFrame(AVFrame *inFrame, AVFrame *outFrame);
      void updateLongName();

      // Format analysis and optimization methods
      void analyzeFormatRequirements(AVFrame *inFrame);

      // Metrics methods
      void updateProcessingMetrics(uint64_t startTime);

    public:
      /// @brief Create an audio transformer node
      /// @param sampleRate Target sample rate
      /// @param channelCount Target channel count
      /// @param format Target sample format
      /// @param split Whether to split channels
      explicit AudioTransformerNode(int sampleRate = 0, int channelCount = 0, AVSampleFormat format = AV_SAMPLE_FMT_NONE);

      /// @brief Clean up resources
      virtual ~AudioTransformerNode() override;

      /// @brief Initialize the transformer
      /// @return True if initialization succeeded
      virtual bool init() override;

      /// @brief Set input frame at specified index
      /// @param frame Input frame
      /// @param idx Input index
      /// @return True if input was accepted
      virtual bool setInput(void * frame, size_t idx) override;

      // Configuration methods
      void setTargetSampleRate(int rate);
      void setTargetChannelCount(int count);
      void setTargetFormat(AVSampleFormat format);
      bool updateChannelMap(const std::vector<int> & map);
      void setTargetChannelLayout(uint64_t layout);

      // Getters
      int getTargetSampleRate() const;
      int getTargetChannelCount() const;
      AVSampleFormat getTargetFormat() const;
      const std::vector<int> & getChannelMap() const;
      uint64_t getTargetChannelLayout() const;

      /// @brief Set target sample format
      /// @param format Target sample format
      void setTargetSampleFormat(AVSampleFormat format) { targetFormat = format; }

      /// @brief Get output sample format
      /// @return Current output sample format
      AVSampleFormat getOutputFormat() const { return targetFormat; }

      /// @brief Get output channel layout
      /// @return Current output channel layout
      uint64_t getOutputChannelLayout() const { return targetChannelLayout; }

      /// @brief Get output sample rate
      /// @return Current output sample rate
      int getOutputSampleRate() const { return targetSampleRate; }

      // Hardware acceleration methods (not applicable for audio)
      bool initHW(AVBufferRef *hwContext) override { return false; }
      virtual bool isHWAccelerated() const override { return false; }
  };

} // namespace FFmpeg
