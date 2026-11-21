#include <mist/ebml_socketglue.h>

#include <cassert>
#include <iostream>

static void checkWebVTTCue(const std::string & codec, const std::string & text) {
  DTSC::Meta meta;
  meta.reInit("", true);
  EBML::toDTSC parser;
  std::string entry;
  EBML::appendElemUInt(entry, EBML::EID_TRACKNUMBER, 3);
  EBML::appendElemUInt(entry, EBML::EID_TRACKTYPE, 17);
  EBML::appendElemHead(entry, EBML::EID_CODECID, codec.size());
  entry += codec;
  std::string track;
  EBML::appendElemHead(track, EBML::EID_TRACKENTRY, entry.size());
  track += entry;
  parser.parseElement(EBML::Element(track.data()), 0, meta);
  std::string scale;
  EBML::appendElemUInt(scale, EBML::EID_TIMECODESCALE, 1000000);
  parser.parseElement(EBML::Element(scale.data()), 0, meta);
  const std::string payload = (codec == "D_WEBVTT/SUBTITLES" ? "cue-id\nline:20%\n" : "") + text;
  std::string block;
  EBML::appendSubtitleBlock(block, payload.data(), payload.size(), 3, 1000, 900, 0);
  parser.parseElement(EBML::Element(block.data()), 0, meta);
  parser.finish();
  DTSC::Packet packet;
  size_t idx;
  uint64_t time;
  assert(parser.fillPacket(meta, idx, time, packet));
  std::string result;
  packet.getString("data", result);
  assert(result == text && packet.getInt("duration") == 900);
}

static void checkVideoBlockGroupKeyframe(bool referenced) {
  DTSC::Meta meta;
  meta.reInit("", true);
  EBML::toDTSC parser;
  std::string entry;
  EBML::appendElemUInt(entry, EBML::EID_TRACKNUMBER, 1);
  EBML::appendElemUInt(entry, EBML::EID_TRACKTYPE, 1);
  const std::string codec = "V_MPEG4/ISO/AVC";
  EBML::appendElemHead(entry, EBML::EID_CODECID, codec.size());
  entry += codec;
  std::string track;
  EBML::appendElemHead(track, EBML::EID_TRACKENTRY, entry.size());
  track += entry;
  parser.parseElement(EBML::Element(track.data()), 0, meta);
  std::string scale;
  EBML::appendElemUInt(scale, EBML::EID_TIMECODESCALE, 1000000);
  parser.parseElement(EBML::Element(scale.data()), 0, meta);
  std::string block;
  EBML::appendSimpleBlock(block, "frame", 5, 1, 0, false, 0);
  block[0] = '\xA1';
  if (referenced) { EBML::appendElemUInt(block, EBML::EID_REFERENCEBLOCK, 1); }
  std::string group;
  EBML::appendElemHead(group, EBML::EID_BLOCKGROUP, block.size());
  group += block;
  parser.parseElement(EBML::Element(group.data()), 0, meta);
  parser.finish();
  size_t idx;
  uint64_t time;
  DTSC::Packet packet;
  assert(parser.fillPacket(meta, idx, time, packet));
  assert(packet.getFlag("keyframe") == !referenced);
}

int main() {
  checkVideoBlockGroupKeyframe(false);
  checkVideoBlockGroupKeyframe(true);
  for (const std::string & codec : {"S_TEXT/WEBVTT", "D_WEBVTT/SUBTITLES"}) {
    checkWebVTTCue(codec, "single line");
    checkWebVTTCue(codec, "first line\nsecond line\nthird line");
  }
  DTSC::Packet emptyCue;
  emptyCue.genericFill(0, 0, 3, 0, 0, 0, false, 200);
  assert(emptyCue && emptyCue.getInt("duration") == 200);
  emptyCue.appendData("cue", 3);
  std::string appended;
  emptyCue.getString("data", appended);
  assert(appended == "cue" && emptyCue.getInt("duration") == 200);
  assert(emptyCue.getDataLen() == DTSC::Packet::genericSize(0, 0, false, 3, 200));

  DTSC::Meta meta;
  meta.reInit("", true);
  std::string entry;
  EBML::appendElemUInt(entry, EBML::EID_TRACKNUMBER, 3);
  EBML::appendElemUInt(entry, EBML::EID_TRACKTYPE, 17);
  const std::string codec = "S_TEXT/UTF8";
  EBML::appendElemHead(entry, EBML::EID_CODECID, codec.size());
  entry += codec;
  std::string track;
  EBML::appendElemHead(track, EBML::EID_TRACKENTRY, entry.size());
  track += entry;
  EBML::toDTSC parser;
  parser.parseElement(EBML::Element(track.data()), 0, meta);
  std::string scale;
  EBML::appendElemUInt(scale, EBML::EID_TIMECODESCALE, 1000000);
  parser.parseElement(EBML::Element(scale.data()), 0, meta);
  std::string block;
  EBML::appendSimpleBlock(block, "hello", 5, 3, 1000, true, 0);
  block[0] = '\xA1';
  EBML::appendElemUInt(block, 0x1B, 2300);
  std::string group;
  EBML::appendElemHead(group, 0x20, block.size());
  group += block;
  parser.parseElement(EBML::Element(group.data()), 123, meta);
  parser.finish();
  size_t idx;
  uint64_t time;
  DTSC::Packet packet;
  if (!parser.fillPacket(meta, idx, time, packet)) {
    std::cerr << "BlockGroup subtitle disappeared\n";
    return 1;
  }
  assert(time == 1000 && packet.getInt("duration") == 2300 && packet.getFlag("keyframe"));
  std::string text;
  packet.getString("data", text);
  assert(text == "hello");
  // Matroska permits BlockDuration on either side of Block inside BlockGroup.
  std::string reordered;
  EBML::appendElemUInt(reordered, EBML::EID_BLOCKDURATION, 900);
  std::string nextBlock;
  EBML::appendSimpleBlock(nextBlock, "before", 6, 3, 4000, true, 0);
  nextBlock[0] = '\xA1';
  reordered += nextBlock;
  std::string nextGroup;
  EBML::appendElemHead(nextGroup, EBML::EID_BLOCKGROUP, reordered.size());
  nextGroup += reordered;
  parser.parseElement(EBML::Element(nextGroup.data()), 456, meta);
  parser.finish();
  assert(parser.fillPacket(meta, idx, time, packet));
  assert(time == 4000 && packet.getInt("duration") == 900);
  packet.getString("data", text);
  assert(text == "before");
  std::string written;
  EBML::appendSubtitleBlock(written, "again", 5, 3, 5000, 1700, 4000);
  assert(written.size() == EBML::sizeSubtitleBlock(3, 5));
  const EBML::Element groupElement(written.data());
  assert(groupElement.getID() == EBML::EID_BLOCKGROUP);
  assert(groupElement.findChild(EBML::EID_BLOCKDURATION).getPayloadLen() == 4);
  assert(groupElement.findChild(EBML::EID_BLOCKDURATION).getValUInt() == 1700);
  return 0;
}
