#include "video_encoder_node.h"

#include "hw_context_manager.h"
#include "../defines.h"
#include "../timing.h"
#include "node_pipeline.h"
#include "packet_context.h"
#include "utils.h"
#include "video_frame_context.h"

#include <cstring>
#include <algorithm>
#include <cctype>
#include <memory>
#include <mutex>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/codec.h>
#include <libavcodec/codec_id.h>
#include <libavutil/buffer.h>
#include <libavutil/channel_layout.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/log.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

namespace FFmpeg {

  VideoEncoderNode::VideoEncoderNode(const std::string & _codecName, uint64_t bitrate, uint64_t gopSize,
                                     const std::string & tune, const std::string & preset, int quality,
                                     const std::string & rateControlMode, int crf, int qp, int cq,
                                     uint64_t maxBitrate, uint64_t vbvBuffer, const std::string & rcOption)
    : ProcessingNode(), tune(tune), preset(preset),
      rateControlMode(rateControlMode), rateControlRcOption(rcOption), quality(quality), multiPass(false),
      targetCrf(std::max(0, crf)), targetQp(std::max(0, qp)), targetCq(std::max(0, cq)),
      targetMaxBitrate(maxBitrate), targetVbvBuffer(vbvBuffer), gopSize(gopSize) {
    if (this->rateControlMode.empty()) { this->rateControlMode = "cbr"; }
    auto toLowerInPlace = [](std::string & value) {
      std::transform(value.begin(), value.end(), value.begin(),
                     [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    };
    toLowerInPlace(this->rateControlMode);
    toLowerInPlace(this->rateControlRcOption);
    if (this->targetMaxBitrate && this->targetMaxBitrate < bitrate) { this->targetMaxBitrate = bitrate; }
    packet = av_packet_alloc();
    codecName = _codecName;
    updateLongName();
  }

  VideoEncoderNode::~VideoEncoderNode() {
    av_packet_free(&packet);
    // Clean up codec context
    if (codecCtx) {
      avcodec_free_context(&codecCtx);
      codecCtx = nullptr;
    }
    if (inputHwFramesCtx) {
      av_buffer_unref(&inputHwFramesCtx);
      inputHwFramesCtx = nullptr;
    }
    if (inputHwDeviceCtx) {
      av_buffer_unref(&inputHwDeviceCtx);
      inputHwDeviceCtx = nullptr;
    }
    if (swFrame) { av_frame_free(&swFrame); }

    // Clean up hardware resources
    if (hwInfo.context) {
      av_buffer_unref(&hwInfo.context);
      hwInfo.context = nullptr;
    }
  }

  void VideoEncoderNode::setHardwareAcceleration(bool allowNvidia, bool allowQsv, bool allowVaapi,
                                                 bool allowMediaToolbox, bool allowSW, const std::string & devicePath) {
    std::lock_guard<std::mutex> lock(mutex);

    // Update hardware acceleration configuration
    config.allowNvidia = allowNvidia;
    config.allowQsv = allowQsv;
    config.allowVaapi = allowVaapi;
    config.allowMediaToolbox = allowMediaToolbox;
    config.allowSW = allowSW;

    MEDIUM_MSG("Video Encoder: Updated encoder hardware acceleration: NVIDIA=%d, QSV=%d, VAAPI=%d, "
               "MediaToolbox=%d, SW=%d, devicePath=%s",
               allowNvidia, allowQsv, allowVaapi, allowMediaToolbox, allowSW, devicePath.c_str());
  }

  void VideoEncoderNode::setSharedHardwareContext(AVBufferRef *deviceCtx, AVPixelFormat hwFormat, AVPixelFormat swFormat) {
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

  bool VideoEncoderNode::configureHardwareAcceleration() {
    if (hwInfo.context && hwInfo.format != AV_PIX_FMT_NONE && hwInfo.swFormat != AV_PIX_FMT_NONE) {
      VERYHIGH_MSG("Video Encoder: Reusing existing hardware context (%s)",
                   av_hwdevice_get_type_name(hwInfo.type));
      hwInfo.isEnabled = true;
      if (hwInfo.deviceName.empty() && hwInfo.type != AV_HWDEVICE_TYPE_NONE) {
        hwInfo.deviceName = av_hwdevice_get_type_name(hwInfo.type);
      }
      return true;
    }

    if (!config.allowNvidia && !config.allowQsv && !config.allowVaapi && !config.allowMediaToolbox && !config.allowSW) {
      MEDIUM_MSG("Video Encoder: All hardware acceleration disabled");
      return false;
    }

    auto & hwManager = HWContextManager::getInstance();

    // Find available devices that can handle our encoding needs
    auto devices = hwManager.getAvailableDevices();
    for (const auto & device : devices) {
      const auto & deviceType = device.type;
      VERYHIGH_MSG("Video Encoder: Checking device type: %s", av_hwdevice_get_type_name(deviceType));
      // Try to initialize device
      const char *devicePath = device.devicePath.empty() ? nullptr : device.devicePath.c_str();
      AVBufferRef *hwContext = hwManager.initDevice(deviceType, devicePath);
      if (hwContext) {
        hwInfo.type = deviceType;
        hwInfo.context = hwContext;
        hwInfo.deviceName = device.devicePath.empty() ? device.name : device.devicePath;

        // Get supported formats for this device
        auto formats = hwManager.getSupportedPixelFormats(deviceType);
        if (!formats.empty()) {
          VERYHIGH_MSG("Video Encoder: Device %s supports %zu formats", av_hwdevice_get_type_name(deviceType), formats.size());

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
                VERYHIGH_MSG("Video Encoder: Found hardware format: %s", av_get_pix_fmt_name(fmt));
              }
            } else if (desc && !(desc->flags & AV_PIX_FMT_FLAG_HWACCEL)) {
              // This is a software format
              if (bestSwFormat == AV_PIX_FMT_NONE || fmt == AV_PIX_FMT_NV12) {
                bestSwFormat = fmt;
                VERYHIGH_MSG("Video Encoder: Found software format: %s", av_get_pix_fmt_name(fmt));
              }
            }
          }

          // Set the formats we found
          if (bestHwFormat != AV_PIX_FMT_NONE && bestSwFormat != AV_PIX_FMT_NONE) {
            // Hardware encoding path
            hwInfo.format = bestHwFormat;
            hwInfo.swFormat = bestSwFormat;
            INFO_MSG("Video Encoder: Selected hardware encoding with device %s, format %s (sw: %s)",
                     av_hwdevice_get_type_name(deviceType), av_get_pix_fmt_name(hwInfo.format),
                     av_get_pix_fmt_name(hwInfo.swFormat));
            hwInfo.isEnabled = true;
            return true;
          }
        }

        // Clean up if we couldn't find compatible formats
        av_buffer_unref(&hwContext);
      } else {
        INFO_MSG("Video Encoder: No hardware context found for device type: %s", av_hwdevice_get_type_name(deviceType));
      }
    }

    // No suitable hardware device found
    hwInfo.cleanup();
    return false;
  }

  bool VideoEncoderNode::validateHWDevice(AVHWDeviceType deviceType, AVBufferRef *hwContext) {
    if (!hwContext) {
      ERROR_MSG("Video Encoder: Invalid hardware context");
      return false;
    }

    // Get hardware frames context
    AVHWFramesContext *framesCtx = (AVHWFramesContext *)hwContext->data;
    if (!framesCtx || !framesCtx->device_ctx) {
      ERROR_MSG("Video Encoder: Invalid hardware frames context");
      return false;
    }

    // Verify device type matches
    if (framesCtx->device_ctx->type != deviceType) {
      ERROR_MSG("Video Encoder: Hardware device type mismatch - expected %s, got %s",
                av_hwdevice_get_type_name(deviceType), av_hwdevice_get_type_name(framesCtx->device_ctx->type));
      return false;
    }

    return true;
  }

  bool VideoEncoderNode::validatePixelFormat(AVPixelFormat format) const {
    if (!codec) {
      ERROR_MSG("Video Encoder: No codec available for format validation");
      return false;
    }

#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(61, 19, 100)
    // Use newer API if available
    int ret = avcodec_get_supported_config(nullptr, codec, AV_CODEC_CONFIG_PIX_FORMAT, 0, (const void **)&format, nullptr);
    return ret >= 0;
#else
    // Fallback for older FFmpeg versions
    // Check if codec has a list of supported pixel formats
    if (codec->pix_fmts) {
      for (int i = 0; codec->pix_fmts[i] != AV_PIX_FMT_NONE; i++) {
        if (codec->pix_fmts[i] == format) { return true; }
      }
      return false;
    }

    // If no explicit list, assume format is supported (conservative approach)
    return true;
#endif
  }

  bool VideoEncoderNode::setInput(void * inData, size_t idx) {
    VideoFrameContext * frame = (VideoFrameContext*)inData;
    if (!frame) {
      trackFrameDrop();
      ERROR_MSG("Video Encoder: Attempted to set null input frame at index %zu", idx);
      return false;
    }
    fpks = frame->getFpks();

    // Check if this is a RAW format encoder (passthrough)
    if (isRawFormat()) {
      DONTEVEN_MSG("Video Encoder: Processing RAW format %s - using passthrough", codecName.c_str());
      // Process RAW frame directly
      if (!processRawFrame(*frame)) {
        ERROR_MSG("Video Encoder: Failed to process RAW frame");
        trackFrameDrop();
        return false;
      }

      return true;
    }

    startTime = Util::getMicros();


    // Check if we need to initialize or reconfigure the codec
    AVFrame *avFrame = frame->getAVFrame();
    if (!avFrame || !frame->getWidth() || !frame->getHeight()) {
      ERROR_MSG("VideoEncoder: Invalid AVFrame in VideoFrameContext");
      return false;
    }

    // Check if codec needs initialization or reconfiguration
    bool needsInit = (!codecCtx);
    bool needsReconfig = false;

    // Check if input parameters changed
    if (codecCtx && (codecCtx->width != frame->getWidth() || codecCtx->height != frame->getHeight() || codecCtx->pix_fmt != frame->getPixelFormat())) {
      needsReconfig = true;
      INFO_MSG("VideoEncoder: Input parameters changed, reconfiguring codec");
    }

    if (needsInit || needsReconfig) {
      bool isHardwareEncoder = isHardwareCodec();
      // Determine pixel format and hardware context BEFORE initializing codec
      AVPixelFormat determinedFormat = frame->getPixelFormat(); // Default software format

      // Handle JPEG-specific format requirements
      if (!isHardwareEncoder && codecName == "JPEG") {
        determinedFormat = AV_PIX_FMT_YUVJ420P; // JPEG requires YUVJ420P
        INFO_MSG("Video Encoder: JPEG encoder requires YUVJ420P format");
      }

      if (frame->isHardwareFrame()) {
        if (avFrame->hw_frames_ctx) {
          // Get hardware frames context
          AVHWFramesContext *framesCtx = reinterpret_cast<AVHWFramesContext *>(avFrame->hw_frames_ctx->data);
          if (framesCtx && framesCtx->device_ctx) {
            // Keep references to the hardware frames/device context so we can configure the encoder properly.
            if (!inputHwFramesCtx || inputHwFramesCtx->data != avFrame->hw_frames_ctx->data) {
              av_buffer_unref(&inputHwFramesCtx);
              inputHwFramesCtx = av_buffer_ref(avFrame->hw_frames_ctx);
              if (!inputHwFramesCtx) {
                ERROR_MSG("Video Encoder: Failed to reference input hw_frames_ctx");
              }
            }
            if (framesCtx->device_ref && (!inputHwDeviceCtx || inputHwDeviceCtx->data != framesCtx->device_ref->data)) {
              av_buffer_unref(&inputHwDeviceCtx);
              inputHwDeviceCtx = av_buffer_ref(framesCtx->device_ref);
              if (!inputHwDeviceCtx) {
                ERROR_MSG("Video Encoder: Failed to reference input hardware device context");
              }
            }

            INFO_MSG("Video Encoder: Hardware device type: %s, format: %s",
                     av_hwdevice_get_type_name(framesCtx->device_ctx->type), av_get_pix_fmt_name((AVPixelFormat)avFrame->format));

            // Check if we're using a hardware encoder that can handle this hardware frame directly
            if (isHardwareEncoder) {
              // Hardware encoder can handle hardware frames directly
              determinedFormat = (AVPixelFormat)avFrame->format;
              INFO_MSG("Video Encoder: Hardware encoder %s accepting hardware frame format: %s", codec->name,
                       av_get_pix_fmt_name(determinedFormat));
            } else {
              // Software encoder - check if we can use hardware format
              bool hwDeviceValid = validateHWDevice(framesCtx->device_ctx->type, avFrame->hw_frames_ctx);
              bool pixelFormatValid = validatePixelFormat((AVPixelFormat)avFrame->format);

              INFO_MSG("Video Encoder: HW device validation: %s, pixel format validation: %s",
                       hwDeviceValid ? "PASS" : "FAIL", pixelFormatValid ? "PASS" : "FAIL");

              if (hwDeviceValid && pixelFormatValid) {
                determinedFormat = (AVPixelFormat)avFrame->format;
                INFO_MSG("Video Encoder: Will use hardware encoding with format: %s", av_get_pix_fmt_name(determinedFormat));
              } else {
                INFO_MSG("Video Encoder: Will use software encoding, converting hardware frames to "
                         "format: %s",
                         av_get_pix_fmt_name(determinedFormat));
              }
            }
          } else {
            WARN_MSG("Video Encoder: Invalid hardware frames context, using software encoding");
          }
        } else {
          WARN_MSG("Video Encoder: Hardware frame without hw_frames_ctx, using software encoding");
          av_buffer_unref(&inputHwFramesCtx);
          av_buffer_unref(&inputHwDeviceCtx);
          inputHwFramesCtx = nullptr;
          inputHwDeviceCtx = nullptr;
        }
      } else {
        av_buffer_unref(&inputHwFramesCtx);
        av_buffer_unref(&inputHwDeviceCtx);
        inputHwFramesCtx = nullptr;
        inputHwDeviceCtx = nullptr;
        if (isHardwareEncoder && hwInfo.swFormat != AV_PIX_FMT_NONE) {
          determinedFormat = hwInfo.swFormat;
          INFO_MSG("Video Encoder: Hardware encoder %s expects software frames in %s",
                   codec && codec->name ? codec->name : "unknown", av_get_pix_fmt_name(determinedFormat));
        } else {
          if (getPreferredSwPixelFormat().count(determinedFormat)){
            INFO_MSG("Video Encoder: Will use software encoding with format: %s", av_get_pix_fmt_name(determinedFormat));
          }else{
            FAIL_MSG("Pixel format not supported by encoder: %s", av_get_pix_fmt_name(determinedFormat));
          }
        }
      }

      // Store the determined format for initializeCodec to use
      targetFormat = determinedFormat;

      FFmpegUtils::setLogLevel(AV_LOG_DEBUG);
      if (!initializeCodec(frame->getWidth(), frame->getHeight())) {
        ERROR_MSG("VideoEncoder: Failed to initialize codec");
        FFmpegUtils::setLogLevel(AV_LOG_WARNING);
        return false;
      }
      FFmpegUtils::setLogLevel(AV_LOG_DEBUG);
    }

    if (!codecCtx && !isRawFormat()) {
      WARN_MSG("Video Encoder: Invalid encoder state");
      trackFrameDrop();
      return false;
    }

    // Regular encoding pipeline for non-RAW formats

    AVFrame *F = frame->getAVFrame();
    if (!F) {
      ERROR_MSG("Video Encoder: Null AVFrame");
      trackFrameDrop();
      return false;
    }

    // Smart frame handling: check for direct compatibility first
    bool canUseDirect = false;

    if (frame->isHardwareFrame()) {
      // Check if we're using a hardware encoder
      bool isHardwareEncoder = false;
      if (codec && codec->name) {
        std::string encoderName = codec->name;
        isHardwareEncoder =
          (encoderName.find("videotoolbox") != std::string::npos || encoderName.find("nvenc") != std::string::npos ||
           encoderName.find("qsv") != std::string::npos || encoderName.find("vaapi") != std::string::npos);
      }

      if (isHardwareEncoder) {
        // Hardware encoder can handle hardware frames directly
        if (!encodeFrame(F)) {
          ERROR_MSG("Video Encoder: Failed to encode hardware frame with hardware encoder");
          trackFrameDrop();
          return false;
        }
      } else {
        // Software encoder with hardware acceleration - check compatibility
        if (hwInfo.isEnabled && F->format == codecCtx->pix_fmt) {
          // Hardware formats match - check device compatibility
          if (F->hw_frames_ctx && hwInfo.context) {
            AVHWFramesContext *frameCtx = reinterpret_cast<AVHWFramesContext *>(F->hw_frames_ctx->data);
            AVHWDeviceContext *encoderCtx = reinterpret_cast<AVHWDeviceContext *>(hwInfo.context->data);

            if (frameCtx && encoderCtx && frameCtx->device_ctx->type == encoderCtx->type) {
              canUseDirect = true;
              VERYHIGH_MSG("Video Encoder: Direct hardware passthrough - compatible device and format");
            }
          }
        }

        if (!canUseDirect) {
          // Need to convert hardware frame to software
          VERYHIGH_MSG("Video Encoder: Converting hardware frame to software format");
          if (!handleHardwareToSoftwareFrame(F)) {
            ERROR_MSG("Video Encoder: Failed to convert hardware frame to software");
            trackFrameDrop();
            return false;
          }
        } else {
          // Direct hardware encoding with software encoder
          if (!encodeFrame(F)) {
            ERROR_MSG("Video Encoder: Failed to encode hardware frame");
            trackFrameDrop();
            return false;
          }
        }
      }
    } else {
      // Software frame - check if format matches encoder
      if (F->format == codecCtx->pix_fmt) {
        // Direct software encoding
        if (!encodeFrame(F)) {
          ERROR_MSG("Video Encoder: Failed to encode software frame");
          trackFrameDrop();
          return false;
        }
      } else {
        // Need format conversion
        WARN_MSG("Video Encoder: frame format mismatch %s != %s",
                     av_get_pix_fmt_name((AVPixelFormat)F->format), av_get_pix_fmt_name(codecCtx->pix_fmt));
        trackFrameDrop();
        return false;
      }
    }

    return true;
  }

  bool VideoEncoderNode::init() {
    std::lock_guard<std::mutex> lock(mutex);

    // Check if this is a RAW format - no codec initialization needed
    if (isRawFormat()) {
      INFO_MSG("Video Encoder: Initializing RAW format encoder for %s (passthrough mode)", codecName.c_str());
      return true;
    }

    const AVCodec *foundCodec = nullptr;

    // Find encoder using hardware-aware approach for all codecs
    auto & hwManager = HWContextManager::getInstance();
    auto devices = hwManager.getAvailableDevices();
    for (const auto & device : devices) {
      const auto & deviceType = device.type;
      const char *hwEncoderName = nullptr;

      if (codecName == "H264") {
        switch (deviceType) {
          case AV_HWDEVICE_TYPE_VIDEOTOOLBOX: hwEncoderName = "h264_videotoolbox"; break;
          case AV_HWDEVICE_TYPE_CUDA: hwEncoderName = "h264_nvenc"; break;
          case AV_HWDEVICE_TYPE_QSV: hwEncoderName = "h264_qsv"; break;
          case AV_HWDEVICE_TYPE_VAAPI: hwEncoderName = "h264_vaapi"; break;
          default: break;
        }
      } else if (codecName == "AV1") {
        switch (deviceType) {
          case AV_HWDEVICE_TYPE_VIDEOTOOLBOX: hwEncoderName = "av1_videotoolbox"; break;
          case AV_HWDEVICE_TYPE_CUDA: hwEncoderName = "av1_nvenc"; break;
          case AV_HWDEVICE_TYPE_QSV: hwEncoderName = "av1_qsv"; break;
          case AV_HWDEVICE_TYPE_VAAPI: hwEncoderName = "av1_vaapi"; break;
          default: break;
        }
      } else if (codecName == "HEVC" || codecName == "H265") {
        switch (deviceType) {
          case AV_HWDEVICE_TYPE_VIDEOTOOLBOX: hwEncoderName = "hevc_videotoolbox"; break;
          case AV_HWDEVICE_TYPE_CUDA: hwEncoderName = "hevc_nvenc"; break;
          case AV_HWDEVICE_TYPE_QSV: hwEncoderName = "hevc_qsv"; break;
          case AV_HWDEVICE_TYPE_VAAPI: hwEncoderName = "hevc_vaapi"; break;
          default: break;
        }
      } else if (codecName == "VP9") {
        switch (deviceType) {
          case AV_HWDEVICE_TYPE_VIDEOTOOLBOX: hwEncoderName = "vp9_videotoolbox"; break;
          case AV_HWDEVICE_TYPE_QSV: hwEncoderName = "vp9_qsv"; break;
          case AV_HWDEVICE_TYPE_VAAPI: hwEncoderName = "vp9_vaapi"; break;
          default: break;
        }
        /*
         // JPEG encoding disabled, though it could work, because... reasons.
      } else if (codecName == "JPEG") {
        switch (deviceType) {
          case AV_HWDEVICE_TYPE_VIDEOTOOLBOX: hwEncoderName = "mjpeg_videotoolbox"; break;
          case AV_HWDEVICE_TYPE_QSV: hwEncoderName = "mjpeg_qsv"; break;
          case AV_HWDEVICE_TYPE_VAAPI: hwEncoderName = "mjpeg_vaapi"; break;
          default: break;
        }
        */
      }

      if (hwEncoderName) {
        foundCodec = avcodec_find_encoder_by_name(hwEncoderName);
        if (foundCodec) {
          break;
        } else {
          INFO_MSG("Video Encoder: Hardware encoder %s not available", hwEncoderName);
        }
      }
    }

    // Fallback to software encoders if no hardware encoder found
    if (!foundCodec && config.allowSW) {
      if (codecName == "H264") {
        foundCodec = avcodec_find_encoder_by_name("libx264");
      } else if (codecName == "AV1") {
        // Try AV1 software encoders in order of preference
        foundCodec = avcodec_find_encoder_by_name("libaom-av1");
        if (!foundCodec) { foundCodec = avcodec_find_encoder_by_name("libsvtav1"); }
        if (!foundCodec) { foundCodec = avcodec_find_encoder_by_name("librav1e"); }
        if (!foundCodec) { foundCodec = avcodec_find_encoder(AV_CODEC_ID_AV1); }
      } else if (codecName == "HEVC" || codecName == "H265") {
        foundCodec = avcodec_find_encoder_by_name("libx265");
        if (!foundCodec) { foundCodec = avcodec_find_encoder(AV_CODEC_ID_HEVC); }
      } else if (codecName == "VP9") {
        foundCodec = avcodec_find_encoder_by_name("libvpx-vp9");
        if (!foundCodec) { foundCodec = avcodec_find_encoder(AV_CODEC_ID_VP9); }
      } else if (codecName == "JPEG") {
        foundCodec = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
      } else {
        // For other codecs, try by name
        foundCodec = avcodec_find_encoder_by_name(codecName.c_str());
      }
    }

    if (!foundCodec) {
      ERROR_MSG("Video Encoder: Could not find encoder for %s", codecName.c_str());
      return false;
    }
    codec = foundCodec;

    // Configure hardware acceleration only for codecs that support it and when using software
    // encoders Hardware encoders handle acceleration internally
    bool isHardwareEncoder = false;
    if (codec && codec->name) {
      std::string encoderName = codec->name;
      isHardwareEncoder =
        (encoderName.find("videotoolbox") != std::string::npos || encoderName.find("nvenc") != std::string::npos ||
         encoderName.find("qsv") != std::string::npos || encoderName.find("vaapi") != std::string::npos);
    }

    if (!isHardwareEncoder && (config.allowQsv || config.allowVaapi || config.allowNvidia || config.allowMediaToolbox)) {
      // Software encoder with hardware acceleration support
      if (!configureHardwareAcceleration()) {
        WARN_MSG("Video Encoder: Hardware acceleration not available, falling back to software");
      }
    } else if (isHardwareEncoder) {
      INFO_MSG("Video Encoder: Using hardware encoder %s - setting up hardware acceleration", codec->name);

      // For hardware encoders, we need to set up hardware acceleration properly
      if (!configureHardwareAcceleration()) {
        WARN_MSG("Video Encoder: Hardware acceleration setup failed for hardware encoder, trying without");
      }

      // Initialize hardware context if available
      if (hwInfo.isEnabled && hwInfo.context) {
        if (!initHW(hwInfo.context)) { WARN_MSG("Video Encoder: Hardware initialization failed"); }
      }

      // Configure hardware encoder options
      if (!configureHWEncoderOptions()) { WARN_MSG("Video Encoder: Failed to configure hardware encoder options"); }

      // For hardware encoders, mark as hardware enabled
      hwInfo.isEnabled = true;
    } else {
      INFO_MSG("Video Encoder: Hardware acceleration disabled, using software encoding");
    }

    INFO_MSG("Video Encoder: Using %s encoder: %s", codecName.c_str(), codec->name);
    return true;
  }

  bool VideoEncoderNode::setupPixelFormat() {
    // Use the targetFormat that was determined in setInput()
    if (targetFormat != AV_PIX_FMT_NONE) {
      codecCtx->pix_fmt = targetFormat;
      INFO_MSG("Video Encoder: Using predetermined format: %s", av_get_pix_fmt_name(codecCtx->pix_fmt));
      return true;
    }

    // Fallback logic if targetFormat wasn't set
    if (hwInfo.isEnabled) {
      codecCtx->pix_fmt = hwInfo.format;
      INFO_MSG("Video Encoder: Configured hardware encoding with format %s", av_get_pix_fmt_name(codecCtx->pix_fmt));
      return true;
    } else if (codecName == "JPEG") {
      // Software encoding - use codec-specific default formats
      // JPEG/MJPEG requires YUVJ420P format
      codecCtx->pix_fmt = AV_PIX_FMT_YUVJ420P;
      INFO_MSG("Video Encoder: Using JPEG-specific format: %s", av_get_pix_fmt_name(codecCtx->pix_fmt));
    }

    // Validate that the codec supports this format
    if (!validatePixelFormat(codecCtx->pix_fmt)) {
      // Try other common software formats
      const AVPixelFormat fallbackFormats[] = {AV_PIX_FMT_YUV422P, AV_PIX_FMT_YUV444P, AV_PIX_FMT_YUV420P10LE, AV_PIX_FMT_NONE};

      for (int i = 0; fallbackFormats[i] != AV_PIX_FMT_NONE; i++) {
        if (validatePixelFormat(fallbackFormats[i])) {
          codecCtx->pix_fmt = fallbackFormats[i];
          break;
        }
      }

      if (codecCtx->pix_fmt == AV_PIX_FMT_NV12 && !validatePixelFormat(codecCtx->pix_fmt)) {
        ERROR_MSG("Video Encoder: No supported software pixel format found");
        return false;
      }
    }

    INFO_MSG("Video Encoder: Configured software encoding with format %s", av_get_pix_fmt_name(codecCtx->pix_fmt));
    return true;
  }

bool VideoEncoderNode::setupRateControl() {
    if (!codecCtx) return false;

    bool isHardware = isHardwareCodec();
    std::string encoderName = codec && codec->name ? codec->name : "";

    auto hasOpt = [&](const char *opt) -> bool { return FFmpegUtils::hasOption(codecCtx->priv_data, opt); };
    auto setOptInt = [&](const char *opt, int64_t val) {
      if (!hasOpt(opt)) { return -1; }
      return av_opt_set_int(codecCtx->priv_data, opt, val, 0);
    };

    // Reset rate control fields before applying new strategy
    codecCtx->bit_rate = 0;
    codecCtx->rc_max_rate = 0;
    codecCtx->rc_min_rate = 0;
    codecCtx->rc_buffer_size = 0;

    auto clamp = [](int v, int minV, int maxV) { return std::max(minV, std::min(maxV, v)); };

    auto applyRcOption = [&]() {
      if (!rateControlRcOption.empty() && hasOpt("rc")) {
        int rcRet = av_opt_set(codecCtx->priv_data, "rc", rateControlRcOption.c_str(), 0);
        if (rcRet < 0) { FFmpegUtils::printAvError("Video Encoder: Failed to set rate control mode", rcRet); }
      }
    };

    if (!isHardware && codecName == "JPEG") {
      // JPEG uses quality-based encoding, not bitrate control
      codecCtx->bit_rate = 0;
      INFO_MSG("Video Encoder: Using quality-based rate control for JPEG");
      int jpeg_quality = (quality > 0 && quality <= 31) ? quality : 2;
      codecCtx->global_quality = FF_QP2LAMBDA * jpeg_quality;
      codecCtx->flags |= AV_CODEC_FLAG_QSCALE;
      INFO_MSG("Video Encoder: Using quality-based encoding for JPEG: quality=%d", jpeg_quality);
      return true;
    }

    auto applyBitrate = [&]() -> bool {
      if (bitrate == 0) { return false; }
      int64_t target = static_cast<int64_t>(bitrate);
      codecCtx->bit_rate = target;
      int64_t maxRate = targetMaxBitrate ? static_cast<int64_t>(targetMaxBitrate) : target;
      if (maxRate < target) { maxRate = target; }
      codecCtx->rc_max_rate = maxRate;
      codecCtx->rc_min_rate = target;
      int64_t bufferSize =
        targetVbvBuffer ? static_cast<int64_t>(targetVbvBuffer) : static_cast<int64_t>(target * 2);
      codecCtx->rc_buffer_size = bufferSize;
      INFO_MSG("Video Encoder: Rate control bitrate=%" PRIu64 " bps (max=%" PRIu64 ", vbv=%" PRIu64 ")",
               bitrate, static_cast<uint64_t>(maxRate), static_cast<uint64_t>(bufferSize));

      if (isHardware) {
        setOptInt("bitrate", target);
        if (targetMaxBitrate && hasOpt("max_bitrate")) { setOptInt("max_bitrate", maxRate); }
        if (bufferSize > 0) {
          if (hasOpt("vbv_buffer_size")) { setOptInt("vbv_buffer_size", bufferSize); }
          if (hasOpt("vbv_buffer")) { setOptInt("vbv_buffer", bufferSize); }
        }
      } else if (hasOpt("b")) {
        setOptInt("b", target);
      }

      applyRcOption();
      return true;
    };

    auto applyCrf = [&]() -> bool {
      int crfValue = quality;
      if (crfValue <= 0) { return false; }

      if (encoderName == "libx264" || encoderName == "libx265") { crfValue = clamp(crfValue, 0, 51); }
      else if (encoderName.find("libaom") != std::string::npos || encoderName == "libvpx-vp9") {
        crfValue = clamp(crfValue, 0, 63);
      }

      int ret = setOptInt("crf", crfValue);

      codecCtx->rc_min_rate = 0;
      if (targetMaxBitrate){
        codecCtx->rc_max_rate = targetMaxBitrate;
      }


      if (ret < 0 && encoderName == "libx265") {
        std::string param = "crf=" + std::to_string(crfValue);
        ret = av_opt_set(codecCtx->priv_data, "x265-params", param.c_str(), 0);
      }
      if (ret < 0 && encoderName.find("libaom") != std::string::npos) {
        ret = setOptInt("cq-level", crfValue);
      }
      if (ret >= 0) {
        INFO_MSG("Video Encoder: Rate control CRF=%d", crfValue);
        applyRcOption();
        return true;
      }

      WARN_MSG("Video Encoder: Failed to apply CRF=%d (encoder=%s)", crfValue, encoderName.c_str());
      return false;
    };

    std::string mode = rateControlMode.empty() ? "cbr" : rateControlMode;
    if (mode == "cbr" || mode == "vbr") {
      if (!applyBitrate()) {
        INFO_MSG("Video Encoder: Bitrate mode requested but bitrate unset; keeping encoder defaults");
      }
      return true;
    }

    if (mode == "cq") {
      if (applyCrf()) { return true; }
      if (applyBitrate()) { return true; }
      INFO_MSG("Video Encoder: CRF mode unsupported by encoder; using defaults");
      return true;
    }

    WARN_MSG("Video Encoder: Unknown rate control mode '%s'; using encoder defaults", mode.c_str());
    return true;
  }

  bool VideoEncoderNode::setupProfile() {
    if (!codecCtx || !codec) return false;

    // Set profile if specified
    if (!profile.empty()) {
      int ret = av_opt_set(codecCtx->priv_data, "profile", profile.c_str(), 0);
      if (ret < 0) {
        FFmpegUtils::printAvError("Video Encoder: Failed to set profile", ret);
        return false;
      }
    }

    return true;
  }

  // Map generic tune/preset/quality to encoder-specific FFmpeg 8 options.
  bool VideoEncoderNode::applyEncoderCompatOptions() {
    if (!codecCtx || !codec || !codec->name) { return true; }

    const char *name = codec->name;
    void *priv = codecCtx->priv_data;

    // Ensure forced keyframes are IDRs
    FFmpegUtils::setOption(priv, "forced_idr", "1", 0);
    FFmpegUtils::setOption(priv, "forced-idr", "1", 0);

    // (no-op) keep function minimal: avoid unused helper warnings

    std::string encName = name;
    std::transform(encName.begin(), encName.end(), encName.begin(), ::tolower);

    // NVIDIA NVENC encoders
    if (encName.find("nvenc") != std::string::npos) {
      // Map common presets to NVENC p1..p7 (p1=slowest/best, p7=fastest)
      std::string nvPreset;
      if (preset == "ultrafast")
        nvPreset = "p7";
      else if (preset == "superfast")
        nvPreset = "p6";
      else if (preset == "veryfast")
        nvPreset = "p5";
      else if (preset == "faster")
        nvPreset = "p4";
      else if (preset == "fast")
        nvPreset = "p3";
      else if (preset == "medium" || preset.empty())
        nvPreset = "p2";
      else if (preset == "slow" || preset == "slower" || preset == "veryslow")
        nvPreset = "p1";

      if (tune == "zerolatency") {
        // Low-latency tune on NVENC
        FFmpegUtils::setOption(priv, "tune", "ll", 0); // ll = low-latency
        FFmpegUtils::setOption(priv, "preset", "ll", 0);
        FFmpegUtils::setOptionInt(priv, "rc-lookahead", 0, 0);
        FFmpegUtils::setOptionInt(priv, "zerolatency", 1, 0);
      } else if (tune == "zerolatency-lq") {
        FFmpegUtils::setOption(priv, "preset", "llhp", 0);
        FFmpegUtils::setOption(priv, "tune", "ull", 0);
        FFmpegUtils::setOptionInt(priv, "rc-lookahead", 0, 0);
        FFmpegUtils::setOptionInt(priv, "zerolatency", 1, 0);
      } else if (tune == "zerolatency-hq") {
        FFmpegUtils::setOption(priv, "preset", "llhq", 0);
        FFmpegUtils::setOption(priv, "tune", "ull", 0);
        FFmpegUtils::setOptionInt(priv, "rc-lookahead", 0, 0);
        FFmpegUtils::setOptionInt(priv, "zerolatency", 1, 0);
      } else {
        if (!nvPreset.empty()) { FFmpegUtils::setOption(priv, "preset", nvPreset.c_str(), 0); }
      }

      // If a quality knob is provided and encoder supports CQ
      if (quality > 0) {
        // Map 1..31 approx to 0..51 scale
        int cq = std::max(0, std::min(51, quality * 2));
        FFmpegUtils::setOptionInt(priv, "cq", cq, 0);
        // Prefer VBR for CQ
        FFmpegUtils::setOption(priv, "rc", "vbr", 0);
      }
      return true;
    }

    if (tune == "zerolatency-lq") { tune = "zerolatency"; }
    if (tune == "zerolatency-hq") { tune = "zerolatency"; }

    // Common zerolatency hints
    if (tune == "zerolatency") {
      if (FFmpegUtils::hasOption(priv, "rc-lookahead")) { FFmpegUtils::setOptionInt(priv, "rc-lookahead", 0, 0); }
      if (FFmpegUtils::hasOption(priv, "look_ahead")) { FFmpegUtils::setOptionInt(priv, "look_ahead", 0, 0); }
      if (FFmpegUtils::hasOption(priv, "async_depth")) { FFmpegUtils::setOptionInt(priv, "async_depth", 1, 0); }
      if (FFmpegUtils::hasOption(priv, "low_power")) { FFmpegUtils::setOptionInt(priv, "low_power", 1, 0); }
    }

    // Apple VideoToolbox encoders
    if (encName.find("videotoolbox") != std::string::npos) {
      if (tune == "zerolatency") {
        if (FFmpegUtils::hasOption(priv, "realtime")) { FFmpegUtils::setOptionInt(priv, "realtime", 1, 0); }
        if (FFmpegUtils::hasOption(priv, "max_frame_delay_count")) {
          FFmpegUtils::setOptionInt(priv, "max_frame_delay_count", 1, 0);
        }
      }
      // Prefer hardware path only
      if (FFmpegUtils::hasOption(priv, "allow_sw")) { FFmpegUtils::setOptionInt(priv, "allow_sw", 0, 0); }
      if (FFmpegUtils::hasOption(priv, "annexb")) { FFmpegUtils::setOptionInt(priv, "annexb", 1, 0); }
      return true;
    }

    // Intel QuickSync encoders
    if (encName.find("_qsv") != std::string::npos) {
      if (quality > 0) {
        // QSV ICQ uses global_quality scale; pass through if available
        FFmpegUtils::setOptionInt(priv, "global_quality", quality, 0);
      }
      return true;
    }

    // VAAPI encoders
    if (encName.find("_vaapi") != std::string::npos) {
      if (tune == "zerolatency") { FFmpegUtils::setOptionInt(priv, "low_power", 1, 0); }
      // Pick sensible rc mode when bitrate is set or not
      if (bitrate > 0) {
        FFmpegUtils::setOption(priv, "rc_mode", "cbr", 0);
      } else if (quality > 0) {
        FFmpegUtils::setOption(priv, "rc_mode", "qp", 0);
        int qp = std::max(0, std::min(51, quality * 2));
        FFmpegUtils::setOptionInt(priv, "qp", qp, 0);
      }
      return true;
    }

    return true;
  }

  bool VideoEncoderNode::configureHWEncoderOptions() {
    if (hwEncoderOptions.empty() || getHWDeviceType() == AV_HWDEVICE_TYPE_NONE) { return true; }

    // Parse options string
    AVDictionary *opts = nullptr;
    int ret = av_dict_parse_string(&opts, hwEncoderOptions.c_str(), "=", ",", 0);
    if (ret < 0) {
      FFmpegUtils::printAvError("Video Encoder: Failed to parse hardware encoder options", ret);
      return false;
    }

    // Apply options to encoder context
    AVDictionaryEntry *opt = nullptr;
    while ((opt = av_dict_get(opts, "", opt, AV_DICT_IGNORE_SUFFIX))) {
      if (FFmpegUtils::hasOption(codecCtx->priv_data, opt->key)) {
        ret = av_opt_set(codecCtx->priv_data, opt->key, opt->value, 0);
        if (ret < 0) {
          ERROR_MSG("Video Encoder: Failed to set hardware encoder option %s=%s", opt->key, opt->value);
          FFmpegUtils::printAvError("", ret);
        } else {
          MEDIUM_MSG("Video Encoder: Set hardware encoder option %s=%s", opt->key, opt->value);
        }
      } else {
        MEDIUM_MSG("Video Encoder: Hardware encoder option '%s' unsupported; ignoring.", opt->key);
      }
    }

    av_dict_free(&opts);
    return true;
  }

  bool VideoEncoderNode::handleHardwareToSoftwareFrame(AVFrame *hwFrame) {
    if (!hwFrame) {
      ERROR_MSG("Video Encoder: Invalid hardware frame");
      return false;
    }

    // Get software frame from pool - use the same format as the hardware frame
    AVPixelFormat swFormat = (AVPixelFormat)hwFrame->format;

    // For hardware frames, we need to get the corresponding software format
    if (hwFrame->hw_frames_ctx) {
      AVHWFramesContext *hwFramesCtx = (AVHWFramesContext *)hwFrame->hw_frames_ctx->data;
      swFormat = hwFramesCtx->sw_format;
    }

    if (!swFrame || swFrame->width != hwFrame->width || swFrame->height != hwFrame->height){
      av_frame_free(&swFrame);
      swFrame = av_frame_alloc();
      swFrame->format = swFormat;
      swFrame->width = hwFrame->width;
      swFrame->height = hwFrame->height;
    }

    // Transfer data from hardware frame to software frame
    int ret = av_hwframe_transfer_data(swFrame, hwFrame, 0);
    if (ret < 0) {
      FFmpegUtils::printAvError("Video Encoder: Failed to transfer hardware frame to software", ret);
      return false;
    }

    // Copy frame properties (timestamps, etc.)
    ret = av_frame_copy_props(swFrame, hwFrame);
    if (ret < 0) {
      FFmpegUtils::printAvError("Video Encoder: Failed to copy frame properties", ret);
      return false;
    }

    HIGH_MSG("Video Encoder: Successfully transferred HW frame to SW format %s",
             av_get_pix_fmt_name((AVPixelFormat)swFrame->format));

    // Encode the transferred frame directly - let encoder handle any format issues
    return encodeFrame(swFrame);
  }

  bool VideoEncoderNode::encodeFrame(AVFrame *frame) {
    if (!frame || !codecCtx) {
      ERROR_MSG("Video Encoder: Invalid encoder state");
      return false;
    }

    // Normalize frame PTS: expect ms; synthesize ms if missing; then rescale to encoder time_base
    int64_t inPtsMs = frame->pts;
    // Rescale ms -> encoder time_base
    frame->pts = av_rescale_q(inPtsMs, (AVRational){1, 1000}, codecCtx->time_base);

    // Set picture type
    if (gopSize){
      if (!lastKeyTime || inPtsMs - lastKeyTime >= gopSize) {
        frame->pict_type = AV_PICTURE_TYPE_I;
        lastKeyTime = inPtsMs;
      } else {
        frame->pict_type = AV_PICTURE_TYPE_P;
      }
    }

    // Send frame to encoder with timeout protection for AV1
    int ret = avcodec_send_frame(codecCtx, frame);
    if (ret < 0) {
      FFmpegUtils::printAvError("Video Encoder: Error sending frame to encoder", ret);
      return false;
    }

    // Receive encoded packets

    size_t outCnt = 0;
    while (ret >= 0) {
      ret = avcodec_receive_packet(codecCtx, packet);
      if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
        // No more packets available right now
        break;
      } else if (ret < 0) {
        FFmpegUtils::printAvError("Video Encoder: Error receiving packet from encoder", ret);
        return false;
      }
      ++outCnt;

      // Create packet context
      PacketContext packetCtx(packet);
      packetCtx.setEncoderNodeId(getNodeId());
      if (!packetCtx.getPacket()) {
        ERROR_MSG("Video Encoder: Could not create packet context");
        return false;
      }

      // Read codec parameters
      AVCodecParameters *params = avcodec_parameters_alloc();
      if (params) {
        avcodec_parameters_from_context(params, codecCtx);
        packetCtx.setCodecParameters(params);

        // Set codec data from extradata if available
        if (codecCtx->extradata && codecCtx->extradata_size > 0) {
          packetCtx.setCodecData(codecCtx->extradata, codecCtx->extradata_size);
        }

        // Set format info for the packet
        PacketContext::FormatInfo formatInfo;
        formatInfo.codecName = codecName;
        formatInfo.width = width;
        formatInfo.height = height;
        formatInfo.isKeyframe = (packet->flags & AV_PKT_FLAG_KEY) != 0;
        formatInfo.fpks = fpks;
        formatInfo.codecId = codecCtx->codec_id;
        packetCtx.setFormatInfo(formatInfo);

        DONTEVEN_MSG("Video Encoder: Set format info: codec=%s, %" PRIu64 "x%" PRIu64 ", keyframe=%d",
                     formatInfo.codecName.c_str(), formatInfo.width, formatInfo.height, formatInfo.isKeyframe);

        avcodec_parameters_free(&params);
      }

      packetCtx.setPts(inPtsMs);

      totalProcessingTime.fetch_add(Util::getMicros(startTime));

      // Add to outputs
      for (auto & cb : callbacks) { cb(&packetCtx); }
    }

    HIGH_MSG("Video Encoder: %zu packets outputted by encoder", outCnt);

    return true;
  }

  void VideoEncoderNode::updateLongName() {
    // Special naming for RAW packetization formats
    std::string lower = codecName;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    if (lower == "yuyv" || lower == "uyvy" || lower == "nv12") {
      longName = "Raw " + codecName;
      if (width && height) { longName += ", " + std::to_string(width) + "x" + std::to_string(height); }
      return;
    }

    longName = codecName + ": ";

    // Add actual encoder name if available (e.g., h264_videotoolbox, libx264, etc.)
    if (codec && codec->long_name) {
      longName += codec->long_name;
    } else if (codec && codec->name) {
      longName += codec->name;
    } else {
      longName = "uninitialized";
    }

    if (!profile.empty()) { longName += ", profile=" + profile; }
    if (rateControlMode == "cbr" || rateControlMode == "vbr") {
      longName += ", "+rateControlMode+"=" + std::to_string(bitrate) + "b/s";
    } else if (rateControlMode == "cq") {
      longName += ", cq=" + std::to_string(quality);
    }
  }

  AVHWDeviceType VideoEncoderNode::getHWDeviceType() const {
    return hwInfo.type;
  }

  bool VideoEncoderNode::isHardwareCodec() const {
    if (!codec || !codec->name) return false;
    std::string encoderName = codec->name;
    return encoderName.find("videotoolbox") != std::string::npos || encoderName.find("nvenc") != std::string::npos ||
      encoderName.find("qsv") != std::string::npos || encoderName.find("vaapi") != std::string::npos;
  }

  std::set<AVPixelFormat> VideoEncoderNode::getPreferredSwPixelFormat() const {
    std::set<AVPixelFormat> r;
    if (codecName == "YUYV") {
      r.insert(AV_PIX_FMT_YUYV422);
      return r;
    }
    if (codecName == "UYVY") {
      r.insert(AV_PIX_FMT_UYVY422);
      return r;
    }
    if (codecName == "NV12") {
      r.insert(AV_PIX_FMT_NV12);
      return r;
    }
    if (codecName == "JPEG") {
      r.insert(AV_PIX_FMT_YUVJ420P);
      return r;
    }
    // Prefer the software format associated with the active hardware context, when available
    if (hwInfo.swFormat != AV_PIX_FMT_NONE) {
      r.insert(hwInfo.swFormat);
      return r;
    }

    if (!codec) {
      ERROR_MSG("Video Encoder: No codec available for format retrieval");
      return r;
    }

#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(61, 19, 100)
    const AVPixelFormat *formats = nullptr;
    int ret = avcodec_get_supported_config(nullptr, codec, AV_CODEC_CONFIG_PIX_FORMAT, 0, (const void **)&formats, nullptr);
    if (ret >= 0 && formats) {
      for (int i = 0; formats[i] != AV_PIX_FMT_NONE; i++) { r.insert(formats[i]); }
    }
#else
    if (codec->pix_fmts) {
      for (int i = 0; codec->pix_fmts[i] != AV_PIX_FMT_NONE; i++) { r.insert(codec->pix_fmts[i]); }
    }
#endif
    return r;
  }

  AVPixelFormat VideoEncoderNode::getPreferredHwPixelFormat() const {
    // Prefer the software format associated with the active hardware context, when available
    if (hwInfo.format != AV_PIX_FMT_NONE) { return hwInfo.format; }
    return AV_PIX_FMT_NONE;
  }

  bool VideoEncoderNode::initHW(AVBufferRef *hwContext) {
    if (!hwContext) return false;

    // Store hardware context
    hwInfo.context = av_buffer_ref(hwContext);
    if (!hwInfo.context) {
      ERROR_MSG("Video Encoder: Failed to reference hardware context");
      return false;
    }

    // Get device type
    hwInfo.type = ((AVHWDeviceContext *)hwContext->data)->type;
    hwInfo.isEnabled = true;

    return true;
  }

  bool VideoEncoderNode::initializeCodec(uint64_t inputWidth, uint64_t inputHeight) {
    std::lock_guard<std::mutex> lock(mutex);

    // Only free existing codec context if parameters have changed
    bool needsRecreation = false;
    if (codecCtx) {
      if (codecCtx->width != inputWidth || codecCtx->height != inputHeight) {
        INFO_MSG("Video Encoder: Codec parameters changed (%dx%d -> %" PRIu64 "x%" PRIu64 "), recreating context",
                 codecCtx->width, codecCtx->height, inputWidth, inputHeight);
        needsRecreation = true;
      } else {
        INFO_MSG("Video Encoder: Codec parameters unchanged, reusing existing context");
        return true; // No need to recreate
      }
    } else {
      INFO_MSG("Video Encoder: Creating new codec context");
      needsRecreation = true;
    }

    // Free existing codec context only if we need to recreate it
    if (needsRecreation && codecCtx) {
      avcodec_free_context(&codecCtx);
      codecCtx = nullptr;
    }

    if (!codec) {
      ERROR_MSG("Video Encoder: No codec available for initialization");
      return false;
    }

    // Create new codec context
    codecCtx = avcodec_alloc_context3(codec);
    if (!codecCtx) {
      ERROR_MSG("Video Encoder: Failed to allocate codec context");
      return false;
    }

    // Set basic parameters
    width = inputWidth;
    height = inputHeight;
    codecCtx->width = inputWidth;
    codecCtx->height = inputHeight;
    INFO_MSG("Resolution set to %" PRIu64 "x%" PRIu64, inputWidth, inputHeight);

    codecCtx->time_base.num = 1;
    codecCtx->time_base.den = 1000;
    codecCtx->pkt_timebase = codecCtx->time_base;

    // Make frame rate as clean as possible
    if (fpks % 1000 == 0){
      codecCtx->framerate.den = 1;
      codecCtx->framerate.num = fpks / 1000;
    }else if (fpks == 29970){
      codecCtx->framerate.den = 1001;
      codecCtx->framerate.num = 30000;
    }else if (fpks == 59940){
      codecCtx->framerate.den = 1001;
      codecCtx->framerate.num = 60000;
    }else if (fpks == 119880){
      codecCtx->framerate.den = 1001;
      codecCtx->framerate.num = 120000;
    }else{
      codecCtx->framerate.den = 1000;
      codecCtx->framerate.num = fpks;
    }

    // Set codec type and basic parameters
    codecCtx->codec_type = AVMEDIA_TYPE_VIDEO;
    codecCtx->max_b_frames = 0; // For low latency
    codecCtx->has_b_frames = false;
    codecCtx->flags |= AV_CODEC_FLAG_CLOSED_GOP;

    // Set codec-specific basic parameters
    if (codecName == "AV1") {
      // AV1-specific basic parameters - avoid conflicting settings
      // Don't set qmin/qmax, refs, slices, compression_level for AV1 as they may conflict with CRF
      INFO_MSG("Video Encoder: Using AV1-specific basic parameters");
    } else if (codecName == "JPEG") {
      // JPEG-specific basic parameters - avoid conflicting settings
      // Don't set qmin/qmax, refs, slices, compression_level for JPEG as they conflict with quality-based encoding
      INFO_MSG("Video Encoder: Using JPEG-specific basic parameters");
    } else {
      // H.264 and other codecs can use these parameters
      codecCtx->qmin = 20;
      codecCtx->qmax = 51;
      //codecCtx->refs = 2;
      codecCtx->slices = 0;
      codecCtx->codec_id = codec->id;
      codecCtx->compression_level = 4;
    }

    // Attach hardware contexts if they are available from upstream frames/device configuration
    if (inputHwFramesCtx) {
      if (codecCtx->hw_frames_ctx) { av_buffer_unref(&codecCtx->hw_frames_ctx); }
      codecCtx->hw_frames_ctx = av_buffer_ref(inputHwFramesCtx);
      if (!codecCtx->hw_frames_ctx) {
        ERROR_MSG("Video Encoder: Failed to attach hw_frames_ctx to encoder");
        avcodec_free_context(&codecCtx);
        return false;
      }
    }

    AVBufferRef *deviceCtxForCodec = hwInfo.context ? hwInfo.context : inputHwDeviceCtx;
    if (deviceCtxForCodec) {
      if (codecCtx->hw_device_ctx) { av_buffer_unref(&codecCtx->hw_device_ctx); }
      codecCtx->hw_device_ctx = av_buffer_ref(deviceCtxForCodec);
      if (!codecCtx->hw_device_ctx) {
        ERROR_MSG("Video Encoder: Failed to attach hw_device_ctx to encoder");
        avcodec_free_context(&codecCtx);
        return false;
      }
    }

    // Set codec-specific profiles (critical for AV1!)
    if (codecName == "AV1") {
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(62, 0, 0)
      codecCtx->profile = AV_PROFILE_AV1_MAIN;
#else
      codecCtx->profile = FF_PROFILE_AV1_MAIN;
#endif
      INFO_MSG("Video Encoder: Set AV1 profile to MAIN");
    } else if (codecName == "H264") {
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(62, 0, 0)
      codecCtx->profile = AV_PROFILE_H264_HIGH;
#else
      codecCtx->profile = FF_PROFILE_H264_HIGH;
#endif
      INFO_MSG("Video Encoder: Set H.264 profile to HIGH");
    }

    // Setup pixel format properly
    if (!setupPixelFormat()) {
      ERROR_MSG("Video Encoder: Failed to configure pixel format");
      avcodec_free_context(&codecCtx);
      return false;
    }

    // Configure rate control
    if (!setupRateControl()) {
      ERROR_MSG("Video Encoder: Failed to configure rate control");
      avcodec_free_context(&codecCtx);
      return false;
    }

    // Configure profile (additional profile settings)
    if (!setupProfile()) {
      ERROR_MSG("Video Encoder: Failed to configure encoder profile");
      avcodec_free_context(&codecCtx);
      return false;
    }

    // Configure hardware acceleration ONLY if we determined we're using hardware encoding
    // For hardware encoders like h264_videotoolbox, the encoder handles hardware acceleration internally
    bool isHardwareEncoder = isHardwareCodec();
    if (hwInfo.isEnabled && !isHardwareEncoder) {
      INFO_MSG("Video Encoder: Setting up hardware acceleration for software encoder with hardware "
               "frames");
      if (!configureHardwareAcceleration()) {
        ERROR_MSG("Video Encoder: Hardware acceleration setup failed, but was required for "
                  "hardware encoding");
        avcodec_free_context(&codecCtx);
        return false;
      }
    } else if (isHardwareEncoder) {
      INFO_MSG("Video Encoder: Using hardware encoder %s - setting up hardware acceleration", codec->name);

      // For hardware encoders, we need to set up hardware acceleration properly
      if (!configureHardwareAcceleration()) {
        WARN_MSG("Video Encoder: Hardware acceleration setup failed for hardware encoder, trying without");
      }

      // Initialize hardware context if available
      if (hwInfo.isEnabled && hwInfo.context) {
        if (!initHW(hwInfo.context)) { WARN_MSG("Video Encoder: Hardware initialization failed"); }
      }

      // Configure hardware encoder options
      if (!configureHWEncoderOptions()) { WARN_MSG("Video Encoder: Failed to configure hardware encoder options"); }

      // For hardware encoders, mark as hardware enabled
      hwInfo.isEnabled = true;
    } else {
      INFO_MSG("Video Encoder: Hardware acceleration disabled, using software encoding");
    }

    // Configure codec-specific settings
    if (codecName == "H264") {
      // Force global headers for H.264 to ensure SPS/PPS are in extradata
      codecCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

      // Set additional H.264 options to ensure proper extradata generation
      if (codecCtx->priv_data) {
        // Force SPS/PPS to be written to extradata if supported by the encoder
        int ret = FFmpegUtils::setOption(codecCtx->priv_data, "global_header", "1", 0);
        if (ret < 0) { FFmpegUtils::printAvError("Video Encoder: Failed to set global_header option", ret); }

        // Ensure we get SPS/PPS in the first packet when supported
        ret = FFmpegUtils::setOption(codecCtx->priv_data, "repeat_headers", "1", 0);
        if (ret < 0) { FFmpegUtils::printAvError("Video Encoder: Failed to set repeat_headers option", ret); }
      }

      MEDIUM_MSG("Video Encoder: Configured H.264 for extradata generation");
    } else if (codecName == "AV1") {
      // Configure AV1 for real-time encoding to prevent hanging
      // Be conservative with threading for AV1 encoders
      codecCtx->thread_count = 0; // Let the encoder decide
      codecCtx->thread_type = FF_THREAD_SLICE; // Use slice threading

      // Set AV1-specific options based on the encoder
      if (codecCtx->priv_data) {
        const char *encoderName = codec->name;
        INFO_MSG("Video Encoder: Configuring AV1 encoder: %s", encoderName);

        if (strcmp(encoderName, "libaom-av1") == 0) {
          // libaom-av1 specific settings for real-time encoding
          int ret;
          ret = av_opt_set(codecCtx->priv_data, "usage", "realtime", 0);
          if (ret < 0) { FFmpegUtils::printAvError("Video Encoder: Failed to set libaom-av1 usage", ret); }

          ret = av_opt_set_int(codecCtx->priv_data, "cpu-used", 8, 0); // 0-8, higher = faster
          if (ret < 0) { FFmpegUtils::printAvError("Video Encoder: Failed to set libaom-av1 cpu-used", ret); }

          // Ensure CRF is in valid range (-1 to 63, -1 = disabled)
          int crf_value = (quality >= 0 && quality <= 63) ? quality : 30;
          ret = av_opt_set_int(codecCtx->priv_data, "crf", crf_value, 0);
          if (ret < 0) { FFmpegUtils::printAvError("Video Encoder: Failed to set libaom-av1 crf", ret); }

          // Disable expensive features for real-time
          av_opt_set_int(codecCtx->priv_data, "enable-cdef", 0, 0);
          av_opt_set_int(codecCtx->priv_data, "enable-restoration", 0, 0);
          av_opt_set_int(codecCtx->priv_data, "enable-global-motion", 0, 0);
          av_opt_set_int(codecCtx->priv_data, "tile-columns", 2, 0);
          av_opt_set_int(codecCtx->priv_data, "tile-rows", 1, 0);
          av_opt_set_int(codecCtx->priv_data, "row-mt", 1, 0);

          INFO_MSG("Video Encoder: Configured libaom-av1 with cpu-used=8 (fastest), crf=%d", crf_value);
        } else if (strcmp(encoderName, "libsvtav1") == 0) {
          // SVT-AV1 specific settings for real-time encoding
          int ret;
          ret = av_opt_set_int(codecCtx->priv_data, "preset", 12, 0); // Fastest preset (-2 to 13)
          if (ret < 0) { FFmpegUtils::printAvError("Video Encoder: Failed to set SVT-AV1 preset", ret); }

          // Ensure CRF is in valid range (0-63)
          int crf_value = (quality >= 0 && quality <= 63) ? quality : 30;
          ret = av_opt_set_int(codecCtx->priv_data, "crf", crf_value, 0);
          if (ret < 0) { FFmpegUtils::printAvError("Video Encoder: Failed to set SVT-AV1 crf", ret); }

          INFO_MSG("Video Encoder: Configured SVT-AV1 with preset=12, crf=%d", crf_value);
        } else if (strcmp(encoderName, "librav1e") == 0) {
          // librav1e specific settings for real-time encoding
          int ret;
          ret = av_opt_set_int(codecCtx->priv_data, "speed", 10, 0); // 0-10, higher = faster
          if (ret < 0) { FFmpegUtils::printAvError("Video Encoder: Failed to set librav1e speed", ret); }

          // Use QP instead of CRF for librav1e (range -1 to 255, -1 = disabled)
          int qp_value = (quality >= 0 && quality <= 255) ? quality : 30;
          ret = av_opt_set_int(codecCtx->priv_data, "qp", qp_value, 0);
          if (ret < 0) { FFmpegUtils::printAvError("Video Encoder: Failed to set librav1e qp", ret); }

          ret = av_opt_set_int(codecCtx->priv_data, "tiles", 4, 0); // Number of tiles for parallel processing
          if (ret < 0) { FFmpegUtils::printAvError("Video Encoder: Failed to set librav1e tiles", ret); }

          INFO_MSG("Video Encoder: Configured librav1e with speed=10 (fastest), qp=%d", qp_value);
        }
      }

      MEDIUM_MSG("Video Encoder: Configured AV1 for real-time encoding");
    } else if (codecName == "HEVC" || codecName == "H265") {
      // Configure HEVC for optimal encoding
      codecCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

      if (codecCtx->priv_data) {
        // Set HEVC-specific options if supported
        int ret = FFmpegUtils::setOption(codecCtx->priv_data, "global_header", "1", 0);
        if (ret < 0) { FFmpegUtils::printAvError("Video Encoder: Failed to set HEVC global_header option", ret); }

        // Set CRF if quality is specified
        if (quality > 0) {
          ret = FFmpegUtils::setOptionInt(codecCtx->priv_data, "crf", quality, 0);
          if (ret < 0) { FFmpegUtils::printAvError("Video Encoder: Failed to set HEVC crf", ret); }
        }
      }

      MEDIUM_MSG("Video Encoder: Configured HEVC encoder");
    } else if (codecName == "VP9") {
      // Configure VP9 for optimal encoding
      if (codecCtx->priv_data) {
        // Set VP9-specific options for real-time encoding
        int ret = av_opt_set_int(codecCtx->priv_data, "cpu-used", 8, 0); // 0-8, higher = faster
        if (ret < 0) { FFmpegUtils::printAvError("Video Encoder: Failed to set VP9 cpu-used", ret); }

        // Set CRF if quality is specified (VP9 CRF range is 0-63)
        if (quality > 0) {
          int crf_value = (quality >= 0 && quality <= 63) ? quality : 30;
          ret = av_opt_set_int(codecCtx->priv_data, "crf", crf_value, 0);
          if (ret < 0) { FFmpegUtils::printAvError("Video Encoder: Failed to set VP9 crf", ret); }
        }

        // Enable real-time mode
        ret = av_opt_set_int(codecCtx->priv_data, "deadline", 1, 0); // 1 = realtime
        if (ret < 0) { FFmpegUtils::printAvError("Video Encoder: Failed to set VP9 deadline", ret); }

        // Enable row-based multithreading
        ret = av_opt_set_int(codecCtx->priv_data, "row-mt", 1, 0);
        if (ret < 0) { FFmpegUtils::printAvError("Video Encoder: Failed to set VP9 row-mt", ret); }
      }

      MEDIUM_MSG("Video Encoder: Configured VP9 for real-time encoding");
    } else if (codecName == "JPEG") {
      // Configure JPEG/MJPEG for optimal encoding
      // JPEG doesn't use GOP size, B-frames, or rate control
      codecCtx->gop_size = 1; // Each frame is independent
      codecCtx->max_b_frames = 0;
      codecCtx->has_b_frames = false;

      MEDIUM_MSG("Video Encoder: Configured JPEG encoder with quality-based encoding");
    }

    // Apply compatibility mapping before opening the codec (hardware-specific knobs)
    applyEncoderCompatOptions();

    // Encoder-specific options via AVDictionary (only when the selected encoder supports them)
    AVDictionary *opts = nullptr;
    if (!tune.empty() && codecName == "H264" && codec && codec->name && std::string(codec->name) == "libx264") {
      av_dict_set(&opts, "tune", tune.c_str(), 0);
      INFO_MSG("Video Encoder: Set tune to %s", tune.c_str());
    }
    if (!preset.empty() && codecName == "H264" && codec && codec->name && std::string(codec->name) == "libx264") {
      av_dict_set(&opts, "preset", preset.c_str(), 0);
      INFO_MSG("Video Encoder: Set preset to %s", preset.c_str());
    }
    if (!tune.empty() && (codecName == "HEVC" || codecName == "H265") && codec && codec->name && std::string(codec->name) == "libx265") {
      av_dict_set(&opts, "tune", tune.c_str(), 0);
      INFO_MSG("Video Encoder: Set HEVC tune to %s", tune.c_str());
    }
    if (!preset.empty() && (codecName == "HEVC" || codecName == "H265") && codec && codec->name &&
        std::string(codec->name) == "libx265") {
      av_dict_set(&opts, "preset", preset.c_str(), 0);
      INFO_MSG("Video Encoder: Set HEVC preset to %s", preset.c_str());
    }

    // Open codec
    int ret = avcodec_open2(codecCtx, codec, &opts);
    av_dict_free(&opts);

    if (ret < 0) {
      FFmpegUtils::printAvError("Video Encoder: Failed to open codec", ret);
      avcodec_free_context(&codecCtx);
      return false;
    }

    // For H.264, check if extradata was generated during codec opening
    if (codecName == "H264") {
      if (codecCtx->extradata && codecCtx->extradata_size > 0) {
        MEDIUM_MSG("Video Encoder: H.264 extradata generated during codec init: %d bytes", codecCtx->extradata_size);
      } else {
        WARN_MSG("Video Encoder: H.264 extradata not available after codec opening - will try "
                 "after first frame");
      }
    }

    updateLongName();
    INFO_MSG("Successfully (re)initialized %s", longName.c_str());

    return true;
  }

  void VideoEncoderNode::convertToAnnexB(std::shared_ptr<PacketContext> packetCtx) {
    if (!packetCtx) return;

    AVPacket *packet = packetCtx->getPacket();
    if (!packet || !packet->data || packet->size <= 4) return;

    // Check if this looks like length-prefixed NAL data
    const uint8_t *data = packet->data;
    size_t dataSize = packet->size;

    // Read first 4-byte length prefix
    uint32_t firstNalLen = (data[0] << 24) | (data[1] << 16) | (data[2] << 8) | data[3];

    // If the first 4 bytes represent a reasonable NAL length, convert
    if (firstNalLen > 0 && firstNalLen + 4 <= dataSize) {
      std::vector<uint8_t> convertedData;
      size_t offset = 0;

      while (offset + 4 < dataSize) {
        // Read 4-byte length prefix
        uint32_t nalLen = (data[offset] << 24) | (data[offset + 1] << 16) | (data[offset + 2] << 8) | data[offset + 3];

        if (nalLen + offset + 4 > dataSize) {
          // Invalid NAL unit length, break
          break;
        }

        // Add Annex-B start code (0x00000001)
        convertedData.push_back(0x00);
        convertedData.push_back(0x00);
        convertedData.push_back(0x00);
        convertedData.push_back(0x01);

        // Add NAL unit data
        convertedData.insert(convertedData.end(), data + offset + 4, data + offset + 4 + nalLen);

        offset += nalLen + 4;
      }

      if (!convertedData.empty() && offset == dataSize) {
        // Successfully converted all data, replace packet data
        av_packet_unref(packet);
        av_new_packet(packet, convertedData.size());
        memcpy(packet->data, convertedData.data(), convertedData.size());

        VERYHIGH_MSG("Video Encoder: Converted %zu bytes from length-prefixed to Annex-B format", convertedData.size());
      }
    }
  }

  bool VideoEncoderNode::processRawFrame(VideoFrameContext & frame) {
    AVFrame *avFrame = frame.getAVFrame();
    if (!avFrame) {
      ERROR_MSG("Video Encoder: Null AVFrame in RAW frame");
      return false;
    }
    width = frame.getWidth();
    height = frame.getHeight();

    DONTEVEN_MSG("Video Encoder: Processing RAW frame format %s (%dx%d)", codecName.c_str(), avFrame->width, avFrame->height);

    // For RAW formats, we create a packet directly from the frame data
    // This is a passthrough since the data is already in the target format

    // Calculate the size of the frame data
    int bufferSize = av_image_get_buffer_size((AVPixelFormat)avFrame->format, avFrame->width, avFrame->height, 1);
    if (bufferSize < 0) {
      FFmpegUtils::printAvError("Video Encoder: Failed to calculate buffer size for RAW frame", bufferSize);
      return false;
    }

    // Create packet from frame data
    AVPacket *packet = av_packet_alloc();
    if (!packet) {
      ERROR_MSG("Video Encoder: Failed to allocate packet for RAW frame");
      return false;
    }

    // Allocate packet data
    int ret = av_new_packet(packet, bufferSize);
    if (ret < 0) {
      FFmpegUtils::printAvError("Video Encoder: Failed to allocate packet data for RAW frame", ret);
      av_packet_free(&packet);
      return false;
    }

    // Copy frame data to packet using av_image_copy_to_buffer
    ret = av_image_copy_to_buffer(packet->data, packet->size, (const uint8_t *const *)avFrame->data, avFrame->linesize,
                                  (AVPixelFormat)avFrame->format, avFrame->width, avFrame->height, 1);
    if (ret < 0) {
      FFmpegUtils::printAvError("Video Encoder: Failed to copy raw frame data to packet", ret);
      av_packet_free(&packet);
      return false;
    }

    // Set packet timing consistently in ms
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(56, 70, 100)
    packet->pts = avFrame->pts;
    packet->duration = avFrame->duration;
#else
    packet->pts = avFrame->pkt_pts;
    packet->duration = avFrame->pkt_duration;
#endif
    packet->dts = packet->pts;

    packet->flags = AV_PKT_FLAG_KEY; // RAW frames are always keyframes

    // Set stream index if we have codec context
    if (codecCtx) {
      packet->stream_index = 0; // Assume video stream is index 0
    }

    // Create packet context
    PacketContext packetCtx(packet);
    packetCtx.setEncoderNodeId(getNodeId());
    if (!packetCtx.getPacket()) {
      ERROR_MSG("Video Encoder: Could not create packet context for RAW frame");
      av_packet_free(&packet);
      return false;
    }

    // Set format info for the packet with proper codec ID for RAW formats
    PacketContext::FormatInfo formatInfo;
    formatInfo.codecName = codecName;
    formatInfo.width = avFrame->width;
    formatInfo.height = avFrame->height;
    formatInfo.fpks = fpks;
    formatInfo.isKeyframe = true; // RAW frames are always keyframes

    // Set the correct codec ID for RAW video
    formatInfo.codecId = AV_CODEC_ID_RAWVIDEO;

    packetCtx.setFormatInfo(formatInfo);

    // Add to outputs and ensure PacketContext carries ms PTS
    packetCtx.setPts(avFrame->pts);

    totalProcessingTime.fetch_add(Util::getMicros(startTime));

    for (auto & cb : callbacks) { cb(&packetCtx); }

    VERYHIGH_MSG("Video Encoder: Created RAW packet: %s, %dx%d, size=%d bytes, pts=%" PRIu64 ", duration=%" PRIu64 "",
                 codecName.c_str(), avFrame->width, avFrame->height, bufferSize, packet->pts, packet->duration);
    av_packet_free(&packet);

    return true;
  }

  bool VideoEncoderNode::isRawFormat() const {
    return FFmpegUtils::isRawFormat(codecName);
  }

} // namespace FFmpeg
