#pragma once

#include "processing_node.h"
#include "video_frame_context.h"

#include <atomic>
#include <mutex>
#include <set>
#include <string>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavformat/avformat.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

namespace FFmpeg {

  /// @brief Node for transforming video frames (scaling, format conversion, etc)
  class VideoTransformerNode : public ProcessingNode {
    public:
      /// @brief Create a transformer node
      /// @param resolution Target resolution (WxH format)
      /// @param quality Quality setting (-1 to 100)
      explicit VideoTransformerNode(const std::string & resolution = "", int quality = 0);

      /// @brief Clean up resources
      ~VideoTransformerNode() override;

      /// @brief Initialize the transformer
      /// @return True if initialization succeeded
      bool init() override;

      // Hardware acceleration interface
      bool supportsHWDevice(AVHWDeviceType type) const;
      bool canProcessHWFormat(AVPixelFormat hwFormat) const;
      bool initHW(AVBufferRef *hwContext) override;
      bool isHWAccelerated() const override;

      // Format and dimension getters
      AVPixelFormat getInputFormat() const;
      AVPixelFormat getOutputFormat() const;
      void getInputDimensions(int & width, int & height) const;
      void getOutputDimensions(int & width, int & height) const;

      // Transformer-specific methods
      void setTargetFormat(const std::set<AVPixelFormat> & formats);
      void setResolution(const std::string & res);
      void setQuality(int q);
      void setScalerAlgorithm(const char *algorithm);

      // Configuration getters
      uint64_t getOutputWidth() const { return outWidth; }
      uint64_t getOutputHeight() const { return outHeight; }
      const std::string & getResolution() const { return resolution; }
      int getQuality() const { return quality; }
      const char *getScalerAlgorithm() const { return scalerAlgorithm; }
      size_t getMaxInputs() const { return 1; } // Transformer only accepts 1 input

      /// @brief Set hardware acceleration configuration
      /// @param allowNvidia Enable NVIDIA acceleration
      /// @param allowQsv Enable Intel QuickSync acceleration
      /// @param allowVaapi Enable VAAPI acceleration
      /// @param allowMediaToolbox Enable Apple MediaToolbox acceleration
      /// @param allowSW Enable software fallback
      /// @param devicePath Optional device path for the accelerator
      void setHardwareAcceleration(bool allowNvidia, bool allowQsv, bool allowVaapi, bool allowMediaToolbox, bool allowSW,
                                   const std::string & devicePath);

      /// @brief Configure hardware acceleration using HWContextManager
      /// @return True if hardware acceleration was configured successfully
      bool configureHardwareAcceleration() override;

      /// @brief Inject an existing hardware context negotiated upstream
      /// @param deviceCtx Shared device context (may be null)
      /// @param hwFormat Hardware pixel format
      /// @param swFormat Companion software pixel format
      void setSharedHardwareContext(AVBufferRef *deviceCtx, AVPixelFormat hwFormat, AVPixelFormat swFormat);

      /// @brief Set input frame for processing
      /// @param frame Input frame to process
      /// @param idx Input index (default 0)
      /// @return True if input was set successfully
      bool setInput(void *, size_t idx = 0) override;

    protected:
      /// @brief Configure output parameters based on input frame and transformation requirements
      /// @param inFrame Input frame to analyze
      void configureOutputParameters(VideoFrameContext * inFrame);

      // Hardware acceleration helpers
      bool tryHardwareScaling(AVFrame *inFrame, AVFrame *outFrame);
      bool tryDirectHWCopy(AVFrame *inFrame, AVFrame *outFrame);
      bool tryNvidiaScaling(AVFrame *inFrame, AVFrame *outFrame);
      bool tryQuickSyncScaling(AVFrame *inFrame, AVFrame *outFrame);
      bool tryVaapiScaling(AVFrame *inFrame, AVFrame *outFrame);
      bool tryVideoToolboxScaling(AVFrame *inFrame, AVFrame *outFrame);
      bool initScaler(AVFrame *inFrame);
      int getScalingFlags() const;
      void updateLongName();

    private:

      // Configuration
      std::string resolution;
      int quality{0};
      std::set<AVPixelFormat> targetFormats;
      AVPixelFormat preferredSwFormat{AV_PIX_FMT_NONE};
      const char *scalerAlgorithm{nullptr};

      // Hardware state
      bool hwPassthrough{false};

      // Scaler context
      SwsContext *swsCtx{nullptr};
      uint64_t outWidth{0};
      uint64_t outHeight{0};

      // Filter graph for hardware scaling
      AVFilterGraph *filterGraph{nullptr};
      AVFilterContext *bufferSrcCtx{nullptr};
      AVFilterContext *bufferSinkCtx{nullptr};
      AVFilterContext *scaleCtx{nullptr};

      // Frame tracking
      AVFrame * outFrame{0};
      AVFrame * swOutFrame{0};
      AVFrame * swInFrame{0};

      // Input format tracking
      AVPixelFormat inFormat{AV_PIX_FMT_NONE};
      AVPixelFormat outFormat{AV_PIX_FMT_NONE};
      int inWidth{0};
      int inHeight{0};

      // Thread safety
      mutable std::mutex mutex;
      std::atomic<bool> isReconfiguring{false};

      // Private helper methods
      bool transformFrame(AVFrame *inFrame, AVFrame *outFrame);
      void disableHardware(const char *reason);
      void configureOutputParametersLocked(VideoFrameContext * inFrame); // Requires caller to hold mutex
  };

} // namespace FFmpeg
