#include <mist/ebml_socketglue.h>
#include <mist/h264.h>
#include <mist/mp4_generic.h>

#include <cstdio>

static bool checkHEVCOpenGOP(const std::string & init = "") {
  DTSC::Meta meta;
  meta.reInit("", true);
  EBML::toDTSC parser;
  std::string entry;
  EBML::appendElemUInt(entry, EBML::EID_TRACKNUMBER, 1);
  EBML::appendElemUInt(entry, EBML::EID_TRACKTYPE, 1);
  const std::string codec = "V_MPEGH/ISO/HEVC";
  EBML::appendElemHead(entry, EBML::EID_CODECID, codec.size());
  entry += codec;
  if (!init.empty()) {
    EBML::appendElemHead(entry, EBML::EID_CODECPRIVATE, init.size());
    entry += init;
  }
  std::string track;
  EBML::appendElemHead(track, EBML::EID_TRACKENTRY, entry.size());
  track += entry;
  parser.parseElement(EBML::Element(track.data()), 0, meta);
  std::string scale;
  EBML::appendElemUInt(scale, EBML::EID_TIMECODESCALE, 1000000);
  parser.parseElement(EBML::Element(scale.data()), 0, meta);
  const uint64_t presentation[] = {0, 120, 40, 80, 240, 160, 200, 360, 280, 320};
  uint64_t previous = 0;
  size_t count = 0;
  const auto drain = [&]() {
    DTSC::Packet packet;
    size_t idx;
    uint64_t time;
    while (parser.fillPacket(meta, idx, time, packet)) {
      if (count >= sizeof(presentation) / sizeof(*presentation) || (count && time <= previous) ||
          int64_t(packet.getInt("offset")) < 0 ||
          int64_t(time) + int64_t(packet.getInt("offset")) != int64_t(presentation[count] + parser.lead)) {
        fprintf(stderr, "HEVC open GOP lost decode/presentation order: frame=%zu DTS=%llu previous=%llu lead=%llu\n",
                count, (unsigned long long)time, (unsigned long long)previous, (unsigned long long)parser.lead);
        return false;
      }
      // The shared lead leaves room for the whole reorder lag, so decoding keeps the 40 ms cadence.
      if (count && time - previous != 40) {
        fprintf(stderr, "HEVC open GOP decode clock left the frame cadence: frame=%zu DTS=%llu previous=%llu\n", count,
                (unsigned long long)time, (unsigned long long)previous);
        return false;
      }
      previous = time;
      ++count;
    }
    return true;
  };
  for (uint64_t time : presentation) {
    std::string block;
    EBML::appendSimpleBlock(block, "frame", 5, 1, time, time % 120 == 0, 0);
    parser.parseElement(EBML::Element(block.data()), 0, meta);
    if (!drain()) { return false; }
  }
  parser.finish();
  return drain() && count == sizeof(presentation) / sizeof(*presentation);
}

static bool checkH264DefaultDepth() {
  const char raw[] =
    "\x67\x64\x00\x28\xac\xd9\x40\x78\x02\x27\xe5\xc0\x44\x00\x00\x03\x00\x04\x00\x00\x03\x00\xf0\x3c\x60\xc6\x58";
  struct Case {
      uint8_t level;
      uint64_t width, height;
      bool frames;
      size_t expected;
  };
  const Case cases[] = {
    {40, 119, 67, true, 4}, {31, 79, 44, true, 5}, {30, 39, 29, true, 6}, {40, 19, 11, true, 16}, {40, 119, 33, false, 4}};
  for (const Case & c : cases) {
    h264::spsUnit sps(raw, sizeof(raw) - 1);
    sps.vuiParametersPresentFlag = true;
    sps.vuiParams.bitstreamRestrictionFlag = false;
    sps.levelIdc = c.level;
    sps.picWidthInMbsMinus1 = c.width;
    sps.picHeightInMapUnitsMinus1 = c.height;
    sps.frameMbsOnlyFlag = c.frames;
    sps.mbAdaptiveFrameFieldFlag = false;
    const std::string nal = sps.generate();
    const std::string annex =
      std::string("\x00\x00\x00\x01", 4) + nal + std::string("\x00\x00\x00\x01\x68\xeb\xe3\xcb\x22\xc0", 10);
    DTSC::Meta meta;
    meta.reInit("", true);
    const size_t idx = meta.addTrack();
    meta.setID(idx, 1);
    meta.setType(idx, "video");
    meta.setCodec(idx, "H264");
    meta.setInit(idx, h264::initFromAnnexB(annex.data(), annex.size()));
    EBML::toDTSC parser;
    std::string scale;
    EBML::appendElemUInt(scale, EBML::EID_TIMECODESCALE, 1000000);
    parser.parseElement(EBML::Element(scale.data()), 0, meta);
    std::string block;
    EBML::appendSimpleBlock(block, "frame", 5, 1, 0, true, 0);
    parser.parseElement(EBML::Element(block.data()), 0, meta);
    const size_t actual = parser.packBuf[1].maxDelay;
    if (actual != c.expected) {
      fprintf(stderr, "H264 default reorder depth: level=%u mbs=%llux%llu frames=%d got=%zu expected=%zu\n", c.level,
              (unsigned long long)(c.width + 1), (unsigned long long)(c.height + 1), c.frames, actual, c.expected);
      return false;
    }
    size_t startupFrames = 0;
    while (!parser.packBuf[1].hasPackets() && startupFrames < 16) {
      ++startupFrames;
      std::string next;
      EBML::appendSimpleBlock(next, "frame", 5, 1, startupFrames * 40, false, 0);
      parser.parseElement(EBML::Element(next.data()), 0, meta);
    }
    if (!parser.packBuf[1].hasPackets()) {
      fprintf(stderr, "H264 without a reorder restriction waited beyond 16 frames / 640 ms at25fps\n");
      return false;
    }
    printf("PASS H264 absent reorder restriction: level=%u depth=%zu startup=%zu frames / %zu ms at25fps\n", c.level,
           actual, startupFrames, startupFrames * 40);
  }
  return true;
}

int main() {
  if (!checkH264DefaultDepth()) { return 1; }
  // VPS/SPS/PPS from a real x265 open-GOP stream (two declared reorder pictures).
  const char init[] = "\x01\x01\x60\x00\x00\x00\x90\x00\x00\x00\x00\x00\x3c\xf0\x00\xfc\xfd\xf8\xf8\x00"
                      "\x00\x0f\x03\x20\x00\x01\x00\x18\x40\x01\x0c\x01\xff\xff\x01\x60\x00\x00\x03\x00"
                      "\x90\x00\x00\x03\x00\x00\x03\x00\x3c\x95\x94\x09\x21\x00\x01\x00\x2b\x42\x01\x01"
                      "\x01\x60\x00\x00\x03\x00\x90\x00\x00\x03\x00\x00\x03\x00\x3c\xa0\x0a\x08\x0b\x9f"
                      "\x79\x65\x65\x92\x4c\xaf\x01\x68\x08\x00\x00\x03\x00\x08\x00\x00\x03\x00\xc8\x40"
                      "\x22\x00\x01\x00\x07\x44\x01\xc1\x72\xb4\x62\x40";
  if (!checkHEVCOpenGOP() || !checkHEVCOpenGOP(std::string(init, sizeof(init) - 1))) { return 1; }
  EBML::trackPredictor predictor;
  const uint64_t presentation[] = {0, 120, 40, 80, 240, 160, 200};
  for (uint64_t time : presentation) { predictor.add(time, 1, 10, 1, time == 0, true); }
  predictor.finish();
  uint64_t previous = 0;
  for (size_t i = 0; i < 7; ++i) {
    const EBML::packetData & packet = predictor.getPacketData(true);
    if ((i && packet.time <= previous) || packet.offset < 0 ||
        int64_t(packet.time) + int64_t(packet.offset) != int64_t(presentation[i])) {
      fprintf(stderr, "B-frame decode timestamps clamped or presentation shifted: frame=%zu DTS=%llu offset=%lld PTS=%llu\n",
              i, (unsigned long long)packet.time, (long long)packet.offset, (unsigned long long)presentation[i]);
      return 1;
    }
    previous = packet.time;
    predictor.remove();
  }
  MP4::TRUN run;
  run.setFlags(MP4::trunsampleOffsets);
  MP4::trunSampleInformation sample = {};
  sample.sampleOffset = -40;
  run.setSampleInformation(sample, 0);
  if (run.getVersion() != 1 || run.getSampleInformation(0).sampleOffset != -40) {
    fprintf(stderr, "negative CMAF composition offset was written unsigned\n");
    return 1;
  }
  MP4::CTTS offsets;
  MP4::CTTSEntry entry = {1, -40};
  offsets.setCTTSEntry(entry, 0);
  if (offsets.getVersion() != 1 || offsets.getCTTSEntry(0).sampleOffset != -40) {
    fprintf(stderr, "negative MP4 composition offset was written unsigned\n");
    return 1;
  }
  puts("PASS monotonic DTS and original presentation times with signed MP4/CMAF offsets");
}
