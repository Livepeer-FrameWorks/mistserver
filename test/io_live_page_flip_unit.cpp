#include "../src/io.h"

#include <cstdio>
#include <stdexcept>
#include <string>
#include <unistd.h>

// A provider input buffers into the stream's live meta, whose track indexes
// differ from its own file meta. The live-page flip must read keys from the
// meta it is buffering into.
class PageFlipHarness : public Mist::InOutBase {
  public:
    DTSC::Meta liveMeta;

    explicit PageFlipHarness(const std::string & name) {
      streamName = name;
      standAlone = true;
      meta.reInit(name + "_file", true);
      size_t own = meta.addTrack();
      meta.setType(own, "video");
      meta.setCodec(own, "H264");

      liveMeta.reInit(name, true);
      for (int i = 0; i < 3; ++i) {
        size_t idx = liveMeta.addTrack();
        liveMeta.setType(idx, i ? "audio" : "video");
        liveMeta.setCodec(idx, i ? "AAC" : "H264");
        liveMeta.setID(idx, i + 1);
      }
    }
};

int main() {
  std::string name = "ioflip" + std::to_string(getpid());
  PageFlipHarness h(name);
  const char payload[16] = {0};
  const uint32_t track = 2; // exists only in the live meta
  try {
    h.bufferLivePacket(0, 0, track, payload, sizeof(payload), 0, true, h.liveMeta);
    h.bufferLivePacket(1000, 0, track, payload, sizeof(payload), 0, true, h.liveMeta);
    // More than FLIP_TARGET_DURATION later: forces a live page transition.
    h.bufferLivePacket(FLIP_TARGET_DURATION + 2000, 0, track, payload, sizeof(payload), 0, true, h.liveMeta);
  } catch (const std::exception & e) {
    fprintf(stderr, "FAIL: live page flip read the wrong meta: %s\n", e.what());
    return 1;
  }
  const Util::RelAccX & pages = h.liveMeta.pages(track);
  if (pages.getEndPos() - pages.getDeleted() < 2) {
    fprintf(stderr, "FAIL: expected a second live page after the flip, have %zu\n",
            (size_t)(pages.getEndPos() - pages.getDeleted()));
    return 1;
  }
  fprintf(stderr, "live page flip: OK\n");
  return 0;
}
