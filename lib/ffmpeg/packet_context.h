#pragma once

#include "video_frame_context.h"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/packet.h>
}
#include <memory>

namespace FFmpeg {

  /// @brief Context for managing AVPacket lifecycle and compressed video frames
  class PacketContext : public Context {
    public:
      /// @brief Create an empty packet context
      PacketContext();

      /// @brief Create a packet context from existing packet
      /// @param packet AVPacket to manage
      explicit PacketContext(AVPacket *packet);

      /// @brief Create a packet context from raw data
      /// @param data Raw packet data
      /// @param size Data size
      /// @param pts Presentation timestamp
      /// @param dts Decoding timestamp
      /// @param isKey Whether this is a keyframe
      PacketContext(const uint8_t *data, size_t size, int64_t pts = AV_NOPTS_VALUE, int64_t dts = AV_NOPTS_VALUE, bool isKey = false);

      /// @brief Destructor
      ~PacketContext() override;

      /// @brief Get the managed packet
      /// @return Pointer to AVPacket
      AVPacket *getPacket() const;

      /// @brief Get packet data
      /// @return Pointer to packet data
      const uint8_t *getData() const override;

      /// @brief Get packet size
      /// @return Size of packet data
      int getSize() const override;

      /// @brief Get packet timestamp
      /// @return Packet timestamp
      int64_t getPts() const override;

      /// @brief Get decoding timestamp
      /// @return Decoding timestamp
      int64_t getDts() const;

      /// @brief Get packet duration
      /// @return Packet duration
      int64_t getDuration() const;

      /// @brief Check if packet contains a keyframe
      /// @return True if packet contains a keyframe
      bool isKeyframe() const;

      /// @brief Get codec parameters
      /// @return Pointer to codec parameters
      const AVCodecParameters *getCodecParameters() const { return codecParams; }

      /// @brief Set codec parameters
      /// @param width Frame width
      /// @param height Frame height
      /// @param codecId Codec ID
      /// @param extradata Extra codec data (SPS/PPS)
      /// @param extradataSize Size of extra data
      void setCodecParameters(int width, int height, AVCodecID codecId, const std::string & extradata);

      /// @brief Set codec parameters
      /// @param params AVCodecParameters to set
      void setCodecParameters(const AVCodecParameters *params);

      /// @brief Set side data
      /// @param type Side data type
      /// @param data Side data buffer
      /// @param size Buffer size
      void setSideData(AVPacketSideDataType type, const uint8_t *data, size_t size);

      /// @brief Get side data
      /// @param type Side data type
      /// @param size Output buffer size
      /// @return Side data buffer or nullptr if not found
      const uint8_t *getSideData(AVPacketSideDataType type, size_t *size) const;

      /// @brief Create a new packet context from this one
      /// @return New packet context
      std::unique_ptr<PacketContext> clone() const;

      /// @brief Reset packet context
      void reset();

      /// @brief Get source node ID
      /// @return Source node ID
      size_t getSourceNodeId() const;

      /// @brief Set source node ID
      /// @param id Source node ID
      void setSourceNodeId(size_t id);

      /// @brief Get encoder node ID
      /// @return Encoder node ID that created this packet
      size_t getEncoderNodeId() const;

      /// @brief Set encoder node ID
      /// @param id Encoder node ID that created this packet
      void setEncoderNodeId(size_t id);

      /// @brief Get frame rate
      /// @return Frame rate
      uint64_t getFrameRate() const;

      // Getters
      const std::string & getCodecData() const;

      // Setters
      void setPts(int64_t pts);
      void setDts(int64_t dts);
      void setKeyframe(bool isKey);
      void setCodecData(const uint8_t *data, size_t size);

      // Format info
      struct FormatInfo {
          std::string codecName;
          uint64_t width;
          uint64_t height;
          uint64_t fpks; // Framerate in frames per thousand seconds
          bool isKeyframe;
          AVCodecID codecId;
          // Audio-specific fields
          uint64_t channels;
          uint64_t sampleRate;
          uint64_t bitDepth;
      };

      void setFormatInfo(const FormatInfo & info);
      const FormatInfo & getFormatInfo() const;

    private:
      AVPacket *packet; ///< Managed packet
      AVCodecParameters *codecParams; ///< Codec parameters
      size_t sourceNodeId; ///< Source node ID
      size_t encoderNodeId; ///< Encoder node ID
      FormatInfo formatInfo;
      std::string codecData;
  };

  /// @brief Shared pointer type for packet context
  using PacketContextPtr = std::shared_ptr<PacketContext>;

} // namespace FFmpeg
