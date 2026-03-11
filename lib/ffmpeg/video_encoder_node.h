#pragma once

#include "packet_context.h"
#include "processing_node.h"
#include "video_frame_context.h"

#include <set>
#include <string>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/codec.h>
#include <libavcodec/codec_id.h>
#include <libavutil/buffer.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
#include <libavutil/pixfmt.h>
}

namespace FFmpeg {

  class NodePipeline;

  /// @brief Node for encoding video frames into packets
  class VideoEncoderNode : public ProcessingNode {
    private:
      // Encoder-specific configuration
      std::string tune;
      std::string preset;
      std::string profile;
      std::string rateControlMode;
      std::string rateControlRcOption;
      int quality;
      bool multiPass;
      int targetCrf;
      int targetQp;
      int targetCq;
      uint64_t targetMaxBitrate;
      uint64_t targetVbvBuffer;

      // Configuration
      uint64_t gopSize;
      std::string resolution;
      AVPixelFormat targetFormat{AV_PIX_FMT_NONE};
      const char *scalerAlgorithm{nullptr};

      // Hardware-specific options
      std::string hwEncoderOptions;
      AVBufferRef *inputHwFramesCtx{nullptr};
      AVBufferRef *inputHwDeviceCtx{nullptr};

      const AVCodec * codec{0};
      AVCodecContext * codecCtx{0};
      std::string codecName;
      AVFrame * swFrame{0};
      AVPacket * packet{0};
      uint64_t startTime;

      // Timestamp normalization state
      int64_t lastPacketPtsMs{AV_NOPTS_VALUE};
      int64_t frameDurationMs{0};
      uint64_t lastKeyTime{0};


    protected:
      void updateLongName();
      bool encodeFrame(AVFrame *frame);
      void convertToAnnexB(std::shared_ptr<PacketContext> packetCtx);
      bool configureHardwareAcceleration() override;
      bool reconfigureEncoder();
      bool validateHWDevice(AVHWDeviceType deviceType, AVBufferRef *hwContext);
      bool validatePixelFormat(AVPixelFormat format) const;
      bool tryHardwareDevice(AVHWDeviceType deviceType, AVBufferRef *hwContext);
      bool handleHardwareToSoftwareFrame(AVFrame *hwFrame);

      /// @brief Initialize hardware acceleration
      /// @param hwContext Hardware context to use
      /// @return True if initialization succeeded
      bool initHW(AVBufferRef *hwContext) override;

      // Enhanced hardware support
      bool configureHWEncoderOptions();

      // Format handling
      bool setupPixelFormat();

      // Raw format handling
      bool processRawFormat(AVFrame *frame);
      bool isRawFormat() const;
      bool processRawFrame(VideoFrameContext & frame);

      // Rate control
      bool setupRateControl();

      // Profile configuration
      bool setupProfile();

      // FFmpeg 8 encoder compatibility shims (map generic knobs to per-encoder options)
      bool applyEncoderCompatOptions();

      // Codec initialization with input properties
      bool initializeCodec(uint64_t inputWidth, uint64_t inputHeight);

    public:
      /// @brief Create a video encoder node
      /// @param codecName Encoder name
      /// @param bitrate Target bitrate
      /// @param gopSize GOP size
      /// @param tune Encoder tuning
      /// @param preset Encoder preset
      /// @param quality Encoding quality
      VideoEncoderNode(const std::string & codecName, uint64_t bitrate = 2000000, uint64_t gopSize = 30,
                       const std::string & tune = "zerolatency", const std::string & preset = "faster", int quality = 20,
                       const std::string & rateControlMode = "bitrate", int crf = 0, int qp = 0, int cq = 0,
                       uint64_t maxBitrate = 0, uint64_t vbvBuffer = 0, const std::string & rcOption = "");

      /// @brief Clean up resources
      ~VideoEncoderNode();

      /// @brief Initialize the encoder
      /// @return True if initialization succeeded
      bool init() override;

      /// @brief Set input frame at specified index
      /// @param frame Input frame
      /// @param idx Input index
      /// @return True if input was accepted
      bool setInput(void * frame, size_t idx = 0) override;

      /// @brief Get current hardware device type
      /// @return Hardware device type
      AVHWDeviceType getHWDeviceType() const override;

      // Getters
      uint64_t getGopSize() const { return gopSize; }
      const std::string & getTune() const { return tune; }
      const std::string & getPreset() const { return preset; }
      int getQuality() const { return quality; }

      /// @brief Set hardware acceleration configuration
      /// @param allowNvidia Enable NVIDIA acceleration
      /// @param allowQsv Enable Intel QuickSync acceleration
      /// @param allowVaapi Enable VAAPI acceleration
      /// @param allowMediaToolbox Enable Apple MediaToolbox acceleration
      /// @param allowSW Enable software fallback
      /// @param devicePath Optional device path for the accelerator
      void setHardwareAcceleration(bool allowNvidia, bool allowQsv, bool allowVaapi, bool allowMediaToolbox, bool allowSW,
                                   const std::string & devicePath);

      /// @brief Inject a shared hardware context negotiated upstream
      void setSharedHardwareContext(AVBufferRef *deviceCtx, AVPixelFormat hwFormat, AVPixelFormat swFormat);

      // Encoder traits
      bool isHardwareCodec() const;
      std::set<AVPixelFormat> getPreferredSwPixelFormat() const;
      AVPixelFormat getPreferredHwPixelFormat() const;
  };

} // namespace FFmpeg
