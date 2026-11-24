#pragma once

#include <mist/stream.h>

namespace Mist {
  // Readiness follows this output's codec and explicit track selection. A
  // thumbnail elsewhere in the stream cannot hold back a playable rendition.
  inline size_t outputSelectionTimingTrack(const DTSC::Meta & meta, const std::map<std::string, std::string> & params,
                                           const JSON::Value & capa, const std::string & ua) {
    const std::set<size_t> tracks = Util::wouldSelect(meta, params, capa, ua);
    size_t audio = INVALID_TRACK_ID;
    for (size_t track : tracks) {
      if (meta.getType(track) == "video") { return track; }
      if (audio == INVALID_TRACK_ID && meta.getType(track) == "audio") { audio = track; }
    }
    return audio;
  }
} // namespace Mist
