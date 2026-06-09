#include "cmaf_live.h"

#include "cmaf.h"
#include "timing.h"

namespace CMAF {
  namespace Live {

    uint64_t getLastmsForJitter(const DTSC::Meta & M, const size_t trackIdx, const uint64_t streamStartTime, const uint64_t maxJitter) {
      uint64_t now = Util::unixMS();
      uint64_t wallClockLastMs = 0;
      if (now > streamStartTime + maxJitter) { wallClockLastMs = now - streamStartTime - maxJitter; }
      return std::min(M.getLastms(trackIdx), wallClockLastMs);
    }

    uint64_t getMaxJitter(const DTSC::Meta & M, const std::map<size_t, Comms::Users> & userSelect) {
      std::map<size_t, Comms::Users>::const_iterator it = userSelect.begin();
      uint64_t maxJitter = 0;
      for (; it != userSelect.end(); it++) {
        uint64_t minKeepAway = M.getMinKeepAway(it->first);
        if (minKeepAway > maxJitter) { maxJitter = minKeepAway; }
      }
      return maxJitter;
    }

    /// lastms calculation incorporating jitter duration
    /// Always ensures the (lastms <= current time - jitter duration)
    uint64_t getLastms(const DTSC::Meta & M, const std::map<size_t, Comms::Users> & userSelect, const size_t trackIdx,
                       const uint64_t streamStartTime) {
      return getLastmsForJitter(M, trackIdx, streamStartTime, getMaxJitter(M, userSelect));
    }

    uint64_t getLiveEdgeMs(const DTSC::Meta & M, const std::map<size_t, Comms::Users> & userSelect,
                           const size_t requestTrackId, const size_t timingTrackId, const uint64_t streamStartTime) {
      const uint64_t requestLastMs = getLastms(M, userSelect, requestTrackId, streamStartTime);
      if (requestTrackId == timingTrackId) { return requestLastMs; }
      return std::min(requestLastMs, getLastms(M, userSelect, timingTrackId, streamStartTime));
    }

    RangeServability isRangeServable(const DTSC::Meta & M, const size_t requestTrackId, const uint64_t startTime,
                                     const uint64_t targetTime) {
      RangeServability r;
      r.ok = false;
      r.snappedStart = startTime;
      r.payloadSize = 0;
      r.firstPart = r.endPart = 0;

      DTSC::Parts parts(M.parts(requestTrackId));
      r.firstValidPart = parts.getFirstValid();
      r.endValidPart = parts.getEndValid();
      if (r.firstValidPart >= r.endValidPart) {
        r.reason = Reason::NO_VALID_PARTS;
        return r;
      }

      const uint64_t firstValidPartTime = M.getPartTime(r.firstValidPart, requestTrackId);
      if (startTime < firstValidPartTime) {
        r.reason = Reason::EXPIRED;
        return r;
      }

      r.firstPart = M.getPartIndex(startTime, requestTrackId);
      r.endPart = M.getPartIndex(targetTime, requestTrackId);
      const size_t lastValidPart = r.endValidPart - 1;
      r.validEndTime = M.getPartTime(lastValidPart, requestTrackId) + parts.getDuration(lastValidPart);
      if (r.firstPart < r.firstValidPart || r.firstPart >= r.endValidPart || r.endPart > r.endValidPart) {
        r.reason = Reason::OUTSIDE_WINDOW;
        return r;
      }
      if (targetTime > r.validEndTime) {
        r.reason = Reason::OUTSIDE_WINDOW;
        return r;
      }

      // Snap the request to the actual media (part) boundary.
      r.snappedStart = M.getPartTime(r.firstPart, requestTrackId);

      if (targetTime <= r.snappedStart) {
        r.reason = Reason::EMPTY_RANGE;
        return r;
      }

      r.payloadSize = CMAF::payloadSize(M, requestTrackId, r.snappedStart, targetTime);
      if (!r.payloadSize) {
        r.reason = Reason::NO_PAYLOAD;
        return r;
      }

      r.reason = Reason::OK;
      r.ok = true;
      return r;
    }

    /// returns the end time for a given partial fragment
    /// returns 0 for a hinted part which never got created
    uint64_t getPartTargetTime(const DTSC::Meta & M, const uint32_t idx, const uint32_t mTrack,
                               const uint64_t startTime, const uint64_t msn, const uint32_t part) {
      DTSC::Fragments fragments(M.fragments(mTrack));
      if (msn < fragments.getFirstValid() || msn >= fragments.getEndValid()) { return 0; }

      // Estimate the target end time for a given part
      // 50 ms is margin of safety to accommodate inconsistencies
      const uint64_t calcTargetTime = startTime + (part + 1) * partDurationMaxMs + 50;

      uint64_t lastms = std::min(M.getLastms(mTrack), M.getLastms(idx));
      uint16_t count = 0;

      // wait until estimated target end time is <= lastms for the track
      while (calcTargetTime > lastms && count++ < 50) {
        Util::wait(calcTargetTime - lastms);
        lastms = std::min(M.getLastms(mTrack), M.getLastms(idx));
      }

      // Duration maybe invalid, indicating msn is not complete
      // But the part is ready. So return the end time
      uint64_t duration = fragments.getDuration(msn);
      if (!duration) { return startTime + ((part + 1) * partDurationMaxMs); }

      // If duration valid, MSN is fully finished
      // Possible that the last partial fragment duration < partDurationMaxMs
      // Find the exact duration of the last partial fragment
      uint64_t partTargetTime = std::min(startTime + duration, startTime + ((part + 1) * partDurationMaxMs));

      if (duration && (partTargetTime - startTime) > duration) { return 0; }
      return partTargetTime;
    }

  } // namespace Live
} // namespace CMAF
