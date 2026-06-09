#pragma once
#include "comms.h"
#include "dtsc.h"

/// Protocol-neutral CMAF live-window / part-availability math.
/// Shared by the HLS playlist builder, the DASH MPD builder, and the
/// segment/part HTTP endpoint so advertisement and serving agree on exactly
/// which media range is safe. Depends only on the DTSC media model, not on any
/// HLS- or DASH-specific types.
namespace CMAF {
  namespace Live {
    /// max partial fragment duration in ms
    const uint32_t partDurationMaxMs = 500;

    /// lastms for a track clamped to (now - streamStart - jitter)
    uint64_t getLastmsForJitter(const DTSC::Meta & M, const size_t trackIdx, const uint64_t streamStartTime, const uint64_t maxJitter);

    /// largest minKeepAway across all selected tracks
    uint64_t getMaxJitter(const DTSC::Meta & M, const std::map<size_t, Comms::Users> & userSelect);

    /// lastms calculation incorporating jitter duration
    /// Always ensures the (lastms <= current time - jitter duration)
    uint64_t getLastms(const DTSC::Meta & M, const std::map<size_t, Comms::Users> & userSelect, const size_t trackIdx,
                       const uint64_t streamStartTime);

    /// safe live edge in ms for the requested track, bounded by the timing track
    uint64_t getLiveEdgeMs(const DTSC::Meta & M, const std::map<size_t, Comms::Users> & userSelect,
                           const size_t requestTrackId, const size_t timingTrackId, const uint64_t streamStartTime);

    /// Why a [startTime, targetTime) range is (not) servable. OK means servable.
    enum class Reason {
      OK,
      NO_VALID_PARTS, ///< track currently has no media parts at all
      EXPIRED, ///< requested start is older than the first valid part
      OUTSIDE_WINDOW, ///< requested range falls outside the valid part range
      EMPTY_RANGE, ///< after snapping, target is not past start
      NO_PAYLOAD ///< range carries no media bytes
    };

    /// Result of a servability check. snappedStart is the request start snapped
    /// to the actual media (part) boundary and is what the caller must serve.
    /// The part indices are carried so callers can reproduce diagnostics without
    /// re-deriving any of the checks.
    struct RangeServability {
        RangeServability()
          : ok(false), reason(Reason::OK), snappedStart(0), payloadSize(0), firstPart(0), endPart(0), firstValidPart(0),
            endValidPart(0), validEndTime(0) {}
        bool ok;
        Reason reason;
        uint64_t snappedStart;
        uint64_t payloadSize;
        size_t firstPart, endPart, firstValidPart, endValidPart;
        uint64_t validEndTime;
    };

    /// The single servability predicate for a CMAF media range on one track.
    /// Reproduces exactly what the segment/part HTTP endpoint will accept and
    /// serve, so manifests and the endpoint never disagree. Does NOT handle the
    /// HLS ".ts" case (CMAF media is always fMP4) nor block/wait.
    RangeServability isRangeServable(const DTSC::Meta & M, const size_t requestTrackId, const uint64_t startTime,
                                     const uint64_t targetTime);

    /// returns the end time for a given partial fragment, waiting for it if needed
    /// returns 0 for a hinted part which never got created
    uint64_t getPartTargetTime(const DTSC::Meta & M, const uint32_t idx, const uint32_t mTrack,
                               const uint64_t startTime, const uint64_t msn, const uint32_t part);
  } // namespace Live
} // namespace CMAF
