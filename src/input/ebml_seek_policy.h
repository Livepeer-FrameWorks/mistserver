#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace Mist {

  /// Keyframe index of one track as the EBML input stores it: key start times
  /// and the byte position of the cluster holding each key.
  struct EBMLSeekTrack {
      std::vector<uint64_t> keyTimes;
      std::vector<uint64_t> keyBpos;
  };

  /// Byte position of the cluster to start reading from so that every track in
  /// `tracks` delivers its packets from `seekTime` onward. Each track starts at
  /// its last key at or before seekTime, or at its first key when seekTime
  /// precedes it; the earliest of those wins. A track whose data starts before
  /// another track's first key (audio before video) is therefore never skipped.
  /// Positions of 0 are unknown and ignored; returns 0 when nothing is known.
  inline uint64_t ebmlSeekPosition(const std::vector<EBMLSeekTrack> & tracks, uint64_t seekTime) {
    uint64_t best = 0;
    for (const EBMLSeekTrack & t : tracks) {
      size_t n = t.keyTimes.size() < t.keyBpos.size() ? t.keyTimes.size() : t.keyBpos.size();
      uint64_t pos = 0;
      for (size_t i = 0; i < n; ++i) {
        if (i && t.keyTimes[i] > seekTime) { break; }
        if (t.keyBpos[i]) { pos = t.keyBpos[i]; }
      }
      if (pos && (!best || pos < best)) { best = pos; }
    }
    return best;
  }

} // namespace Mist
