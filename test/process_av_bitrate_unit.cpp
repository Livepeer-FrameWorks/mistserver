#include "../src/process/process_av_bitrate.h"

#include <iostream>

namespace {
  int fail(const char *message) {
    std::cerr << message << std::endl;
    return 1;
  }
} // namespace

int main() {
  using namespace Mist;

  if (defaultAVBitrate(true) != 2000000) { return fail("video keeps its 2 Mbit/s default"); }
  if (defaultAVBitrate(false) != 128000) {
    return fail("an audio process without a bitrate must default to 128 kbit/s");
  }
  if (opusBitrate(defaultAVBitrate(false)) != defaultAVBitrate(false)) {
    return fail("the audio default must be a bitrate Opus accepts as-is");
  }

  if (opusBitrate(0) != 500 || opusBitrate(499) != 500) { return fail("Opus bitrates below 500 clamp to 500"); }
  if (opusBitrate(96000) != 96000) { return fail("a supported Opus bitrate is kept"); }
  if (opusBitrate(256000) != 256000 || opusBitrate(2000000) != 256000) {
    return fail("Opus bitrates above 256 kbit/s clamp to the 256 kbit/s limit");
  }
  return 0;
}
