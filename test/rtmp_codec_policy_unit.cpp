#include "../src/output/rtmp_codec_policy.h"

#include <iostream>

int main() {
  for (bool map : {false, true}) {
    AMF::Object list("", map ? AMF::AMF0_OBJECT : AMF::AMF0_STRICT_ARRAY);
    Mist::appendEnhancedRTMPAudio(&list, map);
    for (auto & entry : list) {
      const std::string fourcc = map ? entry.Indice() : entry.StrValue();
      if (fourcc == "ec-3") {
        std::cerr << "RTMP advertised ec-3 without an EAC-3 packet writer\n";
        return 1;
      }
    }
  }
  JSON::Value selected;
  for (const char *fourcc : {"ec-3", "Opus", "ac-3", "fLaC"}) { Mist::negotiateEnhancedRTMPAudio(selected, fourcc); }
  if (selected.size() != 3 || selected[0u].asString() != "opus" || selected[1u].asString() != "AC3" ||
      selected[2u].asString() != "FLAC") {
    std::cerr << "Client ec-3 negotiation selected an audio codec RTMP cannot write: " << selected << "\n";
    return 1;
  }
  std::cout << "PASS RTMP enhanced audio lists/maps refuse ec-3 and retain Opus, AC3 and FLAC\n";
  return 0;
}
