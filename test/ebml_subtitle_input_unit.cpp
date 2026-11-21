#include <mist/ebml_socketglue.h>

#include <cassert>
#include <iostream>
int main() {
  std::string payload;
  EBML::appendElemUInt(payload, EBML::EID_TRACKNUMBER, 1);
  const std::string codec = "S_TEXT/UTF8";
  EBML::appendElemHead(payload, EBML::EID_CODECID, codec.size());
  payload += codec;
  std::string entry;
  EBML::appendElemHead(entry, EBML::EID_TRACKENTRY, payload.size());
  entry += payload;
  DTSC::Meta meta;
  meta.reInit("", true);
  if (!EBML::parseTrackEntry(EBML::Element(entry.data()), meta) || meta.getValidTracks().size() != 1 ||
      meta.getCodec(*meta.getValidTracks().begin()) != "subtitle" || meta.getType(*meta.getValidTracks().begin()) != "meta") {
    std::cerr << "Matroska S_TEXT/UTF8 subtitle track disappeared on input\n";
    return 1;
  }
  return 0;
}
