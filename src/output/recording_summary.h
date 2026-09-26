#pragma once

#include <mist/dtsc.h>
#include <mist/json.h>

#include <string>

namespace Mist {
  /// Describes one recorded track for the RECORDING_END summary. A derived
  /// track names its source the way stream JSON does, so a consumer can tell a
  /// transcode from the source passthrough of the same size: once renditions
  /// appear the passthrough can leave the selection early, and its span then
  /// says nothing about how complete the renditions are.
  /// knownSource is the source identifier remembered while the source was
  /// valid; it names the source once the buffer has torn the source down.
  inline void describeRecordedTrack(const DTSC::Meta & M, size_t trackIdx, JSON::Value & T, const std::string & knownSource = "") {
    T["id"] = M.getID(trackIdx);
    T["type"] = M.getType(trackIdx);
    T["codec"] = M.getCodec(trackIdx);
    T["firstms"] = M.getFirstms(trackIdx);
    T["lastms"] = M.getLastms(trackIdx);
    T["bps"] = M.getBps(trackIdx);
    T["rate"] = M.getRate(trackIdx);
    if (M.getWidth(trackIdx)) { T["width"] = M.getWidth(trackIdx); }
    if (M.getHeight(trackIdx)) { T["height"] = M.getHeight(trackIdx); }
    if (M.getChannels(trackIdx)) { T["channels"] = M.getChannels(trackIdx); }
    const size_t src = M.getSourceTrack(trackIdx);
    if (src != INVALID_TRACK_ID && M.trackValid(src)) {
      T["source"] = M.getTrackIdentifier(src);
    } else if (knownSource.size()) {
      T["source"] = knownSource;
    }
  }
} // namespace Mist
