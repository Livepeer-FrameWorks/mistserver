#pragma once

#include <mist/defines.h>
#include <mist/json.h>

#include <cstdint>
#include <string>

namespace Mist {
  inline const char *processExitStatus(int exitCode, const std::string & restartType, uint32_t bootCount) {
    if (exitCode == 2) { return "unrecoverable"; }
    if (exitCode == 0) { return "clean"; }
    if (restartType == "disabled" && bootCount) { return "disabled"; }
    return "retrying";
  }

  inline bool processSupervisorMayStart(bool active, uint8_t streamState, bool sourceEof) {
    return active && !sourceEof && streamState != STRMSTAT_SHUTDOWN && streamState != STRMSTAT_OFF;
  }

  inline std::string processExitTriggerPayload(const std::string & streamName, const std::string & processType,
                                               const std::string & processConfig, uint64_t pid, int exitCode,
                                               uint32_t bootCount, const std::string & status,
                                               const std::string & shortReason, const std::string & longReason) {
    return streamName + "\n" + processType + "\n" + processConfig + "\n" + std::to_string(pid) + "\n" +
      std::to_string(exitCode) + "\n" + std::to_string(bootCount) + "\n" + status + "\n" + shortReason + "\n" + longReason;
  }

  inline std::string processReplaceTriggerPayload(const std::string & streamName, const std::string & processType,
                                                  const std::string & processConfig, int exitCode,
                                                  const std::string & shortReason, const std::string & longReason) {
    return streamName + "\n" + processType + "\n" + processConfig + "\n" + std::to_string(exitCode) + "\n" +
      shortReason + "\n" + longReason;
  }

  /// Parses a PROCESS_REPLACE response into the replacement process configs.
  /// Only objects naming a process are kept; an empty, non-array or unusable response yields an
  /// empty array, which means the failed process is not replaced.
  inline JSON::Value processReplacementConfigs(const std::string & response) {
    JSON::Value replacements;
    replacements.append(JSON::Value());
    replacements.shrink(0);
    JSON::Value parsed = JSON::fromString(response);
    if (!parsed.isArray()) { return replacements; }
    jsonForEachConst (parsed, it) {
      if (!it->isObject() || !(*it)["process"].isString() || (*it)["process"].asStringRef().empty()) { continue; }
      replacements.append(*it);
    }
    return replacements;
  }
} // namespace Mist
