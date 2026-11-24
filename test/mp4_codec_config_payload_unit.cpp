#include <mist/mp4_generic.h>

#include <iostream>
#include <string>

namespace {
  int failures = 0;

  void check(bool ok, const std::string & message) {
    if (ok) { return; }
    std::cerr << "FAIL: " << message << std::endl;
    ++failures;
  }

  /// A codec configuration box keeps its header and holds exactly the payload it was last given,
  /// whether the new payload is longer or shorter than the one it replaces (a Matroska H264 track
  /// without CodecPrivate hands AVCC an empty one).
  template<class Box> void checkPayloads(const char *type) {
    const std::string name(type);
    for (const std::string & first : {std::string(), std::string("\x01\x64\x00\x1f\xff\xe1", 6), std::string(40, '\x42')}) {
      for (const std::string & second : {std::string(), std::string("\x01", 1), std::string(12, '\x07'), std::string(64, '\x11')}) {
        Box box;
        box.setPayload(first);
        box.setPayload(second);
        const std::string what = name + " " + std::to_string(first.size()) + "->" + std::to_string(second.size()) + " bytes: ";
        check(box.isType(type), what + "the box type must survive, got '" + box.getType() + "'");
        check(box.boxedSize() == 8 + second.size(), what + "box size " + std::to_string(box.boxedSize()));
        check(box.payloadSize() == second.size() && std::string(box.payload(), box.payloadSize()) == second,
              what + "the payload must be exactly the last one set");
      }
    }
  }
} // namespace

int main() {
  checkPayloads<MP4::AVCC>("avcC");
  checkPayloads<MP4::HVCC>("hvcC");
  checkPayloads<MP4::AV1C>("av1C");
  {
    // Reading an empty AVC configuration finds no parameter sets and reads nothing past it.
    MP4::AVCC avcc;
    avcc.setPayload(std::string());
    check(!avcc.getSPSCount() && !avcc.getSPSLen() && !avcc.getPPSCount(), "an empty avcC holds no parameter sets");
  }
  if (failures) {
    std::cerr << failures << " failure(s)" << std::endl;
    return 1;
  }
  return 0;
}
