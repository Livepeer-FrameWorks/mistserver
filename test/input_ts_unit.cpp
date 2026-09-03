#include "../src/input/input_ts.h"

#include <cassert>
#include <cstdio>
#include <set>
#include <string>
#include <unistd.h>

namespace {

  std::string makeSinglePesTransportStream() {
    DTSC::Meta meta;
    meta.reInit("", true);
    const size_t track = meta.addTrack(101, 101, 16, 2, true);
    meta.setType(track, "video");
    meta.setCodec(track, "H264");
    std::set<size_t> selectedTracks;
    selectedTracks.insert(track);

    std::string result(TS::PAT, 188);
    result.append(TS::createPMT(selectedTracks, meta), 188);

    const char annexB[] = "\000\000\000\001\147\102\300\012\332\173\001\020\000\000\003\000\020"
                          "\000\000\003\003\040\361\042\152\000\000\000\001\150\316\017\310"
                          "\000\000\000\001\145\210\204";
    std::string pes;
    TS::Packet::getPESVideoLeadIn(pes, sizeof(annexB) - 1, 90000, 0, true);
    pes.append(annexB, sizeof(annexB) - 1);

    size_t offset = 0;
    uint8_t continuity = 0;
    while (offset < pes.size()) {
      TS::Packet packet;
      packet.setPID(TS::getUniqTrackID(meta, track));
      packet.setContinuityCounter(continuity++);
      packet.setUnitStart(offset == 0);
      offset += packet.fillFree(pes.data() + offset, pes.size() - offset);
      packet.addStuffing();
      result.append(packet.checkAndGetBuffer(), 188);
    }
    return result;
  }

  class InputTSProbe : public Mist::InputTS {
    public:
      explicit InputTSProbe(Util::Config *config) : Mist::InputTS(config) {}

      bool openFixture(const std::string & path) {
        standAlone = true;
        readPos = 0;
        return reader.open(path);
      }

      void bufferPostHeader() { postHeader(); }
      void nextPacket() { getNext(); }
      bool hasBufferedPacket() const { return tsStream.hasPacketOnEachTrack(); }
      bool hasCurrentPacket() const { return thisPacket; }
  };

} // namespace

int main() {
  const std::string fixture = makeSinglePesTransportStream();

  TS::Stream directStream;
  TS::Assembler assembler;
  assembler.assemble(directStream, fixture.data(), fixture.size(), true, 0);
  assert(!directStream.hasPacketOnEachTrack());
  directStream.finish();
  assert(directStream.hasPacketOnEachTrack());

  char path[] = "/tmp/mist-input-ts-unit-XXXXXX";
  const int descriptor = mkstemp(path);
  assert(descriptor >= 0);
  assert(write(descriptor, fixture.data(), fixture.size()) == (ssize_t)fixture.size());
  close(descriptor);

  Util::Config config("input-ts-unit");
  InputTSProbe input(&config);
  assert(input.openFixture(path));
  input.bufferPostHeader();
  assert(input.hasBufferedPacket());
  input.nextPacket();
  assert(input.hasCurrentPacket());

  Util::exitReason[0] = 0;
  input.nextPacket();
  assert(!input.hasCurrentPacket());
  assert(!Util::exitReason[0]);

  unlink(path);
  return 0;
}
