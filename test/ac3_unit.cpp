#include <mist/ac3.h>
#include <mist/bitstream.h>
#include <mist/cmaf.h>
#include <mist/ebml_socketglue.h>
#include <mist/mp4_generic.h>
#include <mist/ts_stream.h>

#include <cassert>
#include <iostream>

static std::string enhanced() {
  Utils::bitWriter b;
  b.append(0x0B77, 16);
  b.append(0, 2);
  b.append(0, 3);
  b.append(127, 11);
  b.append(0, 2);
  b.append(3, 2);
  b.append(7, 3);
  b.append(1, 1);
  b.append(16, 5);
  b.append(0, 5);
  b.append(0, 1);
  b.append(0, 1);
  b.append(1, 1);
  b.append(3, 3);
  std::string frame = b.str();
  frame.resize(256);
  return frame;
}
static bool recognized(const DTSC::Meta & metadata, int type, const std::string & descriptors, bool bluray, bool extension = false) {
  std::set<size_t> selected{0};
  TS::Packet original;
  original.FromPointer(TS::createPMT(selected, metadata));
  TS::ProgramMappingTable pmt;
  pmt = original;
  TS::ProgramMappingEntry entry = pmt.getEntry(0);
  const int previousLength = entry.getESInfoLength();
  entry.setStreamType(type);
  entry.setESInfo(descriptors);
  pmt.setSectionLength(pmt.getSectionLength() - previousLength + descriptors.size());
  pmt.calcCRC();
  TS::Stream stream;
  stream.setM2TS(bluray);
  TS::Packet pat;
  pat.FromPointer(TS::PAT);
  stream.parse(pat, 0);
  stream.parse(pmt, 188);
  const std::string frame = enhanced();
  std::string pes;
  TS::Packet::getPESAudioLeadIn(pes, frame.size(), 90000, 64000);
  if (extension) {
    const size_t length = ((unsigned char)pes[4] << 8) | (unsigned char)pes[5];
    pes[3] = 0xFD;
    pes[7] |= 1;
    pes[8] += 3;
    pes[4] = (length + 3) >> 8;
    pes[5] = length + 3;
    pes.append("\x01\x81\x71", 3);
  }
  pes += frame;
  size_t offset = 0;
  unsigned int continuity = 0;
  while (offset < pes.size()) {
    TS::Packet packet;
    packet.setPID(TS::getUniqTrackID(metadata, 0));
    packet.setContinuityCounter(continuity++);
    packet.setUnitStart(offset == 0);
    offset += packet.fillFree(pes.data() + offset, pes.size() - offset);
    packet.addStuffing();
    stream.parse(packet, 188 * (1 + continuity));
  }
  stream.finish();
  if (!stream.hasPacket()) { return false; }
  DTSC::Meta parsed;
  parsed.reInit("", true);
  stream.initializeMetadata(parsed);
  const auto tracks = parsed.getValidTracks();
  return !extension || (!tracks.empty() && parsed.getCodec(*tracks.begin()) == "EAC-3");
}

static bool activePrivateTrack(const DTSC::Meta & meta, int type, bool bluray) {
  TS::Packet original;
  std::set<size_t> selected{0};
  original.FromPointer(TS::createPMT(selected, meta));
  TS::ProgramMappingTable pmt;
  pmt = original;
  pmt.getEntry(0).setStreamType(type);
  pmt.calcCRC();
  TS::Stream stream;
  stream.setM2TS(bluray);
  TS::Packet pat;
  pat.FromPointer(TS::PAT);
  stream.parse(pat, 0);
  stream.parse(pmt, 188);
  return stream.getActiveTracks(true).count(TS::getUniqTrackID(meta, 0));
}

int main(int argc, char **argv) {
  if (argc > 1 && std::string(argv[1]) == "pes-extension") {
    DTSC::Meta metadata;
    metadata.reInit("", true);
    const size_t track = metadata.addTrack();
    metadata.setType(track, "audio");
    metadata.setCodec(track, "EAC-3");
    metadata.setRate(track, 48000);
    metadata.setChannels(track, 6);
    const auto h = AC3::parse(enhanced().data(), enhanced().size());
    metadata.setInit(track, AC3::makeDEC3(h));
    if (!recognized(metadata, 0x84, "", true, true)) {
      std::cerr << "Blu-ray extended PES lost its E-AC3 track" << std::endl;
      return 1;
    }
    return 0;
  }
  if (argc > 1 && std::string(argv[1]) == "aggregate") {
    // ETSI TS 102 366 E.1.4 is MSB-first; F.6.1 explicitly numbers LFE2 as bit 8 (MSB).
    const unsigned int chanmapBits[] = {10, 9, 8, 7, 6, 5, 4, 3, 1};
    const unsigned int channels[] = {2, 2, 1, 1, 2, 2, 2, 1, 1};
    const char *names[] = {"Lc/Rc", "Lrs/Rrs", "Cs", "Ts", "Lsd/Rsd", "Lw/Rw", "Vhl/Vhr", "Vhc", "LFE2"};
    bool failed = false;
    for (size_t location = 0; location < 9; ++location) {
      Utils::bitWriter b;
      b.append(0x0B77, 16); b.append(1, 2); b.append(0, 3); b.append(127, 11);
      b.append(0, 2); b.append(3, 2); b.append(channels[location] == 2 ? 2 : 1, 3);
      b.append(0, 1); b.append(16, 5); b.append(0, 5); b.append(0, 1);
      b.append(1, 1); b.append(1u << chanmapBits[location], 16); b.append(0, 1); b.append(0, 1);
      std::string dependent = b.str(); dependent.resize(256);
      const std::string packet = enhanced() + dependent;
      const AC3::Header h = AC3::parse(packet.data(), packet.size());
      const std::string setup = AC3::makeDEC3(h);
      const AC3::Header stored = AC3::parseDEC3(setup.data(), setup.size());
      const unsigned int serialized = setup.size() == 6 ? (((unsigned char)setup[4] & 1) << 8) | (unsigned char)setup[5] : 0;
      if (!h.valid || h.channels != 6 + channels[location] || !stored.valid || stored.channels != h.channels ||
          h.dependentLocations != (1u << location) || serialized != (1u << location)) {
        std::cerr << names[location] << " channels=" << h.channels << "/" << stored.channels << " chan_loc=" << serialized
                  << ", want channels=" << 6 + channels[location] << " chan_loc=" << (1u << location) << std::endl;
        failed = true;
      }
    }
    return failed ? 1 : 0;
  }
  if (argc > 1 && std::string(argv[1]) == "dependent") {
    Utils::bitWriter b;
    b.append(640, 13);
    b.append(0, 3);
    b.append(0, 2);
    b.append(16, 5);
    b.append(0, 2);
    b.append(3, 3);
    b.append(7, 3);
    b.append(1, 1);
    b.append(0, 3);
    b.append(1, 4);
    b.append(2, 9);
    const std::string setup = b.str();
    const AC3::Header decoded = AC3::parseDEC3(setup.data(), setup.size());
    if (!decoded.valid || decoded.channels != 8) {
      std::cerr << "DEC3 7.1 dependent substream advertised " << decoded.channels << " channels instead of 8" << std::endl;
      return 1;
    }
    return 0;
  }

  const std::string frame = enhanced();
  const AC3::Header h = AC3::parse(frame.data(), frame.size());
  assert(h.valid && h.enhanced && h.frameBytes == 256 && h.rate == 48000 && h.channels == 6 && h.samples == 1536 &&
         h.bitRate == 64000 && h.bsmod == 3);
  const std::string dec3 = AC3::makeDEC3(h);
  assert(dec3 == std::string("\x02\x00\x20\x3f\x00", 5));
  {
    DTSC::Meta fromGroup;
    fromGroup.reInit("", true);
    const size_t audio = fromGroup.addTrack();
    fromGroup.setID(audio, 3);
    fromGroup.setType(audio, "audio");
    fromGroup.setCodec(audio, "EAC-3");
    assert(CMAF::trackHeader(fromGroup, audio).empty());
    EBML::toDTSC parser;
    std::string scale;
    EBML::appendElemUInt(scale, EBML::EID_TIMECODESCALE, 1000000);
    parser.parseElement(EBML::Element(scale.data()), 0, fromGroup);
    std::string group;
    EBML::appendSimpleBlock(group, frame.data(), frame.size(), 3, 0, true, 0);
    parser.parseElement(EBML::Element(group.data()), 0, fromGroup);
    assert(fromGroup.getInit(audio) == dec3);
    assert(fromGroup.getRate(audio) == 48000 && fromGroup.getChannels(audio) == 6);
    MP4::AudioSampleEntry sample(fromGroup, audio);
    const MP4::Box setup = sample.getCodecBox();
    assert(setup.isType("dec3") && setup.payloadSize() >= 5);
  }
  const AC3::Header stored = AC3::parseDEC3(dec3.data(), dec3.size());
  assert(stored.valid && stored.channels == 6 && stored.rate == 48000 && stored.bsmod == 3);
  for (size_t size = 0; size < 7; ++size) { assert(!AC3::parse(frame.data(), size).valid); }
  for (size_t size = 0; size < 5; ++size) { assert(!AC3::parseDEC3(dec3.data(), size).valid); }
  std::string invalid = frame;
  invalid[0] = 0;
  assert(!AC3::parse(invalid.data(), invalid.size()).valid);
  Utils::bitWriter b;
  b.append(0x0B77, 16);
  b.append(0, 16);
  b.append(0, 2);
  b.append(10, 6);
  b.append(8, 5);
  b.append(0, 3);
  b.append(7, 3);
  b.append(0, 4);
  b.append(1, 1);
  const std::string ac3 = b.str();
  const AC3::Header a = AC3::parse(ac3.data(), ac3.size());
  assert(a.valid && !a.enhanced && a.channels == 6 && a.rate == 48000 && a.frameBytes == 320 && a.bitRate == 80000);
  DTSC::Meta metadata;
  metadata.reInit("", true);
  const size_t track = metadata.addTrack();
  metadata.setType(track, "audio");
  metadata.setCodec(track, "EAC-3");
  metadata.setRate(track, 48000);
  metadata.setChannels(track, 6);
  metadata.setSize(track, 16);
  metadata.setInit(track, dec3);
  MP4::AudioSampleEntry entry(metadata, track);
  assert(entry.isType("ec-3"));
  MP4::Box box = entry.getCodecBox();
  assert(box.isType("dec3"));
  assert(std::string(box.payload(), box.payloadSize()) == dec3);
  std::set<size_t> selected;
  selected.insert(track);
  TS::Packet packet;
  packet.FromPointer(TS::createPMT(selected, metadata));
  TS::ProgramMappingTable pmt;
  pmt = packet;
  const TS::ProgramMappingEntry map = pmt.getEntry(0);
  assert(map.getStreamType() == 0x87);
  const std::string reg = TS::ProgramDescriptors(map.getESInfo(), map.getESInfoLength()).getRegistration();
  if (reg != "EAC3") {
    std::cerr << "EAC-3 registration is '" << reg << "' info bytes=" << map.getESInfoLength() << "\n"
              << pmt.toPrettyString(0);
    return 1;
  }
  assert(recognized(metadata, 0x87, "", false));
  assert(recognized(metadata, 0x06, std::string("\x7a\x01\x00", 3), false));
  assert(recognized(metadata, 0x84,
                    std::string("\x05\x04"
                                "HDMV",
                                6),
                    false));
  assert(recognized(metadata, 0x84, "", true));
  assert(!recognized(metadata, 0x84, "", false));
  assert(activePrivateTrack(metadata, 0x86, false));
  assert(!activePrivateTrack(metadata, 0x86, true));
  {
    DTSC::Meta sources;
    sources.reInit("", true);
    const size_t waiting = sources.addTrack();
    sources.setType(waiting, "audio");
    sources.setCodec(waiting, "AC3");
    const size_t later = sources.addTrack();
    sources.setType(later, "meta");
    sources.setCodec(later, "JSON");
    TS::Packet pat, pmt;
    pat.FromPointer(TS::PAT);
    std::set<size_t> selected{waiting, later};
    pmt.FromPointer(TS::createPMT(selected, sources));
    TS::Stream stream;
    stream.parse(pat, 0);
    stream.parse(pmt, 188);
    DTSC::Meta loaded;
    loaded.reInit("", true);
    stream.initializeMetadata(loaded);
    bool json = false;
    for (const size_t track : loaded.getValidTracks()) { json |= loaded.getCodec(track) == "JSON"; }
    assert(json);
  }
  char truncatedEntry[] = {char(0x87), 0, 1, 0, 8};
  assert(!TS::ProgramMappingEntry(truncatedEntry, truncatedEntry + sizeof(truncatedEntry)));
  TS::ProgramMappingTable malformed;
  malformed = packet;
  const size_t programLength = 4 + malformed.getOffset() + 11;
  char *bytes = const_cast<char *>(malformed.checkAndGetBuffer());
  bytes[programLength] = char(0xff);
  bytes[programLength + 1] = char(0xff);
  assert(malformed.getProgramInfoLength() == 0);
  assert(!malformed.getEntry(0));
  const char broken[] = {5, 4, 'E'};
  assert(TS::ProgramDescriptors(broken, sizeof(broken)).getRegistration().empty());
  return 0;
}
