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

int main() {
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
