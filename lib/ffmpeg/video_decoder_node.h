#pragma once

#include "node_pipeline.h"
#include "packet_context.h"
#include "processing_node.h"

#include <set>
#include <string>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/buffer.h>
#include <libavutil/hwcontext.h>
}

namespace FFmpeg {

  class VideoDecoderNode : public ProcessingNode {
    private:
      AVFrame * frame{0};
      const AVCodec * codec{0};
      AVCodecID codecId;
      AVCodecContext * codecCtx{0};
      std::string codecName;
      std::multiset<uint64_t> inTimes;

      /// @brief Update long name
      void updateLongName();

      /// @brief Decode a packet
      /// @param packet Packet to decode
      /// @return True if decoding succeeded
      bool decodePacket(AVPacket *packet);

      /// @brief Process a RAW format packet (no decoding needed)
      /// @param packet RAW packet to process
      /// @return True if processing succeeded
      bool processRawPacket(PacketContext * packet);

      /// @brief Configure hardware acceleration
      /// @return True if configuration succeeded
      virtual bool configureHardwareAcceleration() override;

      /// @brief Negotiate pixel format
      /// @param ctx Codec context
      /// @param formats Available formats
      /// @return Selected format
      AVPixelFormat negotiatePixelFormat(AVCodecContext *ctx, const AVPixelFormat *formats);

      /// @brief Format callback for hardware acceleration
      /// @param ctx Codec context
      /// @param formats Available formats
      /// @return Selected format
      static AVPixelFormat getFormatCallback(AVCodecContext *ctx, const AVPixelFormat *formats);

      /// @brief Reconfigure decoder
      /// @return True if reconfiguration succeeded
      bool reconfigureDecoder();

      /// @brief Validate packet
      /// @param packet Packet to validate
      /// @return True if packet is valid
      bool validatePacket(const PacketContext *packet) const;

      // Timestamp synthesis state (milliseconds)
      int64_t lastPtsMs{AV_NOPTS_VALUE};
      int64_t defaultFrameDurationMs{0};

    public:
      /// @brief Create a video decoder node
      /// @param codecName Name of codec to use
      explicit VideoDecoderNode(const std::string & codecName = "");

      /// @brief Clean up resources
      ~VideoDecoderNode() override;

      /// @brief Initialize the decoder
      /// @return True if initialization succeeded
      bool init() override;

      /// @brief Set input packet
      /// @param packet Input packet
      /// @param idx Input index
      /// @return True if input was set successfully
      bool setInput(void *, size_t idx) override;

      /// @brief Obtain a reference to the hardware device context (caller must unref)
      AVBufferRef *getHWDeviceContext() const;

      /// @brief Obtain a reference to the decoder's default hardware frames context (caller must unref)
      AVBufferRef *getHWFramesContext() const;

      /// @brief Get the hardware pixel format negotiated by the decoder
      AVPixelFormat getHWPixelFormat() const { return hwInfo.format; }

      /// @brief Get the software companion format negotiated by the decoder
      AVPixelFormat getSWPixelFormat() const { return hwInfo.swFormat; }

      /// @brief Initialize hardware acceleration
      /// @param hwContext Hardware context
      /// @return True if initialization succeeded
      bool initHW(AVBufferRef *hwContext) override;

      /// @brief Get hardware device type
      /// @return Hardware device type
      AVHWDeviceType getHWDeviceType() const override;

      /// @brief Check if hardware acceleration is enabled
      /// @return True if hardware acceleration is enabled
      bool isHWAccelerated() const override;

      /// @brief Set hardware acceleration configuration
      /// @param allowNvidia Enable NVIDIA acceleration
      /// @param allowQsv Enable Intel QuickSync acceleration
      /// @param allowVaapi Enable VAAPI acceleration
      /// @param allowMediaToolbox Enable Apple MediaToolbox acceleration
      /// @param allowSW Enable software fallback
      /// @param devicePath Optional device path for the accelerator
      void setHardwareAcceleration(bool allowNvidia, bool allowQsv, bool allowVaapi, bool allowMediaToolbox, bool allowSW,
                                   const std::string & devicePath);

      // Node ID management
      void setNodeId(size_t id) { nodeId = id; }
      size_t getNodeId() const { return nodeId; }
  };

} // namespace FFmpeg
