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

namespace {
  class ExitProbe : public Mist::ProcessSource {
    public:
      using Mist::ProcessSource::determineExitReason;
      using Mist::ProcessSource::ProcessSource;
      using Mist::ProcessSource::statComm;
  };

  int checkSourceStop(const std::string & mode) {
    DTSC::trackValidMask = TRACK_VALID_INT_PROCESS;
    Util::Config config("av-source-stop-test");
    config.is_active = true;
    JSON::Value capa;
    Mist::ProcessSource::init(&config, capa);
    const std::string session = "avstop" + std::to_string(getpid());
    char name[NAME_BUFFER_SIZE];
    snprintf(name, sizeof(name), SEM_SESSION, session.c_str());
    IPC::semaphore lock(name, O_CREAT | O_RDWR, ACCESSPERMS, 1);
    int result = 0;
    {
      Comms::Connections owner;
      owner.reload(session, true);
      Socket::Connection connection;
      ExitProbe source(connection, config, capa);
      source.statComm.reload(session);
      if (!owner || !source.statComm) { return 2; }
      source.statComm.setStatus(COMM_STATUS_ACTIVE | COMM_STATUS_REQDISCONNECT | (mode == "external-stop" ? 0 : COMM_STATUS_SESSSTOP));
      if (mode == "failure-then-stop") { Util::logExitReason(ER_PROCESS_SPECIFIC, "decoder failed"); }
      if (mode == "source-signal") { Util::logExitReason(ER_CLEAN_SIGNAL, "source received signal"); }
      if (mode == "eof-then-stop") { Util::logExitReason(ER_CLEAN_EOF, "source finished"); }
      std::thread signalThread([]() { Util::Config::requestStop(); });
      signalThread.join();
      source.determineExitReason();
      const char *expected = mode == "external-stop" ? ER_CLEAN_SIGNAL
        : mode == "failure-then-stop"                ? ER_PROCESS_SPECIFIC
                                                     : ER_CLEAN_CONTROLLER_REQ;
      if (!Util::mRExitReason || std::string(Util::mRExitReason) != expected) {
        fprintf(stderr, "AV source stop mode=%s expected=%s actual=%s reason=%s\n", mode.c_str(), expected,
                Util::mRExitReason ? Util::mRExitReason : "none", Util::exitReason);
        result = 1;
      }
    }
    lock.unlink();
    if (!result) {
      puts("PASS actual AV source preserves stop classification across threads without masking failures");
    }
    return result;
  }
} // namespace

int main(int argc, char **argv) {
  if (argc == 2) { return checkSourceStop(argv[1]); }
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
