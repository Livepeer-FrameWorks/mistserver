#include "video_transformer_node.h"

#include "../defines.h"
#include "../timing.h"
#include "hw_context_manager.h"
#include "utils.h"
#include "video_frame_context.h"

#include <cstdio>
#include <cstring>
#include <sstream>
extern "C" {
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

static void allocPixels(AVFrame *f) {
  av_image_alloc(f->data, f->linesize, f->width, f->height, (AVPixelFormat)f->format, 32);
}

namespace {
  inline bool isHardwarePixelFormat(AVPixelFormat fmt) {
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(fmt);
    return desc && (desc->flags & AV_PIX_FMT_FLAG_HWACCEL);
  }

  inline AVPixelFormat getFrameHWFormat(const AVFrame *frame) {
    if (!frame) { return AV_PIX_FMT_NONE; }
    if (frame->hw_frames_ctx) {
      const AVHWFramesContext *hwCtx = reinterpret_cast<const AVHWFramesContext *>(frame->hw_frames_ctx->data);
      if (hwCtx) { return hwCtx->format; }
    }
    AVPixelFormat fmt = static_cast<AVPixelFormat>(frame->format);
    return isHardwarePixelFormat(fmt) ? fmt : AV_PIX_FMT_NONE;
  }

  inline AVPixelFormat getFrameSWFormat(const AVFrame *frame) {
    if (!frame) { return AV_PIX_FMT_NONE; }
    if (frame->hw_frames_ctx) {
      const AVHWFramesContext *hwCtx = reinterpret_cast<const AVHWFramesContext *>(frame->hw_frames_ctx->data);
      if (hwCtx) { return hwCtx->sw_format; }
    }
    return static_cast<AVPixelFormat>(frame->format);
  }
} // namespace

namespace FFmpeg {

  VideoTransformerNode::VideoTransformerNode(const std::string & resolution, int quality)
    : resolution(resolution), quality(quality) {
    updateLongName();
  }

  VideoTransformerNode::~VideoTransformerNode() {
    VERYHIGH_MSG("Video Transformer: Destroying VideoTransformerNode");
    if (outFrame) { av_frame_free(&outFrame); }
    if (swOutFrame) { av_frame_free(&swOutFrame); }
    if (swInFrame) { av_frame_free(&swInFrame); }
    if (swsCtx) {
      sws_freeContext(swsCtx);
      swsCtx = nullptr;
    }

    if (hwInfo.context) { av_buffer_unref(&hwInfo.context); }
    hwInfo.format = AV_PIX_FMT_NONE;
    hwInfo.swFormat = AV_PIX_FMT_NONE;
    hwInfo.isEnabled = false;
    hwPassthrough = false;
  }

  void VideoTransformerNode::setHardwareAcceleration(bool allowNvidia, bool allowQsv, bool allowVaapi,
                                                     bool allowMediaToolbox, bool allowSW, const std::string & devicePath) {
    std::lock_guard<std::mutex> lock(mutex);
    config.allowNvidia = allowNvidia;
    config.allowQsv = allowQsv;
    config.allowVaapi = allowVaapi;
    config.allowMediaToolbox = allowMediaToolbox;
    config.allowSW = allowSW;

    // Enable hardware acceleration if any hardware option is enabled
    hwInfo.isEnabled = allowNvidia || allowQsv || allowVaapi || allowMediaToolbox;

    MEDIUM_MSG("Video Transformer: Hardware acceleration configured - NVIDIA: %d, QSV: %d, VAAPI: "
               "%d, MediaToolbox: %d, SW: %d, HW enabled: %d, devicePath: %s",
               allowNvidia, allowQsv, allowVaapi, allowMediaToolbox, allowSW, hwInfo.isEnabled, devicePath.c_str());
  }

  bool VideoTransformerNode::configureHardwareAcceleration() {
    // Reuse existing configuration if already initialized
    if (hwInfo.context && hwInfo.isEnabled && hwInfo.format != AV_PIX_FMT_NONE && hwInfo.swFormat != AV_PIX_FMT_NONE) {
      VERYHIGH_MSG("Video Transformer: Reusing existing hardware configuration (%s -> %s)",
                   av_hwdevice_get_type_name(hwInfo.type), av_get_pix_fmt_name(hwInfo.format));
      if (hwInfo.deviceName.empty() && hwInfo.type != AV_HWDEVICE_TYPE_NONE) {
        hwInfo.deviceName = av_hwdevice_get_type_name(hwInfo.type);
      }
      return true;
    }

    // Clean up stale state before attempting reconfiguration
    hwInfo.cleanup();

    if (!config.allowNvidia && !config.allowQsv && !config.allowVaapi && !config.allowMediaToolbox && !config.allowSW) {
      MEDIUM_MSG("Video Transformer: All hardware acceleration disabled");
      return false;
    }

    auto & hwManager = HWContextManager::getInstance();

    VERYHIGH_MSG("Video Transformer: Configuring hardware acceleration");

    // Find available devices that can handle our transformation needs
    auto devices = hwManager.getAvailableDevices();
    for (const auto & device : devices) {
      const auto & deviceType = device.type;
      VERYHIGH_MSG("Video Transformer: Checking device type: %s", av_hwdevice_get_type_name(deviceType));

      // Try to initialize device
      const char *devicePath = device.devicePath.empty() ? nullptr : device.devicePath.c_str();
      AVBufferRef *hwContext = hwManager.initDevice(deviceType, devicePath);
      if (hwContext) {
        hwInfo.type = deviceType;
        hwInfo.context = hwContext;
        hwInfo.deviceName =
          device.devicePath.empty() ? av_hwdevice_get_type_name(deviceType) : device.devicePath.c_str();

        // Get supported formats for this device
        auto formats = hwManager.getSupportedPixelFormats(deviceType);
        if (!formats.empty()) {
          VERYHIGH_MSG("Video Transformer: Device %s supports %zu formats", av_hwdevice_get_type_name(deviceType),
                       formats.size());

          // Find the best compatible format
          AVPixelFormat bestHwFormat = AV_PIX_FMT_NONE;
          AVPixelFormat bestSwFormat = AV_PIX_FMT_NONE;

          // Look for hardware formats that we can use
          for (const auto & fmt : formats) {
            const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(fmt);
            if (desc && (desc->flags & AV_PIX_FMT_FLAG_HWACCEL)) {
              // This is a hardware format
              if (bestHwFormat == AV_PIX_FMT_NONE) {
                bestHwFormat = fmt;
                VERYHIGH_MSG("Video Transformer: Found hardware format: %s", av_get_pix_fmt_name(fmt));
              }
            } else if (desc && !(desc->flags & AV_PIX_FMT_FLAG_HWACCEL)) {
              // This is a software format
              if (bestSwFormat == AV_PIX_FMT_NONE) {
                bestSwFormat = fmt;
                VERYHIGH_MSG("Video Transformer: Found software format: %s", av_get_pix_fmt_name(fmt));
              }
            }
          }

          // Set the formats we found
          if (bestHwFormat != AV_PIX_FMT_NONE && bestSwFormat != AV_PIX_FMT_NONE) {
            // Hardware transformation path
            hwInfo.format = bestHwFormat;
            hwInfo.swFormat = bestSwFormat;
            MEDIUM_MSG("Video Transformer: Selected hardware transformation with device %s, format "
                       "%s (sw: %s)",
                       av_hwdevice_get_type_name(deviceType), av_get_pix_fmt_name(hwInfo.format),
                       av_get_pix_fmt_name(hwInfo.swFormat));
            hwInfo.isEnabled = true;
            return true;
          }
        }

        // Clean up if we couldn't find compatible formats
        av_buffer_unref(&hwContext);
      } else {
        VERYHIGH_MSG("Video Transformer: No hardware context found for device type: %s", av_hwdevice_get_type_name(deviceType));
      }
    }

    // No suitable hardware device found
    hwInfo.cleanup();
    return false;
  }

  bool VideoTransformerNode::supportsHWDevice(AVHWDeviceType type) const {
    switch (type) {
      case AV_HWDEVICE_TYPE_CUDA: return config.allowNvidia;
      case AV_HWDEVICE_TYPE_QSV: return config.allowQsv;
      case AV_HWDEVICE_TYPE_VAAPI: return config.allowVaapi;
      case AV_HWDEVICE_TYPE_VIDEOTOOLBOX: return config.allowMediaToolbox;
      default: return false;
    }
  }

  bool VideoTransformerNode::canProcessHWFormat(AVPixelFormat hwFormat) const {
    // Check if we can handle this hardware format
    switch (hwFormat) {
      case AV_PIX_FMT_CUDA:
      case AV_PIX_FMT_QSV:
      case AV_PIX_FMT_VAAPI:
      case AV_PIX_FMT_VIDEOTOOLBOX: return true;
      default: return false;
    }
  }

  bool VideoTransformerNode::initHW(AVBufferRef *hwContext) {
    if (!hwContext) {
      ERROR_MSG("Video Transformer: Invalid hardware context");
      return false;
    }

    auto & hwManager = HWContextManager::getInstance();

    // Get hardware frames context info
    AVHWFramesContext *framesCtx = (AVHWFramesContext *)hwContext->data;
    AVHWDeviceType deviceType = framesCtx->device_ctx->type;

    // Check if we support this device type
    if (!supportsHWDevice(deviceType)) {
      ERROR_MSG("Video Transformer: Unsupported hardware device type: %s", av_hwdevice_get_type_name(deviceType));
      return false;
    }

    // Get or create device context from HWContextManager
    AVBufferRef *deviceContext = hwManager.initDevice(deviceType);
    if (!deviceContext) {
      ERROR_MSG("Video Transformer: Failed to get device context from HWContextManager for type: %s",
                av_hwdevice_get_type_name(deviceType));
      return false;
    }

    // Store hardware context and device info
    hwInfo.context = av_buffer_ref(hwContext);
    hwInfo.type = deviceType;
    hwInfo.format = framesCtx->format;
    hwInfo.swFormat = framesCtx->sw_format;
    hwInfo.deviceName = av_hwdevice_get_type_name(deviceType);
    hwInfo.isEnabled = true;

    if (!hwInfo.context) {
      ERROR_MSG("Video Transformer: Failed to reference hardware context");
      hwInfo.cleanup();
      return false;
    }

    MEDIUM_MSG("Video Transformer: Successfully initialized hardware context for device: %s, format: "
               "%s (sw: %s)",
               hwInfo.deviceName.c_str(), av_get_pix_fmt_name(hwInfo.format), av_get_pix_fmt_name(hwInfo.swFormat));
    return true;
  }

  void VideoTransformerNode::setSharedHardwareContext(AVBufferRef *deviceCtx, AVPixelFormat hwFormat, AVPixelFormat swFormat) {
    std::lock_guard<std::mutex> lock(mutex);
    if (hwInfo.context) { av_buffer_unref(&hwInfo.context); }

    if (deviceCtx && hwFormat != AV_PIX_FMT_NONE && swFormat != AV_PIX_FMT_NONE) {
      hwInfo.context = av_buffer_ref(deviceCtx);
      hwInfo.format = hwFormat;
      hwInfo.swFormat = swFormat;
      hwInfo.isEnabled = true;
      hwInfo.type =
        hwInfo.context ? ((AVHWDeviceContext *)hwInfo.context->data)->type : AV_HWDEVICE_TYPE_NONE;
      if (hwInfo.type != AV_HWDEVICE_TYPE_NONE) {
        hwInfo.deviceName = av_hwdevice_get_type_name(hwInfo.type);
      }
    } else {
      hwInfo.cleanup();
    }
  }

  void VideoTransformerNode::updateLongName() {
    std::stringstream ss;
    ss << "Transformer " << av_get_pix_fmt_name(inFormat) << " -> [";
    bool first = true;
    for (auto & T : targetFormats) {
      if (!first) { ss << ", "; }
      ss << av_get_pix_fmt_name(T);
      first = false;
    }
    ss << "] (";
    ss << (scalerAlgorithm ? scalerAlgorithm : "default");
    if (!resolution.empty()) { ss << ", " << resolution; }
    ss << ")";
    longName = ss.str();
  }

  bool VideoTransformerNode::init() {
    VERYHIGH_MSG("Video Transformer: Initializing VideoTransformerNode");
    if (swsCtx) {
      sws_freeContext(swsCtx);
      swsCtx = nullptr;
    }
    outWidth = 0;
    outHeight = 0;
    hwPassthrough = false;
    // Hardware acceleration will be configured when we receive the first input frame
    return true;
  }

  bool VideoTransformerNode::setInput(void * inData, size_t idx) {
    VideoFrameContext * frame = (VideoFrameContext*)inData;
    if (!frame) {
      ERROR_MSG("Video Transformer: Attempted to set null input at index %zu", idx);
      return false;
    }

    // Configure output parameters based on input frame and transformation requirements
    configureOutputParameters(frame);
    AVFrame *inAVFrame = frame->getAVFrame();

    // Start processing time tracking
    uint64_t startTime = Util::getMicros();

    // Smart transformation logic: determine what actually needs to be done
    bool needsFormatChange = (targetFormats.size() && !targetFormats.count((AVPixelFormat)inAVFrame->format));
    bool needsResolutionChange =
      (outWidth > 0 && outHeight > 0 && (inAVFrame->width != outWidth || inAVFrame->height != outHeight));
    bool isHWToSWTransfer =
      frame->isHardwareFrame() && (!targetFormats.size() || targetFormats.count((AVPixelFormat)inAVFrame->format));

    // If hardware passthrough is enabled and no transformation is needed, do direct passthrough
    if (hwPassthrough && !needsResolutionChange && targetFormats.count((AVPixelFormat)inAVFrame->format)) {
      VERYHIGH_MSG("Video Transformer: Hardware passthrough - no transformation needed");

      totalProcessingTime.fetch_add(Util::getMicros(startTime));

      // Simple passthrough - just copy the input frame reference
      for (auto & cb : callbacks){cb(frame);}

      return true;
    }

    // If no actual transformation is needed, do a simple passthrough
    if (!needsFormatChange && !needsResolutionChange && !isHWToSWTransfer) {
      VERYHIGH_MSG("Video Transformer: No transformation needed, doing passthrough");

      totalProcessingTime.fetch_add(Util::getMicros(startTime));

      // Simple passthrough - just copy the input frame reference
      for (auto & cb : callbacks){cb(frame);}
      return true;
    }

    // If only HW-to-SW transfer is needed (no scaling or format conversion)
    if (isHWToSWTransfer && !needsResolutionChange && (!targetFormats.size() || targetFormats.count(frame->getSWFormat()))) {
      VERYHIGH_MSG("Video Transformer: Only HW-to-SW transfer needed, no scaling");

      // Create software frame with same dimensions and format as input
      AVPixelFormat swFormat = frame->getSWFormat();
      if (swFormat == AV_PIX_FMT_NONE) {
        // Fallback to common software format
        swFormat = AV_PIX_FMT_NV12;
      }

      // (Re)allocate frame if/as needed
      if (!outFrame || outFrame->width != inAVFrame->width || outFrame->height != inAVFrame->height || outFrame->format != swFormat){
        av_frame_free(&outFrame);
        outFrame = av_frame_alloc();
        if (!outFrame) {
          ERROR_MSG("Video Transformer: Failed to allocate frame");
          trackFrameDrop();
          return false;
        }
        outFrame->width = inAVFrame->width;
        outFrame->height = inAVFrame->height;
        outFrame->format = swFormat;
        allocPixels(outFrame);
      }

      // Transfer hardware frame to software frame
      int ret = av_hwframe_transfer_data(outFrame, inAVFrame, 0);
      if (ret < 0) {
        FFmpegUtils::printAvError("Video Transformer: Failed to transfer hardware input to software", ret);
        trackFrameDrop();
        return false;
      }

      // Copy frame properties
      av_frame_copy_props(outFrame, inAVFrame);

      // Create frame context
      VideoFrameContext outFrameCtx(outFrame);

      // Copy metadata from input frame
      outFrameCtx.setPts(frame->getPts());
      outFrameCtx.setDuration(frame->getDuration());
      outFrameCtx.setIsKeyFrame(frame->getIsKeyFrame());
      outFrameCtx.setSourceNodeId(getNodeId());
      outFrameCtx.setHWDeviceName("");
      outFrameCtx.setIsHWFrame(false);
      outFrameCtx.setFpks(frame->getFpks());

      totalProcessingTime.fetch_add(Util::getMicros(startTime));

      for (auto & cb : callbacks) { cb(&outFrameCtx); }
      return true;
    }

    // If we reach here, we need actual transformation (scaling and/or format conversion)
    VERYHIGH_MSG("Video Transformer: Transformation needed - format change: %d, resolution change: %d",
                 needsFormatChange, needsResolutionChange);


    // (Re)allocate frame if/as needed
    if (!outFrame || outFrame->width != outWidth || outFrame->height != outHeight || outFrame->format != outFormat){
      av_frame_free(&outFrame);
      outFrame = av_frame_alloc();
      if (!outFrame) {
        ERROR_MSG("Video Transformer: Failed to allocate frame");
        trackFrameDrop();
        return false;
      }
      outFrame->width = outWidth;
      outFrame->height = outHeight;
      outFrame->format = outFormat;
      allocPixels(outFrame);
      INFO_MSG("Allocated %dx%d %s output frame", outFrame->width, outFrame->height, av_get_pix_fmt_name(outFormat));
    }

    // Prepare output frame for transformation
    VideoFrameContext outFrameCtx(outFrame);

    bool success = false;
    AVFrame *outAVFrame = outFrameCtx.getAVFrame();

    // Check if we're doing hardware-to-hardware copy (same format, no scaling)
    if (frame->isHardwareFrame() && outFrameCtx.isHardwareFrame() && !needsFormatChange && !needsResolutionChange) {
      VERYHIGH_MSG("Video Transformer: Attempting hardware-to-hardware copy");
      if (tryDirectHWCopy(inAVFrame, outAVFrame)) {
        VERYHIGH_MSG("Video Transformer: Successfully used direct hardware copy");
        success = true;
      }
    }

    // Try hardware scaling if both frames are hardware and we need scaling/format conversion
    if (!success && hwInfo.isEnabled && frame->isHardwareFrame() && outFrameCtx.isHardwareFrame() &&
        (needsFormatChange || needsResolutionChange)) {
      VERYHIGH_MSG("Video Transformer: Attempting hardware scaling/conversion");
      if (tryHardwareScaling(inAVFrame, outAVFrame)) {
        VERYHIGH_MSG("Video Transformer: Successfully used hardware scaling");
        success = true;
      }
    }

    if (!success && hwInfo.isEnabled && frame->isHardwareFrame()) {
      disableHardware("hardware transformation failed");
    }

    // Fall back to software scaling if needed
    if (!success) {
      VERYHIGH_MSG("Video Transformer: Using software transformation");

      if (outFrameCtx.isHardwareFrame()) {
        HIGH_MSG("Video Transformer: Reconfiguring output for software fallback (outHW=%d)",
                 outFrameCtx.isHardwareFrame() ? 1 : 0);
        configureOutputParametersLocked(frame);
        /// \TODO prepare output frame
        VideoFrameContext outFrameCtx(outFrame);
        outAVFrame = outFrameCtx.getAVFrame();
      }

      // If input is hardware, transfer to software first
      if (frame->isHardwareFrame()) {
        VERYHIGH_MSG("Video Transformer: Converting hardware input to software for scaling");

        // Get software format from input frame
        AVPixelFormat swFormat = frame->getSWFormat();
        if (swFormat == AV_PIX_FMT_NONE) {
          swFormat = AV_PIX_FMT_NV12; // Fallback
        }

        // (Re)allocate frame if/as needed
        if (!swInFrame || swInFrame->width != inAVFrame->width || swInFrame->height != inAVFrame->height || swInFrame->format != swFormat){
          av_frame_free(&swInFrame);
          swInFrame = av_frame_alloc();
          if (!swInFrame) {
            ERROR_MSG("Video Transformer: Failed to allocate frame");
            trackFrameDrop();
            return false;
          }
          swInFrame->width = inAVFrame->width;
          swInFrame->height = inAVFrame->height;
          swInFrame->format = swFormat;
          allocPixels(swInFrame);
        }

        // Transfer hardware to software
        int ret = av_hwframe_transfer_data(swInFrame, inAVFrame, 0);
        if (ret < 0) {
          FFmpegUtils::printAvError("Video Transformer: Failed to transfer hardware input to software", ret);
          trackFrameDrop();
          return false;
        }

        // Copy properties
        av_frame_copy_props(swInFrame, inAVFrame);
      }

      // If output is hardware, we need a software intermediate frame
      if (outFrameCtx.isHardwareFrame()) {
        VERYHIGH_MSG("Video Transformer: Creating software intermediate frame for scaling");

        // Determine software format for output
        AVPixelFormat swOutFormat;
        // Use the software format from the hardware frame context
        if (outAVFrame->hw_frames_ctx) {
          AVHWFramesContext *hwFramesCtx = (AVHWFramesContext *)outAVFrame->hw_frames_ctx->data;
          swOutFormat = hwFramesCtx->sw_format;
        } else {
          swOutFormat = AV_PIX_FMT_NV12; // Fallback
        }

        // (Re)allocate frame if/as needed
        if (!swOutFrame || swOutFrame->width != outWidth || swOutFrame->height != outHeight || swOutFrame->format != swOutFormat){
          av_frame_free(&swOutFrame);
          swOutFrame = av_frame_alloc();
          if (!swOutFrame) {
            ERROR_MSG("Video Transformer: Failed to allocate frame");
            trackFrameDrop();
            return false;
          }
          swOutFrame->width = outWidth;
          swOutFrame->height = outHeight;
          swOutFrame->format = swOutFormat;
          allocPixels(swOutFrame);
        }
      }

      // Now do software scaling
      if (transformFrame(frame->isHardwareFrame() ? swInFrame : inAVFrame, outFrameCtx.isHardwareFrame() ? swOutFrame : outAVFrame)) {
        // If we used a software intermediate output frame, transfer it back to hardware
        if (outFrameCtx.isHardwareFrame()) {
          VERYHIGH_MSG("Video Transformer: Transferring software result back to hardware");
          if (!outAVFrame->hw_frames_ctx) {
            ERROR_MSG("Video Transformer: Cannot transfer software result to hardware - no hw_frames_ctx available");
            trackFrameDrop();
            return false;
          }
          int ret = av_hwframe_transfer_data(outAVFrame, swOutFrame, 0);
          if (ret < 0) {
            FFmpegUtils::printAvError("Video Transformer: Failed to transfer software result to hardware", ret);
            trackFrameDrop();
            return false;
          }
          // Copy properties to final output
          av_frame_copy_props(outAVFrame, swOutFrame);
        }

        success = true;
        VERYHIGH_MSG("Video Transformer: Software scaling completed successfully");
      }
    }

    if (!success) {
      ERROR_MSG("Video Transformer: All transformation methods failed");
      trackFrameDrop();
      return false;
    }

    // Copy metadata from input frame to context
    outFrameCtx.setPts(frame->getPts());
    outFrameCtx.setDuration(frame->getDuration());
    outFrameCtx.setFpks(frame->getFpks());
    outFrameCtx.setIsKeyFrame(frame->getIsKeyFrame());
    outFrameCtx.setSourceNodeId(getNodeId());
    if (outFrameCtx.isHardwareFrame()) {
      outFrameCtx.setHWDeviceName(frame->getHWDeviceName());
      outFrameCtx.setIsHWFrame(true);
    } else {
      outFrameCtx.setHWDeviceName("");
      outFrameCtx.setIsHWFrame(false);
    }

    totalProcessingTime.fetch_add(Util::getMicros(startTime));

    for (auto & cb : callbacks) { cb(&outFrameCtx); }
    return true;
  }

  void VideoTransformerNode::configureOutputParameters(VideoFrameContext * inFrame) {
    std::lock_guard<std::mutex> lock(mutex);
    configureOutputParametersLocked(inFrame);
  }

  // Caller must hold mutex.
  void VideoTransformerNode::configureOutputParametersLocked(VideoFrameContext * inFrame) {
    AVFrame *avFrame = inFrame->getAVFrame();
    if (!avFrame) {
      ERROR_MSG("Video Transformer: Invalid input frame");
      return;
    }

    // Detect input resolution change.
    // Output resolution is based on input resolution, so can only potentially change here as well.
    uint64_t inWidth = avFrame->width ? avFrame->width : inFrame->getWidth();
    uint64_t inHeight = avFrame->height ? avFrame->height : inFrame->getHeight();
    if (this->inWidth != inWidth || this->inHeight != inHeight) {
      INFO_MSG("Video Transformer: input size changed from %dx%d to %dx%d", this->inWidth, this->inHeight, (int)inWidth,
               (int)inHeight);
      this->inWidth = static_cast<int>(inWidth);
      this->inHeight = static_cast<int>(inHeight);
      uint64_t newOutW, newOutH;

      // Determine output dimensions
      if (!resolution.empty() && resolution != "keep source resolution") {
        size_t pos = resolution.find('x');
        if (pos != std::string::npos) {
          try {
            newOutW = std::stoul(resolution.substr(0, pos));
            newOutH = std::stoul(resolution.substr(pos + 1));
          } catch (...) {
            newOutW = inWidth;
            newOutH = inHeight;
          }
        } else {
          newOutW = inWidth;
          newOutH = inHeight;
        }
      } else {
        newOutW = inWidth;
        newOutH = inHeight;
      }
      if (newOutW != outWidth || newOutH != outHeight) {
        INFO_MSG("Video Transformer: output size changed from %" PRIu64 "x%" PRIu64 " to %" PRIu64 "x%" PRIu64,
                 this->outWidth, this->outHeight, newOutW, newOutH);
        outWidth = newOutW;
        outHeight = newOutH;
      }
    }

    AVPixelFormat inputFormat = static_cast<AVPixelFormat>(avFrame->format);
    AVPixelFormat inputHwFormat = getFrameHWFormat(avFrame);
    AVPixelFormat inputSwFormat = getFrameSWFormat(avFrame);
    AVPixelFormat fallbackFormat = preferredSwFormat;
    if (fallbackFormat == AV_PIX_FMT_NONE || isHardwarePixelFormat(fallbackFormat)) {
      fallbackFormat = (inputSwFormat != AV_PIX_FMT_NONE) ? inputSwFormat : AV_PIX_FMT_NV12;
    }
    bool hwCapable = inFrame->isHardwareFrame() && hwInfo.isEnabled && inputHwFormat != AV_PIX_FMT_NONE &&
      canProcessHWFormat(inputHwFormat);

    if (!hwCapable && inFrame->isHardwareFrame() && hwInfo.isEnabled && inputHwFormat == AV_PIX_FMT_NONE) {
      WARN_MSG("Video Transformer: Hardware input frame missing hw context description; using software path");
    }

    if (hwCapable && targetFormats.count(inputHwFormat)) {
      bool canPassthrough = (outWidth == inWidth && outHeight == inHeight);
      hwPassthrough = canPassthrough;
      HIGH_MSG("Video Transformer: configureOutputParametersLocked -> HW target=%s passthrough=%d",
               av_get_pix_fmt_name(inputHwFormat), hwPassthrough ? 1 : 0);
    } else {
      hwPassthrough = false;
      HIGH_MSG("Video Transformer: configureOutputParametersLocked -> SW target");
    }

    bool wantsHW = hwInfo.isEnabled && targetFormats.count(hwInfo.format);
    if (wantsHW) {
      if (!configureHardwareAcceleration()) {
        WARN_MSG("Video Transformer: Hardware acceleration configuration failed, using software fallback");
        if (hwInfo.context) {
          av_buffer_unref(&hwInfo.context);
          hwInfo.context = nullptr;
        }
        hwInfo.format = AV_PIX_FMT_NONE;
        hwInfo.swFormat = AV_PIX_FMT_NONE;
        hwInfo.isEnabled = false;
        hwPassthrough = false;
        wantsHW = false;
      }
    }

    if (!wantsHW) {
      hwPassthrough = false;
      HIGH_MSG("Video Transformer: configureOutputParametersLocked forcing SW target");
    }

    if (this->inFormat != inputFormat || !targetFormats.count(this->outFormat)){
      AVPixelFormat newOut = this->outFormat;
      if (!targetFormats.count(this->outFormat)){
        newOut = *targetFormats.begin();
      }
      INFO_MSG("Video Transformer: format changed from %s->%s to %s->%s", av_get_pix_fmt_name(this->inFormat),
               av_get_pix_fmt_name(this->outFormat), av_get_pix_fmt_name(inputFormat), av_get_pix_fmt_name(newOut));
      this->inFormat = inputFormat;
      this->outFormat = newOut;
      updateLongName();
    }

  }

  void VideoTransformerNode::setTargetFormat(const std::set<AVPixelFormat> & formats) {
    std::lock_guard<std::mutex> lock(mutex);
    targetFormats = formats;
    if (swsCtx) {
      sws_freeContext(swsCtx);
      swsCtx = nullptr;
    }
    updateLongName();
  }

  void VideoTransformerNode::setQuality(int q) {
    std::lock_guard<std::mutex> lock(mutex);
    quality = q;
  }

  void VideoTransformerNode::setResolution(const std::string & res) {
    std::lock_guard<std::mutex> lock(mutex);
    resolution = res;
    updateLongName();
  }

  void VideoTransformerNode::setScalerAlgorithm(const char *algorithm) {
    std::lock_guard<std::mutex> lock(mutex);
    scalerAlgorithm = algorithm;
    updateLongName();
  }

  void VideoTransformerNode::disableHardware(const char *reason) {
    if (!hwInfo.isEnabled) { return; }
    WARN_MSG("Video Transformer: Disabling hardware acceleration%s%s",
             reason ? " - " : "", reason ? reason : "");
    HIGH_MSG("Video Transformer: disableHardware invoked (reason=%s)", reason ? reason : "none");
    hwInfo.isEnabled = false;
  }

  bool VideoTransformerNode::tryHardwareScaling(AVFrame *inFrame, AVFrame *outFrame) {
    if (!inFrame || !outFrame) { return false; }

    if (!inFrame->hw_frames_ctx) {
      disableHardware("input frame lacks hw context");
      return false;
    }

    // Clean up existing filter graph
    if (filterGraph) {
      avfilter_graph_free(&filterGraph);
      filterGraph = nullptr;
      bufferSrcCtx = nullptr;
      bufferSinkCtx = nullptr;
      scaleCtx = nullptr;
    }

    // Create filter graph
    filterGraph = avfilter_graph_alloc();
    if (!filterGraph) {
      disableHardware("failed to allocate filter graph");
      return false;
    }

    AVPixelFormat hwFormat = getFrameHWFormat(inFrame);
    const char *scaleFilter = nullptr;
    switch (hwFormat) {
      case AV_PIX_FMT_VIDEOTOOLBOX: scaleFilter = "scale_vt"; break;
      case AV_PIX_FMT_CUDA: scaleFilter = "scale_cuda"; break;
      case AV_PIX_FMT_QSV: scaleFilter = "scale_qsv"; break;
      case AV_PIX_FMT_VAAPI: scaleFilter = "scale_vaapi"; break;
      default:
        disableHardware("unsupported hardware format for scaling");
        avfilter_graph_free(&filterGraph);
        filterGraph = nullptr;
        return false;
    }

    // Get filter references
    const AVFilter *bufferSrc = avfilter_get_by_name("buffer");
    const AVFilter *bufferSink = avfilter_get_by_name("buffersink");
    const AVFilter *scaleFilterRef = avfilter_get_by_name(scaleFilter);

    if (!bufferSrc || !bufferSink || !scaleFilterRef) {
      disableHardware("failed to resolve filter references");
      avfilter_graph_free(&filterGraph);
      filterGraph = nullptr;
      return false;
    }

    AVPixelFormat srcFormat = static_cast<AVPixelFormat>(inFrame->format);
    AVHWFramesContext *framesCtx = nullptr;
    if (inFrame->hw_frames_ctx) {
      framesCtx = reinterpret_cast<AVHWFramesContext *>(inFrame->hw_frames_ctx->data);
      if (srcFormat == AV_PIX_FMT_NONE && framesCtx) { srcFormat = framesCtx->format; }
    }
    if (srcFormat == AV_PIX_FMT_NONE && hwInfo.format != AV_PIX_FMT_NONE) { srcFormat = hwInfo.format; }
    if (srcFormat == AV_PIX_FMT_NONE) {
      disableHardware("unable to determine hardware input pixel format");
      avfilter_graph_free(&filterGraph);
      filterGraph = nullptr;
      return false;
    }

    AVPixelFormat swFormat = srcFormat;
    if (isHardwarePixelFormat(srcFormat)) {
      if (framesCtx && framesCtx->sw_format != AV_PIX_FMT_NONE) {
        swFormat = framesCtx->sw_format;
      } else if (hwInfo.swFormat != AV_PIX_FMT_NONE) {
        swFormat = hwInfo.swFormat;
      } else {
        disableHardware("hardware input lacks software format mapping");
        avfilter_graph_free(&filterGraph);
        filterGraph = nullptr;
        return false;
      }
    }

    const char *pixFmtName = av_get_pix_fmt_name(swFormat);
    if (!pixFmtName) {
      disableHardware("unable to resolve software pixel format for buffer source");
      avfilter_graph_free(&filterGraph);
      filterGraph = nullptr;
      return false;
    }

    char bufferArgs[256];
    AVRational tb = {1, 25};
    AVRational sar = inFrame->sample_aspect_ratio.num && inFrame->sample_aspect_ratio.den
      ? inFrame->sample_aspect_ratio
      : AVRational{1, 1};

    snprintf(bufferArgs, sizeof(bufferArgs),
             "video_size=%dx%d:pix_fmt=%s:time_base=%d/%d:pixel_aspect=%d/%d",
             inFrame->width, inFrame->height, pixFmtName,
             tb.num, tb.den, sar.num, sar.den);

    int ret = avfilter_graph_create_filter(&bufferSrcCtx, bufferSrc, "in", bufferArgs, nullptr, filterGraph);
    if (ret < 0) {
      FFmpegUtils::printAvError("Video Transformer: Failed to create buffer source", ret);
      disableHardware("buffer source creation failed");
      avfilter_graph_free(&filterGraph);
      filterGraph = nullptr;
      return false;
    }

    AVBufferSrcParameters *par = av_buffersrc_parameters_alloc();
    if (!par) {
      disableHardware("failed to allocate buffer source parameters");
      avfilter_graph_free(&filterGraph);
      bufferSrcCtx = nullptr;
      filterGraph = nullptr;
      return false;
    }
    par->width = inFrame->width;
    par->height = inFrame->height;
    par->format = srcFormat;
    par->time_base = AVRational{1, 25};
    par->sample_aspect_ratio = AVRational{inFrame->sample_aspect_ratio.num ? inFrame->sample_aspect_ratio.num : 1,
                                          inFrame->sample_aspect_ratio.den ? inFrame->sample_aspect_ratio.den : 1};
    if (inFrame->hw_frames_ctx) {
      par->hw_frames_ctx = av_buffer_ref(inFrame->hw_frames_ctx);
      if (!par->hw_frames_ctx) {
        disableHardware("failed to reference hw context for buffer source");
        avfilter_graph_free(&filterGraph);
        bufferSrcCtx = nullptr;
        filterGraph = nullptr;
        av_freep(&par);
        return false;
      }
    } else if (isHardwarePixelFormat(srcFormat)) {
      disableHardware("hardware pixel format without hw context");
      avfilter_graph_free(&filterGraph);
      bufferSrcCtx = nullptr;
      filterGraph = nullptr;
      av_freep(&par);
      return false;
    }

    ret = av_buffersrc_parameters_set(bufferSrcCtx, par);
    av_freep(&par);
    if (ret < 0) {
      FFmpegUtils::printAvError("Video Transformer: Failed to set buffer source parameters", ret);
      disableHardware("failed to configure buffer source");
      avfilter_graph_free(&filterGraph);
      bufferSrcCtx = nullptr;
      filterGraph = nullptr;
      return false;
    }

    // Create scale filter
    char scaleArgs[64];
    snprintf(scaleArgs, sizeof(scaleArgs), "w=%d:h=%d", (int)outWidth, (int)outHeight);
    ret = avfilter_graph_create_filter(&scaleCtx, scaleFilterRef, "scale", scaleArgs, nullptr, filterGraph);
    if (ret < 0) {
      FFmpegUtils::printAvError("Video Transformer: Failed to create scale filter", ret);
      disableHardware("failed to create scale filter");
      avfilter_graph_free(&filterGraph);
      filterGraph = nullptr;
      return false;
    }

    // Create buffer sink
    ret = avfilter_graph_create_filter(&bufferSinkCtx, bufferSink, "out", nullptr, nullptr, filterGraph);
    if (ret < 0) {
      FFmpegUtils::printAvError("Video Transformer: Failed to create buffer sink", ret);
      disableHardware("failed to create buffer sink");
      avfilter_graph_free(&filterGraph);
      filterGraph = nullptr;
      return false;
    }

    // Link filters: buffer -> scale -> buffersink
    ret = avfilter_link(bufferSrcCtx, 0, scaleCtx, 0);
    if (ret < 0) {
      FFmpegUtils::printAvError("Video Transformer: Failed to link buffer to scale", ret);
      disableHardware("failed to link filter graph (buffer->scale)");
      avfilter_graph_free(&filterGraph);
      filterGraph = nullptr;
      return false;
    }

    ret = avfilter_link(scaleCtx, 0, bufferSinkCtx, 0);
    if (ret < 0) {
      FFmpegUtils::printAvError("Video Transformer: Failed to link scale to sink", ret);
      disableHardware("failed to link filter graph (scale->sink)");
      avfilter_graph_free(&filterGraph);
      filterGraph = nullptr;
      return false;
    }

    // Configure the graph
    ret = avfilter_graph_config(filterGraph, nullptr);
    if (ret < 0) {
      FFmpegUtils::printAvError("Video Transformer: Failed to configure filter graph", ret);
      disableHardware("failed to configure filter graph");
      avfilter_graph_free(&filterGraph);
      filterGraph = nullptr;
      return false;
    }

    // Add frame to buffer source
    ret = av_buffersrc_add_frame_flags(bufferSrcCtx, inFrame, AV_BUFFERSRC_FLAG_KEEP_REF);
    if (ret < 0) {
      FFmpegUtils::printAvError("Video Transformer: Failed to add frame to buffer source", ret);
      disableHardware("failed to feed frame into hardware scaler");
      avfilter_graph_free(&filterGraph);
      filterGraph = nullptr;
      return false;
    }

    // Get filtered frame from buffer sink
    ret = av_buffersink_get_frame(bufferSinkCtx, outFrame);
    if (ret < 0) {
      FFmpegUtils::printAvError("Video Transformer: Failed to get frame from buffer sink", ret);
      disableHardware("hardware scaler produced no frame");
      avfilter_graph_free(&filterGraph);
      filterGraph = nullptr;
      return false;
    }

    if (!outFrame->hw_frames_ctx) {
      disableHardware("hardware scaler returned frame without hw context");
      avfilter_graph_free(&filterGraph);
      filterGraph = nullptr;
      av_frame_unref(outFrame);
      return false;
    }

    VERYHIGH_MSG("Video Transformer: Successfully performed %s hardware scaling", scaleFilter);
    return true;
  }

  bool VideoTransformerNode::tryDirectHWCopy(AVFrame *inFrame, AVFrame *outFrame) {
    // This is for when NO transformation is needed - just direct copy
    if (!inFrame || !outFrame) { return false; }

    // Simple direct copy - no transformation needed
    int ret = av_frame_copy(outFrame, inFrame);
    if (ret < 0) {
      FFmpegUtils::printAvError("Video Transformer: Failed to copy frame", ret);
      return false;
    }

    // Copy frame properties including PTS
    av_frame_copy_props(outFrame, inFrame);

    return true;
  }

  bool VideoTransformerNode::initScaler(AVFrame *inFrame) {
    if (!inFrame) {
      ERROR_MSG("Video Transformer: Invalid input frame for scaler initialization");
      return false;
    }

    // Clean up existing scaler
    if (swsCtx) {
      sws_freeContext(swsCtx);
      swsCtx = nullptr;
    }

    // For software scaling, we need to determine the correct output format
    // If we're doing software scaling, use a software format, not the hardware targetFormat
    AVPixelFormat outputFormat = this->outFormat;

    // If targetFormat is a hardware format but we're doing software scaling, use NV12
    if (outputFormat == AV_PIX_FMT_VIDEOTOOLBOX || outputFormat == AV_PIX_FMT_CUDA || outputFormat == AV_PIX_FMT_QSV ||
        outputFormat == AV_PIX_FMT_VAAPI) {
      outputFormat = AV_PIX_FMT_NV12;
    }

    // Create new scaler context
    swsCtx = sws_getContext(inFrame->width, inFrame->height, (AVPixelFormat)inFrame->format, outWidth, outHeight,
                            outputFormat, getScalingFlags(), nullptr, nullptr, nullptr);

    if (!swsCtx) {
      ERROR_MSG("Video Transformer: Failed to create scaler context");
      return false;
    }

    return true;
  }

  int VideoTransformerNode::getScalingFlags() const {
    int flags = 0;

    // Set quality flags based on quality setting
    if (quality <= 0) {
      flags |= SWS_POINT; // Fastest
    } else if (quality <= 10) {
      flags |= SWS_FAST_BILINEAR;
    } else if (quality <= 20) {
      flags |= SWS_BILINEAR;
    } else if (quality <= 30) {
      flags |= SWS_BICUBIC;
    } else {
      flags |= SWS_LANCZOS; // Best quality
    }

    // Add standard flags
    flags |= SWS_ACCURATE_RND;

    return flags;
  }

  bool VideoTransformerNode::transformFrame(AVFrame *inFrame, AVFrame *outFrame) {
    if (!inFrame || !outFrame) {
      ERROR_MSG("Video Transformer: Invalid frames for transform");
      return false;
    }

    // Initialize scaler if needed
    if (!initScaler(inFrame)) {
      ERROR_MSG("Video Transformer: Failed to initialize scaler");
      return false;
    }

    // Scale frame
    int ret = sws_scale(swsCtx, (const uint8_t *const *)inFrame->data, inFrame->linesize, 0, inFrame->height,
                        outFrame->data, outFrame->linesize);

    if (ret <= 0) {
      ERROR_MSG("Video Transformer: Failed to scale frame: %d", ret);
      return false;
    }

    av_frame_copy_props(outFrame, inFrame);

    return true;
  }

  bool VideoTransformerNode::isHWAccelerated() const {
    // If we have hardware passthrough, we're definitely hardware accelerated
    if (hwPassthrough) { return true; }

    // If hardware is not enabled, we're definitely software
    if (!hwInfo.isEnabled) { return false; }

    // Check if we're configured for hardware processing based on target format
    bool isHWFormat = targetFormats.count(AV_PIX_FMT_CUDA) || targetFormats.count(AV_PIX_FMT_QSV) ||
      targetFormats.count(AV_PIX_FMT_VAAPI) || targetFormats.count(AV_PIX_FMT_VIDEOTOOLBOX);

    return isHWFormat;
  }

} // namespace FFmpeg
