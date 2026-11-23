#define main avProcessorMain
#include "../src/process/process_av.cpp"
#undef main

#include <cstdio>
#ifndef _WIN32
#include <execinfo.h>
#include <signal.h>
static void trace(int signal) {
  void *frames[40];
  int count = backtrace(frames, 40);
  backtrace_symbols_fd(frames, count, 2);
  _exit(128 + signal);
}
#endif
class EncoderProbe : public Mist::ProcessSource {
  public:
    using Mist::ProcessSource::ProcessSource;
    void track() {
      meta.reInit("", true);
      thisIdx = meta.addTrack();
      meta.setID(thisIdx, 1);
      meta.setType(thisIdx, "video");
      meta.setCodec(thisIdx, "H264");
      meta.setWidth(thisIdx, 320);
      meta.setHeight(thisIdx, 180);
      meta.setFpks(thisIdx, 10000);
      meta.update(0, 0, thisIdx, 4, 0, true, 100);
      userSelect[thisIdx];
    }
};
int main() {
  if (!getenv("MIST_TEST_HW_DEVICE") || std::string(getenv("MIST_TEST_HW_DEVICE")) != "cuda") { return 77; }
#ifndef _WIN32
  signal(SIGSEGV, trace);
  signal(SIGABRT, trace);
#endif
  isVideo = true;
  codecOut = "H264";
  Mist::opt["resolution"] = "16x16";
  Mist::opt["bitrate"] = 300000;
  Mist::opt["gopsize"] = 10;
  Mist::opt["preset"] = "veryfast";
  Mist::opt["tune"] = "zerolatency";
  Util::Config config("encoder-rejection-test");
  JSON::Value capa;
  Mist::ProcessSource::init(&config, capa);
  Socket::Connection connection;
  EncoderProbe source(connection, config, capa);
  source.track();
  for (int i = 0; i < 20; ++i) {
    if (source.tryEncoder("h264_nvenc", AV_HWDEVICE_TYPE_CUDA, AV_PIX_FMT_CUDA, AV_PIX_FMT_YUV420P)) {
      fprintf(stderr, "Expected NVENC to reject a 16x16 frame\n");
      return 1;
    }
    if (context_out || frameInHW) {
      fprintf(stderr, "Rejected encoder retained context or frame\n");
      return 1;
    }
  }
  puts("PASS NVENC rejects unsupported dimensions 20 times without crashing or retaining encoder frames");
}
