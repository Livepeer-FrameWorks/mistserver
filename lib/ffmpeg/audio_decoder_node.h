#pragma once

#include "processing_node.h"

#include <string>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/opt.h>
}

namespace FFmpeg {

  /// @brief Node for decoding audio packets into frames
  class AudioDecoderNode : public ProcessingNode {
    private:
      // Private helper methods
      bool initCodecContext();
      void cleanupCodecContext();
      void updateLongName();
      void updateDecodingMetrics(uint64_t startTime, int sampleCount);
      void cleanupDecoder();

      // Timestamp synthesis state (milliseconds)
      int64_t lastPtsMs{AV_NOPTS_VALUE};
      int64_t anchorPtsMs{AV_NOPTS_VALUE};
      uint64_t totalDecodedSamples{0};

      const AVCodec * codec{0};
      AVCodecContext * codecCtx{0};
      AVCodecID codecId;
      AVFrame * frame{0};

    public:
      /// @brief Create an audio decoder node
      /// @param codecId Codec ID to use
      /// @param extradata Extra data for codec initialization
      /// @param rate Sample rate
      /// @param ch Number of channels
      /// @param depth Bit depth
      explicit AudioDecoderNode(AVCodecID codecId, const std::string & extradata = "", uint64_t rate = 0,
                                uint64_t ch = 0, uint64_t depth = 0);

      /// @brief Clean up resources
      ~AudioDecoderNode() override;

      /// @brief Initialize the decoder
      /// @return True if initialization succeeded
      bool init() override;

      /// @brief Set input packet at specified index
      /// @param packet Input packet
      /// @param idx Input index
      /// @return True if input was accepted
      bool setInput(void * packet, size_t idx) override;

      // Hardware acceleration methods (not applicable for audio)
      bool isHWAccelerated() const override { return false; }
      bool initHW(AVBufferRef *hwContext) override { return false; }
  };

} // namespace FFmpeg
