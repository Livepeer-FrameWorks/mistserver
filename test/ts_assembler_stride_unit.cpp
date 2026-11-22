#include <mist/ts_stream.h>

#include <cassert>
#include <cstdlib>
#include <iostream>
#include <new>
#include <string>
#include <vector>

namespace {
  thread_local bool trackAllocation = false;
  thread_local size_t largestAllocation = 0;
} // namespace
void *operator new(size_t bytes) {
  if (trackAllocation && bytes > largestAllocation) { largestAllocation = bytes; }
  void *memory = std::malloc(bytes);
  if (!memory) { throw std::bad_alloc(); }
  return memory;
}
void operator delete(void *memory) noexcept {
  std::free(memory);
}

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

} // namespace

int main() {
  {
    std::string packet(188, '\xff');
    packet[0] = 0x47;
    packet[1] = 0x1f;
    packet[2] = '\xff';
    packet[3] = 0x10;
    std::string warm = packet + packet + packet, bulk;
    for (size_t i = 0; i < 8192; ++i) { bulk += packet; }
    TS::Stream stream;
    TS::Assembler assembler;
    assembler.assemble(stream, warm.data(), warm.size(), true, 0);
    largestAllocation = 0;
    trackAllocation = true;
    assembler.assemble(stream, bulk.data(), bulk.size(), true, warm.size());
    trackAllocation = false;
    if (largestAllocation > 16 * 1024) {
      std::cerr << "aligned transport input copied into a bulk allocation: " << largestAllocation << " bytes" << std::endl;
      return 1;
    }
  }
  const std::string transport = makeSinglePesTransportStream();
  std::string m2ts;
  for (size_t pos = 0; pos < transport.size(); pos += 188) {
    m2ts.append("\x47\x10\x00\x10", 4);
    m2ts.append(transport, pos, 188);
  }
  {
    bool failed = false;
    std::string nullPacket(188, '\xff');
    nullPacket[0] = 0x47;
    nullPacket[1] = 0x1f;
    nullPacket[2] = '\xff';
    nullPacket[3] = 0x10;
    const std::string arrival("\x47\x10\x00\x10", 4);
    const std::string warm = arrival + nullPacket + arrival + nullPacket + arrival + nullPacket;
    for (size_t chunk : {size_t(1), size_t(192), size_t(577)}) {
      TS::Stream stream;
      TS::Assembler assembler;
      assembler.assemble(stream, warm.data(), warm.size(), true, 0);
      const std::string resumed = std::string("bad-sync") + m2ts;
      for (size_t pos = 0; pos < resumed.size(); pos += chunk) {
        assembler.assemble(stream, resumed.data() + pos, std::min(chunk, resumed.size() - pos), true, warm.size() + pos);
      }
      assembler.assemble(stream, "", 0, true, warm.size() + resumed.size());
      stream.finish();
      if (!stream.hasPacketOnEachTrack()) {
        std::cerr << "M2TS arrival timestamps stole synchronization after junk with chunk " << chunk << std::endl;
        failed = true;
      }
    }
    std::string invalid = nullPacket;
    invalid[3] = 0; // Reserved adaptation control is a damaged packet, not a new packet boundary.
    const std::string damaged = transport.substr(0, 188) + invalid + transport.substr(188);
    TS::Stream stream;
    TS::Assembler assembler;
    assembler.assemble(stream, damaged.data(), damaged.size(), true, 0);
    assembler.assemble(stream, "", 0, true, damaged.size());
    stream.finish();
    if (!stream.hasPacketOnEachTrack()) {
      std::cerr << "Reserved adaptation control discarded the preceding valid PAT" << std::endl;
      failed = true;
    }
    if (failed) { return 1; }
  }
  for (const std::string & fixture : std::vector<std::string>{transport, m2ts}) {
    const size_t stride = fixture.size() == transport.size() ? 188 : 192;
    for (size_t packets : {size_t(1), size_t(2)}) {
      TS::Stream shortStream;
      TS::Assembler shortAssembler;
      shortAssembler.assemble(shortStream, fixture.data(), packets * stride, true, 0);
      if (!shortAssembler.assemble(shortStream, "", 0, true, packets * stride) || shortAssembler.packetSize() != stride) {
        std::cerr << "EOF discarded " << packets << " complete transport packets with stride " << stride << std::endl;
        return 1;
      }
      TS::Stream resynced;
      TS::Assembler recovering;
      recovering.assemble(resynced, fixture.data(), fixture.size(), true, 0);
      const std::string tail = std::string("bad-sync") + fixture.substr(0, packets * stride);
      recovering.assemble(resynced, tail.data(), tail.size(), true, fixture.size());
      if (!recovering.assemble(resynced, "", 0, true, fixture.size() + tail.size())) {
        std::cerr << "EOF discarded the last " << packets << " packets after resynchronization (stride " << stride
                  << ")" << std::endl;
        return 1;
      }
    }
  }
  for (const std::string & fixture : std::vector<std::string>{transport, m2ts}) {
    for (size_t chunk : std::vector<size_t>{1, 4, 187, 188, 191, 192, 193, 577}) {
      TS::Stream stream;
      TS::Assembler assembler;
      for (size_t pos = 0; pos < fixture.size(); pos += chunk) {
        assembler.assemble(stream, fixture.data() + pos, std::min(chunk, fixture.size() - pos), true, pos);
      }
      stream.finish();
      if (!stream.hasPacketOnEachTrack()) {
        std::cerr << "No parsed video with stride " << (fixture.size() == transport.size() ? 188 : 192) << " and chunk "
                  << chunk << std::endl;
        return 1;
      }
      DTSC::Packet packet;
      stream.getEarliestPacket(packet);
      const uint64_t expected = fixture.size() == transport.size() ? 376 : 384;
      if (packet.getInt("bpos") != expected) {
        std::cerr << "Seek position is " << packet.getInt("bpos") << " rather than packet boundary " << expected << std::endl;
        return 1;
      }
    }
  }
  std::cout << "PASS short TS/M2TS EOF, resynchronization tails, and all chunk boundaries" << std::endl;
  return 0;
}
