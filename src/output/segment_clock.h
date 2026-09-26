#pragma once

#include <cstdint>

namespace Mist {
  /// Wall-clock start times for the segments of one segmented recording.
  /// Media time maps to wall time through the stream's boot offset, which
  /// shifts when packets arrive ahead of real time (a publisher flushing its
  /// encoder at end of stream). The media stays continuous, so a segment is
  /// never stamped before the previous one ended: consumers that re-time
  /// segments by their start would otherwise overlap and drop media.
  class SegmentClock {
    public:
      /// Returns the start to write for a segment mapped to unixMs and lasting
      /// durationMs. Zero (no mapping) is returned as is and does not advance.
      uint64_t stamp(uint64_t unixMs, uint64_t durationMs) {
        if (!unixMs) { return 0; }
        if (lastEnd && unixMs < lastEnd) { unixMs = lastEnd; }
        lastEnd = unixMs + durationMs;
        return unixMs;
      }

    private:
      uint64_t lastEnd = 0;
  };
} // namespace Mist
