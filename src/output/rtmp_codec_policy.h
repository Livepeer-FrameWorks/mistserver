#pragma once
#include <mist/amf.h>
#include <mist/json.h>

namespace Mist {
  inline void appendEnhancedRTMPAudio(AMF::Object *list, bool informationMap = false) {
    for (const char *fourcc : {"ac-3", "Opus", ".mp3", "fLaC", "mp4a"}) {
      if (informationMap) {
        list->addContent(fourcc, 7.0);
      } else {
        list->addContent(fourcc);
      }
    }
  }

  inline void negotiateEnhancedRTMPAudio(JSON::Value & audio, const std::string & fourcc) {
    if (fourcc == "Opus") { audio.append("opus"); }
    if (fourcc == "ac-3") { audio.append("AC3"); }
    if (fourcc == "fLaC") { audio.append("FLAC"); }
  }
} // namespace Mist
