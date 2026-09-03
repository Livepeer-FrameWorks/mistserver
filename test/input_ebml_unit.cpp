#include "../src/input/input_ebml.h"

#include <cassert>
#include <cstdlib>
#include <unistd.h>

namespace {

  class InputEBMLProbe : public Mist::InputEBML {
    public:
      explicit InputEBMLProbe(Util::Config *config) : Mist::InputEBML(config) {
        meta.reInit("", true);
        const size_t track = meta.addTrack();
        meta.setID(track, 1);
        meta.setType(track, "audio");
        meta.setCodec(track, "PCM");
      }

      void setClock(bool live, int64_t bootOffset) {
        meta.setLive(live);
        meta.setBootMsOffset(bootOffset);
      }

      void queue(uint64_t timestamp, bool readyBeforeRead) {
        const char payload = 0;
        parser.packBuf[1].add(timestamp, 1, 1, 0, true, false, (void *)&payload);
        ++parser.bufferedPacks;
        if (readyBeforeRead) { parser.finish(); }
      }

      bool openSource(const char *path) {
        standAlone = true;
        return inFile.open(path);
      }

      uint64_t next() {
        getNext();
        assert(thisPacket);
        assert(thisPacket.getTime() == thisTime);
        return thisTime;
      }
  };

} // namespace

int main() {
  Util::Config config("input-ebml-unit");
  config.is_active = true;

  InputEBMLProbe vod(&config);
  vod.setClock(false, 1000);
  vod.queue(12345, true);
  assert(vod.next() == 12345);

  InputEBMLProbe live(&config);
  const int64_t bootOffset = 1000;
  live.setClock(true, bootOffset);
  const uint64_t before = Util::bootMS() - bootOffset;
  const uint64_t rawFirst = Util::bootMS() + 5000;
  live.queue(rawFirst, true);
  const uint64_t first = live.next();
  const uint64_t after = Util::bootMS() - bootOffset;
  assert(first >= before);
  assert(first <= after);

  live.queue(rawFirst + 150, true);
  const uint64_t second = live.next();
  assert(second == first + 150);

  InputEBMLProbe eofDrain(&config);
  eofDrain.setClock(true, bootOffset);
  char emptyPath[] = "/tmp/mist-input-ebml-unit-XXXXXX";
  const int descriptor = mkstemp(emptyPath);
  assert(descriptor >= 0);
  const char incompleteElement = 0;
  assert(write(descriptor, &incompleteElement, 1) == 1);
  close(descriptor);
  assert(eofDrain.openSource(emptyPath));
  const uint64_t eofBefore = Util::bootMS() - bootOffset;
  eofDrain.queue(Util::bootMS() + 5000, false);
  const uint64_t eofPacket = eofDrain.next();
  const uint64_t eofAfter = Util::bootMS() - bootOffset;
  assert(eofPacket >= eofBefore);
  assert(eofPacket <= eofAfter);
  unlink(emptyPath);
  return 0;
}
