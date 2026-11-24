#ifdef MIST_READINESS_CMAF
#include "../src/output/output_cmaf.h"
using NativeOutput = Mist::OutCMAF;
#else
#include "../src/output/output_hls.h"
using NativeOutput = Mist::OutHLS;
#endif
#include <mist/stream.h>

#include <iostream>

class Probe : public NativeOutput {
  public:
    Probe(Socket::Connection & conn, Util::Config & cfg, JSON::Value & cap) : NativeOutput(conn, cfg, cap) {
      meta.reInit("", true);
      meta.setLive(true);
      meta.setMinimumFragmentDuration(1000);
      isInitialized = true;
    }
    size_t add(const std::string & codec, unsigned fragments) {
      const size_t track = meta.addTrack(32, 32, 1024, 2, true);
      meta.setID(track, track + 1);
      meta.setType(track, codec == "AAC" ? "audio" : "video");
      if (codec == "AAC") {
        meta.setRate(track, 48000);
        meta.setChannels(track, 2);
      }
      meta.setCodec(track, codec);
      meta.setWidth(track, 320);
      meta.setHeight(track, 180);
      for (unsigned ms = 0; ms < fragments * 1000; ms += 100) { meta.update(ms, 0, track, 32, 0, ms % 1000 == 0, 100); }
      return track;
    }
    void video(const std::string & value) { targetParams["video"] = value; }
    void details() {
      for (size_t track : meta.getValidTracks()) {
        std::cerr << meta.getCodec(track) << " id=" << track
                  << " fragments=" << DTSC::Fragments(meta.fragments(track)).getValidCount() << std::endl;
      }
      std::cerr << "global main=" << meta.mainTrack() << std::endl;
    }
};
bool runCase(unsigned mode) {
  Socket::Connection conn;
  Util::Config cfg("segmented-readiness-probe");
  JSON::Value cap;
  NativeOutput::init(&cfg, cap);
  Probe output(conn, cfg, cap);
  const bool expected = mode == 0 || mode == 1 || mode == 4;
  if (mode == 0) { output.add("H264", 12); }
  if (mode == 1) {
    output.add("JPEG", 1);
    output.add("H264", 12);
  }
  if (mode == 2) {
    output.add("JPEG", 12);
    output.add("H264", 1);
  }
  if (mode == 3) { output.add("JPEG", 12); }
  if (mode == 4) {
    output.add("JPEG", 1);
    output.add("H264", 1);
    output.add("AAC", 12);
    output.video("none");
  }
  if (mode == 5) {
    output.add("JPEG", 12);
    size_t slow = output.add("H264", 1);
    output.add("H264", 12);
    output.video(std::to_string(slow));
  }
  const bool ready = output.isReadyForPlay();
  output.details();
  if (ready != expected) {
    std::cerr << "FAIL selection/readiness case " << mode << ": ready=" << ready << " expected=" << expected << std::endl;
    return false;
  }
  std::cout << "PASS selection/readiness case " << mode << std::endl;
  return true;
}
int main() {
  DTSC::trackValidMask = TRACK_VALID_EXT_HUMAN;
  bool ok = true;
  for (unsigned mode = 0; mode < 6; ++mode) { ok = runCase(mode) && ok; }
  return ok ? 0 : 1;
}
