#include "video_frame_context.h"

#include "../defines.h"
#include "../timing.h"
#include "utils.h"

#include <stdexcept>
extern "C" {
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
}

namespace FFmpeg {

  VideoFrameContext::VideoFrameContext(AVFrame *f){
    hwContext = 0;
    swFrame = 0;
    hwFormat = AV_PIX_FMT_NONE;
    swFormat = AV_PIX_FMT_NONE;
    isHWFrame = false;
    isRawMode = false;
    duration = 0;
    isKeyFrame = false;
    frame = f;
    if (!frame) {
      frame = av_frame_alloc();
      if (!frame) {
        ERROR_MSG("Video Frame Context: Failed to allocate frame");
        throw std::runtime_error("Video Frame Context: Failed to allocate frame");
        return;
      }
    }
    width = frame->width;
    height = frame->height;
    pts = frame->pts;
    pixelFormat = (AVPixelFormat)frame->format;
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 30, 100)
    duration = frame->duration;
    // Fix FFmpeg 7.x compatibility - use flags instead of deprecated key_frame field
    isKeyFrame = (frame->flags & AV_FRAME_FLAG_KEY) != 0;
#else
    duration = frame->pkt_duration;
    // For older FFmpeg versions, use the key_frame field
    isKeyFrame = frame->key_frame;
#endif
    if (frame->hw_frames_ctx) {
      isHWFrame = true;
      auto hwFramesCtx = reinterpret_cast<AVHWFramesContext *>(frame->hw_frames_ctx->data);
      hwFormat = hwFramesCtx->format;
      swFormat = hwFramesCtx->sw_format;
      hwDeviceName = av_hwdevice_get_type_name(hwFramesCtx->device_ctx->type);
      VERYHIGH_MSG("Video Frame Context: Hardware frame detected - hwFormat=%d, swFormat=%d, device=%s, frame=%p",
                   hwFormat, swFormat, hwDeviceName.c_str(), frame);
    }
  }

  VideoFrameContext::~VideoFrameContext() {
    VERYHIGH_MSG("VideoFrameContext: Frame context destroyed, frame %p freed", frame);
    if (hwContext) { av_buffer_unref(&hwContext); }
  }

  AVPixelFormat VideoFrameContext::getHWFormat() const {
    if (!frame || !frame->hw_frames_ctx) { return AV_PIX_FMT_NONE; }
    AVHWFramesContext *hwCtx = (AVHWFramesContext *)frame->hw_frames_ctx->data;
    VERYHIGH_MSG("Video Frame Context: Hardware pixel format: %d", hwCtx->format);
    return hwCtx->format;
  }

  AVPixelFormat VideoFrameContext::getSWFormat() const {
    if (!frame) { return AV_PIX_FMT_NONE; }
    AVPixelFormat fmt;
    if (frame->hw_frames_ctx) {
      AVHWFramesContext *hwCtx = (AVHWFramesContext *)frame->hw_frames_ctx->data;
      fmt = hwCtx->sw_format;
    } else {
      fmt = (AVPixelFormat)frame->format;
    }
    VERYHIGH_MSG("Video Frame Context: Software pixel format: %d", fmt);
    return fmt;
  }

  bool VideoFrameContext::transferFromHW() {
    if (!frame || !frame->hw_frames_ctx) {
      ERROR_MSG("Video Frame Context: No hardware frame to transfer from");
      return false;
    }

    VERYHIGH_MSG("Video Frame Context: Transferring frame from hardware to software memory");

    // Get hardware context info
    AVHWFramesContext *hwCtx = (AVHWFramesContext *)frame->hw_frames_ctx->data;

    // Create software frame if needed
    if (!swFrame) {
      swFrame = av_frame_alloc();
      if (!swFrame) {
        ERROR_MSG("Video Frame Context: Failed to allocate software frame");
        return false;
      }
    }

    // Verify format compatibility
    if (swFrame->format != hwCtx->sw_format) {
      ERROR_MSG("Video Frame Context: Software frame format mismatch: expected %d, got %d", hwCtx->sw_format, swFrame->format);
      return false;
    }

    // Track transfer start time
    uint64_t startTime = Util::getMicros();

    // Transfer from hardware to software
    int ret = av_hwframe_transfer_data(swFrame, frame, 0);
    if (ret < 0) {
      FFmpegUtils::printAvError("Video Frame Context: Failed to transfer frame data from hardware", ret);
      return false;
    }

    // Copy properties
    av_frame_copy_props(swFrame, frame);

    // Track transfer time
    uint64_t transferTime = Util::getMicros(startTime);

    VERYHIGH_MSG("Video Frame Context: Successfully transferred frame to software memory in %" PRIu64 " us", transferTime);
    return true;
  }

  bool VideoFrameContext::transferToHW(AVBufferRef *hwFramesCtx) {
    if (!frame || !hwFramesCtx) {
      ERROR_MSG("Video Frame Context: Invalid frame or hardware context for transfer");
      return false;
    }

    VERYHIGH_MSG("Video Frame Context: Transferring frame to hardware memory");

    // Get hardware context info
    AVHWFramesContext *hwCtx = (AVHWFramesContext *)hwFramesCtx->data;

    // Verify format compatibility
    if (frame->format != hwCtx->sw_format) {
      ERROR_MSG("Video Frame Context: Input frame format incompatible with hardware: expected %d, got %d",
                hwCtx->sw_format, frame->format);
      return false;
    }

    // Get a pooled hardware frame
    AVFrame *hwFrame = av_frame_alloc();
    if (!hwFrame) {
      ERROR_MSG("Video Frame Context: Failed to allocate hardware frame");
      return false;
    }
    hwFrame->width = hwCtx->width;
    hwFrame->height = hwCtx->height;
    hwFrame->format = hwCtx->format;

    // Set hardware frames context
    hwFrame->hw_frames_ctx = av_buffer_ref(hwFramesCtx);
    if (!hwFrame->hw_frames_ctx) {
      ERROR_MSG("Video Frame Context: Failed to reference hardware context");
      return false;
    }

    // Track transfer start time
    uint64_t startTime = Util::getMicros();

    // Transfer from software to hardware
    int ret = av_hwframe_transfer_data(hwFrame, frame, 0);
    if (ret < 0) {
      FFmpegUtils::printAvError("Video Frame Context: Failed to transfer frame data to hardware", ret);
      return false;
    }

    // Copy properties
    av_frame_copy_props(hwFrame, frame);

    // Replace software frame with hardware frame
    if (frame) { av_frame_free(&frame); }
    frame = hwFrame;
    isHWFrame = true;

    // Update hardware context
    if (hwContext) { av_buffer_unref(&hwContext); }
    hwContext = av_buffer_ref(hwFramesCtx);

    // Track transfer time
    uint64_t transferTime = Util::getMicros(startTime);

    // Update device info
    hwDeviceName = av_hwdevice_get_type_name(hwCtx->device_ctx->type);
    hwFormat = hwCtx->format;
    swFormat = hwCtx->sw_format;

    VERYHIGH_MSG("Video Frame Context: Successfully transferred frame to hardware memory in %" PRIu64 " us", transferTime);
    return true;
  }

  void VideoFrameContext::reset() {
    // Clean up existing frame if we own it
    if (frame) { av_frame_free(&frame); }
    if (swFrame) { av_frame_free(&swFrame); }
    if (hwContext) { av_buffer_unref(&hwContext); }

    // Reset state
    frame = nullptr;
    swFrame = nullptr;
    hwContext = nullptr;
    hwFormat = AV_PIX_FMT_NONE;
    swFormat = AV_PIX_FMT_NONE;
    isHWFrame = false;
    isRawMode = false;
    rawData.allocate(0);
    width = 0;
    height = 0;
    pixelFormat = AV_PIX_FMT_NONE;
    sourceNodeId = 0;

    VERYHIGH_MSG("Video Frame Context: Reset video frame context");
  }

  const uint8_t *VideoFrameContext::getData() const {
    if (isRawMode) { return reinterpret_cast<const uint8_t *>((const char *)rawData); }
    if (swFrame) {
      return swFrame->data[0];
    } else if (frame) {
      return frame->data[0];
    }
    return nullptr;
  }

  int VideoFrameContext::getSize() const {
    if (isRawMode) { return rawData.size(); }
    if (swFrame) {
      return swFrame->linesize[0] * swFrame->height;
    } else if (frame) {
      return frame->linesize[0] * frame->height;
    }
    return 0;
  }

  std::unique_ptr<VideoFrameContext> VideoFrameContext::clone() const {
    if (!frame) { return nullptr; }

    AVFrame *newFrame = av_frame_clone(frame);
    if (!newFrame) { return nullptr; }

    try {
      return std::unique_ptr<VideoFrameContext>(new VideoFrameContext(newFrame));
    } catch (...) {
      av_frame_free(&newFrame);
      return nullptr;
    }
  }

  bool VideoFrameContext::isHWCompatible(const VideoFrameContext & other) const {
    if (!frame || !other.frame || !frame->hw_frames_ctx || !other.frame->hw_frames_ctx) { return false; }

    AVHWFramesContext *thisCtx = (AVHWFramesContext *)frame->hw_frames_ctx->data;
    AVHWFramesContext *otherCtx = (AVHWFramesContext *)other.frame->hw_frames_ctx->data;

    return thisCtx->device_ctx == otherCtx->device_ctx && thisCtx->format == otherCtx->format &&
      thisCtx->sw_format == otherCtx->sw_format;
  }

  bool VideoFrameContext::createRawFrame(const uint8_t *data, size_t size, uint32_t w, uint32_t h, AVPixelFormat fmt,
                                         const int32_t *stride) {
    if (!data || !size || !w || !h || fmt == AV_PIX_FMT_NONE) {
      ERROR_MSG("Video Frame Context: Invalid raw frame parameters");
      return false;
    }

    // Store frame parameters
    width = w;
    height = h;
    pixelFormat = fmt;

    // Allocate and copy raw data
    if (!rawData.assign(data, size)) {
      ERROR_MSG("Video Frame Context: Failed to allocate raw data buffer");
      return false;
    }
    isRawMode = true;

    frame->width = w;
    frame->height = h;
    frame->format = fmt;

    // Allocate buffer
    int ret = av_frame_get_buffer(frame, 0);
    if (ret < 0) {
      av_frame_free(&frame);
      frame = nullptr;
      FAIL_MSG("Could not allocate RAW buffer");
      return false;
    }

    av_image_fill_linesizes(frame->linesize, pixelFormat, frame->width);
    // Load pixel data into buffer
    av_image_fill_pointers(frame->data, pixelFormat, frame->height, (uint8_t *)(char *)rawData, frame->linesize);

    return true;
  }

  bool VideoFrameContext::getRawData(const uint8_t *& data, size_t & size, uint32_t & w, uint32_t & h,
                                     AVPixelFormat & fmt, int32_t *stride) const {
    if (!isRawMode) { return false; }

    data = reinterpret_cast<const uint8_t *>((const char *)rawData);
    size = rawData.size();
    w = width;
    h = height;
    fmt = pixelFormat;
    if (stride) {
      // Calculate linesize on-demand
      stride[0] = width * av_get_bits_per_pixel(av_pix_fmt_desc_get(fmt)) / 8;
      for (int i = 1; i < AV_NUM_DATA_POINTERS; i++) { stride[i] = stride[0]; }
    }

    return true;
  }

} // namespace FFmpeg
