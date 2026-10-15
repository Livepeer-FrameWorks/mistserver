#pragma once

#include <mist/json.h>

#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <map>
#include <set>
#include <string>

namespace Mist {

  static const uint32_t LIVEPEER_MAX_CONSECUTIVE_REJECTIONS = 5;

  inline bool livepeerFatalUploadStatus(uint32_t status) {
    return status == 401 || status == 403;
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

  /// Largest deadlineMs the gateway accepts.
  static const uint64_t LIVEPEER_MAX_DEADLINE_MS = 3600000;

  /// Extra budget of a segment that starts this process's session on a gateway: the gateway
  /// registers the stream and an orchestrator sets the session up before the segment's own
  /// transcode. The gateway gives the same segment the same allowance (its session setup
  /// allowance), so this process waits as long as the gateway works on it.
  static const uint64_t LIVEPEER_SESSION_SETUP_ALLOWANCE_MS = 2000;

  /// The gateway's total budget for one segment, from its receipt of the
  /// upload to the end of the response, sent as deadlineMs on every upload. A
  /// configured deadline_ms (VOD) applies as is; live gets the segment duration
  /// plus one second. A segment that starts the session on its gateway
  /// (livepeerSegmentStartsSession) gets LIVEPEER_SESSION_SETUP_ALLOWANCE_MS on top.
  inline uint64_t livepeerSegmentDeadlineMs(const JSON::Value & options, uint64_t segmentDurationMs, bool startsSession) {
    int64_t configured = options.isMember("deadline_ms") ? options["deadline_ms"].asInt() : 0;
    uint64_t deadlineMs = configured > 0 ? (uint64_t)configured : segmentDurationMs + 1000;
    if (startsSession) { deadlineMs += LIVEPEER_SESSION_SETUP_ALLOWANCE_MS; }
    return deadlineMs < LIVEPEER_MAX_DEADLINE_MS ? deadlineMs : LIVEPEER_MAX_DEADLINE_MS;
  }

  /// Whether segment keyNo, sent to the broadcaster at addr, starts this process's session
  /// there: it is the first segment this process sends that broadcaster. `sessionStarts` holds,
  /// per broadcaster, the segment that started the session; the first call for a broadcaster
  /// claims it for keyNo. Re-sends of that segment to the same broadcaster start it as well.
  /// Segment 0 starts the session on the broadcaster a process begins with, and the first
  /// segment sent to a broadcaster after switching to it starts the session there.
  inline bool livepeerSegmentStartsSession(std::map<std::string, uint64_t> & sessionStarts, const std::string & addr, uint64_t keyNo) {
    std::map<std::string, uint64_t>::iterator it = sessionStarts.find(addr);
    if (it == sessionStarts.end()) {
      sessionStarts[addr] = keyNo;
      return true;
    }
    return it->second == keyNo;
  }

  /// The gateway answers within its deadline, so the upload socket waits one
  /// second longer (whole seconds, rounded up).
  inline uint64_t livepeerSocketTimeoutSeconds(uint64_t deadlineMs) {
    return (deadlineMs + 1000 + 999) / 1000;
  }

  /// One attempt per upload request: the downloader never re-POSTs on its own
  /// and cancels in-flight work; the upload loop decides every re-send.
  static const uint32_t LIVEPEER_DOWNLOADER_RETRY_COUNT = 1;

  /// A 503 means the gateway had no result for the segment yet; its work goes
  /// on until the deadline and a re-POST of the same segment joins it.
  inline bool livepeerSegmentBudgetSpent(uint64_t elapsedMs, uint64_t deadlineMs) {
    return elapsedMs >= deadlineMs;
  }

  enum class LivepeerBudgetMissStep { SkipSegment, ResendSegment, Fallback };

  /// What to do with a segment whose budget was spent without a result.
  /// `consecutiveMisses` counts it and the misses since the last delivered
  /// segment. Every job falls back once the gateway missed as many
  /// consecutive budgets as it may reject segments. Before that, a live stream
  /// skips the segment, as it does a rejected one; a VOD job cannot leave the
  /// hole, and since its source stays available it sends the same segment
  /// again with a new budget.
  inline LivepeerBudgetMissStep
    livepeerBudgetMissStep(const JSON::Value & options, const std::string & streamName, uint32_t consecutiveMisses) {
    if (livepeerShouldFallback(consecutiveMisses)) { return LivepeerBudgetMissStep::Fallback; }
    if (livepeerRejectedSegmentStopsJob(options, streamName)) { return LivepeerBudgetMissStep::ResendSegment; }
    return LivepeerBudgetMissStep::SkipSegment;
  }

  /// Seconds the session warm-up waits for the gateway's answer. The gateway answers a warm-up
  /// before it does the work.
  static const uint32_t LIVEPEER_SESSION_WARMUP_TIMEOUT_S = 2;

  /// Whether this job sends the session warm-up: the stream's first keyframe, posted to the
  /// gateway's /live/<id>/session so it sets the orchestrator session up while segment 0 is still
  /// being read. Only a live stream needs it (a VOD job's budget absorbs the setup), and only a
  /// gateway configured through hardcoded_broadcasters offers the endpoint; the hosted Livepeer
  /// API (access_token) does not.
  inline bool livepeerSessionWarmupWanted(const JSON::Value & options, const std::string & streamName) {
    if (options.isMember("access_token") && options["access_token"].isString() && options["access_token"]) {
      return false;
    }
    return !livepeerRejectedSegmentStopsJob(options, streamName);
  }

  /// The deadlineMs the warm-up carries: only a configured deadline_ms. Without an auth webhook
  /// the gateway takes the deadline of the stream's first request as the stream's deadline, and a
  /// live segment's own deadline depends on its duration, which the warm-up does not know.
  inline uint64_t livepeerSessionWarmupDeadlineMs(const JSON::Value & options) {
    int64_t configured = options.isMember("deadline_ms") ? options["deadline_ms"].asInt() : 0;
    if (configured <= 0) { return 0; }
    return (uint64_t)configured < LIVEPEER_MAX_DEADLINE_MS ? (uint64_t)configured : LIVEPEER_MAX_DEADLINE_MS;
  }

  /// The duration of the warm-up keyframe in ms: one frame at fpks (frames per 1000 s), or 40 ms
  /// (one frame at 25 fps) when the frame rate is unknown.
  inline uint64_t livepeerSessionWarmupFrameMs(uint64_t fpks) {
    if (!fpks) { return 40; }
    uint64_t ms = (1000000 + fpks / 2) / fpks;
    return ms ? ms : 1;
  }

  /// The source frame rate the warm-up reports in Content-Frame-Rate, in frames per second with
  /// three decimals, or empty when unknown. The gateway cannot probe it from a single frame, and
  /// the stream it registers is checked against the job's profiles at that rate.
  inline std::string livepeerSessionWarmupFrameRate(uint64_t fpks) {
    if (!fpks) { return ""; }
    char rate[32];
    snprintf(rate, sizeof(rate), "%" PRIu64 ".%03" PRIu64, fpks / 1000, fpks % 1000);
    return rate;
  }

  inline JSON::Value buildLivepeerTranscodeConfiguration(const JSON::Value & options, uint64_t deadlineMs) {
    JSON::Value configuration;
    configuration["profiles"] = options["target_profiles"];
    if (options.isMember("workload") && options["workload"].isString()) {
      configuration["workload"] = options["workload"];
    }
    if (deadlineMs > 0) { configuration["deadlineMs"] = deadlineMs; }
    if (options.isMember("min_speed")) { configuration["minSpeed"] = options["min_speed"]; }
    if (options.isMember("job_token") && options["job_token"].isString()) {
      configuration["jobToken"] = options["job_token"];
    }
    return configuration;
  }

} // namespace Mist
