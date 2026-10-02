#pragma once

#include <mist/json.h>

#include <cstddef>
#include <cstdint>
#include <set>
#include <string>

namespace Mist {

  static const uint32_t LIVEPEER_MAX_CONSECUTIVE_REJECTIONS = 5;
  static const uint64_t LIVEPEER_SOCKET_MARGIN_S = 5;

  inline bool livepeerFatalUploadStatus(uint32_t status) {
    return status == 401 || status == 403 || status == 503;
  }

  inline bool livepeerShouldFallback(uint32_t consecutiveRejections) {
    return consecutiveRejections >= LIVEPEER_MAX_CONSECUTIVE_REJECTIONS;
  }

  /// Re-sends of one segment to the same broadcaster after a 422 before moving
  /// on. A gateway can reject the very first segment of a new manifest while it
  /// is still setting that manifest up; the identical segment is accepted a
  /// moment later.
  static const uint32_t LIVEPEER_SAME_BROADCASTER_422_RETRIES = 2;

  enum class LivepeerRejectionStep { RetrySame, SwitchBroadcaster, RejectSegment };

  /// What to do after the `rejectionsHere`-th 422 for one segment at the
  /// current broadcaster: retry it there, try one other broadcaster, or give
  /// the segment up.
  inline LivepeerRejectionStep livepeerRejectionStep(uint32_t rejectionsHere, bool alreadySwitched) {
    if (rejectionsHere <= LIVEPEER_SAME_BROADCASTER_422_RETRIES) { return LivepeerRejectionStep::RetrySame; }
    if (!alreadySwitched) { return LivepeerRejectionStep::SwitchBroadcaster; }
    return LivepeerRejectionStep::RejectSegment;
  }

  /// Backoff before re-sending a rejected segment to the same broadcaster.
  inline uint64_t livepeerRejectionBackoffMs(uint32_t rejectionsHere) {
    uint64_t ms = 250;
    for (uint32_t i = 1; i < rejectionsHere && ms < 2000; ++i) { ms *= 2; }
    return ms < 2000 ? ms : 2000;
  }

  /// A live stream can skip a segment it cannot transcode; a VOD processing
  /// job cannot, because the gap would become a permanent hole in the asset.
  /// It stops Livepeer instead so the local fallback transcodes the whole
  /// source.
  inline bool livepeerRejectedSegmentStopsJob(const JSON::Value & options, const std::string & streamName) {
    if (options.isMember("workload") && options["workload"].isString() && options["workload"].asString() == "vod") {
      return true;
    }
    return streamName.compare(0, 11, "processing+") == 0;
  }

  enum class LivepeerSwitchOutcome { Switched, AlreadySwitched, NoAlternative };

  /// Moves the shared broadcaster `current` off `failedAddr`, the broadcaster
  /// one upload thread just failed a segment on. `failedHere` holds every
  /// broadcaster that segment already failed on. Upload threads share
  /// `current` and call this under one lock: when another thread already
  /// moved `current` to a broadcaster this segment has not failed on, that
  /// choice stands. Otherwise a broadcaster outside `failedHere` (and other
  /// than `failedAddr`) is picked from `candidates` using `pick`; when none is
  /// left, `current` stays as it is.
  inline LivepeerSwitchOutcome livepeerSwitchBroadcaster(std::string & current, const std::string & failedAddr,
                                                         const std::set<std::string> & failedHere,
                                                         const std::set<std::string> & candidates, size_t pick) {
    if (!current.empty() && current != failedAddr && !failedHere.count(current)) {
      return LivepeerSwitchOutcome::AlreadySwitched;
    }
    std::set<std::string> valid;
    for (std::set<std::string>::const_iterator it = candidates.begin(); it != candidates.end(); ++it) {
      if (*it != failedAddr && !failedHere.count(*it)) { valid.insert(*it); }
    }
    if (valid.empty()) { return LivepeerSwitchOutcome::NoAlternative; }
    std::set<std::string>::const_iterator it = valid.begin();
    for (size_t r = pick % valid.size(); r; --r) { ++it; }
    current = *it;
    return LivepeerSwitchOutcome::Switched;
  }

  inline bool livepeerShouldRetryCurrentBroadcaster(bool postSucceeded, bool requestWasSent) {
    return !postSucceeded && requestWasSent;
  }

  inline uint64_t livepeerSocketTimeoutSeconds(uint64_t segmentDurationMs, uint64_t deadlineMs) {
    return deadlineMs ? deadlineMs / 1000 + LIVEPEER_SOCKET_MARGIN_S : segmentDurationMs / 1000 + 2;
  }

  inline size_t livepeerDownloaderRetryCount(uint64_t deadlineMs) {
    return deadlineMs ? 1 : 2;
  }

  inline JSON::Value buildLivepeerTranscodeConfiguration(const JSON::Value & options, uint64_t deadlineMs) {
    JSON::Value configuration;
    configuration["profiles"] = options["target_profiles"];
    if (options.isMember("workload") && options["workload"].isString()) {
      configuration["workload"] = options["workload"];
    }
    if (deadlineMs > 0) { configuration["deadlineMs"] = options["deadline_ms"]; }
    if (options.isMember("min_speed")) { configuration["minSpeed"] = options["min_speed"]; }
    if (options.isMember("job_token") && options["job_token"].isString()) {
      configuration["jobToken"] = options["job_token"];
    }
    return configuration;
  }

} // namespace Mist
