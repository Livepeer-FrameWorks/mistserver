#include <mist/ts_packet.h>
#include <mist/ts_stream.h>

#include <cassert>
#include <cstdint>
#include <set>
#include <string>

namespace {

  uint64_t decodePesTimestamp(const char *data) {
    return ((uint64_t)(data[0] & 0x0E) << 29) | ((uint64_t)data[1] << 22) | ((uint64_t)(data[2] & 0xFE) << 14) |
      ((uint64_t)data[3] << 7) | ((uint64_t)(data[4] & 0xFE) >> 1);
  }

  DTSC::Meta makeOpusMeta(uint8_t channels) {
    DTSC::Meta meta;
    meta.reInit("", true);
    const size_t track = meta.addTrack(8, 8, 16, 2, true);
    meta.setType(track, "audio");
    meta.setCodec(track, "opus");
    meta.setChannels(track, channels);
    return meta;
  }

  DTSC::Meta roundTripThroughPMT(DTSC::Meta & source) {
    std::set<size_t> selected = source.getValidTracks();
    assert(selected.size() == 1);

    TS::Stream stream;
    TS::Packet pat;
    pat.FromPointer(TS::PAT);
    stream.parse(pat, 0);
    TS::Packet pmt;
    pmt.FromPointer(TS::createPMT(selected, source));
    stream.parse(pmt, 188);

    DTSC::Meta parsed;
    parsed.reInit("", true);
    stream.initializeMetadata(parsed);
    return parsed;
  }

  size_t addTrack(DTSC::Meta & meta, size_t id, const std::string & type, const std::string & codec) {
    const size_t track = meta.addTrack(id, id, 16, 2, true);
    meta.setType(track, type);
    meta.setCodec(track, codec);
    return track;
  }

} // namespace

int main(int argc, char **argv) {
  if (argc == 2 && std::string(argv[1]) == "large-pmt") {
    DTSC::Meta meta;
    meta.reInit("", true);
    const size_t video = addTrack(meta, 1, "video", "H264");
    std::set<size_t> selected{video};
    TS::Packet packet;
    packet.FromPointer(TS::createPMT(selected, meta));
    TS::ProgramMappingTable pmt;
    pmt = packet;
    pmt.setSectionLength(300);
    const auto entry = pmt.getEntry(0);
    if (!entry || entry.getStreamType() != 0x1b) {
      fprintf(stderr, "large PMT lost its complete first H264 entry\n");
      return 1;
    }
    return 0;
  }
  if (argc == 2 && std::string(argv[1]) == "malformed-pes") {
    // A PES-bearing PID, not only the PAT, must survive an adaptation field longer than the packet:
    // the stream assembles its payload into a PES and parses it when the next unit starts.
    DTSC::Meta meta;
    meta.reInit("", true);
    const size_t video = addTrack(meta, 1, "video", "H264");
    std::set<size_t> selected{video};
    const unsigned int pid = TS::getUniqTrackID(meta, video);
    uint8_t counter = 0;
    auto pesPacket = [&](bool unitStart, uint64_t pts) {
      std::string raw(188, '\xff');
      raw[0] = 0x47;
      raw[1] = (unitStart ? 0x40 : 0x00) | ((pid >> 8) & 0x1f);
      raw[2] = pid & 0xff;
      raw[3] = 0x10 | (counter++ & 0x0f);
      size_t at = 4;
      if (unitStart) {
        const char header[] = {0, 0, 1, (char)0xe0, 0, 0, (char)0x80, (char)0x80, 5};
        raw.replace(at, sizeof(header), header, sizeof(header));
        at += sizeof(header);
        raw[at++] = 0x21 | ((pts >> 29) & 0x0e);
        raw[at++] = (pts >> 22) & 0xff;
        raw[at++] = 0x01 | ((pts >> 14) & 0xfe);
        raw[at++] = (pts >> 7) & 0xff;
        raw[at++] = 0x01 | ((pts << 1) & 0xfe);
        const char nal[] = {0, 0, 0, 1, 0x09, (char)0xf0};
        raw.replace(at, sizeof(nal), nal, sizeof(nal));
      }
      return raw;
    };
    for (unsigned int length = 184; length <= 255; ++length) {
      TS::Stream stream;
      uint64_t bytePos = 0;
      auto feed = [&](const std::string & raw) {
        TS::Packet packet;
        packet.FromPointer(raw.data());
        stream.parse(packet, bytePos);
        bytePos += 188;
      };
      TS::Packet pat;
      pat.FromPointer(TS::PAT);
      stream.parse(pat, bytePos);
      bytePos += 188;
      TS::Packet pmt;
      pmt.FromPointer(TS::createPMT(selected, meta));
      stream.parse(pmt, bytePos);
      bytePos += 188;
      feed(pesPacket(true, 90000));
      feed(pesPacket(false, 0));
      std::string bad = pesPacket(false, 0);
      bad[3] = 0x30 | (counter - 1);
      bad[4] = length;
      feed(bad);
      feed(pesPacket(false, 0));
      feed(pesPacket(true, 93600));
      feed(pesPacket(true, 97200));
      stream.finish();
      DTSC::Packet out;
      size_t drained = 0;
      while (stream.hasPacket() && drained < 16) {
        stream.getEarliestPacket(out);
        ++drained;
      }
      if (!drained) {
        fprintf(stderr, "adaptation length %u on the PES PID lost every frame of the stream\n", length);
        return 1;
      }
    }
    puts("PASS malformed adaptation lengths on a PES PID stay in bounds and keep the stream's frames");
    return 0;
  }

  if (argc == 2 && std::string(argv[1]) == "malformed") {
    for (unsigned int length = 184; length <= 255; ++length) {
      std::string raw(TS::PAT, 188);
      raw[3] = 0x30;
      raw[4] = length;
      TS::Packet packet;
      packet.FromPointer(raw.data());
      if (packet.getPayloadLength() != 0) {
        fprintf(stderr, "invalid adaptation length %u yielded payload length %d\n", length, packet.getPayloadLength());
        return 1;
      }
      const char *payload = packet.getPayload();
      const char *begin = packet.checkAndGetBuffer();
      if (payload < begin || payload > begin + 188) { return 1; }
      TS::ProgramAssociationTable pat;
      pat = packet;
      std::set<unsigned int> pids;
      pat.parsePIDs(pids);
      if (pat.getProgramCount() || !pids.empty()) {
        fprintf(stderr, "invalid adaptation length yielded PAT programs\n");
        return 1;
      }
      pat.toPrettyString(0);
      TS::ProgramMappingTable pmt;
      pmt = packet;
      if (pmt.getEntry(0)) { return 1; }
      pmt.toPrettyString(0);
      TS::Stream stream;
      stream.parse(packet, 0);
      stream.finish();
    }
    for (unsigned int pointer = 171; pointer <= 255; ++pointer) {
      std::string raw(TS::PAT, 188);
      raw[4] = pointer;
      TS::Packet packet;
      packet.FromPointer(raw.data());
      TS::ProgramAssociationTable pat;
      pat = packet;
      std::set<unsigned int> pids;
      pat.parsePIDs(pids);
      if (!pids.empty()) {
        fprintf(stderr, "out-of-packet PAT pointer %u yielded programs\n", pointer);
        return 1;
      }
      pat.toPrettyString(0);
    }
    puts("PASS malformed adaptation lengths and PAT pointers yield no payload or programs");
    return 0;
  }

  {
    DTSC::Meta meta;
    meta.reInit("", true);
    const size_t track = addTrack(meta, 102, "audio", "AAC");
    meta.setRate(track, 44100);
    meta.setChannels(track, 2);
    std::set<size_t> selected{track};
    TS::Packet packet;
    packet.FromPointer(TS::createPMT(selected, meta));
    TS::ProgramMappingTable pmt;
    pmt = packet;
    auto entry = pmt.getEntry(0);
    const size_t oldLength = entry.getESInfoLength();
    entry.setStreamType(0x06);
    entry.setESInfo("");
    pmt.setSectionLength(pmt.getSectionLength() - oldLength);
    pmt.calcCRC();
    TS::Stream stream;
    TS::Packet pat;
    pat.FromPointer(TS::PAT);
    stream.parse(pat, 0);
    stream.parse(pmt, 188);
    const size_t privatePID = TS::getUniqTrackID(meta, track);
    if (stream.isDataTrack(privatePID)) {
      fprintf(stderr, "unidentified private PES would spawn a worker that never receives media\n");
      return 1;
    }
    const std::string adts("\xff\xf1\x50\x80\x01\x7f\xfc\x01\x02\x03\x04", 11);
    assert(aac::adts(adts.data(), adts.size()));
    std::string pes;
    TS::Packet::getPESAudioLeadIn(pes, adts.size(), 90000, 128000);
    pes += adts;
    packet.clear();
    packet.setPID(TS::getUniqTrackID(meta, track));
    packet.setUnitStart(true);
    packet.fillFree(pes.data(), pes.size());
    packet.addStuffing();
    stream.parse(packet, 376);
    stream.finish();
    if (!stream.hasPacket()) {
      fprintf(stderr, "AAC carried as private PES stream_type 0x06 was dropped\n");
      return 1;
    }
    DTSC::Meta parsed;
    parsed.reInit("", true);
    stream.initializeMetadata(parsed);
    const auto tracks = parsed.getValidTracks();
    assert(!tracks.empty() && parsed.getCodec(*tracks.begin()) == "AAC");
    assert(stream.isDataTrack(privatePID));
  }

  std::string reorderedPes;
  TS::Packet::getPESVideoLeadIn(reorderedPes, 10, 90000, -3600, true);
  assert((reorderedPes[7] & 0xC0) == 0xC0);
  assert(decodePesTimestamp(reorderedPes.data() + 9) == 86400);
  assert(decodePesTimestamp(reorderedPes.data() + 14) == 90000);

  std::string esInfo("\005\004Opus", 6);
  esInfo.append("\177\002\200", 3);
  esInfo.append(1, '\002');

  TS::ProgramDescriptors descriptors(esInfo.data(), esInfo.size());
  assert(descriptors.getRegistration() == "Opus");

  std::string ext = descriptors.getExtension();
  assert(ext.size() == 2);
  assert((uint8_t)ext[0] == 0x80);
  assert((uint8_t)ext[1] == 2);

  DTSC::Meta stereoSource = makeOpusMeta(2);
  std::set<size_t> stereoTracks = stereoSource.getValidTracks();
  const size_t stereoTrack = *stereoTracks.begin();
  TS::ProgramMappingTable mappedPMT;
  TS::Packet mappedPacket;
  mappedPacket.FromPointer(TS::createPMT(stereoTracks, stereoSource, 0, [stereoTrack](const DTSC::Meta &, size_t idx) {
    return idx == stereoTrack ? 777 : 778;
  }));
  mappedPMT = mappedPacket;
  assert(mappedPMT.getPCRPID() == 777);
  assert(mappedPMT.getEntry(0).getElementaryPid() == 777);

  DTSC::Meta stereoParsed = roundTripThroughPMT(stereoSource);
  std::set<size_t> parsedTracks = stereoParsed.getValidTracks();
  assert(parsedTracks.size() == 1);
  const size_t parsedTrack = *parsedTracks.begin();
  assert(stereoParsed.getCodec(parsedTrack) == "opus");
  assert(stereoParsed.getChannels(parsedTrack) == 2);
  assert(stereoParsed.getInit(parsedTrack).size() == 19);
  assert((uint8_t)stereoParsed.getInit(parsedTrack)[9] == 2);
  assert((uint8_t)stereoParsed.getInit(parsedTrack)[18] == 0);

  DTSC::Meta implicitSource = makeOpusMeta(0);
  DTSC::Meta implicitParsed = roundTripThroughPMT(implicitSource);
  parsedTracks = implicitParsed.getValidTracks();
  assert(parsedTracks.size() == 1);
  const size_t implicitTrack = *parsedTracks.begin();
  assert(implicitParsed.getChannels(implicitTrack) == 2);
  assert((uint8_t)implicitParsed.getInit(implicitTrack)[9] == 2);

  DTSC::Meta mappingMeta;
  mappingMeta.reInit("", true);
  const size_t videoOne = addTrack(mappingMeta, 101, "video", "H264");
  const size_t videoTwo = addTrack(mappingMeta, 102, "video", "H264");
  const size_t audio = addTrack(mappingMeta, 201, "audio", "AAC");
  const size_t metadata = addTrack(mappingMeta, 301, "meta", "JSON");
  const size_t subtitle = addTrack(mappingMeta, 302, "meta", "subtitle");
  const size_t unspecified = addTrack(mappingMeta, 401, "unknown", "unknown");
  const std::set<size_t> mappingTracks = {videoOne, videoTwo, audio, metadata, subtitle, unspecified};
  std::map<std::string, std::string> parameters;
  parameters["mappid" + JSON::Value(videoOne).asString()] = "700";
  parameters["vidpidstart"] = "710";
  parameters["audpidstart"] = "720";
  parameters["metapidstart"] = "730";
  parameters["subpidstart"] = "740";

  const std::map<size_t, size_t> pidMap = TS::buildPidMap(mappingMeta, mappingTracks, parameters);
  assert(pidMap.size() == mappingTracks.size());
  assert(pidMap.at(videoOne) == 700);
  assert(pidMap.at(videoTwo) == 710);
  assert(pidMap.at(audio) == 720);
  assert(pidMap.at(metadata) == 730);
  assert(pidMap.at(subtitle) == 740);
  assert(pidMap.at(unspecified) == 741);

  const size_t laterAudio = addTrack(mappingMeta, 202, "audio", "AAC");
  const std::map<size_t, size_t> retained = TS::buildPidMap(mappingMeta, {audio, laterAudio}, parameters, pidMap);
  assert(retained.at(audio) == 720 && retained.at(videoOne) == 700);
  assert(retained.at(laterAudio) == 721);
  const std::map<size_t, size_t> defaults = TS::buildPidMap(mappingMeta, {videoOne, audio}, {});
  const std::map<size_t, size_t> afterVideoEOF = TS::buildPidMap(mappingMeta, {audio, laterAudio}, {}, defaults);
  assert(afterVideoEOF.at(videoOne) == defaults.at(videoOne));
  assert(afterVideoEOF.at(audio) == defaults.at(audio));
  assert(afterVideoEOF.at(laterAudio) != defaults.at(videoOne) && afterVideoEOF.at(laterAudio) != defaults.at(audio));

  std::set<size_t> mappedTracks = mappingTracks;
  TS::Packet customPmtPacket;
  customPmtPacket.FromPointer(TS::createPMT(mappedTracks, mappingMeta, 0,
                                            [&pidMap](const DTSC::Meta &, size_t track) { return pidMap.at(track); }));
  TS::ProgramMappingTable customPmt;
  customPmt = customPmtPacket;
  assert(customPmt.getPCRPID() == 700);
  std::set<size_t> pmtPids;
  TS::ProgramMappingEntry pmtEntry = customPmt.getEntry(0);
  while (pmtEntry) {
    pmtPids.insert(pmtEntry.getElementaryPid());
    pmtEntry.advance();
  }
  for (const auto & mapping : pidMap) { assert(pmtPids.count(mapping.second)); }

  return 0;
}
