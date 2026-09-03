#pragma once

#include <mist/json.h>

#include <cstddef>
#include <cstdint>

namespace Mist {

  static const uint32_t LIVEPEER_MAX_CONSECUTIVE_REJECTIONS = 5;
  static const uint64_t LIVEPEER_SOCKET_MARGIN_S = 5;

  inline bool livepeerFatalUploadStatus(uint32_t status) {
    return status == 401 || status == 403 || status == 503;
  }

  inline bool livepeerShouldFallback(uint32_t consecutiveRejections) {
    return consecutiveRejections >= LIVEPEER_MAX_CONSECUTIVE_REJECTIONS;
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
