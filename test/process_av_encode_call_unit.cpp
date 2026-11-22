// Redirect only this translation unit's boundary calls, leaving static libav symbols intact.
#define av_hwframe_get_buffer testAcquireHardwareFrame
#define av_hwframe_transfer_data testTransferHardwareFrame
#define avcodec_send_frame testSubmitEncoderFrame
#define main avProcessorMain
#include "../src/process/process_av.cpp"
#undef main
#undef av_hwframe_get_buffer
#undef av_hwframe_transfer_data
#undef avcodec_send_frame

#include <cstdio>

namespace {
  int acquires = 0, transfers = 0, sends = 0;
}

extern "C" int testAcquireHardwareFrame(AVBufferRef *pool, AVFrame *frame, int flags) {
  ++acquires;
  frame->format = AV_PIX_FMT_CUDA;
  frame->hw_frames_ctx = av_buffer_ref(pool);
  return frame->hw_frames_ctx ? 0 : AVERROR(ENOMEM);
}
extern "C" int testTransferHardwareFrame(AVFrame *to, const AVFrame *from, int flags) {
  ++transfers;
  if (flags || to->format != AV_PIX_FMT_CUDA || from->format != AV_PIX_FMT_CUDA ||
      to->hw_frames_ctx->data == from->hw_frames_ctx->data) {
    return AVERROR(EINVAL);
  }
  return AVERROR(EIO);
}
extern "C" int testSubmitEncoderFrame(AVCodecContext *, const AVFrame *) {
  ++sends;
  return AVERROR(EAGAIN);
}

int main() {
  DTSC::trackValidMask = TRACK_VALID_INT_PROCESS;
  Util::Config config("av-encode-call-test");
  JSON::Value capa;
  Mist::ProcessSource::init(&config, capa);
  Socket::Connection connection;
  {
    Mist::ProcessSource source(connection, config, capa);
    context_out = avcodec_alloc_context3(0);
    context_out->pix_fmt = AV_PIX_FMT_CUDA;
    context_out->hw_frames_ctx = av_buffer_allocz(sizeof(AVHWFramesContext));
    frameConverted = av_frame_alloc();
    frameConverted->format = AV_PIX_FMT_CUDA;
    frameConverted->hw_frames_ctx = av_buffer_allocz(sizeof(AVHWFramesContext));
    frameInHW = av_frame_alloc();
    config.is_active = true;
    source.encodeVideo();
    if (acquires != 1 || transfers != 1 || sends || config.is_active || !Util::mRExitReason ||
        std::string(Util::mRExitReason) != ER_PROCESS_SPECIFIC) {
      fprintf(stderr, "encodeVideo must stop with a process failure after a hardware transfer error: acquire=%d transfer=%d send=%d active=%d reason=%s\n",
              acquires, transfers, sends, config.is_active.load(), Util::mRExitReason ? Util::mRExitReason : "none");
      return 1;
    }
  }
  av_frame_free(&frameConverted);
  av_frame_free(&frameInHW);
  avcodec_free_context(&context_out);
  puts("PASS actual encodeVideo rejects failed hardware upload before encoder submission with an explicit exit reason");
  return 0;
}
