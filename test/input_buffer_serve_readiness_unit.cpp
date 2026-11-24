#include "input_buffer.h"

#include <mist/defines.h>
#include <mist/timing.h>

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <unistd.h>

class ReadinessProbe : public Mist::InputBuffer {
  public:
    bool visited = false, liveAtReady = false, stateReady = false;
    ReadinessProbe(Util::Config *cfg, const std::string & name, bool buffer) : Mist::InputBuffer(cfg) {
      streamName = name;
      capa["name"] = buffer ? "Buffer" : "MP4";
      meta.reInit("", true);
      meta.setLive(false);
      size_t t = meta.addTrack();
      meta.setID(t, 1);
      meta.setType(t, "video");
      meta.setCodec(t, "H264");
      meta.update(0, 0, t, 1, 0, true, 1);
      const std::string page = "/boot-ready-state-" + name;
      streamStatus.init(page.c_str(), STRMSTATE_PAGE_LEN, true, false);
      if (!streamStatus) {
        std::cerr << "fixture state page could not be created" << std::endl;
        std::exit(3);
      }
      memset(streamStatus.mapped, 0, streamStatus.len);
      cfg->is_active = true;
    }
    void startServing() { serve(); }
    bool keepRunning(bool = true) override { return !visited; }
    void userLeadIn() override {
      visited = true;
      liveAtReady = M.getLive();
      stateReady = streamStatus.mapped[0] == STRMSTAT_READY;
      config->is_active = false;
    }
    void userLeadOut() override {}
    void removeUnused() override {}
    void finish() override {}
};

int main() {
  char tmp[] = "/tmp/mist-buffer-ready-XXXXXX";
  if (!mkdtemp(tmp)) return 3;
  setenv("TMP", tmp, 1);
  setenv("UDP_API", "udp://127.0.0.1:0", 1);
  setenv("NOSESS", "1", 1);
  setenv("MIST_CONTROL", "1", 1);
  bool good = true;
  for (bool buffer : {false, true}) {
    Util::Config cfg("buffer-serve-readiness");
    ReadinessProbe input(&cfg, std::to_string(getpid()) + (buffer ? "buffer" : "vod"), buffer);
    input.startServing();
    if (!input.visited || !input.stateReady || input.liveAtReady != buffer) {
      std::cerr << "FAIL " << (buffer ? "Buffer" : "VoD") << " published READY with live=" << input.liveAtReady
                << " expected " << buffer << std::endl;
      good = false;
    } else
      std::cerr << "PASS " << (buffer ? "Buffer" : "VoD")
                << " metadata role is correct before the first reader callback" << std::endl;
  }
  rmdir(tmp);
  return good ? 0 : 1;
}
