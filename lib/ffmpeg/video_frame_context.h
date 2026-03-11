#pragma once

#include "../util.h"

#include <memory>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixfmt.h>
}

namespace FFmpeg {

  /// @brief Base class for all contexts
  class Context {
    public:
      virtual ~Context() = default;
      virtual const uint8_t *getData() const = 0;
      virtual int getSize() const = 0;
      virtual int64_t getPts() const = 0;
  };

  /// @brief Manages a pair of hardware/software frames with automatic reference counting
  /// @note This class handles the lifecycle of frames and their hardware contexts
  class VideoFrameContext : public Context {
    private:
      // Raw frame data
      Util::ResizeablePointer rawData;
      uint32_t width{0};
      uint32_t height{0};
      uint64_t fpks{0};
      AVPixelFormat pixelFormat{AV_PIX_FMT_NONE};
      bool isRawMode{false};

      // FFmpeg frame
      AVFrame *frame{nullptr};
      AVBufferRef *hwContext{nullptr};
      AVFrame *swFrame{nullptr};
      AVPixelFormat hwFormat{AV_PIX_FMT_NONE};
      AVPixelFormat swFormat{AV_PIX_FMT_NONE};
      bool isHWFrame{false};

      // Frame metadata
      int64_t pts{AV_NOPTS_VALUE};
      int64_t duration{0};
      bool isKeyFrame{false};
      // Node info
      size_t sourceNodeId{0};
      std::string hwDeviceName;

    public:
      explicit VideoFrameContext(AVFrame *f = 0);

      /// @brief Destructor
      ~VideoFrameContext() override;

      // Prevent copying
      VideoFrameContext(const VideoFrameContext &) = delete;
      VideoFrameContext & operator=(const VideoFrameContext &) = delete;

      // Allow moving
      VideoFrameContext(VideoFrameContext &&) noexcept;
      VideoFrameContext & operator=(VideoFrameContext &&) noexcept;

      // Context interface implementation
      const uint8_t *getData() const override;
      int getSize() const override;
      int64_t getPts() const override { return pts; }

      // Frame operations
      bool transferFromHW();
      bool transferToHW(AVBufferRef *hwFramesCtx);
      void reset();
      std::unique_ptr<VideoFrameContext> clone() const;
      bool isHWCompatible(const VideoFrameContext & other) const;

      // Frame access
      AVFrame *getAVFrame() const { return frame; }
      AVFrame *getSWFrame() const { return swFrame ? swFrame : (isHWFrame ? nullptr : frame); }
      bool isHardwareFrame() const { return isHWFrame; }
      virtual AVPixelFormat getHWFormat() const;
      virtual AVPixelFormat getSWFormat() const;
      const std::string & getHWDeviceName() const { return hwDeviceName; }

      // Frame properties
      uint32_t getWidth() const { return width; }
      uint32_t getHeight() const { return height; }
      AVPixelFormat getPixelFormat() const { return pixelFormat; }
      uint64_t getFpks() const { return fpks; }
      int64_t getDuration() const { return duration; }
      bool getIsKeyFrame() const { return isKeyFrame; }
      size_t getSourceNodeId() const { return sourceNodeId; }

      // Raw frame operations
      bool createRawFrame(const uint8_t *data, size_t size, uint32_t w, uint32_t h, AVPixelFormat fmt, const int32_t *stride);
      bool getRawData(const uint8_t *& data, size_t & size, uint32_t & w, uint32_t & h, AVPixelFormat & fmt, int32_t *stride) const;

      // Frame type checks
      bool hasSWFrame() const { return swFrame != nullptr || (!isHWFrame && frame != nullptr); }

      // Setters
      void setWidth(uint32_t w) { width = w; }
      void setHeight(uint32_t h) { height = h; }
      void setPixelFormat(AVPixelFormat fmt) { pixelFormat = fmt; }
      void setFpks(uint64_t f) { fpks = f; }
      void setPts(int64_t p) {
        pts = p;
        if (frame) { frame->pts = p; }
      }
      void setDuration(int64_t d) {
        duration = d;
        if (frame) {
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 30, 100)
          frame->duration = d;
#else
          frame->pkt_duration = d;
#endif
        }
      }
      void setSourceNodeId(size_t id) { sourceNodeId = id; }
      void setIsHWFrame(bool hw) { isHWFrame = hw; }
      void setHWFormat(AVPixelFormat fmt) { hwFormat = fmt; }
      void setSWFormat(AVPixelFormat fmt) { swFormat = fmt; }
      void setHWDeviceName(const std::string & name) { hwDeviceName = name; }
      void setIsKeyFrame(bool key) { isKeyFrame = key; }
      void setHWContext(AVBufferRef *ctx) {
        if (hwContext) { av_buffer_unref(&hwContext); }
        hwContext = av_buffer_ref(ctx);
      }

  };

  using VideoFrameContextPtr = std::shared_ptr<VideoFrameContext>;

} // namespace FFmpeg
