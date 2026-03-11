#include "video_decoder_node.h"

#include "../defines.h"
#include "../h264.h"
#include "hw_context_manager.h"
#include "../timing.h"
#include "node_pipeline.h"
#include "packet_context.h"
#include "utils.h"
#include "video_frame_context.h"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
}
#include <sstream>

namespace FFmpeg {

  const char *safePixFmtName(AVPixelFormat fmt) {
    const char *name = av_get_pix_fmt_name(fmt);
    return name ? name : "unknown";
  }

  std::string formatPixFmtList(const AVPixelFormat *pix_fmts) {
    if (!pix_fmts) { return "null"; }
    std::ostringstream os;
    bool first = true;
    for (int i = 0; pix_fmts[i] != AV_PIX_FMT_NONE; ++i) {
      if (!first) { os << ","; }
      first = false;
      const char *name = av_get_pix_fmt_name(pix_fmts[i]);
      os << (name ? name : "unknown");
    }
    return first ? "empty" : os.str();
  }

  void logHwConfigs(const AVCodec *codec) {
    if (!codec) { return; }
    const AVCodecHWConfig *hwConfig = nullptr;
    int index = 0;
    while ((hwConfig = avcodec_get_hw_config(codec, index++)) != nullptr) {
      const char *typeName = av_hwdevice_get_type_name(hwConfig->device_type);
      const char *pixName = av_get_pix_fmt_name(hwConfig->pix_fmt);
      INFO_MSG("DEBUG: Video Decoder: HW config[%d]: device=%s pix_fmt=%s methods=0x%x",
                index - 1,
                typeName ? typeName : "unknown",
                pixName ? pixName : "unknown",
                hwConfig->methods);
    }
  }

  VideoDecoderNode::VideoDecoderNode(const std::string & _codecName)
    : ProcessingNode() {
    // Set codec name from parameter
    codecName = _codecName;
    frame = av_frame_alloc();
    longName = "Uninitialized " + codecName + " decoder";
  }

  VideoDecoderNode::~VideoDecoderNode() {
    if (codecCtx) { avcodec_free_context(&codecCtx); }
    if (frame) { av_frame_free(&frame); }
    if (hwInfo.context) {
      av_buffer_unref(&hwInfo.context);
      hwInfo.context = nullptr;
    }
  }

  bool VideoDecoderNode::processRawPacket(PacketContext * packet) {
    AVPacket *avPacket = packet->getPacket();
    if (!avPacket->data || !avPacket->size) {
      ERROR_MSG("Video Decoder: Empty RAW packet data");
      return false;
    }

    // Determine pixel format from codec name
    AVPixelFormat pixelFormat = AV_PIX_FMT_NONE;
    if (codecName == "YUYV") {
      pixelFormat = AV_PIX_FMT_YUYV422;
    } else if (codecName == "UYVY") {
      pixelFormat = AV_PIX_FMT_UYVY422;
    } else if (codecName == "NV12") {
      pixelFormat = AV_PIX_FMT_NV12;
    } else {
      ERROR_MSG("Video Decoder: Unimplemented RAW format: %s", codecName.c_str());
      return false;
    }

    uint64_t pTime = Util::getMicros();

    // Create video frame context from RAW data
    VideoFrameContext outCtx;
    if (!outCtx.createRawFrame(avPacket->data, avPacket->size, width, height, pixelFormat, nullptr)) {
      ERROR_MSG("Video Decoder: Failed to create RAW frame context");
      return false;
    }

    // Set frame timing
    outCtx.setPts(avPacket->pts);
    outCtx.setFpks(packet->getFrameRate());

    // Set decode time
    totalProcessingTime.fetch_add(Util::getMicros(pTime));

    for (auto & cb : callbacks) { cb(&outCtx); }

    AVFrame * frm = outCtx.getAVFrame();
    av_frame_free(&frm);
    return true;
  }

  bool VideoDecoderNode::reconfigureDecoder() {
    INFO_MSG("Video Decoder: Starting decoder reconfiguration (codec=%s, ID=%d, %" PRIu64 "x%" PRIu64 ")",
             codecName.c_str(), codecId, width, height);

    // Clean up existing decoder
    if (codecCtx) { avcodec_free_context(&codecCtx); }

    codec = 0;
    if (!codec) {
      // Find encoder using hardware-aware approach for all codecs
      auto & hwManager = HWContextManager::getInstance();
      auto devices = hwManager.getAvailableDevices();
      for (const auto & device : devices) {
        const auto & deviceType = device.type;
        const char *hwEncoderName = nullptr;

        if (codecName == "H264" || codecName == "h264") {
          switch (deviceType) {
            case AV_HWDEVICE_TYPE_VIDEOTOOLBOX: hwEncoderName = "h264_videotoolbox"; break;
            case AV_HWDEVICE_TYPE_CUDA: hwEncoderName = "h264_cuvid"; break;
            case AV_HWDEVICE_TYPE_QSV: hwEncoderName = "h264_qsv"; break;
            case AV_HWDEVICE_TYPE_VAAPI: hwEncoderName = "h264_vaapi"; break;
            default: break;
          }
        } else if (codecName == "AV1" || codecName == "av1") {
          switch (deviceType) {
            case AV_HWDEVICE_TYPE_VIDEOTOOLBOX: hwEncoderName = "av1_videotoolbox"; break;
            case AV_HWDEVICE_TYPE_CUDA: hwEncoderName = "av1_cuvid"; break;
            case AV_HWDEVICE_TYPE_QSV: hwEncoderName = "av1_qsv"; break;
            case AV_HWDEVICE_TYPE_VAAPI: hwEncoderName = "av1_vaapi"; break;
            default: break;
          }
        } else if (codecName == "HEVC" || codecName == "H265" || codecName == "hevc" || codecName == "h265") {
          switch (deviceType) {
            case AV_HWDEVICE_TYPE_VIDEOTOOLBOX: hwEncoderName = "hevc_videotoolbox"; break;
            case AV_HWDEVICE_TYPE_CUDA: hwEncoderName = "hevc_cuvid"; break;
            case AV_HWDEVICE_TYPE_QSV: hwEncoderName = "hevc_qsv"; break;
            case AV_HWDEVICE_TYPE_VAAPI: hwEncoderName = "hevc_vaapi"; break;
            default: break;
          }
        } else if (codecName == "VP9" || codecName == "vp9") {
          switch (deviceType) {
            case AV_HWDEVICE_TYPE_VIDEOTOOLBOX: hwEncoderName = "vp9_videotoolbox"; break;
            case AV_HWDEVICE_TYPE_VAAPI: hwEncoderName = "vp9_vaapi"; break;
            case AV_HWDEVICE_TYPE_QSV: hwEncoderName = "vp9_qsv"; break;
            default: break;
          }
        } else if (codecName == "VP8" || codecName == "vp8") {
          switch (deviceType) {
            case AV_HWDEVICE_TYPE_VIDEOTOOLBOX: hwEncoderName = "vp8_videotoolbox"; break;
            case AV_HWDEVICE_TYPE_VAAPI: hwEncoderName = "vp8_vaapi"; break;
            case AV_HWDEVICE_TYPE_QSV: hwEncoderName = "vp8_qsv"; break;
            default: break;
          }
        }

        if (hwEncoderName) {
          codec = avcodec_find_decoder_by_name(hwEncoderName);
          if (codec) {
            break;
          } else {
            INFO_MSG("Video Decoder: Hardware decoder %s not available", hwEncoderName);
          }
        }
      }
    }

    // Find codec by either name (first try) or ID (second try) if possible
    if (!codec && codecName.size()) {
      codec = avcodec_find_decoder_by_name(codecName.c_str());
      if (codec) {
        MEDIUM_MSG("Video Decoder: Found decoder by name: %s", codec->name);
      } else {
        WARN_MSG("Video Decoder: Failed to find decoder name: %s", codecName.c_str());
      }
    }
    if (!codec && codecId != AV_CODEC_ID_NONE) {
      codec = avcodec_find_decoder(codecId);
      if (codec) {
        MEDIUM_MSG("Video Decoder: Found decoder by ID: %s", codec->name);
      } else {
        WARN_MSG("Video Decoder: Failed to find decoder ID: %d", codecId);
      }
    }
    // If we still don't have a codec, we can't proceed
    if (!codec) {
      ERROR_MSG("Video Decoder: No decoder available");
      return false;
    }
    INFO_MSG("DEBUG: Video Decoder: Selected decoder: %s (%s)", codec->name,
               codec->long_name ? codec->long_name : "no long name");
    logHwConfigs(codec);

    // Create new decoder context
    codecCtx = avcodec_alloc_context3(codec);
    if (!codecCtx) {
      ERROR_MSG("Video Decoder: Failed to allocate decoder context");
      return false;
    }

    // Configure context
    codecCtx->width = width;
    codecCtx->height = height;

    MEDIUM_MSG("Video Decoder: Configured context - width=%" PRIu64 ", height=%" PRIu64 "", width, height);

    codecCtx->time_base.num = 1;
    codecCtx->time_base.den = 1000;
    codecCtx->pkt_timebase = codecCtx->time_base;

    codecCtx->opaque = this;
    codecCtx->get_format = getFormatCallback;

    // Set thread count based on resolution
    if (width >= 1920 || height >= 1080) {
      codecCtx->thread_count = 4;
      codecCtx->thread_type = FF_THREAD_FRAME;
    } else {
      codecCtx->thread_count = 2;
      codecCtx->thread_type = FF_THREAD_SLICE;
    }

    // Set extradata
    if (initData.size()) {
      codecCtx->extradata = (unsigned char *)initData.data();
      codecCtx->extradata_size = initData.size();
    }
    if ((codecId == AV_CODEC_ID_H264 || codecName == "H264" || codecName == "h264") && initData.size() >= 7) {
      h264::initData h264Init((const char *)initData.data(), initData.size());
      if (h264Init) {
        INFO_MSG("DEBUG: Video Decoder: H264 initData profile=%u constraints=0x%02x level=%u nal_len=%u",
                 h264Init.profile, h264Init.constraints, h264Init.level, h264Init.NALULen);
      } else {
        WARN_MSG("Video Decoder: H264 initData parse failed (size=%zu)", initData.size());
      }
    }

    // Configure hardware acceleration
    MEDIUM_MSG("Video Decoder: Configuring hardware acceleration...");
    if (!configureHardwareAcceleration()) {
      ERROR_MSG("Video Decoder: Software decoding not allowed");
      return false;
    }

    // Open decoder
    INFO_MSG("DEBUG: Video Decoder: Open params codec=%s pix_fmt=%s sw_pix_fmt=%s hw_device_ctx=%s extradata=%d",
             codec->name,
             safePixFmtName(codecCtx->pix_fmt),
             safePixFmtName(codecCtx->sw_pix_fmt),
             codecCtx->hw_device_ctx ? "Y" : "N",
             codecCtx->extradata_size);
    AVDictionary *opts = nullptr;
    int ret = avcodec_open2(codecCtx, codec, &opts);
    av_dict_free(&opts);
    if (ret < 0) {
      FAIL_MSG("Could not open decoder: %d", ret);
      FFmpegUtils::printAvError("Video Decoder: Failed to open decoder", ret);
      return false;
    }

    // Update the long name now that we have a properly initialized codec
    updateLongName();

    INFO_MSG("Successfully (re)initialized: %s", longName.c_str());
    return true;
  }

  bool VideoDecoderNode::setInput(void * inData, size_t idx) {
    PacketContext * packet = (PacketContext*)inData;
    if (!packet || !packet->getPacket()) {
      ERROR_MSG("Video Decoder: Invalid packet");
      return false;
    }

    // Update input parameters if needed
    bool needsReconfigure = false;
    {
      const FFmpeg::PacketContext::FormatInfo & info = packet->getFormatInfo();
      // Check if parameters have changed
      if (info.width != width || info.height != height || info.codecName != codecName || info.codecId != codecId ||
          packet->getCodecData() != initData || info.fpks != fpks) {

        // Update parameters
        initData = packet->getCodecData();
        width = info.width;
        height = info.height;
        codecName = info.codecName;
        codecId = info.codecId;
        fpks = info.fpks;
        needsReconfigure = (codecId != AV_CODEC_ID_NONE || !codecName.empty());
      }
    }
    bool isRaw = FFmpegUtils::isRawFormat(codecName);
    if (isRaw) {
      if (needsReconfigure) { updateLongName(); }
    } else {
      if (!codecCtx && (codecId != AV_CODEC_ID_NONE || !codecName.empty())) { needsReconfigure = true; }
    }

    bool success = false;
    if (isRaw) {
      // Handle RAW format - create frame directly from packet data
      success = processRawPacket(packet);
    } else {
      if (needsReconfigure && !reconfigureDecoder()) {
        ERROR_MSG("Video Decoder: Failed to reconfigure decoder");
        return false;
      }
      // Process encoded packet through decoder
      success = decodePacket(packet->getPacket());
    }

    // Update metrics
    if (success) {
      return true;
    } else {
      trackFrameDrop();
      ERROR_MSG("Video Decoder: Failed to process packet (raw=%d)", isRaw);
      return false;
    }
  }

  bool VideoDecoderNode::initHW(AVBufferRef *hwContext) {
    if (!hwContext) {
      ERROR_MSG("Video Decoder: Invalid hardware context");
      return false;
    }

    // Store hardware context
    hwInfo.context = av_buffer_ref(hwContext);
    if (!hwInfo.context) {
      ERROR_MSG("Video Decoder: Failed to reference hardware context");
      return false;
    }

    // Get hardware device context to determine formats
    AVHWDeviceContext *deviceCtx = (AVHWDeviceContext *)hwContext->data;
    if (!deviceCtx) {
      ERROR_MSG("Video Decoder: Invalid hardware device context");
      return false;
    }

    // Set default formats based on device type
    switch (deviceCtx->type) {
      case AV_HWDEVICE_TYPE_CUDA:
        hwInfo.format = AV_PIX_FMT_CUDA;
        hwInfo.swFormat = AV_PIX_FMT_YUV420P;
        break;
      case AV_HWDEVICE_TYPE_QSV:
        hwInfo.format = AV_PIX_FMT_QSV;
        hwInfo.swFormat = AV_PIX_FMT_NV12;
        break;
      case AV_HWDEVICE_TYPE_VAAPI:
        hwInfo.format = AV_PIX_FMT_VAAPI;
        hwInfo.swFormat = AV_PIX_FMT_NV12;
        break;
      case AV_HWDEVICE_TYPE_VIDEOTOOLBOX:
        hwInfo.format = AV_PIX_FMT_VIDEOTOOLBOX;
        hwInfo.swFormat = AV_PIX_FMT_NV12;
        break;
      default:
        ERROR_MSG("Video Decoder: Unsupported hardware device type: %d", deviceCtx->type);
        return false;
    }
    INFO_MSG("DEBUG: Video Decoder: initHW type=%s hw_fmt=%s",
             av_hwdevice_get_type_name(deviceCtx->type),
             safePixFmtName(hwInfo.format));

    // Only set hw_device_ctx here - hw_frames_ctx will be created in the get_format callback
    // after FFmpeg has parsed the stream and determined the correct sw_format
    if (codecCtx) {
      codecCtx->hw_device_ctx = av_buffer_ref(hwInfo.context);
      if (!codecCtx->hw_device_ctx) {
        ERROR_MSG("Video Decoder: Failed to reference hardware device context");
        return false;
      }
    }

    MEDIUM_MSG("Video Decoder: Successfully initialized hardware device context");
    return true;
  }

  void VideoDecoderNode::updateLongName() {
    // Special naming for RAW formats (passthrough ingest)
    if (FFmpegUtils::isRawFormat(codecName)) {
      longName = std::string("VideoRawIngest(") + codecName + ")";
      return;
    }
    longName = "VideoDecoder(";
    longName += codecName;
    if (codec && codec->name) { longName += " [" + std::string(codec->name) + "]"; }
    if (hwInfo.context && !hwInfo.deviceName.empty()) { longName += ", hw=" + hwInfo.deviceName; }
    longName += ")";
  }

  bool VideoDecoderNode::decodePacket(AVPacket *packet) {
    if (!packet || !codecCtx) {
      ERROR_MSG("Video Decoder: Invalid packet or decoder context");
      return false;
    }

    VERYHIGH_MSG("Video Decoder: Sending packet pts=%" PRId64 " dts=%" PRId64 " size=%d flags=0x%x", packet->pts,
                 packet->dts, packet->size, packet->flags);

    uint64_t pTime = Util::getMicros();

    // Send packet to decoder
    int ret = avcodec_send_packet(codecCtx, packet);
    if (ret < 0) {
      FFmpegUtils::printAvError("Video Decoder: Error sending packet to decoder", ret);
      return false;
    }

    // Insert input presentation time into timestamp queue
    inTimes.insert(packet->pts);

    // Receive decoded frame
    ret = avcodec_receive_frame(codecCtx, frame);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
      VERYHIGH_MSG("Video Decoder: Decoder not ready to output frame yet (ret=%s)", ret == AVERROR_EOF ? "EOF" : "EAGAIN");
      return true;
    } else if (ret < 0) {
      FFmpegUtils::printAvError("Video Decoder: Error receiving frame from decoder", ret);
      return false;
    }

    // Grab earliest presentation time from timestamp queue
    auto firstTime = inTimes.begin();
    frame->pts = *firstTime;
    inTimes.erase(firstTime);

    // Create frame context and add to outputs
    VideoFrameContext frameContext(frame); // Take ownership

    // Set frame properties
    frameContext.setSourceNodeId(getNodeId());
    frameContext.setHWDeviceName(hwInfo.deviceName);
    frameContext.setIsHWFrame(codecCtx->hw_frames_ctx);
    if (codecCtx->framerate.den && codecCtx->framerate.num) {
      frameContext.setFpks(codecCtx->framerate.num * 1000 / codecCtx->framerate.den);
    } else {
      frameContext.setFpks(fpks);
    }

    // Transfer hardware context if needed
    if (codecCtx->hw_frames_ctx) { frameContext.setHWContext(codecCtx->hw_frames_ctx); }

    // Set decode time
    totalProcessingTime.fetch_add(Util::getMicros(pTime));

    for (auto & cb : callbacks) { cb(&frameContext); }

    const char *fmtName = av_get_pix_fmt_name(static_cast<AVPixelFormat>(frame->format));
    const char *swFmtName =
      frame->hw_frames_ctx ? av_get_pix_fmt_name(((AVHWFramesContext *)frame->hw_frames_ctx->data)->sw_format) : nullptr;
    HIGH_MSG("Video Decoder: Produced frame pts=%" PRIi64 " hw=%s fmt=%s sw_fmt=%s", frameContext.getPts(),
             codecCtx->hw_frames_ctx ? "Y" : "N", fmtName ? fmtName : "unknown", swFmtName ? swFmtName : "n/a");

    return true;
  }

  bool VideoDecoderNode::init() {
    return true;
  }

  bool VideoDecoderNode::configureHardwareAcceleration() {
    if (!codecCtx || !codec) {
      ERROR_MSG("Video Decoder: Invalid codec context or codec");
      return false;
    }

    // Check if any hardware acceleration is enabled
    bool hwEnabled = config.allowNvidia || config.allowQsv || config.allowVaapi || config.allowMediaToolbox;
    MEDIUM_MSG("Video Decoder: Hardware acceleration check - NVIDIA=%d, QSV=%d, VAAPI=%d, "
               "MediaToolbox=%d, SW=%d",
               config.allowNvidia, config.allowQsv, config.allowVaapi, config.allowMediaToolbox, config.allowSW);

    if (!hwEnabled) {
      MEDIUM_MSG("Video Decoder: Hardware acceleration disabled, using software decoding");
      return config.allowSW;
    }

    auto supportsDeviceType = [this](AVHWDeviceType type) {
      const AVCodecHWConfig *hwConfig = nullptr;
      int index = 0;
      while ((hwConfig = avcodec_get_hw_config(codec, index++)) != nullptr) {
        if (hwConfig->device_type == type) { return true; }
      }
      return false;
    };

    HWContextManager & hwManager = HWContextManager::getInstance();
    auto devices = hwManager.getAvailableDevices();
    for (const auto & device : devices) {
      INFO_MSG("DEBUG: Video Decoder: HW device candidate type=%s hw_fmt=%s sw_fmt=%s path=%s",
               device.name.c_str(),
               safePixFmtName(device.hwFormat),
               safePixFmtName(device.swFormat),
               device.devicePath.empty() ? "default" : device.devicePath.c_str());
      AVHWDeviceType type = device.type;
      if (!supportsDeviceType(type)) {
        INFO_MSG("Video Decoder: '%s' does not support device type %s", codec->name, av_hwdevice_get_type_name(type));
        continue;
      }

      const char *devicePath = device.devicePath.empty() ? nullptr : device.devicePath.c_str();
      AVBufferRef *hwContext = hwManager.initDevice(type, devicePath);
      if (!hwContext) {
        WARN_MSG("Video Decoder: Failed to initialize device type %s", av_hwdevice_get_type_name(type));
        continue;
      }

      if (initHW(hwContext)) {
        MEDIUM_MSG("Video Decoder: Successfully configured hardware acceleration with %s", av_hwdevice_get_type_name(type));
        hwInfo.deviceName = device.devicePath.empty() ? av_hwdevice_get_type_name(type) : device.devicePath;
        codecCtx->pix_fmt = device.hwFormat;
        if (codecCtx->hw_frames_ctx){
          INFO_MSG("Pixel formats: %s/%s", av_get_pix_fmt_name(((AVHWFramesContext *)codecCtx->hw_frames_ctx->data)->format), av_get_pix_fmt_name(((AVHWFramesContext *)codecCtx->hw_frames_ctx->data)->sw_format));
        }else{
          INFO_MSG("Pixel format: %s", av_get_pix_fmt_name(codecCtx->pix_fmt));
        }
        INFO_MSG("DEBUG: Video Decoder: HW selected device=%s device_hw_fmt=%s device_sw_fmt=%s init_hw_fmt=%s init_sw_fmt=%s",
                 hwInfo.deviceName.c_str(),
                 safePixFmtName(device.hwFormat),
                 safePixFmtName(device.swFormat),
                 safePixFmtName(hwInfo.format),
                 safePixFmtName(hwInfo.swFormat));

        return true;
      }

      av_buffer_unref(&hwContext);
    }

    // If we get here, no hardware device worked
    if (!config.allowSW) {
      ERROR_MSG("Video Decoder: No hardware device available and software decoding not allowed");
      return false;
    }

    MEDIUM_MSG("Video Decoder: Using software decoding");
    return true;
  }

  bool VideoDecoderNode::isHWAccelerated() const {
    return hwInfo.context != nullptr;
  }

  AVBufferRef *VideoDecoderNode::getHWDeviceContext() const {
    if (!hwInfo.context) { return nullptr; }
    return av_buffer_ref(hwInfo.context);
  }

  AVBufferRef *VideoDecoderNode::getHWFramesContext() const {
    if (!codecCtx || !codecCtx->hw_frames_ctx) { return nullptr; }
    return av_buffer_ref(codecCtx->hw_frames_ctx);
  }

  AVHWDeviceType VideoDecoderNode::getHWDeviceType() const {
    if (!hwInfo.context) return AV_HWDEVICE_TYPE_NONE;
    AVHWDeviceContext *ctx = (AVHWDeviceContext *)hwInfo.context->data;
    return ctx ? ctx->type : AV_HWDEVICE_TYPE_NONE;
  }

  AVPixelFormat VideoDecoderNode::negotiatePixelFormat(AVCodecContext *avctx, const AVPixelFormat *pix_fmts) {
    INFO_MSG("DEBUG: Video Decoder: negotiatePixelFormat candidates=[%s] ctx_pix=%s ctx_sw=%s hw=%s sw=%s",
             formatPixFmtList(pix_fmts).c_str(),
             safePixFmtName(avctx->pix_fmt),
             safePixFmtName(avctx->sw_pix_fmt),
             safePixFmtName(hwInfo.format),
             safePixFmtName(hwInfo.swFormat));
    // If we have hardware context, try hardware format first
    if (hwInfo.context) {
      // Check if our desired hardware format is supported
      for (int i = 0; pix_fmts[i] != AV_PIX_FMT_NONE; i++) {
        if (pix_fmts[i] == hwInfo.format) {
          MEDIUM_MSG("Video Decoder: Using hardware pixel format: %s", av_get_pix_fmt_name(hwInfo.format));
          return hwInfo.format;
        }
      }
    }

    // Fall back to software format
    for (int i = 0; pix_fmts[i] != AV_PIX_FMT_NONE; i++) {
      if (pix_fmts[i] == hwInfo.swFormat) {
        MEDIUM_MSG("Video Decoder: Using software pixel format: %s", av_get_pix_fmt_name(hwInfo.swFormat));
        return hwInfo.swFormat;
      }
    }

    // If neither preferred format is available, use first supported format
    MEDIUM_MSG("Video Decoder: Using fallback pixel format: %s", av_get_pix_fmt_name(pix_fmts[0]));
    hwInfo.swFormat = pix_fmts[0];
    return pix_fmts[0];
  }

  AVPixelFormat VideoDecoderNode::getFormatCallback(AVCodecContext *ctx, const AVPixelFormat *pix_fmts) {
    VideoDecoderNode *decoder = static_cast<VideoDecoderNode *>(ctx->opaque);
    if (!decoder) { return pix_fmts[0]; }

    INFO_MSG("DEBUG: Video Decoder: getFormatCallback candidates=[%s] ctx_pix=%s ctx_sw=%s has_hw_ctx=%d",
             formatPixFmtList(pix_fmts).c_str(),
             safePixFmtName(ctx->pix_fmt),
             safePixFmtName(ctx->sw_pix_fmt),
             ctx->hw_frames_ctx ? 1 : 0);
    AVPixelFormat selected = decoder->negotiatePixelFormat(ctx, pix_fmts);

    // If we selected a hardware format, we need to set up the hardware frames context
    if (decoder->hwInfo.context && selected == decoder->hwInfo.format) {
      // Create hardware frames context using the format FFmpeg detected from the stream
      ctx->hw_frames_ctx = av_hwframe_ctx_alloc(decoder->hwInfo.context);
      if (!ctx->hw_frames_ctx) { return AV_PIX_FMT_NONE; }

      AVHWFramesContext *frames_ctx = (AVHWFramesContext *)ctx->hw_frames_ctx->data;
      frames_ctx->format = selected;
      frames_ctx->sw_format = ctx->sw_pix_fmt;  // Use format detected from stream
      frames_ctx->width = FFALIGN(ctx->coded_width, 32);
      frames_ctx->height = FFALIGN(ctx->coded_height, 32);
      frames_ctx->initial_pool_size = 32;

      MEDIUM_MSG("Video Decoder: Creating hardware frames context: %dx%d (aligned to %dx%d) sw_fmt=%s",
                 ctx->coded_width, ctx->coded_height, frames_ctx->width, frames_ctx->height,
                 safePixFmtName(frames_ctx->sw_format));

      int ret = av_hwframe_ctx_init(ctx->hw_frames_ctx);
      if (ret < 0) {
        FFmpegUtils::printAvError("Video Decoder: Failed to initialize hardware frames context", ret);
        av_buffer_unref(&ctx->hw_frames_ctx);
        return AV_PIX_FMT_NONE;
      }
    }

    HIGH_MSG("Video Decoder: getFormatCallback selected=%s", safePixFmtName(selected));
    return selected;
  }

  void VideoDecoderNode::setHardwareAcceleration(bool allowNvidia, bool allowQsv, bool allowVaapi,
                                                 bool allowMediaToolbox, bool allowSW, const std::string & devicePath) {
    std::lock_guard<std::mutex> lock(mutex);

    // Update hardware acceleration configuration
    config.allowNvidia = allowNvidia;
    config.allowQsv = allowQsv;
    config.allowVaapi = allowVaapi;
    config.allowMediaToolbox = allowMediaToolbox;
    config.allowSW = allowSW;

    VERYHIGH_MSG("Video Decoder: Hardware acceleration config updated: NVIDIA=%d, QSV=%d, VAAPI=%d, "
                 "MediaToolbox=%d, allowSW=%d, devicePath=%s",
                 allowNvidia, allowQsv, allowVaapi, allowMediaToolbox, allowSW, devicePath.c_str());
  }

} // namespace FFmpeg
